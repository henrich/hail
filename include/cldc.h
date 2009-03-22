#ifndef __CLDC_H__
#define __CLDC_H__

#include <sys/types.h>
#include <cld_msg.h>

struct cldc;
struct cldc_msg;
struct cldc_session;
struct cldc_call_opts;

struct cldc_call_opts {
	int		(*cb)(struct cldc_call_opts *, enum cle_err_codes);
	void		*private;
};

struct cldc_msg {
	uint64_t	seqid;

	struct cldc_session *sess;

	ssize_t		(*cb)(struct cldc_msg *, const void *, size_t, bool);
	void		*cb_private;

	struct cldc_call_opts copts;

	bool		done;

	time_t		expire_time;

	int		data_len;
	uint8_t		data[0];

	int		retries;
};

struct cldc_fh {
	uint64_t	fh_le;			/* fh id, LE */
	struct cldc_session *sess;
	bool		valid;
};

struct cldc_session {
	uint8_t		sid[CLD_SID_SZ];	/* client id */

	struct cldc	*cldc;

	uint8_t		addr[64];		/* server address */
	size_t		addr_len;

	GArray		*fh;			/* file handle table */

	GList		*out_msg;
	time_t		msg_scan_time;

	time_t		expire_time;
	bool		expired;

	uint64_t	next_seqid_in;
	uint64_t	next_seqid_in_tr;
	uint64_t	next_seqid_out;

	bool		confirmed;
};

enum cldc_event {
	CLDC_EVT_NONE,
	CLDC_EVT_SESS_FAILED,
};

struct cldc {
	/* public: set by app */
	void		*private;
	bool		(*timer_ctl)(void *private, bool add,
				     int (*cb)(struct cldc *, void *),
				     time_t secs);
	ssize_t		(*pkt_send)(void *private,
				const void *addr, size_t addrlen,
				const void *buf, size_t buflen);
	void		(*event)(void *private, enum cldc_event evt);

	/* private: managed by lib */
	GHashTable	*sessions;
};

struct cld_client {
	int			fd;		/* UDP socket */

	char			host[256];
	struct sockaddr_in6	addr;
	socklen_t		addrlen;

	struct sockaddr_in6	local_addr;
	socklen_t		local_addrlen;
	int			local_port;
};

extern int cldcli_init(void);
extern void cldcli_free(struct cld_client *);
extern struct cld_client *cldcli_new(const char *remote_host, int remote_port,
				   int local_port);

extern int cldc_new_sess(struct cldc *cldc, const struct cldc_call_opts *copts,
			 const void *addr, size_t addr_len,
			 struct cldc_session **sess_out);
extern int cldc_end_sess(struct cldc_session *sess,
				const struct cldc_call_opts *copts);
extern int cldc_nop(struct cldc_session *sess,
		    const struct cldc_call_opts *copts);
extern int cldc_del(struct cldc_session *sess,
		    const struct cldc_call_opts *copts,
		    const char *pathname);
extern int cldc_open(struct cldc_session *sess,
	      const struct cldc_call_opts *copts,
	      const char *pathname, uint32_t open_mode,
	      uint32_t events, struct cldc_fh **fh_out);
extern int cldc_close(struct cldc_fh *fh, const struct cldc_call_opts *copts);

static inline bool seqid_after_eq(uint64_t a_, uint64_t b_)
{
	int64_t a = (int64_t) a_;
	int64_t b = (int64_t) b_;

	return a - b >= 0;
}

static inline bool seqid_before_eq(uint64_t a_, uint64_t b_)
{
	return seqid_after_eq(b_, a_);
}

static inline bool seqid_in_range(uint64_t a, uint64_t b, uint64_t c)
{
	return seqid_after_eq(a, b) && seqid_before_eq(a, c);
}

#endif /* __CLDC_H__ */
