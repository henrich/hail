/*
 * Copyright (c) 2009, Red Hat, Inc.
 */
#define _GNU_SOURCE
#include "chunkd-config.h"
#include <sys/types.h>
#include <sys/socket.h>
#include <glib.h>
#include <syslog.h>
#include <string.h>
#include <unistd.h>
#include <event.h>
#include <netdb.h>
#include <arpa/nameser.h>
#include <netinet/in.h>
#include <resolv.h>		/* must follow in.h, nameser.h */
#include <errno.h>
#include <cldc.h>
#include "chunkd.h"

#define ALIGN8(n)	((8 - ((n) & 7)) & 7)

#define N_CLD		10	/* 5 * (v4+v6) */

struct cld_host {
	int known;
	struct cldc_host h;
};

struct cld_session {
	bool forced_hosts;		/* Administrator overrode default CLD */
	bool sess_open;
	struct cldc_udp *lib;		/* library state */

	int actx;		/* Active host cldv[actx] */
	struct cld_host cldv[N_CLD];

	struct event ev;	/* Associated with fd */
	char *cfname;		/* /chunk-CELL directory */
	struct cldc_fh *cfh;	/* /chunk-CELL directory fh */
	char *ffname;		/* /chunk-CELL/NID */
	struct cldc_fh *ffh;	/* /chunk-cell/NID, keep open for lock */
	uint32_t nid;
	const char *ourhost;	/* N.B. points to some global data. */
	struct geo *ploc;	/* N.B. points to some global data. */

	void (*state_cb)(enum st_cld);
};

static int cldu_set_cldc(struct cld_session *sp, int newactive);
static int cldu_new_sess(struct cldc_call_opts *carg, enum cle_err_codes errc);
static int cldu_open_c_cb(struct cldc_call_opts *carg, enum cle_err_codes errc);
static int cldu_close_c_cb(struct cldc_call_opts *carg, enum cle_err_codes errc);
static int cldu_open_f_cb(struct cldc_call_opts *carg, enum cle_err_codes errc);
static int cldu_lock_cb(struct cldc_call_opts *carg, enum cle_err_codes errc);
static int cldu_put_cb(struct cldc_call_opts *carg, enum cle_err_codes errc);
static int cldu_make_ffile(char **ret, struct cld_session *sp);

#define SVC "chunk"
static char svc[] = SVC;

/*
 * Identify the next host to be tried.
 *
 * In theory we should at least look at priorities, if not weights. Maybe later.
 */
static int cldu_nextactive(struct cld_session *sp)
{
	int i;
	int n;

	if ((n = sp->actx + 1) >= N_CLD)
		n = 0;
	for (i = 0; i < N_CLD; i++) {
		if (sp->cldv[n].known)
			return n;
		if (++n >= N_CLD)
			n = 0;
	}
	/* Full circle, end on the old actx */
	return sp->actx;
}

static int cldu_setcell(struct cld_session *sp, const char *thiscell,
			uint32_t thisnid, const char *thishost,
			struct geo *locp)
{
	size_t cnlen;
	size_t mlen;
	char nbuf[11];	/* 32 bits in decimal and nul */
	char *mem;

	if (thiscell == NULL) {
		thiscell = "default";
	}

	snprintf(nbuf, 11, "%u", thisnid);

	cnlen = strlen(thiscell);

	mlen = sizeof("/" SVC "-")-1;
	mlen += cnlen;
	mlen++;	// '\0'
	mem = malloc(mlen);
	sprintf(mem, "/%s-%s", svc, thiscell);
	sp->cfname = mem;

	mlen = sizeof("/" SVC "-")-1;
	mlen += cnlen;
	mlen++;	// '/'
	mlen += strlen(nbuf);
	mlen++;	// '\0'
	mem = malloc(mlen);
	sprintf(mem, "/%s-%s/%s", svc, thiscell, nbuf);
	sp->ffname = mem;

	sp->nid = thisnid;
	sp->ourhost = thishost;
	sp->ploc = locp;

	return 0;
}

