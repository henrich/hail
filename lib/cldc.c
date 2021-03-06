
/*
 * Copyright 2009 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#define _GNU_SOURCE
#include "hail-config.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <stdarg.h>
#include <syslog.h>
#include <poll.h>
#include <openssl/sha.h>
#include <openssl/hmac.h>
#include <glib.h>
#include <cld-private.h>
#include <cldc.h>
#include <cld_msg_rpc.h>
#include <ncld.h>
#include <hail_private.h>

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

enum {
	CLDC_MSG_EXPIRE		= 5 * 60,
	CLDC_MSG_SCAN		= 60,
	CLDC_MSG_RETRY		= 5,
	CLDC_MSG_REMEMBER	= 25,
	CLDC_SESS_EXPIRE	= 2 * 60,
};

static const char *user_key(struct cldc_session *sess, const char *user);
static int sess_send_pkt(struct cldc_session *sess,
			const void *pkt, size_t pkt_len);

#ifndef HAVE_STRNLEN
static size_t strnlen(const char *s, size_t maxlen)
{
	int len = 0;

	if (!s)
		return 0;

	while ((len < maxlen) && (*s)) {
		s++;
		len++;
	}

	return len;
}
#endif
#ifndef EBADRQC
#define EBADRQC 56
#endif
#ifndef EBADSLT
#define EBADSLT 57
#endif
#ifndef EBADE
#define EBADE 52
#endif

void cldc_copts_get_data(const struct cldc_call_opts *copts,
			     char **data, size_t *data_len)
{
	*data = copts->resp.data.data_val;
	*data_len = copts->resp.data.data_len;
}

void cldc_copts_get_metadata(const struct cldc_call_opts *copts,
			     struct cldc_node_metadata *md)
{
	md->inum = copts->resp.inum;
	md->vers = copts->resp.vers;
	md->time_create = copts->resp.time_create;
	md->time_modify = copts->resp.time_modify;
	md->flags = copts->resp.flags;
	md->inode_name = copts->resp.inode_name;
}

static void cldc_errlog(int prio, const char *fmt, ...)
{
	char buf[200];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, 200, fmt, ap);
	fprintf(stderr, "%s\n", buf);
	va_end(ap);
}

static int ack_seqid(struct cldc_session *sess, uint64_t seqid_le)
{
	XDR xdrs;
	size_t hdr_len, total_len;
	char buf[CLD_MAX_PKT_MSG_SZ];
	struct cld_pkt_hdr pkt;
	struct cld_pkt_ftr *foot;
	int ret;
	static const char * const magic = CLD_PKT_MAGIC;
	const char *secret_key;

	/* Construct ACK packet */
	memset(&pkt, 0, sizeof(struct cld_pkt_hdr));
	memcpy(&pkt.magic, magic, sizeof(pkt.magic));
	memcpy(&pkt.sid, sess->sid, CLD_SID_SZ);
	pkt.user = sess->user;
	pkt.mi.order = CLD_PKT_ORD_FIRST_LAST;
	pkt.mi.cld_pkt_msg_info_u.mi.xid = 0;
	pkt.mi.cld_pkt_msg_info_u.mi.op = CMO_ACK;

	/* Serialize packet */
	xdrmem_create(&xdrs, (char *)buf,
		      sizeof(buf) - CLD_PKT_FTR_LEN, XDR_ENCODE);
	if (!xdr_cld_pkt_hdr(&xdrs, &pkt)) {
		HAIL_DEBUG(&sess->log, "%s: failed to encode header "
			   "for ack_seqid %llu",
			   __func__,
			   (unsigned long long) seqid_le);
		xdr_destroy(&xdrs);
		return -1009;
	}

	/* Fill in footer */
	hdr_len = xdr_getpos(&xdrs);
	total_len = hdr_len + CLD_PKT_FTR_LEN;
	foot = (struct cld_pkt_ftr *)(buf + hdr_len);
	foot->seqid = seqid_le;
	xdr_destroy(&xdrs);

	secret_key = user_key(sess, sess->user);
	ret = cld_authsign(&sess->log, secret_key,
			     buf, total_len - SHA_DIGEST_LENGTH, foot->sha);
	if (ret) {
		HAIL_ERR(&sess->log, "%s: authsign failed: %d",
			 __func__, ret);
		return ret;
	}

	return sess_send_pkt(sess, buf, total_len);
}

static int rxmsg_generic(struct cldc_session *sess,
			 const struct cld_pkt_hdr *pkt,
			 const struct cld_pkt_ftr *foot)
{
	XDR xdrs;
	struct cld_msg_generic_resp resp;
	struct cldc_msg *req = NULL;
	GList *tmp;

	xdrmem_create(&xdrs, sess->msg_buf, sess->msg_buf_len, XDR_DECODE);
	if (!xdr_cld_msg_generic_resp(&xdrs, &resp)) {
		HAIL_DEBUG(&sess->log, "%s: failed to decode "
			   "cld_msg_generic_resp", __func__);
		xdr_destroy(&xdrs);
		return -1008;
	}
	xdr_destroy(&xdrs);

	/* Find out which outbound message this was a response to */
	tmp = sess->out_msg;
	while (tmp) {
		req = tmp->data;

		HAIL_VERBOSE(&sess->log, "%s: comparing req->xid (%llu) "
			     "with resp.xid_in (%llu)", __func__,
			     (unsigned long long) req->xid,
			     (unsigned long long) resp.xid_in);

		if (req->xid == resp.xid_in)
			break;
		tmp = tmp->next;
	}
	if (!tmp) {
		HAIL_VERBOSE(&sess->log, "%s: no match found with xid_in %llu",
			     __func__, (unsigned long long) resp.xid_in);
		return -1005;
	}

	if (req->done) {
		HAIL_VERBOSE(&sess->log, "%s: re-acking", __func__);
	} else {
		HAIL_VERBOSE(&sess->log, "%s: issuing completion, acking",
			     __func__);

		req->done = true;

		if (req->cb) {
			ssize_t rc = req->cb(req, sess->msg_buf,
					     sess->msg_buf_len, resp.code);
			if (rc < 0)
				return rc;
		}
	}

	return ack_seqid(sess, foot->seqid);
}

static int rxmsg_ack_frag(struct cldc_session *sess,
			  const struct cld_pkt_hdr *pkt,
			  const struct cld_pkt_ftr *foot)
{
	XDR xdrs;
	struct cld_msg_ack_frag ack_msg;
	GList *tmp;

	xdrmem_create(&xdrs, sess->msg_buf, sess->msg_buf_len, XDR_DECODE);
	memset(&ack_msg, 0, sizeof(ack_msg));
	if (!xdr_cld_msg_ack_frag(&xdrs, &ack_msg)) {
		HAIL_INFO(&sess->log, "%s: failed to decode ack_msg",
			  __func__);
		xdr_destroy(&xdrs);
		return -1008;
	}
	xdr_destroy(&xdrs);

	HAIL_INFO(&sess->log, "%s: seqid %llu, want to ack",
		  __func__,
		  (unsigned long long) ack_msg.seqid);

	tmp = sess->out_msg;
	while (tmp) {
		struct cldc_msg *req;
		int i;

		req = tmp->data;
		tmp = tmp->next;

		for (i = 0; i < req->n_pkts; i++) {
			struct cldc_pkt_info *pi;
			struct cld_pkt_ftr *f;
			uint64_t seqid;

			pi = req->pkt_info[i];
			if (!pi)
				continue;
			f = (struct cld_pkt_ftr *)
				pi->data + (pi->pkt_len - CLD_PKT_FTR_LEN);
			seqid = le64_to_cpu(f->seqid);
			if (seqid != ack_msg.seqid)
				continue;

			HAIL_VERBOSE(&sess->log, "%s: seqid %llu, expiring",
				     __func__,
				     (unsigned long long) ack_msg.seqid);

			req->pkt_info[i] = NULL;
			free(pi);
		}
	}

	return 0;
}

static int rxmsg_event(struct cldc_session *sess,
		       const struct cld_pkt_hdr *pkt,
		       const struct cld_pkt_ftr *foot)
{
	XDR xdrs;
	struct cld_msg_event ev;
	struct cldc_fh *fh = NULL;
	GList *tmp;

	xdrmem_create(&xdrs, sess->msg_buf, sess->msg_buf_len, XDR_DECODE);
	if (!xdr_cld_msg_event(&xdrs, &ev)) {
		HAIL_DEBUG(&sess->log, "%s: failed to decode cld_msg_event",
			   __func__);
		xdr_destroy(&xdrs);
		return -1008;
	}
	xdr_destroy(&xdrs);

	for (tmp = sess->cfh; tmp; tmp = tmp->next) {
		fh = tmp->data;
		if (fh->fh == ev.fh)
			break;
		else
			fh = NULL;
	}

	if (!fh)
		return -1011;

	sess->ops->event(sess->private, sess, fh, ev.events);

	return 0;
}

