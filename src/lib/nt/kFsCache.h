/* $Id: kFsCache.h 3199 2018-03-28 18:56:21Z bird $ */
/** @file
 * kFsCache.c - NT directory content cache.
 */

/*
 * Copyright (c) 2016 knut st. osmundsen <bird-kBuild-spamx@anduin.net>
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

#ifndef ___lib_nt_kFsCache_h___
#define ___lib_nt_kFsCache_h___


#include <k/kHlp.h>
#include "ntstat.h"
#ifndef NDEBUG
# include <stdarg.h>
#endif


/** @def KFSCACHE_CFG_UTF16
 * Whether to compile in the UTF-16 names support. */
#define KFSCACHE_CFG_UTF16                  1
/** @def KFSCACHE_CFG_SHORT_NAMES
 * Whether to compile in the short name support. */
#define KFSCACHE_CFG_SHORT_NAMES            1
/** @def KFSCACHE_CFG_PATH_HASH_TAB_SIZE
 * Size of the path hash table. */
#define KFSCACHE_CFG_PATH_HASH_TAB_SIZE     99991
/** The max length paths we consider. */
#define KFSCACHE_CFG_MAX_PATH               1024
/** The max ANSI name length. */
#define KFSCACHE_CFG_MAX_ANSI_NAME          (256*3 + 16)
/** The max UTF-16 name length. */
#define KFSCACHE_CFG_MAX_UTF16_NAME         (256*2 + 16)
/** Enables locking of the cache and thereby making it thread safe. */
#define KFSCACHE_CFG_LOCKING                1



/** Special KFSOBJ::uCacheGen number indicating that it does not apply. */
#define KFSOBJ_CACHE_GEN_IGNORE             KU32_MAX


/** @name KFSOBJ_TYPE_XXX - KFSOBJ::bObjType
 * @{  */
/** Directory, type KFSDIR. */
#define KFSOBJ_TYPE_DIR         KU8_C(0x01)
/** Regular file - type KFSOBJ. */
#define KFSOBJ_TYPE_FILE        KU8_C(0x02)
/** Other file - type KFSOBJ. */
#define KFSOBJ_TYPE_OTHER       KU8_C(0x03)
/** Caching of a negative result - type KFSOBJ.
 * @remarks We will allocate enough space for the largest cache node, so this
 *          can metamorph into any other object should it actually turn up.  */
#define KFSOBJ_TYPE_MISSING     KU8_C(0x04)
///** Invalidated entry flag. */
//#define KFSOBJ_TYPE_F_INVALID   KU8_C(0x20)
/** @} */

/** @name KFSOBJ_F_XXX - KFSOBJ::fFlags
 * @{ */
 /** Use custom generation.
  * @remarks This is given the value 1, as we use it as an index into
  *          KFSCACHE::auGenerations, 0 being the default. */
#define KFSOBJ_F_USE_CUSTOM_GEN         KU32_C(0x00000001)

/** Whether the file system update the modified timestamp of directories
 * when something is removed from it or added to it.
 * @remarks They say NTFS is the only windows filesystem doing this.  */
#define KFSOBJ_F_WORKING_DIR_MTIME      KU32_C(0x00000002)
/** NTFS file system volume. */
#define KFSOBJ_F_NTFS                   KU32_C(0x80000000)
/** Flags that are automatically inherited. */
#define KFSOBJ_F_INHERITED_MASK         KU32_C(0xffffffff)
/** @} */


#define IS_ALPHA(ch) ( ((ch) >= 'A' && (ch) <= 'Z') || ((ch) >= 'a' && (ch) <= 'z') )
#define IS_SLASH(ch) ((ch) == '\\' || (ch) == '/')




/** Pointer to a cache. */
typedef struct KFSCACHE *PKFSCACHE;
/** Pointer to a core object.  */
typedef struct KFSOBJ *PKFSOBJ;
/** Pointer to a directory object.  */
typedef struct KFSDIR *PKFSDIR;
/** Pointer to a directory hash table entry. */
typedef struct KFSOBJHASH *PKFSOBJHASH;



