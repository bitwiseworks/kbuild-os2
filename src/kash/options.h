/*	$NetBSD: options.h,v 1.18 2005/05/07 19:52:17 dsl Exp $	*/

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
 *	@(#)options.h	8.2 (Berkeley) 5/4/95
 */

#ifndef ___options_h
#define ___options_h

struct shparam {
	int nparam;		/* # of positional parameters (without $0) */
	unsigned char malloc;	/* if parameter list dynamically allocated */
	unsigned char reset;	/* if getopts has been reset */
	char **p;		/* parameter list */
	char **optnext;		/* next parameter to be processed by getopts */
	char *optptr;		/* used by getopts */
};


struct optent {
	const char *name;		/* for set -o <name> */
	const char letter;		/* set [+/-]<letter> and $- */
	const char opt_set;		/* mutually exclusive option set */
	char val;			/* value of <letter>flag */
};

/* Those marked [U] are required by posix, but have no effect! */

#ifdef DEBUG
# define NOPTS 20
#else
# define NOPTS 19
#endif

#ifdef DEFINE_OPTIONS
# define DEF_OPTS(name, letter, opt_set) {name, letter, opt_set, 0},
const struct optent ro_optlist[NOPTS + 1] = {
#else
# define DEF_OPTS(name, letter, opt_set)
#endif
#define DEF_OPT(name,letter) DEF_OPTS(name, letter, 0)

DEF_OPT( "errexit",	'e' )	/* exit on error */
#define eflag(psh) (psh)->optlist[0].val
DEF_OPT( "noglob",	'f' )	/* no pathname expansion */
#define fflag(psh) (psh)->optlist[1].val
DEF_OPT( "ignoreeof",	'I' )	/* do not exit on EOF */
#define Iflag(psh) (psh)->optlist[2].val
DEF_OPT( "interactive",'i' )	/* interactive shell */
#define iflag(psh) (psh)->optlist[3].val
DEF_OPT( "monitor",	'm' )	/* job control */
#define mflag(psh) (psh)->optlist[4].val
DEF_OPT( "noexec",	'n' )	/* [U] do not exec commands */
#define nflag(psh) (psh)->optlist[5].val
DEF_OPT( "stdin",	's' )	/* read from stdin */
#define sflag(psh) (psh)->optlist[6].val
DEF_OPT( "xtrace",	'x' )	/* trace after expansion */
#define xflag(psh) (psh)->optlist[7].val
DEF_OPT( "verbose",	'v' )	/* trace read input */
#define vflag(psh) (psh)->optlist[8].val
DEF_OPTS( "vi",		'V', 'V' )	/* vi style editing */
#define Vflag(psh) (psh)->optlist[9].val
DEF_OPTS( "emacs",	'E', 'V' )	/* emacs style editing */
#define	Eflag(psh) (psh)->optlist[10].val
DEF_OPT( "noclobber",	'C' )	/* do not overwrite files with > */
#define	Cflag(psh) (psh)->optlist[11].val
DEF_OPT( "allexport",	'a' )	/* export all variables */
#define	aflag(psh) (psh)->optlist[12].val
DEF_OPT( "notify",	'b' )	/* [U] report completion of background jobs */
#define	bflag(psh) (psh)->optlist[13].val
DEF_OPT( "nounset",	'u' )	/* error expansion of unset variables */
#define	uflag(psh) (psh)->optlist[14].val
DEF_OPT( "quietprofile", 'q' )
#define	qflag(psh) (psh)->optlist[15].val
DEF_OPT( "nolog",	0 )	/* [U] no functon defs in command history */
#define	nolog(psh) (psh)->optlist[16].val
DEF_OPT( "cdprint",	0 )	/* always print result of cd */
#define	cdprint(psh) (psh)->optlist[17].val
DEF_OPT( "tabcomplete",	0 )	/* <tab> causes filename expansion */
#define	tabcomplete(psh) (psh)->optlist[18].val
#ifdef DEBUG
DEF_OPT( "debug",	0 )	/* enable debug prints */
#define	debug(psh) (psh)->optlist[19].val
#endif

#ifdef DEFINE_OPTIONS
	{ 0, 0, 0, 0 },
};
#else
extern const struct optent ro_optlist[];
#endif
#define sizeof_optlist (NOPTS * sizeof(struct optent))


/*extern char *minusc;*/		/* argument to -c option */
/*extern char *arg0;*/		/* $0 */
/*extern struct shparam shellparam;*/  /* $@ */
/*extern char **argptr;*/		/* argument list for builtin commands */
/*extern char *optionarg;*/		/* set by nextopt */
/*extern char *optptr;*/		/* used by nextopt */

#ifndef SH_FORKED_MODE
void subshellinitoptions(struct shinstance *, struct shinstance *);
#endif
void procargs(struct shinstance *, int, char **);
void optschanged(struct shinstance *);
void setparam(struct shinstance *, char **);
void freeparam(struct shinstance *, volatile struct shparam *);
int shiftcmd(struct shinstance *, int, char **);
int setcmd(struct shinstance *, int, char **);
int getoptscmd(struct shinstance *, int, char **);
int nextopt(struct shinstance *, const char *);
void getoptsreset(struct shinstance *, const char *);

#endif
