
#define _GNU_SOURCE
#include "storaged-config.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <syslog.h>
#include "storaged.h"

#define BE_NAME		"fs"

struct fs_obj {
	struct backend_obj	bo;

	int			out_fd;
	char			*out_fn;

	int			in_fd;
	char			*in_fn;

	struct database		*db;
};

static struct fs_obj *fs_obj_alloc(struct server_volume *vol,
				   struct database *db)
{
	struct fs_obj *obj;

	obj = calloc(1, sizeof(*obj));
	if (!obj)
		return NULL;

	obj->bo.private = obj;
	obj->bo.vol = vol;

	obj->out_fd = -1;
	obj->in_fd = -1;
	obj->db = db;

	return obj;
}

static bool cookie_valid(const char *cookie)
{
	/* empty strings are not valid cookies */
	if (!cookie || !*cookie)
		return false;

	/* cookies MUST consist of 100% lowercase hex digits */
	while (*cookie) {
		switch (*cookie) {
		case '0' ... '9':
		case 'a' ... 'f':
			cookie++;
			break;

		default:
			return false;
		}
	}

	return true;
}

static struct backend_obj *fs_obj_new(struct server_volume *vol,
				      struct database *db,
				      const char *cookie)
{
	struct fs_obj *obj;
	char *fn = NULL;

	if (!cookie_valid(cookie))
		return NULL;

	obj = fs_obj_alloc(vol, db);
	if (!obj)
		return NULL;

	if (asprintf(&fn, "%s/%s", vol->path, cookie) < 0) {
		syslog(LOG_ERR, "OOM in object_put");
		goto err_out;
	}

	obj->out_fd = open(fn, O_WRONLY | O_CREAT | O_EXCL, 0600);
	if (obj->out_fd < 0) {
		if (errno != EEXIST)
			syslogerr(fn);
		goto err_out;
	}

	obj->out_fn = fn;
	strcpy(obj->bo.cookie, cookie);

	return &obj->bo;

err_out:
	free(obj);
	return NULL;
}

static struct backend_obj *fs_obj_open(struct server_volume *vol,
				       struct database *db,
				       const char *cookie,
				       enum errcode *err_code)
{
	struct fs_obj *obj;
	sqlite3_stmt *stmt;
	struct stat st;
	int rc;

	if (!cookie_valid(cookie))
		return NULL;

	*err_code = InternalError;

	obj = fs_obj_alloc(vol, db);
	if (!obj)
		return NULL;

	stmt = obj->db->prep_stmts[st_object];
	sqlite3_bind_text(stmt, 1, vol->name, -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 2, cookie, -1, SQLITE_STATIC);

	rc = sqlite3_step(stmt);
	if (rc != SQLITE_ROW) {
		*err_code = NoSuchKey;
		sqlite3_reset(stmt);
		goto err_out;
	}

	strcpy(obj->bo.hashstr, (const char *) sqlite3_column_text(stmt, 1));

	sqlite3_reset(stmt);

	if (asprintf(&obj->in_fn, "%s/%s", vol->path, cookie) < 0)
		goto err_out;

	obj->in_fd = open(obj->in_fn, O_RDONLY);
	if (obj->in_fd < 0) {
		syslog(LOG_ERR, "open obj(%s) failed: %s",
		       obj->in_fn, strerror(errno));
		goto err_out_fn;
	}

	if (fstat(obj->in_fd, &st) < 0) {
		syslog(LOG_ERR, "fstat obj(%s) failed: %s", obj->in_fn,
			strerror(errno));
		goto err_out_fd;
	}

	obj->bo.size = st.st_size;
	obj->bo.mtime = st.st_mtime;

	return &obj->bo;

err_out_fd:
	close(obj->in_fd);
err_out_fn:
	free(obj->in_fn);
err_out:
	free(obj);
	return NULL;
}

static void fs_obj_free(struct backend_obj *bo)
{
	struct fs_obj *obj;

	if (!bo)
		return;

	obj = bo->private;
	g_assert(obj != NULL);

	if (obj->out_fn) {
		unlink(obj->out_fn);
		free(obj->out_fn);
	}

	if (obj->out_fd >= 0)
		close(obj->out_fd);

	if (obj->in_fn)
		free(obj->in_fn);
	if (obj->in_fd >= 0)
		close(obj->in_fd);

	free(obj);
}

static ssize_t fs_obj_read(struct backend_obj *bo, void *ptr, size_t len)
{
	struct fs_obj *obj = bo->private;
	ssize_t rc;

	rc = read(obj->in_fd, ptr, len);
	if (rc < 0)
		syslog(LOG_ERR, "obj read(%s) failed: %s",
		       obj->in_fn, strerror(errno));

	return rc;
}

static ssize_t fs_obj_write(struct backend_obj *bo, const void *ptr,
			    size_t len)
{
	struct fs_obj *obj = bo->private;
	ssize_t rc;

	rc = write(obj->out_fd, ptr, len);
	if (rc < 0)
		syslog(LOG_ERR, "obj write(%s) failed: %s",
		       obj->out_fn, strerror(errno));

	return rc;
}

