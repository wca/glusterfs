/*
  Copyright (c) 2008-2012 Red Hat, Inc. <http://www.redhat.com>
  This file is part of GlusterFS.

  This file is licensed to you under your choice of the GNU Lesser
  General Public License, version 3 or any later version (LGPLv3 or
  later), or the GNU General Public License, version 2 (GPLv2), in all
  cases as published by the Free Software Foundation.
*/

#include "libglusterfsclient.h"
#include "libglusterfsclient-internals.h"
#include <libgen.h>

#define LIBGLUSTERFS_CLIENT_DENTRY_LOC_PREPARE(_new_loc, _loc, _parent, \
                                               _resolved) do {          \
                size_t pathlen = 0;                                     \
                size_t resolvedlen = 0;                                 \
                char *path = NULL;                                      \
                int pad = 0;                                            \
                pathlen   = strlen (_loc->path) + 1;                    \
                path = CALLOC (1, pathlen);                             \
                _new_loc.parent =  _parent;                             \
                resolvedlen = strlen (_resolved);                       \
                strncpy (path, _resolved, resolvedlen);                 \
                if (resolvedlen == 1) /* only root resolved */          \
                        pad = 0;                                        \
                else {                                                  \
                        pad = 1;                                        \
                        path[resolvedlen] = '/';                        \
                }                                                       \
                strcpy_till (path + resolvedlen + pad,                  \
                             loc->path + resolvedlen + pad, '/');       \
                _new_loc.path = path;                                   \
                _new_loc.name = strrchr (path, '/');                    \
                if (_new_loc.name)                                      \
                        _new_loc.name++;                                \
        }while (0);


/* strcpy_till - copy @dname to @dest, until 'delim' is encountered in @dest
 * @dest - destination string
 * @dname - source string
 * @delim - delimiter character
 *
 * return - NULL is returned if '0' is encountered in @dname, otherwise returns
 *          a pointer to remaining string begining in @dest.
 */
static char *
strcpy_till (char *dest, const char *dname, char delim)
{
        char *src = NULL;
        int idx = 0;
        char *ret = NULL;

        src = (char *)dname;
        while (src[idx] && (src[idx] != delim)) {
                dest[idx] = src[idx];
                idx++;
        }

        dest[idx] = 0;

        if (src[idx] == 0)
                ret = NULL;
        else
                ret = &(src[idx]);

        return ret;
}

/* __libgf_client_path_to_parenti - derive parent inode for @path. if immediate 
 *                            parent is not available in the dentry cache, return nearest
 *                            available parent inode and set @reslv to the path of
 *                            the returned directory.
 *
 * @itable - inode table
 * @path   - path whose parent has to be looked up.
 * @reslv  - if immediate parent is not available, reslv will be set to path of the
 *           resolved parent.
 *
 * return - should never return NULL. should at least return '/' inode.
 */
