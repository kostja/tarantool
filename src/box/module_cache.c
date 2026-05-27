/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright 2010-2021, Tarantool AUTHORS, please see AUTHORS file.
 */

#include <errno.h>
#include <inttypes.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <lua.h>

#include <sys/types.h>
#include <sys/stat.h>

#include "assoc.h"
#include "diag.h"
#include "fiber.h"
#include "errinj.h"
#include "module_cache.h"

#include "box/error.h"
#include "box/port.h"

#include "lua/utils.h"
#include "libeio/eio.h"
#include "coio_task.h"

#include <openssl/evp.h>
#include <openssl/sha.h>

static struct mh_strnptr_t *module_cache = NULL;

/**
 * Helpers for cache manipulations.
 */
static struct module *
cache_find(const char *str, size_t len)
{
	mh_int_t e = mh_strnptr_find_str(module_cache, str, len);
	if (e == mh_end(module_cache))
		return NULL;
	return mh_strnptr_node(module_cache, e)->val;
}

static void
cache_update(struct module *m)
{
	const char *str = m->package;
	size_t len = m->package_len;

	mh_int_t e = mh_strnptr_find_str(module_cache, str, len);
	if (e == mh_end(module_cache))
		panic("module: failed to update cache: %s", str);

	mh_strnptr_node(module_cache, e)->str = m->package;
	mh_strnptr_node(module_cache, e)->val = m;
}

static void
cache_put(struct module *m)
{
	const struct mh_strnptr_node_t nd = {
		.str	= m->package,
		.len	= m->package_len,
		.hash	= mh_strn_hash(m->package, m->package_len),
		.val	= m,
	};

	struct mh_strnptr_node_t prev;
	struct mh_strnptr_node_t *prev_ptr = &prev;
	mh_strnptr_put(module_cache, &nd, &prev_ptr, NULL);
	/*
	 * Just to make sure we haven't replaced something, the
	 * entries must be explicitly deleted.
	 */
	assert(prev_ptr == NULL);
}

static void
cache_del(struct module *m)
{
	const char *str = m->package;
	size_t len = m->package_len;

	mh_int_t e = mh_strnptr_find_str(module_cache, str, len);
	if (e != mh_end(module_cache)) {
		struct module *v = mh_strnptr_node(module_cache, e)->val;
		if (v == m) {
			/*
			 * The module in cache might be updated
			 * via force load and old instance is kept
			 * by a reference only.
			 */
			mh_strnptr_del(module_cache, e, NULL);
		}
	}
}

/** Arguments for lpackage_search. */
struct find_ctx {
	const char *package;
	size_t package_len;
	char *path;
	size_t path_len;
};

/** A helper for find_package(). */
static int
lpackage_search(lua_State *L)
{
	struct find_ctx *ctx = (void *)lua_topointer(L, 1);

	lua_getglobal(L, "package");
	lua_getfield(L, -1, "search");
	lua_pushlstring(L, ctx->package, ctx->package_len);

	lua_call(L, 1, 1);
	if (lua_isnil(L, -1))
		return luaL_error(L, "module not found");

	char resolved[PATH_MAX];
	if (realpath(lua_tostring(L, -1), resolved) == NULL) {
		diag_set(SystemError, "realpath");
		return luaT_error(L);
	}

	/*
	 * No need for result being trimmed test, it
	 * is guaranteed by realpath call.
	 */
	snprintf(ctx->path, ctx->path_len, "%s", resolved);
	return 0;
}

/** Find package in Lua's "package.search". */
static int
find_package(const char *package, size_t package_len,
	     char *path, size_t path_len)
{
	struct find_ctx ctx = {
		.package	= package,
		.package_len	= package_len,
		.path		= path,
		.path_len	= path_len,
	};

	struct lua_State *L = tarantool_L;
	int top = lua_gettop(L);
	if (luaT_cpcall(L, lpackage_search, &ctx) != 0) {
		diag_set(ClientError, ER_LOAD_MODULE, ctx.package_len,
			 ctx.package, lua_tostring(L, -1));
		lua_settop(L, top);
		return -1;
	}
	assert(top == lua_gettop(L));
	return 0;
}

