/*	$NetBSD: exec.h,v 1.21 2003/08/07 09:05:31 agc Exp $	*/

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
 *
 *	@(#)exec.h	8.3 (Berkeley) 6/8/95
 */

#ifndef ___exec_h
#define ___exec_h

/* values of cmdtype */
#define CMDUNKNOWN	-1	/* no entry in table for command */
#define CMDNORMAL	0	/* command is an executable program */
#define CMDFUNCTION	1	/* command is a shell function */
#define CMDBUILTIN	2	/* command is a shell builtin */
#define CMDSPLBLTIN	3	/* command is a special shell builtin */


union param {
        struct
        {
            int index;
            int suffix; /* PC suffix index */
        } n;
        int (*bltin)(struct shinstance*, int, char**);
        union node *func;
};

struct cmdentry {
	int cmdtype;
        union param u;
};


/* action to find_command() */
#define DO_ERR		0x01	/* prints errors */
#define DO_ABS		0x02	/* checks absolute paths */
#define DO_NOFUNC	0x04	/* don't return shell functions, for command */
#define DO_ALTPATH	0x08	/* using alternate path */
#define DO_ALTBLTIN	0x20	/* %builtin in alt. path */

/*extern const char *pathopt;*/	/* set by padvance */

#if !defined(__GNUC__) && !defined(__attribute__)
# define __attribute__(a)
#endif

#ifndef SH_FORKED_MODE
void subshellinitexec(shinstance *, shinstance *);
#endif
SH_NORETURN_1 void shellexec(struct shinstance *, char **, char **, const char *, int, int) SH_NORETURN_2;
char *padvance(struct shinstance *, const char **, const char *);
int hashcmd(struct shinstance *, int, char **);
void find_command(struct shinstance *, char *, struct cmdentry *, int, const char *);
int (*find_builtin(struct shinstance *, char *))(struct shinstance *, int, char **);
int (*find_splbltin(struct shinstance *, char *))(struct shinstance *, int, char **);
void hashcd(struct shinstance *);
void changepath(struct shinstance *, const char *);
void deletefuncs(struct shinstance *);
void getcmdentry(struct shinstance *, char *, struct cmdentry *);
void addcmdentry(struct shinstance *, char *, struct cmdentry *);
void defun(struct shinstance *, char *, union node *);
int unsetfunc(struct shinstance *, char *);
int typecmd(struct shinstance *, int, char **);
void hash_special_builtins(struct shinstance *);

#endif
