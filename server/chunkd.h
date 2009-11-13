#ifndef __STORAGED_H__
#define __STORAGED_H__

#include <stdbool.h>
#include <netinet/in.h>
#include <openssl/sha.h>
#include <openssl/ssl.h>
#include <glib.h>
#include <elist.h>
#include <chunk_msg.h>

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
#endif

#define ADDRSIZE	24	/* Enough for IPv6, including port. */

enum {
	STORAGED_PGSZ_INODE	= 1024,
	STORAGED_PGSZ_SESSION	= 512,
	STORAGED_PGSZ_LOCK	= 512,

	MAX_COOKIE_LEN		= 128,

	CLI_REQ_BUF_SZ		= 8192,		/* buffer for req + hdrs */
	CLI_DATA_BUF_SZ		= 16 * 1024,

	STD_COOKIE_MIN		= 7,

	STD_TRASH_MAX		= 1000,

	CLI_MAX_SENDFILE_SZ	= 512 * 1024,
};

struct client;
struct client_write;
struct server_socket;

typedef bool (*cli_evt_func)(struct client *, unsigned int);
typedef bool (*cli_write_func)(struct client *, struct client_write *, bool);

struct timer {
	bool			fired;
	bool			on_list;
	void			(*cb)(struct timer *);
	void			*userdata;
	time_t			expires;
	char			name[32];
};

struct client_write {
	const void		*buf;		/* write buffer */
	int			len;		/* write buffer length */
	cli_write_func		cb;		/* callback */
	void			*cb_data;	/* data passed to cb */
	bool			sendfile;	/* using sendfile? */

	struct list_head	node;
};

/* internal client socket state */
enum client_state {
	evt_read_fixed,				/* read fixed-len rec */
	evt_read_var,				/* read variable-len rec */
	evt_exec_req,				/* execute request */
	evt_data_in,				/* request's content */
	evt_dispose,				/* dispose of client */
	evt_recycle,				/* restart HTTP request parse */
	evt_ssl_accept,				/* SSL cxn negotiation */
};

struct client {
	enum client_state	state;		/* socket state */

	struct sockaddr_in6	addr;		/* inet address */
	char			addr_host[64];	/* ASCII version of inet addr */
	int			fd;		/* socket */

	char			user[CHD_USER_SZ + 1];

	size_t			table_len;
	uint32_t		table_id;

	SSL			*ssl;
	bool			read_want_write;
	bool			write_want_read;

	struct list_head	write_q;	/* list of async writes */
	bool			writing;

	struct chunksrv_req	creq;
	unsigned int		req_used;	/* amount of req_buf in use */
	void			*req_ptr;	/* start of unexamined data */
	uint16_t		key_len;

	char			*hdr_start;	/* current hdr start */
	char			*hdr_end;	/* current hdr end (so far) */

	char			*out_user;
	SHA_CTX			out_hash;
	long			out_len;

	struct backend_obj	*out_bo;

	long			in_len;
	struct backend_obj	*in_obj;

	/* we put the big arrays and objects at the end... */

	char			netbuf[CLI_DATA_BUF_SZ];
	char			netbuf_out[CLI_DATA_BUF_SZ];
	char			key[CHD_KEY_SZ];
	char			table[CHD_KEY_SZ];
};

struct backend_obj {
	void			*private;
	void			*key;
	size_t			key_len;

	uint64_t		size;
	time_t			mtime;
	char			hashstr[50];
};

enum st_cld {
	ST_CLD_INIT, ST_CLD_ACTIVE
};

struct listen_cfg {
	char			*node;
	char			*port;
	char			*port_file;
	bool			encrypt;
};

struct geo {
	char			*area;
	char			*zone;		/* Building */
	char			*rack;
};

struct volume_entry {
	unsigned long long	size;		/* obj size */
	time_t			mtime;		/* obj last-mod time */
	void			*key;		/* obj id */
	int			key_len;
	char			*hash;		/* obj SHA1 checksum */
	char			*owner;		/* obj owner username */
};

struct server_stats {
	unsigned long		poll;		/* number polls */
	unsigned long		event;		/* events dispatched */
	unsigned long		tcp_accept;	/* TCP accepted cxns */
	unsigned long		opt_write;	/* optimistic writes */
};

struct server_poll {
	int			fd;		/* fd to poll for events */
	short			events;		/* POLL* from poll.h */
	bool			busy;		/* if true, do not poll us */

						/* callback function, data */
	bool			(*cb)(int fd, short events, void *userdata);
	void			*userdata;
};

struct server_socket {
	int			fd;
	const struct listen_cfg	*cfg;
};

struct server {
	unsigned long		flags;		/* SFL_xxx above */