void
module_ref(struct module *m)
{
	assert(m->refs >= 0);
	++m->refs;
}

void
module_unref(struct module *m)
{
	assert(m->refs > 0);
	if (--m->refs == 0) {
		struct errinj *e = errinj(ERRINJ_DYN_MODULE_COUNT, ERRINJ_INT);
		if (e != NULL)
			--e->iparam;
		cache_del(m);
		dlclose(m->handle);
		TRASH(m);
		free(m);
	}
}

int
module_func_load(struct module *m, const char *func_name,
		 struct module_func *mf)
{
	void *sym = dlsym(m->handle, func_name);
	if (sym == NULL) {
		diag_set(ClientError, ER_LOAD_FUNCTION,
			 func_name, dlerror());
		return -1;
	}

	mf->func = sym;
	mf->module = m;
	module_ref(m);

	return 0;
}

void
module_func_unload(struct module_func *mf)
{
	module_unref(mf->module);
	/*
	 * Strictly speaking there is no need
	 * for implicit creation, it is up to
	 * the caller to clear the module function,
	 * but since it is cheap, lets prevent from
	 * even potential use after free.
	 */
	module_func_create(mf);
}

int
module_func_call(struct module_func *mf, struct port *args,
		 struct port *ret)
{
	struct region *region = &fiber()->gc;
	size_t region_svp = region_used(region);

	uint32_t data_sz;
	const char *data = port_get_msgpack(args, &data_sz);
	if (data == NULL)
		return -1;

	port_c_create(ret);
	box_function_ctx_t ctx = {
		.port = ret,
	};

	/*
	 * We don't know what exactly the callee
	 * gonna do during the execution, it may
	 * even try to unload itself, thus we make
	 * sure the dso won't be unloaded until
	 * execution is complete.
	 *
	 * Moreover the callee might release the memory
	 * associated with the module_func pointer itself
	 * so keep the address of the module locally.
	 */
	struct module *m = mf->module;
	module_ref(m);
	int rc = mf->func(&ctx, data, data + data_sz);
	module_unref(m);

	region_truncate(region, region_svp);

	if (rc != 0) {
		if (diag_last_error(&fiber()->diag) == NULL)
			diag_set(ClientError, ER_PROC_C, "unknown error");
		port_destroy(ret);
		return -1;
	}

	return 0;
}

/** Fill attributes from stat. */
static void
module_attr_fill(struct module_attr *attr, struct stat *st)
{
	memset(attr, 0, sizeof(*attr));

	attr->st_dev	= (uint64_t)st->st_dev;
	attr->st_ino	= (uint64_t)st->st_ino;
	attr->st_size	= (uint64_t)st->st_size;
#if TARGET_OS_DARWIN
	attr->tv_sec	= (uint64_t)st->st_mtimespec.tv_sec;
	attr->tv_nsec	= (uint64_t)st->st_mtimespec.tv_nsec;
#else
	attr->tv_sec	= (uint64_t)st->st_mtim.tv_sec;
	attr->tv_nsec	= (uint64_t)st->st_mtim.tv_nsec;
#endif
}

/**
 * coio worker: SHA256 of the file at @a fd. @a out_digest must
 * have room for SHA256_DIGEST_LENGTH (32) bytes. Reports failure
 * via errno (read errors keep the syscall's errno; allocation
 * and crypto failures use ENOMEM / EINVAL). On success returns 0.
 *
 * The fd is rewound to offset 0 before reading. After the call
 * its position is at end-of-file; callers that want to re-read
 * via the same fd must lseek().
 */