static inode_t *
__libgf_client_path_to_parenti (libglusterfs_client_ctx_t *ctx,
                                inode_table_t *itable, const char *path,
                                char **reslv)
{
        char *resolved_till = NULL;
        char *strtokptr = NULL;
        char *component = NULL;
        char *next_component = NULL;
        char *pathdup = NULL;
        inode_t *curr = NULL;
        inode_t *parent = NULL;
        size_t pathlen = 0;
        loc_t rootloc = {0, };
        int ret = -1;

        pathlen = STRLEN_0 (path);
        resolved_till = CALLOC (1, pathlen);

        GF_VALIDATE_OR_GOTO("libglusterfsclient-dentry", resolved_till, out);
        pathdup = strdup (path);
        GF_VALIDATE_OR_GOTO("libglusterfsclient-dentry", pathdup, out);

        parent = inode_ref (itable->root);
        /* If the root inode's is outdated, send a revalidate on it.
         * A revalidate on root inode also reduces the window in which an
         * op will fail over distribute because the layout of the root
         * directory did not  get constructed when we sent the lookup on
         * root in glusterfs_init. That can happen when not all children of a
         * distribute volume were up at the time of glusterfs_init.
         */
        if (!libgf_is_iattr_cache_valid (ctx, parent, NULL,
                                        LIBGF_VALIDATE_LOOKUP)) {
                libgf_client_loc_fill (&rootloc, ctx, 1, 0, "/");
                ret = libgf_client_lookup (ctx, &rootloc, NULL, NULL, NULL);
                if (ret == -1) {
                        gf_log ("libglusterfsclient-dentry", GF_LOG_ERROR,
                                "Root inode revalidation failed");
                        inode_unref (parent);
                        parent = NULL;
                        goto out;
                }
                libgf_client_loc_wipe (&rootloc);
        }

        curr = NULL;

        component = strtok_r (pathdup, "/", &strtokptr);

        while (component) {
                curr = inode_search (itable, parent->ino, component);
                if (!curr) {
                        break;
                }
                if (!libgf_is_iattr_cache_valid (ctx, curr, NULL,
                                                LIBGF_VALIDATE_LOOKUP))
                        break;

                /* It is OK to append the component even if it is the       
                   last component in the path, because, if 'next_component'
                   returns NULL, @parent will remain the same and
                   @resolved_till will not be sent back               
                */
                strcat (resolved_till, "/");
                strcat (resolved_till, component);

                next_component = strtok_r (NULL, "/", &strtokptr);

                if (next_component) {
                        inode_unref (parent);
                        parent = curr;
                        curr = NULL;
                } else {
                        /* will break */
                        inode_unref (curr);
                }

                component = next_component;
        }

        if (resolved_till[0] == '\0') {
                strcat (resolved_till, "/");
        }

        free (pathdup);
        
        if (reslv) {
                *reslv = resolved_till;
        } else {
                FREE (resolved_till);
        }

out:
        return parent;
}

static inline void
libgf_client_update_resolved (const char *path, char *resolved)
{
        int32_t pathlen = 0;
        char *tmp = NULL, *dest = NULL, *dname = NULL;
        char append_slash = 0;

        pathlen = strlen (resolved); 
        tmp = (char *)(resolved + pathlen);
        if (*((char *) (resolved + pathlen - 1)) != '/') {
                tmp[0] = '/';
                append_slash = 1;
        }

        if (append_slash) {
                dest = tmp + 1;
        } else {
                dest = tmp;
        }

        if (*((char *) path + pathlen) == '/') {
                dname = (char *) path + pathlen + 1;
        } else {
                dname = (char *) path + pathlen;
        }

        strcpy_till (dest, dname, '/');
}

/* __do_path_resolve - resolve @loc->path into @loc->inode and @loc->parent. also
 *                     update the dentry cache
 *
 * @loc             - loc to resolve. 
 * @ctx             - libglusterfsclient context
 * @lookup_basename - flag whether to lookup basename(loc->path)
 *
 * return - 0 on success
 *         -1 on failure 
 *          
 */
