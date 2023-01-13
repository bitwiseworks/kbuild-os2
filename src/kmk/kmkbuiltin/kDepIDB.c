/* $Id: kDepIDB.c 3315 2020-03-31 01:12:19Z bird $ */
/** @file
 * kDepIDB - Extract dependency information from a MS Visual C++ .idb file.
 */

/*
 * Copyright (c) 2007-2010 knut st. osmundsen <bird-kBuild-spamx@anduin.net>
 *
 * This file is part of kBuild.
 *
 * kBuild is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * kBuild is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with kBuild.  If not, see <http://www.gnu.org/licenses/>
 *
 */


/*********************************************************************************************************************************
*   Header Files                                                                                                                 *
*********************************************************************************************************************************/
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#ifdef HAVE_ALLOCA_H
# include <alloca.h>
#endif
#if !defined(_MSC_VER)
# include <unistd.h>
#else
# include <io.h>
#endif
#include "k/kDefs.h"
#include "k/kTypes.h"
#include "kDep.h"
#include "err.h"
#include "kmkbuiltin.h"


/*********************************************************************************************************************************
*   Defined Constants And Macros                                                                                                 *
*********************************************************************************************************************************/
/*#define DEBUG*/
#ifdef DEBUG
# define dprintf(a)             printf a
# define dump(pb, cb, offBase)  depHexDump(pb,cb,offBase)
#else
# define dprintf(a)             do {} while (0)
# define dump(pb, cb, offBase)  do {} while (0)
#endif

/*********************************************************************************************************************************
*   Structures and Typedefs                                                                                                      *
*********************************************************************************************************************************/
typedef struct KDEPIDBGLOBALS
{
    PKMKBUILTINCTX pCtx;
    DEPGLOBALS  Core;
} KDEPIDBGLOBALS;
typedef KDEPIDBGLOBALS *PKDEPIDBGLOBALS;


/**
 * Scans a stream (chunk of data really) for dependencies.
 *
 * @returns 0 on success.
 * @returns !0 on failure.
 * @param   pThis           The kDepIDB instance.
 * @param   pbStream        The stream bits.
 * @param   cbStream        The size of the stream.
 * @param   pszPrefix       The dependency prefix.
 * @param   cchPrefix       The size of the prefix.
 */
static int ScanStream(PKDEPIDBGLOBALS pThis, KU8 *pbStream, size_t cbStream, const char *pszPrefix, size_t cchPrefix)
{
    const KU8      *pbCur = pbStream;
    size_t          cbLeft = cbStream;
    register char   chFirst = *pszPrefix;
    while (cbLeft > cchPrefix + 2)
    {
        if (    *pbCur != chFirst
            ||  memcmp(pbCur, pszPrefix, cchPrefix))
        {
            pbCur++;
            cbLeft--;
        }
        else
        {
            size_t cchDep;
            pbCur += cchPrefix;
            cchDep = strlen((const char *)pbCur);
            depAdd(&pThis->Core, (const char *) pbCur, cchDep);
            dprintf(("%05x: '%s'\n", pbCur - pbStream, pbCur));

            pbCur += cchDep;
            cbLeft -= cchDep + cchPrefix;
        }
    }

    return 0;
}


/*/////////////////////////////////////////////////////////////////////////////
//
//
//  P D B   7 . 0
//
//
/////////////////////////////////////////////////////////////////////////////*/

/** A PDB 7.0 Page number. */
typedef KU32 PDB70PAGE;
/** Pointer to a PDB 7.0 Page number. */
typedef PDB70PAGE *PPDB70PAGE;

/**
 * A PDB 7.0 stream.
 */
typedef struct PDB70STREAM
{
    /** The size of the stream. */
    KU32        cbStream;
} PDB70STREAM, *PPDB70STREAM;


/** The PDB 7.00 signature. */
#define PDB_SIGNATURE_700 "Microsoft C/C++ MSF 7.00\r\n\x1A" "DS\0\0"
/**
 * The PDB 7.0 header.
 */
