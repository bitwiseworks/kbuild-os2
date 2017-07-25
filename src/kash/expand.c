/*	$NetBSD: expand.c,v 1.71 2005/06/01 15:41:19 lukem Exp $	*/

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
static char sccsid[] = "@(#)expand.c	8.5 (Berkeley) 5/15/95";
#else
__RCSID("$NetBSD: expand.c,v 1.71 2005/06/01 15:41:19 lukem Exp $");
#endif /* not lint */
#endif

#include <sys/types.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>

/*
 * Routines to expand arguments to commands.  We have to deal with
 * backquotes, shell variables, and file metacharacters.
 */

#include "shell.h"
#include "main.h"
#include "nodes.h"
#include "eval.h"
#include "expand.h"
#include "syntax.h"
#include "parser.h"
#include "jobs.h"
#include "options.h"
#include "var.h"
#include "input.h"
#include "output.h"
#include "memalloc.h"
#include "error.h"
#include "mystring.h"
#include "show.h"
#include "shinstance.h"

///*
// * Structure specifying which parts of the string should be searched
// * for IFS characters.
// */
//
//struct ifsregion {
//	struct ifsregion *next;	/* next region in list */
//	int begoff;		/* offset of start of region */
//	int endoff;		/* offset of end of region */
//	int inquotes;		/* search for nul bytes only */
//};
//
//
//char *expdest;			/* output of current string */
//struct nodelist *argbackq;	/* list of back quote expressions */
//struct ifsregion ifsfirst;	/* first struct in list of ifs regions */
//struct ifsregion *ifslastp;	/* last struct in list */
//struct arglist exparg;		/* holds expanded arg list */

STATIC void argstr(shinstance *, char *, int);
STATIC char *exptilde(shinstance *, char *, int);
STATIC void expbackq(shinstance *, union node *, int, int);
STATIC int subevalvar(shinstance *, char *, char *, int, int, int, int);
STATIC char *evalvar(shinstance *, char *, int);
STATIC int varisset(shinstance *, char *, int);
STATIC void varvalue(shinstance *, char *, int, int, int);
STATIC void recordregion(shinstance *, int, int, int);
STATIC void removerecordregions(shinstance *, int);
STATIC void ifsbreakup(shinstance *, char *, struct arglist *);
STATIC void ifsfree(shinstance *);
STATIC void expandmeta(shinstance *, struct strlist *, int);
STATIC void expmeta(shinstance *, char *, char *);
STATIC void addfname(shinstance *, char *);
STATIC struct strlist *expsort(struct strlist *);
STATIC struct strlist *msort(struct strlist *, int);
STATIC int pmatch(char *, char *, int);
STATIC char *cvtnum(shinstance *, int, char *);

/*
 * Expand shell variables and backquotes inside a here document.
 */

void
expandhere(shinstance *psh, union node *arg, int fd)
{
	psh->herefd = fd;
	expandarg(psh, arg, (struct arglist *)NULL, 0);
	xwrite(psh, fd, stackblock(psh), psh->expdest - stackblock(psh));
}


/*
 * Perform variable substitution and command substitution on an argument,
 * placing the resulting list of arguments in arglist.  If EXP_FULL is true,
 * perform splitting and file name expansion.  When arglist is NULL, perform
 * here document expansion.
 */

void
expandarg(shinstance *psh, union node *arg, struct arglist *arglist, int flag)
{
	struct strlist *sp;
	char *p;

	psh->argbackq = arg->narg.backquote;
	STARTSTACKSTR(psh, psh->expdest);
	psh->ifsfirst.next = NULL;
	psh->ifslastp = NULL;
	argstr(psh, arg->narg.text, flag);
	if (arglist == NULL) {
		return;			/* here document expanded */
	}
	STPUTC(psh, '\0', psh->expdest);
	p = grabstackstr(psh, psh->expdest);
	TRACE2((psh, "expandarg: p='%s'\n", p));
	psh->exparg.lastp = &psh->exparg.list;
	/*
	 * TODO - EXP_REDIR
	 */
	if (flag & EXP_FULL) {
		ifsbreakup(psh, p, &psh->exparg);
		*psh->exparg.lastp = NULL;
		psh->exparg.lastp = &psh->exparg.list;
		expandmeta(psh, psh->exparg.list, flag);
	} else {
		if (flag & EXP_REDIR) /*XXX - for now, just remove escapes */
			rmescapes(psh, p);
		sp = (struct strlist *)stalloc(psh, sizeof (struct strlist));
		sp->text = p;
		*psh->exparg.lastp = sp;
		psh->exparg.lastp = &sp->next;
	}
	ifsfree(psh);
	*psh->exparg.lastp = NULL;
	if (psh->exparg.list) {
		*arglist->lastp = psh->exparg.list;
		arglist->lastp = psh->exparg.lastp;
	}
}



/*
 * Perform variable and command substitution.
 * If EXP_FULL is set, output CTLESC characters to allow for further processing.
 * Otherwise treat $@ like $* since no splitting will be performed.
 */