static void cldu_event(int fd, short events, void *userdata)
{
	struct cld_session *sp = userdata;
	int rc;

	if (!sp->lib) {
		applog(LOG_WARNING, "Stray UDP event");
		return;
	}

	rc = cldc_udp_receive_pkt(sp->lib);
	if (rc) {
		applog(LOG_INFO, "cldc_udp_receive_pkt failed: %d", rc);
		/*
		 * Reacting to ICMP messages is a bad idea, because
		 *  - it makes us loop hard in case CLD is down, unless we
		 *    insert additional tricky timeouts
		 *  - it deals poorly with transient problems like CLD reboots
		 */
#if 0
		if (rc == -ECONNREFUSED) {	/* ICMP tells us */
			int newactive;
			/* P3 */ applog(LOG_INFO, "Restarting session");
			// evtimer_del(&sp->tm);
			cldc_kill_sess(sp->lib->sess);
			sp->lib->sess = NULL;
			newactive = cldu_nextactive(sp);
			if (cldu_set_cldc(sp, newactive))
				return;
			// evtimer_add(&sp->tm, &cldc_to_delay);
		}
		return;
#endif
	}
}

static bool cldu_p_timer_ctl(void *priv, bool add,
			     int (*cb)(struct cldc_session *, void *),
			     void *cb_priv, time_t secs)
{
	struct cld_session *sp = priv;
	return cldc_levent_timer(sp->lib, add, cb, cb_priv, secs);
}

static int cldu_p_pkt_send(void *priv, const void *addr, size_t addrlen,
			       const void *buf, size_t buflen)
{
	struct cld_session *sp = priv;
	return cldc_udp_pkt_send(sp->lib, addr, addrlen, buf, buflen);
}

static void cldu_p_event(void *priv, struct cldc_session *csp,
			 struct cldc_fh *fh, uint32_t what)
{
	struct cld_session *sp = priv;
	int newactive;

	if (what == CE_SESS_FAILED) {
		sp->sess_open = false;
		if (sp->lib->sess != csp)
			applog(LOG_ERR, "Stray session failed, sid " SIDFMT,
			       SIDARG(csp->sid));
		else
			applog(LOG_ERR, "Session failed, sid " SIDFMT,
			       SIDARG(csp->sid));
		// evtimer_del(&sp->tm);
		sp->lib->sess = NULL;
		newactive = cldu_nextactive(sp);
		if (cldu_set_cldc(sp, newactive))
			return;
		// evtimer_add(&sp->tm, &cldc_to_delay);
	} else {
		if (csp)
			applog(LOG_INFO, "cldc event 0x%x sid " SIDFMT,
			       what, SIDARG(csp->sid));
		else
			applog(LOG_INFO, "cldc event 0x%x no sid", what);
	}
}

static struct cldc_ops cld_ops = {
	.timer_ctl =	cldu_p_timer_ctl,
	.pkt_send =	cldu_p_pkt_send,
	.event =	cldu_p_event,
};

/*
 * Open the library, start its session, and reguster its socket with libevent.
 * Our session remains consistent in case of an error in this function,
 * so that we can continue and retry meaningfuly.
 */
static int cldu_set_cldc(struct cld_session *sp, int newactive)
{
	struct cldc_host *hp;
	struct cldc_udp *lib;
	struct cldc_call_opts copts;
	int rc;

	if (sp->lib) {
		event_del(&sp->ev);
		cldc_udp_free(sp->lib);
		sp->lib = NULL;
	}

	sp->actx = newactive;
	if (!sp->cldv[sp->actx].known) {
		applog(LOG_ERR, "No CLD hosts");
		goto err_addr;
	}
	hp = &sp->cldv[sp->actx].h;

	rc = cldc_udp_new(hp->host, hp->port, &sp->lib);
	if (rc) {
		applog(LOG_ERR, "cldc_udp_new(%s,%u) error: %d",
		       hp->host, hp->port, rc);
		goto err_lib_new;
	}
	lib = sp->lib;

	if (debugging)
		applog(LOG_INFO, "Selected CLD host %s port %u",
		       hp->host, hp->port);

	/*
	 * This is a little iffy: we assume that it's ok to re-issue
	 * event_set() for an event that was unregistered with event_del().
	 * In any case, there's no other way to set the file descriptor.
	 */
	event_set(&sp->ev, sp->lib->fd, EV_READ | EV_PERSIST, cldu_event, sp);

	if (event_add(&sp->ev, NULL) < 0) {
		applog(LOG_INFO, "Failed to add CLD event");
		goto err_event;
	}

	memset(&copts, 0, sizeof(struct cldc_call_opts));
	copts.cb = cldu_new_sess;
	copts.private = sp;
	rc = cldc_new_sess(&cld_ops, &copts, lib->addr, lib->addr_len,
			   "tabled", "tabled", sp, &lib->sess);
	if (rc) {
		applog(LOG_INFO,
		       "Failed to start CLD session on host %s port %u",
		       hp->host, hp->port);
		goto err_sess;
	}

	// if (debugging)
	//	lib->sess->verbose = true;

	return 0;

err_sess:
err_event:
	cldc_udp_free(sp->lib);
	sp->lib = NULL;
err_lib_new:
err_addr:
	return -1;
}