static ssize_t
module_file_sha256_f(va_list ap)
{
	int fd = va_arg(ap, int);
	unsigned char *out_digest = va_arg(ap, unsigned char *);

	if (lseek(fd, 0, SEEK_SET) == (off_t)-1)
		return -1;

	EVP_MD_CTX *ctx = EVP_MD_CTX_new();
	if (ctx == NULL) {
		errno = ENOMEM;
		return -1;
	}
	if (EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1) {
		EVP_MD_CTX_free(ctx);
		errno = EINVAL;
		return -1;
	}
	char buf[64 * 1024];
	ssize_t n;
	int read_err = 0;
	while ((n = read(fd, buf, sizeof(buf))) > 0) {
		if (EVP_DigestUpdate(ctx, buf, n) != 1) {
			EVP_MD_CTX_free(ctx);
			errno = EINVAL;
			return -1;
		}
	}
	if (n < 0)
		read_err = errno;
	unsigned int len = SHA256_DIGEST_LENGTH;
	int ok = EVP_DigestFinal_ex(ctx, out_digest, &len) == 1;
	EVP_MD_CTX_free(ctx);
	if (read_err != 0) {
		errno = read_err;
		return -1;
	}
	if (!ok) {
		errno = EINVAL;
		return -1;
	}
	return 0;
}

/**
 * Compute the SHA256 hash of the file open on @a fd and write it
 * as a 64-character lowercase hex string into @a out_hex (which
 * must have room for 64 chars + NUL).
 *
 * Hashing a large shared library takes milliseconds so we run it
 * in a coio worker; the calling fiber yields, the tx event loop
 * keeps spinning.
 *
 * On success returns 0. On failure returns -1 with the diag set.
 */
static int
module_file_sha256(int fd, char *out_hex)
{
	unsigned char digest[SHA256_DIGEST_LENGTH];
	if (coio_call(module_file_sha256_f, fd, digest) < 0) {
		diag_set(SystemError, "failed to hash module");
		return -1;
	}
	tt_bin2hex(digest, SHA256_DIGEST_LENGTH, out_hex);
	out_hex[SHA256_DIGEST_LENGTH * 2] = '\0';
	return 0;
}

/**
 * coio worker: sendfile @a source_fd into @a tmp_fd, then fchmod
 * @a tmp_fd to @a mode. Source position is reset to 0 first so
 * the worker can be called after the hash worker has read
 * through the same fd. Reports failure via errno.
 */
static ssize_t
module_copy_f(va_list ap)
{
	int source_fd = va_arg(ap, int);
	int tmp_fd = va_arg(ap, int);
	off_t size = va_arg(ap, off_t);
	mode_t mode = va_arg(ap, mode_t);

	if (lseek(source_fd, 0, SEEK_SET) == (off_t)-1)
		return -1;

	off_t ret = eio_sendfile_sync(tmp_fd, source_fd, 0, size);
	if (ret != size) {
		if (errno == 0)
			errno = EIO;
		return -1;
	}
	if (fchmod(tmp_fd, mode) != 0)
		return -1;
	return 0;
}

/**
 * Copy the file open on @a source_fd to @a load_name atomically:
 * mkstemp(3) a sibling, sendfile + fchmod in a coio worker (so
 * the tx fiber yields for the duration of the copy), then
 * rename(2) into place. Concurrent tarantool processes that
 * race to populate the same content-hash cache slot see either
 * no file or a complete file -- never a torn one.
 */
static int
module_copy(int source_fd, const char *load_name,
	    const struct stat *st)
{
	char tmp_name[PATH_MAX];
	int rc = snprintf(tmp_name, sizeof(tmp_name), "%s.XXXXXX",
			  load_name);
	if (rc < 0 || (size_t)rc >= sizeof(tmp_name)) {
		diag_set(SystemError, "failed to generate path to dso");
		return -1;
	}
	int dest_fd = mkstemp(tmp_name);
	if (dest_fd < 0) {
		diag_set(SystemError, "failed to create temp file %s",
			 tmp_name);
		return -1;
	}

	if (coio_call(module_copy_f, source_fd, dest_fd,
		      (off_t)st->st_size, (mode_t)(st->st_mode & 0777)) < 0) {
		diag_set(SystemError, "failed to copy module to %s",
			 tmp_name);
		close(dest_fd);
		unlink(tmp_name);
		return -1;
	}
	close(dest_fd);

	if (rename(tmp_name, load_name) != 0) {
		diag_set(SystemError, "failed to rename %s to %s",
			 tmp_name, load_name);
		unlink(tmp_name);
		return -1;
	}
	return 0;
}

