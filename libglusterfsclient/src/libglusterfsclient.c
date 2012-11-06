/*
  Copyright (c) 2008-2012 Red Hat, Inc. <http://www.redhat.com>
  This file is part of GlusterFS.

  This file is licensed to you under your choice of the GNU Lesser
  General Public License, version 3 or any later version (LGPLv3 or
  later), or the GNU General Public License, version 2 (GPLv2), in all
  cases as published by the Free Software Foundation.
*/

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <errno.h>
#include <libgen.h>
#include <stddef.h>

#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#ifdef GF_SOLARIS_HOST_OS
#include <sys/statfs.h>
#endif
#include <unistd.h>
#include <xlator.h>
#include <timer.h>
#include "defaults.h"
#include <time.h>
#include <poll.h>
#include "transport.h"
#include "event.h"
#include "libglusterfsclient.h"
#include "libglusterfsclient-internals.h"
#include "compat.h"
#include "compat-errno.h"
#ifndef GF_SOLARIS_HOST_OS
#include <sys/vfs.h>
#endif
#include <utime.h>
#include <sys/param.h>
#include <list.h>
#include <stdarg.h>
#include <sys/statvfs.h>
#include "hashfn.h"
#include <sys/select.h>

#define LIBGF_XL_NAME "libglusterfsclient"
#define LIBGLUSTERFS_INODE_TABLE_LRU_LIMIT 1000 //14057
#define LIBGF_SENDFILE_BLOCK_SIZE 4096
#define LIBGF_READDIR_BLOCK     4096
#define libgf_path_absolute(path) ((path)[0] == '/')

static inline xlator_t *
libglusterfs_graph (xlator_t *graph);
int32_t libgf_client_readlink (libglusterfs_client_ctx_t *ctx, loc_t *loc,
                                        char *buf, size_t bufsize);

int
libgf_realpath_loc_fill (libglusterfs_client_ctx_t *ctx, char *link,
                         loc_t *targetloc);
static int first_init = 1;

/* The global list of virtual mount points */
struct {
        struct list_head list;
        int              entries;
}vmplist;


/* Protects the VMP list above. */
pthread_mutex_t vmplock = PTHREAD_MUTEX_INITIALIZER;

/* Ensures only one thread is ever calling glusterfs_mount.
 * Since that function internally calls routines which
 * use the yacc parser code using global vars, this process
 * needs to be syncronised.
 */
pthread_mutex_t mountlock = PTHREAD_MUTEX_INITIALIZER;

static char cwd[PATH_MAX];
static char cwd_inited = 0;
static pthread_mutex_t cwdlock   = PTHREAD_MUTEX_INITIALIZER;

char *
libgf_vmp_virtual_path (struct vmp_entry *entry, const char *path, char *vpath)
{
        char    *tmp = NULL;

        tmp = ((char *)(path + (entry->vmplen-1)));
        if (strlen (tmp) > 0) {
                if (tmp[0] != '/') {
                        vpath[0] = '/';
                        vpath[1] = '\0';
                        strcat (&vpath[1], tmp);
                } else
                        strcpy (vpath, tmp);
        } else {
                vpath[0] = '/';
                vpath[1] = '\0';
        }

        return vpath;
}

char *
zr_build_process_uuid ()
{
	char           tmp_str[1024] = {0,};
	char           hostname[256] = {0,};
	struct timeval tv = {0,};
	struct tm      now = {0, };
	char           now_str[32];

	if (-1 == gettimeofday(&tv, NULL)) {
		gf_log ("", GF_LOG_ERROR, 
			"gettimeofday: failed %s",
			strerror (errno));		
	}

	if (-1 == gethostname (hostname, 256)) {
		gf_log ("", GF_LOG_ERROR, 
			"gethostname: failed %s",
			strerror (errno));
	}

	localtime_r (&tv.tv_sec, &now);
	strftime (now_str, 32, "%Y/%m/%d-%H:%M:%S", &now);
	snprintf (tmp_str, 1024, "%s-%d-%s:%ld", 
		  hostname, getpid(), now_str, tv.tv_usec);
	
	return strdup (tmp_str);
}


int32_t
libgf_client_forget (xlator_t *this,
		     inode_t *inode)
{
        uint64_t ptr = 0;
	libglusterfs_client_inode_ctx_t *ctx = NULL;
	
	inode_ctx_del (inode, this, &ptr);
        ctx = (libglusterfs_client_inode_ctx_t *)(long) ptr;

	FREE (ctx);

        return 0;
}

xlator_t *
libgf_inode_to_xlator (inode_t *inode)
{
        if (!inode)
                return NULL;

        if (!inode->table)
                return NULL;

        if (!inode->table->xl)
                return NULL;

        if (!inode->table->xl->ctx)
                return NULL;

        return inode->table->xl->ctx->top;
}

libglusterfs_client_fd_ctx_t *
libgf_get_fd_ctx (fd_t *fd)
{
        uint64_t                        ctxaddr = 0;
        libglusterfs_client_fd_ctx_t    *ctx = NULL;

        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, fd, out);

        if (fd_ctx_get (fd, libgf_inode_to_xlator (fd->inode), &ctxaddr) == -1)
                goto out;

        ctx = (libglusterfs_client_fd_ctx_t *)(long)ctxaddr;

out:
        return ctx;
}

libglusterfs_client_fd_ctx_t *
libgf_alloc_fd_ctx (libglusterfs_client_ctx_t *ctx, fd_t *fd, char *vpath)
{
        libglusterfs_client_fd_ctx_t    *fdctx = NULL;
        uint64_t                        ctxaddr = 0;

        fdctx = CALLOC (1, sizeof (*fdctx));
        if (fdctx == NULL) {
                gf_log (LIBGF_XL_NAME, GF_LOG_ERROR,
                        "memory allocation failure");
                fdctx = NULL;
                goto out;
        }

        pthread_mutex_init (&fdctx->lock, NULL);
        fdctx->ctx = ctx;
        ctxaddr = (uint64_t) (long)fdctx;

        if (fd->inode) {
                if (IA_ISDIR (fd->inode->ia_type)) {
                        fdctx->dcache = CALLOC (1, sizeof (struct direntcache));
                        if (fdctx->dcache)
                                INIT_LIST_HEAD (&fdctx->dcache->entries.list);
                        /* If the calloc fails, we can still continue
                         * working as the dcache is not required for correct
                         * operation.
                         */
                }
        }

        if (vpath != NULL) {
                strcpy (fdctx->vpath, vpath);
                if (vpath[strlen(vpath) - 1] != '/') {
                        strcat (fdctx->vpath, "/");
                }
        }

        fd_ctx_set (fd, libgf_inode_to_xlator (fd->inode), ctxaddr);
out:
        return fdctx;
}

libglusterfs_client_fd_ctx_t *
libgf_del_fd_ctx (fd_t *fd)
{
        uint64_t                        ctxaddr = 0;
        libglusterfs_client_fd_ctx_t    *ctx = NULL;

        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, fd, out);

        if (fd_ctx_del (fd, libgf_inode_to_xlator (fd->inode) , &ctxaddr) == -1)
                goto out;

        ctx = (libglusterfs_client_fd_ctx_t *)(long)ctxaddr;

out:
        return ctx;
}

void
libgf_dcache_invalidate (fd_t *fd)
{
        libglusterfs_client_fd_ctx_t    *fd_ctx = NULL;

        if (!fd)
                return;

        fd_ctx = libgf_get_fd_ctx (fd);
        if (!fd_ctx) {
                gf_log (LIBGF_XL_NAME, GF_LOG_ERROR, "No fd context present");
                errno = EBADF;
                return;
        }

        if (!fd_ctx->dcache) {
                gf_log (LIBGF_XL_NAME, GF_LOG_ERROR, "No dcache present");
                return;
        }

        if (!list_empty (&fd_ctx->dcache->entries.list))
                gf_dirent_free (&fd_ctx->dcache->entries);

        INIT_LIST_HEAD (&fd_ctx->dcache->entries.list);

        fd_ctx->dcache->next = NULL;
        fd_ctx->dcache->prev_off = 0;

        return;
}

/* The first entry in the entries is always a placeholder
 * or the list head. The real entries begin from entries->next.
 */
int
libgf_dcache_update (libglusterfs_client_ctx_t *ctx, fd_t *fd,
                     gf_dirent_t *entries)
{
        libglusterfs_client_fd_ctx_t    *fd_ctx = NULL;
        int                             op_ret = -1;

        if ((!ctx) || (!fd) || (!entries)) {
                errno = EINVAL;
                goto out;
        }

        fd_ctx = libgf_get_fd_ctx (fd);
        if (!fd_ctx) {
                gf_log (LIBGF_XL_NAME, GF_LOG_ERROR, "No fd context present");
                errno = EBADF;
		goto out;
        }

        /* dcache is not enabled. */
        if (!fd_ctx->dcache) {
                gf_log (LIBGF_XL_NAME, GF_LOG_ERROR, "No dcache present");
                op_ret = 0;
                goto out;
        }

        /* If we're updating, we must begin with invalidating any previous
         * entries.
         */
        libgf_dcache_invalidate (fd);

        fd_ctx->dcache->next = entries->next;
        /* We still need to store a pointer to the head
         * so we start free'ing from the head when invalidation
         * is required.
         *
         * Need to delink the entries from the list
         * given to us by an underlying translators. Most translators will
         * free this list after this call so we must preserve the dirents in
         * order to cache them.
         */
        list_splice_init (&entries->list, &fd_ctx->dcache->entries.list);
        op_ret = 0;
out:
        return op_ret;
}

int
libgf_dcache_readdir (libglusterfs_client_ctx_t *ctx, fd_t *fd,
                      struct dirent *dirp, off_t *offset)
{
        libglusterfs_client_fd_ctx_t    *fd_ctx = NULL;
        int                             cachevalid = 0;

        if ((!ctx) || (!fd) || (!dirp) || (!offset))
                return 0;

        fd_ctx = libgf_get_fd_ctx (fd);
        if (!fd_ctx) {
                gf_log (LIBGF_XL_NAME, GF_LOG_ERROR, "No fd context present");
                errno = EBADF;
                goto out;
        }

        if (!fd_ctx->dcache) {
                gf_log (LIBGF_XL_NAME, GF_LOG_ERROR, "No dcache present");
                goto out;
        }

        /* We've either run out of entries in the cache
         * or the cache is empty.
         */
        if (!fd_ctx->dcache->next) {
                gf_log (LIBGF_XL_NAME, GF_LOG_TRACE, "No entries present");
                goto out;
        }

        /* The dirent list is created as a circular linked list
         * so this check is needed to ensure, we dont start
         * reading old entries again.
         * If we're reached this situation, the cache is exhausted
         * and we'll need to pre-fetch more entries to continue serving.
         */
        if (fd_ctx->dcache->next == &fd_ctx->dcache->entries) {
                gf_log (LIBGF_XL_NAME, GF_LOG_TRACE, "Entries exhausted");
                goto out;
        }

        /* During sequential reading we generally expect that the offset
         * requested is the same as the offset we served in the previous call
         * to readdir. But, seekdir, rewinddir and libgf_dcache_invalidate
         * require special handling because seekdir/rewinddir change the offset
         * in the fd_ctx and libgf_dcache_invalidate changes the prev_off.
         */
        if (*offset != fd_ctx->dcache->prev_off) {
                /* For all cases of the if branch above, we know that the
                 * cache is now invalid except for the case below. It handles
                 * the case where the two offset values above are different
                 * but different because the previous readdir block was
                 * exhausted, resulting in a prev_off being set to 0 in
                 * libgf_dcache_invalidate, while the requested offset is non
                 * zero because that is what we returned for the last dirent
                 * of the previous readdir block.
                 */
                if ((*offset != 0) && (fd_ctx->dcache->prev_off == 0)) {
                        gf_log (LIBGF_XL_NAME, GF_LOG_TRACE, "Entries"
                                " exhausted");
                        cachevalid = 1;
                } else
                        gf_log (LIBGF_XL_NAME, GF_LOG_TRACE, "Dcache"
                                " invalidated previously");
        } else
                cachevalid = 1;

        if (!cachevalid)
                goto out;

        dirp->d_ino = fd_ctx->dcache->next->d_ino;
        strncpy (dirp->d_name, fd_ctx->dcache->next->d_name,
                 fd_ctx->dcache->next->d_len);

        *offset = fd_ctx->dcache->next->d_off;
        dirp->d_off = *offset;
        fd_ctx->dcache->prev_off = fd_ctx->dcache->next->d_off;
        fd_ctx->dcache->next = fd_ctx->dcache->next->next;

out:
        return cachevalid;
}


int32_t
libgf_client_release (xlator_t *this,
		      fd_t *fd)
{
	libglusterfs_client_fd_ctx_t *fd_ctx = NULL;
        fd_ctx = libgf_get_fd_ctx (fd);
        if (IA_ISDIR (fd->inode->ia_type)) {
                libgf_dcache_invalidate (fd);
                FREE (fd_ctx->dcache);
        }

        libgf_del_fd_ctx (fd);
        if (fd_ctx != NULL) {
                pthread_mutex_destroy (&fd_ctx->lock);
                FREE (fd_ctx);
        }

	return 0;
}

libglusterfs_client_inode_ctx_t *
libgf_get_inode_ctx (inode_t *inode)
{
        uint64_t                                ctxaddr = 0;
        libglusterfs_client_inode_ctx_t         *ictx = NULL;

        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, inode, out);
        if (inode_ctx_get (inode, libgf_inode_to_xlator (inode), &ctxaddr) < 0)
                goto out;

        ictx = (libglusterfs_client_inode_ctx_t *)(long)ctxaddr;

out:
        return ictx;
}

libglusterfs_client_inode_ctx_t *
libgf_del_inode_ctx (inode_t *inode)
{
        uint64_t                                ctxaddr = 0;
        libglusterfs_client_inode_ctx_t         *ictx = NULL;

        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, inode, out);
        if (inode_ctx_del (inode, libgf_inode_to_xlator (inode), &ctxaddr) < 0)
                goto out;

        ictx = (libglusterfs_client_inode_ctx_t *)(long)ctxaddr;

out:
        return ictx;
}

libglusterfs_client_inode_ctx_t *
libgf_alloc_inode_ctx (libglusterfs_client_ctx_t *ctx, inode_t *inode)
{
        uint64_t                                ctxaddr = 0;
        libglusterfs_client_inode_ctx_t         *ictx = NULL;

        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, inode, out);
        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, ctx, out);
        ictx = CALLOC (1, sizeof (*ictx));
        if (ictx == NULL) {
                gf_log (LIBGF_XL_NAME, GF_LOG_ERROR,
                                "memory allocation failure");
                goto out;
        }

        pthread_mutex_init (&ictx->lock, NULL);
        ctxaddr = (uint64_t) (long)ictx;
        if (inode_ctx_put (inode, libgf_inode_to_xlator (inode), ctxaddr) < 0){
                FREE (ictx);
                ictx = NULL;
        }

out:
        return ictx;
}

int
libgf_transform_iattr (libglusterfs_client_ctx_t *libctx, inode_t *inode,
                       struct iatt *buf)
{

        if ((!libctx) || (!buf) || (!inode))
                return -1;

        buf->ia_dev = libctx->fake_fsid;
        /* If the inode is root, the inode number must be 1 not the
         * ino received from the file system.
         */
        if ((inode->ino == 1) && (buf))
                buf->ia_ino = 1;

        return 0;
}

int
libgf_update_iattr_cache (inode_t *inode, int flags, struct iatt *buf)
{
        libglusterfs_client_inode_ctx_t *inode_ctx = NULL;
        time_t                          current = 0;
        int                             op_ret = -1;

        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, inode, out);

        inode_ctx = libgf_get_inode_ctx (inode);
        if (!inode_ctx) {
                gf_log (LIBGF_XL_NAME, GF_LOG_ERROR, "No inode context"
                        " present");
                errno = EINVAL;
                op_ret = -1;
                goto out;
        }

        pthread_mutex_lock (&inode_ctx->lock);
        {
                /* Take a timestamp only after we've acquired the
                 * lock.
                 */
                current = time (NULL);
                if (flags & LIBGF_UPDATE_LOOKUP) {
                        gf_log (LIBGF_XL_NAME, GF_LOG_TRACE, "Updating lookup");
                        inode_ctx->previous_lookup_time = current;
                }

                if (flags & LIBGF_UPDATE_STAT) {
                        gf_log (LIBGF_XL_NAME, GF_LOG_TRACE, "Updating stat");

                        /* Update the cached stat struct only if a new
                         * stat buf is given.
                         */
                        if (buf != NULL) {
                                inode_ctx->previous_stat_time = current;
                                memcpy (&inode_ctx->stbuf, buf,
                                                sizeof (inode_ctx->stbuf));
                        }
                }
        }
        pthread_mutex_unlock (&inode_ctx->lock);
        op_ret = 0;

out:
        return op_ret;
}


int
libgf_invalidate_iattr_cache (inode_t *inode, int flags)
{
        libglusterfs_client_inode_ctx_t         *ictx = NULL;

        if (!inode)
                return -1;

        ictx = libgf_get_inode_ctx (inode);
        if (!ictx) {
                gf_log (LIBGF_XL_NAME, GF_LOG_ERROR, "No inode context"
                        " present");
                return -1;
        }

        pthread_mutex_lock (&ictx->lock);
        {
                if (flags & LIBGF_INVALIDATE_LOOKUP) {
                        gf_log (LIBGF_XL_NAME, GF_LOG_TRACE, "Invalidating"
                                " lookup");
                        ictx->previous_lookup_time = 0;
                }

                if (flags & LIBGF_INVALIDATE_STAT) {
                        gf_log (LIBGF_XL_NAME, GF_LOG_TRACE, "Invalidating"
                                " stat");
                        ictx->previous_stat_time = 0;
                }

        }
        pthread_mutex_unlock (&ictx->lock);

        return 0;
}


int
libgf_is_iattr_cache_valid (libglusterfs_client_ctx_t *ctx, inode_t *inode,
                            struct iatt *sbuf, int flags)
{
        time_t                                  current = 0;
        time_t                                  prev = 0;
        libglusterfs_client_inode_ctx_t         *inode_ctx = NULL;
        int                                     cache_valid = 0;
        time_t                                  timeout = 0;

        if (inode == NULL)
                return 0;

        inode_ctx = libgf_get_inode_ctx (inode);
        if (!inode_ctx) {
                gf_log (LIBGF_XL_NAME, GF_LOG_ERROR, "No inode context"
                        " present\n");
                return 0;
        }

        pthread_mutex_lock (&inode_ctx->lock);
        {
                current = time (NULL);
                if (flags & LIBGF_VALIDATE_LOOKUP) {
                        gf_log (LIBGF_XL_NAME, GF_LOG_TRACE, "Checking lookup");
                        prev = inode_ctx->previous_lookup_time;
                        timeout = ctx->lookup_timeout;
                } else {
                        gf_log (LIBGF_XL_NAME, GF_LOG_TRACE, "Checking stat");
                        prev = inode_ctx->previous_stat_time;
                        timeout = ctx->stat_timeout;
                }

                /* Even if the timeout is set to -1 to cache
                 * infinitely, fops like write must invalidate the
                 * stat cache because writev_cbk cannot update
                 * the cache using the stat returned to it. This is
                 * because write-behind can return a stat bufs filled
                 * with zeroes.
                 */
                if (prev == 0) {
                        cache_valid = 0;
                        gf_log (LIBGF_XL_NAME, GF_LOG_TRACE, "Cache Invalid");
                        goto iattr_unlock_out;
                }

                /* Cache infinitely */
                if (timeout == (time_t)-1) {
                        cache_valid = 1;
                        gf_log (LIBGF_XL_NAME, GF_LOG_TRACE, "Caching On and "
                                "valid");
                        goto iattr_unlock_out;
                }

                /* Disable caching completely */
                if (timeout == 0) {
                        cache_valid = 0;
                        gf_log (LIBGF_XL_NAME, GF_LOG_TRACE, "Cache disabled");
                        goto iattr_unlock_out;
                }

                if ((prev > 0) && (timeout >= (current - prev))) {
                        gf_log (LIBGF_XL_NAME, GF_LOG_TRACE, "Cache valid");
                        cache_valid = 1;
                }

                if (flags & LIBGF_VALIDATE_LOOKUP)
                        goto iattr_unlock_out;

                if ((cache_valid) && (sbuf))
                        *sbuf = inode_ctx->stbuf;
        }
iattr_unlock_out:
        pthread_mutex_unlock (&inode_ctx->lock);

        return cache_valid;
}

int32_t
libgf_client_releasedir (xlator_t *this,
			 fd_t *fd)
{
	libglusterfs_client_fd_ctx_t *fd_ctx = NULL;
        fd_ctx = libgf_get_fd_ctx (fd);
        if (IA_ISDIR (fd->inode->ia_type)) {
                libgf_dcache_invalidate (fd);
                FREE (fd_ctx->dcache);
        }

        libgf_del_fd_ctx (fd);
        if (fd_ctx != NULL) {
                pthread_mutex_destroy (&fd_ctx->lock);
                FREE (fd_ctx);
        }

	return 0;
}

void *poll_proc (void *ptr)
{
        glusterfs_ctx_t *ctx = ptr;

        event_dispatch (ctx->event_pool);

        return NULL;
}


int32_t
xlator_graph_init (xlator_t *xl)
{
        xlator_t *trav = xl;
        int32_t ret = -1;

        while (trav->prev)
                trav = trav->prev;

        while (trav) {
                if (!trav->ready) {
                        ret = xlator_tree_init (trav);
                        if (ret < 0)
                                break;
                }
                trav = trav->next;
        }

        return ret;
}


void
xlator_graph_fini (xlator_t *xl)
{
	xlator_t *trav = xl;
	while (trav->prev)
		trav = trav->prev;

	while (trav) {
		if (!trav->init_succeeded) {
			break;
		}

		xlator_tree_fini (trav);
		trav = trav->next;
	}
}

/* Returns a pointer to the @n'th char matching
 * @c in string @str, starting the search from right or
 * end-of-string, rather than starting from left, as rindex
 * function does.
 */
char *
libgf_rrindex (char *str, int c, int n)
{
        int     len = 0;
        int     occurrence = 0;

        if (str == NULL)
                return NULL;

        len = strlen (str);
        /* Point to last character of string. */
        str += (len - 1);
        while (len > 0) {
                if ((int)*str == c) {
                        ++occurrence;
                        if (occurrence == n)
                                break;
                }
                --len;
                --str;
        }

        return str;
}

char *
libgf_trim_to_prev_dir (char * path)
{
        char    *idx = NULL;
        int      len = 0;

        if (!path)
                return NULL;

        /* Check if we're already at root, if yes
         * then there is no prev dir.
         */
        len = strlen (path);
        if (len == 1)
                return path;

        if (path[len - 1] == '/') {
                path[len - 1] = '\0';
        }

        idx = libgf_rrindex (path, '/', 1);
        /* Move to the char after the / */
        ++idx;
        *idx = '\0';

        return path;
}


char *
libgf_prepend_cwd (const char *userpath, char *abspath, int size)
{
        if ((!userpath) || (!abspath))
                return NULL;

        if (!getcwd (abspath, size))
                return NULL;

        strcat (abspath, "/");
        strcat (abspath, userpath);

        return abspath;
}


/* Performs a lightweight path resolution that only
 * looks for . and  .. and replaces those with the
 * proper names.
 *
 * FIXME: This is a stop-gap measure till we have full
 * fledge path resolution going in here.
 * Function returns path strdup'ed so remember to FREE the
 * string as required.
 */
char *
libgf_resolve_path_light (char *path)
{
        char            *respath = NULL;
        char            *saveptr = NULL;
        char            *tok = NULL;
        int             len = 0;
        int             addslash = 0;
        char            mypath[PATH_MAX];

        if (!path)
                goto out;

        memset (mypath, 0, PATH_MAX);

        if (!libgf_path_absolute (path))
                libgf_prepend_cwd (path, mypath, PATH_MAX);
        else
                strcpy (mypath, path);

        len = strlen (mypath);
        if (len == 0) {
                goto out;
        }

        respath = calloc (PATH_MAX, sizeof (char));
        if (respath == NULL) {
                gf_log (LIBGF_XL_NAME, GF_LOG_ERROR,"Memory allocation failed");
                goto out;
        }

        /* The path only contains a / or a //, so simply add a /
         * and return.
         * This needs special handling because the loop below does
         * not allow us to do so through strtok.
         */
        if (((mypath[0] == '/') && (len == 1))
                        || (strcmp (mypath, "//") == 0)) {
                strcat (respath, "/");
                goto out;
        }

        tok = strtok_r (mypath, "/", &saveptr);
        addslash = 0;
        strcat (respath, "/");
        while (tok) {
                if (addslash) {
                        if ((strcmp (tok, ".") != 0)
                                        && (strcmp (tok, "..") != 0)) {
                                strcat (respath, "/");
                        }
                }

                if ((strcmp (tok, ".") != 0) && (strcmp (tok, "..") != 0)) {
                        strcat (respath, tok);
                        addslash = 1;
                } else if ((strcmp (tok, "..") == 0)) {
                        libgf_trim_to_prev_dir (respath);
                        addslash = 0;
                }

                tok = strtok_r (NULL, "/", &saveptr);
        }

        gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "Path: %s, Resolved Path: %s",
                path, respath);
out:
        return respath;
}

void 
libgf_client_loc_wipe (loc_t *loc)
{
	if (loc->path) {
		FREE (loc->path);
	}

	if (loc->parent) { 
		inode_unref (loc->parent);
		loc->parent = NULL;
	}

	if (loc->inode) {
		inode_unref (loc->inode);
		loc->inode = NULL;
	}

	loc->path = loc->name = NULL;
        loc->ino = 0;
}


int32_t
libgf_client_loc_fill (loc_t *loc,
		       libglusterfs_client_ctx_t *ctx,
		       ino_t ino,
		       ino_t par,
		       const char *name)
{
        inode_t *inode = NULL, *parent = NULL;
	int32_t ret = -1;
	char *path = NULL;

        /* resistance against multiple invocation of loc_fill not to get
           reference leaks via inode_search() */

        inode = loc->inode;
	
        if (!inode) {
                if (ino)
                        inode = inode_search (ctx->itable, ino, NULL);

                if (inode)
                        goto inode_found;

                if (par && name)
                        inode = inode_search (ctx->itable, par, name);
        }

inode_found:
        if (inode) {
                loc->ino = inode->ino;
                loc->inode = inode;
        }

        parent = loc->parent;
        if (!parent) {
                if (inode)
                        parent = inode_parent (inode, par, name);
                else
                        parent = inode_search (ctx->itable, par, NULL);
                loc->parent = parent;
        }
  
	if (!loc->path) {
		if (name && parent) {
			ret = inode_path (parent, name, &path);
			if (ret <= 0) {
				gf_log ("glusterfs-fuse", GF_LOG_ERROR,
					"inode_path failed for %"PRId64"/%s",
					parent->ino, name);
				goto fail;
			} else {
				loc->path = path;
			}
		} else 	if (inode) {
			ret = inode_path (inode, NULL, &path);
			if (ret <= 0) {
				gf_log ("glusterfs-fuse", GF_LOG_ERROR,
					"inode_path failed for %"PRId64,
					inode->ino);
				goto fail;
			} else {
				loc->path = path;
			}
		}
	}

	if (loc->path) {
		loc->name = strrchr (loc->path, '/');
		if (loc->name)
			loc->name++;
		else loc->name = "";
	}
	
	if ((ino != 1) &&
	    (parent == NULL)) {
		gf_log ("fuse-bridge", GF_LOG_ERROR,
			"failed to search parent for %"PRId64"/%s (%"PRId64")",
			(ino_t)par, name, (ino_t)ino);
		ret = -1;
		goto fail;
	}
	ret = 0;
fail:
	return ret;
}


static call_frame_t *
get_call_frame_for_req (libglusterfs_client_ctx_t *ctx, char d)
{
        call_pool_t  *pool = ctx->gf_ctx.pool;
        xlator_t     *this = ctx->gf_ctx.graph;
        call_frame_t *frame = NULL;
  

        frame = create_frame (this, pool);

        frame->root->uid = geteuid ();
        frame->root->gid = getegid ();
        frame->root->pid = ctx->pid;
        frame->root->unique = ctx->counter++;
  
        return frame;
}

void 
libgf_client_fini (xlator_t *this)
{
	FREE (this->private);
        return;
}


int32_t
libgf_client_notify (xlator_t *this, 
                     int32_t event,
                     void *data, 
                     ...)
{
        libglusterfs_client_private_t *priv = this->private;

        switch (event)
        {
        case GF_EVENT_CHILD_UP:
                pthread_mutex_lock (&priv->lock);
                {
                        priv->complete = 1;
                        pthread_cond_broadcast (&priv->init_con_established);
                }
                pthread_mutex_unlock (&priv->lock);
                break;

        default:
                default_notify (this, event, data);
        }

        return 0;
}

int32_t 
libgf_client_init (xlator_t *this)
{
        return 0;
}