static int cldu_new_sess(struct cldc_call_opts *carg, enum cle_err_codes errc)
{
	struct cld_session *sp = carg->private;
	struct cldc_call_opts copts;
	int rc;

	if (errc != CLE_OK) {
		applog(LOG_INFO, "New CLD session creation failed: %d", errc);
		return 0;
	}

	sp->sess_open = true;
	applog(LOG_INFO, "New CLD session created, sid " SIDFMT,
	       SIDARG(sp->lib->sess->sid));

	/*
	 * First, make sure the base directory exists.
	 */
	memset(&copts, 0, sizeof(copts));
	copts.cb = cldu_open_c_cb;
	copts.private = sp;
	rc = cldc_open(sp->lib->sess, &copts, sp->cfname,
		       COM_READ | COM_WRITE | COM_CREATE | COM_DIRECTORY,
		       CE_MASTER_FAILOVER | CE_SESS_FAILED, &sp->cfh);
	if (rc) {
		applog(LOG_ERR, "cldc_open(%s) call error: %d\n",
		       sp->cfname, rc);
	}
	return 0;
}

static int cldu_open_c_cb(struct cldc_call_opts *carg, enum cle_err_codes errc)
{
	struct cld_session *sp = carg->private;
	struct cldc_call_opts copts;
	int rc;

	if (errc != CLE_OK) {
		applog(LOG_ERR, "CLD open(%s) failed: %d", sp->cfname, errc);
		return 0;
	}
	if (sp->cfh == NULL) {
		applog(LOG_ERR, "CLD open(%s) failed: NULL fh", sp->cfname);
		return 0;
	}
	if (!sp->cfh->valid) {
		applog(LOG_ERR, "CLD open(%s) failed: invalid fh", sp->cfname);
		return 0;
	}

	/*
	 * We don't use directory handle to open files in it, so close it.
	 */
	memset(&copts, 0, sizeof(copts));
	copts.cb = cldu_close_c_cb;
	copts.private = sp;
	rc = cldc_close(sp->cfh, &copts);
	if (rc) {
		applog(LOG_ERR, "cldc_close call error %d", rc);
	}

	return 0;
}

static int cldu_close_c_cb(struct cldc_call_opts *carg, enum cle_err_codes errc)
{
	struct cld_session *sp = carg->private;
	struct cldc_call_opts copts;
	int rc;

	if (errc != CLE_OK) {
		applog(LOG_ERR, "CLD close(%s) failed: %d", sp->cfname, errc);
		return 0;
	}

	/*
	 * Then, create the membership file for us.
	 *
	 * It is a bit racy to create a file like this, applications can see
	 * an empty file, or a file with stale contents. But what to do?
	 */
	memset(&copts, 0, sizeof(copts));
	copts.cb = cldu_open_f_cb;
	copts.private = sp;
	rc = cldc_open(sp->lib->sess, &copts, sp->ffname,
		       COM_WRITE | COM_LOCK | COM_CREATE,
		       CE_MASTER_FAILOVER | CE_SESS_FAILED, &sp->ffh);
	if (rc) {
		applog(LOG_ERR, "cldc_open(%s) call error: %d\n",
		       sp->ffname, rc);
	}
	return 0;
}