STATIC void
argstr(shinstance *psh, char *p, int flag)
{
	char c;
	int quotes = flag & (EXP_FULL | EXP_CASE);	/* do CTLESC */
	int firsteq = 1;
	const char *ifs = NULL;
	int ifs_split = EXP_IFS_SPLIT;

	if (flag & EXP_IFS_SPLIT)
		ifs = ifsset(psh) ? ifsval(psh) : " \t\n";

	if (*p == '~' && (flag & (EXP_TILDE | EXP_VARTILDE)))
		p = exptilde(psh, p, flag);
	for (;;) {
		switch (c = *p++) {
		case '\0':
		case CTLENDVAR: /* end of expanding yyy in ${xxx-yyy} */
			return;
		case CTLQUOTEMARK:
			/* "$@" syntax adherence hack */
			if (p[0] == CTLVAR && p[2] == '@' && p[3] == '=')
				break;
			if ((flag & EXP_FULL) != 0)
				STPUTC(psh, c, psh->expdest);
			ifs_split = 0;
			break;
		case CTLQUOTEEND:
			ifs_split = EXP_IFS_SPLIT;
			break;
		case CTLESC:
			if (quotes)
				STPUTC(psh, c, psh->expdest);
			c = *p++;
			STPUTC(psh, c, psh->expdest);
			break;
		case CTLVAR:
			p = evalvar(psh, p, (flag & ~EXP_IFS_SPLIT) | (flag & ifs_split));
			break;
		case CTLBACKQ:
		case CTLBACKQ|CTLQUOTE:
			expbackq(psh, psh->argbackq->n, c & CTLQUOTE, flag);
			psh->argbackq = psh->argbackq->next;
			break;
		case CTLENDARI:
			expari(psh, flag);
			break;
		case ':':
		case '=':
			/*
			 * sort of a hack - expand tildes in variable
			 * assignments (after the first '=' and after ':'s).
			 */
			STPUTC(psh, c, psh->expdest);
			if (flag & EXP_VARTILDE && *p == '~') {
				if (c == '=') {
					if (firsteq)
						firsteq = 0;
					else
						break;
				}
				p = exptilde(psh, p, flag);
			}
			break;
		default:
			STPUTC(psh, c, psh->expdest);
			if (flag & EXP_IFS_SPLIT & ifs_split && strchr(ifs, c) != NULL) {
				/* We need to get the output split here... */
				recordregion(psh, (int)(psh->expdest - stackblock(psh) - 1),
						(int)(psh->expdest - stackblock(psh)), 0);
			}
			break;
		}
	}
}

STATIC char *
exptilde(shinstance *psh, char *p, int flag)
{
	char c, *startp = p;
	const char *home;
	int quotes = flag & (EXP_FULL | EXP_CASE);

	while ((c = *p) != '\0') {
		switch(c) {
		case CTLESC:
			return (startp);
		case CTLQUOTEMARK:
			return (startp);
		case ':':
			if (flag & EXP_VARTILDE)
				goto done;
			break;
		case '/':
			goto done;
		}
		p++;
	}
done:
	*p = '\0';
	if (*(startp+1) == '\0') {
		if ((home = lookupvar(psh, "HOME")) == NULL)
			goto lose;
	} else {
		if ((home = sh_gethomedir(psh, startp+1)) == NULL)
			goto lose;
	}
	if (*home == '\0')
		goto lose;
	*p = c;
	while ((c = *home++) != '\0') {
		if (quotes && SQSYNTAX[(int)c] == CCTL)
			STPUTC(psh, CTLESC, psh->expdest);
		STPUTC(psh, c, psh->expdest);
	}
	return (p);
lose:
	*p = c;
	return (startp);
}


STATIC void
removerecordregions(shinstance *psh, int endoff)
{
	if (psh->ifslastp == NULL)
		return;

	if (psh->ifsfirst.endoff > endoff) {
		while (psh->ifsfirst.next != NULL) {
			struct ifsregion *ifsp;
			INTOFF;
			ifsp = psh->ifsfirst.next->next;
			ckfree(psh, psh->ifsfirst.next);
			psh->ifsfirst.next = ifsp;
			INTON;
		}
		if (psh->ifsfirst.begoff > endoff)
			psh->ifslastp = NULL;
		else {
			psh->ifslastp = &psh->ifsfirst;
			psh->ifsfirst.endoff = endoff;
		}
		return;
	}

	psh->ifslastp = &psh->ifsfirst;
	while (psh->ifslastp->next && psh->ifslastp->next->begoff < endoff)
		psh->ifslastp=psh->ifslastp->next;
	while (psh->ifslastp->next != NULL) {
		struct ifsregion *ifsp;
		INTOFF;
		ifsp = psh->ifslastp->next->next;
		ckfree(psh, psh->ifslastp->next);
		psh->ifslastp->next = ifsp;
		INTON;
	}
	if (psh->ifslastp->endoff > endoff)
		psh->ifslastp->endoff = endoff;
}


/*
 * Expand arithmetic expression.  Backup to start of expression,
 * evaluate, place result in (backed up) result, adjust string position.
 */