glusterfs_handle_t 
glusterfs_init (glusterfs_init_params_t *init_ctx, uint32_t fakefsid)
{
        libglusterfs_client_ctx_t *ctx = NULL;
        libglusterfs_client_private_t *priv = NULL;
        FILE *specfp = NULL;
        xlator_t *graph = NULL, *trav = NULL;
        call_pool_t *pool = NULL;
        int32_t ret = 0;
        struct rlimit lim;
	uint32_t xl_count = 0;
        loc_t       new_loc = {0, };
        struct timeval tv = {0, };
        uint32_t       len = 0;
        char           buf[PATH_MAX];

        if (!init_ctx || (!init_ctx->specfile && !init_ctx->specfp)) {
                errno = EINVAL;
                return NULL;
        }

        ctx = CALLOC (1, sizeof (*ctx));
        if (!ctx) {
		fprintf (stderr, 
			 "libglusterfsclient: %s:%s():%d: out of memory\n",
			 __FILE__, __PRETTY_FUNCTION__, __LINE__);

                errno = ENOMEM;
                return NULL;
        }

        ctx->lookup_timeout = init_ctx->lookup_timeout;
        ctx->stat_timeout = init_ctx->stat_timeout;
        ctx->fake_fsid = fakefsid;
        ctx->pid = getpid ();
        pthread_mutex_init (&ctx->gf_ctx.lock, NULL);
  
        pool = ctx->gf_ctx.pool = CALLOC (1, sizeof (call_pool_t));
        if (!pool) {
                errno = ENOMEM;
                FREE (ctx);
                return NULL;
        }

        LOCK_INIT (&pool->lock);
        INIT_LIST_HEAD (&pool->all_frames);

	/* FIXME: why is count hardcoded to 16384 */
        ctx->gf_ctx.event_pool = event_pool_new (16384);
        ctx->gf_ctx.page_size  = LIBGF_IOBUF_SIZE;
        ctx->gf_ctx.iobuf_pool = iobuf_pool_new (8 * 1048576,
                                                 ctx->gf_ctx.page_size);

        lim.rlim_cur = RLIM_INFINITY;
        lim.rlim_max = RLIM_INFINITY;
        setrlimit (RLIMIT_CORE, &lim);
        setrlimit (RLIMIT_NOFILE, &lim);

        ctx->gf_ctx.cmd_args.log_level = GF_LOG_WARNING;

        if (init_ctx->logfile)
                ctx->gf_ctx.cmd_args.log_file = strdup (init_ctx->logfile);
        else
                ctx->gf_ctx.cmd_args.log_file = strdup ("/dev/stderr");

        if (init_ctx->loglevel) {
                if (!strncasecmp (init_ctx->loglevel, "DEBUG",
                                  strlen ("DEBUG"))) {
                        ctx->gf_ctx.cmd_args.log_level = GF_LOG_DEBUG;
                } else if (!strncasecmp (init_ctx->loglevel, "WARNING",
                                         strlen ("WARNING"))) {
                        ctx->gf_ctx.cmd_args.log_level = GF_LOG_WARNING;
                } else if (!strncasecmp (init_ctx->loglevel, "CRITICAL",
                                         strlen ("CRITICAL"))) {
                        ctx->gf_ctx.cmd_args.log_level = GF_LOG_CRITICAL;
                } else if (!strncasecmp (init_ctx->loglevel, "NONE",
                                         strlen ("NONE"))) {
                        ctx->gf_ctx.cmd_args.log_level = GF_LOG_NONE;
                } else if (!strncasecmp (init_ctx->loglevel, "ERROR",
                                         strlen ("ERROR"))) {
                        ctx->gf_ctx.cmd_args.log_level = GF_LOG_ERROR;
                } else if (!strncasecmp (init_ctx->loglevel, "TRACE",
                                         strlen ("TRACE"))) {
                        ctx->gf_ctx.cmd_args.log_level = GF_LOG_TRACE;
                } else {
			fprintf (stderr, 
				 "libglusterfsclient: %s:%s():%d: Unrecognized log-level \"%s\", possible values are \"DEBUG|WARNING|[ERROR]|CRITICAL|NONE|TRACE\"\n",
                                 __FILE__, __PRETTY_FUNCTION__, __LINE__,
                                 init_ctx->loglevel);
			FREE (ctx->gf_ctx.cmd_args.log_file);
                        FREE (ctx->gf_ctx.pool);
                        FREE (ctx->gf_ctx.event_pool);
                        FREE (ctx);
                        errno = EINVAL;
                        return NULL;
                }
        }

	if (first_init)
        {
                memset (buf, 0, PATH_MAX);

                if (getcwd (buf, PATH_MAX) == NULL) {
                        fprintf (stderr, "libglusterfsclient: cannot get "
                                 "current working directory (%s)",
                                 strerror (errno));
			FREE (ctx->gf_ctx.cmd_args.log_file);
                        FREE (ctx->gf_ctx.pool);
                        FREE (ctx->gf_ctx.event_pool);
                        FREE (ctx);
                        return NULL;
                }

                len = strlen (buf);
                if ((buf[len - 1] != '/')) {
                        if ((len + 2) > PATH_MAX) {
                                errno = ENAMETOOLONG;
                                fprintf (stderr, "libglusterfsclient: cannot"
                                         "get current working directory (%s)",
                                         strerror (errno));
                                FREE (ctx->gf_ctx.cmd_args.log_file);
                                FREE (ctx->gf_ctx.pool);
                                FREE (ctx->gf_ctx.event_pool);
                                FREE (ctx);
                                return NULL;
                        }

                        strcat (buf, "/");
                }

                pthread_mutex_lock (&cwdlock);
                {
                        strcpy (cwd, buf);
                        cwd_inited = 1;
                }
                pthread_mutex_unlock (&cwdlock);

                ret = gf_log_init (ctx->gf_ctx.cmd_args.log_file);
                if (ret == -1) {
			fprintf (stderr, 
				 "libglusterfsclient: %s:%s():%d: failed to open logfile \"%s\"\n", 
				 __FILE__, __PRETTY_FUNCTION__, __LINE__, 
				 ctx->gf_ctx.cmd_args.log_file);
			FREE (ctx->gf_ctx.cmd_args.log_file);
                        FREE (ctx->gf_ctx.pool);
                        FREE (ctx->gf_ctx.event_pool);
                        FREE (ctx);
                        return NULL;
                }

                gf_log_set_loglevel (ctx->gf_ctx.cmd_args.log_level);
        }

        if (init_ctx->specfp) {
                specfp = init_ctx->specfp;
                if (fseek (specfp, 0L, SEEK_SET)) {
			fprintf (stderr, 
				 "libglusterfsclient: %s:%s():%d: fseek on volume file stream failed (%s)\n",
                                 __FILE__, __PRETTY_FUNCTION__, __LINE__,
                                 strerror (errno));
			FREE (ctx->gf_ctx.cmd_args.log_file);
                        FREE (ctx->gf_ctx.pool);
                        FREE (ctx->gf_ctx.event_pool);
                        FREE (ctx);
                        return NULL;
                }
        } else if (init_ctx->specfile) { 
                specfp = fopen (init_ctx->specfile, "r");
                ctx->gf_ctx.cmd_args.volume_file = strdup (init_ctx->specfile);
        }

        if (!specfp) {
		fprintf (stderr, 
			 "libglusterfsclient: %s:%s():%d: could not open volfile: %s\n", 
			 __FILE__, __PRETTY_FUNCTION__, __LINE__,
                         strerror (errno));
		FREE (ctx->gf_ctx.cmd_args.log_file);
                FREE (ctx->gf_ctx.cmd_args.volume_file);
                FREE (ctx->gf_ctx.pool);
                FREE (ctx->gf_ctx.event_pool);
                FREE (ctx);
                return NULL;
        }

        if (init_ctx->volume_name) {
                ctx->gf_ctx.cmd_args.volume_name = strdup (init_ctx->volume_name);
        }

	graph = file_to_xlator_tree (&ctx->gf_ctx, specfp);
        if (!graph) {
		fprintf (stderr, 
			 "libglusterfsclient: %s:%s():%d: cannot create configuration graph (%s)\n",
			 __FILE__, __PRETTY_FUNCTION__, __LINE__,
                         strerror (errno));

		FREE (ctx->gf_ctx.cmd_args.log_file);
                FREE (ctx->gf_ctx.cmd_args.volume_file);
                FREE (ctx->gf_ctx.cmd_args.volume_name);
                FREE (ctx->gf_ctx.pool);
                FREE (ctx->gf_ctx.event_pool);
                FREE (ctx);
                return NULL;
        }

        if (init_ctx->volume_name) {
                trav = graph;
                while (trav) {
                        if (strcmp (trav->name, init_ctx->volume_name) == 0) {
                                graph = trav;
                                break;
                        }
                        trav = trav->next;
                }
        }

        ctx->gf_ctx.graph = libglusterfs_graph (graph);
        if (!ctx->gf_ctx.graph) {
		fprintf (stderr, 
			 "libglusterfsclient: %s:%s():%d: graph creation failed (%s)\n",
			 __FILE__, __PRETTY_FUNCTION__, __LINE__,
                         strerror (errno));

		xlator_tree_free (graph);
		FREE (ctx->gf_ctx.cmd_args.log_file);
                FREE (ctx->gf_ctx.cmd_args.volume_file);
                FREE (ctx->gf_ctx.cmd_args.volume_name);
                FREE (ctx->gf_ctx.pool);
                FREE (ctx->gf_ctx.event_pool);
                FREE (ctx);
                return NULL;
        }
        graph = ctx->gf_ctx.graph;
        ctx->gf_ctx.top = graph;

	trav = graph;
	while (trav) {
		xl_count++;  /* Getting this value right is very important */
		trav = trav->next;
	}

	ctx->gf_ctx.xl_count = xl_count + 1;

        priv = CALLOC (1, sizeof (*priv));
        if (!priv) {
		fprintf (stderr, 
			 "libglusterfsclient: %s:%s():%d: cannot allocate memory (%s)\n",
			 __FILE__, __PRETTY_FUNCTION__, __LINE__,
                         strerror (errno));

		xlator_tree_free (graph);
		FREE (ctx->gf_ctx.cmd_args.log_file);
                FREE (ctx->gf_ctx.cmd_args.volume_file);
                FREE (ctx->gf_ctx.cmd_args.volume_name);
                FREE (ctx->gf_ctx.pool);
                FREE (ctx->gf_ctx.event_pool);
                /* inode_table_destroy (ctx->itable); */
                FREE (ctx);
         
                return NULL;
        }

        pthread_cond_init (&priv->init_con_established, NULL);
        pthread_mutex_init (&priv->lock, NULL);

        graph->private = priv;
        ctx->itable = inode_table_new (LIBGLUSTERFS_INODE_TABLE_LRU_LIMIT,
                                       graph);
        if (!ctx->itable) {
		fprintf (stderr, 
			 "libglusterfsclient: %s:%s():%d: cannot create inode table\n",
			 __FILE__, __PRETTY_FUNCTION__, __LINE__);
		xlator_tree_free (graph); 
		FREE (ctx->gf_ctx.cmd_args.log_file);
                FREE (ctx->gf_ctx.cmd_args.volume_file);
                FREE (ctx->gf_ctx.cmd_args.volume_name);

                FREE (ctx->gf_ctx.pool);
                FREE (ctx->gf_ctx.event_pool);
		xlator_tree_free (graph); 
                /* TODO: destroy graph */
                /* inode_table_destroy (ctx->itable); */
                FREE (ctx);
         
                return NULL;
        }

	set_global_ctx_ptr (&ctx->gf_ctx);
	ctx->gf_ctx.process_uuid = zr_build_process_uuid ();

        if (xlator_graph_init (graph) == -1) {
		fprintf (stderr, 
			 "libglusterfsclient: %s:%s():%d: graph initialization failed\n",
			 __FILE__, __PRETTY_FUNCTION__, __LINE__);
		xlator_tree_free (graph);
		FREE (ctx->gf_ctx.cmd_args.log_file);
                FREE (ctx->gf_ctx.cmd_args.volume_file);
                FREE (ctx->gf_ctx.cmd_args.volume_name);
                FREE (ctx->gf_ctx.pool);
                FREE (ctx->gf_ctx.event_pool);
                /* TODO: destroy graph */
                /* inode_table_destroy (ctx->itable); */
                FREE (ctx);
                return NULL;
        }

	/* Send notify to all translator saying things are ready */
	graph->notify (graph, GF_EVENT_PARENT_UP, graph);

        if (gf_timer_registry_init (&ctx->gf_ctx) == NULL) {
		fprintf (stderr, 
			 "libglusterfsclient: %s:%s():%d: timer init failed (%s)\n", 
			 __FILE__, __PRETTY_FUNCTION__, __LINE__,
                         strerror (errno));

		xlator_graph_fini (graph);
		xlator_tree_free (graph);
		FREE (ctx->gf_ctx.cmd_args.log_file);
                FREE (ctx->gf_ctx.cmd_args.volume_file);
                FREE (ctx->gf_ctx.cmd_args.volume_name);

                FREE (ctx->gf_ctx.pool);
                FREE (ctx->gf_ctx.event_pool);
                /* TODO: destroy graph */
                /* inode_table_destroy (ctx->itable); */
                FREE (ctx);
                return NULL;
        }

        if ((ret = pthread_create (&ctx->reply_thread, NULL, poll_proc,
                                   (void *)&ctx->gf_ctx))) {
		fprintf (stderr, 
			 "libglusterfsclient: %s:%s():%d: reply thread creation failed\n", 
			 __FILE__, __PRETTY_FUNCTION__, __LINE__);
		xlator_graph_fini (graph);
		xlator_tree_free (graph);
		FREE (ctx->gf_ctx.cmd_args.log_file);
                FREE (ctx->gf_ctx.cmd_args.volume_file);
                FREE (ctx->gf_ctx.cmd_args.volume_name);

                FREE (ctx->gf_ctx.pool);
                FREE (ctx->gf_ctx.event_pool);
                /* TODO: destroy graph */
                /* inode_table_destroy (ctx->itable); */
                FREE (ctx);
                return NULL;
        }

        pthread_mutex_lock (&priv->lock); 
        {
                while (!priv->complete) {
                        pthread_cond_wait (&priv->init_con_established,
                                           &priv->lock);
                }
        }
        pthread_mutex_unlock (&priv->lock);

        /* 
         * wait for some time to allow initialization of all children of 
         * distribute before sending lookup on '/'
         */

        tv.tv_sec = 0;
        tv.tv_usec = (100 * 1000);
        select (0, NULL, NULL, NULL, &tv);

        /* workaround for xlators like dht which require lookup to be sent
         * on / */
        libgf_client_loc_fill (&new_loc, ctx, 1, 0, "/");
        ret = libgf_client_lookup (ctx, &new_loc, NULL, NULL, NULL);
        if (ret == -1) {
                gf_log ("libglusterfsclient", GF_LOG_ERROR, "lookup of /"
                        " failed");
                return NULL;
        }
        libgf_client_loc_wipe (&new_loc);

	first_init = 0;
 
        return ctx;
}

struct vmp_entry *
libgf_init_vmpentry (char *vmp, glusterfs_handle_t *vmphandle)
{
        struct vmp_entry        *entry = NULL;
        int                     vmplen = 0;
        int                     appendslash = 0;
        int                     ret = -1;

        entry = CALLOC (1, sizeof (struct vmp_entry));
        if (!entry) {
                gf_log (LIBGF_XL_NAME, GF_LOG_ERROR,"Memory allocation failed");
                return NULL;
        }

        vmplen = strlen (vmp);
        if (vmp[vmplen - 1] != '/') {
                vmplen++;
                appendslash = 1;
        }

        entry->vmp = CALLOC (vmplen + 1, sizeof (char));
        if (!entry->vmp) {
                gf_log (LIBGF_XL_NAME, GF_LOG_ERROR, "Memory allocation "
                        "failed");
                goto free_entry;
        }

        strcpy (entry->vmp, vmp);
        if (appendslash) {
                entry->vmp[vmplen-1] = '/';
                entry->vmp[vmplen] = '\0';
        }
 
        entry->vmplen = vmplen;
        entry->handle = vmphandle;
        INIT_LIST_HEAD (&entry->list);
        gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "New VMP entry: %s", vmp);

        ret = 0;

free_entry:
        if (ret == -1) {
                if (entry->vmp)
                        FREE (entry->vmp);
                if (entry)
                        FREE (entry);
                entry = NULL;
        }
        return entry;
}

void
libgf_free_vmp_entry (struct vmp_entry *entry)
{
        FREE (entry->vmp);
        FREE (entry);
}

int
libgf_count_path_components (char *path)
{
        int     compos = 0;
        char    *pathdup = NULL;
        int     len = 0;

        if (!path)
                return -1;

        pathdup = strdup (path);
        if (!pathdup)
                return -1;

        len = strlen (pathdup);
        if (pathdup[len - 1] == '/')
                pathdup[len - 1] = '\0';

        path = pathdup;
        while ((path = strchr (path, '/'))) {
                compos++;
                ++path;
        }

        free (pathdup);
        return compos;
}

/* Returns the number of components that match between
 * the VMP and the path. Assumes string1 is vmp entry.
 * Assumes both are absolute paths.
 */
int
libgf_strmatchcount (char *string1, char *string2)
{
        int     matchcount = 0;
        char    *s1dup = NULL, *s2dup = NULL;
        char    *tok1 = NULL, *saveptr1 = NULL;
        char    *tok2 = NULL, *saveptr2 = NULL;

        if ((!string1) || (!string2))
                return 0;

        s1dup = strdup (string1);
        if (!s1dup)
                return 0;

        s2dup  = strdup (string2);
        if (!s2dup)
                goto free_s1;

        string1 = s1dup;
        string2 = s2dup;

        tok1 = strtok_r(string1, "/", &saveptr1);
        tok2 = strtok_r (string2, "/", &saveptr2);
        while (tok1) {
                if (!tok2)
                        break;

                if (strcmp (tok1, tok2) != 0)
                        break;

                matchcount++;
                tok1 = strtok_r(NULL, "/", &saveptr1);
                tok2 = strtok_r (NULL, "/", &saveptr2);
        }

        free (s2dup);
free_s1:
        free (s1dup);
        return matchcount;
}

int
libgf_vmp_entry_match (struct vmp_entry *entry, char *path)
{
        return libgf_strmatchcount (entry->vmp, path);
}

#define LIBGF_VMP_EXACT          1
#define LIBGF_VMP_LONGESTPREFIX  0


/* copies vmp from the vmp-entry having glusterfs handle @handle, into @vmp */
char *
libgf_vmp_search_vmp (glusterfs_handle_t handle, char *vmp, size_t vmp_size)
{
        char             *res   = NULL;
        struct vmp_entry *entry = NULL;

        if (handle == NULL) {
                goto out;
        }

        pthread_mutex_lock (&vmplock);
        {
                if (vmplist.entries == 0) {
                        gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "Virtual Mount Point "
                                "list is empty.");
                        goto unlock;
                }

                list_for_each_entry(entry, &vmplist.list, list) {
                        if (entry->handle == handle) {
                                if ((vmp_size) < (strlen (entry->vmp) + 1)) {
                                        errno = ENAMETOOLONG;
                                        goto unlock;
                                }

                                strcpy (vmp, entry->vmp);
                                res = vmp;
                                break;
                        }
                }
        }
unlock:
        pthread_mutex_unlock (&vmplock);

out:
        return res;
}


struct vmp_entry *
_libgf_vmp_search_entry (char *path, int searchtype)
{
        struct vmp_entry        *entry = NULL;
        int                     matchcount = 0;
        struct vmp_entry        *maxentry = NULL;
        int                     maxcount = 0;
        int                     vmpcompcount = 0;

        gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "VMP Search: path %s, type: %s",
                path, (searchtype == LIBGF_VMP_EXACT)?"Exact":"LongestPrefix");
        if (vmplist.entries == 0) {
                gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "Virtual Mount Point "
                        "list is empty.");
                goto out;
        }

        list_for_each_entry(entry, &vmplist.list, list) {
                vmpcompcount = libgf_count_path_components (entry->vmp);
                matchcount = libgf_vmp_entry_match (entry, path);
                gf_log (LIBGF_XL_NAME, GF_LOG_TRACE, "Candidate VMP:  %s,"
                        " Matchcount: %d", entry->vmp, matchcount);
                if ((matchcount > maxcount) && (matchcount == vmpcompcount)) {
                        maxcount = matchcount;
                        maxentry = entry;
                }
        }

        /* To ensure that the longest prefix matched entry is also an exact
         * match, this is used to check whether duplicate entries are present
         * in the vmplist.
         */
        vmpcompcount = 0;
        if ((searchtype == LIBGF_VMP_EXACT) && (maxentry)) {
                vmpcompcount = libgf_count_path_components (maxentry->vmp);
                matchcount = libgf_count_path_components (path);
                gf_log (LIBGF_XL_NAME, GF_LOG_TRACE, "Exact Check: VMP: %s,"
                        " CompCount: %d, Path: %s, CompCount: %d",
                        maxentry->vmp, vmpcompcount, path, matchcount);
                if (vmpcompcount != matchcount) {
                        gf_log (LIBGF_XL_NAME, GF_LOG_TRACE, "No Match");
                        maxentry = NULL;
                } else
                        gf_log (LIBGF_XL_NAME, GF_LOG_TRACE, "Matches!");
        }

out:        
        return maxentry;
} 

/* Used to search for a exactly matching VMP entry.
 */
struct vmp_entry *
libgf_vmp_search_exact_entry (char *path)
{
        struct vmp_entry        *entry = NULL;

        if (!path)
                goto out;

        pthread_mutex_lock (&vmplock);
        {
                entry = _libgf_vmp_search_entry (path, LIBGF_VMP_EXACT);
        }
        pthread_mutex_unlock (&vmplock);

out:
        if (entry)
                gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "VMP Entry found: path :%s"
                        " vmp: %s", path, entry->vmp);
        else
                gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "VMP Entry not found: path"
                        ": %s", path);

        return entry;
}


/* Used to search for a longest prefix matching VMP entry.
 */
struct vmp_entry *
libgf_vmp_search_entry (char *path)
{
        struct vmp_entry        *entry = NULL;

        if (!path)
                goto out;

        pthread_mutex_lock (&vmplock);
        {
                entry = _libgf_vmp_search_entry (path, LIBGF_VMP_LONGESTPREFIX);
        }
        pthread_mutex_unlock (&vmplock);

out:
        if (entry)
                gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "VMP Entry found: path :%s"
                        " vmp: %s", path, entry->vmp);
        else
                gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "VMP Entry not found: path"
                        ": %s", path);

        return entry;
}

int
libgf_vmp_map_ghandle (char *vmp, glusterfs_handle_t *vmphandle)
{
        int                     ret = -1;
        struct vmp_entry        *vmpentry = NULL;

        gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "New Entry: %s", vmp);
        vmpentry = libgf_init_vmpentry (vmp, vmphandle);
        if (!vmpentry) {
                gf_log (LIBGF_XL_NAME, GF_LOG_ERROR, "Failed to create VMP"
                        " entry");
                goto out;
        }

        pthread_mutex_lock (&vmplock);
        {
                if (vmplist.entries == 0) {
                        gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "Empty list");
                        INIT_LIST_HEAD (&vmplist.list);
                }

                list_add_tail (&vmpentry->list, &vmplist.list);
                ++vmplist.entries;
        }
        pthread_mutex_unlock (&vmplock);
        ret = 0;

out:
        return ret;
}

/* Path must be validated already. */
glusterfs_handle_t
libgf_vmp_get_ghandle (char * path)
{
        struct vmp_entry        *entry = NULL;

        entry = libgf_vmp_search_entry (path);

        if (entry == NULL)
                return NULL;

        return entry->handle;
}


/* Returns the handle for the path given in @path,
 * @path can be a relative path. The point is, here we
 * perform any path resolution that is needed and then
 * search for the corresponding vmp handle.
 * @vpath is a result-value argument in that the virtual
 * path inside the handle is copied into it.
 */
glusterfs_handle_t
libgf_resolved_path_handle (const char *path, char *vpath)
{
        char                    *respath = NULL;
        struct vmp_entry        *entry = NULL;
        glusterfs_handle_t      handle = NULL;
        char                    *tmp = NULL;

        if ((!path) || (!vpath))
                return NULL;

        /* We only want compaction before VMP entry search because the
         * VMP cannot be search unless we have an absolute path.
         * For absolute paths, we search for VMP first, then perform the
         * path compaction on the  given virtual path.
         */
        if (!libgf_path_absolute (path)) {
                respath = libgf_resolve_path_light ((char *)path);
                if (respath == NULL)
                        return NULL;
        }

        /* This condition is needed because in case of absolute paths, the path
         * would already include the VMP and we want to ensure that any path
         * compaction that happens does not exclude the VMP. In the absence of
         * this condition an absolute path might get compacted to "/", i.e.
         * exclude the VMP, and the search will fail.
         *
         * For relative paths, respath will aleady include a potential VMP
         * as a consequence of us prepending the CWD in resolve_light above.
         */
        if (libgf_path_absolute (path)) {
                entry = libgf_vmp_search_entry ((char *)path);
                if (!entry)
                        goto free_respath;
                tmp = libgf_vmp_virtual_path (entry, path, vpath);
                if (!tmp)
                        goto free_respath;

                respath = libgf_resolve_path_light (vpath);
                strcpy (vpath, respath);
        } else {
                entry = libgf_vmp_search_entry (respath);
                if (!entry)
                        goto free_respath;
                tmp = libgf_vmp_virtual_path (entry, respath, vpath);
                if (!tmp)
                        goto free_respath;
        }

        handle = entry->handle;
free_respath:
        if (respath)
                free (respath); /* Alloced in libgf_resolve_path_light */

        return handle;
}


int
glusterfs_mount (char *vmp, glusterfs_init_params_t *ipars)
{
        glusterfs_handle_t      vmphandle = NULL;
        int                     ret = -1;
        char                    *vmp_resolved = NULL;
        struct vmp_entry        *vmp_entry = NULL;
        uint32_t                vmphash = 0;
        
        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, vmp, out);
        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, ipars, out);

        vmp_resolved = libgf_resolve_path_light (vmp);
        if (!vmp_resolved) {
                gf_log (LIBGF_XL_NAME, GF_LOG_ERROR, "Path compaction failed");
                goto out;
        }

        vmphash = (dev_t)ReallySimpleHash (vmp, strlen (vmp));
        pthread_mutex_lock (&mountlock);
        {
                vmp_entry = libgf_vmp_search_exact_entry (vmp);
                if (vmp_entry) {
                        gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "Entry exists");
                        ret = 0;
                        goto unlock;
                }

                vmphandle = glusterfs_init (ipars, vmphash);
                if (!vmphandle) {
                        errno = EINVAL;
                        gf_log (LIBGF_XL_NAME, GF_LOG_ERROR, "GlusterFS context"
                                " init failed");
                        goto unlock;
                }

                ret = libgf_vmp_map_ghandle (vmp_resolved, vmphandle);
                if (ret == -1) {
                        gf_log (LIBGF_XL_NAME, GF_LOG_ERROR, "Failed to map new"
                                " handle: %s", vmp);
                        glusterfs_fini (vmphandle);
                }
        }
unlock:
        pthread_mutex_unlock (&mountlock);

out:
        if (vmp_resolved)
                FREE (vmp_resolved);

        return ret;
}

inline int
_libgf_umount (char *vmp)
{
        struct vmp_entry *entry= NULL;
        int               ret = -1;

        entry = _libgf_vmp_search_entry (vmp, LIBGF_VMP_EXACT);
        if (entry == NULL) {
                gf_log ("libglusterfsclient", GF_LOG_ERROR,
                        "path (%s) not mounted", vmp);
                goto out;
        }

        if (entry->handle == NULL) {
                gf_log ("libglusterfsclient", GF_LOG_ERROR,
                        "path (%s) has no corresponding glusterfs handle",
                        vmp);
                goto out;
        }

/*        ret = glusterfs_fini (entry->handle); */
        list_del_init (&entry->list);
        libgf_free_vmp_entry (entry);

        vmplist.entries--; 

out:
        return ret;
}

inline int
libgf_umount (char *vmp)
{
        int ret = -1;

        pthread_mutex_lock (&vmplock);
        { 
                ret = _libgf_umount (vmp);
        }
        pthread_mutex_unlock (&vmplock);
        
        return ret;
}

int
glusterfs_umount (char *vmp)
{ 
        int    ret = -1; 
        char *vmp_resolved = NULL;

        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, vmp, out);

        vmp_resolved = libgf_resolve_path_light (vmp);
        if (!vmp_resolved) {
                gf_log (LIBGF_XL_NAME, GF_LOG_ERROR, "Path compaction failed");
                goto out;
        }

        ret = libgf_umount (vmp_resolved);

out:
        if (vmp_resolved)
                FREE (vmp_resolved);

        return ret;
}