/** Pointer to a user data item. */
typedef struct KFSUSERDATA *PKFSUSERDATA;
/**
 * User data item associated with a cache node.
 */
typedef struct KFSUSERDATA
{
    /** Pointer to the next piece of user data. */
    PKFSUSERDATA    pNext;
    /** The key identifying this user. */
    KUPTR           uKey;
    /** The destructor. */
    void (*pfnDestructor)(PKFSCACHE pCache, PKFSOBJ pObj, PKFSUSERDATA pData);
} KFSUSERDATA;


/**
 * Storage for name strings for the unlikely event that they should grow in
 * length after the KFSOBJ was created.
 */
typedef struct KFSOBJNAMEALLOC
{
    /** Size of the allocation. */
    KU32        cb;
    /** The space for names. */
    char        abSpace[1];
} KFSOBJNAMEALLOC;
/** Name growth allocation. */
typedef KFSOBJNAMEALLOC *PKFSOBJNAMEALLOC;


/**
 * Base cache node.
 */
typedef struct KFSOBJ
{
    /** Magic value (KFSOBJ_MAGIC). */
    KU32                u32Magic;
    /** Number of references. */
    KU32 volatile       cRefs;
    /** The cache generation, see KFSOBJ_CACHE_GEN_IGNORE. */
    KU32                uCacheGen;
    /** The object type, KFSOBJ_TYPE_XXX.   */
    KU8                 bObjType;
    /** Set if the Stats member is valid, clear if not. */
    KBOOL               fHaveStats;
    /** Unused flags. */
    KBOOL               abUnused[2];
    /** Flags, KFSOBJ_F_XXX. */
    KU32                fFlags;

    /** Hash value of the name inserted into the parent hash table.
     * This is 0 if not inserted.  Names are only hashed and inserted as they are
     * first found thru linear searching of its siblings, and which name it is
     * dependens on the lookup function (W or A) and whether the normal name or
     * short name seems to have matched.
     *
     * @note It was ruled out as too much work to hash and track all four names,
     *       so instead this minimalist approach was choosen in stead. */
    KU32                uNameHash;
    /** Pointer to the next child with the same name hash value. */
    PKFSOBJ             pNextNameHash;
    /** Pointer to the parent (directory).
     * This is only NULL for a root. */
    PKFSDIR             pParent;

    /** The directory name.  (Allocated after the structure.) */
    const char         *pszName;
    /** The length of pszName. */
    KU16                cchName;
    /** The length of the parent path (up to where pszName starts).
     * @note This is valuable when constructing an absolute path to this node by
     *       means of the parent pointer (no need for recursion). */
    KU16                cchParent;
#ifdef KFSCACHE_CFG_UTF16
    /** The length of pwszName (in wchar_t's). */
    KU16                cwcName;
    /** The length of the parent UTF-16 path (in wchar_t's).
     * @note This is valuable when constructing an absolute path to this node by
     *       means of the parent pointer (no need for recursion). */
    KU16                cwcParent;
    /** The UTF-16 object name.  (Allocated after the structure.) */
    const wchar_t      *pwszName;
#endif

#ifdef KFSCACHE_CFG_SHORT_NAMES
    /** The short object name.  (Allocated after the structure, could be same
     *  as pszName.) */
    const char         *pszShortName;
    /** The length of pszShortName. */
    KU16                cchShortName;
    /** The length of the short parent path (up to where pszShortName starts). */
    KU16                cchShortParent;
# ifdef KFSCACHE_CFG_UTF16
    /** The length of pwszShortName (in wchar_t's). */
    KU16                cwcShortName;
    /** The length of the short parent UTF-16 path (in wchar_t's). */
    KU16                cwcShortParent;
    /** The UTF-16 short object name.  (Allocated after the structure, possibly
     *  same as pwszName.) */
    const wchar_t      *pwszShortName;
# endif
#endif

    /** Allocation for handling name length increases. */
    PKFSOBJNAMEALLOC    pNameAlloc;

    /** Pointer to the first user data item */
    PKFSUSERDATA        pUserDataHead;

    /** Stats - only valid when fHaveStats is set. */
    BirdStat_T          Stats;
} KFSOBJ;