void
expari(shinstance *psh, int flag)
{
	char *p, *start;
	int result;
	int begoff;
	int quotes = flag & (EXP_FULL | EXP_CASE);
	int quoted;

	/*	ifsfree(); */

	/*
	 * This routine is slightly over-complicated for
	 * efficiency.  First we make sure there is
	 * enough space for the result, which may be bigger
	 * than the expression if we add exponentation.  Next we
	 * scan backwards looking for the start of arithmetic.  If the
	 * next previous character is a CTLESC character, then we
	 * have to rescan starting from the beginning since CTLESC
	 * characters have to be processed left to right.
	 */
#if INT_MAX / 1000000000 >= 10 || INT_MIN / 1000000000 <= -10
#error "integers with more than 10 digits are not supported"
#endif
	CHECKSTRSPACE(psh, 12 - 2, psh->expdest);
	USTPUTC(psh, '\0', psh->expdest);
	start = stackblock(psh);
	p = psh->expdest - 1;
	while (*p != CTLARI && p >= start)
		--p;
	if (*p != CTLARI)
		error(psh, "missing CTLARI (shouldn't happen)");
	if (p > start && *(p-1) == CTLESC)
		for (p = start; *p != CTLARI; p++)
			if (*p == CTLESC)
				p++;

	if (p[1] == '"')
		quoted=1;
	else
		quoted=0;
	begoff = (int)(p - start);
	removerecordregions(psh, begoff);
	if (quotes)
		rmescapes(psh, p+2);
	result = arith(psh, p+2);
	fmtstr(p, 12, "%d", result);

	while (*p++)
		;

	if (quoted == 0)
		recordregion(psh, begoff, (int)(p - 1 - start), 0);
	result = (int)(psh->expdest - p + 1);
	STADJUST(psh, -result, psh->expdest);
}


/*
 * Expand stuff in backwards quotes.
 */

STATIC void
expbackq(shinstance *psh, union node *cmd, int quoted, int flag)
{
	struct backcmd in;
	int i;
	char buf[128];
	char *p;
	char *dest = psh->expdest;
	struct ifsregion saveifs, *savelastp;
	struct nodelist *saveargbackq;
	char lastc;
	int startloc = (int)(dest - stackblock(psh));
	char const *syntax = quoted? DQSYNTAX : BASESYNTAX;
	int saveherefd;
	int quotes = flag & (EXP_FULL | EXP_CASE);
#ifdef SH_DEAL_WITH_CRLF
	int pending_cr = 0;
#endif

	INTOFF;
	saveifs = psh->ifsfirst;
	savelastp = psh->ifslastp;
	saveargbackq = psh->argbackq;
	saveherefd = psh->herefd;
	psh->herefd = -1;
	p = grabstackstr(psh, dest);
	evalbackcmd(psh, cmd, &in);
	ungrabstackstr(psh, p, dest);
	psh->ifsfirst = saveifs;
	psh->ifslastp = savelastp;
	psh->argbackq = saveargbackq;
	psh->herefd = saveherefd;

	p = in.buf;
	lastc = '\0';
	for (;;) {
		if (--in.nleft < 0) {
			if (in.fd < 0)
				break;
			while ((i = shfile_read(&psh->fdtab, in.fd, buf, sizeof buf)) < 0 && errno == EINTR);
			TRACE((psh, "expbackq: read returns %d\n", i));
			if (i <= 0)
				break;
			p = buf;
			in.nleft = i - 1;
		}
		lastc = *p++;
#ifdef SH_DEAL_WITH_CRLF
		if (pending_cr) {
			pending_cr = 0;
			if (lastc != '\n') {
				if (quotes && syntax[(int)'\r'] == CCTL)
					STPUTC(psh, CTLESC, dest);
				STPUTC(psh, '\r', dest);
			}
		}
		if (lastc == '\r')
			pending_cr = '\r';
		else
#endif
		if (lastc != '\0') {
			if (quotes && syntax[(int)lastc] == CCTL)
				STPUTC(psh, CTLESC, dest);
			STPUTC(psh, lastc, dest);
		}
	}
#ifdef SH_DEAL_WITH_CRLF
	if (pending_cr) {
		if (quotes && syntax[(int)'\r'] == CCTL)
			STPUTC(psh, CTLESC, dest);
		STPUTC(psh, '\r', dest);
	}
#endif

	/* Eat all trailing newlines */
	p = stackblock(psh) + startloc;
	while (dest > p && dest[-1] == '\n')
		STUNPUTC(psh, dest);

	if (in.fd >= 0)
		shfile_close(&psh->fdtab, in.fd);
	if (in.buf)
		ckfree(psh, in.buf);
	if (in.jp)
		psh->back_exitstatus = waitforjob(psh, in.jp);
	if (quoted == 0)
		recordregion(psh, startloc, (int)(dest - stackblock(psh)), 0);
	TRACE((psh, "evalbackq: size=%d: \"%.*s\"\n",
		(dest - stackblock(psh)) - startloc,
		(dest - stackblock(psh)) - startloc,
		stackblock(psh) + startloc));
	psh->expdest = dest;
	INTON;
}