static int cldu_open_f_cb(struct cldc_call_opts *carg, enum cle_err_codes errc)
{
	struct cld_session *sp = carg->private;
	struct cldc_call_opts copts;
	int rc;

	if (errc != CLE_OK) {
		applog(LOG_ERR, "CLD open(%s) failed: %d", sp->ffname, errc);
		return 0;
	}
	if (sp->ffh == NULL) {
		applog(LOG_ERR, "CLD open(%s) failed: NULL fh", sp->ffname);
		return 0;
	}
	if (!sp->ffh->valid) {
		applog(LOG_ERR, "CLD open(%s) failed: invalid fh", sp->ffname);
		return 0;
	}

	if (debugging)
		applog(LOG_DEBUG, "CLD file \"%s\" created", sp->ffname);

	/*
	 * Lock the file, in case two hosts got the same NID.
	 */
	memset(&copts, 0, sizeof(copts));
	copts.cb = cldu_lock_cb;
	copts.private = sp;
	rc = cldc_lock(sp->ffh, &copts, 0, false);
	if (rc) {
		applog(LOG_ERR, "cldc_lock call error %d", rc);
	}

	return 0;
}

static int cldu_lock_cb(struct cldc_call_opts *carg, enum cle_err_codes errc)
{
	struct cld_session *sp = carg->private;
	char *buf = NULL; /* stupid gcc 4.4.1 throws a warning */
	int len;
	struct cldc_call_opts copts;
	int rc;

	if (errc != CLE_OK) {
		applog(LOG_ERR, "CLD lock(%s) failed: %d", sp->cfname, errc);
		return 0;
	}

	if (cldu_make_ffile(&buf, sp))
		return 0;
	len = strlen(buf);

	memset(&copts, 0, sizeof(copts));
	copts.cb = cldu_put_cb;
	copts.private = sp;
	rc = cldc_put(sp->ffh, &copts, buf, len);
	if (rc) {
		applog(LOG_ERR, "cldc_put(%s) call error: %d\n",
		       sp->ffname, rc);
	}

	free(buf);

	return 0;
}

static int cldu_put_cb(struct cldc_call_opts *carg, enum cle_err_codes errc)
{
	struct cld_session *sp = carg->private;
	// struct cldc_call_opts copts;
	// int rc;

	if (errc != CLE_OK) {
		applog(LOG_ERR, "CLD put(%s) failed: %d", sp->ffname, errc);
		return 0;
	}

	if (debugging)
		applog(LOG_DEBUG, "CLD file \"%s\" written", sp->ffname);

	return 0;
}

/*
 * Create the file with our parameters in memory, return as ret.
 */
static int cldu_make_ffile(char **ret, struct cld_session *sp)
{
	GList *str_list = NULL;
	char *buf;
	char *str;
	size_t len;
	GList *tmp;
	int rc;

	rc = asprintf(&str,
		"<Chunk>\r\n"
		" <NID>%u</NID>\r\n",
		sp->nid);
	if (rc == -1) {
		applog(LOG_ERR, "OOM in asprintf\n");
		goto error;
	}
	str_list = g_list_append(str_list, str);

	/*
	 * XXX FIXME sockets has to be a parameter, not global
	 */
	for (tmp = chunkd_srv.sockets; tmp; tmp = tmp->next) {
		struct server_socket *sock = tmp->data;
		const struct listen_cfg *cfg = sock->cfg;
		const char *host;

		host = cfg->node;
		if (host == NULL || host[0] == 0)
			host = sp->ourhost;
	
		rc = asprintf(&str,
			" <Socket>\r\n"
			"  <Type>%s</Type>\r\n"
			"  <Host>%s</Host>\r\n"
			"  <Port>%s</Port>\r\n"
			" </Socket>\r\n",
			cfg->encrypt ? "chunk-ssl" : "chunk",
			host,
			cfg->port);
		if (rc == -1) {
			applog(LOG_ERR, "OOM in asprintf\n");
			goto error;
		}
		str_list = g_list_append(str_list, str);
	}

	rc = asprintf(&str,
		" <Geo>\r\n"
		"  <Area>%s</Area>\r\n"
		"  <Building>%s</Building>\r\n"
		"  <Rack>%s</Rack>\r\n"
		" </Geo>\r\n"
		"</Chunk>\r\n",
		sp->ploc->area ? sp->ploc->area : "-",
		sp->ploc->zone ? sp->ploc->zone : "-",
		sp->ploc->rack ? sp->ploc->rack : "-");
	if (rc == -1) {
		applog(LOG_ERR, "OOM in asprintf\n");
		goto error;
	}
	str_list = g_list_append(str_list, str);

	len = 0;
	for (tmp = str_list; tmp; tmp = tmp->next)
		len += strlen(tmp->data);
	len++;		// nul

	buf = malloc(len);
	if (!buf) {
		applog(LOG_ERR, "OOM for ffile");
		rc = -1;
		goto error;
	}

	len = 0;
	for (tmp = str_list; tmp; tmp = tmp->next) {
		strcpy(buf + len, tmp->data);
		len += strlen(tmp->data);
	}
	buf[len] = 0;

	*ret = buf;
	rc = 0;
error:
	for (tmp = str_list; tmp; tmp = tmp->next)
		free(tmp->data);
	g_list_free(str_list);
	return rc;
}