int
glusterfs_umount_all (void)
{
        struct vmp_entry *entry = NULL, *tmp = NULL;

        pthread_mutex_lock (&vmplock);
        {
                if (vmplist.entries > 0) {
                        list_for_each_entry_safe (entry, tmp, &vmplist.list,
                                                  list) {
                                /* even if there are errors, continue with other
                                   mounts
                                */
                                _libgf_umount (entry->vmp);
                        }
                }
        }
        pthread_mutex_unlock (&vmplock);
        
        return 0;
}

void
glusterfs_reset (void)
{
        INIT_LIST_HEAD (&vmplist.list);
        vmplist.entries = 0;

        memset (&vmplock, 0, sizeof (vmplock));
        pthread_mutex_init (&vmplock, NULL);

	first_init = 1;
}

void 
glusterfs_log_lock (void)
{
	gf_log_lock ();
}


void glusterfs_log_unlock (void)
{
	gf_log_unlock ();
}


void
libgf_wait_for_frames_unwind (libglusterfs_client_ctx_t *ctx)
{
        call_pool_t     *pool = NULL;
        int             canreturn = 0;

        if (!ctx)
                return;

        pool = (call_pool_t *)ctx->gf_ctx.pool;
        while (1) {
                LOCK (&pool->lock);
                {
                        if (pool->cnt == 0) {
                                canreturn = 1;
                                goto unlock_out;
                        }
                }
unlock_out:
                UNLOCK (&pool->lock);

                if (canreturn)
                        break;

                gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "Waiting for call frames");
                sleep (1);
        }

        return;
}


int 
glusterfs_fini (glusterfs_handle_t handle)
{
	libglusterfs_client_ctx_t *ctx = handle;

        libgf_wait_for_frames_unwind (ctx);

	FREE (ctx->gf_ctx.cmd_args.log_file);
	FREE (ctx->gf_ctx.cmd_args.volume_file);
	FREE (ctx->gf_ctx.cmd_args.volume_name);
	FREE (ctx->gf_ctx.pool);
        FREE (ctx->gf_ctx.event_pool);
        mem_pool_destroy (ctx->itable->inode_pool);
	 mem_pool_destroy (ctx->itable->dentry_pool);
	 mem_pool_destroy (ctx->itable->fd_mem_pool);
        /* iobuf_pool_destroy (ctx->gf_ctx.iobuf_pool); */
        ((gf_timer_registry_t *)ctx->gf_ctx.timer)->fin = 1;

	xlator_graph_fini (ctx->gf_ctx.graph);
	xlator_tree_free (ctx->gf_ctx.graph);
	ctx->gf_ctx.graph = NULL;
        pthread_cancel (ctx->reply_thread);

        FREE (ctx);

        return 0;
}


int32_t 
libgf_client_lookup_cbk (call_frame_t *frame,
                         void *cookie,
                         xlator_t *this,
                         int32_t op_ret,
                         int32_t op_errno,
                         inode_t *inode,
                         struct iatt *buf,
                         dict_t *dict,
                         struct iatt *postparent)
{
        libgf_client_local_t *local = frame->local;
        libglusterfs_client_ctx_t *ctx = frame->root->state;
	dict_t *xattr_req = NULL;

        if (op_ret == 0) {
		inode_t *parent = NULL;

		if (local->fop.lookup.loc->ino == 1) {
			buf->ia_ino = 1;
		}

		parent = local->fop.lookup.loc->parent;
                if (inode->ino != 1) {
                        inode = inode_link (inode, parent,
                                            local->fop.lookup.loc->name, buf);
                }

                libgf_transform_iattr (ctx, inode, buf);
		inode_lookup (inode);
        } else {
                if ((local->fop.lookup.is_revalidate == 0) 
                    && (op_errno == ENOENT)) {
                        gf_log ("libglusterfsclient", GF_LOG_DEBUG,
                                "%"PRId64": (op_num=%d) %s => -1 (%s)",
				frame->root->unique, frame->root->op,
				local->fop.lookup.loc->path,
				strerror (op_errno));
                } else {
                        gf_log ("libglusterfsclient", GF_LOG_ERROR,
                                "%"PRId64": (op_num=%d) %s => -1 (%s)",
				frame->root->unique, frame->root->op,
				local->fop.lookup.loc->path,
				strerror (op_errno));
                }

                if (local->fop.lookup.is_revalidate == 1) {
			int32_t ret = 0;
                        inode_unref (local->fop.lookup.loc->inode);
                        local->fop.lookup.loc->inode = inode_new (ctx->itable);
                        local->fop.lookup.is_revalidate = 2;

                        if (local->fop.lookup.size > 0) {
                                xattr_req = dict_new ();
                                ret = dict_set (xattr_req, "glusterfs.content",
                                                data_from_uint64 (local->fop.lookup.size));
                                if (ret == -1) {
                                        op_ret = -1;
                                        /* TODO: set proper error code */
                                        op_errno = errno;
                                        inode = NULL;
                                        buf = NULL;
                                        dict = NULL;
                                        dict_unref (xattr_req);
                                        goto out;
                                }
                        }

                        STACK_WIND (frame, libgf_client_lookup_cbk,
                                    FIRST_CHILD (this),
                                    FIRST_CHILD (this)->fops->lookup,
                                    local->fop.lookup.loc, xattr_req);

			if (xattr_req) {
				dict_unref (xattr_req);
				xattr_req = NULL;
			}

                        return 0;
                }
        }

out:
        local->reply_stub = fop_lookup_cbk_stub (frame, NULL, op_ret, op_errno,
                                                 inode, buf, dict, postparent);

        LIBGF_REPLY_NOTIFY (local);
        return 0;
}

int32_t
libgf_client_lookup (libglusterfs_client_ctx_t *ctx,
                     loc_t *loc,
                     struct iatt *stbuf,
                     dict_t **dict,
		     dict_t *xattr_req)
{
        call_stub_t  *stub = NULL;
        int32_t op_ret;
        libgf_client_local_t *local = NULL;
        inode_t *inode = NULL;
        
        local = CALLOC (1, sizeof (*local));
        if (!local) {
                gf_log (LIBGF_XL_NAME, GF_LOG_ERROR, "Memory allocation"
                        " failed");
                errno = ENOMEM;
                return -1;
        }

        if (loc->inode) {
                local->fop.lookup.is_revalidate = 1;
                loc->ino = loc->inode->ino;
        }
        else
                loc->inode = inode_new (ctx->itable);

        local->fop.lookup.loc = loc;

        LIBGF_CLIENT_FOP(ctx, stub, lookup, local, loc, xattr_req);

        op_ret = stub->args.lookup_cbk.op_ret;
        errno = stub->args.lookup_cbk.op_errno;

        if (op_ret == -1)
                goto out;

        inode = stub->args.lookup_cbk.inode;
        if (!(libgf_get_inode_ctx (inode)))
                libgf_alloc_inode_ctx (ctx, inode);
        libgf_transform_iattr (ctx, inode, &stub->args.lookup_cbk.buf);
        libgf_update_iattr_cache (inode, LIBGF_UPDATE_ALL,
                                        &stub->args.lookup_cbk.buf);
        if (stbuf)
                *stbuf = stub->args.lookup_cbk.buf;

        if (dict)
                *dict = dict_ref (stub->args.lookup_cbk.dict);

        if (inode != loc->inode) {
                inode_unref (loc->inode);
                loc->inode = inode_ref (inode);
        }

out:
	call_stub_destroy (stub);
        return op_ret;
}

int 
glusterfs_glh_get (glusterfs_handle_t handle, const char *path, void *buf,
                   size_t size, struct stat *stbuf)
{
        int32_t op_ret = -1;
        loc_t loc = {0, };
        libglusterfs_client_ctx_t *ctx = handle;
        dict_t *dict = NULL;
	dict_t *xattr_req = NULL;
	char *name = NULL, *pathname = NULL;
        struct iatt iatt = {0,};

        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, ctx, out);
        GF_VALIDATE_ABSOLUTE_PATH_OR_GOTO (LIBGF_XL_NAME, path, out);

        gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "path %s, size %lu", path,
                (long unsigned)size);
        if (size < 0) {
                gf_log (LIBGF_XL_NAME, GF_LOG_ERROR, "Invalid size");
                errno = EINVAL;
                goto out;
        }

        if (size == 0) {
                op_ret = 0;
                goto out;
        }

        loc.path = strdup (path);
        if (!loc.path) {
                gf_log (LIBGF_XL_NAME, GF_LOG_ERROR, "Path compaction failed");
                goto out;
        }

	op_ret = libgf_client_path_lookup (&loc, ctx, 0);
	if (op_ret == -1) {
		gf_log ("libglusterfsclient", GF_LOG_ERROR,
			"path lookup failed for (%s)", loc.path);
		goto out;
	}

	pathname = strdup (loc.path);
	name = compat_basename (pathname);

        op_ret = libgf_client_loc_fill (&loc, ctx, 0, loc.parent->ino, name);
        if (op_ret < 0) {
                gf_log ("libglusterfsclient",
                        GF_LOG_ERROR,
                        "libgf_client_loc_fill returned -1, returning EINVAL");
                errno = EINVAL;
                goto out;
        }

        if (size) { 
                xattr_req = dict_new ();
                op_ret = dict_set (xattr_req, "glusterfs.content",
                                   data_from_uint64 (size));
                if (op_ret < 0) {
                        gf_log ("libglusterfsclient",
                                GF_LOG_ERROR,
                                "setting requested content size dictionary failed");
                        goto out;
                }
        }

        op_ret = libgf_client_lookup (ctx, &loc, &iatt, &dict, xattr_req);
        iatt_to_stat (&iatt, stbuf);
        if (!op_ret && stbuf && (iatt.ia_size <= size) && dict && buf) {
                data_t *mem_data = NULL;
                void *mem = NULL;
                
                mem_data = dict_get (dict, "glusterfs.content");
                if (mem_data) {
                        mem = data_to_ptr (mem_data);
                }
                        
                if (mem != NULL) { 
                        memcpy (buf, mem, iatt.ia_size);
                }
        }

out:
	if (xattr_req) {
		dict_unref (xattr_req);
	}
        
	if (dict) {
		dict_unref (dict);
	}

	if (pathname) {
		FREE (pathname);
	}
	libgf_client_loc_wipe (&loc);

        return op_ret;
}

int
glusterfs_get (const char *path, void *buf, size_t size, struct stat *stbuf)
{
        int                     op_ret = -1;
        glusterfs_handle_t      h      = NULL;
        char                    vpath[PATH_MAX];

        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, path, out);
        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, buf, out);
        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, stbuf, out);

        gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "path %s, size %lu", path,
                (long unsigned)size);

        h = libgf_resolved_path_handle (path, vpath);
        if (!h) {
                errno = ENODEV;
                goto out;
        }

        op_ret = glusterfs_glh_get (h, vpath, buf, size, stbuf);

out:
        return op_ret;
}

int
libgf_client_lookup_async_cbk (call_frame_t *frame,
                               void *cookie,
                               xlator_t *this,
                               int32_t op_ret,
                               int32_t op_errno,
                               inode_t *inode,
                               struct iatt *stbuf,
                               dict_t *dict,
                               struct iatt *postparent)
{
        libglusterfs_client_async_local_t *local = frame->local;
        glusterfs_get_cbk_t lookup_cbk = local->fop.lookup_cbk.cbk;
        libglusterfs_client_ctx_t *ctx = frame->root->state;
	glusterfs_iobuf_t *iobuf = NULL;
	dict_t *xattr_req = NULL;
        inode_t *parent = NULL;
        struct stat stat = {0,};

        if (op_ret == 0) {
                parent = local->fop.lookup_cbk.loc->parent;
                inode_link (inode, parent, local->fop.lookup_cbk.loc->name,
                            stbuf);
                libgf_transform_iattr (ctx, inode, stbuf);
                if (!(libgf_get_inode_ctx (inode)))
                        libgf_alloc_inode_ctx (ctx, inode);
                libgf_update_iattr_cache (inode, LIBGF_UPDATE_ALL, stbuf);
                inode_lookup (inode);
        } else {
                if ((local->fop.lookup_cbk.is_revalidate == 0) 
                    && (op_errno == ENOENT)) {
                        gf_log ("libglusterfsclient", GF_LOG_DEBUG,
                                "%"PRId64": (op_num=%d) %s => -1 (%s)",
				frame->root->unique, frame->root->op,
				local->fop.lookup_cbk.loc->path,
				strerror (op_errno));
                } else {
                        gf_log ("libglusterfsclient", GF_LOG_ERROR,
                                "%"PRId64": (op_num=%d) %s => -1 (%s)",
				frame->root->unique, frame->root->op,
                                local->fop.lookup_cbk.loc->path,
				strerror (op_errno));
                }

                if (local->fop.lookup_cbk.is_revalidate == 1) {
			int32_t ret = 0;
                        inode_unref (local->fop.lookup_cbk.loc->inode);
                        local->fop.lookup_cbk.loc->inode = inode_new (ctx->itable);
                        local->fop.lookup_cbk.is_revalidate = 2;

                        if (local->fop.lookup_cbk.size > 0) {
                                xattr_req = dict_new ();
                                ret = dict_set (xattr_req, "glusterfs.content",
                                                data_from_uint64 (local->fop.lookup_cbk.size));
                                if (ret == -1) {
                                        op_ret = -1;
                                        /* TODO: set proper error code */
                                        op_errno = errno;
                                        inode = NULL;
                                        stbuf = NULL;
                                        dict = NULL;
                                        dict_unref (xattr_req);
                                        goto out;
                                }
                        }


                        STACK_WIND (frame, libgf_client_lookup_async_cbk,
                                    FIRST_CHILD (this),
                                    FIRST_CHILD (this)->fops->lookup,
                                    local->fop.lookup_cbk.loc, xattr_req);
			
			if (xattr_req) {
				dict_unref (xattr_req);
				xattr_req = NULL;
			}

                        return 0;
                }
        }

out:
        if (!op_ret && local->fop.lookup_cbk.size && dict) {
                data_t *mem_data = NULL;
                void *mem = NULL;
		struct iovec *vector = NULL;

                mem_data = dict_get (dict, "glusterfs.content");
                if (mem_data) {
                        mem = data_to_ptr (mem_data);
                }

                if (mem && stbuf->ia_size <= local->fop.lookup_cbk.size) {
			iobuf = CALLOC (1, sizeof (*iobuf));
			ERR_ABORT (iobuf);

			vector = CALLOC (1, sizeof (*vector));
			ERR_ABORT (vector);
			vector->iov_base = mem;
			vector->iov_len = stbuf->ia_size;  

			iobuf->vector = vector;
			iobuf->count = 1;
			iobuf->dictref = dict_ref (dict);
		}
	}

        iatt_to_stat (stbuf, &stat);
        lookup_cbk (op_ret, op_errno, iobuf, &stat, local->cbk_data);

	libgf_client_loc_wipe (local->fop.lookup_cbk.loc);
        free (local->fop.lookup_cbk.loc);

        free (local);
        frame->local = NULL;
        STACK_DESTROY (frame->root);

        return 0;
}

/* TODO: implement async dentry lookup */

int
glusterfs_get_async (glusterfs_handle_t handle, 
		     const char *path,
		     size_t size, 
		     glusterfs_get_cbk_t cbk,
		     void *cbk_data)
{
        loc_t *loc = NULL;
        libglusterfs_client_ctx_t *ctx = handle;
        libglusterfs_client_async_local_t *local = NULL;
	int32_t op_ret = 0;
	dict_t *xattr_req = NULL;
	char *name = NULL, *pathname = NULL;

	if (!ctx || !path || path[0] != '/') {
		errno = EINVAL;
		op_ret = -1;
		goto out;
	}

        if (size < 0) {
                errno = EINVAL;
                op_ret = -1;
                goto out;
        }

        if (size == 0) {
                op_ret = 0;
                goto out;
        }

        local = CALLOC (1, sizeof (*local));
        local->fop.lookup_cbk.is_revalidate = 1;

        loc = CALLOC (1, sizeof (*loc));
	loc->path = strdup (path);
	op_ret = libgf_client_path_lookup (loc, ctx, 1);
	if (op_ret == -1) {
		gf_log ("libglusterfsclient",
			GF_LOG_ERROR,
			"path lookup failed for (%s)", path);
		goto out;
	}

	pathname = strdup (path);
	name = compat_basename (pathname);
        op_ret = libgf_client_loc_fill (loc, ctx, 0, loc->parent->ino, name);
	if (op_ret < 0) {
		gf_log ("libglusterfsclient",
			GF_LOG_ERROR,
			"libgf_client_loc_fill returned -1, returning EINVAL");
		errno = EINVAL;
		goto out;
	}

        if (!loc->inode) {
                loc->inode = inode_new (ctx->itable);
                local->fop.lookup_cbk.is_revalidate = 0;
        } 

        local->fop.lookup_cbk.cbk = cbk;
        local->fop.lookup_cbk.size = size;
        local->fop.lookup_cbk.loc = loc;
        local->cbk_data = cbk_data;

        if (size > 0) {
                xattr_req = dict_new ();
                op_ret = dict_set (xattr_req, "glusterfs.content",
                                   data_from_uint64 (size));
                if (op_ret < 0) {
                        dict_unref (xattr_req);
                        xattr_req = NULL;
                        goto out;
                }
        }

        LIBGF_CLIENT_FOP_ASYNC (ctx,
                                local,
                                libgf_client_lookup_async_cbk,
                                lookup,
                                loc,
                                xattr_req);
	if (xattr_req) {
		dict_unref (xattr_req);
		xattr_req = NULL;
	}

out:
	if (pathname) {
		FREE (pathname);
	}
 
        return op_ret;
}

int32_t
libgf_client_getxattr_cbk (call_frame_t *frame,
                           void *cookie,
                           xlator_t *this,
                           int32_t op_ret,
                           int32_t op_errno,
                           dict_t *dict)
{

        libgf_client_local_t *local = frame->local;

        local->reply_stub = fop_getxattr_cbk_stub (frame, NULL, op_ret,
                                                   op_errno, dict);

        LIBGF_REPLY_NOTIFY (local);
        return 0;
}

size_t 
libgf_client_getxattr (libglusterfs_client_ctx_t *ctx, 
                       loc_t *loc,
                       const char *name,
                       void *value,
                       size_t size)
{
        call_stub_t  *stub = NULL;
        int32_t op_ret = 0;
        libgf_client_local_t *local = NULL;

        LIBGF_CLIENT_FOP (ctx, stub, getxattr, local, loc, name);

        op_ret = stub->args.getxattr_cbk.op_ret;
        errno = stub->args.getxattr_cbk.op_errno;

        if (op_ret >= 0) {
                /*
                  gf_log ("LIBGF_CLIENT", GF_LOG_DEBUG,
                  "%"PRId64": %s => %d", frame->root->unique,
                  state->fuse_loc.loc.path, op_ret);
                */

                data_t *value_data = dict_get (stub->args.getxattr_cbk.dict,
                                               (char *)name);
    
                if (value_data) {
                        int32_t copy_len = 0;

                        /* Don't return the value for '\0' */
                        op_ret = value_data->len; 
                        if ((size > 0) && (value != NULL)) {
                                copy_len = size < value_data->len ? 
                                        size : value_data->len;
                                memcpy (value, value_data->data, copy_len);
                                op_ret = copy_len;
                        }
                } else {
                        errno = ENODATA;
                        op_ret = -1;
                }
        }
	
	call_stub_destroy (stub);
        return op_ret;
}

#define LIBGF_DO_GETXATTR       1
#define LIBGF_DO_LGETXATTR      2

ssize_t
__glusterfs_glh_getxattr (glusterfs_handle_t handle, const char *path,
                          const char *name, void *value, size_t size,
                          int whichop)
{
        int32_t op_ret = -1;
        loc_t loc = {0, };
	libglusterfs_client_ctx_t *ctx = handle;
	char *file = NULL;
        char *pathres = NULL, *tmp = NULL;

        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, ctx, out);
        GF_VALIDATE_ABSOLUTE_PATH_OR_GOTO (LIBGF_XL_NAME, path, out);
        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, name, out);

        gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "path %s, name %s, size %lu,"
                " op %d", path, name, (long unsigned)size, whichop);
        if (name[0] == '\0') {
		errno = EINVAL;
                gf_log (LIBGF_XL_NAME, GF_LOG_ERROR, "Invalid argument: Name"
                        " not NULL terminated");
		goto out;
	}

        if (size < 0) {
                errno = EINVAL;
                gf_log (LIBGF_XL_NAME, GF_LOG_ERROR, "Invalid argument: size is"
                        " less than zero");
                goto out;
        }

        pathres = strdup (path);
        if (!pathres) {
                gf_log (LIBGF_XL_NAME, GF_LOG_ERROR, "strdup failed");
                goto out;
        }

        loc.path = strdup (pathres);
	op_ret = libgf_client_path_lookup (&loc, ctx, 1);
	if (op_ret == -1) {
                gf_log ("libglusterfsclient", GF_LOG_ERROR,
			"path lookup failed for (%s)", loc.path);
		goto out;
	}

	tmp = strdup (pathres);
	file = compat_basename (tmp);
        op_ret = libgf_client_loc_fill (&loc, ctx, 0, loc.parent->ino, file);
	if (op_ret == -1) {
		gf_log ("libglusterfsclient",
			GF_LOG_ERROR,
			"libgf_client_loc_fill returned -1, returning EINVAL");
		errno = EINVAL;
		goto out;
	}

        if (whichop == LIBGF_DO_LGETXATTR)
                goto do_getx;

        if (!IA_ISLNK (loc.inode->ia_type))
                goto do_getx;

        libgf_client_loc_wipe (&loc); 
        op_ret = libgf_realpath_loc_fill (ctx, (char *)pathres, &loc);
        if (op_ret == -1) {
                gf_log (LIBGF_XL_NAME, GF_LOG_ERROR, "realpath failed");
                goto out;
        }

do_getx:
	op_ret = libgf_client_getxattr (ctx, &loc, name, value, size);

out:
	if (tmp) {
		FREE (tmp);
	}

        if (pathres)
                FREE (pathres);

        libgf_client_loc_wipe (&loc);

        return op_ret;
}

ssize_t
glusterfs_glh_getxattr (glusterfs_handle_t handle, const char *path,
                        const char *name, void *value, size_t size)
{
        return __glusterfs_glh_getxattr (handle, path, name, value, size,
                                         LIBGF_DO_GETXATTR);
}

ssize_t
glusterfs_glh_lgetxattr (glusterfs_handle_t handle, const char *path,
                         const char *name, void *value, size_t size)
{
        return __glusterfs_glh_getxattr (handle, path, name, value, size,
                                         LIBGF_DO_LGETXATTR);
}

ssize_t
glusterfs_getxattr (const char *path, const char *name, void *value,
                        size_t size)
{
        int                     op_ret = -1;
        char                    vpath[PATH_MAX];
        glusterfs_handle_t      h = NULL;

        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, path, out);
        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, name, out);

        if ((size > 0) && (value == NULL)) {
                errno = EINVAL;
                gf_log (LIBGF_XL_NAME, GF_LOG_ERROR, "Invalid argument value");
                goto out;
        }

        gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "path %s, name %s, size %lu",
                path, name, (long unsigned)size);

        h = libgf_resolved_path_handle (path, vpath);
        if (!h) {
                errno = ENODEV;
                goto out;
        }

        op_ret = __glusterfs_glh_getxattr (h, vpath, name, value, size,
                                           LIBGF_DO_GETXATTR);

out:
        return op_ret;
}

ssize_t
glusterfs_lgetxattr (const char *path, const char *name, void *value,
                     size_t size)
{
        int                     op_ret = -1;
        char                    vpath[PATH_MAX];
        glusterfs_handle_t      h = NULL;

        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, path, out);
        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, name, out);

        if ((size > 0) && (value == NULL)) {
                errno = EINVAL;
                gf_log (LIBGF_XL_NAME, GF_LOG_ERROR, "Invalid argument value");
                goto out;
        }

        gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "path %s, name %s, size %lu",
                path, name, (long unsigned)size);

        h = libgf_resolved_path_handle (path, vpath);
        if (!h) {
                errno = ENODEV;
                goto out;
        }

        op_ret = __glusterfs_glh_getxattr (h, vpath, name, value, size,
                                           LIBGF_DO_LGETXATTR);

out:
        return op_ret;
}

static int32_t
libgf_client_open_cbk (call_frame_t *frame,
                       void *cookie,
                       xlator_t *this,
                       int32_t op_ret,
                       int32_t op_errno,
                       fd_t *fd)
{
        libgf_client_local_t *local = frame->local;

        local->reply_stub = fop_open_cbk_stub (frame, NULL, op_ret, op_errno,
                                               fd);

        LIBGF_REPLY_NOTIFY (local);
        return 0;
}


int 
libgf_client_open (libglusterfs_client_ctx_t *ctx, 
                   loc_t *loc, 
                   fd_t *fd, 
                   int flags)
{
        call_stub_t *stub = NULL;
        int32_t op_ret = 0;
        libgf_client_local_t *local = NULL;

        LIBGF_CLIENT_FOP (ctx, stub, open, local, loc, flags, fd, 0);

        op_ret = stub->args.open_cbk.op_ret;
        errno = stub->args.open_cbk.op_errno;

        gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "open: path %s, status: %d, errno"
                " %d", loc->path, op_ret, errno);
        if (op_ret != -1)
                fd_bind (fd);
	call_stub_destroy (stub);
        return op_ret;
}

static int32_t
libgf_client_create_cbk (call_frame_t *frame,
                         void *cookie,
                         xlator_t *this,
                         int32_t op_ret,
                         int32_t op_errno,
                         fd_t *fd,
                         inode_t *inode,
                         struct iatt *buf,
                         struct iatt *preparent,
                         struct iatt *postparent)
{
        libgf_client_local_t *local = frame->local;

        local->reply_stub = fop_create_cbk_stub (frame, NULL, op_ret, op_errno,
                                                 fd, inode, buf, preparent,
                                                 postparent);

        LIBGF_REPLY_NOTIFY (local);
        return 0;
}

int 
libgf_client_creat (libglusterfs_client_ctx_t *ctx,
                    loc_t *loc,
                    fd_t *fd,
                    int flags,
                    mode_t mode)
{
        call_stub_t *stub = NULL;
        int32_t op_ret = 0;
        libgf_client_local_t *local = NULL;
        inode_t *libgf_inode = NULL;

        LIBGF_CLIENT_FOP (ctx, stub, create, local, loc, flags, mode, fd);
  
        op_ret = stub->args.create_cbk.op_ret;
        errno = stub->args.create_cbk.op_errno;
        gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "Create: path %s, status: %d,"
                " errno: %d", loc->path, op_ret, errno);
        if (op_ret == -1)
                goto out;

	libgf_inode = stub->args.create_cbk.inode;
        inode_link (libgf_inode, loc->parent, loc->name,
                        &stub->args.create_cbk.buf);
        libgf_transform_iattr (ctx, libgf_inode, &stub->args.create_cbk.buf);

        inode_lookup (libgf_inode);

        libgf_alloc_inode_ctx (ctx, libgf_inode);
        libgf_update_iattr_cache (libgf_inode, LIBGF_UPDATE_ALL,
                                        &stub->args.create_cbk.buf);

out:
	call_stub_destroy (stub);
        return op_ret;
}

int32_t
libgf_client_opendir_cbk (call_frame_t *frame,
                          void *cookie,
                          xlator_t *this,
                          int32_t op_ret,
                          int32_t op_errno,
                          fd_t *fd)
{
        libgf_client_local_t *local = frame->local;

        local->reply_stub = fop_opendir_cbk_stub (frame, NULL, op_ret, op_errno,
                                                  fd);

        LIBGF_REPLY_NOTIFY (local);
        return 0;
}

int 
libgf_client_opendir (libglusterfs_client_ctx_t *ctx,
                      loc_t *loc,
                      fd_t *fd)
{
        call_stub_t *stub = NULL;
        int32_t op_ret = -1;
        libgf_client_local_t *local = NULL;

        if (((fd->flags & O_ACCMODE) == O_WRONLY)
                || ((fd->flags & O_ACCMODE) == O_RDWR)) {
                errno = EISDIR;
                goto out;
        }
        LIBGF_CLIENT_FOP (ctx, stub, opendir, local, loc, fd);

        op_ret = stub->args.opendir_cbk.op_ret;
        errno = stub->args.opendir_cbk.op_errno;
        gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "opendir: path %s, status %d,"
                " errno %d", loc->path, op_ret, errno);
        if (op_ret != -1)
                fd_bind (fd);

	call_stub_destroy (stub);
out:
        return op_ret;
}