/** The magic for a KFSOBJ structure (Thelonious Sphere Monk). */
#define KFSOBJ_MAGIC                KU32_C(0x19171010)


/**
 * Directory node in the cache.
 */
typedef struct KFSDIR
{
    /** The core object information. */
    KFSOBJ             Obj;

    /** Child objects. */
    PKFSOBJ            *papChildren;
    /** The number of child objects. */
    KU32                cChildren;
    /** The allocated size of papChildren. */
    KU32                cChildrenAllocated;

    /** Pointer to the child hash table. */
    PKFSOBJ            *papHashTab;
    /** The mask shift of the hash table.
     * Hash table size is a power of two, this is the size minus one.
     *
     * @remarks The hash table is optional and populated by lookup hits.  The
     *          assumption being that a lookup is repeated and will choose a good
     *          name to hash on.  We've got up to 4 different hashes, so this
     *          was the easy way out. */
    KU32                fHashTabMask;

    /** Handle to the directory (we generally keep it open). */
#ifndef DECLARE_HANDLE
    KUPTR               hDir;
#else
    HANDLE              hDir;
#endif
    /** The device number we queried/inherited when opening it. */
    KU64                uDevNo;

    /** The last write time sampled the last time the directory was refreshed.
     * @remarks May differ from st_mtim because it will be updated when the
     *          parent directory is refreshed. */
    KI64                iLastWrite;

    /** Set if populated. */
    KBOOL               fPopulated;
    /** Set if it needs re-populated. */
    KBOOL               fNeedRePopulating;
} KFSDIR;


/**
 * Lookup errors.
 */
typedef enum KFSLOOKUPERROR
{
    /** Lookup was a success. */
    KFSLOOKUPERROR_SUCCESS = 0,
    /** A path component was not found. */
    KFSLOOKUPERROR_PATH_COMP_NOT_FOUND,
    /** A path component is not a directory. */
    KFSLOOKUPERROR_PATH_COMP_NOT_DIR,
    /** The final path entry is not a directory (trailing slash). */
    KFSLOOKUPERROR_NOT_DIR,
    /** Not found. */
    KFSLOOKUPERROR_NOT_FOUND,
    /** The path is too long. */
    KFSLOOKUPERROR_PATH_TOO_LONG,
    /** Unsupported path type. */
    KFSLOOKUPERROR_UNSUPPORTED,
    /** We're out of memory. */
    KFSLOOKUPERROR_OUT_OF_MEMORY,

    /** Error opening directory. */
    KFSLOOKUPERROR_DIR_OPEN_ERROR,
    /** Error reading directory. */
    KFSLOOKUPERROR_DIR_READ_ERROR,
    /** UTF-16 to ANSI conversion error. */
    KFSLOOKUPERROR_ANSI_CONVERSION_ERROR,
    /** ANSI to UTF-16 conversion error. */
    KFSLOOKUPERROR_UTF16_CONVERSION_ERROR,
    /** Internal error. */
    KFSLOOKUPERROR_INTERNAL_ERROR
} KFSLOOKUPERROR;


/** Pointer to an ANSI path hash table entry. */
typedef struct KFSHASHA *PKFSHASHA;
/**
 * ANSI file system path hash table entry.
 * The path hash table allows us to skip parsing and walking a path.
 */
typedef struct KFSHASHA
{
    /** Next entry with the same hash table slot. */
    PKFSHASHA           pNext;
    /** Path hash value. */
    KU32                uHashPath;
    /** The path length. */
    KU16                cchPath;
    /** Set if aboslute path.   */
    KBOOL               fAbsolute;
    /** Index into KFSCACHE:auGenerationsMissing when pFsObj is NULL. */
    KU8                 idxMissingGen;
    /** The cache generation ID. */
    KU32                uCacheGen;
    /** The lookup error (when pFsObj is NULL). */
    KFSLOOKUPERROR      enmError;
    /** The path.  (Allocated after the structure.) */
    const char         *pszPath;
    /** Pointer to the matching FS object.
     * This is NULL for negative path entries? */
    PKFSOBJ             pFsObj;
} KFSHASHA;


