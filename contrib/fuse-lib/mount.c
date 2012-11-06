/*
  FUSE: Filesystem in Userspace
  Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>
  Copyright (c) 2010 Gluster, Inc. <http://www.gluster.com>

  This program can be distributed under the terms of the GNU LGPLv2.
  See the file COPYING.LIB.
*/

#ifndef _CONFIG_H
#define _CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stddef.h>
#include <limits.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <signal.h>
#ifdef GF_LINUX_HOST_OS
#include <mntent.h>
#endif /* GF_LINUX_HOST_OS */
#include <sys/stat.h>
#include <sys/poll.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/mount.h>

#ifdef HAVE_PERFUSE_H
#include <perfuse.h>
#endif /* HAVE_PERFUSE_H */

#ifdef GF_LINUX_HOST_OS
#define _PATH_MOUNT "/bin/mount"
#else /* NetBSD, MacOS X, FreeBSD */
#define _PATH_MOUNT "/sbin/mount"
#endif

#ifdef FUSE_UTIL
#define MALLOC(size) malloc (size)
#define FREE(ptr) free (ptr)
#define GFFUSE_LOGERR(...) fprintf (stderr, ## __VA_ARGS__)
#else /* FUSE_UTIL */
#include "glusterfs.h"
#include "logging.h"
#include "common-utils.h"

#ifdef GF_FUSERMOUNT
#define FUSERMOUNT_PROG FUSERMOUNT_DIR "/fusermount-glusterfs"
#else
#define FUSERMOUNT_PROG "fusermount"
#endif
#define FUSE_DEVFD_ENV "_FUSE_DEVFD"