glusterfs_file_t 
glusterfs_glh_open (glusterfs_handle_t handle, const char *path, int flags,...)
{
        loc_t loc = {0, };
        long op_ret = -1;
        fd_t *fd = NULL;
	int32_t ret = -1;
	libglusterfs_client_ctx_t *ctx = handle;
	char *name = NULL, *pathname = NULL;
        libglusterfs_client_inode_ctx_t *inode_ctx = NULL;
        mode_t mode = 0;
        va_list ap;
        char *pathres = NULL;
        char *vpath = NULL;

        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, ctx, out);
        GF_VALIDATE_ABSOLUTE_PATH_OR_GOTO (LIBGF_XL_NAME, path, out);

        pathres = strdup (path);
        if (!pathres) {
                gf_log (LIBGF_XL_NAME, GF_LOG_ERROR, "strdup failed");
                goto out;
        }

	loc.path = strdup (pathres);
	op_ret = libgf_client_path_lookup (&loc, ctx, 1);

        if ((op_ret == -1) && ((flags & O_CREAT) != O_CREAT)) {
		gf_log ("libglusterfsclient", GF_LOG_ERROR,
			"path lookup failed for (%s)", loc.path);
		goto out;
	}

        if (!op_ret && ((flags & O_CREAT) == O_CREAT) 
            && ((flags & O_EXCL) == O_EXCL)) {
                errno = EEXIST;
                op_ret = -1;
                goto out;
        }

        if (op_ret == 0) {
                flags &= ~O_CREAT;
        }

        if ((op_ret == -1) && ((flags & O_CREAT) == O_CREAT)) {
                libgf_client_loc_wipe (&loc);
                loc.path = strdup (pathres);

                op_ret = libgf_client_path_lookup (&loc, ctx, 0);
                if (op_ret == -1) {
                        gf_log ("libglusterfsclient", GF_LOG_ERROR,
                                "path lookup failed for parent while trying to"
                                " create (%s)", pathres);
                        goto out;
                }

                loc.inode = inode_new (ctx->itable);
        }

	pathname = strdup (pathres);
	name = compat_basename (pathname);

        ret = libgf_client_loc_fill (&loc, ctx, 0, loc.parent->ino, name);
	if (ret == -1) {
		gf_log ("libglusterfsclient",
			GF_LOG_ERROR,
			"libgf_client_loc_fill returned -1, returning EINVAL");
		errno = EINVAL;
		goto out;
	}

        fd = fd_create (loc.inode, ctx->pid);
        fd->flags = flags;

        if (((flags & O_CREAT) == O_CREAT)) {
                /* If we have the st_mode for the basename, check if
                 * it is a directory here itself, rather than sending
                 * a network message through libgf_client_creat, and
                 * then receiving a EISDIR.
                 */
                if (IA_ISDIR (loc.inode->ia_type)) {
                        errno = EISDIR;
                        op_ret = -1;
                        goto op_over;
                }
                va_start (ap, flags);
                mode = va_arg (ap, mode_t);
                va_end (ap);
                op_ret = libgf_client_creat (ctx, &loc, fd, flags, mode);
        } else {
                if (IA_ISDIR (loc.inode->ia_type))
                        op_ret = libgf_client_opendir (ctx, &loc, fd);
                else
                        op_ret = libgf_client_open (ctx, &loc, fd, flags);
        }

op_over:
        if (op_ret == -1) {
                fd_unref (fd);
                fd = NULL;
                goto out;
        }

        vpath = NULL;
        if (IA_ISDIR (loc.inode->ia_type)) {
                vpath = (char *)path;
        }

        if (!libgf_get_fd_ctx (fd)) {
                if (!libgf_alloc_fd_ctx (ctx, fd, vpath)) {
                        gf_log (LIBGF_XL_NAME, GF_LOG_ERROR, "Failed to"
                                " allocate fd context");
                        errno = EINVAL;
                        op_ret = -1;
                        goto out;
                }
        }

        if ((flags & O_TRUNC) && (((flags & O_ACCMODE) == O_RDWR)
                                  || ((flags & O_ACCMODE) == O_WRONLY))) {
                inode_ctx = libgf_get_inode_ctx (fd->inode);
                if (IA_ISREG (inode_ctx->stbuf.ia_type)) {
                                inode_ctx->stbuf.ia_size = 0;
                                inode_ctx->stbuf.ia_blocks = 0;
                }
        }

out:
        libgf_client_loc_wipe (&loc);

	if (pathname) {
		FREE (pathname);
	}

        if (pathres)
                FREE (pathres);

        return fd;
}

glusterfs_file_t
glusterfs_open (const char *path, int flags, ...)
{
        va_list                 ap;
        glusterfs_file_t        fh   = NULL;
        glusterfs_handle_t      h    = NULL; 
        mode_t                  mode = 0;
        char                    vpath[PATH_MAX];

        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, path, out);

        gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "path %s", path);


        h = libgf_resolved_path_handle (path, vpath);
        if (!h) {
                errno = ENODEV;
                goto out;
        }

        if (flags & O_CREAT) {
                va_start (ap, flags);
                mode = va_arg (ap, mode_t);
                va_end (ap);
                fh = glusterfs_glh_open (h, vpath, flags, mode);
        } else
                fh = glusterfs_glh_open (h, vpath, flags);
out:
        return fh;
}

glusterfs_file_t 
glusterfs_glh_creat (glusterfs_handle_t handle, const char *path, mode_t mode)
{
        gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "path %s", path);
	return glusterfs_glh_open (handle, path,
			       (O_CREAT | O_WRONLY | O_TRUNC), mode);
}

glusterfs_file_t
glusterfs_creat (const char *path, mode_t mode)
{
        glusterfs_file_t        fh = NULL;
        char                    vpath[PATH_MAX];
        glusterfs_handle_t      h = NULL;

        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, path, out);

        gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "path %s", path);

        h = libgf_resolved_path_handle (path, vpath);
        if (!h) {
                errno = ENODEV;
                goto out;
        }

        fh = glusterfs_glh_creat (h, vpath, mode);

out:
        return fh;
}

int32_t
libgf_client_flush_cbk (call_frame_t *frame,
                        void *cookie,
                        xlator_t *this,
                        int32_t op_ret,
                        int32_t op_errno)
{
        libgf_client_local_t *local = frame->local;
        
        local->reply_stub = fop_flush_cbk_stub (frame, NULL, op_ret, op_errno);
        
        LIBGF_REPLY_NOTIFY (local);
        return 0;
}


int 
libgf_client_flush (libglusterfs_client_ctx_t *ctx, fd_t *fd)
{
        call_stub_t *stub;
        int32_t op_ret;
        libgf_client_local_t *local = NULL;

        LIBGF_CLIENT_FOP (ctx, stub, flush, local, fd);
        
        op_ret = stub->args.flush_cbk.op_ret;
        errno = stub->args.flush_cbk.op_errno;
        
	call_stub_destroy (stub);        
        return op_ret;
}


int 
glusterfs_close (glusterfs_file_t fd)
{
        int32_t op_ret = -1;
        libglusterfs_client_ctx_t *ctx = NULL;
        libglusterfs_client_fd_ctx_t *fd_ctx = NULL;

        if (!fd) {
                errno = EINVAL;
		goto out;
        }

        fd_ctx = libgf_get_fd_ctx (fd);
        if (!fd_ctx) {
                gf_log (LIBGF_XL_NAME, GF_LOG_ERROR, "No fd context present");
                errno = EBADF;
                goto out;
        }
        ctx = fd_ctx->ctx;

        op_ret = libgf_client_flush (ctx, (fd_t *)fd);

        fd_unref ((fd_t *)fd);

out:
        return op_ret;
}

int32_t
libgf_client_setxattr_cbk (call_frame_t *frame,
                           void *cookie,
                           xlator_t *this,
                           int32_t op_ret,
                           int32_t op_errno)
{
        libgf_client_local_t *local = frame->local;

        local->reply_stub = fop_setxattr_cbk_stub (frame, NULL, op_ret,
                                                   op_errno);

        LIBGF_REPLY_NOTIFY (local);
        return 0;
}

int
libgf_client_setxattr (libglusterfs_client_ctx_t *ctx, 
                       loc_t *loc,
                       const char *name,
                       const void *value,
                       size_t size,
                       int flags)
{
        call_stub_t  *stub = NULL;
        int32_t op_ret = 0;
        dict_t *dict;
        libgf_client_local_t *local = NULL;

        dict = get_new_dict ();

        dict_set (dict, (char *)name,
                  bin_to_data ((void *)value, size));
        dict_ref (dict);


        LIBGF_CLIENT_FOP (ctx, stub, setxattr, local, loc, dict, flags);

        op_ret = stub->args.setxattr_cbk.op_ret;
        errno = stub->args.setxattr_cbk.op_errno;

        gf_log (LIBGF_XL_NAME, GF_LOG_ERROR, "path %s, name %s, status %d,"
                "errno %d", loc->path, name, op_ret, errno);
        dict_unref (dict);
	call_stub_destroy (stub);
        return op_ret;
}


#define LIBGF_DO_SETXATTR       1
#define LIBGF_DO_LSETXATTR      2

int 
__glusterfs_glh_setxattr (glusterfs_handle_t handle, const char *path,
                          const char *name, const void *value,
                          size_t size, int flags, int whichop)
{
        int32_t op_ret = -1;
        loc_t loc = {0, };
	libglusterfs_client_ctx_t *ctx = handle;
        char *tmppath = NULL;
        loc_t *realloc = NULL;
        char *pathres = NULL;

        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, ctx, out);
        GF_VALIDATE_ABSOLUTE_PATH_OR_GOTO (LIBGF_XL_NAME, path, out);

        gf_log (LIBGF_XL_NAME, GF_LOG_ERROR, "path %s, name %s, op %d", path
                ,name, whichop);
        if (size <= 0) {
                errno = EINVAL;
                goto out;
        }

        pathres = strdup (path);
        if (!pathres) {
                gf_log (LIBGF_XL_NAME, GF_LOG_ERROR, "strdup failed");
                goto out;
        }

        loc.path = strdup (pathres);
	op_ret = libgf_client_path_lookup (&loc, ctx, 1);
	if (op_ret == -1) {
		gf_log ("libglusterfsclient", GF_LOG_ERROR,
			"path lookup failed for (%s)", pathres);
		goto out;
	}

        tmppath = strdup (pathres);

        op_ret = libgf_client_loc_fill (&loc, ctx, 0, loc.parent->ino,
                                        compat_basename (tmppath));
        FREE (tmppath);
	if (op_ret == -1) {
		gf_log ("libglusterfsclient",
			GF_LOG_ERROR,
			"libgf_client_loc_fill returned -1, returning EINVAL");
		errno = EINVAL;
		goto out;
	}

        realloc = &loc;
        if (whichop == LIBGF_DO_LSETXATTR)
                goto do_setx;

        if (!IA_ISLNK (loc.inode->ia_type))
                goto do_setx;

        libgf_client_loc_wipe (&loc);
        realloc = &loc;
        libgf_realpath_loc_fill (ctx, (char *)pathres, realloc);

do_setx:
        if (!op_ret)
                op_ret = libgf_client_setxattr (ctx, realloc, name, value,
                                                size, flags);

out:
        if (pathres)
                FREE (pathres);

        libgf_client_loc_wipe (realloc);
        return op_ret;
}

int
glusterfs_glh_setxattr (glusterfs_handle_t handle, const char *path,
                        const char *name, const void *value, size_t size,
                        int flags)
{
        return __glusterfs_glh_setxattr (handle, path, name, value, size, flags
                                         , LIBGF_DO_SETXATTR);
}

int
glusterfs_glh_lsetxattr (glusterfs_handle_t handle, const char *path,
                         const char *name, const void *value, size_t size,
                         int flags)
{
        return __glusterfs_glh_setxattr (handle, path, name, value, size, flags
                                         , LIBGF_DO_LSETXATTR);
}

int
glusterfs_setxattr (const char *path, const char *name, const void *value,
                        size_t size, int flags)
{
        int                     op_ret = -1;
        char                    vpath[PATH_MAX];
        glusterfs_handle_t      h = NULL;

        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, path, out);
        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, name, out);
        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, value, out);

        gf_log (LIBGF_XL_NAME, GF_LOG_ERROR, "path %s, name %s", path, name);

        h = libgf_resolved_path_handle (path, vpath);
        if (!h) {
                errno = ENODEV;
                goto out;
        }

        op_ret = __glusterfs_glh_setxattr (h, vpath, name, value, size, flags,
                                           LIBGF_DO_SETXATTR);

out:
        return op_ret;
}

int
glusterfs_lsetxattr (const char *path, const char *name, const void *value,
                     size_t size, int flags)
{
        int                     op_ret = -1;
        char                    vpath[PATH_MAX];
        glusterfs_handle_t      h = NULL;

        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, path, out);
        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, name, out);
        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, value, out);

        gf_log (LIBGF_XL_NAME, GF_LOG_ERROR, "path %s, name %s", path, name);

        h = libgf_resolved_path_handle (path, vpath);
        if (!h) {
                errno = ENODEV;
                goto out;
        }

        op_ret = __glusterfs_glh_setxattr (h, vpath, name, value, size, flags,
                                           LIBGF_DO_LSETXATTR);

out:
        return op_ret;
}

int32_t
libgf_client_fsetxattr_cbk (call_frame_t *frame,
                            void *cookie,
                            xlator_t *this,
                            int32_t op_ret,
                            int32_t op_errno)
{
        libgf_client_local_t *local = frame->local;

        local->reply_stub = fop_fsetxattr_cbk_stub (frame, NULL, op_ret,
                                                    op_errno);

        LIBGF_REPLY_NOTIFY (local);
        return 0;
}

int
libgf_client_fsetxattr (libglusterfs_client_ctx_t *ctx, 
                        fd_t *fd,
                        const char *name,
                        const void *value,
                        size_t size,
                        int flags)
{
        call_stub_t  *stub = NULL;
        int32_t op_ret = 0;
        dict_t *dict;
        libgf_client_local_t *local = NULL;

        dict = get_new_dict ();

        dict_set (dict, (char *)name,
                  bin_to_data ((void *)value, size));
        dict_ref (dict);

        LIBGF_CLIENT_FOP (ctx, stub, fsetxattr, local, fd, dict, flags);

        op_ret = stub->args.fsetxattr_cbk.op_ret;
        errno = stub->args.fsetxattr_cbk.op_errno;

        gf_log (LIBGF_XL_NAME, GF_LOG_ERROR, "name %s, status %d, errno %d",
                name, op_ret, errno);
        dict_unref (dict);
	call_stub_destroy (stub);

        return op_ret;
}

int 
glusterfs_fsetxattr (glusterfs_file_t fd, 
                     const char *name,
                     const void *value, 
                     size_t size, 
                     int flags)
{
	int32_t op_ret = 0;
        fd_t *__fd = fd;
        libglusterfs_client_fd_ctx_t *fd_ctx = NULL;
        libglusterfs_client_ctx_t *ctx = NULL;
        
        if (!fd) {
                errno = EINVAL;
                op_ret = -1;
                gf_log("libglusterfsclient",
                       GF_LOG_ERROR,
                       "invalid fd");
                goto out;
        }

        if (size <= 0) {
                gf_log (LIBGF_XL_NAME, GF_LOG_ERROR, "Invalid argument: size is"
                        " less than or equal to zero");
                errno = EINVAL;
                op_ret = -1;
                goto out;
        }

        fd_ctx = libgf_get_fd_ctx (fd);
        if (!fd_ctx) {
                gf_log (LIBGF_XL_NAME, GF_LOG_ERROR, "No fd context present");
                errno = EBADF;
		op_ret = -1;
		goto out;
        }

        ctx = fd_ctx->ctx;
        op_ret = libgf_client_fsetxattr (ctx, __fd, name, value, size,
                                         flags);
        
out:
	return op_ret;
}

int32_t
libgf_client_fgetxattr_cbk (call_frame_t *frame,
                            void *cookie,
                            xlator_t *this,
                            int32_t op_ret,
                            int32_t op_errno,
                            dict_t *dict)
{

        libgf_client_local_t *local = frame->local;

        local->reply_stub = fop_fgetxattr_cbk_stub (frame, NULL, op_ret,
                                                    op_errno, dict);

        LIBGF_REPLY_NOTIFY (local);
        return 0;
}

size_t 
libgf_client_fgetxattr (libglusterfs_client_ctx_t *ctx, 
                        fd_t *fd,
                        const char *name,
                        void *value,
                        size_t size)
{
        call_stub_t  *stub = NULL;
        int32_t op_ret = 0;
        libgf_client_local_t *local = NULL;

        LIBGF_CLIENT_FOP (ctx, stub, fgetxattr, local, fd, name);

        op_ret = stub->args.fgetxattr_cbk.op_ret;
        errno = stub->args.fgetxattr_cbk.op_errno;

        if (op_ret >= 0) {
                /*
                  gf_log ("LIBGF_CLIENT", GF_LOG_DEBUG,
                  "%"PRId64": %s => %d", frame->root->unique,
                  state->fuse_loc.loc.path, op_ret);
                */

                data_t *value_data = dict_get (stub->args.fgetxattr_cbk.dict,
                                               (char *)name);
    
                if (value_data) {
                        int32_t copy_len = 0;

                        /* Don't return the value for '\0' */
                        op_ret = value_data->len; 
                        copy_len = size < value_data->len ? 
                                size : value_data->len;
                        memcpy (value, value_data->data, copy_len);
                } else {
                        errno = ENODATA;
                        op_ret = -1;
                }
        }
	
        gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "name %s, status %d, errno %d",
                name, op_ret, errno);
	call_stub_destroy (stub);
        return op_ret;
}

ssize_t 
glusterfs_fgetxattr (glusterfs_file_t fd, 
                     const char *name,
                     void *value, 
                     size_t size)
{
	int32_t op_ret = 0;
        libglusterfs_client_ctx_t *ctx;
        fd_t *__fd = (fd_t *)fd;
        libglusterfs_client_fd_ctx_t *fd_ctx = NULL;

        gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "name %s", name);
        if (size < 0) {
                errno = EINVAL;
                op_ret = -1;
                goto out;
        }

        if (size == 0)
                goto out;

        fd_ctx = libgf_get_fd_ctx (fd);
        if (!fd_ctx) {
                gf_log (LIBGF_XL_NAME, GF_LOG_ERROR, "No fd context present");
                errno = EBADF;
		op_ret = -1;
		goto out;
        }

        ctx = fd_ctx->ctx;
        op_ret = libgf_client_fgetxattr (ctx, __fd, name, value, size);
out:
	return op_ret;
}

ssize_t 
glusterfs_listxattr (glusterfs_handle_t handle,
                     const char *path, 
                     char *list,
                     size_t size)
{
        return ENOSYS;
}

ssize_t 
glusterfs_llistxattr (glusterfs_handle_t handle,
                      const char *path, 
                      char *list,
                      size_t size)
{
        return ENOSYS;
}

ssize_t 
glusterfs_flistxattr (glusterfs_file_t fd, 
                      char *list,
                      size_t size)
{
        return ENOSYS;
}

int 
glusterfs_removexattr (glusterfs_handle_t handle, 
                       const char *path, 
                       const char *name)
{
        return ENOSYS;
}

int 
glusterfs_lremovexattr (glusterfs_handle_t handle, 
                        const char *path, 
                        const char *name)
{
        return ENOSYS;
}

int 
glusterfs_fremovexattr (glusterfs_file_t fd, 
                        const char *name)
{
        return ENOSYS;
}

int32_t
libgf_client_readv_cbk (call_frame_t *frame,
                        void *cookie,
                        xlator_t *this,
                        int32_t op_ret,
                        int32_t op_errno,
                        struct iovec *vector,
                        int32_t count,
                        struct iatt *stbuf,
                        struct iobref *iobref)
{
        libgf_client_local_t *local = frame->local;

        local->reply_stub = fop_readv_cbk_stub (frame, NULL, op_ret, op_errno,
                                                vector, count, stbuf, iobref);
        LIBGF_REPLY_NOTIFY (local);
        return 0;
}

int
libgf_client_iobuf_read (libglusterfs_client_ctx_t *ctx, fd_t *fd, void *buf,
                         size_t size, off_t offset)
{
        call_stub_t          *stub = NULL;
        struct iovec         *vector = NULL;
        int32_t               op_ret = -1;
        int                   count = 0;
        libgf_client_local_t *local = NULL;
        struct iatt          *stbuf = NULL;

        local = CALLOC (1, sizeof (*local));
        ERR_ABORT (local);
        local->fd = fd;
        LIBGF_CLIENT_FOP (ctx, stub, readv, local, fd, size, offset);

        op_ret = stub->args.readv_cbk.op_ret;
        errno = stub->args.readv_cbk.op_errno;
        count = stub->args.readv_cbk.count;
        vector = stub->args.readv_cbk.vector;
        if (op_ret > 0) {
                int i = 0;
                op_ret = 0;
                while (size && (i < count)) {
                        int len = (size < vector[i].iov_len) ?
                                size : vector[i].iov_len;
                        memcpy (buf, vector[i++].iov_base, len);
                        buf += len;
                        size -= len;
                        op_ret += len;
                }
                stbuf = &stub->args.readv_cbk.stbuf;
                libgf_transform_iattr (ctx, fd->inode, stbuf);
                libgf_invalidate_iattr_cache (fd->inode, LIBGF_INVALIDATE_STAT);
        }

        gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "size %lu, offset %"PRIu64,
                (long unsigned)size, offset);
	call_stub_destroy (stub);
        return op_ret;
}

int
libgf_client_read (libglusterfs_client_ctx_t *ctx, fd_t *fd, void *buf,
                   size_t size, off_t offset)
{
        int32_t op_ret = -1;
        int32_t ret = 0;
        size_t  tmp   = 0;

        while (size != 0) {
                tmp = ((size > LIBGF_IOBUF_SIZE) ? LIBGF_IOBUF_SIZE :
                       size);
                op_ret = libgf_client_iobuf_read (ctx, fd, buf, tmp, offset);
                if (op_ret < 0) {
                        ret = op_ret;
                        break;
                }

                ret += op_ret;

                if (op_ret < tmp)
                        break;

                size -= op_ret;
                offset += op_ret;
                buf = (char *)buf + op_ret;
        }

        return ret;
}

ssize_t
glusterfs_read (glusterfs_file_t fd, void *buf, size_t nbytes)
{
        int32_t op_ret = -1;
        off_t offset = 0;
        libglusterfs_client_ctx_t *ctx = NULL;
        libglusterfs_client_fd_ctx_t *fd_ctx = NULL;

        if (nbytes < 0) {
                errno = EINVAL;
                goto out;
        }

        if (nbytes == 0) {
                op_ret = 0;
                goto out;
        }

        if (fd == 0) {
                errno = EINVAL;
		goto out;
        }

        fd_ctx = libgf_get_fd_ctx (fd);
        if (!fd_ctx) {
                gf_log (LIBGF_XL_NAME, GF_LOG_ERROR, "No fd context present");
                errno = EBADF;
		goto out;
        }

        pthread_mutex_lock (&fd_ctx->lock);
        {
                ctx = fd_ctx->ctx;
                offset = fd_ctx->offset;
        }
        pthread_mutex_unlock (&fd_ctx->lock);

        op_ret = libgf_client_read (ctx, (fd_t *)fd, buf, nbytes, offset);

        if (op_ret > 0) {
                offset += op_ret;
                pthread_mutex_lock (&fd_ctx->lock);
                {
                        fd_ctx->offset = offset;
                }
                pthread_mutex_unlock (&fd_ctx->lock);
        }

out:
        return op_ret;
}


ssize_t
libgf_client_iobuf_readv (libglusterfs_client_ctx_t *ctx, fd_t *fd,
                          const struct iovec *dst_vector, int count,
                          size_t size, off_t offset, int *idx,
                          off_t *vec_offset)
{
        call_stub_t          *stub       = NULL;
        struct iovec         *src_vector = NULL;
        int32_t               op_ret     = -1;
        libgf_client_local_t *local      = NULL;
        int                   src        = 0, dst = 0;
        int                   src_count  = 0, dst_count = 0;
        int                   len        = 0, src_len = 0, dst_len = 0;
        off_t                 src_offset = 0, dst_offset = 0;
        struct iatt          *stbuf      = NULL;

        dst = *idx;
        dst_offset = *vec_offset;

        local = CALLOC (1, sizeof (*local));
        ERR_ABORT (local);
        local->fd = fd;
        LIBGF_CLIENT_FOP (ctx, stub, readv, local, fd, size, offset);

        op_ret = stub->args.readv_cbk.op_ret;
        errno = stub->args.readv_cbk.op_errno;
        src_count = stub->args.readv_cbk.count;
        src_vector = stub->args.readv_cbk.vector;
        if (op_ret > 0) {
                while ((size != 0) && (dst < dst_count) && (src < src_count)) {
                        src_len = src_vector[src].iov_len - src_offset;
                        dst_len = dst_vector[dst].iov_len - dst_offset;

                        len = (src_len < dst_len) ? src_len : dst_len;
                        if (len > size) {
                                len = size;
                        }

                        memcpy (dst_vector[dst].iov_base + dst_offset,
				src_vector[src].iov_base + src_offset, len);

                        size -= len;
                        src_offset += len;
                        dst_offset += len;

                        if (src_offset == src_vector[src].iov_len) {
                                src_offset = 0;
                                src++;
                        }

                        if (dst_offset == dst_vector[dst].iov_len) {
                                dst_offset = 0;
                                dst++;
                        }
                }

                stbuf = &stub->args.readv_cbk.stbuf;
                libgf_transform_iattr (ctx, fd->inode, stbuf);
                libgf_invalidate_iattr_cache (fd->inode, LIBGF_UPDATE_STAT);
        }

        *idx = dst;
        *vec_offset = dst_offset;

	call_stub_destroy (stub);
        return op_ret;
}


ssize_t
libgf_client_readv (libglusterfs_client_ctx_t *ctx, fd_t *fd,
                    const struct iovec *dst_vector, int dst_count, off_t offset)
{
        int32_t               op_ret     = -1;
        size_t                size       = 0, tmp = 0, ret = 0;
        int                   i          = 0;
        int                   dst_idx    = 0;
        off_t                 dst_offset = 0;

        for (i = 0; i < dst_count; i++)
        {
                size += dst_vector[i].iov_len;
        }

        while (size != 0) {
                tmp = ((size > LIBGF_IOBUF_SIZE) ? LIBGF_IOBUF_SIZE : size);
                op_ret = libgf_client_iobuf_readv (ctx, fd, dst_vector,
                                                   dst_count, tmp, offset,
                                                   &dst_idx, &dst_offset);
                if (op_ret <= 0) {
                        break;
                }

                offset += op_ret;
                size -= op_ret;
                ret += op_ret;
        }

        return ret;
}


ssize_t
glusterfs_readv (glusterfs_file_t fd, const struct iovec *vec, int count)
{
        int32_t op_ret = -1;
        off_t offset = 0;
        libglusterfs_client_ctx_t *ctx = NULL;
        libglusterfs_client_fd_ctx_t *fd_ctx = NULL;

        if (count < 0) {
                errno = EINVAL;
                goto out;
        }

        if (count == 0) {
                op_ret = 0;
                goto out;
        }

        if (!fd) {
                errno = EINVAL;
		goto out;
        }

        fd_ctx = libgf_get_fd_ctx (fd);
        if (!fd_ctx) {
                errno = EBADF;
		goto out;
        }

        pthread_mutex_lock (&fd_ctx->lock);
        {
                ctx = fd_ctx->ctx;
                offset = fd_ctx->offset;
        }
        pthread_mutex_unlock (&fd_ctx->lock);

        op_ret = libgf_client_readv (ctx, (fd_t *)fd, vec, count, offset);

        if (op_ret > 0) {
                offset += op_ret;
                pthread_mutex_lock (&fd_ctx->lock);
                {
                        fd_ctx->offset = offset;
                }
                pthread_mutex_unlock (&fd_ctx->lock);
        }

out:
        return op_ret;
}


ssize_t 
glusterfs_pread (glusterfs_file_t fd, 
                 void *buf, 
                 size_t count, 
                 off_t offset)
{
        int32_t op_ret = -1;
        libglusterfs_client_ctx_t *ctx = NULL;
        libglusterfs_client_fd_ctx_t *fd_ctx = NULL;

        if (count < 0) {
                errno = EINVAL;
                goto out;
        }

        if (count == 0) {
                op_ret = 0;
                goto out;
        }

        if (!fd) {
                errno = EINVAL;
		goto out;
        }

        fd_ctx = libgf_get_fd_ctx (fd);
        if (!fd_ctx) {
                errno = EBADF;
		goto out;
        }

        ctx = fd_ctx->ctx;

        op_ret = libgf_client_read (ctx, (fd_t *)fd, buf, count, offset);

out:
        return op_ret;
}


int
libgf_client_writev_cbk (call_frame_t *frame,
                         void *cookie,
                         xlator_t *this,
                         int32_t op_ret,
                         int32_t op_errno,
                         struct iatt *prebuf,
                         struct iatt *postbuf)
{
        libgf_client_local_t *local = frame->local;

        local->reply_stub = fop_writev_cbk_stub (frame, NULL, op_ret, op_errno,
                                                 prebuf, postbuf);

        LIBGF_REPLY_NOTIFY (local);
        return 0;
}