static void cldc_msg_free(struct cldc_msg *msg)
{
	int i;

	if (!msg)
		return;

	for (i = 0; i < msg->n_pkts; i++)
		free(msg->pkt_info[i]);

	free(msg);
}

static void sess_expire_outmsg(struct cldc_session *sess, time_t current_time)
{
	GList *tmp, *tmp1;

	tmp = sess->out_msg;
	while (tmp) {
		struct cldc_msg *msg;

		tmp1 = tmp;
		tmp = tmp->next;

		msg = tmp1->data;
		if (current_time > msg->expire_time) {
			cldc_msg_free(msg);
			sess->out_msg = g_list_delete_link(sess->out_msg, tmp1);
		}
	}

	sess->msg_scan_time = current_time + CLDC_MSG_SCAN;
}

static const char *user_key(struct cldc_session *sess, const char *user)
{
	if (!sess || !user || !*user ||
	    (strnlen(user, CLD_MAX_USERNAME) >= CLD_MAX_USERNAME))
		return NULL;
	if (strcmp(sess->user, user))
		return NULL;

	return sess->secret_key;
}

static int rx_complete(struct cldc_session *sess,
		       const struct cld_pkt_hdr *pkt,
		       const struct cld_pkt_ftr *foot)
{
	switch (sess->msg_buf_op) {
	case CMO_ACK:
		HAIL_INFO(&sess->log, "%s: received unexpected ACK", __func__);
		return -EBADRQC;
	case CMO_PING:
		/* send out an ACK */
		return ack_seqid(sess, foot->seqid);
	case CMO_NOT_MASTER:
		HAIL_ERR(&sess->log, "FIXME: not-master message received");
		return -1055;	/* FIXME */
	case CMO_EVENT:
		return rxmsg_event(sess, pkt, foot);
	case CMO_ACK_FRAG:
		return rxmsg_ack_frag(sess, pkt, foot);
	default:
		return rxmsg_generic(sess, pkt, foot);
	}
}

/** Accepts a packet's sequence ID.
 * Depending on the message op, this may involve initializing the session's
 * sequence ID, validating that the packet's ID is in range, or doing nothing.
 *
 * @param sess		The session
 * @param seqid		The sequence ID
 * @param op		The message op
 *
 * @return		0 on success; error code otherwise
 */
static int accept_seqid(struct cldc_session *sess, uint64_t seqid,
			enum cld_msg_op op)
{
	switch (op) {
	case CMO_NEW_SESS:
		/* CMO_NEW_SESS initializes the session's sequence id */
		sess->next_seqid_in = seqid + 1;
		sess->next_seqid_in_tr =
			sess->next_seqid_in - CLDC_MSG_REMEMBER;
		HAIL_VERBOSE(&sess->log, "%s: setting next_seqid_in to %llu",
			     __func__, (unsigned long long) seqid);
		return 0;

	case CMO_NOT_MASTER:
	case CMO_ACK_FRAG:
		/* Ignore sequence ID of these types */
		return 0;

	default:
		/* verify that the sequence id is in range */
		if (seqid == sess->next_seqid_in) {
			sess->next_seqid_in++;
			sess->next_seqid_in_tr++;
			return 0;
		}

		if (seqid_in_range(seqid,
				   sess->next_seqid_in_tr,
				   sess->next_seqid_in)) {
			return 0;
		}

		return -EBADSLT;
	}
}

int cldc_receive_pkt(struct cldc_session *sess,
		     const void *net_addr, size_t net_addrlen,
		     const void *pktbuf, size_t pkt_len)
{
	const char *secret_key;
	struct timeval tv;
	time_t current_time;
	struct cld_pkt_hdr pkt;
	unsigned int hdr_len, msg_len;
	const struct cld_pkt_ftr *foot;
	uint64_t seqid;
	XDR xdrs;
	int ret;

	gettimeofday(&tv, NULL);
	current_time = tv.tv_sec;

	/* Decode the packet header */
	if (pkt_len < CLD_PKT_FTR_LEN) {
		HAIL_DEBUG(&sess->log, "%s: packet too short to have a "
			   "well-formed footer", __func__);
		return -EPROTO;
	}
	xdrmem_create(&xdrs, (void *)pktbuf,
			pkt_len - CLD_PKT_FTR_LEN, XDR_DECODE);
	memset(&pkt, 0, sizeof(pkt));
	if (!xdr_cld_pkt_hdr(&xdrs, &pkt)) {
		HAIL_DEBUG(&sess->log, "%s: failed to decode packet header",
			   __func__);
		xdr_destroy(&xdrs);
		return -EPROTO;
	}
	hdr_len = xdr_getpos(&xdrs);
	xdr_destroy(&xdrs);
	if (memcmp(&pkt.magic, CLD_PKT_MAGIC, sizeof(pkt.magic))) {
		HAIL_DEBUG(&sess->log, "%s: bad pkt magic", __func__);
		return -EPROTO;
	}

	/* check HMAC signature */
	foot = (const struct cld_pkt_ftr *)
		(((char *)pktbuf) + (pkt_len - CLD_PKT_FTR_LEN));
	secret_key = user_key(sess, pkt.user);
	ret = cld_authcheck(&sess->log, secret_key,
			      pktbuf, pkt_len - SHA_DIGEST_LENGTH, foot->sha);
	if (ret) {
		HAIL_DEBUG(&sess->log, "%s: invalid auth (ret=%d)",
			   __func__, ret);
		return -EACCES;
	}

	/* verify stored server addr matches pkt addr */
	if (((sess->addr_len != net_addrlen) ||
	    memcmp(sess->addr, net_addr, net_addrlen))) {
		HAIL_DEBUG(&sess->log, "%s: server address mismatch", __func__);
		return -EBADE;
	}

	/* expire old sess outgoing messages */
	if (current_time >= sess->msg_scan_time)
		sess_expire_outmsg(sess, current_time);

	if (pkt.mi.order & CLD_PKT_IS_FIRST) {
		/* This packet begins a new message.
		 * Determine the new message's op */
		sess->msg_buf_op = pkt.mi.cld_pkt_msg_info_u.mi.op;
	}

	/* verify (or set, for new-sess) sequence id */
	seqid = le64_to_cpu(foot->seqid);
	ret = accept_seqid(sess, seqid, sess->msg_buf_op);
	if (ret) {
		HAIL_DEBUG(&sess->log, "%s: bad seqid %llu",
			   __func__, (unsigned long long) seqid);
		return ret;
	}

	if (pkt.mi.order & CLD_PKT_IS_FIRST)
		sess->msg_buf_len = 0;
	msg_len = pkt_len - hdr_len - CLD_PKT_FTR_LEN;
	if ((sess->msg_buf_len + msg_len) > CLD_MAX_MSG_SZ) {
		HAIL_DEBUG(&sess->log, "%s: message too long", __func__);
		return -EPROTO;
	}
	memcpy(sess->msg_buf + sess->msg_buf_len, pktbuf + hdr_len, msg_len);
	sess->msg_buf_len += msg_len;
	sess->expire_time = current_time + CLDC_SESS_EXPIRE;

	if (pkt.mi.order & CLD_PKT_IS_LAST) {
		HAIL_VERBOSE(&sess->log, "%s: receiving complete message of "
			     "op %s", __func__, cld_opstr(sess->msg_buf_op));
		return rx_complete(sess, &pkt, foot);
	} else {
		return ack_seqid(sess, foot->seqid);
	}
}

static void sess_next_seqid(struct cldc_session *sess, uint64_t *seqid)
{
	uint64_t rc = cpu_to_le64(sess->next_seqid_out++);
	*seqid = rc;
}

/**
 * creates a new cldc_msg
 *
 * @param sess		The session
 * @param copts		The call options
 * @param op		The op of message to create
 * @param xdrproc	The XDR function to use to create the message body
 * @param data		The data to pass to xdrproc
 *
 * @return		The cldc message, or NULL on error,
 */