	char			*config;	/* master config file */
	char			*pid_file;	/* PID file */
	int			pid_fd;

	GList			*listeners;
	GList			*sockets;	/* points into listeners */

	GHashTable		*fd_info;

	struct list_head	wr_trash;
	unsigned int		trash_sz;

	char			*ourhost;
	char			*vol_path;
	char			*cell;
	uint32_t		nid;
	struct geo		loc;

	struct server_stats	stats;		/* global statistics */
};

/* be-fs.c */
extern struct backend_obj *fs_obj_new(uint32_t table_id, const void *kbuf, size_t klen,
				      enum chunk_errcode *err_code);
extern struct backend_obj *fs_obj_open(uint32_t table_id, const char *user,
				       const void *kbuf, size_t klen,
				       enum chunk_errcode *err_code);
extern ssize_t fs_obj_write(struct backend_obj *bo, const void *ptr, size_t len);
extern ssize_t fs_obj_read(struct backend_obj *bo, void *ptr, size_t len);
extern void fs_obj_free(struct backend_obj *bo);
extern bool fs_obj_write_commit(struct backend_obj *bo, const char *user,
				const char *hashstr, bool sync_data);
extern bool fs_obj_delete(uint32_t table_id, const char *user,
		          const void *kbuf, size_t klen,
			  enum chunk_errcode *err_code);
extern ssize_t fs_obj_sendfile(struct backend_obj *bo, int out_fd, size_t len);
extern GList *fs_list_objs(uint32_t table_id, const char *user);
extern bool fs_table_open(const char *user, const void *kbuf, size_t klen,
		   bool tbl_creat, bool excl_creat, uint32_t *table_id,
		   enum chunk_errcode *err_code);

/* object.c */
extern bool object_del(struct client *cli);
extern bool object_put(struct client *cli);
extern bool object_get(struct client *cli, bool want_body);
extern bool cli_evt_data_in(struct client *cli, unsigned int events);
extern void cli_out_end(struct client *cli);
extern void cli_in_end(struct client *cli);

/* cldu.c */
extern void cldu_add_host(const char *host, unsigned int port);
extern int cld_begin(const char *thishost, const char *thiscell, uint32_t nid,
		     struct geo *locp, void (*cb)(enum st_cld));
extern void cld_end(void);

/* util.c */
extern size_t strlist_len(GList *l);
extern void __strlist_free(GList *l);
extern void strlist_free(GList *l);
extern void syslogerr(const char *prefix);
extern void strup(char *s);
extern int write_pid_file(const char *pid_fn);
extern int fsetflags(const char *prefix, int fd, int or_flags);
extern void timer_add(struct timer *timer, time_t expires);
extern void timer_del(struct timer *timer);
extern time_t timers_run(void);
extern char *time2str(char *strbuf, time_t time);
extern void hexstr(const unsigned char *buf, size_t buf_len, char *outstr);

static inline void timer_init(struct timer *timer, const char *name,
			      void (*cb)(struct timer *),
			      void *userdata)
{
	memset(timer, 0, sizeof(*timer));
	timer->cb = cb;
	timer->userdata = userdata;
	strncpy(timer->name, name, sizeof(timer->name));
	timer->name[sizeof(timer->name) - 1] = 0;
}

/* server.c */
extern SSL_CTX *ssl_ctx;
extern int debugging;
extern struct server chunkd_srv;
extern void applog(int prio, const char *fmt, ...);
extern bool cli_err(struct client *cli, enum chunk_errcode code, bool recycle_ok);
extern int cli_writeq(struct client *cli, const void *buf, unsigned int buflen,
		     cli_write_func cb, void *cb_data);
extern bool cli_wr_sendfile(struct client *, cli_write_func);
extern bool cli_wr_set_poll(struct client *cli, bool writable);
extern bool cli_cb_free(struct client *cli, struct client_write *wr,
			bool done);
extern bool cli_write_start(struct client *cli);
extern int cli_req_avail(struct client *cli);
extern int cli_poll_mod(struct client *cli);
extern bool srv_poll_del(int fd);
extern bool srv_poll_ready(int fd);
extern void resp_init_req(struct chunksrv_resp *resp,
		   const struct chunksrv_req *req);

/* config.c */
extern void read_config(void);

static inline bool use_sendfile(struct client *cli)
{
#if defined(HAVE_SENDFILE) && defined(HAVE_SYS_SENDFILE_H)
	return cli->ssl ? false : true;
#else
	return false;
#endif
}

#ifndef HAVE_STRNLEN
extern size_t strnlen(const char *s, size_t maxlen);
#endif

#ifndef HAVE_DAEMON
extern int daemon(int nochdir, int noclose);
#endif

#endif /* __STORAGED_H__ */
