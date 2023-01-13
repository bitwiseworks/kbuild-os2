/* $Id: fts-nt.h 3535 2021-12-20 23:32:28Z bird $ */
/** @file
 * Header for the NT port of BSD fts.h.
 *
 * @copyright   Copyright (c) 1989, 1993 The Regents of the University of California.  All rights reserved.
 * @copyright   NT modifications Copyright (C) 2016 knut st. osmundsen <bird-klibc-spam-xiv@anduin.net>
 * @licenses    BSD3
 */

/*
 * Copyright (c) 1989, 1993
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
 *
 *	@(#)fts.h	8.3 (Berkeley) 8/14/94
 * $FreeBSD$
 *
 */

#ifndef	INCLUDED_FTS_NT_H
#define	INCLUDED_FTS_NT_H

#include <sys/types.h>
#include <stdint.h>
#include "ntstat.h" /* ensure correct stat structure */

typedef uint64_t fts_dev_t;
typedef uint64_t fts_ino_t;
typedef uint32_t fts_nlink_t;
#ifdef _WINNT_
typedef HANDLE fts_fd_t;
# define NT_FTS_INVALID_HANDLE_VALUE	INVALID_HANDLE_VALUE
#else
typedef void * fts_fd_t;
# define NT_FTS_INVALID_HANDLE_VALUE	((void *)~(uintptr_t)0)
#endif
#define FTSCALL __cdecl

typedef struct {
	struct _ftsent *fts_cur;	/* current node */
	struct _ftsent *fts_child;	/* linked list of children */
	struct _ftsent **fts_array;	/* sort array */
	fts_dev_t fts_dev;		/* starting device # */
	char *fts_path;			/* path for this descent */
	size_t fts_pathlen;		/* sizeof(path) */
	wchar_t *fts_wcspath;		/* NT: UTF-16 path for this descent. */
	size_t fts_cwcpath;		/* NT: size of fts_wcspath buffer */
	size_t fts_nitems;		/* elements in the sort array */
	int (FTSCALL *fts_compar)	/* compare function */
	    (const struct _ftsent * const *, const struct _ftsent * const *);

#define	FTS_COMFOLLOW	0x001		/* follow command line symlinks */
#define	FTS_LOGICAL	0x002		/* logical walk */
#define	FTS_NOCHDIR	0x004		/* don't change directories */
#define	FTS_NOSTAT	0x008		/* don't get stat info */
#define	FTS_PHYSICAL	0x010		/* physical walk */
#define	FTS_SEEDOT	0x020		/* return dot and dot-dot */
#define	FTS_XDEV	0x040		/* don't cross devices */
#if 0 /* No whiteout on NT. */
#define	FTS_WHITEOUT	0x080		/* return whiteout information */
#endif
#define FTS_CWDFD       0x100           /* For gnulib fts compatibility, enables fts_cwd_fd. */
#define FTS_TIGHT_CYCLE_CHECK 0x200     /* Ignored currently */
#define FTS_NO_ANSI     0x40000000      /* NT: No ansi name or access path. */
#define	FTS_OPTIONMASK	0x400003ff	/* valid user option mask */

#define	FTS_NAMEONLY	0x10000		/* (private) child names only */
#define	FTS_STOP	0x20000		/* (private) unrecoverable error */
	int fts_options;		/* fts_open options, global flags */
	int fts_cwd_fd;                 /* FTS_CWDFD: AT_FDCWD or a virtual CWD file descriptor. */
	void *fts_clientptr;		/* thunk for sort function */
} FTS;