int
libgf_client_iobuf_write (libglusterfs_client_ctx_t *ctx, fd_t *fd, char *addr,
                          size_t size, off_t offset)
{
        struct iobref        *ioref = NULL;
        struct iobuf         *iob = NULL;
        int                   op_ret = -1;
        struct iovec          iov = {0, };
        call_stub_t          *stub = NULL;
        libgf_client_local_t *local = NULL;

        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, ctx, out);
        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, fd, out);
        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, addr, out);

        ioref = iobref_new ();
        if (!ioref) {
                gf_log (LIBGF_XL_NAME, GF_LOG_ERROR, "Out of memory");
                goto out;
        }

        iob = iobuf_get (ctx->gf_ctx.iobuf_pool);
        if (!iob) {
                gf_log (LIBGF_XL_NAME, GF_LOG_ERROR, "Out of memory");
                goto out;
        }

        memcpy (iob->ptr, addr, size);
        iobref_add (ioref, iob);

        iov.iov_base = iob->ptr;
        iov.iov_len = size;

        LIBGF_CLIENT_FOP (ctx, stub, writev, local, fd, &iov,
                          1, offset, ioref);

        op_ret = stub->args.writev_cbk.op_ret;
        errno = stub->args.writev_cbk.op_errno;

        /* We need to invalidate because it is possible that write-behind
         * is a translator below us and returns a stat filled with zeroes.
         */
        libgf_invalidate_iattr_cache (fd->inode, LIBGF_INVALIDATE_STAT);

out:
        if (iob) {
                iobuf_unref (iob);
        }

        if (ioref) {
                iobref_unref (ioref);
        }

        call_stub_destroy (stub);
        return op_ret;
}

int
libgf_client_writev (libglusterfs_client_ctx_t *ctx, 
                     fd_t *fd, 
                     struct iovec *vector, 
                     int count, 
                     off_t offset)
{
        int                     op_ret = 0;
        int                     written = 0;
        int                     writesize = 0;
        int                     size = 0;
        char                   *base = NULL;
        int                     i = 0;

        for (i = 0; i < count; i++) {
                size = vector[i].iov_len;
                base = vector[i].iov_base;

                while (size > 0) {
                        writesize = (size > LIBGF_IOBUF_SIZE) ?
                                LIBGF_IOBUF_SIZE : size;

                        written = libgf_client_iobuf_write (ctx, fd, base,
                                                            writesize, offset);

                        if (written == -1)
                                goto out;

                        op_ret += written;
                        base += written;
                        size -= written;
                        offset += written;
                }
        }

out:
        return op_ret;
}


ssize_t 
glusterfs_write (glusterfs_file_t fd, 
                 const void *buf, 
                 size_t n)
{
        int32_t op_ret = -1;
        off_t offset = 0;
        struct iovec vector;
        libglusterfs_client_ctx_t *ctx = NULL;
        libglusterfs_client_fd_ctx_t *fd_ctx = NULL;

        if (n < 0) {
                errno = EINVAL;
                goto out;
        }

        if (n == 0) {
                op_ret = 0;
                goto out;
        }

        if (!fd) {
                errno = EINVAL;
		goto out;
        }

        fd_ctx = libgf_get_fd_ctx (fd);
        if (!fd_ctx) {
                errno = EBADF;
		goto out;
        }

        ctx = fd_ctx->ctx;

        pthread_mutex_lock (&fd_ctx->lock);
        {
                offset = fd_ctx->offset;
        }
        pthread_mutex_unlock (&fd_ctx->lock);

        vector.iov_base = (void *)buf;
        vector.iov_len = n;

        op_ret = libgf_client_writev (ctx,
                                      (fd_t *)fd, 
                                      &vector, 
                                      1, 
                                      offset);

        if (op_ret >= 0) {
                offset += op_ret;
                pthread_mutex_lock (&fd_ctx->lock);
                {
                        fd_ctx->offset = offset;
                }
                pthread_mutex_unlock (&fd_ctx->lock);
        }

out:
        return op_ret;
}

ssize_t 
glusterfs_writev (glusterfs_file_t fd, 
                  const struct iovec *vector,
                  int count)
{
        int32_t op_ret = -1;
        off_t offset = 0;
        libglusterfs_client_ctx_t *ctx = NULL;
        libglusterfs_client_fd_ctx_t *fd_ctx = NULL;

        if (count < 0) {
                errno = EINVAL;
                goto out;
        }

        if (count == 0) {
                op_ret = 0;
                goto out;
        }

        if (!fd) {
                errno = EINVAL;
		goto out;
        }

        fd_ctx = libgf_get_fd_ctx (fd);
        if (!fd_ctx) {
                errno = EBADF;
		goto out;
        }

        ctx = fd_ctx->ctx;

        pthread_mutex_lock (&fd_ctx->lock);
        {
                offset = fd_ctx->offset;
        }
        pthread_mutex_unlock (&fd_ctx->lock);


        op_ret = libgf_client_writev (ctx,
                                      (fd_t *)fd, 
                                      (struct iovec *)vector, 
                                      count,
                                      offset);

        if (op_ret >= 0) {
                offset += op_ret;
                pthread_mutex_lock (&fd_ctx->lock);
                {
                        fd_ctx->offset = offset;
                }
                pthread_mutex_unlock (&fd_ctx->lock);
        }

out:
        return op_ret;
}


ssize_t 
glusterfs_pwrite (glusterfs_file_t fd, 
                  const void *buf, 
                  size_t count, 
                  off_t offset)
{
        int32_t op_ret = -1;
        struct iovec vector;
        libglusterfs_client_ctx_t *ctx = NULL;
        libglusterfs_client_fd_ctx_t *fd_ctx = NULL;

        if (count < 0) {
                errno = EINVAL;
                goto out;
        }

        if (count == 0) {
                op_ret = 0;
                goto out;
        }

        if (!fd) {
                errno = EINVAL;
		goto out;
        }

        fd_ctx = libgf_get_fd_ctx (fd);
        if (!fd_ctx) {
                errno = EBADF;
		goto out;
        }

        ctx = fd_ctx->ctx;

        vector.iov_base = (void *)buf;
        vector.iov_len = count;

        op_ret = libgf_client_writev (ctx,
                                      (fd_t *)fd, 
                                      &vector, 
                                      1, 
                                      offset);

out:
        return op_ret;
}


int32_t
libgf_client_readdirp_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                           int32_t op_ret, int32_t op_errno,
                           gf_dirent_t *entries)
{
        libgf_client_local_t *local = frame->local;

        /* Note, we dont let entries reach the stub because there it gets copied
         * while we can simply delink the entries here and link them into our
         * dcache, thereby avoiding the need to perform more allocations and
         * copies.
         */
        local->reply_stub = fop_readdirp_cbk_stub (frame, NULL, op_ret,
                                                   op_errno, NULL);
        if (op_ret > 0)
                libgf_dcache_update (frame->root->state, local->fd, entries);
        LIBGF_REPLY_NOTIFY (local);
        return 0;
}

int 
libgf_client_readdir (libglusterfs_client_ctx_t *ctx, fd_t *fd,
                      struct dirent *dirp, off_t *offset)
{  
        call_stub_t *stub = NULL;
        int op_ret = -1;
        libgf_client_local_t *local = NULL;

        if (libgf_dcache_readdir (ctx, fd, dirp, offset))
                return 1;
        local = CALLOC (1, sizeof (*local));
        ERR_ABORT (local);
        local->fd = fd;
        LIBGF_CLIENT_FOP (ctx, stub, readdirp, local, fd,
                          LIBGF_READDIR_BLOCK, *offset);

        errno = stub->args.readdir_cbk.op_errno;

        op_ret = libgf_dcache_readdir (ctx, fd, dirp, offset);
	call_stub_destroy (stub);
        return op_ret;
}


int
glusterfs_readdir_r (glusterfs_dir_t dirfd, struct dirent *entry,
                     struct dirent **result)
{
        int                           op_ret = -1;
        libglusterfs_client_ctx_t    *ctx = NULL;
        off_t                         offset = 0;
        libglusterfs_client_fd_ctx_t *fd_ctx = NULL;
        struct dirent                *dirp = NULL;

        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, entry, out);

        fd_ctx = libgf_get_fd_ctx (dirfd);
        if (!fd_ctx) {
                gf_log (LIBGF_XL_NAME, GF_LOG_ERROR, "fd context not present");
                errno = EBADF;
		goto out;
        }

        pthread_mutex_lock (&fd_ctx->lock);
        {
                ctx = fd_ctx->ctx;
                offset = fd_ctx->offset;
                dirp = &fd_ctx->dirp;

                gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "offset %"PRIu64, offset);
                memset (dirp, 0, sizeof (struct dirent));
                op_ret = libgf_client_readdir (ctx, (fd_t *)dirfd, dirp,
                                               &offset);
                if (op_ret <= 0) {
                        gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "readdir failed:"
                                " %s", strerror (errno));
                        if (result && (op_ret == 0)) {
                                *result = NULL;
                        } else if (op_ret < 0){
                                op_ret = errno;
                        }
                        goto unlock;
                }

                fd_ctx->offset = offset;

                if (result) {
                        *result = memcpy (entry, dirp, sizeof (*entry));
                } else {
                        memcpy (entry, dirp, sizeof (*entry));
                }

                gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "new offset %"PRIu64", "
                        " entry %s", offset, entry->d_name);
                op_ret = 0;
        }
unlock:
        pthread_mutex_unlock (&fd_ctx->lock);

out:
        return op_ret;
}


void *
glusterfs_readdir (glusterfs_dir_t dirfd)
{
        int op_ret = -1;
        libglusterfs_client_ctx_t *ctx = NULL;
        off_t offset = 0;
        libglusterfs_client_fd_ctx_t *fd_ctx = NULL;
        struct dirent *dirp = NULL;

        fd_ctx = libgf_get_fd_ctx (dirfd);
        if (!fd_ctx) {
                gf_log (LIBGF_XL_NAME, GF_LOG_ERROR, "fd context not present");
                errno = EBADF;
		goto out;
        }

        pthread_mutex_lock (&fd_ctx->lock);
        {
                ctx = fd_ctx->ctx;
                offset = fd_ctx->offset;
                dirp = &fd_ctx->dirp;
        }
        pthread_mutex_unlock (&fd_ctx->lock);

        gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "offset %"PRIu64, offset);
        memset (dirp, 0, sizeof (struct dirent));
        op_ret = libgf_client_readdir (ctx, (fd_t *)dirfd, dirp, &offset);

        if (op_ret <= 0) {
                gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "readdir failed: %s",
                        strerror (errno));
                dirp = NULL;
                goto out;
        }

        pthread_mutex_lock (&fd_ctx->lock);
        {
                fd_ctx->offset = offset;
        }
        pthread_mutex_unlock (&fd_ctx->lock);

        gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "new offset %"PRIu64", entry %s",
                offset, dirp->d_name);
out:
        return dirp;
}


int
glusterfs_getdents (glusterfs_file_t fd, struct dirent *dirp,
                    unsigned int count)
{
        int op_ret = -1;
        libglusterfs_client_ctx_t *ctx = NULL;
        off_t offset = 0;
        libglusterfs_client_fd_ctx_t *fd_ctx = NULL;

        fd_ctx = libgf_get_fd_ctx (fd);
        if (!fd_ctx) {
                errno = EBADF;
		goto out;
        }

        pthread_mutex_lock (&fd_ctx->lock);
        {
                ctx = fd_ctx->ctx;
                offset = fd_ctx->offset;
        }
        pthread_mutex_unlock (&fd_ctx->lock);

        op_ret = libgf_client_readdir (ctx, (fd_t *)fd, dirp, &offset);

        if (op_ret > 0) {
                pthread_mutex_lock (&fd_ctx->lock);
                {
                        fd_ctx->offset = offset;
                }
                pthread_mutex_unlock (&fd_ctx->lock);
        }

out:
        return op_ret;
}


static int32_t
libglusterfs_readv_async_cbk (call_frame_t *frame,
                              void *cookie,
                              xlator_t *this,
                              int32_t op_ret,
                              int32_t op_errno,
                              struct iovec *vector,
                              int32_t count,
                              struct iatt *stbuf,
                              struct iobref *iobref)
{
        glusterfs_iobuf_t *buf;
        libglusterfs_client_async_local_t *local = frame->local;
        fd_t *__fd = local->fop.readv_cbk.fd;
        glusterfs_readv_cbk_t readv_cbk = local->fop.readv_cbk.cbk;

        buf = CALLOC (1, sizeof (*buf));
        ERR_ABORT (buf);

	if (vector) {
		buf->vector = iov_dup (vector, count);
	}

        buf->count = count;

	if (iobref) {
		buf->iobref = iobref_ref (iobref);
	}

        if (op_ret > 0) {
                libglusterfs_client_fd_ctx_t *fd_ctx = NULL;
                fd_ctx = libgf_get_fd_ctx (__fd);
                
                /* update offset only if we have used offset stored in fd_ctx */
                if (local->fop.readv_cbk.update_offset) {
                        pthread_mutex_lock (&fd_ctx->lock);
                        {
                                fd_ctx->offset += op_ret;
                        }
                        pthread_mutex_unlock (&fd_ctx->lock);
                }
        }

        readv_cbk (op_ret, op_errno, buf, local->cbk_data); 

	FREE (local);
	frame->local = NULL;
        STACK_DESTROY (frame->root);

        return 0;
}

void 
glusterfs_free (glusterfs_iobuf_t *buf)
{
        //iov_free (buf->vector, buf->count);
        FREE (buf->vector);
        if (buf->iobref)
                iobref_unref ((struct iobref *) buf->iobref);
        if (buf->dictref)
                dict_unref ((dict_t *) buf->dictref);
        FREE (buf);
}

int
glusterfs_read_async (glusterfs_file_t fd, 
                      size_t nbytes, 
                      off_t offset,
                      glusterfs_readv_cbk_t readv_cbk,
                      void *cbk_data)
{
        libglusterfs_client_ctx_t *ctx;
        fd_t *__fd = (fd_t *)fd;
        libglusterfs_client_async_local_t *local = NULL;
        libglusterfs_client_fd_ctx_t *fd_ctx = NULL;
	int32_t op_ret = 0;

        if (nbytes < 0) {
                errno = EINVAL;
                op_ret = -1;
                goto out;
        }

        if (nbytes == 0) {
                op_ret = 0;
                goto out;
        }

        local = CALLOC (1, sizeof (*local));
        ERR_ABORT (local);
        local->fop.readv_cbk.fd = __fd;
        local->fop.readv_cbk.cbk = readv_cbk;
        local->cbk_data = cbk_data;

        fd_ctx = libgf_get_fd_ctx (fd);
        if (!fd_ctx) {
                errno = EBADF;
		op_ret = -1;
		goto out;
        }

        ctx = fd_ctx->ctx;

        if (offset < 0) {
                pthread_mutex_lock (&fd_ctx->lock);
                {
                        offset = fd_ctx->offset;
                        local->fop.readv_cbk.update_offset = 1;
                }
                pthread_mutex_unlock (&fd_ctx->lock);
        }

        LIBGF_CLIENT_FOP_ASYNC (ctx,
                                local,
                                libglusterfs_readv_async_cbk,
                                readv,
                                __fd,
                                nbytes,
                                offset);

out:
        return op_ret;
}

static int32_t
libglusterfs_writev_async_cbk (call_frame_t *frame,
                               void *cookie,
                               xlator_t *this,
                               int32_t op_ret,
                               int32_t op_errno,
                               struct iatt *prebuf,
                               struct iatt *postbuf)
{
        libglusterfs_client_async_local_t *local = frame->local;
        fd_t *fd = NULL;
        glusterfs_write_cbk_t write_cbk;

        write_cbk = local->fop.write_cbk.cbk;
        fd = local->fop.write_cbk.fd;

        if (op_ret > 0) {
                libglusterfs_client_fd_ctx_t *fd_ctx = NULL;
                fd_ctx = libgf_get_fd_ctx (fd);
                pthread_mutex_lock (&fd_ctx->lock);
                {
                        fd_ctx->offset += op_ret;  
                }
                pthread_mutex_unlock (&fd_ctx->lock);
        }

        write_cbk (op_ret, op_errno, local->cbk_data);

        STACK_DESTROY (frame->root);
        return 0;
}

int32_t
glusterfs_write_async (glusterfs_file_t fd, 
                       const void *buf, 
                       size_t nbytes, 
                       off_t offset,
                       glusterfs_write_cbk_t write_cbk,
                       void *cbk_data)
{
        fd_t *__fd = (fd_t *)fd;
        struct iovec vector;
        off_t __offset = offset;
        libglusterfs_client_ctx_t *ctx = NULL;
        libglusterfs_client_async_local_t *local = NULL;
        libglusterfs_client_fd_ctx_t *fd_ctx = NULL;
	int32_t op_ret = 0;
        struct iobref *iobref = NULL;

        if (nbytes == 0) {
                op_ret = 0;
                goto out;
        }

        if (nbytes < 0) {
                op_ret = -1;
                errno = EINVAL;
                goto out;
        }

        local = CALLOC (1, sizeof (*local));
        ERR_ABORT (local);
        local->fop.write_cbk.fd = __fd;
        local->fop.write_cbk.cbk = write_cbk;
        local->cbk_data = cbk_data;

        vector.iov_base = (void *)buf;
        vector.iov_len = nbytes;
  
        fd_ctx = libgf_get_fd_ctx (fd);
        if (!fd_ctx) {
                errno = EBADF;
		op_ret = -1;
		goto out;
        }

        ctx = fd_ctx->ctx;
 
        if (offset < 0) {
                pthread_mutex_lock (&fd_ctx->lock);
                {
                        __offset = fd_ctx->offset;
                }
                pthread_mutex_unlock (&fd_ctx->lock);
        }

        iobref = iobref_new ();
        LIBGF_CLIENT_FOP_ASYNC (ctx,
                                local,
                                libglusterfs_writev_async_cbk,
                                writev,
                                __fd,
                                &vector,
                                1,
                                __offset,
                                iobref);
        iobref_unref (iobref);

out:
        return op_ret;
}

off_t
glusterfs_lseek (glusterfs_file_t fd, off_t offset, int whence)
{
        off_t __offset = 0;
	int32_t op_ret = -1;
        fd_t *__fd = (fd_t *)fd;
        libglusterfs_client_fd_ctx_t *fd_ctx = NULL;
	libglusterfs_client_ctx_t *ctx = NULL; 

        fd_ctx = libgf_get_fd_ctx (fd);
        if (!fd_ctx) {
                errno = EBADF;
		__offset = -1;
		goto out;
        }

	ctx = fd_ctx->ctx;

        switch (whence)
        {
        case SEEK_SET:
                __offset = offset;
                break;

        case SEEK_CUR:
                pthread_mutex_lock (&fd_ctx->lock);
                {
                        __offset = fd_ctx->offset;
                }
                pthread_mutex_unlock (&fd_ctx->lock);

                __offset += offset;
                break;

        case SEEK_END:
	{
		char cache_valid = 0;
		off_t end = 0;
		loc_t loc = {0, };
		struct iatt stbuf = {0, };

                cache_valid = libgf_is_iattr_cache_valid (ctx, __fd->inode,
                                                          &stbuf,
                                                          LIBGF_VALIDATE_STAT);
                if (cache_valid) {
			end = stbuf.ia_size;
		} else {
			op_ret = libgf_client_loc_fill (&loc, ctx,
                                                        __fd->inode->ino, 0,
                                                        NULL);
			if (op_ret == -1) {
				gf_log ("libglusterfsclient",
					GF_LOG_ERROR,
					"libgf_client_loc_fill returned -1, returning EINVAL");
				errno = EINVAL;
				libgf_client_loc_wipe (&loc);
				__offset = -1;
				goto out;
			}
			
			op_ret = libgf_client_lookup (ctx, &loc, &stbuf, NULL,
                                                      NULL);
			if (op_ret < 0) {
				__offset = -1;
				libgf_client_loc_wipe (&loc);
				goto out;
			}

			end = stbuf.ia_size;
		}

                __offset = end + offset; 
		libgf_client_loc_wipe (&loc);
	}
	break;

	default:
		gf_log ("libglusterfsclient",
			GF_LOG_ERROR,
			"invalid value for whence");
		__offset = -1;
		errno = EINVAL;
		goto out;
        }

        pthread_mutex_lock (&fd_ctx->lock);
        {
                fd_ctx->offset = __offset;
        }
        pthread_mutex_unlock (&fd_ctx->lock);
 
out: 
        return __offset;
}


int32_t
libgf_client_stat_cbk (call_frame_t *frame,
                       void *cookie,
                       xlator_t *this,
                       int32_t op_ret,
                       int32_t op_errno,
                       struct iatt *buf)
{
        libgf_client_local_t *local = frame->local;

        local->reply_stub = fop_stat_cbk_stub (frame, 
                                               NULL, 
                                               op_ret, 
                                               op_errno, 
                                               buf);

        LIBGF_REPLY_NOTIFY (local);
        return 0;
}

int32_t 
libgf_client_stat (libglusterfs_client_ctx_t *ctx, 
                   loc_t *loc,
                   struct iatt *stbuf)
{
        call_stub_t *stub = NULL;
        int32_t op_ret = 0;
        libgf_client_local_t *local = NULL;
        struct iatt cachedbuf = {0, };

        if (libgf_is_iattr_cache_valid (ctx, loc->inode, &cachedbuf,
                                        LIBGF_VALIDATE_STAT)) {
                gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "Cache will be used");
                if (stbuf)
                        memcpy (stbuf, &cachedbuf, sizeof (struct stat));
                goto out;
        }

        LIBGF_CLIENT_FOP (ctx, stub, stat, local, loc);
 
        op_ret = stub->args.stat_cbk.op_ret;
        errno = stub->args.stat_cbk.op_errno;
        libgf_transform_iattr (ctx, loc->inode, &stub->args.stat_cbk.buf);

        gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "path %s, status %d, errno %d",
                loc->path, op_ret, errno);

        if (op_ret == 0) {
                if (stbuf)
                        *stbuf = stub->args.stat_cbk.buf;

                libgf_update_iattr_cache (loc->inode, LIBGF_UPDATE_STAT,
                                          &stub->args.stat_cbk.buf);
        }

	call_stub_destroy (stub);

out:
        return op_ret;
}

int
libgf_realpath_loc_fill (libglusterfs_client_ctx_t *ctx, char *link,
                                loc_t *targetloc)
{
        int             op_ret = -1;
        char            *target = NULL;

        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, ctx, out);
        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, link, out);
        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, targetloc, out);

        targetloc->path = glusterfs_glh_realpath (ctx, link, NULL);

        if (targetloc->path == NULL)
                goto out;

        op_ret = libgf_client_path_lookup (targetloc, ctx, 1);
        if (op_ret == -1)
                goto out;

        target = strdup (targetloc->path);
        op_ret = libgf_client_loc_fill (targetloc, ctx, 0,
                                               targetloc->parent->ino,
                                               compat_basename (target));
        if (op_ret == -1) {
                errno = EINVAL;
                goto out;
        }

out:
        if (target)
                FREE (target);

        return op_ret;
}

#define LIBGF_DO_LSTAT  0x01
#define LIBGF_DO_STAT   0x02

int
__glusterfs_stat (glusterfs_handle_t handle, const char *path,
                  struct stat *buf, int whichstat)
{
        int32_t op_ret = -1;
        loc_t loc = {0, };
        libglusterfs_client_ctx_t *ctx = handle;
	char *name = NULL, *pathname = NULL;
        loc_t targetloc = {0, };
        loc_t *real_loc = NULL;

        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, ctx, out);
        GF_VALIDATE_ABSOLUTE_PATH_OR_GOTO (LIBGF_XL_NAME, path, out);

        gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "path %s, op: %d", path,
                whichstat);

        loc.path = strdup (path);
        if (!loc.path) {
                gf_log (LIBGF_XL_NAME, GF_LOG_ERROR, "strdup failed");
                goto out;
        }

	op_ret = libgf_client_path_lookup (&loc, ctx, 1);
	if (op_ret == -1) {
		gf_log ("libglusterfsclient", GF_LOG_ERROR,
			"path lookup failed for (%s)", loc.path);
		goto out;
	}

	pathname = strdup (loc.path);
	name = compat_basename (pathname);

        op_ret = libgf_client_loc_fill (&loc, ctx, 0, loc.parent->ino, name);
	if (op_ret == -1) {
		gf_log ("libglusterfsclient",
			GF_LOG_ERROR,
			"libgf_client_loc_fill returned -1, returning EINVAL");
		errno = EINVAL;
		goto out;
	}
        real_loc = &loc;
        /* The stat fop in glusterfs calls lstat. So we have to
         * provide the POSIX compatible stat fop. To do so, we need to ensure
         * that if the @path is a symlink, we must perform a stat on the
         * target of that symlink than the symlink itself(..because if
         * do a stat on the symlink, we're actually doing what lstat
         * should do. See posix_stat
         */
        if (whichstat & LIBGF_DO_LSTAT)
                goto lstat_fop;

        if (!IA_ISLNK (loc.inode->ia_type))
                goto lstat_fop;

        op_ret = libgf_realpath_loc_fill (ctx, (char *)loc.path, &targetloc);
        if (op_ret == -1)
                goto out;
        real_loc = &targetloc;

lstat_fop:

        if (!op_ret) {
                struct iatt iatt;
                op_ret = libgf_client_stat (ctx, real_loc, &iatt);
                iatt_to_stat (&iatt, buf);
        }

out:
	if (pathname) {
		FREE (pathname);
	}

        libgf_client_loc_wipe (&loc);
        libgf_client_loc_wipe (&targetloc);

        return op_ret;
}

int
glusterfs_glh_stat (glusterfs_handle_t handle, const char *path,
                    struct stat *buf)
{
        return __glusterfs_stat (handle, path, buf, LIBGF_DO_STAT);
}

int
glusterfs_stat (const char *path, struct stat *buf)
{
        glusterfs_handle_t      h      = NULL;
        int                     op_ret = -1;
        char                    vpath[PATH_MAX];

        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, path, out);
        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, buf, out);

        gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "path %s", path);

        h = libgf_resolved_path_handle (path, vpath);
        if (!h) {
                errno = ENODEV;
                goto out;
        }

        op_ret = glusterfs_glh_stat (h, vpath, buf);

out:
        return op_ret;
}

int
glusterfs_glh_lstat (glusterfs_handle_t handle, const char *path, struct stat *buf)
{
        return __glusterfs_stat (handle, path, buf, LIBGF_DO_LSTAT);
}

int
glusterfs_lstat (const char *path, struct stat *buf)
{
        glusterfs_handle_t      h      = NULL;
        int                     op_ret = -1;
        char                    vpath[PATH_MAX];

        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, path, out);
        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, buf, out);

        gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "path %s", path);

        h = libgf_resolved_path_handle (path, vpath);
        if (!h) {
                errno = ENODEV;
                goto out;
        }

        op_ret = glusterfs_glh_lstat (h, vpath, buf);
out:
        return op_ret;
}

static int32_t
libgf_client_fstat_cbk (call_frame_t *frame,
                        void *cookie,
                        xlator_t *this,
                        int32_t op_ret,
                        int32_t op_errno,
                        struct iatt *buf)
{  
        libgf_client_local_t *local = frame->local;

        local->reply_stub = fop_fstat_cbk_stub (frame, 
                                                NULL, 
                                                op_ret, 
                                                op_errno, 
                                                buf);

        LIBGF_REPLY_NOTIFY (local);
        return 0;

}

int32_t
libgf_client_fstat (libglusterfs_client_ctx_t *ctx, 
                    fd_t *fd, 
                    struct stat *buf)
{
        call_stub_t *stub = NULL;
        int32_t op_ret = 0;
        libgf_client_local_t *local = NULL;
        struct iatt cachedbuf = {0, };

        if (libgf_is_iattr_cache_valid (ctx, fd->inode, &cachedbuf,
                                        LIBGF_VALIDATE_STAT)) {
                if (buf)
                        memcpy (buf, &cachedbuf, sizeof (struct stat));
                gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "Cache will be used");
                goto out;
        }

        LIBGF_CLIENT_FOP (ctx, stub, fstat, local, fd);
 
        op_ret = stub->args.fstat_cbk.op_ret;
        errno = stub->args.fstat_cbk.op_errno;
        gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "status %d, errno %d", op_ret,
                errno);

        if (op_ret == 0) {
                libgf_transform_iattr (ctx, fd->inode,
                                       &stub->args.fstat_cbk.buf);
                if (buf)
                        iatt_to_stat (&stub->args.fstat_cbk.buf, buf);
                libgf_update_iattr_cache (fd->inode, LIBGF_UPDATE_STAT,
                                          &stub->args.fstat_cbk.buf);
        }
	call_stub_destroy (stub);

out:
        return op_ret;
}

int32_t 
glusterfs_fstat (glusterfs_file_t fd, struct stat *buf) 
{
        libglusterfs_client_ctx_t *ctx;
        fd_t *__fd = (fd_t *)fd;
        libglusterfs_client_fd_ctx_t *fd_ctx = NULL;
	int32_t op_ret = -1;

        fd_ctx = libgf_get_fd_ctx (fd);
        if (!fd_ctx) {
                gf_log (LIBGF_XL_NAME, GF_LOG_ERROR, "No fd context present");
                errno = EBADF;
		op_ret = -1;
		goto out;
        }

        ctx = fd_ctx->ctx;

	op_ret = libgf_client_fstat (ctx, __fd, buf);

out:
	return op_ret;
}