#ifdef KFSCACHE_CFG_UTF16
/** Pointer to an UTF-16 path hash table entry. */
typedef struct KFSHASHW *PKFSHASHW;
/**
 * UTF-16 file system path hash table entry. The path hash table allows us
 * to skip parsing and walking a path.
 */
typedef struct KFSHASHW
{
    /** Next entry with the same hash table slot. */
    PKFSHASHW           pNext;
    /** Path hash value. */
    KU32                uHashPath;
    /** The path length (in wchar_t units). */
    KU16                cwcPath;
    /** Set if aboslute path.   */
    KBOOL               fAbsolute;
    /** Index into KFSCACHE:auGenerationsMissing when pFsObj is NULL. */
    KU8                 idxMissingGen;
    /** The cache generation ID. */
    KU32                uCacheGen;
    /** The lookup error (when pFsObj is NULL). */
    KFSLOOKUPERROR      enmError;
    /** The path.  (Allocated after the structure.) */
    const wchar_t      *pwszPath;
    /** Pointer to the matching FS object.
     * This is NULL for negative path entries? */
    PKFSOBJ             pFsObj;
} KFSHASHW;
#endif


/** @def KFSCACHE_LOCK
 *  Locks the cache exclusively. */
/** @def KFSCACHE_UNLOCK
 *  Counterpart to KFSCACHE_LOCK. */
#ifdef KFSCACHE_CFG_LOCKING
# define KFSCACHE_LOCK(a_pCache)        EnterCriticalSection(&(a_pCache)->u.CritSect)
# define KFSCACHE_UNLOCK(a_pCache)      LeaveCriticalSection(&(a_pCache)->u.CritSect)
#else
# define KFSCACHE_LOCK(a_pCache)        do { } while (0)
# define KFSCACHE_UNLOCK(a_pCache)      do { } while (0)
#endif


/** @name KFSCACHE_F_XXX
 * @{ */
/** Whether to cache missing directory entries (KFSOBJ_TYPE_MISSING). */
#define KFSCACHE_F_MISSING_OBJECTS  KU32_C(0x00000001)
/** Whether to cache missing paths. */
#define KFSCACHE_F_MISSING_PATHS    KU32_C(0x00000002)
/** @} */


/**
 * Directory cache instance.
 */