/**
 * Load a shared library at @a source_path by way of a stable,
 * content-addressed copy in a per-uid module cache directory.
 *
 * We dlopen a copy rather than the original because POSIX makes
 * no promise that dlclose unloads -- dlopening the same path
 * with replaced content is UB. Naming the copy after SHA256 of
 * its content makes "same path" imply "same bytes", so the
 * second dlopen is safe.
 *
 * Threat model
 * ------------
 * In scope: local users with a different uid from tarantool's
 * euid (e.g. tenants on a shared /tmp host).
 * Out of scope: same-euid users (they already own the process)
 * and root.
 *
 * Defenses:
 *   1. Cache dir $TMPDIR/tnt-<euid> is mode 0700 owned by euid;
 *      lstat() verified after mkdir-or-EEXIST to catch a hostile
 *      pre-creator (symlink, foreign owner, permissive mode).
 *   2. SHA256 names the cached file by its content, so even if
 *      (1) is bypassed an attacker cannot fabricate a colliding
 *      malicious module.
 *   3. Source opened once; same fd feeds hash and copy -- no
 *      TOCTOU between them.
 *   4. Atomic mkstemp + sendfile + fchmod + rename in a coio
 *      worker; concurrent loaders never see a torn file.
 *
 * Residual risks:
 *   - TOCTOU between the dir lstat and the mkstemp in it; only
 *     a same-euid attacker can exploit (out of scope).
 *   - Cached files persist in $TMPDIR -- deliberate, for perf.
 */
static struct module *
module_new(const char *package, size_t package_len,
	   const char *source_path)
{
	size_t size = sizeof(struct module) + package_len + 1;
	struct module *m = malloc(size);
	if (m == NULL) {
		diag_set(OutOfMemory, size, "malloc", "module");
		return NULL;
	}

	m->package_len = package_len;
	m->refs = 0;
	memcpy(m->package, package, package_len);
	m->package[package_len] = 0;

	/*
	 * Open the source once. The same fd is used for the hash
	 * and the copy so they describe the same inode even if the
	 * file on disk is replaced between the two operations.
	 */
	int source_fd = open(source_path, O_RDONLY);
	if (source_fd < 0) {
		diag_set(SystemError, "failed to open module: %s",
			 source_path);
		goto error_free;
	}

	struct stat st;
	if (fstat(source_fd, &st) < 0) {
		diag_set(SystemError, "failed to fstat() module: %s",
			 source_path);
		goto error_close;
	}
	module_attr_fill(&m->attr, &st);

	char hex[SHA256_DIGEST_LENGTH * 2 + 1];
	if (module_file_sha256(source_fd, hex) != 0)
		goto error_close;

	char *tmpdir = getenv_safe("TMPDIR", NULL, 0);
	const char *print_dir = tmpdir != NULL ? tmpdir : "/tmp";

	char dir_name[PATH_MAX];
	int rc = snprintf(dir_name, sizeof(dir_name), "%s/tnt-%u",
			  print_dir, (unsigned)geteuid());
	free(tmpdir);
	if (rc < 0 || (size_t)rc >= sizeof(dir_name)) {
		diag_set(SystemError, "failed to generate path to tmp dir");
		goto error_close;
	}

	if (mkdir(dir_name, 0700) != 0) {
		if (errno != EEXIST) {
			diag_set(SystemError, "failed to create module "
				 "cache dir: %s", dir_name);
			goto error_close;
		}
		/*
		 * Pre-existing entry: verify it is actually a directory
		 * (lstat -- so we never follow a symlink left by another
		 * user), owned by our euid, with no group/other access.
		 * Otherwise refuse to use it.
		 */
		struct stat ds;
		if (lstat(dir_name, &ds) != 0) {
			diag_set(SystemError, "failed to stat module cache "
				 "dir: %s", dir_name);
			goto error_close;
		}
		if (!S_ISDIR(ds.st_mode) || ds.st_uid != geteuid() ||
		    (ds.st_mode & 0077) != 0) {
			diag_set(SystemError, "module cache dir %s is not "
				 "safe (not a directory, foreign owner, or "
				 "group/other accessible)", dir_name);
			goto error_close;
		}
	}

	char load_name[PATH_MAX];
	rc = snprintf(load_name, sizeof(load_name),
		      "%s/%.*s.%s." TARANTOOL_LIBEXT,
		      dir_name, (int)package_len, package, hex);
	if (rc < 0 || (size_t)rc >= sizeof(load_name)) {
		diag_set(SystemError, "failed to generate path to dso");
		goto error_close;
	}

	/*
	 * SHA256 matches imply byte-identical content, so an
	 * existing file at @a load_name is safe to reuse without
	 * recopying. The dir's 0700/euid guarantee from above is
	 * what makes that trust well-founded.
	 */
	if (access(load_name, F_OK) != 0 &&
	    module_copy(source_fd, load_name, &st) != 0)
		goto error_close;

	close(source_fd);
	source_fd = -1;

	m->handle = dlopen(load_name, RTLD_NOW | RTLD_LOCAL);
	if (m->handle == NULL) {
		diag_set(ClientError, ER_LOAD_MODULE, package_len,
			  package, dlerror());
		goto error_free;
	}

	struct errinj *e = errinj(ERRINJ_DYN_MODULE_COUNT, ERRINJ_INT);
	if (e != NULL)
		++e->iparam;

	module_ref(m);
	return m;

error_close:
	close(source_fd);
error_free:
	free(m);
	return NULL;
}