static int32_t
libgf_client_mkdir_cbk (call_frame_t *frame,
			void *cookie,
			xlator_t *this,
			int32_t op_ret,
			int32_t op_errno,
			inode_t *inode,
                        struct iatt *buf,
                        struct iatt *preparent,
                        struct iatt *postparent)
{
        libgf_client_local_t *local = frame->local;

        local->reply_stub = fop_mkdir_cbk_stub (frame, NULL, op_ret, op_errno,
                                                inode, buf, preparent,
                                                postparent);

        LIBGF_REPLY_NOTIFY (local);
        return 0;
}


static int32_t
libgf_client_mkdir (libglusterfs_client_ctx_t *ctx,
		    loc_t *loc,
		    mode_t mode)
{
	int32_t op_ret = -1;
        call_stub_t *stub = NULL;
        libgf_client_local_t *local = NULL;
        inode_t *libgf_inode = NULL;

        LIBGF_CLIENT_FOP (ctx, stub, mkdir, local, loc, mode);
        op_ret = stub->args.mkdir_cbk.op_ret;
        errno = stub->args.mkdir_cbk.op_errno;

        gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "path %s, status %d, errno %d",
                loc->path, op_ret, errno);
        if (op_ret == -1)
                goto out;

	libgf_inode = stub->args.mkdir_cbk.inode;
        inode_link (libgf_inode, loc->parent, loc->name,
                        &stub->args.mkdir_cbk.buf);
        libgf_transform_iattr (ctx, libgf_inode, &stub->args.mkdir_cbk.buf);

        inode_lookup (libgf_inode);

        libgf_alloc_inode_ctx (ctx, libgf_inode);
        libgf_update_iattr_cache (libgf_inode, LIBGF_UPDATE_ALL,
                                        &stub->args.mkdir_cbk.buf);

out:
	call_stub_destroy (stub);

	return op_ret;
}


int32_t
glusterfs_glh_mkdir (glusterfs_handle_t handle, const char *path, mode_t mode)
{
	libglusterfs_client_ctx_t *ctx = handle;
	loc_t loc = {0, };
	char *pathname = NULL, *name = NULL;
	int32_t op_ret = -1;

        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, ctx, out);
        GF_VALIDATE_ABSOLUTE_PATH_OR_GOTO (LIBGF_XL_NAME, path, out);

        loc.path = strdup (path);
        if (!loc.path) {
                gf_log (LIBGF_XL_NAME, GF_LOG_ERROR, "strdup failed");
                goto out;
        }

	op_ret = libgf_client_path_lookup (&loc, ctx, 1);
	if (op_ret == 0) {
                op_ret = -1;
                errno = EEXIST;
		goto out;
	}

        op_ret = libgf_client_path_lookup (&loc, ctx, 0);
        if (op_ret == -1) {
                errno = ENOENT;
                goto out;
        }

	pathname = strdup (loc.path);
	name = compat_basename (pathname);

        op_ret = libgf_client_loc_fill (&loc, ctx, 0, loc.parent->ino, name);
	if (op_ret == -1) {
		gf_log ("libglusterfsclient",
			GF_LOG_ERROR,
			"libgf_client_loc_fill returned -1, returning EINVAL");
		errno = EINVAL;
		goto out;
	}

        loc.inode = inode_new (ctx->itable);
	op_ret = libgf_client_mkdir (ctx, &loc, mode); 
	if (op_ret == -1) {
		goto out;
	}

out:
	libgf_client_loc_wipe (&loc);
	if (pathname) {
		free (pathname);
		pathname = NULL;
	}

	return op_ret;
}

int32_t
glusterfs_mkdir (const char *path, mode_t mode)
{
        int                     op_ret = -1;
        char                    vpath[PATH_MAX];
        glusterfs_handle_t      h = NULL;

        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, path, out);

        gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "path %s", path);

        h = libgf_resolved_path_handle (path, vpath);
        if (!h) {
                errno = ENODEV;
                goto out;
        }

        op_ret = glusterfs_glh_mkdir (h, vpath, mode);
out:
        return op_ret;
}

static int32_t
libgf_client_rmdir_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                        int32_t op_ret, int32_t op_errno,struct iatt *preparent,
                        struct iatt *postparent)
{
        libgf_client_local_t *local = frame->local;

        local->reply_stub = fop_rmdir_cbk_stub (frame, NULL, op_ret, op_errno,
                                                preparent, postparent);

        LIBGF_REPLY_NOTIFY (local);
        return 0;
}

static int32_t
libgf_client_rmdir (libglusterfs_client_ctx_t *ctx, loc_t *loc)
{
        int32_t op_ret = -1;
        call_stub_t *stub = NULL;
        libgf_client_local_t *local = NULL;

        LIBGF_CLIENT_FOP (ctx, stub, rmdir, local, loc);

        op_ret = stub->args.rmdir_cbk.op_ret;
        errno = stub->args.rmdir_cbk.op_errno;

        gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "path %s, status %d, errno %d",
                loc->path, op_ret, errno);
        if (stub->args.rmdir_cbk.op_ret != 0)
                goto out;

        inode_unlink (loc->inode, loc->parent, loc->name);

out:
	call_stub_destroy (stub);

	return op_ret;
}

int32_t
glusterfs_glh_rmdir (glusterfs_handle_t handle, const char *path)
{
	libglusterfs_client_ctx_t *ctx = handle;
	loc_t loc = {0, };
	char *pathname = NULL, *name = NULL;
	int32_t op_ret = -1;

        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, ctx, out);
        GF_VALIDATE_ABSOLUTE_PATH_OR_GOTO (LIBGF_XL_NAME, path, out);

        loc.path = libgf_resolve_path_light ((char *)path);
        if (!loc.path) {
                gf_log (LIBGF_XL_NAME, GF_LOG_ERROR, "Path compaction failed");
                goto out;
        }

	op_ret = libgf_client_path_lookup (&loc, ctx, 1);
	if (op_ret == -1) {
		gf_log ("libglusterfsclient", GF_LOG_ERROR,
			"path lookup failed for (%s)", loc.path);
		goto out;
	}

	pathname = strdup (loc.path);
	name = compat_basename (pathname);

        op_ret = libgf_client_loc_fill (&loc, ctx, 0, loc.parent->ino, name);
	if (op_ret == -1) {
		gf_log ("libglusterfsclient",
			GF_LOG_ERROR,
			"libgf_client_loc_fill returned -1, returning EINVAL");
		errno = EINVAL;
		goto out;
	}

	op_ret = libgf_client_rmdir (ctx, &loc);
	if (op_ret == -1) {
		goto out;
	}

out:
	libgf_client_loc_wipe (&loc);

	if (pathname) {
		free (pathname);
		pathname = NULL;
	}

	return op_ret;
}

int32_t
glusterfs_rmdir (const char *path)
{
        int                     op_ret = -1;
        char                    vpath[PATH_MAX];
        glusterfs_handle_t      h = NULL;

        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, path, out);

        gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "path %s", path);

        h = libgf_resolved_path_handle (path, vpath);
        if (!h) {
                errno = ENODEV;
                goto out;
        }

        op_ret = glusterfs_glh_rmdir (h, vpath);
out:
        return op_ret;
}

int
libgf_client_setattr_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                          int32_t op_ret, int32_t op_errno,
                          struct iatt *preop, struct iatt *postop)
{
        libgf_client_local_t *local = frame->local;

        local->reply_stub = fop_setattr_cbk_stub (frame, NULL,
                                                  op_ret, op_errno,
                                                  preop, postop);

        LIBGF_REPLY_NOTIFY (local);
        return 0;
}

int
libgf_client_setattr (libglusterfs_client_ctx_t *ctx, loc_t * loc,
                      struct iatt *stbuf, int32_t valid)
{
        int                             op_ret = -1;
        libgf_client_local_t            *local = NULL;
        call_stub_t                     *stub = NULL;

        LIBGF_CLIENT_FOP (ctx, stub, setattr, local, loc,
                          stbuf, valid);

        op_ret = stub->args.setattr_cbk.op_ret;
        errno = stub->args.setattr_cbk.op_errno;

        if (op_ret == -1)
                goto out;

        libgf_transform_iattr (ctx, loc->inode,
                               &stub->args.setattr_cbk.statpost);
        libgf_update_iattr_cache (loc->inode, LIBGF_UPDATE_STAT,
                                  &stub->args.setattr_cbk.statpost);
out:
        call_stub_destroy (stub);
        return op_ret;
}


int
glusterfs_glh_chmod (glusterfs_handle_t handle, const char *path, mode_t mode)
{
        int                             op_ret = -1;
        libglusterfs_client_ctx_t       *ctx = handle;
        loc_t                           loc = {0, };
        char                            *name = NULL;
        struct iatt                     stbuf = {0,};
        int32_t                         valid = 0;

        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, ctx, out);
        GF_VALIDATE_ABSOLUTE_PATH_OR_GOTO (LIBGF_XL_NAME, path, out);

        stbuf.ia_prot = ia_prot_from_st_mode (mode);
        valid |= GF_SET_ATTR_MODE;

        loc.path = strdup (path);
        if (!loc.path) {
                gf_log (LIBGF_XL_NAME, GF_LOG_ERROR, "strdup failed");
                goto out;
        }

        op_ret = libgf_client_path_lookup (&loc, ctx, 1);
        if (op_ret == -1)
                goto out;

        name = strdup (loc.path);
        op_ret = libgf_client_loc_fill (&loc, ctx, 0, loc.parent->ino,
                                                compat_basename (name));
        if (op_ret == -1) {
                errno = EINVAL;
                goto out;
        }

        op_ret = libgf_client_setattr (ctx, &loc, &stbuf, valid);

out:
        if (name)
                FREE (name);

        libgf_client_loc_wipe (&loc);
        return op_ret;
}

int
glusterfs_chmod (const char *path, mode_t mode)
{
        int                     op_ret = -1;
        char                    vpath[PATH_MAX];
        glusterfs_handle_t      h = NULL;

        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, path, out);

        gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "path %s", path);

        h = libgf_resolved_path_handle (path, vpath);
        if (!h) {
                errno = ENODEV;
                goto out;
        }

        op_ret = glusterfs_glh_chmod (h, vpath, mode);
out:
        return op_ret;
}


#define LIBGF_DO_CHOWN  1
#define LIBGF_DO_LCHOWN 2

int
__glusterfs_chown (glusterfs_handle_t handle, const char *path, uid_t owner,
                   gid_t group, int whichop)
{
        int                             op_ret = -1;
        libglusterfs_client_ctx_t       *ctx = handle;
        loc_t                           loc = {0, };
        char                            *name = NULL;
        loc_t                           *oploc = NULL;
        loc_t                           targetloc = {0, };
        struct iatt                     stbuf = {0,};
        int32_t                         valid = 0;

        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, ctx, out);
        GF_VALIDATE_ABSOLUTE_PATH_OR_GOTO (LIBGF_XL_NAME, path, out);

        gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "path %s, op %d", path, whichop);
        stbuf.ia_uid = owner;
        stbuf.ia_gid = group;
        valid |= (GF_SET_ATTR_UID | GF_SET_ATTR_GID);

        loc.path = strdup (path);
        if (!loc.path) {
                gf_log (LIBGF_XL_NAME, GF_LOG_ERROR, "strdup failed");
                goto out;
        }

        op_ret = libgf_client_path_lookup (&loc, ctx, 1);
        if (op_ret == -1)
                goto out;

        name = strdup (loc.path);
        op_ret = libgf_client_loc_fill (&loc, ctx, 0, loc.parent->ino,
                        compat_basename ((char *)name));
        if (op_ret == -1) {
                errno = EINVAL;
                goto out;
        }

        oploc = &loc;
        if (whichop == LIBGF_DO_LCHOWN)
                goto do_lchown;

        if (!IA_ISLNK (loc.inode->ia_type))
                goto do_lchown;

        op_ret = libgf_realpath_loc_fill (ctx, (char *)loc.path, &targetloc);
        if (op_ret == -1)
                goto out;

        oploc = &targetloc;
do_lchown:
        op_ret = libgf_client_setattr (ctx, oploc, &stbuf, valid);
out:
        if (name)
                FREE (name);
        libgf_client_loc_wipe (&loc);
        libgf_client_loc_wipe (&targetloc);
        return op_ret;
}

int
glusterfs_glh_chown (glusterfs_handle_t handle, const char *path, uid_t owner,
                     gid_t group)
{
        return __glusterfs_chown (handle, path, owner, group, LIBGF_DO_CHOWN);
}

int
glusterfs_chown (const char *path, uid_t owner, gid_t group)
{
        int                     op_ret = -1;
        char                    vpath[PATH_MAX];
        glusterfs_handle_t      h = NULL;

        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, path, out);

        gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "path %s", path);

        h = libgf_resolved_path_handle (path, vpath);
        if (!h) {
                errno = ENODEV;
                goto out;
        }

        op_ret = glusterfs_glh_chown (h, vpath, owner, group);

out:
        return op_ret;
}

int
glusterfs_glh_lchown (glusterfs_handle_t handle, const char *path, uid_t owner,
                     gid_t group)
{
        return __glusterfs_chown (handle, path, owner, group, LIBGF_DO_LCHOWN);
}

int
glusterfs_lchown (const char *path, uid_t owner, gid_t group)
{
        int                     op_ret = -1;
        char                    vpath[PATH_MAX];
        glusterfs_handle_t      h = NULL;

        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, path, out);

        gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "path %s", path);

        h = libgf_resolved_path_handle (path, vpath);
        if (!h) {
                errno = ENODEV;
                goto out;
        }

        op_ret = glusterfs_glh_lchown (h, vpath, owner, group);
out:
        return op_ret;
}

glusterfs_dir_t
glusterfs_glh_opendir (glusterfs_handle_t handle, const char *path)
{
        int                             op_ret = -1;
        libglusterfs_client_ctx_t       *ctx = handle;
        loc_t                           loc = {0, };
        fd_t                            *dirfd = NULL;
        char                            *name = NULL;

        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, ctx, out);
        GF_VALIDATE_ABSOLUTE_PATH_OR_GOTO (LIBGF_XL_NAME, path, out);

        gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "path %s", path);

        loc.path = strdup (path);
        if (!loc.path) {
                gf_log (LIBGF_XL_NAME, GF_LOG_ERROR, "strdup failed");
                goto out;
        }
        op_ret = libgf_client_path_lookup (&loc, ctx, 1);

        if (op_ret == -1)
                goto out;

        name = strdup (loc.path);
        op_ret = libgf_client_loc_fill (&loc, ctx, 0, loc.parent->ino,
                        compat_basename (name));
        if (op_ret == -1) {
                errno = EINVAL;
                goto out;
        }

        if (!IA_ISDIR (loc.inode->ia_type) && !IA_ISLNK (loc.inode->ia_type)) {
                errno = ENOTDIR;
                op_ret = -1;
                goto out;
        }

        dirfd = fd_create (loc.inode, ctx->pid);
        op_ret = libgf_client_opendir (ctx, &loc, dirfd);

        if (op_ret == -1) {
                fd_unref (dirfd);
                dirfd = NULL;
                goto out;
        }

        if (!libgf_get_fd_ctx (dirfd)) {
                if (!(libgf_alloc_fd_ctx (ctx, dirfd, (char *)path))) {
                        gf_log (LIBGF_XL_NAME, GF_LOG_ERROR, "Context "
                                "allocation failed");
                        op_ret = -1;
                        errno = EINVAL;
                        goto out;
                }
        }

out:
        if (name)
                FREE (name);

        if (op_ret == -1) {
                fd_unref (dirfd);
                dirfd = NULL;
        }

        libgf_client_loc_wipe (&loc);
        return dirfd;
}

glusterfs_dir_t
glusterfs_opendir (const char *path)
{
        char                    vpath[PATH_MAX];
        glusterfs_dir_t         dir = NULL;
        glusterfs_handle_t      h = NULL;

        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, path, out);
        gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "path %s", path);

        h = libgf_resolved_path_handle (path, vpath);
        if (!h) {
                errno = ENODEV;
                goto out;
        }

        dir = glusterfs_glh_opendir (h, vpath);
out:
        return dir;
}

int
glusterfs_closedir (glusterfs_dir_t dirfd)
{
        int                             op_ret = -1;
        libglusterfs_client_fd_ctx_t    *fdctx = NULL;

        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, dirfd, out);
        fdctx = libgf_get_fd_ctx (dirfd);

        if (fdctx == NULL) {
                errno = EBADF;
                op_ret = -1;
                goto out;
        }

        op_ret = libgf_client_flush (fdctx->ctx, (fd_t *)dirfd);
        fd_unref ((fd_t *)dirfd);

out:
        return op_ret;
}


int
libgf_client_fsetattr_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                           int32_t op_ret, int32_t op_errno,
                           struct iatt *preop, struct iatt *postop)
{
        libgf_client_local_t    *local = frame->local;

        local->reply_stub = fop_fsetattr_cbk_stub (frame, NULL,
                                                   op_ret, op_errno,
                                                   preop, postop);
        LIBGF_REPLY_NOTIFY (local);
        return 0;
}


int
libgf_client_fsetattr (libglusterfs_client_ctx_t *ctx, fd_t *fd,
                       struct iatt *stbuf, int32_t valid)
{
        int                     op_ret = -1;
        libgf_client_local_t    *local = NULL;
        call_stub_t             *stub = NULL;

        LIBGF_CLIENT_FOP (ctx, stub, fsetattr, local, fd, stbuf, valid);

        op_ret = stub->args.fsetattr_cbk.op_ret;
        errno = stub->args.fsetattr_cbk.op_errno;

        if (op_ret == -1)
                goto out;

        libgf_transform_iattr (ctx, fd->inode,
                               &stub->args.fsetattr_cbk.statpost);
        libgf_update_iattr_cache (fd->inode, LIBGF_UPDATE_STAT,
                                  &stub->args.fsetattr_cbk.statpost);
out:
        call_stub_destroy (stub);
        return op_ret;
}


int
glusterfs_fchmod (glusterfs_file_t fd, mode_t mode)
{
        libglusterfs_client_fd_ctx_t    *fdctx = NULL;
        int                             op_ret = -1;
        struct iatt                     stbuf = {0,};
        int32_t                         valid = 0;

        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, fd, out);
        fdctx = libgf_get_fd_ctx (fd);

        if (!fdctx) {
                gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "No fd context present");
                errno = EBADF;
                goto out;
        }

        stbuf.ia_prot = ia_prot_from_st_mode (mode);
        valid |= GF_SET_ATTR_MODE;

        op_ret = libgf_client_fsetattr (fdctx->ctx, fd, &stbuf, valid);
out:
        return op_ret;
}


int
glusterfs_fchown (glusterfs_file_t fd, uid_t uid, gid_t gid)
{
        int                             op_ret = -1;
        libglusterfs_client_fd_ctx_t    *fdctx = NULL;
        struct iatt                     stbuf = {0,};
        int32_t                         valid = 0;

        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, fd, out);

        fdctx = libgf_get_fd_ctx (fd);
        if (!fd) {
                gf_log (LIBGF_XL_NAME, GF_LOG_ERROR, "No fd context present");
                errno = EBADF;
                goto out;
        }
        stbuf.ia_uid = uid;
        stbuf.ia_gid = gid;

        valid |= (GF_SET_ATTR_UID | GF_SET_ATTR_GID);

        op_ret = libgf_client_fsetattr (fdctx->ctx, fd, &stbuf, valid);

out:
        return op_ret;
}

int
libgf_client_fsync_cbk (call_frame_t *frame, void *cookie, xlator_t *xlator,
                        int32_t op_ret, int32_t op_errno, struct iatt *prebuf,
                        struct iatt *postbuf)
{
        libgf_client_local_t    *local = frame->local;

        local->reply_stub = fop_fsync_cbk_stub (frame, NULL, op_ret, op_errno,
                                                prebuf, postbuf);

        LIBGF_REPLY_NOTIFY (local);

        return 0;
}

int
libgf_client_fsync (libglusterfs_client_ctx_t *ctx, fd_t *fd)
{
        libgf_client_local_t    *local = NULL;
        call_stub_t             *stub = NULL;
        int                     op_ret = -1;

        LIBGF_CLIENT_FOP (ctx, stub, fsync, local, fd, 0);

        op_ret = stub->args.fsync_cbk.op_ret;
        errno = stub->args.fsync_cbk.op_errno;

        gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "status %d, errno %d", op_ret,
                errno);
        call_stub_destroy (stub);

        return op_ret;
}

int
glusterfs_fsync (glusterfs_file_t *fd)
{
        libglusterfs_client_fd_ctx_t    *fdctx = NULL;
        int                             op_ret = -1;

        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, fd, out);

        fdctx = libgf_get_fd_ctx ((fd_t *)fd);
        if (!fdctx) {
                gf_log (LIBGF_XL_NAME, GF_LOG_ERROR, "No fd context present");
                errno = EBADF;
                goto out;
        }

        op_ret = libgf_client_fsync (fdctx->ctx, (fd_t *)fd);

out:
        return op_ret;
}

int
libgf_client_ftruncate_cbk (call_frame_t *frame, void *cookie, xlator_t *xlator
                                ,int32_t op_ret, int32_t op_errno,
                                struct iatt *prebuf, struct iatt *postbuf)
{
        libgf_client_local_t    *local = frame->local;

        local->reply_stub = fop_ftruncate_cbk_stub (frame, NULL, op_ret,
                                                    op_errno, prebuf, postbuf);

        LIBGF_REPLY_NOTIFY (local);

        return 0;
}

int
libgf_client_ftruncate (libglusterfs_client_ctx_t *ctx, fd_t *fd,
                                off_t length)
{
        libgf_client_local_t            *local = NULL;
        call_stub_t                     *stub = NULL;
        int                             op_ret = -1;
        libglusterfs_client_fd_ctx_t    *fdctx = NULL;

        if (!(((fd->flags & O_ACCMODE) == O_RDWR)
              || ((fd->flags & O_ACCMODE) == O_WRONLY))) {
                errno = EBADF;
                goto out;
        }

        LIBGF_CLIENT_FOP (ctx, stub, ftruncate, local, fd, length);

        op_ret = stub->args.ftruncate_cbk.op_ret;
        errno = stub->args.ftruncate_cbk.op_errno;

        if (op_ret == -1)
                goto out;

        libgf_transform_iattr (ctx, fd->inode,
                               &stub->args.ftruncate_cbk.postbuf);
        libgf_update_iattr_cache (fd->inode, LIBGF_UPDATE_STAT,
                                        &stub->args.ftruncate_cbk.postbuf);

        fdctx = libgf_get_fd_ctx (fd);
        if (!fd) {
                errno = EINVAL;
                op_ret = -1;
                goto out;
        }

        pthread_mutex_lock (&fdctx->lock);
        {
                fdctx->offset = stub->args.ftruncate_cbk.postbuf.ia_size;
        }
        pthread_mutex_unlock (&fdctx->lock);

out:
        call_stub_destroy (stub);
        return op_ret;
}

int
glusterfs_ftruncate (glusterfs_file_t fd, off_t length)
{
        libglusterfs_client_fd_ctx_t    *fdctx = NULL;
        int                             op_ret = -1;

        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, fd, out);

        fdctx = libgf_get_fd_ctx (fd);
        if (!fdctx) {
                gf_log (LIBGF_XL_NAME, GF_LOG_ERROR, "No fd context present");
                errno = EBADF;
                goto out;
        }

        op_ret = libgf_client_ftruncate (fdctx->ctx, fd, length);

out:
        return op_ret;
}

int
libgf_client_link_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                                int32_t op_ret, int32_t op_errno,
                                inode_t *inode, struct iatt *buf,
                                struct iatt *preparent, struct iatt *postparent)
{
        libgf_client_local_t            *local = frame->local;

        local->reply_stub = fop_link_cbk_stub (frame, NULL, op_ret, op_errno,
                                               inode, buf, preparent,
                                               postparent);

        LIBGF_REPLY_NOTIFY (local);

        return 0;
}

int
libgf_client_link (libglusterfs_client_ctx_t *ctx, loc_t *old, loc_t *new)
{
        call_stub_t                     *stub = NULL;
        libgf_client_local_t            *local = NULL;
        int                             op_ret = -1;
        inode_t                         *inode = NULL;
        struct iatt                     *sbuf = NULL;

        LIBGF_CLIENT_FOP (ctx, stub, link, local, old, new);

        op_ret = stub->args.link_cbk.op_ret;
        errno = stub->args.link_cbk.op_errno;

        gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "old %s, new %s, status %d,"
                " errno %d", old->path, new->path, op_ret, errno);
        if (op_ret == -1)
                goto out;

        inode = stub->args.link_cbk.inode;
        sbuf = &stub->args.link_cbk.buf;
        inode_link (inode, new->parent, compat_basename ((char *)new->path),
                    sbuf);
        libgf_transform_iattr (ctx, inode, sbuf);
        inode_lookup (inode);
        libgf_update_iattr_cache (inode, LIBGF_UPDATE_STAT, sbuf);

out:
        call_stub_destroy (stub);
        return op_ret;
}

int
glusterfs_glh_link (glusterfs_handle_t handle, const char *oldpath,
                        const char *newpath)
{
        libglusterfs_client_ctx_t       *ctx = handle;
        int                             op_ret = -1;
        loc_t                           old = {0,};
        loc_t                           new = {0,};
        char                            *oldname = NULL;
        char                            *newname = NULL;

        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, ctx, out);
        GF_VALIDATE_ABSOLUTE_PATH_OR_GOTO (LIBGF_XL_NAME, oldpath, out);
        GF_VALIDATE_ABSOLUTE_PATH_OR_GOTO (LIBGF_XL_NAME, newpath, out);

        gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "old %s, new %s", oldpath,
                newpath);

        old.path = strdup (oldpath);
        if (!old.path) {
                gf_log (LIBGF_XL_NAME, GF_LOG_ERROR, "strdup failed");
                goto out;
        }

        op_ret = libgf_client_path_lookup (&old, ctx, 1);
        if (op_ret == -1) {
                errno = ENOENT;
                goto out;
        }

        oldname = strdup (old.path);
        op_ret = libgf_client_loc_fill (&old, ctx, 0, old.parent->ino,
                                                compat_basename (oldname));
        if (op_ret == -1) {
                errno = EINVAL;
                goto out;
        }

        if (IA_ISDIR (old.inode->ia_type)) {
                errno = EPERM;
                op_ret = -1;
                goto out;
        }

        new.path = strdup (newpath);
        if (!new.path) {
                gf_log (LIBGF_XL_NAME, GF_LOG_ERROR, "strdup failed");
                goto out;
        }

        op_ret = libgf_client_path_lookup (&new, ctx, 1);
        if (op_ret == 0) {
                errno = EEXIST;
                op_ret = -1;
                goto out;
        }

        newname = strdup (new.path);
        new.inode = inode_ref (old.inode);
        libgf_client_loc_fill (&new, ctx, 0, new.parent->ino,
                        compat_basename (newname));
        op_ret = libgf_client_link (ctx, &old, &new);

out:
        if (oldname)
                FREE (oldname);
        if (newname)
                FREE (newname);
        libgf_client_loc_wipe (&old);
        libgf_client_loc_wipe (&new);

        return op_ret;
}

int
glusterfs_link (const char *oldpath, const char *newpath)
{
        int                     op_ret = -1;
        char                    oldvpath[PATH_MAX];
        char                    newvpath[PATH_MAX];
        glusterfs_handle_t      oldh = NULL;
        glusterfs_handle_t      newh = NULL;

        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, oldpath, out);
        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, newpath, out);

        gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "old %s, new %s", oldpath,
                newpath);

        oldh = libgf_resolved_path_handle (oldpath, oldvpath);
        if (!oldh) {
                errno = ENODEV;
                goto out;
        }

        newh = libgf_resolved_path_handle (newpath, newvpath);
        if (!newh) {
                errno = ENODEV;
                goto out;
        }

        /* Cannot hard link across glusterfs mounts. */
        if (newh != oldh) {
                errno = EXDEV;
                goto out;
        }

        op_ret = glusterfs_glh_link (newh, oldvpath, newvpath);
out:
        return op_ret;
}

int32_t
libgf_client_statfs_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                         int32_t op_ret, int32_t op_errno, struct statvfs *buf)
{
        libgf_client_local_t    *local = frame->local;

        local->reply_stub = fop_statfs_cbk_stub (frame, NULL, op_ret, op_errno,
                                                 buf);

        LIBGF_REPLY_NOTIFY (local);
        return 0;
}

int32_t
libgf_client_statvfs (libglusterfs_client_ctx_t *ctx, loc_t *loc,
                      struct statvfs *buf)
{
        call_stub_t             *stub = NULL;
        libgf_client_local_t    *local = NULL;
        int32_t                 op_ret = -1;

        /* statfs fop receives struct statvfs as an argument */

