/* $Id: quoted_spawn.c 2851 2016-08-31 17:30:52Z bird $ */
/** @file
 * quote_spawn - Correctly Quote The _spawnvp arguments, windows specific.
 */

/*
 * Copyright (c) 2010-2016 knut st. osmundsen <bird-kBuild-spamx@anduin.net>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Alternatively, the content of this file may be used under the terms of the
 * GPL version 2 or later, or LGPL version 2.1 or later.
 */


/*******************************************************************************
*   Header Files                                                               *
*******************************************************************************/
#include "quoted_spawn.h"

#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>


/**
 * Tests if a strings needs quoting.
 *
 * @returns 1 if needs, 0 if it doesn't.
 * @param   pszArg              The string in question.
 */
static int quoted_spawn_need_quoting(const char *pszArg)
{
    for (;;)
        switch (*pszArg++)
        {
            case 0:
                return 0;

            case ' ':
            case '"':
            case '&':
            case '>':
            case '<':
            case '|':
            case '%':
            /* Quote the control chars (tab is included). */
            case 1:
            case 2:
            case 3:
            case 4:
            case 5:
            case 6:
            case 7:
            case 8:
            case 9:
            case 10:
            case 11:
            case 13:
            case 14:
            case 15:
            case 16:
            case 17:
            case 18:
            case 19:
            case 20:
            case 21:
            case 22:
            case 23:
            case 24:
            case 25:
            case 26:
            case 27:
            case 28:
            case 29:
            case 30:
            case 31:
                return 1;
        }
}

/**
 * Frees any quoted arguments.
 *
 * @returns NULL.
 * @param   papszArgsOrg    The original argument vector.
 * @param   papszArgsQuoted The quoted argument vector.
 * @param   cArgs           The number of arguments in the vector.
 */
static const char * const *
quoted_spawn_free(const char * const *papszArgsOrg, const char * const *papszArgsQuoted, unsigned cArgs)
{
    if (   papszArgsOrg    != papszArgsQuoted
        && papszArgsQuoted != NULL)
    {
        int      iSavedErrno = errno; /* A bit of paranoia. */
        unsigned i           = cArgs;
        while (i-- > 0)
            if (papszArgsQuoted[i] != papszArgsOrg[i])
                free((char *)papszArgsQuoted[i]);
        free((void *)papszArgsQuoted);
        errno = iSavedErrno;
    }
    return NULL;
}

/**
 * Quote an argument string.
 *
 * @returns Quoted argument string (new).
 * @param   pszArgOrg           The original string.
 */
static const char *quoted_spawn_quote_arg(const char *pszArgOrg)
{
    size_t cchArgOrg = strlen(pszArgOrg);
    size_t cchArgNew = 1 + cchArgOrg * 2 + 1 + 1;
    char  *pszArgNew = malloc(cchArgNew);
    if (pszArgNew)
    {
        char ch;
        char *pszDst = pszArgNew;
        *pszDst++ = '"';
        while ((ch = *pszArgOrg++))
        {
            if (ch == '\\')
            {
                size_t cSlashes = 1;
                for (;;)
                {
                    *pszDst++ = '\\';
                     ch = *pszArgOrg;
                     if (ch != '\\')
                         break;
                     pszArgOrg++;
                     cSlashes++;
                }
                if (ch == '"' || ch == '\0')
                {
                    while (cSlashes-- > 0)
                        *pszDst++ = '\\';
                    if (ch == '\0')
                        break;
                    *pszDst++ = '\\';
                    *pszDst++ = '"';
                }
            }
            else if (ch == '"')
            {
                *pszDst++ = '\\';
                *pszDst++ = '"';
            }
            else
                *pszDst++ = ch;
        }
        *pszDst++ = '"';
        *pszDst = '\0';
        assert((size_t)(pszDst - pszArgNew) < cchArgNew - 1);
    }
    return pszArgNew;
}

