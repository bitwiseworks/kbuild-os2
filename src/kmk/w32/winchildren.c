/* $Id: winchildren.c 3359 2020-06-05 16:17:17Z bird $ */
/** @file
 * Child process creation and management for kmk.
 */

/*
 * Copyright (c) 2018 knut st. osmundsen <bird-kBuild-spamx@anduin.net>
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

/* No GNU coding style here atm, convert if upstreamed. */

/** @page pg_win_children   Windows child process creation and managment
 *
 * This new implementation aims at addressing the following:
 *
 *      1. Speed up process creation by doing the expensive CreateProcess call
 *         in a worker thread.
 *
 *      2. No 64 process limit imposed by WaitForMultipleObjects.
 *
 *      3. Better distribute jobs among processor groups.
 *
 *      4. Offloading more expensive kmkbuiltin operations to worker threads,
 *         making the main thread focus on managing child processes.
 *
 *      5. Output synchronization using reusable pipes.
 *
 *
 * To be quite honest, the first item (CreateProcess expense) didn't occur to me
 * at first and was more of a sideeffect discovered along the way.  A test
 * rebuilding IPRT went from 4m52s to 3m19s on a 8 thread system.
 *
 * The 2nd and 3rd goals are related to newer build servers that have lots of
 * CPU threads and various Windows NT (aka NT OS/2 at the time) design choices
 * made in the late 1980ies.
 *
 * WaitForMultipleObjects does not support waiting for more than 64 objects,
 * unlike poll and select.  This is just something everyone ends up having to
 * work around in the end.
 *
 * Affinity masks are uintptr_t sized, so 64-bit hosts can only manage 64
 * processors and 32-bit only 32.  Workaround was introduced with Windows 7
 * (IIRC) and is called processor groups.  The CPU threads are grouped into 1 or
 * more groups of up to 64 processors.  Processes are generally scheduled to a
 * signle processor group at first, but threads may be changed to be scheduled
 * on different groups.  This code will try distribute children evenly among the
 * processor groups, using a very simple algorithm (see details in code).
 *
 *
 * @section sec_win_children_av     Remarks on Microsoft Defender and other AV
 *
 * Part of the motivation for writing this code was horrible CPU utilization on
 * a brand new AMD Threadripper 1950X system with lots of memory and SSDs,
 * running 64-bit Windows 10 build 16299.
 *
 * Turns out Microsoft defender adds some overhead to CreateProcess
 * and other stuff:
 *     - Old make with CreateProcess on main thread:
 *          - With runtime defender enabled: 14 min  6 seconds
 *          - With runtime defender disabled: 4 min 49 seconds
 *     - New make with CreateProcess on worker thread (this code):
 *          - With runtime defender enabled:  6 min 29 seconds
 *          - With runtime defender disabled: 4 min 36 seconds
 *          - With runtime defender disabled out dir only: 5 min 59 seconds
 *
 * See also kWorker / kSubmit for more bickering about AV & disk encryption.
 */


/*********************************************************************************************************************************
*   Header Files                                                                                                                 *
*********************************************************************************************************************************/
#include <Windows.h>
#include <Winternl.h>

#include "../makeint.h"
#include "../job.h"
#include "../filedef.h"
#include "../debug.h"
#include "../kmkbuiltin.h"
#include "winchildren.h"

#include <assert.h>
#include <process.h>
#include <intrin.h>

#include "nt/nt_child_inject_standard_handles.h"
#include "console.h"

#ifndef KMK_BUILTIN_STANDALONE
extern void kmk_cache_exec_image_w(const wchar_t *); /* imagecache.c */
#endif

/* Option values from main.c: */
extern const char *win_job_object_mode;
extern const char *win_job_object_name;
extern int         win_job_object_no_kill;


/*********************************************************************************************************************************
*   Defined Constants And Macros                                                                                                 *
*********************************************************************************************************************************/
#define MKWINCHILD_MAX_PATH     1024

#define MKWINCHILD_DO_SET_PROCESSOR_GROUP

/** Checks the UTF-16 environment variable pointed to is the PATH. */
#define IS_PATH_ENV_VAR(a_cwcVar, a_pwszVar) \
    (   (a_cwcVar) >= 5 \
     &&  (a_pwszVar)[4] == L'=' \
     && ((a_pwszVar)[0] == L'P' || (a_pwszVar)[0] == L'p') \
     && ((a_pwszVar)[1] == L'A' || (a_pwszVar)[1] == L'a') \
     && ((a_pwszVar)[2] == L'T' || (a_pwszVar)[2] == L't') \
     && ((a_pwszVar)[3] == L'H' || (a_pwszVar)[3] == L'h') )


/*********************************************************************************************************************************
*   Structures and Typedefs                                                                                                      *
*********************************************************************************************************************************/
/** Pointer to a childcare worker thread.   */
typedef struct WINCHILDCAREWORKER *PWINCHILDCAREWORKER;
/** Pointer to a windows child process. */
typedef struct WINCHILD *PWINCHILD;


/**
 * Child process type.
 */
typedef enum WINCHILDTYPE
{
    WINCHILDTYPE_INVALID = 0,
    /** Normal child process. */
    WINCHILDTYPE_PROCESS,
#ifdef KMK
    /** kmkbuiltin command. */
    WINCHILDTYPE_BUILT_IN,
    /** kmkbuiltin_append result write out. */
    WINCHILDTYPE_APPEND,
    /** kSubmit job. */
    WINCHILDTYPE_SUBMIT,
    /** kmk_redirect job. */
    WINCHILDTYPE_REDIRECT,
#endif
    /** End of valid child types. */
    WINCHILDTYPE_END
} WINCHILDTYPE;


/**
 * Windows child process.
 */
typedef struct WINCHILD
{
    /** Magic / eyecatcher (WINCHILD_MAGIC). */
    ULONG                   uMagic;
    /** Child type. */
    WINCHILDTYPE            enmType;
    /** Pointer to the next child process. */
    PWINCHILD               pNext;
    /** The pid for this child. */
    pid_t                   pid;
    /** The make child structure associated with this child. */
    struct child           *pMkChild;

    /** The process exit code. */
    int                     iExitCode;
    /** Kill signal, in case we or someone else killed it. */
    int                     iSignal;
    /** Set if core was dumped. */
    int                     fCoreDumped;
    /** Set if the a child process is a candidate for cl.exe where we supress
     * annoying source name output. */
    BOOL                    fProbableClExe;
    /** The worker executing this child. */
    PWINCHILDCAREWORKER     pWorker;

    /** Type specific data. */
    union
    {
        /** Data for WINCHILDTYPE_PROCESS.   */
        struct
        {
            /** Argument vector (single allocation, strings following array). */
            char          **papszArgs;
            /** Length of the argument strings. */
            size_t          cbArgsStrings;
            /** Environment vector.  Only a copy if fEnvIsCopy is set. */
            char          **papszEnv;
            /** If we made a copy of the environment, this is the size of the
             * strings and terminator string (not in array).  This is done to
             * speed up conversion, since MultiByteToWideChar can handle '\0'. */
            size_t          cbEnvStrings;
            /** The make shell to use (copy). */
            char           *pszShell;
            /** Handle to use for standard out. */
            HANDLE          hStdOut;
            /** Handle to use for standard out. */
            HANDLE          hStdErr;
            /** Whether to close hStdOut after creating the process.  */
            BOOL            fCloseStdOut;
            /** Whether to close hStdErr after creating the process.  */
            BOOL            fCloseStdErr;
            /** Whether to catch output from the process. */
            BOOL            fCatchOutput;

            /** Child process handle. */
            HANDLE          hProcess;
        } Process;

        /** Data for WINCHILDTYPE_BUILT_IN.   */
        struct
        {
            /** The built-in command. */
            PCKMKBUILTINENTRY pBuiltIn;
            /** Number of arguments. */
            int             cArgs;
            /** Argument vector (single allocation, strings following array). */
            char          **papszArgs;
            /** Environment vector.  Only a copy if fEnvIsCopy is set. */
            char          **papszEnv;
        } BuiltIn;

        /** Data for WINCHILDTYPE_APPEND.   */
        struct
        {
            /** The filename. */
            char           *pszFilename;
            /** How much to append. */
            size_t          cbAppend;
            /** What to append. */
            char           *pszAppend;
            /** Whether to truncate the file. */
            int             fTruncate;
        } Append;

        /** Data for WINCHILDTYPE_SUBMIT.   */
        struct
        {
            /** The event we're to wait on (hooked up to a pipe) */
            HANDLE          hEvent;
            /** Parameter for the cleanup callback. */
            void           *pvSubmitWorker;
            /** Standard output catching pipe. Optional. */
            PWINCCWPIPE     pStdOut;
            /** Standard error catching pipe. Optional. */
            PWINCCWPIPE     pStdErr;
        } Submit;

        /** Data for WINCHILDTYPE_REDIRECT.   */
        struct
        {
            /** Child process handle. */
            HANDLE          hProcess;
        } Redirect;
    } u;

} WINCHILD;
/** WINCHILD::uMagic value. */
#define WINCHILD_MAGIC      0xbabebabeU


/**
 * Data for a windows childcare worker thread.
 *
 * We use one worker thread per child, reusing the threads when possible.
 *
 * This setup helps avoid the 64-bit handle with the WaitForMultipleObject API.
 *
 * It also helps using all CPUs on systems with more than one CPU group
 * (typically systems with more than 64 CPU threads or/and multiple sockets, or
 * special configs).
 *
 * This helps facilitates using pipes for collecting output child rather
 * than temporary files.  Pipes doesn't involve NTFS and can easily be reused.
 *
 * Finally, kBuild specific, this allows running kmkbuiltin_xxxx commands in
 * threads.
 */
typedef struct WINCHILDCAREWORKER
{
    /** Magic / eyecatcher (WINCHILDCAREWORKER_MAGIC). */
    ULONG                   uMagic;
    /** The worker index. */
    unsigned int            idxWorker;
    /** The processor group for this worker. */
    unsigned int            iProcessorGroup;
    /** The thread ID. */
    unsigned int            tid;
    /** The thread handle. */
    HANDLE                  hThread;
    /** The event the thread is idling on. */
    HANDLE                  hEvtIdle;
    /** The pipe catching standard output from a child. */
    PWINCCWPIPE             pStdOut;
    /** The pipe catching standard error from a child. */
    PWINCCWPIPE             pStdErr;

    /** Pointer to the current child. */
    PWINCHILD volatile      pCurChild;
    /** List of children pending execution on this worker.
     * This is updated atomitically just like g_pTailCompletedChildren.  */
    PWINCHILD volatile      pTailTodoChildren;
    /** TRUE if idle, FALSE if not. */
    long volatile           fIdle;
} WINCHILDCAREWORKER;
/** WINCHILD::uMagic value. */
#define WINCHILDCAREWORKER_MAGIC    0xdad0dad0U


/*********************************************************************************************************************************
*   Global Variables                                                                                                             *
*********************************************************************************************************************************/
/** Whether it's initialized or not. */
static BOOL                 g_fInitialized = FALSE;
/** Set when we're shutting down everything. */
static BOOL volatile        g_fShutdown = FALSE;
/** Event used to wait for children. */
static HANDLE               g_hEvtWaitChildren = INVALID_HANDLE_VALUE;
/** Number of childcare workers currently in g_papChildCareworkers. */
static unsigned             g_cChildCareworkers = 0;
/** Maximum number of childcare workers in g_papChildCareworkers. */
static unsigned             g_cChildCareworkersMax = 0;
/** Pointer to childcare workers. */
static PWINCHILDCAREWORKER *g_papChildCareworkers = NULL;
/** The processor group allocator state. */
static MKWINCHILDCPUGROUPALLOCSTATE g_ProcessorGroupAllocator;
/** Number of processor groups in the system.   */
static unsigned             g_cProcessorGroups = 1;
/** Array detailing how many active processors there are in each group. */
static unsigned const      *g_pacProcessorsInGroup = &g_cProcessorGroups;
/** Kernel32!GetActiveProcessorGroupCount */
static WORD (WINAPI        *g_pfnGetActiveProcessorGroupCount)(VOID);
/** Kernel32!GetActiveProcessorCount */
static DWORD (WINAPI       *g_pfnGetActiveProcessorCount)(WORD);
/** Kernel32!SetThreadGroupAffinity */
static BOOL (WINAPI        *g_pfnSetThreadGroupAffinity)(HANDLE, CONST GROUP_AFFINITY *, GROUP_AFFINITY *);
/** NTDLL!NtQueryInformationProcess */
static NTSTATUS (NTAPI     *g_pfnNtQueryInformationProcess)(HANDLE, PROCESSINFOCLASS, PVOID, ULONG, PULONG);
/** Set if the windows host is 64-bit. */
static BOOL                 g_f64BitHost = (K_ARCH_BITS == 64);
/** Windows version info.
 * @note Putting this before the volatile stuff, hoping to keep it in a
 *       different cache line than the static bits above. */
static OSVERSIONINFOA       g_VersionInfo = { sizeof(g_VersionInfo), 4, 0, 1381, VER_PLATFORM_WIN32_NT, {0} };

/** Children that has been completed.
 * This is updated atomically, pushing completed children in LIFO fashion
 * (thus 'tail'), then hitting g_hEvtWaitChildren if head. */
static PWINCHILD volatile   g_pTailCompletedChildren = NULL;

/** Number of idle pending children.
 * This is updated before g_hEvtWaitChildren is signalled. */
static unsigned volatile    g_cPendingChildren = 0;

/** Number of idle childcare worker threads. */
static unsigned volatile    g_cIdleChildcareWorkers = 0;
/** Index of the last idle child careworker (just a hint). */
static unsigned volatile    g_idxLastChildcareWorker = 0;

#ifdef WITH_RW_LOCK
/** RW lock for serializing kmkbuiltin_redirect and CreateProcess. */
static SRWLOCK              g_RWLock;
#endif

/** The job object for this make instance, if we created/opened one. */
static HANDLE               g_hJob = NULL;


/*********************************************************************************************************************************
*   Internal Functions                                                                                                           *
*********************************************************************************************************************************/
static void mkWinChildInitJobObjectAssociation(void);

#if K_ARCH_BITS == 32 && !defined(_InterlockedCompareExchangePointer)
/** _InterlockedCompareExchangePointer is missing? (VS2010) */
K_INLINE void *_InterlockedCompareExchangePointer(void * volatile *ppvDst, void *pvNew, void *pvOld)
{
    return (void *)_InterlockedCompareExchange((long volatile *)ppvDst, (intptr_t)pvNew, (intptr_t)pvOld);
}
#endif


/**
 * Initializes the windows child module.
 *
 * @param   cJobSlots           The number of job slots.
 */
void MkWinChildInit(unsigned int cJobSlots)
{
    HMODULE hmod;

    /*
     * Figure out how many childcare workers first.
     */
    static unsigned int const s_cMaxWorkers = 4096;
    unsigned cWorkers;
    if (cJobSlots >= 1 && cJobSlots < s_cMaxWorkers)
        cWorkers = cJobSlots;
    else
        cWorkers = s_cMaxWorkers;

    /*
     * Allocate the array and the child completed event object.
     */
    g_papChildCareworkers = (PWINCHILDCAREWORKER *)xcalloc(cWorkers * sizeof(g_papChildCareworkers[0]));
    g_cChildCareworkersMax = cWorkers;

    g_hEvtWaitChildren = CreateEvent(NULL, FALSE /*fManualReset*/, FALSE /*fInitialState*/, NULL /*pszName*/);
    if (!g_hEvtWaitChildren)
        fatal(NILF, INTSTR_LENGTH, _("MkWinChildInit: CreateEvent failed: %u"), GetLastError());

    /*
     * NTDLL imports that we need.
     */
    hmod = GetModuleHandleA("NTDLL.DLL");
    *(FARPROC *)&g_pfnNtQueryInformationProcess = GetProcAddress(hmod, "NtQueryInformationProcess");
    if (!g_pfnNtQueryInformationProcess)
        fatal(NILF, 0, _("MkWinChildInit: NtQueryInformationProcess not found"));

#if K_ARCH_BITS == 32
    /*
     * Initialize g_f64BitHost.
     */
    if (!IsWow64Process(GetCurrentProcess(), &g_f64BitHost))
        fatal(NILF, INTSTR_LENGTH, _("MkWinChildInit: IsWow64Process failed: %u"), GetLastError());
#elif K_ARCH_BITS == 64
    assert(g_f64BitHost);
#else
# error "K_ARCH_BITS is bad/missing"
#endif

    /*
     * Figure out how many processor groups there are.
     * For that we need to first figure the windows version.
     */
    if (!GetVersionExA(&g_VersionInfo))
    {
        DWORD uRawVer = GetVersion();
        g_VersionInfo.dwMajorVersion = uRawVer & 0xff;
        g_VersionInfo.dwMinorVersion = (uRawVer >>  8) &   0xff;
        g_VersionInfo.dwBuildNumber  = (uRawVer >> 16) & 0x7fff;
    }
    if (g_VersionInfo.dwMajorVersion >= 6)
    {
        hmod = GetModuleHandleA("KERNEL32.DLL");
        *(FARPROC *)&g_pfnGetActiveProcessorGroupCount = GetProcAddress(hmod, "GetActiveProcessorGroupCount");
        *(FARPROC *)&g_pfnGetActiveProcessorCount      = GetProcAddress(hmod, "GetActiveProcessorCount");
        *(FARPROC *)&g_pfnSetThreadGroupAffinity       = GetProcAddress(hmod, "SetThreadGroupAffinity");
        if (   g_pfnSetThreadGroupAffinity
            && g_pfnGetActiveProcessorCount
            && g_pfnGetActiveProcessorGroupCount)
        {
            unsigned int *pacProcessorsInGroup;
            unsigned      iGroup;
            g_cProcessorGroups = g_pfnGetActiveProcessorGroupCount();
            if (g_cProcessorGroups == 0)
                g_cProcessorGroups = 1;

            pacProcessorsInGroup = (unsigned int *)xmalloc(sizeof(g_pacProcessorsInGroup[0]) * g_cProcessorGroups);
            g_pacProcessorsInGroup = pacProcessorsInGroup;
            for (iGroup = 0; iGroup < g_cProcessorGroups; iGroup++)
                pacProcessorsInGroup[iGroup] = g_pfnGetActiveProcessorCount(iGroup);

            MkWinChildInitCpuGroupAllocator(&g_ProcessorGroupAllocator);
        }
        else
        {
            g_pfnSetThreadGroupAffinity       = NULL;
            g_pfnGetActiveProcessorCount      = NULL;
            g_pfnGetActiveProcessorGroupCount = NULL;
        }
    }

#ifdef WITH_RW_LOCK
    /*
     * For serializing with standard file handle manipulation (kmkbuiltin_redirect).
     */
    InitializeSRWLock(&g_RWLock);
#endif

    /*
     * Associate with a job object.
     */
    mkWinChildInitJobObjectAssociation();

    /*
     * This is dead code that was thought to fix a problem observed doing
     * `tcc.exe /c "kmk |& tee bld.log"` and leading to a crash in cl.exe
     * when spawned with fInheritHandles = FALSE, see hStdErr=NULL in the
     * child.  However, it turns out this was probably caused by not clearing
     * the CRT file descriptor and handle table in the startup info.
     * Leaving the code here in case it comes in handy after all.
     */
#if 0
    {
        struct
        {
            DWORD  uStdHandle;
            HANDLE hHandle;
        } aHandles[3] = { { STD_INPUT_HANDLE, NULL }, { STD_OUTPUT_HANDLE, NULL }, { STD_ERROR_HANDLE, NULL } };
        int i;

        for (i = 0; i < 3; i++)
            aHandles[i].hHandle = GetStdHandle(aHandles[i].uStdHandle);

        for (i = 0; i < 3; i++)
            if (   aHandles[i].hHandle == NULL
                || aHandles[i].hHandle == INVALID_HANDLE_VALUE)
            {
                int fd = open("nul", _O_RDWR);
                if (fd >= 0)
                {
                    if (_dup2(fd, i) >= 0)
                    {
                        assert((HANDLE)_get_osfhandle(i) != aHandles[i].hHandle);
                        assert((HANDLE)_get_osfhandle(i) == GetStdHandle(aHandles[i].uStdHandle));
                    }
                    else
                        ONNNS(fatal, NILF, "_dup2(%d('nul'), %d) failed: %u (%s)", fd, i, errno, strerror(errno));
                    if (fd != i)
                        close(fd);
                }
                else
                    ONNS(fatal, NILF, "open(nul,RW) failed: %u (%s)", i, errno, strerror(errno));
            }
            else
            {
                int j;
                for (j = i + 1; j < 3; j++)
                    if (aHandles[j].hHandle == aHandles[i].hHandle)
                    {
                        int fd = _dup(j);
                        if (fd >= 0)
                        {
                            if (_dup2(fd, j) >= 0)
                            {
                                aHandles[j].hHandle = (HANDLE)_get_osfhandle(j);
                                assert(aHandles[j].hHandle != aHandles[i].hHandle);
                                assert(aHandles[j].hHandle == GetStdHandle(aHandles[j].uStdHandle));
                            }
                            else
                                ONNNS(fatal, NILF, "_dup2(%d, %d) failed: %u (%s)", fd, j, errno, strerror(errno));
                            if (fd != j)
                                close(fd);
                        }
                        else
                            ONNS(fatal, NILF, "_dup(%d) failed: %u (%s)", j, errno, strerror(errno));
                    }
            }
    }
#endif
}