static struct cldc_msg *cldc_new_msg(struct cldc_session *sess,
				     const struct cldc_call_opts *copts,
				     enum cld_msg_op op,
				     xdrproc_t xdrproc, const void *data)
{
	struct cldc_msg *msg;
	struct timeval tv;
	size_t i, body_len, n_pkts;
	char *body;
	XDR xbdy;

	/* Encode the message body */
	body_len = xdr_sizeof(xdrproc, (void *)data);
	body = alloca(body_len);
	xdrmem_create(&xbdy, body, body_len, XDR_ENCODE);
	if (!xdrproc(&xbdy, (void *)data)) {
		HAIL_DEBUG(&sess->log, "%s: failed to encode "
			   "message", __func__);
		xdr_destroy(&xbdy);
		return NULL;
	}
	xdr_destroy(&xbdy);

	if (body_len == 0)
		/* Some packets (like ACKS) just have a header, and no message
		 * body. */
		n_pkts = 1;
	else {
		/* round up */
		n_pkts = (body_len + CLD_MAX_PKT_MSG_SZ - 1) /
			CLD_MAX_PKT_MSG_SZ;
	}

	/* Create cldc_msg */
	msg = calloc(1, sizeof(*msg) +
		        (n_pkts * sizeof(struct cldc_pkt_info *)));
	if (!msg)
		return NULL;

	msg->n_pkts = n_pkts;
	cld_rand64(&msg->xid);
	msg->op = op;
	msg->sess = sess;
	if (copts)
		memcpy(&msg->copts, copts, sizeof(msg->copts));
	gettimeofday(&tv, NULL);
	msg->expire_time = tv.tv_sec + CLDC_MSG_EXPIRE;

	for (i = 0; i < msg->n_pkts; i++) {
		XDR xhdr;
		struct cld_pkt_hdr pkt;
		struct cldc_pkt_info *pi;
		int hdr_len, body_chunk_len, pkt_len;

		/* Set up packet header */
		memcpy(&pkt.magic, CLD_PKT_MAGIC, sizeof(pkt.magic));
		memcpy(&pkt.sid, sess->sid, CLD_SID_SZ);
		pkt.user = sess->user;
		if (i == 0) {
			if (i == (msg->n_pkts - 1))
				pkt.mi.order = CLD_PKT_ORD_FIRST_LAST;
			else
				pkt.mi.order = CLD_PKT_ORD_FIRST;
			pkt.mi.cld_pkt_msg_info_u.mi.xid = msg->xid;
			pkt.mi.cld_pkt_msg_info_u.mi.op = op;
		} else {
			if (i == (msg->n_pkts - 1))
				pkt.mi.order = CLD_PKT_ORD_LAST;
			else
				pkt.mi.order = CLD_PKT_ORD_MID;
		}

		/* Allocate memory */
		hdr_len = xdr_sizeof((xdrproc_t)xdr_cld_pkt_hdr, &pkt);
		body_chunk_len = MIN(body_len, CLD_MAX_PKT_MSG_SZ);
		pkt_len = hdr_len + body_chunk_len + CLD_PKT_FTR_LEN;
		pi = calloc(1, sizeof(*pi) + pkt_len);
		if (!pi)
			goto err_out;
		pi->pkt_len = pkt_len;
		msg->pkt_info[i] = pi;
		strncpy(pi->user, sess->user, CLD_MAX_USERNAME - 1);

		/* Fill in the packet header */
		xdrmem_create(&xhdr, (char *)pi->data, hdr_len, XDR_ENCODE);
		if (!xdr_cld_pkt_hdr(&xhdr, &pkt)) {
			HAIL_DEBUG(&sess->log, "%s: failed to encode header "
				   "for packet %zu", __func__, i);
			xdr_destroy(&xhdr);
			goto err_out;
		}

		/* Fill in the body */
		memcpy(pi->data + hdr_len, body, body_chunk_len);
		body += body_chunk_len;
		body_len -= body_chunk_len;
	}

	return msg;

err_out:
	cldc_msg_free(msg);
	return NULL;
}

static void sess_msg_drop(struct cldc_session *sess)
{
	GList *tmp = sess->out_msg;
	struct cldc_msg *msg;

	while (tmp) {
		msg = tmp->data;
		tmp = tmp->next;

		if (!msg->done && msg->cb)
			msg->cb(msg, NULL, 0, CLE_TIMEOUT);

		cldc_msg_free(msg);
	}

	g_list_free(sess->out_msg);
	sess->out_msg = NULL;
}

static void sess_expire(struct cldc_session *sess)
{
	sess->expired = true;
	sess_msg_drop(sess);

	sess->ops->timer_ctl(sess->private, false, NULL, NULL, 0);

	sess->ops->event(sess->private, sess, NULL, CE_SESS_FAILED);
	/*
	 * Do not do sess_free here, or else the application is bound to
	 * step into use-after-free. Only call sess_free when requested.
	 * However, it may be requested right from this callback. Or later.
	 */
}

static int sess_send_pkt(struct cldc_session *sess,
			 const void *pkt, size_t pkt_len)
{
	HAIL_VERBOSE(&sess->log, "%s: sending %ld bytes",
		     __func__, (long)pkt_len);
	return sess->ops->pkt_send(sess->private,
				   sess->addr, sess->addr_len,
				   pkt, pkt_len);
}

static int sess_timer(struct cldc_session *sess, void *priv)
{
	GList *tmp = sess->out_msg;
	struct timeval tv;

	gettimeofday(&tv, NULL);
	if (tv.tv_sec > sess->expire_time) {
		sess_expire(sess);
		return 0;
	}

	while (tmp) {
		struct cldc_msg *msg;
		int i;

		msg = tmp->data;
		tmp = tmp->next;

		if (msg->done)
			continue;

		for (i = 0; i < msg->n_pkts; i++) {
			struct cldc_pkt_info *pi;

			pi = msg->pkt_info[i];
			if (!pi)
				continue;
			pi->retries++;
			sess_send_pkt(sess, pi->data, pi->pkt_len);
		}
	}

	sess->ops->timer_ctl(sess->private, true, sess_timer, sess,
			     CLDC_MSG_RETRY);
	return CLDC_MSG_RETRY;
}

static int sess_send(struct cldc_session *sess, struct cldc_msg *msg)
{
	int ret, i;
	const char *secret_key;

	secret_key = user_key(sess, sess->user);

	for (i = 0; i < msg->n_pkts; i++) {
		struct cldc_pkt_info *pi;
		struct cld_pkt_ftr *foot;

		pi = msg->pkt_info[i];

		/* Add the sequence number to the end of the packet */
		foot = (struct cld_pkt_ftr *)
			(pi->data + pi->pkt_len - CLD_PKT_FTR_LEN);
		memset(foot, 0, CLD_PKT_FTR_LEN);
		sess_next_seqid(sess, &foot->seqid);

		/* Add the signature to the end of the packet */
		ret = cld_authsign(&sess->log, secret_key,
				     pi->data,
				     pi->pkt_len - SHA_DIGEST_LENGTH,foot->sha);
		if (ret)
			return ret;

		/* attempt first send */
		if (sess_send_pkt(sess, pi->data, pi->pkt_len) < 0)
			return -EIO;
	}

	/* add to list of outgoing packets, waiting to be ack'd */
	sess->out_msg = g_list_prepend(sess->out_msg, msg);

	return 0;
}

static void sess_free(struct cldc_session *sess)
{
	GList *tmp;

	if (!sess)
		return;

	if (sess->cfh) {
		for (tmp = sess->cfh; tmp; tmp = tmp->next)
			free(tmp->data);
		g_list_free(sess->cfh);
	}

	tmp = sess->out_msg;
	while (tmp) {
		cldc_msg_free(tmp->data);
		tmp = tmp->next;
	}
	g_list_free(sess->out_msg);

	memset(sess, 0x55, sizeof(*sess));
	free(sess);
}

static ssize_t end_sess_cb(struct cldc_msg *msg, const void *resp_p,
			   size_t resp_len, enum cle_err_codes resp_rc)
{
	if (msg->copts.cb)
		return msg->copts.cb(&msg->copts, resp_rc);

	sess_free(msg->sess);
	return 0;
}

int cldc_end_sess(struct cldc_session *sess, const struct cldc_call_opts *copts)
{
	struct cldc_msg *msg;

	if (!sess->confirmed)
		return -EINVAL;

	/* create END-SESS message */
	msg = cldc_new_msg(sess, copts, CMO_END_SESS,
			   (xdrproc_t)xdr_void, NULL);
	if (!msg)
		return -ENOMEM;

	msg->cb = end_sess_cb;

	return sess_send(sess, msg);
}

