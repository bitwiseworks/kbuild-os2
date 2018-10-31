/*-
 * Copyright (c) 1991, 1993, 1994
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef lint
#if 0
static char sccsid[] = "@(#)utils.c	8.3 (Berkeley) 4/1/94";
#include <sys/cdefs.h>
__FBSDID("$FreeBSD: src/bin/cp/utils.c,v 1.43 2004/04/06 20:06:44 markm Exp $");
#endif
#endif /* not lint */

/*********************************************************************************************************************************
*   Header Files                                                                                                                 *
*********************************************************************************************************************************/
#define MSC_DO_64_BIT_IO
#include "config.h"
#ifndef _MSC_VER
# include <sys/param.h>
#endif
#include <sys/stat.h>
#ifdef VM_AND_BUFFER_CACHE_SYNCHRONIZED
# include <sys/mman.h>
#endif

#include "err.h"
#include <errno.h>
#include <fcntl.h>
#include "fts.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#ifndef __HAIKU__
# include <sysexits.h>
#endif
#include <unistd.h>
#ifdef __sun__
# include "solfakes.h"
#endif
#ifdef __HAIKU__
# include "haikufakes.h"
#endif
#ifdef _MSC_VER
# include "mscfakes.h"
#else
# include <sys/time.h>
#endif
#include "cp_extern.h"
#include "cmp_extern.h"


/*********************************************************************************************************************************
*   Defined Constants And Macros                                                                                                 *
*********************************************************************************************************************************/
#define	cp_pct(x,y)	(int)(100.0 * (double)(x) / (double)(y))

#ifndef MAXBSIZE
# define MAXBSIZE 0x10000
#endif
#ifndef O_BINARY
# define O_BINARY 0
#endif

#ifndef S_ISVTX
# define S_ISVTX 0
#endif