/**
 * Create or open a job object for this make instance and its children.
 *
 * Depending on the --job-object=mode value, we typically create/open a job
 * object here if we're the root make instance.  The job object is then
 * typically configured to kill all remaining processes when the root make
 * terminates, so that there aren't any stuck processes around messing up
 * subsequent builds.  This is very handy on build servers.
 *
 * If we're it no-kill mode, the job object is pretty pointless for manual
 * cleanup as the job object becomes invisible (or something) when the last
 * handle to it closes, i.e. g_hJob.  On windows 8 and later it looks
 * like any orphaned children are immediately assigned to the parent job
 * object.  Too bad for kmk_kill and such.
 *
 * win_job_object_mode values: login, root, each, none
 */
static void mkWinChildInitJobObjectAssociation(void)
{
    BOOL        fCreate   = TRUE;
    char        szJobName[128];
    const char *pszJobName = win_job_object_name;

    /* Skip if disabled. */
    if (strcmp(win_job_object_mode, "none") == 0)
        return;

    /* Skip if not root make instance, unless we're having one job object
       per make instance. */
    if (   makelevel != 0
        && strcmp(win_job_object_mode, "each") != 0)
        return;

    /* Format the the default job object name if --job-object-name
       wasn't given. */
    if (!pszJobName || *pszJobName == '\0')
    {
        pszJobName = szJobName;
        if (strcmp(win_job_object_mode, "login") == 0)
        {
            /* Use the AuthenticationId like mspdbsrv.exe does. */
            HANDLE hToken;
            if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken))
            {
                TOKEN_STATISTICS TokenStats;
                DWORD            cbRet = 0;
                memset(&TokenStats, 0, sizeof(TokenStats));
                if (GetTokenInformation(hToken, TokenStatistics, &TokenStats, sizeof(TokenStats), &cbRet))
                    snprintf(szJobName, sizeof(szJobName), "kmk-job-obj-login-%08x.%08x",
                             (unsigned)TokenStats.AuthenticationId.HighPart, (unsigned)TokenStats.AuthenticationId.LowPart);
                else
                {
                    ONN(message, 0, _("GetTokenInformation failed: %u (cbRet=%u)"), GetLastError(), cbRet);
                    return;
                }
                CloseHandle(hToken);
            }
            else
            {
                ON(message, 0, _("OpenProcessToken failed: %u"), GetLastError());
                return;
            }
        }
        else
        {
            SYSTEMTIME Now = {0};
            GetSystemTime(&Now);
            snprintf(szJobName, sizeof(szJobName), "kmk-job-obj-%04u-%02u-%02uT%02u-%02u-%02uZ%u",
                     Now.wYear, Now.wMonth, Now.wDay, Now.wHour, Now.wMinute, Now.wSecond, getpid());
        }
    }

    /* In login mode and when given a job object name, we try open it first. */
    if (   win_job_object_name
        || strcmp(win_job_object_mode, "login") == 0)
    {
        g_hJob = OpenJobObjectA(JOB_OBJECT_ASSIGN_PROCESS, win_job_object_no_kill /*bInheritHandle*/, pszJobName);
        if (g_hJob)
            fCreate = FALSE;
        else
        {
            DWORD dwErr = GetLastError();
            if (dwErr != ERROR_PATH_NOT_FOUND && dwErr != ERROR_FILE_NOT_FOUND)
            {
                OSN(message, 0, _("OpenJobObjectA(,,%s) failed: %u"), pszJobName, GetLastError());
                return;
            }
        }
    }

    if (fCreate)
    {
        SECURITY_ATTRIBUTES SecAttr = { sizeof(SecAttr), NULL, TRUE /*bInheritHandle*/ };
        g_hJob = CreateJobObjectA(win_job_object_no_kill ? &SecAttr : NULL, pszJobName);
        if (g_hJob)
        {
            /* We need to set the BREAKAWAY_OK flag, as we don't want make CreateProcess
               fail if someone tries to break way.  Also set KILL_ON_JOB_CLOSE unless
               --job-object-no-kill is given. */
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION Info;
            DWORD cbActual = 0;
            memset(&Info, 0, sizeof(Info));
            if (QueryInformationJobObject(g_hJob, JobObjectExtendedLimitInformation, &Info, sizeof(Info), &cbActual))
            {
                Info.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_BREAKAWAY_OK;
                if (!win_job_object_no_kill)
                    Info.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
                else
                    Info.BasicLimitInformation.LimitFlags &= ~JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
                if (!SetInformationJobObject(g_hJob, JobObjectExtendedLimitInformation, &Info, sizeof(Info)))
                    OSSN(message, 0, _("SetInformationJobObject(%s,JobObjectExtendedLimitInformation,{%s},) failed: %u"),
                         pszJobName, win_job_object_mode, GetLastError());
            }
            else
                OSN(message, 0, _("QueryInformationJobObject(%s,JobObjectExtendedLimitInformation,,,) failed: %u"),
                    pszJobName, GetLastError());
        }
        else
        {
            OSN(message, 0, _("CreateJobObjectA(NULL,%s) failed: %u"), pszJobName, GetLastError());
            return;
        }
    }

    /* Make it our job object. */
    if (!(AssignProcessToJobObject(g_hJob, GetCurrentProcess())))
        OSN(message, 0, _("AssignProcessToJobObject(%s, me) failed: %u"), pszJobName, GetLastError());
}

/**
 * Used by mkWinChildcareWorkerThread() and MkWinChildWait() to get the head
 * child from a lifo (g_pTailCompletedChildren, pTailTodoChildren).
 *
 * @returns Head child.
 * @param   ppTail          Pointer to the child variable.
 * @param   pChild          Tail child.
 */
static PWINCHILD mkWinChildDequeFromLifo(PWINCHILD volatile *ppTail, PWINCHILD pChild)
{
    if (pChild->pNext)
    {
        PWINCHILD pPrev;
        do
        {
            pPrev = pChild;
            pChild = pChild->pNext;
        } while (pChild->pNext);
        pPrev->pNext = NULL;
    }
    else
    {
        PWINCHILD const pWantedChild = pChild;
        pChild = _InterlockedCompareExchangePointer(ppTail, NULL, pWantedChild);
        if (pChild != pWantedChild)
        {
            PWINCHILD pPrev;
            do
            {
                pPrev = pChild;
                pChild = pChild->pNext;
            } while (pChild->pNext);
            pPrev->pNext = NULL;
            assert(pChild == pWantedChild);
        }
    }
    return pChild;
}

/**
 * Output error message while running on a worker thread.
 *
 * @returns -1
 * @param   pWorker             The calling worker.  Mainly for getting the
 *                              current child and its stderr output unit.  Pass
 *                              NULL if the output should go thru the child
 *                              stderr buffering.
 * @param   iType               The error type:
 *                                  - 0: more of a info directly to stdout,
 *                                  - 1: child related error,
 *                                  - 2: child related error for immedate release.
 * @param   pszFormat           The message format string.
 * @param   ...                 Argument for the message.
 */
static int MkWinChildError(PWINCHILDCAREWORKER pWorker, int iType, const char *pszFormat, ...)
{
    /*
     * Format the message into stack buffer.
     */
    char        szMsg[4096];
    int         cchMsg;
    int         cchPrefix;
    va_list     va;

    /* Compose the prefix, being paranoid about it not exceeding the buffer in any way. */
    const char *pszInfix = iType == 0 ? "info: " : "error: ";
    const char *pszProgram = program;
    if (strlen(pszProgram) > 80)
    {
#ifdef KMK
        pszProgram = "kmk";
#else
        pszProgram = "gnumake";
#endif
    }
    if (makelevel == 0)
        cchPrefix = snprintf(szMsg, sizeof(szMsg) / 2, "%s: %s", pszProgram, pszInfix);
    else
        cchPrefix = snprintf(szMsg, sizeof(szMsg) / 2, "%s[%u]: %s", pszProgram, makelevel, pszInfix);
    assert(cchPrefix < sizeof(szMsg) / 2 && cchPrefix > 0);

    /* Format the user specified message. */
    va_start(va, pszFormat);
    cchMsg = vsnprintf(&szMsg[cchPrefix], sizeof(szMsg) - 2 - cchPrefix, pszFormat, va);
    va_end(va);
    szMsg[sizeof(szMsg) - 2] = '\0';
    cchMsg = strlen(szMsg);

    /* Make sure there's a newline at the end of it (we reserved space for that). */
    if (cchMsg <= 0 || szMsg[cchMsg - 1] != '\n')
    {
        szMsg[cchMsg++] = '\n';
        szMsg[cchMsg]   = '\0';
    }

#ifdef CONFIG_WITH_OUTPUT_IN_MEMORY
    /*
     * Try use the stderr of the current child of the worker.
     */
    if (   iType != 0
        && iType != 3
        && pWorker)
    {
        PWINCHILD pChild = pWorker->pCurChild;
        if (pChild)
        {
            struct child *pMkChild = pChild->pMkChild;
            if (pMkChild)
            {
                output_write_text(&pMkChild->output, 1 /*is_err*/, szMsg, cchMsg);
                return -1;
            }
        }
    }
#endif

    /*
     * Fallback to writing directly to stderr.
     */
    maybe_con_fwrite(szMsg, cchMsg, 1, iType == 0 ? stdout : stderr);
    return -1;
}

/**
 * Duplicates the given UTF-16 string.
 *
 * @returns 0
 * @param   pwszSrc             The UTF-16 string to duplicate.
 * @param   cwcSrc              Length, may include the terminator.
 * @param   ppwszDst            Where to return the duplicate.
 */
static int mkWinChildDuplicateUtf16String(const WCHAR *pwszSrc, size_t cwcSrc, WCHAR **ppwszDst)
{
    size_t cb = sizeof(WCHAR) * cwcSrc;
    if (cwcSrc > 0 && pwszSrc[cwcSrc - 1] == L'\0')
        *ppwszDst = (WCHAR *)memcpy(xmalloc(cb), pwszSrc, cb);
    else
    {
        WCHAR *pwszDst = (WCHAR *)xmalloc(cb + sizeof(WCHAR));
        memcpy(pwszDst, pwszSrc, cb);
        pwszDst[cwcSrc] = L'\0';
        *ppwszDst = pwszDst;
    }
    return 0;
}


/**
 * Used to flush data we're read but not yet written at the termination of a
 * process.
 *
 * @param   pChild          The child.
 * @param   pPipe           The pipe.
 */
static void mkWinChildcareWorkerFlushUnwritten(PWINCHILD pChild, PWINCCWPIPE pPipe)
{
    DWORD cbUnwritten = pPipe->offPendingRead - pPipe->cbWritten;
    assert(pPipe->cbWritten      <= pPipe->cbBuffer - 16);
    assert(pPipe->offPendingRead <= pPipe->cbBuffer - 16);
    if (cbUnwritten)
    {
#ifdef CONFIG_WITH_OUTPUT_IN_MEMORY
        if (pChild && pChild->pMkChild)
        {
            output_write_bin(&pChild->pMkChild->output, pPipe->iWhich == 2, &pPipe->pbBuffer[pPipe->cbWritten], cbUnwritten);
            pPipe->cbWritten += cbUnwritten;
        }
        else
#endif
        {
            DWORD cbWritten = 0;
            if (WriteFile(GetStdHandle(pPipe->iWhich == 1 ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE),
                          &pPipe->pbBuffer[pPipe->cbWritten], cbUnwritten, &cbWritten, NULL))
                pPipe->cbWritten += cbWritten <= cbUnwritten ? cbWritten : cbUnwritten; /* paranoia */
        }
        pPipe->fHaveWrittenOut = TRUE;
    }
}

/**
 * This logic mirrors kwSandboxConsoleFlushAll.
 *
 * @returns TRUE if it looks like a CL.EXE source line, otherwise FALSE.
 * @param   pPipe               The pipe.
 * @param   offStart            The start of the output in the pipe buffer.
 * @param   offEnd              The end of the output in the pipe buffer.
 */
static BOOL mkWinChildcareWorkerIsClExeSourceLine(PWINCCWPIPE pPipe, DWORD offStart, DWORD offEnd)
{
    if (offEnd < offStart + 2)
        return FALSE;
    if (offEnd - offStart > 80)
        return FALSE;

    if (   pPipe->pbBuffer[offEnd - 2] != '\r'
        || pPipe->pbBuffer[offEnd - 1] != '\n')
        return FALSE;

    offEnd -= 2;
    while (offEnd-- > offStart)
    {
        char ch = pPipe->pbBuffer[offEnd];
        if (isalnum(ch) || ch == '.' || ch == ' ' || ch == '_' || ch == '-')
        { /* likely */ }
        else
            return FALSE;
    }

    return TRUE;
}

/**
 * Adds output to the given standard output for the child.
 *
 * There is no pending read when this function is called, so we're free to
 * reshuffle the buffer if desirable.
 *
 * @param   pChild          The child. Optional (kSubmit).
 * @param   iWhich          Which standard descriptor number.
 * @param   cbNewData       How much more output was caught.
 */
static void mkWinChildcareWorkerCaughtMoreOutput(PWINCHILD pChild, PWINCCWPIPE pPipe, DWORD cbNewData)
{
    DWORD offStart = pPipe->cbWritten;
    assert(offStart <= pPipe->offPendingRead);
    assert(offStart <= pPipe->cbBuffer - 16);
    assert(pPipe->offPendingRead <= pPipe->cbBuffer - 16);
    if (cbNewData > 0)
    {
        DWORD offRest;

        /* Move offPendingRead ahead by cbRead. */
        pPipe->offPendingRead += cbNewData;
        assert(pPipe->offPendingRead <= pPipe->cbBuffer);
        if (pPipe->offPendingRead > pPipe->cbBuffer)
            pPipe->offPendingRead = pPipe->cbBuffer;

        /* Locate the last newline in the buffer. */
        offRest = pPipe->offPendingRead;
        while (offRest > offStart && pPipe->pbBuffer[offRest - 1] != '\n')
            offRest--;

        /* If none were found and we've less than 16 bytes left in the buffer, try
           find a word boundrary to flush on instead. */
        if (   offRest <= offStart
            && pPipe->cbBuffer - pPipe->offPendingRead + offStart < 16)
        {
            offRest = pPipe->offPendingRead;
            while (   offRest > offStart
                   && isalnum(pPipe->pbBuffer[offRest - 1]))
                offRest--;
            if (offRest == offStart)
                offRest = pPipe->offPendingRead;
        }
        /* If this is a potential CL.EXE process, we will keep the source
           filename unflushed and maybe discard it at the end. */
        else if (   pChild
                 && pChild->fProbableClExe
                 && pPipe->iWhich == 1
                 && offRest == pPipe->offPendingRead
                 && mkWinChildcareWorkerIsClExeSourceLine(pPipe, offStart, offRest))
            offRest = offStart;

        if (offRest > offStart)
        {
            /* Write out offStart..offRest. */
            DWORD cbToWrite = offRest - offStart;
#ifdef CONFIG_WITH_OUTPUT_IN_MEMORY
            if (pChild && pChild->pMkChild)
            {
                output_write_bin(&pChild->pMkChild->output, pPipe->iWhich == 2, &pPipe->pbBuffer[offStart], cbToWrite);
                offStart += cbToWrite;
                pPipe->cbWritten = offStart;
            }
            else
#endif
            {
                DWORD cbWritten = 0;
                if (WriteFile(GetStdHandle(pPipe->iWhich == 1 ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE),
                              &pPipe->pbBuffer[offStart], cbToWrite, &cbWritten, NULL))
                {
                    offStart += cbWritten <= cbToWrite ? cbWritten : cbToWrite; /* paranoia */
                    pPipe->cbWritten = offStart;
                }
            }
            pPipe->fHaveWrittenOut = TRUE;
        }
    }

    /* Shuffle the data to the front of the buffer.  */
    if (offStart > 0)
    {
        DWORD cbUnwritten = pPipe->offPendingRead - offStart;
        if (cbUnwritten > 0)
            memmove(pPipe->pbBuffer, &pPipe->pbBuffer[offStart], cbUnwritten);
        pPipe->offPendingRead -= pPipe->cbWritten;
        pPipe->cbWritten       = 0;
    }
}