static ssize_t new_sess_cb(struct cldc_msg *msg, const void *resp_p,
			   size_t resp_len, enum cle_err_codes resp_rc)
{
	if (resp_rc == CLE_OK)
		msg->sess->confirmed = true;

	if (msg->copts.cb)
		return msg->copts.cb(&msg->copts, resp_rc);

	return 0;
}

static int cldc_new_sess_log(const struct cldc_ops *ops,
			     const struct cldc_call_opts *copts,
			     const void *addr, size_t addr_len,
			     const char *user, const char *secret_key,
			     void *private, struct hail_log *log,
			     struct cldc_session **sess_out)
{
	struct cldc_session *sess;
	struct cldc_msg *msg;
	struct timeval tv;

	if (addr_len > sizeof(sess->addr))
		return -EINVAL;
	if (!user || !*user || !secret_key || !*secret_key)
		return -EINVAL;
	if (strlen(user) >= sizeof(sess->user))
		return -EINVAL;
	if (strlen(secret_key) >= sizeof(sess->secret_key))
		return -EINVAL;

	sess = calloc(1, sizeof(*sess));
	if (!sess)
		return -ENOMEM;

	sess->private = private;
	sess->ops = ops;
	sess->log = *log;		/* save off caller's stack */
	strcpy(sess->user, user);
	strcpy(sess->secret_key, secret_key);

	/* create random SID, next_seqid_out */
	cld_rand64(sess->sid);
	cld_rand64(&sess->next_seqid_out);

	/* init other session vars */
	memcpy(sess->addr, addr, addr_len);
	sess->addr_len = addr_len;

	/* create NEW-SESS message */
	msg = cldc_new_msg(sess, copts, CMO_NEW_SESS,
			   (xdrproc_t)xdr_void, NULL);
	if (!msg) {
		sess_free(sess);
		return -ENOMEM;
	}

	msg->cb = new_sess_cb;

	/* save session */
	*sess_out = sess;

	gettimeofday(&tv, NULL);
	sess->expire_time = tv.tv_sec + CLDC_SESS_EXPIRE;

	sess->ops->timer_ctl(sess->private, true, sess_timer, sess,
			     CLDC_MSG_RETRY);

	return sess_send(sess, msg);
}

int cldc_new_sess(const struct cldc_ops *ops,
		  const struct cldc_call_opts *copts,
		  const void *addr, size_t addr_len,
		  const char *user, const char *secret_key,
		  void *private,
		  struct cldc_session **sess_out)
{
	struct hail_log log;

	log.debug = false;
	log.verbose = false;
	log.func = cldc_errlog;
	return cldc_new_sess_log(ops, copts, addr, addr_len, user, secret_key,
				 private, &log, sess_out);
}

/*
 * Force-clean the slate in the library. This may leave the server confused.
 */
void cldc_kill_sess(struct cldc_session *sess)
{
	sess->ops->timer_ctl(sess->private, false, NULL, NULL, 0);
	sess_free(sess);
}

static ssize_t generic_end_cb(struct cldc_msg *msg, const void *resp_p,
			      size_t resp_len, enum cle_err_codes resp_rc)
{
	if (msg->copts.cb)
		return msg->copts.cb(&msg->copts, resp_rc);

	return 0;
}

int cldc_nop(struct cldc_session *sess, const struct cldc_call_opts *copts)
{
	struct cldc_msg *msg;

	if (!sess->confirmed)
		return -EINVAL;

	/* create NOP message */
	msg = cldc_new_msg(sess, copts, CMO_NOP,
			   (xdrproc_t)xdr_void, NULL);
	if (!msg)
		return -ENOMEM;

	msg->cb = generic_end_cb;

	return sess_send(sess, msg);
}

int cldc_del(struct cldc_session *sess, const struct cldc_call_opts *copts,
	     const char *pathname)
{
	struct cldc_msg *msg;
	struct cld_msg_del del;
	size_t plen;

	if (!sess->confirmed)
		return -EINVAL;

	/* first char must be slash */
	if (*pathname != '/')
		return -EINVAL;

	plen = strlen(pathname);
	if (plen > CLD_INODE_NAME_MAX)
		return -EINVAL;

	/* create DEL message */
	del.inode_name = (char *)pathname;
	msg = cldc_new_msg(sess, copts, CMO_DEL,
			   (xdrproc_t)xdr_cld_msg_del, &del);
	if (!msg)
		return -ENOMEM;

	msg->cb = generic_end_cb;

	return sess_send(sess, msg);
}

static ssize_t open_end_cb(struct cldc_msg *msg, const void *resp_p,
			   size_t resp_len, enum cle_err_codes resp_rc)
{
	if (resp_rc == CLE_OK) {
		struct cldc_fh *fh = msg->cb_private;
		XDR xdrs;
		struct cld_msg_open_resp resp;

		xdrmem_create(&xdrs, (void *)resp_p, resp_len, XDR_DECODE);
		memset(&resp, 0, sizeof(resp));
		if (!xdr_cld_msg_open_resp(&xdrs, &resp)) {
			xdr_destroy(&xdrs);
			return -1009;
		}

		fh->fh = resp.fh;
		fh->valid = true;
	}

	if (msg->copts.cb)
		return msg->copts.cb(&msg->copts, resp_rc);

	return 0;
}

int cldc_open(struct cldc_session *sess,
	      const struct cldc_call_opts *copts,
	      const char *pathname, uint32_t open_mode,
	      uint32_t events, struct cldc_fh **fh_out)
{
	struct cldc_msg *msg;
	struct cld_msg_open open;
	struct cldc_fh *fh;
	size_t plen;

	*fh_out = NULL;

	if (!sess->confirmed)
		return -EINVAL;

	/* first char must be slash */
	if (*pathname != '/')
		return -EINVAL;

	plen = strlen(pathname);
	if (plen > CLD_INODE_NAME_MAX)
		return -EINVAL;

	/* create OPEN message */
	open.mode = open_mode;
	open.events = events;
	open.inode_name = (char *)pathname;
	msg = cldc_new_msg(sess, copts, CMO_OPEN,
			   (xdrproc_t)xdr_cld_msg_open, &open);
	if (!msg)
		return -ENOMEM;

	fh = calloc(1, sizeof(*fh));
	if (!fh) {
		cldc_msg_free(msg);
		return -ENOMEM;
	}
	fh->sess = sess;
	sess->cfh = g_list_append(sess->cfh, fh);	// kept open - to front

	msg->cb = open_end_cb;
	msg->cb_private = fh;

	*fh_out = fh;

	return sess_send(sess, msg);
}

static ssize_t close_end_cb(struct cldc_msg *msg, const void *resp_p,
			      size_t resp_len, enum cle_err_codes resp_rc)
{
	struct cldc_fh *fh = msg->cb_private;
	struct cldc_session *sess = fh->sess;

	if (msg->copts.cb)
		return msg->copts.cb(&msg->copts, resp_rc);

	sess->cfh = g_list_remove(sess->cfh, fh);
	memset(fh, 0x77, sizeof(*fh));
	free(fh);

	return 0;
}

int cldc_close(struct cldc_fh *fh, const struct cldc_call_opts *copts)
{
	struct cldc_session *sess;
	struct cldc_msg *msg;
	struct cld_msg_close close_msg;

	if (!fh->valid)
		return -EINVAL;

	sess = fh->sess;
	if (sess->expired) {
		fh->valid = false;
		return -EPIPE;
	}

	/* create CLOSE message */
	close_msg.fh = fh->fh;
	msg = cldc_new_msg(sess, copts, CMO_CLOSE,
			   (xdrproc_t)xdr_cld_msg_close, &close_msg);
	if (!msg)
		return -ENOMEM;

	/* mark FH as invalid from this point forward */
	fh->valid = false;

	msg->cb = close_end_cb;
	msg->cb_private = fh;

	return sess_send(sess, msg);
}

int cldc_lock(struct cldc_fh *fh, const struct cldc_call_opts *copts,
	      uint32_t lock_flags, bool wait_for_lock)
{
	struct cldc_session *sess;
	struct cldc_msg *msg;
	struct cld_msg_lock lock;

	if (!fh->valid)
		return -EINVAL;

	sess = fh->sess;

	/* create LOCK message */
	lock.fh = fh->fh;
	lock.flags = lock_flags;
	msg = cldc_new_msg(sess, copts,
			   wait_for_lock ? CMO_LOCK : CMO_TRYLOCK,
			   (xdrproc_t)xdr_cld_msg_lock, &lock);
	if (!msg)
		return -ENOMEM;

	msg->cb = generic_end_cb;

	return sess_send(sess, msg);
}