typedef struct PDB70HDR
{
    /** The signature string. */
    KU8         szSignature[sizeof(PDB_SIGNATURE_700)];
    /** The page size. */
    KU32        cbPage;
    /** The start page. */
    PDB70PAGE   iStartPage;
    /** The number of pages in the file. */
    PDB70PAGE   cPages;
    /** The root stream directory. */
    KU32        cbRoot;
    /** Unknown function, always 0. */
    KU32        u32Reserved;
    /** The page index of the root page table. */
    PDB70PAGE   iRootPages;
} PDB70HDR, *PPDB70HDR;

/**
 * The PDB 7.0 root directory.
 */
typedef struct PDB70ROOT
{
    /** The number of streams */
    KU32        cStreams;
    /** Array of streams. */
    PDB70STREAM aStreams[1];
    /* KU32 aiPages[] */
} PDB70ROOT, *PPDB70ROOT;

/**
 * The PDB 7.0 name stream (#1) header.
 */
typedef struct PDB70NAMES
{
    /** The structure version. */
    KU32            Version;
    /** Timestamp.  */
    KU32            TimeStamp;
    /** Unknown. */
    KU32            Unknown1;
    /** GUID. */
    KU32            u32Guid[4];
    /** The size of the following name table. */
    KU32            cbNames;
    /** The name table. */
    char            szzNames[1];
} PDB70NAMES, *PPDB70NAMES;

/** The version / magic of the names structure. */
#define PDB70NAMES_VERSION  20000404


static int Pdb70ValidateHeader(PKDEPIDBGLOBALS pThis, PPDB70HDR pHdr, size_t cbFile)
{
    if (pHdr->cbPage * pHdr->cPages != cbFile)
        return errx(pThis->pCtx, 1, "Bad PDB 2.0 header - cbPage * cPages != cbFile.");
    if (pHdr->iStartPage >= pHdr->cPages && pHdr->iStartPage <= 0)
        return errx(pThis->pCtx, 1, "Bad PDB 2.0 header - iStartPage=%u cPages=%u.",
                    pHdr->iStartPage, pHdr->cPages);
    if (pHdr->iRootPages >= pHdr->cPages && pHdr->iRootPages <= 0)
        return errx(pThis->pCtx, 1, "Bad PDB 2.0 header - iRootPages=%u cPage=%u.",
                    pHdr->iStartPage, pHdr->cPages);
    return 0;
}

#ifdef DEBUG
static size_t Pdb70Align(PPDB70HDR pHdr, size_t cb)
{
    if (cb == ~(KU32)0 || !cb)
        return 0;
    return ((cb + pHdr->cbPage - 1) / pHdr->cbPage) * pHdr->cbPage;
}
#endif /* DEBUG */

static size_t Pdb70Pages(PPDB70HDR pHdr, size_t cb)
{
    if (cb == ~(KU32)0 || !cb)
        return 0;
    return (cb + pHdr->cbPage - 1) / pHdr->cbPage;
}

static void *Pdb70AllocAndRead(PKDEPIDBGLOBALS pThis, PPDB70HDR pHdr, size_t cb, PPDB70PAGE paiPageMap)
{
    const size_t    cbPage = pHdr->cbPage;
    size_t          cPages = Pdb70Pages(pHdr, cb);
    KU8            *pbBuf = malloc(cPages * cbPage + 1);
    if (pbBuf)
    {
        size_t iPage = 0;
        while (iPage < cPages)
        {
            size_t off = paiPageMap[iPage];
            if (off < pHdr->cPages)
            {
                off *= cbPage;
                memcpy(pbBuf + iPage * cbPage, (KU8 *)pHdr + off, cbPage);
                dump(pbBuf + iPage * cbPage, iPage + 1 < cPages ? cbPage : cb % cbPage, off);
            }
            else
            {
                warnx(pThis->pCtx, "warning: Invalid page index %u (max %u)!\n", (unsigned)off, pHdr->cPages);
                memset(pbBuf + iPage * cbPage, 0, cbPage);
            }

            iPage++;
        }
        pbBuf[cPages * cbPage] = '\0';
    }
    else
    {
        errx(pThis->pCtx, 1, "failed to allocate %lu bytes", (unsigned long)(cPages * cbPage + 1));
        return NULL;
    }
    return pbBuf;
}

