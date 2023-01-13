/*	$NetBSD: syntax.c,v 1.1 2004/01/17 17:38:12 dsl Exp $	*/

#include "shell.h"
#include "syntax.h"
#include "parser.h"
#include "shinstance.h"

#ifdef _MSC_VER /* doesn't implement the fancy initializers I think... */

char basesyntax[257] = {CSHEOF};
char dqsyntax[257] = {CSHEOF};
char sqsyntax[257] = {CSHEOF};
char arisyntax[257] = {CSHEOF};
char is_type[257] = {0};

void init_syntax(void)
{
    char *tab;
    int i;

#define ndx(ch) (ch + 1 - CHAR_MIN)
#define set(ch, val) tab[ndx(ch)] = val
#define set_range(s, e, val) for (i = ndx(s); i <= ndx(e); i++) tab[i] = val

    /*basesyntax*/
    tab = basesyntax;
    set_range(CTL_FIRST, CTL_LAST, CCTL);
    set('\n', CNL);
    set('\\', CBACK);
    set('\'', CSQUOTE);
    set('"', CDQUOTE);
    set('`', CBQUOTE);
    set('$', CVAR);
    set('}', CENDVAR);
    set('<', CSPCL);
    set('>', CSPCL);
    set('(', CSPCL);
    set(')', CSPCL);
    set(';', CSPCL);
    set('&', CSPCL);
    set('|', CSPCL);
    set(' ', CSPCL);
    set('\t', CSPCL);

    tab = dqsyntax;
    set_range(CTL_FIRST, CTL_LAST, CCTL);
    set('\n', CNL);
    set('\\', CBACK);
    set('"', CDQUOTE);
    set('`', CBQUOTE);
    set('$', CVAR);
    set('}', CENDVAR);
    /* ':/' for tilde expansion, '-' for [a\-x] pattern ranges */
    set('!', CCTL);
    set('*', CCTL);
    set('?', CCTL);
    set('[', CCTL);
    set('=', CCTL);
    set('~', CCTL);
    set(':', CCTL);
    set('/', CCTL);
    set('-', CCTL);

    tab = sqsyntax;
    set_range(CTL_FIRST, CTL_LAST, CCTL);
    set('\n', CNL);
    set('\'', CSQUOTE);
    /* ':/' for tilde expansion, '-' for [a\-x] pattern ranges */
    set('!', CCTL);
    set('*', CCTL) ;
    set('?', CCTL);
    set('[', CCTL);
    set('=', CCTL);
    set('~', CCTL);
    set(':', CCTL);
    set('/', CCTL);
    set('-', CCTL);

    tab = arisyntax;
    set_range(CTL_FIRST, CTL_LAST, CCTL);
    set('\n', CNL);
    set('\\', CBACK);
    set('`', CBQUOTE);
    set('\'', CSQUOTE);
    set('"', CDQUOTE);
    set('$', CVAR);
    set('}', CENDVAR);
    set('(', CLP);
    set(')', CRP);

    tab = is_type;
    set_range('0', '9', ISDIGIT);
    set_range('a', 'z', ISLOWER);
    set_range('A', 'Z', ISUPPER);
    set('_', ISUNDER);
    set('#', ISSPECL);
    set('?', ISSPECL);
    set('$', ISSPECL);
    set('!', ISSPECL);
    set('-', ISSPECL);
    set('*', ISSPECL);
    set('@', ISSPECL);
}

#else /* !_MSC_VER */

#if CWORD != 0
#error initialisation assumes 'CWORD' is zero
#endif

#define ndx(ch) (ch + 1 - CHAR_MIN)
#define set(ch, val) [ndx(ch)] = val,
#define set_range(s, e, val) [ndx(s) ... ndx(e)] = val,

/* Shut up the pedantic warnings about set_range() */
#ifdef __GNUC__
# if __GNUC__ >= 5 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 2)
#  pragma GCC diagnostic ignored "-Wpedantic"
# endif
#endif

/* syntax table used when not in quotes */
const char basesyntax[257] = { CSHEOF,
    set_range(CTL_FIRST, CTL_LAST, CCTL)
    set('\n', CNL)
    set('\\', CBACK)
    set('\'', CSQUOTE)
    set('"', CDQUOTE)
    set('`', CBQUOTE)
    set('$', CVAR)
    set('}', CENDVAR)
    set('<', CSPCL)
    set('>', CSPCL)
    set('(', CSPCL)
    set(')', CSPCL)
    set(';', CSPCL)
    set('&', CSPCL)
    set('|', CSPCL)
    set(' ', CSPCL)
    set('\t', CSPCL)
};

/* syntax table used when in double quotes */
const char dqsyntax[257] = { CSHEOF,
    set_range(CTL_FIRST, CTL_LAST, CCTL)
    set('\n', CNL)
    set('\\', CBACK)
    set('"', CDQUOTE)
    set('`', CBQUOTE)
    set('$', CVAR)
    set('}', CENDVAR)
    /* ':/' for tilde expansion, '-' for [a\-x] pattern ranges */
    set('!', CCTL)
    set('*', CCTL)
    set('?', CCTL)
    set('[', CCTL)
    set('=', CCTL)
    set('~', CCTL)
    set(':', CCTL)
    set('/', CCTL)
    set('-', CCTL)
};

/* syntax table used when in single quotes */
const char sqsyntax[257] = { CSHEOF,
    set_range(CTL_FIRST, CTL_LAST, CCTL)
    set('\n', CNL)
    set('\'', CSQUOTE)
    /* ':/' for tilde expansion, '-' for [a\-x] pattern ranges */
    set('!', CCTL)
    set('*', CCTL)
    set('?', CCTL)
    set('[', CCTL)
    set('=', CCTL)
    set('~', CCTL)
    set(':', CCTL)
    set('/', CCTL)
    set('-', CCTL)
};

/* syntax table used when in arithmetic */
const char arisyntax[257] = { CSHEOF,
    set_range(CTL_FIRST, CTL_LAST, CCTL)
    set('\n', CNL)
    set('\\', CBACK)
    set('`', CBQUOTE)
    set('\'', CSQUOTE)
    set('"', CDQUOTE)
    set('$', CVAR)
    set('}', CENDVAR)
    set('(', CLP)
    set(')', CRP)
};

/* character classification table */
const char is_type[257] = { 0,
    set_range('0', '9', ISDIGIT)
    set_range('a', 'z', ISLOWER)
    set_range('A', 'Z', ISUPPER)
    set('_', ISUNDER)
    set('#', ISSPECL)
    set('?', ISSPECL)
    set('$', ISSPECL)
    set('!', ISSPECL)
    set('-', ISSPECL)
    set('*', ISSPECL)
    set('@', ISSPECL)
};
#endif /* !_MSC_VER */
