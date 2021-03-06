/*
 * Copyright 2010-2016, Tarantool AUTHORS, please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include <stdlib.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <string.h>
#include <ctype.h>
#include <assert.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>

#include <lz4.h>
#include <lz4frame.h>
#include <zstd_static.h>

#include <bit/bit.h>
#include <small/rlist.h>

#include "crc32.h"
#include "clock.h"
#include "trivia/config.h"
#include "tt_pthread.h"

#define sspacked __attribute__((packed))
#define ssunused __attribute__((unused))
#define ssinline __attribute__((always_inline))

#define sslikely(e)   __builtin_expect(!! (e), 1)
#define ssunlikely(e) __builtin_expect(!! (e), 0)

#define sscast(ptr, t, f) \
	((t*)((char*)(ptr) - __builtin_offsetof(t, f)))

#define ss_align(align, len) \
	(((uintptr_t)(len) + ((align) - 1)) & ~((uintptr_t)((align) - 1)))

#define ss_cmp(a, b) \
	((a) == (b) ? 0 : (((a) > (b)) ? 1 : -1))

struct sspath {
	char path[PATH_MAX];
};

static inline void
ss_pathinit(struct sspath *p)
{
	p->path[0] = 0;
}

static inline void
ss_pathset(struct sspath *p, char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	vsnprintf(p->path, sizeof(p->path), fmt, args);
	va_end(args);
}

static inline void
ss_path(struct sspath *p, char *dir, uint64_t id, char *ext)
{
	ss_pathset(p, "%s/%020"PRIu64"%s", dir, id, ext);
}

static inline void
ss_pathcompound(struct sspath *p, char *dir, uint64_t a, uint64_t b, char *ext)
{
	ss_pathset(p, "%s/%020"PRIu64".%020"PRIu64"%s", dir, a, b, ext);
}

static inline char*
ss_pathof(struct sspath *p) {
	return p->path;
}

static inline int
ss_pathis_set(struct sspath *p) {
	return p->path[0] != 0;
}

struct ssiov {
	struct iovec *v;
	int iovmax;
	int iovc;
};

static inline void
ss_iovinit(struct ssiov *v, struct iovec *vp, int max)
{
	v->v = vp;
	v->iovc = 0;
	v->iovmax = max;
}

static inline int
ss_iovensure(struct ssiov *v, int count) {
	return (v->iovc + count) < v->iovmax;
}

static inline int
ss_iovhas(struct ssiov *v) {
	return v->iovc > 0;
}

static inline void
ss_iovreset(struct ssiov *v) {
	v->iovc = 0;
}

static inline void
ss_iovadd(struct ssiov *v, void *ptr, size_t size)
{
	assert(v->iovc < v->iovmax);
	v->v[v->iovc].iov_base = ptr;
	v->v[v->iovc].iov_len = size;
	v->iovc++;
}

struct ssmmap {
	char *p;
	size_t size;
};

static inline void
ss_mmapinit(struct ssmmap *m) {
	m->p = NULL;
	m->size = 0;
}

struct ssvfs;

struct ssvfsif {
	int     (*init)(struct ssvfs*, va_list);
	void    (*free)(struct ssvfs*);
	int64_t (*size)(struct ssvfs*, char*);
	int     (*exists)(struct ssvfs*, char*);
	int     (*unlink)(struct ssvfs*, char*);
	int     (*rename)(struct ssvfs*, char*, char*);
	int     (*mkdir)(struct ssvfs*, char*, int);
	int     (*rmdir)(struct ssvfs*, char*);
	int     (*open)(struct ssvfs*, char*, int, int);
	int     (*close)(struct ssvfs*, int);
	int     (*sync)(struct ssvfs*, int);
	int     (*advise)(struct ssvfs*, int, int, uint64_t, uint64_t);
	int     (*truncate)(struct ssvfs*, int, uint64_t);
	int64_t (*pread)(struct ssvfs*, int, uint64_t, void*, int);
	int64_t (*pwrite)(struct ssvfs*, int, uint64_t, void*, int);
	int64_t (*write)(struct ssvfs*, int, void*, int);
	int64_t (*writev)(struct ssvfs*, int, struct ssiov*);
	int64_t (*seek)(struct ssvfs*, int, uint64_t);
	int     (*mmap)(struct ssvfs*, struct ssmmap*, int, uint64_t, int);
	int     (*mmap_allocate)(struct ssvfs*, struct ssmmap*, uint64_t);
	int     (*mremap)(struct ssvfs*, struct ssmmap*, uint64_t);
	int     (*munmap)(struct ssvfs*, struct ssmmap*);
};

struct ssvfs {
	struct ssvfsif *i;
	char priv[48];
};

static inline int
ss_vfsinit(struct ssvfs *f, struct ssvfsif *i, ...)
{
	f->i = i;
	va_list args;
	va_start(args, i);
	int rc = i->init(f, args);
	va_end(args);
	return rc;
}

static inline void
ss_vfsfree(struct ssvfs *f)
{
	f->i->free(f);
}

#define ss_vfssize(fs, path)                 (fs)->i->size(fs, path)
#define ss_vfsexists(fs, path)               (fs)->i->exists(fs, path)
#define ss_vfsunlink(fs, path)               (fs)->i->unlink(fs, path)
#define ss_vfsrename(fs, src, dest)          (fs)->i->rename(fs, src, dest)
#define ss_vfsmkdir(fs, path, mode)          (fs)->i->mkdir(fs, path, mode)
#define ss_vfsrmdir(fs, path)                (fs)->i->rmdir(fs, path)
#define ss_vfsopen(fs, path, flags, mode)    (fs)->i->open(fs, path, flags, mode)
#define ss_vfsclose(fs, fd)                  (fs)->i->close(fs, fd)
#define ss_vfssync(fs, fd)                   (fs)->i->sync(fs, fd)
#define ss_vfsadvise(fs, fd, hint, off, len) (fs)->i->advise(fs, fd, hint, off, len)
#define ss_vfstruncate(fs, fd, size)         (fs)->i->truncate(fs, fd, size)
#define ss_vfspread(fs, fd, off, buf, size)  (fs)->i->pread(fs, fd, off, buf, size)
#define ss_vfspwrite(fs, fd, off, buf, size) (fs)->i->pwrite(fs, fd, off, buf, size)
#define ss_vfswrite(fs, fd, buf, size)       (fs)->i->write(fs, fd, buf, size)
#define ss_vfswritev(fs, fd, iov)            (fs)->i->writev(fs, fd, iov)
#define ss_vfsseek(fs, fd, off)              (fs)->i->seek(fs, fd, off)
#define ss_vfsmmap(fs, m, fd, size, ro)      (fs)->i->mmap(fs, m, fd, size, ro)
#define ss_vfsmmap_allocate(fs, m, size)     (fs)->i->mmap_allocate(fs, m, size)
#define ss_vfsmremap(fs, m, size)            (fs)->i->mremap(fs, m, size)
#define ss_vfsmunmap(fs, m)                  (fs)->i->munmap(fs, m)

static struct ssvfsif ss_stdvfs;

struct ssfile {
	int fd;
	uint64_t size;
	int creat;
	struct sspath path;
	struct ssvfs *vfs;
} sspacked;

static inline void
ss_fileinit(struct ssfile *f, struct ssvfs *vfs)
{
	ss_pathinit(&f->path);
	f->vfs   = vfs;
	f->fd    = -1;
	f->size  = 0;
	f->creat = 0;
}

static inline int
ss_fileopen_as(struct ssfile *f, char *path, int flags)
{
	f->creat = (flags & O_CREAT ? 1 : 0);
	f->fd = ss_vfsopen(f->vfs, path, flags, 0644);
	if (ssunlikely(f->fd == -1))
		return -1;
	ss_pathset(&f->path, "%s", path);
	f->size = 0;
	if (f->creat)
		return 0;
	int64_t size = ss_vfssize(f->vfs, path);
	if (ssunlikely(size == -1)) {
		ss_vfsclose(f->vfs, f->fd);
		f->fd = -1;
		return -1;
	}
	f->size = size;
	return 0;
}

static inline int
ss_fileopen(struct ssfile *f, char *path) {
	return ss_fileopen_as(f, path, O_RDWR);
}

static inline int
ss_filenew(struct ssfile *f, char *path) {
	return ss_fileopen_as(f, path, O_RDWR|O_CREAT);
}

static inline int
ss_fileclose(struct ssfile *f)
{
	if (ssunlikely(f->fd != -1)) {
		int rc = ss_vfsclose(f->vfs, f->fd);
		if (ssunlikely(rc == -1))
			return -1;
		f->fd  = -1;
		f->vfs = NULL;
	}
	return 0;
}

static inline int
ss_filerename(struct ssfile *f, char *path)
{
	int rc = ss_vfsrename(f->vfs, ss_pathof(&f->path), path);
	if (ssunlikely(rc == -1))
		return -1;
	ss_pathset(&f->path, "%s", path);
	return 0;
}

static inline int
ss_filesync(struct ssfile *f) {
	return ss_vfssync(f->vfs, f->fd);
}

static inline int
ss_fileadvise(struct ssfile *f, int hint, uint64_t off, uint64_t len) {
	return ss_vfsadvise(f->vfs, f->fd, hint, off, len);
}

static inline int
ss_fileresize(struct ssfile *f, uint64_t size)
{
	int rc = ss_vfstruncate(f->vfs, f->fd, size);
	if (ssunlikely(rc == -1))
		return -1;
	f->size = size;
	return 0;
}

static inline int
ss_filepread(struct ssfile *f, uint64_t off, void *buf, int size)
{
	int64_t rc = ss_vfspread(f->vfs, f->fd, off, buf, size);
	if (ssunlikely(rc == -1))
		return -1;
	assert(rc == size);
	return rc;
}

static inline int
ss_filepwrite(struct ssfile *f, uint64_t off, void *buf, int size)
{
	int64_t rc = ss_vfspwrite(f->vfs, f->fd, off, buf, size);
	if (ssunlikely(rc == -1))
		return -1;
	assert(rc == size);
	return rc;
}

static inline int
ss_filewrite(struct ssfile *f, void *buf, int size)
{
	int64_t rc = ss_vfswrite(f->vfs, f->fd, buf, size);
	if (ssunlikely(rc == -1))
		return -1;
	assert(rc == size);
	f->size += rc;
	return rc;
}

static inline int
ss_filewritev(struct ssfile *f, struct ssiov *iov)
{
	int64_t rc = ss_vfswritev(f->vfs, f->fd, iov);
	if (ssunlikely(rc == -1))
		return -1;
	f->size += rc;
	return rc;
}

static inline int
ss_fileseek(struct ssfile *f, uint64_t off)
{
	return ss_vfsseek(f->vfs, f->fd, off);
}

static inline uint64_t
ss_filesvp(struct ssfile *f) {
	return f->size;
}

static inline int
ss_filerlb(struct ssfile *f, uint64_t svp)
{
	if (ssunlikely(f->size == svp))
		return 0;
	int rc = ss_vfstruncate(f->vfs, f->fd, svp);
	if (ssunlikely(rc == -1))
		return -1;
	f->size = svp;
	rc = ss_fileseek(f, f->size);
	if (ssunlikely(rc == -1))
		return -1;
	return 0;
}

struct ssa;

struct ssaif {
	int   (*open)(struct ssa*, va_list);
	int   (*close)(struct ssa*);
	void *(*malloc)(struct ssa*, int);
	void *(*realloc)(struct ssa*, void*, int);
	int   (*ensure)(struct ssa*, int, int);
	void  (*free)(struct ssa*, void*);
};

struct ssa {
	struct ssaif *i;
	char priv[48];
};

static inline int
ss_aopen(struct ssa *a, struct ssaif *i, ...) {
	a->i = i;
	va_list args;
	va_start(args, i);
	int rc = i->open(a, args);
	va_end(args);
	return rc;
}

static inline int
ss_aclose(struct ssa *a) {
	return a->i->close(a);
}

static inline void*
ss_malloc(struct ssa *a, int size) {
	return a->i->malloc(a, size);
}

static inline void*
ss_realloc(struct ssa *a, void *ptr, int size) {
	return a->i->realloc(a, ptr, size);
}

static inline void
ss_free(struct ssa *a, void *ptr) {
	a->i->free(a, ptr);
}

static inline char*
ss_strdup(struct ssa *a, char *str) {
	int sz = strlen(str) + 1;
	char *s = ss_malloc(a, sz);
	if (ssunlikely(s == NULL))
		return NULL;
	memcpy(s, str, sz);
	return s;
}

static struct ssaif ss_stda;

static struct ssaif ss_ooma;

struct sstrace {
	pthread_mutex_t lock;
	const char *file;
	const char *function;
	int line;
	char message[100];
};

static inline void
ss_traceinit(struct sstrace *t) {
	tt_pthread_mutex_init(&t->lock, NULL);
	t->message[0] = 0;
	t->line = 0;
	t->function = NULL;
	t->file = NULL;
}

static inline void
ss_tracefree(struct sstrace *t) {
	tt_pthread_mutex_destroy(&t->lock);
}

static inline int
ss_tracecopy(struct sstrace *t, char *buf, int bufsize) {
	tt_pthread_mutex_lock(&t->lock);
	int len = snprintf(buf, bufsize, "%s", t->message);
	tt_pthread_mutex_unlock(&t->lock);
	return len;
}

static inline void
ss_vtrace(struct sstrace *t,
          const char *file,
          const char *function, int line,
          char *fmt, va_list args)
{
	tt_pthread_mutex_lock(&t->lock);
	t->file     = file;
	t->function = function;
	t->line     = line;
	vsnprintf(t->message, sizeof(t->message), fmt, args);
	tt_pthread_mutex_unlock(&t->lock);
}

static inline int
ss_traceset(struct sstrace *t,
            const char *file,
            const char *function, int line,
            char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	ss_vtrace(t, file, function, line, fmt, args);
	va_end(args);
	return -1;
}

#define ss_trace(t, fmt, ...) \
	ss_traceset(t, __FILE__, __func__, __LINE__, fmt, __VA_ARGS__)

struct ssgc {
	pthread_mutex_t lock;
	int mark;
	int sweep;
	int complete;
};

static inline void
ss_gcinit(struct ssgc *gc)
{
	tt_pthread_mutex_init(&gc->lock, NULL);
	gc->mark     = 0;
	gc->sweep    = 0;
	gc->complete = 0;
}

static inline void
ss_gcfree(struct ssgc *gc)
{
	tt_pthread_mutex_destroy(&gc->lock);
}

static inline void
ss_gcmark(struct ssgc *gc, int n)
{
	tt_pthread_mutex_lock(&gc->lock);
	gc->mark += n;
	tt_pthread_mutex_unlock(&gc->lock);
}

static inline void
ss_gcsweep(struct ssgc *gc, int n)
{
	tt_pthread_mutex_lock(&gc->lock);
	gc->sweep += n;
	tt_pthread_mutex_unlock(&gc->lock);
}

static inline void
ss_gccomplete(struct ssgc *gc)
{
	tt_pthread_mutex_lock(&gc->lock);
	gc->complete = 1;
	tt_pthread_mutex_unlock(&gc->lock);
}

static inline int
ss_gcinprogress(struct ssgc *gc)
{
	tt_pthread_mutex_lock(&gc->lock);
	int v = gc->complete;
	tt_pthread_mutex_unlock(&gc->lock);
	return !v;
}

static inline int
ss_gcrotateready(struct ssgc *gc, int wm)
{
	tt_pthread_mutex_lock(&gc->lock);
	int rc = gc->mark >= wm;
	tt_pthread_mutex_unlock(&gc->lock);
	return rc;
}

static inline int
ss_gcgarbage(struct ssgc *gc)
{
	tt_pthread_mutex_lock(&gc->lock);
	int ready = (gc->mark == gc->sweep);
	int rc = gc->complete && ready;
	tt_pthread_mutex_unlock(&gc->lock);
	return rc;
}

enum ssorder {
	SS_LT,
	SS_LTE,
	SS_GT,
	SS_GTE,
	SS_EQ,
	SS_STOP
};

static inline enum ssorder
ss_orderof(char *order, int size)
{
	enum ssorder cmp = SS_STOP;
	if (strncmp(order, ">", size) == 0) {
		cmp = SS_GT;
	} else
	if (strncmp(order, ">=", size) == 0) {
		cmp = SS_GTE;
	} else
	if (strncmp(order, "<", size) == 0) {
		cmp = SS_LT;
	} else
	if (strncmp(order, "<=", size) == 0) {
		cmp = SS_LTE;
	}
	return cmp;
}

static inline char*
ss_ordername(enum ssorder o)
{
	switch (o) {
	case SS_LT:  return "<";
	case SS_LTE: return "<=";
	case SS_GT:  return ">";
	case SS_GTE: return ">=";
	default: break;
	}
	return NULL;
}

typedef int (*sstriggerf)(void *arg);

struct sstrigger {
	sstriggerf function;
	void *arg;
};

static inline void
ss_triggerinit(struct sstrigger *t)
{
	t->function = NULL;
	t->arg = NULL;
}

static inline void
ss_triggerset(struct sstrigger *t, void *pointer)
{
	t->function = (sstriggerf)(uintptr_t)pointer;
}

static inline void
ss_triggerset_arg(struct sstrigger *t, void *pointer)
{
	t->arg = pointer;
}

static inline void
ss_triggerrun(struct sstrigger *t)
{
	if (t->function == NULL)
		return;
	t->function(t->arg);
}

struct ssbuf {
	char *reserve;
	char *s, *p, *e;
};

static inline void
ss_bufinit(struct ssbuf *b)
{
	b->reserve = NULL;
	b->s = NULL;
	b->p = NULL;
	b->e = NULL;
}

static inline void
ss_bufinit_reserve(struct ssbuf *b, void *buf, int size)
{
	b->reserve = buf;
	b->s = buf;
	b->p = b->s;
	b->e = b->s + size;
}

static inline void
ss_buffree(struct ssbuf *b, struct ssa *a)
{
	if (ssunlikely(b->s == NULL))
		return;
	if (ssunlikely(b->s != b->reserve))
		ss_free(a, b->s);
	b->s = NULL;
	b->p = NULL;
	b->e = NULL;
}

static inline int
ss_bufsize(struct ssbuf *b) {
	return b->e - b->s;
}

static inline int
ss_bufused(struct ssbuf *b) {
	return b->p - b->s;
}

static inline int
ss_bufunused(struct ssbuf *b) {
	return b->e - b->p;
}

static inline void
ss_bufreset(struct ssbuf *b) {
	b->p = b->s;
}

static inline void
ss_bufgc(struct ssbuf *b, struct ssa *a, int wm)
{
	if (ssunlikely(ss_bufsize(b) >= wm)) {
		ss_buffree(b, a);
		ss_bufinit(b);
		return;
	}
	ss_bufreset(b);
}

static inline int
ss_bufensure(struct ssbuf *b, struct ssa *a, int size)
{
	if (sslikely(b->e - b->p >= size))
		return 0;
	int sz = ss_bufsize(b) * 2;
	int actual = ss_bufused(b) + size;
	if (ssunlikely(actual > sz))
		sz = actual;
	char *p;
	if (ssunlikely(b->s == b->reserve)) {
		p = ss_malloc(a, sz);
		if (ssunlikely(p == NULL))
			return -1;
		memcpy(p, b->s, ss_bufused(b));
	} else {
		p = ss_realloc(a, b->s, sz);
		if (ssunlikely(p == NULL))
			return -1;
	}
	b->p = p + (b->p - b->s);
	b->e = p + sz;
	b->s = p;
	assert((b->e - b->p) >= size);
	return 0;
}

static inline void
ss_bufadvance(struct ssbuf *b, int size)
{
	b->p += size;
}

static inline int
ss_bufadd(struct ssbuf *b, struct ssa *a, void *buf, int size)
{
	int rc = ss_bufensure(b, a, size);
	if (ssunlikely(rc == -1))
		return -1;
	memcpy(b->p, buf, size);
	ss_bufadvance(b, size);
	return 0;
}

static inline int
ss_bufin(struct ssbuf *b, void *v) {
	assert(b->s != NULL);
	return (char*)v >= b->s && (char*)v < b->p;
}

static inline void*
ss_bufat(struct ssbuf *b, int size, int i) {
	return b->s + size * i;
}

static inline void
ss_bufset(struct ssbuf *b, int size, int i, char *buf, int bufsize)
{
	assert(b->s + (size * i + bufsize) <= b->p);
	memcpy(b->s + size * i, buf, bufsize);
}

#define SS_INJECTION_SD_BUILD_0      0
#define SS_INJECTION_SD_BUILD_1      1
#define SS_INJECTION_SI_BRANCH_0     2
#define SS_INJECTION_SI_COMPACTION_0 3
#define SS_INJECTION_SI_COMPACTION_1 4
#define SS_INJECTION_SI_COMPACTION_2 5
#define SS_INJECTION_SI_COMPACTION_3 6
#define SS_INJECTION_SI_COMPACTION_4 7
#define SS_INJECTION_SI_RECOVER_0    8
#define SS_INJECTION_SI_SNAPSHOT_0   9
#define SS_INJECTION_SI_SNAPSHOT_1   10
#define SS_INJECTION_SI_SNAPSHOT_2   11

struct ssinjection {
	uint32_t e[12];
	uint32_t oom;
	uint32_t io;
};

#ifdef SS_INJECTION_ENABLE
	#define SS_INJECTION(E, ID, X) \
	if ((E)->e[(ID)]) { \
		X; \
	} else {}
#else
	#define SS_INJECTION(E, ID, X)
#endif

#define ss_crcp(p, size, crc) \
	crc32_calc(crc, p, size)

#define ss_crcs(p, size, crc) \
	crc32_calc(crc, (char*)p + sizeof(uint32_t), size - sizeof(uint32_t))

enum sstype {
	SS_UNDEF,
	SS_STRING,
	SS_STRINGPTR,
	SS_U32,
	SS_U32REV,
	SS_U64,
	SS_U64REV,
	SS_I64,
	SS_OBJECT,
	SS_FUNCTION
};

static inline char*
ss_typeof(enum sstype type) {
	switch (type) {
	case SS_UNDEF:     return "undef";
	case SS_STRING:    return "string";
	case SS_STRINGPTR: return "stringptr";
	case SS_U32:       return "u32";
	case SS_U32REV:    return "u32rev";
	case SS_U64:       return "u64";
	case SS_U64REV:    return "u64rev";
	case SS_I64:       return "i64";
	case SS_OBJECT:    return "object";
	case SS_FUNCTION:  return "function";
	}
	return NULL;
}

typedef void *(*ssthreadf)(void*);

struct ssthread {
	pthread_t id;
	ssthreadf f;
	void *arg;
	struct rlist link;
};

struct ssthreadpool {
	struct rlist list;
	int n;
};

enum ssquotaop {
	SS_QGROW,
	SS_QADD,
	SS_QREMOVE
};

struct ssquota {
	int enable;
	int wait;
	uint64_t limit;
	uint64_t used;
	pthread_mutex_t lock;
	pthread_cond_t cond;
};

static int ss_quotainit(struct ssquota*);
static int ss_quotaset(struct ssquota*, uint64_t);
static int ss_quotaenable(struct ssquota*, int);
static int ss_quotafree(struct ssquota*);
static int ss_quota(struct ssquota*, enum ssquotaop, uint64_t);

static inline uint64_t
ss_quotaused(struct ssquota *q)
{
	tt_pthread_mutex_lock(&q->lock);
	uint64_t used = q->used;
	tt_pthread_mutex_unlock(&q->lock);
	return used;
}

static inline int
ss_quotaused_percent(struct ssquota *q)
{
	tt_pthread_mutex_lock(&q->lock);
	int percent;
	if (q->limit == 0) {
		percent = 0;
	} else {
		percent = (q->used * 100) / q->limit;
	}
	tt_pthread_mutex_unlock(&q->lock);
	return percent;
}

struct ssrbnode {
	struct ssrbnode *p, *l, *r;
	uint8_t color;
} sspacked;

struct ssrb {
	struct ssrbnode *root;
} sspacked;

static inline void
ss_rbinit(struct ssrb *t) {
	t->root = NULL;
}

static inline void
ss_rbinitnode(struct ssrbnode *n) {
	n->color = 2;
	n->p = NULL;
	n->l = NULL;
	n->r = NULL;
}

#define ss_rbget(name, compare) \
\
static inline int \
name(struct ssrb *t, \
     void *scheme ssunused, \
     void *key ssunused, int keysize ssunused, \
     struct ssrbnode **match) \
{ \
	struct ssrbnode *n = t->root; \
	*match = NULL; \
	int rc = 0; \
	while (n) { \
		*match = n; \
		switch ((rc = (compare))) { \
		case  0: return 0; \
		case -1: n = n->r; \
			break; \
		case  1: n = n->l; \
			break; \
		} \
	} \
	return rc; \
}

#define ss_rbtruncate(name, executable) \
\
static inline void \
name(struct ssrbnode *n, void *arg) \
{ \
	if (n->l) \
		name(n->l, arg); \
	if (n->r) \
		name(n->r, arg); \
	executable; \
}

static struct ssrbnode *ss_rbmin(struct ssrb*);
static struct ssrbnode *ss_rbmax(struct ssrb*);
static struct ssrbnode *ss_rbnext(struct ssrb*, struct ssrbnode*);
static struct ssrbnode *ss_rbprev(struct ssrb*, struct ssrbnode*);

static void ss_rbset(struct ssrb*, struct ssrbnode*, int, struct ssrbnode*);
static void ss_rbreplace(struct ssrb*, struct ssrbnode*, struct ssrbnode*);
static void ss_rbremove(struct ssrb*, struct ssrbnode*);

struct ssqf {
	uint8_t   qf_qbits;
	uint8_t   qf_rbits;
	uint8_t   qf_elem_bits;
	uint32_t  qf_entries;
	uint64_t  qf_index_mask;
	uint64_t  qf_rmask;
	uint64_t  qf_elem_mask;
	uint64_t  qf_max_size;
	uint32_t  qf_table_size;
	uint64_t *qf_table;
	struct ssbuf     qf_buf;
};

static int  ss_qfinit(struct ssqf*);
static int  ss_qfensure(struct ssqf*, struct ssa*, uint32_t);
static void ss_qffree(struct ssqf*, struct ssa*);
static void ss_qfgc(struct ssqf*, struct ssa*, int);
static void ss_qfreset(struct ssqf*);
static void ss_qfrecover(struct ssqf*, int, int, uint32_t, uint64_t*);
static void ss_qfadd(struct ssqf*, uint64_t);
static int  ss_qfhas(struct ssqf*, uint64_t);

static inline unsigned int
ss_fnv(char *key, int len)
{
	unsigned char *p = (unsigned char*)key;
	unsigned char *end = p + len;
	unsigned h = 2166136261;
	while (p < end) {
		h = (h * 16777619) ^ *p;
		p++;
	}
	return h;
}

struct sshtnode {
	uint32_t hash;
};

struct ssht {
	struct sshtnode **i;
	int count;
	int size;
};

static inline int
ss_htinit(struct ssht *t, struct ssa *a, int size)
{
	int sz = size * sizeof(struct sshtnode*);
	t->i = (struct sshtnode**)ss_malloc(a, sz);
	if (ssunlikely(t->i == NULL))
		return -1;
	t->count = 0;
	t->size = size;
	memset(t->i, 0, sz);
	return 0;
}

static inline void
ss_htfree(struct ssht *t, struct ssa *a)
{
	if (ssunlikely(t->i == NULL))
		return;
	ss_free(a, t->i);
	t->i = NULL;
	t->size = 0;
}

static inline void
ss_htreset(struct ssht *t)
{
	int sz = t->size * sizeof(struct sshtnode*);
	memset(t->i, 0, sz);
	t->count = 0;
}

static inline int
ss_htisfull(struct ssht *t)
{
	return t->count > (t->size / 2);
}

static inline int
ss_htplace(struct ssht *t, struct sshtnode *node)
{
	uint32_t pos = node->hash % t->size;
	for (;;) {
		if (t->i[pos] != NULL) {
			pos = (pos + 1) % t->size;
			continue;
		}
		return pos;
	}
	return -1;
}

static inline int
ss_htresize(struct ssht *t, struct ssa *a)
{
	struct ssht nt;
	int rc = ss_htinit(&nt, a, t->size * 2);
	if (ssunlikely(rc == -1))
		return -1;
	int i = 0;
	while (i < t->size) {
		if (t->i[i]) {
			int pos = ss_htplace(&nt, t->i[i]);
			nt.i[pos] = t->i[i];
		}
		i++;
	}
	nt.count = t->count;
	ss_htfree(t, a);
	*t = nt;
	return 0;
}

#define ss_htsearch(name, compare) \
static inline int \
name(struct ssht *t, uint32_t hash, \
     char *key ssunused, \
     uint32_t size ssunused, void *ptr ssunused) \
{ \
	uint32_t pos = hash % t->size; \
	for (;;) { \
		if (t->i[pos] != NULL) { \
			if ( (compare) ) \
				return pos; \
			pos = (pos + 1) % t->size; \
			continue; \
		} \
		return pos; \
	} \
	return -1; \
}

static inline void
ss_htset(struct ssht *t, int pos, struct sshtnode *node)
{
	if (t->i[pos] == NULL)
		t->count++;
	t->i[pos] = node;
}

/* range queue */

struct ssrqnode {
	uint32_t q, v;
	struct rlist link;
};

struct ssrqq {
	uint32_t count;
	uint32_t q;
	struct rlist list;
};

struct ssrq {
	uint32_t range_count;
	uint32_t range;
	uint32_t last;
	struct ssrqq *q;
};

static inline void
ss_rqinitnode(struct ssrqnode *n) {
	rlist_create(&n->link);
	n->q = UINT32_MAX;
	n->v = 0;
}

static inline int
ss_rqinit(struct ssrq *q, struct ssa *a, uint32_t range, uint32_t count)
{
	q->range_count = count + 1 /* zero */;
	q->range = range;
	q->q = ss_malloc(a, sizeof(struct ssrqq) * q->range_count);
	if (ssunlikely(q->q == NULL))
		return -1;
	uint32_t i = 0;
	while (i < q->range_count) {
		struct ssrqq *p = &q->q[i];
		rlist_create(&p->list);
		p->count = 0;
		p->q = i;
		i++;
	}
	q->last = 0;
	return 0;
}

static inline void
ss_rqfree(struct ssrq *q, struct ssa *a)
{
	if (q->q) {
		ss_free(a, q->q);
		q->q = NULL;
	}
}

static inline void
ss_rqadd(struct ssrq *q, struct ssrqnode *n, uint32_t v)
{
	uint32_t pos;
	if (ssunlikely(v == 0)) {
		pos = 0;
	} else {
		pos = (v / q->range) + 1;
		if (ssunlikely(pos >= q->range_count))
			pos = q->range_count - 1;
	}
	struct ssrqq *p = &q->q[pos];
	rlist_create(&n->link);
	n->v = v;
	n->q = pos;
	rlist_add(&p->list, &n->link);
	if (ssunlikely(p->count == 0)) {
		if (pos > q->last)
			q->last = pos;
	}
	p->count++;
}

static inline void
ss_rqdelete(struct ssrq *q, struct ssrqnode *n)
{
	struct ssrqq *p = &q->q[n->q];
	p->count--;
	rlist_del(&n->link);
	if (ssunlikely(p->count == 0 && q->last == n->q))
	{
		int i = n->q - 1;
		while (i >= 0) {
			struct ssrqq *p = &q->q[i];
			if (p->count > 0) {
				q->last = i;
				return;
			}
			i--;
		}
	}
}

static inline void
ss_rqupdate(struct ssrq *q, struct ssrqnode *n, uint32_t v)
{
	if (sslikely(n->q != UINT32_MAX))
		ss_rqdelete(q, n);
	ss_rqadd(q, n, v);
}

static inline struct ssrqnode*
ss_rqprev(struct ssrq *q, struct ssrqnode *n)
{
	int pos;
	struct ssrqq *p;
	if (sslikely(n)) {
		pos = n->q;
		p = &q->q[pos];
		if (n->link.next != (&p->list)) {
			return sscast(n->link.next, struct ssrqnode, link);
		}
		pos--;
	} else {
		pos = q->last;
	}
	for (; pos >= 0; pos--) {
		p = &q->q[pos];
		if (ssunlikely(p->count == 0))
			continue;
		return sscast(p->list.next, struct ssrqnode, link);
	}
	return NULL;
}

enum ssfilterop {
	SS_FINPUT,
	SS_FOUTPUT
};

struct ssfilter;

struct ssfilterif {
	char *name;
	int (*init)(struct ssfilter*, va_list);
	int (*free)(struct ssfilter*);
	int (*start)(struct ssfilter*, struct ssbuf*);
	int (*next)(struct ssfilter*, struct ssbuf*, char*, int);
	int (*complete)(struct ssfilter*, struct ssbuf*);
};

struct ssfilter {
	struct ssfilterif *i;
	enum ssfilterop op;
	struct ssa *a;
	char priv[90];
};

static inline int
ss_filterinit(struct ssfilter *c, struct ssfilterif *ci, struct ssa *a, enum ssfilterop op, ...)
{
	c->op = op;
	c->a  = a;
	c->i  = ci;
	va_list args;
	va_start(args, op);
	int rc = c->i->init(c, args);
	va_end(args);
	return rc;
}

static inline int
ss_filterfree(struct ssfilter *c)
{
	return c->i->free(c);
}

static inline int
ss_filterstart(struct ssfilter *c, struct ssbuf *dest)
{
	return c->i->start(c, dest);
}

static inline int
ss_filternext(struct ssfilter *c, struct ssbuf *dest, char *buf, int size)
{
	return c->i->next(c, dest, buf, size);
}

static inline int
ss_filtercomplete(struct ssfilter *c, struct ssbuf *dest)
{
	return c->i->complete(c, dest);
}

static struct ssfilterif ss_nonefilter;

static struct ssfilterif ss_lz4filter;

static struct ssfilterif ss_zstdfilter;

static inline struct ssfilterif*
ss_filterof(char *name)
{
	if (strcmp(name, "none") == 0)
		return &ss_nonefilter;
	if (strcmp(name, "lz4") == 0)
		return &ss_lz4filter;
	if (strcmp(name, "zstd") == 0)
		return &ss_zstdfilter;
	return NULL;
}

struct ssiter;

struct ssiterif {
	void  (*close)(struct ssiter*);
	int   (*has)(struct ssiter*);
	void *(*of)(struct ssiter*);
	void  (*next)(struct ssiter*);
};

struct ssiter {
	struct ssiterif *vif;
	char priv[150];
};

#define ss_iterinit(iterator_if, i) \
do { \
	(i)->vif = &iterator_if; \
} while (0)

#define ss_iteropen(iterator_if, i, ...) iterator_if##_open(i, __VA_ARGS__)
#define ss_iterclose(iterator_if, i) iterator_if##_close(i)
#define ss_iterhas(iterator_if, i) iterator_if##_has(i)
#define ss_iterof(iterator_if, i) iterator_if##_of(i)
#define ss_iternext(iterator_if, i) iterator_if##_next(i)

#define ss_iteratorclose(i) (i)->vif->close(i)
#define ss_iteratorhas(i) (i)->vif->has(i)
#define ss_iteratorof(i) (i)->vif->of(i)
#define ss_iteratornext(i) (i)->vif->next(i)

static struct ssiterif ss_bufiter;
static struct ssiterif ss_bufiterref;

struct ssbufiter {
	struct ssbuf *buf;
	int vsize;
	void *v;
} sspacked;

static inline int
ss_bufiter_open(struct ssiter *i, struct ssbuf *buf, int vsize)
{
	struct ssbufiter *bi = (struct ssbufiter*)i->priv;
	bi->buf = buf;
	bi->vsize = vsize;
	bi->v = bi->buf->s;
	if (ssunlikely(bi->v == NULL))
		return 0;
	if (ssunlikely(! ss_bufin(bi->buf, bi->v))) {
		bi->v = NULL;
		return 0;
	}
	return 1;
}

static inline void
ss_bufiter_close(struct ssiter *i ssunused)
{ }

static inline int
ss_bufiter_has(struct ssiter *i)
{
	struct ssbufiter *bi = (struct ssbufiter*)i->priv;
	return bi->v != NULL;
}

static inline void*
ss_bufiter_of(struct ssiter *i)
{
	struct ssbufiter *bi = (struct ssbufiter*)i->priv;
	return bi->v;
}

static inline void
ss_bufiter_next(struct ssiter *i)
{
	struct ssbufiter *bi = (struct ssbufiter*)i->priv;
	if (ssunlikely(bi->v == NULL))
		return;
	bi->v = (char*)bi->v + bi->vsize;
	if (ssunlikely(! ss_bufin(bi->buf, bi->v)))
		bi->v = NULL;
}

static inline int
ss_bufiterref_open(struct ssiter *i, struct ssbuf *buf, int vsize) {
	return ss_bufiter_open(i, buf, vsize);
}

static inline void
ss_bufiterref_close(struct ssiter *i ssunused)
{ }

static inline int
ss_bufiterref_has(struct ssiter *i) {
	return ss_bufiter_has(i);
}

static inline void*
ss_bufiterref_of(struct ssiter *i)
{
	struct ssbufiter *bi = (struct ssbufiter*)i->priv;
	if (ssunlikely(bi->v == NULL))
		return NULL;
	return *(void**)bi->v;
}

static inline void
ss_bufiterref_next(struct ssiter *i) {
	ss_bufiter_next(i);
}

struct ssblob {
	struct ssmmap map;
	char *s, *p, *e;
	struct ssvfs *vfs;
};

static inline void
ss_blobinit(struct ssblob *b, struct ssvfs *vfs)
{
	ss_mmapinit(&b->map);
	b->s   = NULL;
	b->p   = NULL;
	b->e   = NULL;
	b->vfs = vfs;
}

static inline int
ss_blobfree(struct ssblob *b)
{
	return ss_vfsmunmap(b->vfs, &b->map);
}
static inline int
ss_blobsize(struct ssblob *b) {
	return b->e - b->s;
}

static inline int
ss_blobused(struct ssblob *b) {
	return b->p - b->s;
}

static inline int
ss_blobunused(struct ssblob *b) {
	return b->e - b->p;
}

static inline void
ss_blobadvance(struct ssblob *b, int size)
{
	b->p += size;
}

static inline int
ss_blobrealloc(struct ssblob *b, int size)
{
	int rc = ss_vfsmremap(b->vfs, &b->map, size);
	if (ssunlikely(rc == -1))
		return -1;
	char *p = b->map.p;
	b->p = p + (b->p - b->s);
	b->e = p + size;
	b->s = p;
	assert((b->e - b->p) <= size);
	return 0;
}

static inline int
ss_blobensure(struct ssblob *b, int size)
{
	if (sslikely(b->e - b->p >= size))
		return 0;
	int sz = ss_blobsize(b) * 2;
	int actual = ss_blobused(b) + size;
	if (ssunlikely(actual > sz))
		sz = actual;
	return ss_blobrealloc(b, sz);
}

static inline int
ss_blobfit(struct ssblob *b)
{
	if (ss_blobunused(b) == 0)
		return 0;
	return ss_blobrealloc(b, ss_blobused(b));
}

static inline int
ss_blobadd(struct ssblob *b, void *buf, int size)
{
	int rc = ss_blobensure(b, size);
	if (ssunlikely(rc == -1))
		return -1;
	memcpy(b->p, buf, size);
	ss_blobadvance(b, size);
	return 0;
}

struct ssavg {
	uint64_t count;
	uint64_t total;
	uint32_t min, max;
	double   avg;
	char sz[32];
};

static inline void
ss_avgupdate(struct ssavg *a, uint32_t v)
{
	a->count++;
	a->total += v;
	a->avg = (double)a->total / (double)a->count;
	if (v < a->min)
		a->min = v;
	if (v > a->max)
		a->max = v;
}

static inline void
ss_avgprepare(struct ssavg *a)
{
	snprintf(a->sz, sizeof(a->sz), "%"PRIu32" %"PRIu32" %.1f",
	         a->min, a->max, a->avg);
}

static struct ssiterif ss_bufiter =
{
	.close   = ss_bufiter_close,
	.has     = ss_bufiter_has,
	.of      = ss_bufiter_of,
	.next    = ss_bufiter_next
};

static struct ssiterif ss_bufiterref =
{
	.close   = ss_bufiterref_close,
	.has     = ss_bufiterref_has,
	.of      = ss_bufiterref_of,
	.next    = ss_bufiterref_next
};

struct sslz4filter {
	LZ4F_compressionContext_t compress;
	LZ4F_decompressionContext_t decompress;
	size_t total_size;
} sspacked;

static int
ss_lz4filter_init(struct ssfilter *f, va_list args ssunused)
{
	struct sslz4filter *z = (struct sslz4filter*)f->priv;
	LZ4F_errorCode_t rc = -1;
	switch (f->op) {
	case SS_FINPUT:
		rc = LZ4F_createCompressionContext(&z->compress, LZ4F_VERSION);
		z->total_size = 0;
		break;
	case SS_FOUTPUT:
		rc = LZ4F_createDecompressionContext(&z->decompress,
						     LZ4F_VERSION);
		break;
	}
	if (ssunlikely(rc != 0))
		return -1;
	return 0;
}

static int
ss_lz4filter_free(struct ssfilter *f)
{
	struct sslz4filter *z = (struct sslz4filter*)f->priv;
	(void)z;
	switch (f->op) {
	case SS_FINPUT:
		LZ4F_freeCompressionContext(z->compress);
		break;
	case SS_FOUTPUT:
		LZ4F_freeDecompressionContext(z->decompress);
		break;
	}
	return 0;
}

#ifndef LZ4F_MAXHEADERFRAME_SIZE
/* Defined in lz4frame.c file */
#define LZ4F_MAXHEADERFRAME_SIZE 15
#endif

static int
ss_lz4filter_start(struct ssfilter *f, struct ssbuf *dest)
{
	struct sslz4filter *z = (struct sslz4filter*)f->priv;
	int rc;
	size_t block;
	size_t sz;
	switch (f->op) {
	case SS_FINPUT:;
		block = LZ4F_MAXHEADERFRAME_SIZE;
		rc = ss_bufensure(dest, f->a, block);
		if (ssunlikely(rc == -1))
			return -1;
		sz = LZ4F_compressBegin(z->compress, dest->p, block, NULL);
		if (ssunlikely(LZ4F_isError(sz)))
			return -1;
		ss_bufadvance(dest, sz);
		break;
	case SS_FOUTPUT:
		/* do nothing */
		break;
	}
	return 0;
}

static int
ss_lz4filter_next(struct ssfilter *f, struct ssbuf *dest, char *buf, int size)
{
	struct sslz4filter *z = (struct sslz4filter*)f->priv;
	if (ssunlikely(size == 0))
		return 0;
	int rc;
	switch (f->op) {
	case SS_FINPUT:;
		/* See comments in ss_lz4filter_complete() */
		int capacity = LZ4F_compressBound(z->total_size + size, NULL);
		assert(capacity >= ss_bufused(dest));
		rc = ss_bufensure(dest, f->a, capacity - ss_bufused(dest));
		if (ssunlikely(rc == -1))
			return -1;
		size_t sz = LZ4F_compressUpdate(z->compress, dest->p,
						ss_bufunused(dest),
						buf, size, NULL);
		if (ssunlikely(LZ4F_isError(sz)))
			return -1;
		ss_bufadvance(dest, sz);
		z->total_size += size;
		break;
	case SS_FOUTPUT:;
		/* do a single-pass decompression.
		 *
		 * Assume that destination buffer is allocated to
		 * original size.
		 */
		size_t pos = 0;
		while (pos < (size_t)size)
		{
			size_t o_size = ss_bufunused(dest);
			size_t i_size = size - pos;
			LZ4F_errorCode_t rc;
			rc = LZ4F_decompress(z->decompress, dest->p, &o_size,
					     buf + pos, &i_size, NULL);
			if (LZ4F_isError(rc))
				return -1;
			ss_bufadvance(dest, o_size);
			pos += i_size;
		}
		break;
	}
	return 0;
}

static int
ss_lz4filter_complete(struct ssfilter *f, struct ssbuf *dest)
{
	struct sslz4filter *z = (struct sslz4filter*)f->priv;
	int rc;
	switch (f->op) {
	case SS_FINPUT:;
		/*
		 * FIXME: LZ4F_compressXXX API is not designed for dynamically
		 * growing buffers. LZ4F_compressUpdate() compress data
		 * incrementally, but target buffer must be of fixed size.
		 * https://github.com/Cyan4973/lz4/blob/d86dc916771c126afb797637dda9f6421c0cb998/examples/frameCompress.c#L35
		 *
		 * z->compress (LZ4F_cctx_internal_t) has a temporary buffer
		 * cctxPtr->tmpIn which accumulates cctxPrr->tmpInSize bytes
		 * from the previous LZ4F_compressUpdate() calls. It may
		 * contain up to bufferSize ( 64KB - 4MB ) + 16 bytes.
		 * It is not efficient to pre-allocate, say, 4MB every time.
		 * This filter calculates the total size of input and then
		 * calls LZ4F_compressBound() to determine the total size
		 * of output (capacity).
		 */
#if 0
		LZ4F_cctx_internal_t* cctxPtr = z->compress;
		size_t block = (cctxPtr->tmpInSize + 16);
#endif
		int capacity = LZ4F_compressBound(z->total_size, NULL);
		assert(capacity >= ss_bufused(dest));
		rc = ss_bufensure(dest, f->a, capacity - ss_bufused(dest));
		if (ssunlikely(rc == -1))
			return -1;
		size_t sz = LZ4F_compressEnd(z->compress, dest->p,
					     ss_bufunused(dest), NULL);
		if (ssunlikely(LZ4F_isError(sz)))
			return -1;
		ss_bufadvance(dest, sz);
		break;
	case SS_FOUTPUT:
		/* do nothing */
		break;
	}
	return 0;
}

static struct ssfilterif ss_lz4filter =
{
	.name     = "lz4",
	.init     = ss_lz4filter_init,
	.free     = ss_lz4filter_free,
	.start    = ss_lz4filter_start,
	.next     = ss_lz4filter_next,
	.complete = ss_lz4filter_complete
};

static int
ss_nonefilter_init(struct ssfilter *f ssunused, va_list args ssunused)
{
	return 0;
}

static int
ss_nonefilter_free(struct ssfilter *f ssunused)
{
	return 0;
}

static int
ss_nonefilter_start(struct ssfilter *f ssunused, struct ssbuf *dest ssunused)
{
	return 0;
}

static int
ss_nonefilter_next(struct ssfilter *f ssunused,
                   struct ssbuf *dest ssunused,
                   char *buf ssunused, int size ssunused)
{
	return 0;
}

static int
ss_nonefilter_complete(struct ssfilter *f ssunused, struct ssbuf *dest ssunused)
{
	return 0;
}

static struct ssfilterif ss_nonefilter =
{
	.name     = "none",
	.init     = ss_nonefilter_init,
	.free     = ss_nonefilter_free,
	.start    = ss_nonefilter_start,
	.next     = ss_nonefilter_next,
	.complete = ss_nonefilter_complete
};

struct ssooma {
	pthread_mutex_t lock;
	uint32_t fail_from;
	uint32_t n;
	int ref;
};

static struct ssooma oom_alloc;

static inline int
ss_oomaclose(struct ssa *a ssunused)
{
	tt_pthread_mutex_destroy(&oom_alloc.lock);
	return 0;
}

static inline int
ss_oomaopen(struct ssa *a ssunused, va_list args)
{
	oom_alloc.fail_from = va_arg(args, int);
	oom_alloc.n = 0;
	tt_pthread_mutex_init(&oom_alloc.lock, NULL);
	return 0;
}

static inline int
ss_oomaevent(void)
{
	tt_pthread_mutex_lock(&oom_alloc.lock);
	int generate_fail = oom_alloc.n >= oom_alloc.fail_from;
	oom_alloc.n++;
	tt_pthread_mutex_unlock(&oom_alloc.lock);
	return generate_fail;
}

static inline void*
ss_oomamalloc(struct ssa *a ssunused, int size)
{
	if (ss_oomaevent())
		return NULL;
	return malloc(size);
}

static inline int
ss_oomaensure(struct ssa *a ssunused, int n, int size)
{
	if (ss_oomaevent())
		return -1;
	(void)n;
	(void)size;
	return 0;
}

static inline void*
ss_oomarealloc(struct ssa *a ssunused, void *ptr, int size)
{
	if (ss_oomaevent())
		return NULL;
	return realloc(ptr, size);
}

static inline void
ss_oomafree(struct ssa *a ssunused, void *ptr)
{
	free(ptr);
}

static struct ssaif ss_ooma =
{
	.open    = ss_oomaopen,
	.close   = ss_oomaclose,
	.malloc  = ss_oomamalloc,
	.ensure  = ss_oomaensure,
	.realloc = ss_oomarealloc,
	.free    = ss_oomafree
};

/*
 * Quotient Filter.
 *
 * Based on implementation made by Vedant Kumar <vsk@berkeley.edu>
*/

#define ss_qflmask(n) ((1ULL << (n)) - 1ULL)

static void
ss_qfrecover(struct ssqf *f, int q, int r, uint32_t size, uint64_t *table)
{
	f->qf_qbits      = q;
	f->qf_rbits      = r;
	f->qf_elem_bits  = f->qf_rbits + 3;
	f->qf_index_mask = ss_qflmask(q);
	f->qf_rmask      = ss_qflmask(r);
	f->qf_elem_mask  = ss_qflmask(f->qf_elem_bits);
	f->qf_entries    = 0;
	f->qf_max_size   = 1 << q;
	f->qf_table_size = size;
	f->qf_table      = table;
}

static int
ss_qfinit(struct ssqf *f)
{
	memset(f, 0, sizeof(*f));
	ss_bufinit(&f->qf_buf);
	return 0;
}

static int
ss_qfensure(struct ssqf *f, struct ssa *a, uint32_t count)
{
	int q = 6;
	int r = 1;
	while (q < 32) {
		if (count < (1UL << q))
			break;
		q++;
	}
	f->qf_qbits      = q;
	f->qf_rbits      = r;
	f->qf_elem_bits  = f->qf_rbits + 3;
	f->qf_index_mask = ss_qflmask(q);
	f->qf_rmask      = ss_qflmask(r);
	f->qf_elem_mask  = ss_qflmask(f->qf_elem_bits);
	f->qf_entries    = 0;
	f->qf_max_size   = 1 << q;
	f->qf_table_size = ((1 << q) * (r + 3)) / 8;
	if (f->qf_table_size % 8)
		f->qf_table_size++;
	int rc = ss_bufensure(&f->qf_buf, a, f->qf_table_size);
	if (ssunlikely(rc == -1))
		return -1;
	ss_bufadvance(&f->qf_buf, f->qf_table_size);
	f->qf_table = (uint64_t*)f->qf_buf.s;
	memset(f->qf_table, 0, f->qf_table_size);
	return 0;
}

static void
ss_qffree(struct ssqf *f, struct ssa *a)
{
	if (f->qf_table) {
		ss_buffree(&f->qf_buf, a);
		f->qf_table = NULL;
	}
}

static void
ss_qfgc(struct ssqf *f, struct ssa *a, int wm)
{
	if (ssunlikely(ss_bufsize(&f->qf_buf) >= wm)) {
		ss_buffree(&f->qf_buf, a);
		ss_bufinit(&f->qf_buf);
		return;
	}
	ss_bufreset(&f->qf_buf);
}

static void
ss_qfreset(struct ssqf *f)
{
	memset(f->qf_table, 0, f->qf_table_size);
	ss_bufreset(&f->qf_buf);
	f->qf_entries = 0;
}

static inline uint64_t
ss_qfincr(struct ssqf *f, uint64_t idx) {
	return (idx + 1) & f->qf_index_mask;
}

static inline uint64_t
ss_qfdecr(struct ssqf *f, uint64_t idx) {
	return (idx - 1) & f->qf_index_mask;
}

static inline int
ss_qfoccupied_is(uint64_t elt) {
	return elt & 1;
}

static inline uint64_t
ss_qfoccupied_set(uint64_t elt) {
	return elt | 1;
}

static inline uint64_t
ss_qfoccupied_clr(uint64_t elt) {
	return elt & ~1;
}

static inline int
ss_qfcontinuation_is(uint64_t elt) {
	return elt & 2;
}

static inline uint64_t
ss_qfcontinuation_set(uint64_t elt) {
	return elt | 2;
}

static inline int
ss_qfshifted_is(uint64_t elt) {
	return elt & 4;
}

static inline uint64_t
ss_qfshifted_set(uint64_t elt) {
	return elt | 4;
}

static inline int
ss_qfremainder_of(uint64_t elt) {
	return elt >> 3;
}

static inline int
ss_qfis_empty(uint64_t elt) {
	return (elt & 7) == 0;
}

static inline uint64_t
ss_qfhash_to_q(struct ssqf *f, uint64_t h) {
	return (h >> f->qf_rbits) & f->qf_index_mask;
}

static inline uint64_t
ss_qfhash_to_r(struct ssqf *f, uint64_t h) {
	return h & f->qf_rmask;
}

static inline uint64_t
ss_qfget(struct ssqf *f, uint64_t idx)
{
	size_t bitpos  = f->qf_elem_bits * idx;
	size_t tabpos  = bitpos / 64;
	size_t slotpos = bitpos % 64;
	int spillbits  = (slotpos + f->qf_elem_bits) - 64;
	uint64_t elt;
	elt = (f->qf_table[tabpos] >> slotpos) & f->qf_elem_mask;
	if (spillbits > 0) {
		tabpos++;
		uint64_t x = f->qf_table[tabpos] & ss_qflmask(spillbits);
		elt |= x << (f->qf_elem_bits - spillbits);
	}
	return elt;
}

static inline void
ss_qfset(struct ssqf *f, uint64_t idx, uint64_t elt)
{
	size_t bitpos = f->qf_elem_bits * idx;
	size_t tabpos = bitpos / 64;
	size_t slotpos = bitpos % 64;
	int spillbits = (slotpos + f->qf_elem_bits) - 64;
	elt &= f->qf_elem_mask;
	f->qf_table[tabpos] &= ~(f->qf_elem_mask << slotpos);
	f->qf_table[tabpos] |= elt << slotpos;
	if (spillbits > 0) {
		tabpos++;
		f->qf_table[tabpos] &= ~ss_qflmask(spillbits);
		f->qf_table[tabpos] |= elt >> (f->qf_elem_bits - spillbits);
	}
}

static inline uint64_t
ss_qffind(struct ssqf *f, uint64_t fq)
{
	uint64_t b = fq;
	while (ss_qfshifted_is( ss_qfget(f, b)))
		b = ss_qfdecr(f, b);
	uint64_t s = b;
	while (b != fq) {
		do {
			s = ss_qfincr(f, s);
		} while (ss_qfcontinuation_is(ss_qfget(f, s)));
		do {
			b = ss_qfincr(f, b);
		} while (! ss_qfoccupied_is(ss_qfget(f, b)));
	}
	return s;
}

static inline void
ss_qfinsert(struct ssqf *f, uint64_t s, uint64_t elt)
{
	uint64_t prev;
	uint64_t curr = elt;
	int empty;
	do {
		prev = ss_qfget(f, s);
		empty = ss_qfis_empty(prev);
		if (! empty) {
			prev = ss_qfshifted_set(prev);
			if (ss_qfoccupied_is(prev)) {
				curr = ss_qfoccupied_set(curr);
				prev = ss_qfoccupied_clr(prev);
			}
		}
		ss_qfset(f, s, curr);
		curr = prev;
		s = ss_qfincr(f, s);
	} while (! empty);
}

static inline int
ss_qffull(struct ssqf *f) {
	return f->qf_entries >= f->qf_max_size;
}

static void
ss_qfadd(struct ssqf *f, uint64_t h)
{
	if (ssunlikely(ss_qffull(f)))
		return;

	uint64_t fq    = ss_qfhash_to_q(f, h);
	uint64_t fr    = ss_qfhash_to_r(f, h);
	uint64_t T_fq  = ss_qfget(f, fq);
	uint64_t entry = (fr << 3) & ~7;

	if (sslikely(ss_qfis_empty(T_fq))) {
		ss_qfset(f, fq, ss_qfoccupied_set(entry));
		f->qf_entries++;
		return;
	}

	if (! ss_qfoccupied_is(T_fq))
		ss_qfset(f, fq, ss_qfoccupied_set(T_fq));

	uint64_t start = ss_qffind(f, fq);
	uint64_t s = start;

	if (ss_qfoccupied_is(T_fq)) {
		do {
			uint64_t rem = ss_qfremainder_of(ss_qfget(f, s));
			if (rem == fr) {
				return;
			} else if (rem > fr) {
				break;
			}
			s = ss_qfincr(f, s);
		} while (ss_qfcontinuation_is(ss_qfget(f, s)));

		if (s == start) {
			uint64_t old_head = ss_qfget(f, start);
			ss_qfset(f, start, ss_qfcontinuation_set(old_head));
		} else {
			entry = ss_qfcontinuation_set(entry);
		}
	}

	if (s != fq)
		entry = ss_qfshifted_set(entry);

	ss_qfinsert(f, s, entry);
	f->qf_entries++;
}

static int
ss_qfhas(struct ssqf *f, uint64_t h)
{
	uint64_t fq   = ss_qfhash_to_q(f, h);
	uint64_t fr   = ss_qfhash_to_r(f, h);
	uint64_t T_fq = ss_qfget(f, fq);

	if (! ss_qfoccupied_is(T_fq))
		return 0;

	uint64_t s = ss_qffind(f, fq);
	do {
		uint64_t rem = ss_qfremainder_of(ss_qfget(f, s));
		if (rem == fr)
			return 1;
		else
		if (rem > fr)
			return 0;
		s = ss_qfincr(f, s);
	} while (ss_qfcontinuation_is(ss_qfget(f, s)));

	return 0;
}

static int
ss_quotainit(struct ssquota *q)
{
	q->enable = 0;
	q->wait   = 0;
	q->limit  = 0;
	q->used   = 0;
	tt_pthread_mutex_init(&q->lock, NULL);
	tt_pthread_cond_init(&q->cond, NULL);
	return 0;
}

static int
ss_quotaset(struct ssquota *q, uint64_t limit)
{
	q->limit = limit;
	return 0;
}

static int
ss_quotaenable(struct ssquota *q, int v)
{
	q->enable = v;
	return 0;
}

static int
ss_quotafree(struct ssquota *q)
{
	tt_pthread_mutex_destroy(&q->lock);
	tt_pthread_cond_destroy(&q->cond);
	return 0;
}

static int
ss_quota(struct ssquota *q, enum ssquotaop op, uint64_t v)
{
	if (sslikely(v == 0))
		return 0;
	tt_pthread_mutex_lock(&q->lock);
	switch (op) {
	case SS_QADD:
		if (ssunlikely(!q->enable || q->limit == 0)) {
			/* .. */
		} else {
			if (ssunlikely((q->used + v) >= q->limit)) {
				q->wait++;
				tt_pthread_cond_wait(&q->cond, &q->lock);
			}
		}
	case SS_QGROW:
		q->used += v;
		break;
	case SS_QREMOVE:
		q->used -= v;
		if (ssunlikely(q->wait)) {
			q->wait--;
			tt_pthread_cond_signal(&q->cond);
		}
		break;
	}
	tt_pthread_mutex_unlock(&q->lock);
	return 0;
}

#define SS_RBBLACK 0
#define SS_RBRED   1
#define SS_RBUNDEF 2

static struct ssrbnode *
ss_rbmin(struct ssrb *t)
{
	struct ssrbnode *n = t->root;
	if (ssunlikely(n == NULL))
		return NULL;
	while (n->l)
		n = n->l;
	return n;
}

static struct ssrbnode *
ss_rbmax(struct ssrb *t)
{
	struct ssrbnode *n = t->root;
	if (ssunlikely(n == NULL))
		return NULL;
	while (n->r)
		n = n->r;
	return n;
}

static struct ssrbnode *
ss_rbnext(struct ssrb *t, struct ssrbnode *n)
{
	if (ssunlikely(n == NULL))
		return ss_rbmin(t);
	if (n->r) {
		n = n->r;
		while (n->l)
			n = n->l;
		return n;
	}
	struct ssrbnode *p;
	while ((p = n->p) && p->r == n)
		n = p;
	return p;
}

static struct ssrbnode *
ss_rbprev(struct ssrb *t, struct ssrbnode *n)
{
	if (ssunlikely(n == NULL))
		return ss_rbmax(t);
	if (n->l) {
		n = n->l;
		while (n->r)
			n = n->r;
		return n;
	}
	struct ssrbnode *p;
	while ((p = n->p) && p->l == n)
		n = p;
	return p;
}

static inline void
ss_rbrotate_left(struct ssrb *t, struct ssrbnode *n)
{
	struct ssrbnode *p = n;
	struct ssrbnode *q = n->r;
	struct ssrbnode *parent = n->p;
	if (sslikely(p->p != NULL)) {
		if (parent->l == p)
			parent->l = q;
		else
			parent->r = q;
	} else {
		t->root = q;
	}
	q->p = parent;
	p->p = q;
	p->r = q->l;
	if (p->r)
		p->r->p = p;
	q->l = p;
}

static inline void
ss_rbrotate_right(struct ssrb *t, struct ssrbnode *n)
{
	struct ssrbnode *p = n;
	struct ssrbnode *q = n->l;
	struct ssrbnode *parent = n->p;
	if (sslikely(p->p != NULL)) {
		if (parent->l == p)
			parent->l = q;
		else
			parent->r = q;
	} else {
		t->root = q;
	}
	q->p = parent;
	p->p = q;
	p->l = q->r;
	if (p->l)
		p->l->p = p;
	q->r = p;
}

static inline void
ss_rbset_fixup(struct ssrb *t, struct ssrbnode *n)
{
	struct ssrbnode *p;
	while ((p = n->p) && (p->color == SS_RBRED))
	{
		struct ssrbnode *g = p->p;
		if (p == g->l) {
			struct ssrbnode *u = g->r;
			if (u && u->color == SS_RBRED) {
				g->color = SS_RBRED;
				p->color = SS_RBBLACK;
				u->color = SS_RBBLACK;
				n = g;
			} else {
				if (n == p->r) {
					ss_rbrotate_left(t, p);
					n = p;
					p = n->p;
				}
				g->color = SS_RBRED;
				p->color = SS_RBBLACK;
				ss_rbrotate_right(t, g);
			}
		} else {
			struct ssrbnode *u = g->l;
			if (u && u->color == SS_RBRED) {
				g->color = SS_RBRED;
				p->color = SS_RBBLACK;
				u->color = SS_RBBLACK;
				n = g;
			} else {
				if (n == p->l) {
					ss_rbrotate_right(t, p);
					n = p;
					p = n->p;
				}
				g->color = SS_RBRED;
				p->color = SS_RBBLACK;
				ss_rbrotate_left(t, g);
			}
		}
	}
	t->root->color = SS_RBBLACK;
}

static void
ss_rbset(struct ssrb *t, struct ssrbnode *p, int prel, struct ssrbnode *n)
{
	n->color = SS_RBRED;
	n->p     = p;
	n->l     = NULL;
	n->r     = NULL;
	if (sslikely(p)) {
		assert(prel != 0);
		if (prel > 0)
			p->l = n;
		else
			p->r = n;
	} else {
		t->root = n;
	}
	ss_rbset_fixup(t, n);
}

static void
ss_rbreplace(struct ssrb *t, struct ssrbnode *o, struct ssrbnode *n)
{
	struct ssrbnode *p = o->p;
	if (p) {
		if (p->l == o) {
			p->l = n;
		} else {
			p->r = n;
		}
	} else {
		t->root = n;
	}
	if (o->l)
		o->l->p = n;
	if (o->r)
		o->r->p = n;
	*n = *o;
}

static void
ss_rbremove(struct ssrb *t, struct ssrbnode *n)
{
	if (ssunlikely(n->color == SS_RBUNDEF))
		return;
	struct ssrbnode *l = n->l;
	struct ssrbnode *r = n->r;
	struct ssrbnode *x = NULL;
	if (l == NULL) {
		x = r;
	} else
	if (r == NULL) {
		x = l;
	} else {
		x = r;
		while (x->l)
			x = x->l;
	}
	struct ssrbnode *p = n->p;
	if (p) {
		if (p->l == n) {
			p->l = x;
		} else {
			p->r = x;
		}
	} else {
		t->root = x;
	}
	uint8_t color;
	if (l && r) {
		color    = x->color;
		x->color = n->color;
		x->l     = l;
		l->p     = x;
		if (x != r) {
			p    = x->p;
			x->p = n->p;
			n    = x->r;
			p->l = n;
			x->r = r;
			r->p = x;
		} else {
			x->p = p;
			p    = x;
			n    = x->r;
		}
	} else {
		color = n->color;
		n     = x;
	}
	if (n)
		n->p = p;

	if (color == SS_RBRED)
		return;
	if (n && n->color == SS_RBRED) {
		n->color = SS_RBBLACK;
		return;
	}

	struct ssrbnode *s;
	do {
		if (ssunlikely(n == t->root))
			break;

		if (n == p->l) {
			s = p->r;
			if (s->color == SS_RBRED)
			{
				s->color = SS_RBBLACK;
				p->color = SS_RBRED;
				ss_rbrotate_left(t, p);
				s = p->r;
			}
			if ((!s->l || (s->l->color == SS_RBBLACK)) &&
			    (!s->r || (s->r->color == SS_RBBLACK)))
			{
				s->color = SS_RBRED;
				n = p;
				p = p->p;
				continue;
			}
			if ((!s->r || (s->r->color == SS_RBBLACK)))
			{
				s->l->color = SS_RBBLACK;
				s->color    = SS_RBRED;
				ss_rbrotate_right(t, s);
				s = p->r;
			}
			s->color    = p->color;
			p->color    = SS_RBBLACK;
			s->r->color = SS_RBBLACK;
			ss_rbrotate_left(t, p);
			n = t->root;
			break;
		} else {
			s = p->l;
			if (s->color == SS_RBRED)
			{
				s->color = SS_RBBLACK;
				p->color = SS_RBRED;
				ss_rbrotate_right(t, p);
				s = p->l;
			}
			if ((!s->l || (s->l->color == SS_RBBLACK)) &&
				(!s->r || (s->r->color == SS_RBBLACK)))
			{
				s->color = SS_RBRED;
				n = p;
				p = p->p;
				continue;
			}
			if ((!s->l || (s->l->color == SS_RBBLACK)))
			{
				s->r->color = SS_RBBLACK;
				s->color    = SS_RBRED;
				ss_rbrotate_left(t, s);
				s = p->l;
			}
			s->color    = p->color;
			p->color    = SS_RBBLACK;
			s->l->color = SS_RBBLACK;
			ss_rbrotate_right(t, p);
			n = t->root;
			break;
		}
	} while (n->color == SS_RBBLACK);
	if (n)
		n->color = SS_RBBLACK;
}

static inline int
ss_stdaopen(struct ssa *a ssunused, va_list args ssunused) {
	return 0;
}

static inline int
ss_stdaclose(struct ssa *a ssunused) {
	return 0;
}

static inline void*
ss_stdamalloc(struct ssa *a ssunused, int size) {
	return malloc(size);
}

static inline void*
ss_stdarealloc(struct ssa *a ssunused, void *ptr, int size) {
	return realloc(ptr,  size);
}

static inline void
ss_stdafree(struct ssa *a ssunused, void *ptr) {
	assert(ptr != NULL);
	free(ptr);
}

static struct ssaif ss_stda =
{
	.open    = ss_stdaopen,
	.close   = ss_stdaclose,
	.malloc  = ss_stdamalloc,
	.ensure  = NULL,
	.realloc = ss_stdarealloc,
	.free    = ss_stdafree
};

static inline int
ss_stdvfs_init(struct ssvfs *f ssunused, va_list args ssunused)
{
	return 0;
}

static inline void
ss_stdvfs_free(struct ssvfs *f ssunused)
{ }

static int64_t
ss_stdvfs_size(struct ssvfs *f ssunused, char *path)
{
	struct stat st;
	int rc = lstat(path, &st);
	if (ssunlikely(rc == -1))
		return -1;
	return st.st_size;
}

static int
ss_stdvfs_exists(struct ssvfs *f ssunused, char *path)
{
	struct stat st;
	int rc = lstat(path, &st);
	return rc == 0;
}

static int
ss_stdvfs_unlink(struct ssvfs *f ssunused, char *path)
{
	return unlink(path);
}

static int
ss_stdvfs_rename(struct ssvfs *f ssunused, char *src, char *dest)
{
	return rename(src, dest);
}

static int
ss_stdvfs_mkdir(struct ssvfs *f ssunused, char *path, int mode)
{
	return mkdir(path, mode);
}

static int
ss_stdvfs_rmdir(struct ssvfs *f ssunused, char *path)
{
	return rmdir(path);
}

static int
ss_stdvfs_open(struct ssvfs *f ssunused, char *path, int flags, int mode)
{
	return open(path, flags, mode);
}

static int
ss_stdvfs_close(struct ssvfs *f ssunused, int fd)
{
	return close(fd);
}

static int
ss_stdvfs_sync(struct ssvfs *f ssunused, int fd)
{
	return fdatasync(fd);
}

static int
ss_stdvfs_advise(struct ssvfs *f ssunused, int fd, int hint, uint64_t off, uint64_t len)
{
	(void)hint;
#if !defined(HAVE_POSIX_FADVISE)
	(void)fd;
	(void)off;
	(void)len;
	return 0;
#else
	return posix_fadvise(fd, off, len, POSIX_FADV_DONTNEED);
#endif
}

static int
ss_stdvfs_truncate(struct ssvfs *f ssunused, int fd, uint64_t size)
{
	return ftruncate(fd, size);
}

static int64_t
ss_stdvfs_pread(struct ssvfs *f ssunused, int fd, uint64_t off, void *buf, int size)
{
	int n = 0;
	do {
		int r;
		do {
			r = pread(fd, (char*)buf + n, size - n, off + n);
		} while (r == -1 && errno == EINTR);
		if (r <= 0)
			return -1;
		n += r;
	} while (n != size);

	return n;
}

static int64_t
ss_stdvfs_pwrite(struct ssvfs *f ssunused, int fd, uint64_t off, void *buf, int size)
{
	int n = 0;
	do {
		int r;
		do {
			r = pwrite(fd, (char*)buf + n, size - n, off + n);
		} while (r == -1 && errno == EINTR);
		if (r <= 0)
			return -1;
		n += r;
	} while (n != size);

	return n;
}

static int64_t
ss_stdvfs_write(struct ssvfs *f ssunused, int fd, void *buf, int size)
{
	int n = 0;
	do {
		int r;
		do {
			r = write(fd, (char*)buf + n, size - n);
		} while (r == -1 && errno == EINTR);
		if (r <= 0)
			return -1;
		n += r;
	} while (n != size);

	return n;
}

static int64_t
ss_stdvfs_writev(struct ssvfs *f ssunused, int fd, struct ssiov *iov)
{
	struct iovec *v = iov->v;
	int n = iov->iovc;
	int size = 0;
	do {
		int r;
		do {
			r = writev(fd, v, n);
		} while (r == -1 && errno == EINTR);
		if (r < 0)
			return -1;
		size += r;
		while (n > 0) {
			if (v->iov_len > (size_t)r) {
				v->iov_base = (char*)v->iov_base + r;
				v->iov_len -= r;
				break;
			} else {
				r -= v->iov_len;
				v++;
				n--;
			}
		}
	} while (n > 0);

	return size;
}

static int64_t
ss_stdvfs_seek(struct ssvfs *f ssunused, int fd, uint64_t off)
{
	return lseek(fd, off, SEEK_SET);
}

static int
ss_stdvfs_mmap(struct ssvfs *f ssunused, struct ssmmap *m, int fd, uint64_t size, int ro)
{
	int flags = PROT_READ;
	if (! ro)
		flags |= PROT_WRITE;
	m->p = mmap(NULL, size, flags, MAP_SHARED, fd, 0);
	if (m->p == MAP_FAILED) {
		m->p = NULL;
		return -1;
	}
	m->size = size;
	return 0;
}

static int
ss_stdvfs_mmap_allocate(struct ssvfs *f ssunused, struct ssmmap *m, uint64_t size)
{
	int flags = PROT_READ|PROT_WRITE;
	m->p = mmap(NULL, size, flags, MAP_PRIVATE|MAP_ANON, -1, 0);
	if (ssunlikely(m->p == MAP_FAILED)) {
		m->p = NULL;
		return -1;
	}
	m->size = size;
	return 0;
}

static int
ss_stdvfs_mremap(struct ssvfs *f ssunused, struct ssmmap *m, uint64_t size)
{
	if (ssunlikely(m->p == NULL))
		return ss_stdvfs_mmap_allocate(f, m, size);
	void *p;
#if !defined(HAVE_MREMAP)
	p = mmap(NULL, size, PROT_READ|PROT_WRITE,
	         MAP_PRIVATE|MAP_ANON, -1, 0);
	if (p == MAP_FAILED)
		return -1;
	uint64_t to_copy = m->size;
	if (to_copy > size)
		to_copy = size;
	memcpy(p, m->p, size);
	munmap(m->p, m->size);
#else
	p = mremap(m->p, m->size, size, MREMAP_MAYMOVE);
	if (ssunlikely(p == MAP_FAILED))
		return -1;
#endif
	m->p = p;
	m->size = size;
	return 0;
}

static int
ss_stdvfs_munmap(struct ssvfs *f ssunused, struct ssmmap *m)
{
	if (ssunlikely(m->p == NULL))
		return 0;
	int rc = munmap(m->p, m->size);
	m->p = NULL;
	return rc;
}

static struct ssvfsif ss_stdvfs =
{
	.init          = ss_stdvfs_init,
	.free          = ss_stdvfs_free,
	.size          = ss_stdvfs_size,
	.exists        = ss_stdvfs_exists,
	.unlink        = ss_stdvfs_unlink,
	.rename        = ss_stdvfs_rename,
	.mkdir         = ss_stdvfs_mkdir,
	.rmdir         = ss_stdvfs_rmdir,
	.open          = ss_stdvfs_open,
	.close         = ss_stdvfs_close,
	.sync          = ss_stdvfs_sync,
	.advise        = ss_stdvfs_advise,
	.truncate      = ss_stdvfs_truncate,
	.pread         = ss_stdvfs_pread,
	.pwrite        = ss_stdvfs_pwrite,
	.write         = ss_stdvfs_write,
	.writev        = ss_stdvfs_writev,
	.seek          = ss_stdvfs_seek,
	.mmap          = ss_stdvfs_mmap,
	.mmap_allocate = ss_stdvfs_mmap_allocate,
	.mremap        = ss_stdvfs_mremap,
	.munmap        = ss_stdvfs_munmap
};

struct  sstestvfs {
	pthread_mutex_t lock;
	uint32_t fail_from;
	uint32_t n;
};

static inline int
ss_testvfs_init(struct ssvfs *f, va_list args ssunused)
{
	struct sstestvfs *o = (struct sstestvfs*)f->priv;
	o->fail_from = va_arg(args, int);
	o->n = 0;
	tt_pthread_mutex_init(&o->lock, NULL);
	return 0;
}

static inline void
ss_testvfs_free(struct ssvfs *f)
{
	struct sstestvfs *o = (struct sstestvfs*)f->priv;
	tt_pthread_mutex_destroy(&o->lock);
}

static inline int
ss_testvfs_call(struct ssvfs *f)
{
	struct sstestvfs *o = (struct sstestvfs*)f->priv;
	tt_pthread_mutex_lock(&o->lock);
	int generate_fail = o->n >= o->fail_from;
	o->n++;
	tt_pthread_mutex_unlock(&o->lock);
	return generate_fail;
}

static int64_t
ss_testvfs_size(struct ssvfs *f, char *path)
{
	if (ss_testvfs_call(f))
		return -1;
	return ss_stdvfs.size(f, path);
}

static int
ss_testvfs_exists(struct ssvfs *f, char *path)
{
	if (ss_testvfs_call(f))
		return -1;
	return ss_stdvfs.exists(f, path);
}

static int
ss_testvfs_unlink(struct ssvfs *f, char *path)
{
	if (ss_testvfs_call(f))
		return -1;
	return ss_stdvfs.unlink(f, path);
}

static int
ss_testvfs_rename(struct ssvfs *f, char *src, char *dest)
{
	if (ss_testvfs_call(f))
		return -1;
	return ss_stdvfs.rename(f, src, dest);
}

static int
ss_testvfs_mkdir(struct ssvfs *f, char *path, int mode)
{
	if (ss_testvfs_call(f))
		return -1;
	return ss_stdvfs.mkdir(f, path, mode);
}

static int
ss_testvfs_rmdir(struct ssvfs *f, char *path)
{
	if (ss_testvfs_call(f))
		return -1;
	return ss_stdvfs.rmdir(f, path);
}

static int
ss_testvfs_open(struct ssvfs *f, char *path, int flags, int mode)
{
	if (ss_testvfs_call(f))
		return -1;
	return ss_stdvfs.open(f, path, flags, mode);
}

static int
ss_testvfs_close(struct ssvfs *f, int fd)
{
	if (ss_testvfs_call(f))
		return -1;
	return ss_stdvfs.close(f, fd);
}

static int
ss_testvfs_sync(struct ssvfs *f, int fd)
{
	if (ss_testvfs_call(f))
		return -1;
	return ss_stdvfs.sync(f, fd);
}

static int
ss_testvfs_advise(struct ssvfs *f, int fd, int hint, uint64_t off, uint64_t len)
{
	if (ss_testvfs_call(f))
		return -1;
	return ss_stdvfs.advise(f, fd, hint, off, len);
}

static int
ss_testvfs_truncate(struct ssvfs *f, int fd, uint64_t size)
{
	if (ss_testvfs_call(f))
		return -1;
	return ss_stdvfs.truncate(f, fd, size);
}

static int64_t
ss_testvfs_pread(struct ssvfs *f, int fd, uint64_t off, void *buf, int size)
{
	if (ss_testvfs_call(f))
		return -1;
	return ss_stdvfs.pread(f, fd, off, buf, size);
}

static int64_t
ss_testvfs_pwrite(struct ssvfs *f, int fd, uint64_t off, void *buf, int size)
{
	if (ss_testvfs_call(f))
		return -1;
	return ss_stdvfs.pwrite(f, fd, off, buf, size);
}

static int64_t
ss_testvfs_write(struct ssvfs *f, int fd, void *buf, int size)
{
	if (ss_testvfs_call(f))
		return -1;
	return ss_stdvfs.write(f, fd, buf, size);
}

static int64_t
ss_testvfs_writev(struct ssvfs *f, int fd, struct ssiov *iov)
{
	if (ss_testvfs_call(f))
		return -1;
	return ss_stdvfs.writev(f, fd, iov);
}

static int64_t
ss_testvfs_seek(struct ssvfs *f, int fd, uint64_t off)
{
	if (ss_testvfs_call(f))
		return -1;
	return ss_stdvfs.seek(f, fd, off);
}

static int
ss_testvfs_mmap(struct ssvfs *f, struct ssmmap *m, int fd, uint64_t size, int ro)
{
	if (ss_testvfs_call(f))
		return -1;
	return ss_stdvfs.mmap(f, m, fd, size, ro);
}

static int
ss_testvfs_mmap_allocate(struct ssvfs *f, struct ssmmap *m, uint64_t size)
{
	if (ss_testvfs_call(f))
		return -1;
	return ss_stdvfs.mmap_allocate(f, m, size);
}

static int
ss_testvfs_mremap(struct ssvfs *f, struct ssmmap *m, uint64_t size)
{
	if (ss_testvfs_call(f))
		return -1;
	return ss_stdvfs.mremap(f, m, size);
}

static int
ss_testvfs_munmap(struct ssvfs *f, struct ssmmap *m)
{
	if (ss_testvfs_call(f))
		return -1;
	return ss_stdvfs.munmap(f, m);
}

static struct ssvfsif ss_testvfs =
{
	.init          = ss_testvfs_init,
	.free          = ss_testvfs_free,
	.size          = ss_testvfs_size,
	.exists        = ss_testvfs_exists,
	.unlink        = ss_testvfs_unlink,
	.rename        = ss_testvfs_rename,
	.mkdir         = ss_testvfs_mkdir,
	.rmdir         = ss_testvfs_rmdir,
	.open          = ss_testvfs_open,
	.close         = ss_testvfs_close,
	.sync          = ss_testvfs_sync,
	.advise        = ss_testvfs_advise,
	.truncate      = ss_testvfs_truncate,
	.pread         = ss_testvfs_pread,
	.pwrite        = ss_testvfs_pwrite,
	.write         = ss_testvfs_write,
	.writev        = ss_testvfs_writev,
	.seek          = ss_testvfs_seek,
	.mmap          = ss_testvfs_mmap,
	.mmap_allocate = ss_testvfs_mmap_allocate,
	.mremap        = ss_testvfs_mremap,
	.munmap        = ss_testvfs_munmap
};

struct sszstdfilter {
	void *ctx;
} sspacked;

static const size_t ZSTD_blockHeaderSize = 3;

static int
ss_zstdfilter_init(struct ssfilter *f, va_list args ssunused)
{
	struct sszstdfilter *z = (struct sszstdfilter*)f->priv;
	switch (f->op) {
	case SS_FINPUT:
		z->ctx = ZSTD_createCCtx();
		if (ssunlikely(z->ctx == NULL))
			return -1;
		break;
	case SS_FOUTPUT:
		z->ctx = NULL;
		break;
	}
	return 0;
}

static int
ss_zstdfilter_free(struct ssfilter *f)
{
	struct sszstdfilter *z = (struct sszstdfilter*)f->priv;
	switch (f->op) {
	case SS_FINPUT:
		ZSTD_freeCCtx(z->ctx);
		break;
	case SS_FOUTPUT:
		break;
	}
	return 0;
}

static int
ss_zstdfilter_start(struct ssfilter *f, struct ssbuf *dest)
{
	(void)dest;
	struct sszstdfilter *z = (struct sszstdfilter*)f->priv;
	size_t sz;
	switch (f->op) {
	case SS_FINPUT:;
		int compressionLevel = 3; /* fast */
		sz = ZSTD_compressBegin(z->ctx, compressionLevel);
		if (ssunlikely(ZSTD_isError(sz)))
			return -1;
		break;
	case SS_FOUTPUT:
		/* do nothing */
		break;
	}
	return 0;
}

static int
ss_zstdfilter_next(struct ssfilter *f, struct ssbuf *dest, char *buf, int size)
{
	struct sszstdfilter *z = (struct sszstdfilter*)f->priv;
	int rc;
	if (ssunlikely(size == 0))
		return 0;
	switch (f->op) {
	case SS_FINPUT:;
		size_t block = ZSTD_compressBound(size);
		rc = ss_bufensure(dest, f->a, block);
		if (ssunlikely(rc == -1))
			return -1;
		size_t sz = ZSTD_compressContinue(z->ctx, dest->p, block, buf, size);
		if (ssunlikely(ZSTD_isError(sz)))
			return -1;
		ss_bufadvance(dest, sz);
		break;
	case SS_FOUTPUT:
		/* do a single-pass decompression.
		 *
		 * Assume that destination buffer is allocated to
		 * original size.
		 */
		sz = ZSTD_decompress(dest->p, ss_bufunused(dest), buf, size);
		if (ssunlikely(ZSTD_isError(sz)))
			return -1;
		break;
	}
	return 0;
}

static int
ss_zstdfilter_complete(struct ssfilter *f, struct ssbuf *dest)
{
	struct sszstdfilter *z = (struct sszstdfilter*)f->priv;
	int rc;
	switch (f->op) {
	case SS_FINPUT:;
		size_t block = ZSTD_blockHeaderSize;
		rc = ss_bufensure(dest, f->a, block);
		if (ssunlikely(rc == -1))
			return -1;
		size_t sz = ZSTD_compressEnd(z->ctx, dest->p, block);
		if (ssunlikely(ZSTD_isError(sz)))
			return -1;
		ss_bufadvance(dest, sz);
		break;
	case SS_FOUTPUT:
		/* do nothing */
		break;
	}
	return 0;
}

static struct ssfilterif ss_zstdfilter =
{
	.name     = "zstd",
	.init     = ss_zstdfilter_init,
	.free     = ss_zstdfilter_free,
	.start    = ss_zstdfilter_start,
	.next     = ss_zstdfilter_next,
	.complete = ss_zstdfilter_complete
};

typedef int (*sfcmpf)(char*, int, char*, int, void*);

struct sffield {
	enum sstype    type;
	int       position;
	int       position_ref;
	int       position_key;
	uint32_t  fixed_size;
	uint32_t  fixed_offset;
	char     *name;
	char     *options;
	int       key;
	sfcmpf    cmp;
};

struct sfscheme {
	struct sffield **fields;
	struct sffield **keys;
	int       fields_count;
	int       keys_count;
	sfcmpf    cmp;
	void     *cmparg;
	int       var_offset;
	int       var_count;
};

static inline struct sffield*
sf_fieldnew(struct ssa *a, char *name)
{
	struct sffield *f = ss_malloc(a, sizeof(struct sffield));
	if (ssunlikely(f == NULL))
		return NULL;
	f->key = 0;
	f->fixed_size = 0;
	f->fixed_offset = 0;
	f->position = 0;
	f->position_ref = 0;
	f->name = ss_strdup(a, name);
	if (ssunlikely(f->name == NULL)) {
		ss_free(a, f);
		return NULL;
	}
	f->type = SS_UNDEF;
	f->options = NULL;
	f->cmp = NULL;
	return f;
}

static inline void
sf_fieldfree(struct sffield *f, struct ssa *a)
{
	if (f->name) {
		ss_free(a, f->name);
		f->name = NULL;
	}
	if (f->options) {
		ss_free(a, f->options);
		f->options = NULL;
	}
	ss_free(a, f);
}

static inline int
sf_fieldoptions(struct sffield *f, struct ssa *a, char *options)
{
	char *sz = ss_strdup(a, options);
	if (ssunlikely(sz == NULL))
		return -1;
	if (f->options)
		ss_free(a, f->options);
	f->options = sz;
	return 0;
}

static void sf_schemeinit(struct sfscheme*);
static void sf_schemefree(struct sfscheme*, struct ssa*);
static int  sf_schemeadd(struct sfscheme*, struct ssa*, struct sffield*);
static int  sf_schemevalidate(struct sfscheme*, struct ssa*);
static int  sf_schemesave(struct sfscheme*, struct ssa*, struct ssbuf*);
static int  sf_schemeload(struct sfscheme*, struct ssa*, char*, int);
static struct sffield*
sf_schemefind(struct sfscheme*, char *);

static int  sf_schemecompare_prefix(struct sfscheme*, char*, uint32_t, char*);
static int  sf_schemecompare(char*, int, char*, int, void*);

static inline int
sf_compare(struct sfscheme *s, char *a, int asize, char *b, int bsize)
{
	return s->cmp(a, asize, b, bsize, s->cmparg);
}

static inline int
sf_compareprefix(struct sfscheme *s, char *a, int asize, char *b, int bsize ssunused)
{
	return sf_schemecompare_prefix(s, a, asize, b);
}

enum sfstorage {
	SF_RAW,
	SF_SPARSE
};

struct sfvar {
	uint32_t offset;
	uint32_t size;
} sspacked;

struct sfv {
	char     *pointer;
	uint32_t  size;
};

static inline char*
sf_fieldof_ptr(struct sfscheme *s, struct sffield *f, char *data, uint32_t *size)
{
	if (sslikely(f->fixed_size > 0)) {
		if (sslikely(size))
			*size = f->fixed_size;
		return data + f->fixed_offset;
	}
	register struct sfvar *v =
		&((struct sfvar*)(data + s->var_offset))[f->position_ref];
	if (sslikely(size))
		*size = v->size;
	return data + v->offset;
}

static inline char*
sf_fieldof(struct sfscheme *s, int pos, char *data, uint32_t *size)
{
	return sf_fieldof_ptr(s, s->fields[pos], data, size);
}

static inline char*
sf_field(struct sfscheme *s, int pos, char *data)
{
	register struct sffield *f = s->fields[pos];
	if (sslikely(f->fixed_size > 0))
		return data + f->fixed_offset;
	register struct sfvar *v =
		&((struct sfvar*)(data + s->var_offset))[f->position_ref];
	return data + v->offset;
}

static inline int
sf_fieldsize(struct sfscheme *s, int pos, char *data)
{
	register struct sffield *f = s->fields[pos];
	if (sslikely(f->fixed_size > 0))
		return f->fixed_size;
	register struct sfvar *v =
		&((struct sfvar*)(data + s->var_offset))[f->position_ref];
	return v->size;
}

static inline int
sf_writesize(struct sfscheme *s, struct sfv *v)
{
	int sum = s->var_offset;
	int i;
	for (i = 0; i < s->fields_count; i++) {
		struct sffield *f = s->fields[i];
		if (f->fixed_size != 0)
			continue;
		sum += sizeof(struct sfvar)+ v[i].size;
	}
	return sum;
}

static inline void
sf_write(struct sfscheme *s, struct sfv *v, char *dest)
{
	int var_value_offset =
		s->var_offset + sizeof(struct sfvar) * s->var_count;
	struct sfvar *var = (struct sfvar*)(dest + s->var_offset);
	int i;
	for (i = 0; i < s->fields_count; i++) {
		struct sffield *f = s->fields[i];
		if (f->fixed_size) {
			assert(f->fixed_size == v[i].size);
			memcpy(dest + f->fixed_offset, v[i].pointer, f->fixed_size);
			continue;
		}
		struct sfvar *current = &var[f->position_ref];
		current->offset = var_value_offset;
		current->size   = v[i].size;
		memcpy(dest + var_value_offset, v[i].pointer, v[i].size);
		var_value_offset += current->size;
	}
}

static inline uint64_t
sf_hash(struct sfscheme *s, char *data)
{
	uint64_t hash = 0;
	int i;
	for (i = 0; i < s->keys_count; i++)
		hash ^= ss_fnv(sf_field(s, i, data), sf_fieldsize(s, i, data));
	return hash;
}

static inline int
sf_comparable_size(struct sfscheme *s, char *data)
{
	int sum = s->var_offset;
	int i;
	for (i = 0; i < s->fields_count; i++) {
		struct sffield *f = s->fields[i];
		if (f->fixed_size != 0)
			continue;
		if (f->key)
			sum += sf_fieldsize(s, i, data);
		sum += sizeof(struct sfvar);
	}
	return sum;
}

static inline void
sf_comparable_write(struct sfscheme *s, char *src, char *dest)
{
	int var_value_offset =
		s->var_offset + sizeof(struct sfvar) * s->var_count;
	memcpy(dest, src, s->var_offset);
	struct sfvar *var = (struct sfvar*)(dest + s->var_offset);
	int i;
	for (i = 0; i < s->fields_count; i++) {
		struct sffield *f = s->fields[i];
		if (f->fixed_size != 0)
			continue;
		struct sfvar *current = &var[f->position_ref];
		current->offset = var_value_offset;
		if (! f->key) {
			current->size = 0;
			continue;
		}
		char *ptr = sf_fieldof_ptr(s, f, src, &current->size);
		memcpy(dest + var_value_offset, ptr, current->size);
		var_value_offset += current->size;
	}
}

struct sflimit {
	uint32_t u32_min;
	uint32_t u32_max;
	uint64_t u64_min;
	uint64_t u64_max;
	int64_t  i64_min;
	int64_t  i64_max;
	char    *string_min;
	int      string_min_size;
	char    *string_max;
	int      string_max_size;
};

static inline int
sf_limitinit(struct sflimit *b, struct ssa *a)
{
	b->u32_min = 0;
	b->u32_max = UINT32_MAX;
	b->u64_min = 0;
	b->u64_max = UINT64_MAX;
	b->i64_min = INT64_MIN;
	b->i64_max = UINT64_MAX;
	b->string_min_size = 0;
	b->string_min = "";
	b->string_max_size = 1024;
	b->string_max = ss_malloc(a, b->string_max_size);
	if (ssunlikely(b->string_max == NULL))
		return -1;
	memset(b->string_max, 0xff, b->string_max_size);
	return 0;
}

static inline void
sf_limitfree(struct sflimit *b, struct ssa *a)
{
	if (b->string_max)
		ss_free(a, b->string_max);
}

static inline void
sf_limitset(struct sflimit *b, struct sfscheme *s, struct sfv *fields, enum ssorder order)
{
	int i;
	for (i = 0; i < s->fields_count; i++) {
		struct sfv *v = &fields[i];
		if (v->pointer)
			continue;
		struct sffield *part = s->fields[i];
		switch (part->type) {
		case SS_U32:
			if (order == SS_LT || order == SS_LTE) {
				v->pointer = (char*)&b->u32_max;
				v->size = sizeof(uint32_t);
			} else {
				v->pointer = (char*)&b->u32_min;
				v->size = sizeof(uint32_t);
			}
			break;
		case SS_U32REV:
			if (order == SS_LT || order == SS_LTE) {
				v->pointer = (char*)&b->u32_min;
				v->size = sizeof(uint32_t);
			} else {
				v->pointer = (char*)&b->u32_max;
				v->size = sizeof(uint32_t);
			}
			break;
		case SS_U64:
			if (order == SS_LT || order == SS_LTE) {
				v->pointer = (char*)&b->u64_max;
				v->size = sizeof(uint64_t);
			} else {
				v->pointer = (char*)&b->u64_min;
				v->size = sizeof(uint64_t);
			}
			break;
		case SS_U64REV:
			if (order == SS_LT || order == SS_LTE) {
				v->pointer = (char*)&b->u64_min;
				v->size = sizeof(uint64_t);
			} else {
				v->pointer = (char*)&b->u64_max;
				v->size = sizeof(uint64_t);
			}
			break;
		case SS_I64:
			if (order == SS_LT || order == SS_LTE) {
				v->pointer = (char*)&b->i64_max;
				v->size = sizeof(int64_t);
			} else {
				v->pointer = (char*)&b->i64_min;
				v->size = sizeof(int64_t);
			}
			break;
		case SS_STRING:
			if (order == SS_LT || order == SS_LTE) {
				v->pointer = b->string_max;
				v->size = b->string_max_size;
			} else {
				v->pointer = b->string_min;
				v->size = b->string_min_size;
			}
			break;
		default: assert(0);
			break;
		}
	}
}

typedef int (*sfupsertf)(int count,
                         char **src,    uint32_t *src_size,
                         char **upsert, uint32_t *upsert_size,
                         char **result, uint32_t *result_size,
                         void *arg);

struct sfupsert {
	sfupsertf function;
	void *arg;
};

static inline void
sf_upsertinit(struct sfupsert *u)
{
	memset(u, 0, sizeof(*u));
}

static inline void
sf_upsertset(struct sfupsert *u, sfupsertf function)
{
	u->function = function;
}

static inline void
sf_upsertset_arg(struct sfupsert *u, void *arg)
{
	u->arg = arg;
}

static inline int
sf_upserthas(struct sfupsert *u) {
	return u->function != NULL;
}

static inline int
sf_cmpstring(char *a, int asz, char *b, int bsz, void *arg ssunused)
{
	int size = (asz < bsz) ? asz : bsz;
	int rc = memcmp(a, b, size);
	if (ssunlikely(rc == 0)) {
		if (sslikely(asz == bsz))
			return 0;
		return (asz < bsz) ? -1 : 1;
	}
	return rc > 0 ? 1 : -1;
}

static inline int
sf_cmpu32(char *a, int asz ssunused, char *b, int bsz ssunused, void *arg ssunused)
{
	uint32_t av = load_u32(a);
	uint32_t bv = load_u32(b);
	if (av == bv)
		return 0;
	return (av > bv) ? 1 : -1;
}

static inline int
sf_cmpu32_reverse(char *a, int asz ssunused, char *b, int bsz ssunused, void *arg ssunused)
{
	uint32_t av = load_u32(a);
	uint32_t bv = load_u32(b);
	if (av == bv)
		return 0;
	return (av > bv) ? -1 : 1;
}

static inline int
sf_cmpu64(char *a, int asz ssunused, char *b, int bsz ssunused,
              void *arg ssunused)
{
	uint64_t av = load_u64(a);
	uint64_t bv = load_u64(b);
	if (av == bv)
		return 0;
	return (av > bv) ? 1 : -1;
}

static inline int
sf_cmpu64_reverse(char *a, int asz ssunused, char *b, int bsz ssunused,
              void *arg ssunused)
{
	uint64_t av = load_u64(a);
	uint64_t bv = load_u64(b);
	if (av == bv)
		return 0;
	return (av > bv) ? -1 : 1;
}

static int
sf_schemecompare(char *a, int asize ssunused, char *b, int bsize ssunused, void *arg)
{
	struct sfscheme *s = arg;
	struct sffield **part = s->keys;
	struct sffield **last = part + s->keys_count;
	int rc;
	while (part < last) {
		struct sffield *key = *part;
		uint32_t a_fieldsize;
		char *a_field = sf_fieldof_ptr(s, key, a, &a_fieldsize);
		uint32_t b_fieldsize;
		char *b_field = sf_fieldof_ptr(s, key, b, &b_fieldsize);
		rc = key->cmp(a_field, a_fieldsize, b_field, b_fieldsize, NULL);
		if (rc != 0)
			return rc;
		part++;
	}
	return 0;
}

static int
sf_schemecompare_prefix(struct sfscheme *s, char *prefix, uint32_t prefixsize, char *key)
{
	uint32_t keysize;
	key = sf_fieldof(s, 0, key, &keysize);
	if (keysize < prefixsize)
		return 0;
	return (memcmp(prefix, key, prefixsize) == 0) ? 1 : 0;
}

static void
sf_schemeinit(struct sfscheme *s)
{
	s->fields = NULL;
	s->fields_count = 0;
	s->keys = NULL;
	s->keys_count = 0;
	s->var_offset = 0;
	s->var_count  = 0;
	s->cmp = sf_schemecompare;
	s->cmparg = s;
}

static void
sf_schemefree(struct sfscheme *s, struct ssa *a)
{
	if (s->fields) {
		int i = 0;
		while (i < s->fields_count) {
			sf_fieldfree(s->fields[i], a);
			i++;
		}
		ss_free(a, s->fields);
		s->fields = NULL;
	}
	if (s->keys) {
		ss_free(a, s->keys);
		s->keys = NULL;
	}
}

static int
sf_schemeadd(struct sfscheme *s, struct ssa *a, struct sffield *f)
{
	int size = sizeof(struct sffield*) * (s->fields_count + 1);
	struct sffield **fields = ss_malloc(a, size);
	if (ssunlikely(fields == NULL))
		return -1;
	memcpy(fields, s->fields, size - sizeof(struct sffield*));
	fields[s->fields_count] = f;
	f->position = s->fields_count;
	f->position_key = -1;
	if (s->fields)
		ss_free(a, s->fields);
	s->fields = fields;
	s->fields_count++;
	return 0;
}

static inline int
sf_schemeset(struct sfscheme *s, struct sffield *f, char *opt)
{
	(void)s;
	if (strcmp(opt, "string") == 0) {
		f->type = SS_STRING;
		f->fixed_size = 0;
		f->cmp = sf_cmpstring;
	} else
	if (strcmp(opt, "u32") == 0) {
		f->type = SS_U32;
		f->fixed_size = sizeof(uint32_t);
		f->cmp = sf_cmpu32;
	} else
	if (strcmp(opt, "u32_rev") == 0) {
		f->type = SS_U32REV;
		f->fixed_size = sizeof(uint32_t);
		f->cmp = sf_cmpu32_reverse;
	} else
	if (strcmp(opt, "u64") == 0) {
		f->type = SS_U64;
		f->fixed_size = sizeof(uint64_t);
		f->cmp = sf_cmpu64;
	} else
	if (strcmp(opt, "u64_rev") == 0) {
		f->type = SS_U64REV;
		f->fixed_size = sizeof(uint64_t);
		f->cmp = sf_cmpu64_reverse;
	} else
	if (strncmp(opt, "key", 3) == 0) {
		char *p = opt + 3;
		if (ssunlikely(*p != '('))
			return -1;
		p++;
		if (ssunlikely(! isdigit(*p)))
			return -1;
		int v = 0;
		while (isdigit(*p)) {
			v = (v * 10) + *p - '0';
			p++;
		}
		if (ssunlikely(*p != ')'))
			return -1;
		p++;
		f->position_key = v;
		f->key = 1;
	} else {
		return -1;
	}
	return 0;
}

static int
sf_schemevalidate(struct sfscheme *s, struct ssa *a)
{
	/* validate fields */
	if (s->fields_count == 0) {
		return -1;
	}
	int fixed_offset = 0;
	int fixed_pos = 0;
	int i = 0;
	while (i < s->fields_count)
	{
		/* validate and apply field options */
		struct sffield *f = s->fields[i];
		if (f->options == NULL) {
			return -1;
		}
		char opts[256];
		snprintf(opts, sizeof(opts), "%s", f->options);
		char *p;
		for (p = strtok(opts, " ,"); p;
		     p = strtok(NULL, " ,"))
		{
			int rc = sf_schemeset(s, f, p);
			if (ssunlikely(rc == -1))
				return -1;
		}
		/* calculate offset and position for fixed
		 * size types */
		if (f->fixed_size > 0) {
			f->position_ref = fixed_pos;
			fixed_pos++;
			f->fixed_offset = fixed_offset;
			fixed_offset += f->fixed_size;
		} else {
			s->var_count++;
		}
		if (f->key)
			s->keys_count++;
		i++;
	}
	s->var_offset = fixed_offset;

	/* validate keys */
	if (ssunlikely(s->keys_count == 0))
		return -1;
	int size = sizeof(struct sffield*) * s->keys_count;
	s->keys = ss_malloc(a, size);
	if (ssunlikely(s->keys == NULL))
		return -1;
	memset(s->keys, 0, size);
	int pos_var = 0;
	i = 0;
	while (i < s->fields_count) {
		struct sffield *f = s->fields[i];
		if (f->key) {
			if (ssunlikely(f->position_key < 0))
				return -1;
			if (ssunlikely(f->position_key >= s->fields_count))
				return -1;
			if (ssunlikely(f->position_key >= s->keys_count))
				return -1;
			if (ssunlikely(s->keys[f->position_key] != NULL))
				return -1;
			s->keys[f->position_key] = f;
		}
		if (f->fixed_size == 0)
			f->position_ref = pos_var++;
		i++;
	}
	i = 0;
	while (i < s->keys_count) {
		struct sffield *f = s->keys[i];
		if (f == NULL)
			return -1;
		i++;
	}
	return 0;
}

static int
sf_schemesave(struct sfscheme *s, struct ssa *a, struct ssbuf *buf)
{
	/* fields count */
	uint32_t v = s->fields_count;
	int rc = ss_bufadd(buf, a, &v, sizeof(uint32_t));
	if (ssunlikely(rc == -1))
		return -1;
	int i = 0;
	while (i < s->fields_count) {
		struct sffield *field = s->fields[i];
		/* name */
		v = strlen(field->name) + 1;
		rc = ss_bufensure(buf, a, sizeof(uint32_t) + v);
		if (ssunlikely(rc == -1))
			goto error;
		memcpy(buf->p, &v, sizeof(v));
		ss_bufadvance(buf, sizeof(uint32_t));
		memcpy(buf->p, field->name, v);
		ss_bufadvance(buf, v);
		/* options */
		v = strlen(field->options) + 1;
		rc = ss_bufensure(buf, a, sizeof(uint32_t) + v);
		if (ssunlikely(rc == -1))
			goto error;
		memcpy(buf->p, &v, sizeof(v));
		ss_bufadvance(buf, sizeof(uint32_t));
		memcpy(buf->p, field->options, v);
		ss_bufadvance(buf, v);
		i++;
	}
	return 0;
error:
	ss_buffree(buf, a);
	return -1;
}

static int
sf_schemeload(struct sfscheme *s, struct ssa *a, char *buf, int size ssunused)
{
	/* count */
	char *p = buf;
	uint32_t v = load_u32(p);
	p += sizeof(uint32_t);
	int count = v;
	int i = 0;
	while (i < count) {
		/* name */
		v = load_u32(p);
		p += sizeof(uint32_t);
		struct sffield *field = sf_fieldnew(a, p);
		if (ssunlikely(field == NULL))
			goto error;
		p += v;
		/* options */
		v = load_u32(p);
		p += sizeof(uint32_t);
		int rc = sf_fieldoptions(field, a, p);
		if (ssunlikely(rc == -1)) {
			sf_fieldfree(field, a);
			goto error;
		}
		rc = sf_schemeadd(s, a, field);
		if (ssunlikely(rc == -1)) {
			sf_fieldfree(field, a);
			goto error;
		}
		p += v;
		i++;
	}
	return 0;
error:
	sf_schemefree(s, a);
	return -1;
}

static struct sffield*
sf_schemefind(struct sfscheme *s, char *name)
{
	int i;
	for (i = 0; i < s->fields_count; i++)
		if (strcmp(s->fields[i]->name, name) == 0)
			return s->fields[i];
	return NULL;
}

#define SR_VERSION_MAGIC      8529643324614668147ULL

#define SR_VERSION_A         '2'
#define SR_VERSION_B         '1'
#define SR_VERSION_C         '1'

#define SR_VERSION_STORAGE_A '2'
#define SR_VERSION_STORAGE_B '1'
#define SR_VERSION_STORAGE_C '1'

struct srversion {
	uint64_t magic;
	uint8_t  a, b, c;
} sspacked;

static inline void
sr_version(struct srversion *v)
{
	v->magic = SR_VERSION_MAGIC;
	v->a = SR_VERSION_A;
	v->b = SR_VERSION_B;
	v->c = SR_VERSION_C;
}

static inline void
sr_version_storage(struct srversion *v)
{
	v->magic = SR_VERSION_MAGIC;
	v->a = SR_VERSION_STORAGE_A;
	v->b = SR_VERSION_STORAGE_B;
	v->c = SR_VERSION_STORAGE_C;
}

static inline int
sr_versionstorage_check(struct srversion *v)
{
	if (v->magic != SR_VERSION_MAGIC)
		return 0;
	if (v->a != SR_VERSION_STORAGE_A)
		return 0;
	if (v->b != SR_VERSION_STORAGE_B)
		return 0;
	if (v->c != SR_VERSION_STORAGE_C)
		return 0;
	return 1;
}

enum {
	SR_ERROR_NONE  = 0,
	SR_ERROR = 1,
	SR_ERROR_MALFUNCTION = 2
};

struct srerror {
	pthread_mutex_t lock;
	int type;
	const char *file;
	const char *function;
	int line;
	char error[256];
};

static inline void
sr_errorinit(struct srerror *e) {
	e->type = SR_ERROR_NONE;
	e->error[0] = 0;
	e->line = 0;
	e->function = NULL;
	e->file = NULL;
	tt_pthread_mutex_init(&e->lock, NULL);
}

/* TODO: where is se_delete() ? */
static inline __attribute__((unused)) void
sr_errorfree(struct srerror *e) {
	tt_pthread_mutex_destroy(&e->lock);
}

static inline void
sr_errorreset(struct srerror *e) {
	tt_pthread_mutex_lock(&e->lock);
	e->type = SR_ERROR_NONE;
	e->error[0] = 0;
	e->line = 0;
	e->function = NULL;
	e->file = NULL;
	tt_pthread_mutex_unlock(&e->lock);
}

static inline void
sr_errorrecover(struct srerror *e) {
	tt_pthread_mutex_lock(&e->lock);
	assert(e->type == SR_ERROR_MALFUNCTION);
	e->type = SR_ERROR;
	tt_pthread_mutex_unlock(&e->lock);
}

static inline void
sr_malfunction_set(struct srerror *e) {
	tt_pthread_mutex_lock(&e->lock);
	e->type = SR_ERROR_MALFUNCTION;
	tt_pthread_mutex_unlock(&e->lock);
}

static inline int
sr_errorof(struct srerror *e) {
	tt_pthread_mutex_lock(&e->lock);
	int type = e->type;
	tt_pthread_mutex_unlock(&e->lock);
	return type;
}

static inline int
sr_errorcopy(struct srerror *e, char *buf, int bufsize) {
	tt_pthread_mutex_lock(&e->lock);
	int len = snprintf(buf, bufsize, "%s", e->error);
	tt_pthread_mutex_unlock(&e->lock);
	return len;
}

static inline void
sr_verrorset(struct srerror *e, int type,
             const char *file,
             const char *function, int line,
             char *fmt, va_list args)
{
	tt_pthread_mutex_lock(&e->lock);
	if (ssunlikely(e->type == SR_ERROR_MALFUNCTION)) {
		tt_pthread_mutex_unlock(&e->lock);
		return;
	}
	e->file     = file;
	e->function = function;
	e->line     = line;
	e->type     = type;
	int len;
	len = snprintf(e->error, sizeof(e->error), "%s:%d ", file, line);
	vsnprintf(e->error + len, sizeof(e->error) - len, fmt, args);
	tt_pthread_mutex_unlock(&e->lock);
}

static inline int
sr_errorset(struct srerror *e, int type,
            const char *file,
            const char *function, int line,
            char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	sr_verrorset(e, type, file, function, line, fmt, args);
	va_end(args);
	return -1;
}

#define sr_e(e, type, fmt, ...) \
	sr_errorset(e, type, __FILE__, __func__, __LINE__, fmt, __VA_ARGS__)

#define sr_error(e, fmt, ...) \
	sr_e(e, SR_ERROR, fmt, __VA_ARGS__)

#define sr_malfunction(e, fmt, ...) \
	sr_e(e, SR_ERROR_MALFUNCTION, fmt, __VA_ARGS__)

#define sr_oom(e) \
	sr_e(e, SR_ERROR, "%s", "memory allocation failed")

#define sr_oom_malfunction(e) \
	sr_e(e, SR_ERROR_MALFUNCTION, "%s", "memory allocation failed")

enum {
	SR_OFFLINE,
	SR_ONLINE,
	SR_RECOVER,
	SR_SHUTDOWN_PENDING,
	SR_SHUTDOWN,
	SR_DROP_PENDING,
	SR_DROP,
	SR_MALFUNCTION
};

struct srstatus {
	int status;
	pthread_mutex_t lock;
};

static inline void
sr_statusinit(struct srstatus *s)
{
	s->status = SR_OFFLINE;
	tt_pthread_mutex_init(&s->lock, NULL);
}

static inline void
sr_statusfree(struct srstatus *s)
{
	tt_pthread_mutex_destroy(&s->lock);
}

static inline int
sr_statusset(struct srstatus *s, int status)
{
	tt_pthread_mutex_lock(&s->lock);
	int old = s->status;
	s->status = status;
	tt_pthread_mutex_unlock(&s->lock);
	return old;
}

static inline int
sr_status(struct srstatus *s)
{
	tt_pthread_mutex_lock(&s->lock);
	int status = s->status;
	tt_pthread_mutex_unlock(&s->lock);
	return status;
}

static inline char*
sr_statusof(struct srstatus *s)
{
	int status = sr_status(s);
	switch (status) {
	case SR_OFFLINE:          return "offline";
	case SR_ONLINE:           return "online";
	case SR_RECOVER:          return "recover";
	case SR_SHUTDOWN_PENDING: return "shutdown_pending";
	case SR_SHUTDOWN:         return "shutdown";
	case SR_DROP_PENDING:     return "drop";
	case SR_DROP:             return "drop";
	case SR_MALFUNCTION:      return "malfunction";
	}
	assert(0);
	return NULL;
}

static inline int
sr_statusactive_is(int status)
{
	switch (status) {
	case SR_ONLINE:
	case SR_RECOVER:
		return 1;
	case SR_SHUTDOWN_PENDING:
	case SR_SHUTDOWN:
	case SR_DROP_PENDING:
	case SR_DROP:
	case SR_OFFLINE:
	case SR_MALFUNCTION:
		return 0;
	}
	assert(0);
	return 0;
}

static inline int
sr_statusactive(struct srstatus *s) {
	return sr_statusactive_is(sr_status(s));
}

static inline int
sr_online(struct srstatus *s) {
	return sr_status(s) == SR_ONLINE;
}

struct srstat {
	pthread_mutex_t lock;
	/* memory */
	uint64_t v_count;
	uint64_t v_allocated;
	/* key-value */
	struct ssavg    key, value;
	/* set */
	uint64_t set;
	struct ssavg    set_latency;
	/* delete */
	uint64_t del;
	struct ssavg    del_latency;
	/* upsert */
	uint64_t upsert;
	struct ssavg    upsert_latency;
	/* get */
	uint64_t get;
	struct ssavg    get_read_disk;
	struct ssavg    get_read_cache;
	struct ssavg    get_latency;
	/* transaction */
	uint64_t tx;
	uint64_t tx_rlb;
	uint64_t tx_conflict;
	uint64_t tx_lock;
	struct ssavg    tx_latency;
	struct ssavg    tx_stmts;
	/* cursor */
	uint64_t cursor;
	struct ssavg    cursor_latency;
	struct ssavg    cursor_read_disk;
	struct ssavg    cursor_read_cache;
	struct ssavg    cursor_ops;
};

static inline void
sr_statinit(struct srstat *s)
{
	memset(s, 0, sizeof(*s));
	tt_pthread_mutex_init(&s->lock, NULL);
}

static inline void
sr_statfree(struct srstat *s) {
	tt_pthread_mutex_destroy(&s->lock);
}

static inline void
sr_statprepare(struct srstat *s)
{
	ss_avgprepare(&s->key);
	ss_avgprepare(&s->value);
	ss_avgprepare(&s->set_latency);
	ss_avgprepare(&s->del_latency);
	ss_avgprepare(&s->upsert_latency);
	ss_avgprepare(&s->get_read_disk);
	ss_avgprepare(&s->get_read_cache);
	ss_avgprepare(&s->get_latency);
	ss_avgprepare(&s->tx_latency);
	ss_avgprepare(&s->tx_stmts);
	ss_avgprepare(&s->cursor_latency);
	ss_avgprepare(&s->cursor_read_disk);
	ss_avgprepare(&s->cursor_read_cache);
	ss_avgprepare(&s->cursor_ops);
}

static inline void
sr_statkey(struct srstat *s, int size)
{
	tt_pthread_mutex_lock(&s->lock);
	ss_avgupdate(&s->key, size);
	tt_pthread_mutex_unlock(&s->lock);
}

static inline void
sr_statset(struct srstat *s, uint64_t start)
{
	uint64_t diff = clock_monotonic64() - start;
	tt_pthread_mutex_lock(&s->lock);
	s->set++;
	ss_avgupdate(&s->set_latency, diff);
	tt_pthread_mutex_unlock(&s->lock);
}

static inline void
sr_statdelete(struct srstat *s, uint64_t start)
{
	uint64_t diff = clock_monotonic64() - start;
	tt_pthread_mutex_lock(&s->lock);
	s->del++;
	ss_avgupdate(&s->del_latency, diff);
	tt_pthread_mutex_unlock(&s->lock);
}

static inline void
sr_statupsert(struct srstat *s, uint64_t start)
{
	uint64_t diff = clock_monotonic64() - start;
	tt_pthread_mutex_lock(&s->lock);
	s->upsert++;
	ss_avgupdate(&s->upsert_latency, diff);
	tt_pthread_mutex_unlock(&s->lock);
}

static inline void
sr_statget(struct srstat *s, uint64_t diff, int read_disk, int read_cache)
{
	tt_pthread_mutex_lock(&s->lock);
	s->get++;
	ss_avgupdate(&s->get_read_disk, read_disk);
	ss_avgupdate(&s->get_read_cache, read_cache);
	ss_avgupdate(&s->get_latency, diff);
	tt_pthread_mutex_unlock(&s->lock);
}

static inline void
sr_stattx(struct srstat *s, uint64_t start, uint32_t count,
          int rlb, int conflict)
{
	uint64_t diff = clock_monotonic64() - start;
	tt_pthread_mutex_lock(&s->lock);
	s->tx++;
	s->tx_rlb += rlb;
	s->tx_conflict += conflict;
	ss_avgupdate(&s->tx_stmts, count);
	ss_avgupdate(&s->tx_latency, diff);
	tt_pthread_mutex_unlock(&s->lock);
}

static inline void
sr_stattx_lock(struct srstat *s)
{
	tt_pthread_mutex_lock(&s->lock);
	s->tx_lock++;
	tt_pthread_mutex_unlock(&s->lock);
}

static inline void
sr_statcursor(struct srstat *s, uint64_t start, int read_disk, int read_cache, int ops)
{
	uint64_t diff = clock_monotonic64() - start;
	tt_pthread_mutex_lock(&s->lock);
	s->cursor++;
	ss_avgupdate(&s->cursor_read_disk, read_disk);
	ss_avgupdate(&s->cursor_read_cache, read_cache);
	ss_avgupdate(&s->cursor_latency, diff);
	ss_avgupdate(&s->cursor_ops, ops);
	tt_pthread_mutex_unlock(&s->lock);
}

enum srseqop {
	SR_DSN,
	SR_DSNNEXT,
	SR_NSN,
	SR_NSNNEXT,
	SR_ASN,
	SR_ASNNEXT,
	SR_SSN,
	SR_SSNNEXT,
	SR_BSN,
	SR_BSNNEXT,
	SR_LSN,
	SR_LSNNEXT,
	SR_LFSN,
	SR_LFSNNEXT,
	SR_TSN,
	SR_TSNNEXT
};

struct srseq {
	pthread_mutex_t lock;
	/** Log sequence number. */
	uint64_t lsn;
	/** Transaction sequence number. */
	uint64_t tsn;
	/** Node sequence number. */
	uint64_t nsn;
	/** Snapshot sequence number. */
	uint64_t ssn;
	/** Anti-cache sequence number. */
	uint64_t asn;
	/** Log file sequence number. */
	uint64_t lfsn;
	/** Database sequence number. */
	uint32_t dsn;
	/** Backup sequence number. */
	uint32_t bsn;
};

static inline void
sr_seqinit(struct srseq *n) {
	memset(n, 0, sizeof(*n));
	tt_pthread_mutex_init(&n->lock, NULL);
}

static inline void
sr_seqfree(struct srseq *n) {
	tt_pthread_mutex_destroy(&n->lock);
}

static inline void
sr_seqlock(struct srseq *n) {
	tt_pthread_mutex_lock(&n->lock);
}

static inline void
sr_sequnlock(struct srseq *n) {
	tt_pthread_mutex_unlock(&n->lock);
}

static inline uint64_t
sr_seqdo(struct srseq *n, enum srseqop op)
{
	uint64_t v = 0;
	switch (op) {
	case SR_LSN:       v = n->lsn;
		break;
	case SR_LSNNEXT:   v = ++n->lsn;
		break;
	case SR_TSN:       v = n->tsn;
		break;
	case SR_TSNNEXT:   v = ++n->tsn;
		break;
	case SR_NSN:       v = n->nsn;
		break;
	case SR_NSNNEXT:   v = ++n->nsn;
		break;
	case SR_LFSN:      v = n->lfsn;
		break;
	case SR_LFSNNEXT:  v = ++n->lfsn;
		break;
	case SR_SSN:       v = n->ssn;
		break;
	case SR_SSNNEXT:   v = ++n->ssn;
		break;
	case SR_ASN:       v = n->asn;
		break;
	case SR_ASNNEXT:   v = ++n->asn;
		break;
	case SR_BSN:       v = n->bsn;
		break;
	case SR_BSNNEXT:   v = ++n->bsn;
		break;
	case SR_DSN:       v = n->dsn;
		break;
	case SR_DSNNEXT:   v = ++n->dsn;
		break;
	}
	return v;
}

static inline uint64_t
sr_seq(struct srseq *n, enum srseqop op)
{
	sr_seqlock(n);
	uint64_t v = sr_seqdo(n, op);
	sr_sequnlock(n);
	return v;
}

struct srzone {
	uint32_t enable;
	char     name[4];
	uint32_t mode;
	uint32_t compact_wm;
	uint32_t compact_mode;
	uint32_t branch_prio;
	uint32_t branch_wm;
	uint32_t branch_age;
	uint32_t branch_age_period;
	uint64_t branch_age_period_us;
	uint32_t branch_age_wm;
	uint32_t backup_prio;
	uint32_t snapshot_period;
	uint64_t snapshot_period_us;
	uint32_t anticache_period;
	uint64_t anticache_period_us;
	uint32_t expire_prio;
	uint32_t expire_period;
	uint64_t expire_period_us;
	uint32_t gc_prio;
	uint32_t gc_period;
	uint64_t gc_period_us;
	uint32_t gc_wm;
	uint32_t lru_prio;
	uint32_t lru_period;
	uint64_t lru_period_us;
};

struct srzonemap {
	struct srzone zones[11];
};

static inline void
sr_zonemap_set(struct srzonemap *m, uint32_t percent, struct srzone *z)
{
	if (ssunlikely(percent > 100))
		percent = 100;
	percent = percent - percent % 10;
	int p = percent / 10;
	m->zones[p] = *z;
	snprintf(m->zones[p].name, sizeof(m->zones[p].name), "%d", percent);
}

static inline struct srzone*
sr_zonemap(struct srzonemap *m, uint32_t percent)
{
	if (ssunlikely(percent > 100))
		percent = 100;
	percent = percent - percent % 10;
	int p = percent / 10;
	struct srzone *z = &m->zones[p];
	if (!z->enable) {
		while (p >= 0) {
			z = &m->zones[p];
			if (z->enable)
				return z;
			p--;
		}
		return NULL;
	}
	return z;
}

struct runtime {
	struct srstatus *status;
	struct srerror *e;
	struct sfupsert *fmt_upsert;
	enum sfstorage fmt_storage;
	struct sfscheme *scheme;
	struct srseq *seq;
	struct ssa *a;
	struct ssa *aref;
	struct ssvfs *vfs;
	struct ssquota *quota;
	struct srzonemap *zonemap;
	struct ssinjection *i;
	struct srstat *stat;
};

static inline void
sr_init(struct runtime *r,
        struct srstatus *status,
        struct srerror *e,
        struct ssa *a,
        struct ssa *aref,
        struct ssvfs *vfs,
        struct ssquota *quota,
        struct srzonemap *zonemap,
        struct srseq *seq,
        enum sfstorage fmt_storage,
        struct sfupsert *fmt_upsert,
        struct sfscheme *scheme,
        struct ssinjection *i,
        struct srstat *stat)
{
	r->status      = status;
	r->e           = e;
	r->a           = a;
	r->aref        = aref;
	r->vfs         = vfs;
	r->quota       = quota;
	r->zonemap     = zonemap;
	r->seq         = seq;
	r->scheme      = scheme;
	r->fmt_storage = fmt_storage;
	r->fmt_upsert  = fmt_upsert;
	r->i           = i;
	r->stat        = stat;
}

static inline struct srzone *sr_zoneof(struct runtime *r)
{
	int p = ss_quotaused_percent(r->quota);
	return sr_zonemap(r->zonemap, p);
}

struct srconfstmt;
struct srconf;

typedef int (*srconff)(struct srconf*, struct srconfstmt*);

enum srconfop {
	SR_WRITE,
	SR_READ,
	SR_SERIALIZE
};

enum {
	SR_RO = 1,
	SR_NS = 2
};

struct srconf {
	char    *key;
	int      flags;
	enum sstype   type;
	srconff  function;
	void    *value;
	void    *ptr;
	struct srconf  *next;
};

struct srconfdump {
	uint8_t  type;
	uint16_t keysize;
	uint32_t valuesize;
} sspacked;

struct srconfstmt {
	enum srconfop op;
	const char *path;
	void       *value;
	enum sstype valuetype;
	int         valuesize;
	struct srconf     *match;
	struct ssbuf      *serialize;
	void       *ptr;
	struct runtime         *r;
};

static int sr_confexec(struct srconf*, struct srconfstmt*);
static int sr_conf_read(struct srconf*, struct srconfstmt*);
static int sr_conf_write(struct srconf*, struct srconfstmt*);
static int sr_conf_serialize(struct srconf*, struct srconfstmt*);

static inline struct srconf*
sr_c(struct srconf **link, struct srconf **cp, srconff func,
     char *key, int type,
     void *value)
{
	struct srconf *c = *cp;
	c->key      = key;
	c->function = func;
	c->flags    = 0;
	c->type     = type;
	c->value    = value;
	c->ptr      = NULL;
	c->next     = NULL;
	*cp = c + 1;
	if (sslikely(link)) {
		if (sslikely(*link))
			(*link)->next = c;
		*link = c;
	}
	return c;
}

static inline struct srconf*
sr_C(struct srconf **link, struct srconf **cp, srconff func,
     char *key, int type,
     void *value, int flags, void *ptr)
{
	struct srconf *c = sr_c(link, cp, func, key, type, value);
	c->flags = flags;
	c->ptr = ptr;
	return c;
}

static inline char*
sr_confkey(struct srconfdump *v) {
	return (char*)v + sizeof(struct srconfdump);
}

static inline char*
sr_confvalue(struct srconfdump *v) {
	return sr_confkey(v) + v->keysize;
}

static int sr_conf_read(struct srconf *m, struct srconfstmt *s)
{
	switch (m->type) {
	case SS_U32:
		s->valuesize = sizeof(uint32_t);
		if (s->valuetype == SS_I64) {
			store_u64(s->value, (int64_t)load_u32(m->value));
		} else
		if (s->valuetype == SS_U32) {
			store_u32(s->value, load_u32(m->value));
		} else
		if (s->valuetype == SS_U64) {
			store_u64(s->value, load_u32(m->value));
		} else {
			goto bad_type;
		}
		break;
	case SS_U64:
		s->valuesize = sizeof(uint64_t);
		if (s->valuetype == SS_I64) {
			store_u64(s->value, load_u64(m->value));
		} else
		if (s->valuetype == SS_U32) {
			store_u32(s->value, load_u64(m->value));
		} else
		if (s->valuetype == SS_U64) {
			store_u64(s->value, load_u64(m->value));
		} else {
			goto bad_type;
		}
		break;
	case SS_STRING: {
		if (s->valuetype != SS_STRING)
			goto bad_type;
		char **result = s->value;
		*result = NULL;
		s->valuesize = 0;
		char *string = m->value;
		if (string == NULL)
			break;
		int size = strlen(string) + 1;
		s->valuesize = size;
		*result = malloc(size);
		if (ssunlikely(*result == NULL))
			return sr_oom(s->r->e);
		memcpy(*result, string, size);
		break;
	}
	case SS_STRINGPTR: {
		if (s->valuetype != SS_STRING)
			goto bad_type;
		char **result = s->value;
		*result = NULL;
		s->valuesize = 0;
		char **string = m->value;
		if (*string == NULL)
			break;
		int size = strlen(*string) + 1;
		s->valuesize = size;
		*result = malloc(size);
		if (ssunlikely(*result == NULL))
			return sr_oom(s->r->e);
		memcpy(*result, *string, size);
		break;
	}
	case SS_OBJECT:
		if (s->valuetype != SS_STRING)
			goto bad_type;
		*(void**)s->value = m->value;
		s->valuesize = sizeof(void*);
		break;
	default:
		goto bad_type;
	}

	return 0;

bad_type:
	return sr_error(s->r->e, "configuration read bad type (%s) -> (%s) %s",
	                ss_typeof(s->valuetype),
	                ss_typeof(m->type), s->path);
}

static int
sr_conf_write(struct srconf *m, struct srconfstmt *s)
{
	if (m->flags & SR_RO) {
		sr_error(s->r->e, "%s is read-only", s->path);
		return -1;
	}
	switch (m->type) {
	case SS_U32:
		if (s->valuetype == SS_I64) {
			store_u32(m->value, load_u64(s->value));
		} else
		if (s->valuetype == SS_U32) {
			store_u32(m->value, load_u32(s->value));
		} else
		if (s->valuetype == SS_U64) {
			store_u32(m->value, load_u64(s->value));
		} else {
			goto bad_type;
		}
		break;
	case SS_U64:
		if (s->valuetype == SS_I64) {
			store_u64(m->value, load_u64(s->value));
		} else
		if (s->valuetype == SS_U32) {
			store_u64(m->value, load_u32(s->value));
		} else
		if (s->valuetype == SS_U64) {
			store_u64(m->value, load_u64(s->value));
		} else {
			goto bad_type;
		}
		break;
	case SS_STRINGPTR: {
		char **string = m->value;
		if (s->valuetype == SS_STRING) {
			int len = s->valuesize + 1;
			char *sz;
			sz = ss_malloc(s->r->a, len);
			if (ssunlikely(sz == NULL))
				return sr_oom(s->r->e);
			memcpy(sz, s->value, s->valuesize);
			sz[s->valuesize] = 0;
			if (*string)
				ss_free(s->r->a, *string);
			*string = sz;
		} else {
			goto bad_type;
		}
		break;
	}
	default:
		goto bad_type;
	}
	return 0;

bad_type:
	return sr_error(s->r->e, "configuration write bad type (%s) for (%s) %s",
	                ss_typeof(s->valuetype),
	                ss_typeof(m->type), s->path);
}

static inline int
sr_conf_write_cast(enum sstype a, enum sstype b)
{
	switch (a) {
	case SS_U32:
		if (b == SS_I64) {
		} else
		if (b == SS_U32) {
		} else
		if (b == SS_U64) {
		} else {
			return -1;
		}
		break;
	case SS_U64:
		if (b == SS_I64) {
		} else
		if (b == SS_U32) {
		} else
		if (b == SS_U64) {
		} else {
			return -1;
		}
		break;
	case SS_STRING:
	case SS_STRINGPTR:
		if (b == SS_STRING) {
		} else {
			return -1;
		}
		break;
	default:
		return -1;
	}
	return 0;
}

static int
sr_conf_serialize(struct srconf *m, struct srconfstmt *s)
{
	char buf[128];
	char name_function[] = "function";
	char name_object[] = "object";
	void *value = NULL;
	struct srconfdump v = {
		.type = m->type
	};
	switch (m->type) {
	case SS_U32:
		v.valuesize  = snprintf(buf, sizeof(buf), "%" PRIu32, load_u32(m->value));
		v.valuesize += 1;
		value = buf;
		break;
	case SS_U64:
		v.valuesize  = snprintf(buf, sizeof(buf), "%" PRIu64, load_u64(m->value));
		v.valuesize += 1;
		value = buf;
		break;
	case SS_I64:
		v.valuesize  = snprintf(buf, sizeof(buf), "%" PRIi64, load_u64(m->value));
		v.valuesize += 1;
		value = buf;
		break;
	case SS_STRING: {
		char *string = m->value;
		if (string) {
			v.valuesize = strlen(string) + 1;
			value = string;
		} else {
			v.valuesize = 0;
		}
		break;
	}
	case SS_STRINGPTR: {
		char **string = (char**)m->value;
		if (*string) {
			v.valuesize = strlen(*string) + 1;
			value = *string;
		} else {
			v.valuesize = 0;
		}
		v.type = SS_STRING;
		break;
	}
	case SS_OBJECT:
		v.type = SS_STRING;
		v.valuesize = sizeof(name_object);
		value = name_object;
		break;
	case SS_FUNCTION:
		v.type = SS_STRING;
		v.valuesize = sizeof(name_function);
		value = name_function;
		break;
	default:
		return -1;
	}
	char name[128];
	v.keysize  = snprintf(name, sizeof(name), "%s", s->path);
	v.keysize += 1;
	struct ssbuf *p = s->serialize;
	int size = sizeof(v) + v.keysize + v.valuesize;
	int rc = ss_bufensure(p, s->r->a, size);
	if (ssunlikely(rc == -1))
		return sr_oom(s->r->e);
	memcpy(p->p, &v, sizeof(v));
	memcpy(p->p + sizeof(v), name, v.keysize);
	memcpy(p->p + sizeof(v) + v.keysize, value, v.valuesize);
	ss_bufadvance(p, size);
	return 0;
}

static inline int
sr_confexec_serialize(struct srconf *c, struct srconfstmt *stmt, char *root)
{
	char path[256];
	while (c) {
		if (root)
			snprintf(path, sizeof(path), "%s.%s", root, c->key);
		else
			snprintf(path, sizeof(path), "%s", c->key);
		int rc;
		if (c->flags & SR_NS) {
			rc = sr_confexec_serialize(c->value, stmt, path);
			if (ssunlikely(rc == -1))
				return -1;
		} else {
			stmt->path = path;
			rc = c->function(c, stmt);
			if (ssunlikely(rc == -1))
				return -1;
			stmt->path = NULL;
		}
		c = c->next;
	}
	return 0;
}

static int sr_confexec(struct srconf *start, struct srconfstmt *s)
{
	if (s->op == SR_SERIALIZE)
		return sr_confexec_serialize(start, s, NULL);
	char path[256];
	snprintf(path, sizeof(path), "%s", s->path);
	char *ptr = NULL;
	char *token;
	token = strtok_r(path, ".", &ptr);
	if (ssunlikely(token == NULL))
		return -1;
	struct srconf *c = start;
	while (c) {
		if (strcmp(token, c->key) != 0) {
			c = c->next;
			continue;
		}
		if (c->flags & SR_NS) {
			token = strtok_r(NULL, ".", &ptr);
			if (ssunlikely(token == NULL))
			{
				if (s->op == SR_WRITE && c->type != SS_UNDEF) {
					int rc = sr_conf_write_cast(c->type, s->valuetype);
					if (ssunlikely(rc == -1))
						goto bad_type;
				}
				s->match = c;
				if (c->function)
					return c->function(c, s);
				/* not supported */
				goto bad_path;
			}
			c = (struct srconf*)c->value;
			continue;
		}
		s->match = c;
		token = strtok_r(NULL, ".", &ptr);
		if (ssunlikely(token != NULL))
			goto bad_path;
		return c->function(c, s);
	}

bad_path:
	return sr_error(s->r->e, "bad configuration path: %s", s->path);

bad_type:
	return sr_error(s->r->e, "incompatible type (%s) for (%s) %s",
	                ss_typeof(s->valuetype),
	                ss_typeof(c->type), s->path);
}

struct so;

struct soif {
	int      (*open)(struct so*);
	int      (*close)(struct so*);
	int      (*destroy)(struct so*);
	void     (*free)(struct so*);
	void    *(*document)(struct so*);
	int      (*drop)(struct so*);
	int      (*setstring)(struct so*, const char*, void*, int);
	int      (*setint)(struct so*, const char*, int64_t);
	int      (*setobject)(struct so*, const char*, void*);
	void    *(*getobject)(struct so*, const char*);
	void    *(*getstring)(struct so*, const char*, int*);
	int64_t  (*getint)(struct so*, const char*);
	int      (*set)(struct so*, struct so*);
	int      (*upsert)(struct so*, struct so*);
	int      (*del)(struct so*, struct so*);
	void    *(*get)(struct so*, struct so*);
	void    *(*begin)(struct so*);
	int      (*commit)(struct so*);
	void    *(*cursor)(struct so*);
};

struct sotype {
	uint32_t magic;
	char *name;
};

struct so {
	struct soif *i;
	struct sotype *type;
	struct so *parent;
	struct so *env;
};

static inline void
so_init(struct so *o, struct sotype *type, struct soif *i, struct so *parent, struct so *env)
{
	o->type      = type;
	o->i         = i;
	o->parent    = parent;
	o->env       = env;
}

static inline void*
so_cast_dynamic(void *ptr, struct sotype *type,
          const char *file,
          const char *function, int line)
{
	int eq = ptr != NULL && ((struct so*)ptr)->type == type;
	if (sslikely(eq))
		return ptr;
	fprintf(stderr, "%s:%d %s(%p) expected '%s' object\n",
	        file, line, function, ptr, type->name);
	abort();
	return NULL;
}

#define so_cast(o, cast, type) \
	((cast)so_cast_dynamic(o, type, __FILE__, __func__, __LINE__))

#define so_open(o)      (o)->i->open(o)
#define so_close(o)     (o)->i->close(o)
#define so_destroy(o)   (o)->i->destroy(o)
#define so_free(o)      (o)->i->free(o)
#define so_document(o)  (o)->i->document(o)
#define so_drop(o)      (o)->i->drop(o)
#define so_set(o, v)    (o)->i->set(o, v)
#define so_upsert(o, v) (o)->i->upsert(o, v)
#define so_delete(o, v) (o)->i->del(o, v)
#define so_get(o, v)    (o)->i->get(o, v)
#define so_begin(o)     (o)->i->begin(o)
#define so_commit(o)    (o)->i->commit(o)
#define so_cursor(o)    (o)->i->cursor(o)

#define so_setstring(o, path, pointer, size) \
	(o)->i->setstring(o, path, pointer, size)
#define so_setint(o, path, v) \
	(o)->i->setint(o, path, v)
#define so_getobject(o, path) \
	(o)->i->getobject(o, path)
#define so_getstring(o, path, sizep) \
	(o)->i->getstring(o, path, sizep)
#define so_getint(o, path) \
	(o)->i->getnumber(o, path)

#define SVNONE       0
#define SVDELETE     1
#define SVUPSERT     2
#define SVGET        4
#define SVDUP        8
#define SVBEGIN     16
#define SVCONFLICT  32

struct sv;

struct svif {
	uint8_t   (*flags)(struct sv*);
	void      (*lsnset)(struct sv*, uint64_t);
	uint64_t  (*lsn)(struct sv*);
	uint32_t  (*timestamp)(struct sv*);
	char     *(*pointer)(struct sv*);
	uint32_t  (*size)(struct sv*);
};

struct sv {
	struct svif *i;
	void *v, *arg;
} sspacked;

static inline void
sv_init(struct sv *v, struct svif *i, void *vptr, void *arg) {
	v->i   = i;
	v->v   = vptr;
	v->arg = arg;
}

static inline uint8_t
sv_flags(struct sv *v) {
	return v->i->flags(v);
}

static inline int
sv_isflags(int flags, int value) {
	return (flags & value) > 0;
}

static inline int
sv_is(struct sv *v, int flags) {
	return sv_isflags(sv_flags(v), flags) > 0;
}

static inline uint64_t
sv_lsn(struct sv *v) {
	return v->i->lsn(v);
}

static inline void
sv_lsnset(struct sv *v, uint64_t lsn) {
	v->i->lsnset(v, lsn);
}

static inline uint32_t
sv_timestamp(struct sv *v) {
	return v->i->timestamp(v);
}

static inline char*
sv_pointer(struct sv *v) {
	return v->i->pointer(v);
}

static inline uint32_t
sv_size(struct sv *v) {
	return v->i->size(v);
}

static inline char*
sv_field(struct sv *v, struct runtime *r, int pos, uint32_t *size) {
	return sf_fieldof(r->scheme, pos, v->i->pointer(v), size);
}

static inline uint64_t
sv_hash(struct sv *v, struct runtime *r) {
	return sf_hash(r->scheme, sv_pointer(v));
}


struct svv {
	uint64_t lsn;
	uint32_t size;
	uint32_t timestamp;
	uint8_t  flags;
	uint16_t refs;
	void *log;
} sspacked;

static struct svif sv_vif;

static inline char*
sv_vpointer(struct svv *v) {
	return (char*)(v) + sizeof(struct svv);
}

static inline uint32_t
sv_vsize(struct svv *v) {
	return sizeof(struct svv) + v->size;
}

static inline struct svv*
sv_vbuild(struct runtime *r, struct sfv *fields, uint32_t ts)
{
	int size = sf_writesize(r->scheme, fields);
	struct svv *v = ss_malloc(r->a, sizeof(struct svv) + size);
	if (ssunlikely(v == NULL))
		return NULL;
	v->size      = size;
	v->lsn       = 0;
	v->timestamp = ts;
	v->flags     = 0;
	v->refs      = 1;
	v->log       = NULL;
	char *ptr = sv_vpointer(v);
	sf_write(r->scheme, fields, ptr);
	/* update runtime statistics */
	tt_pthread_mutex_lock(&r->stat->lock);
	r->stat->v_count++;
	r->stat->v_allocated += sizeof(struct svv) + size;
	tt_pthread_mutex_unlock(&r->stat->lock);
	return v;
}

static inline struct svv*
sv_vbuildraw(struct runtime *r, char *src, int size, uint64_t ts)
{
	struct svv *v = ss_malloc(r->a, sizeof(struct svv) + size);
	if (ssunlikely(v == NULL))
		return NULL;
	v->size      = size;
	v->timestamp = ts;
	v->flags     = 0;
	v->refs      = 1;
	v->lsn       = 0;
	v->log       = NULL;
	memcpy(sv_vpointer(v), src, size);
	/* update runtime statistics */
	tt_pthread_mutex_lock(&r->stat->lock);
	r->stat->v_count++;
	r->stat->v_allocated += sizeof(struct svv) + size;
	tt_pthread_mutex_unlock(&r->stat->lock);
	return v;
}

static inline struct svv*
sv_vdup(struct runtime *r, struct sv *src)
{
	struct svv *v = sv_vbuildraw(r, sv_pointer(src), sv_size(src), 0);
	if (ssunlikely(v == NULL))
		return NULL;
	v->flags     = sv_flags(src);
	v->lsn       = sv_lsn(src);
	v->timestamp = sv_timestamp(src);
	return v;
}

static inline void
sv_vref(struct svv *v) {
	v->refs++;
}

static inline int
sv_vunref(struct runtime *r, struct svv *v)
{
	if (sslikely(--v->refs == 0)) {
		uint32_t size = sv_vsize(v);
		/* update runtime statistics */
		tt_pthread_mutex_lock(&r->stat->lock);
		assert(r->stat->v_count > 0);
		assert(r->stat->v_allocated >= size);
		r->stat->v_count--;
		r->stat->v_allocated -= size;
		tt_pthread_mutex_unlock(&r->stat->lock);
		ss_free(r->a, v);
		return 1;
	}
	return 0;
}

struct svref {
	struct svv      *v;
	struct svref    *next;
	uint8_t  flags;
	struct ssrbnode node;
} sspacked;

static struct svif sv_refif;

static inline struct svref*
sv_refnew(struct runtime *r, struct svv *v)
{
	struct svref *ref = ss_malloc(r->aref, sizeof(struct svref));
	if (ssunlikely(ref == NULL))
		return NULL;
	ref->v = v;
	ref->next = NULL;
	ref->flags = 0;
	memset(&ref->node, 0, sizeof(ref->node));
	return ref;
}

static inline void
sv_reffree(struct runtime *r, struct svref *v)
{
	while (v) {
		struct svref *n = v->next;
		sv_vunref(r, v->v);
		ss_free(r->aref, v);
		v = n;
	}
}

static inline struct svref*
sv_refvisible(struct svref *v, uint64_t vlsn) {
	while (v && v->v->lsn > vlsn)
		v = v->next;
	return v;
}

static inline int
sv_refvisible_gte(struct svref *v, uint64_t vlsn) {
	while (v) {
		if (v->v->lsn >= vlsn)
			return 1;
		v = v->next;
	}
	return 0;
}

static struct svif sv_upsertvif;

struct svupsertnode {
	uint64_t lsn;
	uint32_t timestamp;
	uint8_t  flags;
	struct ssbuf    buf;
};

#define SV_UPSERTRESRV 16

struct svupsert {
	struct svupsertnode reserve[SV_UPSERTRESRV];
	struct ssbuf stack;
	struct ssbuf tmp;
	int max;
	int count;
	struct sv result;
};

static inline void
sv_upsertinit(struct svupsert *u)
{
	const int reserve = SV_UPSERTRESRV;
	int i = 0;
	while (i < reserve) {
		ss_bufinit(&u->reserve[i].buf);
		i++;
	}
	memset(&u->result, 0, sizeof(u->result));
	u->max = reserve;
	u->count = 0;
	ss_bufinit_reserve(&u->stack, u->reserve, sizeof(u->reserve));
	ss_bufinit(&u->tmp);
}

static inline void
sv_upsertfree(struct svupsert *u, struct runtime *r)
{
	struct svupsertnode *n = (struct svupsertnode*)u->stack.s;
	int i = 0;
	while (i < u->max) {
		ss_buffree(&n[i].buf, r->a);
		i++;
	}
	ss_buffree(&u->stack, r->a);
	ss_buffree(&u->tmp, r->a);
}

static inline void
sv_upsertreset(struct svupsert *u)
{
	struct svupsertnode *n = (struct svupsertnode*)u->stack.s;
	int i = 0;
	while (i < u->count) {
		ss_bufreset(&n[i].buf);
		i++;
	}
	u->count = 0;
	ss_bufreset(&u->stack);
	ss_bufreset(&u->tmp);
	memset(&u->result, 0, sizeof(u->result));
}

static inline void
sv_upsertgc(struct svupsert *u, struct runtime *r, int wm_stack, int wm_buf)
{
	struct svupsertnode *n = (struct svupsertnode*)u->stack.s;
	if (u->max >= wm_stack) {
		sv_upsertfree(u, r);
		sv_upsertinit(u);
		return;
	}
	ss_bufgc(&u->tmp, r->a, wm_buf);
	int i = 0;
	while (i < u->count) {
		ss_bufgc(&n[i].buf, r->a, wm_buf);
		i++;
	}
	u->count = 0;
	memset(&u->result, 0, sizeof(u->result));
}

static inline int
sv_upsertpush_raw(struct svupsert *u, struct runtime *r,
		  char *pointer, int size,
                  uint8_t flags,
                  uint64_t lsn,
                  uint32_t timestamp)
{
	struct svupsertnode *n;
	int rc;
	if (sslikely(u->max > u->count)) {
		n = (struct svupsertnode*)u->stack.p;
		ss_bufreset(&n->buf);
	} else {
		rc = ss_bufensure(&u->stack, r->a, sizeof(struct svupsertnode));
		if (ssunlikely(rc == -1))
			return -1;
		n = (struct svupsertnode*)u->stack.p;
		ss_bufinit(&n->buf);
		u->max++;
	}
	rc = ss_bufensure(&n->buf, r->a, size);
	if (ssunlikely(rc == -1))
		return -1;
	memcpy(n->buf.p, pointer, size);
	n->flags = flags;
	n->lsn = lsn;
	n->timestamp = timestamp;
	ss_bufadvance(&n->buf, size);
	ss_bufadvance(&u->stack, sizeof(struct svupsertnode));
	u->count++;
	return 0;
}

static inline int
sv_upsertpush(struct svupsert *u, struct runtime *r, struct sv *v)
{
	return sv_upsertpush_raw(u, r, sv_pointer(v),
	                         sv_size(v),
	                         sv_flags(v), sv_lsn(v), sv_timestamp(v));
}

static inline struct svupsertnode*
sv_upsertpop(struct svupsert *u)
{
	if (u->count == 0)
		return NULL;
	int pos = u->count - 1;
	u->count--;
	u->stack.p -= sizeof(struct svupsertnode);
	return ss_bufat(&u->stack, sizeof(struct svupsertnode), pos);
}

static inline int
sv_upsertdo(struct svupsert *u, struct runtime *r, struct svupsertnode *a,
	    struct svupsertnode *b)
{
	assert(r->scheme->fields_count <= 16);
	assert(b->flags & SVUPSERT);

	uint32_t  src_size[16];
	char     *src[16];
	void     *src_ptr;
	uint32_t *src_size_ptr;

	uint32_t  upsert_size[16];
	char     *upsert[16];
	uint32_t  result_size[16];
	char     *result[16];

	int i = 0;
	if (sslikely(a && !(a->flags & SVDELETE)))
	{
		src_ptr = src;
		src_size_ptr = src_size;
		for (; i < r->scheme->fields_count; i++) {
			src[i]    = sf_fieldof(r->scheme, i, a->buf.s, &src_size[i]);
			upsert[i] = sf_fieldof(r->scheme, i, b->buf.s, &upsert_size[i]);
			result[i] = src[i];
			result_size[i] = src_size[i];
		}
	} else {
		src_ptr = NULL;
		src_size_ptr = NULL;
		for (; i < r->scheme->fields_count; i++) {
			upsert[i] = sf_fieldof(r->scheme, i, b->buf.s, &upsert_size[i]);
			result[i] = upsert[i];
			result_size[i] = upsert_size[i];
		}
	}

	/* execute */
	int rc;
	rc = r->fmt_upsert->function(r->scheme->fields_count,
	                             src_ptr,
	                             src_size_ptr,
	                             upsert,
	                             upsert_size,
	                             result,
	                             result_size,
	                             r->fmt_upsert->arg);
	if (ssunlikely(rc == -1))
		return -1;

	/* validate and create new record */
	struct sfv v[16];
	i = 0;
	for ( ; i < r->scheme->fields_count; i++) {
		v[i].pointer = result[i];
		v[i].size = result_size[i];
	}
	int size = sf_writesize(r->scheme, v);
	ss_bufreset(&u->tmp);
	rc = ss_bufensure(&u->tmp, r->a, size);
	if (ssunlikely(rc == -1))
		goto cleanup;
	sf_write(r->scheme, v, u->tmp.s);
	ss_bufadvance(&u->tmp, size);

	/* save result */
	rc = sv_upsertpush_raw(u, r, u->tmp.s, ss_bufused(&u->tmp),
	                       b->flags & ~SVUPSERT,
	                       b->lsn,
	                       b->timestamp);
cleanup:
	/* free fields */
	i = 0;
	for ( ; i < r->scheme->fields_count; i++) {
		if (src_ptr == NULL) {
			if (v[i].pointer != upsert[i])
				free(v[i].pointer);
		} else {
			if (v[i].pointer != src[i])
				free(v[i].pointer);
		}
	}
	return rc;
}

static inline int
sv_upsert(struct svupsert *u, struct runtime *r)
{
	assert(u->count >= 1 );
	struct svupsertnode *f = ss_bufat(&u->stack,
					  sizeof(struct svupsertnode),
					  u->count - 1);
	int rc;
	if (f->flags & SVUPSERT) {
		f = sv_upsertpop(u);
		rc = sv_upsertdo(u, r, NULL, f);
		if (ssunlikely(rc == -1))
			return -1;
	}
	if (u->count == 1)
		goto done;
	while (u->count > 1) {
		struct svupsertnode *f = sv_upsertpop(u);
		struct svupsertnode *s = sv_upsertpop(u);
		assert(f != NULL);
		assert(s != NULL);
		rc = sv_upsertdo(u, r, f, s);
		if (ssunlikely(rc == -1))
			return -1;
	}
done:
	sv_init(&u->result, &sv_upsertvif, u->stack.s, NULL);
	return 0;
}


struct svlogindex {
	uint32_t id;
	uint32_t head, tail;
	uint32_t count;
	void *ptr;
} sspacked;

struct svlogv {
	struct sv v;
	uint32_t id;
	uint32_t next;
} sspacked;

struct svlog {
	int count_write;
	struct svlogindex reserve_i[2];
	struct svlogv reserve_v[1];
	struct ssbuf index;
	struct ssbuf buf;
};

static inline void
sv_loginit(struct svlog *l)
{
	ss_bufinit_reserve(&l->index, l->reserve_i, sizeof(l->reserve_i));
	ss_bufinit_reserve(&l->buf, l->reserve_v, sizeof(l->reserve_v));
	l->count_write = 0;
}

static inline void
sv_logfree(struct svlog *l, struct ssa *a)
{
	ss_buffree(&l->buf, a);
	ss_buffree(&l->index, a);
	l->count_write = 0;
}

static inline void
sv_logreset(struct svlog *l)
{
	ss_bufreset(&l->buf);
	ss_bufreset(&l->index);
	l->count_write = 0;
}

static inline int
sv_logcount(struct svlog *l) {
	return ss_bufused(&l->buf) / sizeof(struct svlogv);
}

static inline int
sv_logcount_write(struct svlog *l) {
	return l->count_write;
}

static inline struct svlogv*
sv_logat(struct svlog *l, int pos) {
	return ss_bufat(&l->buf, sizeof(struct svlogv), pos);
}

static inline int
sv_logadd(struct svlog *l, struct ssa *a, struct svlogv *v, void *ptr)
{
	uint32_t n = sv_logcount(l);
	int rc = ss_bufadd(&l->buf, a, v, sizeof(struct svlogv));
	if (ssunlikely(rc == -1))
		return -1;
	struct svlogindex *i = (struct svlogindex*)l->index.s;
	while ((char*)i < l->index.p) {
		if (sslikely(i->id == v->id)) {
			struct svlogv *tail = sv_logat(l, i->tail);
			tail->next = n;
			i->tail = n;
			i->count++;
			goto done;
		}
		i++;
	}
	rc = ss_bufensure(&l->index, a, sizeof(struct svlogindex));
	if (ssunlikely(rc == -1)) {
		l->buf.p -= sizeof(struct svlogv);
		return -1;
	}
	i = (struct svlogindex*)l->index.p;
	i->id    = v->id;
	i->head  = n;
	i->tail  = n;
	i->ptr   = ptr;
	i->count = 1;
	ss_bufadvance(&l->index, sizeof(struct svlogindex));
done:
	if (! (sv_flags(&v->v) & SVGET))
		l->count_write++;
	return 0;
}

static inline void
sv_logreplace(struct svlog *l, int n, struct svlogv *v)
{
	struct svlogv *ov = sv_logat(l, n);
	if (! (sv_flags(&ov->v) & SVGET))
		l->count_write--;
	if (! (sv_flags(&v->v) & SVGET))
		l->count_write++;
	ss_bufset(&l->buf, sizeof(struct svlogv), n, (char*)v, sizeof(struct svlogv));
}

struct svmergesrc {
	struct ssiter *i, src;
	uint8_t dup;
	void *ptr;
} sspacked;

struct svmerge {
	struct svmergesrc reserve[16];
	struct ssbuf buf;
};

static inline void
sv_mergeinit(struct svmerge *m)
{
	ss_bufinit_reserve(&m->buf, m->reserve, sizeof(m->reserve));
}

static inline int
sv_mergeprepare(struct svmerge *m, struct runtime *r, int count)
{
	int rc = ss_bufensure(&m->buf, r->a, sizeof(struct svmergesrc) * count);
	if (ssunlikely(rc == -1))
		return sr_oom(r->e);
	return 0;
}

static inline struct svmergesrc*
sv_mergenextof(struct svmergesrc *src)
{
	return (struct svmergesrc*)((char*)src + sizeof(struct svmergesrc));
}

static inline void
sv_mergefree(struct svmerge *m, struct ssa *a)
{
	ss_buffree(&m->buf, a);
}

static inline void
sv_mergereset(struct svmerge *m)
{
	m->buf.p = m->buf.s;
}

static inline struct svmergesrc*
sv_mergeadd(struct svmerge *m, struct ssiter *i)
{
	assert(m->buf.p < m->buf.e);
	struct svmergesrc *s = (struct svmergesrc*)m->buf.p;
	s->dup = 0;
	s->i = i;
	s->ptr = NULL;
	if (i == NULL)
		s->i = &s->src;
	ss_bufadvance(&m->buf, sizeof(struct svmergesrc));
	return s;
}

/*
 * Merge serveral sorted streams into one.
 * Track duplicates.
 *
 * Merger does not recognize duplicates from
 * a single stream, assumed that they are tracked
 * by the incoming data sources.
*/

struct svmergeiter {
	enum ssorder order;
	struct svmerge *merge;
	struct svmergesrc *src, *end;
	struct svmergesrc *v;
	struct runtime *r;
} sspacked;

static inline void
sv_mergeiter_dupreset(struct svmergeiter *i, struct svmergesrc *pos)
{
	struct svmergesrc *v = i->src;
	while (v != pos) {
		v->dup = 0;
		v = sv_mergenextof(v);
	}
}

static inline void
sv_mergeiter_gt(struct svmergeiter *i)
{
	if (i->v) {
		i->v->dup = 0;
		ss_iteratornext(i->v->i);
	}
	i->v = NULL;
	struct svmergesrc *min, *src;
	struct sv *minv;
	minv = NULL;
	min  = NULL;
	src  = i->src;
	for (; src < i->end; src = sv_mergenextof(src))
	{
		struct sv *v = ss_iteratorof(src->i);
		if (v == NULL)
			continue;
		if (min == NULL) {
			minv = v;
			min = src;
			continue;
		}
		int rc;
		rc = sf_compare(i->r->scheme,
		                sv_pointer(minv), sv_size(minv),
		                sv_pointer(v), sv_size(v));
		switch (rc) {
		case 0:
			/*
			assert(sv_lsn(v) < sv_lsn(minv));
			*/
			src->dup = 1;
			break;
		case 1:
			sv_mergeiter_dupreset(i, src);
			minv = v;
			min = src;
			break;
		}
	}
	if (ssunlikely(min == NULL))
		return;
	i->v = min;
}

static inline void
sv_mergeiter_lt(struct svmergeiter *i)
{
	if (i->v) {
		i->v->dup = 0;
		ss_iteratornext(i->v->i);
	}
	i->v = NULL;
	struct svmergesrc *max, *src;
	struct sv *maxv;
	maxv = NULL;
	max  = NULL;
	src  = i->src;
	for (; src < i->end; src = sv_mergenextof(src))
	{
		struct sv *v = ss_iteratorof(src->i);
		if (v == NULL)
			continue;
		if (max == NULL) {
			maxv = v;
			max = src;
			continue;
		}
		int rc;
		rc = sf_compare(i->r->scheme,
		                sv_pointer(maxv), sv_size(maxv),
		                sv_pointer(v), sv_size(v));
		switch (rc) {
		case  0:
			/*
			assert(sv_lsn(v) < sv_lsn(maxv));
			*/
			src->dup = 1;
			break;
		case -1:
			sv_mergeiter_dupreset(i, src);
			maxv = v;
			max = src;
			break;
		}
	}
	if (ssunlikely(max == NULL))
		return;
	i->v = max;
}

static inline void
sv_mergeiter_next(struct ssiter *it)
{
	struct svmergeiter *im = (struct svmergeiter*)it->priv;
	switch (im->order) {
	case SS_GT:
	case SS_GTE:
		sv_mergeiter_gt(im);
		break;
	case SS_LT:
	case SS_LTE:
		sv_mergeiter_lt(im);
		break;
	default: assert(0);
	}
}

static inline int
sv_mergeiter_open(struct ssiter *i, struct runtime *r, struct svmerge *m, enum ssorder o)
{
	struct svmergeiter *im = (struct svmergeiter*)i->priv;
	im->merge = m;
	im->r     = r;
	im->order = o;
	im->src   = (struct svmergesrc*)(im->merge->buf.s);
	im->end   = (struct svmergesrc*)(im->merge->buf.p);
	im->v     = NULL;
	sv_mergeiter_next(i);
	return 0;
}

static inline void
sv_mergeiter_close(struct ssiter *i ssunused)
{ }

static inline int
sv_mergeiter_has(struct ssiter *i)
{
	struct svmergeiter *im = (struct svmergeiter*)i->priv;
	return im->v != NULL;
}

static inline void *
sv_mergeiter_of(struct ssiter *i)
{
	struct svmergeiter *im = (struct svmergeiter*)i->priv;
	if (ssunlikely(im->v == NULL))
		return NULL;
	return ss_iteratorof(im->v->i);
}

static inline uint32_t
sv_mergeisdup(struct ssiter *i)
{
	struct svmergeiter *im = (struct svmergeiter*)i->priv;
	assert(im->v != NULL);
	if (im->v->dup)
		return SVDUP;
	return 0;
}

static struct ssiterif sv_mergeiter;

struct svreaditer {
	struct ssiter *merge;
	uint64_t vlsn;
	int next;
	int nextdup;
	int save_delete;
	struct svupsert *u;
	struct runtime *r;
	struct sv *v;
} sspacked;

static inline int
sv_readiter_upsert(struct svreaditer *i)
{
	sv_upsertreset(i->u);
	/* upsert begin */
	struct sv *v = ss_iterof(sv_mergeiter, i->merge);
	assert(v != NULL);
	assert(sv_flags(v) & SVUPSERT);
	int rc = sv_upsertpush(i->u, i->r, v);
	if (ssunlikely(rc == -1))
		return -1;
	ss_iternext(sv_mergeiter, i->merge);
	/* iterate over upsert statements */
	int skip = 0;
	for (; ss_iterhas(sv_mergeiter, i->merge); ss_iternext(sv_mergeiter, i->merge))
	{
		v = ss_iterof(sv_mergeiter, i->merge);
		int dup = sv_is(v, SVDUP) || sv_mergeisdup(i->merge);
		if (! dup)
			break;
		if (skip)
			continue;
		int rc = sv_upsertpush(i->u, i->r, v);
		if (ssunlikely(rc == -1))
			return -1;
		if (! (sv_flags(v) & SVUPSERT))
			skip = 1;
	}
	/* upsert */
	rc = sv_upsert(i->u, i->r);
	if (ssunlikely(rc == -1))
		return -1;
	return 0;
}

static inline void
sv_readiter_next(struct ssiter *i)
{
	struct svreaditer *im = (struct svreaditer*)i->priv;
	if (im->next)
		ss_iternext(sv_mergeiter, im->merge);
	im->next = 0;
	im->v = NULL;
	for (; ss_iterhas(sv_mergeiter, im->merge); ss_iternext(sv_mergeiter, im->merge))
	{
		struct sv *v = ss_iterof(sv_mergeiter, im->merge);
		int dup = sv_is(v, SVDUP) || sv_mergeisdup(im->merge);
		if (im->nextdup) {
			if (dup)
				continue;
			else
				im->nextdup = 0;
		}
		/* skip version out of visible range */
		if (sv_lsn(v) > im->vlsn) {
			continue;
		}
		im->nextdup = 1;
		if (ssunlikely(!im->save_delete && sv_is(v, SVDELETE)))
			continue;
		if (ssunlikely(sv_is(v, SVUPSERT))) {
			int rc = sv_readiter_upsert(im);
			if (ssunlikely(rc == -1))
				return;
			im->v = &im->u->result;
			im->next = 0;
		} else {
			im->v = v;
			im->next = 1;
		}
		break;
	}
}

static inline void
sv_readiter_forward(struct ssiter *i)
{
	struct svreaditer *im = (struct svreaditer*)i->priv;
	if (im->next)
		ss_iternext(sv_mergeiter, im->merge);
	im->next = 0;
	im->v = NULL;
	for (; ss_iterhas(sv_mergeiter, im->merge); ss_iternext(sv_mergeiter, im->merge))
	{
		struct sv *v = ss_iterof(sv_mergeiter, im->merge);
		int dup = sv_is(v, SVDUP) || sv_mergeisdup(im->merge);
		if (dup)
			continue;
		im->next = 0;
		im->v = v;
		break;
	}
}

static inline int
sv_readiter_open(struct ssiter *i, struct runtime *r, struct ssiter *iterator, struct svupsert *u,
                 uint64_t vlsn, int save_delete)
{
	struct svreaditer *im = (struct svreaditer*)i->priv;
	im->r     = r;
	im->u     = u;
	im->merge = iterator;
	im->vlsn  = vlsn;
	assert(im->merge->vif == &sv_mergeiter);
	im->v = NULL;
	im->next = 0;
	im->nextdup = 0;
	im->save_delete = save_delete;
	/* iteration can start from duplicate */
	sv_readiter_next(i);
	return 0;
}

static inline void
sv_readiter_close(struct ssiter *i ssunused)
{ }

static inline int
sv_readiter_has(struct ssiter *i)
{
	struct svreaditer *im = (struct svreaditer*)i->priv;
	return im->v != NULL;
}

static inline void*
sv_readiter_of(struct ssiter *i)
{
	struct svreaditer *im = (struct svreaditer*)i->priv;
	if (ssunlikely(im->v == NULL))
		return NULL;
	return im->v;
}

static struct ssiterif sv_readiter;

struct svwriteiter {
	uint64_t  vlsn;
	uint64_t  vlsn_lru;
	uint64_t  limit;
	uint64_t  size;
	uint32_t  sizev;
	uint32_t  expire;
	uint32_t  now;
	int       save_delete;
	int       save_upsert;
	int       next;
	int       upsert;
	uint64_t  prevlsn;
	int       vdup;
	struct sv       *v;
	struct svupsert *u;
	struct ssiter   *merge;
	struct runtime       *r;
} sspacked;

static inline int
sv_writeiter_upsert(struct svwriteiter *i)
{
	/* apply upsert only on statements which are the latest or
	 * ready to be garbage-collected */
	sv_upsertreset(i->u);

	/* upsert begin */
	struct sv *v = ss_iterof(sv_mergeiter, i->merge);
	assert(v != NULL);
	assert(sv_flags(v) & SVUPSERT);
	assert(sv_lsn(v) <= i->vlsn);
	int rc = sv_upsertpush(i->u, i->r, v);
	if (ssunlikely(rc == -1))
		return -1;
	ss_iternext(sv_mergeiter, i->merge);

	/* iterate over upsert statements */
	int last_non_upd = 0;
	for (; ss_iterhas(sv_mergeiter, i->merge); ss_iternext(sv_mergeiter, i->merge))
	{
		v = ss_iterof(sv_mergeiter, i->merge);
		int flags = sv_flags(v);
		int dup = sv_isflags(flags, SVDUP) || sv_mergeisdup(i->merge);
		if (! dup)
			break;
		/* stop forming upserts on a second non-upsert stmt,
		 * but continue to iterate stream */
		if (last_non_upd)
			continue;
		last_non_upd = ! sv_isflags(flags, SVUPSERT);
		int rc = sv_upsertpush(i->u, i->r, v);
		if (ssunlikely(rc == -1))
			return -1;
	}

	/* upsert */
	rc = sv_upsert(i->u, i->r);
	if (ssunlikely(rc == -1))
		return -1;
	return 0;
}

static inline void
sv_writeiter_next(struct ssiter *i)
{
	struct svwriteiter *im = (struct svwriteiter*)i->priv;
	if (im->next)
		ss_iternext(sv_mergeiter, im->merge);
	im->next = 0;
	im->v = NULL;
	im->vdup = 0;

	for (; ss_iterhas(sv_mergeiter, im->merge); ss_iternext(sv_mergeiter, im->merge))
	{
		struct sv *v = ss_iterof(sv_mergeiter, im->merge);
		if (im->expire > 0) {
			uint32_t timestamp = sv_timestamp(v);
			if ((im->now - timestamp) >= im->expire)
				 continue;
		}
		uint64_t lsn = sv_lsn(v);
		if (lsn < im->vlsn_lru)
			continue;
		int flags = sv_flags(v);
		int dup = sv_isflags(flags, SVDUP) || sv_mergeisdup(im->merge);
		if (im->size >= im->limit) {
			if (! dup)
				break;
		}

		if (ssunlikely(dup)) {
			/* keep atleast one visible version for <= vlsn */
			if (im->prevlsn <= im->vlsn) {
				if (im->upsert) {
					im->upsert = sv_isflags(flags, SVUPSERT);
				} else {
					continue;
				}
			}
		} else {
			im->upsert = 0;
			/* delete (stray or on branch) */
			if (! im->save_delete) {
				int del = sv_isflags(flags, SVDELETE);
				if (ssunlikely(del && (lsn <= im->vlsn))) {
					im->prevlsn = lsn;
					continue;
				}
			}
			im->size += im->sizev + sv_size(v);
			/* upsert (track first statement start) */
			if (sv_isflags(flags, SVUPSERT))
				im->upsert = 1;
		}

		/* upsert */
		if (sv_isflags(flags, SVUPSERT)) {
			if (! im->save_upsert) {
				if (lsn <= im->vlsn) {
					int rc;
					rc = sv_writeiter_upsert(im);
					if (ssunlikely(rc == -1))
						return;
					im->upsert = 0;
					im->prevlsn = lsn;
					im->v = &im->u->result;
					im->vdup = dup;
					im->next = 0;
					break;
				}
			}
		}

		im->prevlsn = lsn;
		im->v = v;
		im->vdup = dup;
		im->next = 1;
		break;
	}
}

static inline int
sv_writeiter_open(struct ssiter *i, struct runtime *r, struct ssiter *merge, struct svupsert *u,
                  uint64_t limit,
                  uint32_t sizev,
                  uint32_t expire,
                  uint32_t timestamp,
                  uint64_t vlsn,
                  uint64_t vlsn_lru,
                  int save_delete,
                  int save_upsert)
{
	struct svwriteiter *im = (struct svwriteiter*)i->priv;
	im->u           = u;
	im->r           = r;
	im->merge       = merge;
	im->limit       = limit;
	im->size        = 0;
	im->sizev       = sizev;
	im->expire      = expire;
	im->now         = timestamp;
	im->vlsn        = vlsn;
	im->vlsn_lru    = vlsn_lru;
	im->save_delete = save_delete;
	im->save_upsert = save_upsert;
	assert(im->merge->vif == &sv_mergeiter);
	im->next  = 0;
	im->prevlsn  = 0;
	im->v = NULL;
	im->vdup = 0;
	im->upsert = 0;
	sv_writeiter_next(i);
	return 0;
}

static inline void
sv_writeiter_close(struct ssiter *i ssunused)
{ }

static inline int
sv_writeiter_has(struct ssiter *i)
{
	struct svwriteiter *im = (struct svwriteiter*)i->priv;
	return im->v != NULL;
}

static inline void*
sv_writeiter_of(struct ssiter *i)
{
	struct svwriteiter *im = (struct svwriteiter*)i->priv;
	if (ssunlikely(im->v == NULL))
		return NULL;
	return im->v;
}

static inline int
sv_writeiter_resume(struct ssiter *i)
{
	struct svwriteiter *im = (struct svwriteiter*)i->priv;
	im->v       = ss_iterof(sv_mergeiter, im->merge);
	if (ssunlikely(im->v == NULL))
		return 0;
	im->vdup    = sv_is(im->v, SVDUP) || sv_mergeisdup(im->merge);
	im->prevlsn = sv_lsn(im->v);
	im->next    = 1;
	im->upsert  = 0;
	im->size    = im->sizev + sv_size(im->v);
	return 1;
}

static inline int
sv_writeiter_is_duplicate(struct ssiter *i)
{
	struct svwriteiter *im = (struct svwriteiter*)i->priv;
	assert(im->v != NULL);
	return im->vdup;
}

static struct ssiterif sv_writeiter;

struct svindexpos {
	struct ssrbnode *node;
	int rc;
};

struct svindex {
	struct ssrb i;
	uint32_t count;
	uint32_t used;
	uint64_t lsnmin;
} sspacked;

ss_rbget(sv_indexmatch,
         sf_compare(scheme, sv_vpointer((sscast(n, struct svref, node))->v),
                    (sscast(n, struct svref, node))->v->size,
                    key, keysize))

static int
sv_indexinit(struct svindex*);

static int
sv_indexfree(struct svindex*, struct runtime*);

static int
sv_indexupdate(struct svindex*, struct svindexpos*, struct svref*);

static struct svref *
sv_indexget(struct svindex*, struct runtime*, struct svindexpos*, struct svref*);

static inline int
sv_indexset(struct svindex *i, struct runtime *r, struct svref  *v)
{
	struct svindexpos pos;
	sv_indexget(i, r, &pos, v);
	sv_indexupdate(i, &pos, v);
	return 0;
}

static inline uint32_t
sv_indexused(struct svindex *i) {
	return i->count * sizeof(struct svv) + i->used;
}

struct svindexiter {
	struct svindex *index;
	struct ssrbnode *v;
	struct svref *vcur;
	struct sv current;
	enum ssorder order;
} sspacked;

static inline int
sv_indexiter_open(struct ssiter *i, struct runtime *r, struct svindex *index, enum ssorder o, void *key, int keysize)
{
	struct svindexiter *ii = (struct svindexiter*)i->priv;
	ii->index   = index;
	ii->order   = o;
	ii->v       = NULL;
	ii->vcur    = NULL;
	sv_init(&ii->current, &sv_refif, NULL, NULL);
	int rc;
	int eq = 0;
	switch (ii->order) {
	case SS_LT:
	case SS_LTE:
		if (ssunlikely(key == NULL)) {
			ii->v = ss_rbmax(&ii->index->i);
			break;
		}
		rc = sv_indexmatch(&ii->index->i, r->scheme, key, keysize, &ii->v);
		if (ii->v == NULL)
			break;
		switch (rc) {
		case 0:
			eq = 1;
			if (ii->order == SS_LT)
				ii->v = ss_rbprev(&ii->index->i, ii->v);
			break;
		case 1:
			ii->v = ss_rbprev(&ii->index->i, ii->v);
			break;
		}
		break;
	case SS_GT:
	case SS_GTE:
		if (ssunlikely(key == NULL)) {
			ii->v = ss_rbmin(&ii->index->i);
			break;
		}
		rc = sv_indexmatch(&ii->index->i, r->scheme, key, keysize, &ii->v);
		if (ii->v == NULL)
			break;
		switch (rc) {
		case  0:
			eq = 1;
			if (ii->order == SS_GT)
				ii->v = ss_rbnext(&ii->index->i, ii->v);
			break;
		case -1:
			ii->v = ss_rbnext(&ii->index->i, ii->v);
			break;
		}
		break;
	default: assert(0);
	}
	ii->vcur = NULL;
	if (ii->v) {
		ii->vcur = sscast(ii->v, struct svref, node);
		ii->current.v = ii->vcur;
	}
	return eq;
}

static inline void
sv_indexiter_close(struct ssiter *i ssunused)
{}

static inline int
sv_indexiter_has(struct ssiter *i)
{
	struct svindexiter *ii = (struct svindexiter*)i->priv;
	return ii->v != NULL;
}

static inline void*
sv_indexiter_of(struct ssiter *i)
{
	struct svindexiter *ii = (struct svindexiter*)i->priv;
	if (ssunlikely(ii->v == NULL))
		return NULL;
	return &ii->current;
}

static inline void
sv_indexiter_next(struct ssiter *i)
{
	struct svindexiter *ii = (struct svindexiter*)i->priv;
	if (ssunlikely(ii->v == NULL))
		return;
	assert(ii->vcur != NULL);
	struct svref *v = ii->vcur->next;
	if (v) {
		ii->vcur = v;
		ii->current.v = ii->vcur;
		return;
	}
	switch (ii->order) {
	case SS_LT:
	case SS_LTE:
		ii->v = ss_rbprev(&ii->index->i, ii->v);
		break;
	case SS_GT:
	case SS_GTE:
		ii->v = ss_rbnext(&ii->index->i, ii->v);
		break;
	default: assert(0);
	}
	if (sslikely(ii->v)) {
		ii->vcur = sscast(ii->v, struct svref, node);
		ii->current.v = ii->vcur;
	} else {
		ii->vcur = NULL;
	}
}

static struct ssiterif sv_indexiter;

ss_rbtruncate(sv_indextruncate,
              sv_reffree((struct runtime*)arg, sscast(n, struct svref, node)))

static int sv_indexinit(struct svindex *i)
{
	i->lsnmin = UINT64_MAX;
	i->count  = 0;
	i->used   = 0;
	ss_rbinit(&i->i);
	return 0;
}

static int sv_indexfree(struct svindex *i, struct runtime *r)
{
	if (i->i.root)
		sv_indextruncate(i->i.root, r);
	ss_rbinit(&i->i);
	return 0;
}

static inline struct svref*
sv_vset(struct svref *head, struct svref *v)
{
	assert(head->v->lsn != v->v->lsn);
	struct svv *vv = v->v;
	/* default */
	if (sslikely(head->v->lsn < vv->lsn)) {
		v->next = head;
		head->flags |= SVDUP;
		return v;
	}
	/* redistribution (starting from highest lsn) */
	struct svref *prev = head;
	struct svref *c = head->next;
	while (c) {
		assert(c->v->lsn != vv->lsn);
		if (c->v->lsn < vv->lsn)
			break;
		prev = c;
		c = c->next;
	}
	prev->next = v;
	v->next = c;
	v->flags |= SVDUP;
	return head;
}

static struct svref*
sv_indexget(struct svindex *i, struct runtime *r, struct svindexpos *p, struct svref *v)
{
	p->rc = sv_indexmatch(&i->i, r->scheme, sv_vpointer(v->v), v->v->size, &p->node);
	if (p->rc == 0 && p->node)
		return sscast(p->node, struct svref, node);
	return NULL;
}

static int sv_indexupdate(struct svindex *i, struct svindexpos *p, struct svref *v)
{
	if (p->rc == 0 && p->node) {
		struct svref *head = sscast(p->node, struct svref, node);
		struct svref *update = sv_vset(head, v);
		if (head != update)
			ss_rbreplace(&i->i, p->node, &update->node);
	} else {
		ss_rbset(&i->i, p->node, p->rc, &v->node);
	}
	if (v->v->lsn < i->lsnmin)
		i->lsnmin = v->v->lsn;
	i->count++;
	i->used += v->v->size;
	return 0;
}

static struct ssiterif sv_indexiter =
{
	.close   = sv_indexiter_close,
	.has     = sv_indexiter_has,
	.of      = sv_indexiter_of,
	.next    = sv_indexiter_next
};

static struct ssiterif sv_mergeiter =
{
	.close = sv_mergeiter_close,
	.has   = sv_mergeiter_has,
	.of    = sv_mergeiter_of,
	.next  = sv_mergeiter_next
};

static struct ssiterif sv_readiter =
{
	.close   = sv_readiter_close,
	.has     = sv_readiter_has,
	.of      = sv_readiter_of,
	.next    = sv_readiter_next
};

static uint8_t
sv_refifflags(struct sv *v) {
	struct svref *ref = (struct svref*)v->v;
	return ((struct svv*)ref->v)->flags | ref->flags;
}

static uint64_t
sv_refiflsn(struct sv *v) {
	return ((struct svv*)((struct svref*)v->v)->v)->lsn;
}

static void
sv_refiflsnset(struct sv *v, uint64_t lsn) {
	((struct svv*)((struct svref*)v->v)->v)->lsn = lsn;
}

static uint32_t
sv_refiftimestamp(struct sv *v) {
	return ((struct svv*)((struct svref*)v->v)->v)->timestamp;
}

static char*
sv_refifpointer(struct sv *v) {
	return sv_vpointer(((struct svv*)((struct svref*)v->v)->v));
}

static uint32_t
sv_refifsize(struct sv *v) {
	return ((struct svv*)((struct svref*)v->v)->v)->size;
}

static struct svif sv_refif =
{
	.flags     = sv_refifflags,
	.lsn       = sv_refiflsn,
	.lsnset    = sv_refiflsnset,
	.timestamp = sv_refiftimestamp,
	.pointer   = sv_refifpointer,
	.size      = sv_refifsize
};

static uint8_t
sv_upsertvifflags(struct sv *v) {
	struct svupsertnode *n = v->v;
	return n->flags;
}

static uint64_t
sv_upsertviflsn(struct sv *v) {
	struct svupsertnode *n = v->v;
	return n->lsn;
}

static void
sv_upsertviflsnset(struct sv *v ssunused, uint64_t lsn ssunused) {
	assert(0);
}

static uint32_t
sv_upsertviftimestamp(struct sv *v) {
	struct svupsertnode *n = v->v;
	return n->timestamp;
}

static char*
sv_upsertvifpointer(struct sv *v) {
	struct svupsertnode *n = v->v;
	return n->buf.s;
}

static uint32_t
sv_upsertvifsize(struct sv *v) {
	struct svupsertnode *n = v->v;
	return ss_bufused(&n->buf);
}

static struct svif sv_upsertvif =
{
	.flags     = sv_upsertvifflags,
	.lsn       = sv_upsertviflsn,
	.lsnset    = sv_upsertviflsnset,
	.timestamp = sv_upsertviftimestamp,
	.pointer   = sv_upsertvifpointer,
	.size      = sv_upsertvifsize
};

static uint8_t
sv_vifflags(struct sv *v) {
	return ((struct svv*)v->v)->flags;
}

static uint64_t
sv_viflsn(struct sv *v) {
	return ((struct svv*)v->v)->lsn;
}

static void
sv_viflsnset(struct sv *v, uint64_t lsn) {
	((struct svv*)v->v)->lsn = lsn;
}

static uint32_t
sv_viftimestamp(struct sv *v) {
	return ((struct svv*)v->v)->timestamp;
}

static char*
sv_vifpointer(struct sv *v) {
	return sv_vpointer(((struct svv*)v->v));
}

static uint32_t
sv_vifsize(struct sv *v) {
	return ((struct svv*)v->v)->size;
}

static struct svif sv_vif =
{
	.flags     = sv_vifflags,
	.lsn       = sv_viflsn,
	.lsnset    = sv_viflsnset,
	.timestamp = sv_viftimestamp,
	.pointer   = sv_vifpointer,
	.size      = sv_vifsize
};

static struct ssiterif sv_writeiter =
{
	.close   = sv_writeiter_close,
	.has     = sv_writeiter_has,
	.of      = sv_writeiter_of,
	.next    = sv_writeiter_next
};

struct sxv {
	uint64_t id;
	uint32_t lo;
	uint64_t csn;
	void *index;
	struct svv *v;
	struct sxv *next;
	struct sxv *prev;
	struct sxv *gc;
	struct ssrbnode node;
} sspacked;

struct sxvpool {
	struct sxv *head;
	int n;
	struct runtime *r;
};

static inline void
sx_vpool_init(struct sxvpool *p, struct runtime *r)
{
	p->head = NULL;
	p->n = 0;
	p->r = r;
}

static inline void
sx_vpool_free(struct sxvpool *p)
{
	struct sxv *n, *c = p->head;
	while (c) {
		n = c->next;
		ss_free(p->r->a, c);
		c = n;
	}
}

static inline struct sxv*
sx_vpool_pop(struct sxvpool *p)
{
	if (ssunlikely(p->n == 0))
		return NULL;
	struct sxv *v = p->head;
	p->head = v->next;
	p->n--;
	return v;
}

static inline void
sx_vpool_push(struct sxvpool *p, struct sxv *v)
{
	v->v    = NULL;
	v->next = NULL;
	v->prev = NULL;
	v->next = p->head;
	p->head = v;
	p->n++;
}

static inline struct sxv*
sx_valloc(struct sxvpool *p, struct svv *ref)
{
	struct sxv *v = sx_vpool_pop(p);
	if (ssunlikely(v == NULL)) {
		v = ss_malloc(p->r->a, sizeof(struct sxv));
		if (ssunlikely(v == NULL))
			return NULL;
	}
	v->index = NULL;
	v->id    = 0;
	v->lo    = 0;
	v->csn   = 0;
	v->v     = ref;
	v->next  = NULL;
	v->prev  = NULL;
	v->gc    = NULL;
	memset(&v->node, 0, sizeof(v->node));
	return v;
}

static inline void
sx_vfree(struct sxvpool *p, struct sxv *v)
{
	sv_vunref(p->r, v->v);
	sx_vpool_push(p, v);
}

static inline void
sx_vfreeall(struct sxvpool *p, struct sxv *v)
{
	while (v) {
		struct sxv *next = v->next;
		sx_vfree(p, v);
		v = next;
	}
}

static inline struct sxv*
sx_vmatch(struct sxv *head, uint64_t id)
{
	struct sxv *c = head;
	while (c) {
		if (c->id == id)
			break;
		c = c->next;
	}
	return c;
}

static inline void
sx_vreplace(struct sxv *v, struct sxv *n)
{
	if (v->prev)
		v->prev->next = n;
	if (v->next)
		v->next->prev = n;
	n->next = v->next;
	n->prev = v->prev;
}

static inline void
sx_vlink(struct sxv *head, struct sxv *v)
{
	struct sxv *c = head;
	while (c->next)
		c = c->next;
	c->next = v;
	v->prev = c;
	v->next = NULL;
}

static inline void
sx_vunlink(struct sxv *v)
{
	if (v->prev)
		v->prev->next = v->next;
	if (v->next)
		v->next->prev = v->prev;
	v->prev = NULL;
	v->next = NULL;
}

static inline void
sx_vcommit(struct sxv *v, uint32_t csn)
{
	v->id  = UINT64_MAX;
	v->lo  = UINT32_MAX;
	v->csn = csn;
}

static inline int
sx_vcommitted(struct sxv *v)
{
	return v->id == UINT64_MAX && v->lo == UINT32_MAX;
}

static inline void
sx_vabort(struct sxv *v)
{
	v->v->flags |= SVCONFLICT;
}

static inline void
sx_vabort_all(struct sxv *v)
{
	while (v) {
		sx_vabort(v);
		v = v->next;
	}
}

static inline int
sx_vaborted(struct sxv *v)
{
	return v->v->flags & SVCONFLICT;
}

static struct svif sx_vif;

enum sxstate {
	SXUNDEF,
	SXREADY,
	SXCOMMIT,
	SXPREPARE,
	SXROLLBACK,
	SXLOCK
};

enum sxtype {
	SXRO,
	SXRW
};

struct sxindex {
	struct ssrb      i;
	uint32_t  dsn;
	struct so       *object;
	void     *ptr;
	struct runtime       *r;
	struct rlist    link;
};

struct sx;

typedef int (*sxpreparef)(struct sx*, struct sv*, struct so*, void*);

struct sx {
	enum sxtype     type;
	enum sxstate    state;
	uint64_t   id;
	uint64_t   vlsn;
	uint64_t   csn;
	int        log_read;
	struct svlog     *log;
	struct rlist     deadlock;
	struct ssrbnode   node;
	struct sxmanager *manager;
};

struct sxmanager {
	pthread_mutex_t  lock;
	struct rlist      indexes;
	struct ssrb        i;
	uint32_t    count_rd;
	uint32_t    count_rw;
	uint32_t    count_gc;
	uint64_t    csn;
	struct sxv        *gc;
	struct sxvpool     pool;
	struct runtime         *r;
};

static int sx_managerinit(struct sxmanager*, struct runtime*);
static int sx_managerfree(struct sxmanager*);
static int sx_indexinit(struct sxindex*, struct sxmanager*, struct runtime*, struct so*, void*);
static int sx_indexset(struct sxindex*, uint32_t);
static int sx_indexfree(struct sxindex*, struct sxmanager*);
static struct sx *sx_find(struct sxmanager*, uint64_t);
static void sx_init(struct sxmanager*, struct sx*, struct svlog*);
static enum sxstate sx_begin(struct sxmanager*, struct sx*, enum sxtype, struct svlog*, uint64_t);
static void sx_gc(struct sx*);
static enum sxstate sx_prepare(struct sx*, sxpreparef, void*);
static enum sxstate sx_commit(struct sx*);
static enum sxstate sx_rollback(struct sx*);
static int sx_set(struct sx*, struct sxindex*, struct svv*);
static int sx_get(struct sx*, struct sxindex*, struct sv*, struct sv*);
static uint64_t sx_min(struct sxmanager*);
static uint64_t sx_max(struct sxmanager*);
static uint64_t sx_vlsn(struct sxmanager*);
static enum sxstate sx_set_autocommit(struct sxmanager*, struct sxindex*, struct sx*, struct svlog*, struct svv*);
static enum sxstate sx_get_autocommit(struct sxmanager*, struct sxindex*);

static int sx_deadlock(struct sx*);

static inline int
sx_count(struct sxmanager *m) {
	return m->count_rd + m->count_rw;
}

static int sx_managerinit(struct sxmanager *m, struct runtime *r)
{
	ss_rbinit(&m->i);
	m->count_rd = 0;
	m->count_rw = 0;
	m->count_gc = 0;
	m->csn = 0;
	m->gc  = NULL;
	tt_pthread_mutex_init(&m->lock, NULL);
	rlist_create(&m->indexes);
	sx_vpool_init(&m->pool, r);
	m->r = r;
	return 0;
}

static int sx_managerfree(struct sxmanager *m)
{
	assert(sx_count(m) == 0);
	sx_vpool_free(&m->pool);
	tt_pthread_mutex_destroy(&m->lock);
	return 0;
}

static int sx_indexinit(struct sxindex *i, struct sxmanager *m, struct runtime *r, struct so *object, void *ptr)
{
	ss_rbinit(&i->i);
	rlist_create(&i->link);
	i->dsn = 0;
	i->object = object;
	i->ptr = ptr;
	i->r = r;
	rlist_add(&m->indexes, &i->link);
	return 0;
}

static int sx_indexset(struct sxindex *i, uint32_t dsn)
{
	i->dsn = dsn;
	return 0;
}

ss_rbtruncate(sx_truncate, sx_vfreeall(arg, sscast(n, struct sxv, node)))

static inline void
sx_indextruncate(struct sxindex *i, struct sxmanager *m)
{
	if (i->i.root == NULL)
		return;
	sx_truncate(i->i.root, &m->pool);
	ss_rbinit(&i->i);
}

static int sx_indexfree(struct sxindex *i, struct sxmanager *m)
{
	sx_indextruncate(i, m);
	rlist_del(&i->link);
	return 0;
}

static uint64_t sx_min(struct sxmanager *m)
{
	tt_pthread_mutex_lock(&m->lock);
	uint64_t id = 0;
	if (sx_count(m) > 0) {
		struct ssrbnode *node = ss_rbmin(&m->i);
		struct sx *min = sscast(node, struct sx, node);
		id = min->id;
	}
	tt_pthread_mutex_unlock(&m->lock);
	return id;
}

static uint64_t sx_max(struct sxmanager *m)
{
	tt_pthread_mutex_lock(&m->lock);
	uint64_t id = 0;
	if (sx_count(m) > 0) {
		struct ssrbnode *node = ss_rbmax(&m->i);
		struct sx *max = sscast(node, struct sx, node);
		id = max->id;
	}
	tt_pthread_mutex_unlock(&m->lock);
	return id;
}

static uint64_t sx_vlsn(struct sxmanager *m)
{
	tt_pthread_mutex_lock(&m->lock);
	uint64_t vlsn;
	if (sx_count(m) > 0) {
		struct ssrbnode *node = ss_rbmin(&m->i);
		struct sx *min = sscast(node, struct sx, node);
		vlsn = min->vlsn;
	} else {
		vlsn = sr_seq(m->r->seq, SR_LSN);
	}
	tt_pthread_mutex_unlock(&m->lock);
	return vlsn;
}

ss_rbget(sx_matchtx, ss_cmp((sscast(n, struct sx, node))->id, load_u64(key)))

static struct sx *sx_find(struct sxmanager *m, uint64_t id)
{
	struct ssrbnode *n = NULL;
	int rc = sx_matchtx(&m->i, NULL, (char*)&id, sizeof(id), &n);
	if (rc == 0 && n)
		return  sscast(n, struct sx, node);
	return NULL;
}

static void sx_init(struct sxmanager *m, struct sx *x, struct svlog *log)
{
	x->manager = m;
	x->log = log;
	rlist_create(&x->deadlock);
}

static inline enum sxstate
sx_promote(struct sx *x, enum sxstate state)
{
	x->state = state;
	return state;
}

static enum sxstate
sx_begin(struct sxmanager *m, struct sx *x, enum sxtype type,
	 struct svlog *log, uint64_t vlsn)
{
	sx_promote(x, SXREADY);
	x->type = type;
	x->log_read = -1;
	sr_seqlock(m->r->seq);
	x->csn = m->csn;
	x->id = sr_seqdo(m->r->seq, SR_TSNNEXT);
	if (sslikely(vlsn == UINT64_MAX))
		x->vlsn = sr_seqdo(m->r->seq, SR_LSN);
	else
		x->vlsn = vlsn;
	sr_sequnlock(m->r->seq);
	sx_init(m, x, log);
	tt_pthread_mutex_lock(&m->lock);
	struct ssrbnode *n = NULL;
	int rc = sx_matchtx(&m->i, NULL, (char*)&x->id, sizeof(x->id), &n);
	if (rc == 0 && n) {
		assert(0);
	} else {
		ss_rbset(&m->i, n, rc, &x->node);
	}
	if (type == SXRO)
		m->count_rd++;
	else
		m->count_rw++;
	tt_pthread_mutex_unlock(&m->lock);
	return SXREADY;
}

static inline void
sx_untrack(struct sxv *v)
{
	if (v->prev == NULL) {
		struct sxindex *i = v->index;
		if (v->next == NULL)
			ss_rbremove(&i->i, &v->node);
		else
			ss_rbreplace(&i->i, &v->node, &v->next->node);
	}
	sx_vunlink(v);
}

static inline uint64_t
sx_csn(struct sxmanager *m)
{
	uint64_t csn = UINT64_MAX;
	if (m->count_rw == 0)
		return csn;
	struct ssrbnode *p = ss_rbmin(&m->i);
	struct sx *min = NULL;
	while (p) {
		min = sscast(p, struct sx, node);
		if (min->type == SXRO) {
			p = ss_rbnext(&m->i, p);
			continue;
		}
		break;
	}
	assert(min != NULL);
	return min->csn;
}

static inline void
sx_garbage_collect(struct sxmanager *m)
{
	uint64_t min_csn = sx_csn(m);
	struct sxv *gc = NULL;
	uint32_t count = 0;
	struct sxv *next;
	struct sxv *v = m->gc;
	for (; v; v = next)
	{
		next = v->gc;
		assert(v->v->flags & SVGET);
		assert(sx_vcommitted(v));
		if (v->csn > min_csn) {
			v->gc = gc;
			gc = v;
			count++;
			continue;
		}
		sx_untrack(v);
		sx_vfree(&m->pool, v);
	}
	m->count_gc = count;
	m->gc = gc;
}

static void sx_gc(struct sx *x)
{
	struct sxmanager *m = x->manager;
	sx_promote(x, SXUNDEF);
	x->log = NULL;
	if (m->count_gc == 0)
		return;
	sx_garbage_collect(m);
}

static inline void
sx_end(struct sx *x)
{
	struct sxmanager *m = x->manager;
	tt_pthread_mutex_lock(&m->lock);
	ss_rbremove(&m->i, &x->node);
	if (x->type == SXRO)
		m->count_rd--;
	else
		m->count_rw--;
	tt_pthread_mutex_unlock(&m->lock);
}

static inline void
sx_rollback_svp(struct sx *x, struct ssiter *i, int free)
{
	struct sxmanager *m = x->manager;
	int gc = 0;
	for (; ss_iterhas(ss_bufiter, i); ss_iternext(ss_bufiter, i))
	{
		struct svlogv *lv = ss_iterof(ss_bufiter, i);
		struct sxv *v = lv->v.v;
		/* remove from index and replace head with
		 * a first waiter */
		sx_untrack(v);
		/* translate log version from struct sxv to struct svv */
		sv_init(&lv->v, &sv_vif, v->v, NULL);
		if (free) {
			int size = sv_vsize((struct svv*)v->v);
			if (sv_vunref(m->r, v->v))
				gc += size;
		}
		sx_vpool_push(&m->pool, v);
	}
	ss_quota(m->r->quota, SS_QREMOVE, gc);
}

static enum sxstate sx_rollback(struct sx *x)
{
	struct sxmanager *m = x->manager;
	struct ssiter i;
	ss_iterinit(ss_bufiter, &i);
	ss_iteropen(ss_bufiter, &i, &x->log->buf, sizeof(struct svlogv));
	/* support log free after commit and half-commit mode */
	if (x->state == SXCOMMIT) {
		int gc = 0;
		for (; ss_iterhas(ss_bufiter, &i); ss_iternext(ss_bufiter, &i))
		{
			struct svlogv *lv = ss_iterof(ss_bufiter, &i);
			struct svv *v = lv->v.v;
			int size = sv_vsize(v);
			if (sv_vunref(m->r, v))
				gc += size;
		}
		ss_quota(m->r->quota, SS_QREMOVE, gc);
		sx_promote(x, SXROLLBACK);
		return SXROLLBACK;
	}
	sx_rollback_svp(x, &i, 1);
	sx_promote(x, SXROLLBACK);
	sx_end(x);
	return SXROLLBACK;
}

static inline int
sx_preparecb(struct sx *x, struct svlogv *v, uint64_t lsn, sxpreparef prepare, void *arg)
{
	if (sslikely(lsn == x->vlsn))
		return 0;
	if (prepare) {
		struct sxindex *i = ((struct sxv*)v->v.v)->index;
		if (prepare(x, &v->v, i->object, arg))
			return 1;
	}
	return 0;
}

static enum sxstate sx_prepare(struct sx *x, sxpreparef prepare, void *arg)
{
	uint64_t lsn = sr_seq(x->manager->r->seq, SR_LSN);
	/* proceed read-only transactions */
	if (x->type == SXRO || sv_logcount_write(x->log) == 0)
		return sx_promote(x, SXPREPARE);
	struct ssiter i;
	ss_iterinit(ss_bufiter, &i);
	ss_iteropen(ss_bufiter, &i, &x->log->buf, sizeof(struct svlogv));
	enum sxstate rc;
	for (; ss_iterhas(ss_bufiter, &i); ss_iternext(ss_bufiter, &i))
	{
		struct svlogv *lv = ss_iterof(ss_bufiter, &i);
		struct sxv *v = lv->v.v;
		if ((int)v->lo == x->log_read)
			break;
		if (sx_vaborted(v))
			return sx_promote(x, SXROLLBACK);
		if (sslikely(v->prev == NULL)) {
			rc = sx_preparecb(x, lv, lsn, prepare, arg);
			if (ssunlikely(rc != 0))
				return sx_promote(x, SXROLLBACK);
			continue;
		}
		if (sx_vcommitted(v->prev)) {
			if (v->prev->csn > x->csn)
				return sx_promote(x, SXROLLBACK);
			continue;
		}
		/* force commit for read-only conflicts */
		if (v->prev->v->flags & SVGET) {
			rc = sx_preparecb(x, lv, lsn, prepare, arg);
			if (ssunlikely(rc != 0))
				return sx_promote(x, SXROLLBACK);
			continue;
		}
		return sx_promote(x, SXLOCK);
	}
	return sx_promote(x, SXPREPARE);
}

static enum sxstate sx_commit(struct sx *x)
{
	assert(x->state == SXPREPARE);

	struct sxmanager *m = x->manager;
	struct ssiter i;
	ss_iterinit(ss_bufiter, &i);
	ss_iteropen(ss_bufiter, &i, &x->log->buf, sizeof(struct svlogv));
	uint64_t csn = ++m->csn;
	for (; ss_iterhas(ss_bufiter, &i); ss_iternext(ss_bufiter, &i))
	{
		struct svlogv *lv = ss_iterof(ss_bufiter, &i);
		struct sxv *v = lv->v.v;
		if ((int)v->lo == x->log_read)
			break;
		/* abort conflict reader */
		if (v->prev && !sx_vcommitted(v->prev)) {
			assert(v->prev->v->flags & SVGET);
			sx_vabort(v->prev);
		}
		/* abort waiters */
		sx_vabort_all(v->next);
		/* mark stmt as commited */
		sx_vcommit(v, csn);
		/* translate log version from struct sxv to struct svv */
		sv_init(&lv->v, &sv_vif, v->v, NULL);
		/* schedule read stmt for gc */
		if (v->v->flags & SVGET) {
			sv_vref(v->v);
			v->gc = m->gc;
			m->gc = v;
			m->count_gc++;
		} else {
			sx_untrack(v);
			sx_vpool_push(&m->pool, v);
		}
	}

	/* rollback latest reads */
	sx_rollback_svp(x, &i, 0);

	sx_promote(x, SXCOMMIT);
	sx_end(x);
	return SXCOMMIT;
}

ss_rbget(sx_match,
         sf_compare(scheme, sv_vpointer((sscast(n, struct sxv, node))->v),
                    (sscast(n, struct sxv, node))->v->size,
                    key, keysize))

static int sx_set(struct sx *x, struct sxindex *index, struct svv *version)
{
	struct sxmanager *m = x->manager;
	struct runtime *r = m->r;
	if (! (version->flags & SVGET)) {
		x->log_read = -1;
	}
	/* allocate mvcc container */
	struct sxv *v = sx_valloc(&m->pool, version);
	if (ssunlikely(v == NULL)) {
		ss_quota(r->quota, SS_QREMOVE, sv_vsize(version));
		sv_vunref(r, version);
		return -1;
	}
	v->id = x->id;
	v->index = index;
	struct svlogv lv;
	lv.id   = index->dsn;
	lv.next = UINT32_MAX;
	sv_init(&lv.v, &sx_vif, v, NULL);
	/* update concurrent index */
	struct ssrbnode *n = NULL;
	int rc = sx_match(&index->i, index->r->scheme,
	                  sv_vpointer(version),
	                  version->size,
	                  &n);
	if (ssunlikely(rc == 0 && n)) {
		/* exists */
	} else {
		int pos = rc;
		/* unique */
		v->lo = sv_logcount(x->log);
		rc = sv_logadd(x->log, r->a, &lv, index->ptr);
		if (ssunlikely(rc == -1)) {
			sr_oom(r->e);
			goto error;
		}
		ss_rbset(&index->i, n, pos, &v->node);
		return 0;
	}
	struct sxv *head = sscast(n, struct sxv, node);
	/* match previous update made by current
	 * transaction */
	struct sxv *own = sx_vmatch(head, x->id);
	if (ssunlikely(own))
	{
		if (ssunlikely(version->flags & SVUPSERT)) {
			sr_error(r->e, "%s", "only one upsert statement is "
			         "allowed per a transaction key");
			goto error;
		}
		/* replace old document with the new one */
		lv.next = sv_logat(x->log, own->lo)->next;
		v->lo = own->lo;
		if (ssunlikely(sx_vaborted(own)))
			sx_vabort(v);
		sx_vreplace(own, v);
		if (sslikely(head == own))
			ss_rbreplace(&index->i, &own->node, &v->node);
		/* update log */
		sv_logreplace(x->log, v->lo, &lv);

		ss_quota(r->quota, SS_QREMOVE, sv_vsize(own->v));
		sx_vfree(&m->pool, own);
		return 0;
	}
	/* update log */
	v->lo = sv_logcount(x->log);
	rc = sv_logadd(x->log, r->a, &lv, index->ptr);
	if (ssunlikely(rc == -1)) {
		sr_oom(r->e);
		goto error;
	}
	/* add version */
	sx_vlink(head, v);
	return 0;
error:
	ss_quota(r->quota, SS_QREMOVE, sv_vsize(v->v));
	sx_vfree(&m->pool, v);
	return -1;
}

static int sx_get(struct sx *x, struct sxindex *index, struct sv *key, struct sv *result)
{
	struct sxmanager *m = x->manager;
	struct ssrbnode *n = NULL;
	int rc;
	rc = sx_match(&index->i, index->r->scheme,
	              sv_pointer(key),
	              sv_size(key),
	              &n);
	if (! (rc == 0 && n))
		goto add;
	struct sxv *head = sscast(n, struct sxv, node);
	struct sxv *v = sx_vmatch(head, x->id);
	if (v == NULL)
		goto add;
	if (ssunlikely((v->v->flags & SVGET) > 0))
		return 0;
	if (ssunlikely((v->v->flags & SVDELETE) > 0))
		return 2;
	struct sv vv;
	sv_init(&vv, &sv_vif, v->v, NULL);
	struct svv *ret = sv_vdup(m->r, &vv);
	if (ssunlikely(ret == NULL)) {
		rc = sr_oom(m->r->e);
	} else {
		sv_init(result, &sv_vif, ret, NULL);
		rc = 1;
	}
	return rc;

add:
	/* track a start of the latest read sequence in the
	 * transactional log */
	if (x->log_read == -1)
		x->log_read = sv_logcount(x->log);
	rc = sx_set(x, index, key->v);
	if (ssunlikely(rc == -1))
		return -1;
	sv_vref((struct svv*)key->v);
	return 0;
}

static enum sxstate
sx_set_autocommit(struct sxmanager *m, struct sxindex *index,
		  struct sx *x, struct svlog *log, struct svv *v)
{
	if (sslikely(m->count_rw == 0)) {
		sx_init(m, x, log);
		struct svlogv lv;
		lv.id   = index->dsn;
		lv.next = UINT32_MAX;
		sv_init(&lv.v, &sv_vif, v, NULL);
		sv_logadd(x->log, m->r->a, &lv, index->ptr);
		sr_seq(m->r->seq, SR_TSNNEXT);
		sx_promote(x, SXCOMMIT);
		return SXCOMMIT;
	}
	sx_begin(m, x, SXRW, log, 0);
	int rc = sx_set(x, index, v);
	if (ssunlikely(rc == -1)) {
		sx_rollback(x);
		return SXROLLBACK;
	}
	enum sxstate s = sx_prepare(x, NULL, NULL);
	if (sslikely(s == SXPREPARE))
		sx_commit(x);
	else
	if (s == SXLOCK)
		sx_rollback(x);
	return s;
}

static enum sxstate sx_get_autocommit(struct sxmanager *m, struct sxindex *index ssunused)
{
	sr_seq(m->r->seq, SR_TSNNEXT);
	return SXCOMMIT;
}

static inline int
sx_deadlock_in(struct sxmanager *m, struct rlist *mark, struct sx *t, struct sx *p)
{
	if (p->deadlock.next != &p->deadlock)
		return 0;
	rlist_add(mark, &p->deadlock);
	struct ssiter i;
	ss_iterinit(ss_bufiter, &i);
	ss_iteropen(ss_bufiter, &i, &p->log->buf, sizeof(struct svlogv));
	for (; ss_iterhas(ss_bufiter, &i); ss_iternext(ss_bufiter, &i))
	{
		struct svlogv *lv = ss_iterof(ss_bufiter, &i);
		struct sxv *v = lv->v.v;
		if (v->prev == NULL)
			continue;
		do {
			struct sx *n = sx_find(m, v->id);
			assert(n != NULL);
			if (ssunlikely(n == t))
				return 1;
			int rc = sx_deadlock_in(m, mark, t, n);
			if (ssunlikely(rc == 1))
				return 1;
			v = v->prev;
		} while (v);
	}
	return 0;
}

static inline void
sx_deadlock_unmark(struct rlist *mark)
{
	struct sx *t, *n;
	rlist_foreach_entry_safe(t, mark, deadlock, n) {
		rlist_create(&t->deadlock);
	}
}

static int sx_deadlock(struct sx *t)
{
	struct sxmanager *m = t->manager;
	struct rlist mark;
	rlist_create(&mark);
	struct ssiter i;
	ss_iterinit(ss_bufiter, &i);
	ss_iteropen(ss_bufiter, &i, &t->log->buf, sizeof(struct svlogv));
	while (ss_iterhas(ss_bufiter, &i))
	{
		struct svlogv *lv = ss_iterof(ss_bufiter, &i);
		struct sxv *v = lv->v.v;
		if (v->prev == NULL) {
			ss_iternext(ss_bufiter, &i);
			continue;
		}
		struct sx *p = sx_find(m, v->prev->id);
		assert(p != NULL);
		int rc = sx_deadlock_in(m, &mark, t, p);
		if (ssunlikely(rc)) {
			sx_deadlock_unmark(&mark);
			return 1;
		}
		ss_iternext(ss_bufiter, &i);
	}
	sx_deadlock_unmark(&mark);
	return 0;
}

static uint8_t
sx_vifflags(struct sv *v) {
	return ((struct sxv*)v->v)->v->flags;
}

static uint64_t
sx_viflsn(struct sv *v) {
	return ((struct sxv*)v->v)->v->lsn;
}

static void
sx_viflsnset(struct sv *v, uint64_t lsn) {
	((struct sxv*)v->v)->v->lsn = lsn;
}

static uint32_t
sx_viftimestamp(struct sv *v) {
	return ((struct sxv*)v->v)->v->timestamp;
}

static char*
sx_vifpointer(struct sv *v) {
	return sv_vpointer(((struct sxv*)v->v)->v);
}

static uint32_t
sx_vifsize(struct sv *v) {
	return ((struct sxv*)v->v)->v->size;
}

static struct svif sx_vif =
{
	.flags     = sx_vifflags,
	.lsn       = sx_viflsn,
	.lsnset    = sx_viflsnset,
	.timestamp = sx_viftimestamp,
	.pointer   = sx_vifpointer,
	.size      = sx_vifsize
};

struct slconf {
	int   enable;
	char *path;
	int   sync_on_rotate;
	int   sync_on_write;
	int   rotatewm;
};

struct sldirtype {
	char *ext;
	uint32_t mask;
	int count;
};

struct sldirid {
	uint32_t mask;
	uint64_t id;
};

static int sl_dirread(struct ssbuf*, struct ssa*, struct sldirtype*, char*);

struct slv {
	uint32_t crc;
	uint64_t lsn;
	uint32_t dsn;
	uint32_t size;
	uint32_t timestamp;
	uint8_t  flags;
} sspacked;

static struct svif sl_vif;

static inline uint32_t
sl_vdsn(struct sv *v) {
	return ((struct slv*)v->v)->dsn;
}

static inline uint32_t
sl_vtimestamp(struct sv *v) {
	return ((struct slv*)v->v)->timestamp;
}

struct sl {
	uint64_t id;
	struct ssgc gc;
	pthread_mutex_t filelock;
	struct ssfile file;
	struct slpool *p;
	struct rlist link;
	struct rlist linkcopy;
};

struct slpool {
	pthread_mutex_t lock;
	struct slconf *conf;
	struct rlist list;
	int gc;
	int n;
	struct ssiov iov;
	struct runtime *r;
};

struct sltx {
	struct slpool *p;
	struct sl *l;
	int recover;
	uint64_t lsn;
	uint64_t svp;
};

static int sl_poolinit(struct slpool*, struct runtime*);
static int sl_poolopen(struct slpool*, struct slconf*);
static int sl_poolrotate(struct slpool*);
static int sl_poolrotate_ready(struct slpool*);
static int sl_poolshutdown(struct slpool*);
static int sl_poolgc_enable(struct slpool*, int);
static int sl_poolgc(struct slpool*);
static int sl_poolfiles(struct slpool*);
static int sl_poolcopy(struct slpool*, char*, struct ssbuf*);

static int sl_begin(struct slpool*, struct sltx*, uint64_t, int);
static int sl_commit(struct sltx*);
static int sl_rollback(struct sltx*);
static int sl_write(struct sltx*, struct svlog*);

static int sl_iter_open(struct ssiter *i, struct runtime*, struct ssfile*, int);
static int sl_iter_error(struct ssiter*);
static int sl_iter_continue(struct ssiter*);

static struct ssiterif sl_iter;

static inline struct sl*
sl_alloc(struct slpool *p, uint64_t id)
{
	struct sl *l = ss_malloc(p->r->a, sizeof(*l));
	if (ssunlikely(l == NULL)) {
		sr_oom_malfunction(p->r->e);
		return NULL;
	}
	l->id   = id;
	l->p    = NULL;
	ss_gcinit(&l->gc);
	tt_pthread_mutex_init(&l->filelock, NULL);
	ss_fileinit(&l->file, p->r->vfs);
	rlist_create(&l->link);
	rlist_create(&l->linkcopy);
	return l;
}

static inline int
sl_close(struct slpool *p, struct sl *l)
{
	int rc = ss_fileclose(&l->file);
	if (ssunlikely(rc == -1)) {
		sr_malfunction(p->r->e, "log file '%s' close error: %s",
		               ss_pathof(&l->file.path),
		               strerror(errno));
	}
	tt_pthread_mutex_destroy(&l->filelock);
	ss_gcfree(&l->gc);
	ss_free(p->r->a, l);
	return rc;
}

static inline struct sl*
sl_open(struct slpool *p, uint64_t id)
{
	struct sl *l = sl_alloc(p, id);
	if (ssunlikely(l == NULL))
		return NULL;
	struct sspath path;
	ss_path(&path, p->conf->path, id, ".log");
	int rc = ss_fileopen(&l->file, path.path);
	if (ssunlikely(rc == -1)) {
		sr_malfunction(p->r->e, "log file '%s' open error: %s",
		               ss_pathof(&l->file.path),
		               strerror(errno));
		goto error;
	}
	return l;
error:
	sl_close(p, l);
	return NULL;
}

static inline struct sl*
sl_new(struct slpool *p, uint64_t id)
{
	struct sl *l = sl_alloc(p, id);
	if (ssunlikely(l == NULL))
		return NULL;
	struct sspath path;
	ss_path(&path, p->conf->path, id, ".log");
	int rc = ss_filenew(&l->file, path.path);
	if (ssunlikely(rc == -1)) {
		sr_malfunction(p->r->e, "log file '%s' create error: %s",
		               path.path, strerror(errno));
		goto error;
	}
	struct srversion v;
	sr_version_storage(&v);
	rc = ss_filewrite(&l->file, &v, sizeof(v));
	if (ssunlikely(rc == -1)) {
		sr_malfunction(p->r->e, "log file '%s' header write error: %s",
		               ss_pathof(&l->file.path),
		               strerror(errno));
		goto error;
	}
	return l;
error:
	sl_close(p, l);
	return NULL;
}

static int sl_poolinit(struct slpool *p, struct runtime *r)
{
	tt_pthread_mutex_init(&p->lock, NULL);
	rlist_create(&p->list);
	p->n    = 0;
	p->r    = r;
	p->gc   = 1;
	p->conf = NULL;
	struct iovec *iov =
		ss_malloc(r->a, sizeof(struct iovec) * 1021);
	if (ssunlikely(iov == NULL))
		return sr_oom_malfunction(r->e);
	ss_iovinit(&p->iov, iov, 1021);
	return 0;
}

static inline int
sl_poolcreate(struct slpool *p)
{
	int rc;
	rc = ss_vfsmkdir(p->r->vfs, p->conf->path, 0755);
	if (ssunlikely(rc == -1))
		return sr_malfunction(p->r->e, "log directory '%s' create error: %s",
		                      p->conf->path, strerror(errno));
	return 1;
}

static inline int
sl_poolrecover(struct slpool *p)
{
	struct ssbuf list;
	ss_bufinit(&list);
	struct sldirtype types[] =
	{
		{ "log", 1, 0 },
		{ NULL,  0, 0 }
	};
	int rc = sl_dirread(&list, p->r->a, types, p->conf->path);
	if (ssunlikely(rc == -1))
		return sr_malfunction(p->r->e, "log directory '%s' open error",
		                      p->conf->path);
	struct ssiter i;
	ss_iterinit(ss_bufiter, &i);
	ss_iteropen(ss_bufiter, &i, &list, sizeof(struct sldirid));
	while(ss_iterhas(ss_bufiter, &i)) {
		struct sldirid *id = ss_iterof(ss_bufiter, &i);
		struct sl *l = sl_open(p, id->id);
		if (ssunlikely(l == NULL)) {
			ss_buffree(&list, p->r->a);
			return -1;
		}
		rlist_add(&p->list, &l->link);
		p->n++;
		ss_iternext(ss_bufiter, &i);
	}
	ss_buffree(&list, p->r->a);
	if (p->n) {
		struct sl *last = sscast(p->list.prev, struct sl, link);
		p->r->seq->lfsn = last->id;
		p->r->seq->lfsn++;
	}
	return 0;
}

static int sl_poolopen(struct slpool *p, struct slconf *conf)
{
	p->conf = conf;
	if (ssunlikely(! p->conf->enable))
		return 0;
	int exists = ss_vfsexists(p->r->vfs, p->conf->path);
	int rc;
	if (! exists)
		rc = sl_poolcreate(p);
	else
		rc = sl_poolrecover(p);
	if (ssunlikely(rc == -1))
		return -1;
	return 0;
}

static int sl_poolrotate(struct slpool *p)
{
	if (ssunlikely(! p->conf->enable))
		return 0;
	uint64_t lfsn = sr_seq(p->r->seq, SR_LFSNNEXT);
	struct sl *l = sl_new(p, lfsn);
	if (ssunlikely(l == NULL))
		return -1;
	struct sl *log = NULL;
	tt_pthread_mutex_lock(&p->lock);
	if (p->n)
		log = sscast(p->list.prev, struct sl, link);
	rlist_add(&p->list, &l->link);
	p->n++;
	tt_pthread_mutex_unlock(&p->lock);
	if (log) {
		assert(log->file.fd != -1);
		if (p->conf->sync_on_rotate) {
			int rc = ss_filesync(&log->file);
			if (ssunlikely(rc == -1)) {
				sr_malfunction(p->r->e, "log file '%s' sync error: %s",
				               ss_pathof(&log->file.path),
				               strerror(errno));
				return -1;
			}
		}
		ss_fileadvise(&log->file, 0, 0, log->file.size);
		ss_gccomplete(&log->gc);
	}
	return 0;
}

static int sl_poolrotate_ready(struct slpool *p)
{
	if (ssunlikely(! p->conf->enable))
		return 0;
	tt_pthread_mutex_lock(&p->lock);
	assert(p->n > 0);
	struct sl *l = sscast(p->list.prev, struct sl, link);
	int ready = ss_gcrotateready(&l->gc, p->conf->rotatewm);
	tt_pthread_mutex_unlock(&p->lock);
	return ready;
}

static int sl_poolshutdown(struct slpool *p)
{
	int rcret = 0;
	int rc;
	if (p->n) {
		struct sl *l, *n;
		rlist_foreach_entry_safe(l, &p->list, link, n) {
			rc = sl_close(p, l);
			if (ssunlikely(rc == -1))
				rcret = -1;
		}
	}
	if (p->iov.v)
		ss_free(p->r->a, p->iov.v);
	tt_pthread_mutex_destroy(&p->lock);
	return rcret;
}

static inline int
sl_gc(struct slpool *p, struct sl *l)
{
	int rc;
	rc = ss_vfsunlink(p->r->vfs, ss_pathof(&l->file.path));
	if (ssunlikely(rc == -1)) {
		return sr_malfunction(p->r->e, "log file '%s' unlink error: %s",
		                      ss_pathof(&l->file.path),
		                      strerror(errno));
	}
	rc = sl_close(p, l);
	if (ssunlikely(rc == -1))
		return -1;
	return 1;
}

static int sl_poolgc_enable(struct slpool *p, int enable)
{
	tt_pthread_mutex_lock(&p->lock);
	p->gc = enable;
	tt_pthread_mutex_unlock(&p->lock);
	return 0;
}

static int sl_poolgc(struct slpool *p)
{
	if (ssunlikely(! p->conf->enable))
		return 0;
	for (;;) {
		tt_pthread_mutex_lock(&p->lock);
		if (ssunlikely(! p->gc)) {
			tt_pthread_mutex_unlock(&p->lock);
			return 0;
		}
		struct sl *current = NULL, *l;
		rlist_foreach_entry(l, &p->list, link) {
			if (sslikely(! ss_gcgarbage(&l->gc)))
				continue;
			rlist_del(&l->link);
			p->n--;
			current = l;
			break;
		}
		tt_pthread_mutex_unlock(&p->lock);
		if (current) {
			int rc = sl_gc(p, current);
			if (ssunlikely(rc == -1))
				return -1;
		} else {
			break;
		}
	}
	return 0;
}

static int sl_poolfiles(struct slpool *p)
{
	tt_pthread_mutex_lock(&p->lock);
	int n = p->n;
	tt_pthread_mutex_unlock(&p->lock);
	return n;
}

static int sl_poolcopy(struct slpool *p, char *dest, struct ssbuf *buf)
{
	struct rlist list;
	rlist_create(&list);
	tt_pthread_mutex_lock(&p->lock);
	struct sl *l;
	rlist_foreach_entry(l, &p->list, link) {
		if (ss_gcinprogress(&l->gc))
			break;
		rlist_add(&list, &l->linkcopy);
	}
	tt_pthread_mutex_unlock(&p->lock);

	ss_bufreset(buf);
	struct sl *n;
	rlist_foreach_entry_safe(l, &list, link, n)
	{
		rlist_create(&l->linkcopy);
		struct sspath path;
		ss_path(&path, dest, l->id, ".log");
		struct ssfile file;
		ss_fileinit(&file, p->r->vfs);
		int rc = ss_filenew(&file, path.path);
		if (ssunlikely(rc == -1)) {
			sr_malfunction(p->r->e, "log file '%s' create error: %s",
			               path.path, strerror(errno));
			return -1;
		}
		rc = ss_bufensure(buf, p->r->a, l->file.size);
		if (ssunlikely(rc == -1)) {
			sr_oom_malfunction(p->r->e);
			ss_fileclose(&file);
			return -1;
		}
		rc = ss_filepread(&l->file, 0, buf->s, l->file.size);
		if (ssunlikely(rc == -1)) {
			sr_malfunction(p->r->e, "log file '%s' read error: %s",
			               ss_pathof(&l->file.path),
			               strerror(errno));
			ss_fileclose(&file);
			return -1;
		}
		ss_bufadvance(buf, l->file.size);
		rc = ss_filewrite(&file, buf->s, l->file.size);
		if (ssunlikely(rc == -1)) {
			sr_malfunction(p->r->e, "log file '%s' write error: %s",
			               path.path,
			               strerror(errno));
			ss_fileclose(&file);
			return -1;
		}
		/* sync? */
		rc = ss_fileclose(&file);
		if (ssunlikely(rc == -1)) {
			sr_malfunction(p->r->e, "log file '%s' close error: %s",
			               path.path, strerror(errno));
			return -1;
		}
		ss_bufreset(buf);
	}
	return 0;
}

static int sl_begin(struct slpool *p, struct sltx *t, uint64_t lsn, int recover)
{
	tt_pthread_mutex_lock(&p->lock);
	if (sslikely(lsn == 0)) {
		lsn = sr_seq(p->r->seq, SR_LSNNEXT);
	} else {
		sr_seqlock(p->r->seq);
		if (lsn > p->r->seq->lsn)
			p->r->seq->lsn = lsn;
		sr_sequnlock(p->r->seq);
	}
	t->lsn = lsn;
	t->recover = recover;
	t->svp = 0;
	t->p = p;
	t->l = NULL;
	if (! p->conf->enable)
		return 0;
	assert(p->n > 0);
	struct sl *l = sscast(p->list.prev, struct sl, link);
	tt_pthread_mutex_lock(&l->filelock);
	t->svp = ss_filesvp(&l->file);
	t->l = l;
	t->p = p;
	return 0;
}

static int sl_commit(struct sltx *t)
{
	if (t->p->conf->enable)
		tt_pthread_mutex_unlock(&t->l->filelock);
	tt_pthread_mutex_unlock(&t->p->lock);
	return 0;
}

static int sl_rollback(struct sltx *t)
{
	int rc = 0;
	if (t->p->conf->enable) {
		rc = ss_filerlb(&t->l->file, t->svp);
		if (ssunlikely(rc == -1))
			sr_malfunction(t->p->r->e, "log file '%s' truncate error: %s",
			               ss_pathof(&t->l->file.path),
			               strerror(errno));
		tt_pthread_mutex_unlock(&t->l->filelock);
	}
	tt_pthread_mutex_unlock(&t->p->lock);
	return rc;
}

static inline void
sl_writeadd(struct slpool *p, struct sltx *t, struct slv *lv, struct svlogv *logv)
{
	struct sv *v = &logv->v;
	lv->lsn       = t->lsn;
	lv->dsn       = logv->id;
	lv->flags     = sv_flags(v);
	lv->size      = sv_size(v);
	lv->timestamp = sv_timestamp(v);
	lv->crc       = ss_crcp(sv_pointer(v), lv->size, 0);
	lv->crc       = ss_crcs(lv, sizeof(struct slv), lv->crc);
	ss_iovadd(&p->iov, lv, sizeof(struct slv));
	ss_iovadd(&p->iov, sv_pointer(v), lv->size);
	((struct svv*)v->v)->log = t->l;
}

static inline int
sl_writestmt(struct sltx *t, struct svlog *vlog)
{
	struct slpool *p = t->p;
	struct svlogv *stmt = NULL;
	struct ssiter i;
	ss_iterinit(ss_bufiter, &i);
	ss_iteropen(ss_bufiter, &i, &vlog->buf, sizeof(struct svlogv));
	for (; ss_iterhas(ss_bufiter, &i); ss_iternext(ss_bufiter, &i)) {
		struct svlogv *logv = ss_iterof(ss_bufiter, &i);
		struct sv *v = &logv->v;
		assert(v->i == &sv_vif);
		sv_lsnset(v, t->lsn);
		if (sslikely(! (sv_is(v, SVGET)))) {
			assert(stmt == NULL);
			stmt = logv;
		}
	}
	assert(stmt != NULL);
	struct slv lv;
	sl_writeadd(t->p, t, &lv, stmt);
	int rc = ss_filewritev(&t->l->file, &p->iov);
	if (ssunlikely(rc == -1)) {
		sr_malfunction(p->r->e, "log file '%s' write error: %s",
		               ss_pathof(&t->l->file.path),
		               strerror(errno));
		return -1;
	}
	ss_gcmark(&t->l->gc, 1);
	ss_iovreset(&p->iov);
	return 0;
}

static int
sl_writestmt_multi(struct sltx *t, struct svlog *vlog)
{
	struct slpool *p = t->p;
	struct sl *l = t->l;
	struct slv lvbuf[510]; /* 1 + 510 per syscall */
	int lvp;
	int rc;
	lvp = 0;
	/* transaction header */
	struct slv *lv = &lvbuf[0];
	lv->lsn       = t->lsn;
	lv->dsn       = 0;
	lv->timestamp = 0;
	lv->flags     = SVBEGIN;
	lv->size      = sv_logcount_write(vlog);
	lv->crc       = ss_crcs(lv, sizeof(struct slv), 0);
	ss_iovadd(&p->iov, lv, sizeof(struct slv));
	lvp++;
	/* body */
	struct ssiter i;
	ss_iterinit(ss_bufiter, &i);
	ss_iteropen(ss_bufiter, &i, &vlog->buf, sizeof(struct svlogv));
	for (; ss_iterhas(ss_bufiter, &i); ss_iternext(ss_bufiter, &i))
	{
		if (ssunlikely(! ss_iovensure(&p->iov, 2))) {
			rc = ss_filewritev(&l->file, &p->iov);
			if (ssunlikely(rc == -1)) {
				sr_malfunction(p->r->e, "log file '%s' write error: %s",
				               ss_pathof(&l->file.path),
				               strerror(errno));
				return -1;
			}
			ss_iovreset(&p->iov);
			lvp = 0;
		}
		struct svlogv *logv = ss_iterof(ss_bufiter, &i);
		struct sv *v = &logv->v;
		assert(v->i == &sv_vif);
		sv_lsnset(v, t->lsn);
		if (sv_is(v, SVGET))
			continue;
		lv = &lvbuf[lvp];
		sl_writeadd(p, t, lv, logv);
		lvp++;
	}
	if (sslikely(ss_iovhas(&p->iov))) {
		rc = ss_filewritev(&l->file, &p->iov);
		if (ssunlikely(rc == -1)) {
			sr_malfunction(p->r->e, "log file '%s' write error: %s",
			               ss_pathof(&l->file.path),
			               strerror(errno));
			return -1;
		}
		ss_iovreset(&p->iov);
	}
	ss_gcmark(&l->gc, sv_logcount_write(vlog));
	return 0;
}

static int sl_write(struct sltx *t, struct svlog *vlog)
{
	int count = sv_logcount_write(vlog);
	/* fast path for log-disabled, recover or
	 * ro-transactions
	 */
	if (t->recover || !t->p->conf->enable || count == 0)
	{
		struct ssiter i;
		ss_iterinit(ss_bufiter, &i);
		ss_iteropen(ss_bufiter, &i, &vlog->buf, sizeof(struct svlogv));
		for (; ss_iterhas(ss_bufiter, &i); ss_iternext(ss_bufiter, &i))
		{
			struct svlogv *v = ss_iterof(ss_bufiter, &i);
			sv_lsnset(&v->v, t->lsn);
		}
		return 0;
	}

	/* write single or multi-stmt transaction */
	int rc;
	if (sslikely(count == 1)) {
		rc = sl_writestmt(t, vlog);
	} else {
		rc = sl_writestmt_multi(t, vlog);
	}
	if (ssunlikely(rc == -1))
		return -1;

	/* sync */
	if (t->p->conf->sync_on_write) {
		rc = ss_filesync(&t->l->file);
		if (ssunlikely(rc == -1)) {
			sr_malfunction(t->p->r->e, "log file '%s' sync error: %s",
			               ss_pathof(&t->l->file.path),
			               strerror(errno));
			return -1;
		}
	}
	return 0;
}

static inline ssize_t sl_diridof(char *s)
{
	size_t v = 0;
	while (*s && *s != '.') {
		if (ssunlikely(!isdigit(*s)))
			return -1;
		v = (v * 10) + *s - '0';
		s++;
	}
	return v;
}

static inline struct sldirid*
sl_dirmatch(struct ssbuf *list, uint64_t id)
{
	if (ssunlikely(ss_bufused(list) == 0))
		return NULL;
	struct sldirid *n = (struct sldirid*)list->s;
	while ((char*)n < list->p) {
		if (n->id == id)
			return n;
		n++;
	}
	return NULL;
}

static inline struct sldirtype*
sl_dirtypeof(struct sldirtype *types, char *ext)
{
	struct sldirtype *p = &types[0];
	int n = 0;
	while (p[n].ext != NULL) {
		if (strcmp(p[n].ext, ext) == 0)
			return &p[n];
		n++;
	}
	return NULL;
}

static int
sl_dircmp(const void *p1, const void *p2)
{
	struct sldirid *a = (struct sldirid*)p1;
	struct sldirid *b = (struct sldirid*)p2;
	assert(a->id != b->id);
	return (a->id > b->id)? 1: -1;
}

static int sl_dirread(struct ssbuf *list, struct ssa *a, struct sldirtype *types, char *dir)
{
	DIR *d = opendir(dir);
	if (ssunlikely(d == NULL))
		return -1;

	struct dirent *de;
	while ((de = readdir(d))) {
		if (ssunlikely(de->d_name[0] == '.'))
			continue;
		ssize_t id = sl_diridof(de->d_name);
		if (ssunlikely(id == -1))
			goto error;
		char *ext = strstr(de->d_name, ".");
		if (ssunlikely(ext == NULL))
			goto error;
		ext++;
		struct sldirtype *type = sl_dirtypeof(types, ext);
		if (ssunlikely(type == NULL))
			continue;
		struct sldirid *n = sl_dirmatch(list, id);
		if (n) {
			n->mask |= type->mask;
			type->count++;
			continue;
		}
		int rc = ss_bufensure(list, a, sizeof(struct sldirid));
		if (ssunlikely(rc == -1))
			goto error;
		n = (struct sldirid*)list->p;
		ss_bufadvance(list, sizeof(struct sldirid));
		n->id  = id;
		n->mask = type->mask;
		type->count++;
	}
	closedir(d);

	if (ssunlikely(ss_bufused(list) == 0))
		return 0;

	int n = ss_bufused(list) / sizeof(struct sldirid);
	qsort(list->s, n, sizeof(struct sldirid), sl_dircmp);
	return n;

error:
	closedir(d);
	return -1;
}

struct sliter {
	int validate;
	int error;
	struct ssfile *log;
	struct ssmmap map;
	struct slv *v;
	struct slv *next;
	uint32_t count;
	uint32_t pos;
	struct sv current;
	struct runtime *r;
} sspacked;

static void
sl_iterseterror(struct sliter *i)
{
	i->error = 1;
	i->v     = NULL;
	i->next  = NULL;
}

static int
sl_iternext_of(struct sliter *i, struct slv *next, int validate)
{
	if (next == NULL)
		return 0;
	char *eof   = (char*)i->map.p + i->map.size;
	char *start = (char*)next;

	/* eof */
	if (ssunlikely(start == eof)) {
		if (i->count != i->pos) {
			sr_malfunction(i->r->e, "corrupted log file '%s': transaction is incomplete",
			               ss_pathof(&i->log->path));
			sl_iterseterror(i);
			return -1;
		}
		i->v = NULL;
		i->next = NULL;
		return 0;
	}

	char *end = start + next->size;
	if (ssunlikely((start > eof || (end > eof)))) {
		sr_malfunction(i->r->e, "corrupted log file '%s': bad record size",
		               ss_pathof(&i->log->path));
		sl_iterseterror(i);
		return -1;
	}
	if (validate && i->validate)
	{
		uint32_t crc = 0;
		if (! (next->flags & SVBEGIN)) {
			crc = ss_crcp(start + sizeof(struct slv), next->size, 0);
		}
		crc = ss_crcs(start, sizeof(struct slv), crc);
		if (ssunlikely(crc != next->crc)) {
			sr_malfunction(i->r->e, "corrupted log file '%s': bad record crc",
			               ss_pathof(&i->log->path));
			sl_iterseterror(i);
			return -1;
		}
	}
	i->pos++;
	if (i->pos > i->count) {
		/* next transaction */
		i->v     = NULL;
		i->pos   = 0;
		i->count = 0;
		i->next  = next;
		return 0;
	}
	i->v = next;
	sv_init(&i->current, &sl_vif, i->v, NULL);
	return 1;
}

static int sl_itercontinue_of(struct sliter *i)
{
	if (ssunlikely(i->error))
		return -1;
	if (ssunlikely(i->v))
		return 1;
	if (ssunlikely(i->next == NULL))
		return 0;
	int validate = 0;
	i->pos   = 0;
	i->count = 0;
	struct slv *v = i->next;
	if (v->flags & SVBEGIN) {
		validate = 1;
		i->count = v->size;
		v = (struct slv*)((char*)i->next + sizeof(struct slv));
	} else {
		i->count = 1;
		v = i->next;
	}
	return sl_iternext_of(i, v, validate);
}

static inline int
sl_iterprepare(struct sliter *i)
{
	struct srversion *ver = (struct srversion*)i->map.p;
	if (! sr_versionstorage_check(ver))
		return sr_malfunction(i->r->e, "bad log file '%s' version",
		                      ss_pathof(&i->log->path));
	if (ssunlikely(i->log->size < (sizeof(struct srversion))))
		return sr_malfunction(i->r->e, "corrupted log file '%s': bad size",
		                      ss_pathof(&i->log->path));
	struct slv *next = (struct slv*)((char*)i->map.p + sizeof(struct srversion));
	int rc = sl_iternext_of(i, next, 1);
	if (ssunlikely(rc == -1))
		return -1;
	if (sslikely(i->next))
		return sl_itercontinue_of(i);
	return 0;
}

static int sl_iter_open(struct ssiter *i, struct runtime *r, struct ssfile *file, int validate)
{
	struct sliter *li = (struct sliter*)i->priv;
	memset(li, 0, sizeof(*li));
	li->r        = r;
	li->log      = file;
	li->validate = validate;
	if (ssunlikely(li->log->size < sizeof(struct srversion))) {
		sr_malfunction(li->r->e, "corrupted log file '%s': bad size",
		               ss_pathof(&li->log->path));
		return -1;
	}
	if (ssunlikely(li->log->size == sizeof(struct srversion)))
		return 0;
	int rc = ss_vfsmmap(r->vfs, &li->map, li->log->fd, li->log->size, 1);
	if (ssunlikely(rc == -1)) {
		sr_malfunction(li->r->e, "failed to mmap log file '%s': %s",
		               ss_pathof(&li->log->path),
		               strerror(errno));
		return -1;
	}
	rc = sl_iterprepare(li);
	if (ssunlikely(rc == -1))
		ss_vfsmunmap(r->vfs, &li->map);
	return 0;
}

static void
sl_iter_close(struct ssiter *i)
{
	struct sliter *li = (struct sliter*)i->priv;
	ss_vfsmunmap(li->r->vfs, &li->map);
}

static int
sl_iter_has(struct ssiter *i)
{
	struct sliter *li = (struct sliter*)i->priv;
	return li->v != NULL;
}

static void*
sl_iter_of(struct ssiter *i)
{
	struct sliter *li = (struct sliter*)i->priv;
	if (ssunlikely(li->v == NULL))
		return NULL;
	return &li->current;
}

static void
sl_iter_next(struct ssiter *i)
{
	struct sliter *li = (struct sliter*)i->priv;
	if (ssunlikely(li->v == NULL))
		return;
	struct slv *next =
		(struct slv*)((char*)li->v + sizeof(struct slv) + li->v->size);
	sl_iternext_of(li, next, 1);
}

static struct ssiterif sl_iter =
{
	.close   = sl_iter_close,
	.has     = sl_iter_has,
	.of      = sl_iter_of,
	.next    = sl_iter_next
};

static int sl_iter_error(struct ssiter *i)
{
	struct sliter *li = (struct sliter*)i->priv;
	return li->error;
}

static int sl_iter_continue(struct ssiter *i)
{
	struct sliter *li = (struct sliter*)i->priv;
	return sl_itercontinue_of(li);
}

static uint8_t
sl_vifflags(struct sv *v) {
	return ((struct slv*)v->v)->flags;
}

static uint64_t
sl_viflsn(struct sv *v) {
	return ((struct slv*)v->v)->lsn;
}

static char*
sl_vifpointer(struct sv *v) {
	return (char*)v->v + sizeof(struct slv);
}

static uint32_t
sl_viftimestamp(struct sv *v) {
	return ((struct slv*)v->v)->size;
}

static uint32_t
sl_vifsize(struct sv *v) {
	return ((struct slv*)v->v)->size;
}

static struct svif sl_vif =
{
	.flags     = sl_vifflags,
	.lsn       = sl_viflsn,
	.lsnset    = NULL,
	.timestamp = sl_viftimestamp,
	.pointer   = sl_vifpointer,
	.size      = sl_vifsize
};

#define SD_IDBRANCH 1

struct sdid {
	uint64_t parent;
	uint64_t id;
	uint8_t  flags;
} sspacked;

static inline void
sd_idinit(struct sdid *i, uint64_t id, uint64_t parent, uint8_t flags)
{
	i->id     = id;
	i->parent = parent;
	i->flags  = flags;
}

struct sdv {
	uint32_t offset;
	uint8_t  flags;
	uint64_t lsn;
	uint32_t timestamp;
	uint32_t size;
} sspacked;

static struct svif sd_vif;
static struct svif sd_vrawif;

struct sdpageheader {
	uint32_t crc;
	uint32_t crcdata;
	uint32_t count;
	uint32_t countdup;
	uint32_t sizeorigin;
	uint32_t sizekeys;
	uint32_t size;
	uint64_t lsnmin;
	uint64_t lsnmindup;
	uint64_t lsnmax;
	uint32_t tsmin;
	uint32_t reserve;
} sspacked;

struct sdpage {
	struct sdpageheader *h;
};

static inline void
sd_pageinit(struct sdpage *p, struct sdpageheader *h) {
	p->h = h;
}

static inline struct sdv*
sd_pagev(struct sdpage *p, uint32_t pos) {
	assert(pos < p->h->count);
	return (struct sdv*)((char*)p->h + sizeof(struct sdpageheader) + sizeof(struct sdv) * pos);
}

static inline void*
sd_pagepointer(struct sdpage *p, struct sdv *v) {
	assert((sizeof(struct sdv) * p->h->count) + v->offset <= p->h->sizeorigin);
	return ((char*)p->h + sizeof(struct sdpageheader) +
	         sizeof(struct sdv) * p->h->count) + v->offset;
}

static inline char*
sd_pagesparse_keyread(struct sdpage *p, uint32_t offset, uint32_t *size)
{
	char *ptr = (char*)p->h + sizeof(struct sdpageheader) +
	            (p->h->sizeorigin - p->h->sizekeys) + offset;
	*size = *(uint32_t*)ptr;
	return ptr + sizeof(uint32_t);
}

static inline char*
sd_pagesparse_field(struct sdpage *p, struct sdv *v, int pos, uint32_t *size)
{
	uint32_t *offsets = (uint32_t*)sd_pagepointer(p, v);
	return sd_pagesparse_keyread(p, offsets[pos], size);
}

static inline void
sd_pagesparse_convert(struct sdpage *p, struct runtime *r, struct sdv *v, char *dest)
{
	char *ptr = dest;
	memcpy(ptr, v, sizeof(struct sdv));
	ptr += sizeof(struct sdv);
	struct sfv fields[8];
	int i = 0;
	while (i < r->scheme->fields_count) {
		struct sfv *k = &fields[i];
		k->pointer = sd_pagesparse_field(p, v, i, &k->size);
		i++;
	}
	sf_write(r->scheme, fields, ptr);
}

struct sdpageiter {
	struct sdpage *page;
	struct ssbuf *xfbuf;
	int64_t pos;
	struct sdv *v;
	struct sv current;
	enum ssorder order;
	void *key;
	int keysize;
	struct runtime *r;
} sspacked;

static inline void
sd_pageiter_result(struct sdpageiter *i)
{
	if (ssunlikely(i->v == NULL))
		return;
	if (sslikely(i->r->fmt_storage == SF_RAW)) {
		sv_init(&i->current, &sd_vif, i->v, i->page->h);
		return;
	}
	sd_pagesparse_convert(i->page, i->r, i->v, i->xfbuf->s);
	sv_init(&i->current, &sd_vrawif, i->xfbuf->s, NULL);
}

static inline void
sd_pageiter_end(struct sdpageiter *i)
{
	i->pos = i->page->h->count;
	i->v   = NULL;
}

static inline int
sd_pageiter_cmp(struct sdpageiter *i, struct runtime *r, struct sdv *v)
{
	if (sslikely(r->fmt_storage == SF_RAW)) {
		return sf_compare(r->scheme, sd_pagepointer(i->page, v),
		                  v->size, i->key, i->keysize);
	}
	struct sffield **part = r->scheme->keys;
	struct sffield **last = part + r->scheme->keys_count;
	int rc;
	while (part < last) {
		struct sffield *key = *part;
		uint32_t a_fieldsize;
		char *a_field = sd_pagesparse_field(i->page, v, key->position, &a_fieldsize);
		uint32_t b_fieldsize;
		char *b_field = sf_fieldof_ptr(r->scheme, key, i->key, &b_fieldsize);
		rc = key->cmp(a_field, a_fieldsize, b_field, b_fieldsize, NULL);
		if (rc != 0)
			return rc;
		part++;
	}
	return 0;
}

static inline int
sd_pageiter_search(struct sdpageiter *i)
{
	int min = 0;
	int mid = 0;
	int max = i->page->h->count - 1;
	while (max >= min)
	{
		mid = min + (max - min) / 2;
		int rc = sd_pageiter_cmp(i, i->r, sd_pagev(i->page, mid));
		switch (rc) {
		case -1: min = mid + 1;
			continue;
		case  1: max = mid - 1;
			continue;
		default: return mid;
		}
	}
	return min;
}

static inline void
sd_pageiter_chain_head(struct sdpageiter *i, int64_t pos)
{
	/* find first non-duplicate key */
	while (pos >= 0) {
		struct sdv *v = sd_pagev(i->page, pos);
		if (sslikely(! (v->flags & SVDUP))) {
			i->pos = pos;
			i->v = v;
			return;
		}
		pos--;
	}
	sd_pageiter_end(i);
}

static inline void
sd_pageiter_chain_next(struct sdpageiter *i)
{
	/* skip to next duplicate chain */
	int64_t pos = i->pos + 1;
	while (pos < i->page->h->count) {
		struct sdv *v = sd_pagev(i->page, pos);
		if (sslikely(! (v->flags & SVDUP))) {
			i->pos = pos;
			i->v = v;
			return;
		}
		pos++;
	}
	sd_pageiter_end(i);
}

static inline int
sd_pageiter_gt(struct sdpageiter *i, int e)
{
	if (i->key == NULL) {
		i->pos = 0;
		i->v = sd_pagev(i->page, i->pos);
		return 0;
	}
	int64_t pos = sd_pageiter_search(i);
	if (ssunlikely(pos >= i->page->h->count))
		pos = i->page->h->count - 1;
	sd_pageiter_chain_head(i, pos);
	if (i->v == NULL)
		return 0;
	int rc = sd_pageiter_cmp(i, i->r, i->v);
	int match = rc == 0;
	switch (rc) {
		case  0:
			if (e) {
				break;
			}
		case -1:
			sd_pageiter_chain_next(i);
			break;
	}
	return match;
}

static inline int
sd_pageiter_lt(struct sdpageiter *i, int e)
{
	if (i->key == NULL) {
		sd_pageiter_chain_head(i, i->page->h->count - 1);
		return 0;
	}
	int64_t pos = sd_pageiter_search(i);
	if (ssunlikely(pos >= i->page->h->count))
		pos = i->page->h->count - 1;
	sd_pageiter_chain_head(i, pos);
	if (i->v == NULL)
		return 0;
	int rc = sd_pageiter_cmp(i, i->r, i->v);
	int match = rc == 0;
	switch (rc) {
		case 0:
			if (e) {
				break;
			}
		case 1:
			sd_pageiter_chain_head(i, i->pos - 1);
			break;
	}
	return match;
}

static inline int
sd_pageiter_open(struct ssiter *i, struct runtime *r, struct ssbuf *xfbuf, struct sdpage *page, enum ssorder o,
                 void *key, int keysize)
{
	struct sdpageiter *pi = (struct sdpageiter*)i->priv;
	pi->r       = r;
	pi->page    = page;
	pi->xfbuf   = xfbuf;
	pi->order   = o;
	pi->key     = key;
	pi->keysize = keysize;
	pi->v       = NULL;
	pi->pos     = 0;
	if (ssunlikely(pi->page->h->count == 0)) {
		sd_pageiter_end(pi);
		return 0;
	}
	int rc = 0;
	switch (pi->order) {
	case SS_GT:  rc = sd_pageiter_gt(pi, 0);
		break;
	case SS_GTE: rc = sd_pageiter_gt(pi, 1);
		break;
	case SS_LT:  rc = sd_pageiter_lt(pi, 0);
		break;
	case SS_LTE: rc = sd_pageiter_lt(pi, 1);
		break;
	default: assert(0);
	}
	sd_pageiter_result(pi);
	return rc;
}

static inline void
sd_pageiter_close(struct ssiter *i ssunused)
{ }

static inline int
sd_pageiter_has(struct ssiter *i)
{
	struct sdpageiter *pi = (struct sdpageiter*)i->priv;
	return pi->v != NULL;
}

static inline void*
sd_pageiter_of(struct ssiter *i)
{
	struct sdpageiter *pi = (struct sdpageiter*)i->priv;
	if (ssunlikely(pi->v == NULL))
		return NULL;
	return &pi->current;
}

static inline void
sd_pageiter_next(struct ssiter *i)
{
	struct sdpageiter *pi = (struct sdpageiter*)i->priv;
	if (pi->v == NULL)
		return;
	switch (pi->order) {
	case SS_GTE:
	case SS_GT:
		pi->pos++;
		if (ssunlikely(pi->pos >= pi->page->h->count)) {
			sd_pageiter_end(pi);
			return;
		}
		pi->v = sd_pagev(pi->page, pi->pos);
		break;
	case SS_LT:
	case SS_LTE: {
		/* key (dup) (dup) key (eof) */
		struct sdv *v;
		int64_t pos = pi->pos + 1;
		if (pos < pi->page->h->count) {
			v = sd_pagev(pi->page, pos);
			if (v->flags & SVDUP) {
				pi->pos = pos;
				pi->v   = v;
				break;
			}
		}
		/* skip current chain and position to
		 * the previous one */
		sd_pageiter_chain_head(pi, pi->pos);
		sd_pageiter_chain_head(pi, pi->pos - 1);
		break;
	}
	default: assert(0);
	}
	sd_pageiter_result(pi);
}

static struct ssiterif sd_pageiter;

struct sdbuildref {
	uint32_t m, msize;
	uint32_t v, vsize;
	uint32_t k, ksize;
	uint32_t c, csize;
} sspacked;

struct sdbuild {
	struct ssbuf list, m, v, k, c;
	struct ssfilterif *compress_if;
	int timestamp;
	int compress_dup;
	int compress;
	int crc;
	uint32_t vmax;
	uint32_t n;
	struct ssht tracker;
};

static void sd_buildinit(struct sdbuild*);
static void sd_buildfree(struct sdbuild*, struct runtime*);
static void sd_buildreset(struct sdbuild*, struct runtime*);
static void sd_buildgc(struct sdbuild*, struct runtime*, int);

static inline struct sdbuildref*
sd_buildref(struct sdbuild *b) {
	return ss_bufat(&b->list, sizeof(struct sdbuildref), b->n);
}

static inline struct sdpageheader*
sd_buildheader(struct sdbuild *b) {
	return (struct sdpageheader*)(b->m.s + sd_buildref(b)->m);
}

static inline struct sdv*
sd_buildmin(struct sdbuild *b) {
	return (struct sdv*)((char*)sd_buildheader(b) + sizeof(struct sdpageheader));
}

static inline char*
sd_buildminkey(struct sdbuild *b) {
	struct sdbuildref *r = sd_buildref(b);
	return b->v.s + r->v + sd_buildmin(b)->offset;
}

static inline struct sdv*
sd_buildmax(struct sdbuild *b) {
	struct sdpageheader *h = sd_buildheader(b);
	return (struct sdv*)((char*)h + sizeof(struct sdpageheader) + sizeof(struct sdv) * (h->count - 1));
}

static inline char*
sd_buildmaxkey(struct sdbuild *b) {
	struct sdbuildref *r = sd_buildref(b);
	return b->v.s + r->v + sd_buildmax(b)->offset;
}

static int sd_buildbegin(struct sdbuild*, struct runtime*, int, int, int, int, struct ssfilterif*);
static int sd_buildend(struct sdbuild*, struct runtime*);
static int sd_buildcommit(struct sdbuild*, struct runtime*);
static int sd_buildadd(struct sdbuild*, struct runtime*, struct sv*, uint32_t);

#define SD_INDEXEXT_AMQF 1

struct sdindexheader {
	uint32_t  crc;
	struct srversion version;
	struct sdid      id;
	uint64_t  offset;
	uint32_t  size;
	uint32_t  sizevmax;
	uint32_t  count;
	uint32_t  keys;
	uint64_t  total;
	uint64_t  totalorigin;
	uint32_t  tsmin;
	uint64_t  lsnmin;
	uint64_t  lsnmax;
	uint32_t  dupkeys;
	uint64_t  dupmin;
	uint32_t  extension;
	uint8_t   extensions;
	char      reserve[31];
} sspacked;

struct sdindexamqf {
	uint8_t  q, r;
	uint32_t entries;
	uint32_t size;
	uint64_t table[];
} sspacked;

struct sdindexpage {
	uint64_t offset;
	uint32_t offsetindex;
	uint32_t size;
	uint32_t sizeorigin;
	uint16_t sizemin;
	uint16_t sizemax;
	uint64_t lsnmin;
	uint64_t lsnmax;
} sspacked;

struct sdindex {
	struct ssbuf i, v;
	struct sdindexheader *h;
};

static inline char*
sd_indexpage_min(struct sdindex *i, struct sdindexpage *p) {
	return (char*)i->i.s + sizeof(struct sdindexheader) +
	             (i->h->count * sizeof(struct sdindexpage)) + p->offsetindex;
}

static inline char*
sd_indexpage_max(struct sdindex *i, struct sdindexpage *p) {
	return sd_indexpage_min(i, p) + p->sizemin;
}

static inline void
sd_indexinit(struct sdindex *i) {
	ss_bufinit(&i->i);
	ss_bufinit(&i->v);
	i->h = NULL;
}

static inline void
sd_indexfree(struct sdindex *i, struct runtime *r) {
	ss_buffree(&i->i, r->a);
	ss_buffree(&i->v, r->a);
}

static inline struct sdindexheader*
sd_indexheader(struct sdindex *i) {
	return (struct sdindexheader*)(i->i.s);
}

static inline struct sdindexpage*
sd_indexpage(struct sdindex *i, uint32_t pos)
{
	assert(pos < i->h->count);
	char *p = (char*)ss_bufat(&i->i, sizeof(struct sdindexpage), pos);
   	p += sizeof(struct sdindexheader);
	return (struct sdindexpage*)p;
}

static inline struct sdindexpage*
sd_indexmin(struct sdindex *i) {
	return sd_indexpage(i, 0);
}

static inline struct sdindexpage*
sd_indexmax(struct sdindex *i) {
	return sd_indexpage(i, i->h->count - 1);
}

static inline uint32_t
sd_indexkeys(struct sdindex *i)
{
	if (ssunlikely(i->i.s == NULL))
		return 0;
	return sd_indexheader(i)->keys;
}

static inline uint32_t
sd_indextotal(struct sdindex *i)
{
	if (ssunlikely(i->i.s == NULL))
		return 0;
	return sd_indexheader(i)->total;
}

static inline uint32_t
sd_indexsize_ext(struct sdindexheader *h)
{
	return sizeof(struct sdindexheader) + h->size + h->extension;
}

static inline struct sdindexamqf*
sd_indexamqf(struct sdindex *i) {
	struct sdindexheader *h = sd_indexheader(i);
	assert(h->extensions & SD_INDEXEXT_AMQF);
	return (struct sdindexamqf*)(i->i.s + sizeof(struct sdindexheader) + h->size);
}

static int sd_indexbegin(struct sdindex*, struct runtime*);
static int sd_indexcommit(struct sdindex*, struct runtime*, struct sdid*, struct ssqf*, uint64_t);
static int sd_indexadd(struct sdindex*, struct runtime*, struct sdbuild*, uint64_t);
static int sd_indexcopy(struct sdindex*, struct runtime*, struct sdindexheader*);

struct sdindexiter {
	struct sdindex *index;
	struct sdindexpage *v;
	int pos;
	enum ssorder cmp;
	void *key;
	int keysize;
	struct runtime *r;
} sspacked;

static inline int
sd_indexiter_route(struct sdindexiter *i)
{
	int begin = 0;
	int end = i->index->h->count - 1;
	while (begin != end) {
		int mid = begin + (end - begin) / 2;
		struct sdindexpage *page = sd_indexpage(i->index, mid);
		int rc = sf_compare(i->r->scheme,
		                    sd_indexpage_max(i->index, page),
		                    page->sizemax,
		                    i->key,
		                    i->keysize);
		if (rc < 0) {
			begin = mid + 1;
		} else {
			/* rc >= 0 */
			end = mid;
		}
	}
	if (ssunlikely(end >= (int)i->index->h->count))
		end = i->index->h->count - 1;
	return end;
}

static inline int
sd_indexiter_open(struct ssiter *i, struct runtime *r, struct sdindex *index, enum ssorder o, void *key, int keysize)
{
	struct sdindexiter *ii = (struct sdindexiter*)i->priv;
	ii->r       = r;
	ii->index   = index;
	ii->cmp     = o;
	ii->key     = key;
	ii->keysize = keysize;
	ii->v       = NULL;
	ii->pos     = 0;
	if (ssunlikely(ii->index->h->count == 1)) {
		/* skip bootstrap node  */
		if (ii->index->h->lsnmin == UINT64_MAX &&
		    ii->index->h->lsnmax == 0)
			return 0;
	}
	if (ii->key == NULL) {
		switch (ii->cmp) {
		case SS_LT:
		case SS_LTE: ii->pos = ii->index->h->count - 1;
			break;
		case SS_GT:
		case SS_GTE: ii->pos = 0;
			break;
		default:
			assert(0);
		}
		ii->v = sd_indexpage(ii->index, ii->pos);
		return 0;
	}
	if (sslikely(ii->index->h->count > 1))
		ii->pos = sd_indexiter_route(ii);

	struct sdindexpage *p = sd_indexpage(ii->index, ii->pos);
	int rc;
	switch (ii->cmp) {
	case SS_LTE:
	case SS_LT:
		rc = sf_compare(ii->r->scheme, sd_indexpage_min(ii->index, p),
		                p->sizemin, ii->key, ii->keysize);
		if (rc ==  1 || (rc == 0 && ii->cmp == SS_LT))
			ii->pos--;
		break;
	case SS_GTE:
	case SS_GT:
		rc = sf_compare(ii->r->scheme, sd_indexpage_max(ii->index, p),
		                p->sizemax, ii->key, ii->keysize);
		if (rc == -1 || (rc == 0 && ii->cmp == SS_GT))
			ii->pos++;
		break;
	default: assert(0);
	}
	if (ssunlikely(ii->pos == -1 ||
	               ii->pos >= (int)ii->index->h->count))
		return 0;
	ii->v = sd_indexpage(ii->index, ii->pos);
	return 0;
}

static inline void
sd_indexiter_close(struct ssiter *i ssunused)
{ }

static inline int
sd_indexiter_has(struct ssiter *i)
{
	struct sdindexiter *ii = (struct sdindexiter*)i->priv;
	return ii->v != NULL;
}

static inline void*
sd_indexiter_of(struct ssiter *i)
{
	struct sdindexiter *ii = (struct sdindexiter*)i->priv;
	return ii->v;
}

static inline void
sd_indexiter_next(struct ssiter *i)
{
	struct sdindexiter *ii = (struct sdindexiter*)i->priv;
	switch (ii->cmp) {
	case SS_LT:
	case SS_LTE: ii->pos--;
		break;
	case SS_GT:
	case SS_GTE: ii->pos++;
		break;
	default:
		assert(0);
		break;
	}
	if (ssunlikely(ii->pos < 0))
		ii->v = NULL;
	else
	if (ssunlikely(ii->pos >= (int)ii->index->h->count))
		ii->v = NULL;
	else
		ii->v = sd_indexpage(ii->index, ii->pos);
}

static struct ssiterif sd_indexiter;

#define SD_SEALED 1

struct sdseal {
	uint32_t  crc;
	struct srversion version;
	uint8_t   flags;
	uint32_t  index_crc;
	uint64_t  index_offset;
} sspacked;

static inline void
sd_sealset_open(struct sdseal *s)
{
	sr_version_storage(&s->version);
	s->flags = 0;
	s->index_crc = 0;
	s->index_offset = 0;
	s->crc = ss_crcs(s, sizeof(struct sdseal), 0);
}

static inline void
sd_sealset_close(struct sdseal *s, struct sdindexheader *h)
{
	sr_version_storage(&s->version);
	s->flags = SD_SEALED;
	s->index_crc = h->crc;
	s->index_offset = h->offset;
	s->crc = ss_crcs(s, sizeof(struct sdseal), 0);
}

static inline int
sd_sealvalidate(struct sdseal *s, struct sdindexheader *h)
{
	uint32_t crc = ss_crcs(s, sizeof(struct sdseal), 0);
	if (ssunlikely(s->crc != crc))
		return -1;
	if (ssunlikely(h->crc != s->index_crc))
		return -1;
	if (ssunlikely(h->offset != s->index_offset))
		return -1;
	if (ssunlikely(! sr_versionstorage_check(&s->version)))
		return -1;
	if (ssunlikely(s->flags != SD_SEALED))
		return -1;
	return 0;
}

struct sdcbuf {
	struct ssbuf a; /* decompression */
	struct ssbuf b; /* transformation */
	struct ssiter index_iter;
	struct ssiter page_iter;
	struct sdcbuf *next;
};

struct sdc {
	struct sdbuild build;
	struct ssqf qf;
	struct svupsert upsert;
	struct ssbuf a;        /* result */
	struct ssbuf b;        /* redistribute buffer */
	struct ssbuf c;        /* file buffer */
	struct ssbuf d;        /* page read buffer */
	struct sdcbuf *head;   /* compression buffer list */
	int count;
};

static inline void
sd_cinit(struct sdc *sc)
{
	sv_upsertinit(&sc->upsert);
	sd_buildinit(&sc->build);
	ss_qfinit(&sc->qf);
	ss_bufinit(&sc->a);
	ss_bufinit(&sc->b);
	ss_bufinit(&sc->c);
	ss_bufinit(&sc->d);
	sc->count = 0;
	sc->head = NULL;
}

static inline void
sd_cfree(struct sdc *sc, struct runtime *r)
{
	sd_buildfree(&sc->build, r);
	ss_qffree(&sc->qf, r->a);
	sv_upsertfree(&sc->upsert, r);
	ss_buffree(&sc->a, r->a);
	ss_buffree(&sc->b, r->a);
	ss_buffree(&sc->c, r->a);
	ss_buffree(&sc->d, r->a);
	struct sdcbuf *b = sc->head;
	struct sdcbuf *next;
	while (b) {
		next = b->next;
		ss_buffree(&b->a, r->a);
		ss_buffree(&b->b, r->a);
		ss_free(r->a, b);
		b = next;
	}
}

static inline void
sd_cgc(struct sdc *sc, struct runtime *r, int wm)
{
	sd_buildgc(&sc->build, r, wm);
	ss_qfgc(&sc->qf, r->a, wm);
	sv_upsertgc(&sc->upsert, r, 600, 512);
	ss_bufgc(&sc->a, r->a, wm);
	ss_bufgc(&sc->b, r->a, wm);
	ss_bufgc(&sc->c, r->a, wm);
	ss_bufgc(&sc->d, r->a, wm);
	struct sdcbuf *b = sc->head;
	while (b) {
		ss_bufgc(&b->a, r->a, wm);
		ss_bufgc(&b->b, r->a, wm);
		b = b->next;
	}
}

static inline int
sd_censure(struct sdc *c, struct runtime *r, int count)
{
	if (c->count < count) {
		while (count-- >= 0) {
			struct sdcbuf *b = ss_malloc(r->a, sizeof(struct sdcbuf));
			if (ssunlikely(b == NULL))
				return -1;
			ss_bufinit(&b->a);
			ss_bufinit(&b->b);
			b->next = c->head;
			c->head = b;
			c->count++;
		}
	}
	return 0;
}

struct sdmergeconf {
	uint32_t    write;
	uint32_t    stream;
	uint64_t    size_stream;
	uint64_t    size_node;
	uint32_t    size_page;
	uint32_t    checksum;
	uint32_t    expire;
	uint32_t    timestamp;
	uint32_t    compression_key;
	uint32_t    compression;
	struct ssfilterif *compression_if;
	uint32_t    amqf;
	uint64_t    vlsn;
	uint64_t    vlsn_lru;
	uint32_t    save_delete;
	uint32_t    save_upsert;
};

struct sdmerge {
	struct sdindex     index;
	struct ssiter      *merge;
	struct ssiter      i;
	struct sdmergeconf *conf;
	struct runtime          *r;
	struct sdbuild     *build;
	struct ssqf        *qf;
	uint64_t    processed;
	uint64_t    current;
	uint64_t    limit;
	int         resume;
};

static int
sd_mergeinit(struct sdmerge*, struct runtime*, struct ssiter*, struct sdbuild*, struct ssqf*, struct svupsert*,
	     struct sdmergeconf*);
static int sd_mergefree(struct sdmerge*);
static int sd_merge(struct sdmerge*);
static int sd_mergepage(struct sdmerge*, uint64_t);
static int sd_mergecommit(struct sdmerge*, struct sdid*, uint64_t);

struct sdreadarg {
	struct sdindex    *index;
	struct ssbuf      *buf;
	struct ssbuf      *buf_xf;
	struct ssbuf      *buf_read;
	struct ssiter     *index_iter;
	struct ssiter     *page_iter;
	struct ssmmap     *mmap;
	struct ssblob     *memory;
	struct ssfile     *file;
	enum ssorder     o;
	int         has;
	uint64_t    has_vlsn;
	int         use_memory;
	int         use_mmap;
	int         use_mmap_copy;
	int         use_compression;
	struct ssfilterif *compression_if;
	struct runtime         *r;
};

struct sdread {
	struct sdreadarg ra;
	struct sdindexpage *ref;
	struct sdpage page;
	int reads;
} sspacked;

static inline int
sd_read_page(struct sdread *i, struct sdindexpage *ref)
{
	struct sdreadarg *arg = &i->ra;
	struct runtime *r = arg->r;

	ss_bufreset(arg->buf);
	int rc = ss_bufensure(arg->buf, r->a, ref->sizeorigin);
	if (ssunlikely(rc == -1))
		return sr_oom(r->e);
	ss_bufreset(arg->buf_xf);
	rc = ss_bufensure(arg->buf_xf, r->a, arg->index->h->sizevmax);
	if (ssunlikely(rc == -1))
		return sr_oom(r->e);

	i->reads++;

	/* in-memory mode only offsets */
	uint64_t branch_start_offset =
		arg->index->h->offset - arg->index->h->total - sizeof(struct sdseal);
	uint64_t branch_ref_offset =
		ref->offset - branch_start_offset;

	/* compression */
	if (arg->use_compression)
	{
		char *page_pointer;
		if (arg->use_memory) {
			page_pointer = arg->memory->map.p + branch_ref_offset;
		} else
		if (arg->use_mmap) {
			page_pointer = arg->mmap->p + ref->offset;
		} else {
			ss_bufreset(arg->buf_read);
			rc = ss_bufensure(arg->buf_read, r->a, ref->size);
			if (ssunlikely(rc == -1))
				return sr_oom(r->e);
			rc = ss_filepread(arg->file, ref->offset, arg->buf_read->s, ref->size);
			if (ssunlikely(rc == -1)) {
				sr_error(r->e, "db file '%s' read error: %s",
				         ss_pathof(&arg->file->path),
				         strerror(errno));
				return -1;
			}
			ss_bufadvance(arg->buf_read, ref->size);
			page_pointer = arg->buf_read->s;
		}

		/* copy header */
		memcpy(arg->buf->p, page_pointer, sizeof(struct sdpageheader));
		ss_bufadvance(arg->buf, sizeof(struct sdpageheader));

		/* decompression */
		struct ssfilter f;
		rc = ss_filterinit(&f, (struct ssfilterif*)arg->compression_if, r->a, SS_FOUTPUT);
		if (ssunlikely(rc == -1)) {
			sr_error(r->e, "db file '%s' decompression error",
			         ss_pathof(&arg->file->path));
			return -1;
		}
		int size = ref->size - sizeof(struct sdpageheader);
		rc = ss_filternext(&f, arg->buf, page_pointer + sizeof(struct sdpageheader), size);
		if (ssunlikely(rc == -1)) {
			sr_error(r->e, "db file '%s' decompression error",
			         ss_pathof(&arg->file->path));
			return -1;
		}
		ss_filterfree(&f);
		sd_pageinit(&i->page, (struct sdpageheader*)arg->buf->s);
		return 0;
	}

	/* in-memory mode */
	if (arg->use_memory) {
		sd_pageinit(&i->page, (struct sdpageheader*)(arg->memory->map.p + branch_ref_offset));
		return 0;
	}

	/* mmap */
	if (arg->use_mmap) {
		if (arg->use_mmap_copy) {
			memcpy(arg->buf->s, arg->mmap->p + ref->offset, ref->sizeorigin);
			sd_pageinit(&i->page, (struct sdpageheader*)(arg->buf->s));
		} else {
			sd_pageinit(&i->page, (struct sdpageheader*)(arg->mmap->p + ref->offset));
		}
		return 0;
	}

	/* default */
	rc = ss_filepread(arg->file, ref->offset, arg->buf->s, ref->sizeorigin);
	if (ssunlikely(rc == -1)) {
		sr_error(r->e, "db file '%s' read error: %s",
		         ss_pathof(&arg->file->path),
		         strerror(errno));
		return -1;
	}
	ss_bufadvance(arg->buf, ref->sizeorigin);
	sd_pageinit(&i->page, (struct sdpageheader*)(arg->buf->s));
	return 0;
}

static inline int
sd_read_openpage(struct sdread *i, void *key, int keysize)
{
	struct sdreadarg *arg = &i->ra;
	assert(i->ref != NULL);
	int rc = sd_read_page(i, i->ref);
	if (ssunlikely(rc == -1))
		return -1;
	ss_iterinit(sd_pageiter, arg->page_iter);
	return ss_iteropen(sd_pageiter, arg->page_iter, arg->r,
	                   arg->buf_xf,
	                   &i->page, arg->o, key, keysize);
}

static inline void
sd_read_next(struct ssiter*);

static inline int
sd_read_open(struct ssiter *iptr, struct sdreadarg *arg, void *key, int keysize)
{
	struct sdread *i = (struct sdread*)iptr->priv;
	i->reads = 0;
	i->ra = *arg;
	ss_iterinit(sd_indexiter, arg->index_iter);
	ss_iteropen(sd_indexiter, arg->index_iter, arg->r, arg->index,
	            arg->o, key, keysize);
	i->ref = ss_iterof(sd_indexiter, arg->index_iter);
	if (i->ref == NULL)
		return 0;
	if (arg->has) {
		assert(arg->o == SS_GTE);
		if (sslikely(i->ref->lsnmax <= arg->has_vlsn)) {
			i->ref = NULL;
			return 0;
		}
	}
	int rc = sd_read_openpage(i, key, keysize);
	if (ssunlikely(rc == -1)) {
		i->ref = NULL;
		return -1;
	}
	if (ssunlikely(! ss_iterhas(sd_pageiter, i->ra.page_iter))) {
		sd_read_next(iptr);
		rc = 0;
	}
	return rc;
}

static inline void
sd_read_close(struct ssiter *iptr)
{
	struct sdread *i = (struct sdread*)iptr->priv;
	i->ref = NULL;
}

static inline int
sd_read_has(struct ssiter *iptr)
{
	struct sdread *i = (struct sdread*)iptr->priv;
	if (ssunlikely(i->ref == NULL))
		return 0;
	return ss_iterhas(sd_pageiter, i->ra.page_iter);
}

static inline void*
sd_read_of(struct ssiter *iptr)
{
	struct sdread *i = (struct sdread*)iptr->priv;
	if (ssunlikely(i->ref == NULL))
		return NULL;
	return ss_iterof(sd_pageiter, i->ra.page_iter);
}

static inline void
sd_read_next(struct ssiter *iptr)
{
	struct sdread *i = (struct sdread*)iptr->priv;
	if (ssunlikely(i->ref == NULL))
		return;
	ss_iternext(sd_pageiter, i->ra.page_iter);
retry:
	if (sslikely(ss_iterhas(sd_pageiter, i->ra.page_iter)))
		return;
	ss_iternext(sd_indexiter, i->ra.index_iter);
	i->ref = ss_iterof(sd_indexiter, i->ra.index_iter);
	if (i->ref == NULL)
		return;
	int rc = sd_read_openpage(i, NULL, 0);
	if (ssunlikely(rc == -1)) {
		i->ref = NULL;
		return;
	}
	goto retry;
}

static inline int
sd_read_stat(struct ssiter *iptr)
{
	struct sdread *i = (struct sdread*)iptr->priv;
	return i->reads;
}

static struct ssiterif sd_read;

static int sd_writeseal(struct runtime*, struct ssfile*, struct ssblob*);
static int sd_writepage(struct runtime*, struct ssfile*, struct ssblob*, struct sdbuild*);
static int sd_writeindex(struct runtime*, struct ssfile*, struct ssblob*, struct sdindex*);
static int sd_seal(struct runtime*, struct ssfile*, struct ssblob*, struct sdindex*, uint64_t);

static int sd_recover_open(struct ssiter*, struct runtime*, struct ssfile*);
static int sd_recover_complete(struct ssiter*);

static struct ssiterif sd_recover;

struct sdschemeheader {
	uint32_t crc;
	uint32_t size;
	uint32_t count;
} sspacked;

struct sdschemeopt {
	uint8_t  type;
	uint8_t  id;
	uint32_t size;
} sspacked;

struct sdscheme {
	struct ssbuf buf;
};

static inline void
sd_schemeinit(struct sdscheme *c) {
	ss_bufinit(&c->buf);
}

static inline void
sd_schemefree(struct sdscheme *c, struct runtime *r) {
	ss_buffree(&c->buf, r->a);
}

static inline char*
sd_schemesz(struct sdschemeopt *o) {
	assert(o->type == SS_STRING);
	return (char*)o + sizeof(struct sdschemeopt);
}

static inline uint32_t
sd_schemeu32(struct sdschemeopt *o) {
	assert(o->type == SS_U32);
	return load_u32((char*)o + sizeof(struct sdschemeopt));
}

static inline uint64_t
sd_schemeu64(struct sdschemeopt *o) {
	assert(o->type == SS_U64);
	return load_u64((char*)o + sizeof(struct sdschemeopt));
}

static int sd_schemebegin(struct sdscheme*, struct runtime*);
static int sd_schemeadd(struct sdscheme*, struct runtime*, uint8_t, enum sstype, void*, uint32_t);
static int sd_schemecommit(struct sdscheme*);
static int sd_schemewrite(struct sdscheme*, struct runtime*, char*, int);
static int sd_schemerecover(struct sdscheme*, struct runtime*, char*);

struct sdschemeiter {
	struct sdscheme *c;
	char *p;
} sspacked;

static inline int
sd_schemeiter_open(struct ssiter *i, struct runtime *r, struct sdscheme *c, int validate)
{
	struct sdschemeiter *ci = (struct sdschemeiter*)i->priv;
	ci->c = c;
	ci->p = NULL;
	if (validate) {
		struct sdschemeheader *h = (struct sdschemeheader*)c->buf.s;
		uint32_t crc = ss_crcs(h, ss_bufused(&c->buf), 0);
		if (h->crc != crc) {
			sr_malfunction(r->e, "%s", "scheme file corrupted");
			return -1;
		}
	}
	ci->p = c->buf.s + sizeof(struct sdschemeheader);
	return 0;
}

static inline void
sd_schemeiter_close(struct ssiter *i ssunused)
{
	struct sdschemeiter *ci = (struct sdschemeiter*)i->priv;
	(void)ci;
}

static inline int
sd_schemeiter_has(struct ssiter *i)
{
	struct sdschemeiter *ci = (struct sdschemeiter*)i->priv;
	return ci->p < ci->c->buf.p;
}

static inline void*
sd_schemeiter_of(struct ssiter *i)
{
	struct sdschemeiter *ci = (struct sdschemeiter*)i->priv;
	if (ssunlikely(ci->p >= ci->c->buf.p))
		return NULL;
	return ci->p;
}

static inline void
sd_schemeiter_next(struct ssiter *i)
{
	struct sdschemeiter *ci = (struct sdschemeiter*)i->priv;
	if (ssunlikely(ci->p >= ci->c->buf.p))
		return;
	struct sdschemeopt *o = (struct sdschemeopt*)ci->p;
	ci->p = (char*)o + sizeof(struct sdschemeopt) + o->size;
}

static struct ssiterif sd_schemeiter;

struct sdsnapshotheader {
	uint32_t crc;
	uint32_t size;
	uint32_t nodes;
	uint64_t lru_v;
	uint64_t lru_steps;
	uint64_t lru_intr_lsn;
	uint64_t lru_intr_sum;
	uint64_t read_disk;
	uint64_t read_cache;
	uint64_t reserve[4];
} sspacked;

struct sdsnapshotnode {
	uint32_t crc;
	uint64_t id;
	uint64_t size_file;
	uint32_t size;
	uint32_t branch_count;
	uint64_t temperature_reads;
	uint64_t reserve[4];
	/* struct sdindexheader[] */
} sspacked;

struct sdsnapshot {
	uint32_t current;
	struct ssbuf buf;
};

static inline void
sd_snapshot_init(struct sdsnapshot *s)
{
	s->current = 0;
	ss_bufinit(&s->buf);
}

static inline void
sd_snapshot_free(struct sdsnapshot *s, struct runtime *r)
{
	ss_buffree(&s->buf, r->a);
}

static inline struct sdsnapshotheader*
sd_snapshot_header(struct sdsnapshot *s) {
	return (struct sdsnapshotheader*)s->buf.s;
}

static inline int
sd_snapshot_is(struct sdsnapshot *s) {
	return s->buf.s != NULL;
}

static int sd_snapshot_begin(struct sdsnapshot*, struct runtime*);
static int sd_snapshot_add(struct sdsnapshot*, struct runtime*, uint64_t, uint64_t, uint32_t, uint64_t);
static int sd_snapshot_addbranch(struct sdsnapshot*, struct runtime*, struct sdindexheader*);
static int sd_snapshot_commit(struct sdsnapshot*, uint64_t, uint64_t, uint64_t, uint64_t,
			      uint64_t, uint64_t);

struct sdsnapshotiter {
	struct sdsnapshot *s;
	struct sdsnapshotnode *n;
	uint32_t npos;
} sspacked;

static inline int
sd_snapshotiter_open(struct ssiter *i, struct runtime *r, struct sdsnapshot *s)
{
	struct sdsnapshotiter *si = (struct sdsnapshotiter*)i->priv;
	si->s = s;
	si->n = NULL;
	si->npos = 0;
	if (ssunlikely(ss_bufused(&s->buf) < (int)sizeof(struct sdsnapshotheader)))
		goto error;
	struct sdsnapshotheader *h = (struct sdsnapshotheader*)s->buf.s;
	uint32_t crc = ss_crcs(h, sizeof(*h), 0);
	if (h->crc != crc)
		goto error;
	if (ssunlikely((int)h->size != ss_bufused(&s->buf)))
		goto error;
	si->n = (struct sdsnapshotnode*)(s->buf.s + sizeof(struct sdsnapshotheader));
	return 0;
error:
	sr_malfunction(r->e, "%s", "snapshot file corrupted");
	return -1;
}

static inline void
sd_snapshotiter_close(struct ssiter *i ssunused)
{ }

static inline int
sd_snapshotiter_has(struct ssiter *i)
{
	struct sdsnapshotiter *si = (struct sdsnapshotiter*)i->priv;
	return si->n != NULL;
}

static inline void*
sd_snapshotiter_of(struct ssiter *i)
{
	struct sdsnapshotiter *si = (struct sdsnapshotiter*)i->priv;
	if (ssunlikely(si->n == NULL))
		return NULL;
	return si->n;
}

static inline void
sd_snapshotiter_next(struct ssiter *i)
{
	struct sdsnapshotiter *si = (struct sdsnapshotiter*)i->priv;
	if (ssunlikely(si->n == NULL))
		return;
	si->npos++;
	struct sdsnapshotheader *h = (struct sdsnapshotheader*)si->s->buf.s;
	if (si->npos < h->nodes) {
		si->n = (struct sdsnapshotnode*)((char*)si->n + sizeof(struct sdsnapshotnode) + si->n->size);
		return;
	}
	si->n = NULL;
}

static struct ssiterif sd_snapshotiter;

static void sd_buildinit(struct sdbuild *b)
{
	memset(&b->tracker, 0, sizeof(b->tracker));
	ss_bufinit(&b->list);
	ss_bufinit(&b->m);
	ss_bufinit(&b->v);
	ss_bufinit(&b->c);
	ss_bufinit(&b->k);
	b->n = 0;
	b->compress = 0;
	b->compress_dup = 0;
	b->compress_if = NULL;
	b->crc = 0;
	b->vmax = 0;
}

static inline void
sd_buildfree_tracker(struct sdbuild *b, struct runtime *r)
{
	if (b->tracker.count == 0)
		return;
	int i = 0;
	for (; i < b->tracker.size; i++) {
		if (b->tracker.i[i] == NULL)
			continue;
		ss_free(r->a, b->tracker.i[i]);
		b->tracker.i[i] = NULL;
	}
	b->tracker.count = 0;
}

static void sd_buildfree(struct sdbuild *b, struct runtime *r)
{
	sd_buildfree_tracker(b, r);
	ss_htfree(&b->tracker, r->a);
	ss_buffree(&b->list, r->a);
	ss_buffree(&b->m, r->a);
	ss_buffree(&b->v, r->a);
	ss_buffree(&b->c, r->a);
	ss_buffree(&b->k, r->a);
}

static void sd_buildreset(struct sdbuild *b, struct runtime *r)
{
	sd_buildfree_tracker(b, r);
	ss_htreset(&b->tracker);
	ss_bufreset(&b->list);
	ss_bufreset(&b->m);
	ss_bufreset(&b->v);
	ss_bufreset(&b->c);
	ss_bufreset(&b->k);
	b->n = 0;
	b->vmax = 0;
}

static void sd_buildgc(struct sdbuild *b, struct runtime *r, int wm)
{
	sd_buildfree_tracker(b, r);
	ss_htreset(&b->tracker);
	ss_bufgc(&b->list, r->a, wm);
	ss_bufgc(&b->m, r->a, wm);
	ss_bufgc(&b->v, r->a, wm);
	ss_bufgc(&b->c, r->a, wm);
	ss_bufgc(&b->k, r->a, wm);
	b->n = 0;
	b->vmax = 0;
}

static int
sd_buildbegin(struct sdbuild *b, struct runtime *r, int crc,
	      int timestamp,
	      int compress_dup,
	      int compress,
	      struct ssfilterif *compress_if)
{
	b->crc = crc;
	b->compress_dup = compress_dup;
	b->compress = compress;
	b->compress_if = compress_if;
	b->timestamp = timestamp;
	int rc;
	if (compress_dup && b->tracker.size == 0) {
		rc = ss_htinit(&b->tracker, r->a, 32768);
		if (ssunlikely(rc == -1))
			return sr_oom(r->e);
	}
	rc = ss_bufensure(&b->list, r->a, sizeof(struct sdbuildref));
	if (ssunlikely(rc == -1))
		return sr_oom(r->e);
	struct sdbuildref *ref =
		(struct sdbuildref*)ss_bufat(&b->list, sizeof(struct sdbuildref), b->n);
	ref->m     = ss_bufused(&b->m);
	ref->msize = 0;
	ref->v     = ss_bufused(&b->v);
	ref->vsize = 0;
	ref->k     = ss_bufused(&b->k);
	ref->ksize = 0;
	ref->c     = ss_bufused(&b->c);
	ref->csize = 0;
	rc = ss_bufensure(&b->m, r->a, sizeof(struct sdpageheader));
	if (ssunlikely(rc == -1))
		return sr_oom(r->e);
	struct sdpageheader *h = sd_buildheader(b);
	memset(h, 0, sizeof(*h));
	h->lsnmin    = UINT64_MAX;
	h->lsnmindup = UINT64_MAX;
	h->tsmin     = UINT32_MAX;
	h->reserve   = 0;
	ss_bufadvance(&b->list, sizeof(struct sdbuildref));
	ss_bufadvance(&b->m, sizeof(struct sdpageheader));
	return 0;
}

struct sdbuildkey {
	struct sshtnode node;
	uint32_t offset;
	uint32_t offsetstart;
	uint32_t size;
};

ss_htsearch(sd_buildsearch,
            (sscast(t->i[pos], struct sdbuildkey, node)->node.hash == hash) &&
            (sscast(t->i[pos], struct sdbuildkey, node)->size == size) &&
            (memcmp(((struct sdbuild*)ptr)->k.s +
                    sscast(t->i[pos], struct sdbuildkey, node)->offsetstart, key, size) == 0))

static inline int
sd_buildadd_sparse(struct sdbuild *b, struct runtime *r, struct sv *v)
{
	int i = 0;
	for (; i < r->scheme->fields_count; i++)
	{
		uint32_t fieldsize;
		char *field = sv_field(v, r, i, &fieldsize);

		int offsetstart = ss_bufused(&b->k);
		int offset = (offsetstart - sd_buildref(b)->k);

		/* match a field copy */
		int is_duplicate = 0;
		uint32_t hash = 0;
		int pos = 0;
		if (b->compress_dup) {
			hash = ss_fnv(field, fieldsize);
			pos = sd_buildsearch(&b->tracker, hash, field, fieldsize, b);
			if (b->tracker.i[pos]) {
				is_duplicate = 1;
				struct sdbuildkey *ref = sscast(b->tracker.i[pos], struct sdbuildkey, node);
				offset = ref->offset;
			}
		}

		/* offset */
		int rc;
		rc = ss_bufensure(&b->v, r->a, sizeof(uint32_t));
		if (ssunlikely(rc == -1))
			return sr_oom(r->e);
		*(uint32_t*)b->v.p = offset;
		ss_bufadvance(&b->v, sizeof(uint32_t));
		if (is_duplicate)
			continue;

		/* copy field */
		rc = ss_bufensure(&b->k, r->a, sizeof(uint32_t) + fieldsize);
		if (ssunlikely(rc == -1))
			return sr_oom(r->e);
		*(uint32_t*)b->k.p = fieldsize;
		ss_bufadvance(&b->k, sizeof(uint32_t));
		memcpy(b->k.p, field, fieldsize);
		ss_bufadvance(&b->k, fieldsize);

		/* add field reference */
		if (b->compress_dup) {
			if (ssunlikely(ss_htisfull(&b->tracker))) {
				rc = ss_htresize(&b->tracker, r->a);
				if (ssunlikely(rc == -1))
					return sr_oom(r->e);
			}
			struct sdbuildkey *ref = ss_malloc(r->a, sizeof(struct sdbuildkey));
			if (ssunlikely(ref == NULL))
				return sr_oom(r->e);
			ref->node.hash = hash;
			ref->offset = offset;
			ref->offsetstart = offsetstart + sizeof(uint32_t);
			ref->size = fieldsize;
			ss_htset(&b->tracker, pos, &ref->node);
		}
	}

	return 0;
}

static inline int
sd_buildadd_raw(struct sdbuild *b, struct runtime *r, struct sv *v, uint32_t size)
{
	int rc = ss_bufensure(&b->v, r->a, size);
	if (ssunlikely(rc == -1))
		return sr_oom(r->e);
	memcpy(b->v.p, sv_pointer(v), size);
	ss_bufadvance(&b->v, size);
	return 0;
}

int sd_buildadd(struct sdbuild *b, struct runtime *r, struct sv *v, uint32_t flags)
{
	/* prepare document metadata */
	int rc = ss_bufensure(&b->m, r->a, sizeof(struct sdv));
	if (ssunlikely(rc == -1))
		return sr_oom(r->e);
	uint64_t lsn = sv_lsn(v);
	uint32_t timestamp = sv_timestamp(v);
	uint32_t size = sv_size(v);
	struct sdpageheader *h = sd_buildheader(b);
	struct sdv *sv = (struct sdv*)b->m.p;
	sv->flags = flags;
	sv->offset = ss_bufused(&b->v) - sd_buildref(b)->v;
	sv->size = size;
	sv->lsn = lsn;
	sv->timestamp = timestamp;
	ss_bufadvance(&b->m, sizeof(struct sdv));
	/* copy document */
	switch (r->fmt_storage) {
	case SF_RAW:
		rc = sd_buildadd_raw(b, r, v, size);
		break;
	case SF_SPARSE:
		rc = sd_buildadd_sparse(b, r, v);
		break;
	}
	if (ssunlikely(rc == -1))
		return -1;
	/* update page header */
	h->count++;
	size += sizeof(struct sdv) + size;
	if (size > b->vmax)
		b->vmax = size;
	if (lsn > h->lsnmax)
		h->lsnmax = lsn;
	if (lsn < h->lsnmin)
		h->lsnmin = lsn;
	if (timestamp < h->tsmin)
		h->tsmin = timestamp;
	if (sv->flags & SVDUP) {
		h->countdup++;
		if (lsn < h->lsnmindup)
			h->lsnmindup = lsn;
	}
	return 0;
}

static inline int
sd_buildcompress(struct sdbuild *b, struct runtime *r)
{
	assert(b->compress_if != &ss_nonefilter);
	/* reserve header */
	int rc = ss_bufensure(&b->c, r->a, sizeof(struct sdpageheader));
	if (ssunlikely(rc == -1))
		return -1;
	ss_bufadvance(&b->c, sizeof(struct sdpageheader));
	/* compression (including meta-data) */
	struct sdbuildref *ref = sd_buildref(b);
	struct ssfilter f;
	rc = ss_filterinit(&f, b->compress_if, r->a, SS_FINPUT);
	if (ssunlikely(rc == -1))
		return -1;
	rc = ss_filterstart(&f, &b->c);
	if (ssunlikely(rc == -1))
		goto error;
	rc = ss_filternext(&f, &b->c, b->m.s + ref->m + sizeof(struct sdpageheader),
	                   ref->msize - sizeof(struct sdpageheader));
	if (ssunlikely(rc == -1))
		goto error;
	rc = ss_filternext(&f, &b->c, b->v.s + ref->v, ref->vsize);
	if (ssunlikely(rc == -1))
		goto error;
	rc = ss_filternext(&f, &b->c, b->k.s + ref->k, ref->ksize);
	if (ssunlikely(rc == -1))
		goto error;
	rc = ss_filtercomplete(&f, &b->c);
	if (ssunlikely(rc == -1))
		goto error;
	ss_filterfree(&f);
	return 0;
error:
	ss_filterfree(&f);
	return -1;
}

static int sd_buildend(struct sdbuild *b, struct runtime *r)
{
	/* update sizes */
	struct sdbuildref *ref = sd_buildref(b);
	ref->msize = ss_bufused(&b->m) - ref->m;
	ref->vsize = ss_bufused(&b->v) - ref->v;
	ref->ksize = ss_bufused(&b->k) - ref->k;
	ref->csize = 0;
	/* calculate data crc (non-compressed) */
	struct sdpageheader *h = sd_buildheader(b);
	uint32_t crc = 0;
	if (sslikely(b->crc)) {
		crc = ss_crcp(b->m.s + ref->m, ref->msize, 0);
		crc = ss_crcp(b->v.s + ref->v, ref->vsize, crc);
		crc = ss_crcp(b->k.s + ref->k, ref->ksize, crc);
	}
	h->crcdata = crc;
	/* compression */
	if (b->compress) {
		int rc = sd_buildcompress(b, r);
		if (ssunlikely(rc == -1))
			return -1;
		ref->csize = ss_bufused(&b->c) - ref->c;
	}
	/* update page header */
	int total = ref->msize + ref->vsize + ref->ksize;
	h->sizekeys = ref->ksize;
	h->sizeorigin = total - sizeof(struct sdpageheader);
	h->size = h->sizeorigin;
	if (b->compress)
		h->size = ref->csize - sizeof(struct sdpageheader);
	else
		h->size = h->sizeorigin;
	h->crc = ss_crcs(h, sizeof(struct sdpageheader), 0);
	if (b->compress)
		memcpy(b->c.s + ref->c, h, sizeof(struct sdpageheader));
	return 0;
}

static int sd_buildcommit(struct sdbuild *b, struct runtime *r)
{
	if (b->compress_dup)
		sd_buildfree_tracker(b, r);
	if (b->compress) {
		ss_bufreset(&b->m);
		ss_bufreset(&b->v);
		ss_bufreset(&b->k);
	}
	b->n++;
	return 0;
}

static int sd_indexbegin(struct sdindex *i, struct runtime *r)
{
	int rc = ss_bufensure(&i->i, r->a, sizeof(struct sdindexheader));
	if (ssunlikely(rc == -1))
		return sr_oom(r->e);
	struct sdindexheader *h = sd_indexheader(i);
	sr_version_storage(&h->version);
	h->crc         = 0;
	h->size        = 0;
	h->sizevmax    = 0;
	h->count       = 0;
	h->keys        = 0;
	h->total       = 0;
	h->totalorigin = 0;
	h->extension   = 0;
	h->extensions  = 0;
	h->lsnmin      = UINT64_MAX;
	h->lsnmax      = 0;
	h->tsmin       = UINT32_MAX;
	h->offset      = 0;
	h->dupkeys     = 0;
	h->dupmin      = UINT64_MAX;
	memset(h->reserve, 0, sizeof(h->reserve));
	sd_idinit(&h->id, 0, 0, 0);
	i->h = NULL;
	ss_bufadvance(&i->i, sizeof(struct sdindexheader));
	return 0;
}

static int sd_indexcommit(struct sdindex *i, struct runtime *r, struct sdid *id, struct ssqf *qf, uint64_t offset)
{
	int size = ss_bufused(&i->v);
	int size_extension = 0;
	int extensions = 0;
	if (qf) {
		extensions = SD_INDEXEXT_AMQF;
		size_extension += sizeof(struct sdindexamqf);
		size_extension += qf->qf_table_size;
	}
	int rc = ss_bufensure(&i->i, r->a, size + size_extension);
	if (ssunlikely(rc == -1))
		return sr_oom(r->e);
	memcpy(i->i.p, i->v.s, size);
	ss_bufadvance(&i->i, size);
	if (qf) {
		struct sdindexamqf *qh = (struct sdindexamqf*)(i->i.p);
		qh->q       = qf->qf_qbits;
		qh->r       = qf->qf_rbits;
		qh->entries = qf->qf_entries;
		qh->size    = qf->qf_table_size;
		ss_bufadvance(&i->i, sizeof(struct sdindexamqf));
		memcpy(i->i.p, qf->qf_table, qf->qf_table_size);
		ss_bufadvance(&i->i, qf->qf_table_size);
	}
	ss_buffree(&i->v, r->a);
	i->h = sd_indexheader(i);
	i->h->offset     = offset;
	i->h->id         = *id;
	i->h->extension  = size_extension;
	i->h->extensions = extensions;
	i->h->crc = ss_crcs(i->h, sizeof(struct sdindexheader), 0);
	return 0;
}

static inline int
sd_indexadd_raw(struct sdindex *i, struct runtime *r, struct sdindexpage *p, char *min, char *max)
{
	/* reformat document to exclude non-key fields */
	p->sizemin = sf_comparable_size(r->scheme, min);
	p->sizemax = sf_comparable_size(r->scheme, max);
	int rc = ss_bufensure(&i->v, r->a, p->sizemin + p->sizemax);
	if (ssunlikely(rc == -1))
		return sr_oom(r->e);
	sf_comparable_write(r->scheme, min, i->v.p);
	ss_bufadvance(&i->v, p->sizemin);
	sf_comparable_write(r->scheme, max, i->v.p);
	ss_bufadvance(&i->v, p->sizemax);
	return 0;
}

static inline int
sd_indexadd_sparse(struct sdindex *i, struct runtime *r, struct sdbuild *build, struct sdindexpage *p, char *min, char *max)
{
	struct sfv fields[16];

	/* min */
	int part = 0;
	while (part < r->scheme->fields_count)
	{
		/* read field offset */
		uint32_t offset = *(uint32_t*)min;
		min += sizeof(uint32_t);
		/* read field */
		char *field = build->k.s + sd_buildref(build)->k + offset;
		int fieldsize = *(uint32_t*)field;
		field += sizeof(uint32_t);
		/* copy only key fields, others are set to zero */
		struct sfv *k = &fields[part];
		if (r->scheme->fields[part]->key) {
			k->pointer = field;
			k->size = fieldsize;
		} else {
			k->pointer = NULL;
			k->size = 0;
		}
		part++;
	}
	p->sizemin = sf_writesize(r->scheme, fields);
	int rc = ss_bufensure(&i->v, r->a, p->sizemin);
	if (ssunlikely(rc == -1))
		return sr_oom(r->e);
	sf_write(r->scheme, fields, i->v.p);
	ss_bufadvance(&i->v, p->sizemin);

	/* max */
	part = 0;
	while (part < r->scheme->fields_count)
	{
		/* read field offset */
		uint32_t offset = *(uint32_t*)max;
		max += sizeof(uint32_t);

		/* read field */
		char *field = build->k.s + sd_buildref(build)->k + offset;
		int fieldsize = *(uint32_t*)field;
		field += sizeof(uint32_t);

		struct sfv *k = &fields[part];
		if (r->scheme->fields[part]->key) {
			k->pointer = field;
			k->size = fieldsize;
		} else {
			k->pointer = NULL;
			k->size = 0;
		}
		part++;
	}
	p->sizemax = sf_writesize(r->scheme, fields);
	rc = ss_bufensure(&i->v, r->a, p->sizemax);
	if (ssunlikely(rc == -1))
		return sr_oom(r->e);
	sf_write(r->scheme, fields, i->v.p);
	ss_bufadvance(&i->v, p->sizemax);
	return 0;
}

static int sd_indexadd(struct sdindex *i, struct runtime *r, struct sdbuild *build, uint64_t offset)
{
	int rc = ss_bufensure(&i->i, r->a, sizeof(struct sdindexpage));
	if (ssunlikely(rc == -1))
		return sr_oom(r->e);
	struct sdpageheader *ph = sd_buildheader(build);

	int size = ph->size + sizeof(struct sdpageheader);
	int sizeorigin = ph->sizeorigin + sizeof(struct sdpageheader);

	/* prepare page header */
	struct sdindexpage *p = (struct sdindexpage*)i->i.p;
	p->offset      = offset;
	p->offsetindex = ss_bufused(&i->v);
	p->lsnmin      = ph->lsnmin;
	p->lsnmax      = ph->lsnmax;
	p->size        = size;
	p->sizeorigin  = sizeorigin;
	p->sizemin     = 0;
	p->sizemax     = 0;

	/* copy keys */
	if (ssunlikely(ph->count > 0))
	{
		char *min = sd_buildminkey(build);
		char *max = sd_buildmaxkey(build);
		switch (r->fmt_storage) {
		case SF_RAW:
			rc = sd_indexadd_raw(i, r, p, min, max);
			break;
		case SF_SPARSE:
			rc = sd_indexadd_sparse(i, r, build, p, min, max);
			break;
		}
		if (ssunlikely(rc == -1))
			return -1;
	}

	/* update index info */
	struct sdindexheader *h = sd_indexheader(i);
	h->count++;
	h->size  += sizeof(struct sdindexpage) + p->sizemin + p->sizemax;
	h->keys  += ph->count;
	h->total += size;
	h->totalorigin += sizeorigin;
	if (build->vmax > h->sizevmax)
		h->sizevmax = build->vmax;
	if (ph->lsnmin < h->lsnmin)
		h->lsnmin = ph->lsnmin;
	if (ph->lsnmax > h->lsnmax)
		h->lsnmax = ph->lsnmax;
	if (ph->tsmin < h->tsmin)
		h->tsmin = ph->tsmin;
	h->dupkeys += ph->countdup;
	if (ph->lsnmindup < h->dupmin)
		h->dupmin = ph->lsnmindup;
	ss_bufadvance(&i->i, sizeof(struct sdindexpage));
	return 0;
}

static int sd_indexcopy(struct sdindex *i, struct runtime *r, struct sdindexheader *h)
{
	int size = sd_indexsize_ext(h);
	int rc = ss_bufensure(&i->i, r->a, size);
	if (ssunlikely(rc == -1))
		return sr_oom(r->e);
	memcpy(i->i.s, (char*)h, size);
	ss_bufadvance(&i->i, size);
	i->h = sd_indexheader(i);
	return 0;
}

static struct ssiterif sd_indexiter =
{
	.close = sd_indexiter_close,
	.has   = sd_indexiter_has,
	.of    = sd_indexiter_of,
	.next  = sd_indexiter_next
};

static int
sd_mergeinit(struct sdmerge *m, struct runtime *r, struct ssiter *i,
	     struct sdbuild *build, struct ssqf *qf, struct svupsert *upsert,
	     struct sdmergeconf *conf)
{
	m->conf      = conf;
	m->build     = build;
	m->qf        = qf;
	m->r         = r;
	m->merge     = i;
	m->processed = 0;
	m->current   = 0;
	m->limit     = 0;
	m->resume    = 0;
	if (conf->amqf) {
		int rc = ss_qfensure(qf, r->a, conf->stream);
		if (ssunlikely(rc == -1))
			return sr_oom(r->e);
	}
	sd_indexinit(&m->index);
	ss_iterinit(sv_writeiter, &m->i);
	ss_iteropen(sv_writeiter, &m->i, r, i, upsert,
	            (uint64_t)conf->size_page, sizeof(struct sdv),
	            conf->expire,
	            conf->timestamp,
	            conf->vlsn,
	            conf->vlsn_lru,
	            conf->save_delete,
	            conf->save_upsert);
	return 0;
}

static int sd_mergefree(struct sdmerge *m)
{
	sd_indexfree(&m->index, m->r);
	return 0;
}

static inline int
sd_mergehas(struct sdmerge *m)
{
	if (! ss_iterhas(sv_writeiter, &m->i))
		return 0;
	if (m->current > m->limit)
		return 0;
	return 1;
}

static int sd_merge(struct sdmerge *m)
{
	if (ssunlikely(! ss_iterhas(sv_writeiter, &m->i)))
		return 0;
	struct sdmergeconf *conf = m->conf;
	sd_indexinit(&m->index);
	int rc = sd_indexbegin(&m->index, m->r);
	if (ssunlikely(rc == -1))
		return -1;
	if (conf->amqf)
		ss_qfreset(m->qf);
	m->current = 0;
	m->limit   = 0;
	uint64_t processed = m->processed;
	uint64_t left = (conf->size_stream - processed);
	if (left >= (conf->size_node * 2)) {
		m->limit = conf->size_node;
	} else
	if (left > (conf->size_node)) {
		m->limit = conf->size_node * 2;
	} else {
		m->limit = UINT64_MAX;
	}
	return sd_mergehas(m);
}

static int sd_mergepage(struct sdmerge *m, uint64_t offset)
{
	struct sdmergeconf *conf = m->conf;
	sd_buildreset(m->build, m->r);
	if (m->resume) {
		m->resume = 0;
		if (ssunlikely(! sv_writeiter_resume(&m->i)))
			return 0;
	}
	if (! sd_mergehas(m))
		return 0;
	int rc;
	rc = sd_buildbegin(m->build, m->r, conf->checksum,
	                   conf->expire,
	                   conf->compression_key,
	                   conf->compression,
	                   conf->compression_if);
	if (ssunlikely(rc == -1))
		return -1;
	while (ss_iterhas(sv_writeiter, &m->i))
	{
		struct sv *v = ss_iterof(sv_writeiter, &m->i);
		uint8_t flags = sv_flags(v);
		if (sv_writeiter_is_duplicate(&m->i))
			flags |= SVDUP;
		rc = sd_buildadd(m->build, m->r, v, flags);
		if (ssunlikely(rc == -1))
			return -1;
		if (conf->amqf) {
			ss_qfadd(m->qf, sv_hash(v, m->r));
		}
		ss_iternext(sv_writeiter, &m->i);
	}
	rc = sd_buildend(m->build, m->r);
	if (ssunlikely(rc == -1))
		return -1;
	rc = sd_indexadd(&m->index, m->r, m->build, offset);
	if (ssunlikely(rc == -1))
		return -1;
	m->current = sd_indextotal(&m->index);
	m->resume  = 1;
	return 1;
}

static int sd_mergecommit(struct sdmerge *m, struct sdid *id, uint64_t offset)
{
	m->processed += sd_indextotal(&m->index);
	struct ssqf *qf = NULL;
	if (m->conf->amqf)
		qf = m->qf;
	return sd_indexcommit(&m->index, m->r, id, qf, offset);
}

static struct ssiterif sd_pageiter =
{
	.close   = sd_pageiter_close,
	.has     = sd_pageiter_has,
	.of      = sd_pageiter_of,
	.next    = sd_pageiter_next
};

static struct ssiterif sd_read =
{
	.close = sd_read_close,
	.has   = sd_read_has,
	.of    = sd_read_of,
	.next  = sd_read_next
};

struct sdrecover {
	struct ssfile *file;
	int corrupt;
	struct sdindexheader *v;
	struct sdindexheader *actual;
	struct sdseal *seal;
	struct ssmmap map;
	struct runtime *r;
} sspacked;

static int
sd_recovernext_of(struct sdrecover *i, struct sdseal *next)
{
	if (next == NULL)
		return 0;

	char *eof = (char*)i->map.p + i->map.size;
	char *pointer = (char*)next;

	/* eof */
	if (ssunlikely(pointer == eof)) {
		i->v = NULL;
		return 0;
	}

	/* validate seal pointer */
	if (ssunlikely(((pointer + sizeof(struct sdseal)) > eof))) {
		sr_malfunction(i->r->e, "corrupted db file '%s': bad seal size",
		               ss_pathof(&i->file->path));
		i->corrupt = 1;
		i->v = NULL;
		return -1;
	}
	pointer = i->map.p + next->index_offset;

	/* validate index pointer */
	if (ssunlikely(((pointer + sizeof(struct sdindexheader)) > eof))) {
		sr_malfunction(i->r->e, "corrupted db file '%s': bad index size",
		               ss_pathof(&i->file->path));
		i->corrupt = 1;
		i->v = NULL;
		return -1;
	}
	struct sdindexheader *index = (struct sdindexheader*)(pointer);

	/* validate index crc */
	uint32_t crc = ss_crcs(index, sizeof(struct sdindexheader), 0);
	if (index->crc != crc) {
		sr_malfunction(i->r->e, "corrupted db file '%s': bad index crc",
		               ss_pathof(&i->file->path));
		i->corrupt = 1;
		i->v = NULL;
		return -1;
	}

	/* validate index size */
	char *end = pointer + sizeof(struct sdindexheader) + index->size +
	            index->extension;
	if (ssunlikely(end > eof)) {
		sr_malfunction(i->r->e, "corrupted db file '%s': bad index size",
		               ss_pathof(&i->file->path));
		i->corrupt = 1;
		i->v = NULL;
		return -1;
	}

	/* validate seal */
	int rc = sd_sealvalidate(next, index);
	if (ssunlikely(rc == -1)) {
		sr_malfunction(i->r->e, "corrupted db file '%s': bad seal",
		               ss_pathof(&i->file->path));
		i->corrupt = 1;
		i->v = NULL;
		return -1;
	}
	i->seal = next;
	i->actual = index;
	i->v = index;
	return 1;
}

static int sd_recover_open(struct ssiter *i, struct runtime *r,
			   struct ssfile *file)
{
	struct sdrecover *ri = (struct sdrecover*)i->priv;
	memset(ri, 0, sizeof(*ri));
	ri->r = r;
	ri->file = file;
	if (ssunlikely(ri->file->size < (sizeof(struct sdseal) + sizeof(struct sdindexheader)))) {
		sr_malfunction(ri->r->e, "corrupted db file '%s': bad size",
		               ss_pathof(&ri->file->path));
		ri->corrupt = 1;
		return -1;
	}
	int rc = ss_vfsmmap(r->vfs, &ri->map, ri->file->fd, ri->file->size, 1);
	if (ssunlikely(rc == -1)) {
		sr_malfunction(ri->r->e, "failed to mmap db file '%s': %s",
		               ss_pathof(&ri->file->path),
		               strerror(errno));
		return -1;
	}
	struct sdseal *seal = (struct sdseal*)((char*)ri->map.p);
	rc = sd_recovernext_of(ri, seal);
	if (ssunlikely(rc == -1))
		ss_vfsmunmap(r->vfs, &ri->map);
	return rc;
}

static void
sd_recoverclose(struct ssiter *i)
{
	struct sdrecover *ri = (struct sdrecover*)i->priv;
	ss_vfsmunmap(ri->r->vfs, &ri->map);
}

static int
sd_recoverhas(struct ssiter *i)
{
	struct sdrecover *ri = (struct sdrecover*)i->priv;
	return ri->v != NULL;
}

static void*
sd_recoverof(struct ssiter *i)
{
	struct sdrecover *ri = (struct sdrecover*)i->priv;
	return ri->v;
}

static void
sd_recovernext(struct ssiter *i)
{
	struct sdrecover *ri = (struct sdrecover*)i->priv;
	if (ssunlikely(ri->v == NULL))
		return;
	struct sdseal *next =
		(struct sdseal*)((char*)ri->v +
		    (sizeof(struct sdindexheader) + ri->v->size) +
		     ri->v->extension);
	sd_recovernext_of(ri, next);
}

static struct ssiterif sd_recover =
{
	.close   = sd_recoverclose,
	.has     = sd_recoverhas,
	.of      = sd_recoverof,
	.next    = sd_recovernext
};

static int sd_recover_complete(struct ssiter *i)
{
	struct sdrecover *ri = (struct sdrecover*)i->priv;
	if (ssunlikely(ri->seal == NULL))
		return -1;
	if (sslikely(ri->corrupt == 0))
		return  0;
	/* truncate file to the end of a latest actual
	 * index */
	char *eof =
		(char*)ri->map.p +
		       ri->actual->offset + sizeof(struct sdindexheader) +
		       ri->actual->size +
		       ri->actual->extension;
	uint64_t file_size = eof - ri->map.p;
	int rc = ss_fileresize(ri->file, file_size);
	if (ssunlikely(rc == -1))
		return -1;
	sr_errorreset(ri->r->e);
	return 0;
}

static int sd_schemebegin(struct sdscheme *c, struct runtime *r)
{
	int rc = ss_bufensure(&c->buf, r->a, sizeof(struct sdschemeheader));
	if (ssunlikely(rc == -1))
		return sr_oom(r->e);
	struct sdschemeheader *h = (struct sdschemeheader*)c->buf.s;
	memset(h, 0, sizeof(struct sdschemeheader));
	ss_bufadvance(&c->buf, sizeof(struct sdschemeheader));
	return 0;
}

static int
sd_schemeadd(struct sdscheme *c, struct runtime *r, uint8_t id,
	     enum sstype type, void *value, uint32_t size)
{
	struct sdschemeopt opt = {
		.type = type,
		.id   = id,
		.size = size
	};
	int rc = ss_bufadd(&c->buf, r->a, &opt, sizeof(opt));
	if (ssunlikely(rc == -1))
		goto error;
	rc = ss_bufadd(&c->buf, r->a, value, size);
	if (ssunlikely(rc == -1))
		goto error;
	struct sdschemeheader *h = (struct sdschemeheader*)c->buf.s;
	h->count++;
	return 0;
error:
	return sr_oom(r->e);
}

static int sd_schemecommit(struct sdscheme *c)
{
	if (ssunlikely(ss_bufused(&c->buf) == 0))
		return 0;
	struct sdschemeheader *h = (struct sdschemeheader*)c->buf.s;
	h->size = ss_bufused(&c->buf) - sizeof(struct sdschemeheader);
	h->crc  = ss_crcs((char*)h, ss_bufused(&c->buf), 0);
	return 0;
}

static int
sd_schemewrite(struct sdscheme *c, struct runtime *r, char *path, int sync)
{
	struct ssfile meta;
	ss_fileinit(&meta, r->vfs);
	int rc = ss_filenew(&meta, path);
	if (ssunlikely(rc == -1))
		goto error;
	rc = ss_filewrite(&meta, c->buf.s, ss_bufused(&c->buf));
	if (ssunlikely(rc == -1))
		goto error;
	if (sync) {
		rc = ss_filesync(&meta);
		if (ssunlikely(rc == -1))
			goto error;
	}
	rc = ss_fileclose(&meta);
	if (ssunlikely(rc == -1))
		goto error;
	return 0;
error:
	sr_error(r->e, "scheme file '%s' error: %s",
	         path, strerror(errno));
	ss_fileclose(&meta);
	return -1;
}

static int
sd_schemerecover(struct sdscheme *c, struct runtime *r, char *path)
{
	ssize_t size = ss_vfssize(r->vfs, path);
	if (ssunlikely(size == -1))
		goto error;
	if (ssunlikely((unsigned int)size < sizeof(struct sdschemeheader))) {
		sr_error(r->e, "scheme file '%s' is corrupted", path);
		return -1;
	}
	int rc = ss_bufensure(&c->buf, r->a, size);
	if (ssunlikely(rc == -1))
		return sr_oom(r->e);
	struct ssfile meta;
	ss_fileinit(&meta, r->vfs);
	rc = ss_fileopen(&meta, path);
	if (ssunlikely(rc == -1))
		goto error;
	rc = ss_filepread(&meta, 0, c->buf.s, size);
	if (ssunlikely(rc == -1))
		goto error;
	rc = ss_fileclose(&meta);
	if (ssunlikely(rc == -1))
		goto error;
	ss_bufadvance(&c->buf, size);
	return 0;
error:
	sr_error(r->e, "scheme file '%s' error: %s",
	         path, strerror(errno));
	return -1;
}

static struct ssiterif sd_schemeiter =
{
	.close   = sd_schemeiter_close,
	.has     = sd_schemeiter_has,
	.of      = sd_schemeiter_of,
	.next    = sd_schemeiter_next
};

static int sd_snapshot_begin(struct sdsnapshot *s, struct runtime *r)
{
	int rc = ss_bufensure(&s->buf, r->a, sizeof(struct sdsnapshotheader));
	if (ssunlikely(rc == -1))
		return sr_oom(r->e);
	struct sdsnapshotheader *h = sd_snapshot_header(s);
	memset(h, 0, sizeof(*h));
	ss_bufadvance(&s->buf, sizeof(*h));
	return 0;
}

static int
sd_snapshot_add(struct sdsnapshot *s, struct runtime *r, uint64_t id,
		uint64_t file_size, uint32_t branch_count, uint64_t tr)
{
	int rc = ss_bufensure(&s->buf, r->a, sizeof(struct sdsnapshotnode));
	if (ssunlikely(rc == -1))
		return sr_oom(r->e);
	s->current = (uint32_t)(s->buf.p - s->buf.s);
	struct sdsnapshotnode *n = (struct sdsnapshotnode*)s->buf.p;
	n->crc               = 0;
	n->id                = id;
	n->size_file         = file_size;
	n->size              = 0;
	n->branch_count      = branch_count;
	n->temperature_reads = tr;
	n->reserve[0]        = 0;
	n->reserve[1]        = 0;
	n->reserve[2]        = 0;
	n->reserve[3]        = 0;
	n->crc = ss_crcs((char*)n, sizeof(*n), 0);
	ss_bufadvance(&s->buf, sizeof(*n));
	struct sdsnapshotheader *h = sd_snapshot_header(s);
	h->nodes++;
	return 0;
}

static int
sd_snapshot_addbranch(struct sdsnapshot *s, struct runtime *r,
		      struct sdindexheader *h)
{
	int size = sd_indexsize_ext(h);
	int rc = ss_bufensure(&s->buf, r->a, size);
	if (ssunlikely(rc == -1))
		return sr_oom(r->e);
	memcpy(s->buf.p, (void*)h, size);
	ss_bufadvance(&s->buf, size);
	struct sdsnapshotnode *n = (struct sdsnapshotnode*)(s->buf.s + s->current);
	n->size += size;
	return 0;
}

static int
sd_snapshot_commit(struct sdsnapshot *s,
		   uint64_t lru_v,
		   uint64_t lru_steps,
		   uint64_t lru_intr_lsn,
		   uint64_t lru_intr_sum,
		   uint64_t read_disk,
		   uint64_t read_cache)
{
	struct sdsnapshotheader *h = sd_snapshot_header(s);
	h->lru_v        = lru_v;
	h->lru_steps    = lru_steps;
	h->lru_intr_lsn = lru_intr_lsn;
	h->lru_intr_sum = lru_intr_sum;
	h->read_disk    = read_disk;
	h->read_cache   = read_cache;
	h->size         = ss_bufused(&s->buf);
	h->crc = ss_crcs((char*)h, sizeof(*h), 0);
	return 0;
}

static struct ssiterif sd_snapshotiter =
{
	.close   = sd_snapshotiter_close,
	.has     = sd_snapshotiter_has,
	.of      = sd_snapshotiter_of,
	.next    = sd_snapshotiter_next
};

static uint8_t
sd_vifflags(struct sv *v)
{
	return ((struct sdv*)v->v)->flags;
}

static uint64_t
sd_viflsn(struct sv *v)
{
	return ((struct sdv*)v->v)->lsn;
}

static char*
sd_vifpointer(struct sv *v)
{
	struct sdpage p = {
		.h = (struct sdpageheader*)v->arg
	};
	return sd_pagepointer(&p, (struct sdv*)v->v);
}

static uint32_t
sd_viftimestamp(struct sv *v)
{
	return ((struct sdv*)v->v)->timestamp;
}

static uint32_t
sd_vifsize(struct sv *v)
{
	return ((struct sdv*)v->v)->size;
}

static struct svif sd_vif =
{
	.flags     = sd_vifflags,
	.lsn       = sd_viflsn,
	.lsnset    = NULL,
	.timestamp = sd_viftimestamp,
	.pointer   = sd_vifpointer,
	.size      = sd_vifsize
};

static char*
sd_vrawifpointer(struct sv *v)
{
	return (char*)v->v + sizeof(struct sdv);
}

static struct svif sd_vrawif =
{
	.flags     = sd_vifflags,
	.lsn       = sd_viflsn,
	.lsnset    = NULL,
	.timestamp = sd_viftimestamp,
	.pointer   = sd_vrawifpointer,
	.size      = sd_vifsize
};

static int
sd_writeseal(struct runtime *r, struct ssfile *file, struct ssblob *blob)
{
	struct sdseal seal;
	sd_sealset_open(&seal);
	SS_INJECTION(r->i, SS_INJECTION_SD_BUILD_1,
	             seal.crc++); /* corrupt seal */
	int rc;
	rc = ss_filewrite(file, &seal, sizeof(seal));
	if (ssunlikely(rc == -1)) {
		sr_malfunction(r->e, "file '%s' write error: %s",
		               ss_pathof(&file->path),
		               strerror(errno));
		return -1;
	}
	if (blob) {
		rc = ss_blobadd(blob, &seal, sizeof(seal));
		if (ssunlikely(rc == -1))
			return sr_oom_malfunction(r->e);
	}
	return 0;
}

static int
sd_writepage(struct runtime *r, struct ssfile *file, struct ssblob *blob,
	     struct sdbuild *b)
{
	SS_INJECTION(r->i, SS_INJECTION_SD_BUILD_0,
	             sr_malfunction(r->e, "%s", "error injection");
	             return -1);
	struct sdbuildref *ref = sd_buildref(b);
	struct iovec iovv[3];
	struct ssiov iov;
	ss_iovinit(&iov, iovv, 3);
	if (ss_bufused(&b->c) > 0) {
		/* compressed */
		ss_iovadd(&iov, b->c.s, ref->csize);
	} else {
		/* uncompressed */
		ss_iovadd(&iov, b->m.s + ref->m, ref->msize);
		ss_iovadd(&iov, b->v.s + ref->v, ref->vsize);
		ss_iovadd(&iov, b->k.s + ref->k, ref->ksize);
	}
	int rc;
	rc = ss_filewritev(file, &iov);
	if (ssunlikely(rc == -1)) {
		sr_malfunction(r->e, "file '%s' write error: %s",
		               ss_pathof(&file->path),
		               strerror(errno));
		return -1;
	}
	if (blob) {
		int i = 0;
		while (i < iov.iovc) {
			struct iovec *v = &iovv[i];
			rc = ss_blobadd(blob, v->iov_base, v->iov_len);
			if (ssunlikely(rc == -1))
				return sr_oom_malfunction(r->e);
			i++;
		}
	}
	return 0;
}

static int
sd_writeindex(struct runtime *r, struct ssfile *file,
	      struct ssblob *blob, struct sdindex *index)
{
	int rc;
	rc = ss_filewrite(file, index->i.s, ss_bufused(&index->i));
	if (ssunlikely(rc == -1)) {
		sr_malfunction(r->e, "file '%s' write error: %s",
		               ss_pathof(&file->path),
		               strerror(errno));
		return -1;
	}
	if (blob) {
		rc = ss_blobadd(blob, index->i.s, ss_bufused(&index->i));
		if (ssunlikely(rc == -1))
			return sr_oom_malfunction(r->e);
	}
	return 0;
}

static int
sd_seal(struct runtime *r, struct ssfile *file, struct ssblob *blob, struct sdindex *index, uint64_t offset)
{
	struct sdseal seal;
	sd_sealset_close(&seal, index->h);
	int rc;
	rc = ss_filepwrite(file, offset, &seal, sizeof(seal));
	if (ssunlikely(rc == -1)) {
		sr_malfunction(r->e, "file '%s' write error: %s",
		               ss_pathof(&file->path),
		               strerror(errno));
		return -1;
	}
	if (blob) {
		assert(blob->map.size >= sizeof(seal));
		memcpy(blob->map.p, &seal, sizeof(seal));
	}
	return 0;
}

enum sistorage {
	SI_SCACHE,
	SI_SANTI_CACHE,
	SI_SIN_MEMORY
};

struct sischeme {
	uint32_t    id;
	char       *name;
	char       *path;
	uint32_t    path_fail_on_exists;
	uint32_t    path_fail_on_drop;
	char       *path_backup;
	uint32_t    mmap;
	enum sistorage storage;
	char       *storage_sz;
	uint32_t    sync;
	uint64_t    node_size;
	uint32_t    node_page_size;
	uint32_t    node_page_checksum;
	uint32_t    node_compact_load;
	uint32_t    expire;
	uint32_t    compression;
	char       *compression_sz;
	struct ssfilterif *compression_if;
	uint32_t    compression_branch;
	char       *compression_branch_sz;
	struct ssfilterif *compression_branch_if;
	uint32_t    compression_key;
	uint32_t    temperature;
	uint32_t    amqf;
	uint64_t    lru;
	uint32_t    lru_step;
	uint32_t    buf_gc_wm;
	enum sfstorage fmt_storage;
	struct sfupsert    fmt_upsert;
	struct sfscheme    scheme;
	struct srversion   version;
	struct srversion   version_storage;
};

static void si_schemeinit(struct sischeme*);
static void si_schemefree(struct sischeme*, struct runtime*);
static int  si_schemedeploy(struct sischeme*, struct runtime*);
static int  si_schemerecover(struct sischeme*, struct runtime*);

struct sibranch {
	struct sdid id;
	struct sdindex index;
	struct ssblob copy;
	struct sibranch *link;
	struct sibranch *next;
} sspacked;

static inline void
si_branchinit(struct sibranch *b, struct runtime *r)
{
	memset(&b->id, 0, sizeof(b->id));
	sd_indexinit(&b->index);
	ss_blobinit(&b->copy, r->vfs);
	b->link = NULL;
	b->next = NULL;
}

static inline struct sibranch*
si_branchnew(struct runtime *r)
{
	struct sibranch *b = (struct sibranch*)ss_malloc(r->a, sizeof(struct sibranch));
	if (ssunlikely(b == NULL)) {
		sr_oom_malfunction(r->e);
		return NULL;
	}
	si_branchinit(b, r);
	return b;
}

static inline void
si_branchset(struct sibranch *b, struct sdindex *i)
{
	b->id = i->h->id;
	b->index = *i;
}

static inline void
si_branchfree(struct sibranch *b, struct runtime *r)
{
	sd_indexfree(&b->index, r);
	ss_blobfree(&b->copy);
	ss_free(r->a, b);
}

static inline int
si_branchis_root(struct sibranch *b) {
	return b->next == NULL;
}

static inline int
si_branchload(struct sibranch *b, struct runtime *r, struct ssfile *file)
{
	struct sdindexheader *h = b->index.h;
	uint64_t offset = h->offset - h->total - sizeof(struct sdseal);
	uint64_t size   = h->total + sizeof(struct sdseal) + sizeof(struct sdindexheader) +
	                  h->size + h->extension;
	assert(b->copy.s == NULL);
	int rc;
	rc = ss_blobensure(&b->copy, size);
	if (ssunlikely(rc == -1))
		return sr_oom_malfunction(r->e);
	rc = ss_filepread(file, offset, b->copy.s, size);
	if (ssunlikely(rc == -1)) {
		sr_malfunction(r->e, "db file '%s' read error: %s",
		               ss_pathof(&file->path), strerror(errno));
		return -1;
	}
	ss_blobadvance(&b->copy, size);
	return 0;
}

#define SI_NONE       0
#define SI_LOCK       1
#define SI_ROTATE     2
#define SI_SPLIT      4
#define SI_PROMOTE    8
#define SI_REVOKE     16
#define SI_RDB        32
#define SI_RDB_DBI    64
#define SI_RDB_DBSEAL 128
#define SI_RDB_UNDEF  256
#define SI_RDB_REMOVE 512

struct sinode {
	uint32_t   recover;
	uint16_t   flags;
	uint64_t   update_time;
	uint32_t   used;
	uint32_t   backup;
	uint64_t   lru;
	uint64_t   ac;
	uint32_t   in_memory;
	struct sibranch   self;
	struct sibranch  *branch;
	uint32_t   branch_count;
	uint32_t   temperature;
	uint64_t   temperature_reads;
	uint16_t   refs;
	pthread_mutex_t reflock;
	struct svindex    i0, i1;
	struct ssfile     file;
	struct ssmmap     map, map_swap;
	struct ssrbnode   node;
	struct ssrqnode   nodecompact;
	struct ssrqnode   nodebranch;
	struct ssrqnode   nodetemp;
	struct rlist     gc;
	struct rlist     commit;
} sspacked;

static struct sinode *si_nodenew(struct runtime*);
static int
si_nodeopen(struct sinode*, struct runtime*, struct sischeme*,
	    struct sspath*, struct sdsnapshotnode*);
static int
si_nodecreate(struct sinode*, struct runtime*, struct sischeme*, struct sdid*);
static int si_nodefree(struct sinode*, struct runtime*, int);
static int si_nodemap(struct sinode*, struct runtime*);
static int si_noderead(struct sinode*, struct runtime*, struct ssbuf*);
static int si_nodegc_index(struct runtime*, struct svindex*);
static int si_nodegc(struct sinode*, struct runtime*, struct sischeme*);
static int si_nodeseal(struct sinode*, struct runtime*, struct sischeme*);
static int si_nodecomplete(struct sinode*, struct runtime*, struct sischeme*);

static inline void
si_nodelock(struct sinode *node) {
	assert(! (node->flags & SI_LOCK));
	node->flags |= SI_LOCK;
}

static inline void
si_nodeunlock(struct sinode *node) {
	assert((node->flags & SI_LOCK) > 0);
	node->flags &= ~SI_LOCK;
}

static inline void
si_nodesplit(struct sinode *node) {
	node->flags |= SI_SPLIT;
}

static inline void
si_noderef(struct sinode *node)
{
	tt_pthread_mutex_lock(&node->reflock);
	node->refs++;
	tt_pthread_mutex_unlock(&node->reflock);
}

static inline uint16_t
si_nodeunref(struct sinode *node)
{
	tt_pthread_mutex_lock(&node->reflock);
	assert(node->refs > 0);
	uint16_t v = node->refs--;
	tt_pthread_mutex_unlock(&node->reflock);
	return v;
}

static inline uint16_t
si_noderefof(struct sinode *node)
{
	tt_pthread_mutex_lock(&node->reflock);
	uint16_t v = node->refs;
	tt_pthread_mutex_unlock(&node->reflock);
	return v;
}

static inline struct svindex*
si_noderotate(struct sinode *node) {
	node->flags |= SI_ROTATE;
	return &node->i0;
}

static inline void
si_nodeunrotate(struct sinode *node) {
	assert((node->flags & SI_ROTATE) > 0);
	node->flags &= ~SI_ROTATE;
	node->i0 = node->i1;
	sv_indexinit(&node->i1);
}

static inline struct svindex*
si_nodeindex(struct sinode *node) {
	if (node->flags & SI_ROTATE)
		return &node->i1;
	return &node->i0;
}

static inline struct svindex*
si_nodeindex_priority(struct sinode *node, struct svindex **second)
{
	if (ssunlikely(node->flags & SI_ROTATE)) {
		*second = &node->i0;
		return &node->i1;
	}
	*second = NULL;
	return &node->i0;
}

static inline struct sinode*
si_nodeof(struct ssrbnode *node) {
	return sscast(node, struct sinode, node);
}

static inline int
si_nodecmp(struct sinode *n, void *key, int size, struct sfscheme *s)
{
	struct sdindexpage *min = sd_indexmin(&n->self.index);
	struct sdindexpage *max = sd_indexmax(&n->self.index);
	int l = sf_compare(s, sd_indexpage_min(&n->self.index, min),
	                   min->sizemin, key, size);
	int r = sf_compare(s, sd_indexpage_max(&n->self.index, max),
	                   max->sizemax, key, size);
	/* inside range */
	if (l <= 0 && r >= 0)
		return 0;
	/* key > range */
	if (l == -1)
		return -1;
	/* key < range */
	assert(r == 1);
	return 1;
}

static inline uint64_t
si_nodesize(struct sinode *n)
{
	uint64_t size = 0;
	struct sibranch *b = n->branch;
	while (b) {
		size += sd_indexsize_ext(b->index.h) +
		        sd_indextotal(&b->index);
		b = b->next;
	}
	return size;
}

struct sinodeview {
	struct sinode   *node;
	uint16_t  flags;
	uint32_t  branch_count;
};

static inline void
si_nodeview_init(struct sinodeview *v, struct sinode *node)
{
	v->node         = node;
	v->branch_count = node->branch_count;
	v->flags        = node->flags;
}

static inline void
si_nodeview_open(struct sinodeview *v, struct sinode *node)
{
	si_noderef(node);
	si_nodeview_init(v, node);
}

static inline void
si_nodeview_close(struct sinodeview *v)
{
	si_nodeunref(v->node);
	v->node = NULL;
}

struct siplanner {
	struct ssrq branch;
	struct ssrq compact;
	struct ssrq temp;
	void *i;
};

/* plan */
#define SI_BRANCH        1
#define SI_AGE           2
#define SI_COMPACT       4
#define SI_COMPACT_INDEX 8
#define SI_CHECKPOINT    16
#define SI_GC            32
#define SI_TEMP          64
#define SI_BACKUP        128
#define SI_BACKUPEND     256
#define SI_SHUTDOWN      512
#define SI_DROP          1024
#define SI_SNAPSHOT      2048
#define SI_ANTICACHE     4096
#define SI_LRU           8192
#define SI_NODEGC        16384
#define SI_EXPIRE        32768

/* explain */
#define SI_ENONE         0
#define SI_ERETRY        1
#define SI_EINDEX_SIZE   2
#define SI_EINDEX_AGE    3
#define SI_EBRANCH_COUNT 4

struct siplan {
	int explain;
	int plan;
	/* branch:
	 *   a: index_size
	 * age:
	 *   a: ttl
	 *   b: ttl_wm
	 * compact:
	 *   a: branches
	 *   b: mode
	 * compact_index:
	 *   a: index_size
	 * checkpoint:
	 *   a: lsn
	 * nodegc:
	 * gc:
	 *   a: lsn
	 *   b: percent
	 * expire:
	 *   a: ttl
	 * lru:
	 * temperature:
	 * anticache:
	 *   a: asn
	 *   b: available
	 *   c: *node_size
	 * snapshot:
	 *   a: ssn
	 * backup:
	 *   a: bsn
	 * shutdown:
	 * drop:
	 */
	uint64_t a, b, c;
	struct sinode *node;
};

static int si_planinit(struct siplan*);
static int si_plannerinit(struct siplanner*, struct ssa*, void*);
static int si_plannerfree(struct siplanner*, struct ssa*);
static int si_plannertrace(struct siplan*, uint32_t, struct sstrace*);
static int si_plannerupdate(struct siplanner*, int, struct sinode*);
static int si_plannerremove(struct siplanner*, int, struct sinode*);
static int si_planner(struct siplanner*, struct siplan*);

static inline int
si_amqfhas_branch(struct runtime *r, struct sibranch *b, char *key)
{
	struct sdindexamqf *qh = sd_indexamqf(&b->index);
	struct ssqf qf;
	ss_qfrecover(&qf, qh->q, qh->r, qh->size, qh->table);
	return ss_qfhas(&qf, sf_hash(r->scheme, key));
}

enum siref {
	SI_REFFE,
	SI_REFBE
};

struct si {
	struct srstatus   status;
	pthread_mutex_t    lock;
	struct siplanner  p;
	struct ssrb       i;
	int        n;
	uint64_t   update_time;
	uint32_t   backup;
	uint32_t   snapshot_run;
	uint64_t   snapshot;
	uint64_t   lru_run_lsn;
	uint64_t   lru_v;
	uint64_t   lru_steps;
	uint64_t   lru_intr_lsn;
	uint64_t   lru_intr_sum;
	uint64_t   read_disk;
	uint64_t   read_cache;
	uint64_t   size;
	pthread_mutex_t ref_lock;
	uint32_t   ref_fe;
	uint32_t   ref_be;
	uint32_t   gc_count;
	struct rlist     gc;
	struct ssbuf      readbuf;
	struct svupsert   u;
	struct sischeme   scheme;
	struct so        *object;
	struct runtime         r;
	struct rlist     link;
};

static inline int
si_active(struct si *i) {
	return sr_statusactive(&i->status);
}

static inline void
si_lock(struct si *i) {
	tt_pthread_mutex_lock(&i->lock);
}

static inline void
si_unlock(struct si *i) {
	tt_pthread_mutex_unlock(&i->lock);
}

static inline struct runtime*
si_r(struct si *i) {
	return &i->r;
}

static inline struct sischeme*
si_scheme(struct si *i) {
	return &i->scheme;
}

static struct si *si_init(struct runtime*, struct so*);
static int si_open(struct si*);
static int si_close(struct si*);
static int si_insert(struct si*, struct sinode*);
static int si_remove(struct si*, struct sinode*);
static int si_replace(struct si*, struct sinode*, struct sinode*);
static int si_refs(struct si*);
static int si_ref(struct si*, enum siref);
static int si_unref(struct si*, enum siref);
static int si_plan(struct si*, struct siplan*);
static int
si_execute(struct si*, struct sdc*, struct siplan*, uint64_t, uint64_t);

static inline void
si_lru_add(struct si *i, struct svref *ref)
{
	i->lru_intr_sum += ref->v->size;
	if (ssunlikely(i->lru_intr_sum >= i->scheme.lru_step))
	{
		uint64_t lsn = sr_seq(i->r.seq, SR_LSN);
		i->lru_v += (lsn - i->lru_intr_lsn);
		i->lru_steps++;
		i->lru_intr_lsn = lsn;
		i->lru_intr_sum = 0;
	}
}

static inline uint64_t
si_lru_vlsn_of(struct si *i)
{
	assert(i->scheme.lru_step != 0);
	uint64_t size = i->size;
	if (sslikely(size <= i->scheme.lru))
		return 0;
	uint64_t lru_v = i->lru_v;
	uint64_t lru_steps = i->lru_steps;
	uint64_t lru_avg_step;
	uint64_t oversize = size - i->scheme.lru;
	uint64_t steps = 1 + oversize / i->scheme.lru_step;
	lru_avg_step = lru_v / lru_steps;
	return i->lru_intr_lsn + (steps * lru_avg_step);
}

static inline uint64_t
si_lru_vlsn(struct si *i)
{
	if (sslikely(i->scheme.lru == 0))
		return 0;
	si_lock(i);
	int rc = si_lru_vlsn_of(i);
	si_unlock(i);
	return rc;
}

static uint32_t si_gcv(struct runtime*, struct svv*);
static uint32_t si_gcref(struct runtime*, struct svref*);

struct sicachebranch {
	struct sibranch *branch;
	struct sdindexpage *ref;
	struct sdpage page;
	struct ssiter i;
	struct ssiter page_iter;
	struct ssiter index_iter;
	struct ssbuf buf_a;
	struct ssbuf buf_b;
	int open;
	struct sicachebranch *next;
} sspacked;

struct sicache {
	struct sicachebranch *path;
	struct sicachebranch *branch;
	uint32_t count;
	uint64_t nsn;
	struct sinode *node;
	struct sicache *next;
	struct sicachepool *pool;
};

struct sicachepool {
	struct sicache *head;
	int n;
	struct runtime *r;
};

static inline void
si_cacheinit(struct sicache *c, struct sicachepool *pool)
{
	c->path   = NULL;
	c->branch = NULL;
	c->count  = 0;
	c->node   = NULL;
	c->nsn    = 0;
	c->next   = NULL;
	c->pool   = pool;
}

static inline void
si_cachefree(struct sicache *c)
{
	struct ssa *a = c->pool->r->a;
	struct sicachebranch *next;
	struct sicachebranch *cb = c->path;
	while (cb) {
		next = cb->next;
		ss_buffree(&cb->buf_a, a);
		ss_buffree(&cb->buf_b, a);
		ss_free(a, cb);
		cb = next;
	}
}

static inline void
si_cachereset(struct sicache *c)
{
	struct sicachebranch *cb = c->path;
	while (cb) {
		ss_bufreset(&cb->buf_a);
		ss_bufreset(&cb->buf_b);
		cb->branch = NULL;
		cb->ref = NULL;
		ss_iterclose(sd_read, &cb->i);
		cb->open = 0;
		cb = cb->next;
	}
	c->branch = NULL;
	c->node   = NULL;
	c->nsn    = 0;
	c->count  = 0;
}

static inline struct sicachebranch*
si_cacheadd(struct sicache *c, struct sibranch *b)
{
	struct sicachebranch *nb =
		ss_malloc(c->pool->r->a, sizeof(struct sicachebranch));
	if (ssunlikely(nb == NULL))
		return NULL;
	nb->branch  = b;
	nb->ref     = NULL;
	memset(&nb->i, 0, sizeof(nb->i));
	ss_iterinit(sd_read, &nb->i);
	nb->open    = 0;
	nb->next    = NULL;
	ss_bufinit(&nb->buf_a);
	ss_bufinit(&nb->buf_b);
	return nb;
}

static inline int
si_cachevalidate(struct sicache *c, struct sinode *n)
{
	if (sslikely(c->node == n && c->nsn == n->self.id.id))
	{
		if (sslikely(n->branch_count == c->count)) {
			c->branch = c->path;
			return 0;
		}
		assert(n->branch_count > c->count);
		/* c b a */
		/* e d c b a */
		struct sicachebranch *head = NULL;
		struct sicachebranch *last = NULL;
		struct sicachebranch *cb = c->path;
		struct sibranch *b = n->branch;
		while (b) {
			if (cb->branch == b) {
				assert(last != NULL);
				last->next = cb;
				break;
			}
			struct sicachebranch *nb = si_cacheadd(c, b);
			if (ssunlikely(nb == NULL))
				return -1;
			if (! head)
				head = nb;
			if (last)
				last->next = nb;
			last = nb;
			b = b->next;
		}
		c->path   = head;
		c->count  = n->branch_count;
		c->branch = c->path;
		return 0;
	}
	struct sicachebranch *last = c->path;
	struct sicachebranch *cb = last;
	struct sibranch *b = n->branch;
	while (cb && b) {
		cb->branch = b;
		cb->ref = NULL;
		cb->open = 0;
		ss_iterclose(sd_read, &cb->i);
		ss_bufreset(&cb->buf_a);
		ss_bufreset(&cb->buf_b);
		last = cb;
		cb = cb->next;
		b  = b->next;
	}
	while (cb) {
		cb->branch = NULL;
		cb->ref = NULL;
		cb->open = 0;
		ss_iterclose(sd_read, &cb->i);
		ss_bufreset(&cb->buf_a);
		ss_bufreset(&cb->buf_b);
		cb = cb->next;
	}
	while (b) {
		cb = si_cacheadd(c, b);
		if (ssunlikely(cb == NULL))
			return -1;
		if (last)
			last->next = cb;
		last = cb;
		if (c->path == NULL)
			c->path = cb;
		b = b->next;
	}
	c->count  = n->branch_count;
	c->node   = n;
	c->nsn    = n->self.id.id;
	c->branch = c->path;
	return 0;
}

static inline struct sicachebranch*
si_cacheseek(struct sicache *c, struct sibranch *seek)
{
	while (c->branch) {
		struct sicachebranch *cb = c->branch;
		c->branch = c->branch->next;
		if (sslikely(cb->branch == seek))
			return cb;
	}
	return NULL;
}

static inline struct sicachebranch*
si_cachefollow(struct sicache *c, struct sibranch *seek)
{
	while (c->branch) {
		struct sicachebranch *cb = c->branch;
		c->branch = c->branch->next;
		if (sslikely(cb->branch == seek))
			return cb;
	}
	return NULL;
}

static inline void
si_cachepool_init(struct sicachepool *p, struct runtime *r)
{
	p->head = NULL;
	p->n    = 0;
	p->r    = r;
}

static inline void
si_cachepool_free(struct sicachepool *p)
{
	struct sicache *next;
	struct sicache *c = p->head;
	while (c) {
		next = c->next;
		si_cachefree(c);
		ss_free(p->r->a, c);
		c = next;
	}
}

static inline struct sicache*
si_cachepool_pop(struct sicachepool *p)
{
	struct sicache *c;
	if (sslikely(p->n > 0)) {
		c = p->head;
		p->head = c->next;
		p->n--;
		si_cachereset(c);
		c->pool = p;
		return c;
	}
	c = ss_malloc(p->r->a, sizeof(struct sicache));
	if (ssunlikely(c == NULL))
		return NULL;
	si_cacheinit(c, p);
	return c;
}

static inline void
si_cachepool_push(struct sicache *c)
{
	struct sicachepool *p = c->pool;
	c->next = p->head;
	p->head = c;
	p->n++;
}

struct sitx {
	int ro;
	struct rlist nodelist;
	struct si *index;
};

static void si_begin(struct sitx*, struct si*);
static void si_commit(struct sitx*);

static inline void
si_txtrack(struct sitx *x, struct sinode *n) {
	if (rlist_empty(&n->commit))
		rlist_add(&x->nodelist, &n->commit);
}

static void
si_write(struct sitx*, struct svlog*, struct svlogindex*, uint64_t, int);

struct siread {
	enum ssorder   order;
	void     *prefix;
	void     *key;
	uint32_t  keysize;
	uint32_t  prefixsize;
	int       has;
	uint64_t  vlsn;
	struct svmerge   merge;
	int       cache_only;
	int       oldest_only;
	int       read_disk;
	int       read_cache;
	struct sv       *upsert_v;
	int       upsert_eq;
	struct sv        result;
	struct sicache  *cache;
	struct runtime       *r;
	struct si       *index;
};

static int
si_readopen(struct siread*, struct si*, struct sicache*, enum ssorder,
	    uint64_t,
	    void*, uint32_t,
	    void*, uint32_t);
static int si_readclose(struct siread*);
static void si_readcache_only(struct siread*);
static void si_readoldest_only(struct siread*);
static void si_readhas(struct siread*);
static void si_readupsert(struct siread*, struct sv*, int);
static int  si_read(struct siread*);
static int  si_readcommited(struct si*, struct runtime*, struct sv*, int);

struct siiter {
	struct si *index;
	struct ssrbnode *v;
	enum ssorder order;
	void *key;
	int keysize;
} sspacked;

ss_rbget(si_itermatch,
         si_nodecmp(sscast(n, struct sinode, node), key, keysize, scheme))

static inline int
si_iter_open(struct ssiter *i, struct runtime *r,
	     struct si *index, enum ssorder o, void *key, int keysize)
{
	struct siiter *ii = (struct siiter*)i->priv;
	ii->index   = index;
	ii->order   = o;
	ii->key     = key;
	ii->keysize = keysize;
	ii->v       = NULL;
	int eq = 0;
	if (ssunlikely(ii->index->n == 1)) {
		ii->v = ss_rbmin(&ii->index->i);
		return 1;
	}
	if (ssunlikely(ii->key == NULL)) {
		switch (ii->order) {
		case SS_LT:
		case SS_LTE:
			ii->v = ss_rbmax(&ii->index->i);
			break;
		case SS_GT:
		case SS_GTE:
			ii->v = ss_rbmin(&ii->index->i);
			break;
		default:
			assert(0);
			break;
		}
		return 0;
	}
	/* route */
	assert(ii->key != NULL);
	int rc;
	rc = si_itermatch(&ii->index->i, r->scheme, ii->key, ii->keysize, &ii->v);
	if (ssunlikely(ii->v == NULL)) {
		assert(rc != 0);
		if (rc == 1)
			ii->v = ss_rbmin(&ii->index->i);
		else
			ii->v = ss_rbmax(&ii->index->i);
	} else {
		eq = rc == 0 && ii->v;
		if (rc == 1) {
			ii->v = ss_rbprev(&ii->index->i, ii->v);
			if (ssunlikely(ii->v == NULL))
				ii->v = ss_rbmin(&ii->index->i);
		}
	}
	assert(ii->v != NULL);
	return eq;
}

static inline void
si_iter_close(struct ssiter *i ssunused)
{ }

static inline int
si_iter_has(struct ssiter *i)
{
	struct siiter *ii = (struct siiter*)i->priv;
	return ii->v != NULL;
}

static inline void*
si_iter_of(struct ssiter *i)
{
	struct siiter *ii = (struct siiter*)i->priv;
	if (ssunlikely(ii->v == NULL))
		return NULL;
	struct sinode *n = si_nodeof(ii->v);
	return n;
}

static inline void
si_iter_next(struct ssiter *i)
{
	struct siiter *ii = (struct siiter*)i->priv;
	switch (ii->order) {
	case SS_LT:
	case SS_LTE:
		ii->v = ss_rbprev(&ii->index->i, ii->v);
		break;
	case SS_GT:
	case SS_GTE:
		ii->v = ss_rbnext(&ii->index->i, ii->v);
		break;
	default: assert(0);
	}
}

static struct ssiterif si_iter;

static int si_drop(struct si*);
static int si_dropmark(struct si*);
static int si_droprepository(struct runtime*, char*, int);

static int si_anticache(struct si*, struct siplan*);

static int si_snapshot(struct si*, struct siplan*);

static int si_backup(struct si*, struct sdc*, struct siplan*);

static int
si_merge(struct si*, struct sdc*, struct sinode*, uint64_t,
	 uint64_t, struct ssiter*, uint64_t, uint32_t);

static int si_branch(struct si*, struct sdc*, struct siplan*, uint64_t);
static int
si_compact(struct si*, struct sdc*, struct siplan*, uint64_t,
	   uint64_t, struct ssiter*, uint64_t);
static int
si_compact_index(struct si*, struct sdc*, struct siplan*, uint64_t, uint64_t);

struct sitrack {
	struct ssrb i;
	int count;
	uint64_t nsn;
	uint64_t lsn;
};

static inline void
si_trackinit(struct sitrack *t) {
	ss_rbinit(&t->i);
	t->count = 0;
	t->nsn = 0;
	t->lsn = 0;
}

ss_rbtruncate(si_tracktruncate,
              si_nodefree(sscast(n, struct sinode, node), (struct runtime*)arg, 0))

static inline void
si_trackfree(struct sitrack *t, struct runtime *r) {
	if (t->i.root)
		si_tracktruncate(t->i.root, r);
}

static inline void
si_trackmetrics(struct sitrack *t, struct sinode *n)
{
	struct sibranch *b = n->branch;
	while (b) {
		struct sdindexheader *h = b->index.h;
		if (b->id.parent > t->nsn)
			t->nsn = b->id.parent;
		if (b->id.id > t->nsn)
			t->nsn = b->id.id;
		if (h->lsnmin != UINT64_MAX && h->lsnmin > t->lsn)
			t->lsn = h->lsnmin;
		if (h->lsnmax > t->lsn)
			t->lsn = h->lsnmax;
		b = b->next;
	}
}

static inline void
si_tracknsn(struct sitrack *t, uint64_t nsn)
{
	if (t->nsn < nsn)
		t->nsn = nsn;
}

ss_rbget(si_trackmatch, ss_cmp((sscast(n, struct sinode, node))->self.id.id, load_u64(key)))

static inline void
si_trackset(struct sitrack *t, struct sinode *n)
{
	struct ssrbnode *p = NULL;
	int rc = si_trackmatch(&t->i, NULL, (char*)&n->self.id.id,
	                       sizeof(n->self.id.id), &p);
	assert(! (rc == 0 && p));
	ss_rbset(&t->i, p, rc, &n->node);
	t->count++;
}

static inline struct sinode*
si_trackget(struct sitrack *t, uint64_t id)
{
	struct ssrbnode *p = NULL;
	int rc = si_trackmatch(&t->i, NULL, (char*)&id, sizeof(id), &p);
	if (rc == 0 && p)
		return sscast(p, struct sinode, node);
	return NULL;
}

static inline void
si_trackreplace(struct sitrack *t, struct sinode *o, struct sinode *n)
{
	ss_rbreplace(&t->i, &o->node, &n->node);
}

static struct sinode *si_bootstrap(struct si*, uint64_t);
static int si_recover(struct si*);

struct siprofiler {
	uint32_t  total_node_count;
	uint64_t  total_node_size;
	uint64_t  total_node_origin_size;
	uint32_t  total_branch_count;
	uint32_t  total_branch_avg;
	uint32_t  total_branch_max;
	uint32_t  total_page_count;
	uint64_t  total_snapshot_size;
	uint64_t  total_amqf_size;
	uint32_t  temperature_avg;
	uint32_t  temperature_min;
	uint32_t  temperature_max;
	uint64_t  memory_used;
	uint64_t  count;
	uint64_t  count_dup;
	uint64_t  read_disk;
	uint64_t  read_cache;
	int       histogram_branch[20];
	int       histogram_branch_20plus;
	char      histogram_branch_sz[256];
	char     *histogram_branch_ptr;
	char      histogram_temperature_sz[256];
	char     *histogram_temperature_ptr;
	struct si       *i;
} sspacked;

static int si_profilerbegin(struct siprofiler*, struct si*);
static int si_profilerend(struct siprofiler*);
static int si_profiler(struct siprofiler*);

static int si_anticache(struct si *index, struct siplan *plan)
{
	struct sinode *n = plan->node;
	struct runtime *r = &index->r;

	/* promote */
	if (n->flags & SI_PROMOTE) {
		struct sibranch *b = n->branch;
		while (b) {
			int rc;
			rc = si_branchload(b, r, &n->file);
			if (ssunlikely(rc == -1))
				return -1;
			b = b->next;
		}
		si_lock(index);
		n->flags &= ~SI_PROMOTE;
		n->in_memory = 1;
		si_nodeunlock(n);
		si_unlock(index);
		return 0;
	}

	/* revoke */
	assert(n->flags & SI_REVOKE);
	si_lock(index);
	n->flags &= ~SI_REVOKE;
	n->in_memory = 0;
	si_unlock(index);
	struct sibranch *b = n->branch;
	while (b) {
		ss_blobfree(&b->copy);
		ss_blobinit(&b->copy, r->vfs);
		b = b->next;
	}
	si_lock(index);
	si_nodeunlock(n);
	si_unlock(index);
	return 0;
}

static inline int
si_backupend(struct si *index, struct sdc *c, struct siplan *plan)
{
	struct runtime *r = &index->r;
	/* copy index scheme file */
	char src[PATH_MAX];
	snprintf(src, sizeof(src), "%s/scheme", index->scheme.path);

	char dst[PATH_MAX];
	snprintf(dst, sizeof(dst), "%s/%" PRIu32 ".incomplete/%s/scheme",
	         index->scheme.path_backup,
	         (uint32_t)plan->a,
	         index->scheme.name);

	/* prepare buffer */
	ssize_t size = ss_vfssize(r->vfs, src);
	if (ssunlikely(size == -1)) {
		sr_error(r->e, "backup db file '%s' read error: %s",
		         src, strerror(errno));
		return -1;
	}
	int rc = ss_bufensure(&c->c, r->a, size);
	if (ssunlikely(rc == -1))
		return sr_oom(r->e);

	/* read scheme file */
	struct ssfile file;
	ss_fileinit(&file, r->vfs);
	rc = ss_fileopen(&file, src);
	if (ssunlikely(rc == -1)) {
		sr_error(r->e, "backup db file '%s' open error: %s",
		         src, strerror(errno));
		return -1;
	}
	rc = ss_filepread(&file, 0, c->c.s, size);
	if (ssunlikely(rc == -1)) {
		sr_error(r->e, "backup db file '%s' read error: %s",
		         src, strerror(errno));
		ss_fileclose(&file);
		return -1;
	}
	ss_fileclose(&file);

	/* write scheme file */
	ss_fileinit(&file, r->vfs);
	rc = ss_filenew(&file, dst);
	if (ssunlikely(rc == -1)) {
		sr_error(r->e, "backup db file '%s' create error: %s",
		         dst, strerror(errno));
		return -1;
	}
	rc = ss_filewrite(&file, c->c.s, size);
	if (ssunlikely(rc == -1)) {
		sr_error(r->e, "backup db file '%s' write error: %s",
		         dst, strerror(errno));
		ss_fileclose(&file);
		return -1;
	}
	/* sync? */
	rc = ss_fileclose(&file);
	if (ssunlikely(rc == -1)) {
		sr_error(r->e, "backup db file '%s' close error: %s",
		         dst, strerror(errno));
		return -1;
	}

	/* finish index backup */
	si_lock(index);
	index->backup = plan->a;
	si_unlock(index);
	return 0;
}

static int si_backup(struct si *index, struct sdc *c, struct siplan *plan)
{
	struct runtime *r = &index->r;
	if (ssunlikely(plan->plan == SI_BACKUPEND))
		return si_backupend(index, c, plan);

	struct sinode *node = plan->node;
	char dst[PATH_MAX];
	snprintf(dst, sizeof(dst), "%s/%" PRIu32 ".incomplete/%s",
	         index->scheme.path_backup,
	         (uint32_t)plan->a,
	         index->scheme.name);

	/* read origin file */
	int rc = si_noderead(node, r, &c->c);
	if (ssunlikely(rc == -1))
		return -1;

	/* copy */
	struct sspath path;
	ss_path(&path, dst, node->self.id.id, ".db");
	struct ssfile file;
	ss_fileinit(&file, r->vfs);
	rc = ss_filenew(&file, path.path);
	if (ssunlikely(rc == -1)) {
		sr_error(r->e, "backup db file '%s' create error: %s",
		         path.path, strerror(errno));
		return -1;
	}
	rc = ss_filewrite(&file, c->c.s, node->file.size);
	if (ssunlikely(rc == -1)) {
		sr_error(r->e, "backup db file '%s' write error: %s",
				 path.path, strerror(errno));
		ss_fileclose(&file);
		return -1;
	}
	/* sync? */
	rc = ss_fileclose(&file);
	if (ssunlikely(rc == -1)) {
		sr_error(r->e, "backup db file '%s' close error: %s",
				 path.path, strerror(errno));
		return -1;
	}

	si_lock(index);
	node->backup = plan->a;
	si_nodeunlock(node);
	si_unlock(index);
	return 0;
}

static struct si *si_init(struct runtime *r, struct so *object)
{
	struct si *i = ss_malloc(r->a, sizeof(struct si));
	if (ssunlikely(i == NULL))
		return NULL;
	i->r = *r;
	sr_statusinit(&i->status);
	int rc = si_plannerinit(&i->p, r->a, i);
	if (ssunlikely(rc == -1)) {
		ss_free(r->a, i);
		return NULL;
	}
	ss_bufinit(&i->readbuf);
	sv_upsertinit(&i->u);
	ss_rbinit(&i->i);
	tt_pthread_mutex_init(&i->lock, NULL);
	si_schemeinit(&i->scheme);
	rlist_create(&i->link);
	rlist_create(&i->gc);
	i->gc_count     = 0;
	i->update_time  = 0;
	i->lru_run_lsn  = 0;
	i->lru_v        = 0;
	i->lru_steps    = 1;
	i->lru_intr_lsn = 0;
	i->lru_intr_sum = 0;
	i->size         = 0;
	i->read_disk    = 0;
	i->read_cache   = 0;
	i->backup       = 0;
	i->snapshot_run = 0;
	i->snapshot     = 0;
	i->n            = 0;
	tt_pthread_mutex_init(&i->ref_lock, NULL);
	i->ref_fe       = 0;
	i->ref_be       = 0;
	i->object       = object;
	return i;
}

static int si_open(struct si *i)
{
	return si_recover(i);
}

ss_rbtruncate(si_truncate,
              si_nodefree(sscast(n, struct sinode, node), (struct runtime*)arg, 0))

static int si_close(struct si *i)
{
	int rc_ret = 0;
	int rc = 0;
	struct sinode *node, *n;
	rlist_foreach_entry_safe(node, &i->gc, gc, n) {
		rc = si_nodefree(node, &i->r, 1);
		if (ssunlikely(rc == -1))
			rc_ret = -1;
	}
	rlist_create(&i->gc);
	i->gc_count = 0;
	if (i->i.root)
		si_truncate(i->i.root, &i->r);
	i->i.root = NULL;
	sv_upsertfree(&i->u, &i->r);
	ss_buffree(&i->readbuf, i->r.a);
	si_plannerfree(&i->p, i->r.a);
	tt_pthread_mutex_destroy(&i->lock);
	tt_pthread_mutex_destroy(&i->ref_lock);
	sr_statusfree(&i->status);
	si_schemefree(&i->scheme, &i->r);
	ss_free(i->r.a, i);
	return rc_ret;
}

ss_rbget(si_match,
         sf_compare(scheme,
                    sd_indexpage_min(&(sscast(n, struct sinode, node))->self.index,
                                     sd_indexmin(&(sscast(n, struct sinode, node))->self.index)),
                    sd_indexmin(&(sscast(n, struct sinode, node))->self.index)->sizemin,
                                key, keysize))

static int si_insert(struct si *i, struct sinode *n)
{
	struct sdindexpage *min = sd_indexmin(&n->self.index);
	struct ssrbnode *p = NULL;
	int rc = si_match(&i->i, i->r.scheme,
	                  sd_indexpage_min(&n->self.index, min),
	                  min->sizemin, &p);
	assert(! (rc == 0 && p));
	ss_rbset(&i->i, p, rc, &n->node);
	i->n++;
	return 0;
}

static int si_remove(struct si *i, struct sinode *n)
{
	ss_rbremove(&i->i, &n->node);
	i->n--;
	return 0;
}

static int si_replace(struct si *i, struct sinode *o, struct sinode *n)
{
	ss_rbreplace(&i->i, &o->node, &n->node);
	return 0;
}

static int si_refs(struct si *i)
{
	tt_pthread_mutex_lock(&i->ref_lock);
	int v = i->ref_be + i->ref_fe;
	tt_pthread_mutex_unlock(&i->ref_lock);
	return v;
}

static int si_ref(struct si *i, enum siref ref)
{
	tt_pthread_mutex_lock(&i->ref_lock);
	if (ref == SI_REFBE)
		i->ref_be++;
	else
		i->ref_fe++;
	tt_pthread_mutex_unlock(&i->ref_lock);
	return 0;
}

static int si_unref(struct si *i, enum siref ref)
{
	int prev_ref = 0;
	tt_pthread_mutex_lock(&i->ref_lock);
	if (ref == SI_REFBE) {
		prev_ref = i->ref_be;
		if (i->ref_be > 0)
			i->ref_be--;
	} else {
		prev_ref = i->ref_fe;
		if (i->ref_fe > 0)
			i->ref_fe--;
	}
	tt_pthread_mutex_unlock(&i->ref_lock);
	return prev_ref;
}

static int si_plan(struct si *i, struct siplan *plan)
{
	si_lock(i);
	int rc = si_planner(&i->p, plan);
	si_unlock(i);
	return rc;
}

static int
si_execute(struct si *i, struct sdc *c, struct siplan *plan,
	   uint64_t vlsn, uint64_t vlsn_lru)
{
	int rc = -1;
	switch (plan->plan) {
	case SI_NODEGC:
		rc = si_nodefree(plan->node, &i->r, 1);
		break;
	case SI_CHECKPOINT:
	case SI_BRANCH:
	case SI_AGE:
		rc = si_branch(i, c, plan, vlsn);
		break;
	case SI_LRU:
	case SI_EXPIRE:
	case SI_GC:
	case SI_COMPACT:
		rc = si_compact(i, c, plan, vlsn, vlsn_lru, NULL, 0);
		break;
	case SI_COMPACT_INDEX:
		rc = si_compact_index(i, c, plan, vlsn, vlsn_lru);
		break;
	case SI_ANTICACHE:
		rc = si_anticache(i, plan);
		break;
	case SI_SNAPSHOT:
		rc = si_snapshot(i, plan);
		break;
	case SI_BACKUP:
	case SI_BACKUPEND:
		rc = si_backup(i, c, plan);
		break;
	case SI_SHUTDOWN:
		rc = si_close(i);
		break;
	case SI_DROP:
		rc = si_drop(i);
		break;
	}
	/* garbage collect buffers */
	if (plan->plan != SI_SHUTDOWN &&
	    plan->plan != SI_DROP) {
		sd_cgc(c, &i->r, i->scheme.buf_gc_wm);
	}
	return rc;
}

static inline int
si_branchcreate(struct si *index, struct sdc *c,
		struct sinode *parent, struct svindex *vindex,
		uint64_t vlsn, struct sibranch **result)
{
	struct runtime *r = &index->r;
	struct sibranch *branch = NULL;

	/* in-memory mode blob */
	int rc;
	struct ssblob copy, *blob = NULL;
	if (parent->in_memory) {
		ss_blobinit(&copy, r->vfs);
		rc = ss_blobensure(&copy, 10ULL * 1024 * 1024);
		if (ssunlikely(rc == -1))
			return sr_oom_malfunction(r->e);
		blob = &copy;
	}

	struct svmerge vmerge;
	sv_mergeinit(&vmerge);
	rc = sv_mergeprepare(&vmerge, r, 1);
	if (ssunlikely(rc == -1))
		return -1;
	struct svmergesrc *s = sv_mergeadd(&vmerge, NULL);
	ss_iterinit(sv_indexiter, &s->src);
	ss_iteropen(sv_indexiter, &s->src, r, vindex, SS_GTE, NULL, 0);
	struct ssiter i;
	ss_iterinit(sv_mergeiter, &i);
	ss_iteropen(sv_mergeiter, &i, r, &vmerge, SS_GTE);

	/* merge iter is not used */
	uint32_t timestamp = time(NULL);
	struct sdmergeconf mergeconf = {
		.stream          = vindex->count,
		.size_stream     = UINT32_MAX,
		.size_node       = UINT64_MAX,
		.size_page       = index->scheme.node_page_size,
		.checksum        = index->scheme.node_page_checksum,
		.expire          = index->scheme.expire,
		.timestamp       = timestamp,
		.compression_key = index->scheme.compression_key,
		.compression     = index->scheme.compression_branch,
		.compression_if  = index->scheme.compression_branch_if,
		.amqf            = index->scheme.amqf,
		.vlsn            = vlsn,
		.vlsn_lru        = 0,
		.save_delete     = 1,
		.save_upsert     = 1
	};
	struct sdmerge merge;
	rc = sd_mergeinit(&merge, r, &i, &c->build, &c->qf,
	                  &c->upsert, &mergeconf);
	if (ssunlikely(rc == -1))
		return -1;

	while ((rc = sd_merge(&merge)) > 0)
	{
		assert(branch == NULL);

		/* write open seal */
		uint64_t seal = parent->file.size;
		rc = sd_writeseal(r, &parent->file, blob);
		if (ssunlikely(rc == -1))
			goto e0;

		/* write pages */
		uint64_t offset = parent->file.size;
		while ((rc = sd_mergepage(&merge, offset)) == 1)
		{
			rc = sd_writepage(r, &parent->file, blob, merge.build);
			if (ssunlikely(rc == -1))
				goto e0;
			offset = parent->file.size;
		}
		if (ssunlikely(rc == -1))
			goto e0;
		struct sdid id = {
			.parent = parent->self.id.id,
			.flags  = SD_IDBRANCH,
			.id     = sr_seq(r->seq, SR_NSNNEXT)
		};
		rc = sd_mergecommit(&merge, &id, parent->file.size);
		if (ssunlikely(rc == -1))
			goto e0;

		/* write index */
		rc = sd_writeindex(r, &parent->file, blob, &merge.index);
		if (ssunlikely(rc == -1))
			goto e0;
		if (index->scheme.sync) {
			rc = ss_filesync(&parent->file);
			if (ssunlikely(rc == -1)) {
				sr_malfunction(r->e, "file '%s' sync error: %s",
				               ss_pathof(&parent->file.path),
				               strerror(errno));
				goto e0;
			}
		}

		SS_INJECTION(r->i, SS_INJECTION_SI_BRANCH_0,
		             sd_mergefree(&merge);
		             sr_malfunction(r->e, "%s", "error injection");
		             return -1);

		/* seal the branch */
		rc = sd_seal(r, &parent->file, blob, &merge.index, seal);
		if (ssunlikely(rc == -1))
			goto e0;
		if (index->scheme.sync == 2) {
			rc = ss_filesync(&parent->file);
			if (ssunlikely(rc == -1)) {
				sr_malfunction(r->e, "file '%s' sync error: %s",
				               ss_pathof(&parent->file.path),
				               strerror(errno));
				goto e0;
			}
		}

		/* create new branch object */
		branch = si_branchnew(r);
		if (ssunlikely(branch == NULL))
			goto e0;
		si_branchset(branch, &merge.index);
	}
	sv_mergefree(&vmerge, r->a);

	if (ssunlikely(rc == -1)) {
		sr_oom_malfunction(r->e);
		goto e0;
	}

	/* in case of expire, branch may not be created if there
	 * are no keys left */
	if (ssunlikely(branch == NULL))
		return 0;

	/* in-memory mode support */
	if (blob) {
		rc = ss_blobfit(blob);
		if (ssunlikely(rc == -1)) {
			ss_blobfree(blob);
			goto e1;
		}
		branch->copy = copy;
	}
	/* mmap support */
	if (index->scheme.mmap) {
		ss_mmapinit(&parent->map_swap);
		rc = ss_vfsmmap(r->vfs, &parent->map_swap, parent->file.fd,
		              parent->file.size, 1);
		if (ssunlikely(rc == -1)) {
			sr_malfunction(r->e, "db file '%s' mmap error: %s",
			               ss_pathof(&parent->file.path),
			               strerror(errno));
			goto e1;
		}
	}

	*result = branch;
	return 0;
e0:
	sd_mergefree(&merge);
	if (blob)
		ss_blobfree(blob);
		sv_mergefree(&vmerge, r->a);
	return -1;
e1:
	si_branchfree(branch, r);
	return -1;
}

static int
si_branch(struct si *index, struct sdc *c, struct siplan *plan, uint64_t vlsn)
{
	struct runtime *r = &index->r;
	struct sinode *n = plan->node;
	assert(n->flags & SI_LOCK);

	si_lock(index);
	if (ssunlikely(n->used == 0)) {
		si_nodeunlock(n);
		si_unlock(index);
		return 0;
	}
	struct svindex *i;
	i = si_noderotate(n);
	si_unlock(index);

	struct sibranch *branch = NULL;
	int rc = si_branchcreate(index, c, n, i, vlsn, &branch);
	if (ssunlikely(rc == -1))
		return -1;
	if (ssunlikely(branch == NULL)) {
		si_lock(index);
		uint32_t used = sv_indexused(i);
		n->used -= used;
		ss_quota(r->quota, SS_QREMOVE, used);
		struct svindex swap = *i;
		si_nodeunrotate(n);
		si_nodeunlock(n);
		si_plannerupdate(&index->p, SI_BRANCH|SI_COMPACT, n);
		si_unlock(index);
		si_nodegc_index(r, &swap);
		return 0;
	}

	/* commit */
	si_lock(index);
	branch->next = n->branch;
	n->branch->link = branch;
	n->branch = branch;
	n->branch_count++;
	uint32_t used = sv_indexused(i);
	n->used -= used;
	ss_quota(r->quota, SS_QREMOVE, used);
	index->size +=
		sd_indexsize_ext(branch->index.h) +
		sd_indextotal(&branch->index);
	struct svindex swap = *i;
	si_nodeunrotate(n);
	si_nodeunlock(n);
	si_plannerupdate(&index->p, SI_BRANCH|SI_COMPACT, n);
	struct ssmmap swap_map = n->map;
	n->map = n->map_swap;
	memset(&n->map_swap, 0, sizeof(n->map_swap));
	si_unlock(index);

	/* gc */
	if (index->scheme.mmap) {
		int rc = ss_vfsmunmap(r->vfs, &swap_map);
		if (ssunlikely(rc == -1)) {
			sr_malfunction(r->e, "db file '%s' munmap error: %s",
			               ss_pathof(&n->file.path),
			               strerror(errno));
			return -1;
		}
	}
	si_nodegc_index(r, &swap);
	return 1;
}

static int
si_compact(struct si *index, struct sdc *c, struct siplan *plan,
	   uint64_t vlsn,
	   uint64_t vlsn_lru,
	   struct ssiter *vindex,
	   uint64_t vindex_used)
{
	struct runtime *r = &index->r;
	struct sinode *node = plan->node;
	assert(node->flags & SI_LOCK);

	/* prepare for compaction */
	int rc;
	rc = sd_censure(c, r, node->branch_count);
	if (ssunlikely(rc == -1))
		return sr_oom_malfunction(r->e);
	struct svmerge merge;
	sv_mergeinit(&merge);
	rc = sv_mergeprepare(&merge, r, node->branch_count + 1);
	if (ssunlikely(rc == -1))
		return -1;

	/* read node file into memory */
	int use_mmap = index->scheme.mmap;
	struct ssmmap *map = &node->map;
	struct ssmmap  preload;
	if (index->scheme.node_compact_load) {
		rc = si_noderead(node, r, &c->c);
		if (ssunlikely(rc == -1))
			return -1;
		preload.p = c->c.s;
		preload.size = ss_bufused(&c->c);
		map = &preload;
		use_mmap = 1;
	}

	/* include vindex into merge process */
	struct svmergesrc *s;
	uint32_t count = 0;
	uint64_t size_stream = 0;
	if (vindex) {
		s = sv_mergeadd(&merge, vindex);
		size_stream = vindex_used;
	}

	struct sdcbuf *cbuf = c->head;
	struct sibranch *b = node->branch;
	while (b) {
		s = sv_mergeadd(&merge, NULL);
		/* choose compression type */
		int compression;
		struct ssfilterif *compression_if;
		if (! si_branchis_root(b)) {
			compression    = index->scheme.compression_branch;
			compression_if = index->scheme.compression_branch_if;
		} else {
			compression    = index->scheme.compression;
			compression_if = index->scheme.compression_if;
		}
		struct sdreadarg arg = {
			.index           = &b->index,
			.buf             = &cbuf->a,
			.buf_xf          = &cbuf->b,
			.buf_read        = &c->d,
			.index_iter      = &cbuf->index_iter,
			.page_iter       = &cbuf->page_iter,
			.use_memory      = node->in_memory,
			.use_mmap        = use_mmap,
			.use_mmap_copy   = 0,
			.use_compression = compression,
			.compression_if  = compression_if,
			.has             = 0,
			.has_vlsn        = 0,
			.o               = SS_GTE,
			.memory          = &b->copy,
			.mmap            = map,
			.file            = &node->file,
			.r               = r
		};
		ss_iterinit(sd_read, &s->src);
		int rc = ss_iteropen(sd_read, &s->src, &arg, NULL, 0);
		if (ssunlikely(rc == -1))
			return sr_oom_malfunction(r->e);
		size_stream += sd_indextotal(&b->index);
		count += sd_indexkeys(&b->index);
		cbuf = cbuf->next;
		b = b->next;
	}
	struct ssiter i;
	ss_iterinit(sv_mergeiter, &i);
	ss_iteropen(sv_mergeiter, &i, r, &merge, SS_GTE);
	rc = si_merge(index, c, node, vlsn, vlsn_lru, &i, size_stream, count);
	sv_mergefree(&merge, r->a);
	return rc;
}

static int
si_compact_index(struct si *index, struct sdc *c, struct siplan *plan,
		 uint64_t vlsn,
		 uint64_t vlsn_lru)
{
	struct sinode *node = plan->node;

	si_lock(index);
	if (ssunlikely(node->used == 0)) {
		si_nodeunlock(node);
		si_unlock(index);
		return 0;
	}
	struct svindex *vindex;
	vindex = si_noderotate(node);
	si_unlock(index);

	uint64_t size_stream = sv_indexused(vindex);
	struct ssiter i;
	ss_iterinit(sv_indexiter, &i);
	ss_iteropen(sv_indexiter, &i, &index->r, vindex, SS_GTE, NULL, 0);
	return si_compact(index, c, plan, vlsn, vlsn_lru, &i, size_stream);
}

static int si_droprepository(struct runtime *r, char *repo, int drop_directory)
{
	DIR *dir = opendir(repo);
	if (dir == NULL) {
		sr_malfunction(r->e, "directory '%s' open error: %s",
		               repo, strerror(errno));
		return -1;
	}
	char path[1024];
	int rc;
	struct dirent *de;
	while ((de = readdir(dir))) {
		if (de->d_name[0] == '.')
			continue;
		/* skip drop file */
		if (ssunlikely(strcmp(de->d_name, "drop") == 0))
			continue;
		snprintf(path, sizeof(path), "%s/%s", repo, de->d_name);
		rc = ss_vfsunlink(r->vfs, path);
		if (ssunlikely(rc == -1)) {
			sr_malfunction(r->e, "db file '%s' unlink error: %s",
			               path, strerror(errno));
			closedir(dir);
			return -1;
		}
	}
	closedir(dir);

	snprintf(path, sizeof(path), "%s/drop", repo);
	rc = ss_vfsunlink(r->vfs, path);
	if (ssunlikely(rc == -1)) {
		sr_malfunction(r->e, "db file '%s' unlink error: %s",
		               path, strerror(errno));
		return -1;
	}
	if (drop_directory) {
		rc = ss_vfsrmdir(r->vfs, repo);
		if (ssunlikely(rc == -1)) {
			sr_malfunction(r->e, "directory '%s' unlink error: %s",
			               repo, strerror(errno));
			return -1;
		}
	}
	return 0;
}

static int si_dropmark(struct si *i)
{
	/* create drop file */
	char path[1024];
	snprintf(path, sizeof(path), "%s/drop", i->scheme.path);
	struct ssfile drop;
	ss_fileinit(&drop, i->r.vfs);
	int rc = ss_filenew(&drop, path);
	if (ssunlikely(rc == -1)) {
		sr_malfunction(i->r.e, "drop file '%s' create error: %s",
		               path, strerror(errno));
		return -1;
	}
	ss_fileclose(&drop);
	return 0;
}

static int si_drop(struct si *i)
{
	struct runtime r = i->r;
	struct sspath path;
	ss_pathinit(&path);
	ss_pathset(&path, "%s", i->scheme.path);
	/* drop file must exists at this point */
	/* shutdown */
	int rc = si_close(i);
	if (ssunlikely(rc == -1))
		return -1;
	/* remove directory */
	rc = si_droprepository(&r, path.path, 1);
	return rc;
}

static uint32_t si_gcv(struct runtime *r, struct svv *v)
{
	uint32_t size = sv_vsize(v);
	struct sl *log = (struct sl*)v->log;
	if (sv_vunref(r, v)) {
		if (log)
			ss_gcsweep(&log->gc, 1);
		return size;
	}
	return 0;
}

static uint32_t si_gcref(struct runtime *r, struct svref *gc)
{
	uint32_t used = 0;
	struct svref *v = gc;
	while (v) {
		struct svref *n = v->next;
		uint32_t size = sv_vsize(v->v);
		if (si_gcv(r, v->v))
			used += size;
		ss_free(r->aref, v);
		v = n;
	}
	return used;
}

static struct ssiterif si_iter =
{
	.close = si_iter_close,
	.has   = si_iter_has,
	.of    = si_iter_of,
	.next  = si_iter_next
};

static int
si_redistribute(struct si *index, struct runtime *r, struct sdc *c, struct sinode *node, struct ssbuf *result)
{
	(void)index;
	struct svindex *vindex = si_nodeindex(node);
	struct ssiter i;
	ss_iterinit(sv_indexiter, &i);
	ss_iteropen(sv_indexiter, &i, r, vindex, SS_GTE, NULL, 0);
	while (ss_iterhas(sv_indexiter, &i))
	{
		struct sv *v = ss_iterof(sv_indexiter, &i);
		int rc = ss_bufadd(&c->b, r->a, &v->v, sizeof(struct svref**));
		if (ssunlikely(rc == -1))
			return sr_oom_malfunction(r->e);
		ss_iternext(sv_indexiter, &i);
	}
	if (ssunlikely(ss_bufused(&c->b) == 0))
		return 0;
	ss_iterinit(ss_bufiterref, &i);
	ss_iteropen(ss_bufiterref, &i, &c->b, sizeof(struct svref*));
	struct ssiter j;
	ss_iterinit(ss_bufiterref, &j);
	ss_iteropen(ss_bufiterref, &j, result, sizeof(struct sinode*));
	struct sinode *prev = ss_iterof(ss_bufiterref, &j);
	ss_iternext(ss_bufiterref, &j);
	while (1)
	{
		struct sinode *p = ss_iterof(ss_bufiterref, &j);
		if (p == NULL) {
			assert(prev != NULL);
			while (ss_iterhas(ss_bufiterref, &i)) {
				struct svref *v = ss_iterof(ss_bufiterref, &i);
				v->next = NULL;
				sv_indexset(&prev->i0, r, v);
				ss_iternext(ss_bufiterref, &i);
			}
			break;
		}
		while (ss_iterhas(ss_bufiterref, &i))
		{
			struct svref *v = ss_iterof(ss_bufiterref, &i);
			v->next = NULL;
			struct sdindexpage *page = sd_indexmin(&p->self.index);
			int rc = sf_compare(r->scheme, sv_vpointer(v->v), v->v->size,
			                    sd_indexpage_min(&p->self.index, page),
			                    page->sizemin);
			if (ssunlikely(rc >= 0))
				break;
			sv_indexset(&prev->i0, r, v);
			ss_iternext(ss_bufiterref, &i);
		}
		if (ssunlikely(! ss_iterhas(ss_bufiterref, &i)))
			break;
		prev = p;
		ss_iternext(ss_bufiterref, &j);
	}
	assert(ss_iterof(ss_bufiterref, &i) == NULL);
	return 0;
}

static inline void
si_redistribute_set(struct si *index, struct runtime *r, uint64_t now, struct svref *v)
{
	index->update_time = now;
	/* match node */
	struct ssiter i;
	ss_iterinit(si_iter, &i);
	ss_iteropen(si_iter, &i, r, index, SS_GTE, sv_vpointer(v->v), v->v->size);
	struct sinode *node = ss_iterof(si_iter, &i);
	assert(node != NULL);
	/* update node */
	struct svindex *vindex = si_nodeindex(node);
	sv_indexset(vindex, r, v);
	node->update_time = index->update_time;
	node->used += sv_vsize(v->v);
	/* schedule node */
	si_plannerupdate(&index->p, SI_BRANCH, node);
}

static int
si_redistribute_index(struct si *index, struct runtime *r, struct sdc *c, struct sinode *node)
{
	struct svindex *vindex = si_nodeindex(node);
	struct ssiter i;
	ss_iterinit(sv_indexiter, &i);
	ss_iteropen(sv_indexiter, &i, r, vindex, SS_GTE, NULL, 0);
	while (ss_iterhas(sv_indexiter, &i)) {
		struct sv *v = ss_iterof(sv_indexiter, &i);
		int rc = ss_bufadd(&c->b, r->a, &v->v, sizeof(struct svref**));
		if (ssunlikely(rc == -1))
			return sr_oom_malfunction(r->e);
		ss_iternext(sv_indexiter, &i);
	}
	if (ssunlikely(ss_bufused(&c->b) == 0))
		return 0;
	uint64_t now = clock_monotonic64();
	ss_iterinit(ss_bufiterref, &i);
	ss_iteropen(ss_bufiterref, &i, &c->b, sizeof(struct svref*));
	while (ss_iterhas(ss_bufiterref, &i)) {
		struct svref *v = ss_iterof(ss_bufiterref, &i);
		v->next = NULL;
		si_redistribute_set(index, r, now, v);
		ss_iternext(ss_bufiterref, &i);
	}
	return 0;
}

static int
si_splitfree(struct ssbuf *result, struct runtime *r)
{
	struct ssiter i;
	ss_iterinit(ss_bufiterref, &i);
	ss_iteropen(ss_bufiterref, &i, result, sizeof(struct sinode*));
	while (ss_iterhas(ss_bufiterref, &i))
	{
		struct sinode *p = ss_iterof(ss_bufiterref, &i);
		si_nodefree(p, r, 0);
		ss_iternext(ss_bufiterref, &i);
	}
	return 0;
}

static inline int
si_split(struct si *index, struct sdc *c, struct ssbuf *result,
         struct sinode   *parent,
         struct ssiter   *i,
         uint64_t  size_node,
         uint64_t  size_stream,
         uint32_t  stream,
         uint64_t  vlsn,
         uint64_t  vlsn_lru)
{
	struct runtime *r = &index->r;
	uint32_t timestamp = time(NULL);
	int rc;
	struct sdmergeconf mergeconf = {
		.stream          = stream,
		.size_stream     = size_stream,
		.size_node       = size_node,
		.size_page       = index->scheme.node_page_size,
		.checksum        = index->scheme.node_page_checksum,
		.expire          = index->scheme.expire,
		.timestamp       = timestamp,
		.compression_key = index->scheme.compression_key,
		.compression     = index->scheme.compression,
		.compression_if  = index->scheme.compression_if,
		.amqf            = index->scheme.amqf,
		.vlsn            = vlsn,
		.vlsn_lru        = vlsn_lru,
		.save_delete     = 0,
		.save_upsert     = 0
	};
	struct sinode *n = NULL;
	struct sdmerge merge;
	rc = sd_mergeinit(&merge, r, i, &c->build, &c->qf, &c->upsert, &mergeconf);
	if (ssunlikely(rc == -1))
		return -1;
	while ((rc = sd_merge(&merge)) > 0)
	{
		/* create new node */
		n = si_nodenew(r);
		if (ssunlikely(n == NULL))
			goto error;
		struct sdid id = {
			.parent = parent->self.id.id,
			.flags  = 0,
			.id     = sr_seq(index->r.seq, SR_NSNNEXT)
		};
		rc = si_nodecreate(n, r, &index->scheme, &id);
		if (ssunlikely(rc == -1))
			goto error;
		n->branch = &n->self;
		n->branch_count++;

		struct ssblob *blob = NULL;
		if (parent->in_memory) {
			blob = &n->self.copy;
			rc = ss_blobensure(blob, index->scheme.node_size);
			if (ssunlikely(rc == -1))
				goto error;
			n->in_memory = 1;
		}

		/* write open seal */
		uint64_t seal = n->file.size;
		rc = sd_writeseal(r, &n->file, blob);
		if (ssunlikely(rc == -1))
			goto error;

		/* write pages */
		uint64_t offset = n->file.size;
		while ((rc = sd_mergepage(&merge, offset)) == 1) {
			rc = sd_writepage(r, &n->file, blob, merge.build);
			if (ssunlikely(rc == -1))
				goto error;
			offset = n->file.size;
		}
		if (ssunlikely(rc == -1))
			goto error;

		rc = sd_mergecommit(&merge, &id, n->file.size);
		if (ssunlikely(rc == -1))
			goto error;

		/* write index */
		rc = sd_writeindex(r, &n->file, blob, &merge.index);
		if (ssunlikely(rc == -1))
			goto error;

		/* update seal */
		rc = sd_seal(r, &n->file, blob, &merge.index, seal);
		if (ssunlikely(rc == -1))
			goto error;

		/* in-memory mode */
		if (blob) {
			rc = ss_blobfit(blob);
			if (ssunlikely(rc == -1))
				goto error;
		}
		/* mmap mode */
		if (index->scheme.mmap) {
			rc = si_nodemap(n, r);
			if (ssunlikely(rc == -1))
				goto error;
		}

		/* add node to the list */
		rc = ss_bufadd(result, index->r.a, &n, sizeof(struct sinode*));
		if (ssunlikely(rc == -1)) {
			sr_oom_malfunction(index->r.e);
			goto error;
		}

		si_branchset(&n->self, &merge.index);
	}
	if (ssunlikely(rc == -1))
		goto error;
	return 0;
error:
	if (n)
		si_nodefree(n, r, 0);
	sd_mergefree(&merge);
	si_splitfree(result, r);
	return -1;
}

static int
si_merge(struct si *index, struct sdc *c, struct sinode *node,
	 uint64_t vlsn,
	 uint64_t vlsn_lru,
	 struct ssiter *stream,
	 uint64_t size_stream,
	 uint32_t n_stream)
{
	struct runtime *r = &index->r;
	struct ssbuf *result = &c->a;
	struct ssiter i;

	/* begin compaction.
	 *
	 * Split merge stream into a number of
	 * a new nodes.
	 */
	int rc;
	rc = si_split(index, c, result,
	              node, stream,
	              index->scheme.node_size,
	              size_stream,
	              n_stream,
	              vlsn,
	              vlsn_lru);
	if (ssunlikely(rc == -1))
		return -1;

	SS_INJECTION(r->i, SS_INJECTION_SI_COMPACTION_0,
	             si_splitfree(result, r);
	             sr_malfunction(r->e, "%s", "error injection");
	             return -1);

	/* mask removal of a single node as a
	 * single node update */
	int count = ss_bufused(result) / sizeof(struct sinode*);
	int count_index;

	si_lock(index);
	count_index = index->n;
	si_unlock(index);

	struct sinode *n;
	if (ssunlikely(count == 0 && count_index == 1))
	{
		n = si_bootstrap(index, node->self.id.id);
		if (ssunlikely(n == NULL))
			return -1;
		rc = ss_bufadd(result, r->a, &n, sizeof(struct sinode*));
		if (ssunlikely(rc == -1)) {
			sr_oom_malfunction(r->e);
			si_nodefree(n, r, 1);
			return -1;
		}
		count++;
	}

	/* commit compaction changes */
	si_lock(index);
	struct svindex *j = si_nodeindex(node);
	si_plannerremove(&index->p, SI_COMPACT|SI_BRANCH|SI_TEMP, node);
	si_nodesplit(node);
	index->size -= si_nodesize(node);
	switch (count) {
	case 0: /* delete */
		si_remove(index, node);
		si_redistribute_index(index, r, c, node);
		break;
	case 1: /* self update */
		n = *(struct sinode**)result->s;
		n->i0 = *j;
		n->temperature = node->temperature;
		n->temperature_reads = node->temperature_reads;
		n->used = sv_indexused(j);
		index->size += si_nodesize(n);
		si_nodelock(n);
		si_replace(index, node, n);
		si_plannerupdate(&index->p, SI_COMPACT|SI_BRANCH|SI_TEMP, n);
		break;
	default: /* split */
		rc = si_redistribute(index, r, c, node, result);
		if (ssunlikely(rc == -1)) {
			si_unlock(index);
			si_splitfree(result, r);
			return -1;
		}
		ss_iterinit(ss_bufiterref, &i);
		ss_iteropen(ss_bufiterref, &i, result, sizeof(struct sinode*));
		n = ss_iterof(ss_bufiterref, &i);
		n->used = sv_indexused(&n->i0);
		n->temperature = node->temperature;
		n->temperature_reads = node->temperature_reads;
		index->size += si_nodesize(n);
		si_nodelock(n);
		si_replace(index, node, n);
		si_plannerupdate(&index->p, SI_COMPACT|SI_BRANCH|SI_TEMP, n);
		for (ss_iternext(ss_bufiterref, &i); ss_iterhas(ss_bufiterref, &i);
		     ss_iternext(ss_bufiterref, &i)) {
			n = ss_iterof(ss_bufiterref, &i);
			n->used = sv_indexused(&n->i0);
			n->temperature = node->temperature;
			n->temperature_reads = node->temperature_reads;
			index->size += si_nodesize(n);
			si_nodelock(n);
			si_insert(index, n);
			si_plannerupdate(&index->p, SI_COMPACT|SI_BRANCH|SI_TEMP, n);
		}
		break;
	}
	sv_indexinit(j);
	si_unlock(index);

	/* compaction completion */

	/* seal nodes */
	ss_iterinit(ss_bufiterref, &i);
	ss_iteropen(ss_bufiterref, &i, result, sizeof(struct sinode*));
	while (ss_iterhas(ss_bufiterref, &i))
	{
		n  = ss_iterof(ss_bufiterref, &i);
		rc = si_nodeseal(n, r, &index->scheme);
		if (ssunlikely(rc == -1)) {
			si_nodefree(node, r, 0);
			return -1;
		}
		SS_INJECTION(r->i, SS_INJECTION_SI_COMPACTION_3,
		             si_nodefree(node, r, 0);
		             sr_malfunction(r->e, "%s", "error injection");
		             return -1);
		ss_iternext(ss_bufiterref, &i);
	}

	SS_INJECTION(r->i, SS_INJECTION_SI_COMPACTION_1,
	             si_nodefree(node, r, 0);
	             sr_malfunction(r->e, "%s", "error injection");
	             return -1);

	/* gc node */
	uint16_t refs = si_noderefof(node);
	if (sslikely(refs == 0)) {
		rc = si_nodefree(node, r, 1);
		if (ssunlikely(rc == -1))
			return -1;
	} else {
		/* node concurrently being read, schedule for
		 * delayed removal */
		si_nodegc(node, r, &index->scheme);
		si_lock(index);
		rlist_add(&index->gc, &node->gc);
		index->gc_count++;
		si_unlock(index);
	}

	SS_INJECTION(r->i, SS_INJECTION_SI_COMPACTION_2,
	             sr_malfunction(r->e, "%s", "error injection");
	             return -1);

	/* complete new nodes */
	ss_iterinit(ss_bufiterref, &i);
	ss_iteropen(ss_bufiterref, &i, result, sizeof(struct sinode*));
	while (ss_iterhas(ss_bufiterref, &i))
	{
		n = ss_iterof(ss_bufiterref, &i);
		rc = si_nodecomplete(n, r, &index->scheme);
		if (ssunlikely(rc == -1))
			return -1;
		SS_INJECTION(r->i, SS_INJECTION_SI_COMPACTION_4,
		             sr_malfunction(r->e, "%s", "error injection");
		             return -1);
		ss_iternext(ss_bufiterref, &i);
	}

	/* unlock */
	si_lock(index);
	ss_iterinit(ss_bufiterref, &i);
	ss_iteropen(ss_bufiterref, &i, result, sizeof(struct sinode*));
	while (ss_iterhas(ss_bufiterref, &i))
	{
		n = ss_iterof(ss_bufiterref, &i);
		si_nodeunlock(n);
		ss_iternext(ss_bufiterref, &i);
	}
	si_unlock(index);
	return 0;
}

static struct sinode *si_nodenew(struct runtime *r)
{
	struct sinode *n = (struct sinode*)ss_malloc(r->a, sizeof(struct sinode));
	if (ssunlikely(n == NULL)) {
		sr_oom_malfunction(r->e);
		return NULL;
	}
	n->recover = 0;
	n->backup = 0;
	n->lru = 0;
	n->ac = 0;
	n->flags = 0;
	n->update_time = 0;
	n->used = 0;
	n->in_memory = 0;
	si_branchinit(&n->self, r);
	n->branch = NULL;
	n->branch_count = 0;
	n->temperature = 0;
	n->temperature_reads = 0;
	n->refs = 0;
	tt_pthread_mutex_init(&n->reflock, NULL);
	ss_fileinit(&n->file, r->vfs);
	ss_mmapinit(&n->map);
	ss_mmapinit(&n->map_swap);
	sv_indexinit(&n->i0);
	sv_indexinit(&n->i1);
	ss_rbinitnode(&n->node);
	ss_rqinitnode(&n->nodecompact);
	ss_rqinitnode(&n->nodebranch);
	ss_rqinitnode(&n->nodetemp);
	rlist_create(&n->gc);
	rlist_create(&n->commit);
	return n;
}

ss_rbtruncate(si_nodegc_indexgc,
              si_gcref((struct runtime*)arg, sscast(n, struct svref, node)))

static int si_nodegc_index(struct runtime *r, struct svindex *i)
{
	if (i->i.root)
		si_nodegc_indexgc(i->i.root, r);
	sv_indexinit(i);
	return 0;
}

static inline int
si_nodeclose(struct sinode *n, struct runtime *r, int gc)
{
	int rcret = 0;

	int rc = ss_vfsmunmap(r->vfs, &n->map);
	if (ssunlikely(rc == -1)) {
		sr_malfunction(r->e, "db file '%s' munmap error: %s",
		               ss_pathof(&n->file.path),
		               strerror(errno));
		rcret = -1;
	}
	rc = ss_fileclose(&n->file);
	if (ssunlikely(rc == -1)) {
		sr_malfunction(r->e, "db file '%s' close error: %s",
		               ss_pathof(&n->file.path),
		               strerror(errno));
		rcret = -1;
	}
	if (gc) {
		si_nodegc_index(r, &n->i0);
		si_nodegc_index(r, &n->i1);
	} else {
		sv_indexfree(&n->i0, r);
		sv_indexfree(&n->i1, r);
		tt_pthread_mutex_destroy(&n->reflock);
	}
	return rcret;
}

static inline int
si_noderecover_snapshot(struct sinode *n, struct runtime *r, struct sdsnapshotnode *sn)
{
	char *p = (char*)sn + sizeof(struct sdsnapshotnode);
	uint32_t i = 0;
	int first = 1;
	int rc;
	while (i < sn->branch_count) {
		struct sdindexheader *h = (struct sdindexheader*)p;
		struct sibranch *b;
		if (first) {
			b = &n->self;
		} else {
			b = si_branchnew(r);
			if (ssunlikely(b == NULL))
				return -1;
		}

		struct sdindex index;
		sd_indexinit(&index);
		rc = sd_indexcopy(&index, r, h);
		if (ssunlikely(rc == -1)) {
			if (! first)
				si_branchfree(b, r);
			return -1;
		}
		si_branchset(b, &index);

		b->next   = n->branch;
		n->branch = b;
		n->branch_count++;
		first = 0;
		p += sd_indexsize_ext(h);
		i++;
	}
	return 0;
}

static inline int
si_noderecover(struct sinode *n, struct runtime *r, struct sdsnapshotnode *sn, int in_memory)
{
	/* fast recover from snapshot file */
	if (sn) {
		n->temperature_reads = sn->temperature_reads;
		if (! in_memory)
			return si_noderecover_snapshot(n, r, sn);
	}

	/* recover branches */
	struct sibranch *b = NULL;
	struct ssiter i;
	ss_iterinit(sd_recover, &i);
	ss_iteropen(sd_recover, &i, r, &n->file);
	int first = 1;
	int rc;
	while (ss_iteratorhas(&i))
	{
		struct sdindexheader *h = ss_iteratorof(&i);
		if (first) {
			b = &n->self;
		} else {
			b = si_branchnew(r);
			if (ssunlikely(b == NULL))
				goto e0;
		}
		struct sdindex index;
		sd_indexinit(&index);
		rc = sd_indexcopy(&index, r, h);
		if (ssunlikely(rc == -1))
			goto e0;
		si_branchset(b, &index);

		if (in_memory) {
			rc = si_branchload(b, r, &n->file);
			if (ssunlikely(rc == -1))
				goto e0;
		}

		b->next   = n->branch;
		n->branch = b;
		n->branch_count++;

		first = 0;
		ss_iteratornext(&i);
	}
	rc = sd_recover_complete(&i);
	if (ssunlikely(rc == -1))
		goto e1;
	ss_iteratorclose(&i);

	n->in_memory = in_memory;
	return 0;
e0:
	if (b && !first)
		si_branchfree(b, r);
e1:
	ss_iteratorclose(&i);
	return -1;
}

static int
si_nodeopen(struct sinode *n, struct runtime *r,
	    struct sischeme *scheme, struct sspath *path,
	    struct sdsnapshotnode *sn)
{
	int rc = ss_fileopen(&n->file, path->path);
	if (ssunlikely(rc == -1)) {
		sr_malfunction(r->e, "db file '%s' open error: %s "
		               "(please ensure storage version compatibility)",
		               ss_pathof(&n->file.path),
		               strerror(errno));
		return -1;
	}
	rc = ss_fileseek(&n->file, n->file.size);
	if (ssunlikely(rc == -1)) {
		sr_malfunction(r->e, "db file '%s' seek error: %s",
		               ss_pathof(&n->file.path),
		               strerror(errno));
		return -1;
	}
	int in_memory = 0;
	if (scheme->storage == SI_SIN_MEMORY)
		in_memory = 1;
	rc = si_noderecover(n, r, sn, in_memory);
	if (ssunlikely(rc == -1))
		return -1;
	if (scheme->mmap) {
		rc = si_nodemap(n, r);
		if (ssunlikely(rc == -1))
			return -1;
	}
	return 0;
}

static int
si_nodecreate(struct sinode *n, struct runtime *r, struct sischeme *scheme,
	      struct sdid *id)
{
	struct sspath path;
	ss_pathcompound(&path, scheme->path, id->parent, id->id,
	                ".db.incomplete");
	int rc = ss_filenew(&n->file, path.path);
	if (ssunlikely(rc == -1)) {
		sr_malfunction(r->e, "db file '%s' create error: %s",
		               path.path, strerror(errno));
		return -1;
	}
	return 0;
}

static int si_nodemap(struct sinode *n, struct runtime *r)
{
	int rc = ss_vfsmmap(r->vfs, &n->map, n->file.fd, n->file.size, 1);
	if (ssunlikely(rc == -1)) {
		sr_malfunction(r->e, "db file '%s' mmap error: %s",
		               ss_pathof(&n->file.path),
		               strerror(errno));
		return -1;
	}
	return 0;
}

static inline void
si_nodefree_branches(struct sinode *n, struct runtime *r)
{
	struct sibranch *p = n->branch;
	struct sibranch *next = NULL;
	while (p && p != &n->self) {
		next = p->next;
		si_branchfree(p, r);
		p = next;
	}
	sd_indexfree(&n->self.index, r);
	ss_blobfree(&n->self.copy);
}

static int si_nodefree(struct sinode *n, struct runtime *r, int gc)
{
	int rcret = 0;
	int rc;
	if (gc && ss_pathis_set(&n->file.path)) {
		ss_fileadvise(&n->file, 0, 0, n->file.size);
		rc = ss_vfsunlink(r->vfs, ss_pathof(&n->file.path));
		if (ssunlikely(rc == -1)) {
			sr_malfunction(r->e, "db file '%s' unlink error: %s",
			               ss_pathof(&n->file.path),
			               strerror(errno));
			rcret = -1;
		}
	}
	si_nodefree_branches(n, r);
	rc = si_nodeclose(n, r, gc);
	if (ssunlikely(rc == -1))
		rcret = -1;
	ss_free(r->a, n);
	return rcret;
}

static int si_noderead(struct sinode *n, struct runtime *r, struct ssbuf *dest)
{
	int rc = ss_bufensure(dest, r->a, n->file.size);
	if (ssunlikely(rc == -1))
		return sr_oom_malfunction(r->e);
	rc = ss_filepread(&n->file, 0, dest->s, n->file.size);
	if (ssunlikely(rc == -1)) {
		sr_malfunction(r->e, "db file '%s' read error: %s",
		               ss_pathof(&n->file.path),
		               strerror(errno));
		return -1;
	}
	ss_bufadvance(dest, n->file.size);
	return 0;
}

static int si_nodeseal(struct sinode *n, struct runtime *r, struct sischeme *scheme)
{
	int rc;
	if (scheme->sync) {
		rc = ss_filesync(&n->file);
		if (ssunlikely(rc == -1)) {
			sr_malfunction(r->e, "db file '%s' sync error: %s",
			               ss_pathof(&n->file.path),
			               strerror(errno));
			return -1;
		}
	}
	struct sspath path;
	ss_pathcompound(&path, scheme->path,
	                n->self.id.parent, n->self.id.id,
	                ".db.seal");
	rc = ss_filerename(&n->file, path.path);
	if (ssunlikely(rc == -1)) {
		sr_malfunction(r->e, "db file '%s' rename error: %s",
		               ss_pathof(&n->file.path),
		               strerror(errno));
		return -1;
	}
	return 0;
}

static int
si_nodecomplete(struct sinode *n, struct runtime *r, struct sischeme *scheme)
{
	struct sspath path;
	ss_path(&path, scheme->path, n->self.id.id, ".db");
	int rc = ss_filerename(&n->file, path.path);
	if (ssunlikely(rc == -1)) {
		sr_malfunction(r->e, "db file '%s' rename error: %s",
		               ss_pathof(&n->file.path),
		               strerror(errno));
	}
	return rc;
}

static int si_nodegc(struct sinode *n, struct runtime *r, struct sischeme *scheme)
{
	struct sspath path;
	ss_path(&path, scheme->path, n->self.id.id, ".db.gc");
	int rc = ss_filerename(&n->file, path.path);
	if (ssunlikely(rc == -1)) {
		sr_malfunction(r->e, "db file '%s' rename error: %s",
		               ss_pathof(&n->file.path),
		               strerror(errno));
	}
	return rc;
}

static int si_planinit(struct siplan *p)
{
	p->plan    = SI_NONE;
	p->explain = SI_ENONE;
	p->a       = 0;
	p->b       = 0;
	p->c       = 0;
	p->node    = NULL;
	return 0;
}

static int si_plannerinit(struct siplanner *p, struct ssa *a, void *i)
{
	int rc = ss_rqinit(&p->compact, a, 1, 20);
	if (ssunlikely(rc == -1))
		return -1;
	/* 1Mb step up to 4Gb */
	rc = ss_rqinit(&p->branch, a, 1024 * 1024, 4000);
	if (ssunlikely(rc == -1)) {
		ss_rqfree(&p->compact, a);
		return -1;
	}
	rc = ss_rqinit(&p->temp, a, 1, 100);
	if (ssunlikely(rc == -1)) {
		ss_rqfree(&p->compact, a);
		ss_rqfree(&p->branch, a);
		return -1;
	}
	p->i = i;
	return 0;
}

static int si_plannerfree(struct siplanner *p, struct ssa *a)
{
	ss_rqfree(&p->compact, a);
	ss_rqfree(&p->branch, a);
	ss_rqfree(&p->temp, a);
	return 0;
}

static int si_plannertrace(struct siplan *p, uint32_t id, struct sstrace *t)
{
	char *plan = NULL;
	switch (p->plan) {
	case SI_BRANCH: plan = "branch";
		break;
	case SI_AGE: plan = "age";
		break;
	case SI_COMPACT: plan = "compact";
		break;
	case SI_CHECKPOINT: plan = "checkpoint";
		break;
	case SI_NODEGC: plan = "node gc";
		break;
	case SI_GC: plan = "gc";
		break;
	case SI_EXPIRE: plan = "expire";
		break;
	case SI_TEMP: plan = "temperature";
		break;
	case SI_BACKUP:
	case SI_BACKUPEND: plan = "backup";
		break;
	case SI_SHUTDOWN: plan = "database shutdown";
		break;
	case SI_DROP: plan = "database drop";
		break;
	case SI_SNAPSHOT: plan = "snapshot";
		break;
	case SI_ANTICACHE: plan = "anticache";
		break;
	}
	char *explain = NULL;
	switch (p->explain) {
	case SI_ENONE:
		explain = "none";
		break;
	case SI_ERETRY:
		explain = "retry expected";
		break;
	case SI_EINDEX_SIZE:
		explain = "index size";
		break;
	case SI_EINDEX_AGE:
		explain = "index age";
		break;
	case SI_EBRANCH_COUNT:
		explain = "branch count";
		break;
	}
	if (p->node) {
		ss_trace(t, "%s <%" PRIu32 ":%020" PRIu64 ".db explain: %s>",
		         plan, id, p->node->self.id.id, explain);
	} else {
		ss_trace(t, "%s <%" PRIu32 " explain: %s>",
		         plan, id, explain);
	}
	return 0;
}

static int si_plannerupdate(struct siplanner *p, int mask, struct sinode *n)
{
	if (mask & SI_BRANCH)
		ss_rqupdate(&p->branch, &n->nodebranch, n->used);
	if (mask & SI_COMPACT)
		ss_rqupdate(&p->compact, &n->nodecompact, n->branch_count);
	if (mask & SI_TEMP)
		ss_rqupdate(&p->temp, &n->nodetemp, n->temperature);
	return 0;
}

static int si_plannerremove(struct siplanner *p, int mask, struct sinode *n)
{
	if (mask & SI_BRANCH)
		ss_rqdelete(&p->branch, &n->nodebranch);
	if (mask & SI_COMPACT)
		ss_rqdelete(&p->compact, &n->nodecompact);
	if (mask & SI_TEMP)
		ss_rqdelete(&p->temp, &n->nodetemp);
	return 0;
}

static inline int
si_plannerpeek_backup(struct siplanner *p, struct siplan *plan)
{
	/* try to peek a node which has
	 * bsn <= required value
	*/
	int rc_inprogress = 0;
	struct sinode *n;
	struct ssrqnode *pn = NULL;
	while ((pn = ss_rqprev(&p->branch, pn))) {
		n = sscast(pn, struct sinode, nodebranch);
		if (n->backup < plan->a) {
			if (n->flags & SI_LOCK) {
				rc_inprogress = 2;
				continue;
			}
			goto match;
		}
	}
	if (rc_inprogress) {
		plan->explain = SI_ERETRY;
		return 2;
	}
	struct si *index = p->i;
	if (index->backup < plan->a) {
		plan->plan = SI_BACKUPEND;
		plan->node = 0;
		return 1;
	}
	return 0;
match:
	si_nodelock(n);
	plan->explain = SI_ENONE;
	plan->node = n;
	return 1;
}

static inline int
si_plannerpeek_checkpoint(struct siplanner *p, struct siplan *plan)
{
	/* try to peek a node which has min
	 * lsn <= required value
	*/
	int rc_inprogress = 0;
	struct sinode *n;
	struct ssrqnode *pn = NULL;
	while ((pn = ss_rqprev(&p->branch, pn))) {
		n = sscast(pn, struct sinode, nodebranch);
		if (n->i0.lsnmin <= plan->a) {
			if (n->flags & SI_LOCK) {
				rc_inprogress = 2;
				continue;
			}
			goto match;
		}
	}
	if (rc_inprogress)
		plan->explain = SI_ERETRY;
	return rc_inprogress;
match:
	si_nodelock(n);
	plan->explain = SI_ENONE;
	plan->node = n;
	return 1;
}

static inline int
si_plannerpeek_branch(struct siplanner *p, struct siplan *plan)
{
	/* try to peek a node with a biggest in-memory index */
	struct sinode *n;
	struct ssrqnode *pn = NULL;
	while ((pn = ss_rqprev(&p->branch, pn))) {
		n = sscast(pn, struct sinode, nodebranch);
		if (n->flags & SI_LOCK)
			continue;
		if (n->used >= plan->a)
			goto match;
		return 0;
	}
	return 0;
match:
	si_nodelock(n);
	plan->explain = SI_EINDEX_SIZE;
	plan->node = n;
	return 1;
}

static inline int
si_plannerpeek_age(struct siplanner *p, struct siplan *plan)
{
	/* try to peek a node with update >= a and in-memory
	 * index size >= b */

	/* full scan */
	uint64_t now = clock_monotonic64();
	struct sinode *n = NULL;
	struct ssrqnode *pn = NULL;
	while ((pn = ss_rqprev(&p->branch, pn))) {
		n = sscast(pn, struct sinode, nodebranch);
		if (n->flags & SI_LOCK)
			continue;
		if (n->used >= plan->b && ((now - n->update_time) >= plan->a))
			goto match;
	}
	return 0;
match:
	si_nodelock(n);
	plan->explain = SI_EINDEX_AGE;
	plan->node = n;
	return 1;
}

static inline int
si_plannerpeek_compact(struct siplanner *p, struct siplan *plan)
{
	/* try to peek a node with a biggest number
	 * of branches */
	struct sinode *n;
	struct ssrqnode *pn = NULL;
	while ((pn = ss_rqprev(&p->compact, pn))) {
		n = sscast(pn, struct sinode, nodecompact);
		if (n->flags & SI_LOCK)
			continue;
		if (n->branch_count >= plan->a)
			goto match;
		return 0;
	}
	return 0;
match:
	si_nodelock(n);
	plan->explain = SI_EBRANCH_COUNT;
	plan->node = n;
	return 1;
}

static inline int
si_plannerpeek_compact_temperature(struct siplanner *p, struct siplan *plan)
{
	/* try to peek a hottest node with number of
	 * branches >= watermark */
	struct sinode *n;
	struct ssrqnode *pn = NULL;
	while ((pn = ss_rqprev(&p->temp, pn))) {
		n = sscast(pn, struct sinode, nodetemp);
		if (n->flags & SI_LOCK)
			continue;
		if (n->branch_count >= plan->a)
			goto match;
		return 0;
	}
	return 0;
match:
	si_nodelock(n);
	plan->explain = SI_ENONE;
	plan->node = n;
	return 1;
}

static inline int
si_plannerpeek_gc(struct siplanner *p, struct siplan *plan)
{
	/* try to peek a node with a biggest number
	 * of branches which is ready for gc */
	int rc_inprogress = 0;
	struct sinode *n;
	struct ssrqnode *pn = NULL;
	while ((pn = ss_rqprev(&p->compact, pn))) {
		n = sscast(pn, struct sinode, nodecompact);
		struct sdindexheader *h = n->self.index.h;
		if (sslikely(h->dupkeys == 0) || (h->dupmin >= plan->a))
			continue;
		uint32_t used = (h->dupkeys * 100) / h->keys;
		if (used >= plan->b) {
			if (n->flags & SI_LOCK) {
				rc_inprogress = 2;
				continue;
			}
			goto match;
		}
	}
	if (rc_inprogress)
		plan->explain = SI_ERETRY;
	return rc_inprogress;
match:
	si_nodelock(n);
	plan->explain = SI_ENONE;
	plan->node = n;
	return 1;
}

static inline int
si_plannerpeek_expire(struct siplanner *p, struct siplan *plan)
{
	/* full scan */
	int rc_inprogress = 0;
	uint32_t now = time(NULL);
	struct sinode *n = NULL;
	struct ssrqnode *pn = NULL;
	while ((pn = ss_rqprev(&p->branch, pn))) {
		n = sscast(pn, struct sinode, nodebranch);
		struct sdindexheader *h = n->self.index.h;
		if (h->tsmin == UINT32_MAX)
			continue;
		uint32_t diff = now - h->tsmin;
		if (sslikely(diff >= plan->a)) {
			if (n->flags & SI_LOCK) {
				rc_inprogress = 2;
				continue;
			}
			goto match;
		}
	}
	if (rc_inprogress)
		plan->explain = SI_ERETRY;
	return rc_inprogress;
match:
	si_nodelock(n);
	plan->node = n;
	return 1;
}


static inline int
si_plannerpeek_snapshot(struct siplanner *p, struct siplan *plan)
{
	struct si *index = p->i;
	if (index->snapshot >= plan->a)
		return 0;
	if (index->snapshot_run) {
		/* snaphot inprogress */
		plan->explain = SI_ERETRY;
		return 2;
	}
	index->snapshot_run = 1;
	return 1;
}

static inline int
si_plannerpeek_anticache(struct siplanner *p, struct siplan *plan)
{
	struct si *index = p->i;
	if (index->scheme.storage != SI_SANTI_CACHE)
		return 0;
	int rc_inprogress = 0;
	struct sinode *n;
	struct ssrqnode *pn = NULL;
	while ((pn = ss_rqprev(&p->temp, pn))) {
		n = sscast(pn, struct sinode, nodetemp);
		if (n->flags & SI_LOCK) {
			rc_inprogress = 2;
			continue;
		}
		if (n->ac >= plan->a)
			continue;
		n->ac = plan->a;
		uint64_t size = si_nodesize(n) + n->used;
		if (size <= plan->b) {
			/* promote */
			if (n->in_memory)
				continue;
			plan->c = size;
			n->flags |= SI_PROMOTE;
		} else {
			/* revoke in_memory flag */
			if (! n->in_memory)
				continue;
			plan->c = 0;
			n->flags |= SI_REVOKE;
		}
		goto match;
	}
	if (rc_inprogress) {
		plan->explain = SI_ERETRY;
		return 2;
	}
	return 0;
match:
	si_nodelock(n);
	plan->explain = SI_ENONE;
	plan->node = n;
	return 1;
}

static inline int
si_plannerpeek_lru(struct siplanner *p, struct siplan *plan)
{
	struct si *index = p->i;
	if (sslikely(! index->scheme.lru))
		return 0;
	if (! index->lru_run_lsn) {
		index->lru_run_lsn = si_lru_vlsn_of(index);
		if (sslikely(index->lru_run_lsn == 0))
			return 0;
	}
	int rc_inprogress = 0;
	struct sinode *n;
	struct ssrqnode *pn = NULL;
	while ((pn = ss_rqprev(&p->compact, pn))) {
		n = sscast(pn, struct sinode, nodecompact);
		struct sdindexheader *h = n->self.index.h;
		if (h->lsnmin < index->lru_run_lsn) {
			if (n->flags & SI_LOCK) {
				rc_inprogress = 2;
				continue;
			}
			goto match;
		}
	}
	if (rc_inprogress)
		plan->explain = SI_ERETRY;
	else
		index->lru_run_lsn = 0;
	return rc_inprogress;
match:
	si_nodelock(n);
	plan->explain = SI_ENONE;
	plan->node = n;
	return 1;
}

static inline int
si_plannerpeek_shutdown(struct siplanner *p, struct siplan *plan)
{
	struct si *index = p->i;
	int status = sr_status(&index->status);
	switch (status) {
	case SR_DROP:
		if (si_refs(index) > 0)
			return 2;
		plan->plan = SI_DROP;
		return 1;
	case SR_SHUTDOWN:
		if (si_refs(index) > 0)
			return 2;
		plan->plan = SI_SHUTDOWN;
		return 1;
	}
	return 0;
}

static inline int
si_plannerpeek_nodegc(struct siplanner *p, struct siplan *plan)
{
	struct si *index = p->i;
	if (sslikely(index->gc_count == 0))
		return 0;
	int rc_inprogress = 0;
	struct sinode *n;
	rlist_foreach_entry(n, &index->gc, gc) {
		if (sslikely(si_noderefof(n) == 0)) {
			rlist_del(&n->gc);
			index->gc_count--;
			plan->explain = SI_ENONE;
			plan->node = n;
			return 1;
		} else {
			rc_inprogress = 2;
		}
	}
	return rc_inprogress;
}

static int si_planner(struct siplanner *p, struct siplan *plan)
{
	switch (plan->plan) {
	case SI_BRANCH:
	case SI_COMPACT_INDEX:
		return si_plannerpeek_branch(p, plan);
	case SI_COMPACT:
		if (plan->b == 1)
			return si_plannerpeek_compact_temperature(p, plan);
		return si_plannerpeek_compact(p, plan);
	case SI_NODEGC:
		return si_plannerpeek_nodegc(p, plan);
	case SI_GC:
		return si_plannerpeek_gc(p, plan);
	case SI_EXPIRE:
		return si_plannerpeek_expire(p, plan);
	case SI_CHECKPOINT:
		return si_plannerpeek_checkpoint(p, plan);
	case SI_AGE:
		return si_plannerpeek_age(p, plan);
	case SI_BACKUP:
		return si_plannerpeek_backup(p, plan);
	case SI_SNAPSHOT:
		return si_plannerpeek_snapshot(p, plan);
	case SI_ANTICACHE:
		return si_plannerpeek_anticache(p, plan);
	case SI_LRU:
		return si_plannerpeek_lru(p, plan);
	case SI_SHUTDOWN:
	case SI_DROP:
		return si_plannerpeek_shutdown(p, plan);
	}
	return -1;
}

static int si_profilerbegin(struct siprofiler *p, struct si *i)
{
	memset(p, 0, sizeof(*p));
	p->i = i;
	p->temperature_min = 100;
	si_lock(i);
	return 0;
}

static int si_profilerend(struct siprofiler *p)
{
	si_unlock(p->i);
	return 0;
}

static void
si_profiler_histogram_branch(struct siprofiler *p)
{
	/* prepare histogram string */
	int size = 0;
	int i = 0;
	while (i < 20) {
		if (p->histogram_branch[i] == 0) {
			i++;
			continue;
		}
		size += snprintf(p->histogram_branch_sz + size,
		                 sizeof(p->histogram_branch_sz) - size,
		                 "[%d]:%d ", i,
		                 p->histogram_branch[i]);
		i++;
	}
	if (p->histogram_branch_20plus) {
		size += snprintf(p->histogram_branch_sz + size,
		                 sizeof(p->histogram_branch_sz) - size,
		                 "[20+]:%d ",
		                 p->histogram_branch_20plus);
	}
	if (size == 0)
		p->histogram_branch_ptr = NULL;
	else {
		p->histogram_branch_ptr = p->histogram_branch_sz;
	}
}

static void
si_profiler_histogram_temperature(struct siprofiler *p)
{
	/* build histogram */
	static struct {
		int nodes;
		int branches;
	} h[101];
	memset(h, 0, sizeof(h));
	struct sinode *n;
	struct ssrqnode *pn = NULL;
	while ((pn = ss_rqprev(&p->i->p.temp, pn)))
	{
		n = sscast(pn, struct sinode, nodetemp);
		h[pn->v].nodes++;
		h[pn->v].branches += n->branch_count;
	}

	/* prepare histogram string */
	int count = 0;
	int i = 100;
	int size = 0;
	while (i >= 0 && count < 10) {
		if (h[i].nodes == 0) {
			i--;
			continue;
		}
		size += snprintf(p->histogram_temperature_sz + size,
		                 sizeof(p->histogram_temperature_sz) - size,
		                 "[%d]:%d-%d ", i,
		                 h[i].nodes, h[i].branches);
		i--;
		count++;
	}
	if (size == 0)
		p->histogram_temperature_ptr = NULL;
	else {
		p->histogram_temperature_ptr = p->histogram_temperature_sz;
	}
}

static int si_profiler(struct siprofiler *p)
{
	uint32_t temperature_total = 0;
	uint64_t memory_used = 0;
	struct ssrbnode *pn;
	struct sinode *n;
	pn = ss_rbmin(&p->i->i);
	while (pn) {
		n = sscast(pn, struct sinode, node);
		if (p->temperature_max < n->temperature)
			p->temperature_max = n->temperature;
		if (p->temperature_min > n->temperature)
			p->temperature_min = n->temperature;
		temperature_total += n->temperature;
		p->total_node_count++;
		p->count += n->i0.count;
		p->count += n->i1.count;
		p->total_branch_count += n->branch_count;
		if (p->total_branch_max < n->branch_count)
			p->total_branch_max = n->branch_count;
		if (n->branch_count < 20)
			p->histogram_branch[n->branch_count]++;
		else
			p->histogram_branch_20plus++;
		memory_used += sv_indexused(&n->i0);
		memory_used += sv_indexused(&n->i1);
		struct sibranch *b = n->branch;
		while (b) {
			p->count += b->index.h->keys;
			p->count_dup += b->index.h->dupkeys;
			int indexsize = sd_indexsize_ext(b->index.h);
			p->total_snapshot_size += indexsize;
			p->total_node_size += indexsize + b->index.h->total;
			p->total_node_origin_size += indexsize + b->index.h->totalorigin;
			p->total_page_count += b->index.h->count;
			if (b->index.h->extensions & SD_INDEXEXT_AMQF) {
				p->total_amqf_size +=
					sizeof(struct sdindexamqf) + sd_indexamqf(&b->index)->size;
			}
			b = b->next;
		}
		pn = ss_rbnext(&p->i->i, pn);
	}
	if (p->total_node_count > 0) {
		p->total_branch_avg =
			p->total_branch_count / p->total_node_count;
		p->temperature_avg =
			temperature_total / p->total_node_count;
	}
	p->memory_used = memory_used;
	p->read_disk  = p->i->read_disk;
	p->read_cache = p->i->read_cache;

	si_profiler_histogram_branch(p);
	si_profiler_histogram_temperature(p);
	return 0;
}

static int
si_readopen(struct siread *q, struct si *i, struct sicache *c, enum ssorder o,
	    uint64_t vlsn,
	    void *prefix, uint32_t prefixsize,
	    void *key, uint32_t keysize)
{
	q->order       = o;
	q->key         = key;
	q->keysize     = keysize;
	q->vlsn        = vlsn;
	q->index       = i;
	q->r           = &i->r;
	q->cache       = c;
	q->prefix      = prefix;
	q->prefixsize  = prefixsize;
	q->has         = 0;
	q->upsert_v    = NULL;
	q->upsert_eq   = 0;
	q->cache_only  = 0;
	q->oldest_only = 0;
	q->read_disk   = 0;
	q->read_cache  = 0;
	memset(&q->result, 0, sizeof(q->result));
	sv_mergeinit(&q->merge);
	si_lock(i);
	return 0;
}

static void si_readcache_only(struct siread *q)
{
	q->cache_only = 1;
}

static void si_readoldest_only(struct siread *q)
{
	q->oldest_only = 1;
}

static void si_readhas(struct siread *q)
{
	q->has = 1;
}

static void si_readupsert(struct siread *q, struct sv *v, int eq)
{
	q->upsert_v  = v;
	q->upsert_eq = eq;
}

static int si_readclose(struct siread *q)
{
	si_unlock(q->index);
	sv_mergefree(&q->merge, q->r->a);
	return 0;
}

static inline int
si_readdup(struct siread *q, struct sv *result)
{
	struct svv *v;
	if (sslikely(result->i == &sv_vif)) {
		v = result->v;
		sv_vref(v);
	} else {
		v = sv_vdup(q->r, result);
		if (ssunlikely(v == NULL))
			return sr_oom(q->r->e);
	}
	sv_init(&q->result, &sv_vif, v, NULL);
	return 1;
}

static inline void
si_readstat(struct siread *q, int cache, struct sinode *n, uint32_t reads)
{
	struct si *i = q->index;
	if (cache) {
		i->read_cache += reads;
		q->read_cache += reads;
	} else {
		i->read_disk += reads;
		q->read_disk += reads;
	}
	/* update temperature */
	if (i->scheme.temperature) {
		n->temperature_reads += reads;
		uint64_t total = i->read_disk + i->read_cache;
		if (ssunlikely(total == 0))
			return;
		n->temperature = (n->temperature_reads * 100ULL) / total;
		si_plannerupdate(&q->index->p, SI_TEMP, n);
	}
}

static inline int
si_getresult(struct siread *q, struct sv *v, int compare)
{
	int rc;
	if (compare) {
		rc = sf_compare(q->r->scheme, sv_pointer(v), sv_size(v),
		                q->key, q->keysize);
		if (ssunlikely(rc != 0))
			return 0;
	}
	if (q->prefix) {
		rc = sf_compareprefix(q->r->scheme,
		                      q->prefix,
		                      q->prefixsize,
		                      sv_pointer(v), sv_size(v));
		if (ssunlikely(! rc))
			return 0;
	}
	if (ssunlikely(q->has))
		return sv_lsn(v) > q->vlsn;
	if (ssunlikely(sv_is(v, SVDELETE)))
		return 2;
	rc = si_readdup(q, v);
	if (ssunlikely(rc == -1))
		return -1;
	return 1;
}

static inline int
si_getindex(struct siread *q, struct sinode *n)
{
	struct svindex *second;
	struct svindex *first = si_nodeindex_priority(n, &second);
	struct ssiter i;
	ss_iterinit(sv_indexiter, &i);
	int rc;
	if (first->count > 0) {
		rc = ss_iteropen(sv_indexiter, &i, q->r, first,
		                 SS_GTE, q->key, q->keysize);
		if (rc) {
			goto result;
		}
	}
	if (sslikely(second == NULL || !second->count))
		return 0;
	rc = ss_iteropen(sv_indexiter, &i, q->r, second,
	                 SS_GTE, q->key, q->keysize);
	if (! rc) {
		return 0;
	}
result:;
	si_readstat(q, 1, n, 1);
	struct sv *v = ss_iterof(sv_indexiter, &i);
	assert(v != NULL);
	struct svref *visible = v->v;
	if (sslikely(! q->has)) {
		visible = sv_refvisible(visible, q->vlsn);
		if (visible == NULL)
			return 0;
	}
	struct sv vret;
	sv_init(&vret, &sv_vif, visible->v, NULL);
	return si_getresult(q, &vret, 0);
}

static inline int
si_getbranch(struct siread *q, struct sinode *n, struct sicachebranch *c)
{
	struct sibranch *b = c->branch;
	/* amqf */
	struct sischeme *scheme = &q->index->scheme;
	int rc;
	if (scheme->amqf) {
		rc = si_amqfhas_branch(q->r, b, q->key);
		if (sslikely(! rc))
			return 0;
	}
	/* choose compression type */
	int compression;
	struct ssfilterif *compression_if;
	if (! si_branchis_root(b)) {
		compression    = scheme->compression_branch;
		compression_if = scheme->compression_branch_if;
	} else {
		compression    = scheme->compression;
		compression_if = scheme->compression_if;
	}
	struct sdreadarg arg = {
		.index           = &b->index,
		.buf             = &c->buf_a,
		.buf_xf          = &c->buf_b,
		.buf_read        = &q->index->readbuf,
		.index_iter      = &c->index_iter,
		.page_iter       = &c->page_iter,
		.use_memory      = n->in_memory,
		.use_mmap        = scheme->mmap,
		.use_mmap_copy   = 0,
		.use_compression = compression,
		.compression_if  = compression_if,
		.has             = q->has,
		.has_vlsn        = q->vlsn,
		.o               = SS_GTE,
		.mmap            = &n->map,
		.memory          = &b->copy,
		.file            = &n->file,
		.r               = q->r
	};
	ss_iterinit(sd_read, &c->i);
	rc = ss_iteropen(sd_read, &c->i, &arg, q->key, q->keysize);
	int reads = sd_read_stat(&c->i);
	si_readstat(q, 0, n, reads);
	if (ssunlikely(rc <= 0))
		return rc;
	/* prepare sources */
	sv_mergereset(&q->merge);
	sv_mergeadd(&q->merge, &c->i);
	struct ssiter i;
	ss_iterinit(sv_mergeiter, &i);
	ss_iteropen(sv_mergeiter, &i, q->r, &q->merge, SS_GTE);
	uint64_t vlsn = q->vlsn;
	if (ssunlikely(q->has))
		vlsn = UINT64_MAX;
	struct ssiter j;
	ss_iterinit(sv_readiter, &j);
	ss_iteropen(sv_readiter, &j, q->r, &i, &q->index->u, vlsn, 1);
	struct sv *v = ss_iterof(sv_readiter, &j);
	if (ssunlikely(v == NULL))
		return 0;
	return si_getresult(q, v, 1);
}

static inline int
si_get(struct siread *q)
{
	assert(q->key != NULL);
	struct ssiter i;
	ss_iterinit(si_iter, &i);
	ss_iteropen(si_iter, &i, q->r, q->index, SS_GTE, q->key, q->keysize);
	struct sinode *node;
	node = ss_iterof(si_iter, &i);
	assert(node != NULL);

	/* search in memory */
	int rc;
	rc = si_getindex(q, node);
	if (rc != 0)
		return rc;
	if (q->cache_only)
		return 2;
	struct sinodeview view;
	si_nodeview_open(&view, node);
	rc = si_cachevalidate(q->cache, node);
	if (ssunlikely(rc == -1)) {
		sr_oom(q->r->e);
		return -1;
	}
	si_unlock(q->index);

	/* search on disk */
	struct svmerge *m = &q->merge;
	rc = sv_mergeprepare(m, q->r, 1);
	assert(rc == 0);
	struct sicachebranch *b;
	if (q->oldest_only) {
		b = si_cacheseek(q->cache, &node->self);
		assert(b != NULL);
		rc = si_getbranch(q, node, b);
	} else {
		b = q->cache->branch;
		while (b && b->branch) {
			rc = si_getbranch(q, node, b);
			if (rc != 0)
				break;
			b = b->next;
		}
	}

	si_lock(q->index);
	si_nodeview_close(&view);
	return rc;
}

static inline int
si_rangebranch(struct siread *q, struct sinode *n,
	       struct sibranch *b, struct svmerge *m)
{
	struct sicachebranch *c = si_cachefollow(q->cache, b);
	assert(c->branch == b);
	/* iterate cache */
	if (ss_iterhas(sd_read, &c->i)) {
		struct svmergesrc *s = sv_mergeadd(m, &c->i);
		si_readstat(q, 1, n, 1);
		s->ptr = c;
		return 1;
	}
	if (c->open) {
		return 1;
	}
	if (q->cache_only) {
		return 2;
	}
	c->open = 1;
	/* choose compression type */
	struct sischeme *scheme = &q->index->scheme;
	int compression;
	struct ssfilterif *compression_if;
	if (! si_branchis_root(b)) {
		compression    = scheme->compression_branch;
		compression_if = scheme->compression_branch_if;
	} else {
		compression    = scheme->compression;
		compression_if = scheme->compression_if;
	}
	struct sdreadarg arg = {
		.index           = &b->index,
		.buf             = &c->buf_a,
		.buf_xf          = &c->buf_b,
		.buf_read        = &q->index->readbuf,
		.index_iter      = &c->index_iter,
		.page_iter       = &c->page_iter,
		.use_memory      = n->in_memory,
		.use_mmap        = scheme->mmap,
		.use_mmap_copy   = 1,
		.use_compression = compression,
		.compression_if  = compression_if,
		.has             = 0,
		.has_vlsn        = 0,
		.o               = q->order,
		.memory          = &b->copy,
		.mmap            = &n->map,
		.file            = &n->file,
		.r               = q->r
	};
	ss_iterinit(sd_read, &c->i);
	int rc = ss_iteropen(sd_read, &c->i, &arg, q->key, q->keysize);
	int reads = sd_read_stat(&c->i);
	si_readstat(q, 0, n, reads);
	if (ssunlikely(rc == -1))
		return -1;
	if (ssunlikely(! ss_iterhas(sd_read, &c->i)))
		return 0;
	struct svmergesrc *s = sv_mergeadd(m, &c->i);
	s->ptr = c;
	return 1;
}

static inline int
si_range(struct siread *q)
{
	assert(q->has == 0);

	struct ssiter i;
	ss_iterinit(si_iter, &i);
	ss_iteropen(si_iter, &i, q->r, q->index, q->order, q->key, q->keysize);
	struct sinode *node;
next_node:
	node = ss_iterof(si_iter, &i);
	if (ssunlikely(node == NULL))
		return 0;

	/* prepare sources */
	struct svmerge *m = &q->merge;
	int count = node->branch_count + 2 + 1;
	int rc = sv_mergeprepare(m, q->r, count);
	if (ssunlikely(rc == -1)) {
		sr_errorreset(q->r->e);
		return -1;
	}

	/* external source (upsert) */
	struct svmergesrc *s;
	struct sv upbuf_reserve;
	struct ssbuf upbuf;
	if (ssunlikely(q->upsert_v && q->upsert_v->v)) {
		ss_bufinit_reserve(&upbuf, &upbuf_reserve, sizeof(upbuf_reserve));
		ss_bufadd(&upbuf, NULL, (void*)&q->upsert_v, sizeof(struct sv*));
		s = sv_mergeadd(m, NULL);
		ss_iterinit(ss_bufiterref, &s->src);
		ss_iteropen(ss_bufiterref, &s->src, &upbuf, sizeof(struct sv*));
	}

	/* in-memory indexes */
	struct svindex *second;
	struct svindex *first = si_nodeindex_priority(node, &second);
	if (first->count) {
		s = sv_mergeadd(m, NULL);
		ss_iterinit(sv_indexiter, &s->src);
		ss_iteropen(sv_indexiter, &s->src, q->r, first, q->order,
		            q->key, q->keysize);
	}
	if (ssunlikely(second && second->count)) {
		s = sv_mergeadd(m, NULL);
		ss_iterinit(sv_indexiter, &s->src);
		ss_iteropen(sv_indexiter, &s->src, q->r, second, q->order,
		            q->key, q->keysize);
	}

	/* cache and branches */
	rc = si_cachevalidate(q->cache, node);
	if (ssunlikely(rc == -1)) {
		sr_oom(q->r->e);
		return -1;
	}

	if (q->oldest_only) {
		rc = si_rangebranch(q, node, &node->self, m);
		if (ssunlikely(rc == -1 || rc == 2))
			return rc;
	} else {
		struct sibranch *b = node->branch;
		while (b) {
			rc = si_rangebranch(q, node, b, m);
			if (ssunlikely(rc == -1 || rc == 2))
				return rc;
			b = b->next;
		}
	}

	/* merge and filter data stream */
	struct ssiter j;
	ss_iterinit(sv_mergeiter, &j);
	ss_iteropen(sv_mergeiter, &j, q->r, m, q->order);
	struct ssiter k;
	ss_iterinit(sv_readiter, &k);
	ss_iteropen(sv_readiter, &k, q->r, &j, &q->index->u, q->vlsn, 0);
	struct sv *v = ss_iterof(sv_readiter, &k);
	if (ssunlikely(v == NULL)) {
		sv_mergereset(&q->merge);
		ss_iternext(si_iter, &i);
		goto next_node;
	}

	rc = 1;
	/* convert upsert search to SS_EQ */
	if (q->upsert_eq) {
		rc = sf_compare(q->r->scheme, sv_pointer(v), sv_size(v),
		                q->key, q->keysize);
		rc = rc == 0;
	}
	/* do prefix search */
	if (q->prefix && rc) {
		rc = sf_compareprefix(q->r->scheme, q->prefix, q->prefixsize,
		                      sv_pointer(v),
		                      sv_size(v));
	}
	if (sslikely(rc == 1)) {
		if (ssunlikely(si_readdup(q, v) == -1))
			return -1;
	}

	/* skip a possible duplicates from data sources */
	sv_readiter_forward(&k);
	return rc;
}

static int si_read(struct siread *q)
{
	switch (q->order) {
	case SS_EQ:
		return si_get(q);
	case SS_LT:
	case SS_LTE:
	case SS_GT:
	case SS_GTE:
		return si_range(q);
	default:
		break;
	}
	return -1;
}

static int
si_readcommited(struct si *index, struct runtime *r, struct sv *v, int recover)
{
	/* search node index */
	struct ssiter i;
	ss_iterinit(si_iter, &i);
	ss_iteropen(si_iter, &i, r, index, SS_GTE,
	            sv_pointer(v), sv_size(v));
	struct sinode *node;
	node = ss_iterof(si_iter, &i);
	assert(node != NULL);

	uint64_t lsn = sv_lsn(v);
	int rc;
	/* search in-memory */
	if (recover == 2) {
		struct svindex *second;
		struct svindex *first = si_nodeindex_priority(node, &second);
		ss_iterinit(sv_indexiter, &i);
		if (sslikely(first->count > 0)) {
			rc = ss_iteropen(sv_indexiter, &i, r, first, SS_GTE,
			                 sv_pointer(v), sv_size(v));
			if (rc) {
				struct sv *ref = ss_iterof(sv_indexiter, &i);
				if (sv_refvisible_gte((struct svref*)ref->v, lsn))
					return 1;
			}
		}
		if (second && !second->count) {
			rc = ss_iteropen(sv_indexiter, &i, r, second, SS_GTE,
			                 sv_pointer(v), sv_size(v));
			if (rc) {
				struct sv *ref = ss_iterof(sv_indexiter, &i);
				if (sv_refvisible_gte((struct svref*)ref->v, lsn))
					return 1;
			}
		}
	}

	/* search branches */
	struct sibranch *b;
	for (b = node->branch; b; b = b->next)
	{
		ss_iterinit(sd_indexiter, &i);
		ss_iteropen(sd_indexiter, &i, r, &b->index, SS_GTE,
		            sv_pointer(v), sv_size(v));
		struct sdindexpage *page = ss_iterof(sd_indexiter, &i);
		if (page == NULL)
			continue;
		if (page->lsnmax >= lsn)
			return 1;
	}
	return 0;
}

/*
	repository recover states
	-------------------------

	compaction

	000000001.000000002.db.incomplete  (1)
	000000001.000000002.db.seal        (2)
	000000002.db                       (3)
	000000001.000000003.db.incomplete
	000000001.000000003.db.seal
	000000003.db
	(4)

	1. remove incomplete, mark parent as having incomplete
	2. find parent, mark as having seal
	3. add
	4. recover:
		a. if parent has incomplete and seal - remove both
		b. if parent has incomplete - remove incomplete
		c. if parent has seal - remove parent, complete seal

	see: snapshot recover
	see: scheme recover
	see: test/crash/durability.test.c
*/

static struct sinode *
si_bootstrap(struct si *i, uint64_t parent)
{
	struct runtime *r = &i->r;
	/* create node */
	struct sinode *n = si_nodenew(r);
	if (ssunlikely(n == NULL))
		return NULL;
	struct sdid id = {
		.parent = parent,
		.flags  = 0,
		.id     = sr_seq(r->seq, SR_NSNNEXT)
	};
	int rc;
	rc = si_nodecreate(n, r, &i->scheme, &id);
	if (ssunlikely(rc == -1))
		goto e0;
	n->branch = &n->self;
	n->branch_count++;

	/* in-memory mode support */
	struct ssblob *blob = NULL;
	if (i->scheme.storage == SI_SIN_MEMORY) {
		blob = &n->self.copy;
		rc = ss_blobensure(blob, 4096);
		if (ssunlikely(rc == -1))
			goto e0;
		n->in_memory = 1;
	}

	/* create index with one empty page */
	struct sdindex index;
	sd_indexinit(&index);
	rc = sd_indexbegin(&index, r);
	if (ssunlikely(rc == -1))
		goto e0;

	struct ssqf f, *qf = NULL;
	ss_qfinit(&f);

	struct sdbuild build;
	sd_buildinit(&build);
	rc = sd_buildbegin(&build, r,
	                   i->scheme.node_page_checksum,
	                   i->scheme.expire > 0,
	                   i->scheme.compression_key,
	                   i->scheme.compression,
	                   i->scheme.compression_if);
	if (ssunlikely(rc == -1))
		goto e1;
	sd_buildend(&build, r);
	rc = sd_indexadd(&index, r, &build, sizeof(struct sdseal));
	if (ssunlikely(rc == -1))
		goto e1;

	/* write seal */
	uint64_t seal = n->file.size;
	rc = sd_writeseal(r, &n->file, blob);
	if (ssunlikely(rc == -1))
		goto e1;
	/* write page */
	rc = sd_writepage(r, &n->file, blob, &build);
	if (ssunlikely(rc == -1))
		goto e1;
	/* amqf */
	if (i->scheme.amqf) {
		rc = ss_qfensure(&f, r->a, 0);
		if (ssunlikely(rc == -1))
			goto e1;
		qf = &f;
	}
	rc = sd_indexcommit(&index, r, &id, qf, n->file.size);
	if (ssunlikely(rc == -1))
		goto e1;
	ss_qffree(&f, r->a);
	/* write index */
	rc = sd_writeindex(r, &n->file, blob, &index);
	if (ssunlikely(rc == -1))
		goto e1;
	/* close seal */
	rc = sd_seal(r, &n->file, blob, &index, seal);
	if (ssunlikely(rc == -1))
		goto e1;
	if (blob) {
		rc = ss_blobfit(blob);
		if (ssunlikely(rc == -1))
			goto e1;
	}
	if (i->scheme.mmap) {
		rc = si_nodemap(n, r);
		if (ssunlikely(rc == -1))
			goto e1;
	}
	si_branchset(&n->self, &index);

	sd_buildcommit(&build, r);
	sd_buildfree(&build, r);
	return n;
e1:
	ss_qffree(&f, r->a);
	sd_indexfree(&index, r);
	sd_buildfree(&build, r);
e0:
	si_nodefree(n, r, 0);
	return NULL;
}

static inline int
si_deploy(struct si *i, struct runtime *r, int create_directory)
{
	/* create directory */
	int rc;
	if (sslikely(create_directory)) {
		rc = ss_vfsmkdir(r->vfs, i->scheme.path, 0755);
		if (ssunlikely(rc == -1)) {
			sr_malfunction(r->e, "directory '%s' create error: %s",
			               i->scheme.path, strerror(errno));
			return -1;
		}
	}
	/* create scheme file */
	rc = si_schemedeploy(&i->scheme, r);
	if (ssunlikely(rc == -1)) {
		sr_malfunction_set(r->e);
		return -1;
	}
	/* create initial node */
	struct sinode *n = si_bootstrap(i, 0);
	if (ssunlikely(n == NULL))
		return -1;
	SS_INJECTION(r->i, SS_INJECTION_SI_RECOVER_0,
	             si_nodefree(n, r, 0);
	             sr_malfunction(r->e, "%s", "error injection");
	             return -1);
	rc = si_nodecomplete(n, r, &i->scheme);
	if (ssunlikely(rc == -1)) {
		si_nodefree(n, r, 1);
		return -1;
	}
	si_insert(i, n);
	si_plannerupdate(&i->p, SI_COMPACT|SI_BRANCH|SI_TEMP, n);
	i->size = si_nodesize(n);
	return 1;
}

static inline int64_t
si_processid(char **str)
{
	char *s = *str;
	size_t v = 0;
	while (*s && *s != '.') {
		if (ssunlikely(! isdigit(*s)))
			return -1;
		v = (v * 10) + *s - '0';
		s++;
	}
	*str = s;
	return v;
}

static inline int
si_process(char *name, uint64_t *nsn, uint64_t *parent)
{
	/* id.db */
	/* id.id.db.incomplete */
	/* id.id.db.seal */
	/* id.id.db.gc */
	char *token = name;
	int64_t id = si_processid(&token);
	if (ssunlikely(id == -1))
		return -1;
	*parent = id;
	*nsn = id;
	if (strcmp(token, ".db") == 0)
		return SI_RDB;
	else
	if (strcmp(token, ".db.gc") == 0)
		return SI_RDB_REMOVE;
	if (ssunlikely(*token != '.'))
		return -1;
	token++;
	id = si_processid(&token);
	if (ssunlikely(id == -1))
		return -1;
	*nsn = id;
	if (strcmp(token, ".db.incomplete") == 0)
		return SI_RDB_DBI;
	else
	if (strcmp(token, ".db.seal") == 0)
		return SI_RDB_DBSEAL;
	return -1;
}

static inline int
si_trackdir(struct sitrack *track, struct runtime *r, struct si *i)
{
	DIR *dir = opendir(i->scheme.path);
	if (ssunlikely(dir == NULL)) {
		sr_malfunction(r->e, "directory '%s' open error: %s",
		               i->scheme.path, strerror(errno));
		return -1;
	}
	struct dirent *de;
	while ((de = readdir(dir))) {
		if (ssunlikely(de->d_name[0] == '.'))
			continue;
		uint64_t id_parent = 0;
		uint64_t id = 0;
		int rc = si_process(de->d_name, &id, &id_parent);
		if (ssunlikely(rc == -1))
			continue; /* skip unknown file */
		si_tracknsn(track, id_parent);
		si_tracknsn(track, id);

		struct sinode *head, *node;
		struct sspath path;
		switch (rc) {
		case SI_RDB_DBI:
		case SI_RDB_DBSEAL: {
			/* find parent node and mark it as having
			 * incomplete compaction process */
			head = si_trackget(track, id_parent);
			if (sslikely(head == NULL)) {
				head = si_nodenew(r);
				if (ssunlikely(head == NULL))
					goto error;
				head->self.id.id = id_parent;
				head->recover = SI_RDB_UNDEF;
				si_trackset(track, head);
			}
			head->recover |= rc;
			/* remove any incomplete file made during compaction */
			if (rc == SI_RDB_DBI) {
				ss_pathcompound(&path, i->scheme.path, id_parent, id,
				                ".db.incomplete");
				rc = ss_vfsunlink(r->vfs, path.path);
				if (ssunlikely(rc == -1)) {
					sr_malfunction(r->e, "db file '%s' unlink error: %s",
					               path.path, strerror(errno));
					goto error;
				}
				continue;
			}
			assert(rc == SI_RDB_DBSEAL);
			/* recover 'sealed' node */
			node = si_nodenew(r);
			if (ssunlikely(node == NULL))
				goto error;
			node->recover = SI_RDB_DBSEAL;
			ss_pathcompound(&path, i->scheme.path, id_parent, id,
			                ".db.seal");
			rc = si_nodeopen(node, r, &i->scheme, &path, NULL);
			if (ssunlikely(rc == -1)) {
				si_nodefree(node, r, 0);
				goto error;
			}
			si_trackset(track, node);
			si_trackmetrics(track, node);
			continue;
		}
		case SI_RDB_REMOVE:
			ss_path(&path, i->scheme.path, id, ".db.gc");
			rc = ss_vfsunlink(r->vfs, ss_pathof(&path));
			if (ssunlikely(rc == -1)) {
				sr_malfunction(r->e, "db file '%s' unlink error: %s",
				               ss_pathof(&path), strerror(errno));
				goto error;
			}
			continue;
		}
		assert(rc == SI_RDB);

		head = si_trackget(track, id);
		if (head != NULL && (head->recover & SI_RDB)) {
			/* loaded by snapshot */
			continue;
		}

		/* recover node */
		node = si_nodenew(r);
		if (ssunlikely(node == NULL))
			goto error;
		node->recover = SI_RDB;
		ss_path(&path, i->scheme.path, id, ".db");
		rc = si_nodeopen(node, r, &i->scheme, &path, NULL);
		if (ssunlikely(rc == -1)) {
			si_nodefree(node, r, 0);
			goto error;
		}
		si_trackmetrics(track, node);

		/* track node */
		if (sslikely(head == NULL)) {
			si_trackset(track, node);
		} else {
			/* replace a node previously created by a
			 * incomplete compaction */
			si_trackreplace(track, head, node);
			head->recover &= ~SI_RDB_UNDEF;
			node->recover |= head->recover;
			si_nodefree(head, r, 0);
		}
	}
	closedir(dir);
	return 0;
error:
	closedir(dir);
	return -1;
}

static inline int
si_trackvalidate(struct sitrack *track, struct ssbuf *buf, struct runtime *r, struct si *i)
{
	ss_bufreset(buf);
	struct ssrbnode *p = ss_rbmax(&track->i);
	while (p) {
		struct sinode *n = sscast(p, struct sinode, node);
		switch (n->recover) {
		case SI_RDB|SI_RDB_DBI|SI_RDB_DBSEAL|SI_RDB_REMOVE:
		case SI_RDB|SI_RDB_DBSEAL|SI_RDB_REMOVE:
		case SI_RDB|SI_RDB_REMOVE:
		case SI_RDB_UNDEF|SI_RDB_DBSEAL|SI_RDB_REMOVE:
		case SI_RDB|SI_RDB_DBI|SI_RDB_DBSEAL:
		case SI_RDB|SI_RDB_DBI:
		case SI_RDB:
		case SI_RDB|SI_RDB_DBSEAL:
		case SI_RDB_UNDEF|SI_RDB_DBSEAL: {
			/* match and remove any leftover ancestor */
			struct sinode *ancestor = si_trackget(track, n->self.id.parent);
			if (ancestor && (ancestor != n))
				ancestor->recover |= SI_RDB_REMOVE;
			break;
		}
		case SI_RDB_DBSEAL: {
			/* find parent */
			struct sinode *parent = si_trackget(track, n->self.id.parent);
			if (parent) {
				/* schedule node for removal, if has incomplete merges */
				if (parent->recover & SI_RDB_DBI)
					n->recover |= SI_RDB_REMOVE;
				else
					parent->recover |= SI_RDB_REMOVE;
			}
			if (! (n->recover & SI_RDB_REMOVE)) {
				/* complete node */
				int rc = si_nodecomplete(n, r, &i->scheme);
				if (ssunlikely(rc == -1))
					return -1;
				n->recover = SI_RDB;
			}
			break;
		}
		default:
			/* corrupted states */
			return sr_malfunction(r->e, "corrupted database repository: %s",
			                      i->scheme.path);
		}
		p = ss_rbprev(&track->i, p);
	}
	return 0;
}

static inline int
si_recovercomplete(struct sitrack *track, struct runtime *r, struct si *index, struct ssbuf *buf)
{
	/* prepare and build primary index */
	ss_bufreset(buf);
	struct ssrbnode *p = ss_rbmin(&track->i);
	while (p) {
		struct sinode *n = sscast(p, struct sinode, node);
		int rc = ss_bufadd(buf, r->a, &n, sizeof(struct sinode*));
		if (ssunlikely(rc == -1))
			return sr_oom_malfunction(r->e);
		p = ss_rbnext(&track->i, p);
	}
	struct ssiter i;
	ss_iterinit(ss_bufiterref, &i);
	ss_iteropen(ss_bufiterref, &i, buf, sizeof(struct sinode*));
	while (ss_iterhas(ss_bufiterref, &i))
	{
		struct sinode *n = ss_iterof(ss_bufiterref, &i);
		if (n->recover & SI_RDB_REMOVE) {
			int rc = si_nodefree(n, r, 1);
			if (ssunlikely(rc == -1))
				return -1;
			ss_iternext(ss_bufiterref, &i);
			continue;
		}
		n->recover = SI_RDB;
		si_insert(index, n);
		si_plannerupdate(&index->p, SI_COMPACT|SI_BRANCH|SI_TEMP, n);
		ss_iternext(ss_bufiterref, &i);
	}
	return 0;
}

static inline int
si_tracksnapshot(struct sitrack *track, struct runtime *r, struct si *i, struct sdsnapshot *s)
{
	/* read snapshot */
	struct ssiter iter;
	ss_iterinit(sd_snapshotiter, &iter);
	int rc;
	rc = ss_iteropen(sd_snapshotiter, &iter, r, s);
	if (ssunlikely(rc == -1))
		return -1;
	for (; ss_iterhas(sd_snapshotiter, &iter);
	      ss_iternext(sd_snapshotiter, &iter))
	{
		struct sdsnapshotnode *n = ss_iterof(sd_snapshotiter, &iter);
		/* skip updated nodes */
		struct sspath path;
		ss_path(&path, i->scheme.path, n->id, ".db");
		rc = ss_vfsexists(r->vfs, path.path);
		if (! rc)
			continue;
		uint64_t size = ss_vfssize(r->vfs, path.path);
		if (size != n->size_file)
			continue;
		/* recover node */
		struct sinode *node = si_nodenew(r);
		if (ssunlikely(node == NULL))
			return -1;
		node->recover = SI_RDB;
		rc = si_nodeopen(node, r, &i->scheme, &path, n);
		if (ssunlikely(rc == -1)) {
			si_nodefree(node, r, 0);
			return -1;
		}
		si_trackmetrics(track, node);
		si_trackset(track, node);
	}
	/* recover index temperature (read stats) */
	struct sdsnapshotheader *h = sd_snapshot_header(s);
	i->read_cache = h->read_cache;
	i->read_disk  = h->read_disk;
	i->lru_v      = h->lru_v;
	i->lru_steps  = h->lru_steps;
	return 0;
}

static inline void
si_recoversize(struct si *i)
{
	struct ssrbnode *pn = ss_rbmin(&i->i);
	while (pn) {
		struct sinode *n = sscast(pn, struct sinode, node);
		i->size += si_nodesize(n);
		pn = ss_rbnext(&i->i, pn);
	}
}

static inline int
si_recoverindex(struct si *i, struct runtime *r, struct sdsnapshot *s)
{
	struct sitrack track;
	si_trackinit(&track);
	struct ssbuf buf;
	ss_bufinit(&buf);
	int rc;
	if (sd_snapshot_is(s)) {
		rc = si_tracksnapshot(&track, r, i, s);
		if (ssunlikely(rc == -1))
			goto error;
	}
	rc = si_trackdir(&track, r, i);
	if (ssunlikely(rc == -1))
		goto error;
	if (ssunlikely(track.count == 0))
		return 1;
	rc = si_trackvalidate(&track, &buf, r, i);
	if (ssunlikely(rc == -1))
		goto error;
	rc = si_recovercomplete(&track, r, i, &buf);
	if (ssunlikely(rc == -1))
		goto error;
	/* set actual metrics */
	if (track.nsn > r->seq->nsn)
		r->seq->nsn = track.nsn;
	if (track.lsn > r->seq->lsn)
		r->seq->lsn = track.lsn;
	si_recoversize(i);
	ss_buffree(&buf, r->a);
	return 0;
error:
	ss_buffree(&buf, r->a);
	si_trackfree(&track, r);
	return -1;
}

static inline int
si_recoverdrop(struct si *i, struct runtime *r)
{
	char path[1024];
	snprintf(path, sizeof(path), "%s/drop", i->scheme.path);
	int rc = ss_vfsexists(r->vfs, path);
	if (sslikely(! rc))
		return 0;
	if (i->scheme.path_fail_on_drop) {
		sr_malfunction(r->e, "attempt to recover a dropped database: %s:",
		               i->scheme.path);
		return -1;
	}
	rc = si_droprepository(r, i->scheme.path, 0);
	if (ssunlikely(rc == -1))
		return -1;
	return 1;
}

static inline int
si_recoversnapshot(struct si *i, struct runtime *r, struct sdsnapshot *s)
{
	/* recovery stages:

	   snapshot            (1) ok
	   snapshot.incomplete (2) remove snapshot.incomplete
	   snapshot            (3) remove snapshot.incomplete, load snapshot
	   snapshot.incomplete
	*/

	/* recover snapshot file (crash recover) */
	int snapshot = 0;
	int snapshot_incomplete = 0;

	char path[1024];
	snprintf(path, sizeof(path), "%s/index", i->scheme.path);
	snapshot = ss_vfsexists(r->vfs, path);
	snprintf(path, sizeof(path), "%s/index.incomplete", i->scheme.path);
	snapshot_incomplete = ss_vfsexists(r->vfs, path);

	int rc;
	if (snapshot_incomplete) {
		rc = ss_vfsunlink(r->vfs, path);
		if (ssunlikely(rc == -1)) {
			sr_malfunction(r->e, "index file '%s' unlink error: %s",
			               path, strerror(errno));
			return -1;
		}
	}
	if (! snapshot)
		return 0;

	/* read snapshot file */
	snprintf(path, sizeof(path), "%s/index", i->scheme.path);

	ssize_t size = ss_vfssize(r->vfs, path);
	if (ssunlikely(size == -1)) {
		sr_malfunction(r->e, "index file '%s' read error: %s",
		               path, strerror(errno));
		return -1;
	}
	rc = ss_bufensure(&s->buf, r->a, size);
	if (ssunlikely(rc == -1))
		return sr_oom_malfunction(r->e);
	struct ssfile file;
	ss_fileinit(&file, r->vfs);
	rc = ss_fileopen(&file, path);
	if (ssunlikely(rc == -1)) {
		sr_malfunction(r->e, "index file '%s' open error: %s",
		               path, strerror(errno));
		return -1;
	}
	rc = ss_filepread(&file, 0, s->buf.s, size);
	if (ssunlikely(rc == -1)) {
		sr_malfunction(r->e, "index file '%s' read error: %s",
		               path, strerror(errno));
		ss_fileclose(&file);
		return -1;
	}
	ss_bufadvance(&s->buf, size);
	ss_fileclose(&file);
	return 0;
}

static int si_recover(struct si *i)
{
	struct runtime *r = &i->r;
	int exist = ss_vfsexists(r->vfs, i->scheme.path);
	if (exist == 0)
		goto deploy;
	if (i->scheme.path_fail_on_exists) {
		sr_error(r->e, "directory '%s' already exists", i->scheme.path);
		return -1;
	}
	int rc = si_recoverdrop(i, r);
	switch (rc) {
	case -1: return -1;
	case  1: goto deploy;
	}
	rc = si_schemerecover(&i->scheme, r);
	if (ssunlikely(rc == -1))
		return -1;
	r->scheme = &i->scheme.scheme;
	r->fmt_storage = i->scheme.fmt_storage;
	struct sdsnapshot snapshot;
	sd_snapshot_init(&snapshot);
	rc = si_recoversnapshot(i, r, &snapshot);
	if (ssunlikely(rc == -1)) {
		sd_snapshot_free(&snapshot, r);
		return -1;
	}
	rc = si_recoverindex(i, r, &snapshot);
	sd_snapshot_free(&snapshot, r);
	if (sslikely(rc <= 0))
		return rc;
deploy:
	return si_deploy(i, r, !exist);
}

enum {
	SI_SCHEME_NONE,
	SI_SCHEME_VERSION,
	SI_SCHEME_VERSION_STORAGE,
	SI_SCHEME_NAME,
	SI_SCHEME_FORMAT_STORAGE,
	SI_SCHEME_SCHEME,
	SI_SCHEME_NODE_SIZE,
	SI_SCHEME_NODE_PAGE_SIZE,
	SI_SCHEME_NODE_PAGE_CHECKSUM,
	SI_SCHEME_SYNC,
	SI_SCHEME_COMPRESSION,
	SI_SCHEME_COMPRESSION_KEY,
	SI_SCHEME_COMPRESSION_BRANCH,
	SI_SCHEME_COMPRESSION_RESERVED0,
	SI_SCHEME_COMPRESSION_RESERVED1,
	SI_SCHEME_AMQF,
	SI_SCHEME_CACHE_MODE,
	SI_SCHEME_EXPIRE
};

static void si_schemeinit(struct sischeme *s)
{
	memset(s, 0, sizeof(*s));
	sr_version(&s->version);
	sr_version_storage(&s->version_storage);
}

static void si_schemefree(struct sischeme *s, struct runtime *r)
{
	if (s->name) {
		ss_free(r->a, s->name);
		s->name = NULL;
	}
	if (s->path) {
		ss_free(r->a, s->path);
		s->path = NULL;
	}
	if (s->path_backup) {
		ss_free(r->a, s->path_backup);
		s->path_backup = NULL;
	}
	if (s->storage_sz) {
		ss_free(r->a, s->storage_sz);
		s->storage_sz = NULL;
	}
	if (s->compression_sz) {
		ss_free(r->a, s->compression_sz);
		s->compression_sz = NULL;
	}
	if (s->compression_branch_sz) {
		ss_free(r->a, s->compression_branch_sz);
		s->compression_branch_sz = NULL;
	}
	sf_schemefree(&s->scheme, r->a);
}

static int si_schemedeploy(struct sischeme *s, struct runtime *r)
{
	struct sdscheme c;
	sd_schemeinit(&c);
	int rc;
	rc = sd_schemebegin(&c, r);
	if (ssunlikely(rc == -1))
		return -1;
	struct ssbuf buf;
	ss_bufinit(&buf);
	rc = sd_schemeadd(&c, r, SI_SCHEME_VERSION, SS_STRING, &s->version,
	                  sizeof(s->version));
	if (ssunlikely(rc == -1))
		goto error;
	rc = sd_schemeadd(&c, r, SI_SCHEME_VERSION_STORAGE, SS_STRING,
	                  &s->version_storage, sizeof(s->version_storage));
	if (ssunlikely(rc == -1))
		goto error;
	rc = sd_schemeadd(&c, r, SI_SCHEME_NAME, SS_STRING, s->name,
	                  strlen(s->name) + 1);
	if (ssunlikely(rc == -1))
		goto error;
	rc = sf_schemesave(&s->scheme, r->a, &buf);
	if (ssunlikely(rc == -1))
		goto error;
	rc = sd_schemeadd(&c, r, SI_SCHEME_SCHEME, SS_STRING, buf.s,
	                  ss_bufused(&buf));
	if (ssunlikely(rc == -1))
		goto error;
	ss_buffree(&buf, r->a);
	uint32_t v;
	v = s->fmt_storage;
	rc = sd_schemeadd(&c, r, SI_SCHEME_FORMAT_STORAGE, SS_U32, &v, sizeof(v));
	if (ssunlikely(rc == -1))
		goto error;
	rc = sd_schemeadd(&c, r, SI_SCHEME_NODE_SIZE, SS_U64,
	                  &s->node_size,
	                  sizeof(s->node_size));
	if (ssunlikely(rc == -1))
		goto error;
	rc = sd_schemeadd(&c, r, SI_SCHEME_NODE_PAGE_SIZE, SS_U32,
	                  &s->node_page_size,
	                  sizeof(s->node_page_size));
	if (ssunlikely(rc == -1))
		goto error;
	rc = sd_schemeadd(&c, r, SI_SCHEME_NODE_PAGE_CHECKSUM, SS_U32,
	                  &s->node_page_checksum,
	                  sizeof(s->node_page_checksum));
	if (ssunlikely(rc == -1))
		goto error;
	rc = sd_schemeadd(&c, r, SI_SCHEME_SYNC, SS_U32,
	                  &s->sync,
	                  sizeof(s->sync));
	if (ssunlikely(rc == -1))
		goto error;
	rc = sd_schemeadd(&c, r, SI_SCHEME_COMPRESSION, SS_STRING,
	                  s->compression_if->name,
	                  strlen(s->compression_if->name) + 1);
	if (ssunlikely(rc == -1))
		goto error;
	rc = sd_schemeadd(&c, r, SI_SCHEME_COMPRESSION_BRANCH, SS_STRING,
	                  s->compression_branch_if->name,
	                  strlen(s->compression_branch_if->name) + 1);
	if (ssunlikely(rc == -1))
		goto error;
	rc = sd_schemeadd(&c, r, SI_SCHEME_COMPRESSION_KEY, SS_U32,
	                  &s->compression_key,
	                  sizeof(s->compression_key));
	if (ssunlikely(rc == -1))
		goto error;
	rc = sd_schemeadd(&c, r, SI_SCHEME_AMQF, SS_U32,
	                  &s->amqf, sizeof(s->amqf));
	if (ssunlikely(rc == -1))
		goto error;
	rc = sd_schemeadd(&c, r, SI_SCHEME_EXPIRE, SS_U32,
	                  &s->expire, sizeof(s->expire));
	if (ssunlikely(rc == -1))
		goto error;
	rc = sd_schemecommit(&c);
	if (ssunlikely(rc == -1))
		return -1;
	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/scheme", s->path);
	rc = sd_schemewrite(&c, r, path, 0);
	sd_schemefree(&c, r);
	return rc;
error:
	ss_buffree(&buf, r->a);
	sd_schemefree(&c, r);
	return -1;
}

static int si_schemerecover(struct sischeme *s, struct runtime *r)
{
	struct sdscheme c;
	sd_schemeinit(&c);
	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/scheme", s->path);
	int version_storage_set = 0;
	int rc;
	rc = sd_schemerecover(&c, r, path);
	if (ssunlikely(rc == -1))
		goto error;
	struct ssiter i;
	ss_iterinit(sd_schemeiter, &i);
	rc = ss_iteropen(sd_schemeiter, &i, r, &c, 1);
	if (ssunlikely(rc == -1))
		goto error;
	while (ss_iterhas(sd_schemeiter, &i))
	{
		struct sdschemeopt *opt = ss_iterof(sd_schemeiter, &i);
		switch (opt->id) {
		case SI_SCHEME_VERSION:
			break;
		case SI_SCHEME_VERSION_STORAGE: {
			if (opt->size != sizeof(struct srversion))
				goto error;
			struct srversion *version = (struct srversion*)sd_schemesz(opt);
			if (! sr_versionstorage_check(version))
				goto error_format;
			version_storage_set = 1;
			break;
		}
		case SI_SCHEME_FORMAT_STORAGE:
			s->fmt_storage = sd_schemeu32(opt);
			break;
		case SI_SCHEME_SCHEME: {
			sf_schemefree(&s->scheme, r->a);
			sf_schemeinit(&s->scheme);
			struct ssbuf buf;
			ss_bufinit(&buf);
			rc = sf_schemeload(&s->scheme, r->a, sd_schemesz(opt), opt->size);
			if (ssunlikely(rc == -1))
				goto error;
			rc = sf_schemevalidate(&s->scheme, r->a);
			if (ssunlikely(rc == -1))
				goto error;
			ss_buffree(&buf, r->a);
			break;
		}
		case SI_SCHEME_NODE_SIZE:
			s->node_size = sd_schemeu64(opt);
			break;
		case SI_SCHEME_NODE_PAGE_SIZE:
			s->node_page_size = sd_schemeu32(opt);
			break;
		case SI_SCHEME_COMPRESSION_KEY:
			s->compression_key = sd_schemeu32(opt);
			break;
		case SI_SCHEME_COMPRESSION: {
			char *name = sd_schemesz(opt);
			struct ssfilterif *cif = ss_filterof(name);
			if (ssunlikely(cif == NULL))
				goto error;
			s->compression_if = cif;
			s->compression = s->compression_if != &ss_nonefilter;
			ss_free(r->a, s->compression_sz);
			s->compression_sz = ss_strdup(r->a, cif->name);
			if (ssunlikely(s->compression_sz == NULL))
				goto error;
			break;
		}
		case SI_SCHEME_COMPRESSION_BRANCH: {
			char *name = sd_schemesz(opt);
			struct ssfilterif *cif = ss_filterof(name);
			if (ssunlikely(cif == NULL))
				goto error;
			s->compression_branch_if = cif;
			s->compression_branch = s->compression_branch_if != &ss_nonefilter;
			ss_free(r->a, s->compression_branch_sz);
			s->compression_branch_sz = ss_strdup(r->a, cif->name);
			if (ssunlikely(s->compression_branch_sz == NULL))
				goto error;
			break;
		}
		case SI_SCHEME_AMQF:
			s->amqf = sd_schemeu32(opt);
			break;
		case SI_SCHEME_EXPIRE:
			s->expire = sd_schemeu32(opt);
			break;
		default: /* skip unknown */
			break;
		}
		ss_iternext(sd_schemeiter, &i);
	}
	if (ssunlikely(! version_storage_set))
		goto error_format;
	sd_schemefree(&c, r);
	return 0;
error_format:
	sr_error(r->e, "%s", "incompatible storage format version");
error:
	sd_schemefree(&c, r);
	return -1;
}

static int si_snapshot(struct si *index, struct siplan *plan)
{
	struct runtime *r = &index->r;

	struct ssfile file;
	ss_fileinit(&file, r->vfs);

	/* prepare to take snapshot */
	struct sdsnapshot snapshot;
	sd_snapshot_init(&snapshot);
	int rc = ss_bufensure(&snapshot.buf, r->a, 1 * 1024 * 1024);
	if (ssunlikely(rc == -1))
		goto error_oom;
	rc = sd_snapshot_begin(&snapshot, r);
	if (ssunlikely(rc == -1))
		goto error_oom;

	/* save node index image */
	si_lock(index);
	struct ssrbnode *p = NULL;
	while ((p = ss_rbnext(&index->i, p)))
	{
		struct sinode *n = sscast(p, struct sinode, node);
		rc = sd_snapshot_add(&snapshot, r, n->self.id.id,
		                     n->file.size,
		                     n->branch_count,
		                     n->temperature_reads);
		if (ssunlikely(rc == -1)) {
			si_unlock(index);
			goto error_oom;
		}
		struct sibranch *b = &n->self;
		while (b) {
			rc = sd_snapshot_addbranch(&snapshot, r, b->index.h);
			if (ssunlikely(rc == -1)) {
				si_unlock(index);
				goto error_oom;
			}
			b = b->link;
		}
	}
	sd_snapshot_commit(&snapshot,
	                   index->lru_v,
	                   index->lru_steps,
	                   index->lru_intr_lsn,
	                   index->lru_intr_sum,
	                   index->read_disk,
	                   index->read_cache);
	si_unlock(index);

	/* create snapshot.inprogress */
	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/index.incomplete",
	         index->scheme.path);
	rc = ss_filenew(&file, path);
	if (ssunlikely(rc == -1)) {
		sr_malfunction(r->e, "index file '%s' create error: %s",
		               path, strerror(errno));
		goto error;
	}
	rc = ss_filewrite(&file, snapshot.buf.s, ss_bufused(&snapshot.buf));
	if (ssunlikely(rc == -1)) {
		sr_malfunction(r->e, "index file '%s' write error: %s",
		               path, strerror(errno));
		goto error;
	}

	SS_INJECTION(r->i, SS_INJECTION_SI_SNAPSHOT_0,
	             ss_fileclose(&file);
	             sd_snapshot_free(&snapshot, r);
				 sr_malfunction(r->e, "%s", "error injection");
				 return -1);

	/* sync snapshot file */
	if (index->scheme.sync) {
		rc = ss_filesync(&file);
		if (ssunlikely(rc == -1)) {
			sr_malfunction(r->e, "index file '%s' sync error: %s",
			               path, strerror(errno));
			goto error;
		}
	}

	/* remove old snapshot file (if exists) */
	snprintf(path, sizeof(path), "%s/index", index->scheme.path);
	ss_vfsunlink(r->vfs, path);

	SS_INJECTION(r->i, SS_INJECTION_SI_SNAPSHOT_1,
	             ss_fileclose(&file);
	             sd_snapshot_free(&snapshot, r);
				 sr_malfunction(r->e, "%s", "error injection");
				 return -1);

	/* rename snapshot.incomplete to snapshot */
	rc = ss_filerename(&file, path);
	if (ssunlikely(rc == -1)) {
		sr_malfunction(r->e, "index file '%s' rename error: %s",
		               ss_pathof(&file.path),
		               strerror(errno));
		goto error;
	}

	SS_INJECTION(r->i, SS_INJECTION_SI_SNAPSHOT_2,
	             ss_fileclose(&file);
	             sd_snapshot_free(&snapshot, r);
				 sr_malfunction(r->e, "%s", "error injection");
				 return -1);

	/* close snapshot file */
	rc = ss_fileclose(&file);
	if (ssunlikely(rc == -1)) {
		sr_malfunction(r->e, "index file '%s' close error: %s",
		               path, strerror(errno));
		goto error;
	}

	sd_snapshot_free(&snapshot, r);

	/* finish index snapshot */
	si_lock(index);
	index->snapshot = plan->a;
	index->snapshot_run = 0;
	si_unlock(index);
	return 0;

error_oom:
	sr_oom(r->e);
error:
	ss_fileclose(&file);
	sd_snapshot_free(&snapshot, r);
	return -1;
}

static void si_begin(struct sitx *x, struct si *index)
{
	x->index = index;
	rlist_create(&x->nodelist);
	si_lock(index);
}

static void si_commit(struct sitx *x)
{
	/* reschedule nodes */
	struct sinode *node, *n;
	rlist_foreach_entry_safe(node, &x->nodelist, commit, n) {
		rlist_create(&node->commit);
		si_plannerupdate(&x->index->p, SI_BRANCH, node);
	}
	si_unlock(x->index);
}

static inline int si_set(struct sitx *x, struct svv *v, uint64_t time)
{
	struct si *index = x->index;
	index->update_time = time;
	/* match node */
	struct ssiter i;
	ss_iterinit(si_iter, &i);
	ss_iteropen(si_iter, &i, &index->r, index, SS_GTE,
	            sv_vpointer(v), v->size);
	struct sinode *node = ss_iterof(si_iter, &i);
	assert(node != NULL);
	struct svref *ref = sv_refnew(&index->r, v);
	assert(ref != NULL);
	/* insert into node index */
	struct svindex *vindex = si_nodeindex(node);
	struct svindexpos pos;
	sv_indexget(vindex, &index->r, &pos, ref);
	sv_indexupdate(vindex, &pos, ref);
	/* update node */
	node->update_time = index->update_time;
	node->used += sv_vsize(v);
	if (index->scheme.lru)
		si_lru_add(index, ref);
	si_txtrack(x, node);
	return 0;
}

static void
si_write(struct sitx *x, struct svlog *l, struct svlogindex *li, uint64_t time,
	 int recover)
{
	struct runtime *r = &x->index->r;
	struct svlogv *cv = sv_logat(l, li->head);
	int c = li->count;
	while (c) {
		struct svv *v = cv->v.v;
		if (recover) {
			if (si_readcommited(x->index, r, &cv->v, recover)) {
				uint32_t gc = si_gcv(r, v);
				ss_quota(r->quota, SS_QREMOVE, gc);
				goto next;
			}
		}
		if (v->flags & SVGET) {
			assert(v->log == NULL);
			sv_vunref(r, v);
			goto next;
		}
		si_set(x, v, time);
next:
		cv = sv_logat(l, cv->next);
		c--;
	}
	return;
}

struct syconf {
	char *path;
	int   path_create;
	char *path_backup;
	int   sync;
};

struct sy {
	struct syconf *conf;
};

static int sy_init(struct sy*);
static int sy_open(struct sy*, struct runtime*, struct syconf*);
static int sy_close(struct sy*, struct runtime*);

static int sy_init(struct sy *e)
{
	e->conf = NULL;
	return 0;
}

static int
sy_deploy(struct sy *e, struct runtime *r)
{
	int rc;
	rc = ss_vfsmkdir(r->vfs, e->conf->path, 0755);
	if (ssunlikely(rc == -1)) {
		sr_error(r->e, "directory '%s' create error: %s",
		         e->conf->path, strerror(errno));
		return -1;
	}
	return 0;
}

static inline ssize_t
sy_processid(char **str) {
	char *s = *str;
	size_t v = 0;
	while (*s && *s != '.') {
		if (ssunlikely(!isdigit(*s)))
			return -1;
		v = (v * 10) + *s - '0';
		s++;
	}
	*str = s;
	return v;
}

static inline int
sy_process(char *name, uint32_t *bsn)
{
	/* id */
	/* id.incomplete */
	char *token = name;
	ssize_t id = sy_processid(&token);
	if (ssunlikely(id == -1))
		return -1;
	*bsn = id;
	if (strcmp(token, ".incomplete") == 0)
		return 1;
	return 0;
}

static inline int
sy_recoverbackup(struct sy *i, struct runtime *r)
{
	if (i->conf->path_backup == NULL)
		return 0;
	int rc;
	int exists = ss_vfsexists(r->vfs, i->conf->path_backup);
	if (! exists) {
		rc = ss_vfsmkdir(r->vfs, i->conf->path_backup, 0755);
		if (ssunlikely(rc == -1)) {
			sr_error(r->e, "backup directory '%s' create error: %s",
					 i->conf->path_backup, strerror(errno));
			return -1;
		}
	}
	/* recover backup sequential number */
	DIR *dir = opendir(i->conf->path_backup);
	if (ssunlikely(dir == NULL)) {
		sr_error(r->e, "backup directory '%s' open error: %s",
				 i->conf->path_backup, strerror(errno));
		return -1;
	}
	uint32_t bsn = 0;
	struct dirent *de;
	while ((de = readdir(dir))) {
		if (ssunlikely(de->d_name[0] == '.'))
			continue;
		uint32_t id = 0;
		rc = sy_process(de->d_name, &id);
		switch (rc) {
		case  1:
		case  0:
			if (id > bsn)
				bsn = id;
			break;
		case -1: /* skip unknown file */
			continue;
		}
	}
	closedir(dir);
	r->seq->bsn = bsn;
	return 0;
}

static int sy_open(struct sy *e, struct runtime *r, struct syconf *conf)
{
	e->conf = conf;
	int rc = sy_recoverbackup(e, r);
	if (ssunlikely(rc == -1))
		return -1;
	int exists = ss_vfsexists(r->vfs, conf->path);
	if (exists == 0) {
		if (ssunlikely(! conf->path_create)) {
			sr_error(r->e, "directory '%s' does not exist", conf->path);
			return -1;
		}
		return sy_deploy(e, r);
	}
	return 0;
}

static int sy_close(struct sy *e, struct runtime *r)
{
	(void)e;
	(void)r;
	return 0;
}

struct scworker {
	char name[16];
	struct sstrace trace;
	struct sdc dc;
	struct rlist link;
	struct rlist linkidle;
} sspacked;

struct scworkerpool {
	pthread_mutex_t lock;
	struct rlist list;
	struct rlist listidle;
	int total;
	int idle;
};

static int sc_workerpool_init(struct scworkerpool*);
static int sc_workerpool_new(struct scworkerpool*, struct runtime*);

static inline struct scworker*
sc_workerpool_pop(struct scworkerpool *p, struct runtime *r)
{
	tt_pthread_mutex_lock(&p->lock);
	if (sslikely(p->idle >= 1))
		goto pop_idle;
	int rc = sc_workerpool_new(p, r);
	if (ssunlikely(rc == -1)) {
		tt_pthread_mutex_unlock(&p->lock);
		return NULL;
	}
	assert(p->idle >= 1);
pop_idle:;
	struct scworker *w =
		rlist_shift_tail_entry(&p->listidle, struct scworker, linkidle);
	p->idle--;
	tt_pthread_mutex_unlock(&p->lock);
	return w;
}

static inline void
sc_workerpool_push(struct scworkerpool *p, struct scworker *w)
{
	tt_pthread_mutex_lock(&p->lock);
	rlist_add_tail(&p->listidle, &w->linkidle);
	p->idle++;
	tt_pthread_mutex_unlock(&p->lock);
}

enum {
	SC_QBRANCH  = 0,
	SC_QGC      = 1,
	SC_QEXPIRE  = 2,
	SC_QLRU     = 3,
	SC_QBACKUP  = 4,
	SC_QMAX
};

struct scdb {
	uint32_t workers[SC_QMAX];
	struct si *index;
	uint32_t active;
};

struct sctask {
	struct siplan plan;
	struct scdb  *db;
	struct si    *shutdown;
	int    on_backup;
	int    rotate;
	int    gc;
};

struct scheduler {
	pthread_mutex_t        lock;
	uint64_t       checkpoint_lsn_last;
	uint64_t       checkpoint_lsn;
	uint32_t       checkpoint;
	uint32_t       age;
	uint64_t       age_time;
	uint32_t       expire;
	uint64_t       expire_time;
	uint64_t       anticache_asn;
	uint64_t       anticache_asn_last;
	uint64_t       anticache_storage;
	uint64_t       anticache_time;
	uint64_t       anticache_limit;
	uint64_t       anticache;
	uint64_t       snapshot_ssn;
	uint64_t       snapshot_ssn_last;
	uint64_t       snapshot_time;
	uint64_t       snapshot;
	uint64_t       gc_time;
	uint32_t       gc;
	uint64_t       lru_time;
	uint32_t       lru;
	uint32_t       backup_bsn;
	uint32_t       backup_bsn_last;
	uint32_t       backup_bsn_last_complete;
	uint32_t       backup_events;
	uint32_t       backup;
	int            rotate;
	int            rr;
	int            count;
	struct scdb         **i;
	struct rlist   shutdown;
	int            shutdown_pending;
	struct scworkerpool   wp;
	struct slpool        *lp;
	char          *backup_path;
	struct sstrigger     *on_event;
	struct runtime            *r;
};

static int sc_init(struct scheduler*, struct runtime*, struct sstrigger*, struct slpool*);
static int sc_set(struct scheduler *s, uint64_t, char*);
static int sc_add(struct scheduler*, struct si*);
static int sc_del(struct scheduler*, struct si*, int);

static inline void
sc_start(struct scheduler *s, int task)
{
	int i = 0;
	while (i < s->count) {
		s->i[i]->active |= task;
		i++;
	}
}

static inline int
sc_end(struct scheduler *s, struct scdb *db, int task)
{
	db->active &= ~task;
	int complete = 1;
	int i = 0;
	while (i < s->count) {
		if (s->i[i]->active & task)
			complete = 0;
		i++;
	}
	return complete;
}

static inline void
sc_task_checkpoint(struct scheduler *s)
{
	uint64_t lsn = sr_seq(s->r->seq, SR_LSN);
	s->checkpoint_lsn = lsn;
	s->checkpoint = 1;
	sc_start(s, SI_CHECKPOINT);
}

static inline void
sc_task_checkpoint_done(struct scheduler *s)
{
	s->checkpoint = 0;
	s->checkpoint_lsn_last = s->checkpoint_lsn;
	s->checkpoint_lsn = 0;
}

static inline void
sc_task_anticache(struct scheduler *s)
{
	s->anticache = 1;
	s->anticache_storage = s->anticache_limit;
	s->anticache_asn = sr_seq(s->r->seq, SR_ASNNEXT);
	sc_start(s, SI_ANTICACHE);
}

static inline void
sc_task_anticache_done(struct scheduler *s, uint64_t now)
{
	s->anticache = 0;
	s->anticache_asn_last = s->anticache_asn;
	s->anticache_asn = 0;
	s->anticache_storage = 0;
	s->anticache_time = now;
}

static inline void
sc_task_snapshot(struct scheduler *s)
{
	s->snapshot = 1;
	s->snapshot_ssn = sr_seq(s->r->seq, SR_SSNNEXT);
	sc_start(s, SI_SNAPSHOT);
}

static inline void
sc_task_snapshot_done(struct scheduler *s, uint64_t now)
{
	s->snapshot = 0;
	s->snapshot_ssn_last = s->snapshot_ssn;
	s->snapshot_ssn = 0;
	s->snapshot_time = now;
}

static inline void
sc_task_expire(struct scheduler *s)
{
	s->expire = 1;
	sc_start(s, SI_EXPIRE);
}

static inline void
sc_task_expire_done(struct scheduler *s, uint64_t now)
{
	s->expire = 0;
	s->expire_time = now;
}

static inline void
sc_task_gc(struct scheduler *s)
{
	s->gc = 1;
	sc_start(s, SI_GC);
}

static inline void
sc_task_gc_done(struct scheduler *s, uint64_t now)
{
	s->gc = 0;
	s->gc_time = now;
}

static inline void
sc_task_lru(struct scheduler *s)
{
	s->lru = 1;
	sc_start(s, SI_LRU);
}

static inline void
sc_task_lru_done(struct scheduler *s, uint64_t now)
{
	s->lru = 0;
	s->lru_time = now;
}

static inline void
sc_task_age(struct scheduler *s)
{
	s->age = 1;
	sc_start(s, SI_AGE);
}

static inline void
sc_task_age_done(struct scheduler *s, uint64_t now)
{
	s->age = 0;
	s->age_time = now;
}

static int sc_step(struct scheduler*, struct scworker*, uint64_t);

static int sc_ctl_call(struct scheduler*, uint64_t);
static int sc_ctl_branch(struct scheduler*, uint64_t, struct si*);
static int sc_ctl_compact(struct scheduler*, uint64_t, struct si*);
static int sc_ctl_compact_index(struct scheduler*, uint64_t, struct si*);
static int sc_ctl_anticache(struct scheduler*);
static int sc_ctl_snapshot(struct scheduler*);
static int sc_ctl_checkpoint(struct scheduler*);
static int sc_ctl_expire(struct scheduler*);
static int sc_ctl_gc(struct scheduler*);
static int sc_ctl_lru(struct scheduler*);
static int sc_ctl_backup(struct scheduler*);
static int sc_ctl_shutdown(struct scheduler*, struct si*);

struct screadarg {
	struct sv        v;
	char     *prefix;
	int       prefixsize;
	struct sv        vup;
	struct sicache  *cache;
	int       cachegc;
	enum ssorder   order;
	int       has;
	int       upsert;
	int       upsert_eq;
	int       cache_only;
	int       oldest_only;
	uint64_t  vlsn;
	int       vlsn_generate;
};

struct scread {
	struct so         *db;
	struct si         *index;
	struct screadarg   arg;
	int         start;
	int         read_disk;
	int         read_cache;
	struct svv        *result;
	int         rc;
	struct runtime         *r;
};

static void sc_readopen(struct scread*, struct runtime*, struct so*, struct si*);
static void sc_readclose(struct scread*);
static int sc_read(struct scread*, struct scheduler*);

static int sc_write(struct scheduler*, struct svlog*, uint64_t, int);

static int sc_backupstart(struct scheduler*);
static int sc_backupbegin(struct scheduler*);
static int sc_backupend(struct scheduler*, struct scworker*);
static int sc_backuperror(struct scheduler*);

static int sc_backupstart(struct scheduler *s)
{
	if (ssunlikely(s->backup_path == NULL)) {
		sr_error(s->r->e, "%s", "backup is not enabled");
		return -1;
	}
	/* begin backup procedure
	 * state 0
	 *
	 * disable log garbage-collection
	*/
	sl_poolgc_enable(s->lp, 0);
	tt_pthread_mutex_lock(&s->lock);
	if (ssunlikely(s->backup > 0)) {
		tt_pthread_mutex_unlock(&s->lock);
		sl_poolgc_enable(s->lp, 1);
		/* in progress */
		return 0;
	}
	uint64_t bsn = sr_seq(s->r->seq, SR_BSNNEXT);
	s->backup = 1;
	s->backup_bsn = bsn;
	sc_start(s, SI_BACKUP);
	tt_pthread_mutex_unlock(&s->lock);
	return 0;
}

static int sc_backupbegin(struct scheduler *s)
{
	/*
	 * a. create backup_path/<bsn.incomplete> directory
	 * b. create database directories
	 * c. create log directory
	*/
	char path[1024];
	snprintf(path, sizeof(path), "%s/%" PRIu32 ".incomplete",
	         s->backup_path, s->backup_bsn);
	int rc = ss_vfsmkdir(s->r->vfs, path, 0755);
	if (ssunlikely(rc == -1)) {
		sr_error(s->r->e, "backup directory '%s' create error: %s",
		         path, strerror(errno));
		return -1;
	}
	int i = 0;
	while (i < s->count) {
		struct scdb *db = s->i[i];
		snprintf(path, sizeof(path), "%s/%" PRIu32 ".incomplete/%s",
		         s->backup_path, s->backup_bsn,
		         db->index->scheme.name);
		rc = ss_vfsmkdir(s->r->vfs, path, 0755);
		if (ssunlikely(rc == -1)) {
			sr_error(s->r->e, "backup directory '%s' create error: %s",
			         path, strerror(errno));
			return -1;
		}
		i++;
	}
	snprintf(path, sizeof(path), "%s/%" PRIu32 ".incomplete/log",
	         s->backup_path, s->backup_bsn);
	rc = ss_vfsmkdir(s->r->vfs, path, 0755);
	if (ssunlikely(rc == -1)) {
		sr_error(s->r->e, "backup directory '%s' create error: %s",
		         path, strerror(errno));
		return -1;
	}
	return 0;
}

static int sc_backupend(struct scheduler *s, struct scworker *w)
{
	/*
	 * a. rotate log file
	 * b. copy log files
	 * c. enable log gc
	 * d. rename <bsn.incomplete> into <bsn>
	 * e. set last backup, set COMPLETE
	 */

	/* force log rotation */
	ss_trace(&w->trace, "%s", "log rotation for backup");
	int rc = sl_poolrotate(s->lp);
	if (ssunlikely(rc == -1))
		return -1;

	/* copy log files */
	ss_trace(&w->trace, "%s", "log files backup");

	char path[1024];
	snprintf(path, sizeof(path), "%s/%" PRIu32 ".incomplete/log",
	         s->backup_path, s->backup_bsn);
	rc = sl_poolcopy(s->lp, path, &w->dc.c);
	if (ssunlikely(rc == -1)) {
		sr_errorrecover(s->r->e);
		return -1;
	}

	/* enable log gc */
	sl_poolgc_enable(s->lp, 1);

	/* complete backup */
	snprintf(path, sizeof(path), "%s/%" PRIu32 ".incomplete",
	         s->backup_path, s->backup_bsn);
	char newpath[1024];
	snprintf(newpath, sizeof(newpath), "%s/%" PRIu32,
	         s->backup_path, s->backup_bsn);
	rc = ss_vfsrename(s->r->vfs, path, newpath);
	if (ssunlikely(rc == -1)) {
		sr_error(s->r->e, "backup directory '%s' rename error: %s",
		         path, strerror(errno));
		return -1;
	}

	/* complete */
	s->backup_bsn_last = s->backup_bsn;
	s->backup_bsn_last_complete = 1;
	s->backup = 0;
	s->backup_bsn = 0;
	return 0;
}

static int sc_backuperror(struct scheduler *s)
{
	sl_poolgc_enable(s->lp, 1);
	s->backup = 0;
	s->backup_bsn_last_complete = 0;
	return 0;
}

static int
sc_init(struct scheduler *s, struct runtime *r,
	struct sstrigger *on_event, struct slpool *lp)
{
	uint64_t now = clock_monotonic64();
	tt_pthread_mutex_init(&s->lock, NULL);
	s->checkpoint_lsn           = 0;
	s->checkpoint_lsn_last      = 0;
	s->checkpoint               = 0;
	s->age                      = 0;
	s->age_time                 = now;
	s->expire                   = 0;
	s->expire_time              = now;
	s->backup_bsn               = 0;
	s->backup_bsn_last          = 0;
	s->backup_bsn_last_complete = 0;
	s->backup_events            = 0;
	s->backup                   = 0;
	s->anticache_asn            = 0;
	s->anticache_asn_last       = 0;
	s->anticache_storage        = 0;
	s->anticache_time           = now;
	s->anticache                = 0;
	s->anticache_limit          = 0;
	s->snapshot_ssn             = 0;
	s->snapshot_ssn_last        = 0;
	s->snapshot_time            = now;
	s->snapshot                 = 0;
	s->gc                       = 0;
	s->gc_time                  = now;
	s->lru                      = 0;
	s->lru_time                 = now;
	s->rotate                   = 0;
	s->i                        = NULL;
	s->count                    = 0;
	s->rr                       = 0;
	s->r                        = r;
	s->on_event                 = on_event;
	s->backup_path              = NULL;
	s->lp                       = lp;
	sc_workerpool_init(&s->wp);
	rlist_create(&s->shutdown);
	s->shutdown_pending = 0;
	return 0;
}

static int sc_set(struct scheduler *s, uint64_t anticache, char *backup_path)
{
	s->anticache_limit = anticache;
	s->backup_path = backup_path;
	return 0;
}

static int sc_add(struct scheduler *s, struct si *index)
{
	struct scdb *db = ss_malloc(s->r->a, sizeof(struct scdb));
	if (ssunlikely(db == NULL))
		return -1;
	db->index  = index;
	db->active = 0;
	memset(db->workers, 0, sizeof(db->workers));

	tt_pthread_mutex_lock(&s->lock);
	int count = s->count + 1;
	struct scdb **i = ss_malloc(s->r->a, count * sizeof(struct scdb*));
	if (ssunlikely(i == NULL)) {
		tt_pthread_mutex_unlock(&s->lock);
		ss_free(s->r->a, db);
		return -1;
	}
	memcpy(i, s->i, s->count * sizeof(struct scdb*));
	i[s->count] = db;
	void *iprev = s->i;
	s->i = i;
	s->count = count;
	tt_pthread_mutex_unlock(&s->lock);
	if (iprev)
		ss_free(s->r->a, iprev);
	return 0;
}

static int sc_del(struct scheduler *s, struct si *index, int lock)
{
	if (ssunlikely(s->i == NULL))
		return 0;
	if (lock)
		tt_pthread_mutex_lock(&s->lock);
	struct scdb *db = NULL;
	struct scdb **iprev;
	int count = s->count - 1;
	if (ssunlikely(count == 0)) {
		iprev = s->i;
		db = s->i[0];
		s->count = 0;
		s->i = NULL;
		goto free;
	}
	struct scdb **i = ss_malloc(s->r->a, count * sizeof(struct scdb*));
	if (ssunlikely(i == NULL)) {
		if (lock)
			tt_pthread_mutex_unlock(&s->lock);
		return -1;
	}
	int j = 0;
	int k = 0;
	while (j < s->count) {
		if (s->i[j]->index == index) {
			db = s->i[j];
			j++;
			continue;
		}
		i[k] = s->i[j];
		k++;
		j++;
	}
	iprev = s->i;
	s->i = i;
	s->count = count;
	if (ssunlikely(s->rr >= s->count))
		s->rr = 0;
free:
	if (lock)
		tt_pthread_mutex_unlock(&s->lock);
	ss_free(s->r->a, iprev);
	ss_free(s->r->a, db);
	return 0;
}

static int sc_ctl_call(struct scheduler *s, uint64_t vlsn)
{
	int rc = sr_statusactive(s->r->status);
	if (ssunlikely(rc == 0))
		return 0;
	struct scworker *w = sc_workerpool_pop(&s->wp, s->r);
	if (ssunlikely(w == NULL))
		return -1;
	rc = sc_step(s, w, vlsn);
	sc_workerpool_push(&s->wp, w);
	return rc;
}

static int sc_ctl_branch(struct scheduler *s, uint64_t vlsn, struct si *index)
{
	struct runtime *r = s->r;
	int rc = sr_statusactive(r->status);
	if (ssunlikely(rc == 0))
		return 0;
	struct srzone *z = sr_zoneof(r);
	struct scworker *w = sc_workerpool_pop(&s->wp, r);
	if (ssunlikely(w == NULL))
		return -1;
	while (1) {
		uint64_t vlsn_lru = si_lru_vlsn(index);
		struct siplan plan = {
			.explain   = SI_ENONE,
			.plan      = SI_BRANCH,
			.a         = z->branch_wm,
			.b         = 0,
			.c         = 0,
			.node      = NULL
		};
		rc = si_plan(index, &plan);
		if (rc == 0)
			break;
		rc = si_execute(index, &w->dc, &plan, vlsn, vlsn_lru);
		if (ssunlikely(rc == -1))
			break;
	}
	sc_workerpool_push(&s->wp, w);
	return rc;
}

static int sc_ctl_compact(struct scheduler *s, uint64_t vlsn, struct si *index)
{
	struct runtime *r = s->r;
	int rc = sr_statusactive(r->status);
	if (ssunlikely(rc == 0))
		return 0;
	struct srzone *z = sr_zoneof(r);
	struct scworker *w = sc_workerpool_pop(&s->wp, r);
	if (ssunlikely(w == NULL))
		return -1;
	while (1) {
		uint64_t vlsn_lru = si_lru_vlsn(index);
		struct siplan plan = {
			.explain   = SI_ENONE,
			.plan      = SI_COMPACT,
			.a         = z->compact_wm,
			.b         = z->compact_mode,
			.c         = 0,
			.node      = NULL
		};
		rc = si_plan(index, &plan);
		if (rc == 0)
			break;
		rc = si_execute(index, &w->dc, &plan, vlsn, vlsn_lru);
		if (ssunlikely(rc == -1))
			break;
	}
	sc_workerpool_push(&s->wp, w);
	return rc;
}

static int
sc_ctl_compact_index(struct scheduler *s, uint64_t vlsn, struct si *index)
{
	struct runtime *r = s->r;
	int rc = sr_statusactive(r->status);
	if (ssunlikely(rc == 0))
		return 0;
	struct srzone *z = sr_zoneof(r);
	struct scworker *w = sc_workerpool_pop(&s->wp, r);
	if (ssunlikely(w == NULL))
		return -1;
	while (1) {
		uint64_t vlsn_lru = si_lru_vlsn(index);
		struct siplan plan = {
			.explain   = SI_ENONE,
			.plan      = SI_COMPACT_INDEX,
			.a         = z->branch_wm,
			.b         = 0,
			.c         = 0,
			.node      = NULL
		};
		rc = si_plan(index, &plan);
		if (rc == 0)
			break;
		rc = si_execute(index, &w->dc, &plan, vlsn, vlsn_lru);
		if (ssunlikely(rc == -1))
			break;
	}
	sc_workerpool_push(&s->wp, w);
	return rc;
}

static int sc_ctl_anticache(struct scheduler *s)
{
	tt_pthread_mutex_lock(&s->lock);
	sc_task_anticache(s);
	tt_pthread_mutex_unlock(&s->lock);
	return 0;
}

static int sc_ctl_snapshot(struct scheduler *s)
{
	tt_pthread_mutex_lock(&s->lock);
	sc_task_snapshot(s);
	tt_pthread_mutex_unlock(&s->lock);
	return 0;
}

static int sc_ctl_checkpoint(struct scheduler *s)
{
	tt_pthread_mutex_lock(&s->lock);
	sc_task_checkpoint(s);
	tt_pthread_mutex_unlock(&s->lock);
	return 0;
}

static int sc_ctl_expire(struct scheduler *s)
{
	tt_pthread_mutex_lock(&s->lock);
	sc_task_expire(s);
	tt_pthread_mutex_unlock(&s->lock);
	return 0;
}

static int sc_ctl_gc(struct scheduler *s)
{
	tt_pthread_mutex_lock(&s->lock);
	sc_task_gc(s);
	tt_pthread_mutex_unlock(&s->lock);
	return 0;
}

static int sc_ctl_lru(struct scheduler *s)
{
	tt_pthread_mutex_lock(&s->lock);
	sc_task_lru(s);
	tt_pthread_mutex_unlock(&s->lock);
	return 0;
}

static int sc_ctl_backup(struct scheduler *s)
{
	return sc_backupstart(s);
}

static int sc_ctl_shutdown(struct scheduler *s, struct si *i)
{
	tt_pthread_mutex_lock(&s->lock);
	s->shutdown_pending++;
	rlist_add(&s->shutdown, &i->link);
	tt_pthread_mutex_unlock(&s->lock);
	return 0;
}

static void sc_readclose(struct scread *r)
{
	struct runtime *rt = r->r;
	/* free key, prefix, upsert and a pending result */
	if (r->arg.v.v)
		sv_vunref(rt, r->arg.v.v);
	if (r->arg.vup.v)
		sv_vunref(rt, r->arg.vup.v);
	if (ssunlikely(r->result))
		sv_vunref(rt, r->result);
	/* free read cache */
	if (sslikely(r->arg.cachegc && r->arg.cache))
		si_cachepool_push(r->arg.cache);
}

static void
sc_readopen(struct scread *r, struct runtime *rt, struct so *db, struct si *index)
{
	r->db = db;
	r->index = index;
	r->start = 0;
	r->read_disk = 0;
	r->read_cache = 0;
	r->result = NULL;
	r->rc = 0;
	r->r = rt;
}

static int sc_read(struct scread *r, struct scheduler *s)
{
	struct screadarg *arg = &r->arg;
	struct si *index = r->index;

	if (sslikely(arg->vlsn_generate))
		arg->vlsn = sr_seq(s->r->seq, SR_LSN);

	struct siread q;
	si_readopen(&q, index, arg->cache,
	            arg->order,
	            arg->vlsn,
	            arg->prefix,
	            arg->prefixsize,
	            sv_pointer(&arg->v),
	            sv_size(&arg->v));
	if (arg->upsert)
		si_readupsert(&q, &arg->vup, arg->upsert_eq);
	if (arg->cache_only)
		si_readcache_only(&q);
	if (arg->oldest_only)
		si_readoldest_only(&q);
	if (arg->has)
		si_readhas(&q);
	r->rc = si_read(&q);
	r->read_disk  += q.read_disk;
	r->read_cache += q.read_cache;
	r->result = q.result.v;
	si_readclose(&q);
	return r->rc;
}

static inline int
sc_rotate(struct scheduler*s, struct scworker *w)
{
	ss_trace(&w->trace, "%s", "log rotation");
	int rc = sl_poolrotate_ready(s->lp);
	if (rc) {
		rc = sl_poolrotate(s->lp);
		if (ssunlikely(rc == -1))
			return -1;
	}
	return 0;
}

static inline int
sc_gc(struct scheduler *s, struct scworker *w)
{
	ss_trace(&w->trace, "%s", "log gc");
	int rc = sl_poolgc(s->lp);
	if (ssunlikely(rc == -1))
		return -1;
	return 0;
}

static inline int
sc_execute(struct sctask *t, struct scworker *w, uint64_t vlsn)
{
	struct si *index;
	if (ssunlikely(t->shutdown))
		index = t->shutdown;
	else
		index = t->db->index;

	si_plannertrace(&t->plan, index->scheme.id, &w->trace);
	uint64_t vlsn_lru = si_lru_vlsn(index);
	return si_execute(index, &w->dc, &t->plan, vlsn, vlsn_lru);
}

static inline struct scdb*
sc_peek(struct scheduler *s)
{
	if (s->rr >= s->count)
		s->rr = 0;
	int start = s->rr;
	int limit = s->count;
	int i = start;
first_half:
	while (i < limit) {
		struct scdb *db = s->i[i];
		if (ssunlikely(! si_active(db->index))) {
			i++;
			continue;
		}
		s->rr = i;
		return db;
	}
	if (i > start) {
		i = 0;
		limit = start;
		goto first_half;
	}
	s->rr = 0;
	return NULL;
}

static inline void
sc_next(struct scheduler *s)
{
	s->rr++;
	if (s->rr >= s->count)
		s->rr = 0;
}

static inline int
sc_plan(struct scheduler *s, struct siplan *plan)
{
	struct scdb *db = s->i[s->rr];
	return si_plan(db->index, plan);


}

static inline int
sc_planquota(struct scheduler *s, struct siplan *plan, uint32_t quota, uint32_t quota_limit)
{
	struct scdb *db = s->i[s->rr];
	if (db->workers[quota] >= quota_limit)
		return 2;
	return si_plan(db->index, plan);
}

static inline int
sc_do_shutdown(struct scheduler *s, struct sctask *task)
{
	if (sslikely(s->shutdown_pending == 0))
		return 0;
	struct si *index, *n;
	rlist_foreach_entry_safe(index, &s->shutdown, link, n) {
		task->plan.plan = SI_SHUTDOWN;
		int rc;
		rc = si_plan(index, &task->plan);
		if (rc == 1) {
			s->shutdown_pending--;
			rlist_del(&index->link);
			sc_del(s, index, 0);
			task->shutdown = index;
			task->db = NULL;
			task->gc = 1;
			return 1;
		}
	}
	return 0;
}

static int
sc_do(struct scheduler *s, struct sctask *task, struct scworker *w, struct srzone *zone,
      struct scdb *db, uint64_t vlsn, uint64_t now)
{
	int rc;
	ss_trace(&w->trace, "%s", "schedule");

	/* node gc */
	task->plan.plan = SI_NODEGC;
	rc = sc_plan(s, &task->plan);
	if (rc == 1) {
		si_ref(db->index, SI_REFBE);
		task->db = db;
		return 1;
	}

	/* checkpoint */
	if (s->checkpoint) {
		task->plan.plan = SI_CHECKPOINT;
		task->plan.a = s->checkpoint_lsn;
		rc = sc_plan(s, &task->plan);
		switch (rc) {
		case 1:
			db->workers[SC_QBRANCH]++;
			si_ref(db->index, SI_REFBE);
			task->db = db;
			task->gc = 1;
			return 1;
		case 0: /* complete */
			if (sc_end(s, db, SI_CHECKPOINT))
				sc_task_checkpoint_done(s);
			break;
		}
	}

	/* anti-cache */
	if (s->anticache) {
		task->plan.plan = SI_ANTICACHE;
		task->plan.a = s->anticache_asn;
		task->plan.b = s->anticache_storage;
		rc = sc_plan(s, &task->plan);
		switch (rc) {
		case 1:
			si_ref(db->index, SI_REFBE);
			task->db = db;
			uint64_t size = task->plan.c;
			if (size > 0) {
				if (ssunlikely(size > s->anticache_storage))
					s->anticache_storage = 0;
				else
					s->anticache_storage -= size;
			}
			return 1;
		case 0: /* complete */
			if (sc_end(s, db, SI_ANTICACHE))
				sc_task_anticache_done(s, now);
			break;
		}
	}

	/* snapshot */
	if (s->snapshot) {
		task->plan.plan = SI_SNAPSHOT;
		task->plan.a = s->snapshot_ssn;
		rc = sc_plan(s, &task->plan);
		switch (rc) {
		case 1:
			si_ref(db->index, SI_REFBE);
			task->db = db;
			return 1;
		case 0: /* complete */
			if (sc_end(s, db, SI_SNAPSHOT))
				sc_task_snapshot_done(s, now);
			break;
		}
	}

	/* backup */
	if (s->backup)
	{
		/* backup procedure.
		 *
		 * state 0 (start)
		 * -------
		 *
		 * a. disable log gc
		 * b. mark to start backup (state 1)
		 *
		 * state 1 (background, delayed start)
		 * -------
		 *
		 * a. create backup_path/<bsn.incomplete> directory
		 * b. create database directories
		 * c. create log directory
		 * d. state 2
		 *
		 * state 2 (background, copy)
		 * -------
		 *
		 * a. schedule and execute node backup which bsn < backup_bsn
		 * b. state 3
		 *
		 * state 3 (background, completion)
		 * -------
		 *
		 * a. rotate log file
		 * b. copy log files
		 * c. enable log gc, schedule gc
		 * d. rename <bsn.incomplete> into <bsn>
		 * e. set last backup, set COMPLETE
		 *
		*/
		if (s->backup == 1) {
			/* state 1 */
			rc = sc_backupbegin(s);
			if (ssunlikely(rc == -1)) {
				sc_backuperror(s);
				goto backup_error;
			}
			s->backup = 2;
		}
		/* state 2 */
		task->plan.plan = SI_BACKUP;
		task->plan.a = s->backup_bsn;
		rc = sc_planquota(s, &task->plan, SC_QBACKUP, zone->backup_prio);
		switch (rc) {
		case 1:
			db->workers[SC_QBACKUP]++;
			si_ref(db->index, SI_REFBE);
			task->db = db;
			return 1;
		case 0: /* state 3 */
			if (sc_end(s, db, SI_BACKUP)) {
				rc = sc_backupend(s, w);
				if (ssunlikely(rc == -1)) {
					sc_backuperror(s);
					goto backup_error;
				}
				s->backup_events++;
				task->gc = 1;
				task->on_backup = 1;
			}
			break;
		}
backup_error:;
	}

	/* expire */
	if (s->expire) {
		task->plan.plan = SI_EXPIRE;
		task->plan.a = db->index->scheme.expire;
		rc = sc_planquota(s, &task->plan, SC_QEXPIRE, zone->expire_prio);
		switch (rc) {
		case 1:
			if (zone->mode == 0)
				task->plan.plan = SI_COMPACT_INDEX;
			si_ref(db->index, SI_REFBE);
			db->workers[SC_QEXPIRE]++;
			task->db = db;
			return 1;
		case 0: /* complete */
			if (sc_end(s, db, SI_EXPIRE))
				sc_task_expire_done(s, now);
			break;
		}
	}

	/* garbage-collection */
	if (s->gc) {
		task->plan.plan = SI_GC;
		task->plan.a = vlsn;
		task->plan.b = zone->gc_wm;
		rc = sc_planquota(s, &task->plan, SC_QGC, zone->gc_prio);
		switch (rc) {
		case 1:
			if (zone->mode == 0)
				task->plan.plan = SI_COMPACT_INDEX;
			si_ref(db->index, SI_REFBE);
			db->workers[SC_QGC]++;
			task->db = db;
			return 1;
		case 0: /* complete */
			if (sc_end(s, db, SI_GC))
				sc_task_gc_done(s, now);
			break;
		}
	}

	/* lru */
	if (s->lru) {
		task->plan.plan = SI_LRU;
		rc = sc_planquota(s, &task->plan, SC_QLRU, zone->lru_prio);
		switch (rc) {
		case 1:
			if (zone->mode == 0)
				task->plan.plan = SI_COMPACT_INDEX;
			si_ref(db->index, SI_REFBE);
			db->workers[SC_QLRU]++;
			task->db = db;
			return 1;
		case 0: /* complete */
			if (sc_end(s, db, SI_LRU))
				sc_task_lru_done(s, now);
			break;
		}
	}

	/* index aging */
	if (s->age) {
		task->plan.plan = SI_AGE;
		task->plan.a = zone->branch_age * 1000000; /* ms */
		task->plan.b = zone->branch_age_wm;
		rc = sc_planquota(s, &task->plan, SC_QBRANCH, zone->branch_prio);
		switch (rc) {
		case 1:
			if (zone->mode == 0)
				task->plan.plan = SI_COMPACT_INDEX;
			si_ref(db->index, SI_REFBE);
			db->workers[SC_QBRANCH]++;
			task->db = db;
			return 1;
		case 0: /* complete */
			if (sc_end(s, db, SI_AGE))
				sc_task_age_done(s, now);
			break;
		}
	}

	/* compact_index (compaction with in-memory index) */
	if (zone->mode == 0) {
		task->plan.plan = SI_COMPACT_INDEX;
		task->plan.a = zone->branch_wm;
		rc = sc_plan(s, &task->plan);
		if (rc == 1) {
			si_ref(db->index, SI_REFBE);
			task->db = db;
			task->gc = 1;
			return 1;
		}
		goto no_job;
	}

	/* branching */
	task->plan.plan = SI_BRANCH;
	task->plan.a = zone->branch_wm;
	rc = sc_planquota(s, &task->plan, SC_QBRANCH, zone->branch_prio);
	if (rc == 1) {
		db->workers[SC_QBRANCH]++;
		si_ref(db->index, SI_REFBE);
		task->db = db;
		task->gc = 1;
		return 1;
	}

	/* compaction */
	task->plan.plan = SI_COMPACT;
	task->plan.a = zone->compact_wm;
	task->plan.b = zone->compact_mode;
	rc = sc_plan(s, &task->plan);
	if (rc == 1) {
		si_ref(db->index, SI_REFBE);
		task->db = db;
		return 1;
	}

no_job:
	si_planinit(&task->plan);
	return 0;
}

static inline void
sc_periodic_done(struct scheduler *s, uint64_t now)
{
	/* checkpoint */
	if (ssunlikely(s->checkpoint))
		sc_task_checkpoint_done(s);
	/* anti-cache */
	if (ssunlikely(s->anticache))
		sc_task_anticache_done(s, now);
	/* snapshot */
	if (ssunlikely(s->snapshot))
		sc_task_snapshot_done(s, now);
	/* expire */
	if (ssunlikely(s->expire))
		sc_task_expire_done(s, now);
	/* gc */
	if (ssunlikely(s->gc))
		sc_task_gc_done(s, now);
	/* lru */
	if (ssunlikely(s->lru))
		sc_task_lru_done(s, now);
	/* age */
	if (ssunlikely(s->age))
		sc_task_age_done(s, now);
}

static inline void
sc_periodic(struct scheduler *s, struct sctask *task, struct srzone *zone, uint64_t now)
{
	if (ssunlikely(s->count == 0))
		return;
	/* log gc and rotation */
	if (s->rotate == 0) {
		task->rotate = 1;
		s->rotate = 1;
	}
	/* checkpoint */
	switch (zone->mode) {
	case 0:  /* compact_index */
		break;
	case 1:  /* compact_index + branch_count prio */
		assert(0);
		break;
	case 2:  /* checkpoint */
	{
		if (s->checkpoint == 0)
			sc_task_checkpoint(s);
		break;
	}
	default: /* branch + compact */
		assert(zone->mode == 3);
	}
	/* anti-cache */
	if (s->anticache == 0 && zone->anticache_period) {
		if ((now - s->anticache_time) >= zone->anticache_period_us)
			sc_task_anticache(s);
	}
	/* snapshot */
	if (s->snapshot == 0 && zone->snapshot_period) {
		if ((now - s->snapshot_time) >= zone->snapshot_period_us)
			sc_task_snapshot(s);
	}
	/* expire */
	if (s->expire == 0 && zone->expire_prio && zone->expire_period) {
		if ((now - s->expire_time) >= zone->expire_period_us)
			sc_task_expire(s);
	}
	/* gc */
	if (s->gc == 0 && zone->gc_prio && zone->gc_period) {
		if ((now - s->gc_time) >= zone->gc_period_us)
			sc_task_gc(s);
	}
	/* lru */
	if (s->lru == 0 && zone->lru_prio && zone->lru_period) {
		if ((now - s->lru_time) >= zone->lru_period_us)
			sc_task_lru(s);
	}
	/* aging */
	if (s->age == 0 && zone->branch_prio && zone->branch_age_period) {
		if ((now - s->age_time) >= zone->branch_age_period_us)
			sc_task_age(s);
	}
}

static int
sc_schedule(struct scheduler *s, struct sctask *task, struct scworker *w, uint64_t vlsn)
{
	uint64_t now = clock_monotonic64();
	struct srzone *zone = sr_zoneof(s->r);
	int rc;
	tt_pthread_mutex_lock(&s->lock);
	/* start periodic tasks */
	sc_periodic(s, task, zone, now);
	/* database shutdown-drop */
	rc = sc_do_shutdown(s, task);
	if (rc) {
		tt_pthread_mutex_unlock(&s->lock);
		return rc;
	}
	/* peek a database */
	struct scdb *db = sc_peek(s);
	if (ssunlikely(db == NULL)) {
		/* complete on-going periodic tasks when there
		 * are no active databases left */
		sc_periodic_done(s, now);
		tt_pthread_mutex_unlock(&s->lock);
		return 0;
	}
	rc = sc_do(s, task, w, zone, db, vlsn, now);
	/* schedule next database */
	sc_next(s);
	tt_pthread_mutex_unlock(&s->lock);
	return rc;
}

static inline int
sc_complete(struct scheduler *s, struct sctask *t)
{
	tt_pthread_mutex_lock(&s->lock);
	struct scdb *db = t->db;
	switch (t->plan.plan) {
	case SI_BRANCH:
	case SI_AGE:
	case SI_CHECKPOINT:
		db->workers[SC_QBRANCH]--;
		break;
	case SI_COMPACT_INDEX:
		break;
	case SI_BACKUP:
	case SI_BACKUPEND:
		db->workers[SC_QBACKUP]--;
		break;
	case SI_SNAPSHOT:
		break;
	case SI_ANTICACHE:
		break;
	case SI_EXPIRE:
		db->workers[SC_QEXPIRE]--;
		break;
	case SI_GC:
		db->workers[SC_QGC]--;
		break;
	case SI_LRU:
		db->workers[SC_QLRU]--;
		break;
	}
	if (db)
		si_unref(db->index, SI_REFBE);
	if (t->rotate == 1)
		s->rotate = 0;
	tt_pthread_mutex_unlock(&s->lock);
	return 0;
}

static inline void
sc_taskinit(struct sctask *task)
{
	si_planinit(&task->plan);
	task->on_backup = 0;
	task->rotate = 0;
	task->gc = 0;
	task->db = NULL;
	task->shutdown = NULL;
}

static int
sc_step(struct scheduler *s, struct scworker *w, uint64_t vlsn)
{
	struct sctask task;
	sc_taskinit(&task);
	int rc = sc_schedule(s, &task, w, vlsn);
	int rc_job = rc;
	if (task.rotate) {
		rc = sc_rotate(s, w);
		if (ssunlikely(rc == -1))
			goto error;
	}
	/* trigger backup competion */
	if (task.on_backup)
		ss_triggerrun(s->on_event);
	if (rc_job > 0) {
		rc = sc_execute(&task, w, vlsn);
		if (ssunlikely(rc == -1)) {
			if (task.plan.plan != SI_BACKUP &&
			    task.plan.plan != SI_BACKUPEND) {
				sr_statusset(&task.db->index->status,
				             SR_MALFUNCTION);
				goto error;
			}
			tt_pthread_mutex_lock(&s->lock);
			sc_backuperror(s);
			tt_pthread_mutex_unlock(&s->lock);
		}
	}
	if (task.gc) {
		rc = sc_gc(s, w);
		if (ssunlikely(rc == -1))
			goto error;
	}
	sc_complete(s, &task);
	ss_trace(&w->trace, "%s", "sleep");
	return rc_job;
error:
	ss_trace(&w->trace, "%s", "malfunction");
	return -1;
}

static inline struct scworker*
sc_workernew(struct runtime *r, int id)
{
	struct scworker *w = ss_malloc(r->a, sizeof(struct scworker));
	if (ssunlikely(w == NULL)) {
		sr_oom_malfunction(r->e);
		return NULL;
	}
	snprintf(w->name, sizeof(w->name), "%d", id);
	sd_cinit(&w->dc);
	rlist_create(&w->link);
	rlist_create(&w->linkidle);
	ss_traceinit(&w->trace);
	ss_trace(&w->trace, "%s", "init");
	return w;
}

static inline void
sc_workerfree(struct scworker *w, struct runtime *r)
{
	sd_cfree(&w->dc, r);
	ss_tracefree(&w->trace);
	ss_free(r->a, w);
}

static int sc_workerpool_init(struct scworkerpool *p)
{
	tt_pthread_mutex_init(&p->lock, NULL);
	rlist_create(&p->list);
	rlist_create(&p->listidle);
	p->total = 0;
	p->idle = 0;
	return 0;
}

static int sc_workerpool_new(struct scworkerpool *p, struct runtime *r)
{
	struct scworker *w = sc_workernew(r, p->total);
	if (ssunlikely(w == NULL))
		return -1;
	rlist_add(&p->list, &w->link);
	rlist_add(&p->listidle, &w->linkidle);
	p->total++;
	p->idle++;
	return 0;
}

static int sc_write(struct scheduler *s, struct svlog *log, uint64_t lsn, int recover)
{
	/* write-ahead log */
	struct sltx tl;
	sl_begin(s->lp, &tl, lsn, recover);
	int rc = sl_write(&tl, log);
	if (ssunlikely(rc == -1)) {
		sl_rollback(&tl);
		return -1;
	}
	sl_commit(&tl);

	/* index */
	uint64_t now = clock_monotonic64();
	struct svlogindex *i   = (struct svlogindex*)log->index.s;
	struct svlogindex *end = (struct svlogindex*)log->index.p;
	while (i < end) {
		struct si *index = i->ptr;
		struct sitx x;
		si_begin(&x, index);
		si_write(&x, log, i, now, recover);
		si_commit(&x);
		i++;
	}
	return 0;
}

enum {
	SEUNDEF,
	SEDESTROYED,
	SE,
	SECONF,
	SECONFCURSOR,
	SECONFKV,
	SEREQ,
	SEDOCUMENT,
	SEDB,
	SEDBCURSOR,
	SETX,
	SEVIEW,
	SECURSOR
};

static struct sotype se_o[];

#define se_cast(ptr, cast, id) so_cast(ptr, cast, &se_o[id])

static inline struct so*
se_cast_validate(void *ptr)
{
	struct so *o = ptr;
	if ((char*)o->type >= (char*)&se_o[0] &&
	    (char*)o->type <= (char*)&se_o[SECURSOR])
		return ptr;
	return NULL;
}

typedef void (*serecovercbf)(char*, void*);

struct serecovercb {
	serecovercbf function;
	void *arg;
};

struct seconfrt {
	/* phia */
	char      version[16];
	char      version_storage[16];
	char      build[32];
	/* memory */
	uint64_t  memory_used;
	uint32_t  pager_pools;
	uint32_t  pager_pool_size;
	uint32_t  pager_ref_pools;
	uint32_t  pager_ref_pool_size;
	/* scheduler */
	char      zone[4];
	uint32_t  checkpoint_active;
	uint64_t  checkpoint_lsn;
	uint64_t  checkpoint_lsn_last;
	uint32_t  snapshot_active;
	uint64_t  snapshot_ssn;
	uint64_t  snapshot_ssn_last;
	uint32_t  anticache_active;
	uint64_t  anticache_asn;
	uint64_t  anticache_asn_last;
	uint32_t  backup_active;
	uint32_t  backup_last;
	uint32_t  backup_last_complete;
	uint32_t  gc_active;
	uint32_t  expire_active;
	uint32_t  lru_active;
	/* log */
	uint32_t  log_files;
	/* metric */
	struct srseq     seq;
	/* performance */
	uint32_t  tx_rw;
	uint32_t  tx_ro;
	uint32_t  tx_gc_queue;
	struct srstat    stat;
};

struct seconf {
	/* phia */
	char         *path;
	uint32_t      path_create;
	int           recover;
	int           recover_complete;
	/* backup */
	char         *backup_path;
	/* compaction */
	struct srzonemap     zones;
	/* scheduler */
	struct serecovercb   on_recover;
	struct sstrigger     on_event;
	uint32_t      event_on_backup;
	/* memory */
	uint64_t      memory_limit;
	uint64_t      anticache;
	/* log */
	uint32_t      log_enable;
	char         *log_path;
	uint32_t      log_sync;
	uint32_t      log_rotate_wm;
	uint32_t      log_rotate_sync;
	struct sfscheme      scheme;
	int           confmax;
	struct srconf       *conf;
	struct so           *env;
};

static int se_confinit(struct seconf*, struct so*);
static void se_conffree(struct seconf*);
static int se_confvalidate(struct seconf*);
static int se_confserialize(struct seconf*, struct ssbuf*);
static int se_confset_string(struct so*, const char*, void*, int);
static int se_confset_int(struct so*, const char*, int64_t);
static void *se_confget_object(struct so*, const char*);
static void *se_confget_string(struct so*, const char*, int*);
static int64_t se_confget_int(struct so*, const char*);

struct seconfkv {
	struct so    o;
	struct ssbuf key;
	struct ssbuf value;
};

struct seconfcursor {
	struct so o;
	struct ssbuf dump;
	int first;
	struct srconfdump *pos;
};

static struct so *se_confcursor_new(struct so*);

struct phia_env {
	struct so          o;
	struct srstatus    status;
	pthread_mutex_t     apilock;
	/** List of open spaces. */
	struct rlist db;
	/** List of active read views. */
	struct rlist view;
	struct srseq       seq;
	struct seconf      conf;
	struct ssquota     quota;
	struct ssvfs       vfs;
	struct ssa         a_oom;
	struct ssa         a;
	struct ssa         a_ref;
	struct sicachepool cachepool;
	struct syconf      repconf;
	struct sy          rep;
	struct slconf      lpconf;
	struct slpool      lp;
	struct sxmanager   xm;
	struct scheduler          scheduler;
	struct srerror     error;
	struct srstat      stat;
	struct sflimit     limit;
	struct ssinjection ei;
	struct runtime          r;
};

static inline void
se_apilock(struct so *o) {
	tt_pthread_mutex_lock(&((struct phia_env*)o)->apilock);
}

static inline void
se_apiunlock(struct so *o) {
	tt_pthread_mutex_unlock(&((struct phia_env*)o)->apilock);
}

static inline struct phia_env *se_of(struct so *o) {
	return (struct phia_env*)o->env;
}

struct sedocument {
	struct so        o;
	int       created;
	struct sv        v;
	enum ssorder   order;
	int       orderset;
	int       flagset;
	struct sfv       fields[8];
	int       fields_count;
	int       fields_count_keys;
	void     *prefix;
	void     *prefixcopy;
	uint32_t  prefixsize;
	void     *value;
	uint32_t  valuesize;
	/* recover */
	void     *raw;
	uint32_t  rawsize;
	uint32_t  timestamp;
	void     *log;
	/* read options */
	int       cache_only;
	int       oldest_only;
	/* stats */
	int       read_disk;
	int       read_cache;
	int       read_latency;
	/* events */
	int       event;
};

static struct so *se_document_new(struct phia_env*, struct so*, struct sv*);

static inline int
se_document_validate_ro(struct sedocument *o, struct so *dest)
{
	struct phia_env *e = se_of(&o->o);
	if (ssunlikely(o->o.parent != dest))
		return sr_error(&e->error, "%s", "incompatible document parent db");
	struct svv *v = o->v.v;
	if (! o->flagset) {
		o->flagset = 1;
		v->flags = SVGET;
	}
	return 0;
}

static inline int
se_document_validate(struct sedocument *o, struct so *dest, uint8_t flags)
{
	struct phia_env *e = se_of(&o->o);
	if (ssunlikely(o->o.parent != dest))
		return sr_error(&e->error, "%s", "incompatible document parent db");
	struct svv *v = o->v.v;
	if (o->flagset) {
		if (ssunlikely(v->flags != flags))
			return sr_error(&e->error, "%s", "incompatible document flags");
	} else {
		o->flagset = 1;
		v->flags = flags;
	}
	if (v->lsn != 0) {
		uint64_t lsn = sr_seq(&e->seq, SR_LSN);
		if (v->lsn <= lsn)
			return sr_error(&e->error, "%s", "incompatible document lsn");
	}
	return 0;
}

struct sedb {
	struct so         o;
	uint32_t   created;
	struct siprofiler rtp;
	struct sischeme  *scheme;
	struct si        *index;
	struct runtime *r;
	struct sxindex    coindex;
	uint64_t   txn_min;
	uint64_t   txn_max;
	/** Member of env->db list. */
	struct rlist link;
};

static inline int
se_dbactive(struct sedb *o) {
	return si_active(o->index);
}

static struct so *se_dbnew(struct phia_env*, char*, int);
static struct so *se_dbmatch(struct phia_env*, char*);
static struct so *se_dbmatch_id(struct phia_env*, uint32_t);
static struct so *se_dbresult(struct phia_env*, struct scread*);
static void *
se_dbread(struct sedb*, struct sedocument*, struct sx*, int, struct sicache*);
static int se_dbvisible(struct sedb*, uint64_t);
static void se_dbbind(struct phia_env*);
static void se_dbunbind(struct phia_env*, uint64_t);

struct setx {
	struct so o;
	int64_t lsn;
	int half_commit;
	uint64_t start;
	struct svlog log;
	struct sx t;
};

static struct so *se_txnew(struct phia_env*);

struct seviewdb {
	struct so        o;
	uint64_t  txn_id;
	int       ready;
	struct ssbuf     list;
	char     *pos;
	struct sedb     *v;
} sspacked;

static struct so *se_viewdb_new(struct phia_env*, uint64_t);

struct seview {
	struct so        o;
	uint64_t  vlsn;
	struct ssbuf     name;
	struct sx        t;
	struct svlog     log;
	int       db_view_only;
	/* Member of env->view list. */
	struct rlist link;
} sspacked;

static struct so *se_viewnew(struct phia_env*, uint64_t, char*, int);
static int se_viewupdate(struct seview*);

struct secursor {
	struct so o;
	struct svlog log;
	struct sx t;
	uint64_t start;
	int ops;
	int read_disk;
	int read_cache;
	int read_commited;
	struct sicache *cache;
};

static struct so *se_cursornew(struct phia_env*, uint64_t);

enum {
	SE_RECOVER_1P = 1,
	SE_RECOVER_2P = 2,
	SE_RECOVER_NP = 3
};

static int se_recoverbegin(struct sedb*);
static int se_recoverend(struct sedb*);
static int se_recover(struct phia_env*);
static int se_recover_repository(struct phia_env*);

static int
se_open(struct so *o)
{
	struct phia_env *e = se_cast(o, struct phia_env*, SE);
	/* recover phases */
	int status = sr_status(&e->status);
	switch (e->conf.recover) {
	case SE_RECOVER_1P: break;
	case SE_RECOVER_2P:
		if (status == SR_RECOVER)
			goto online;
		break;
	case SE_RECOVER_NP:
		if (status == SR_RECOVER) {
			sr_statusset(&e->status, SR_ONLINE);
			return 0;
		}
		if (status == SR_ONLINE) {
			sr_statusset(&e->status, SR_RECOVER);
			return 0;
		}
		break;
	}
	if (status != SR_OFFLINE)
		return -1;

	/* validate configuration */
	int rc;
	rc = se_confvalidate(&e->conf);
	if (ssunlikely(rc == -1))
		return -1;
	sr_statusset(&e->status, SR_RECOVER);

	/* set memory quota (disable during recovery) */
	ss_quotaset(&e->quota, e->conf.memory_limit);
	ss_quotaenable(&e->quota, 0);

	/* repository recover */
	rc = se_recover_repository(e);
	if (ssunlikely(rc == -1))
		return -1;
	/* databases recover */
	struct sedb *item, *n;
	rlist_foreach_entry_safe(item, &e->db, link, n) {
		rc = so_open(&item->o);
		if (ssunlikely(rc == -1))
			return -1;
	}
	/* recover logpool */
	rc = se_recover(e);
	if (ssunlikely(rc == -1))
		return -1;
	if (e->conf.recover == SE_RECOVER_2P)
		return 0;

online:
	/* complete */
	rlist_foreach_entry_safe(item, &e->db, link, n) {
		rc = so_open(&item->o);
		if (ssunlikely(rc == -1))
			return -1;
	}
	/* enable quota */
	ss_quotaenable(&e->quota, 1);
	sr_statusset(&e->status, SR_ONLINE);

	/* run thread-pool and scheduler */
	sc_set(&e->scheduler, e->conf.anticache,
	        e->conf.backup_path);
	return 0;
}

static int
se_destroy(struct so *o)
{
	struct phia_env *e = se_cast(o, struct phia_env*, SE);
	int rcret = 0;
	int rc;
	sr_statusset(&e->status, SR_SHUTDOWN);
	{
		struct sedb *db, *next;
		rlist_foreach_entry_safe(db, &e->db, link, next) {
			rc = so_destroy(&db->o);
			if (ssunlikely(rc == -1))
				rcret = -1;
		}
	}
	rc = sl_poolshutdown(&e->lp);
	if (ssunlikely(rc == -1))
		rcret = -1;
	rc = sy_close(&e->rep, &e->r);
	if (ssunlikely(rc == -1))
		rcret = -1;
	sx_managerfree(&e->xm);
	ss_vfsfree(&e->vfs);
	si_cachepool_free(&e->cachepool);
	se_conffree(&e->conf);
	ss_quotafree(&e->quota);
	tt_pthread_mutex_destroy(&e->apilock);
	sf_limitfree(&e->limit, &e->a);
	sr_statfree(&e->stat);
	sr_seqfree(&e->seq);
	sr_statusfree(&e->status);
	free(e);
	return rcret;
}

static int
se_close(struct so *o)
{
	return se_destroy(o);
}

static void*
se_begin(struct so *o)
{
	struct phia_env *e = se_of(o);
	return se_txnew(e);
}

static void*
se_cursor(struct so *o)
{
	struct phia_env *e = se_cast(o, struct phia_env*, SE);
	return se_cursornew(e, UINT64_MAX);
}

static struct soif seif =
{
	.open         = se_open,
	.close        = se_close,
	.destroy      = se_destroy,
	.free         = NULL,
	.document     = NULL,
	.drop         = NULL,
	.setstring    = se_confset_string,
	.setint       = se_confset_int,
	.setobject    = NULL,
	.getobject    = se_confget_object,
	.getstring    = se_confget_string,
	.getint       = se_confget_int,
	.set          = NULL,
	.upsert       = NULL,
	.del          = NULL,
	.get          = NULL,
	.begin        = se_begin,
	.commit       = NULL,
	.cursor       = se_cursor,
};

static inline int
se_confv(struct srconf *c, struct srconfstmt *s)
{
	switch (s->op) {
	case SR_SERIALIZE: return sr_conf_serialize(c, s);
	case SR_READ:      return sr_conf_read(c, s);
	case SR_WRITE:     return sr_conf_write(c, s);
	}
	assert(0);
	return -1;
}

static inline int
se_confv_offline(struct srconf *c, struct srconfstmt *s)
{
	struct phia_env *e = s->ptr;
	if (s->op == SR_WRITE) {
		if (sr_status(&e->status)) {
			sr_error(s->r->e, "write to %s is offline-only", s->path);
			return -1;
		}
	}
	return se_confv(c, s);
}

static inline int
se_confphia_error(struct srconf *c, struct srconfstmt *s)
{
	struct phia_env *e = s->ptr;
	char *errorp;
	char  error[128];
	error[0] = 0;
	int len = sr_errorcopy(&e->error, error, sizeof(error));
	if (sslikely(len == 0))
		errorp = NULL;
	else
		errorp = error;
	struct srconf conf = {
		.key      = c->key,
		.flags    = c->flags,
		.type     = c->type,
		.function = NULL,
		.value    = errorp,
		.ptr      = NULL,
		.next     = NULL
	};
	return se_confv(&conf, s);
}

static inline struct srconf*
se_confphia(struct phia_env *e, struct seconfrt *rt, struct srconf **pc)
{
	struct srconf *phia = *pc;
	struct srconf *p = NULL;
	sr_C(&p, pc, se_confv, "version", SS_STRING, rt->version, SR_RO, NULL);
	sr_C(&p, pc, se_confv, "version_storage", SS_STRING, rt->version_storage, SR_RO, NULL);
	sr_C(&p, pc, se_confv, "build", SS_STRING, rt->build, SR_RO, NULL);
	sr_C(&p, pc, se_confphia_error, "error", SS_STRING, NULL, SR_RO, NULL);
	sr_c(&p, pc, se_confv_offline, "path", SS_STRINGPTR, &e->conf.path);
	sr_c(&p, pc, se_confv_offline, "path_create", SS_U32, &e->conf.path_create);
	sr_c(&p, pc, se_confv_offline, "recover", SS_U32, &e->conf.recover);
	return sr_C(NULL, pc, NULL, "phia", SS_UNDEF, phia, SR_NS, NULL);
}

static inline struct srconf*
se_confmemory(struct phia_env *e, struct seconfrt *rt, struct srconf **pc)
{
	struct srconf *memory = *pc;
	struct srconf *p = NULL;
	sr_c(&p, pc, se_confv_offline, "limit", SS_U64, &e->conf.memory_limit);
	sr_C(&p, pc, se_confv, "used", SS_U64, &rt->memory_used, SR_RO, NULL);
	sr_c(&p, pc, se_confv_offline, "anticache", SS_U64, &e->conf.anticache);
	return sr_C(NULL, pc, NULL, "memory", SS_UNDEF, memory, SR_NS, NULL);
}

static inline int
se_confcompaction_set(struct srconf *c ssunused, struct srconfstmt *s)
{
	struct phia_env *e = s->ptr;
	if (s->op != SR_WRITE) {
		sr_error(&e->error, "%s", "bad operation");
		return -1;
	}
	if (ssunlikely(sr_statusactive(&e->status))) {
		sr_error(s->r->e, "write to %s is offline-only", s->path);
		return -1;
	}
	/* validate argument */
	uint32_t percent = load_u32(s->value);
	if (percent > 100) {
		sr_error(&e->error, "%s", "bad argument");
		return -1;
	}
	struct srzone z;
	memset(&z, 0, sizeof(z));
	z.enable = 1;
	sr_zonemap_set(&e->conf.zones, percent, &z);
	return 0;
}

static inline struct srconf*
se_confcompaction(struct phia_env *e, struct seconfrt *rt ssunused, struct srconf **pc)
{
	struct srconf *compaction = NULL;
	struct srconf *prev = NULL;
	struct srconf *p;
	int i = 0;
	for (; i < 11; i++) {
		struct srzone *z = &e->conf.zones.zones[i];
		if (! z->enable)
			continue;
		struct srconf *zone = *pc;
		p = NULL;
		sr_c(&p, pc, se_confv_offline, "mode", SS_U32, &z->mode);
		sr_c(&p, pc, se_confv_offline, "compact_wm", SS_U32, &z->compact_wm);
		sr_c(&p, pc, se_confv_offline, "compact_mode", SS_U32, &z->compact_mode);
		sr_c(&p, pc, se_confv_offline, "branch_prio", SS_U32, &z->branch_prio);
		sr_c(&p, pc, se_confv_offline, "branch_wm", SS_U32, &z->branch_wm);
		sr_c(&p, pc, se_confv_offline, "branch_age", SS_U32, &z->branch_age);
		sr_c(&p, pc, se_confv_offline, "branch_age_period", SS_U32, &z->branch_age_period);
		sr_c(&p, pc, se_confv_offline, "branch_age_wm", SS_U32, &z->branch_age_wm);
		sr_c(&p, pc, se_confv_offline, "anticache_period", SS_U32, &z->anticache_period);
		sr_c(&p, pc, se_confv_offline, "snapshot_period", SS_U32, &z->snapshot_period);
		sr_c(&p, pc, se_confv_offline, "expire_prio", SS_U32, &z->expire_prio);
		sr_c(&p, pc, se_confv_offline, "expire_period", SS_U32, &z->expire_period);
		sr_c(&p, pc, se_confv_offline, "gc_wm", SS_U32, &z->gc_wm);
		sr_c(&p, pc, se_confv_offline, "gc_prio", SS_U32, &z->gc_prio);
		sr_c(&p, pc, se_confv_offline, "gc_period", SS_U32, &z->gc_period);
		sr_c(&p, pc, se_confv_offline, "lru_prio", SS_U32, &z->lru_prio);
		sr_c(&p, pc, se_confv_offline, "lru_period", SS_U32, &z->lru_period);
		sr_c(&p, pc, se_confv_offline, "backup_prio", SS_U32, &z->backup_prio);
		prev = sr_C(&prev, pc, NULL, z->name, SS_UNDEF, zone, SR_NS, NULL);
		if (compaction == NULL)
			compaction = prev;
	}
	return sr_C(NULL, pc, se_confcompaction_set, "compaction", SS_U32,
	            compaction, SR_NS, NULL);
}

static inline int
se_confscheduler_trace(struct srconf *c, struct srconfstmt *s)
{
	struct scworker *w = c->value;
	char tracesz[128];
	char *trace;
	int tracelen = ss_tracecopy(&w->trace, tracesz, sizeof(tracesz));
	if (sslikely(tracelen == 0))
		trace = NULL;
	else
		trace = tracesz;
	struct srconf conf = {
		.key      = c->key,
		.flags    = c->flags,
		.type     = c->type,
		.function = NULL,
		.value    = trace,
		.ptr      = NULL,
		.next     = NULL
	};
	return se_confv(&conf, s);
}

static inline int
se_confscheduler_checkpoint(struct srconf *c, struct srconfstmt *s)
{
	if (s->op != SR_WRITE)
		return se_confv(c, s);
	struct phia_env *e = s->ptr;
	return sc_ctl_checkpoint(&e->scheduler);
}

static inline int
se_confscheduler_snapshot(struct srconf *c, struct srconfstmt *s)
{
	if (s->op != SR_WRITE)
		return se_confv(c, s);
	struct phia_env *e = s->ptr;
	return sc_ctl_snapshot(&e->scheduler);
}

static inline int
se_confscheduler_anticache(struct srconf *c, struct srconfstmt *s)
{
	if (s->op != SR_WRITE)
		return se_confv(c, s);
	struct phia_env *e = s->ptr;
	return sc_ctl_anticache(&e->scheduler);
}

static inline int
se_confscheduler_on_recover(struct srconf *c, struct srconfstmt *s)
{
	struct phia_env *e = s->ptr;
	if (s->op != SR_WRITE)
		return se_confv(c, s);
	if (ssunlikely(sr_statusactive(&e->status))) {
		sr_error(s->r->e, "write to %s is offline-only", s->path);
		return -1;
	}
	e->conf.on_recover.function =
		(serecovercbf)(uintptr_t)s->value;
	return 0;
}

static inline int
se_confscheduler_on_recover_arg(struct srconf *c, struct srconfstmt *s)
{
	struct phia_env *e = s->ptr;
	if (s->op != SR_WRITE)
		return se_confv(c, s);
	if (ssunlikely(sr_statusactive(&e->status))) {
		sr_error(s->r->e, "write to %s is offline-only", s->path);
		return -1;
	}
	e->conf.on_recover.arg = s->value;
	return 0;
}

static inline int
se_confscheduler_on_event(struct srconf *c, struct srconfstmt *s)
{
	struct phia_env *e = s->ptr;
	if (s->op != SR_WRITE)
		return se_confv(c, s);
	if (ssunlikely(sr_statusactive(&e->status))) {
		sr_error(s->r->e, "write to %s is offline-only", s->path);
		return -1;
	}
	ss_triggerset(&e->conf.on_event, s->value);
	return 0;
}

static inline int
se_confscheduler_on_event_arg(struct srconf *c, struct srconfstmt *s)
{
	struct phia_env *e = s->ptr;
	if (s->op != SR_WRITE)
		return se_confv(c, s);
	if (ssunlikely(sr_statusactive(&e->status))) {
		sr_error(s->r->e, "write to %s is offline-only", s->path);
		return -1;
	}
	ss_triggerset_arg(&e->conf.on_event, s->value);
	return 0;
}

static inline int
se_confscheduler_gc(struct srconf *c, struct srconfstmt *s)
{
	if (s->op != SR_WRITE)
		return se_confv(c, s);
	struct phia_env *e = s->ptr;
	return sc_ctl_gc(&e->scheduler);
}

static inline int
se_confscheduler_expire(struct srconf *c, struct srconfstmt *s)
{
	if (s->op != SR_WRITE)
		return se_confv(c, s);
	struct phia_env *e = s->ptr;
	return sc_ctl_expire(&e->scheduler);
}

static inline int
se_confscheduler_lru(struct srconf *c, struct srconfstmt *s)
{
	if (s->op != SR_WRITE)
		return se_confv(c, s);
	struct phia_env *e = s->ptr;
	return sc_ctl_lru(&e->scheduler);
}

static inline int
se_confscheduler_run(struct srconf *c, struct srconfstmt *s)
{
	if (s->op != SR_WRITE)
		return se_confv(c, s);
	struct phia_env *e = s->ptr;
	uint64_t vlsn = sx_vlsn(&e->xm);
	return sc_ctl_call(&e->scheduler, vlsn);
}

static inline struct srconf*
se_confscheduler(struct phia_env *e, struct seconfrt *rt, struct srconf **pc)
{
	struct srconf *scheduler = *pc;
	struct srconf *prev;
	struct srconf *p = NULL;
	sr_C(&p, pc, se_confv, "zone", SS_STRING, rt->zone, SR_RO, NULL);
	sr_C(&p, pc, se_confv, "checkpoint_active", SS_U32, &rt->checkpoint_active, SR_RO, NULL);
	sr_C(&p, pc, se_confv, "checkpoint_lsn", SS_U64, &rt->checkpoint_lsn, SR_RO, NULL);
	sr_C(&p, pc, se_confv, "checkpoint_lsn_last", SS_U64, &rt->checkpoint_lsn_last, SR_RO, NULL);
	sr_c(&p, pc, se_confscheduler_checkpoint, "checkpoint",  SS_FUNCTION, NULL);
	sr_C(&p, pc, se_confv, "anticache_active", SS_U32, &rt->anticache_active, SR_RO, NULL);
	sr_C(&p, pc, se_confv, "anticache_asn", SS_U64, &rt->anticache_asn, SR_RO, NULL);
	sr_C(&p, pc, se_confv, "anticache_asn_last", SS_U64, &rt->anticache_asn_last, SR_RO, NULL);
	sr_c(&p, pc, se_confscheduler_anticache, "anticache", SS_FUNCTION, NULL);
	sr_C(&p, pc, se_confv, "snapshot_active", SS_U32, &rt->snapshot_active, SR_RO, NULL);
	sr_C(&p, pc, se_confv, "snapshot_ssn", SS_U64, &rt->snapshot_ssn, SR_RO, NULL);
	sr_C(&p, pc, se_confv, "snapshot_ssn_last", SS_U64, &rt->snapshot_ssn_last, SR_RO, NULL);
	sr_c(&p, pc, se_confscheduler_snapshot, "snapshot", SS_FUNCTION, NULL);
	sr_c(&p, pc, se_confscheduler_on_recover, "on_recover", SS_STRING, NULL);
	sr_c(&p, pc, se_confscheduler_on_recover_arg, "on_recover_arg", SS_STRING, NULL);
	sr_c(&p, pc, se_confscheduler_on_event, "on_event", SS_STRING, NULL);
	sr_c(&p, pc, se_confscheduler_on_event_arg, "on_event_arg", SS_STRING, NULL);
	sr_c(&p, pc, se_confv_offline, "event_on_backup", SS_U32, &e->conf.event_on_backup);
	sr_C(&p, pc, se_confv, "gc_active", SS_U32, &rt->gc_active, SR_RO, NULL);
	sr_c(&p, pc, se_confscheduler_gc, "gc", SS_FUNCTION, NULL);
	sr_C(&p, pc, se_confv, "expire_active", SS_U32, &rt->expire_active, SR_RO, NULL);
	sr_c(&p, pc, se_confscheduler_expire, "expire", SS_FUNCTION, NULL);
	sr_C(&p, pc, se_confv, "lru_active", SS_U32, &rt->lru_active, SR_RO, NULL);
	sr_c(&p, pc, se_confscheduler_lru, "lru", SS_FUNCTION, NULL);
	sr_c(&p, pc, se_confscheduler_run, "run", SS_FUNCTION, NULL);
	prev = p;
	struct scworker *w;
	rlist_foreach_entry(w, &e->scheduler.wp.list, link) {
		struct srconf *worker = *pc;
		p = NULL;
		sr_C(&p, pc, se_confscheduler_trace, "trace", SS_STRING, w, SR_RO, NULL);
		sr_C(&prev, pc, NULL, w->name, SS_UNDEF, worker, SR_NS, NULL);
	}
	return sr_C(NULL, pc, NULL, "scheduler", SS_UNDEF, scheduler, SR_NS, NULL);
}

static inline int
se_conflog_rotate(struct srconf *c, struct srconfstmt *s)
{
	if (s->op != SR_WRITE)
		return se_confv(c, s);
	struct phia_env *e = s->ptr;
	return sl_poolrotate(&e->lp);
}

static inline int
se_conflog_gc(struct srconf *c, struct srconfstmt *s)
{
	if (s->op != SR_WRITE)
		return se_confv(c, s);
	struct phia_env *e = s->ptr;
	return sl_poolgc(&e->lp);
}

static inline struct srconf*
se_conflog(struct phia_env *e, struct seconfrt *rt, struct srconf **pc)
{
	struct srconf *log = *pc;
	struct srconf *p = NULL;
	sr_c(&p, pc, se_confv_offline, "enable", SS_U32, &e->conf.log_enable);
	sr_c(&p, pc, se_confv_offline, "path", SS_STRINGPTR, &e->conf.log_path);
	sr_c(&p, pc, se_confv_offline, "sync", SS_U32, &e->conf.log_sync);
	sr_c(&p, pc, se_confv_offline, "rotate_wm", SS_U32, &e->conf.log_rotate_wm);
	sr_c(&p, pc, se_confv_offline, "rotate_sync", SS_U32, &e->conf.log_rotate_sync);
	sr_c(&p, pc, se_conflog_rotate, "rotate", SS_FUNCTION, NULL);
	sr_c(&p, pc, se_conflog_gc, "gc", SS_FUNCTION, NULL);
	sr_C(&p, pc, se_confv, "files", SS_U32, &rt->log_files, SR_RO, NULL);
	return sr_C(NULL, pc, NULL, "log", SS_UNDEF, log, SR_NS, NULL);
}

static inline struct srconf*
se_confperformance(struct phia_env *e ssunused, struct seconfrt *rt, struct srconf **pc)
{
	struct srconf *perf = *pc;
	struct srconf *p = NULL;
	sr_C(&p, pc, se_confv, "documents", SS_U64, &rt->stat.v_count, SR_RO, NULL);
	sr_C(&p, pc, se_confv, "documents_used", SS_U64, &rt->stat.v_allocated, SR_RO, NULL);
	sr_C(&p, pc, se_confv, "key", SS_STRING, rt->stat.key.sz, SR_RO, NULL);
	sr_C(&p, pc, se_confv, "value", SS_STRING, rt->stat.value.sz, SR_RO, NULL);
	sr_C(&p, pc, se_confv, "set", SS_U64, &rt->stat.set, SR_RO, NULL);
	sr_C(&p, pc, se_confv, "set_latency", SS_STRING, rt->stat.set_latency.sz, SR_RO, NULL);
	sr_C(&p, pc, se_confv, "delete", SS_U64, &rt->stat.del, SR_RO, NULL);
	sr_C(&p, pc, se_confv, "delete_latency", SS_STRING, rt->stat.del_latency.sz, SR_RO, NULL);
	sr_C(&p, pc, se_confv, "upsert", SS_U64, &rt->stat.upsert, SR_RO, NULL);
	sr_C(&p, pc, se_confv, "upsert_latency", SS_STRING, rt->stat.upsert_latency.sz, SR_RO, NULL);
	sr_C(&p, pc, se_confv, "get", SS_U64, &rt->stat.get, SR_RO, NULL);
	sr_C(&p, pc, se_confv, "get_latency", SS_STRING, rt->stat.get_latency.sz, SR_RO, NULL);
	sr_C(&p, pc, se_confv, "get_read_disk", SS_STRING, rt->stat.get_read_disk.sz, SR_RO, NULL);
	sr_C(&p, pc, se_confv, "get_read_cache", SS_STRING, rt->stat.get_read_cache.sz, SR_RO, NULL);
	sr_C(&p, pc, se_confv, "tx_active_rw", SS_U32, &rt->tx_rw, SR_RO, NULL);
	sr_C(&p, pc, se_confv, "tx_active_ro", SS_U32, &rt->tx_ro, SR_RO, NULL);
	sr_C(&p, pc, se_confv, "tx", SS_U64, &rt->stat.tx, SR_RO, NULL);
	sr_C(&p, pc, se_confv, "tx_rollback", SS_U64, &rt->stat.tx_rlb, SR_RO, NULL);
	sr_C(&p, pc, se_confv, "tx_conflict", SS_U64, &rt->stat.tx_conflict, SR_RO, NULL);
	sr_C(&p, pc, se_confv, "tx_lock", SS_U64, &rt->stat.tx_lock, SR_RO, NULL);
	sr_C(&p, pc, se_confv, "tx_latency", SS_STRING, rt->stat.tx_latency.sz, SR_RO, NULL);
	sr_C(&p, pc, se_confv, "tx_ops", SS_STRING, rt->stat.tx_stmts.sz, SR_RO, NULL);
	sr_C(&p, pc, se_confv, "tx_gc_queue", SS_U32, &rt->tx_gc_queue, SR_RO, NULL);
	sr_C(&p, pc, se_confv, "cursor", SS_U64, &rt->stat.cursor, SR_RO, NULL);
	sr_C(&p, pc, se_confv, "cursor_latency", SS_STRING, rt->stat.cursor_latency.sz, SR_RO, NULL);
	sr_C(&p, pc, se_confv, "cursor_read_disk", SS_STRING, rt->stat.cursor_read_disk.sz, SR_RO, NULL);
	sr_C(&p, pc, se_confv, "cursor_read_cache", SS_STRING, rt->stat.cursor_read_cache.sz, SR_RO, NULL);
	sr_C(&p, pc, se_confv, "cursor_ops", SS_STRING, rt->stat.cursor_ops.sz, SR_RO, NULL);
	return sr_C(NULL, pc, NULL, "performance", SS_UNDEF, perf, SR_NS, NULL);
}

static inline struct srconf*
se_confmetric(struct phia_env *e ssunused, struct seconfrt *rt, struct srconf **pc)
{
	struct srconf *metric = *pc;
	struct srconf *p = NULL;
	sr_C(&p, pc, se_confv, "lsn",  SS_U64, &rt->seq.lsn, SR_RO, NULL);
	sr_C(&p, pc, se_confv, "tsn",  SS_U64, &rt->seq.tsn, SR_RO, NULL);
	sr_C(&p, pc, se_confv, "nsn",  SS_U64, &rt->seq.nsn, SR_RO, NULL);
	sr_C(&p, pc, se_confv, "ssn",  SS_U64, &rt->seq.ssn, SR_RO, NULL);
	sr_C(&p, pc, se_confv, "asn",  SS_U64, &rt->seq.asn, SR_RO, NULL);
	sr_C(&p, pc, se_confv, "dsn",  SS_U32, &rt->seq.dsn, SR_RO, NULL);
	sr_C(&p, pc, se_confv, "bsn",  SS_U32, &rt->seq.bsn, SR_RO, NULL);
	sr_C(&p, pc, se_confv, "lfsn", SS_U64, &rt->seq.lfsn, SR_RO, NULL);
	return sr_C(NULL, pc, NULL, "metric", SS_UNDEF, metric, SR_NS, NULL);
}

static inline int
se_confdb_set(struct srconf *c ssunused, struct srconfstmt *s)
{
	/* set(db) */
	struct phia_env *e = s->ptr;
	if (s->op == SR_WRITE) {
		char *name = s->value;
		struct sedb *db = (struct sedb*)se_dbmatch(e, name);
		if (ssunlikely(db)) {
			sr_error(&e->error, "database '%s' already exists", name);
			return -1;
		}
		db = (struct sedb*)se_dbnew(e, name, s->valuesize);
		if (ssunlikely(db == NULL))
			return -1;
		rlist_add(&e->db, &db->link);
		return 0;
	}

	/* get() */
	if (s->op == SR_READ) {
		uint64_t txn = sr_seq(&e->seq, SR_TSN);
		struct so *c = se_viewdb_new(e, txn);
		if (ssunlikely(c == NULL))
			return -1;
		*(void**)s->value = c;
		return 0;
	}

	sr_error(&e->error, "%s", "bad operation");
	return -1;
}

static inline int
se_confdb_get(struct srconf *c, struct srconfstmt *s)
{
	/* get(db.name) */
	struct phia_env *e = s->ptr;
	if (s->op != SR_READ) {
		sr_error(&e->error, "%s", "bad operation");
		return -1;
	}
	assert(c->ptr != NULL);
	struct sedb *db = c->ptr;
	int status = sr_status(&db->index->status);
	if (status == SR_SHUTDOWN_PENDING ||
	    status == SR_DROP_PENDING) {
		sr_error(&e->error, "%s", "database has been scheduled for shutdown/drop");
		return -1;
	}
	si_ref(db->index, SI_REFFE);
	*(void**)s->value = db;
	return 0;
}

static inline int
se_confdb_upsert(struct srconf *c, struct srconfstmt *s)
{
	if (s->op != SR_WRITE)
		return se_confv(c, s);
	struct sedb *db = c->ptr;
	if (ssunlikely(se_dbactive(db))) {
		sr_error(s->r->e, "write to %s is offline-only", s->path);
		return -1;
	}
	/* set upsert function */
	sfupsertf upsert = (sfupsertf)(uintptr_t)s->value;
	sf_upsertset(&db->scheme->fmt_upsert, upsert);
	return 0;
}

static inline int
se_confdb_upsertarg(struct srconf *c, struct srconfstmt *s)
{
	if (s->op != SR_WRITE)
		return se_confv(c, s);
	struct sedb *db = c->ptr;
	if (ssunlikely(se_dbactive(db))) {
		sr_error(s->r->e, "write to %s is offline-only", s->path);
		return -1;
	}
	sf_upsertset_arg(&db->scheme->fmt_upsert, s->value);
	return 0;
}

static inline int
se_confdb_status(struct srconf *c, struct srconfstmt *s)
{
	struct sedb *db = c->value;
	char *status = sr_statusof(&db->index->status);
	struct srconf conf = {
		.key      = c->key,
		.flags    = c->flags,
		.type     = c->type,
		.function = NULL,
		.value    = status,
		.ptr      = NULL,
		.next     = NULL
	};
	return se_confv(&conf, s);
}

static inline int
se_confdb_branch(struct srconf *c, struct srconfstmt *s)
{
	if (s->op != SR_WRITE)
		return se_confv(c, s);
	struct sedb *db = c->value;
	struct phia_env *e = se_of(&db->o);
	uint64_t vlsn = sx_vlsn(&e->xm);
	return sc_ctl_branch(&e->scheduler, vlsn, db->index);
}

static inline int
se_confdb_compact(struct srconf *c, struct srconfstmt *s)
{
	if (s->op != SR_WRITE)
		return se_confv(c, s);
	struct sedb *db = c->value;
	struct phia_env *e = se_of(&db->o);
	uint64_t vlsn = sx_vlsn(&e->xm);
	return sc_ctl_compact(&e->scheduler, vlsn, db->index);
}

static inline int
se_confdb_compact_index(struct srconf *c, struct srconfstmt *s)
{
	if (s->op != SR_WRITE)
		return se_confv(c, s);
	struct sedb *db = c->value;
	struct phia_env *e = se_of(&db->o);
	uint64_t vlsn = sx_vlsn(&e->xm);
	return sc_ctl_compact_index(&e->scheduler, vlsn, db->index);
}

static inline int
se_confv_dboffline(struct srconf *c, struct srconfstmt *s)
{
	struct sedb *db = c->ptr;
	if (s->op == SR_WRITE) {
		if (se_dbactive(db)) {
			sr_error(s->r->e, "write to %s is offline-only", s->path);
			return -1;
		}
	}
	return se_confv(c, s);
}

static inline int
se_confdb_scheme(struct srconf *c ssunused, struct srconfstmt *s)
{
	/* set(scheme, field) */
	struct sedb *db = c->ptr;
	struct phia_env *e = se_of(&db->o);
	if (s->op != SR_WRITE) {
		sr_error(&e->error, "%s", "bad operation");
		return -1;
	}
	if (ssunlikely(se_dbactive(db))) {
		sr_error(s->r->e, "write to %s is offline-only", s->path);
		return -1;
	}
	if (ssunlikely(db->scheme->scheme.fields_count == 8)) {
		sr_error(s->r->e, "%s", "fields number limit reached");
		return -1;
	}
	char *name = s->value;
	struct sffield *field = sf_schemefind(&db->scheme->scheme, name);
	if (ssunlikely(field)) {
		sr_error(&e->error, "field '%s' is already set", name);
		return -1;
	}
	/* create new field */
	field = sf_fieldnew(&e->a, name);
	if (ssunlikely(field == NULL))
		return sr_oom(&e->error);
	int rc;
	rc = sf_fieldoptions(field, &e->a, "string");
	if (ssunlikely(rc == -1)) {
		sf_fieldfree(field, &e->a);
		return sr_oom(&e->error);
	}
	rc = sf_schemeadd(&db->scheme->scheme, &e->a, field);
	if (ssunlikely(rc == -1)) {
		sf_fieldfree(field, &e->a);
		return sr_oom(&e->error);
	}
	return 0;
}

static inline int
se_confdb_field(struct srconf *c, struct srconfstmt *s)
{
	struct sedb *db = c->ptr;
	struct phia_env *e = se_of(&db->o);
	if (s->op != SR_WRITE)
		return se_confv(c, s);
	if (ssunlikely(se_dbactive(db))) {
		sr_error(s->r->e, "write to %s is offline-only", s->path);
		return -1;
	}
	char *path = s->value;
	/* update key-part path */
	struct sffield *field = sf_schemefind(&db->scheme->scheme, c->key);
	assert(field != NULL);
	return sf_fieldoptions(field, &e->a, path);
}

static inline struct srconf*
se_confdb(struct phia_env *e, struct seconfrt *rt ssunused, struct srconf **pc)
{
	struct srconf *db = NULL;
	struct srconf *prev = NULL;
	struct srconf *p;
	struct sedb *o;
	rlist_foreach_entry(o, &e->db, link)
	{
		si_profilerbegin(&o->rtp, o->index);
		si_profiler(&o->rtp);
		si_profilerend(&o->rtp);
		/* database index */
		struct srconf *index = *pc;
		p = NULL;
		sr_C(&p, pc, se_confv, "memory_used", SS_U64, &o->rtp.memory_used, SR_RO, NULL);
		sr_C(&p, pc, se_confv, "size", SS_U64, &o->rtp.total_node_size, SR_RO, NULL);
		sr_C(&p, pc, se_confv, "size_uncompressed", SS_U64, &o->rtp.total_node_origin_size, SR_RO, NULL);
		sr_C(&p, pc, se_confv, "size_snapshot", SS_U64, &o->rtp.total_snapshot_size, SR_RO, NULL);
		sr_C(&p, pc, se_confv, "size_amqf", SS_U64, &o->rtp.total_amqf_size, SR_RO, NULL);
		sr_C(&p, pc, se_confv, "count", SS_U64, &o->rtp.count, SR_RO, NULL);
		sr_C(&p, pc, se_confv, "count_dup", SS_U64, &o->rtp.count_dup, SR_RO, NULL);
		sr_C(&p, pc, se_confv, "read_disk", SS_U64, &o->rtp.read_disk, SR_RO, NULL);
		sr_C(&p, pc, se_confv, "read_cache", SS_U64, &o->rtp.read_cache, SR_RO, NULL);
		sr_C(&p, pc, se_confv, "temperature_avg", SS_U32, &o->rtp.temperature_avg, SR_RO, NULL);
		sr_C(&p, pc, se_confv, "temperature_min", SS_U32, &o->rtp.temperature_min, SR_RO, NULL);
		sr_C(&p, pc, se_confv, "temperature_max", SS_U32, &o->rtp.temperature_max, SR_RO, NULL);
		sr_C(&p, pc, se_confv, "temperature_histogram", SS_STRINGPTR, &o->rtp.histogram_temperature_ptr, SR_RO, NULL);
		sr_C(&p, pc, se_confv, "node_count", SS_U32, &o->rtp.total_node_count, SR_RO, NULL);
		sr_C(&p, pc, se_confv, "branch_count", SS_U32, &o->rtp.total_branch_count, SR_RO, NULL);
		sr_C(&p, pc, se_confv, "branch_avg", SS_U32, &o->rtp.total_branch_avg, SR_RO, NULL);
		sr_C(&p, pc, se_confv, "branch_max", SS_U32, &o->rtp.total_branch_max, SR_RO, NULL);
		sr_C(&p, pc, se_confv, "branch_histogram", SS_STRINGPTR, &o->rtp.histogram_branch_ptr, SR_RO, NULL);
		sr_C(&p, pc, se_confv, "page_count", SS_U32, &o->rtp.total_page_count, SR_RO, NULL);
		/* scheme */
		struct srconf *scheme = *pc;
		p = NULL;
		int i = 0;
		while (i < o->scheme->scheme.fields_count) {
			struct sffield *field = o->scheme->scheme.fields[i];
			sr_C(&p, pc, se_confdb_field, field->name, SS_STRING, field->options, 0, o);
			i++;
		}
		/* database */
		struct srconf *database = *pc;
		p = NULL;
		sr_C(&p, pc, se_confv, "name", SS_STRINGPTR, &o->scheme->name, SR_RO, NULL);
		sr_C(&p, pc, se_confv_dboffline, "id", SS_U32, &o->scheme->id, 0, o);
		sr_C(&p, pc, se_confdb_status,   "status", SS_STRING, o, SR_RO, NULL);
		sr_C(&p, pc, se_confv_dboffline, "storage", SS_STRINGPTR, &o->scheme->storage_sz, 0, o);
		sr_C(&p, pc, se_confv_dboffline, "temperature", SS_U32, &o->scheme->temperature, 0, o);
		sr_C(&p, pc, se_confv_dboffline, "expire", SS_U32, &o->scheme->expire, 0, o);
		sr_C(&p, pc, se_confv_dboffline, "amqf", SS_U32, &o->scheme->amqf, 0, o);
		sr_C(&p, pc, se_confv_dboffline, "path", SS_STRINGPTR, &o->scheme->path, 0, o);
		sr_C(&p, pc, se_confv_dboffline, "path_fail_on_exists", SS_U32, &o->scheme->path_fail_on_exists, 0, o);
		sr_C(&p, pc, se_confv_dboffline, "path_fail_on_drop", SS_U32, &o->scheme->path_fail_on_drop, 0, o);
		sr_C(&p, pc, se_confv_dboffline, "mmap", SS_U32, &o->scheme->mmap, 0, o);
		sr_C(&p, pc, se_confv_dboffline, "sync", SS_U32, &o->scheme->sync, 0, o);
		sr_C(&p, pc, se_confv_dboffline, "node_preload", SS_U32, &o->scheme->node_compact_load, 0, o);
		sr_C(&p, pc, se_confv_dboffline, "node_size", SS_U64, &o->scheme->node_size, 0, o);
		sr_C(&p, pc, se_confv_dboffline, "page_size", SS_U32, &o->scheme->node_page_size, 0, o);
		sr_C(&p, pc, se_confv_dboffline, "page_checksum", SS_U32, &o->scheme->node_page_checksum, 0, o);
		sr_C(&p, pc, se_confv_dboffline, "compression_key", SS_U32, &o->scheme->compression_key, 0, o);
		sr_C(&p, pc, se_confv_dboffline, "compression_branch", SS_STRINGPTR, &o->scheme->compression_branch_sz, 0, o);
		sr_C(&p, pc, se_confv_dboffline, "compression", SS_STRINGPTR, &o->scheme->compression_sz, 0, o);
		sr_C(&p, pc, se_confv_dboffline, "lru", SS_U64, &o->scheme->lru, 0, o);
		sr_C(&p, pc, se_confv_dboffline, "lru_step", SS_U32, &o->scheme->lru_step, 0, o);
		sr_C(&p, pc, se_confdb_upsert, "upsert", SS_STRING, NULL, 0, o);
		sr_C(&p, pc, se_confdb_upsertarg, "upsert_arg", SS_STRING, NULL, 0, o);
		sr_c(&p, pc, se_confdb_branch, "branch", SS_FUNCTION, o);
		sr_c(&p, pc, se_confdb_compact, "compact", SS_FUNCTION, o);
		sr_c(&p, pc, se_confdb_compact_index, "compact_index", SS_FUNCTION, o);
		sr_C(&p, pc, NULL, "index", SS_UNDEF, index, SR_NS, o);
		sr_C(&p, pc, se_confdb_scheme, "scheme", SS_UNDEF, scheme, SR_NS, o);
		sr_C(&prev, pc, se_confdb_get, o->scheme->name, SS_STRING, database, SR_NS, o);
		if (db == NULL)
			db = prev;
	}
	return sr_C(NULL, pc, se_confdb_set, "db", SS_STRING, db, SR_NS, NULL);
}

static inline int
se_confview_set(struct srconf *c, struct srconfstmt *s)
{
	if (s->op != SR_WRITE)
		return se_confv(c, s);
	struct phia_env *e = s->ptr;
	uint64_t lsn = sr_seq(&e->seq, SR_LSN);
	/* create view object */
	struct seview *view = (struct seview*)se_viewnew(e, lsn, s->value, s->valuesize);
	if (ssunlikely(view == NULL))
		return -1;
	return 0;
}

static inline int
se_confview_lsn(struct srconf *c, struct srconfstmt *s)
{
	int rc = se_confv(c, s);
	if (ssunlikely(rc == -1))
		return -1;
	if (s->op != SR_WRITE)
		return 0;
	struct seview *view  = c->ptr;
	se_viewupdate(view);
	return 0;
}

static inline int
se_confview_get(struct srconf *c, struct srconfstmt *s)
{
	/* get(view.name) */
	struct phia_env *e = s->ptr;
	if (s->op != SR_READ) {
		sr_error(&e->error, "%s", "bad operation");
		return -1;
	}
	assert(c->ptr != NULL);
	*(void**)s->value = c->ptr;
	return 0;
}

static inline struct srconf*
se_confview(struct phia_env *e, struct seconfrt *rt ssunused, struct srconf **pc)
{
	struct srconf *view = NULL;
	(void) e;
	struct srconf *prev = NULL;
	struct seview *s;

	rlist_foreach_entry(s, &e->view, link)
	{
		struct srconf *p = sr_C(NULL, pc, se_confview_lsn, "lsn", SS_U64, &s->vlsn, 0, s);
		sr_C(&prev, pc, se_confview_get, s->name.s, SS_STRING, p, SR_NS, s);
		if (view == NULL)
			view = prev;
	}
	return sr_C(NULL, pc, se_confview_set, "view", SS_STRING,
	            view, SR_NS, NULL);
}


static inline int
se_confbackup_run(struct srconf *c, struct srconfstmt *s)
{
	if (s->op != SR_WRITE)
		return se_confv(c, s);
	struct phia_env *e = s->ptr;
	return sc_ctl_backup(&e->scheduler);
}

static inline struct srconf*
se_confbackup(struct phia_env *e, struct seconfrt *rt, struct srconf **pc)
{
	struct srconf *backup = *pc;
	struct srconf *p = NULL;
	sr_c(&p, pc, se_confv_offline, "path", SS_STRINGPTR, &e->conf.backup_path);
	sr_c(&p, pc, se_confbackup_run, "run", SS_FUNCTION, NULL);
	sr_C(&p, pc, se_confv, "active", SS_U32, &rt->backup_active, SR_RO, NULL);
	sr_c(&p, pc, se_confv, "last", SS_U32, &rt->backup_last);
	sr_c(&p, pc, se_confv, "last_complete", SS_U32, &rt->backup_last_complete);
	return sr_C(NULL, pc, NULL, "backup", 0, backup, SR_NS, NULL);
}

static inline int
se_confdebug_oom(struct srconf *c, struct srconfstmt *s)
{
	struct phia_env *e = s->ptr;
	assert(e->ei.oom == 0);
	int rc = se_confv(c, s);
	if (ssunlikely(rc == -1))
		return rc;
	ss_aclose(&e->a);
	ss_aopen(&e->a_oom, &ss_ooma, e->ei.oom);
	e->a = e->a_oom;
	return 0;
}

static inline int
se_confdebug_io(struct srconf *c, struct srconfstmt *s)
{
	struct phia_env *e = s->ptr;
	assert(e->ei.io == 0);
	int rc = se_confv(c, s);
	if (ssunlikely(rc == -1))
		return rc;
	ss_vfsfree(&e->vfs);
	ss_vfsinit(&e->vfs, &ss_testvfs, e->ei.io);
	return 0;
}

static inline struct srconf*
se_confdebug(struct phia_env *e, struct seconfrt *rt ssunused, struct srconf **pc)
{
	struct srconf *prev = NULL;
	struct srconf *p = NULL;
	prev = p;
	struct srconf *ei = *pc;
	sr_c(&p, pc, se_confdebug_oom, "oom",     SS_U32, &e->ei.oom);
	sr_c(&p, pc, se_confdebug_io, "io",       SS_U32, &e->ei.io);
	sr_c(&p, pc, se_confv, "sd_build_0",      SS_U32, &e->ei.e[0]);
	sr_c(&p, pc, se_confv, "sd_build_1",      SS_U32, &e->ei.e[1]);
	sr_c(&p, pc, se_confv, "si_branch_0",     SS_U32, &e->ei.e[2]);
	sr_c(&p, pc, se_confv, "si_compaction_0", SS_U32, &e->ei.e[3]);
	sr_c(&p, pc, se_confv, "si_compaction_1", SS_U32, &e->ei.e[4]);
	sr_c(&p, pc, se_confv, "si_compaction_2", SS_U32, &e->ei.e[5]);
	sr_c(&p, pc, se_confv, "si_compaction_3", SS_U32, &e->ei.e[6]);
	sr_c(&p, pc, se_confv, "si_compaction_4", SS_U32, &e->ei.e[7]);
	sr_c(&p, pc, se_confv, "si_recover_0",    SS_U32, &e->ei.e[8]);
	sr_c(&p, pc, se_confv, "si_snapshot_0",   SS_U32, &e->ei.e[9]);
	sr_c(&p, pc, se_confv, "si_snapshot_1",   SS_U32, &e->ei.e[10]);
	sr_c(&p, pc, se_confv, "si_snapshot_2",   SS_U32, &e->ei.e[11]);
	sr_C(&prev, pc, NULL, "error_injection", SS_UNDEF, ei, SR_NS, NULL);
	struct srconf *debug = prev;
	return sr_C(NULL, pc, NULL, "debug", SS_UNDEF, debug, SR_NS, NULL);
}

static struct srconf*
se_confprepare(struct phia_env *e, struct seconfrt *rt, struct srconf *c, int serialize)
{
	/* phia */
	struct srconf *pc = c;
	struct srconf *phia     = se_confphia(e, rt, &pc);
	struct srconf *memory     = se_confmemory(e, rt, &pc);
	struct srconf *compaction = se_confcompaction(e, rt, &pc);
	struct srconf *scheduler  = se_confscheduler(e, rt, &pc);
	struct srconf *perf       = se_confperformance(e, rt, &pc);
	struct srconf *metric     = se_confmetric(e, rt, &pc);
	struct srconf *log        = se_conflog(e, rt, &pc);
	struct srconf *view       = se_confview(e, rt, &pc);
	struct srconf *backup     = se_confbackup(e, rt, &pc);
	struct srconf *db         = se_confdb(e, rt, &pc);
	struct srconf *debug      = se_confdebug(e, rt, &pc);

	phia->next     = memory;
	memory->next     = compaction;
	compaction->next = scheduler;
	scheduler->next  = perf;
	perf->next       = metric;
	metric->next     = log;
	log->next        = view;
	view->next       = backup;
	backup->next     = db;
	if (! serialize)
		db->next = debug;
	return phia;
}

static int
se_confrt(struct phia_env *e, struct seconfrt *rt)
{
	/* phia */
	snprintf(rt->version, sizeof(rt->version),
	         "%d.%d.%d",
	         SR_VERSION_A - '0',
	         SR_VERSION_B - '0',
	         SR_VERSION_C - '0');
	snprintf(rt->version_storage, sizeof(rt->version_storage),
	         "%d.%d.%d",
	         SR_VERSION_STORAGE_A - '0',
	         SR_VERSION_STORAGE_B - '0',
	         SR_VERSION_STORAGE_C - '0');
	snprintf(rt->build, sizeof(rt->build), "%s",
	         PACKAGE_VERSION);

	/* memory */
	rt->memory_used = ss_quotaused(&e->quota);

	/* scheduler */
	tt_pthread_mutex_lock(&e->scheduler.lock);
	rt->checkpoint_active    = e->scheduler.checkpoint;
	rt->checkpoint_lsn_last  = e->scheduler.checkpoint_lsn_last;
	rt->checkpoint_lsn       = e->scheduler.checkpoint_lsn;
	rt->snapshot_active      = e->scheduler.snapshot;
	rt->snapshot_ssn         = e->scheduler.snapshot_ssn;
	rt->snapshot_ssn_last    = e->scheduler.snapshot_ssn_last;
	rt->anticache_active     = e->scheduler.anticache;
	rt->anticache_asn        = e->scheduler.anticache_asn;
	rt->anticache_asn_last   = e->scheduler.anticache_asn_last;
	rt->backup_active        = e->scheduler.backup;
	rt->backup_last          = e->scheduler.backup_bsn_last;
	rt->backup_last_complete = e->scheduler.backup_bsn_last_complete;
	rt->expire_active        = e->scheduler.expire;
	rt->gc_active            = e->scheduler.gc;
	rt->lru_active           = e->scheduler.lru;
	tt_pthread_mutex_unlock(&e->scheduler.lock);

	int v = ss_quotaused_percent(&e->quota);
	struct srzone *z = sr_zonemap(&e->conf.zones, v);
	memcpy(rt->zone, z->name, sizeof(rt->zone));

	/* log */
	rt->log_files = sl_poolfiles(&e->lp);

	/* metric */
	sr_seqlock(&e->seq);
	rt->seq = e->seq;
	sr_sequnlock(&e->seq);

	/* performance */
	rt->tx_rw = e->xm.count_rw;
	rt->tx_ro = e->xm.count_rd;
	rt->tx_gc_queue = e->xm.count_gc;

	tt_pthread_mutex_lock(&e->stat.lock);
	rt->stat = e->stat;
	tt_pthread_mutex_unlock(&e->stat.lock);
	sr_statprepare(&rt->stat);
	return 0;
}

static inline int
se_confensure(struct seconf *c)
{
	struct phia_env *e = (struct phia_env*)c->env;
	int confmax = 2048;
	confmax *= sizeof(struct srconf);
	if (sslikely(confmax <= c->confmax))
		return 0;
	struct srconf *cptr = ss_malloc(&e->a, confmax);
	if (ssunlikely(cptr == NULL))
		return sr_oom(&e->error);
	ss_free(&e->a, c->conf);
	c->conf = cptr;
	c->confmax = confmax;
	return 0;
}

static int se_confserialize(struct seconf *c, struct ssbuf *buf)
{
	int rc;
	rc = se_confensure(c);
	if (ssunlikely(rc == -1))
		return -1;
	struct phia_env *e = (struct phia_env*)c->env;
	struct seconfrt rt;
	se_confrt(e, &rt);
	struct srconf *conf = c->conf;
	struct srconf *root;
	root = se_confprepare(e, &rt, conf, 1);
	struct srconfstmt stmt = {
		.op        = SR_SERIALIZE,
		.path      = NULL,
		.value     = NULL,
		.valuesize = 0,
		.valuetype = SS_UNDEF,
		.serialize = buf,
		.ptr       = e,
		.r         = &e->r
	};
	return sr_confexec(root, &stmt);
}

static int
se_confquery(struct phia_env *e, int op, const char *path,
             enum sstype valuetype, void *value, int valuesize,
             int *size)
{
	int rc;
	rc = se_confensure(&e->conf);
	if (ssunlikely(rc == -1))
		return -1;
	struct seconfrt rt;
	se_confrt(e, &rt);
	struct srconf *conf = e->conf.conf;
	struct srconf *root;
	root = se_confprepare(e, &rt, conf, 0);
	struct srconfstmt stmt = {
		.op        = op,
		.path      = path,
		.value     = value,
		.valuesize = valuesize,
		.valuetype = valuetype,
		.serialize = NULL,
		.ptr       = e,
		.r         = &e->r
	};
	rc = sr_confexec(root, &stmt);
	if (size)
		*size = stmt.valuesize;
	return rc;
}

static int
se_confset_string(struct so *o, const char *path, void *string, int size)
{
	struct phia_env *e = se_of(o);
	if (string && size == 0)
		size = strlen(string) + 1;
	return se_confquery(e, SR_WRITE, path, SS_STRING,
	                   string, size, NULL);
}

static int se_confset_int(struct so *o, const char *path, int64_t v)
{
	struct phia_env *e = se_of(o);
	return se_confquery(e, SR_WRITE, path, SS_I64,
	                    &v, sizeof(v), NULL);
}

static void *se_confget_object(struct so *o, const char *path)
{
	struct phia_env *e = se_of(o);
	if (path == NULL)
		return se_confcursor_new(o);
	void *result = NULL;
	int rc = se_confquery(e, SR_READ, path, SS_OBJECT,
	                      &result, sizeof(void*), NULL);
	if (ssunlikely(rc == -1))
		return NULL;
	return result;
}

static void *se_confget_string(struct so *o, const char *path, int *size)
{
	struct phia_env *e = se_of(o);
	void *result = NULL;
	int rc = se_confquery(e, SR_READ, path, SS_STRING,
	                      &result, sizeof(void*), size);
	if (ssunlikely(rc == -1))
		return NULL;
	return result;
}

static int64_t se_confget_int(struct so *o, const char *path)
{
	struct phia_env *e = se_of(o);
	int64_t result = 0;
	int rc = se_confquery(e, SR_READ, path, SS_I64,
	                      &result, sizeof(void*), NULL);
	if (ssunlikely(rc == -1))
		return -1;
	return result;
}

static int se_confinit(struct seconf *c, struct so *e)
{
	struct phia_env *o = se_of(e);
	c->confmax = 2048;
	c->conf = ss_malloc(&o->a, sizeof(struct srconf) * c->confmax);
	if (ssunlikely(c->conf == NULL))
		return -1;
	sf_schemeinit(&c->scheme);
	c->env                 = e;
	c->path                = NULL;
	c->path_create         = 1;
	c->recover             = 1;
	c->memory_limit        = 0;
	c->anticache           = 0;
	c->log_enable          = 1;
	c->log_path            = NULL;
	c->log_rotate_wm       = 500000;
	c->log_sync            = 0;
	c->log_rotate_sync     = 1;
	c->on_recover.function = NULL;
	c->on_recover.arg      = NULL;
	ss_triggerinit(&c->on_event);
	c->event_on_backup     = 0;
	struct srzone def = {
		.enable            = 1,
		.mode              = 3, /* branch + compact */
		.compact_wm        = 2,
		.compact_mode      = 0, /* branch priority */
		.branch_prio       = 1,
		.branch_wm         = 10 * 1024 * 1024,
		.branch_age        = 40,
		.branch_age_period = 40,
		.branch_age_wm     = 1 * 1024 * 1024,
		.anticache_period  = 0,
		.snapshot_period   = 0,
		.backup_prio       = 1,
		.expire_prio       = 0,
		.expire_period     = 0,
		.gc_prio           = 1,
		.gc_period         = 60,
		.gc_wm             = 30,
		.lru_prio          = 0,
		.lru_period        = 0
	};
	struct srzone redzone = {
		.enable            = 1,
		.mode              = 2, /* checkpoint */
		.compact_wm        = 4,
		.compact_mode      = 0,
		.branch_prio       = 0,
		.branch_wm         = 0,
		.branch_age        = 0,
		.branch_age_period = 0,
		.branch_age_wm     = 0,
		.anticache_period  = 0,
		.snapshot_period   = 0,
		.backup_prio       = 0,
		.expire_prio       = 0,
		.expire_period     = 0,
		.gc_prio           = 0,
		.gc_period         = 0,
		.gc_wm             = 0,
		.lru_prio          = 0,
		.lru_period        = 0
	};
	sr_zonemap_set(&o->conf.zones,  0, &def);
	sr_zonemap_set(&o->conf.zones, 80, &redzone);
	c->backup_path = NULL;
	return 0;
}

static void se_conffree(struct seconf *c)
{
	struct phia_env *e = (struct phia_env*)c->env;
	if (c->conf) {
		ss_free(&e->a, c->conf);
		c->conf = NULL;
	}
	if (c->path) {
		ss_free(&e->a, c->path);
		c->path = NULL;
	}
	if (c->log_path) {
		ss_free(&e->a, c->log_path);
		c->log_path = NULL;
	}
	if (c->backup_path) {
		ss_free(&e->a, c->backup_path);
		c->backup_path = NULL;
	}
	sf_schemefree(&c->scheme, &e->a);
}

static int se_confvalidate(struct seconf *c)
{
	struct phia_env *e = (struct phia_env*)c->env;
	if (c->path == NULL) {
		sr_error(&e->error, "%s", "repository path is not set");
		return -1;
	}
	char path[1024];
	if (c->log_path == NULL) {
		snprintf(path, sizeof(path), "%s/log", c->path);
		c->log_path = ss_strdup(&e->a, path);
		if (ssunlikely(c->log_path == NULL)) {
			return sr_oom(&e->error);
		}
	}
	int i = 0;
	for (; i < 11; i++) {
		struct srzone *z = &e->conf.zones.zones[i];
		if (! z->enable)
			continue;
		if (z->compact_wm <= 1) {
			sr_error(&e->error, "bad %d.compact_wm value", i * 10);
			return -1;
		}
		/* convert periodic times from sec to usec */
		z->branch_age_period_us = z->branch_age_period * 1000000;
		z->snapshot_period_us   = z->snapshot_period * 1000000;
		z->anticache_period_us  = z->anticache_period * 1000000;
		z->gc_period_us         = z->gc_period * 1000000;
		z->expire_period_us     = z->expire_period * 1000000;
		z->lru_period_us        = z->lru_period * 1000000;
	}
	return 0;
}

static void
se_confkv_free(struct so *o)
{
	struct seconfkv *v = (struct seconfkv*)o;
	struct phia_env *e = se_of(o);
	ss_buffree(&v->key, &e->a);
	ss_buffree(&v->value, &e->a);
	ss_free(&e->a, v);
}

static int
se_confkv_destroy(struct so *o)
{
	struct seconfkv *v = se_cast(o, struct seconfkv*, SECONFKV);
	ss_bufreset(&v->key);
	ss_bufreset(&v->value);
	so_free(&v->o);
	return 0;
}

static void *se_confkv_getstring(struct so *o, const char *path, int *size)
{
	struct seconfkv *v = se_cast(o, struct seconfkv*, SECONFKV);
	int len;
	if (strcmp(path, "key") == 0) {
		len = ss_bufused(&v->key);
		if (size)
			*size = len;
		return v->key.s;
	} else
	if (strcmp(path, "value") == 0) {
		len = ss_bufused(&v->value);
		if (size)
			*size = len;
		if (len == 0)
			return NULL;
		return v->value.s;
	}
	return NULL;
}

static struct soif seconfkvif =
{
	.open         = NULL,
	.close        = NULL,
	.destroy      = se_confkv_destroy,
	.free         = se_confkv_free,
	.document     = NULL,
	.drop         = NULL,
	.setstring    = NULL,
	.setint       = NULL,
	.setobject    = NULL,
	.getobject    = NULL,
	.getstring    = se_confkv_getstring,
	.getint       = NULL,
	.set          = NULL,
	.upsert       = NULL,
	.del          = NULL,
	.get          = NULL,
	.begin        = NULL,
	.commit       = NULL,
	.cursor       = NULL,
};

static inline struct so *se_confkv_new(struct phia_env *e, struct srconfdump *vp)
{
	struct seconfkv *v;
	v = ss_malloc(&e->a, sizeof(struct seconfkv));
	if (ssunlikely(v == NULL)) {
		sr_oom(&e->error);
		return NULL;
	}
	so_init(&v->o, &se_o[SECONFKV], &seconfkvif, &e->o, &e->o);
	ss_bufinit(&v->key);
	ss_bufinit(&v->value);
	int rc;
	rc = ss_bufensure(&v->key, &e->a, vp->keysize);
	if (ssunlikely(rc == -1)) {
		so_free(&v->o);
		sr_oom(&e->error);
		return NULL;
	}
	rc = ss_bufensure(&v->value, &e->a, vp->valuesize);
	if (ssunlikely(rc == -1)) {
		so_free(&v->o);
		sr_oom(&e->error);
		return NULL;
	}
	memcpy(v->key.s, sr_confkey(vp), vp->keysize);
	memcpy(v->value.s, sr_confvalue(vp), vp->valuesize);
	ss_bufadvance(&v->key, vp->keysize);
	ss_bufadvance(&v->value, vp->valuesize);
	return &v->o;
}

static void
se_confcursor_free(struct so *o)
{
	struct phia_env *e = se_of(o);
	struct seconfcursor *c = (struct seconfcursor*)o;
	ss_buffree(&c->dump, &e->a);
	ss_free(&e->a, o);
}

static int
se_confcursor_destroy(struct so *o)
{
	struct seconfcursor *c = se_cast(o, struct seconfcursor*, SECONFCURSOR);
	ss_bufreset(&c->dump);
	so_free(&c->o);
	return 0;
}

static void*
se_confcursor_get(struct so *o, struct so *v)
{
	struct seconfcursor *c = se_cast(o, struct seconfcursor*, SECONFCURSOR);
	if (v) {
		so_destroy(v);
	}
	if (c->first) {
		assert( ss_bufsize(&c->dump) >= (int)sizeof(struct srconfdump) );
		c->first = 0;
		c->pos = (struct srconfdump*)c->dump.s;
	} else {
		int size = sizeof(struct srconfdump) + c->pos->keysize + c->pos->valuesize;
		c->pos = (struct srconfdump*)((char*)c->pos + size);
		if ((char*)c->pos >= c->dump.p)
			c->pos = NULL;
	}
	if (ssunlikely(c->pos == NULL))
		return NULL;
	struct phia_env *e = se_of(&c->o);
	return se_confkv_new(e, c->pos);
}

static struct soif seconfcursorif =
{
	.open         = NULL,
	.destroy      = se_confcursor_destroy,
	.free         = se_confcursor_free,
	.document     = NULL,
	.drop         = NULL,
	.setstring    = NULL,
	.setint       = NULL,
	.setobject    = NULL,
	.getobject    = NULL,
	.getstring    = NULL,
	.getint       = NULL,
	.set          = NULL,
	.upsert       = NULL,
	.del          = NULL,
	.get          = se_confcursor_get,
	.begin        = NULL,
	.commit       = NULL,
	.cursor       = NULL,
};

static struct so *se_confcursor_new(struct so *o)
{
	struct phia_env *e = (struct phia_env*)o;
	struct seconfcursor *c;
	c = ss_malloc(&e->a, sizeof(struct seconfcursor));
	if (ssunlikely(c == NULL)) {
		sr_oom(&e->error);
		return NULL;
	}
	so_init(&c->o, &se_o[SECONFCURSOR], &seconfcursorif, &e->o, &e->o);
	c->pos = NULL;
	c->first = 1;
	ss_bufinit(&c->dump);
	int rc = se_confserialize(&e->conf, &c->dump);
	if (ssunlikely(rc == -1)) {
		so_free(&c->o);
		sr_oom(&e->error);
		return NULL;
	}
	return &c->o;
}

static void
se_cursorfree(struct so *o)
{
	struct phia_env *e = se_of(o);
	ss_free(&e->a, o);
}

static int
se_cursordestroy(struct so *o)
{
	struct secursor *c = se_cast(o, struct secursor*, SECURSOR);
	struct phia_env *e = se_of(&c->o);
	uint64_t id = c->t.id;
	if (! c->read_commited)
		sx_rollback(&c->t);
	if (c->cache)
		si_cachepool_push(c->cache);
	se_dbunbind(e, id);
	sr_statcursor(&e->stat, c->start,
	              c->read_disk,
	              c->read_cache,
	              c->ops);
	so_free(&c->o);
	return 0;
}

static void*
se_cursorget(struct so *o, struct so *v)
{
	struct secursor *c = se_cast(o, struct secursor*, SECURSOR);
	struct sedocument *key = se_cast(v, struct sedocument*, SEDOCUMENT);
	struct sedb *db = se_cast(v->parent, struct sedb*, SEDB);
	if (ssunlikely(! key->orderset))
		key->order = SS_GTE;
	/* this statistics might be not complete, because
	 * last statement is not accounted here */
	c->read_disk  += key->read_disk;
	c->read_cache += key->read_cache;
	c->ops++;
	struct sx *x = &c->t;
	if (c->read_commited)
		x = NULL;
	return se_dbread(db, key, x, 0, c->cache);
}

static int
se_cursorset_int(struct so *o, const char *path, int64_t v)
{
	struct secursor *c = se_cast(o, struct secursor*, SECURSOR);
	if (strcmp(path, "read_commited") == 0) {
		if (c->read_commited)
			return -1;
		if (v != 1)
			return -1;
		sx_rollback(&c->t);
		c->read_commited = 1;
		return 0;
	}
	return -1;
}

static struct soif secursorif =
{
	.open         = NULL,
	.close        = NULL,
	.destroy      = se_cursordestroy,
	.free         = se_cursorfree,
	.document     = NULL,
	.drop         = NULL,
	.setstring    = NULL,
	.setint       = se_cursorset_int,
	.setobject    = NULL,
	.getobject    = NULL,
	.getstring    = NULL,
	.getint       = NULL,
	.set          = NULL,
	.upsert       = NULL,
	.del          = NULL,
	.get          = se_cursorget,
	.begin        = NULL,
	.commit       = NULL,
	.cursor       = NULL,
};

static struct so *se_cursornew(struct phia_env *e, uint64_t vlsn)
{
	struct secursor *c;
	c = ss_malloc(&e->a, sizeof(struct secursor));
	if (ssunlikely(c == NULL)) {
		sr_oom(&e->error);
		return NULL;
	}
	so_init(&c->o, &se_o[SECURSOR], &secursorif, &e->o, &e->o);
	sv_loginit(&c->log);
	sx_init(&e->xm, &c->t, &c->log);
	c->start = clock_monotonic64();
	c->ops = 0;
	c->read_disk = 0;
	c->read_cache = 0;
	c->t.state = SXUNDEF;
	c->cache = si_cachepool_pop(&e->cachepool);
	if (ssunlikely(c->cache == NULL)) {
		so_free(&c->o);
		sr_oom(&e->error);
		return NULL;
	}
	c->read_commited = 0;
	sx_begin(&e->xm, &c->t, SXRO, &c->log, vlsn);
	se_dbbind(e);
	return &c->o;
}

static int
se_dbscheme_init(struct sedb *db, char *name, int size)
{
	struct phia_env *e = se_of(&db->o);
	/* prepare index scheme */
	struct sischeme *scheme = db->scheme;
	if (size == 0)
		size = strlen(name);
	scheme->name = ss_malloc(&e->a, size + 1);
	if (ssunlikely(scheme->name == NULL))
		goto error;
	memcpy(scheme->name, name, size);
	scheme->name[size] = 0;
	scheme->id                    = sr_seq(&e->seq, SR_DSNNEXT);
	scheme->sync                  = 2;
	scheme->mmap                  = 0;
	scheme->storage               = SI_SCACHE;
	scheme->node_size             = 64 * 1024 * 1024;
	scheme->node_compact_load     = 0;
	scheme->node_page_size        = 128 * 1024;
	scheme->node_page_checksum    = 1;
	scheme->compression_key       = 0;
	scheme->compression           = 0;
	scheme->compression_if        = &ss_nonefilter;
	scheme->compression_branch    = 0;
	scheme->compression_branch_if = &ss_nonefilter;
	scheme->temperature           = 0;
	scheme->expire                = 0;
	scheme->amqf                  = 0;
	scheme->fmt_storage           = SF_RAW;
	scheme->path_fail_on_exists   = 0;
	scheme->path_fail_on_drop     = 1;
	scheme->lru                   = 0;
	scheme->lru_step              = 128 * 1024;
	scheme->buf_gc_wm             = 1024 * 1024;
	scheme->storage_sz = ss_strdup(&e->a, "cache");
	if (ssunlikely(scheme->storage_sz == NULL))
		goto error;
	scheme->compression_sz =
		ss_strdup(&e->a, scheme->compression_if->name);
	if (ssunlikely(scheme->compression_sz == NULL))
		goto error;
	scheme->compression_branch_sz =
		ss_strdup(&e->a, scheme->compression_branch_if->name);
	if (ssunlikely(scheme->compression_branch_sz == NULL))
		goto error;
	sf_upsertinit(&scheme->fmt_upsert);
	sf_schemeinit(&scheme->scheme);
	return 0;
error:
	sr_oom(&e->error);
	return -1;
}

static int
se_dbscheme_set(struct sedb *db)
{
	struct phia_env *e = se_of(&db->o);
	struct sischeme *s = si_scheme(db->index);
	/* set default scheme */
	int rc;
	if (s->scheme.fields_count == 0)
	{
		struct sffield *field = sf_fieldnew(&e->a, "key");
		if (ssunlikely(field == NULL))
			return sr_oom(&e->error);
		rc = sf_fieldoptions(field, &e->a, "string,key(0)");
		if (ssunlikely(rc == -1)) {
			sf_fieldfree(field, &e->a);
			return sr_oom(&e->error);
		}
		rc = sf_schemeadd(&s->scheme, &e->a, field);
		if (ssunlikely(rc == -1)) {
			sf_fieldfree(field, &e->a);
			return sr_oom(&e->error);
		}
		field = sf_fieldnew(&e->a, "value");
		if (ssunlikely(field == NULL))
			return sr_oom(&e->error);
		rc = sf_fieldoptions(field, &e->a, "string");
		if (ssunlikely(rc == -1)) {
			sf_fieldfree(field, &e->a);
			return sr_oom(&e->error);
		}
		rc = sf_schemeadd(&s->scheme, &e->a, field);
		if (ssunlikely(rc == -1)) {
			sf_fieldfree(field, &e->a);
			return sr_oom(&e->error);
		}
	}
	/* validate scheme and set keys */
	rc = sf_schemevalidate(&s->scheme, &e->a);
	if (ssunlikely(rc == -1)) {
		sr_error(&e->error, "incomplete scheme", s->name);
		return -1;
	}
	/* storage */
	if (strcmp(s->storage_sz, "cache") == 0) {
		s->storage = SI_SCACHE;
	} else
	if (strcmp(s->storage_sz, "anti-cache") == 0) {
		s->storage = SI_SANTI_CACHE;
	} else
	if (strcmp(s->storage_sz, "in-memory") == 0) {
		s->storage = SI_SIN_MEMORY;
	} else {
		sr_error(&e->error, "unknown storage type '%s'", s->storage_sz);
		return -1;
	}
	/* compression_key */
	if (s->compression_key) {
		s->fmt_storage = SF_SPARSE;
	}
	/* compression */
	s->compression_if = ss_filterof(s->compression_sz);
	if (ssunlikely(s->compression_if == NULL)) {
		sr_error(&e->error, "unknown compression type '%s'",
		         s->compression_sz);
		return -1;
	}
	s->compression = s->compression_if != &ss_nonefilter;
	/* compression branch */
	s->compression_branch_if = ss_filterof(s->compression_branch_sz);
	if (ssunlikely(s->compression_branch_if == NULL)) {
		sr_error(&e->error, "unknown compression type '%s'",
		         s->compression_branch_sz);
		return -1;
	}
	s->compression_branch = s->compression_branch_if != &ss_nonefilter;
	/* path */
	if (s->path == NULL) {
		char path[1024];
		snprintf(path, sizeof(path), "%s/%s", e->conf.path, s->name);
		s->path = ss_strdup(&e->a, path);
		if (ssunlikely(s->path == NULL))
			return sr_oom(&e->error);
	}
	/* backup path */
	s->path_backup = e->conf.backup_path;
	if (e->conf.backup_path) {
		s->path_backup = ss_strdup(&e->a, e->conf.backup_path);
		if (ssunlikely(s->path_backup == NULL))
			return sr_oom(&e->error);
	}

	db->r->scheme = &s->scheme;
	db->r->fmt_storage = s->fmt_storage;
	db->r->fmt_upsert = &s->fmt_upsert;
	return 0;
}

static int
se_dbopen(struct so *o)
{
	struct sedb *db = se_cast(o, struct sedb*, SEDB);
	struct phia_env *e = se_of(&db->o);
	int status = sr_status(&db->index->status);
	if (status == SR_RECOVER ||
	    status == SR_DROP_PENDING)
		goto online;
	if (status != SR_OFFLINE)
		return -1;
	int rc = se_dbscheme_set(db);
	if (ssunlikely(rc == -1))
		return -1;
	sx_indexset(&db->coindex, db->scheme->id);
	rc = se_recoverbegin(db);
	if (ssunlikely(rc == -1))
		return -1;

	if (sr_status(&e->status) == SR_RECOVER)
		if (e->conf.recover != SE_RECOVER_NP)
			return 0;
online:
	se_recoverend(db);
	rc = sc_add(&e->scheduler, db->index);
	if (ssunlikely(rc == -1))
		return -1;
	return 0;
}

static inline int
se_dbfree(struct sedb *db, int close)
{
	struct phia_env *e = se_of(&db->o);
	int rcret = 0;
	int rc;
	sx_indexfree(&db->coindex, &e->xm);
	if (close) {
		rc = si_close(db->index);
		if (ssunlikely(rc == -1))
			rcret = -1;
	}
	ss_free(&e->a, db);
	return rcret;
}

static inline void
se_dbunref(struct sedb *db)
{
	struct phia_env *e = se_of(&db->o);
	/* do nothing during env shutdown */
	int status = sr_status(&e->status);
	if (status == SR_SHUTDOWN)
		return;
	/* reduce reference counter */
	int ref;
	ref = si_unref(db->index, SI_REFFE);
	if (ref > 1)
		return;
	/* drop/shutdown pending:
	 *
	 * switch state and transfer job to
	 * the scheduler.
	*/
	status = sr_status(&db->index->status);
	switch (status) {
	case SR_SHUTDOWN_PENDING:
		status = SR_SHUTDOWN;
		break;
	case SR_DROP_PENDING:
		status = SR_DROP;
		break;
	default:
		return;
	}
	/* destroy database object */
	struct si *index = db->index;
	rlist_del(&db->link);
	se_dbfree(db, 0);

	/* schedule index shutdown or drop */
	sr_statusset(&index->status, status);
	sc_ctl_shutdown(&e->scheduler, index);
}

static int
se_dbdestroy(struct so *o)
{
	struct sedb *db = se_cast(o, struct sedb*, SEDB);
	struct phia_env *e = se_of(&db->o);
	int status = sr_status(&e->status);
	if (status == SR_SHUTDOWN ||
	    status == SR_OFFLINE) {
		return se_dbfree(db, 1);
	}
	se_dbunref(db);
	return 0;
}

static int
se_dbclose(struct so *o)
{
	struct sedb *db = se_cast(o, struct sedb*, SEDB);
	struct phia_env *e = se_of(&db->o);
	int status = sr_status(&db->index->status);
	if (ssunlikely(! sr_statusactive_is(status)))
		return -1;
	/* set last visible transaction id */
	db->txn_max = sx_max(&e->xm);
	sr_statusset(&db->index->status, SR_SHUTDOWN_PENDING);
	return 0;
}

static int
se_dbdrop(struct so *o)
{
	struct sedb *db = se_cast(o, struct sedb*, SEDB);
	struct phia_env *e = se_of(&db->o);
	int status = sr_status(&db->index->status);
	if (ssunlikely(! sr_statusactive_is(status)))
		return -1;
	int rc = si_dropmark(db->index);
	if (ssunlikely(rc == -1))
		return -1;
	/* set last visible transaction id */
	db->txn_max = sx_max(&e->xm);
	sr_statusset(&db->index->status, SR_DROP_PENDING);
	return 0;
}

static struct so *se_dbresult(struct phia_env *e, struct scread *r)
{
	struct sv result;
	sv_init(&result, &sv_vif, r->result, NULL);
	r->result = NULL;

	struct sedocument *v = (struct sedocument*)se_document_new(e, r->db, &result);
	if (ssunlikely(v == NULL))
		return NULL;
	v->cache_only   = r->arg.cache_only;
	v->oldest_only  = r->arg.oldest_only;
	v->read_disk    = r->read_disk;
	v->read_cache   = r->read_cache;
	v->read_latency = 0;
	if (result.v) {
		v->read_latency = clock_monotonic64() - r->start;
		sr_statget(&e->stat,
		           v->read_latency,
		           v->read_disk,
		           v->read_cache);
	}

	/* propagate current document settings to
	 * the result one */
	v->orderset = 1;
	v->order = r->arg.order;
	if (v->order == SS_GTE)
		v->order = SS_GT;
	else
	if (v->order == SS_LTE)
		v->order = SS_LT;

	/* set prefix */
	if (r->arg.prefix) {
		v->prefix = r->arg.prefix;
		v->prefixcopy = r->arg.prefix;
		v->prefixsize = r->arg.prefixsize;
	}

	v->created = 1;
	v->flagset = 1;
	return &v->o;
}

static void *
se_dbread(struct sedb *db, struct sedocument *o, struct sx *x, int x_search,
          struct sicache *cache)
{
	struct phia_env *e = se_of(&db->o);
	uint64_t start  = clock_monotonic64();

	/* prepare the key */
	int auto_close = !o->created;
	int rc = so_open(&o->o);
	if (ssunlikely(rc == -1))
		goto error;
	rc = se_document_validate_ro(o, &db->o);
	if (ssunlikely(rc == -1))
		goto error;
	if (ssunlikely(! sr_online(&db->index->status)))
		goto error;

	struct sv vup;
	sv_init(&vup, &sv_vif, NULL, NULL);

	struct sedocument *ret = NULL;

	/* concurrent */
	if (x_search && o->order == SS_EQ) {
		/* note: prefix is ignored during concurrent
		 * index search */
		int rc = sx_get(x, &db->coindex, &o->v, &vup);
		if (ssunlikely(rc == -1 || rc == 2 /* delete */))
			goto error;
		if (rc == 1 && !sv_is(&vup, SVUPSERT)) {
			ret = (struct sedocument*)se_document_new(e, &db->o, &vup);
			if (sslikely(ret)) {
				ret->cache_only  = o->cache_only;
				ret->oldest_only = o->oldest_only;
				ret->created     = 1;
				ret->orderset    = 1;
				ret->flagset     = 1;
			} else {
				sv_vunref(db->r, vup.v);
			}
			if (auto_close)
				so_destroy(&o->o);
			return ret;
		}
	} else {
		sx_get_autocommit(&e->xm, &db->coindex);
	}

	/* prepare read cache */
	int cachegc = 0;
	if (cache == NULL) {
		cachegc = 1;
		cache = si_cachepool_pop(&e->cachepool);
		if (ssunlikely(cache == NULL)) {
			if (vup.v)
				sv_vunref(db->r, vup.v);
			sr_oom(&e->error);
			goto error;
		}
	}

	sv_vref(o->v.v);

	/* prepare request */
	struct scread q;
	sc_readopen(&q, db->r, &db->o, db->index);
	q.start = start;
	struct screadarg *arg = &q.arg;
	arg->v           = o->v;
	arg->prefix      = o->prefixcopy;
	arg->prefixsize  = o->prefixsize;
	arg->vup         = vup;
	arg->cache       = cache;
	arg->cachegc     = cachegc;
	arg->order       = o->order;
	arg->has         = 0;
	arg->upsert      = 0;
	arg->upsert_eq   = 0;
	arg->cache_only  = o->cache_only;
	arg->oldest_only = o->oldest_only;
	if (x) {
		arg->vlsn = x->vlsn;
		arg->vlsn_generate = 0;
	} else {
		arg->vlsn = 0;
		arg->vlsn_generate = 1;
	}
	if (sf_upserthas(&db->scheme->fmt_upsert)) {
		arg->upsert = 1;
		if (arg->order == SS_EQ) {
			arg->order = SS_GTE;
			arg->upsert_eq = 1;
		}
	}

	/* read index */
	rc = sc_read(&q, &e->scheduler);
	if (rc == 1) {
		ret = (struct sedocument*)se_dbresult(e, &q);
		if (ret)
			o->prefixcopy = NULL;
	}
	sc_readclose(&q);

	if (auto_close)
		so_destroy(&o->o);
	return ret;
error:
	if (auto_close)
		so_destroy(&o->o);
	return NULL;
}

static inline int
se_dbwrite(struct sedb *db, struct sedocument *o, uint8_t flags)
{
	struct phia_env *e = se_of(&db->o);

	int auto_close = !o->created;
	if (ssunlikely(! sr_online(&db->index->status)))
		goto error;

	/* create document */
	int rc = so_open(&o->o);
	if (ssunlikely(rc == -1))
		goto error;
	rc = se_document_validate(o, &db->o, flags);
	if (ssunlikely(rc == -1))
		goto error;

	struct svv *v = o->v.v;
	sv_vref(v);

	/* destroy document object */
	if (auto_close) {
		/* ensure quota */
		ss_quota(&e->quota, SS_QADD, sv_vsize(v));
		so_destroy(&o->o);
	}

	/* single-statement transaction */
	struct svlog log;
	sv_loginit(&log);
	struct sx x;
	enum sxstate state = sx_set_autocommit(&e->xm, &db->coindex, &x, &log, v);
	switch (state) {
	case SXLOCK: return 2;
	case SXROLLBACK: return 1;
	default: break;
	}

	/* write wal and index */
	rc = sc_write(&e->scheduler, &log, 0, 0);
	if (ssunlikely(rc == -1))
		sx_rollback(&x);

	sx_gc(&x);
	return rc;

error:
	if (auto_close)
		so_destroy(&o->o);
	return -1;
}

static int
se_dbset(struct so *o, struct so *v)
{
	struct sedb *db = se_cast(o, struct sedb*, SEDB);
	struct sedocument *key = se_cast(v, struct sedocument*, SEDOCUMENT);
	struct phia_env *e = se_of(&db->o);
	uint64_t start = clock_monotonic64();
	int rc = se_dbwrite(db, key, 0);
	sr_statset(&e->stat, start);
	return rc;
}

static int
se_dbupsert(struct so *o, struct so *v)
{
	struct sedb *db = se_cast(o, struct sedb*, SEDB);
	struct sedocument *key = se_cast(v, struct sedocument*, SEDOCUMENT);
	struct phia_env *e = se_of(&db->o);
	uint64_t start = clock_monotonic64();
	if (! sf_upserthas(&db->scheme->fmt_upsert))
		return sr_error(&e->error, "%s", "upsert callback is not set");
	int rc = se_dbwrite(db, key, SVUPSERT);
	sr_statupsert(&e->stat, start);
	return rc;
}

static int
se_dbdel(struct so *o, struct so *v)
{
	struct sedb *db = se_cast(o, struct sedb*, SEDB);
	struct sedocument *key = se_cast(v, struct sedocument*, SEDOCUMENT);
	struct phia_env *e = se_of(&db->o);
	uint64_t start = clock_monotonic64();
	int rc = se_dbwrite(db, key, SVDELETE);
	sr_statdelete(&e->stat, start);
	return rc;
}

static void *
se_dbget(struct so *o, struct so *v)
{
	struct sedb *db = se_cast(o, struct sedb*, SEDB);
	struct sedocument *key = se_cast(v, struct sedocument*, SEDOCUMENT);
	return se_dbread(db, key, NULL, 0, NULL);
}

static void *
se_dbdocument(struct so *o)
{
	struct sedb *db = se_cast(o, struct sedb*, SEDB);
	struct phia_env *e = se_of(&db->o);
	return se_document_new(e, &db->o, NULL);
}

static void *
se_dbget_string(struct so *o, const char *path, int *size)
{
	struct sedb *db = se_cast(o, struct sedb*, SEDB);
	if (strcmp(path, "name") == 0) {
		int namesize = strlen(db->scheme->name) + 1;
		if (size)
			*size = namesize;
		char *name = malloc(namesize);
		if (name == NULL)
			return NULL;
		memcpy(name, db->scheme->name, namesize);
		return name;
	}
	return NULL;
}

static int64_t
se_dbget_int(struct so *o, const char *path)
{
	struct sedb *db = se_cast(o, struct sedb*, SEDB);
	if (strcmp(path, "id") == 0)
		return db->scheme->id;
	else
	if (strcmp(path, "key-count") == 0)
		return db->scheme->scheme.keys_count;
	return -1;
}

static struct soif sedbif =
{
	.open         = se_dbopen,
	.close        = se_dbclose,
	.destroy      = se_dbdestroy,
	.free         = NULL,
	.document     = se_dbdocument,
	.drop         = se_dbdrop,
	.setstring    = NULL,
	.setint       = NULL,
	.setobject    = NULL,
	.getobject    = NULL,
	.getstring    = se_dbget_string,
	.getint       = se_dbget_int,
	.set          = se_dbset,
	.upsert       = se_dbupsert,
	.del          = se_dbdel,
	.get          = se_dbget,
	.begin        = NULL,
	.commit       = NULL,
	.cursor       = NULL,
};

static struct so *se_dbnew(struct phia_env *e, char *name, int size)
{
	struct sedb *o = ss_malloc(&e->a, sizeof(struct sedb));
	if (ssunlikely(o == NULL)) {
		sr_oom(&e->error);
		return NULL;
	}
	memset(o, 0, sizeof(*o));
	so_init(&o->o, &se_o[SEDB], &sedbif, &e->o, &e->o);
	o->index = si_init(&e->r, &o->o);
	if (ssunlikely(o->index == NULL)) {
		ss_free(&e->a, o);
		return NULL;
	}
	o->r = si_r(o->index);
	o->scheme = si_scheme(o->index);
	int rc;
	rc = se_dbscheme_init(o, name, size);
	if (ssunlikely(rc == -1)) {
		si_close(o->index);
		ss_free(&e->a, o);
		return NULL;
	}
	sr_statusset(&o->index->status, SR_OFFLINE);
	sx_indexinit(&o->coindex, &e->xm, o->r, &o->o, o->index);
	o->txn_min = sx_min(&e->xm);
	o->txn_max = UINT32_MAX;
	return &o->o;
}

static struct so *se_dbmatch(struct phia_env *e, char *name)
{
	struct sedb *db;
	rlist_foreach_entry(db, &e->db, link) {
		if (strcmp(db->scheme->name, name) == 0)
			return &db->o;
	}
	return NULL;
}

static struct so *se_dbmatch_id(struct phia_env *e, uint32_t id)
{
	struct sedb *db;
	rlist_foreach_entry(db, &e->db, link) {
		if (db->scheme->id == id)
			return &db->o;
	}
	return NULL;
}

static int se_dbvisible(struct sedb *db, uint64_t txn)
{
	return txn > db->txn_min && txn <= db->txn_max;
}

static void se_dbbind(struct phia_env *e)
{
	struct sedb *db;
	rlist_foreach_entry(db, &e->db, link) {
		int status = sr_status(&db->index->status);
		if (sr_statusactive_is(status))
			si_ref(db->index, SI_REFFE);
	}
}

static void se_dbunbind(struct phia_env *e, uint64_t txn)
{
	struct sedb *db, *tmp;
	rlist_foreach_entry_safe(db, &e->db, link, tmp) {
		if (se_dbvisible(db, txn))
			se_dbunref(db);
	}
}

enum {
	SE_DOCUMENT_FIELD,
	SE_DOCUMENT_ORDER,
	SE_DOCUMENT_PREFIX,
	SE_DOCUMENT_LSN,
	SE_DOCUMENT_TIMESTAMP,
	SE_DOCUMENT_LOG,
	SE_DOCUMENT_RAW,
	SE_DOCUMENT_FLAGS,
	SE_DOCUMENT_CACHE_ONLY,
	SE_DOCUMENT_OLDEST_ONLY,
	SE_DOCUMENT_EVENT,
	SE_DOCUMENT_REUSE,
	SE_DOCUMENT_UNKNOWN
};

static inline int
se_document_opt(const char *path)
{
	switch (path[0]) {
	case 'o':
		if (sslikely(strcmp(path, "order") == 0))
			return SE_DOCUMENT_ORDER;
		if (sslikely(strcmp(path, "oldest_only") == 0))
			return SE_DOCUMENT_OLDEST_ONLY;
		break;
	case 'l':
		if (sslikely(strcmp(path, "lsn") == 0))
			return SE_DOCUMENT_LSN;
		if (sslikely(strcmp(path, "log") == 0))
			return SE_DOCUMENT_LOG;
		break;
	case 't':
		if (sslikely(strcmp(path, "timestamp") == 0))
			return SE_DOCUMENT_TIMESTAMP;
		break;
	case 'p':
		if (sslikely(strcmp(path, "prefix") == 0))
			return SE_DOCUMENT_PREFIX;
		break;
	case 'r':
		if (sslikely(strcmp(path, "raw") == 0))
			return SE_DOCUMENT_RAW;
		if (sslikely(strcmp(path, "reuse") == 0))
			return SE_DOCUMENT_REUSE;
		break;
	case 'f':
		if (sslikely(strcmp(path, "flags") == 0))
			return SE_DOCUMENT_FLAGS;
		break;
	case 'c':
		if (sslikely(strcmp(path, "cache_only") == 0))
			return SE_DOCUMENT_CACHE_ONLY;
		break;
	case 'e':
		if (sslikely(strcmp(path, "event") == 0))
			return SE_DOCUMENT_EVENT;
		break;
	}
	return SE_DOCUMENT_FIELD;
}

static inline int
se_document_create(struct sedocument *o)
{
	struct sedb *db = (struct sedb*)o->o.parent;
	struct phia_env *e = se_of(&db->o);

	assert(o->v.v == NULL);

	/* reuse document */
	uint32_t timestamp = UINT32_MAX;
	if (db->scheme->expire > 0) {
		if (ssunlikely(o->timestamp > 0))
			timestamp = o->timestamp;
		else
			timestamp = time(NULL);
	}

	/* create document from raw data */
	struct svv *v;
	if (o->raw) {
		v = sv_vbuildraw(db->r, o->raw, o->rawsize, timestamp);
		if (ssunlikely(v == NULL))
			return sr_oom(&e->error);
		sv_init(&o->v, &sv_vif, v, NULL);
		return 0;
	}

	if (o->prefix) {
		if (db->scheme->scheme.keys[0]->type != SS_STRING)
			return sr_error(&e->error, "%s", "prefix search is only "
			                "supported for a string key");

		void *copy = ss_malloc(&e->a, o->prefixsize);
		if (ssunlikely(copy == NULL))
			return sr_oom(&e->error);
		memcpy(copy, o->prefix, o->prefixsize);
		o->prefixcopy = copy;

		if (o->fields_count_keys == 0 && o->prefix)
		{
			memset(o->fields, 0, sizeof(o->fields));
			o->fields[0].pointer = o->prefix;
			o->fields[0].size = o->prefixsize;
			sf_limitset(&e->limit, &db->scheme->scheme, o->fields, SS_GTE);
			goto allocate;
		}
	}

	/* create document using current format, supplied
	 * key-chain and value */
	if (ssunlikely(o->fields_count_keys != db->scheme->scheme.keys_count))
	{
		/* set unspecified min/max keys, depending on
		 * iteration order */
		sf_limitset(&e->limit, &db->scheme->scheme,
		            o->fields, o->order);
		o->fields_count = db->scheme->scheme.fields_count;
		o->fields_count_keys = db->scheme->scheme.keys_count;
	}

allocate:
	v = sv_vbuild(db->r, o->fields, timestamp);
	if (ssunlikely(v == NULL))
		return sr_oom(&e->error);
	sv_init(&o->v, &sv_vif, v, NULL);
	return 0;
}

static int
se_document_open(struct so *o)
{
	struct sedocument *v = se_cast(o, struct sedocument*, SEDOCUMENT);
	if (sslikely(v->created)) {
		assert(v->v.v != NULL);
		return 0;
	}
	int rc = se_document_create(v);
	if (ssunlikely(rc == -1))
		return -1;
	v->created = 1;
	return 0;
}

static void
se_document_free(struct so *o)
{
	struct phia_env *e = se_of(o);
	ss_free(&e->a, o);
}

static int
se_document_destroy(struct so *o)
{
	struct sedocument *v = se_cast(o, struct sedocument*, SEDOCUMENT);
	struct phia_env *e = se_of(o);
	if (v->v.v)
		si_gcv(&e->r, v->v.v);
	v->v.v = NULL;
	if (v->prefixcopy)
		ss_free(&e->a, v->prefixcopy);
	v->prefixcopy = NULL;
	v->prefix = NULL;
	so_free(&v->o);
	return 0;
}

static struct sfv*
se_document_setfield(struct sedocument *v, const char *path, void *pointer, int size)
{
	struct phia_env *e = se_of(&v->o);
	struct sedb *db = (struct sedb*)v->o.parent;
	struct sffield *field = sf_schemefind(&db->scheme->scheme, (char*)path);
	if (ssunlikely(field == NULL))
		return NULL;
	assert(field->position < (int)(sizeof(v->fields) / sizeof(struct sfv)));
	struct sfv *fv = &v->fields[field->position];
	if (size == 0)
		size = strlen(pointer);
	int fieldsize_max;
	if (field->key) {
		fieldsize_max = 1024;
	} else {
		fieldsize_max = 2 * 1024 * 1024;
	}
	if (ssunlikely(size > fieldsize_max)) {
		sr_error(&e->error, "field '%s' is too big (%d limit)",
		         pointer, fieldsize_max);
		return NULL;
	}
	if (fv->pointer == NULL) {
		v->fields_count++;
		if (field->key)
			v->fields_count_keys++;
	}
	fv->pointer = pointer;
	fv->size = size;
	sr_statkey(&e->stat, size);
	return fv;
}

static int
se_document_setstring(struct so *o, const char *path, void *pointer, int size)
{
	struct sedocument *v = se_cast(o, struct sedocument*, SEDOCUMENT);
	struct phia_env *e = se_of(o);
	if (ssunlikely(v->v.v))
		return sr_error(&e->error, "%s", "document is read-only");
	switch (se_document_opt(path)) {
	case SE_DOCUMENT_FIELD: {
		struct sfv *fv = se_document_setfield(v, path, pointer, size);
		if (ssunlikely(fv == NULL))
			return -1;
		break;
	}
	case SE_DOCUMENT_ORDER:
		if (size == 0)
			size = strlen(pointer);
		enum ssorder cmp = ss_orderof(pointer, size);
		if (ssunlikely(cmp == SS_STOP)) {
			sr_error(&e->error, "%s", "bad order name");
			return -1;
		}
		v->order = cmp;
		v->orderset = 1;
		break;
	case SE_DOCUMENT_PREFIX:
		v->prefix = pointer;
		v->prefixsize = size;
		break;
	case SE_DOCUMENT_LOG:
		v->log = pointer;
		break;
	case SE_DOCUMENT_RAW:
		v->raw = pointer;
		v->rawsize = size;
		break;
	default:
		return -1;
	}
	return 0;
}

static void*
se_document_getstring(struct so *o, const char *path, int *size)
{
	struct sedocument *v = se_cast(o, struct sedocument*, SEDOCUMENT);
	switch (se_document_opt(path)) {
	case SE_DOCUMENT_FIELD: {
		/* match field */
		struct sedb *db = (struct sedb*)o->parent;
		struct sffield *field = sf_schemefind(&db->scheme->scheme, (char*)path);
		if (ssunlikely(field == NULL))
			return NULL;
		/* database result document */
		if (v->v.v)
			return sv_field(&v->v, db->r, field->position, (uint32_t*)size);
		/* database field document */
		assert(field->position < (int)(sizeof(v->fields) / sizeof(struct sfv)));
		struct sfv *fv = &v->fields[field->position];
		if (fv->pointer == NULL)
			return NULL;
		if (size)
			*size = fv->size;
		return fv->pointer;
	}
	case SE_DOCUMENT_PREFIX: {
		if (v->prefix == NULL)
			return NULL;
		if (size)
			*size = v->prefixsize;
		return v->prefix;
	}
	case SE_DOCUMENT_ORDER: {
		char *order = ss_ordername(v->order);
		if (size)
			*size = strlen(order) + 1;
		return order;
	}
	case SE_DOCUMENT_EVENT: {
		char *type = "none";
		if (v->event == 1)
			type = "on_backup";
		if (size)
			*size = strlen(type);
		return type;
	}
	case SE_DOCUMENT_RAW:
		if (v->raw) {
			if (size)
				*size = v->rawsize;
			return v->raw;
		}
		if (v->v.v == NULL)
			return NULL;
		if (size)
			*size = sv_size(&v->v);
		return sv_pointer(&v->v);
	}
	return NULL;
}

static int
se_document_setint(struct so *o, const char *path, int64_t num)
{
	struct sedocument *v = se_cast(o, struct sedocument*, SEDOCUMENT);
	switch (se_document_opt(path)) {
	case SE_DOCUMENT_TIMESTAMP:
		v->timestamp = num;
		break;
	case SE_DOCUMENT_CACHE_ONLY:
		v->cache_only = num;
		break;
	case SE_DOCUMENT_OLDEST_ONLY:
		v->oldest_only = num;
		break;
	default:
		return -1;
	}
	return 0;
}

static int64_t
se_document_getint(struct so *o, const char *path)
{
	struct sedocument *v = se_cast(o, struct sedocument*, SEDOCUMENT);
	switch (se_document_opt(path)) {
	case SE_DOCUMENT_LSN: {
		uint64_t lsn = -1;
		if (v->v.v)
			lsn = ((struct svv*)(v->v.v))->lsn;
		return lsn;
	}
	case SE_DOCUMENT_EVENT:
		return v->event;
	case SE_DOCUMENT_CACHE_ONLY:
		return v->cache_only;
	case SE_DOCUMENT_FLAGS: {
		uint64_t flags = -1;
		if (v->v.v)
			flags = ((struct svv*)(v->v.v))->flags;
		return flags;
	}
	}
	return -1;
}

static int
se_document_setobject(struct so *o, const char *path, void *object)
{
	struct sedocument *v = se_cast(o, struct sedocument*, SEDOCUMENT);
	switch (se_document_opt(path)) {
	case SE_DOCUMENT_REUSE: {
		struct phia_env *e = se_of(o);
		struct sedocument *reuse = se_cast(object, struct sedocument*, SEDOCUMENT);
		if (ssunlikely(v->created))
			return sr_error(&e->error, "%s", "document is read-only");
		assert(v->v.v == NULL);
		if (ssunlikely(object == o->parent))
			return sr_error(&e->error, "%s", "bad document operation");
		if (ssunlikely(! reuse->created))
			return sr_error(&e->error, "%s", "bad document operation");
		sv_init(&v->v, &sv_vif, reuse->v.v, NULL);
		sv_vref(v->v.v);
		v->created = 1;
		break;
	}
	default:
		return -1;
	}
	return 0;
}

static struct soif sedocumentif =
{
	.open         = se_document_open,
	.close        = NULL,
	.destroy      = se_document_destroy,
	.free         = se_document_free,
	.document     = NULL,
	.drop         = NULL,
	.setstring    = se_document_setstring,
	.setint       = se_document_setint,
	.setobject    = se_document_setobject,
	.getobject    = NULL,
	.getstring    = se_document_getstring,
	.getint       = se_document_getint,
	.set          = NULL,
	.upsert       = NULL,
	.del          = NULL,
	.get          = NULL,
	.begin        = NULL,
	.commit       = NULL,
	.cursor       = NULL,
};

static struct so *
se_document_new(struct phia_env *e, struct so *parent, struct sv *vp)
{
	struct sedocument *v;
	v = ss_malloc(&e->a, sizeof(struct sedocument));
	if (ssunlikely(v == NULL)) {
		sr_oom(&e->error);
		return NULL;
	}
	memset(v, 0, sizeof(*v));
	so_init(&v->o, &se_o[SEDOCUMENT], &sedocumentif, parent, &e->o);
	v->order = SS_EQ;
	if (vp) {
		v->v = *vp;
	}
	return &v->o;
}

static struct sotype se_o[] =
{
	{ 0L,          "undefined"         },
	{ 0x9BA14568L, "destroyed"         },
	{ 0x06154834L, "env"               },
	{ 0x20490B34L, "env_conf"          },
	{ 0x6AB65429L, "env_conf_cursor"   },
	{ 0x00FCDE12L, "env_conf_kv"       },
	{ 0x64519F00L, "req"               },
	{ 0x2FABCDE2L, "document"          },
	{ 0x34591111L, "database"          },
	{ 0x63102654L, "database_cursor"   },
	{ 0x13491FABL, "transaction"       },
	{ 0x22FA0348L, "view"              },
	{ 0x45ABCDFAL, "cursor"            }
};

static inline void
se_recoverf(struct phia_env *e, char *fmt, ...)
{
	if (e->conf.on_recover.function == NULL)
		return;
	char trace[1024];
	va_list args;
	va_start(args, fmt);
	vsnprintf(trace, sizeof(trace), fmt, args);
	va_end(args);
	e->conf.on_recover.function(trace, e->conf.on_recover.arg);
}

static int se_recoverbegin(struct sedb *db)
{
	/* open and recover repository */
	sr_statusset(&db->index->status, SR_RECOVER);
	struct phia_env *e = se_of(&db->o);
	/* do not allow to recover existing databases
	 * during online (only create), since logpool
	 * reply is required. */
	if (sr_status(&e->status) == SR_ONLINE)
		if (e->conf.recover != SE_RECOVER_NP)
			db->scheme->path_fail_on_exists = 1;
	se_recoverf(e, "loading database '%s'", db->scheme->path);
	int rc = si_open(db->index);
	if (ssunlikely(rc == -1))
		goto error;
	db->created = rc;
	return 0;
error:
	sr_statusset(&db->index->status, SR_MALFUNCTION);
	return -1;
}

static int se_recoverend(struct sedb *db)
{
	int status = sr_status(&db->index->status);
	if (ssunlikely(status == SR_DROP_PENDING))
		return 0;
	sr_statusset(&db->index->status, SR_ONLINE);
	return 0;
}

static int
se_recoverlog(struct phia_env *e, struct sl *log)
{
	struct so *tx = NULL;
	struct sedb *db = NULL;
	struct ssiter i;
	ss_iterinit(sl_iter, &i);
	int processed = 0;
	int rc = ss_iteropen(sl_iter, &i, &e->r, &log->file, 1);
	if (ssunlikely(rc == -1))
		return -1;
	for (;;)
	{
		struct sv *v = ss_iteratorof(&i);
		if (ssunlikely(v == NULL))
			break;

		/* reply transaction */
		uint64_t lsn = sv_lsn(v);
		tx = so_begin(&e->o);
		if (ssunlikely(tx == NULL))
			goto error;

		while (ss_iteratorhas(&i)) {
			v = ss_iteratorof(&i);
			assert(sv_lsn(v) == lsn);
			/* match a database */
			uint32_t timestamp = sl_vtimestamp(v);
			uint32_t dsn = sl_vdsn(v);
			if (db == NULL || db->scheme->id != dsn)
				db = (struct sedb*)se_dbmatch_id(e, dsn);
			if (ssunlikely(db == NULL)) {
				sr_malfunction(&e->error, "database id %" PRIu32
				               " is not declared", dsn);
				goto rlb;
			}
			struct so *o = so_document(&db->o);
			if (ssunlikely(o == NULL))
				goto rlb;
			so_setstring(o, "raw", sv_pointer(v), sv_size(v));
			so_setstring(o, "log", log, 0);
			so_setint(o, "timestamp", timestamp);

			int flags = sv_flags(v);
			if (flags == SVDELETE) {
				rc = so_delete(tx, o);
			} else
			if (flags == SVUPSERT) {
				rc = so_upsert(tx, o);
			} else {
				assert(flags == 0);
				rc = so_set(tx, o);
			}
			if (ssunlikely(rc == -1))
				goto rlb;
			ss_gcmark(&log->gc, 1);
			processed++;
			if ((processed % 100000) == 0)
				se_recoverf(e, " %.1fM processed", processed / 1000000.0);
			ss_iteratornext(&i);
		}
		if (ssunlikely(sl_iter_error(&i)))
			goto rlb;

		so_setint(tx, "lsn", lsn);
		rc = so_commit(tx);
		if (ssunlikely(rc != 0))
			goto error;
		rc = sl_iter_continue(&i);
		if (ssunlikely(rc == -1))
			goto error;
		if (rc == 0)
			break;
	}
	ss_iteratorclose(&i);
	return 0;
rlb:
	so_destroy(tx);
error:
	ss_iteratorclose(&i);
	return -1;
}

static inline int
se_recoverlogpool(struct phia_env *e)
{
	struct sl *log;
	rlist_foreach_entry(log, &e->lp.list, link) {
		char *path = ss_pathof(&log->file.path);
		se_recoverf(e, "loading journal '%s'", path);
		int rc = se_recoverlog(e, log);
		if (ssunlikely(rc == -1))
			return -1;
		ss_gccomplete(&log->gc);
	}
	return 0;
}

static int se_recover(struct phia_env *e)
{
	struct slconf *lc = &e->lpconf;
	lc->enable         = e->conf.log_enable;
	lc->path           = e->conf.log_path;
	lc->rotatewm       = e->conf.log_rotate_wm;
	lc->sync_on_rotate = e->conf.log_rotate_sync;
	lc->sync_on_write  = e->conf.log_sync;
	int rc = sl_poolopen(&e->lp, lc);
	if (ssunlikely(rc == -1))
		return -1;
	if (e->conf.recover == SE_RECOVER_2P)
		return 0;
	/* recover log files */
	rc = se_recoverlogpool(e);
	if (ssunlikely(rc == -1))
		goto error;
	rc = sl_poolrotate(&e->lp);
	if (ssunlikely(rc == -1))
		goto error;
	return 0;
error:
	sr_statusset(&e->status, SR_MALFUNCTION);
	return -1;
}

static int se_recover_repository(struct phia_env *e)
{
	struct syconf *rc = &e->repconf;
	rc->path        = e->conf.path;
	rc->path_create = e->conf.path_create;
	rc->path_backup = e->conf.backup_path;
	rc->sync = 0;
	se_recoverf(e, "recovering repository '%s'", e->conf.path);
	return sy_open(&e->rep, &e->r, rc);
}

static inline int
se_txwrite(struct setx *t, struct sedocument *o, uint8_t flags)
{
	struct phia_env *e = se_of(&t->o);
	struct sedb *db = se_cast(o->o.parent, struct sedb*, SEDB);

	int auto_close = !o->created;

	/* validate req */
	if (ssunlikely(t->t.state == SXPREPARE)) {
		sr_error(&e->error, "%s", "transaction is in 'prepare' state (read-only)");
		goto error;
	}

	/* validate database status */
	int status = sr_status(&db->index->status);
	switch (status) {
	case SR_SHUTDOWN_PENDING:
	case SR_DROP_PENDING:
		if (ssunlikely(! se_dbvisible(db, t->t.id))) {
			sr_error(&e->error, "%s", "database is invisible for the transaction");
			goto error;
		}
		break;
	case SR_RECOVER:
	case SR_ONLINE: break;
	default: goto error;
	}

	/* create document */
	int rc = so_open(&o->o);
	if (ssunlikely(rc == -1))
		goto error;
	rc = se_document_validate(o, &db->o, flags);
	if (ssunlikely(rc == -1))
		goto error;

	struct svv *v = o->v.v;
	sv_vref(v);
	v->log = o->log;

	/* destroy document object */
	int size = sv_vsize(v);
	if (auto_close) {
		ss_quota(&e->quota, SS_QADD, size);
		so_destroy(&o->o);
	}

	/* concurrent index only */
	rc = sx_set(&t->t, &db->coindex, v);
	if (ssunlikely(rc == -1)) {
		if (auto_close)
			ss_quota(&e->quota, SS_QREMOVE, size);
		return -1;
	}
	return 0;
error:
	if (auto_close)
		so_destroy(&o->o);
	return -1;
}

static int
se_txset(struct so *o, struct so *v)
{
	struct setx *t = se_cast(o, struct setx*, SETX);
	struct sedocument *key = se_cast(v, struct sedocument*, SEDOCUMENT);
	return se_txwrite(t, key, 0);
}

static int
se_txupsert(struct so *o, struct so *v)
{
	struct setx *t = se_cast(o, struct setx*, SETX);
	struct sedocument *key = se_cast(v, struct sedocument*, SEDOCUMENT);
	struct phia_env *e = se_of(&t->o);
	struct sedb *db = se_cast(v->parent, struct sedb*, SEDB);
	if (! sf_upserthas(&db->scheme->fmt_upsert))
		return sr_error(&e->error, "%s", "upsert callback is not set");
	return se_txwrite(t, key, SVUPSERT);
}

static int
se_txdelete(struct so *o, struct so *v)
{
	struct setx *t = se_cast(o, struct setx*, SETX);
	struct sedocument *key = se_cast(v, struct sedocument*, SEDOCUMENT);
	return se_txwrite(t, key, SVDELETE);
}

static void *
se_txget(struct so *o, struct so *v)
{
	struct setx *t = se_cast(o, struct setx*, SETX);
	struct sedocument *key = se_cast(v, struct sedocument*, SEDOCUMENT);
	struct phia_env *e = se_of(&t->o);
	struct sedb *db = se_cast(key->o.parent, struct sedb*, SEDB);
	/* validate database */
	int status = sr_status(&db->index->status);
	switch (status) {
	case SR_SHUTDOWN_PENDING:
	case SR_DROP_PENDING:
		if (ssunlikely(! se_dbvisible(db, t->t.id))) {
			sr_error(&e->error, "%s", "database is invisible for the transaction");
			goto error;
		}
		break;
	case SR_ONLINE:
	case SR_RECOVER:
		break;
	default: goto error;
	}
	return se_dbread(db, key, &t->t, 1, NULL);
error:
	so_destroy(&key->o);
	return NULL;
}

static inline void
se_txfree(struct so *o)
{
	struct phia_env *e = se_of(o);
	struct setx *t = (struct setx*)o;
	sv_logfree(&t->log, &e->a);
	ss_free(&e->a, o);
}

static inline void
se_txend(struct setx *t, int rlb, int conflict)
{
	struct phia_env *e = se_of(&t->o);
	uint32_t count = sv_logcount(&t->log);
	sx_gc(&t->t);
	sv_logreset(&t->log);
	sr_stattx(&e->stat, t->start, count, rlb, conflict);
	se_dbunbind(e, t->t.id);
	so_free(&t->o);
}

static int
se_txrollback(struct so *o)
{
	struct setx *t = se_cast(o, struct setx*, SETX);
	sx_rollback(&t->t);
	se_txend(t, 1, 0);
	return 0;
}

static int
se_txprepare(struct sx *x, struct sv *v, struct so *o, void *ptr)
{
	struct sicache *cache = ptr;
	struct sedb *db = (struct sedb*)o;
	struct phia_env *e = se_of(&db->o);

	struct scread q;
	sc_readopen(&q, db->r, &db->o, db->index);
	struct screadarg *arg = &q.arg;
	arg->v             = *v;
	arg->vup.v         = NULL;
	arg->prefix        = NULL;
	arg->prefixsize    = 0;
	arg->cache         = cache;
	arg->cachegc       = 0;
	arg->order         = SS_EQ;
	arg->has           = 1;
	arg->upsert        = 0;
	arg->upsert_eq     = 0;
	arg->cache_only    = 0;
	arg->oldest_only   = 0;
	arg->vlsn          = x->vlsn;
	arg->vlsn_generate = 0;
	int rc = sc_read(&q, &e->scheduler);
	sc_readclose(&q);
	return rc;
}

static int
se_txcommit(struct so *o)
{
	struct setx *t = se_cast(o, struct setx*, SETX);
	struct phia_env *e = se_of(o);
	int status = sr_status(&e->status);
	if (ssunlikely(! sr_statusactive_is(status)))
		return -1;
	int recover = (status == SR_RECOVER);

	/* prepare transaction */
	if (t->t.state == SXREADY || t->t.state == SXLOCK)
	{
		struct sicache *cache = NULL;
		sxpreparef prepare = NULL;
		if (! recover) {
			prepare = se_txprepare;
			cache = si_cachepool_pop(&e->cachepool);
			if (ssunlikely(cache == NULL))
				return sr_oom(&e->error);
		}
		enum sxstate s = sx_prepare(&t->t, prepare, cache);
		if (cache)
			si_cachepool_push(cache);
		if (s == SXLOCK) {
			sr_stattx_lock(&e->stat);
			return 2;
		}
		if (s == SXROLLBACK) {
			sx_rollback(&t->t);
			se_txend(t, 0, 1);
			return 1;
		}
		assert(s == SXPREPARE);

		sx_commit(&t->t);

		if (t->half_commit) {
			/* Half commit mode.
			 *
			 * A half committed transaction is no longer
			 * being part of concurrent index, but still can be
			 * commited or rolled back.
			 * Yet, it is important to maintain external
			 * serial commit order.
			*/
			return 0;
		}
	}
	assert(t->t.state == SXCOMMIT);

	/* do wal write and backend commit */
	if (ssunlikely(recover))
		recover = (e->conf.recover == 3) ? 2: 1;
	int rc;
	rc = sc_write(&e->scheduler, &t->log, t->lsn, recover);
	if (ssunlikely(rc == -1))
		sx_rollback(&t->t);

	se_txend(t, 0, 0);
	return rc;
}

static int
se_txset_int(struct so *o, const char *path, int64_t v)
{
	struct setx *t = se_cast(o, struct setx*, SETX);
	if (strcmp(path, "lsn") == 0) {
		t->lsn = v;
		return 0;
	} else
	if (strcmp(path, "half_commit") == 0) {
		t->half_commit = v;
		return 0;
	}
	return -1;
}

static int64_t
se_txget_int(struct so *o, const char *path)
{
	struct setx *t = se_cast(o, struct setx*, SETX);
	if (strcmp(path, "deadlock") == 0)
		return sx_deadlock(&t->t);
	return -1;
}

static struct soif setxif =
{
	.open         = NULL,
	.close        = NULL,
	.destroy      = se_txrollback,
	.free         = se_txfree,
	.document     = NULL,
	.drop         = NULL,
	.setstring    = NULL,
	.setint       = se_txset_int,
	.setobject    = NULL,
	.getobject    = NULL,
	.getstring    = NULL,
	.getint       = se_txget_int,
	.set          = se_txset,
	.upsert       = se_txupsert,
	.del          = se_txdelete,
	.get          = se_txget,
	.begin        = NULL,
	.commit       = se_txcommit,
	.cursor       = NULL
};

static struct so *
se_txnew(struct phia_env *e)
{
	struct setx *t;
	t = ss_malloc(&e->a, sizeof(struct setx));
	if (ssunlikely(t == NULL)) {
		sr_oom(&e->error);
		return NULL;
	}
	so_init(&t->o, &se_o[SETX], &setxif, &e->o, &e->o);
	sv_loginit(&t->log);
	sx_init(&e->xm, &t->t, &t->log);
	t->start = clock_monotonic64();
	t->lsn = 0;
	t->half_commit = 0;
	sx_begin(&e->xm, &t->t, SXRW, &t->log, UINT64_MAX);
	se_dbbind(e);
	return &t->o;
}

static void
se_viewfree(struct so *o)
{
	struct phia_env *e = se_of(o);
	struct seview *s = (struct seview*)o;
	ss_buffree(&s->name, &e->a);
	ss_free(&e->a, o);
}

static int
se_viewdestroy(struct so *o)
{
	struct seview *s = se_cast(o, struct seview*, SEVIEW);
	struct phia_env *e = se_of(o);
	uint32_t id = s->t.id;
	se_dbunbind(e, id);
	if (sslikely(! s->db_view_only))
		sx_rollback(&s->t);
	ss_bufreset(&s->name);
	rlist_del(&s->link);
	so_free(&s->o);
	return 0;
}

static void*
se_viewget(struct so *o, struct so *key)
{
	struct seview *s = se_cast(o, struct seview*, SEVIEW);
	struct phia_env *e = se_of(o);
	struct sedocument *v = se_cast(key, struct sedocument*, SEDOCUMENT);
	struct sedb *db = se_cast(key->parent, struct sedb*, SEDB);
	if (s->db_view_only) {
		sr_error(&e->error, "view '%s' is in db-cursor-only mode", s->name);
		return NULL;
	}
	return se_dbread(db, v, &s->t, 0, NULL);
}

static void*
se_viewcursor(struct so *o)
{
	struct seview *s = se_cast(o, struct seview*, SEVIEW);
	struct phia_env *e = se_of(o);
	if (s->db_view_only) {
		sr_error(&e->error, "view '%s' is in db-view-only mode", s->name);
		return NULL;
	}
	return se_cursornew(e, s->vlsn);
}

static void *se_viewget_object(struct so *o, const char *path)
{
	struct seview *s = se_cast(o, struct seview*, SEVIEW);
	struct phia_env *e = se_of(o);
	if (strcmp(path, "db") == 0)
		return se_viewdb_new(e, s->t.id);
	return NULL;
}

static int
se_viewset_int(struct so *o, const char *path, int64_t v ssunused)
{
	struct seview *s = se_cast(o, struct seview*, SEVIEW);
	if (strcmp(path, "db-view-only") == 0) {
		if (s->db_view_only)
			return -1;
		sx_rollback(&s->t);
		s->db_view_only = 1;
		return 0;
	}
	return -1;
}

static struct soif seviewif =
{
	.open         = NULL,
	.close        = NULL,
	.destroy      = se_viewdestroy,
	.free         = se_viewfree,
	.document     = NULL,
	.drop         = NULL,
	.setstring    = NULL,
	.setint       = se_viewset_int,
	.setobject    = NULL,
	.getobject    = se_viewget_object,
	.getstring    = NULL,
	.getint       = NULL,
	.set          = NULL,
	.upsert       = NULL,
	.del          = NULL,
	.get          = se_viewget,
	.begin        = NULL,
	.commit       = NULL,
	.cursor       = se_viewcursor
};

static struct so *
se_viewnew(struct phia_env *e, uint64_t vlsn, char *name, int size)
{
	struct seview *s;
	rlist_foreach_entry(s, &e->view, link) {
		if (ssunlikely(strcmp(s->name.s, name) == 0)) {
			sr_error(&e->error, "view '%s' already exists", name);
			return NULL;
		}
	}
	s = ss_malloc(&e->a, sizeof(struct seview));
	if (ssunlikely(s == NULL)) {
		sr_oom(&e->error);
		return NULL;
	}
	so_init(&s->o, &se_o[SEVIEW], &seviewif, &e->o, &e->o);
	s->vlsn = vlsn;
	ss_bufinit(&s->name);
	int rc;
	if (size == 0)
		size = strlen(name);
	rc = ss_bufensure(&s->name, &e->a, size + 1);
	if (ssunlikely(rc == -1)) {
		so_free(&s->o);
		sr_oom(&e->error);
		return NULL;
	}
	memcpy(s->name.s, name, size);
	s->name.s[size] = 0;
	ss_bufadvance(&s->name, size + 1);
	sv_loginit(&s->log);
	sx_begin(&e->xm, &s->t, SXRO, &s->log, vlsn);
	s->db_view_only = 0;
	se_dbbind(e);
	rlist_add(&e->view, &s->link);
	return &s->o;
}

static int se_viewupdate(struct seview *s)
{
	struct phia_env *e = se_of(&s->o);
	uint32_t id = s->t.id;
	if (! s->db_view_only) {
		sx_rollback(&s->t);
		sx_begin(&e->xm, &s->t, SXRO, &s->log, s->vlsn);
	}
	s->t.id = id;
	return 0;
}

static void
se_viewdb_free(struct so *o)
{
	struct seviewdb *c = (struct seviewdb*)o;
	struct phia_env *e = se_of(&c->o);
	ss_buffree(&c->list, &e->a);
	ss_free(&e->a, c);
}

static int
se_viewdb_destroy(struct so *o)
{
	struct seviewdb *c = se_cast(o, struct seviewdb*, SEDBCURSOR);
	ss_bufreset(&c->list);
	so_free(&c->o);
	return 0;
}

static void*
se_viewdb_get(struct so *o, struct so *v ssunused)
{
	struct seviewdb *c = se_cast(o, struct seviewdb*, SEDBCURSOR);
	if (c->ready) {
		c->ready = 0;
		return c->v;
	}
	if (ssunlikely(c->pos == NULL))
		return NULL;
	c->pos += sizeof(struct sedb**);
	if (c->pos >= c->list.p) {
		c->pos = NULL;
		c->v = NULL;
		return NULL;
	}
	c->v = *(struct sedb**)c->pos;
	return c->v;
}

static struct soif seviewdbif =
{
	.open         = NULL,
	.close        = NULL,
	.destroy      = se_viewdb_destroy,
	.free         = se_viewdb_free,
	.document     = NULL,
	.drop         = NULL,
	.setstring    = NULL,
	.setint       = NULL,
	.setobject    = NULL,
	.getobject    = NULL,
	.getstring    = NULL,
	.getint       = NULL,
	.set          = NULL,
	.upsert       = NULL,
	.del          = NULL,
	.get          = se_viewdb_get,
	.begin        = NULL,
	.commit       = NULL,
	.cursor       = NULL,
};

static inline int
se_viewdb_open(struct seviewdb *c)
{
	struct phia_env *e = se_of(&c->o);
	int rc;
	struct sedb *db;
	rlist_foreach_entry(db, &e->db, link) {
		int status = sr_status(&db->index->status);
		if (status != SR_ONLINE)
			continue;
		if (se_dbvisible(db, c->txn_id)) {
			rc = ss_bufadd(&c->list, &e->a, &db, sizeof(db));
			if (ssunlikely(rc == -1))
				return -1;
		}
	}
	if (ss_bufsize(&c->list) == 0)
		return 0;
	c->ready = 1;
	c->pos = c->list.s;
	c->v = *(struct sedb**)c->list.s;
	return 0;
}

static struct so *se_viewdb_new(struct phia_env *e, uint64_t txn_id)
{
	struct seviewdb *c;
	c = ss_malloc(&e->a, sizeof(struct seviewdb));
	if (ssunlikely(c == NULL)) {
		sr_oom(&e->error);
		return NULL;
	}
	so_init(&c->o, &se_o[SEDBCURSOR], &seviewdbif,
	        &e->o, &e->o);
	c->txn_id = txn_id;
	c->v      = NULL;
	c->pos    = NULL;
	c->ready  = 0;
	ss_bufinit(&c->list);
	int rc = se_viewdb_open(c);
	if (ssunlikely(rc == -1)) {
		so_free(&c->o);
		sr_oom(&e->error);
		return NULL;
	}
	return &c->o;
}

static inline void
sp_unsupported(struct so *o, const char *method)
{
	fprintf(stderr, "\n%s(%s): unsupported operation\n",
	        (char*)method, o->type->name);
	abort();
}

static inline struct so*
sp_cast(void *ptr, const char *method)
{
	struct so *o = se_cast_validate(ptr);
	if (ssunlikely(o == NULL)) {
		fprintf(stderr, "\n%s(%p): bad object\n", method, ptr);
		abort();
	}
	return o;
}

struct phia_env *phia_env(void)
{
	struct phia_env *e = malloc(sizeof(*e));
	if (ssunlikely(e == NULL))
		return NULL;
	memset(e, 0, sizeof(*e));
	so_init(&e->o, &se_o[SE], &seif, &e->o, &e->o /* self */);
	sr_statusinit(&e->status);
	sr_statusset(&e->status, SR_OFFLINE);
	ss_vfsinit(&e->vfs, &ss_stdvfs);
	ss_aopen(&e->a, &ss_stda);
	ss_aopen(&e->a_ref, &ss_stda);
	int rc;
	rc = se_confinit(&e->conf, &e->o);
	if (ssunlikely(rc == -1))
		goto error;
	rlist_create(&e->db);
	rlist_create(&e->view);
	tt_pthread_mutex_init(&e->apilock, NULL);
	ss_quotainit(&e->quota);
	sr_seqinit(&e->seq);
	sr_errorinit(&e->error);
	sr_statinit(&e->stat);
	sf_limitinit(&e->limit, &e->a);
	sr_init(&e->r, &e->status, &e->error, &e->a, &e->a_ref, &e->vfs, &e->quota,
	        &e->conf.zones, &e->seq, SF_RAW, NULL,
	        NULL, &e->ei, &e->stat);
	sy_init(&e->rep);
	sl_poolinit(&e->lp, &e->r);
	sx_managerinit(&e->xm, &e->r);
	si_cachepool_init(&e->cachepool, &e->r);
	sc_init(&e->scheduler, &e->r, &e->conf.on_event, &e->lp);
	return e;
error:
	sr_statusfree(&e->status);
	free(e);
	return NULL;
}

void *phia_document(void *ptr)
{
	struct so *o = sp_cast(ptr, __func__);
	if (ssunlikely(o->i->document == NULL)) {
		sp_unsupported(o, __func__);
		return NULL;
	}
	struct so *e = o->env;
	se_apilock(e);
	void *h = o->i->document(o);
	se_apiunlock(e);
	return h;
}

int phia_open(void *ptr)
{
	struct so *o = sp_cast(ptr, __func__);
	if (ssunlikely(o->i->open == NULL)) {
		sp_unsupported(o, __func__);
		return -1;
	}
	struct so *e = o->env;
	se_apilock(e);
	int rc = o->i->open(o);
	se_apiunlock(e);
	return rc;
}

int phia_close(void *ptr)
{
	struct so *o = sp_cast(ptr, __func__);
	if (ssunlikely(o->i->close == NULL)) {
		sp_unsupported(o, __func__);
		return -1;
	}
	struct so *e = o->env;
	se_apilock(e);
	int rc = o->i->close(o);
	se_apiunlock(e);
	return rc;
}

int phia_drop(void *ptr)
{
	struct so *o = sp_cast(ptr, __func__);
	if (ssunlikely(o->i->drop == NULL)) {
		sp_unsupported(o, __func__);
		return -1;
	}
	struct so *e = o->env;
	se_apilock(e);
	int rc = o->i->drop(o);
	se_apiunlock(e);
	return rc;
}

int phia_destroy(void *ptr)
{
	struct so *o = sp_cast(ptr, __func__);
	if (ssunlikely(o->i->destroy == NULL)) {
		sp_unsupported(o, __func__);
		return -1;
	}
	struct so *e = o->env;
	int rc;
	if (ssunlikely(e == o)) {
		rc = o->i->destroy(o);
		return rc;
	}
	se_apilock(e);
	rc = o->i->destroy(o);
	se_apiunlock(e);
	return rc;
}

int phia_service(struct phia_env *env)
{
	return sc_ctl_call(&env->scheduler, sx_vlsn(&env->xm));
}

int phia_setstring(void *ptr, const char *path, const void *pointer, int size)
{
	struct so *o = sp_cast(ptr, __func__);
	if (ssunlikely(o->i->setstring == NULL)) {
		sp_unsupported(o, __func__);
		return -1;
	}
	struct so *e = o->env;
	se_apilock(e);
	int rc = o->i->setstring(o, path, (void*)pointer, size);
	se_apiunlock(e);
	return rc;
}

int phia_setint(void *ptr, const char *path, int64_t v)
{
	struct so *o = sp_cast(ptr, __func__);
	if (ssunlikely(o->i->setint == NULL)) {
		sp_unsupported(o, __func__);
		return -1;
	}
	struct so *e = o->env;
	se_apilock(e);
	int rc = o->i->setint(o, path, v);
	se_apiunlock(e);
	return rc;
}

int phia_setobject(void *ptr, const char *path, void *v)
{
	struct so *o = sp_cast(ptr, __func__);
	if (ssunlikely(o->i->setobject == NULL)) {
		sp_unsupported(o, __func__);
		return -1;
	}
	struct so *e = o->env;
	se_apilock(e);
	int rc = o->i->setobject(o, path, v);
	se_apiunlock(e);
	return rc;
}

void *phia_getobject(void *ptr, const char *path)
{
	struct so *o = sp_cast(ptr, __func__);
	if (ssunlikely(o->i->getobject == NULL)) {
		sp_unsupported(o, __func__);
		return NULL;
	}
	struct so *e = o->env;
	se_apilock(e);
	void *h = o->i->getobject(o, path);
	se_apiunlock(e);
	return h;
}

void *phia_getstring(void *ptr, const char *path, int *size)
{
	struct so *o = sp_cast(ptr, __func__);
	if (ssunlikely(o->i->getstring == NULL)) {
		sp_unsupported(o, __func__);
		return NULL;
	}
	struct so *e = o->env;
	se_apilock(e);
	void *h = o->i->getstring(o, path, size);
	se_apiunlock(e);
	return h;
}

int64_t phia_getint(void *ptr, const char *path)
{
	struct so *o = sp_cast(ptr, __func__);
	if (ssunlikely(o->i->getint == NULL)) {
		sp_unsupported(o, __func__);
		return -1;
	}
	struct so *e = o->env;
	se_apilock(e);
	int64_t rc = o->i->getint(o, path);
	se_apiunlock(e);
	return rc;
}

int phia_set(void *ptr, void *v)
{
	struct so *o = sp_cast(ptr, __func__);
	if (ssunlikely(o->i->set == NULL)) {
		sp_unsupported(o, __func__);
		return -1;
	}
	struct so *e = o->env;
	se_apilock(e);
	int rc = o->i->set(o, v);
	se_apiunlock(e);
	return rc;
}

int phia_upsert(void *ptr, void *v)
{
	struct so *o = sp_cast(ptr, __func__);
	if (ssunlikely(o->i->upsert == NULL)) {
		sp_unsupported(o, __func__);
		return -1;
	}
	struct so *e = o->env;
	se_apilock(e);
	int rc = o->i->upsert(o, v);
	se_apiunlock(e);
	return rc;
}

int phia_delete(void *ptr, void *v)
{
	struct so *o = sp_cast(ptr, __func__);
	if (ssunlikely(o->i->del == NULL)) {
		sp_unsupported(o, __func__);
		return -1;
	}
	struct so *e = o->env;
	se_apilock(e);
	int rc = o->i->del(o, v);
	se_apiunlock(e);
	return rc;
}

void *phia_get(void *ptr, void *v)
{
	struct so *o = sp_cast(ptr, __func__);
	if (ssunlikely(o->i->get == NULL)) {
		sp_unsupported(o, __func__);
		return NULL;
	}
	struct so *e = o->env;
	se_apilock(e);
	void *h = o->i->get(o, v);
	se_apiunlock(e);
	return h;
}

void *phia_cursor(void *ptr)
{
	struct so *o = sp_cast(ptr, __func__);
	if (ssunlikely(o->i->cursor == NULL)) {
		sp_unsupported(o, __func__);
		return NULL;
	}
	struct so *e = o->env;
	se_apilock(e);
	void *h = o->i->cursor(o);
	se_apiunlock(e);
	return h;
}

void *phia_begin(void *ptr)
{
	struct so *o = sp_cast(ptr, __func__);
	if (ssunlikely(o->i->begin == NULL)) {
		sp_unsupported(o, __func__);
		return NULL;
	}
	struct so *e = o->env;
	se_apilock(e);
	void *h = o->i->begin(o);
	se_apiunlock(e);
	return h;
}

int phia_commit(void *ptr)
{
	struct so *o = sp_cast(ptr, __func__);
	if (ssunlikely(o->i->commit == NULL)) {
		sp_unsupported(o, __func__);
		return -1;
	}
	struct so *e = o->env;
	se_apilock(e);
	int rc = o->i->commit(o);
	se_apiunlock(e);
	return rc;
}