typedef struct KFSCACHE
{
    /** Magic value (KFSCACHE_MAGIC). */
    KU32                u32Magic;
    /** Cache flags. */
    KU32                fFlags;

    /** The default and custom cache generations for stuff that exists, indexed by
     *  KFSOBJ_F_USE_CUSTOM_GEN.
     *
     * The custom generation can be used to invalidate parts of the file system that
     * are known to be volatile without triggering refreshing of the more static
     * parts.  Like the 'out' directory in a kBuild setup or a 'TEMP' directory are
     * expected to change and you need to invalidate the caching of these frequently
     * to stay on top of things.  Whereas the sources, headers, compilers, sdk,
     * ddks, windows directory and such generally doesn't change all that often.
     */
    KU32                auGenerations[2];
    /** The current cache generation for missing objects, negative results, ++.
     * This comes with a custom variant too.  Indexed by KFSOBJ_F_USE_CUSTOM_GEN. */
    KU32                auGenerationsMissing[2];

    /** Number of cache objects. */
    KSIZE               cObjects;
    /** Memory occupied by the cache object structures. */
    KSIZE               cbObjects;
    /** Number of lookups. */
    KSIZE               cLookups;
    /** Number of hits in the path hash tables. */
    KSIZE               cPathHashHits;
    /** Number of hits walking the file system hierarchy. */
    KSIZE               cWalkHits;
    /** Number of child searches. */
    KSIZE               cChildSearches;
    /** Number of cChildLookups resolved thru hash table hits. */
    KSIZE               cChildHashHits;
    /** The number of child hash tables. */
    KSIZE               cChildHashTabs;
    /** The sum of all child hash table sizes. */
    KSIZE               cChildHashEntriesTotal;
    /** Number of children inserted into the hash tables. */
    KSIZE               cChildHashed;
    /** Number of collisions in the child hash tables. */
    KSIZE               cChildHashCollisions;
    /** Number times a object name changed. */
    KSIZE               cNameChanges;
    /** Number times a object name grew and needed KFSOBJNAMEALLOC.
     *  (Subset of cNameChanges) */
    KSIZE               cNameGrowths;

    /** The root directory. */
    KFSDIR              RootDir;

#ifdef KFSCACHE_CFG_LOCKING
    /** Critical section protecting the cache. */
    union
    {
# ifdef _WINBASE_
        CRITICAL_SECTION    CritSect;
# endif
        KU64                abPadding[2 * 4 + 4 * sizeof(void *)];
    } u;
#endif

    /** File system hash table for ANSI filename strings. */
    PKFSHASHA           apAnsiPaths[KFSCACHE_CFG_PATH_HASH_TAB_SIZE];
    /** Number of paths in the apAnsiPaths hash table. */
    KSIZE               cAnsiPaths;
    /** Number of collisions in the apAnsiPaths hash table. */
    KSIZE               cAnsiPathCollisions;
    /** Amount of memory used by the path entries. */
    KSIZE               cbAnsiPaths;

#ifdef KFSCACHE_CFG_UTF16
    /** Number of paths in the apUtf16Paths hash table. */
    KSIZE               cUtf16Paths;
    /** Number of collisions in the apUtf16Paths hash table. */
    KSIZE               cUtf16PathCollisions;
    /** Amount of memory used by the UTF-16 path entries. */
    KSIZE               cbUtf16Paths;
    /** File system hash table for UTF-16 filename strings. */
    PKFSHASHW           apUtf16Paths[KFSCACHE_CFG_PATH_HASH_TAB_SIZE];
#endif
} KFSCACHE;

/** Magic value for KFSCACHE::u32Magic (Jon Batiste).  */
#define KFSCACHE_MAGIC              KU32_C(0x19861111)


/** @def KW_LOG
 * Generic logging.
 * @param a     Argument list for kFsCacheDbgPrintf  */
#if 1 /*def NDEBUG - enable when needed! */
# define KFSCACHE_LOG(a) do { } while (0)
#else
# define KFSCACHE_LOG(a) kFsCacheDbgPrintf a
void        kFsCacheDbgPrintfV(const char *pszFormat, va_list va);
void        kFsCacheDbgPrintf(const char *pszFormat, ...);
#endif


KBOOL       kFsCacheDirEnsurePopuplated(PKFSCACHE pCache, PKFSDIR pDir, KFSLOOKUPERROR *penmError);
KBOOL       kFsCacheDirAddChild(PKFSCACHE pCache, PKFSDIR pParent, PKFSOBJ pChild, KFSLOOKUPERROR *penmError);
PKFSOBJ     kFsCacheCreateObject(PKFSCACHE pCache, PKFSDIR pParent,
                                 char const *pszName, KU16 cchName, wchar_t const *pwszName, KU16 cwcName,
#ifdef KFSCACHE_CFG_SHORT_NAMES
                                 char const *pszShortName, KU16 cchShortName, wchar_t const *pwszShortName, KU16 cwcShortName,
#endif
                                 KU8 bObjType, KFSLOOKUPERROR *penmError);
PKFSOBJ     kFsCacheCreateObjectW(PKFSCACHE pCache, PKFSDIR pParent, wchar_t const *pwszName, KU32 cwcName,
#ifdef KFSCACHE_CFG_SHORT_NAMES
                                  wchar_t const *pwszShortName, KU32 cwcShortName,
#endif
                                  KU8 bObjType, KFSLOOKUPERROR *penmError);
PKFSOBJ     kFsCacheLookupA(PKFSCACHE pCache, const char *pszPath, KFSLOOKUPERROR *penmError);
PKFSOBJ     kFsCacheLookupW(PKFSCACHE pCache, const wchar_t *pwszPath, KFSLOOKUPERROR *penmError);
PKFSOBJ     kFsCacheLookupRelativeToDirA(PKFSCACHE pCache, PKFSDIR pParent, const char *pszPath, KU32 cchPath, KU32 fFlags,
                                         KFSLOOKUPERROR *penmError, PKFSOBJ *ppLastAncestor);