static PPDB70ROOT Pdb70AllocAndReadRoot(PKDEPIDBGLOBALS pThis, PPDB70HDR pHdr)
{
    /*
     * The tricky bit here is to find the right length. Really?
     * (Todo: Check if we can just use the stream #0 size..)
     */
    PPDB70PAGE piPageMap = (KU32 *)((KU8 *)pHdr + pHdr->iRootPages * pHdr->cbPage);
    PPDB70ROOT pRoot = Pdb70AllocAndRead(pThis, pHdr, pHdr->cbRoot, piPageMap);
    if (pRoot)
    {
#if 1
        /* This stuff is probably unnecessary: */
        /* size = stream header + array of stream. */
        size_t cb = K_OFFSETOF(PDB70ROOT, aStreams[pRoot->cStreams]);
        free(pRoot);
        pRoot = Pdb70AllocAndRead(pThis, pHdr, cb, piPageMap);
        if (pRoot)
        {
            /* size += page tables. */
            unsigned iStream = pRoot->cStreams;
            while (iStream-- > 0)
                if (pRoot->aStreams[iStream].cbStream != ~(KU32)0)
                    cb += Pdb70Pages(pHdr, pRoot->aStreams[iStream].cbStream) * sizeof(PDB70PAGE);
            free(pRoot);
            pRoot = Pdb70AllocAndRead(pThis, pHdr, cb, piPageMap);
            if (pRoot)
            {
                /* validate? */
                return pRoot;
            }
        }
#else
        /* validate? */
        return pRoot;
#endif
    }
    return NULL;
}

static void *Pdb70AllocAndReadStream(PKDEPIDBGLOBALS pThis, PPDB70HDR pHdr, PPDB70ROOT pRoot, unsigned iStream, size_t *pcbStream)
{
    const size_t    cbStream = pRoot->aStreams[iStream].cbStream;
    PPDB70PAGE      paiPageMap;
    if (    iStream >= pRoot->cStreams
        ||  cbStream == ~(KU32)0)
    {
        errx(pThis->pCtx, 1, "Invalid stream %d", iStream);
        return NULL;
    }

    paiPageMap = (PPDB70PAGE)&pRoot->aStreams[pRoot->cStreams];
    while (iStream-- > 0)
        if (pRoot->aStreams[iStream].cbStream != ~(KU32)0)
            paiPageMap += Pdb70Pages(pHdr, pRoot->aStreams[iStream].cbStream);

    if (pcbStream)
        *pcbStream = cbStream;
    return Pdb70AllocAndRead(pThis, pHdr, cbStream, paiPageMap);
}