STATIC int
subevalvar(shinstance *psh, char *p, char *str, int strloc, int subtype, int startloc, int varflags)
{
	char *startp;
	char *loc = NULL;
	char *q;
	int c = 0;
	int saveherefd = psh->herefd;
	struct nodelist *saveargbackq = psh->argbackq;
	int amount;

	psh->herefd = -1;
	argstr(psh, p, 0);
	STACKSTRNUL(psh, psh->expdest);
	psh->herefd = saveherefd;
	psh->argbackq = saveargbackq;
	startp = stackblock(psh) + startloc;
	if (str == NULL)
	    str = stackblock(psh) + strloc;

	switch (subtype) {
	case VSASSIGN:
		setvar(psh, str, startp, 0);
		amount = (int)(startp - psh->expdest);
		STADJUST(psh, amount, psh->expdest);
		varflags &= ~VSNUL;
		if (c != 0)
			*loc = c;
		return 1;

	case VSQUESTION:
		if (*p != CTLENDVAR) {
			outfmt(&psh->errout, "%s\n", startp);
			error(psh, (char *)NULL);
		}
		error(psh, "%.*s: parameter %snot set", p - str - 1,
		      str, (varflags & VSNUL) ? "null or "
					      : nullstr);
		/* NOTREACHED */

	case VSTRIMLEFT:
		for (loc = startp; loc < str; loc++) {
			c = *loc;
			*loc = '\0';
			if (patmatch(psh, str, startp, varflags & VSQUOTE))
				goto recordleft;
			*loc = c;
			if ((varflags & VSQUOTE) && *loc == CTLESC)
			        loc++;
		}
		return 0;

	case VSTRIMLEFTMAX:
		for (loc = str - 1; loc >= startp;) {
			c = *loc;
			*loc = '\0';
			if (patmatch(psh, str, startp, varflags & VSQUOTE))
				goto recordleft;
			*loc = c;
			loc--;
			if ((varflags & VSQUOTE) && loc > startp &&
			    *(loc - 1) == CTLESC) {
				for (q = startp; q < loc; q++)
					if (*q == CTLESC)
						q++;
				if (q > loc)
					loc--;
			}
		}
		return 0;

	case VSTRIMRIGHT:
	        for (loc = str - 1; loc >= startp;) {
			if (patmatch(psh, str, loc, varflags & VSQUOTE))
				goto recordright;
			loc--;
			if ((varflags & VSQUOTE) && loc > startp &&
			    *(loc - 1) == CTLESC) {
				for (q = startp; q < loc; q++)
					if (*q == CTLESC)
						q++;
				if (q > loc)
					loc--;
			}
		}
		return 0;

	case VSTRIMRIGHTMAX:
		for (loc = startp; loc < str - 1; loc++) {
			if (patmatch(psh, str, loc, varflags & VSQUOTE))
				goto recordright;
			if ((varflags & VSQUOTE) && *loc == CTLESC)
			        loc++;
		}
		return 0;

	default:
		sh_abort(psh);
	}

recordleft:
	*loc = c;
	amount = (int)(((str - 1) - (loc - startp)) - psh->expdest);
	STADJUST(psh, amount, psh->expdest);
	while (loc != str - 1)
		*startp++ = *loc++;
	return 1;

recordright:
	amount = (int)(loc - psh->expdest);
	STADJUST(psh, amount, psh->expdest);
	STPUTC(psh, '\0', psh->expdest);
	STADJUST(psh, -1, psh->expdest);
	return 1;
}


/*
 * Expand a variable, and return a pointer to the next character in the
 * input string.
 */

STATIC char *
evalvar(shinstance *psh, char *p, int flag)
{
	int subtype;
	int varflags;
	char *var;
	char *val;
	int patloc;
	int c;
	int set;
	int special;
	int startloc;
	int varlen;
	int apply_ifs;
	int quotes = flag & (EXP_FULL | EXP_CASE);

	varflags = (unsigned char)*p++;
	subtype = varflags & VSTYPE;
	var = p;
	special = !is_name(*p);
	p = strchr(p, '=') + 1;

again: /* jump here after setting a variable with ${var=text} */
	if (special) {
		set = varisset(psh, var, varflags & VSNUL);
		val = NULL;
	} else {
		val = lookupvar(psh, var);
		if (val == NULL || ((varflags & VSNUL) && val[0] == '\0')) {
			val = NULL;
			set = 0;
		} else
			set = 1;
	}

	varlen = 0;
	startloc = (int)(psh->expdest - stackblock(psh));

	if (!set && uflag(psh)) {
		switch (subtype) {
		case VSNORMAL:
		case VSTRIMLEFT:
		case VSTRIMLEFTMAX:
		case VSTRIMRIGHT:
		case VSTRIMRIGHTMAX:
		case VSLENGTH:
			error(psh, "%.*s: parameter not set", p - var - 1, var);
			/* NOTREACHED */
		}
	}

	if (set && subtype != VSPLUS) {
		/* insert the value of the variable */
		if (special) {
			varvalue(psh, var, varflags & VSQUOTE, subtype, flag);
			if (subtype == VSLENGTH) {
				varlen = (int)(psh->expdest - stackblock(psh) - startloc);
				STADJUST(psh, -varlen, psh->expdest);
			}
		} else {
			char const *syntax = (varflags & VSQUOTE) ? DQSYNTAX
								  : BASESYNTAX;

			if (subtype == VSLENGTH) {
				for (;*val; val++)
					varlen++;
			} else {
				while (*val) {
					if (quotes && syntax[(int)*val] == CCTL)
						STPUTC(psh, CTLESC, psh->expdest);
					STPUTC(psh, *val++, psh->expdest);
				}

			}
		}
	}


	apply_ifs = ((varflags & VSQUOTE) == 0 ||
		(*var == '@' && psh->shellparam.nparam != 1));

	switch (subtype) {
	case VSLENGTH:
		psh->expdest = cvtnum(psh, varlen, psh->expdest);
		break;

	case VSNORMAL:
		break;

	case VSPLUS:
		set = !set;
		/* FALLTHROUGH */
	case VSMINUS:
		if (!set) {
		        argstr(psh, p, flag | (apply_ifs ? EXP_IFS_SPLIT : 0));
			/*
			 * ${x-a b c} doesn't get split, but removing the
			 * 'apply_ifs = 0' apparantly breaks ${1+"$@"}..
			 * ${x-'a b' c} should generate 2 args.
			 */
			/* We should have marked stuff already */
			apply_ifs = 0;
		}
		break;

	case VSTRIMLEFT:
	case VSTRIMLEFTMAX:
	case VSTRIMRIGHT:
	case VSTRIMRIGHTMAX:
		if (!set)
			break;
		/*
		 * Terminate the string and start recording the pattern
		 * right after it
		 */
		STPUTC(psh, '\0', psh->expdest);
		patloc = (int)(psh->expdest - stackblock(psh));
		if (subevalvar(psh, p, NULL, patloc, subtype,
			       startloc, varflags) == 0) {
			int amount = (int)(psh->expdest - stackblock(psh) - patloc) + 1;
			STADJUST(psh, -amount, psh->expdest);
		}
		/* Remove any recorded regions beyond start of variable */
		removerecordregions(psh, startloc);
		apply_ifs = 1;
		break;

	case VSASSIGN:
	case VSQUESTION:
		if (set)
			break;
		if (subevalvar(psh, p, var, 0, subtype, startloc, varflags)) {
			varflags &= ~VSNUL;
			/*
			 * Remove any recorded regions beyond
			 * start of variable
			 */
			removerecordregions(psh, startloc);
			goto again;
		}
		apply_ifs = 0;
		break;

	default:
		sh_abort(psh);
	}

	if (apply_ifs)
		recordregion(psh, startloc, (int)(psh->expdest - stackblock(psh)),
			     varflags & VSQUOTE);

	if (subtype != VSNORMAL) {	/* skip to end of alternative */
		int nesting = 1;
		for (;;) {
			if ((c = *p++) == CTLESC)
				p++;
			else if (c == CTLBACKQ || c == (CTLBACKQ|CTLQUOTE)) {
				if (set)
					psh->argbackq = psh->argbackq->next;
			} else if (c == CTLVAR) {
				if ((*p++ & VSTYPE) != VSNORMAL)
					nesting++;
			} else if (c == CTLENDVAR) {
				if (--nesting == 0)
					break;
			}
		}
	}
	return p;
}