static bool fs_obj_write_commit(struct backend_obj *bo, const char *user,
				const char *hashstr, bool sync_data)
{
	struct fs_obj *obj = bo->private;
	sqlite3_stmt *stmt;
	int rc;

	if (sync_data && fsync(obj->out_fd) < 0) {
		syslog(LOG_ERR, "fsync(%s) failed: %s",
		       obj->out_fn, strerror(errno));
		return false;
	}

	close(obj->out_fd);
	obj->out_fd = -1;

	/* begin trans */
	if (!sql_begin(obj->db)) {
		syslog(LOG_ERR, "SQL BEGIN failed in put-end");
		return false;
	}

	/* insert object */
	stmt = obj->db->prep_stmts[st_add_obj];
	sqlite3_bind_text(stmt, 1, obj->bo.vol->name, -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 2, hashstr, -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 3, obj->bo.cookie, -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 4, user, -1, SQLITE_STATIC);

	rc = sqlite3_step(stmt);
	sqlite3_reset(stmt);

	if (rc != SQLITE_DONE) {
		syslog(LOG_ERR, "SQL INSERT(obj) failed");
		goto err_out_rb;
	}

	/* commit */
	if (!sql_commit(obj->db)) {
		syslog(LOG_ERR, "SQL COMMIT");
		return false;
	}

	free(obj->out_fn);
	obj->out_fn = NULL;

	return true;

err_out_rb:
	sql_rollback(obj->db);
	return false;
}

static bool __object_del(struct database *db, const char *volume, const char *fn)
{
	int rc;
	sqlite3_stmt *stmt;

	/* delete object metadata */
	stmt = db->prep_stmts[st_del_obj];
	sqlite3_bind_text(stmt, 1, volume, -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 2, fn, -1, SQLITE_STATIC);

	rc = sqlite3_step(stmt);
	sqlite3_reset(stmt);

	if (rc != SQLITE_DONE) {
		syslog(LOG_ERR, "SQL st_del_obj failed: %d", rc);
		return false;
	}

	return true;
}

static bool fs_obj_delete(struct server_volume *vol, struct database *db,
			  const char *cookie, enum errcode *err_code)
{
	sqlite3_stmt *stmt;
	char *fn;
	int rc;

	*err_code = InternalError;

	/* begin trans */
	if (!sql_begin(db)) {
		syslog(LOG_ERR, "SQL BEGIN failed in obj-del");
		return false;
	}

	/* read existing object info, if any */
	stmt = db->prep_stmts[st_object];
	sqlite3_bind_text(stmt, 1, vol->name, -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 2, cookie, -1, SQLITE_STATIC);

	rc = sqlite3_step(stmt);
	if (rc != SQLITE_ROW) {
		sqlite3_reset(stmt);
		*err_code = NoSuchKey;
		goto err_out;
	}

	sqlite3_reset(stmt);

	if (!__object_del(db, vol->name, cookie))
		goto err_out;

	if (!sql_commit(db)) {
		syslog(LOG_ERR, "SQL COMMIT failed in obj-del");
		return false;
	}

	/* build data filename */
	fn = alloca(strlen(vol->path) + strlen(cookie) + 2);
	sprintf(fn, "%s/%s", vol->path, cookie);

	if (unlink(fn) < 0)
		syslog(LOG_ERR, "DANGLING DATA ERROR: "
		       "object data(%s) unlink failed: %s",
		       fn, strerror(errno));

	return true;

err_out:
	sql_rollback(db);
	return false;
}

static GList *fs_list_objs(struct server_volume *vol, struct database *db)
{
	char *zsql = "select name, hash from objects where volume = ?";
	sqlite3_stmt *select;
	GList *res = NULL;
	const char *dummy;
	int rc;

	/* build SQL SELECT statement */
	rc = sqlite3_prepare_v2(db->sqldb, zsql, -1, &select, &dummy);
	if (rc != SQLITE_OK)
		return NULL;

	/* exec SQL query */
	sqlite3_bind_text(select, 1, vol->name, -1, SQLITE_STATIC);

	/* iterate through each returned SQL data row */
	while (1) {
		const char *name, *hash;

		rc = sqlite3_step(select);
		if (rc != SQLITE_ROW)
			break;

		name = (const char *) sqlite3_column_text(select, 0);
		hash = (const char *) sqlite3_column_text(select, 1);

		res = g_list_append(res, strdup(name));
		res = g_list_append(res, strdup(hash));
	}

	sqlite3_finalize(select);

	return res;
}

static struct backend_info fs_info = {
	.name			= BE_NAME,
	.obj_new		= fs_obj_new,
	.obj_open		= fs_obj_open,
	.obj_read		= fs_obj_read,
	.obj_write		= fs_obj_write,
	.obj_write_commit	= fs_obj_write_commit,
	.obj_delete		= fs_obj_delete,
	.obj_free		= fs_obj_free,
	.list_objs		= fs_list_objs,
};

int be_fs_init(void)
{
	return register_storage(&fs_info);
}
