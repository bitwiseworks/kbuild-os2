/*	$NetBSD: redir.c,v 1.29 2004/07/08 03:57:33 christos Exp $	*/

/*-
 * Copyright (c) 1991, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Kenneth Almquist.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
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

#if 0
#ifndef lint
static char sccsid[] = "@(#)redir.c	8.2 (Berkeley) 5/4/95";
#else
__RCSID("$NetBSD: redir.c,v 1.29 2004/07/08 03:57:33 christos Exp $");
#endif /* not lint */
#endif

#include <sys/types.h>
#include <limits.h>         /* PIPE_BUF */
#include <string.h>
#include <errno.h>
#include <stdlib.h>

/*
 * Code for dealing with input/output redirection.
 */

#include "main.h"
#include "shell.h"
#include "nodes.h"
#include "jobs.h"
#include "options.h"
#include "expand.h"
#include "redir.h"
#include "output.h"
#include "memalloc.h"
#include "error.h"
#include "shinstance.h"


#define EMPTY -2		/* marks an unused slot in redirtab */
#ifndef PIPE_BUF
# define PIPESIZE 4096		/* amount of buffering in a pipe */
#else
# define PIPESIZE PIPE_BUF
#endif


MKINIT
struct redirtab {
	struct redirtab *next;
	short renamed[10];
};


//MKINIT struct redirtab *redirlist;

/*
 * We keep track of whether or not fd0 has been redirected.  This is for
 * background commands, where we want to redirect fd0 to /dev/null only
 * if it hasn't already been redirected.
*/
//int fd0_redirected = 0;

STATIC void openredirect(shinstance *, union node *, char[10], int);
STATIC int openhere(shinstance *, union node *);


/*
 * Process a list of redirection commands.  If the REDIR_PUSH flag is set,
 * old file descriptors are stashed away so that the redirection can be
 * undone by calling popredir.  If the REDIR_BACKQ flag is set, then the
 * standard output, and the standard error if it becomes a duplicate of
 * stdout, is saved in memory.
 */

void
redirect(shinstance *psh, union node *redir, int flags)
{
	union node *n;
	struct redirtab *sv = NULL;
	int i;
	int fd;
	int try;
	char memory[10];	/* file descriptors to write to memory */

	for (i = 10 ; --i >= 0 ; )
		memory[i] = 0;
	memory[1] = flags & REDIR_BACKQ;
	if (flags & REDIR_PUSH) {
		/* We don't have to worry about REDIR_VFORK here, as
		 * flags & REDIR_PUSH is never true if REDIR_VFORK is set.
		 */
		sv = ckmalloc(psh, sizeof (struct redirtab));
		for (i = 0 ; i < 10 ; i++)
			sv->renamed[i] = EMPTY;
		sv->next = psh->redirlist;
		psh->redirlist = sv;
	}
	for (n = redir ; n ; n = n->nfile.next) {
		fd = n->nfile.fd;
		try = 0;
		if ((n->nfile.type == NTOFD || n->nfile.type == NFROMFD) &&
		    n->ndup.dupfd == fd)
			continue; /* redirect from/to same file descriptor */

		if ((flags & REDIR_PUSH) && sv->renamed[fd] == EMPTY) {
			INTOFF;
again:
			if ((i = shfile_fcntl(&psh->fdtab, fd, F_DUPFD, 10)) == -1) {
				switch (errno) {
				case EBADF:
					if (!try) {
						openredirect(psh, n, memory, flags);
						try++;
						goto again;
					}
					/* FALLTHROUGH*/
				default:
					INTON;
					error(psh, "%d: %s", fd, sh_strerror(psh, errno));
					/* NOTREACHED */
				}
			}
			if (!try) {
				sv->renamed[fd] = i;
				shfile_close(&psh->fdtab, fd);
			}
			INTON;
		} else {
			shfile_close(&psh->fdtab, fd);
		}
                if (fd == 0)
                        psh->fd0_redirected++;
		if (!try)
			openredirect(psh, n, memory, flags);
	}
	if (memory[1])
		psh->out1 = &psh->memout;
	if (memory[2])
		psh->out2 = &psh->memout;
}


STATIC void
openredirect(shinstance *psh, union node *redir, char memory[10], int flags)
{
	int fd = redir->nfile.fd;
	char *fname;
	int f;
	int oflags = O_WRONLY|O_CREAT|O_TRUNC, eflags;

	/*
	 * We suppress interrupts so that we won't leave open file
	 * descriptors around.  This may not be such a good idea because
	 * an open of a device or a fifo can block indefinitely.
	 */
	INTOFF;
	memory[fd] = 0;
	switch (redir->nfile.type) {
	case NFROM:
		fname = redir->nfile.expfname;
		if (flags & REDIR_VFORK)
			eflags = O_NONBLOCK;
		else
			eflags = 0;
		if ((f = shfile_open(&psh->fdtab, fname, O_RDONLY|eflags, 0)) < 0)
			goto eopen;
		if (eflags)
			(void)shfile_fcntl(&psh->fdtab, f, F_SETFL, shfile_fcntl(&psh->fdtab, f, F_GETFL, 0) & ~eflags);
		break;
	case NFROMTO:
		fname = redir->nfile.expfname;
		if ((f = shfile_open(&psh->fdtab, fname, O_RDWR|O_CREAT|O_TRUNC, 0666)) < 0)
			goto ecreate;
		break;
	case NTO:
		if (Cflag(psh))
			oflags |= O_EXCL;
		/* FALLTHROUGH */
	case NCLOBBER:
		fname = redir->nfile.expfname;
		if ((f = shfile_open(&psh->fdtab, fname, oflags, 0666)) < 0)
			goto ecreate;
		break;
	case NAPPEND:
		fname = redir->nfile.expfname;
		if ((f = shfile_open(&psh->fdtab, fname, O_WRONLY|O_CREAT|O_APPEND, 0666)) < 0)
			goto ecreate;
		break;
	case NTOFD:
	case NFROMFD:
		if (redir->ndup.dupfd >= 0) {	/* if not ">&-" */
			if (memory[redir->ndup.dupfd])
				memory[fd] = 1;
			else
				copyfd(psh, redir->ndup.dupfd, fd);
		}
		INTON;
		return;
	case NHERE:
	case NXHERE:
		f = openhere(psh, redir);
		break;
	default:
		sh_abort(psh);
	}

	if (f != fd) {
		movefd(psh, f, fd);
	}
	INTON;
	return;
ecreate:
	error(psh, "cannot create %s: %s", fname, errmsg(psh, errno, E_CREAT));
eopen:
	error(psh, "cannot open %s: %s", fname, errmsg(psh, errno, E_OPEN));
}