/*
 * Test whether a specialized variable is set.
 */

STATIC int
varisset(shinstance *psh, char *name, int nulok)
{
	if (*name == '!')
		return psh->backgndpid != -1;
	else if (*name == '@' || *name == '*') {
		if (*psh->shellparam.p == NULL)
			return 0;

		if (nulok) {
			char **av;

			for (av = psh->shellparam.p; *av; av++)
				if (**av != '\0')
					return 1;
			return 0;
		}
	} else if (is_digit(*name)) {
		char *ap;
		int num = atoi(name);

		if (num > psh->shellparam.nparam)
			return 0;

		if (num == 0)
			ap = psh->arg0;
		else
			ap = psh->shellparam.p[num - 1];

		if (nulok && (ap == NULL || *ap == '\0'))
			return 0;
	}
	return 1;
}



/*
 * Add the value of a specialized variable to the stack string.
 */

STATIC void
varvalue(shinstance *psh, char *name, int quoted, int subtype, int flag)
{
	int num;
	char *p;
	int i;
	char sep;
	char **ap;
	char const *syntax;

#define STRTODEST(p) \
	do {\
	if (flag & (EXP_FULL | EXP_CASE) && subtype != VSLENGTH) { \
		syntax = quoted? DQSYNTAX : BASESYNTAX; \
		while (*p) { \
			if (syntax[(int)*p] == CCTL) \
				STPUTC(psh, CTLESC, psh->expdest); \
			STPUTC(psh, *p++, psh->expdest); \
		} \
	} else \
		while (*p) \
			STPUTC(psh, *p++, psh->expdest); \
	} while (0)


	switch (*name) {
	case '$':
		num = psh->rootpid;
		goto numvar;
	case '?':
		num = psh->exitstatus;
		goto numvar;
	case '#':
		num = psh->shellparam.nparam;
		goto numvar;
	case '!':
		num = psh->backgndpid;
numvar:
		psh->expdest = cvtnum(psh, num, psh->expdest);
		break;
	case '-':
		for (i = 0; psh->optlist[i].name; i++) {
			if (psh->optlist[i].val)
				STPUTC(psh, psh->optlist[i].letter, psh->expdest);
		}
		break;
	case '@':
		if (flag & EXP_FULL && quoted) {
			for (ap = psh->shellparam.p ; (p = *ap++) != NULL ; ) {
				STRTODEST(p);
				if (*ap)
					STPUTC(psh, '\0', psh->expdest);
			}
			break;
		}
		/* fall through */
	case '*':
		if (ifsset(psh) != 0)
			sep = ifsval(psh)[0];
		else
			sep = ' ';
		for (ap = psh->shellparam.p ; (p = *ap++) != NULL ; ) {
			STRTODEST(p);
			if (*ap && sep)
				STPUTC(psh, sep, psh->expdest);
		}
		break;
	case '0':
		p = psh->arg0;
		STRTODEST(p);
		break;
	default:
		if (is_digit(*name)) {
			num = atoi(name);
			if (num > 0 && num <= psh->shellparam.nparam) {
				p = psh->shellparam.p[num - 1];
				STRTODEST(p);
			}
		}
		break;
	}
}