static int Pdb70Process(PKDEPIDBGLOBALS pThis, KU8 *pbFile, size_t cbFile)
{
    PPDB70HDR   pHdr = (PPDB70HDR)pbFile;
    PPDB70ROOT  pRoot;
    PPDB70NAMES pNames;
    size_t      cbStream = 0;
    unsigned    fDone = 0;
    unsigned    iStream;
    int         rc = 0;
    dprintf(("pdb70\n"));

    /*
     * Validate the header and read the root stream.
     */
    if (Pdb70ValidateHeader(pThis, pHdr, cbFile))
        return 1;
    pRoot = Pdb70AllocAndReadRoot(pThis, pHdr);
    if (!pRoot)
        return 1;

    /*
     * The names we want are usually all found in the 'Names' stream, that is #1.
     */
    dprintf(("Reading the names stream....\n"));
    pNames = Pdb70AllocAndReadStream(pThis, pHdr, pRoot, 1, &cbStream);
    if (pNames)
    {
        dprintf(("Names: Version=%u cbNames=%u (%#x)\n", pNames->Version, pNames->cbNames, pNames->cbNames));
        if (    pNames->Version == PDB70NAMES_VERSION
            &&  pNames->cbNames > 32
            &&  pNames->cbNames + K_OFFSETOF(PDB70NAMES, szzNames) <= pRoot->aStreams[1].cbStream)
        {
            /*
             * Iterate the names and add the /mr/inversedeps/ ones to the dependency list.
             */
            const char *psz = &pNames->szzNames[0];
            size_t cb = pNames->cbNames;
            size_t off = 0;
            dprintf(("0x0000 #0: %6d bytes  [root / toc]\n", pRoot->aStreams[0].cbStream));
            for (iStream = 1; cb > 0; iStream++)
            {
                int fAdded = 0;
                size_t cch = strlen(psz);
                if (   cch >= sizeof("/mr/inversedeps/")
                    && !memcmp(psz, "/mr/inversedeps/", sizeof("/mr/inversedeps/") - 1))
                {
                    depAdd(&pThis->Core, psz + sizeof("/mr/inversedeps/") - 1, cch - (sizeof("/mr/inversedeps/") - 1));
                    fAdded = 1;
                }
                dprintf(("%#06x #%d: %6d bytes  %s%s\n", off, iStream,
                         iStream < pRoot->cStreams ? pRoot->aStreams[iStream].cbStream : -1,
                         psz, fAdded ? "  [dep]" : ""));
                (void)fAdded;

                /* next */
                if (cch >= cb)
                {
                    dprintf(("warning! cch=%d cb=%d\n", cch, cb));
                    cch = cb - 1;
                }
                cb  -= cch + 1;
                psz += cch + 1;
                off += cch + 1;
            }
            rc = 0;
            fDone = 1;
        }
        else
            dprintf(("Unknown version or bad size: Version=%u cbNames=%d cbStream=%d\n",
                     pNames->Version, pNames->cbNames, cbStream));
        free(pNames);
    }

    if (!fDone)
    {
        /*
         * Iterate the streams in the root and scan their content for
         * dependencies.
         */
        rc = 0;
        for (iStream = 0; iStream < pRoot->cStreams && !rc; iStream++)
        {
            KU8 *pbStream;
            if (    pRoot->aStreams[iStream].cbStream == ~(KU32)0
                ||  !pRoot->aStreams[iStream].cbStream)
                continue;
            dprintf(("Stream #%d: %#x bytes (%#x aligned)\n", iStream, pRoot->aStreams[iStream].cbStream,
                     Pdb70Align(pHdr, pRoot->aStreams[iStream].cbStream)));
            pbStream = (KU8 *)Pdb70AllocAndReadStream(pThis, pHdr, pRoot, iStream, &cbStream);
            if (pbStream)
            {
                rc = ScanStream(pThis, pbStream, cbStream, "/mr/inversedeps/", sizeof("/mr/inversedeps/") - 1);
                free(pbStream);
            }
            else
                rc = 1;
        }
    }

    free(pRoot);
    return rc;
}



/*/////////////////////////////////////////////////////////////////////////////
//
//
//  P D B   2 . 0
//
//
/////////////////////////////////////////////////////////////////////////////*/


/** A PDB 2.0 Page number. */
typedef KU16 PDB20PAGE;
/** Pointer to a PDB 2.0 Page number. */
typedef PDB20PAGE *PPDB20PAGE;

/**
 * A PDB 2.0 stream.
 */
typedef struct PDB20STREAM
{
    /** The size of the stream. */
    KU32        cbStream;
    /** Some unknown value. */
    KU32        u32Unknown;
} PDB20STREAM, *PPDB20STREAM;

/** The PDB 2.00 signature. */
#define PDB_SIGNATURE_200 "Microsoft C/C++ program database 2.00\r\n\x1A" "JG\0"
/**
 * The PDB 2.0 header.
 */
typedef struct PDB20HDR
{
    /** The signature string. */
    KU8         szSignature[sizeof(PDB_SIGNATURE_200)];
    /** The page size. */
    KU32        cbPage;
    /** The start page - whatever that is... */
    PDB20PAGE   iStartPage;
    /** The number of pages in the file. */
    PDB20PAGE   cPages;
    /** The root stream directory. */
    PDB20STREAM RootStream;
    /** The root page table. */
    PDB20PAGE   aiRootPageMap[1];
} PDB20HDR, *PPDB20HDR;

/**
 * The PDB 2.0 root directory.
 */