int cldc_unlock(struct cldc_fh *fh, const struct cldc_call_opts *copts)
{
	struct cldc_session *sess;
	struct cldc_msg *msg;
	struct cld_msg_unlock unlock;

	if (!fh->valid)
		return -EINVAL;

	sess = fh->sess;

	/* create UNLOCK message */
	unlock.fh = fh->fh;
	msg = cldc_new_msg(sess, copts, CMO_UNLOCK,
			   (xdrproc_t)xdr_cld_msg_unlock, &unlock);
	if (!msg)
		return -ENOMEM;

	msg->cb = generic_end_cb;

	return sess_send(sess, msg);
}

int cldc_put(struct cldc_fh *fh, const struct cldc_call_opts *copts,
	     const void *data, size_t data_len)
{
	struct cldc_session *sess;
	struct cldc_msg *msg;
	struct cld_msg_put put;

	if (!data || !data_len || data_len > CLD_MAX_PAYLOAD_SZ)
		return -EINVAL;

	if (!fh->valid)
		return -EINVAL;

	sess = fh->sess;

	/* create PUT message */
	put.fh = fh->fh;
	put.data.data_len = data_len;
	put.data.data_val = (char *)data;
	msg = cldc_new_msg(sess, copts, CMO_PUT,
			   (xdrproc_t)xdr_cld_msg_put, &put);
	if (!msg)
		return -ENOMEM;

	msg->cb = generic_end_cb;

	sess_send(sess, msg);

	return 0;
}

static ssize_t get_end_cb(struct cldc_msg *msg, const void *resp_p,
			  size_t resp_len, enum cle_err_codes resp_rc)
{
	if (resp_rc == CLE_OK) {
		XDR xin;
		struct cld_msg_get_resp *resp = &msg->copts.resp;

		/* Parse GET response.
		 * Avoid memory allocation in xdr_string by pointing
		 * variable-length elements at static buffers. */
		xdrmem_create(&xin, (void *)resp_p, resp_len, XDR_DECODE);
		memset(resp, 0, sizeof(struct cld_msg_get_resp));
		resp->inode_name = msg->sess->inode_name_temp;
		resp->data.data_val = msg->sess->payload;
		resp->data.data_len = 0;
		if (!xdr_cld_msg_get_resp(&xin, resp)) {
			xdr_destroy(&xin);
			return -1009;
		}
		xdr_destroy(&xin);
	}

	if (msg->copts.cb)
		return msg->copts.cb(&msg->copts, resp_rc);
	return 0;
}

int cldc_get(struct cldc_fh *fh, const struct cldc_call_opts *copts,
	     bool metadata_only)
{
	struct cldc_session *sess;
	struct cldc_msg *msg;
	struct cld_msg_get get;

	if (!fh->valid)
		return -EINVAL;

	sess = fh->sess;

	/* create GET message */
	get.fh = fh->fh;
	msg = cldc_new_msg(sess, copts, CMO_GET,
			   (xdrproc_t)xdr_cld_msg_get, &get);
	if (!msg)
		return -ENOMEM;

	msg->cb = get_end_cb;

	return sess_send(sess, msg);
}

int cldc_dirent_count(const void *data, size_t data_len)
{
	const void *p = data;
	size_t tmp_len = data_len;
	size_t str_len, rec_len, pad, total_len;
	const uint16_t *tmp16;
	int count = 0;

	while (tmp_len > 0) {
		if (tmp_len < 2)
			return -2;

		tmp16		= p;
		str_len		= le16_to_cpu(*tmp16);
		rec_len		= str_len + 2;
		pad		= CLD_ALIGN8(rec_len);
		total_len	= rec_len + pad;

		if (total_len > tmp_len)
			return -2;

		count++;

		p += total_len;
		tmp_len -= total_len;
	}

	return count;
}

static int dirent_length(const void *buf, size_t buflen, size_t *str_len_out)
{
	size_t str_len, rec_len, pad, total_len;
	const uint16_t *tmp16;

	if (buflen < 2)
		return -1;

	tmp16		= buf;
	str_len		= le16_to_cpu(*tmp16);
	rec_len		= str_len + 2;
	pad		= CLD_ALIGN8(rec_len);
	total_len	= rec_len + pad;

	if (total_len > buflen)
		return -1;

	if (str_len_out)
		*str_len_out = str_len;

	return total_len;
}

int cldc_dirent_first(struct cld_dirent_cur *dc)
{
	int dirent_len;

	dirent_len = dirent_length(dc->p, dc->tmp_len, NULL);
	if (dirent_len < 0)
		return -2;

	return 0;
}

int cldc_dirent_next(struct cld_dirent_cur *dc)
{
	int dirent_len;

	dirent_len = dirent_length(dc->p, dc->tmp_len, NULL);
	if (dirent_len < 0)
		return -2;

	dc->p += dirent_len;
	dc->tmp_len -= dirent_len;

	dirent_len = dirent_length(dc->p, dc->tmp_len, NULL);
	if (dirent_len < 0)
		return -2;

	return 0;
}

void cldc_dirent_cur_init(struct cld_dirent_cur *dc, const void *buf, size_t buflen)
{
	memset(dc, 0, sizeof(*dc));
	dc->p = buf;
	dc->tmp_len = buflen;
}

void cldc_dirent_cur_fini(struct cld_dirent_cur *dc)
{
	/* do nothing */
}

char *cldc_dirent_name(struct cld_dirent_cur *dc)
{
	const uint16_t *tmp16 = dc->p;
	size_t str_len = le16_to_cpu(*tmp16);
	char *s;

	s = malloc(str_len + 1);
	if (!s)
		return NULL;

	memcpy(s, dc->p + 2, str_len);
	s[str_len] = 0;

	return s;
}

/*
 * On error, return the code (not negated code like a kernel function would).
 */
static int ncld_getsrv(char **hostp, unsigned short *portp,
		       struct hail_log *log)
{
	enum { hostsz = 64 };
	char hostb[hostsz];
	GList *host_list = NULL;
	GList *tmp;
	struct cldc_host *hp;

	if (gethostname(hostb, hostsz-1) < 0)
		return errno;
	hostb[hostsz-1] = 0;

	if (cldc_getaddr(&host_list, hostb, log))
		return 1001;

	/*
	 * This is a good place to compare weights and priorities, maybe later.
	 * For now, just grab first host in the list.
	 */
	hp = host_list->data;		/* cannot be NULL if success above */
	*hostp = hp->host;
	*portp = hp->port;

	for (tmp = host_list; tmp; tmp = tmp->next) {
		hp = tmp->data;
		free(hp->host);
		free(hp);
	}
	g_list_free(host_list);
	return 0;
}

static int ncld_gethost(char **hostp, unsigned short *portp,
			const char *hostname, int port)
{
	if (port <= 0 || port >= 65536)
		return EINVAL;
	*portp = port;

	if (!(*hostp = strdup(hostname)))
		return ENOMEM;

	return 0;
}

static void ncld_tcp_timer_event(struct cld_timer *timer)
{
	struct ncld_sess *nsess = timer->userdata;
	struct cldc_tcp *tcp = nsess->tcp;

	if (tcp->cb)
		tcp->cb(tcp->sess, tcp->cb_private);
}

enum {
	NCLD_CMD_END = 0,
	NCLD_CMD_SESEV		/* argument - 4 bytes in host order */
};

/*
 * All the error printouts are likely to be lost for daemons, but it's
 * not a big deal. We abort instead in order to indicate that something
 * went wrong, so system features should report it (usualy as a core).
 * When debugging, strace or -F mode will capture the output.
 */
static void ncld_thread_command(struct ncld_sess *nsess)
{
	ssize_t rrc;
	unsigned char cmd;
	uint32_t what;

	rrc = read(nsess->to_thread[0], &cmd, 1);
	if (rrc < 0) {
		fprintf(stderr, "read error: %s\n", strerror(errno));
		abort();
	}
	if (rrc < 1) {
		fprintf(stderr, "short read\n");
		abort();
	}

	switch (cmd) {
	case NCLD_CMD_END:
		/* No answer to requestor. Wait with g_thread_join. */
		g_thread_exit(NULL);
		break;
	case NCLD_CMD_SESEV:
		rrc = read(nsess->to_thread[0], &what, sizeof(uint32_t));
		if (rrc < sizeof(uint32_t)) {
			fprintf(stderr, "bad read param\n");
			g_thread_exit(NULL);
		}
		if (nsess->event)
			nsess->event(nsess->event_arg, what);
		break;
	default:
		fprintf(stderr, "bad command 0x%x\n", cmd);
		abort();
	}
}