/*
 * Record the fact that we have to scan this region of the
 * string for IFS characters.
 */

STATIC void
recordregion(shinstance *psh, int start, int end, int inquotes)
{
	struct ifsregion *ifsp;

	if (psh->ifslastp == NULL) {
		ifsp = &psh->ifsfirst;
	} else {
		if (psh->ifslastp->endoff == start
		    && psh->ifslastp->inquotes == inquotes) {
			/* extend previous area */
			psh->ifslastp->endoff = end;
			return;
		}
		ifsp = (struct ifsregion *)ckmalloc(psh, sizeof (struct ifsregion));
		psh->ifslastp->next = ifsp;
	}
	psh->ifslastp = ifsp;
	psh->ifslastp->next = NULL;
	psh->ifslastp->begoff = start;
	psh->ifslastp->endoff = end;
	psh->ifslastp->inquotes = inquotes;
}



/*
 * Break the argument string into pieces based upon IFS and add the
 * strings to the argument list.  The regions of the string to be
 * searched for IFS characters have been stored by recordregion.
 */
STATIC void
ifsbreakup(shinstance *psh, char *string, struct arglist *arglist)
{
	struct ifsregion *ifsp;
	struct strlist *sp;
	char *start;
	char *p;
	char *q;
	const char *ifs;
	const char *ifsspc;
	int inquotes;

	start = string;
	ifsspc = NULL;
	inquotes = 0;

	if (psh->ifslastp == NULL) {
		/* Return entire argument, IFS doesn't apply to any of it */
		sp = (struct strlist *)stalloc(psh, sizeof *sp);
		sp->text = start;
		*arglist->lastp = sp;
		arglist->lastp = &sp->next;
		return;
	}

	ifs = ifsset(psh) ? ifsval(psh) : " \t\n";

	for (ifsp = &psh->ifsfirst; ifsp != NULL; ifsp = ifsp->next) {
		p = string + ifsp->begoff;
		inquotes = ifsp->inquotes;
		ifsspc = NULL;
		while (p < string + ifsp->endoff) {
			q = p;
			if (*p == CTLESC)
				p++;
			if (inquotes) {
				/* Only NULs (probably from "$@") end args */
				if (*p != 0) {
					p++;
					continue;
				}
			} else {
				if (!strchr(ifs, *p)) {
					p++;
					continue;
				}
				ifsspc = strchr(" \t\n", *p);

				/* Ignore IFS whitespace at start */
				if (q == start && ifsspc != NULL) {
					p++;
					start = p;
					continue;
				}
			}

			/* Save this argument... */
			*q = '\0';
			sp = (struct strlist *)stalloc(psh, sizeof *sp);
			sp->text = start;
			*arglist->lastp = sp;
			arglist->lastp = &sp->next;
			p++;

			if (ifsspc != NULL) {
				/* Ignore further trailing IFS whitespace */
				for (; p < string + ifsp->endoff; p++) {
					q = p;
					if (*p == CTLESC)
						p++;
					if (strchr(ifs, *p) == NULL) {
						p = q;
						break;
					}
					if (strchr(" \t\n", *p) == NULL) {
						p++;
						break;
					}
				}
			}
			start = p;
		}
	}

	/*
	 * Save anything left as an argument.
	 * Traditionally we have treated 'IFS=':'; set -- x$IFS' as
	 * generating 2 arguments, the second of which is empty.
	 * Some recent clarification of the Posix spec say that it
	 * should only generate one....
	 */
	if (*start /* || (!ifsspc && start > string) */) {
		sp = (struct strlist *)stalloc(psh, sizeof *sp);
		sp->text = start;
		*arglist->lastp = sp;
		arglist->lastp = &sp->next;
	}
}

STATIC void
ifsfree(shinstance *psh)
{
	while (psh->ifsfirst.next != NULL) {
		struct ifsregion *ifsp;
		INTOFF;
		ifsp = psh->ifsfirst.next->next;
		ckfree(psh, psh->ifsfirst.next);
		psh->ifsfirst.next = ifsp;
		INTON;
	}
	psh->ifslastp = NULL;
	psh->ifsfirst.next = NULL;
}



/*
 * Expand shell metacharacters.  At this point, the only control characters
 * should be escapes.  The results are stored in the list psh->exparg.
 */

//char *expdir;


STATIC void
expandmeta(shinstance *psh, struct strlist *str, int flag)
{
	char *p;
	struct strlist **savelastp;
	struct strlist *sp;
	char c;
	/* TODO - EXP_REDIR */

	while (str) {
		if (fflag(psh))
			goto nometa;
		p = str->text;
		for (;;) {			/* fast check for meta chars */
			if ((c = *p++) == '\0')
				goto nometa;
			if (c == '*' || c == '?' || c == '[' || c == '!')
				break;
		}
		savelastp = psh->exparg.lastp;
		INTOFF;
		if (psh->expdir == NULL) {
			size_t i = strlen(str->text);
			psh->expdir = ckmalloc(psh, i < 2048 ? 2048 : i); /* XXX */
		}

		expmeta(psh, psh->expdir, str->text);
		ckfree(psh, psh->expdir);
		psh->expdir = NULL;
		INTON;
		if (psh->exparg.lastp == savelastp) {
			/*
			 * no matches
			 */
nometa:
			*psh->exparg.lastp = str;
			rmescapes(psh, str->text);
			psh->exparg.lastp = &str->next;
		} else {
			*psh->exparg.lastp = NULL;
			*savelastp = sp = expsort(*savelastp);
			while (sp->next != NULL)
				sp = sp->next;
			psh->exparg.lastp = &sp->next;
		}
		str = str->next;
	}
}