int
copy_file(CPUTILSINSTANCE *pThis, const FTSENT *entp, int dne, int changed_only, int *pcopied)
{
	/*static*/ char buf[MAXBSIZE];
	struct stat *fs;
	int ch, checkch, from_fd, rcount, rval, to_fd;
	ssize_t wcount;
	size_t wresid;
	size_t wtotal;
	char *bufp;
#ifdef VM_AND_BUFFER_CACHE_SYNCHRONIZED
	char *p;
#endif

	*pcopied = 0;

	if ((from_fd = open(entp->fts_path, O_RDONLY | O_BINARY | KMK_OPEN_NO_INHERIT, 0)) == -1) {
		warn(pThis->pCtx, "open: %s", entp->fts_path);
		return (1);
	}

	fs = entp->fts_statp;

	/*
	 * If the file exists and we're interactive, verify with the user.
	 * If the file DNE, set the mode to be the from file, minus setuid
	 * bits, modified by the umask; arguably wrong, but it makes copying
	 * executables work right and it's been that way forever.  (The
	 * other choice is 666 or'ed with the execute bits on the from file
	 * modified by the umask.)
	 */
	if (!dne) {
		/* compare the files first if requested */
		if (changed_only) {
                        if (cmp_fd_and_file(pThis->pCtx, from_fd, entp->fts_path, pThis->to.p_path,
					    1 /* silent */, 0 /* lflag */,
					    0 /* special */) == OK_EXIT) {
				close(from_fd);
				return (0);
			}
			if (lseek(from_fd, 0, SEEK_SET) != 0) {
    				close(from_fd);
				if ((from_fd = open(entp->fts_path, O_RDONLY | O_BINARY | KMK_OPEN_NO_INHERIT, 0)) == -1) {
					warn(pThis->pCtx, "open: %s", entp->fts_path);
					return (1);
				}
			}
		}

#define YESNO "(y/n [n]) "
		if (pThis->nflag) {
			if (pThis->vflag)
				kmk_builtin_ctx_printf(pThis->pCtx, 0, "%s not overwritten\n", pThis->to.p_path);
			return (0);
		} else if (pThis->iflag) {
			(void)fprintf(stderr, "overwrite %s? %s",
					pThis->to.p_path, YESNO);
			checkch = ch = getchar();
			while (ch != '\n' && ch != EOF)
				ch = getchar();
			if (checkch != 'y' && checkch != 'Y') {
				(void)close(from_fd);
				kmk_builtin_ctx_printf(pThis->pCtx, 1, "not overwritten\n");
				return (1);
			}
		}

		if (pThis->fflag) {
		    /* remove existing destination file name,
		     * create a new file  */
		    (void)unlink(pThis->to.p_path);
		    to_fd = open(pThis->to.p_path, O_WRONLY | O_TRUNC | O_CREAT | O_BINARY | KMK_OPEN_NO_INHERIT,
				 fs->st_mode & ~(S_ISUID | S_ISGID));
		} else
		    /* overwrite existing destination file name */
		    to_fd = open(pThis->to.p_path, O_WRONLY | O_TRUNC | O_BINARY | KMK_OPEN_NO_INHERIT, 0);
	} else
		to_fd = open(pThis->to.p_path, O_WRONLY | O_TRUNC | O_CREAT | O_BINARY | KMK_OPEN_NO_INHERIT,
		    fs->st_mode & ~(S_ISUID | S_ISGID));

	if (to_fd == -1) {
		warn(pThis->pCtx, "open: %s", pThis->to.p_path);
		(void)close(from_fd);
		return (1);
	}

	rval = 0;
	*pcopied = 1;

	/*
	 * Mmap and write if less than 8M (the limit is so we don't totally
	 * trash memory on big files.  This is really a minor hack, but it
	 * wins some CPU back.
	 */
#ifdef VM_AND_BUFFER_CACHE_SYNCHRONIZED
	if (S_ISREG(fs->st_mode) && fs->st_size > 0 &&
	    fs->st_size <= 8 * 1048576) {
		if ((p = mmap(NULL, (size_t)fs->st_size, PROT_READ,
		    MAP_SHARED, from_fd, (off_t)0)) == MAP_FAILED) {
			warn(pThis->pCtx, "mmap: %s", entp->fts_path);
			rval = 1;
		} else {
			wtotal = 0;
			for (bufp = p, wresid = fs->st_size; ;
			    bufp += wcount, wresid -= (size_t)wcount) {
				wcount = write(to_fd, bufp, wresid);
				wtotal += wcount;
# if defined(SIGINFO) && defined(KMK_BUILTIN_STANDALONE)
				if (g_cp_info) {
					g_cp_info = 0;
					kmk_builtin_ctx_printf(pThis->pCtx, 1,
						"%s -> %s %3d%%\n",
						entp->fts_path, pThis->to.p_path,
						cp_pct(wtotal, fs->st_size));

				}
#endif
				if (wcount >= (ssize_t)wresid || wcount <= 0)
					break;
			}
			if (wcount != (ssize_t)wresid) {
				warn(pThis->pCtx, "write[%zd != %zu]: %s", wcount, wresid, pThis->to.p_path);
				rval = 1;
			}
			/* Some systems don't unmap on close(2). */
			if (munmap(p, fs->st_size) < 0) {
				warn(pThis->pCtx, "munmap: %s", entp->fts_path);
				rval = 1;
			}
		}
	} else
#endif
	{
		wtotal = 0;
		while ((rcount = read(from_fd, buf, MAXBSIZE)) > 0) {
			for (bufp = buf, wresid = rcount; ;
			    bufp += wcount, wresid -= wcount) {
				wcount = write(to_fd, bufp, wresid);
				wtotal += wcount;
#if defined(SIGINFO) && defined(KMK_BUILTIN_STANDALONE)
				if (g_cp_info) {
					g_cp_info = 0;
					kmk_builtin_ctx_printf(pThis->pCtx, 1,
						"%s -> %s %3d%%\n",
						entp->fts_path, pThis->to.p_path,
						cp_pct(wtotal, fs->st_size));

				}
#endif
				if (wcount >= (ssize_t)wresid || wcount <= 0)
					break;
			}
			if (wcount != (ssize_t)wresid) {
				warn(pThis->pCtx, "write[%zd != %zu]: %s", wcount, wresid, pThis->to.p_path);
				rval = 1;
				break;
			}
		}
		if (rcount < 0) {
			warn(pThis->pCtx, "read: %s", entp->fts_path);
			rval = 1;
		}
	}

	/*
	 * Don't remove the target even after an error.  The target might
	 * not be a regular file, or its attributes might be important,
	 * or its contents might be irreplaceable.  It would only be safe
	 * to remove it if we created it and its length is 0.
	 */

	if (pThis->pflag && copy_file_attribs(pThis, fs, to_fd))
		rval = 1;
	(void)close(from_fd);
	if (close(to_fd)) {
		warn(pThis->pCtx, "close: %s", pThis->to.p_path);
		rval = 1;
	}
	return (rval);
}

int
copy_link(CPUTILSINSTANCE *pThis, const FTSENT *p, int exists)
{
	int len;
	char llink[PATH_MAX];

	if ((len = readlink(p->fts_path, llink, sizeof(llink) - 1)) == -1) {
		warn(pThis->pCtx, "readlink: %s", p->fts_path);
		return (1);
	}
	llink[len] = '\0';
	if (exists && unlink(pThis->to.p_path)) {
		warn(pThis->pCtx, "unlink: %s", pThis->to.p_path);
		return (1);
	}
	if (symlink(llink, pThis->to.p_path)) {
		warn(pThis->pCtx, "symlink: %s", llink);
		return (1);
	}
	return (pThis->pflag ? copy_file_attribs(pThis, p->fts_statp, -1) : 0);
}