        /* libgf_client_statfs_cbk will be the callback, not
           libgf_client_statvfs_cbk. see definition of LIBGF_CLIENT_FOP
        */
        LIBGF_CLIENT_FOP (ctx, stub, statfs, local, loc);

        op_ret = stub->args.statfs_cbk.op_ret;
        errno = stub->args.statfs_cbk.op_errno;
        gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "path %s, status %d, errno %d",
                loc->path, op_ret, errno);
        if (op_ret == -1)
                goto out;

        if (buf)
                memcpy (buf, &stub->args.statfs_cbk.buf, sizeof (*buf));
out:
        call_stub_destroy (stub);
        return op_ret;
}

int
glusterfs_glh_statfs (glusterfs_handle_t handle, const char *path,
                        struct statfs *buf)
{
        struct statvfs                  stvfs = {0, };
        int32_t                         op_ret = -1;
        loc_t                           loc = {0, };
        libglusterfs_client_ctx_t       *ctx = handle;
        char                            *name = NULL;

        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, ctx, out);
        GF_VALIDATE_ABSOLUTE_PATH_OR_GOTO (LIBGF_XL_NAME, path, out);

        gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "path %s", path);

        loc.path = strdup (path);
        if (!loc.path) {
                gf_log (LIBGF_XL_NAME, GF_LOG_ERROR, "strdup failed");
                goto out;
        }

        op_ret = libgf_client_path_lookup (&loc, ctx, 1);
        if (op_ret == -1) {
                gf_log ("libglusterfsclient", GF_LOG_ERROR,
                        "path lookup failed for (%s)", loc.path);
                goto out;
        }

        name = strdup (loc.path);
        op_ret = libgf_client_loc_fill (&loc, ctx, 0, loc.parent->ino,
                                        compat_basename (name));
        if (op_ret == -1) {
                gf_log ("libglusterfsclient", GF_LOG_ERROR,
                                "libgf_client_loc_fill returned -1, "
                                "returning EINVAL");
                        errno = EINVAL;
                goto out;
        }

        op_ret = libgf_client_statvfs (ctx, &loc, &stvfs);
        if (op_ret == 0) {
#ifdef GF_SOLARIS_HOST_OS
		buf->f_fstyp = 0;
                buf->f_bsize = stvfs.f_bsize;
                buf->f_blocks = stvfs.f_blocks;
                buf->f_bfree = stvfs.f_bfree;
		buf->f_files = stvfs.f_bavail;
                buf->f_ffree = stvfs.f_ffree;
#else
                buf->f_type = 0;
                buf->f_bsize = stvfs.f_bsize;
                buf->f_blocks = stvfs.f_blocks;
                buf->f_bfree = stvfs.f_bfree;
                buf->f_bavail = stvfs.f_bavail;
                buf->f_files = stvfs.f_bavail;
                buf->f_ffree = stvfs.f_ffree;
                /* FIXME: buf->f_fsid has either "val" or "__val" as member
                   based on conditional macro expansion. see definition of
                   fsid_t - Raghu
                   It seems have different structure member names on
                   different archs, so I am stepping down to doing a struct
                   to struct copy. :Shehjar
                */
                memcpy (&buf->f_fsid, &stvfs.f_fsid, sizeof (stvfs.f_fsid));
                buf->f_namelen = stvfs.f_namemax;
#endif
        }

out:
        if (name)
                FREE (name);
        libgf_client_loc_wipe (&loc);
        return op_ret;
}

int
glusterfs_statfs (const char *path, struct statfs *buf)
{
        int                     op_ret = -1;
        char                    vpath[PATH_MAX];
        glusterfs_handle_t      h      = NULL;

        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, path, out);
        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, buf, out);

        gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "path %s", path);

        h = libgf_resolved_path_handle (path, vpath);
        if (!h) {
                errno = ENODEV;
                goto out;
        }

        op_ret = glusterfs_glh_statfs (h, vpath, buf);
out:
        return op_ret;
}

int
glusterfs_glh_statvfs (glusterfs_handle_t handle, const char *path,
                                struct statvfs *buf)
{
        int32_t                         op_ret = -1;
        loc_t                           loc = {0, };
        libglusterfs_client_ctx_t       *ctx = handle;
        char                            *name = NULL;

        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, ctx, out);
        GF_VALIDATE_ABSOLUTE_PATH_OR_GOTO (LIBGF_XL_NAME, path, out);

        gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "path %s", path);

        loc.path = strdup (path);
        if (!loc.path) {
                gf_log (LIBGF_XL_NAME, GF_LOG_ERROR, "strdup failed");
                goto out;
        }
        op_ret = libgf_client_path_lookup (&loc, ctx, 1);
        if (op_ret == -1) {
                gf_log ("libglusterfsclient", GF_LOG_ERROR,
                        "path lookup failed for (%s)", loc.path);
                goto out;
        }

        name = strdup (loc.path);
        op_ret = libgf_client_loc_fill (&loc, ctx, 0, loc.parent->ino,
                                                compat_basename (name));
	if (op_ret == -1) {
                gf_log ("libglusterfsclient", GF_LOG_ERROR,
                                "libgf_client_loc_fill returned -1, returning"
                                " EINVAL");
                errno = EINVAL;
                goto out;
	}

        op_ret = libgf_client_statvfs (ctx, &loc, buf);
        if (op_ret != -1)
                /* Should've been a call to libgf_transform_iattr but
                 * that only handles struct stat
                 */
                buf->f_fsid = (unsigned long)ctx->fake_fsid;

out:
        if (name)
                FREE (name);
        libgf_client_loc_wipe (&loc);
        return op_ret;
}

int
glusterfs_statvfs (const char *path, struct statvfs *buf)
{
        char                    vpath[PATH_MAX];
        glusterfs_handle_t      h      = NULL;
        int                     op_ret = -1;

        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, path, out);
        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, buf, out);

        gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "path %s", path);

        h = libgf_resolved_path_handle (path, vpath);
        if (!h) {
                errno = ENODEV;
                goto out;
        }

        op_ret = glusterfs_glh_statvfs (h, vpath, buf);
out:
        return op_ret;
}

int32_t
libgf_client_rename_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                         int32_t op_ret, int32_t op_errno, struct iatt *buf,
                         struct iatt *preoldparent, struct iatt *postoldparent,
                         struct iatt *prenewparent, struct iatt *postnewparent)
{
        libgf_client_local_t *local = frame->local;

        local->reply_stub = fop_rename_cbk_stub (frame, NULL, op_ret, op_errno,
                                                 buf, preoldparent,
                                                 postoldparent, prenewparent,
                                                 postnewparent);

        LIBGF_REPLY_NOTIFY (local);
        return 0;
}

int32_t
libgf_client_rename (libglusterfs_client_ctx_t *ctx, loc_t *oldloc,
                     loc_t *newloc)
{
        int                             op_ret = -1;
        libgf_client_local_t            *local = NULL;
        call_stub_t                     *stub = NULL;

        LIBGF_CLIENT_FOP (ctx, stub, rename, local, oldloc, newloc);

        op_ret = stub->args.rename_cbk.op_ret;
        errno = stub->args.rename_cbk.op_errno;

        gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "old %s, new %s, status %d, errno"
                " %d", oldloc->path, newloc->path, op_ret, errno);
        if (op_ret == -1)
                goto out;

        if (!libgf_get_inode_ctx (newloc->inode))
                libgf_alloc_inode_ctx (ctx, newloc->inode);

        libgf_transform_iattr (ctx, newloc->inode, &stub->args.rename_cbk.buf);
        libgf_update_iattr_cache (newloc->inode, LIBGF_UPDATE_STAT,
                                        &stub->args.rename_cbk.buf);

        inode_unlink (oldloc->inode, oldloc->parent, oldloc->name);
out:
        call_stub_destroy (stub);
        return op_ret;
}

int
glusterfs_glh_rename (glusterfs_handle_t handle, const char *oldpath,
                      const char *newpath)
{
        int32_t                         op_ret = -1;
        loc_t                           oldloc = {0, }, newloc = {0, };
        libglusterfs_client_ctx_t       *ctx = handle;
        char                            *newname = NULL;
        char                            *oldname = NULL;

        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, ctx, out);
        GF_VALIDATE_ABSOLUTE_PATH_OR_GOTO (LIBGF_XL_NAME, oldpath, out);
        GF_VALIDATE_ABSOLUTE_PATH_OR_GOTO (LIBGF_XL_NAME, newpath, out);

        gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "old %s, new %s", oldpath,
                newpath);

        oldloc.path = strdup (oldpath);
        if (!oldloc.path) {
                gf_log (LIBGF_XL_NAME, GF_LOG_ERROR, "strdup failed");
                goto out;
        }

        op_ret = libgf_client_path_lookup (&oldloc, ctx, 1);
        if (op_ret == -1) {
                gf_log ("libglusterfsclient", GF_LOG_ERROR,
                        "path lookup failed for (%s)", oldloc.path);
                goto out;
        }

        newloc.path = strdup (newpath);
        if (!newloc.path) {
                gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "strdup failed");
                goto out;
        }

        op_ret = libgf_client_path_lookup (&newloc, ctx, 1);

        oldname = strdup (oldloc.path);
        op_ret = libgf_client_loc_fill (&oldloc, ctx, 0, oldloc.parent->ino,
                                        compat_basename (oldname));
	if (op_ret == -1) {
                gf_log ("libglusterfsclient", GF_LOG_ERROR,
                                "libgf_client_loc_fill returned -1,"
                                " returning EINVAL");
                errno = EINVAL;
                goto out;
        }

        newname = strdup (newloc.path);
        op_ret = libgf_client_loc_fill (&newloc, ctx, 0, newloc.parent->ino,
                                        compat_basename (newname));
	if (op_ret == -1) {
                gf_log ("libglusterfsclient", GF_LOG_ERROR,
                                "libgf_client_loc_fill returned -1,"
                                " returning EINVAL");
                errno = EINVAL;
                goto out;
        }
        op_ret = libgf_client_rename (ctx, &oldloc, &newloc);

out:
        if (oldname)
                FREE (oldname);
        if (newname)
                FREE (newname);
        libgf_client_loc_wipe (&newloc);
        libgf_client_loc_wipe (&oldloc);

        return op_ret;
}


int
glusterfs_rename (const char *oldpath, const char *newpath)
{
        int                     op_ret = -1;
        char                    oldvpath[PATH_MAX];
        char                    newvpath[PATH_MAX];
        glusterfs_handle_t      oldh = NULL;
        glusterfs_handle_t      newh = NULL;

        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, oldpath, out);
        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, newpath, out);

        gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "Old %s, new %s", oldpath,
                newpath);

        oldh = libgf_resolved_path_handle (oldpath, oldvpath);
        if (!oldh) {
                errno = ENODEV;
                goto out;
        }

        newh = libgf_resolved_path_handle (newpath, newvpath);
        if (!newh) {
                errno = ENODEV;
                goto out;
        }

        if (oldh != newh) {
                errno = EXDEV;
                goto out;
        }

        op_ret = glusterfs_glh_rename (oldh, oldvpath, newvpath);
out:
        return op_ret;
}


int
glusterfs_glh_utimes (glusterfs_handle_t handle, const char *path,
                        const struct timeval times[2])
{
        int32_t                         op_ret = -1;
        loc_t                           loc = {0, };
        libglusterfs_client_ctx_t       *ctx = handle;
        char                            *name = NULL;
        struct iatt                     stbuf = {0,};
        int32_t                         valid = 0;

        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, ctx, out);
        GF_VALIDATE_ABSOLUTE_PATH_OR_GOTO (LIBGF_XL_NAME, path, out);

        gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "path %s", path);
        stbuf.ia_atime = times[0].tv_sec;
        stbuf.ia_atime_nsec = times[0].tv_usec * 1000;
        stbuf.ia_mtime = times[1].tv_sec;
        stbuf.ia_mtime_nsec = times[1].tv_usec * 1000;

        valid |= (GF_SET_ATTR_ATIME | GF_SET_ATTR_MTIME);

        loc.path = strdup (path);
        if (!loc.path) {
                gf_log (LIBGF_XL_NAME, GF_LOG_ERROR, "strdup failed");
                goto out;
        }

        op_ret = libgf_client_path_lookup (&loc, ctx, 1);
        if (op_ret == -1) {
                gf_log ("libglusterfsclient", GF_LOG_ERROR,
                        "path lookup failed for (%s)", loc.path);
                goto out;
        }

        name = strdup (loc.path);
        op_ret = libgf_client_loc_fill (&loc, ctx, 0, loc.parent->ino,
                                                compat_basename (name));
        if (op_ret == -1) {
                gf_log ("libglusterfsclient", GF_LOG_ERROR,
                                "libgf_client_loc_fill returned -1"
                                " returning EINVAL");
                errno = EINVAL;
                goto out;
        }

        op_ret = libgf_client_setattr (ctx, &loc, &stbuf, valid);
out:
        if (name)
                FREE (name);
        libgf_client_loc_wipe (&loc);
        return op_ret;
}

int
glusterfs_utimes (const char *path, const struct timeval times[2])
{
        char                    vpath[PATH_MAX];
        int                     op_ret = -1;
        glusterfs_handle_t      h = NULL;

        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, path, out);

        gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "path %s", path);

        h = libgf_resolved_path_handle (path, vpath);
        if (!h) {
                errno = ENODEV;
                goto out;
        }

        op_ret = glusterfs_glh_utimes (h, vpath, times);
out:
        return op_ret;
}

int
glusterfs_glh_utime (glusterfs_handle_t handle, const char *path,
                        const struct utimbuf *buf)
{
        int32_t                         op_ret = -1;
        loc_t                           loc = {0, };
        libglusterfs_client_ctx_t       *ctx = handle;
        char                            *name = NULL;
        struct iatt                     stbuf = {0,};
        int32_t                         valid = 0;

        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, ctx, out);
        GF_VALIDATE_ABSOLUTE_PATH_OR_GOTO (LIBGF_XL_NAME, path, out);

        gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "path %s", path);
        if (buf) {
                stbuf.ia_atime = buf->actime;
                stbuf.ia_atime_nsec = 0;

                stbuf.ia_mtime = buf->modtime;
                stbuf.ia_mtime_nsec = 0;
        }

        valid |= (GF_SET_ATTR_ATIME | GF_SET_ATTR_MTIME);

        loc.path = strdup (path);
        if (!loc.path) {
                gf_log (LIBGF_XL_NAME, GF_LOG_ERROR, "strdup failed");
                goto out;
        }

        op_ret = libgf_client_path_lookup (&loc, ctx, 1);
        if (op_ret == -1) {
                gf_log ("libglusterfsclient", GF_LOG_ERROR,
                        "path lookup failed for (%s)", loc.path);
                goto out;
        }

        name = strdup (loc.path);
        op_ret = libgf_client_loc_fill (&loc, ctx, 0, loc.parent->ino,
                                                compat_basename (name));
        if (op_ret == -1) {
                gf_log ("libglusterfsclient", GF_LOG_ERROR,
                                "libgf_client_loc_fill returned -1,"
                                " returning EINVAL");
                errno = EINVAL;
                goto out;
        }

        op_ret = libgf_client_setattr (ctx, &loc, &stbuf, valid);
out:
        if (name)
                FREE (name);
        libgf_client_loc_wipe (&loc);
        return op_ret;
}

int
glusterfs_utime (const char *path, const struct utimbuf *buf)
{
        char                    vpath[PATH_MAX];
        int                     op_ret = -1;
        glusterfs_handle_t      h      = NULL;

        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, path, out);
        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, buf, out);

        gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "path %s", path);

        h = libgf_resolved_path_handle (path, vpath);
        if (!h) {
                errno = ENODEV;
                goto out;
        }

        op_ret = glusterfs_glh_utime (h, vpath, buf);
out:
        return op_ret;
}

static int32_t
libgf_client_mknod_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                                int32_t op_ret, int32_t op_errno,
                                inode_t *inode, struct iatt *buf,
                                struct iatt *preparent, struct iatt *postparent)
{
        libgf_client_local_t *local = frame->local;

        local->reply_stub = fop_mknod_cbk_stub (frame, NULL, op_ret, op_errno,
                                                inode, buf, preparent,
                                                postparent);

        LIBGF_REPLY_NOTIFY (local);
        return 0;
}

static int32_t
libgf_client_mknod (libglusterfs_client_ctx_t *ctx, loc_t *loc, mode_t mode,
                    dev_t rdev)
{
        int32_t                 op_ret = -1;
        call_stub_t             *stub = NULL;
        libgf_client_local_t    *local = NULL;
        inode_t                 *inode = NULL;

        LIBGF_CLIENT_FOP (ctx, stub, mknod, local, loc, mode, rdev);

        op_ret = stub->args.mknod_cbk.op_ret;
        errno = stub->args.mknod_cbk.op_errno;
        if (op_ret == -1)
                goto out;

        gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "path %s, status %d, errno %d",
                loc->path, op_ret, errno);
        inode = stub->args.mknod_cbk.inode;
        inode_link (inode, loc->parent, loc->name, &stub->args.mknod_cbk.buf);
        libgf_transform_iattr (ctx, inode, &stub->args.mknod_cbk.buf);
        inode_lookup (inode);

        if (!libgf_alloc_inode_ctx (ctx, inode))
                libgf_alloc_inode_ctx (ctx, inode);

        libgf_update_iattr_cache (inode, LIBGF_UPDATE_STAT,
                                        &stub->args.mknod_cbk.buf);

out:
	call_stub_destroy (stub);
        return op_ret;
}

int
glusterfs_glh_mknod(glusterfs_handle_t handle, const char *path, mode_t mode,
                        dev_t dev)
{
        libglusterfs_client_ctx_t       *ctx = handle;
        loc_t                           loc = {0, };
        char                            *name = NULL;
        int32_t                         op_ret = -1;

        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, ctx, out);
        GF_VALIDATE_ABSOLUTE_PATH_OR_GOTO (LIBGF_XL_NAME, path, out);

        gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "path %s", path);

        loc.path = strdup (path);
        if (!loc.path) {
                gf_log (LIBGF_XL_NAME, GF_LOG_ERROR, "strdup failed");
                goto out;
        }

        op_ret = libgf_client_path_lookup (&loc, ctx, 1);
        if (op_ret == 0) {
                op_ret = -1;
                errno = EEXIST;
                goto out;
        }

        op_ret = libgf_client_path_lookup (&loc, ctx, 0);
        if (op_ret == -1) {
                errno = ENOENT;
                goto out;
        }

        name = strdup (loc.path);
        op_ret = libgf_client_loc_fill (&loc, ctx, 0, loc.parent->ino,
                                                compat_basename (name));
	if (op_ret == -1) {
                gf_log ("libglusterfsclient", GF_LOG_ERROR,
                                "libgf_client_loc_fill returned -1, "
                                " returning EINVAL");
                errno = EINVAL;
                goto out;
        }

        loc.inode = inode_new (ctx->itable);
        op_ret = libgf_client_mknod (ctx, &loc, mode, dev);

out:
	libgf_client_loc_wipe (&loc);
	if (name)
                FREE (name);

	return op_ret;
}

int
glusterfs_mknod(const char *pathname, mode_t mode, dev_t dev)
{
        char                    vpath[PATH_MAX]; 
        int                     op_ret = -1;
        glusterfs_handle_t      h      = NULL;

        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, pathname, out);

        gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "path %s", pathname);

        h = libgf_resolved_path_handle (pathname, vpath);
        if (!h) {
                errno = ENODEV;
                goto out;
        }

out:
        return op_ret;
}

int
glusterfs_glh_mkfifo (glusterfs_handle_t handle, const char *path, mode_t mode)
{

        libglusterfs_client_ctx_t       *ctx = handle;
        loc_t                           loc = {0, };
        char                            *name = NULL;
        int32_t                         op_ret = -1;
        dev_t                           dev = 0;

        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, ctx, out);
        GF_VALIDATE_ABSOLUTE_PATH_OR_GOTO (LIBGF_XL_NAME, path, out);

        gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "path %s", path);
        loc.path = libgf_resolve_path_light ((char *)path);
        if (!loc.path) {
                gf_log (LIBGF_XL_NAME, GF_LOG_ERROR, "Failed to resolve name");
                goto out;
        }

        op_ret = libgf_client_path_lookup (&loc, ctx, 1);
        if (op_ret == 0) {
                op_ret = -1;
                errno = EEXIST;
                goto out;
        }

        op_ret = libgf_client_path_lookup (&loc, ctx, 0);
        if (op_ret == -1) {
                errno = ENOENT;
                goto out;
        }

        name = strdup (loc.path);
        op_ret = libgf_client_loc_fill (&loc, ctx, 0, loc.parent->ino,
                                                compat_basename (name));
        if (op_ret == -1) {
                gf_log ("libglusterfsclient", GF_LOG_ERROR,
                                "libgf_client_loc_fill returned -1, "
                                "returning EINVAL");
                errno = EINVAL;
                goto out;
        }

        loc.inode = inode_new (ctx->itable);
        op_ret = libgf_client_mknod (ctx, &loc, mode | S_IFIFO, dev);

out:
	libgf_client_loc_wipe (&loc);
        if (name)
                free (name);

        return op_ret;
}

int
glusterfs_mkfifo (const char *path, mode_t mode)
{
        int                     op_ret = -1;
        char                    vpath[PATH_MAX];
        glusterfs_handle_t      h = NULL;

        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, path, out);

        gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "path %s", path);

        h = libgf_resolved_path_handle (path, vpath);
        if (!h) {
                errno = ENODEV;
                goto out;
        }

        op_ret = glusterfs_glh_mkfifo (h, vpath, mode);
out:
        return op_ret;
}

int32_t
libgf_client_unlink_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                         int32_t op_ret, int32_t op_errno,
                         struct iatt *preparent, struct iatt *postparent)
{
        libgf_client_local_t    *local = frame->local;

        local->reply_stub = fop_unlink_cbk_stub (frame, NULL, op_ret, op_errno,
                                                 preparent, postparent);

        LIBGF_REPLY_NOTIFY (local);
        return 0;
}

int
libgf_client_unlink (libglusterfs_client_ctx_t *ctx, loc_t *loc)
{
        int                             op_ret = -1;
        libgf_client_local_t            *local = NULL;
        call_stub_t                     *stub = NULL;

        LIBGF_CLIENT_FOP (ctx, stub, unlink, local, loc);

        op_ret = stub->args.unlink_cbk.op_ret;
        errno = stub->args.unlink_cbk.op_errno;

        gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "path %s", loc->path);
        if (op_ret == -1)
                goto out;

        inode_unlink (loc->inode, loc->parent, loc->name);

out:
        call_stub_destroy (stub);
        return op_ret;
}

int
glusterfs_glh_unlink (glusterfs_handle_t handle, const char *path)
{
        int32_t                         op_ret = -1;
        loc_t                           loc = {0, };
        libglusterfs_client_ctx_t       *ctx = handle;
        char                            *name = NULL;

        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, ctx, out);
        GF_VALIDATE_ABSOLUTE_PATH_OR_GOTO (LIBGF_XL_NAME, path, out);

        gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "path %s", path);

        loc.path = strdup (path);
        if (!loc.path) {
                gf_log (LIBGF_XL_NAME, GF_LOG_ERROR, "strdup failed");
                goto out;
        }

        op_ret = libgf_client_path_lookup (&loc, ctx, 1);
        if (op_ret == -1) {
                gf_log ("libglusterfsclient", GF_LOG_ERROR,
                        "path lookup failed for (%s)", loc.path);
                goto out;
        }

        name = strdup (loc.path);
        op_ret = libgf_client_loc_fill (&loc, ctx, 0, loc.parent->ino,
                                                compat_basename (name));
	if (op_ret == -1) {
                gf_log ("libglusterfsclient", GF_LOG_ERROR,
                                "libgf_client_loc_fill returned -1, "
                                " returning EINVAL");
                errno = EINVAL;
                goto out;
        }

        op_ret = libgf_client_unlink (ctx, &loc);

out:
        if (name)
                FREE (name);
        libgf_client_loc_wipe (&loc);
        return op_ret;
}

int
glusterfs_unlink (const char *path)
{
        char                    vpath[PATH_MAX];
        int                     op_ret = -1;
        glusterfs_handle_t      h      = NULL;

        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, path, out);

        gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "path %s", path);

        h = libgf_resolved_path_handle (path, vpath);
        if (!h) {
                errno = ENODEV;
                goto out;
        }

        op_ret = glusterfs_glh_unlink (h, vpath);

out:
        return op_ret;
}

static int32_t
libgf_client_symlink_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                                int32_t op_ret, int32_t op_errno,
                                inode_t *inode, struct iatt *buf,
                                struct iatt *preparent, struct iatt *postparent)
{
        libgf_client_local_t *local = frame->local;

        local->reply_stub = fop_symlink_cbk_stub (frame, NULL, op_ret,
                                                  op_errno, inode, buf,
                                                  preparent, postparent);

        LIBGF_REPLY_NOTIFY (local);
        return 0;
}

int32_t
libgf_client_symlink (libglusterfs_client_ctx_t *ctx, const char *linkpath,
                      loc_t *loc)
{
        int                     op_ret = -1;
        libgf_client_local_t    *local = NULL;
        call_stub_t             *stub = NULL;
        inode_t                 *inode = NULL;

        LIBGF_CLIENT_FOP (ctx, stub, symlink, local, linkpath, loc);

        op_ret = stub->args.symlink_cbk.op_ret;
        errno = stub->args.symlink_cbk.op_errno;
        if (op_ret == -1)
                goto out;

        gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "target: %s, link: %s, status %d"
                " errno %d", linkpath, loc->path, op_ret, errno);
        inode = stub->args.symlink_cbk.inode;
        inode_link (inode, loc->parent, loc->name,
                        &stub->args.symlink_cbk.buf);
        libgf_transform_iattr (ctx, inode, &stub->args.symlink_cbk.buf);
        inode_lookup (inode);
        if (!libgf_get_inode_ctx (inode))
                libgf_alloc_inode_ctx (ctx, inode);

        libgf_update_iattr_cache (inode, LIBGF_UPDATE_STAT,
                                        &stub->args.symlink_cbk.buf);
out:
        call_stub_destroy (stub);
        return op_ret;
}

int
glusterfs_glh_symlink (glusterfs_handle_t handle, const char *oldpath,
                       const char *newpath)
{
        int32_t                         op_ret = -1;
        libglusterfs_client_ctx_t       *ctx = handle;
        loc_t                           oldloc = {0, };
        loc_t                           newloc = {0, };
        char                            *oldname = NULL;
        char                            *newname = NULL;

        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, ctx, out);
        GF_VALIDATE_ABSOLUTE_PATH_OR_GOTO (LIBGF_XL_NAME, newpath, out);

        gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "target: %s, link: %s", oldpath,
                newpath);
        /* Old path does not need to be interpreted or looked up */
        oldloc.path = strdup (oldpath);

	newloc.path = strdup (newpath);
        if (!newloc.path) {
                gf_log (LIBGF_XL_NAME, GF_LOG_ERROR, "strdup failed");
                goto out;
        }

	op_ret = libgf_client_path_lookup (&newloc, ctx, 1);
	if (op_ret == 0) {
                gf_log ("libglusterfsclient", GF_LOG_ERROR,
                        "new path (%s) already exists, "
                        " returning EEXIST", newloc.path);
                op_ret = -1;
                errno = EEXIST;
                goto out;
        }

        op_ret = libgf_client_path_lookup (&newloc, ctx, 0);
        if (op_ret == -1) {
                errno = ENOENT;
                goto out;
        }

        newloc.inode = inode_new (ctx->itable);
        newname = strdup (newloc.path);
        op_ret = libgf_client_loc_fill (&newloc, ctx, 0, newloc.parent->ino,
                                                compat_basename (newname));

        if (op_ret == -1) {
                gf_log ("libglusterfsclient", GF_LOG_ERROR,
                                "libgf_client_loc_fill returned -1, "
                                "returning EINVAL");
                errno = EINVAL;
                goto out;
        }

        op_ret = libgf_client_symlink (ctx, oldpath, &newloc);

out:
        if (newname)
                FREE (newname);

        if (oldname)
                FREE (oldname);
        libgf_client_loc_wipe (&oldloc);
        libgf_client_loc_wipe (&newloc);
        return op_ret;
}

int
glusterfs_symlink (const char *oldpath, const char *newpath)
{
        char                    vpath[PATH_MAX];
        int                     op_ret = -1;
        glusterfs_handle_t      h      = NULL;

        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, oldpath, out);
        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, newpath, out);

        gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "target: %s, link: %s", oldpath,
                newpath);

        h = libgf_resolved_path_handle (newpath, vpath);
        if (!h) {
                errno = ENODEV;
                goto out;
        }

        op_ret = glusterfs_glh_symlink (h, oldpath, vpath);
out:
        return op_ret;
}


int32_t
libgf_client_readlink_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                                int32_t op_ret, int32_t op_errno,
                                const char *path, struct iatt *sbuf)
{
        libgf_client_local_t    *local = frame->local;

        local->reply_stub = fop_readlink_cbk_stub (frame, NULL, op_ret,
                                                   op_errno, path, sbuf);

        LIBGF_REPLY_NOTIFY (local);
        return 0;
}