/**
 * Catches output from the given pipe.
 *
 * @param   pChild          The child. Optional (kSubmit).
 * @param   pPipe           The pipe.
 * @param   fDraining       Set if we're draining the pipe after the process
 *                          terminated.
 */
static void mkWinChildcareWorkerCatchOutput(PWINCHILD pChild, PWINCCWPIPE pPipe, BOOL fDraining)
{
    /*
     * Deal with already pending read.
     */
    if (pPipe->fReadPending)
    {
        DWORD cbRead = 0;
        if (GetOverlappedResult(pPipe->hPipeMine, &pPipe->Overlapped, &cbRead, !fDraining))
        {
            mkWinChildcareWorkerCaughtMoreOutput(pChild, pPipe, cbRead);
            pPipe->fReadPending = FALSE;
        }
        else if (fDraining && GetLastError() == ERROR_IO_INCOMPLETE)
            return;
        else
        {
            MkWinChildError(pChild ? pChild->pWorker : NULL, 2, "GetOverlappedResult failed: %u\n", GetLastError());
            pPipe->fReadPending = FALSE;
            if (fDraining)
                return;
        }
    }

    /*
     * Read data till one becomes pending.
     */
    for (;;)
    {
        DWORD cbRead;

        memset(&pPipe->Overlapped, 0, sizeof(pPipe->Overlapped));
        pPipe->Overlapped.hEvent = pPipe->hEvent;
        ResetEvent(pPipe->hEvent);

        assert(pPipe->offPendingRead < pPipe->cbBuffer);
        SetLastError(0);
        cbRead = 0;
        if (!ReadFile(pPipe->hPipeMine, &pPipe->pbBuffer[pPipe->offPendingRead],
                      pPipe->cbBuffer - pPipe->offPendingRead, &cbRead, &pPipe->Overlapped))
        {
            DWORD dwErr = GetLastError();
            if (dwErr == ERROR_IO_PENDING)
                pPipe->fReadPending = TRUE;
            else
                MkWinChildError(pChild ? pChild->pWorker : NULL, 2,
                                "ReadFile failed on standard %s: %u\n",
                                pPipe->iWhich == 1 ? "output" : "error",  GetLastError());
            return;
        }

        mkWinChildcareWorkerCaughtMoreOutput(pChild, pPipe, cbRead);
    }
}

/**
 * Makes sure the output pipes are drained and pushed to output.
 *
 * @param   pChild              The child. Optional (kSubmit).
 * @param   pStdOut             The standard output pipe structure.
 * @param   pStdErr             The standard error pipe structure.
 */
void MkWinChildcareWorkerDrainPipes(PWINCHILD pChild, PWINCCWPIPE pStdOut, PWINCCWPIPE pStdErr)
{
    mkWinChildcareWorkerCatchOutput(pChild, pStdOut, TRUE /*fDraining*/);
    mkWinChildcareWorkerCatchOutput(pChild, pStdErr, TRUE /*fDraining*/);

    /* Drop lone 'source.c' line from CL.exe, but only if no other output at all. */
    if (   pChild
        && pChild->fProbableClExe
        && !pStdOut->fHaveWrittenOut
        && !pStdErr->fHaveWrittenOut
        && pStdErr->cbWritten == pStdErr->offPendingRead
        && pStdOut->cbWritten < pStdOut->offPendingRead
        && mkWinChildcareWorkerIsClExeSourceLine(pStdOut, pStdOut->cbWritten, pStdOut->offPendingRead))
    {
        if (!pStdOut->fReadPending)
            pStdOut->cbWritten = pStdOut->offPendingRead = 0;
        else
            pStdOut->cbWritten = pStdOut->offPendingRead;
    }
    else
    {
        mkWinChildcareWorkerFlushUnwritten(pChild, pStdOut);
        mkWinChildcareWorkerFlushUnwritten(pChild, pStdErr);
    }
}

/**
 * Commmon worker for waiting on a child process and retrieving the exit code.
 *
 * @returns Child exit code.
 * @param   pWorker             The worker.
 * @param   pChild              The child.
 * @param   hProcess            The process handle.
 * @param   pwszJob             The job name.
 * @param   fCatchOutput        Set if we need to work the output pipes
 *                              associated with the worker.
 */
static int mkWinChildcareWorkerWaitForProcess(PWINCHILDCAREWORKER pWorker, PWINCHILD pChild, HANDLE hProcess,
                                              WCHAR const *pwszJob, BOOL fCatchOutput)
{
    DWORD const msStart = GetTickCount();
    DWORD       msNextMsg = msStart + 15000;

    /* Reset the written indicators on the pipes before we start loop. */
    pWorker->pStdOut->fHaveWrittenOut = FALSE;
    pWorker->pStdErr->fHaveWrittenOut = FALSE;

    for (;;)
    {
        /*
         * Do the waiting and output catching.
         */
        DWORD dwStatus;
        if (!fCatchOutput)
            dwStatus = WaitForSingleObject(hProcess, 15001 /*ms*/);
        else
        {
            HANDLE ahHandles[3] = { hProcess, pWorker->pStdOut->hEvent, pWorker->pStdErr->hEvent };
            dwStatus = WaitForMultipleObjects(3, ahHandles, FALSE /*fWaitAll*/, 1000 /*ms*/);
            if (dwStatus == WAIT_OBJECT_0 + 1)
                mkWinChildcareWorkerCatchOutput(pChild, pWorker->pStdOut, FALSE /*fDraining*/);
            else if (dwStatus == WAIT_OBJECT_0 + 2)
                mkWinChildcareWorkerCatchOutput(pChild, pWorker->pStdErr, FALSE /*fDraining*/);
        }
        assert(dwStatus != WAIT_FAILED);

        /*
         * Get the exit code and return if the process was signalled as done.
         */
        if (dwStatus == WAIT_OBJECT_0)
        {
            DWORD dwExitCode = -42;
            if (GetExitCodeProcess(hProcess, &dwExitCode))
            {
                pChild->iExitCode = (int)dwExitCode;
                if (fCatchOutput)
                    MkWinChildcareWorkerDrainPipes(pChild, pWorker->pStdOut, pWorker->pStdErr);
                return dwExitCode;
            }
        }
        /*
         * Loop again if just a timeout or pending output?
         * Put out a message every 15 or 30 seconds if the job takes a while.
         */
        else if (   dwStatus == WAIT_TIMEOUT
                 || dwStatus == WAIT_OBJECT_0 + 1
                 || dwStatus == WAIT_OBJECT_0 + 2
                 || dwStatus == WAIT_IO_COMPLETION)
        {
            DWORD msNow = GetTickCount();
            if (msNow >= msNextMsg)
            {
                if (   !pChild->pMkChild
                    || !pChild->pMkChild->recursive) /* ignore make recursions */
                {
                    if (   !pChild->pMkChild
                        || !pChild->pMkChild->file
                        || !pChild->pMkChild->file->name)
                        MkWinChildError(NULL, 0, "Pid %u ('%ls') still running after %u seconds\n",
                                        GetProcessId(hProcess), pwszJob, (msNow - msStart) / 1000);
                    else
                        MkWinChildError(NULL, 0, "Target '%s' (pid %u) still running after %u seconds\n",
                                        pChild->pMkChild->file->name, GetProcessId(hProcess), (msNow - msStart) / 1000);
                }

                /* After 15s, 30s, 60s, 120s, 180s, ... */
                if (msNextMsg == msStart + 15000)
                    msNextMsg += 15000;
                else
                    msNextMsg += 30000;
            }
            continue;
        }

        /* Something failed. */
        pChild->iExitCode = GetLastError();
        if (pChild->iExitCode == 0)
            pChild->iExitCode = -4242;
        return pChild->iExitCode;
    }
}


/**
 * Closes standard handles that need closing before destruction.
 *
 * @param   pChild          The child (WINCHILDTYPE_PROCESS).
 */
static void mkWinChildcareWorkerCloseStandardHandles(PWINCHILD pChild)
{
    if (   pChild->u.Process.fCloseStdOut
        && pChild->u.Process.hStdOut != INVALID_HANDLE_VALUE)
    {
        CloseHandle(pChild->u.Process.hStdOut);
        pChild->u.Process.hStdOut      = INVALID_HANDLE_VALUE;
        pChild->u.Process.fCloseStdOut = FALSE;
    }
    if (   pChild->u.Process.fCloseStdErr
        && pChild->u.Process.hStdErr != INVALID_HANDLE_VALUE)
    {
        CloseHandle(pChild->u.Process.hStdErr);
        pChild->u.Process.hStdErr      = INVALID_HANDLE_VALUE;
        pChild->u.Process.fCloseStdErr = FALSE;
    }
}


/**
 * Does the actual process creation.
 *
 * @returns 0 if there is anything to wait on, otherwise non-zero windows error.
 * @param   pWorker             The childcare worker.
 * @param   pChild              The child.
 * @param   pwszImageName       The image path.
 * @param   pwszCommandLine     The command line.
 * @param   pwszzEnvironment    The enviornment block.
 */
static int mkWinChildcareWorkerCreateProcess(PWINCHILDCAREWORKER pWorker, WCHAR const *pwszImageName,
                                             WCHAR const *pwszCommandLine, WCHAR const *pwszzEnvironment, WCHAR const *pwszCwd,
                                             BOOL pafReplace[3], HANDLE pahChild[3], BOOL fCatchOutput, HANDLE *phProcess)
{
    PROCESS_INFORMATION ProcInfo;
    STARTUPINFOW        StartupInfo;
    DWORD               fFlags       = CREATE_UNICODE_ENVIRONMENT;
    BOOL const          fHaveHandles = pafReplace[0] | pafReplace[1] | pafReplace[2];
    BOOL                fRet;
    DWORD               dwErr;
#ifdef KMK
    extern int          process_priority;
#endif

    /*
     * Populate startup info.
     *
     * Turns out we can get away without passing TRUE for the inherit handles
     * parameter to CreateProcess when we're not using STARTF_USESTDHANDLES.
     * At least on NT, which is all worth caring about at this point + context IMO.
     *
     * Not inherting the handles is a good thing because it means we won't
     * accidentally end up with a pipe handle or such intended for a different
     * child process, potentially causing the EOF/HUP event to be delayed.
     *
     * Since the present handle inhertiance requirements only involves standard
     * output and error, we'll never set the inherit handles flag and instead
     * do manual handle duplication and planting.
     */
    memset(&StartupInfo, 0, sizeof(StartupInfo));
    StartupInfo.cb = sizeof(StartupInfo);
    GetStartupInfoW(&StartupInfo);
    StartupInfo.lpReserved2 = 0; /* No CRT file handle + descriptor info possible, sorry. */
    StartupInfo.cbReserved2 = 0;
    if (   !fHaveHandles
        && !fCatchOutput)
        StartupInfo.dwFlags &= ~STARTF_USESTDHANDLES;
    else
    {
        fFlags |= CREATE_SUSPENDED;
        StartupInfo.dwFlags &= ~STARTF_USESTDHANDLES;
    }

    /*
     * Flags.
     */
#ifdef KMK
    switch (process_priority)
    {
        case 1: fFlags |= CREATE_SUSPENDED | IDLE_PRIORITY_CLASS; break;
        case 2: fFlags |= CREATE_SUSPENDED | BELOW_NORMAL_PRIORITY_CLASS; break;
        case 3: fFlags |= CREATE_SUSPENDED | NORMAL_PRIORITY_CLASS; break;
        case 4: fFlags |= CREATE_SUSPENDED | HIGH_PRIORITY_CLASS; break;
        case 5: fFlags |= CREATE_SUSPENDED | REALTIME_PRIORITY_CLASS; break;
    }
#endif
    if (g_cProcessorGroups > 1)
        fFlags |= CREATE_SUSPENDED;

    /*
     * Try create the process.
     */
    DB(DB_JOBS, ("CreateProcessW(%ls, %ls,,, TRUE, %#x...)\n", pwszImageName, pwszCommandLine, fFlags));
    memset(&ProcInfo, 0, sizeof(ProcInfo));
#ifdef WITH_RW_LOCK
    AcquireSRWLockShared(&g_RWLock);
#endif

    fRet = CreateProcessW((WCHAR *)pwszImageName, (WCHAR *)pwszCommandLine, NULL /*pProcSecAttr*/, NULL /*pThreadSecAttr*/,
                          FALSE /*fInheritHandles*/, fFlags, (WCHAR *)pwszzEnvironment, pwszCwd, &StartupInfo, &ProcInfo);
    dwErr = GetLastError();

#ifdef WITH_RW_LOCK
    ReleaseSRWLockShared(&g_RWLock);
#endif
    if (fRet)
        *phProcess = ProcInfo.hProcess;
    else
    {
        MkWinChildError(pWorker, 1, "CreateProcess(%ls) failed: %u\n", pwszImageName, dwErr);
        return (int)dwErr;
    }

    /*
     * If the child is suspended, we've got some adjustment work to be done.
     */
    dwErr = ERROR_SUCCESS;
    if (fFlags & CREATE_SUSPENDED)
    {
        /*
         * First do handle inhertiance as that's the most complicated.
         */
        if (fHaveHandles || fCatchOutput)
        {
            char szErrMsg[128];
            if (fCatchOutput)
            {
                if (!pafReplace[1])
                {
                    pafReplace[1] = TRUE;
                    pahChild[1]   = pWorker->pStdOut->hPipeChild;
                }
                if (!pafReplace[2])
                {
                    pafReplace[2] = TRUE;
                    pahChild[2]   = pWorker->pStdErr->hPipeChild;
                }
            }
            dwErr = nt_child_inject_standard_handles(ProcInfo.hProcess, pafReplace, pahChild, szErrMsg, sizeof(szErrMsg));
            if (dwErr != 0)
                MkWinChildError(pWorker, 1, "%s\n", szErrMsg);
        }

        /*
         * Assign processor group (ignore failure).
         */
#ifdef MKWINCHILD_DO_SET_PROCESSOR_GROUP
        if (g_cProcessorGroups > 1)
        {
            GROUP_AFFINITY Affinity = { 0 /* == all active apparently */, pWorker->iProcessorGroup, { 0, 0, 0 } };
            fRet = g_pfnSetThreadGroupAffinity(ProcInfo.hThread, &Affinity, NULL);
            assert(fRet);
        }
#endif

#ifdef KMK
        /*
         * Set priority (ignore failure).
         */
        switch (process_priority)
        {
            case 1: fRet = SetThreadPriority(ProcInfo.hThread, THREAD_PRIORITY_IDLE); break;
            case 2: fRet = SetThreadPriority(ProcInfo.hThread, THREAD_PRIORITY_BELOW_NORMAL); break;
            case 3: fRet = SetThreadPriority(ProcInfo.hThread, THREAD_PRIORITY_NORMAL); break;
            case 4: fRet = SetThreadPriority(ProcInfo.hThread, THREAD_PRIORITY_HIGHEST); break;
            case 5: fRet = SetThreadPriority(ProcInfo.hThread, THREAD_PRIORITY_TIME_CRITICAL); break;
            default: fRet = TRUE;
        }
        assert(fRet);
#endif

        /*
         * Inject the job object if we're in a non-killing mode, to postpone
         * the closing of the job object and maybe make it more useful.
         */
        if (win_job_object_no_kill && g_hJob)
        {
            HANDLE hWhatever = INVALID_HANDLE_VALUE;
            DuplicateHandle(GetCurrentProcess(), g_hJob, ProcInfo.hProcess, &hWhatever, GENERIC_ALL,
                            TRUE /*bInheritHandle*/, DUPLICATE_SAME_ACCESS);
        }

        /*
         * Resume the thread if the adjustments succeeded, otherwise kill it.
         */
        if (dwErr == ERROR_SUCCESS)
        {
            fRet = ResumeThread(ProcInfo.hThread);
            assert(fRet);
            if (!fRet)
            {
                dwErr = GetLastError();
                MkWinChildError(pWorker, 1, "ResumeThread failed on child process: %u\n", dwErr);
            }
        }
        if (dwErr != ERROR_SUCCESS)
            TerminateProcess(ProcInfo.hProcess, dwErr);
    }

    /*
     * Close unnecessary handles and cache the image.
     */
    CloseHandle(ProcInfo.hThread);
    kmk_cache_exec_image_w(pwszImageName);
    return 0;
}

/**
 * Converts a argument vector that has already been quoted correctly.
 *
 * The argument vector is typically the result of quote_argv().
 *
 * @returns 0 on success, non-zero on failure.
 * @param   pWorker             The childcare worker.
 * @param   papszArgs           The argument vector to convert.
 * @param   ppwszCommandLine    Where to return the command line.
 */