/*
 * Do metacharacter (i.e. *, ?, [...]) expansion.
 */

STATIC void
expmeta(shinstance *psh, char *enddir, char *name)
{
	char *p;
	const char *cp;
	char *q;
	char *start;
	char *endname;
	int metaflag;
	struct stat statb;
	shdir *dirp;
	shdirent *dp;
	int atend;
	int matchdot;

	metaflag = 0;
	start = name;
	for (p = name ; ; p++) {
		if (*p == '*' || *p == '?')
			metaflag = 1;
		else if (*p == '[') {
			q = p + 1;
			if (*q == '!')
				q++;
			for (;;) {
				while (*q == CTLQUOTEMARK)
					q++;
				if (*q == CTLESC)
					q++;
				if (*q == '/' || *q == '\0')
					break;
				if (*++q == ']') {
					metaflag = 1;
					break;
				}
			}
		} else if (*p == '!' && p[1] == '!'	&& (p == name || p[-1] == '/')) {
			metaflag = 1;
		} else if (*p == '\0')
			break;
		else if (*p == CTLQUOTEMARK)
			continue;
		else if (*p == CTLESC)
			p++;
		if (*p == '/') {
			if (metaflag)
				break;
			start = p + 1;
		}
	}
	if (metaflag == 0) {	/* we've reached the end of the file name */
		if (enddir != psh->expdir)
			metaflag++;
		for (p = name ; ; p++) {
			if (*p == CTLQUOTEMARK)
				continue;
			if (*p == CTLESC)
				p++;
			*enddir++ = *p;
			if (*p == '\0')
				break;
		}
		if (metaflag == 0 || shfile_lstat(&psh->fdtab, psh->expdir, &statb) >= 0)
			addfname(psh, psh->expdir);
		TRACE2((psh, "expandarg: return #1 (metaflag=%d)\n", metaflag));
		return;
	}
	endname = p;
	if (start != name) {
		p = name;
		while (p < start) {
			while (*p == CTLQUOTEMARK)
				p++;
			if (*p == CTLESC)
				p++;
			*enddir++ = *p++;
		}
	}
	if (enddir == psh->expdir) {
		cp = ".";
	} else if (enddir == psh->expdir + 1 && *psh->expdir == '/') {
		cp = "/";
	} else {
		cp = psh->expdir;
		enddir[-1] = '\0';
	}
	if ((dirp = shfile_opendir(&psh->fdtab, cp)) == NULL) {
		TRACE2((psh, "expandarg: return #2 (shfile_opendir(,%s) failed)\n", cp));
		return;
	}
	if (enddir != psh->expdir)
		enddir[-1] = '/';
	if (*endname == 0) {
		atend = 1;
	} else {
		atend = 0;
		*endname++ = '\0';
	}
	matchdot = 0;
	p = start;
	while (*p == CTLQUOTEMARK)
		p++;
	if (*p == CTLESC)
		p++;
	if (*p == '.')
		matchdot++;
	while (! int_pending() && (dp = shfile_readdir(dirp)) != NULL) {
		if (dp->name[0] == '.' && ! matchdot)
			continue;
		if (patmatch(psh, start, dp->name, 0)) {
			if (atend) {
				scopy(dp->name, enddir);
				addfname(psh, psh->expdir);
			} else {
				for (p = enddir, cp = dp->name;
				     (*p++ = *cp++) != '\0';)
					continue;
				p[-1] = '/';
				expmeta(psh, p, endname);
			}
		}
	}
	shfile_closedir(dirp);
	if (! atend)
		endname[-1] = '/';
}


/*
 * Add a file name to the list.
 */

STATIC void
addfname(shinstance *psh, char *name)
{
	char *p;
	struct strlist *sp;

	p = stalloc(psh, strlen(name) + 1);
	scopy(name, p);
	sp = (struct strlist *)stalloc(psh, sizeof *sp);
	sp->text = p;
	*psh->exparg.lastp = sp;
	psh->exparg.lastp = &sp->next;
}


/*
 * Sort the results of file name expansion.  It calculates the number of
 * strings to sort and then calls msort (short for merge sort) to do the
 * work.
 */

STATIC struct strlist *
expsort(struct strlist *str)
{
	int len;
	struct strlist *sp;

	len = 0;
	for (sp = str ; sp ; sp = sp->next)
		len++;
	return msort(str, len);
}


STATIC struct strlist *
msort(struct strlist *list, int len)
{
	struct strlist *p, *q = NULL;
	struct strlist **lpp;
	int half;
	int n;

	if (len <= 1)
		return list;
	half = len >> 1;
	p = list;
	for (n = half ; --n >= 0 ; ) {
		q = p;
		p = p->next;
	}
	q->next = NULL;			/* terminate first half of list */
	q = msort(list, half);		/* sort first half of list */
	p = msort(p, len - half);		/* sort second half */
	lpp = &list;
	for (;;) {
		if (strcmp(p->text, q->text) < 0) {
			*lpp = p;
			lpp = &p->next;
			if ((p = *lpp) == NULL) {
				*lpp = q;
				break;
			}
		} else {
			*lpp = q;
			lpp = &q->next;
			if ((q = *lpp) == NULL) {
				*lpp = p;
				break;
			}
		}
	}
	return list;
}