/**
 * Quotes the arguments in an argument vector, producing a new vector.
 *
 * @returns The quoted argument vector.
 * @param   papszArgsOrg    The vector which arguments to quote.
 * @param   iFirstArg       The first argument that needs quoting.
 * @param   pcArgs          Where to return the argument count.
 */
static const char * const *
quoted_spawn_quote_vector(const char * const *papszArgsOrg, unsigned iFirstArg, unsigned *pcArgs)
{
    const char **papszArgsQuoted;
    unsigned     cArgs;
    unsigned     iArg;

    /* finish counting them and allocate the result array. */
    cArgs = iFirstArg;
    while (papszArgsOrg[cArgs])
        cArgs++;
    *pcArgs = cArgs;

    papszArgsQuoted = (const char **)calloc(sizeof(const char *), cArgs + 1);
    if (!papszArgsQuoted)
        return NULL;

    /* Process the arguments up to the first quoted one (no need to
       re-examine them). */
    for (iArg = 0; iArg < iFirstArg; iArg++)
        papszArgsQuoted[iArg] = papszArgsOrg[iArg];

    papszArgsQuoted[iArg] = quoted_spawn_quote_arg(papszArgsOrg[iArg]);
    if (!papszArgsQuoted[iArg])
        return quoted_spawn_free(papszArgsOrg, papszArgsQuoted, cArgs);

    /* Process the remaining arguments. */
    while (iArg < cArgs)
    {
        if (!quoted_spawn_need_quoting(papszArgsOrg[iArg]))
            papszArgsQuoted[iArg] = papszArgsOrg[iArg];
        else
        {
            papszArgsQuoted[iArg] = quoted_spawn_quote_arg(papszArgsOrg[iArg]);
            if (!papszArgsQuoted[iArg])
                return quoted_spawn_free(papszArgsOrg, papszArgsQuoted, cArgs);
        }
        iArg++;
    }

    return papszArgsQuoted;
}

/**
 * Checks if any of the arguments in the vector needs quoting and does the job.
 *
 * @returns If anything needs quoting a new vector is returned, otherwise the
 *          original is returned.
 * @param   papszArgsOrg    The argument vector to check.
 * @param   pcArgs          Where to return the argument count.
 */
static const char * const *
quoted_spawn_maybe_quote(const char * const *papszArgsOrg, unsigned *pcArgs)
{
    unsigned iArg;
    for (iArg = 0; papszArgsOrg[iArg]; iArg++)
        if (quoted_spawn_need_quoting(papszArgsOrg[iArg]))
            return quoted_spawn_quote_vector(papszArgsOrg, iArg, pcArgs);
    *pcArgs = iArg;
    return papszArgsOrg;
}

/**
 * Wrapper for _spawnvp.
 *
 * @returns The process handle, see _spawnvp for details.
 * @param   fMode               The spawn mode, see _spawnvp for details.
 * @param   pszExecPath         The path to the executable, or just the name
 *                              if a PATH search is desired.
 * @param   papszArgs           The arguments to pass to the new process.
 */
intptr_t quoted_spawnvp(int fMode, const char *pszExecPath, const char * const *papszArgs)
{
    intptr_t            hProcess;
    unsigned            cArgs;
    const char * const *papszArgsQuoted = quoted_spawn_maybe_quote(papszArgs, &cArgs);
    if (papszArgsQuoted)
    {
//unsigned i;
//fprintf(stderr,  "debug: spawning '%s'\n",  pszExecPath);
//for (i = 0; i < cArgs; i++)
//    fprintf(stderr,  "debug: #%02u: '%s'\n",  i,  papszArgsQuoted[i]);
        hProcess = _spawnvp(fMode, pszExecPath, papszArgsQuoted);
        quoted_spawn_free(papszArgs, papszArgsQuoted, cArgs);
    }
    else
    {
        errno = ENOMEM;
        hProcess = -1;
    }

    return hProcess;
}