static int mkWinChildcareWorkerConvertQuotedArgvToCommandline(PWINCHILDCAREWORKER pWorker, char **papszArgs,
                                                              WCHAR **ppwszCommandLine)
{
    WCHAR   *pwszCmdLine;
    WCHAR   *pwszDst;

    /*
     * Calc length the converted length.
     */
    unsigned cwcNeeded = 1;
    unsigned i = 0;
    const char *pszSrc;
    while ((pszSrc = papszArgs[i]) != NULL)
    {
        int cwcThis = MultiByteToWideChar(CP_ACP, 0 /*fFlags*/, pszSrc, -1, NULL, 0);
        if (cwcThis > 0 || *pszSrc == '\0')
            cwcNeeded += cwcThis + 1;
        else
        {
            DWORD dwErr = GetLastError();
            MkWinChildError(pWorker, 1, _("MultiByteToWideChar failed to convert argv[%u] (%s): %u\n"), i, pszSrc, dwErr);
            return dwErr;
        }
        i++;
    }

    /*
     * Allocate and do the conversion.
     */
    pwszCmdLine = pwszDst = (WCHAR *)xmalloc(cwcNeeded * sizeof(WCHAR));
    i = 0;
    while ((pszSrc = papszArgs[i]) != NULL)
    {
        int cwcThis;
        if (i > 0)
        {
            *pwszDst++ = ' ';
            cwcNeeded--;
        }

        cwcThis = MultiByteToWideChar(CP_ACP, 0 /*fFlags*/, pszSrc, -1, pwszDst, cwcNeeded);
        if (!cwcThis && *pszSrc != '\0')
        {
            DWORD dwErr = GetLastError();
            MkWinChildError(pWorker, 1, _("MultiByteToWideChar failed to convert argv[%u] (%s): %u\n"), i, pszSrc, dwErr);
            free(pwszCmdLine);
            return dwErr;
        }
        if (cwcThis > 0 && pwszDst[cwcThis - 1] == '\0')
            cwcThis--;
        pwszDst += cwcThis;
        cwcNeeded -= cwcThis;
        i++;
    }
    *pwszDst++ = '\0';

    *ppwszCommandLine = pwszCmdLine;
    return 0;
}


#define MKWCCWCMD_F_CYGWIN_SHELL    1
#define MKWCCWCMD_F_MKS_SHELL       2
#define MKWCCWCMD_F_HAVE_SH         4
#define MKWCCWCMD_F_HAVE_KASH_C     8 /**< kmk_ash -c "..." */

/*
 * @param   pWorker         The childcare worker if on one, otherwise NULL.
 */
static int mkWinChildcareWorkerConvertCommandline(PWINCHILDCAREWORKER pWorker, char **papszArgs, unsigned fFlags,
                                                  WCHAR **ppwszCommandLine)
{
    struct ARGINFO
    {
        size_t   cchSrc;
        size_t   cwcDst;           /**< converted size w/o terminator. */
        size_t   cwcDstExtra : 24; /**< Only set with fSlowly. */
        size_t   fSlowly     : 1;
        size_t   fQuoteIt    : 1;
        size_t   fEndSlashes : 1; /**< if escapes needed for trailing backslashes. */
        size_t   fExtraSpace : 1; /**< if kash -c "" needs an extra space before the quote. */
    }     *paArgInfo;
    size_t cArgs;
    size_t i;
    size_t cwcNeeded;
    WCHAR *pwszDst;
    WCHAR *pwszCmdLine;

    /*
     * Count them first so we can allocate an info array of the stack.
     */
    cArgs = 0;
    while (papszArgs[cArgs] != NULL)
        cArgs++;
    paArgInfo = (struct ARGINFO *)alloca(sizeof(paArgInfo[0]) * cArgs);

    /*
     * Preprocess them and calculate the exact command line length.
     */
    cwcNeeded = 1;
    for (i = 0; i < cArgs; i++)
    {
        char  *pszSrc = papszArgs[i];
        size_t cchSrc = strlen(pszSrc);
        paArgInfo[i].cchSrc = cchSrc;
        if (cchSrc == 0)
        {
            /* empty needs quoting. */
            paArgInfo[i].cwcDst      = 2;
            paArgInfo[i].cwcDstExtra = 0;
            paArgInfo[i].fSlowly     = 0;
            paArgInfo[i].fQuoteIt    = 1;
            paArgInfo[i].fExtraSpace = 0;
            paArgInfo[i].fEndSlashes = 0;
        }
        else
        {
            const char *pszSpace  = memchr(pszSrc, ' ', cchSrc);
            const char *pszTab    = memchr(pszSrc, '\t', cchSrc);
            const char *pszDQuote = memchr(pszSrc, '"', cchSrc);
            const char *pszEscape = memchr(pszSrc, '\\', cchSrc);
            int cwcDst = MultiByteToWideChar(CP_ACP, 0 /*fFlags*/, pszSrc, cchSrc + 1, NULL, 0);
            if (cwcDst >= 0)
                --cwcDst;
            else
            {
                DWORD dwErr = GetLastError();
                MkWinChildError(pWorker, 1, _("MultiByteToWideChar failed to convert argv[%u] (%s): %u\n"), i, pszSrc, dwErr);
                return dwErr;
            }
#if 0
            if (!pszSpace && !pszTab && !pszDQuote && !pszEscape)
            {
                /* no special handling needed. */
                paArgInfo[i].cwcDst      = cwcDst;
                paArgInfo[i].cwcDstExtra = 0;
                paArgInfo[i].fSlowly     = 0;
                paArgInfo[i].fQuoteIt    = 0;
                paArgInfo[i].fExtraSpace = 0;
                paArgInfo[i].fEndSlashes = 0;
            }
            else if (!pszDQuote && !pszEscape)
            {
                /* Just double quote it. */
                paArgInfo[i].cwcDst      = cwcDst + 2;
                paArgInfo[i].cwcDstExtra = 0;
                paArgInfo[i].fSlowly     = 0;
                paArgInfo[i].fQuoteIt    = 1;
                paArgInfo[i].fExtraSpace = 0;
                paArgInfo[i].fEndSlashes = 0;
            }
            else
#endif
            {
                /* Complicated, need to scan the string to figure out what to do. */
                size_t cwcDstExtra;
                int cBackslashes;
                char ch;

                paArgInfo[i].fQuoteIt    = 0;
                paArgInfo[i].fSlowly     = 1;
                paArgInfo[i].fExtraSpace = 0;
                paArgInfo[i].fEndSlashes = 0;

                cwcDstExtra  = 0;
                cBackslashes = 0;
                while ((ch = *pszSrc++) != '\0')
                {
                    switch (ch)
                    {
                        default:
                            cBackslashes = 0;
                            break;

                        case '\\':
                            cBackslashes++;
                            break;

                        case '"':
                            if (fFlags & (MKWCCWCMD_F_CYGWIN_SHELL | MKWCCWCMD_F_MKS_SHELL))
                                cwcDstExtra += 1; /* just an extra '"' */
                            else
                                cwcDstExtra += 1 + cBackslashes; /* extra '\\' for the '"' and for each preceeding slash. */
                            cBackslashes = 0;
                            break;

                        case ' ':
                        case '\t':
                            if (!paArgInfo[i].fQuoteIt)
                            {
                                paArgInfo[i].fQuoteIt = 1;
                                cwcDstExtra += 2;
                            }
                            cBackslashes = 0;
                            break;
                    }
                }

                /* If we're quoting the argument and it ends with trailing '\\', it/they must be escaped. */
                if (   cBackslashes > 0
                    && paArgInfo[i].fQuoteIt
                    && !(fFlags & (MKWCCWCMD_F_CYGWIN_SHELL | MKWCCWCMD_F_MKS_SHELL)))
                {
                    cwcDstExtra += cBackslashes;
                    paArgInfo[i].fEndSlashes = 1;
                }

                paArgInfo[i].cwcDst      = cwcDst + cwcDstExtra;
                paArgInfo[i].cwcDstExtra = cwcDstExtra;
            }
        }

        if (   (fFlags & MKWCCWCMD_F_HAVE_KASH_C)
            && paArgInfo[i].fQuoteIt)
        {
            paArgInfo[i].fExtraSpace = 1;
            paArgInfo[i].cwcDst++;
            paArgInfo[i].cwcDstExtra++;
        }

        cwcNeeded += (i != 0) + paArgInfo[i].cwcDst;
    }

    /*
     * Allocate the result buffer and do the actual conversion.
     */
    pwszDst = pwszCmdLine = (WCHAR *)xmalloc(sizeof(WCHAR) * cwcNeeded);
    for (i = 0; i < cArgs; i++)
    {
        char  *pszSrc = papszArgs[i];
        size_t cwcDst = paArgInfo[i].cwcDst;

        if (i != 0)
            *pwszDst++ = L' ';

        if (paArgInfo[i].fQuoteIt)
        {
            *pwszDst++ = L'"';
            cwcDst -= 2;
        }

        if (!paArgInfo[i].fSlowly)
        {
            int cwcDst2 = MultiByteToWideChar(CP_ACP, 0 /*fFlags*/, pszSrc, paArgInfo[i].cchSrc, pwszDst, cwcDst + 1);
            assert(cwcDst2 >= 0);
            pwszDst += cwcDst;
        }
        else
        {
            /* Do the conversion into the end of the output buffer, then move
               it up to where it should be char by char.  */
            int             cBackslashes;
            size_t          cwcLeft     = paArgInfo[i].cwcDst - paArgInfo[i].cwcDstExtra;
            WCHAR volatile *pwchSlowSrc = pwszDst + paArgInfo[i].cwcDstExtra;
            WCHAR volatile *pwchSlowDst = pwszDst;
            int cwcDst2 = MultiByteToWideChar(CP_ACP, 0 /*fFlags*/, pszSrc, paArgInfo[i].cchSrc,
                                              (WCHAR *)pwchSlowSrc, cwcLeft + 1);
            assert(cwcDst2 >= 0);

            cBackslashes = 0;
            while (cwcLeft-- > 0)
            {
                WCHAR wcSrc = *pwchSlowSrc++;
                if (wcSrc != L'\\' && wcSrc != L'"')
                    cBackslashes = 0;
                else if (wcSrc == L'\\')
                    cBackslashes++;
                else if (   (fFlags & (MKWCCWCMD_F_CYGWIN_SHELL | MKWCCWCMD_F_HAVE_SH))
                         ==           (MKWCCWCMD_F_CYGWIN_SHELL | MKWCCWCMD_F_HAVE_SH))
                {
                    *pwchSlowDst++ = L'"'; /* cygwin: '"' instead of '\\', no escaped slashes. */
                    cBackslashes = 0;
                }
                else
                {
                    if (!(fFlags & (MKWCCWCMD_F_CYGWIN_SHELL | MKWCCWCMD_F_MKS_SHELL)))
                        cBackslashes += 1; /* one extra escape the '"' and one for each preceeding slash. */
                    while (cBackslashes > 0)
                    {
                        *pwchSlowDst++ = L'\\';
                        cBackslashes--;
                    }
                }
                *pwchSlowDst++ = wcSrc;
                assert((uintptr_t)pwchSlowDst <= (uintptr_t)pwchSlowSrc);
            }

            if (paArgInfo[i].fEndSlashes)
                while (cBackslashes-- > 0)
                    *pwchSlowDst++ = L'\\';

            pwszDst += cwcDst;
            assert(pwszDst == (WCHAR *)pwchSlowDst);
        }

        if (paArgInfo[i].fExtraSpace)
            *pwszDst++ = L' ';
        if (paArgInfo[i].fQuoteIt)
            *pwszDst++ = L'"';
    }
    *pwszDst = L'\0';
    *ppwszCommandLine = pwszCmdLine;
    return 0;
}

static int mkWinChildcareWorkerConvertCommandlineWithShell(PWINCHILDCAREWORKER pWorker, const WCHAR *pwszShell, char **papszArgs,
                                                           WCHAR **ppwszCommandLine)
{
    MkWinChildError(pWorker, 1, "%s: not found!\n", papszArgs[0]);
//__debugbreak();
    return ERROR_FILE_NOT_FOUND;
}

/**
 * Searches the environment block for the PATH variable.
 *
 * @returns Pointer to the path in the block or "." in pwszPathFallback.
 * @param   pwszzEnv            The UTF-16 environment block to search.
 * @param   pwszPathFallback    Fallback.
 */
static const WCHAR *mkWinChildcareWorkerFindPathValue(const WCHAR *pwszzEnv, WCHAR pwszPathFallback[4])
{
    while (*pwszzEnv)
    {
        size_t cwcVar = wcslen(pwszzEnv);
        if (!IS_PATH_ENV_VAR(cwcVar, pwszzEnv))
            pwszzEnv += cwcVar + 1;
        else if (cwcVar > 5)
            return &pwszzEnv[5];
        else
            break;
    }
    pwszPathFallback[0] = L'.';
    pwszPathFallback[1] = L'\0';
    return pwszPathFallback;
}

/**
 * Checks if we need to had this executable file to the shell.
 *
 * @returns TRUE if it's shell fooder, FALSE if we think windows can handle it.
 * @param   hFile               Handle to the file in question
 */
static BOOL mkWinChildcareWorkerCheckIfNeedShell(HANDLE hFile)
{
    /*
     * Read the first 512 bytes and check for an executable image header.
     */
    union
    {
        DWORD dwSignature;
        WORD  wSignature;
        BYTE  ab[128];
    } uBuf;
    DWORD cbRead;
    uBuf.dwSignature = 0;
    if (   ReadFile(hFile, &uBuf, sizeof(uBuf), &cbRead, NULL /*pOverlapped*/)
        && cbRead == sizeof(uBuf))
    {
        if (uBuf.wSignature == IMAGE_DOS_SIGNATURE)
            return FALSE;
        if (uBuf.dwSignature == IMAGE_NT_SIGNATURE)
            return FALSE;
        if (   uBuf.wSignature == IMAGE_OS2_SIGNATURE    /* NE */
            || uBuf.wSignature == 0x5d4c                 /* LX */
            || uBuf.wSignature == IMAGE_OS2_SIGNATURE_LE /* LE */)
            return FALSE;
    }
    return TRUE;
}

/**
 * Checks if the image path looks like microsoft CL.exe.
 *
 * @returns TRUE / FALSE.
 * @param   pwszImagePath   The executable image path to evalutate.
 * @param   cwcImagePath    The length of the image path.
 */
static BOOL mkWinChildIsProbableClExe(WCHAR const *pwszImagePath, size_t cwcImagePath)
{
    assert(pwszImagePath[cwcImagePath] == '\0');
    return cwcImagePath > 7
        && (pwszImagePath[cwcImagePath - 7] == L'/' || pwszImagePath[cwcImagePath - 7] == L'\\')
        && (pwszImagePath[cwcImagePath - 6] == L'c' || pwszImagePath[cwcImagePath - 6] == L'C')
        && (pwszImagePath[cwcImagePath - 5] == L'l' || pwszImagePath[cwcImagePath - 5] == L'L')
        &&  pwszImagePath[cwcImagePath - 4] == L'.'
        && (pwszImagePath[cwcImagePath - 3] == L'e' || pwszImagePath[cwcImagePath - 3] == L'E')
        && (pwszImagePath[cwcImagePath - 2] == L'x' || pwszImagePath[cwcImagePath - 2] == L'X')
        && (pwszImagePath[cwcImagePath - 1] == L'e' || pwszImagePath[cwcImagePath - 1] == L'E');
}

/**
 * Temporary workaround for seemingly buggy kFsCache.c / dir-nt-bird.c.
 *
 * Something is not invalidated / updated correctly!
 */
static BOOL mkWinChildcareWorkerIsRegularFileW(PWINCHILDCAREWORKER pWorker, wchar_t const *pwszPath)
{
    BOOL fRet = FALSE;
#ifdef KMK
    if (utf16_regular_file_p(pwszPath))
        fRet = TRUE;
    else
#endif
    {
        /* Don't believe the cache. */
        DWORD dwAttr = GetFileAttributesW(pwszPath);
        if (dwAttr != INVALID_FILE_ATTRIBUTES)
        {
            if (!(dwAttr & FILE_ATTRIBUTE_DIRECTORY))
            {
#ifdef KMK
                extern void dir_cache_invalid_volatile(void);
                dir_cache_invalid_volatile();
                if (utf16_regular_file_p(pwszPath))
                    MkWinChildError(pWorker, 1, "kFsCache was out of sync! pwszPath=%S\n", pwszPath);
                else
                {
                    dir_cache_invalid_all();
                    if (utf16_regular_file_p(pwszPath))
                        MkWinChildError(pWorker, 1, "kFsCache was really out of sync! pwszPath=%S\n", pwszPath);
                    else
                        MkWinChildError(pWorker, 1, "kFsCache is really out of sync!! pwszPath=%S\n", pwszPath);
                }
#endif
                fRet = TRUE;
            }
        }
    }
    return fRet;
}


/**
 * Tries to locate the image file, searching the path and maybe falling back on
 * the shell in case it knows more (think cygwin with its own view of the file
 * system).
 *
 * This will also check for shell script, falling back on the shell too to
 * handle those.
 *
 * @returns 0 on success, windows error code on failure.
 * @param   pWorker         The childcare worker.
 * @param   pszArg0         The first argument.
 * @param   pwszSearchPath  In case mkWinChildcareWorkerConvertEnvironment
 *                          had a chance of locating the search path already.
 * @param   pwszzEnv        The environment block, in case we need to look for
 *                          the path.
 * @param   pszShell        The shell.
 * @param   ppwszImagePath  Where to return the pointer to the image path.  This
 *                          could be the shell.
 * @param   pfNeedShell     Where to return shell vs direct execution indicator.
 * @param   pfProbableClExe Where to return an indicator of probably CL.EXE.
 */