int
copy_fifo(CPUTILSINSTANCE *pThis, struct stat *from_stat, int exists)
{
	if (exists && unlink(pThis->to.p_path)) {
		warn(pThis->pCtx, "unlink: %s", pThis->to.p_path);
		return (1);
	}
	if (mkfifo(pThis->to.p_path, from_stat->st_mode)) {
		warn(pThis->pCtx, "mkfifo: %s", pThis->to.p_path);
		return (1);
	}
	return (pThis->pflag ? copy_file_attribs(pThis, from_stat, -1) : 0);
}

int
copy_special(CPUTILSINSTANCE *pThis, struct stat *from_stat, int exists)
{
	if (exists && unlink(pThis->to.p_path)) {
		warn(pThis->pCtx, "unlink: %s", pThis->to.p_path);
		return (1);
	}
	if (mknod(pThis->to.p_path, from_stat->st_mode, from_stat->st_rdev)) {
		warn(pThis->pCtx, "mknod: %s", pThis->to.p_path);
		return (1);
	}
	return (pThis->pflag ? copy_file_attribs(pThis, from_stat, -1) : 0);
}

int
copy_file_attribs(CPUTILSINSTANCE *pThis, struct stat *fs, int fd)
{
	/*static*/ struct timeval tv[2];
	struct stat ts;
	int rval, gotstat, islink, fdval;

	rval = 0;
	fdval = fd != -1;
	islink = !fdval && S_ISLNK(fs->st_mode);
	fs->st_mode &= S_ISUID | S_ISGID | S_ISVTX |
		       S_IRWXU | S_IRWXG | S_IRWXO;

#ifdef HAVE_ST_TIMESPEC
	TIMESPEC_TO_TIMEVAL(&tv[0], &fs->st_atimespec);
	TIMESPEC_TO_TIMEVAL(&tv[1], &fs->st_mtimespec);
#else
        tv[0].tv_sec = fs->st_atime;
        tv[1].tv_sec = fs->st_mtime;
        tv[0].tv_usec = tv[1].tv_usec = 0;
#endif
	if (islink ? lutimes(pThis->to.p_path, tv) : utimes(pThis->to.p_path, tv)) {
		warn(pThis->pCtx, "%sutimes: %s", islink ? "l" : "", pThis->to.p_path);
		rval = 1;
	}
	if (fdval ? fstat(fd, &ts) :
	    (islink ? lstat(pThis->to.p_path, &ts) : stat(pThis->to.p_path, &ts)))
		gotstat = 0;
	else {
		gotstat = 1;
		ts.st_mode &= S_ISUID | S_ISGID | S_ISVTX |
			      S_IRWXU | S_IRWXG | S_IRWXO;
	}
	/*
	 * Changing the ownership probably won't succeed, unless we're root
	 * or POSIX_CHOWN_RESTRICTED is not set.  Set uid/gid before setting
	 * the mode; current BSD behavior is to remove all setuid bits on
	 * chown.  If chown fails, lose setuid/setgid bits.
	 */
	if (!gotstat || fs->st_uid != ts.st_uid || fs->st_gid != ts.st_gid)
		if (fdval ? fchown(fd, fs->st_uid, fs->st_gid) :
		    (islink ? lchown(pThis->to.p_path, fs->st_uid, fs->st_gid) :
		    chown(pThis->to.p_path, fs->st_uid, fs->st_gid))) {
			if (errno != EPERM) {
				warn(pThis->pCtx, "chown: %s", pThis->to.p_path);
				rval = 1;
			}
			fs->st_mode &= ~(S_ISUID | S_ISGID);
		}

	if (!gotstat || fs->st_mode != ts.st_mode)
		if (fdval ? fchmod(fd, fs->st_mode) :
		    (islink ? lchmod(pThis->to.p_path, fs->st_mode) :
		    chmod(pThis->to.p_path, fs->st_mode))) {
			warn(pThis->pCtx, "chmod: %s", pThis->to.p_path);
			rval = 1;
		}

#ifdef HAVE_ST_FLAGS
	if (!gotstat || fs->st_flags != ts.st_flags)
		if (fdval ?
		    fchflags(fd, fs->st_flags) :
		    (islink ? (errno = ENOSYS) :
		    chflags(pThis->to.p_path, fs->st_flags))) {
			warn(pThis->pCtx, "chflags: %s", pThis->to.p_path);
			rval = 1;
		}
#endif

	return (rval);
}