static gpointer ncld_sess_thr(gpointer data)
{
	struct ncld_sess *nsess = data;
	struct pollfd pfd[2];
	time_t tmo;
	int i;
	int rc;

	for (;;) {
		g_mutex_lock(nsess->mutex);
		tmo = cld_timers_run(&nsess->tlist);
		g_mutex_unlock(nsess->mutex);

		memset(pfd, 0, sizeof(pfd));
		pfd[0].fd = nsess->to_thread[0];
		pfd[0].events = POLLIN;
		pfd[1].fd = nsess->tcp->fd;
		pfd[1].events = POLLIN;

		rc = poll(pfd, 2, tmo ? tmo*1000 : -1);
		if (rc == 0)
			continue;
		if (rc < 0) {
			fprintf(stderr, "poll error: %s\n", strerror(errno));
			abort();
		}

		for (i = 0; i < ARRAY_SIZE(pfd); i++) {
			if (pfd[i].revents) {
				if (i == 0) {
					ncld_thread_command(nsess);
				} else {
					g_mutex_lock(nsess->mutex);
					cldc_tcp_receive_pkt_data(nsess->tcp);
					g_mutex_unlock(nsess->mutex);
				}
			}
		}
	}

	return NULL;
}

/*
 * Ask the thread to exit and wait until it does.
 */
static void ncld_thr_end(struct ncld_sess *nsess)
{
	unsigned char cmd;

	cmd = NCLD_CMD_END;
	write(nsess->to_thread[1], &cmd, 1);
	g_thread_join(nsess->thread);
}

static bool ncld_p_timer_ctl(void *priv, bool add,
			     int (*cb)(struct cldc_session *, void *),
			     void *cb_priv, time_t secs)
{
	struct ncld_sess *nsess = priv;
	struct cldc_tcp *tcp = nsess->tcp;

	if (add) {
		tcp->cb = cb;
		tcp->cb_private = cb_priv;
		cld_timer_add(&nsess->tlist, &nsess->tcp_timer, time(NULL) + secs);
	} else {
		cld_timer_del(&nsess->tlist, &nsess->tcp_timer);
	}
	return true;
}

static int ncld_p_pkt_send(void *priv, const void *addr, size_t addrlen,
			       const void *buf, size_t buflen)
{
	struct ncld_sess *nsess = priv;
	return cldc_tcp_pkt_send(nsess->tcp, addr, addrlen, buf, buflen);
}

static void ncld_p_event(void *priv, struct cldc_session *csp,
			 struct cldc_fh *fh, uint32_t what)
{
	struct ncld_sess *nsess = priv;
	unsigned char cmd;

	if (what == CE_SESS_FAILED) {
		if (nsess->tcp->sess != csp)
			abort();
		if (!nsess->is_up)
			return;
		nsess->is_up = false;

		/*
		 * The cldc layer must deliver the callbacks for all pending
		 * CLD operations of the failed session. If we force-wake their
		 * waiters, all sorts of funny things happen with the lifetimes
		 * of related structures.
		 */
		// 
		// g_cond_broadcast(nsess->cond);

		/*
		 * This is a trick. As a direct callback from clcd layer,
		 * we are running under nsess->mutex, so we cannot call back
		 * into a user of ncld. If we do, it may invoke another
		 * ncld operation and deadlock. So, bump session callbacks
		 * into the part of the helper thread that runs unlocked.
		 *
		 * Notice that we are already running on the context of the
		 * thread that will deliver the event, so pipe really is not
		 * needed: could as well set a flag and test it right after
		 * the call to cldc_tcp_receive_pkt_data(). But pipe also provides
		 * a queue of events, just in case. It's not like these events
		 * are super-performance critical.
		 */
		cmd = NCLD_CMD_SESEV;
		write(nsess->to_thread[1], &cmd, 1);
		write(nsess->to_thread[1], &what, sizeof(what));
	}
}

static struct cldc_ops ncld_ops = {
	.timer_ctl	= ncld_p_timer_ctl,
	.pkt_send	= ncld_p_pkt_send,
	.event		= ncld_p_event,
};

static int ncld_new_sess(struct cldc_call_opts *copts, enum cle_err_codes errc)
{
	struct ncld_sess *nsess = copts->private;

	/*
	 * All callbacks from cldc layer run on the context of the thread
	 * with nsess->mutex locked, so we don't lock it again here.
	 *
	 * We set is_up on the context that is invoked from cldc,
	 * so the flag has a sane meaning in case we immediately
	 * get a session failure event.
	 */
	nsess->errc = errc;
	nsess->open_done = true;
	if (!errc)
		nsess->is_up = true;
	g_cond_broadcast(nsess->cond);
	return 0;
}

static int ncld_wait_session(struct ncld_sess *nsess)
{
	g_mutex_lock(nsess->mutex);
	while (!nsess->open_done)
		g_cond_wait(nsess->cond, nsess->mutex);
	g_mutex_unlock(nsess->mutex);
	return nsess->errc;
}

/*
 * Initiating session may take a while, since we retry failures.
 * We promise that eventually we return from this function.
 *
 * On error, returns NULL and sets the error code (system errno if below 1000,
 * otherwise our own mysterious codes that you should just print in decimal).
 *
 * Keep in mind that the (*event)() is likely to be called on a context
 * of the extra thread that we spawn. At least we won't steal some random
 * context and monkey with signals. Also, (*event)() may be called before
 * this function returns, with the session going down, for example.
 * This is kind of dirty, but oh well. Maybe we'll fix this later.
 *
 * @param host Host name (NULL if resolving SRV records)
 * @param port Port
 * @param error Buffer for the error code
 * @param ev_func Session event function (ok to be NULL)
 * @param ev_arg User-supplied argument to the session event function
 * @param cld_user The user identifier to be used to authentication
 * @param cld_key The user key to be used to authentication
 * @param log The application log descriptor (ok to be NULL)
 */
struct ncld_sess *ncld_sess_open(const char *host, int port, int *error,
				 void (*ev_func)(void *, unsigned int),
				 void *ev_arg,
				 const char *cld_user, const char *cld_key,
				 struct hail_log *log)
{
	struct ncld_sess *nsess;
	struct hail_log nlog;
	struct cldc_call_opts copts;
	int err;
	GError *gerr;
	int rc;

	if (!log) {
		nlog.debug = false;
		nlog.verbose = false;
		nlog.func = cldc_errlog;
		log = &nlog;
	}

	err = ENOMEM;
	nsess = malloc(sizeof(struct ncld_sess));
	if (!nsess)
		goto out_sesalloc;
	memset(nsess, 0, sizeof(struct ncld_sess));
	cld_timer_init(&nsess->tcp_timer, "nsess-tcp-timer",
		       ncld_tcp_timer_event, nsess);
	nsess->mutex = g_mutex_new();
	if (!nsess->mutex)
		goto out_mutex;
	nsess->cond = g_cond_new();
	if (!nsess->cond)
		goto out_cond;

	if (!host) {
		err = ncld_getsrv(&nsess->host, &nsess->port, log);
		if (err)
			goto out_srv;
	} else {
		err = ncld_gethost(&nsess->host, &nsess->port, host, port);
		if (err)
			goto out_srv;
	}

	nsess->event = ev_func;
	nsess->event_arg = ev_arg;

	if (pipe(nsess->to_thread) < 0) {
		err = errno;
		goto out_pipe_to;
	}

	if (cldc_tcp_new(nsess->host, nsess->port, &nsess->tcp)) {
		err = 1023;
		goto out_tcp;
	}

	nsess->thread = g_thread_create(ncld_sess_thr, nsess, TRUE, &gerr);
	if (nsess->thread == NULL) {
		err = 1022;
		goto out_thread;
	}

	g_mutex_lock(nsess->mutex);
	memset(&copts, 0, sizeof(copts));
	copts.cb = ncld_new_sess;
	copts.private = nsess;
	if (cldc_new_sess_log(&ncld_ops, &copts,
			      nsess->tcp->addr, nsess->tcp->addr_len,
			      cld_user, cld_key, nsess, log,
			      &nsess->tcp->sess)) {
		g_mutex_unlock(nsess->mutex);
		err = 1024;
		goto out_session;
	}
	g_mutex_unlock(nsess->mutex);