static int mkWinChildcareWorkerFindImage(PWINCHILDCAREWORKER pWorker, char const *pszArg0, WCHAR *pwszSearchPath,
                                         WCHAR const *pwszzEnv, const char *pszShell,
                                         WCHAR **ppwszImagePath, BOOL *pfNeedShell, BOOL *pfProbableClExe)
{
    /** @todo Slap a cache on this code. We usually end up executing the same
     *        stuff over and over again (e.g. compilers, linkers, etc).
     *        Hitting the file system is slow on windows. */

    /*
     * Convert pszArg0 to unicode so we can work directly on that.
     */
    WCHAR     wszArg0[MKWINCHILD_MAX_PATH + 4]; /* +4 for painless '.exe' appending */
    DWORD     dwErr;
    size_t    cbArg0  = strlen(pszArg0) + 1;
    int const cwcArg0 = MultiByteToWideChar(CP_ACP, 0 /*fFlags*/, pszArg0, cbArg0, wszArg0, MKWINCHILD_MAX_PATH);
    if (cwcArg0 > 0)
    {
        HANDLE hFile = INVALID_HANDLE_VALUE;
        WCHAR  wszPathBuf[MKWINCHILD_MAX_PATH + 4]; /* +4 for painless '.exe' appending */
        int    cwc;

        /*
         * If there isn't an .exe suffix, we may have to add one.
         * Also we ASSUME that .exe suffixes means no hash bang detection needed.
         */
        int const fHasExeSuffix = cwcArg0 > CSTRLEN(".exe")
                               &&  wszArg0[cwcArg0 - 4] == '.'
                               && (wszArg0[cwcArg0 - 3] == L'e' || wszArg0[cwcArg0 - 3] == L'E')
                               && (wszArg0[cwcArg0 - 2] == L'x' || wszArg0[cwcArg0 - 2] == L'X')
                               && (wszArg0[cwcArg0 - 1] == L'e' || wszArg0[cwcArg0 - 1] == L'E');

        /*
         * If there isn't any path specified, we need to search the PATH env.var.
         */
        int const fHasPath =  wszArg0[1] == L':'
                           || wszArg0[0] == L'\\'
                           || wszArg0[0] == L'/'
                           || wmemchr(wszArg0, L'/', cwcArg0)
                           || wmemchr(wszArg0, L'\\', cwcArg0);

        /* Before we do anything, flip UNIX slashes to DOS ones. */
        WCHAR *pwc = wszArg0;
        while ((pwc = wcschr(pwc, L'/')) != NULL)
            *pwc++ = L'\\';

        /* Don't need to set these all the time... */
        *pfNeedShell = FALSE;
        *pfProbableClExe = FALSE;

        /*
         * If any kind of path is specified in arg0, we will not search the
         * PATH env.var and can limit ourselves to maybe slapping a .exe on to it.
         */
        if (fHasPath)
        {
            /*
             * If relative to a CWD, turn it into an absolute one.
             */
            unsigned  cwcPath  = cwcArg0;
            WCHAR    *pwszPath = wszArg0;
            if (   *pwszPath != L'\\'
                && (pwszPath[1] != ':' || pwszPath[2] != L'\\') )
            {
                DWORD cwcAbsPath = GetFullPathNameW(wszArg0, MKWINCHILD_MAX_PATH, wszPathBuf, NULL);
                if (cwcAbsPath > 0)
                {
                    cwcPath  = cwcAbsPath + 1; /* include terminator, like MultiByteToWideChar does. */
                    pwszPath = wszPathBuf;
                }
            }

            /*
             * Check with .exe suffix first.
             * We don't open .exe files and look for hash bang stuff, we just
             * assume they are executable images that CreateProcess can deal with.
             */
            if (!fHasExeSuffix)
            {
                pwszPath[cwcPath - 1] = L'.';
                pwszPath[cwcPath    ] = L'e';
                pwszPath[cwcPath + 1] = L'x';
                pwszPath[cwcPath + 2] = L'e';
                pwszPath[cwcPath + 3] = L'\0';
            }

            if (mkWinChildcareWorkerIsRegularFileW(pWorker, pwszPath))
            {
                *pfProbableClExe = mkWinChildIsProbableClExe(pwszPath, cwcPath + 4 - 1);
                return mkWinChildDuplicateUtf16String(pwszPath, cwcPath + 4, ppwszImagePath);
            }

            /*
             * If no suffix was specified, try without .exe too, but now we need
             * to see if it's for the shell or CreateProcess.
             */
            if (!fHasExeSuffix)
            {
                pwszPath[cwcPath - 1] = L'\0';
#ifdef KMK
                if (mkWinChildcareWorkerIsRegularFileW(pWorker, pwszPath))
#endif
                {
                    hFile = CreateFileW(pwszPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE | FILE_SHARE_WRITE,
                                        NULL /*pSecAttr*/, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
                    if (hFile != INVALID_HANDLE_VALUE)
                    {
                        *pfNeedShell = mkWinChildcareWorkerCheckIfNeedShell(hFile);
                        CloseHandle(hFile);
                        if (!*pfNeedShell)
                        {
                            *pfProbableClExe = mkWinChildIsProbableClExe(pwszPath, cwcPath - 1);
                            return mkWinChildDuplicateUtf16String(pwszPath, cwcPath, ppwszImagePath);
                        }
                    }
                }
            }
        }
        /*
         * No path, need to search the PATH env.var. for the executable, maybe
         * adding an .exe suffix while do so if that is missing.
         */
        else
        {
            BOOL  fSearchedCwd = FALSE;
            WCHAR wszPathFallback[4];
            if (!pwszSearchPath)
                pwszSearchPath = (WCHAR *)mkWinChildcareWorkerFindPathValue(pwszzEnv, wszPathFallback);

            for (;;)
            {
                size_t cwcCombined;

                /*
                 * Find the end of the current PATH component.
                 */
                size_t cwcSkip;
                WCHAR  wcEnd;
                size_t cwcComponent = 0;
                WCHAR  wc;
                while ((wc = pwszSearchPath[cwcComponent]) != L'\0')
                {
                    if (wc != ';' && wc != ':')
                    { /* likely */ }
                    else if (wc == ';')
                        break;
                    else if (cwcComponent != (pwszSearchPath[cwcComponent] != L'"' ? 1 : 2))
                        break;
                   cwcComponent++;
                }
                wcEnd = wc;

                /* Trim leading spaces and double quotes. */
                while (   cwcComponent > 0
                       && ((wc = *pwszSearchPath) == L'"' || wc == L' ' || wc == L'\t'))
                {
                    pwszSearchPath++;
                    cwcComponent--;
                }
                cwcSkip = cwcComponent;

                /* Trim trailing spaces & double quotes. */
                while (   cwcComponent > 0
                       && ((wc = pwszSearchPath[cwcComponent - 1]) == L'"' || wc == L' ' || wc == L'\t'))
                    cwcComponent--;

                /*
                 * Skip empty components.  Join the component and the filename, making sure to
                 * resolve any CWD relative stuff first.
                 */
                cwcCombined = cwcComponent + 1 + cwcArg0;
                if (cwcComponent > 0 && cwcCombined <= MKWINCHILD_MAX_PATH)
                {
                    /* Copy the component into wszPathBuf, maybe abspath'ing it. */
                    DWORD  cwcAbsPath = 0;
                    if (   *pwszSearchPath != L'\\'
                        && (pwszSearchPath[1] != ':' || pwszSearchPath[2] != L'\\') )
                    {
                        /* To save an extra buffer + copying, we'll temporarily modify the PATH
                           value in our converted UTF-16 environment block.  */
                        WCHAR const wcSaved = pwszSearchPath[cwcComponent];
                        pwszSearchPath[cwcComponent] = L'\0';
                        cwcAbsPath = GetFullPathNameW(pwszSearchPath, MKWINCHILD_MAX_PATH, wszPathBuf, NULL);
                        pwszSearchPath[cwcComponent] = wcSaved;
                        if (cwcAbsPath > 0 && cwcAbsPath + 1 + cwcArg0 <= MKWINCHILD_MAX_PATH)
                            cwcCombined = cwcAbsPath + 1 + cwcArg0;
                        else
                            cwcAbsPath = 0;
                    }
                    if (cwcAbsPath == 0)
                    {
                        memcpy(wszPathBuf, pwszSearchPath, cwcComponent * sizeof(WCHAR));
                        cwcAbsPath = cwcComponent;
                    }

                    /* Append the filename. */
                    if ((wc = wszPathBuf[cwcAbsPath - 1]) == L'\\' || wc == L'/' || wc == L':')
                    {
                        memcpy(&wszPathBuf[cwcAbsPath], wszArg0, cwcArg0 * sizeof(WCHAR));
                        cwcCombined--;
                    }
                    else
                    {
                        wszPathBuf[cwcAbsPath] = L'\\';
                        memcpy(&wszPathBuf[cwcAbsPath + 1], wszArg0, cwcArg0 * sizeof(WCHAR));
                    }
                    assert(wszPathBuf[cwcCombined - 1] == L'\0');

                    /* DOS slash conversion */
                    pwc = wszPathBuf;
                    while ((pwc = wcschr(pwc, L'/')) != NULL)
                        *pwc++ = L'\\';

                    /*
                     * Search with exe suffix first.
                     */
                    if (!fHasExeSuffix)
                    {
                        wszPathBuf[cwcCombined - 1] = L'.';
                        wszPathBuf[cwcCombined    ] = L'e';
                        wszPathBuf[cwcCombined + 1] = L'x';
                        wszPathBuf[cwcCombined + 2] = L'e';
                        wszPathBuf[cwcCombined + 3] = L'\0';
                    }
                    if (mkWinChildcareWorkerIsRegularFileW(pWorker, wszPathBuf))
                    {
                        *pfProbableClExe = mkWinChildIsProbableClExe(wszPathBuf, cwcCombined + (fHasExeSuffix ? 0 : 4) - 1);
                        return mkWinChildDuplicateUtf16String(wszPathBuf, cwcCombined + (fHasExeSuffix ? 0 : 4), ppwszImagePath);
                    }
                    if (!fHasExeSuffix)
                    {
                        wszPathBuf[cwcCombined - 1] = L'\0';
#ifdef KMK
                        if (mkWinChildcareWorkerIsRegularFileW(pWorker, wszPathBuf))
#endif
                        {
                            /*
                             * Check if the file exists w/o the added '.exe' suffix.  If it does,
                             * we need to check if we can pass it to CreateProcess or need the shell.
                             */
                            hFile = CreateFileW(wszPathBuf, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE | FILE_SHARE_WRITE,
                                                NULL /*pSecAttr*/, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
                            if (hFile != INVALID_HANDLE_VALUE)
                            {
                                *pfNeedShell = mkWinChildcareWorkerCheckIfNeedShell(hFile);
                                CloseHandle(hFile);
                                if (!*pfNeedShell)
                                {
                                    *pfProbableClExe = mkWinChildIsProbableClExe(wszPathBuf, cwcCombined - 1);
                                    return mkWinChildDuplicateUtf16String(wszPathBuf, cwcCombined, ppwszImagePath);
                                }
                                break;
                            }
                        }
                    }
                }

                /*
                 * Advance to the next component.
                 */
                if (wcEnd != '\0')
                    pwszSearchPath += cwcSkip + 1;
                else if (fSearchedCwd)
                    break;
                else
                {
                    fSearchedCwd = TRUE;
                    wszPathFallback[0] = L'.';
                    wszPathFallback[1] = L'\0';
                    pwszSearchPath = wszPathFallback;
                }
            }
        }

        /*
         * We need the shell.  It will take care of finding/reporting missing
         * image files and such.
         */
        *pfNeedShell = TRUE;
        if (pszShell)
        {
            cwc = MultiByteToWideChar(CP_ACP, 0 /*fFlags*/, pszShell, strlen(pszShell) + 1, wszPathBuf, MKWINCHILD_MAX_PATH);
            if (cwc > 0)
                return mkWinChildDuplicateUtf16String(wszPathBuf, cwc, ppwszImagePath);
            dwErr = GetLastError();
            MkWinChildError(pWorker, 1, _("MultiByteToWideChar failed to convert shell (%s): %u\n"), pszShell, dwErr);
        }
        else
        {
            MkWinChildError(pWorker, 1, "%s: not found!\n", pszArg0);
            dwErr = ERROR_FILE_NOT_FOUND;
        }
    }
    else
    {
        dwErr = GetLastError();
        MkWinChildError(pWorker, 1, _("MultiByteToWideChar failed to convert argv[0] (%s): %u\n"), pszArg0, dwErr);
    }
    return dwErr == ERROR_INSUFFICIENT_BUFFER ? ERROR_FILENAME_EXCED_RANGE : dwErr;
}

/**
 * Creates the environment block.
 *
 * @returns 0 on success, windows error code on failure.
 * @param   pWorker         The childcare worker if on one, otherwise NULL.
 * @param   papszEnv        The environment vector to convert.
 * @param   cbEnvStrings    The size of the environment strings, iff they are
 *                          sequential in a block.  Otherwise, zero.
 * @param   ppwszEnv        Where to return the pointer to the environment
 *                          block.
 * @param   ppwszSearchPath Where to return the pointer to the path value
 *                          within the environment block.  This will not be set
 *                          if cbEnvStrings is non-zero, more efficient to let
 *                          mkWinChildcareWorkerFindImage() search when needed.
 */
static int mkWinChildcareWorkerConvertEnvironment(PWINCHILDCAREWORKER pWorker, char **papszEnv, size_t cbEnvStrings,
                                                  WCHAR **ppwszEnv, WCHAR const **ppwszSearchPath)
{
    DWORD  dwErr;
    int    cwcRc;
    int    cwcDst;
    WCHAR *pwszzDst;

    *ppwszSearchPath = NULL;

    /*
     * We've got a little optimization here with help from mkWinChildCopyStringArray.
     */
    if (cbEnvStrings)
    {
        cwcDst = cbEnvStrings + 32;
        pwszzDst = (WCHAR *)xmalloc(cwcDst * sizeof(WCHAR));
        cwcRc = MultiByteToWideChar(CP_ACP, 0 /*fFlags*/, papszEnv[0], cbEnvStrings, pwszzDst, cwcDst);
        if (cwcRc != 0)
        {
            *ppwszEnv = pwszzDst;
            return 0;
        }

        /* Resize the allocation and try again. */
        dwErr = GetLastError();
        if (dwErr == ERROR_INSUFFICIENT_BUFFER)
        {
            cwcRc = MultiByteToWideChar(CP_ACP, 0 /*fFlags*/, papszEnv[0], cbEnvStrings, NULL, 0);
            if (cwcRc > 0)
                cwcDst = cwcRc + 32;
            else
                cwcDst *= 2;
            pwszzDst = (WCHAR *)xrealloc(pwszzDst, cwcDst);
            cwcRc = MultiByteToWideChar(CP_ACP, 0 /*fFlags*/, papszEnv[0], cbEnvStrings, pwszzDst, cwcDst);
            if (cwcRc != 0)
            {
                *ppwszEnv = pwszzDst;
                return 0;
            }
            dwErr = GetLastError();
        }
        MkWinChildError(pWorker, 1, _("MultiByteToWideChar failed to convert environment block: %u\n"), dwErr);
    }
    /*
     * Need to convert it string by string.
     */
    else
    {
        size_t offPathValue = ~(size_t)0;
        size_t offDst;

        /*
         * Estimate the size first.
         */
        size_t      cEnvVars;
        size_t      cwcDst = 32;
        size_t      iVar   = 0;
        const char *pszSrc;
        while ((pszSrc = papszEnv[iVar]) != NULL)
        {
            cwcDst += strlen(pszSrc) + 1;
            iVar++;
        }
        cEnvVars = iVar;

        /* Allocate estimated WCHARs and convert the variables one by one, reallocating
           the block as needed. */
        pwszzDst = (WCHAR *)xmalloc(cwcDst * sizeof(WCHAR));
        cwcDst--; /* save one wchar for the terminating empty string. */
        offDst = 0;
        for (iVar = 0; iVar < cEnvVars; iVar++)
        {
            size_t       cwcLeft = cwcDst - offDst;
            size_t const cbSrc   = strlen(pszSrc = papszEnv[iVar]) + 1;
            assert(cwcDst >= offDst);


            cwcRc = MultiByteToWideChar(CP_ACP, 0 /*fFlags*/, pszSrc, cbSrc, &pwszzDst[offDst], cwcLeft);
            if (cwcRc > 0)
            { /* likely */ }
            else
            {
                dwErr = GetLastError();
                if (dwErr == ERROR_INSUFFICIENT_BUFFER)
                {
                    /* Need more space.  So, calc exacly how much and resize the block accordingly. */
                    size_t cbSrc2 = cbSrc;
                    size_t iVar2  = iVar;
                    cwcLeft = 1;
                    for (;;)
                    {
                        size_t cwcRc2 = MultiByteToWideChar(CP_ACP, 0 /*fFlags*/, pszSrc, cbSrc, NULL, 0);
                        if (cwcRc2 > 0)
                            cwcLeft += cwcRc2;
                        else
                            cwcLeft += cbSrc * 4;

                        /* advance */
                        iVar2++;
                        if (iVar2 >= cEnvVars)
                            break;
                        pszSrc = papszEnv[iVar2];
                        cbSrc2 = strlen(pszSrc) + 1;
                    }
                    pszSrc = papszEnv[iVar];

                    /* Grow the allocation and repeat the conversion. */
                    if (offDst + cwcLeft > cwcDst + 1)
                    {
                        cwcDst   = offDst + cwcLeft;
                        pwszzDst = (WCHAR *)xrealloc(pwszzDst, cwcDst * sizeof(WCHAR));
                        cwcDst--; /* save one wchar for the terminating empty string. */
                        cwcRc = MultiByteToWideChar(CP_ACP, 0 /*fFlags*/, pszSrc, cbSrc, &pwszzDst[offDst], cwcLeft - 1);
                        if (cwcRc <= 0)
                            dwErr = GetLastError();
                    }
                }
                if (cwcRc <= 0)
                {
                    MkWinChildError(pWorker, 1, _("MultiByteToWideChar failed to convert environment string #%u (%s): %u\n"),
                                    iVar, pszSrc, dwErr);
                    free(pwszzDst);
                    return dwErr;
                }
            }

            /* Look for the PATH. */
            if (   offPathValue == ~(size_t)0
                && IS_PATH_ENV_VAR(cwcRc, &pwszzDst[offDst]) )
                offPathValue = offDst + 4 + 1;

            /* Advance. */
            offDst += cwcRc;
        }
        pwszzDst[offDst++] = '\0';

        if (offPathValue != ~(size_t)0)
            *ppwszSearchPath = &pwszzDst[offPathValue];
        *ppwszEnv = pwszzDst;
        return 0;
    }
    free(pwszzDst);
    return dwErr;
}

/**
 * Childcare worker: handle regular process.
 *
 * @param   pWorker             The worker.
 * @param   pChild              The kSubmit child.
 */
static void mkWinChildcareWorkerThreadHandleProcess(PWINCHILDCAREWORKER pWorker, PWINCHILD pChild)
{
    WCHAR  *pwszSearchPath   = NULL;
    WCHAR  *pwszzEnvironment = NULL;
    WCHAR  *pwszCommandLine  = NULL;
    WCHAR  *pwszImageName    = NULL;
    BOOL    fNeedShell       = FALSE;
    BOOL    fProbableClExe   = FALSE;
    int     rc;

    /*
     * First we convert the environment so we get the PATH we need to
     * search for the executable.
     */
    rc = mkWinChildcareWorkerConvertEnvironment(pWorker, pChild->u.Process.papszEnv ? pChild->u.Process.papszEnv : environ,
                                                pChild->u.Process.cbEnvStrings,
                                                &pwszzEnvironment, &pwszSearchPath);
    /*
     * Find the executable and maybe checking if it's a shell script, then
     * convert it to a command line.
     */
    if (rc == 0)
        rc = mkWinChildcareWorkerFindImage(pWorker, pChild->u.Process.papszArgs[0], pwszSearchPath, pwszzEnvironment,
                                           pChild->u.Process.pszShell, &pwszImageName, &fNeedShell, &pChild->fProbableClExe);
    if (rc == 0)
    {
        if (!fNeedShell)
            rc = mkWinChildcareWorkerConvertCommandline(pWorker, pChild->u.Process.papszArgs, 0 /*fFlags*/, &pwszCommandLine);
        else
            rc = mkWinChildcareWorkerConvertCommandlineWithShell(pWorker, pwszImageName, pChild->u.Process.papszArgs,
                                                                 &pwszCommandLine);

        /*
         * Create the child process.
         */
        if (rc == 0)
        {
            BOOL afReplace[3] = { FALSE, pChild->u.Process.hStdOut != INVALID_HANDLE_VALUE, pChild->u.Process.hStdErr != INVALID_HANDLE_VALUE };
            HANDLE ahChild[3] = { INVALID_HANDLE_VALUE, pChild->u.Process.hStdOut, pChild->u.Process.hStdErr };
            rc = mkWinChildcareWorkerCreateProcess(pWorker, pwszImageName, pwszCommandLine, pwszzEnvironment,
                                                   NULL /*pwszCwd*/, afReplace, ahChild, pChild->u.Process.fCatchOutput,
                                                   &pChild->u.Process.hProcess);
            mkWinChildcareWorkerCloseStandardHandles(pChild);
            if (rc == 0)
            {
                /*
                 * Wait for the child to complete.
                 */
                mkWinChildcareWorkerWaitForProcess(pWorker, pChild, pChild->u.Process.hProcess, pwszImageName,
                                                   pChild->u.Process.fCatchOutput);
            }
            else
                pChild->iExitCode = rc;
        }
        else
            pChild->iExitCode = rc;
    }
    else
        pChild->iExitCode = rc;
    free(pwszCommandLine);
    free(pwszImageName);
    free(pwszzEnvironment);

    /* In case we failed, we must make sure the child end of pipes
       used by $(shell no_such_command.exe) are closed, otherwise
       the main thread will be stuck reading the parent end.  */
    mkWinChildcareWorkerCloseStandardHandles(pChild);
}

#ifdef KMK

/**
 * Childcare worker: handle builtin command.
 *
 * @param   pWorker             The worker.
 * @param   pChild              The kSubmit child.
 */
static void mkWinChildcareWorkerThreadHandleBuiltIn(PWINCHILDCAREWORKER pWorker, PWINCHILD pChild)
{
    PCKMKBUILTINENTRY pBuiltIn = pChild->u.BuiltIn.pBuiltIn;
    KMKBUILTINCTX Ctx =
    {
        pBuiltIn->uName.s.sz,
        pChild->pMkChild ? &pChild->pMkChild->output : NULL,
        pWorker,
    };
    if (pBuiltIn->uFnSignature == FN_SIG_MAIN)
        pChild->iExitCode = pBuiltIn->u.pfnMain(pChild->u.BuiltIn.cArgs, pChild->u.BuiltIn.papszArgs,
                                                pChild->u.BuiltIn.papszEnv, &Ctx);
    else if (pBuiltIn->uFnSignature == FN_SIG_MAIN_SPAWNS)
        pChild->iExitCode = pBuiltIn->u.pfnMainSpawns(pChild->u.BuiltIn.cArgs, pChild->u.BuiltIn.papszArgs,
                                                      pChild->u.BuiltIn.papszEnv, &Ctx, pChild->pMkChild, NULL /*pPid*/);
    else
    {
        assert(0);
        pChild->iExitCode = 98;
    }
}

/**
 * Childcare worker: handle append write-out.
 *
 * @param   pWorker             The worker.
 * @param   pChild              The kSubmit child.
 */
static void mkWinChildcareWorkerThreadHandleAppend(PWINCHILDCAREWORKER pWorker, PWINCHILD pChild)
{
    int fd = open(pChild->u.Append.pszFilename,
                  pChild->u.Append.fTruncate
                  ? O_WRONLY | O_TRUNC  | O_CREAT | _O_NOINHERIT | _O_BINARY
                  : O_WRONLY | O_APPEND | O_CREAT | _O_NOINHERIT | _O_BINARY,
                  0666);
    if (fd >= 0)
    {
        ssize_t cbWritten = write(fd, pChild->u.Append.pszAppend, pChild->u.Append.cbAppend);
        if (cbWritten == (ssize_t)pChild->u.Append.cbAppend)
        {
            if (close(fd) >= 0)
            {
                pChild->iExitCode = 0;
                return;
            }
            MkWinChildError(pWorker, 1, "kmk_builtin_append: close failed on '%s': %u (%s)\n",
                            pChild->u.Append.pszFilename, errno, strerror(errno));
        }
        else
            MkWinChildError(pWorker, 1, "kmk_builtin_append: error writing %lu bytes to on '%s': %u (%s)\n",
                            pChild->u.Append.cbAppend, pChild->u.Append.pszFilename, errno, strerror(errno));
        close(fd);
    }
    else
        MkWinChildError(pWorker, 1, "kmk_builtin_append: error opening '%s': %u (%s)\n",
                        pChild->u.Append.pszFilename, errno, strerror(errno));
    pChild->iExitCode = 1;
}

/**
 * Childcare worker: handle kSubmit job.
 *
 * @param   pWorker             The worker.
 * @param   pChild              The kSubmit child.
 */
static void mkWinChildcareWorkerThreadHandleSubmit(PWINCHILDCAREWORKER pWorker, PWINCHILD pChild)
{
    void  *pvSubmitWorker = pChild->u.Submit.pvSubmitWorker;

    /*
     * Prep the wait handles.
     */
    HANDLE ahHandles[3]   = { pChild->u.Submit.hEvent, NULL, NULL };
    DWORD  cHandles       = 1;
    if (pChild->u.Submit.pStdOut)
    {
        assert(pChild->u.Submit.pStdErr);
        pChild->u.Submit.pStdOut->fHaveWrittenOut = FALSE;
        ahHandles[cHandles++] = pChild->u.Submit.pStdOut->hEvent;
        pChild->u.Submit.pStdErr->fHaveWrittenOut = FALSE;
        ahHandles[cHandles++] = pChild->u.Submit.pStdErr->hEvent;
    }

    /*
     * Wait loop.
     */
    for (;;)
    {
        int   iExitCode = -42;
        int   iSignal   = -1;
        DWORD dwStatus;
        if (cHandles == 1)
            dwStatus = WaitForSingleObject(ahHandles[0], INFINITE);
        else
        {
            dwStatus = WaitForMultipleObjects(cHandles, ahHandles, FALSE /*fWaitAll*/, INFINITE);
            assert(dwStatus != WAIT_FAILED);
            if (dwStatus == WAIT_OBJECT_0 + 1)
                mkWinChildcareWorkerCatchOutput(pChild, pChild->u.Submit.pStdOut, FALSE /*fDraining*/);
            else if (dwStatus == WAIT_OBJECT_0 + 2)
                mkWinChildcareWorkerCatchOutput(pChild, pChild->u.Submit.pStdErr, FALSE /*fDraining*/);
        }
        if (kSubmitSubProcGetResult((intptr_t)pvSubmitWorker, dwStatus == WAIT_OBJECT_0 /*fBlock*/, &iExitCode, &iSignal) == 0)
        {
            if (pChild->u.Submit.pStdOut)
                MkWinChildcareWorkerDrainPipes(pChild, pChild->u.Submit.pStdOut, pChild->u.Submit.pStdErr);

            pChild->iExitCode = iExitCode;
            pChild->iSignal   = iSignal;
            /* Cleanup must be done on the main thread. */
            return;
        }

        if (pChild->iSignal != 0)
            kSubmitSubProcKill((intptr_t)pvSubmitWorker, pChild->iSignal);
    }
}

/**
 * Childcare worker: handle kmk_redirect process.
 *
 * @param   pWorker             The worker.
 * @param   pChild              The redirect child.
 */
static void mkWinChildcareWorkerThreadHandleRedirect(PWINCHILDCAREWORKER pWorker, PWINCHILD pChild)
{
    mkWinChildcareWorkerWaitForProcess(pWorker, pChild, pChild->u.Redirect.hProcess, L"kmk_redirect", FALSE /*fCatchOutput*/);
}

#endif /* KMK */

/**
 * Childcare worker thread.
 *
 * @returns 0
 * @param   pvUser              The worker instance.
 */
static unsigned int __stdcall mkWinChildcareWorkerThread(void *pvUser)
{
    PWINCHILDCAREWORKER pWorker = (PWINCHILDCAREWORKER)pvUser;
    assert(pWorker->uMagic == WINCHILDCAREWORKER_MAGIC);

#ifdef MKWINCHILD_DO_SET_PROCESSOR_GROUP
    /*
     * Adjust process group if necessary.
     *
     * Note! It seems that setting the mask to zero means that we select all
     *       active processors.  Couldn't find any alternative API for getting
     *       the correct active processor mask.
     */
    if (g_cProcessorGroups > 1)
    {
        GROUP_AFFINITY Affinity = { 0 /* == all active apparently */, pWorker->iProcessorGroup, { 0, 0, 0 } };
        BOOL fRet = g_pfnSetThreadGroupAffinity(GetCurrentThread(), &Affinity, NULL);
        assert(fRet); (void)fRet;
# ifndef NDEBUG
        {
            GROUP_AFFINITY ActualAffinity = { 0xbeefbeefU, 0xbeef, { 0xbeef, 0xbeef, 0xbeef } };
            fRet = GetThreadGroupAffinity(GetCurrentThread(), &ActualAffinity);
            assert(fRet); (void)fRet;
            assert(ActualAffinity.Group == pWorker->iProcessorGroup);
        }
# endif
    }
#endif

    /*
     * Work loop.
     */
    while (!g_fShutdown)
    {
        /*
         * Try go idle.
         */
        PWINCHILD pChild = pWorker->pTailTodoChildren;
        if (!pChild)
        {
            _InterlockedExchange(&pWorker->fIdle, TRUE);
            pChild = pWorker->pTailTodoChildren;
            if (!pChild)
            {
                DWORD dwStatus;

                _InterlockedIncrement((long *)&g_cIdleChildcareWorkers);
                _InterlockedExchange((long *)&g_idxLastChildcareWorker, pWorker->idxWorker);
                dwStatus = WaitForSingleObject(pWorker->hEvtIdle, INFINITE);
                _InterlockedExchange(&pWorker->fIdle, FALSE);
                _InterlockedDecrement((long *)&g_cIdleChildcareWorkers);

                assert(dwStatus != WAIT_FAILED);
                if (dwStatus == WAIT_FAILED)
                    Sleep(20);

                pChild = pWorker->pTailTodoChildren;
            }
            else
                _InterlockedExchange(&pWorker->fIdle, FALSE);
        }
        if (pChild)
        {
            /*
             * We got work to do.  First job is to deque the job.
             */
            pChild = mkWinChildDequeFromLifo(&pWorker->pTailTodoChildren, pChild);
            assert(pChild);
            if (pChild)
            {
                PWINCHILD pTailExpect;

                pChild->pWorker = pWorker;
                pWorker->pCurChild = pChild;
                switch (pChild->enmType)
                {
                    case WINCHILDTYPE_PROCESS:
                        mkWinChildcareWorkerThreadHandleProcess(pWorker, pChild);
                        break;
#ifdef KMK
                    case WINCHILDTYPE_BUILT_IN:
                        mkWinChildcareWorkerThreadHandleBuiltIn(pWorker, pChild);
                        break;
                    case WINCHILDTYPE_APPEND:
                        mkWinChildcareWorkerThreadHandleAppend(pWorker, pChild);
                        break;
                    case WINCHILDTYPE_SUBMIT:
                        mkWinChildcareWorkerThreadHandleSubmit(pWorker, pChild);
                        break;
                    case WINCHILDTYPE_REDIRECT:
                        mkWinChildcareWorkerThreadHandleRedirect(pWorker, pChild);
                        break;
#endif
                    default:
                        assert(0);
                }
                pWorker->pCurChild = NULL;
                pChild->pWorker = NULL;

                /*
                 * Move the child to the completed list.
                 */
                pTailExpect = NULL;
                for (;;)
                {
                    PWINCHILD pTailActual;
                    pChild->pNext = pTailExpect;
                    pTailActual = _InterlockedCompareExchangePointer(&g_pTailCompletedChildren, pChild, pTailExpect);
                    if (pTailActual != pTailExpect)
                        pTailExpect = pTailActual;
                    else
                    {
                        _InterlockedDecrement(&g_cPendingChildren);
                        if (pTailExpect)
                            break;
                        if (SetEvent(g_hEvtWaitChildren))
                            break;
                        MkWinChildError(pWorker, 1, "SetEvent(g_hEvtWaitChildren=%p) failed: %u\n",
                                        g_hEvtWaitChildren, GetLastError());
                        break;
                    }
                }
            }
        }
    }

    _endthreadex(0);
    return 0;
}

/**
 * Creates a pipe for catching child output.
 *
 * This is a custom CreatePipe implementation that allows for overlapped I/O on
 * our end of the pipe.  Silly that they don't offer an API that does this.
 *
 * @returns The pipe that was created. NULL on failure.
 * @param   pPipe               The structure for the pipe.
 * @param   iWhich              Which standard descriptor this is a pipe for.
 * @param   idxWorker           The worker index.
 */
PWINCCWPIPE MkWinChildcareCreateWorkerPipe(unsigned iWhich, unsigned int idxWorker)
{
    /*
     * We try generate a reasonably unique name from the get go, so this retry
     * loop shouldn't really ever be needed.  But you never know.
     */
    static unsigned s_iSeqNo = 0;
    DWORD const     cMaxInstances = 1;
    DWORD const     cbPipe        = 4096;
    DWORD const     cMsTimeout    = 0;
    unsigned        cTries        = 256;
    while (cTries-- > 0)
    {
        /* Create the pipe (our end). */
        HANDLE hPipeRead;
        DWORD  fOpenMode = PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE;
        DWORD  fPipeMode = PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS;
        WCHAR  wszName[MAX_PATH];
        s_iSeqNo++;
        _snwprintf(wszName, MAX_PATH, L"\\\\.\\pipe\\kmk-winchildren-%u-%u-%u-%s-%u-%u",
                   GetCurrentProcessId(), GetCurrentThreadId(), idxWorker, iWhich == 1 ? L"out" : L"err", s_iSeqNo, GetTickCount());
        hPipeRead = CreateNamedPipeW(wszName, fOpenMode, fPipeMode, cMaxInstances, cbPipe, cbPipe, cMsTimeout, NULL /*pSecAttr*/);
        if (hPipeRead == INVALID_HANDLE_VALUE && GetLastError() == ERROR_INVALID_PARAMETER)
        {
            fOpenMode &= ~FILE_FLAG_FIRST_PIPE_INSTANCE;
            fPipeMode &= ~PIPE_REJECT_REMOTE_CLIENTS;
            hPipeRead = CreateNamedPipeW(wszName, fOpenMode, fPipeMode, cMaxInstances, cbPipe, cbPipe, cMsTimeout, NULL /*pSecAttr*/);
        }
        if (hPipeRead != INVALID_HANDLE_VALUE)
        {
            /* Connect the other end. */
            HANDLE hPipeWrite = CreateFileW(wszName, GENERIC_WRITE | FILE_READ_ATTRIBUTES, 0 /*fShareMode*/, NULL /*pSecAttr*/,
                                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL /*hTemplateFile*/);
            if (hPipeWrite != INVALID_HANDLE_VALUE)
            {
                /*
                 * Create the event object and we're done.
                 *
                 * It starts in signalled stated so we don't need special code
                 * for handing when we start waiting.
                 */
                HANDLE hEvent = CreateEventW(NULL /*pSecAttr*/, TRUE /*fManualReset*/, TRUE /*fInitialState*/, NULL /*pwszName*/);
                if (hEvent != NULL)
                {
                    PWINCCWPIPE pPipe = (PWINCCWPIPE)xcalloc(sizeof(*pPipe));
                    pPipe->hPipeMine    = hPipeRead;
                    pPipe->hPipeChild   = hPipeWrite;
                    pPipe->hEvent       = hEvent;
                    pPipe->iWhich       = iWhich;
                    pPipe->fReadPending = FALSE;
                    pPipe->cbBuffer     = cbPipe;
                    pPipe->pbBuffer     = xcalloc(cbPipe);
                    return pPipe;
                }

                CloseHandle(hPipeWrite);
                CloseHandle(hPipeRead);
                return NULL;
            }
            CloseHandle(hPipeRead);
        }
    }
    return NULL;
}

/**
 * Destroys a childcare worker pipe.
 *
 * @param   pPipe       The pipe.
 */
void MkWinChildcareDeleteWorkerPipe(PWINCCWPIPE pPipe)
{
    if (pPipe->hPipeChild != NULL)
    {
        CloseHandle(pPipe->hPipeChild);
        pPipe->hPipeChild = NULL;
    }

    if (pPipe->hPipeMine != NULL)
    {
        if (pPipe->fReadPending)
            if (!CancelIo(pPipe->hPipeMine))
                WaitForSingleObject(pPipe->hEvent, INFINITE);
        CloseHandle(pPipe->hPipeMine);
        pPipe->hPipeMine = NULL;
    }

    if (pPipe->hEvent != NULL)
    {
        CloseHandle(pPipe->hEvent);
        pPipe->hEvent = NULL;
    }

    if (pPipe->pbBuffer)
    {
        free(pPipe->pbBuffer);
        pPipe->pbBuffer = NULL;
    }
}

/**
 * Initializes the processor group allocator.
 *
 * @param   pState              The allocator to initialize.
 */
void MkWinChildInitCpuGroupAllocator(PMKWINCHILDCPUGROUPALLOCSTATE pState)
{
    /* We shift the starting group with the make nesting level as part of
       our very simple distribution strategy. */
    pState->idxGroup = makelevel;
    pState->idxProcessorInGroup = 0;
}

/**
 * Allocate CPU group for the next child process.
 *
 * @returns CPU group.
 * @param   pState              The allocator state.  Must be initialized by
 *                              MkWinChildInitCpuGroupAllocator().
 */
unsigned int MkWinChildAllocateCpuGroup(PMKWINCHILDCPUGROUPALLOCSTATE pState)
{
    unsigned int iGroup = 0;
    if (g_cProcessorGroups > 1)
    {
        unsigned int cMaxInGroup;
        unsigned int cInGroup;

        iGroup = pState->idxGroup % g_cProcessorGroups;

        /* Advance.  We employ a very simple strategy that does 50% in
           each group for each group cycle.  Odd processor counts are
           caught in odd group cycles.  The init function selects the
           starting group based on make nesting level to avoid stressing
           out the first group. */
        cInGroup = ++pState->idxProcessorInGroup;
        cMaxInGroup = g_pacProcessorsInGroup[iGroup];
        if (   !(cMaxInGroup & 1)
            || !((pState->idxGroup / g_cProcessorGroups) & 1))
            cMaxInGroup /= 2;
        else
            cMaxInGroup = cMaxInGroup / 2 + 1;
        if (cInGroup >= cMaxInGroup)
        {
            pState->idxProcessorInGroup = 0;
            pState->idxGroup++;
        }
    }
    return iGroup;
}

/**
 * Creates another childcare worker.
 *
 * @returns The new worker, if we succeeded.
 */
static PWINCHILDCAREWORKER mkWinChildcareCreateWorker(void)
{
    PWINCHILDCAREWORKER pWorker = (PWINCHILDCAREWORKER)xcalloc(sizeof(*pWorker));
    pWorker->uMagic    = WINCHILDCAREWORKER_MAGIC;
    pWorker->idxWorker = g_cChildCareworkers;
    pWorker->hEvtIdle  = CreateEventW(NULL, FALSE /*fManualReset*/, FALSE /*fInitialState*/, NULL /*pwszName*/);
    if (pWorker->hEvtIdle)
    {
        pWorker->pStdOut = MkWinChildcareCreateWorkerPipe(1, pWorker->idxWorker);
        if (pWorker->pStdOut)
        {
            pWorker->pStdErr = MkWinChildcareCreateWorkerPipe(2, pWorker->idxWorker);
            if (pWorker->pStdErr)
            {
                /* Before we start the thread, assign it to a processor group. */
                pWorker->iProcessorGroup = MkWinChildAllocateCpuGroup(&g_ProcessorGroupAllocator);

                /* Try start the thread. */
                pWorker->hThread = (HANDLE)_beginthreadex(NULL, 0 /*cbStack*/, mkWinChildcareWorkerThread, pWorker,
                                                          0, &pWorker->tid);
                if (pWorker->hThread != NULL)
                {
                    pWorker->idxWorker = g_cChildCareworkers++; /* paranoia */
                    g_papChildCareworkers[pWorker->idxWorker] = pWorker;
                    return pWorker;
                }

                /* Bail out! */
                ONS (error, NILF, "_beginthreadex failed: %u (%s)\n", errno, strerror(errno));
                MkWinChildcareDeleteWorkerPipe(pWorker->pStdErr);
            }
            else
                ON (error, NILF, "Failed to create stderr pipe: %u\n", GetLastError());
            MkWinChildcareDeleteWorkerPipe(pWorker->pStdOut);
        }
        else
            ON (error, NILF, "Failed to create stdout pipe: %u\n", GetLastError());
        CloseHandle(pWorker->hEvtIdle);
    }
    else
        ON (error, NILF, "CreateEvent failed: %u\n", GetLastError());
    pWorker->uMagic = ~WINCHILDCAREWORKER_MAGIC;
    free(pWorker);
    return NULL;
}

/**
 * Helper for copying argument and environment vectors.
 *
 * @returns Single alloc block copy.
 * @param   papszSrc    The source vector.
 * @param   pcbStrings  Where to return the size of the strings & terminator.
 */
static char **mkWinChildCopyStringArray(char **papszSrc, size_t *pcbStrings)
{
    const char *psz;
    char      **papszDstArray;
    char       *pszDstStr;
    size_t      i;

    /* Calc sizes first. */
    size_t      cbStrings = 1; /* (one extra for terminator string) */
    size_t      cStrings = 0;
    while ((psz = papszSrc[cStrings]) != NULL)
    {
        cbStrings += strlen(psz) + 1;
        cStrings++;
    }
    *pcbStrings = cbStrings;

    /* Allocate destination. */
    papszDstArray = (char **)xmalloc(cbStrings + (cStrings + 1) * sizeof(papszDstArray[0]));
    pszDstStr = (char *)&papszDstArray[cStrings + 1];

    /* Copy it. */
    for (i = 0; i < cStrings; i++)
    {
        const char *pszSource = papszSrc[i];
        size_t      cchString = strlen(pszSource);
        papszDstArray[i] = pszDstStr;
        memcpy(pszDstStr, pszSource, cchString);
        pszDstStr += cchString;
        *pszDstStr++ = '\0';
    }
    *pszDstStr = '\0';
    assert(&pszDstStr[1] - papszDstArray[0] == cbStrings);
    papszDstArray[i] = NULL;
    return papszDstArray;
}

/**
 * Allocate and init a WINCHILD.
 *
 * @returns The new windows child structure.
 * @param   enmType         The child type.
 */
static PWINCHILD mkWinChildNew(WINCHILDTYPE enmType)
{
    PWINCHILD pChild = xcalloc(sizeof(*pChild));
    pChild->enmType     = enmType;
    pChild->fCoreDumped = 0;
    pChild->iSignal     = 0;
    pChild->iExitCode   = 222222;
    pChild->uMagic      = WINCHILD_MAGIC;
    pChild->pid         = (intptr_t)pChild;
    return pChild;
}

/**
 * Destructor for WINCHILD.
 *
 * @param   pChild              The child structure to destroy.
 */
static void mkWinChildDelete(PWINCHILD pChild)
{
    assert(pChild->uMagic == WINCHILD_MAGIC);
    pChild->uMagic = ~WINCHILD_MAGIC;

    switch (pChild->enmType)
    {
        case WINCHILDTYPE_PROCESS:
        {
            if (pChild->u.Process.papszArgs)
            {
                free(pChild->u.Process.papszArgs);
                pChild->u.Process.papszArgs = NULL;
            }
            if (pChild->u.Process.cbEnvStrings && pChild->u.Process.papszEnv)
            {
                free(pChild->u.Process.papszEnv);
                pChild->u.Process.papszEnv = NULL;
            }
            if (pChild->u.Process.pszShell)
            {
                free(pChild->u.Process.pszShell);
                pChild->u.Process.pszShell = NULL;
            }
            if (pChild->u.Process.hProcess)
            {
                CloseHandle(pChild->u.Process.hProcess);
                pChild->u.Process.hProcess = NULL;
            }
            mkWinChildcareWorkerCloseStandardHandles(pChild);
            break;
        }

#ifdef KMK
        case WINCHILDTYPE_BUILT_IN:
            if (pChild->u.BuiltIn.papszArgs)
            {
                free(pChild->u.BuiltIn.papszArgs);
                pChild->u.BuiltIn.papszArgs = NULL;
            }
            if (pChild->u.BuiltIn.papszEnv)
            {
                free(pChild->u.BuiltIn.papszEnv);
                pChild->u.BuiltIn.papszEnv = NULL;
            }
            break;

        case WINCHILDTYPE_APPEND:
            if (pChild->u.Append.pszFilename)
            {
                free(pChild->u.Append.pszFilename);
                pChild->u.Append.pszFilename = NULL;
            }
            if (pChild->u.Append.pszAppend)
            {
                free(pChild->u.Append.pszAppend);
                pChild->u.Append.pszAppend = NULL;
            }
            break;

        case WINCHILDTYPE_SUBMIT:
            if (pChild->u.Submit.pvSubmitWorker)
            {
                kSubmitSubProcCleanup((intptr_t)pChild->u.Submit.pvSubmitWorker);
                pChild->u.Submit.pvSubmitWorker = NULL;
            }
            break;

        case WINCHILDTYPE_REDIRECT:
            if (pChild->u.Redirect.hProcess)
            {
                CloseHandle(pChild->u.Redirect.hProcess);
                pChild->u.Redirect.hProcess = NULL;
            }
            break;
#endif /* KMK */

        default:
            assert(0);
    }

    free(pChild);
}

/**
 * Queues the child with a worker, creating new workers if necessary.
 *
 * @returns 0 on success, windows error code on failure (child destroyed).
 * @param   pChild          The child.
 * @param   pPid            Where to return the PID (optional).
 */
static int mkWinChildPushToCareWorker(PWINCHILD pChild, pid_t *pPid)
{
    PWINCHILDCAREWORKER pWorker = NULL;
    PWINCHILD pOldChild;
    PWINCHILD pCurChild;

    /*
     * There are usually idle workers around, except for at the start.
     */
    if (g_cIdleChildcareWorkers > 0)
    {
        /*
         * Try the idle hint first and move forward from it.
         */
        unsigned int const cWorkers = g_cChildCareworkers;
        unsigned int       iHint    = g_idxLastChildcareWorker;
        unsigned int       i;
        for (i = iHint; i < cWorkers; i++)
        {
            PWINCHILDCAREWORKER pPossibleWorker = g_papChildCareworkers[i];
            if (pPossibleWorker->fIdle)
            {
                pWorker = pPossibleWorker;
                break;
            }
        }
        if (!pWorker)
        {
            /* Scan from the start. */
            if (iHint > cWorkers)
                iHint = cWorkers;
            for (i = 0; i < iHint; i++)
            {
                PWINCHILDCAREWORKER pPossibleWorker = g_papChildCareworkers[i];
                if (pPossibleWorker->fIdle)
                {
                    pWorker = pPossibleWorker;
                    break;
                }
            }
        }
    }
    if (!pWorker)
    {
        /*
         * Try create more workers if we haven't reached the max yet.
         */
        if (g_cChildCareworkers < g_cChildCareworkersMax)
            pWorker = mkWinChildcareCreateWorker();

        /*
         * Queue it with an existing worker.  Look for one without anthing extra scheduled.
         */
        if (!pWorker)
        {
            unsigned int i = g_cChildCareworkers;
            if (i == 0)
                fatal(NILF, 0, _("Failed to create worker threads for managing child processes!\n"));
            pWorker = g_papChildCareworkers[--i];
            if (pWorker->pTailTodoChildren)
                while (i-- > 0)
                {
                    PWINCHILDCAREWORKER pPossibleWorker = g_papChildCareworkers[i];
                    if (!pPossibleWorker->pTailTodoChildren)
                    {
                        pWorker = pPossibleWorker;
                        break;
                    }
                }
        }
    }

    /*
     * Do the queueing.
     */
    pOldChild = NULL;
    for (;;)
    {
        pChild->pNext = pOldChild;
        pCurChild = _InterlockedCompareExchangePointer((void **)&pWorker->pTailTodoChildren, pChild, pOldChild);
        if (pCurChild == pOldChild)
        {
            DWORD volatile dwErr;
            _InterlockedIncrement(&g_cPendingChildren);
            if (   !pWorker->fIdle
                || SetEvent(pWorker->hEvtIdle))
            {
                *pPid = pChild->pid;
                return 0;
            }

            _InterlockedDecrement(&g_cPendingChildren);
            dwErr = GetLastError();
            assert(0);
            mkWinChildDelete(pChild);
            return dwErr ? dwErr : -20;
        }
        pOldChild = pCurChild;
    }
}

/**
 * Creates a regular child process (job.c).
 *
 * Will copy the information and push it to a childcare thread that does the
 * actual process creation.
 *
 * @returns 0 on success, windows status code on failure.
 * @param   papszArgs           The arguments.
 * @param   papszEnv            The environment (optional).
 * @param   pszShell            The SHELL variable value (optional).
 * @param   pMkChild            The make child structure (optional).
 * @param   pPid                Where to return the pid.
 */
int MkWinChildCreate(char **papszArgs, char **papszEnv, const char *pszShell, struct child *pMkChild, pid_t *pPid)
{
    PWINCHILD pChild = mkWinChildNew(WINCHILDTYPE_PROCESS);
    pChild->pMkChild = pMkChild;

    pChild->u.Process.papszArgs = mkWinChildCopyStringArray(papszArgs, &pChild->u.Process.cbArgsStrings);
    if (   !papszEnv
        || !pMkChild
        || pMkChild->environment == papszEnv)
    {
        pChild->u.Process.cbEnvStrings = 0;
        pChild->u.Process.papszEnv = papszEnv;
    }
    else
        pChild->u.Process.papszEnv = mkWinChildCopyStringArray(papszEnv, &pChild->u.Process.cbEnvStrings);
    if (pszShell)
        pChild->u.Process.pszShell = xstrdup(pszShell);
    pChild->u.Process.hStdOut = INVALID_HANDLE_VALUE;
    pChild->u.Process.hStdErr = INVALID_HANDLE_VALUE;

    /* We always catch the output in order to prevent character soups courtesy
       of the microsoft CRT and/or linkers writing character by character to the
       console.  Always try write whole lines, even when --output-sync is none. */
    pChild->u.Process.fCatchOutput = TRUE;

    return mkWinChildPushToCareWorker(pChild, pPid);
}

/**
 * Creates a chile process with a pipe hooked up to stdout.
 *
 * @returns 0 on success, non-zero on failure.
 * @param   papszArgs       The argument vector.
 * @param   papszEnv        The environment vector (optional).
 * @param   fdErr           File descriptor to hook up to stderr.
 * @param   pPid            Where to return the pid.
 * @param   pfdReadPipe     Where to return the read end of the pipe.
 */
int MkWinChildCreateWithStdOutPipe(char **papszArgs, char **papszEnv, int fdErr, pid_t *pPid, int *pfdReadPipe)
{
    /*
     * Create the pipe.
     */
    HANDLE hReadPipe;
    HANDLE hWritePipe;
    if (CreatePipe(&hReadPipe, &hWritePipe, NULL, 0 /* default size */))
    {
        //if (SetHandleInformation(hWritePipe, HANDLE_FLAG_INHERIT /* clear */ , HANDLE_FLAG_INHERIT /*set*/))
        {
            int fdReadPipe = _open_osfhandle((intptr_t)hReadPipe, O_RDONLY);
            if (fdReadPipe >= 0)
            {
                PWINCHILD pChild;
                int rc;

                /*
                 * Get a handle for fdErr.  Ignore failure.
                 */
                HANDLE hStdErr = INVALID_HANDLE_VALUE;
                if (fdErr >= 0)
                {
                    HANDLE hNative = (HANDLE)_get_osfhandle(fdErr);
                    if (!DuplicateHandle(GetCurrentProcess(), hNative, GetCurrentProcess(),
                                         &hStdErr, 0 /*DesiredAccess*/, TRUE /*fInherit*/, DUPLICATE_SAME_ACCESS))
                    {
                        ONN(error, NILF, _("DuplicateHandle failed on stderr descriptor (%u): %u\n"), fdErr, GetLastError());
                        hStdErr = INVALID_HANDLE_VALUE;
                    }
                }

                /*
                 * Push it off to the worker thread.
                 */
                pChild = mkWinChildNew(WINCHILDTYPE_PROCESS);
                pChild->u.Process.papszArgs = mkWinChildCopyStringArray(papszArgs, &pChild->u.Process.cbArgsStrings);
                pChild->u.Process.papszEnv  = mkWinChildCopyStringArray(papszEnv ? papszEnv : environ,
                                                                        &pChild->u.Process.cbEnvStrings);
                //if (pszShell)
                //    pChild->u.Process.pszShell = xstrdup(pszShell);
                pChild->u.Process.hStdOut   = hWritePipe;
                pChild->u.Process.hStdErr   = hStdErr;
                pChild->u.Process.fCloseStdErr = TRUE;
                pChild->u.Process.fCloseStdOut = TRUE;

                rc = mkWinChildPushToCareWorker(pChild, pPid);
                if (rc == 0)
                    *pfdReadPipe = fdReadPipe;
                else
                {
                    ON(error, NILF, _("mkWinChildPushToCareWorker failed on pipe: %d\n"), rc);
                    close(fdReadPipe);
                    *pfdReadPipe = -1;
                    *pPid = -1;
                }
                return rc;
            }

            ON(error, NILF, _("_open_osfhandle failed on pipe: %u\n"), errno);
        }
        //else
        //    ON(error, NILF, _("SetHandleInformation failed on pipe: %u\n"), GetLastError());
        if (hReadPipe != INVALID_HANDLE_VALUE)
            CloseHandle(hReadPipe);
        CloseHandle(hWritePipe);
    }
    else
        ON(error, NILF, _("CreatePipe failed: %u\n"), GetLastError());
    *pfdReadPipe = -1;
    *pPid = -1;
    return -1;
}

#ifdef KMK

/**
 * Interface used by kmkbuiltin.c for executing builtin commands on threads.
 *
 * @returns 0 on success, windows status code on failure.
 * @param   pBuiltIn        The kmk built-in command entry.
 * @param   cArgs           The number of arguments in papszArgs.
 * @param   papszArgs       The argument vector.
 * @param   papszEnv        The environment vector, optional.
 * @param   pMkChild        The make child structure.
 * @param   pPid            Where to return the pid.
 */
int MkWinChildCreateBuiltIn(PCKMKBUILTINENTRY pBuiltIn, int cArgs, char **papszArgs, char **papszEnv,
                            struct child *pMkChild, pid_t *pPid)
{
    size_t    cbIgnored;
    PWINCHILD pChild = mkWinChildNew(WINCHILDTYPE_BUILT_IN);
    pChild->pMkChild            = pMkChild;
    pChild->u.BuiltIn.pBuiltIn  = pBuiltIn;
    pChild->u.BuiltIn.cArgs     = cArgs;
    pChild->u.BuiltIn.papszArgs = mkWinChildCopyStringArray(papszArgs, &cbIgnored);
    pChild->u.BuiltIn.papszEnv  = papszEnv ? mkWinChildCopyStringArray(papszEnv, &cbIgnored) : NULL;
    return mkWinChildPushToCareWorker(pChild, pPid);
}

/**
 * Interface used by append.c for do the slow file system part.
 *
 * This will append the given buffer to the specified file and free the buffer.
 *
 * @returns 0 on success, windows status code on failure.
 *
 * @param   pszFilename     The name of the file to append to.
 * @param   ppszAppend      What to append.  The pointer pointed to is set to
 *                          NULL once we've taken ownership of the buffer and
 *                          promise to free it.
 * @param   cbAppend        How much to append.
 * @param   fTruncate       Whether to truncate the file before appending to it.
 * @param   pMkChild        The make child structure.
 * @param   pPid            Where to return the pid.
 */
int MkWinChildCreateAppend(const char *pszFilename, char **ppszAppend, size_t cbAppend, int fTruncate,
                           struct child *pMkChild, pid_t *pPid)
{
    size_t    cbFilename = strlen(pszFilename) + 1;
    PWINCHILD pChild     = mkWinChildNew(WINCHILDTYPE_APPEND);
    pChild->pMkChild            = pMkChild;
    pChild->u.Append.fTruncate  = fTruncate;
    pChild->u.Append.pszAppend  = *ppszAppend;
    pChild->u.Append.cbAppend   = cbAppend;
    pChild->u.Append.pszFilename = (char *)memcpy(xmalloc(cbFilename), pszFilename, cbFilename);
    *ppszAppend = NULL;
    return mkWinChildPushToCareWorker(pChild, pPid);
}

/**
 * Interface used by kSubmit.c for registering stuff to wait on.
 *
 * @returns 0 on success, windows status code on failure.
 * @param   hEvent          The event object handle to wait on.
 * @param   pvSubmitWorker  The argument to pass back to kSubmit to clean up.
 * @param   pStdOut         Standard output pipe for the worker. Optional.
 * @param   pStdErr         Standard error pipe for the worker. Optional.
 * @param   pMkChild        The make child structure.
 * @param   pPid            Where to return the pid.
 */
int MkWinChildCreateSubmit(intptr_t hEvent, void *pvSubmitWorker, PWINCCWPIPE pStdOut, PWINCCWPIPE pStdErr,
                           struct child *pMkChild, pid_t *pPid)
{
    PWINCHILD pChild = mkWinChildNew(WINCHILDTYPE_SUBMIT);
    pChild->pMkChild                = pMkChild;
    pChild->u.Submit.hEvent         = (HANDLE)hEvent;
    pChild->u.Submit.pvSubmitWorker = pvSubmitWorker;
    pChild->u.Submit.pStdOut        = pStdOut;
    pChild->u.Submit.pStdErr        = pStdErr;
    return mkWinChildPushToCareWorker(pChild, pPid);
}

/**
 * Interface used by redirect.c for registering stuff to wait on.
 *
 * @returns 0 on success, windows status code on failure.
 * @param   hProcess        The process object to wait on.
 * @param   pPid            Where to return the pid.
 */
int MkWinChildCreateRedirect(intptr_t hProcess, pid_t *pPid)
{
    PWINCHILD pChild = mkWinChildNew(WINCHILDTYPE_REDIRECT);
    pChild->u.Redirect.hProcess = (HANDLE)hProcess;
    return mkWinChildPushToCareWorker(pChild, pPid);
}


/**
 * New interface used by redirect.c for spawning and waitin on a child.
 *
 * This interface is only used when kmk_builtin_redirect is already running on
 * a worker thread.
 *
 * @returns exit status.
 * @param   pvWorker        The worker instance.
 * @param   pszExecutable   The executable image to run.
 * @param   papszArgs       Argument vector.
 * @param   fQuotedArgv     Whether the argument vector is already quoted and
 *                          just need some space to be turned into a command
 *                          line.
 * @param   papszEnvVars    Environment vector.
 * @param   pszCwd          The working directory of the child.  Optional.
 * @param   pafReplace      Which standard handles to replace. Maybe modified!
 * @param   pahReplace      The replacement handles.  Maybe modified!
 *
 */
int MkWinChildBuiltInExecChild(void *pvWorker, const char *pszExecutable, char **papszArgs, BOOL fQuotedArgv,
                               char **papszEnvVars, const char *pszCwd, BOOL pafReplace[3], HANDLE pahReplace[3])
{
    PWINCHILDCAREWORKER pWorker = (PWINCHILDCAREWORKER)pvWorker;
    WCHAR              *pwszSearchPath   = NULL;
    WCHAR              *pwszzEnvironment = NULL;
    WCHAR              *pwszCommandLine  = NULL;
    WCHAR              *pwszImageName    = NULL;
    WCHAR              *pwszCwd          = NULL;
    BOOL                fNeedShell       = FALSE;
    PWINCHILD           pChild;
    int                 rc;
    assert(pWorker->uMagic == WINCHILDCAREWORKER_MAGIC);
    pChild = pWorker->pCurChild;
    assert(pChild != NULL && pChild->uMagic == WINCHILD_MAGIC);

    /*
     * Convert the CWD first since it's optional and we don't need to clean
     * up anything here if it fails.
     */
    if (pszCwd)
    {
        size_t cchCwd = strlen(pszCwd);
        int    cwcCwd = MultiByteToWideChar(CP_ACP, 0 /*fFlags*/, pszCwd, cchCwd + 1, NULL, 0);
        pwszCwd = xmalloc((cwcCwd + 1) * sizeof(WCHAR)); /* (+1 in case cwcCwd is 0) */
        cwcCwd = MultiByteToWideChar(CP_ACP, 0 /*fFlags*/, pszCwd, cchCwd + 1, pwszCwd, cwcCwd + 1);
        if (!cwcCwd)
        {
            rc = GetLastError();
            MkWinChildError(pWorker, 1, _("MultiByteToWideChar failed to convert CWD (%s): %u\n"), pszCwd, (unsigned)rc);
            return rc;
        }
    }

    /*
     * Before we search for the image, we convert the environment so we don't
     * have to traverse it twice to find the PATH.
     */
    rc = mkWinChildcareWorkerConvertEnvironment(pWorker, papszEnvVars ? papszEnvVars : environ, 0/*cbEnvStrings*/,
                                                &pwszzEnvironment, &pwszSearchPath);
    /*
     * Find the executable and maybe checking if it's a shell script, then
     * convert it to a command line.
     */
    if (rc == 0)
        rc = mkWinChildcareWorkerFindImage(pWorker, pszExecutable, pwszSearchPath, pwszzEnvironment, NULL /*pszShell*/,
                                           &pwszImageName, &fNeedShell, &pChild->fProbableClExe);
    if (rc == 0)
    {
        assert(!fNeedShell);
        if (!fQuotedArgv)
            rc = mkWinChildcareWorkerConvertCommandline(pWorker, papszArgs, 0 /*fFlags*/, &pwszCommandLine);
        else
            rc = mkWinChildcareWorkerConvertQuotedArgvToCommandline(pWorker, papszArgs, &pwszCommandLine);

        /*
         * Create the child process.
         */
        if (rc == 0)
        {
            HANDLE hProcess;
            rc = mkWinChildcareWorkerCreateProcess(pWorker, pwszImageName, pwszCommandLine, pwszzEnvironment,
                                                   pwszCwd, pafReplace, pahReplace, TRUE /*fCatchOutput*/, &hProcess);
            if (rc == 0)
            {
                /*
                 * Wait for the child to complete.
                 */
                rc = mkWinChildcareWorkerWaitForProcess(pWorker, pChild, hProcess, pwszImageName, TRUE /*fCatchOutput*/);
                CloseHandle(hProcess);
            }
        }
    }

    free(pwszCwd);
    free(pwszCommandLine);
    free(pwszImageName);
    free(pwszzEnvironment);

    return rc;
}

#endif /* CONFIG_NEW_WIN_CHILDREN */

/**
 * Interface used to kill process when processing Ctrl-C and fatal errors.
 *
 * @returns 0 on success, -1 & errno on error.
 * @param   pid                 The process to kill (PWINCHILD).
 * @param   iSignal             What to kill it with.
 * @param   pMkChild            The make child structure for validation.
 */
int MkWinChildKill(pid_t pid, int iSignal, struct child *pMkChild)
{
    PWINCHILD pChild = (PWINCHILD)pid;
    if (pChild)
    {
        assert(pChild->uMagic == WINCHILD_MAGIC);
        if (pChild->uMagic == WINCHILD_MAGIC)
        {
            switch (pChild->enmType)
            {
                case WINCHILDTYPE_PROCESS:
                    assert(pChild->pMkChild == pMkChild);
                    TerminateProcess(pChild->u.Process.hProcess, DBG_TERMINATE_PROCESS);
                    pChild->iSignal = iSignal;
                    break;
#ifdef KMK
                case WINCHILDTYPE_SUBMIT:
                {
                    pChild->iSignal = iSignal;
                    SetEvent(pChild->u.Submit.hEvent);
                    break;
                }

                case WINCHILDTYPE_REDIRECT:
                    TerminateProcess(pChild->u.Redirect.hProcess, DBG_TERMINATE_PROCESS);
                    pChild->iSignal = iSignal;
                    break;

                case WINCHILDTYPE_BUILT_IN:
                    break;

#endif /* KMK */
                default:
                    assert(0);
            }
        }
    }
    return -1;
}

/**
 * Wait for a child process to complete
 *
 * @returns 0 on success, windows error code on failure.
 * @param   fBlock          Whether to block.
 * @param   pPid            Where to return the pid if a child process
 *                          completed.  This is set to zero if none.
 * @param   piExitCode      Where to return the exit code.
 * @param   piSignal        Where to return the exit signal number.
 * @param   pfCoreDumped    Where to return the core dumped indicator.
 * @param   ppMkChild       Where to return the associated struct child pointer.
 */
int MkWinChildWait(int fBlock, pid_t *pPid, int *piExitCode, int *piSignal, int *pfCoreDumped, struct child **ppMkChild)
{
    PWINCHILD pChild;

    *pPid         = 0;
    *piExitCode   = -222222;
    *pfCoreDumped = 0;
    *ppMkChild    = NULL;

    /*
     * Wait if necessary.
     */
    if (fBlock && !g_pTailCompletedChildren && g_cPendingChildren > 0)
    {
        DWORD dwStatus = WaitForSingleObject(g_hEvtWaitChildren, INFINITE);
        if (dwStatus == WAIT_FAILED)
            return (int)GetLastError();
    }

    /*
     * Try unlink the last child in the LIFO.
     */
    pChild = g_pTailCompletedChildren;
    if (!pChild)
        return 0;
    pChild = mkWinChildDequeFromLifo(&g_pTailCompletedChildren, pChild);
    assert(pChild);

    /*
     * Set return values and ditch the child structure.
     */
    *pPid         = pChild->pid;
    *piExitCode   = pChild->iExitCode;
    *pfCoreDumped = pChild->fCoreDumped;
    *ppMkChild    = pChild->pMkChild;
    switch (pChild->enmType)
    {
        case WINCHILDTYPE_PROCESS:
            break;
#ifdef KMK
        case WINCHILDTYPE_BUILT_IN:
        case WINCHILDTYPE_APPEND:
        case WINCHILDTYPE_SUBMIT:
        case WINCHILDTYPE_REDIRECT:
            break;
#endif /* KMK */
        default:
            assert(0);
    }
    mkWinChildDelete(pChild);

#ifdef KMK
    /* Flush the volatile directory cache. */
    dir_cache_invalid_after_job();
#endif
    return 0;
}

/**
 * Get the child completed event handle.
 *
 * Needed when w32os.c is waiting for a job token to become available, given
 * that completed children is the typical source of these tokens (esp. for kmk).
 *
 * @returns Zero if no active children, event handle if waiting is required.
 */
intptr_t MkWinChildGetCompleteEventHandle(void)
{
    /* We don't return the handle if we've got completed children.  This
       is a safe guard against being called twice in a row without any
       MkWinChildWait call inbetween. */
    if (!g_pTailCompletedChildren)
        return (intptr_t)g_hEvtWaitChildren;
    return 0;
}

/**
 * Emulate execv() for restarting kmk after one or more makefiles has been made.
 *
 * Does not return.
 *
 * @param   papszArgs           The arguments.
 * @param   papszEnv            The environment.
 */
void MkWinChildReExecMake(char **papszArgs, char **papszEnv)
{
    PROCESS_INFORMATION     ProcInfo;
    STARTUPINFOW            StartupInfo;
    WCHAR                  *pwszCommandLine;
    WCHAR                  *pwszzEnvironment;
    WCHAR                  *pwszPathIgnored;
    int                     rc;

    /*
     * Get the executable name.
     */
    WCHAR wszImageName[MKWINCHILD_MAX_PATH];
    DWORD cwcImageName = GetModuleFileNameW(GetModuleHandle(NULL), wszImageName, MKWINCHILD_MAX_PATH);
    if (cwcImageName == 0)
        ON(fatal, NILF, _("MkWinChildReExecMake: GetModuleFileName failed: %u\n"), GetLastError());

    /*
     * Create the command line and environment.
     */
    rc = mkWinChildcareWorkerConvertCommandline(NULL, papszArgs, 0 /*fFlags*/, &pwszCommandLine);
    if (rc != 0)
        ON(fatal, NILF, _("MkWinChildReExecMake: mkWinChildcareWorkerConvertCommandline failed: %u\n"), rc);

    rc = mkWinChildcareWorkerConvertEnvironment(NULL, papszEnv ? papszEnv : environ, 0 /*cbEnvStrings*/,
                                                &pwszzEnvironment, &pwszPathIgnored);
    if (rc != 0)
        ON(fatal, NILF, _("MkWinChildReExecMake: mkWinChildcareWorkerConvertEnvironment failed: %u\n"), rc);

#ifdef KMK
    /*
     * Flush the file system cache to avoid messing up tools fetching
     * going on in the "exec'ed" make by keeping directories open.
     */
    dir_cache_invalid_all_and_close_dirs(1);
#endif

    /*
     * Fill out the startup info and try create the process.
     */
    memset(&ProcInfo, 0, sizeof(ProcInfo));
    memset(&StartupInfo, 0, sizeof(StartupInfo));
    StartupInfo.cb = sizeof(StartupInfo);
    GetStartupInfoW(&StartupInfo);
    if (!CreateProcessW(wszImageName, pwszCommandLine, NULL /*pProcSecAttr*/, NULL /*pThreadSecAttr*/,
                        TRUE /*fInheritHandles*/, CREATE_UNICODE_ENVIRONMENT, pwszzEnvironment, NULL /*pwsz*/,
                        &StartupInfo, &ProcInfo))
        ON(fatal, NILF, _("MkWinChildReExecMake: CreateProcessW failed: %u\n"), GetLastError());
    CloseHandle(ProcInfo.hThread);

    /*
     * Wait for it to complete and forward the status code to our parent.
     */
    for (;;)
    {
        DWORD dwExitCode = -2222;
        DWORD dwStatus = WaitForSingleObject(ProcInfo.hProcess, INFINITE);
        if (   dwStatus == WAIT_IO_COMPLETION
            || dwStatus == WAIT_TIMEOUT /* whatever */)
            continue; /* however unlikely, these aren't fatal. */

        /* Get the status code and terminate. */
        if (dwStatus == WAIT_OBJECT_0)
        {
            if (!GetExitCodeProcess(ProcInfo.hProcess, &dwExitCode))
            {
                ON(fatal, NILF, _("MkWinChildReExecMake: GetExitCodeProcess failed: %u\n"), GetLastError());
                dwExitCode = -2222;
            }
        }
        else if (dwStatus)
            dwExitCode = dwStatus;

        CloseHandle(ProcInfo.hProcess);
        for (;;)
            exit(dwExitCode);
    }
}

#ifdef WITH_RW_LOCK
/** Serialization with kmkbuiltin_redirect. */
void MkWinChildExclusiveAcquire(void)
{
    AcquireSRWLockExclusive(&g_RWLock);
}

/** Serialization with kmkbuiltin_redirect. */
void MkWinChildExclusiveRelease(void)
{
    ReleaseSRWLockExclusive(&g_RWLock);
}
#endif /* WITH_RW_LOCK */

/**
 * Implementation of the CLOSE_ON_EXEC macro.
 *
 * @returns errno value.
 * @param   fd          The file descriptor to hide from children.
 */
int MkWinChildUnrelatedCloseOnExec(int fd)
{
    if (fd >= 0)
    {
        HANDLE hNative = (HANDLE)_get_osfhandle(fd);
        if (hNative != INVALID_HANDLE_VALUE && hNative != NULL)
        {
            if (SetHandleInformation(hNative, HANDLE_FLAG_INHERIT /*clear*/ , 0 /*set*/))
                return 0;
        }
        return errno;
    }
    return EINVAL;
}