#define GFFUSE_LOGERR(...) \
        gf_log ("glusterfs-fuse", GF_LOG_ERROR, ## __VA_ARGS__)
#endif /* !FUSE_UTIL */

/*
 * Functions below, until following note, were taken from libfuse
 * (http://git.gluster.com/?p=users/csaba/fuse.git;a=commit;h=b988bbf9)
 * almost verbatim. What has been changed:
 * - style adopted to that of glusterfs
 * - s/fprintf/gf_log/
 * - s/free/FREE/, s/malloc/MALLOC/
 * - there are some other minor things
 */

static int
mtab_needs_update (const char *mnt)
{
#ifdef GF_LINUX_HOST_OS
        int res;
        struct stat stbuf;

        /* If mtab is within new mount, don't touch it */
        if (strncmp (mnt, _PATH_MOUNTED, strlen (mnt)) == 0 &&
            _PATH_MOUNTED[strlen (mnt)] == '/')
                return 0;

        /*
         * Skip mtab update if /etc/mtab:
         *
         *  - doesn't exist,
         *  - is a symlink,
         *  - is on a read-only filesystem.
         */
        res = lstat (_PATH_MOUNTED, &stbuf);
        if (res == -1) {
                if (errno == ENOENT)
                        return 0;
        } else {
                if (S_ISLNK (stbuf.st_mode))
                        return 0;

                res = access (_PATH_MOUNTED, W_OK);
                if (res == -1 && errno == EROFS)
                        return 0;
        }
#endif /* !GF_LINUX_HOST_OS */

        return 1;
}

#ifndef FUSE_UTIL
static
#endif
int
fuse_mnt_add_mount (const char *progname, const char *fsname,
                    const char *mnt, const char *type, const char *opts)
{
        int res;
        int status;
        sigset_t blockmask;
        sigset_t oldmask;

        if (!mtab_needs_update (mnt))
                return 0;

        sigemptyset (&blockmask);
        sigaddset (&blockmask, SIGCHLD);
        res = sigprocmask (SIG_BLOCK, &blockmask, &oldmask);
        if (res == -1) {
                GFFUSE_LOGERR ("%s: sigprocmask: %s",
                               progname, strerror (errno));
                return -1;
        }

        res = fork ();
        if (res == -1) {
                GFFUSE_LOGERR ("%s: fork: %s", progname, strerror (errno));
                goto out_restore;
        }
        if (res == 0) {
                char templ[] = "/tmp/fusermountXXXXXX";
                char *tmp;

                sigprocmask (SIG_SETMASK, &oldmask, NULL);
                setuid (geteuid ());

                /*
                 * hide in a directory, where mount isn't able to resolve
                 * fsname as a valid path
                 */
                tmp = mkdtemp (templ);
                if (!tmp) {
                        GFFUSE_LOGERR ("%s: failed to create temporary directory",
                                       progname);
                        exit (1);
                }
                if (chdir (tmp)) {
                        GFFUSE_LOGERR ("%s: failed to chdir to %s: %s",
                                       progname, tmp, strerror (errno));
                        exit (1);
                }
                rmdir (tmp);
                execl (_PATH_MOUNT, _PATH_MOUNT, "-i", "-f", "-t", type,
                       "-o", opts, fsname, mnt, NULL);
                GFFUSE_LOGERR ("%s: failed to execute %s: %s",
                               progname, _PATH_MOUNT, strerror (errno));
                exit (1);
        }

        res = waitpid (res, &status, 0);
        if (res == -1)
                GFFUSE_LOGERR ("%s: waitpid: %s", progname, strerror (errno));
        res = (res != -1 && status == 0) ? 0 : -1;

 out_restore:
        sigprocmask (SIG_SETMASK, &oldmask, NULL);
        return res;
}

#ifndef FUSE_UTIL
static
#endif
char
*fuse_mnt_resolve_path (const char *progname, const char *orig)
{
        char buf[PATH_MAX];
        char *copy;
        char *dst;
        char *end;
        char *lastcomp;
        const char *toresolv;

        if (!orig[0]) {
                GFFUSE_LOGERR ("%s: invalid mountpoint '%s'", progname, orig);
                return NULL;
        }

        copy = strdup (orig);
        if (copy == NULL) {
                GFFUSE_LOGERR ("%s: failed to allocate memory", progname);
                return NULL;
        }

        toresolv = copy;
        lastcomp = NULL;
        for (end = copy + strlen (copy) - 1; end > copy && *end == '/'; end --);
        if (end[0] != '/') {
                char *tmp;
                end[1] = '\0';
                tmp = strrchr (copy, '/');
                if (tmp == NULL) {
                        lastcomp = copy;
                        toresolv = ".";
                } else {
                        lastcomp = tmp + 1;
                        if (tmp == copy)
                                toresolv = "/";
                }
                if (strcmp (lastcomp, ".") == 0 || strcmp (lastcomp, "..") == 0) {
                        lastcomp = NULL;
                        toresolv = copy;
                }
                else if (tmp)
                        tmp[0] = '\0';
        }
        if (realpath (toresolv, buf) == NULL) {
                GFFUSE_LOGERR ("%s: bad mount point %s: %s", progname, orig,
                               strerror (errno));
                FREE (copy);
                return NULL;
        }
        if (lastcomp == NULL)
                dst = strdup (buf);
        else {
                dst = (char *) MALLOC (strlen (buf) + 1 + strlen (lastcomp) + 1);
                if (dst) {
                        unsigned buflen = strlen (buf);
                        if (buflen && buf[buflen-1] == '/')
                                sprintf (dst, "%s%s", buf, lastcomp);
                        else
                                sprintf (dst, "%s/%s", buf, lastcomp);
                }
        }
        FREE (copy);
        if (dst == NULL)
                GFFUSE_LOGERR ("%s: failed to allocate memory", progname);
        return dst;
}

#ifndef FUSE_UTIL
static char *
escape (char *s)
{
        size_t len = 0;
        char *p = NULL;
        char *q = NULL;
        char *e = NULL;

        for (p = s; *p; p++) {
                if (*p == ',')
                       len++;
                len++;
        }

        e = CALLOC (1, len + 1);
        if (!e)
                return NULL;

        for (p = s, q = e; *p; p++, q++) {
                if (*p == ',') {
                        *q = '\\';
                        q++;
                }
                *q = *p;
        }

        return e;
}

static int
fuse_mount_fusermount (const char *mountpoint, char *fsname, char *mnt_param,
                       int fd)
{
        int  pid = -1;
        int  res = 0;
        int  ret = -1;
        char *fm_mnt_params = NULL;
        char *efsname = NULL;

#ifndef GF_FUSERMOUNT
        GFFUSE_LOGERR ("Mounting via helper utility "
                       "(unprivileged mounting) is supported "
                       "only if glusterfs is compiled with "
                       "--enable-fusermount");
        return -1;
#endif

        efsname = escape (fsname);
        if (!efsname) {
                GFFUSE_LOGERR ("Out of memory");

                return -1;
        }
        ret = asprintf (&fm_mnt_params,
                        "%s,fsname=%s,nonempty,subtype=glusterfs",
                        mnt_param, efsname);
        FREE (efsname);
        if (ret == -1) {
                GFFUSE_LOGERR ("Out of memory");

                goto out;
        }

        /* fork to exec fusermount */
        pid = fork ();
        if (pid == -1) {
                GFFUSE_LOGERR ("fork() failed: %s", strerror (errno));
                ret = -1;
                goto out;
        }

        if (pid == 0) {
                char env[10];
                const char *argv[32];
                int a = 0;

                argv[a++] = FUSERMOUNT_PROG;
                argv[a++] = "-o";
                argv[a++] = fm_mnt_params;
                argv[a++] = "--";
                argv[a++] = mountpoint;
                argv[a++] = NULL;

                snprintf (env, sizeof (env), "%i", fd);
                setenv (FUSE_DEVFD_ENV, env, 1);
                execvp (FUSERMOUNT_PROG, (char **)argv);
                GFFUSE_LOGERR ("failed to exec fusermount: %s",
                               strerror (errno));
                _exit (1);
        }

        ret = waitpid (pid, &res, 0);
        ret = (ret == pid && res == 0) ? 0 : -1;
 out:
        FREE (fm_mnt_params);
        return ret;
}
#endif

#ifdef GF_BSD_HOST_OS
#define umount2(dir, flags) unmount(dir, ((flags) != 0) ? MNT_FORCE : 0)
#endif

#ifndef FUSE_UTIL
static
#endif
int
fuse_mnt_umount (const char *progname, const char *abs_mnt,
                 const char *rel_mnt, int lazy)
{
        int res;
        int status;
        sigset_t blockmask;
        sigset_t oldmask;

        if (!mtab_needs_update (abs_mnt)) {
                res = umount2 (rel_mnt, lazy ? 2 : 0);
                if (res == -1)
                        GFFUSE_LOGERR ("%s: failed to unmount %s: %s",
                                       progname, abs_mnt, strerror (errno));
                return res;
        }

        sigemptyset (&blockmask);
        sigaddset (&blockmask, SIGCHLD);
        res = sigprocmask (SIG_BLOCK, &blockmask, &oldmask);
        if (res == -1) {
                GFFUSE_LOGERR ("%s: sigprocmask: %s", progname,
                               strerror (errno));
                return -1;
        }

        res = fork ();
        if (res == -1) {
                GFFUSE_LOGERR ("%s: fork: %s", progname, strerror (errno));
                goto out_restore;
        }
        if (res == 0) {
                sigprocmask (SIG_SETMASK, &oldmask, NULL);
                setuid (geteuid ());
                execl ("/bin/umount", "/bin/umount", "-i", rel_mnt,
                      lazy ? "-l" : NULL, NULL);
                GFFUSE_LOGERR ("%s: failed to execute /bin/umount: %s",
                               progname, strerror (errno));
                exit (1);
        }
        res = waitpid (res, &status, 0);
        if (res == -1)
                GFFUSE_LOGERR ("%s: waitpid: %s", progname, strerror (errno));

        if (status != 0)
                res = -1;

 out_restore:
        sigprocmask (SIG_SETMASK, &oldmask, NULL);
        return res;
}

#ifdef FUSE_UTIL
int
fuse_mnt_check_empty (const char *progname, const char *mnt,
                      mode_t rootmode, off_t rootsize)
{
        int isempty = 1;

        if (S_ISDIR (rootmode)) {
                struct dirent *ent;
                DIR *dp = opendir (mnt);
                if (dp == NULL) {
                        fprintf (stderr,
                                 "%s: failed to open mountpoint for reading: %s\n",
                                 progname, strerror (errno));
                        return -1;
                }
                while ((ent = readdir (dp)) != NULL) {
                        if (strcmp (ent->d_name, ".") != 0 &&
                            strcmp (ent->d_name, "..") != 0) {
                                isempty = 0;
                                break;
                        }
                }
                closedir (dp);
        } else if (rootsize)
                isempty = 0;

        if (!isempty) {
                fprintf (stderr, "%s: mountpoint is not empty\n", progname);
                fprintf (stderr, "%s: if you are sure this is safe, "
                         "use the 'nonempty' mount option\n", progname);
                return -1;
        }
        return 0;
}

int
fuse_mnt_check_fuseblk (void)
{
        char buf[256];
        FILE *f = fopen ("/proc/filesystems", "r");
        if (!f)
                return 1;

        while (fgets (buf, sizeof (buf), f))
                if (strstr (buf, "fuseblk\n")) {
                        fclose (f);
                        return 1;
                }

        fclose (f);
        return 0;
}
#endif

#ifndef FUSE_UTIL
void
gf_fuse_unmount (const char *mountpoint, int fd)
{
        int res;
        int pid;

        if (!mountpoint)
                return;

        if (fd != -1) {
                struct pollfd pfd;

                pfd.fd = fd;
                pfd.events = 0;
                res = poll (&pfd, 1, 0);
                /* If file poll returns POLLERR on the device file descriptor,
                   then the filesystem is already unmounted */
                if (res == 1 && (pfd.revents & POLLERR))
                        return;

                /* Need to close file descriptor, otherwise synchronous umount
                   would recurse into filesystem, and deadlock */
                close (fd);
        }

        if (geteuid () == 0) {
                fuse_mnt_umount ("fuse", mountpoint, mountpoint, 1);
                return;
        }

        res = umount2 (mountpoint, 2);
        if (res == 0)
                return;

        pid = fork ();
        if (pid == -1)
                return;

        if (pid == 0) {
                const char *argv[] = { FUSERMOUNT_PROG, "-u", "-q", "-z",
                                       "--", mountpoint, NULL };

                execvp (FUSERMOUNT_PROG, (char **)argv);
                _exit (1);
        }
        waitpid (pid, NULL, 0);
}
#endif

/*
 * Functions below are loosely modelled after similar functions of libfuse
 */

#ifndef FUSE_UTIL
static int
fuse_mount_sys (const char *mountpoint, char *fsname, char *mnt_param, int fd)
{
        int ret = -1;
        unsigned mounted = 0;
        char *mnt_param_mnt = NULL;
        char *fstype = "fuse.glusterfs";
        char *source = fsname;

        ret = asprintf (&mnt_param_mnt,
                        "%s,fd=%i,rootmode=%o,user_id=%i,group_id=%i",
                        mnt_param, fd, S_IFDIR, getuid (), getgid ());
        if (ret == -1) {
                GFFUSE_LOGERR ("Out of memory");

                goto out;
        }
#ifdef __FreeBSD__
        ret = mount (source, mountpoint, 0, mnt_param_mnt);
#else
        ret = mount (source, mountpoint, fstype, 0,
                     mnt_param_mnt);
#endif
#ifdef GF_LINUX_HOST_OS
        if (ret == -1 && errno == ENODEV) {
                /* fs subtype support was added by 79c0b2df aka
                   v2.6.21-3159-g79c0b2d. Probably we have an
                   older kernel ... */
                fstype = "fuse";
                ret = asprintf (&source, "glusterfs#%s", fsname);
                if (ret == -1) {
                        GFFUSE_LOGERR ("Out of memory");

                        goto out;
                }
                ret = mount (source, mountpoint, fstype, 0,
                             mnt_param_mnt);
        }
#endif /* GF_LINUX_HOST_OS */

        if (ret == -1)
                goto out;
        else
                mounted = 1;

#ifdef GF_LINUX_HOST_OS
        if (geteuid () == 0) {
                char *newmnt = fuse_mnt_resolve_path ("fuse", mountpoint);

                if (!newmnt) {
                        ret = -1;

                        goto out;
                }

                ret = fuse_mnt_add_mount ("fuse", source, newmnt, fstype,
                                          mnt_param);
                FREE (newmnt);
                if (ret == -1) {
                        GFFUSE_LOGERR ("failed to add mtab entry");

                        goto out;
                }
        }
#endif /* GF_LINUX_HOST_OS */

out:
        if (ret == -1) {
                if (mounted)
                        umount2 (mountpoint, 2); /* lazy umount */
        }
        FREE (mnt_param_mnt);
        if (source != fsname)
                FREE (source);

        return ret;
}

int
gf_fuse_mount (const char *mountpoint, char *fsname, char *mnt_param,
               pid_t *mnt_pid, int status_fd)
{
        int   fd  = -1;
        pid_t pid = -1;
        int   ret = -1;

        fd = open ("/dev/fuse", O_RDWR);
        if (fd == -1) {
                GFFUSE_LOGERR ("cannot open /dev/fuse (%s)",
                                strerror (errno));
                return -1;
        }

        /* start mount agent */
        pid = fork();
        switch (pid) {
        case 0:
                /* hello it's mount agent */
                if (!mnt_pid) {
                        /* daemonize mount agent, caller is
                         * not interested in waiting for it
                         */
                        pid = fork ();
                        if (pid)
                                exit (pid == -1 ? 1 : 0);
                }

                ret = fuse_mount_sys (mountpoint, fsname, mnt_param, fd);
                if (ret == -1) {
                        gf_log ("glusterfs-fuse", GF_LOG_INFO,
                                "direct mount failed (%s), "
                                "retry to mount via fusermount",
                                strerror (errno));

                        ret = fuse_mount_fusermount (mountpoint, fsname,
                                                     mnt_param, fd);
                }

                if (ret == -1)
                        GFFUSE_LOGERR ("mount failed");

                if (status_fd >= 0)
                        (void)write (status_fd, &ret, sizeof (ret));
                exit (!!ret);
                /* bye mount agent */
        case -1:
                close (fd);
                fd = -1;
        }

        if (mnt_pid)
               *mnt_pid = pid;

        return fd;
}
#endif