typedef struct PDB20ROOT
{
    /** The number of streams */
    KU16        cStreams;
    /** Reserved or high part of cStreams. */
    KU16        u16Reserved;
    /** Array of streams. */
    PDB20STREAM aStreams[1];
} PDB20ROOT, *PPDB20ROOT;


static int Pdb20ValidateHeader(PKDEPIDBGLOBALS pThis, PPDB20HDR pHdr, size_t cbFile)
{
    if (pHdr->cbPage * pHdr->cPages != cbFile)
        return errx(pThis->pCtx, 1, "Bad PDB 2.0 header - cbPage * cPages != cbFile.");
    if (pHdr->iStartPage >= pHdr->cPages && pHdr->iStartPage <= 0)
        return errx(pThis->pCtx, 1, "Bad PDB 2.0 header - cbPage * cPages != cbFile.");
    return 0;
}

static size_t Pdb20Pages(PPDB20HDR pHdr, size_t cb)
{
    if (cb == ~(KU32)0 || !cb)
        return 0;
    return (cb + pHdr->cbPage - 1) / pHdr->cbPage;
}

static void *Pdb20AllocAndRead(PKDEPIDBGLOBALS pThis, PPDB20HDR pHdr, size_t cb, PPDB20PAGE paiPageMap)
{
    size_t cPages = Pdb20Pages(pHdr, cb);
    KU8   *pbBuf = malloc(cPages * pHdr->cbPage + 1);
    if (pbBuf)
    {
        size_t iPage = 0;
        while (iPage < cPages)
        {
            size_t off = paiPageMap[iPage];
            off *= pHdr->cbPage;
            memcpy(pbBuf + iPage * pHdr->cbPage, (KU8 *)pHdr + off, pHdr->cbPage);
            iPage++;
        }
        pbBuf[cPages * pHdr->cbPage] = '\0';
    }
    else
        errx(pThis->pCtx, 1, "failed to allocate %lu bytes", (unsigned long)(cPages * pHdr->cbPage + 1));
    return pbBuf;
}

static PPDB20ROOT Pdb20AllocAndReadRoot(PKDEPIDBGLOBALS pThis, PPDB20HDR pHdr)
{
    /*
     * The tricky bit here is to find the right length.
     * (Todo: Check if we can just use the stream size..)
     */
    PPDB20ROOT pRoot = Pdb20AllocAndRead(pThis, pHdr, sizeof(*pRoot), &pHdr->aiRootPageMap[0]);
    if (pRoot)
    {
        /* size = stream header + array of stream. */
        size_t cb = K_OFFSETOF(PDB20ROOT, aStreams[pRoot->cStreams]);
        free(pRoot);
        pRoot = Pdb20AllocAndRead(pThis, pHdr, cb, &pHdr->aiRootPageMap[0]);
        if (pRoot)
        {
            /* size += page tables. */
            unsigned iStream = pRoot->cStreams;
            while (iStream-- > 0)
                if (pRoot->aStreams[iStream].cbStream != ~(KU32)0)
                    cb += Pdb20Pages(pHdr, pRoot->aStreams[iStream].cbStream) * sizeof(PDB20PAGE);
            free(pRoot);
            pRoot = Pdb20AllocAndRead(pThis, pHdr, cb, &pHdr->aiRootPageMap[0]);
            if (pRoot)
            {
                /* validate? */
                return pRoot;
            }
        }
    }
    return NULL;

}

static void *Pdb20AllocAndReadStream(PKDEPIDBGLOBALS pThis, PPDB20HDR pHdr, PPDB20ROOT pRoot, unsigned iStream, size_t *pcbStream)
{
    size_t      cbStream = pRoot->aStreams[iStream].cbStream;
    PPDB20PAGE  paiPageMap;
    if (    iStream >= pRoot->cStreams
        ||  cbStream == ~(KU32)0)
    {
        errx(pThis->pCtx, 1, "Invalid stream %d", iStream);
        return NULL;
    }

    paiPageMap = (PPDB20PAGE)&pRoot->aStreams[pRoot->cStreams];
    while (iStream-- > 0)
        if (pRoot->aStreams[iStream].cbStream != ~(KU32)0)
            paiPageMap += Pdb20Pages(pHdr, pRoot->aStreams[iStream].cbStream);

    if (pcbStream)
        *pcbStream = cbStream;
    return Pdb20AllocAndRead(pThis, pHdr, cbStream, paiPageMap);
}