/*
 * Returns true if the pattern matches the string.
 */

int
patmatch(shinstance *psh, char *pattern, char *string, int squoted)
{
#ifdef notdef
	if (pattern[0] == '!' && pattern[1] == '!')
		return 1 - pmatch(pattern + 2, string);
	else
#endif
		return pmatch(pattern, string, squoted);
}


STATIC int
pmatch(char *pattern, char *string, int squoted)
{
	char *p, *q;
	char c;

	p = pattern;
	q = string;
	for (;;) {
		switch (c = *p++) {
		case '\0':
			goto breakloop;
		case CTLESC:
			if (squoted && *q == CTLESC)
				q++;
			if (*q++ != *p++)
				return 0;
			break;
		case CTLQUOTEMARK:
			continue;
		case '?':
			if (squoted && *q == CTLESC)
				q++;
			if (*q++ == '\0')
				return 0;
			break;
		case '*':
			c = *p;
			while (c == CTLQUOTEMARK || c == '*')
				c = *++p;
			if (c != CTLESC &&  c != CTLQUOTEMARK &&
			    c != '?' && c != '*' && c != '[') {
				while (*q != c) {
					if (squoted && *q == CTLESC &&
					    q[1] == c)
						break;
					if (*q == '\0')
						return 0;
					if (squoted && *q == CTLESC)
						q++;
					q++;
				}
			}
			do {
				if (pmatch(p, q, squoted))
					return 1;
				if (squoted && *q == CTLESC)
					q++;
			} while (*q++ != '\0');
			return 0;
		case '[': {
			char *endp;
			int invert, found;
			char chr;

			endp = p;
			if (*endp == '!')
				endp++;
			for (;;) {
				while (*endp == CTLQUOTEMARK)
					endp++;
				if (*endp == '\0')
					goto dft;		/* no matching ] */
				if (*endp == CTLESC)
					endp++;
				if (*++endp == ']')
					break;
			}
			invert = 0;
			if (*p == '!') {
				invert++;
				p++;
			}
			found = 0;
			chr = *q++;
			if (squoted && chr == CTLESC)
				chr = *q++;
			if (chr == '\0')
				return 0;
			c = *p++;
			do {
				if (c == CTLQUOTEMARK)
					continue;
				if (c == CTLESC)
					c = *p++;
				if (*p == '-' && p[1] != ']') {
					p++;
					while (*p == CTLQUOTEMARK)
						p++;
					if (*p == CTLESC)
						p++;
					if (chr >= c && chr <= *p)
						found = 1;
					p++;
				} else {
					if (chr == c)
						found = 1;
				}
			} while ((c = *p++) != ']');
			if (found == invert)
				return 0;
			break;
		}
dft:	        default:
			if (squoted && *q == CTLESC)
				q++;
			if (*q++ != c)
				return 0;
			break;
		}
	}
breakloop:
	if (*q != '\0')
		return 0;
	return 1;
}



/*
 * Remove any CTLESC characters from a string.
 */

void
rmescapes(shinstance *psh, char *str)
{
	char *p, *q;

	p = str;
	while (*p != CTLESC && *p != CTLQUOTEMARK) {
		if (*p++ == '\0')
			return;
	}
	q = p;
	while (*p) {
		if (*p == CTLQUOTEMARK) {
			p++;
			continue;
		}
		if (*p == CTLESC)
			p++;
		*q++ = *p++;
	}
	*q = '\0';
}



/*
 * See if a pattern matches in a case statement.
 */

int
casematch(shinstance *psh, union node *pattern, char *val)
{
	struct stackmark smark;
	int result;
	char *p;

	setstackmark(psh, &smark);
	psh->argbackq = pattern->narg.backquote;
	STARTSTACKSTR(psh, psh->expdest);
	psh->ifslastp = NULL;
	argstr(psh, pattern->narg.text, EXP_TILDE | EXP_CASE);
	STPUTC(psh, '\0', psh->expdest);
	p = grabstackstr(psh, psh->expdest);
	result = patmatch(psh, p, val, 0);
	popstackmark(psh, &smark);
	return result;
}

/*
 * Our own itoa().
 */

STATIC char *
cvtnum(shinstance *psh, int num, char *buf)
{
	char temp[32];
	int neg = num < 0;
	char *p = temp + 31;

	temp[31] = '\0';

	do {
		*--p = num % 10 + '0';
	} while ((num /= 10) != 0);

	if (neg)
		*--p = '-';

	while (*p)
		STPUTC(psh, *p++, buf);
	return buf;
}

/*
 * Do most of the work for wordexp(3).
 */

int
wordexpcmd(shinstance *psh, int argc, char **argv)
{
	size_t len;
	int i;

	out1fmt(psh, "%d", argc - 1);
	out1c(psh, '\0');
	for (i = 1, len = 0; i < argc; i++)
		len += strlen(argv[i]);
	out1fmt(psh, "%zd", len);
	out1c(psh, '\0');
	for (i = 1; i < argc; i++) {
		out1str(psh, argv[i]);
		out1c(psh, '\0');
	}
	return (0);
}