	rc = ncld_wait_session(nsess);
	if (rc) {
		err = 1100 + rc % 1000;
		goto out_start;
	}

	return nsess;

out_start:
	cldc_kill_sess(nsess->tcp->sess);
out_session:
	ncld_thr_end(nsess);
out_thread:
	cldc_tcp_free(nsess->tcp);
out_tcp:
	close(nsess->to_thread[0]);
	close(nsess->to_thread[1]);
out_pipe_to:
	free(nsess->host);
out_srv:
	g_cond_free(nsess->cond);
out_cond:
	g_mutex_free(nsess->mutex);
out_mutex:
	free(nsess);
out_sesalloc:
	*error = err;
	return NULL;
}

static int ncld_open_cb(struct cldc_call_opts *copts, enum cle_err_codes errc)
{
	struct ncld_fh *fh = copts->private;

	fh->errc = errc;
	fh->is_open = true;
	g_cond_broadcast(fh->sess->cond);
	return 0;
}

static int ncld_wait_open(struct ncld_fh *fh)
{
	struct ncld_sess *nsess = fh->sess;

	g_mutex_lock(nsess->mutex);
	while (!fh->is_open)
		g_cond_wait(nsess->cond, nsess->mutex);
	g_mutex_unlock(nsess->mutex);
	return fh->errc;
}

/*
 * We do not provide anything like ncld_set_callback because it inevitably
 * leads to lost events. Unused arguments are not too onerous, so just
 * put zero into the events mask if you don't want notifications.
 *
 * FIXME It's a moot point for now anyway, since we don't have unlock-notify,
 * or a waiting lock anyway.
 *
 * On error, return NULL and set the error code (can be errno or our own code).
 */
struct ncld_fh *ncld_open(struct ncld_sess *nsess, const char *fname,
			  unsigned int mode, int *error, unsigned int events,
			  void (*ev_func)(void *, unsigned int), void *ev_arg)
{
	struct ncld_fh *fh;
	struct cldc_call_opts copts;
	int err;
	int rc;

	err = EBUSY;
	if (!nsess->is_up)
		goto out_session;

	err = ENOMEM;
	fh = malloc(sizeof(struct ncld_fh));
	if (!fh)
		goto out_alloc;
	memset(fh, 0, sizeof(struct ncld_fh));
	fh->sess = nsess;
	fh->event_mask = events;
	fh->event_func = ev_func;
	fh->event_arg = ev_arg;

	g_mutex_lock(nsess->mutex);
	memset(&copts, 0, sizeof(copts));
	copts.cb = ncld_open_cb;
	copts.private = fh;
	rc = cldc_open(nsess->tcp->sess, &copts, fname, mode, events, &fh->fh);
	if (rc) {
		err = -rc;
		g_mutex_unlock(nsess->mutex);
		goto out_open;
	}
	g_mutex_unlock(nsess->mutex);

	rc = ncld_wait_open(fh);
	if (rc) {
		err = 1100 + rc;
		goto out_start;
	}

	nsess->handles = g_list_prepend(nsess->handles, fh);
	return fh;

out_start:
	/*
	 * This is screwy, but we do nothing here. There is no way to free
	 * a cldc file handle that failed its open. It is stuck in the
	 * array of handles until the session closes (although it can be
	 * garbage-collected if we're lucky).
	 */
out_open:
	free(fh);
out_alloc:
out_session:
	*error = err;
	return NULL;
}

struct ncld_delio {
	struct ncld_sess *sess;
	bool is_done;
	int errc;
};

static int ncld_del_cb(struct cldc_call_opts *copts, enum cle_err_codes errc)
{
	struct ncld_delio *dp = copts->private;
	struct ncld_sess *nsess = dp->sess;

	dp->errc = errc;
	dp->is_done = true;
	g_cond_broadcast(nsess->cond);
	return 0;
}

static int ncld_wait_del(struct ncld_delio *dp)
{
	struct ncld_sess *nsess = dp->sess;

	g_mutex_lock(nsess->mutex);
	while (!dp->is_done)
		g_cond_wait(nsess->cond, nsess->mutex);
	g_mutex_unlock(nsess->mutex);
	return dp->errc;
}

int ncld_del(struct ncld_sess *nsess, const char *fname)
{
	struct cldc_call_opts copts;
	struct ncld_delio dpb;
	int rc;

	if (!nsess->is_up)
		return -EBUSY;

	memset(&dpb, 0, sizeof(struct ncld_delio));
	dpb.sess = nsess;

	g_mutex_lock(nsess->mutex);
	memset(&copts, 0, sizeof(copts));
	copts.cb = ncld_del_cb;
	copts.private = &dpb;
	rc = cldc_del(nsess->tcp->sess, &copts, fname);
	if (rc) {
		g_mutex_unlock(nsess->mutex);
		return -rc;
	}
	/* 
	 * A delete operation is not accounted for end-session (e.g. nios).
	 * This means: do not call ncld_del and ncld_close together.
	 * The ncld_close can be invoked by ncld_sess_close, so don't
	 * do that either.
	 */
	g_mutex_unlock(nsess->mutex);

	rc = ncld_wait_del(&dpb);
	if (rc)
		return rc + 1100;

	return 0;
}

static int ncld_read_cb(struct cldc_call_opts *copts, enum cle_err_codes errc)
{
	struct ncld_read *rp = copts->private;
	struct ncld_fh *fh = rp->fh;

	if (errc) {
		rp->errc = errc;
	} else {
		char *p;
		size_t l;
		cldc_copts_get_data(copts, &p, &l);
		cldc_copts_get_metadata(copts, &rp->meta);
		/* use assignments to defeat dumb gcc warnings. */
		rp->ptr = p;
		rp->length = l;
	}
	rp->is_done = true;
	g_cond_broadcast(fh->sess->cond);
	return 0;
}

static int ncld_wait_read(struct ncld_read *rp)
{
	struct ncld_fh *fh = rp->fh;
	struct ncld_sess *nsess = fh->sess;

	g_mutex_lock(nsess->mutex);
	while (!rp->is_done)
		g_cond_wait(nsess->cond, nsess->mutex);
	--fh->nios;
	g_mutex_unlock(nsess->mutex);
	return rp->errc;
}

/*
 * @error Error code buffer.
 * @return Pointer to struct ncld_read or NULL if error.
 */
struct ncld_read *ncld_get(struct ncld_fh *fh, int *error)
{
	struct ncld_sess *nsess = fh->sess;
	struct ncld_read *rp;
	struct cldc_call_opts copts;
	int rc;

	if (!fh->is_open) {
		*error = EBUSY;
		return NULL;
	}

	rp = malloc(sizeof(struct ncld_read));
	if (!rp) {
		*error = ENOMEM;
		return NULL;
	}
	memset(rp, 0, sizeof(struct ncld_read));
	rp->fh = fh;

	g_mutex_lock(nsess->mutex);
	memset(&copts, 0, sizeof(copts));
	copts.cb = ncld_read_cb;
	copts.private = rp;
	rc = cldc_get(fh->fh, &copts, false);
	if (rc) {
		g_mutex_unlock(nsess->mutex);
		free(rp);
		*error = -rc;
		return NULL;
	}
	fh->nios++;
	g_mutex_unlock(nsess->mutex);

	rc = ncld_wait_read(rp);
	if (rc) {
		free(rp);
		*error = rc + 1100;
		return NULL;
	}

	return rp;
}

static int ncld_read_meta_cb(struct cldc_call_opts *copts, enum cle_err_codes errc)
{
	struct ncld_read *rp = copts->private;
	struct ncld_fh *fh = rp->fh;

	if (errc) {
		rp->errc = errc;
	} else {
		cldc_copts_get_metadata(copts, &rp->meta);
		rp->ptr = NULL;
		rp->length = 0;
	}
	rp->is_done = true;
	g_cond_broadcast(fh->sess->cond);
	return 0;
}

/*
 * @error Error code buffer.
 * @return Pointer to struct ncld_read or NULL if error.
 */
struct ncld_read *ncld_get_meta(struct ncld_fh *fh, int *error)
{
	struct ncld_sess *nsess = fh->sess;
	struct ncld_read *rp;
	struct cldc_call_opts copts;
	int rc;

	if (!fh->is_open) {
		*error = EBUSY;
		return NULL;
	}

	rp = malloc(sizeof(struct ncld_read));
	if (!rp) {
		*error = ENOMEM;
		return NULL;
	}
	memset(rp, 0, sizeof(struct ncld_read));
	rp->fh = fh;