int32_t
libgf_client_readlink (libglusterfs_client_ctx_t *ctx, loc_t *loc, char *buf,
                       size_t bufsize)
{
        int                             op_ret = -1;
        libgf_client_local_t            *local = NULL;
        call_stub_t                     *stub = NULL;
        size_t                           cpy_size = 0;

        LIBGF_CLIENT_FOP (ctx, stub, readlink, local, loc, bufsize);

        op_ret = stub->args.readlink_cbk.op_ret;
        errno = stub->args.readlink_cbk.op_errno;

        if (op_ret != -1) {
                cpy_size = ((op_ret <= bufsize) ? op_ret : bufsize);
                memcpy (buf, stub->args.readlink_cbk.buf, cpy_size);
                op_ret = cpy_size;
                gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "link: %s, target: %s,"
                        " status %d, errno %d", loc->path, buf, op_ret, errno);
        } else
                gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "link: %s, status %d, "
                        "errno %d", loc->path, op_ret, errno);

        call_stub_destroy (stub);
        return op_ret;
}

ssize_t
glusterfs_glh_readlink (glusterfs_handle_t handle, const char *path, char *buf,
                                size_t bufsize)
{
        int32_t                         op_ret = -1;
        loc_t                           loc = {0, };
        libglusterfs_client_ctx_t       *ctx = handle;
        char                            *name = NULL;

        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, ctx, out);
        GF_VALIDATE_ABSOLUTE_PATH_OR_GOTO (LIBGF_XL_NAME, path, out);

        gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "path %s", path);
        if (bufsize < 0) {
                errno = EINVAL;
                goto out;
        }

        if (bufsize == 0) {
                op_ret = 0;
                goto out;
        }

        loc.path = strdup (path);
        if (!loc.path) {
                gf_log (LIBGF_XL_NAME, GF_LOG_ERROR, "strdup failed");
                goto out;
        }

        op_ret = libgf_client_path_lookup (&loc, ctx, 1);
        if (op_ret == -1) {
                gf_log ("libglusterfsclient", GF_LOG_ERROR,
                        "path lookup failed for (%s)", loc.path);
                goto out;
        }

        name = strdup (loc.path);
        op_ret = libgf_client_loc_fill (&loc, ctx, 0, loc.parent->ino,
                                                compat_basename (name));
        if (op_ret == -1) {
                gf_log ("libglusterfsclient", GF_LOG_ERROR,
                                "libgf_client_loc_fill returned -1, "
                                "returning EINVAL");
                errno = EINVAL;
                goto out;
        }

        op_ret = libgf_client_readlink (ctx, &loc, buf, bufsize);

out:
        if (name)
                FREE (name);

        libgf_client_loc_wipe (&loc);
        return op_ret;
}

ssize_t
glusterfs_readlink (const char *path, char *buf, size_t bufsize)
{
        char                    vpath[PATH_MAX];
        int                     op_ret = -1;
        glusterfs_handle_t      h      = NULL;

        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, path, out);
        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, buf, out);

        gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "path %s", path);

        h = libgf_resolved_path_handle (path, vpath);
        if (!h) {
                errno = ENODEV;
                goto out;
        }

        op_ret = glusterfs_glh_readlink (h, vpath, buf, bufsize);
out:
        return op_ret;
}

char *
glusterfs_glh_realpath (glusterfs_handle_t handle, const char *path,
                                char *resolved_path)
{
        char                            *buf = NULL;
        char                            *rpath = NULL;
        char                            *start = NULL, *end = NULL;
        char                            *dest = NULL;
        libglusterfs_client_ctx_t       *ctx = handle;
        long int                        path_max = 0;
        char                            *ptr = NULL;
        struct stat                     stbuf = {0, };
        long int                        new_size = 0;
        char                            *new_rpath = NULL;
        int                             dest_offset = 0;
        char                            *rpath_limit = 0;
        int                             ret = 0, num_links = 0;
        char                            *vpath = NULL, *tmppath = NULL;
        char                             absolute_path[PATH_MAX];

        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, ctx, out);
        GF_VALIDATE_ABSOLUTE_PATH_OR_GOTO (LIBGF_XL_NAME, path, out);

#ifdef PATH_MAX
        path_max = PATH_MAX;
#else
        path_max = pathconf (path, _PC_PATH_MAX);
        if (path_max <= 0) {
                path_max = 1024;
        }
#endif

        if (resolved_path == NULL) {
                rpath = CALLOC (1, path_max);
                if (rpath == NULL) {
                        errno = ENOMEM;
                        goto out;
                }
        } else {
                rpath = resolved_path;
        }

        rpath_limit = rpath + path_max;

        if (path[0] == '/') {
                rpath[0] = '/';
                dest = rpath + 1;
        } else {
                /*
                   FIXME: can $CWD be a valid path on glusterfs server? hence is
                   it better to handle this case or just return EINVAL for
                   relative paths?
                */
                ptr = getcwd (rpath, path_max);
                if (ptr == NULL) {
                        goto err;
                }
                dest = rpath + strlen (rpath);
        }

        for (start = end = (char *)path; *end; start = end) {
                if (dest[-1] != '/') {
                        *dest++ = '/';
                }

                while (*start == '/') {
                        start++;
                }

                for (end = start; *end && *end != '/'; end++);

                if ((end - start) == 0) {
                        break;
                }

                if ((end - start == 1) && (start[0] == '.')) {
                        /* do nothing */
                } else if (((end - start) == 2) && (start[0] == '.')
                           && (start[1] == '.')) {
                        if (dest > rpath + 1) {
                                while (--dest[-1] != '/');
                        }
                } else {
                        if ((dest + (end - start + 1)) >= rpath_limit) {
                                if (resolved_path == NULL) {
                                        errno = ENAMETOOLONG;
                                        if (dest > rpath + 1)
                                                dest--;
                                        *dest = '\0';
                                        goto err;
                                }

                                dest_offset = dest - rpath;
                                new_size = rpath_limit - rpath;
                                if ((end - start + 1) > path_max) {
                                        new_size = (end - start + 1);
                                } else {
                                        new_size = path_max;
                                }

                                new_rpath = realloc (rpath, new_size);
                                if (new_rpath == NULL) {
                                        goto err;
                                }


                                dest = new_rpath + dest_offset;
                                rpath = new_rpath;
                                rpath_limit = rpath + new_size;
                        }

                        memcpy (dest, start, end - start);
                        dest +=  end - start;
                        *dest = '\0';

                        ret = glusterfs_glh_lstat (handle, rpath, &stbuf);
                        if (ret == -1) {
                                gf_log ("libglusterfsclient", GF_LOG_ERROR,
                                        "glusterfs_glh_stat returned -1 for"
                                        " path (%s):(%s)", rpath,
                                        strerror (errno));
                                goto err;
                        }

                        if (S_ISLNK (stbuf.st_mode)) {
                                buf = calloc (1, path_max);
                                if (buf == NULL) {
                                        errno = ENOMEM;
                                        goto err;
                                }

                                if (++num_links > MAXSYMLINKS)
                                {
                                        errno = ELOOP;
                                        FREE (buf);
                                        goto err;
                                }

                                ret = glusterfs_glh_readlink (handle, rpath,
                                                              buf,
                                                              path_max - 1);
                                if (ret < 0) {
                                        gf_log ("libglusterfsclient",
                                                GF_LOG_ERROR,
                                                "glusterfs_readlink returned %d"
                                                " for path (%s):(%s)",
                                                ret, rpath, strerror (errno));
                                        FREE (buf);
                                        goto err;
                                }
                                buf[ret] = '\0';

                                if (buf[0] != '/') {
                                        tmppath = strdup (rpath);
                                        tmppath = dirname (tmppath);
                                        sprintf (absolute_path, "%s/%s",
                                                 tmppath, buf);
                                        FREE (buf);
                                        buf = libgf_resolve_path_light ((char *)absolute_path);
                                        FREE (tmppath);
                                }

                                rpath = glusterfs_glh_realpath (handle, buf,
                                                                rpath);
                                FREE (buf);
                                if (rpath == NULL) {
                                        goto out;
                                }
                                dest = rpath + strlen (rpath);

                        } else if (!S_ISDIR (stbuf.st_mode) && *end != '\0') {
                                errno = ENOTDIR;
                                goto err;
                        }
                }
        }
        if (dest > rpath + 1 && dest[-1] == '/')
                --dest;
        *dest = '\0';

out:
        if (vpath)
                FREE (vpath);
        return rpath;

err:
        if (vpath)
                FREE (vpath);
        if (resolved_path == NULL) {
                FREE (rpath);
        }

        return NULL;
}

char *
glusterfs_realpath (const char *path, char *resolved_path)
{
        char                    *res   = NULL;
        char                     vpath[PATH_MAX];
        glusterfs_handle_t       h     = NULL;
        char                    *realp = NULL;

        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, path, out);

        gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "path %s", path);

        h = libgf_resolved_path_handle (path, vpath);
        if (!h) {
                errno = ENODEV;
                goto out;
        }

        realp = CALLOC (PATH_MAX, sizeof (char));
        if (!realp)
                goto out;

        libgf_vmp_search_vmp (h, realp, PATH_MAX);
        res = glusterfs_glh_realpath (h, vpath, resolved_path);
        if (!res)
                goto out;

        /* This copy is needed to ensure that when we return the real resolved
         * path, we return a path that accounts for the app's view of the
         * path, i.e. it starts with the VMP, in case this is an absolute path.
         */
        if (libgf_path_absolute (path)) {
                strcat (realp, resolved_path);
                strcpy (resolved_path, realp);
        }

        gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "path %s, resolved %s", path,
                resolved_path);
out:
        if (realp)
                FREE (realp);

        return res;
}

int
glusterfs_glh_remove (glusterfs_handle_t handle, const char *path)
{
        loc_t                           loc = {0, };
        int                             op_ret = -1;
        libglusterfs_client_ctx_t       *ctx = handle;
        char                            *name = NULL;

        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, handle, out);
        GF_VALIDATE_ABSOLUTE_PATH_OR_GOTO (LIBGF_XL_NAME, path, out);

        gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "path %s", path);

        loc.path = strdup (path);
        if (!loc.path) {
                gf_log (LIBGF_XL_NAME, GF_LOG_ERROR, "strdup failed");
                goto out;
        }

        op_ret = libgf_client_path_lookup (&loc, ctx, 1);
        if (op_ret == -1)
                goto out;

        name = strdup (loc.path);
        op_ret = libgf_client_loc_fill (&loc, ctx, 0, loc.parent->ino,
                                        compat_basename (name));
        if (op_ret == -1)
                goto out;

        if (IA_ISDIR (loc.inode->ia_type))
                op_ret = libgf_client_rmdir (ctx, &loc);
        else
                op_ret = libgf_client_unlink (ctx, &loc);

out:
        if (name)
                FREE (name);
        return op_ret;

}

int
glusterfs_remove(const char *pathname)
{
        int                     op_ret = -1;
        char                    vpath[PATH_MAX];
        glusterfs_handle_t      h = NULL;

        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, pathname, out);

        gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "path %s", pathname);

        h = libgf_resolved_path_handle (pathname, vpath);
        if (!h) {
                errno = ENODEV;
                goto out;
        }

        op_ret = glusterfs_glh_remove (h, vpath);
out:
        return op_ret;
}

void
glusterfs_rewinddir (glusterfs_dir_t dirfd)
{
        libglusterfs_client_fd_ctx_t    *fd_ctx = NULL;

        fd_ctx = libgf_get_fd_ctx ((fd_t *)dirfd);
        if (!fd_ctx) {
                gf_log (LIBGF_XL_NAME, GF_LOG_ERROR, "No fd context present");
                errno = EBADF;
                goto out;
        }

        pthread_mutex_lock (&fd_ctx->lock);
        {
                fd_ctx->offset = 0;
                gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "Offset: %"PRIu64,
                        fd_ctx->offset);
        }
        pthread_mutex_unlock (&fd_ctx->lock);

out:
        return;
}

void
glusterfs_seekdir (glusterfs_dir_t dirfd, off_t offset)
{
        libglusterfs_client_fd_ctx_t    *fd_ctx = NULL;

        fd_ctx = libgf_get_fd_ctx ((fd_t *)dirfd);
        if (!fd_ctx) {
                gf_log (LIBGF_XL_NAME, GF_LOG_ERROR, "No fd context present");
                goto out;
        }

        pthread_mutex_lock (&fd_ctx->lock);
        {
                fd_ctx->offset = offset;
                gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "Offset: %"PRIu64,
                        fd_ctx->offset);
        }
        pthread_mutex_unlock (&fd_ctx->lock);

out:
        return;
}

off_t
glusterfs_telldir (glusterfs_dir_t dirfd)
{
        libglusterfs_client_fd_ctx_t    *fd_ctx = NULL;
	off_t                           off = -1;

        fd_ctx = libgf_get_fd_ctx ((fd_t *)dirfd);
        if (!fd_ctx) {
                gf_log (LIBGF_XL_NAME, GF_LOG_ERROR, "No fd context present");
                errno = EBADF;
                goto out;
        }

        pthread_mutex_lock (&fd_ctx->lock);
        {
                off = fd_ctx->offset;
                gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "Offset: %"PRIu64,
                        fd_ctx->offset);
        }
        pthread_mutex_unlock (&fd_ctx->lock);

out:
        return off;
}

struct libgf_client_sendfile_data {
        int                reads_sent;
        int                reads_completed;
        int                out_fd;
        int32_t            op_ret;
        int32_t            op_errno;
        pthread_mutex_t    lock;
        pthread_cond_t     cond;
};

int
libgf_client_sendfile_read_cbk (int op_ret, int op_errno,
                                glusterfs_iobuf_t *buf, void *cbk_data)
{
        struct libgf_client_sendfile_data *sendfile_data = cbk_data;
        int                                bytes = 0;

        if (op_ret > 0) {
                bytes = writev (sendfile_data->out_fd, buf->vector, buf->count);
                if (bytes != op_ret) {
                        op_ret = -1;
                        op_errno = errno;
                }

                glusterfs_free (buf);
        }

        pthread_mutex_lock (&sendfile_data->lock);
        {
                if (sendfile_data->op_ret != -1) {
                        if (op_ret == -1) {
                                sendfile_data->op_ret = -1;
                                sendfile_data->op_errno = op_errno;
                        } else {
                                sendfile_data->op_ret += op_ret;
                        }
                }

                sendfile_data->reads_completed++;

                if (sendfile_data->reads_completed
                    == sendfile_data->reads_sent) {
                        pthread_cond_broadcast (&sendfile_data->cond);
                } 
        }
        pthread_mutex_unlock (&sendfile_data->lock);

        return 0;
}


ssize_t
glusterfs_sendfile (int out_fd, glusterfs_file_t in_fd, off_t *offset,
                    size_t count)
{
        ssize_t                           ret = -1;
        struct libgf_client_sendfile_data cbk_data = {0, };
        off_t                             off = -1;
        size_t                            size = 0;
        int                               flags = 0;
        int                               non_block = 0;

        
        pthread_mutex_init (&cbk_data.lock, NULL);
        pthread_cond_init (&cbk_data.cond, NULL);
        cbk_data.out_fd = out_fd;
        
        if (offset) {
                off = *offset;
        }

        flags = fcntl (out_fd, F_GETFL);

        if (flags != -1) {
                non_block = flags & O_NONBLOCK;

                if (non_block) {
                        ret = fcntl (out_fd, F_SETFL, flags & ~O_NONBLOCK);
                }
        }

        while (count != 0) {
                /* 
                 * FIXME: what's the optimal size for reads and writes?
                 */
                size = (count > LIBGF_SENDFILE_BLOCK_SIZE) ? 
                        LIBGF_SENDFILE_BLOCK_SIZE : count;

                /* 
                 * we don't wait for reply to previous read, we just send all
                 * reads in a single go.
                 */
                ret = glusterfs_read_async (in_fd, size, off,
                                            libgf_client_sendfile_read_cbk,
                                            &cbk_data);
                if (ret == -1) {
                        break;
                }

                pthread_mutex_lock (&cbk_data.lock);
                {
                        cbk_data.reads_sent++;
                }
                pthread_mutex_unlock (&cbk_data.lock);

                if (offset) {
                        off += size;
                }

                count -= size;
        }

        pthread_mutex_lock (&cbk_data.lock);
        {
                /* 
                 * if we've not received replies to all the reads we've sent,
                 * wait for them
                 */
                if (cbk_data.reads_sent > cbk_data.reads_completed) {
                        pthread_cond_wait (&cbk_data.cond,
                                           &cbk_data.lock);
                }
        }
        pthread_mutex_unlock (&cbk_data.lock);

        if (offset != NULL) {
                *offset = off;
        }
 
        /* if we were able to stack_wind all the reads */

        if (ret == 0) {
                ret = cbk_data.op_ret;
                errno = cbk_data.op_errno;
        }

        if (non_block) {
                fcntl (out_fd, F_SETFL, flags);
        }

        return ret;
}


static int32_t
libgf_client_lk_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                     int32_t op_ret, int32_t op_errno, struct flock *lock)
{
        libgf_client_local_t *local = frame->local;

        local->reply_stub = fop_lk_cbk_stub (frame, NULL, op_ret, op_errno,
                                             lock);

        LIBGF_REPLY_NOTIFY (local);

        return 0;
}


int
libgf_client_lk (libglusterfs_client_ctx_t *ctx, fd_t *fd, int cmd,
                 struct flock *lock)
{
        call_stub_t          *stub = NULL;
        int32_t               op_ret;
        libgf_client_local_t *local = NULL;
        
        LIBGF_CLIENT_FOP(ctx, stub, lk, local, fd, cmd, lock);

        op_ret = stub->args.lk_cbk.op_ret;
        errno = stub->args.lk_cbk.op_errno;
        if (op_ret == 0) {
                *lock = stub->args.lk_cbk.lock;
        }

	call_stub_destroy (stub);
        return op_ret;
}


int
glusterfs_fcntl (glusterfs_file_t fd, int cmd, ...)
{
        int                           ret = -1;
        struct flock                 *lock = NULL;
        va_list                       ap;
        libglusterfs_client_ctx_t    *ctx = NULL;
        libglusterfs_client_fd_ctx_t *fd_ctx = NULL;

        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, fd, out);

        fd_ctx = libgf_get_fd_ctx (fd);
        if (!fd_ctx) {
                errno = EBADF;
		goto out;
        }

        ctx = fd_ctx->ctx;

        switch (cmd) {
        case F_SETLK:
        case F_SETLKW:
        case F_GETLK:
#if F_SETLK != F_SETLK64
        case F_SETLK64:
#endif
#if F_SETLKW != F_SETLKW64
        case F_SETLKW64:
#endif
#if F_GETLK != F_GETLK64
        case F_GETLK64:
#endif
                va_start (ap, cmd);
                lock = va_arg (ap, struct flock *);
                va_end (ap);

                if (!lock) {
                        errno = EINVAL;
                        goto out;
                }

                ret = libgf_client_lk (ctx, fd, cmd, lock); 
                break;

        default:
                errno = EINVAL;
                break;
        }

out:
        return ret;
}


int
libgf_client_chdir (const char *path)
{
        int                op_ret            = 0;
        uint32_t           resulting_cwd_len = 0;

        pthread_mutex_lock (&cwdlock);
        {
                if (!libgf_path_absolute (path)) {
                        resulting_cwd_len = strlen (path) + strlen (cwd)
                                + ((path[strlen (path) - 1] == '/')
                                   ? 0 : 1) + 1;

                        if (resulting_cwd_len > PATH_MAX) {
                                op_ret = -1;
                                errno = ENAMETOOLONG;
                                goto unlock;
                        }
                        strcat (cwd, path);
                } else {
                        resulting_cwd_len = strlen (path)
                                + ((path[strlen (path) - 1] == '/')
                                   ? 0 : 1) + 1;
                                
                        if (resulting_cwd_len > PATH_MAX) {
                                op_ret = -1;
                                errno = ENAMETOOLONG;
                                goto unlock;
                        }

                        strcpy (cwd, path);
                }

                if (cwd[strlen (cwd) - 1] != '/') {
                        strcat (cwd, "/");
                }
        }
unlock:
        pthread_mutex_unlock (&cwdlock);

        return op_ret;
}


int
glusterfs_fchdir (glusterfs_file_t fd)
{
        int                           op_ret = -1;
        char                          vpath[PATH_MAX];
        char                          vmp[PATH_MAX]; 
        char                         *res    = NULL;
        libglusterfs_client_fd_ctx_t *fd_ctx = NULL;
        glusterfs_handle_t            handle = NULL;
        
        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, fd, out);

        /* FIXME: there is a race-condition between glusterfs_fchdir and
           glusterfs_close. If two threads of application call glusterfs_fchdir
           and glusterfs_close on the same fd, there is a possibility of
           glusterfs_fchdir accessing freed memory of fd_ctx.
        */

        fd_ctx = libgf_get_fd_ctx (fd);
        if (!fd_ctx) {
                errno = EBADF;
		goto out;
        }

        pthread_mutex_lock (&fd_ctx->lock);
        {
                handle = fd_ctx->ctx;
                strcpy (vpath, fd_ctx->vpath);
        }
        pthread_mutex_unlock (&fd_ctx->lock);

        if (vpath[0] == '\0') {
                errno = ENOTDIR;
                goto out;
        }

        res = libgf_vmp_search_vmp (handle, vmp, PATH_MAX);
        if (res == NULL) {
                errno = EBADF;
                goto out;
        }

        /* both vmp and vpath are terminated with '/'. Also path starts with a
           '/'. Hence the extra '/' amounts to NULL character at the end of the
           string.
        */
        if ((strlen (vmp) + strlen (vpath)) > PATH_MAX) {
                errno = ENAMETOOLONG;
                goto out;
        }

        pthread_mutex_lock (&cwdlock);
        {
                strcpy (cwd, vmp);
                res = vpath;
                if (res[0] == '/') {
                        res++;
                }

                strcat (cwd, res);
        }
        pthread_mutex_unlock (&cwdlock);

        op_ret = 0; 
out:
        return op_ret;
}


int
glusterfs_chdir (const char *path)
{
        int32_t            op_ret            = -1;
        glusterfs_handle_t handle            = NULL;
        loc_t              loc               = {0, };
        char               vpath[PATH_MAX]; 

        handle = libgf_resolved_path_handle (path, vpath);

        if (handle != NULL)  {
                loc.path = strdup (vpath);
                if (!loc.path) {
                        gf_log (LIBGF_XL_NAME, GF_LOG_ERROR, "Path compaction "
                                "failed");
                        goto out;
                }

                op_ret = libgf_client_path_lookup (&loc, handle, 0);
        }

        if ((handle == NULL) || (op_ret == 0)) {
                op_ret = libgf_client_chdir (path);
        }

out:
        return op_ret;
}


char *
glusterfs_getcwd (char *buf, size_t size)
{
        char              *res      = NULL;
        size_t             len      = 0; 
        loc_t              loc      = {0, };
        glusterfs_handle_t handle   = NULL;
        char               vpath[PATH_MAX];
        int32_t            op_ret   = 0; 

        pthread_mutex_lock (&cwdlock);
        {
                if (!cwd_inited) {
                        errno = ENODEV;
                        goto unlock;
                }

                if (buf == NULL) {
                        buf = CALLOC (1, len);
                        if (buf == NULL) {
                                gf_log (LIBGF_XL_NAME, GF_LOG_ERROR,
                                        "out of memory");
                                goto unlock;
                        }
                } else {
                        if (size == 0) {
                                errno = EINVAL;
                                goto unlock;
                        }

                        if (len > size) {
                                errno = ERANGE;
                                goto unlock;
                        }
                }

                strcpy (buf, cwd);
                res = buf; 
        }
unlock:
        pthread_mutex_unlock (&cwdlock);

        if (res != NULL) {
                handle = libgf_resolved_path_handle (res, vpath);

                if (handle != NULL)  {
                        loc.path = strdup (vpath);
                        if (loc.path == NULL) {
                                gf_log (LIBGF_XL_NAME, GF_LOG_ERROR,
                                        "strdup failed");
                        } else {
                                op_ret = libgf_client_path_lookup (&loc, handle,
                                                                   0);
                                if (op_ret == -1) {
                                        res = NULL;
                                }
                        }
                }
        }

        return res;
}

int32_t
libgf_client_truncate_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                           int32_t op_ret, int32_t op_errno,
                           struct iatt *prebuf, struct iatt *postbuf)
{
        libgf_client_local_t *local = frame->local;

        local->reply_stub = fop_truncate_cbk_stub (frame, NULL, op_ret,
                                                   op_errno, prebuf, postbuf);

        LIBGF_REPLY_NOTIFY (local);
        return 0;
}

int32_t 
libgf_client_truncate (libglusterfs_client_ctx_t *ctx, 
                       loc_t *loc, off_t length)
{
        call_stub_t *stub = NULL;
        int32_t op_ret = 0;
        libgf_client_local_t *local = NULL;

        LIBGF_CLIENT_FOP (ctx, stub, truncate, local, loc, length);
 
        op_ret = stub->args.truncate_cbk.op_ret;
        errno = stub->args.truncate_cbk.op_errno;

        gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "path %s, status %d, errno %d",
                loc->path, op_ret, errno);

        if (op_ret == -1) {
                goto out;
        }

        libgf_transform_iattr (ctx, loc->inode,
                               &stub->args.truncate_cbk.postbuf);

        libgf_update_iattr_cache (loc->inode, LIBGF_UPDATE_STAT,
                                  &stub->args.truncate_cbk.postbuf);
	call_stub_destroy (stub);

out:
        return op_ret;
}

int
glusterfs_glh_truncate (glusterfs_handle_t handle, const char *path,
                        off_t length)
{
        int32_t op_ret = -1;
        loc_t loc = {0, };
        libglusterfs_client_ctx_t *ctx = handle;
	char *name = NULL, *pathname = NULL;

        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, ctx, out);
        GF_VALIDATE_ABSOLUTE_PATH_OR_GOTO (LIBGF_XL_NAME, path, out);

        loc.path = strdup (path);
        if (!loc.path) {
                gf_log (LIBGF_XL_NAME, GF_LOG_ERROR, "strdup failed");
                goto out;
        }

	op_ret = libgf_client_path_lookup (&loc, ctx, 1);
	if (op_ret == -1) {
		gf_log ("libglusterfsclient", GF_LOG_ERROR,
			"path lookup failed for (%s)", loc.path);
		goto out;
	}

	pathname = strdup (loc.path);
	name = compat_basename (pathname);

        op_ret = libgf_client_loc_fill (&loc, ctx, 0, loc.parent->ino, name);
	if (op_ret == -1) {
		gf_log ("libglusterfsclient",
			GF_LOG_ERROR,
			"libgf_client_loc_fill returned -1, returning EINVAL");
		errno = EINVAL;
		goto out;
	}

        op_ret = libgf_client_truncate (ctx, &loc, length);

out:
        libgf_client_loc_wipe (&loc);

        return op_ret;
}

int
glusterfs_truncate (const char *path, off_t length)
{
        int                     op_ret = -1;
        char                    vpath[PATH_MAX];
        glusterfs_handle_t      h = NULL;

        GF_VALIDATE_OR_GOTO (LIBGF_XL_NAME, path, out);

        gf_log (LIBGF_XL_NAME, GF_LOG_DEBUG, "path:%s length:%"PRIu64, path,
                length);
        h = libgf_resolved_path_handle (path, vpath);
        if (!h) {
                errno = ENODEV;
                goto out;
        }

        op_ret = glusterfs_glh_truncate (h, vpath, length);
out:
        return op_ret;
}

static struct xlator_fops libgf_client_fops = {
};

static struct xlator_cbks libgf_client_cbks = {
        .forget      = libgf_client_forget,
	.release     = libgf_client_release,
	.releasedir  = libgf_client_releasedir,
};

static inline xlator_t *
libglusterfs_graph (xlator_t *graph)
{
        int       ret = 0;
        xlator_t *top = NULL;
        xlator_list_t *xlchild, *xlparent;

        top = CALLOC (1, sizeof (*top));
        ERR_ABORT (top);

        xlchild = CALLOC (1, sizeof(*xlchild));
        ERR_ABORT (xlchild);
        xlchild->xlator = graph;
        top->children = xlchild;
        top->ctx = graph->ctx;
        top->next = graph;
        top->name = strdup (LIBGF_XL_NAME);

        xlparent = CALLOC (1, sizeof(*xlparent));
        xlparent->xlator = top;
        graph->parents = xlparent;
        ret = asprintf (&top->type, LIBGF_XL_NAME);
        if (-1 == ret) {
                fprintf (stderr, "failed to set the top xl's type");
        }

        top->init = libgf_client_init;
        top->fops = &libgf_client_fops;
        top->mops = &libgf_client_mops;
        top->cbks = &libgf_client_cbks; 
        top->notify = libgf_client_notify;
        top->fini = libgf_client_fini;
        //  fill_defaults (top);

        return top;
}