static int32_t
__do_path_resolve (loc_t *loc, libglusterfs_client_ctx_t *ctx,
                   char lookup_basename)
{
        int32_t         op_ret = -1;
        char           *resolved  = NULL;
        inode_t        *parent = NULL, *inode = NULL;
        dentry_t       *dentry = NULL;
        loc_t          new_loc = {0, };
	char           *pathname = NULL, *directory = NULL;
	char           *file = NULL;   
        
        parent = loc->parent;
        if (parent) {
                inode_ref (parent);
                gf_log ("libglusterfsclient-dentry", GF_LOG_DEBUG,
                        "loc->parent(%"PRId64") already present. sending "
                        "lookup for %"PRId64"/%s", parent->ino, parent->ino,
                        loc->path);
                resolved = strdup (loc->path);
                resolved = dirname (resolved);
        } else {
                parent = __libgf_client_path_to_parenti (ctx, ctx->itable,
                                                         loc->path, &resolved);
        }

        if (parent == NULL) {
                /* fire in the bush.. run! run!! run!!! */
                gf_log ("libglusterfsclient-dentry",
                        GF_LOG_CRITICAL,
                        "failed to get parent inode number");
                op_ret = -1;
                goto out;
        }               

        gf_log ("libglusterfsclient-dentry",
                GF_LOG_DEBUG,
                "resolved path(%s) till %"PRId64"(%s). "
                "sending lookup for remaining path",
                loc->path, parent->ino, resolved);

	pathname = strdup (loc->path);
	directory = dirname (pathname);
        pathname = NULL;

        while (strcmp (resolved, directory) != 0) 
        {
                dentry = NULL;

                LIBGLUSTERFS_CLIENT_DENTRY_LOC_PREPARE (new_loc, loc, parent,
                                                        resolved);

		if (pathname) {
			free (pathname);
			pathname = NULL;
		}

		pathname = strdup (new_loc.path);
		file = compat_basename (pathname);

                new_loc.inode = inode_search (ctx->itable, parent->ino, file);
                if (new_loc.inode) {
                        if (libgf_is_iattr_cache_valid (ctx, new_loc.inode,
                                                        NULL,
                                                        LIBGF_VALIDATE_LOOKUP))
                                dentry = dentry_search_for_inode (new_loc.inode,
                                                                  parent->ino,
                                                                  file);
                }

                if (dentry == NULL) {
                        op_ret = libgf_client_lookup (ctx, &new_loc, NULL, NULL,
                                                      0);
                        if (op_ret == -1) {
                                inode_ref (new_loc.parent);
                                libgf_client_loc_wipe (&new_loc);
                                goto out;
                        }
                }

                parent = inode_ref (new_loc.inode);
                libgf_client_loc_wipe (&new_loc);

                libgf_client_update_resolved (loc->path, resolved);
        }

	if (pathname) {
		free (pathname);
		pathname = NULL;
	} 

        if (lookup_basename) {
                pathname = strdup (loc->path);
                file = compat_basename (pathname);

                inode = inode_search (ctx->itable, parent->ino, file);
                if (!inode) {
                        libgf_client_loc_fill (&new_loc, ctx, 0, parent->ino,
                                               file);

                        op_ret = libgf_client_lookup (ctx, &new_loc, NULL, NULL,
                                                      0);
                        if (op_ret == -1) {
                                libgf_client_loc_wipe (&new_loc);
                                goto out;
                        }
                
                        inode = inode_ref (new_loc.inode);
                        libgf_client_loc_wipe (&new_loc);
                }
        }

        op_ret = 0;
out:
        loc->inode = inode;
        loc->parent = parent;

        FREE (resolved);
	if (pathname) {
		FREE (pathname);
	}

	if (directory) {
		FREE (directory);
	}

        return op_ret;
}


/* resolves loc->path to loc->parent and loc->inode */
int32_t
libgf_client_path_lookup (loc_t *loc,
                          libglusterfs_client_ctx_t *ctx,
                          char lookup_basename)
{
        char       *pathname  = NULL;
        char       *directory = NULL;
        inode_t    *inode = NULL;
        inode_t    *parent = NULL;
        int32_t     op_ret = 0;

        pathname  = strdup (loc->path);
        directory = dirname (pathname);
        parent = inode_from_path (ctx->itable, directory);

        if (parent != NULL) {
                loc->parent = parent;

                if (!lookup_basename) {
                        gf_log ("libglusterfsclient",
                                GF_LOG_DEBUG,
                                "resolved dirname(%s) to %"PRId64,
                                loc->path, parent->ino);
                        goto out;
                } else {
                        inode = inode_from_path (ctx->itable, loc->path);
                        if (inode != NULL) {
                                gf_log ("libglusterfsclient",
                                        GF_LOG_DEBUG,
                                        "resolved path(%s) to %"PRId64"/%"PRId64,
                                        loc->path, parent->ino, inode->ino);
                                loc->inode = inode;
                                goto out;
                        }
                }
        }

        if (parent) {
                inode_unref (parent);
        } else if (inode) {
                inode_unref (inode);
                gf_log ("libglusterfsclient",
                        GF_LOG_ERROR,
                        "undesired behaviour. inode(%"PRId64") for %s "
                        "exists without parent (%s)",
                        inode->ino, loc->path, directory);
        }
        op_ret = __do_path_resolve (loc, ctx, lookup_basename);
out:    
        if (pathname)
                free (pathname);

        return op_ret;
}