static int Pdb20Process(PKDEPIDBGLOBALS pThis, KU8 *pbFile, size_t cbFile)
{
    PPDB20HDR   pHdr = (PPDB20HDR)pbFile;
    PPDB20ROOT  pRoot;
    unsigned    iStream;
    int         rc = 0;

    /*
     * Validate the header and read the root stream.
     */
    if (Pdb20ValidateHeader(pThis, pHdr, cbFile))
        return 1;
    pRoot = Pdb20AllocAndReadRoot(pThis, pHdr);
    if (!pRoot)
        return 1;

    /*
     * Iterate the streams in the root and scan their content for
     * dependencies.
     */
    rc = 0;
    for (iStream = 0; iStream < pRoot->cStreams && !rc; iStream++)
    {
        KU8 *pbStream;
        if (pRoot->aStreams[iStream].cbStream == ~(KU32)0)
            continue;
        pbStream = (KU8 *)Pdb20AllocAndReadStream(pThis, pHdr, pRoot, iStream, NULL);
        if (pbStream)
        {
            rc = ScanStream(pThis, pbStream, pRoot->aStreams[iStream].cbStream, "/ipm/header/", sizeof("/ipm/header/") - 1);
            free(pbStream);
        }
        else
            rc = 1;
    }

    free(pRoot);
    return rc;
}


/**
 * Make an attempt at parsing a Visual C++ IDB file.
 */
static int ProcessIDB(PKDEPIDBGLOBALS pThis, FILE *pInput)
{
    size_t      cbFile;
    KU8        *pbFile;
    void       *pvOpaque;
    int         rc = 0;

    /*
     * Read the file into memory.
     */
    pbFile = (KU8 *)depReadFileIntoMemory(pInput, &cbFile, &pvOpaque);
    if (!pbFile)
        return 1;

    /*
     * Figure out which parser to use.
     */
    if (!memcmp(pbFile, PDB_SIGNATURE_700, sizeof(PDB_SIGNATURE_700)))
        rc = Pdb70Process(pThis, pbFile, cbFile);
    else if (!memcmp(pbFile, PDB_SIGNATURE_200, sizeof(PDB_SIGNATURE_200)))
        rc = Pdb20Process(pThis, pbFile, cbFile);
    else
        rc = errx(pThis->pCtx, 1, "Doesn't recognize the header of the Visual C++ IDB file.");

    depFreeFileMemory(pbFile, pvOpaque);
    return rc;
}


static void kDepIDBUsage(PKMKBUILTINCTX pCtx, int fIsErr)
{
    kmk_builtin_ctx_printf(pCtx, fIsErr,
                           "usage: %s -o <output> -t <target> [-fqs] <vc idb-file>\n"
                           "   or: %s --help\n"
                           "   or: %s --version\n",
                           pCtx->pszProgName, pCtx->pszProgName, pCtx->pszProgName);
}