typedef struct _ftsent {
	struct _ftsent *fts_cycle;	/* cycle node */
	struct _ftsent *fts_parent;	/* parent directory */
	struct _ftsent *fts_link;	/* next file in directory */
	int64_t fts_number;		/* local numeric value */
#define	fts_bignum	fts_number	/* XXX non-std, should go away */
	void *fts_pointer;		/* local address value */
	char *fts_accpath;		/* access path */
	wchar_t *fts_wcsaccpath;	/* NT: UTF-16 access path */
	char *fts_path;			/* root path */
	wchar_t *fts_wcspath;		/* NT: UTF-16 root path */
	int fts_errno;			/* errno for this node */
	size_t fts_alloc_size;		/* internal - size of the allocation for this entry. */
	fts_fd_t fts_dirfd;		/* NT: Handle to the directory (NT_FTS_)INVALID_HANDLE_VALUE if not valid */
	size_t fts_pathlen;		/* strlen(fts_path) */
	size_t fts_cwcpath;		/* NT: length of fts_wcspath. */
	size_t fts_namelen;		/* strlen(fts_name) */
	size_t fts_cwcname;		/* NT: length of fts_wcsname. */

	fts_ino_t fts_ino;		/* inode */
	fts_dev_t fts_dev;		/* device */
	fts_nlink_t fts_nlink;		/* link count */

#define	FTS_ROOTPARENTLEVEL	-1
#define	FTS_ROOTLEVEL		 0
	long fts_level;			/* depth (-1 to N) */

#define	FTS_D		 1		/* preorder directory */
#define	FTS_DC		 2		/* directory that causes cycles */
#define	FTS_DEFAULT	 3		/* none of the above */
#define	FTS_DNR		 4		/* unreadable directory */
#define	FTS_DOT		 5		/* dot or dot-dot */
#define	FTS_DP		 6		/* postorder directory */
#define	FTS_ERR		 7		/* error; errno is set */
#define	FTS_F		 8		/* regular file */
#define	FTS_INIT	 9		/* initialized only */
#define	FTS_NS		10		/* stat(2) failed */
#define	FTS_NSOK	11		/* no stat(2) requested */
#define	FTS_SL		12		/* symbolic link */
#define	FTS_SLNONE	13		/* symbolic link without target */
#define	FTS_W		14		/* whiteout object */
	int fts_info;			/* user status for FTSENT structure */

#define	FTS_DONTCHDIR	 0x01		/* don't chdir .. to the parent */
#define	FTS_SYMFOLLOW	 0x02		/* followed a symlink to get here */
#define	FTS_ISW		 0x04		/* this is a whiteout object */
	unsigned fts_flags;		/* private flags for FTSENT structure */

#define	FTS_AGAIN	 1		/* read node again */
#define	FTS_FOLLOW	 2		/* follow symbolic link */
#define	FTS_NOINSTR	 3		/* no instructions */
#define	FTS_SKIP	 4		/* discard node */
	int fts_instr;			/* fts_set() instructions */

	struct stat *fts_statp;		/* stat(2) information */
	char *fts_name;			/* file name */
	wchar_t *fts_wcsname;		/* NT: UTF-16 file name. */
	FTS *fts_fts;			/* back pointer to main FTS */
	BirdStat_T fts_stat;		/* NT: We always got stat info. */
} FTSENT;


#ifdef __cplusplus
extern "C" {
#endif

FTSENT	*FTSCALL nt_fts_children(FTS *, int);
int	 FTSCALL nt_fts_close(FTS *);
void	*FTSCALL nt_fts_get_clientptr(FTS *);
#define	 fts_get_clientptr(fts)	((fts)->fts_clientptr)
FTS	*FTSCALL nt_fts_get_stream(FTSENT *);
#define	 fts_get_stream(ftsent)	((ftsent)->fts_fts)
FTS	*FTSCALL nt_fts_open(char * const *, int, int (FTSCALL*)(const FTSENT * const *, const FTSENT * const *));
FTS	*FTSCALL nt_fts_openw(wchar_t * const *, int, int (FTSCALL*)(const FTSENT * const *, const FTSENT * const *));
FTSENT	*FTSCALL nt_fts_read(FTS *);
int	 FTSCALL nt_fts_set(FTS *, FTSENT *, int);
void	 FTSCALL nt_fts_set_clientptr(FTS *, void *);

/* API mappings. */
#define fts_children(a_pFts, a_iInstr)                  nt_fts_children(a_pFts, a_iInstr)
#define fts_close(a_pFts)                               nt_fts_close(a_pFts)
#define fts_open(a_papszArgs, a_fOptions, a_pfnCompare) nt_fts_open(a_papszArgs, a_fOptions, a_pfnCompare)
#define fts_read(a_pFts)                                nt_fts_read(a_pFts)
#define fts_set(a_pFts, a_pFtsEntry, a_iInstr)          nt_fts_set(a_pFts, a_pFtsEntry, a_iInstr)
#define fts_set_clientptr(a_pFts, a_pvUser)             nt_fts_set_clientptr(a_pFts, a_pvUser)

#ifdef __cplusplus
}
#endif

#endif /* !INCLUDED_FTS_NT_H */