	g_mutex_lock(nsess->mutex);
	memset(&copts, 0, sizeof(copts));
	copts.cb = ncld_read_meta_cb;
	copts.private = rp;
	rc = cldc_get(fh->fh, &copts, true);
	if (rc) {
		g_mutex_unlock(nsess->mutex);
		free(rp);
		*error = -rc;
		return NULL;
	}
	fh->nios++;
	g_mutex_unlock(nsess->mutex);

	rc = ncld_wait_read(rp);
	if (rc) {
		free(rp);
		*error = rc + 1100;
		return NULL;
	}

	return rp;
}

void ncld_read_free(struct ncld_read *rp)
{
	/*
	 * FIXME: Actually the rp->ptr points to sess->msg_buf, so we
	 * cannot issue 2 cldc_get independently.  Ditto for inode_name
	 * in rp->meta.
	 *
	 * Note also that ncld_read is used for get_meta, and data
	 * buffer may not be present/filled in.
	 */
	/* free(rp->ptr); */

	free(rp);
}

struct ncld_genio {
	struct ncld_fh	*fh;
	bool		is_done;
	int		errc;
};

static int ncld_genio_cb(struct cldc_call_opts *copts, enum cle_err_codes errc)
{
	struct ncld_genio *ap = copts->private;
	struct ncld_fh *fh = ap->fh;

	ap->errc = errc;
	ap->is_done = true;
	g_cond_broadcast(fh->sess->cond);
	return 0;
}

static int ncld_wait_genio(struct ncld_genio *ap)
{
	struct ncld_fh *fh = ap->fh;
	struct ncld_sess *nsess = fh->sess;

	g_mutex_lock(nsess->mutex);
	while (!ap->is_done)
		g_cond_wait(nsess->cond, nsess->mutex);
	--fh->nios;
	g_mutex_unlock(nsess->mutex);
	return ap->errc;
}

/*
 * @return: Zero or error code.
 */
int ncld_write(struct ncld_fh *fh, const void *data, long len)
{
	struct ncld_sess *nsess = fh->sess;
	struct cldc_call_opts copts;
	struct ncld_genio apb;
	int rc;

	if (!fh->is_open)
		return -EBUSY;

	memset(&apb, 0, sizeof(struct ncld_genio));
	apb.fh = fh;

	g_mutex_lock(nsess->mutex);
	memset(&copts, 0, sizeof(copts));
	copts.cb = ncld_genio_cb;
	copts.private = &apb;
	rc = cldc_put(fh->fh, &copts, data, len);
	if (rc) {
		g_mutex_unlock(nsess->mutex);
		return -rc;
	}
	fh->nios++;
	g_mutex_unlock(nsess->mutex);

	rc = ncld_wait_genio(&apb);
	if (rc)
		return rc + 1100;

	return 0;
}

int ncld_trylock(struct ncld_fh *fh)
{
	struct ncld_sess *nsess = fh->sess;
	struct cldc_call_opts copts;
	struct ncld_genio apb;
	int rc;

	if (!fh->is_open)
		return -EBUSY;

	memset(&apb, 0, sizeof(struct ncld_genio));
	apb.fh = fh;

	g_mutex_lock(nsess->mutex);
	memset(&copts, 0, sizeof(copts));
	copts.cb = ncld_genio_cb;
	copts.private = &apb;
	rc = cldc_lock(fh->fh, &copts, 0, false);
	if (rc) {
		g_mutex_unlock(nsess->mutex);
		return -rc;
	}
	fh->nios++;
	g_mutex_unlock(nsess->mutex);

	rc = ncld_wait_genio(&apb);
	if (rc)
		return rc + 1100;

	return 0;
}

int ncld_unlock(struct ncld_fh *fh)
{
	struct ncld_sess *nsess = fh->sess;
	struct cldc_call_opts copts;
	struct ncld_genio apb;
	int rc;

	if (!fh->is_open)
		return -EBUSY;

	memset(&apb, 0, sizeof(struct ncld_genio));
	apb.fh = fh;

	g_mutex_lock(nsess->mutex);
	memset(&copts, 0, sizeof(copts));
	copts.cb = ncld_genio_cb;
	copts.private = &apb;
	rc = cldc_unlock(fh->fh, &copts);
	if (rc) {
		g_mutex_unlock(nsess->mutex);
		return -rc;
	}
	fh->nios++;
	g_mutex_unlock(nsess->mutex);

	rc = ncld_wait_genio(&apb);
	if (rc)
		return rc + 1100;

	return 0;
}

/*
 * This probably does not do what you expect (or anything useful actually).
 * When locks conflict, instead of waiting until the lock is acquired,
 * we return when it was put in the queue on the server (with code 1).
 * Applications using this supply a callback and a mask to ncld_open.
 * FIXME: This does not work at present, since server does not post them.
 */
int ncld_qlock(struct ncld_fh *fh)
{
	struct ncld_sess *nsess = fh->sess;
	struct cldc_call_opts copts;
	struct ncld_genio apb;
	int rc;

	if (!fh->is_open)
		return -EBUSY;

	memset(&apb, 0, sizeof(struct ncld_genio));
	apb.fh = fh;

	g_mutex_lock(nsess->mutex);
	memset(&copts, 0, sizeof(copts));
	copts.cb = ncld_genio_cb;
	copts.private = &apb;
	rc = cldc_lock(fh->fh, &copts, 0, true);
	if (rc) {
		g_mutex_unlock(nsess->mutex);
		return -rc;
	}
	fh->nios++;
	g_mutex_unlock(nsess->mutex);

	rc = ncld_wait_genio(&apb);
	if (rc) {
		if (rc != CLE_LOCK_PENDING)
			return rc + 1100;
		rc = 1;
	}
	return rc;
}

static int ncld_close_cb(struct cldc_call_opts *copts, enum cle_err_codes errc)
{
	struct ncld_fh *fh = copts->private;

	fh->errc = errc;
	fh->is_open = false;
	g_cond_broadcast(fh->sess->cond);
	return 0;
}

/*
 * The poisoning in ncld_close is mostly for bug-catching. Do not try to
 * force-close file handles if other threads bomb them I/Os. This cannot work
 * for us, because users keep pointers, not file descriptor numbers.
 * Applications should stop application I/O first, then close.
 */
void ncld_close(struct ncld_fh *fh)
{
	struct ncld_sess *nsess = fh->sess;
	struct cldc_call_opts copts;
	int rc;

	if (!fh->is_open)
		abort();

	g_mutex_lock(nsess->mutex);
	memset(&copts, 0, sizeof(copts));
	copts.cb = ncld_close_cb;
	copts.private = fh;
	rc = cldc_close(fh->fh, &copts);
	g_mutex_unlock(nsess->mutex);

	if (rc == 0) {
		g_mutex_lock(nsess->mutex);
		while (fh->is_open)
			g_cond_wait(nsess->cond, nsess->mutex);
		g_mutex_unlock(nsess->mutex);
		/* At this point, we ignore fh->errc. */
	}

	/*
	 * The cldc layer dowes not allow us to kill handles willy-nilly,
	 * so the only thing we can do is to wait for I/Os to terminate.
	 *
	 * N.B.: this is making use of the fact that we only have one
	 * conditional per session, and therefore end-of-I/O pokes us here.
	 */
	g_mutex_lock(nsess->mutex);
	while (fh->nios)
		g_cond_wait(nsess->cond, nsess->mutex);
	g_mutex_unlock(nsess->mutex);

	nsess->handles = g_list_remove_all(nsess->handles, fh);
	free(fh);
}

void ncld_sess_close(struct ncld_sess *nsess)
{
	gpointer p;

	while ((p = g_list_nth_data(nsess->handles, 0)))
		ncld_close(p);
	g_list_free(nsess->handles);

	cldc_kill_sess(nsess->tcp->sess);
	ncld_thr_end(nsess);
	cldc_tcp_free(nsess->tcp);
	close(nsess->to_thread[0]);
	close(nsess->to_thread[1]);
	g_cond_free(nsess->cond);
	g_mutex_free(nsess->mutex);
	free(nsess->host);
	free(nsess);
}

void ncld_init(void)
{
	cldc_init();
}

/*
 * For extra safety, call cldc_init after g_thread_init, if present.
 * Currently we just call srand(), but since we use GLib, we may need
 * to add some Glib stuff here and that must come after g_thread_init.
 */
void cldc_init()
{
	srand(time(NULL) ^ getpid());	// for cld_rand64 et.al.
}