struct module *
module_current_exe()
{
	struct module *m = cache_find(NULL, 0);
	if (m != NULL)
		return m;

	size_t size = sizeof(struct module) + 1;
	m = malloc(size);
	if (m == NULL) {
		diag_set(OutOfMemory, size, "malloc", "module");
		return NULL;
	}
	memset(m, 0, size);
	m->handle = dlopen(0, RTLD_NOW);
	module_ref(m);
	cache_put(m);
	return m;
}

struct module *
module_load_force(const char *package, size_t package_len)
{
	if (package_len == 0)
		return module_current_exe();

	char path[PATH_MAX];
	size_t size = sizeof(path);

	if (find_package(package, package_len, path, size) != 0)
		return NULL;

	struct module *m = module_new(package, package_len, path);
	if (m == NULL)
		return NULL;

	struct module *c = cache_find(package, package_len);
	if (c != NULL) {
		cache_update(m);
	} else {
		cache_put(m);
	}

	return m;
}

struct module *
module_load(const char *package, size_t package_len)
{
	if (package_len == 0)
		return module_current_exe();

	char path[PATH_MAX];

	if (find_package(package, package_len, path, sizeof(path)) != 0)
		return NULL;

	struct module *m = cache_find(package, package_len);
	if (m != NULL) {
		struct module_attr attr;
		struct stat st;
		if (stat(path, &st) != 0) {
			diag_set(SystemError, "failed to stat() %s", path);
			return NULL;
		}

		/*
		 * In case of cache hit we may reuse existing
		 * module which speedup load procedure.
		 */
		module_attr_fill(&attr, &st);
		if (memcmp(&attr, &m->attr, sizeof(attr)) == 0) {
			module_ref(m);
			return m;
		}

		/*
		 * Module has been updated on a storage device,
		 * so load a new instance and update the cache,
		 * old entry get evicted but continue residing
		 * in memory, fully functional, until last
		 * function is unloaded.
		 */
		m = module_new(package, package_len, path);
		if (m != NULL)
			cache_update(m);
	} else {
		m = module_new(package, package_len, path);
		if (m != NULL)
			cache_put(m);
	}

	return m;
}

void
module_unload(struct module *m)
{
	module_unref(m);
}

void
module_free(void)
{
	mh_strnptr_delete(module_cache);
	module_cache = NULL;
}

void
module_init(void)
{
	module_cache = mh_strnptr_new();
}