PKFSOBJ     kFsCacheLookupRelativeToDirW(PKFSCACHE pCache, PKFSDIR pParent, const wchar_t *pwszPath, KU32 cwcPath, KU32 fFlags,
                                         KFSLOOKUPERROR *penmError, PKFSOBJ *ppLastAncestor);
PKFSOBJ     kFsCacheLookupWithLengthA(PKFSCACHE pCache, const char *pchPath, KSIZE cchPath, KFSLOOKUPERROR *penmError);
PKFSOBJ     kFsCacheLookupWithLengthW(PKFSCACHE pCache, const wchar_t *pwcPath, KSIZE cwcPath, KFSLOOKUPERROR *penmError);
PKFSOBJ     kFsCacheLookupNoMissingA(PKFSCACHE pCache, const char *pszPath, KFSLOOKUPERROR *penmError);
PKFSOBJ     kFsCacheLookupNoMissingW(PKFSCACHE pCache, const wchar_t *pwszPath, KFSLOOKUPERROR *penmError);

/** @name KFSCACHE_LOOKUP_F_XXX - lookup flags
 * @{ */
/** No inserting new cache entries.
 * This effectively prevent directories from being repopulated too.  */
#define KFSCACHE_LOOKUP_F_NO_INSERT     KU32_C(1)
/** No refreshing cache entries. */
#define KFSCACHE_LOOKUP_F_NO_REFRESH    KU32_C(2)
/** @} */

KU32        kFsCacheObjRelease(PKFSCACHE pCache, PKFSOBJ pObj);
KU32        kFsCacheObjReleaseTagged(PKFSCACHE pCache, PKFSOBJ pObj, const char *pszWhere);
#ifndef NDEBUG /* enable to debug object release. */
# define kFsCacheObjRelease(a_pCache, a_pObj) kFsCacheObjReleaseTagged(a_pCache, a_pObj, __FUNCTION__)
#endif
KU32        kFsCacheObjRetain(PKFSOBJ pObj);
PKFSUSERDATA kFsCacheObjAddUserData(PKFSCACHE pCache, PKFSOBJ pObj, KUPTR uKey, KSIZE cbUserData);
PKFSUSERDATA kFsCacheObjGetUserData(PKFSCACHE pCache, PKFSOBJ pObj, KUPTR uKey);
KBOOL       kFsCacheObjGetFullPathA(PKFSOBJ pObj, char *pszPath, KSIZE cbPath, char chSlash);
KBOOL       kFsCacheObjGetFullPathW(PKFSOBJ pObj, wchar_t *pwszPath, KSIZE cwcPath, wchar_t wcSlash);
KBOOL       kFsCacheObjGetFullShortPathA(PKFSOBJ pObj, char *pszPath, KSIZE cbPath, char chSlash);
KBOOL       kFsCacheObjGetFullShortPathW(PKFSOBJ pObj, wchar_t *pwszPath, KSIZE cwcPath, wchar_t wcSlash);

KBOOL       kFsCacheFileSimpleOpenReadClose(PKFSCACHE pCache, PKFSOBJ pFileObj, KU64 offStart, void *pvBuf, KSIZE cbToRead);

PKFSCACHE   kFsCacheCreate(KU32 fFlags);
void        kFsCacheDestroy(PKFSCACHE);
void        kFsCacheInvalidateMissing(PKFSCACHE pCache);
void        kFsCacheInvalidateAll(PKFSCACHE pCache);
void        kFsCacheInvalidateCustomMissing(PKFSCACHE pCache);
void        kFsCacheInvalidateCustomBoth(PKFSCACHE pCache);
KBOOL       kFsCacheSetupCustomRevisionForTree(PKFSCACHE pCache, PKFSOBJ pRoot);
KBOOL       kFsCacheInvalidateDeletedDirectoryA(PKFSCACHE pCache, const char *pszDir);

#endif