/*
 * Handle here documents.  Normally we fork off a process to write the
 * data to a pipe.  If the document is short, we can stuff the data in
 * the pipe without forking.
 */

STATIC int
openhere(shinstance *psh, union node *redir)
{
	int pip[2];
	size_t len = 0;

	if (shfile_pipe(&psh->fdtab, pip) < 0)
		error(psh, "Pipe call failed");
	if (redir->type == NHERE) {
		len = strlen(redir->nhere.doc->narg.text);
		if (len <= PIPESIZE) {
			xwrite(psh, pip[1], redir->nhere.doc->narg.text, len);
			goto out;
		}
	}
	if (forkshell(psh, (struct job *)NULL, (union node *)NULL, FORK_NOJOB) == 0) {
		shfile_close(&psh->fdtab, pip[0]);
		sh_signal(psh, SIGINT, SH_SIG_IGN);
		sh_signal(psh, SIGQUIT, SH_SIG_IGN);
		sh_signal(psh, SIGHUP, SH_SIG_IGN);
#ifdef SIGTSTP
		sh_signal(psh, SIGTSTP, SH_SIG_IGN);
#endif
		sh_signal(psh, SIGPIPE, SH_SIG_DFL);
		if (redir->type == NHERE)
			xwrite(psh, pip[1], redir->nhere.doc->narg.text, len);
		else
			expandhere(psh, redir->nhere.doc, pip[1]);
		sh__exit(psh, 0);
	}
out:
	shfile_close(&psh->fdtab, pip[1]);
	return pip[0];
}



/*
 * Undo the effects of the last redirection.
 */

void
popredir(shinstance *psh)
{
	struct redirtab *rp = psh->redirlist;
	int i;

	for (i = 0 ; i < 10 ; i++) {
		if (rp->renamed[i] != EMPTY) {
                        if (i == 0)
                                psh->fd0_redirected--;
			if (rp->renamed[i] >= 0) {
				movefd(psh, rp->renamed[i], i);
			} else {
				shfile_close(&psh->fdtab, i);
			}
		}
	}
	INTOFF;
	psh->redirlist = rp->next;
	ckfree(psh, rp);
	INTON;
}

/*
 * Undo all redirections.  Called on error or interrupt.
 */

#ifdef mkinit

INCLUDE "redir.h"

RESET {
	while (psh->redirlist)
		popredir(psh);
}

SHELLPROC {
	clearredir(psh, 0);
}

#endif

/* Return true if fd 0 has already been redirected at least once.  */
int
fd0_redirected_p(shinstance *psh) {
        return psh->fd0_redirected != 0;
}

/*
 * Discard all saved file descriptors.
 */

void
clearredir(shinstance *psh, int vforked)
{
	struct redirtab *rp;
	int i;

	for (rp = psh->redirlist ; rp ; rp = rp->next) {
		for (i = 0 ; i < 10 ; i++) {
			if (rp->renamed[i] >= 0) {
				shfile_close(&psh->fdtab, rp->renamed[i]);
			}
			if (!vforked)
				rp->renamed[i] = EMPTY;
		}
	}
}



/*
 * Copy a file descriptor to be >= to.  Returns -1
 * if the source file descriptor is closed, EMPTY if there are no unused
 * file descriptors left.
 */

int
copyfd(shinstance *psh, int from, int to)
{
	int newfd;

	newfd = shfile_fcntl(&psh->fdtab, from, F_DUPFD, to);
	if (newfd < 0) {
		if (errno == EMFILE)
			return EMPTY;
		error(psh, "%d: %s", from, sh_strerror(psh, errno));
	}
	return newfd;
}


/*
 * Move a file descriptor to be == to.  Returns -1
 * if the source file descriptor is closed, EMPTY if there are no unused
 * file descriptors left.
 */

int
movefd(shinstance *psh, int from, int to)
{
	int newfd;

	newfd = shfile_movefd(&psh->fdtab, from, to);
	if (newfd < 0) {
		if (errno == EMFILE)
			return EMPTY;
		error(psh, "%d: %s", from, sh_strerror(psh, errno));
	}
	return newfd;
}


/*
 * Move a file descriptor to be >= to.  Returns -1
 * if the source file descriptor is closed, EMPTY if there are no unused
 * file descriptors left.
 */

int
movefd_above(shinstance *psh, int from, int to)
{
	int newfd;

	newfd = shfile_movefd_above(&psh->fdtab, from, to);
	if (newfd < 0) {
		if (errno == EMFILE)
			return EMPTY;
		error(psh, "%d: %s", from, sh_strerror(psh, errno));
	}
	return newfd;
}