/*
 */
static struct cld_session ses;

/*
 * This initiates our sole session with a CLD instance.
 *
 * Mostly due to our laziness and lack of need, thishost and locp are saved
 * by reference, so their lifetime must exceed the lifetime of the session
 * (the time between cld_begin and cld_end).
 */
int cld_begin(const char *thishost, const char *thiscell, uint32_t nid,
	      struct geo *locp, void (*cb)(enum st_cld),
	      void log(const char *fmt, ...))
{

	cld_ops.printf = log;

	if (!nid)
		return 0;
	cldc_init();

	/*
	 * As long as we permit pre-seeding lists of CLD hosts,
	 * we cannot wipe our session anymore. Note though, as long
	 * as cld_end terminates it right, we can call cld_begin again.
	 */
	// memset(&ses, 0, sizeof(struct cld_session));
	ses.state_cb = cb;

	if (cldu_setcell(&ses, thiscell, nid, thishost, locp)) {
		/* Already logged error */
		goto err_cell;
	}

	if (!ses.forced_hosts) {
		GList *tmp, *host_list = NULL;
		int i;

		if (cldc_getaddr(&host_list, thishost, debugging, log)) {
			/* Already logged error */
			goto err_addr;
		}

		/* copy host_list into cld_session host array,
		 * taking ownership of alloc'd strings along the way
		 */
		i = 0;
		for (tmp = host_list; tmp; tmp = tmp->next) {
			struct cldc_host *hp = tmp->data;
			if (i < N_CLD) {
				memcpy(&ses.cldv[i].h, hp,
				       sizeof(struct cldc_host));
				ses.cldv[i].known = 1;
				i++;
			} else {
				free(hp->host);
			}
			free(hp);
		}

		g_list_free(host_list);
	}

	/*
	 * FIXME: We should find next suitable host according to
	 * the priority and weight (among those which are up).
	 * -- Actually, it only works when recovering from CLD failure.
	 *    Thereafter, any slave CLD redirects us to the master.
	 */
	if (cldu_set_cldc(&ses, 0)) {
		/* Already logged error */
		goto err_net;
	}

	return 0;

err_net:
err_addr:
err_cell:
	return -1;
}

void cld_end(void)
{
	int i;

	if (!ses.nid)
		return;

	if (ses.lib) {
		event_del(&ses.ev);
		// if (ses.sess_open)	/* kill it always, include half-open */
		cldc_kill_sess(ses.lib->sess);
		cldc_udp_free(ses.lib);
		ses.lib = NULL;
	}

	if (!ses.forced_hosts) {
		for (i = 0; i < N_CLD; i++) {
			if (ses.cldv[i].known)
				free(ses.cldv[i].h.host);
		}
	}

	free(ses.cfname);
	free(ses.ffname);
}

void cldu_add_host(const char *hostname, unsigned int port,
		   void log(const char *fmt, ...))
{
	static struct cld_session *sp = &ses;
	struct cld_host *hp;
	int i;

	for (i = 0; i < N_CLD; i++) {
		hp = &sp->cldv[i];
		if (!hp->known)
			break;
	}
	if (i >= N_CLD)
		return;

	if (cldc_saveaddr(&hp->h, 100, 100, port, strlen(hostname), hostname,
			  debugging, log))
		return;
	hp->known = 1;

	sp->forced_hosts = true;
}