int kmk_builtin_kDepIDB(int argc, char **argv, char **envp, PKMKBUILTINCTX pCtx)
{
    int             i;
    KDEPIDBGLOBALS  This;

    /* Arguments. */
    FILE       *pOutput = NULL;
    const char *pszOutput = NULL;
    FILE       *pInput = NULL;
    const char *pszTarget = NULL;
    int         fStubs = 0;
    int         fFixCase = 0;
    /* Argument parsing. */
    int         fInput = 0;             /* set when we've found input argument. */
    int         fQuiet = 0;

    /* Init the instance data. */
    This.pCtx = pCtx;

    /*
     * Parse arguments.
     */
    if (argc <= 1)
    {
        kDepIDBUsage(pCtx, 0);
        return 1;
    }
    for (i = 1; i < argc; i++)
    {
        if (argv[i][0] == '-')
        {
            const char *psz = &argv[i][1];
            if (*psz == '-')
            {
                if (!strcmp(psz, "-quiet"))
                    psz = "q";
                else if (!strcmp(psz, "-help"))
                    psz = "?";
                else if (!strcmp(psz, "-version"))
                    psz = "V";
            }

            switch (*psz)
            {
                /*
                 * Output file.
                 */
                case 'o':
                {
                    pszOutput = &argv[i][2];
                    if (pOutput)
                        return errx(pCtx, 2, "only one output file!");
                    if (!*pszOutput)
                    {
                        if (++i >= argc)
                            return errx(pCtx, 2, "The '-o' argument is missing the filename.");
                        pszOutput = argv[i];
                    }
                    if (pszOutput[0] == '-' && !pszOutput[1])
                        pOutput = stdout;
                    else
                        pOutput = fopen(pszOutput, "w" KMK_FOPEN_NO_INHERIT_MODE);
                    if (!pOutput)
                        return err(pCtx, 1, "Failed to create output file '%s'", pszOutput);
                    break;
                }

                /*
                 * Target name.
                 */
                case 't':
                {
                    if (pszTarget)
                        return errx(pCtx, 2, "only one target!");
                    pszTarget = &argv[i][2];
                    if (!*pszTarget)
                    {
                        if (++i >= argc)
                            return errx(pCtx, 2, "The '-t' argument is missing the target name.");
                        pszTarget = argv[i];
                    }
                    break;
                }

                /*
                 * Fix case.
                 */
                case 'f':
                {
                    fFixCase = 1;
                    break;
                }

                /*
                 * Quiet.
                 */
                case 'q':
                {
                    fQuiet = 1;
                    break;
                }

                /*
                 * Generate stubs.
                 */
                case 's':
                {
                    fStubs = 1;
                    break;
                }

                /*
                 * The mandatory version & help.
                 */
                case '?':
                    kDepIDBUsage(pCtx, 0);
                    return 0;
                case 'V':
                case 'v':
                    return kbuild_version(pCtx->pszProgName);

                /*
                 * Invalid argument.
                 */
                default:
                    errx(pCtx, 2, "Invalid argument '%s.'", argv[i]);
                    kDepIDBUsage(pCtx, 1);
                    return 2;
            }
        }
        else
        {
            pInput = fopen(argv[i], "rb" KMK_FOPEN_NO_INHERIT_MODE);
            if (!pInput)
                return err(pCtx, 1, "Failed to open input file '%s'", argv[i]);
            fInput = 1;
        }

        /*
         * End of the line?
         */
        if (fInput)
        {
            if (++i < argc)
                return errx(pCtx, 2, "No arguments shall follow the input spec.");
            break;
        }
    }

    /*
     * Got all we require?
     */
    if (!pInput)
        return errx(pCtx, 2, "No input!");
    if (!pOutput)
        return errx(pCtx, 2, "No output!");
    if (!pszTarget)
        return errx(pCtx, 2, "No target!");

    /*
     * Do the parsing.
     */
    depInit(&This.Core);
    i = ProcessIDB(&This, pInput);
    fclose(pInput);

    /*
     * Write the dependecy file.
     */
    if (!i)
    {
        depOptimize(&This.Core, fFixCase, fQuiet, NULL /*pszIgnoredExt*/);
        depPrintTargetWithDeps(&This.Core, pOutput, pszTarget, 1 /*fEscapeTarget*/);
        if (fStubs)
            depPrintStubs(&This.Core, pOutput);
    }

    /*
     * Close the output, delete output on failure.
     */
    if (!i && ferror(pOutput))
        i = errx(pCtx, 1, "Error writing to '%s'.", pszOutput);
    fclose(pOutput);
    if (i)
    {
        if (unlink(pszOutput))
            warnx(pCtx, "warning: failed to remove output file '%s' on failure.", pszOutput);
    }

    depCleanup(&This.Core);
    return i;
}

#ifdef KMK_BUILTIN_STANDALONE
int main(int argc, char **argv, char **envp)
{
    KMKBUILTINCTX Ctx = { "kmk_kDepIDB", NULL };
    return kmk_builtin_kDepIDB(argc, argv, envp, &Ctx);
}
#endif

