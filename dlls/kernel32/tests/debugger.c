/*
 * Unit tests for the debugger facility
 *
 * Copyright (c) 2007 Francois Gouget for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stdio.h>
#include <assert.h>

#include <windows.h>
#include <winternl.h>
#include <winreg.h>
#include "wine/test.h"
#include "wine/heap.h"
#include "wine/rbtree.h"

#ifndef STATUS_DEBUGGER_INACTIVE
#define STATUS_DEBUGGER_INACTIVE         ((NTSTATUS) 0xC0000354)
#endif

#define child_ok (winetest_set_location(__FILE__, __LINE__), 0) ? (void)0 : test_child_ok

static int    myARGC;
static char** myARGV;

static BOOL (WINAPI *pCheckRemoteDebuggerPresent)(HANDLE,PBOOL);
static BOOL (WINAPI *pDebugActiveProcessStop)(DWORD);
static BOOL (WINAPI *pDebugSetProcessKillOnExit)(BOOL);
static BOOL (WINAPI *pIsDebuggerPresent)(void);

static void (WINAPI *pDbgBreakPoint)(void);

static LONG child_failures;

static HMODULE ntdll;

static void WINAPIV WINETEST_PRINTF_ATTR(2, 3) test_child_ok(int condition, const char *msg, ...)
{
    __ms_va_list valist;

    __ms_va_start(valist, msg);
    winetest_vok(condition, msg, valist);
    __ms_va_end(valist);
    if (!condition) ++child_failures;
}

/* Copied from the process test */
static void get_file_name(char* buf)
{
    char path[MAX_PATH];

    buf[0] = '\0';
    GetTempPathA(sizeof(path), path);
    GetTempFileNameA(path, "wt", 0, buf);
}

typedef struct tag_reg_save_value
{
    const char *name;
    DWORD type;
    BYTE *data;
    DWORD size;
} reg_save_value;

static DWORD save_value(HKEY hkey, const char *value, reg_save_value *saved)
{
    DWORD ret;
    saved->name=value;
    saved->data=0;
    saved->size=0;
    ret=RegQueryValueExA(hkey, value, NULL, &saved->type, NULL, &saved->size);
    if (ret == ERROR_SUCCESS)
    {
        saved->data=HeapAlloc(GetProcessHeap(), 0, saved->size);
        RegQueryValueExA(hkey, value, NULL, &saved->type, saved->data, &saved->size);
    }
    return ret;
}

static void restore_value(HKEY hkey, reg_save_value *saved)
{
    if (saved->data)
    {
        RegSetValueExA(hkey, saved->name, 0, saved->type, saved->data, saved->size);
        HeapFree(GetProcessHeap(), 0, saved->data);
    }
    else
        RegDeleteValueA(hkey, saved->name);
}

static void get_events(const char* name, HANDLE *start_event, HANDLE *done_event)
{
    const char* basename;
    char* event_name;

    basename=strrchr(name, '\\');
    basename=(basename ? basename+1 : name);
    event_name=HeapAlloc(GetProcessHeap(), 0, 6+strlen(basename)+1);

    sprintf(event_name, "start_%s", basename);
    *start_event=CreateEventA(NULL, 0,0, event_name);
    sprintf(event_name, "done_%s", basename);
    *done_event=CreateEventA(NULL, 0,0, event_name);
    HeapFree(GetProcessHeap(), 0, event_name);
}

static void save_blackbox(const char* logfile, void* blackbox, int size, const char *dbgtrace)
{
    HANDLE hFile;
    DWORD written;

    hFile=CreateFileA(logfile, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, 0);
    if (hFile == INVALID_HANDLE_VALUE)
        return;
    WriteFile(hFile, blackbox, size, &written, NULL);
    if (dbgtrace && dbgtrace[0])
        WriteFile(hFile, dbgtrace, strlen(dbgtrace), &written, NULL);
    CloseHandle(hFile);
}

static int load_blackbox(const char* logfile, void* blackbox, int size)
{
    HANDLE hFile;
    DWORD read;
    BOOL ret;
    char buf[4096];

    hFile=CreateFileA(logfile, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, 0);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        ok(0, "unable to open '%s'\n", logfile);
        return 0;
    }
    SetLastError(0xdeadbeef);
    ret=ReadFile(hFile, blackbox, size, &read, NULL);
    ok(ret, "ReadFile failed: %d\n", GetLastError());
    ok(read == size, "wrong size for '%s': read=%d\n", logfile, read);
    ret = ReadFile(hFile, buf, sizeof(buf) - 1, &read, NULL);
    if (ret && read)
    {
        buf[read] = 0;
        trace("debugger traces:\n%s", buf);
    }
    CloseHandle(hFile);
    return 1;
}

static DWORD WINAPI thread_proc(void *arg)
{
    Sleep(10000);
    trace("exiting\n");
    ExitThread(1);
}

static void run_background_thread(void)
{
    DWORD tid;
    HANDLE thread = CreateThread(NULL, 0, thread_proc, NULL, 0, &tid);
    ok(thread != NULL, "CreateThrad failed\n");
    CloseHandle(thread);
}

typedef struct
{
    DWORD pid;
} crash_blackbox_t;

static void doCrash(int argc,  char** argv)
{
    volatile char* p;

    /* make sure the exception gets to the debugger */
    SetErrorMode( 0 );
    SetUnhandledExceptionFilter( NULL );

    run_background_thread();

    if (argc >= 4)
    {
        crash_blackbox_t blackbox;
        blackbox.pid=GetCurrentProcessId();
        save_blackbox(argv[3], &blackbox, sizeof(blackbox), NULL);
    }

    /* Just crash */
    trace("child: crashing...\n");
    p=NULL;
    *p=0;
}

typedef struct
{
    int argc;
    DWORD pid;
    BOOL debug_rc;
    DWORD debug_err;
    BOOL attach_rc;
    DWORD attach_err;
    BOOL nokill_rc;
    DWORD nokill_err;
    BOOL detach_rc;
    DWORD detach_err;
    DWORD failures;
} debugger_blackbox_t;

struct debugger_context
{
    DWORD pid;
    DEBUG_EVENT ev;
    unsigned process_cnt;
    unsigned dll_cnt;
    void *image_base;
    DWORD thread_tag;
    unsigned thread_cnt;
    struct wine_rb_tree threads;
    struct debuggee_thread *current_thread;
    struct debuggee_thread *main_thread;
};

struct debuggee_thread
{
    DWORD tid;
    DWORD tag;
    HANDLE handle;
    CONTEXT ctx;
    struct wine_rb_entry entry;
};

int debuggee_thread_compare(const void *key, const struct wine_rb_entry *entry)
{
    struct debuggee_thread *thread = WINE_RB_ENTRY_VALUE(entry, struct debuggee_thread, entry);
    return memcmp(key, &thread->tid, sizeof(thread->tid));
}

static void add_thread(struct debugger_context *ctx, DWORD tid)
{
    struct debuggee_thread *thread;
    if (!ctx->thread_cnt++) wine_rb_init(&ctx->threads, debuggee_thread_compare);
    thread = heap_alloc(sizeof(*thread));
    thread->tid = tid;
    thread->tag = ctx->thread_tag;
    thread->handle = NULL;
    wine_rb_put(&ctx->threads, &tid, &thread->entry);
    if (!ctx->main_thread) ctx->main_thread = thread;
}

static struct debuggee_thread *get_debuggee_thread(struct debugger_context *ctx, DWORD tid)
{
    struct wine_rb_entry *entry = wine_rb_get(&ctx->threads, &tid);
    ok(entry != NULL, "unknown thread %x\n", tid);
    return WINE_RB_ENTRY_VALUE(entry, struct debuggee_thread, entry);
}

static void remove_thread(struct debugger_context *ctx, DWORD tid)
{
    struct debuggee_thread *thread = get_debuggee_thread(ctx, tid);

    wine_rb_remove(&ctx->threads, &thread->entry);
    if (thread->handle) CloseHandle(thread->handle);
    heap_free(thread);
}

static void *get_ip(const CONTEXT *ctx)
{
#ifdef __i386__
    return (void *)ctx->Eip;
#elif defined(__x86_64__)
    return (void *)ctx->Rip;
#else
    return NULL;
#endif
}

#define fetch_thread_context(a) fetch_thread_context_(__LINE__,a)
static void fetch_thread_context_(unsigned line, struct debuggee_thread *thread)
{
    BOOL ret;

    if (!thread->handle)
    {
        thread->handle = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_QUERY_INFORMATION,
                                    FALSE, thread->tid);
        ok_(__FILE__,line)(thread->handle != NULL, "OpenThread failed: %u\n", GetLastError());
    }

    memset(&thread->ctx, 0xaa, sizeof(thread->ctx));
    thread->ctx.ContextFlags = CONTEXT_FULL;
    ret = GetThreadContext(thread->handle, &thread->ctx);
    ok_(__FILE__,line)(ret, "GetThreadContext failed: %u\n", GetLastError());
}

#define set_thread_context(a,b) set_thread_context_(__LINE__,a,b)
static void set_thread_context_(unsigned line, struct debugger_context *ctx, struct debuggee_thread *thread)
{
    BOOL ret;
    ret = SetThreadContext(thread->handle, &thread->ctx);
    ok_(__FILE__,line)(ret, "SetThreadContext failed: %u\n", GetLastError());
}

static void fetch_process_context(struct debugger_context *ctx)
{
    struct debuggee_thread *thread;

    WINE_RB_FOR_EACH_ENTRY(thread, &ctx->threads, struct debuggee_thread, entry)
    {
        fetch_thread_context(thread);
    }
}

#define WAIT_EVENT_TIMEOUT 20000
#define POLL_EVENT_TIMEOUT 200

#define next_event(a,b) next_event_(__LINE__,a,b)
static void next_event_(unsigned line, struct debugger_context *ctx, unsigned timeout)
{
    BOOL ret;

    ctx->current_thread = NULL;

    for (;;)
    {
        if (ctx->process_cnt && ctx->ev.dwDebugEventCode != -1)
        {
            ret = ContinueDebugEvent(ctx->ev.dwProcessId, ctx->ev.dwThreadId, DBG_CONTINUE);
            ok_(__FILE__,line)(ret, "ContinueDebugEvent failed, last error %d.\n", GetLastError());
        }

        ret = WaitForDebugEvent(&ctx->ev, timeout);
        if (!ret)
        {
            ok_(__FILE__,line)(GetLastError() == ERROR_SEM_TIMEOUT,
                               "WaitForDebugEvent failed, last error %d.\n", GetLastError());
            ctx->ev.dwDebugEventCode = -1;
            return;
        }

        if (ctx->ev.dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT)
        {
            if (!ctx->process_cnt) ctx->pid = ctx->ev.dwProcessId;
            ctx->process_cnt++;
        }

        if (ctx->ev.dwDebugEventCode == OUTPUT_DEBUG_STRING_EVENT) continue; /* ignore for now */
        if (ctx->ev.dwProcessId == ctx->pid) break;

        ok_(__FILE__,line)(ctx->process_cnt > 1, "unexpected event pid\n");
    }

    switch (ctx->ev.dwDebugEventCode)
    {
    case CREATE_PROCESS_DEBUG_EVENT:
        add_thread(ctx, ctx->ev.dwThreadId);
        ctx->image_base = ctx->ev.u.CreateProcessInfo.lpBaseOfImage;
        break;
    case EXIT_PROCESS_DEBUG_EVENT:
        remove_thread(ctx, ctx->ev.dwThreadId);
        return;
    case CREATE_THREAD_DEBUG_EVENT:
        add_thread(ctx, ctx->ev.dwThreadId);
        break;
    case EXIT_THREAD_DEBUG_EVENT:
        remove_thread(ctx, ctx->ev.dwThreadId);
        return;
    case LOAD_DLL_DEBUG_EVENT:
        ok(ctx->ev.u.LoadDll.lpBaseOfDll != ctx->image_base, "process image reported as DLL load event\n");
        ctx->dll_cnt++;
        break;
    case UNLOAD_DLL_DEBUG_EVENT:
        ctx->dll_cnt--;
        break;
    }

    ctx->current_thread = get_debuggee_thread(ctx, ctx->ev.dwThreadId);
}

#define wait_for_breakpoint(a) wait_for_breakpoint_(__LINE__,a)
static void wait_for_breakpoint_(unsigned line, struct debugger_context *ctx)
{
    do next_event_(line, ctx, 2000);
    while (ctx->ev.dwDebugEventCode == LOAD_DLL_DEBUG_EVENT || ctx->ev.dwDebugEventCode == UNLOAD_DLL_DEBUG_EVENT
           || ctx->ev.dwDebugEventCode == CREATE_THREAD_DEBUG_EVENT);

    ok(ctx->ev.dwDebugEventCode == EXCEPTION_DEBUG_EVENT, "dwDebugEventCode = %d\n", ctx->ev.dwDebugEventCode);
    ok(ctx->ev.u.Exception.ExceptionRecord.ExceptionCode == EXCEPTION_BREAKPOINT, "ExceptionCode = %x\n",
       ctx->ev.u.Exception.ExceptionRecord.ExceptionCode);
}

static void process_attach_events(struct debugger_context *ctx)
{
    DEBUG_EVENT ev;
    BOOL ret;

    ctx->ev.dwDebugEventCode = -1;
    next_event(ctx, 0);
    ok(ctx->ev.dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT, "dwDebugEventCode = %d\n", ctx->ev.dwDebugEventCode);

    next_event(ctx, 0);
    if (ctx->ev.dwDebugEventCode == LOAD_DLL_DEBUG_EVENT) /* Vista+ reports ntdll.dll before reporting threads */
    {
        ok(ctx->ev.dwDebugEventCode == LOAD_DLL_DEBUG_EVENT, "dwDebugEventCode = %d\n", ctx->ev.dwDebugEventCode);
        ok(ctx->ev.u.LoadDll.lpBaseOfDll == ntdll, "The first reported DLL is not ntdll.dll\n");
        next_event(ctx, 0);
    }

    while (ctx->ev.dwDebugEventCode == CREATE_THREAD_DEBUG_EVENT)
        next_event(ctx, 0);

    do
    {
        /* even when there are more pending events, they are not reported until current event is continued */
        ret = WaitForDebugEvent(&ev, 10);
        ok(GetLastError() == ERROR_SEM_TIMEOUT, "WaitForDebugEvent returned %x(%u)\n", ret, GetLastError());

        next_event(ctx, WAIT_EVENT_TIMEOUT);
        if (ctx->ev.dwDebugEventCode == LOAD_DLL_DEBUG_EVENT)
            ok(ctx->ev.u.LoadDll.lpBaseOfDll != ntdll, "ntdll.dll reported out of order\n");
    } while (ctx->ev.dwDebugEventCode == LOAD_DLL_DEBUG_EVENT || ctx->ev.dwDebugEventCode == UNLOAD_DLL_DEBUG_EVENT);
    ok(ctx->dll_cnt > 2, "dll_cnt = %d\n", ctx->dll_cnt);

    /* a new thread is created and it executes DbgBreakPoint, which causes the exception */
    ok(ctx->ev.dwDebugEventCode == CREATE_THREAD_DEBUG_EVENT, "dwDebugEventCode = %d\n", ctx->ev.dwDebugEventCode);
    if (ctx->ev.dwDebugEventCode == CREATE_THREAD_DEBUG_EVENT)
    {
        DWORD last_thread = ctx->ev.dwThreadId;
        next_event(ctx, WAIT_EVENT_TIMEOUT);
        ok(ctx->ev.dwThreadId == last_thread, "unexpected thread\n");
    }

    ok(ctx->ev.dwDebugEventCode == EXCEPTION_DEBUG_EVENT, "dwDebugEventCode = %d\n", ctx->ev.dwDebugEventCode);
    ok(ctx->ev.u.Exception.ExceptionRecord.ExceptionCode == EXCEPTION_BREAKPOINT, "ExceptionCode = %x\n",
       ctx->ev.u.Exception.ExceptionRecord.ExceptionCode);
    ok(ctx->ev.u.Exception.ExceptionRecord.ExceptionAddress == pDbgBreakPoint, "ExceptionAddress != DbgBreakPoint\n");

    /* flush debug events */
    do next_event(ctx, POLL_EVENT_TIMEOUT);
    while (ctx->ev.dwDebugEventCode == LOAD_DLL_DEBUG_EVENT || ctx->ev.dwDebugEventCode == UNLOAD_DLL_DEBUG_EVENT
           || ctx->ev.dwDebugEventCode == CREATE_THREAD_DEBUG_EVENT || ctx->ev.dwDebugEventCode == EXIT_THREAD_DEBUG_EVENT);
    ok(ctx->ev.dwDebugEventCode == -1, "dwDebugEventCode = %d\n", ctx->ev.dwDebugEventCode);
}

static void doDebugger(int argc, char** argv)
{
    const char* logfile;
    debugger_blackbox_t blackbox;
    HANDLE start_event = 0, done_event = 0, debug_event;
    char buf[4096] = "";
    struct debugger_context ctx = { 0 };

    blackbox.argc=argc;
    logfile=(argc >= 4 ? argv[3] : NULL);
    blackbox.pid=(argc >= 5 ? atol(argv[4]) : 0);

    blackbox.attach_err=0;
    if (strstr(myARGV[2], "attach"))
    {
        blackbox.attach_rc=DebugActiveProcess(blackbox.pid);
        if (!blackbox.attach_rc)
            blackbox.attach_err=GetLastError();
    }
    else
        blackbox.attach_rc=TRUE;

    if (strstr(myARGV[2], "process"))
    {
        strcat(buf, "processing debug messages\n");
        process_attach_events(&ctx);
    }

    debug_event=(argc >= 6 ? (HANDLE)(INT_PTR)atol(argv[5]) : NULL);
    blackbox.debug_err=0;
    if (debug_event && strstr(myARGV[2], "event"))
    {
        strcat(buf, "setting event\n");
        blackbox.debug_rc=SetEvent(debug_event);
        if (!blackbox.debug_rc)
            blackbox.debug_err=GetLastError();
    }
    else
        blackbox.debug_rc=TRUE;

    if (strstr(myARGV[2], "process"))
    {
        next_event(&ctx, WAIT_EVENT_TIMEOUT);
        ok(ctx.ev.dwDebugEventCode == EXCEPTION_DEBUG_EVENT, "dwDebugEventCode = %d\n", ctx.ev.dwDebugEventCode);
        ok(ctx.ev.u.Exception.ExceptionRecord.ExceptionCode == STATUS_ACCESS_VIOLATION, "ExceptionCode = %x\n",
           ctx.ev.u.Exception.ExceptionRecord.ExceptionCode);
    }

    if (logfile)
    {
        get_events(logfile, &start_event, &done_event);
    }

    if (strstr(myARGV[2], "order"))
    {
        strcat(buf, "waiting for the start signal...\n");
        WaitForSingleObject(start_event, INFINITE);
    }

    blackbox.nokill_err=0;
    if (strstr(myARGV[2], "nokill"))
    {
        blackbox.nokill_rc=pDebugSetProcessKillOnExit(FALSE);
        if (!blackbox.nokill_rc)
            blackbox.nokill_err=GetLastError();
    }
    else
        blackbox.nokill_rc=TRUE;

    blackbox.detach_err=0;
    if (strstr(myARGV[2], "detach"))
    {
        blackbox.detach_rc=pDebugActiveProcessStop(blackbox.pid);
        if (!blackbox.detach_rc)
            blackbox.detach_err=GetLastError();
    }
    else
        blackbox.detach_rc=TRUE;

    if (debug_event && strstr(myARGV[2], "late"))
    {
        strcat(buf, "setting event\n");
        blackbox.debug_rc=SetEvent(debug_event);
        if (!blackbox.debug_rc)
            blackbox.debug_err=GetLastError();
    }

    strcat(buf, "done debugging...\n");
    if (logfile)
    {
        blackbox.failures = winetest_get_failures();
        save_blackbox(logfile, &blackbox, sizeof(blackbox), buf);
    }

    SetEvent(done_event);

    /* Just exit with a known value */
    ExitProcess(0xdeadbeef);
}

static void crash_and_debug(HKEY hkey, const char* argv0, const char* dbgtasks)
{
    static BOOL skip_crash_and_debug = FALSE;
    BOOL bRet;
    DWORD ret;
    HANDLE start_event, done_event;
    char* cmd;
    char dbglog[MAX_PATH];
    char childlog[MAX_PATH];
    PROCESS_INFORMATION	info;
    STARTUPINFOA startup;
    DWORD exit_code;
    crash_blackbox_t crash_blackbox;
    debugger_blackbox_t dbg_blackbox;
    DWORD wait_code;

    if (skip_crash_and_debug)
    {
        win_skip("Skipping crash_and_debug\n");
        return;
    }

    ret=RegSetValueExA(hkey, "auto", 0, REG_SZ, (BYTE*)"1", 2);
    if (ret == ERROR_ACCESS_DENIED)
    {
        skip_crash_and_debug = TRUE;
        skip("No write access to change the debugger\n");
        return;
    }

    ok(ret == ERROR_SUCCESS, "unable to set AeDebug/auto: ret=%d\n", ret);

    get_file_name(dbglog);
    get_events(dbglog, &start_event, &done_event);
    cmd=HeapAlloc(GetProcessHeap(), 0, strlen(argv0)+10+strlen(dbgtasks)+1+strlen(dbglog)+2+34+1);
    sprintf(cmd, "%s debugger %s \"%s\" %%ld %%ld", argv0, dbgtasks, dbglog);
    ret=RegSetValueExA(hkey, "debugger", 0, REG_SZ, (BYTE*)cmd, strlen(cmd)+1);
    ok(ret == ERROR_SUCCESS, "unable to set AeDebug/debugger: ret=%d\n", ret);
    HeapFree(GetProcessHeap(), 0, cmd);

    get_file_name(childlog);
    cmd=HeapAlloc(GetProcessHeap(), 0, strlen(argv0)+16+strlen(dbglog)+2+1);
    sprintf(cmd, "%s debugger crash \"%s\"", argv0, childlog);

    trace("running %s...\n", dbgtasks);
    memset(&startup, 0, sizeof(startup));
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_SHOWNORMAL;
    ret=CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &info);
    ok(ret, "CreateProcess: err=%d\n", GetLastError());
    HeapFree(GetProcessHeap(), 0, cmd);
    CloseHandle(info.hThread);

    /* The process exits... */
    trace("waiting for child exit...\n");
    wait_code = WaitForSingleObject(info.hProcess, 30000);
#if defined(_WIN64) && defined(__MINGW32__)
    /* Mingw x64 doesn't output proper unwind info */
    skip_crash_and_debug = broken(wait_code == WAIT_TIMEOUT);
    if (skip_crash_and_debug)
    {
        TerminateProcess(info.hProcess, WAIT_TIMEOUT);
        WaitForSingleObject(info.hProcess, 5000);
        CloseHandle(info.hProcess);
        DeleteFileA(dbglog);
        DeleteFileA(childlog);
        win_skip("Giving up on child process\n");
        return;
    }
#endif
    ok(wait_code == WAIT_OBJECT_0, "Timed out waiting for the child to crash\n");
    bRet = GetExitCodeProcess(info.hProcess, &exit_code);
    ok(bRet, "GetExitCodeProcess failed: err=%d\n", GetLastError());
    if (strstr(dbgtasks, "code2"))
    {
        /* If, after attaching to the debuggee, the debugger exits without
         * detaching, then the debuggee gets a special exit code.
         */
        ok(exit_code == STATUS_DEBUGGER_INACTIVE ||
           broken(exit_code == STATUS_ACCESS_VIOLATION) || /* Intermittent Vista+ */
           broken(exit_code == WAIT_ABANDONED), /* NT4, W2K */
           "wrong exit code : %08x\n", exit_code);
    }
    else
        ok(exit_code == STATUS_ACCESS_VIOLATION ||
           broken(exit_code == WAIT_ABANDONED), /* NT4, W2K, W2K3 */
           "wrong exit code : %08x\n", exit_code);
    CloseHandle(info.hProcess);

    /* ...before the debugger */
    if (strstr(dbgtasks, "order"))
        ok(SetEvent(start_event), "SetEvent(start_event) failed\n");

    trace("waiting for the debugger...\n");
    wait_code = WaitForSingleObject(done_event, 5000);
#if defined(_WIN64) && defined(__MINGW32__)
    /* Mingw x64 doesn't output proper unwind info */
    skip_crash_and_debug = broken(wait_code == WAIT_TIMEOUT);
    if (skip_crash_and_debug)
    {
        DeleteFileA(dbglog);
        DeleteFileA(childlog);
        win_skip("Giving up on debugger\n");
        return;
    }
#endif
    ok(wait_code == WAIT_OBJECT_0, "Timed out waiting for the debugger\n");

    ok(load_blackbox(childlog, &crash_blackbox, sizeof(crash_blackbox)), "failed to open: %s\n", childlog);
    ok(load_blackbox(dbglog, &dbg_blackbox, sizeof(dbg_blackbox)), "failed to open: %s\n", dbglog);

    ok(dbg_blackbox.argc == 6, "wrong debugger argument count: %d\n", dbg_blackbox.argc);
    ok(dbg_blackbox.pid == crash_blackbox.pid, "the child and debugged pids don't match: %d != %d\n", crash_blackbox.pid, dbg_blackbox.pid);
    ok(dbg_blackbox.debug_rc, "debugger: SetEvent(debug_event) failed err=%d\n", dbg_blackbox.debug_err);
    ok(dbg_blackbox.attach_rc, "DebugActiveProcess(%d) failed err=%d\n", dbg_blackbox.pid, dbg_blackbox.attach_err);
    ok(dbg_blackbox.nokill_rc, "DebugSetProcessKillOnExit(FALSE) failed err=%d\n", dbg_blackbox.nokill_err);
    ok(dbg_blackbox.detach_rc, "DebugActiveProcessStop(%d) failed err=%d\n", dbg_blackbox.pid, dbg_blackbox.detach_err);
    ok(!dbg_blackbox.failures, "debugger reported %u failures\n", dbg_blackbox.failures);

    DeleteFileA(dbglog);
    DeleteFileA(childlog);
}

static void crash_and_winedbg(HKEY hkey, const char* argv0)
{
    BOOL bRet;
    DWORD ret;
    char* cmd;
    PROCESS_INFORMATION	info;
    STARTUPINFOA startup;
    DWORD exit_code;

    ret=RegSetValueExA(hkey, "auto", 0, REG_SZ, (BYTE*)"1", 2);
    ok(ret == ERROR_SUCCESS, "unable to set AeDebug/auto: ret=%d\n", ret);

    cmd=HeapAlloc(GetProcessHeap(), 0, strlen(argv0)+15+1);
    sprintf(cmd, "%s debugger crash", argv0);

    memset(&startup, 0, sizeof(startup));
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_SHOWNORMAL;
    ret=CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &info);
    ok(ret, "CreateProcess: err=%d\n", GetLastError());
    HeapFree(GetProcessHeap(), 0, cmd);
    CloseHandle(info.hThread);

    trace("waiting for child exit...\n");
    ok(WaitForSingleObject(info.hProcess, 60000) == WAIT_OBJECT_0, "Timed out waiting for the child to crash\n");
    bRet = GetExitCodeProcess(info.hProcess, &exit_code);
    ok(bRet, "GetExitCodeProcess failed: err=%d\n", GetLastError());
    ok(exit_code == STATUS_ACCESS_VIOLATION, "exit code = %08x\n", exit_code);
    CloseHandle(info.hProcess);
}

static void test_ExitCode(void)
{
    static const char* AeDebug="Software\\Microsoft\\Windows NT\\CurrentVersion\\AeDebug";
    static const char* WineDbg="Software\\Wine\\WineDbg";
    char test_exe[MAX_PATH];
    DWORD ret;
    HKEY hkey;
    DWORD disposition;
    reg_save_value auto_value;
    reg_save_value debugger_value;

    GetModuleFileNameA(GetModuleHandleA(NULL), test_exe, sizeof(test_exe));
    if (GetFileAttributesA(test_exe) == INVALID_FILE_ATTRIBUTES)
        strcat(test_exe, ".so");
    if (GetFileAttributesA(test_exe) == INVALID_FILE_ATTRIBUTES)
    {
        ok(0, "could not find the test executable '%s'\n", test_exe);
        return;
    }

    ret=RegCreateKeyExA(HKEY_LOCAL_MACHINE, AeDebug, 0, NULL, REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, NULL, &hkey, &disposition);
    if (ret == ERROR_SUCCESS)
    {
        save_value(hkey, "auto", &auto_value);
        save_value(hkey, "debugger", &debugger_value);
        trace("HKLM\\%s\\debugger is set to '%s'\n", AeDebug, debugger_value.data);
    }
    else if (ret == ERROR_ACCESS_DENIED)
    {
        skip("not enough privileges to change the debugger\n");
        return;
    }
    else if (ret != ERROR_FILE_NOT_FOUND)
    {
        ok(0, "could not open the AeDebug key: %d\n", ret);
        return;
    }
    else debugger_value.data = NULL;

    if (debugger_value.data && debugger_value.type == REG_SZ &&
        strstr((char*)debugger_value.data, "winedbg --auto"))
    {
        HKEY hkeyWinedbg;
        ret=RegCreateKeyA(HKEY_CURRENT_USER, WineDbg, &hkeyWinedbg);
        if (ret == ERROR_SUCCESS)
        {
            static DWORD zero;
            reg_save_value crash_dlg_value;
            save_value(hkeyWinedbg, "ShowCrashDialog", &crash_dlg_value);
            RegSetValueExA(hkeyWinedbg, "ShowCrashDialog", 0, REG_DWORD, (BYTE *)&zero, sizeof(DWORD));
            crash_and_winedbg(hkey, test_exe);
            restore_value(hkeyWinedbg, &crash_dlg_value);
            RegCloseKey(hkeyWinedbg);
        }
        else
            ok(0, "Couldn't access WineDbg Key - error %u\n", ret);
    }

    if (winetest_interactive)
        /* Since the debugging process never sets the debug event, it isn't recognized
           as a valid debugger and, after the debugger exits, Windows will show a dialog box
           asking the user what to do */
        crash_and_debug(hkey, test_exe, "dbg,none");
    else
        skip("\"none\" debugger test needs user interaction\n");
    ok(disposition == REG_OPENED_EXISTING_KEY, "expected REG_OPENED_EXISTING_KEY, got %d\n", disposition);
    crash_and_debug(hkey, test_exe, "dbg,event,order");
    crash_and_debug(hkey, test_exe, "dbg,attach,event,code2");
    if (pDebugSetProcessKillOnExit)
        crash_and_debug(hkey, test_exe, "dbg,attach,event,nokill");
    else
        win_skip("DebugSetProcessKillOnExit is not available\n");
    if (pDebugActiveProcessStop)
    {
        crash_and_debug(hkey, test_exe, "dbg,attach,event,detach");
        crash_and_debug(hkey, test_exe, "dbg,attach,detach,late");
    }
    else
        win_skip("DebugActiveProcessStop is not available\n");
    crash_and_debug(hkey, test_exe, "dbg,attach,process,event,detach");

    if (disposition == REG_CREATED_NEW_KEY)
    {
        RegCloseKey(hkey);
        RegDeleteKeyA(HKEY_LOCAL_MACHINE, AeDebug);
    }
    else
    {
        restore_value(hkey, &auto_value);
        restore_value(hkey, &debugger_value);
        RegCloseKey(hkey);
    }
}

static void test_RemoteDebugger(void)
{
    BOOL bret, present;
    if(!pCheckRemoteDebuggerPresent)
    {
        win_skip("CheckRemoteDebuggerPresent is not available\n");
        return;
    }
    present = TRUE;
    SetLastError(0xdeadbeef);
    bret = pCheckRemoteDebuggerPresent(GetCurrentProcess(),&present);
    ok(bret , "expected CheckRemoteDebuggerPresent to succeed\n");
    ok(0xdeadbeef == GetLastError(),
       "expected error to be unchanged, got %d/%x\n",GetLastError(), GetLastError());

    present = TRUE;
    SetLastError(0xdeadbeef);
    bret = pCheckRemoteDebuggerPresent(NULL,&present);
    ok(!bret , "expected CheckRemoteDebuggerPresent to fail\n");
    ok(present, "expected parameter to be unchanged\n");
    ok(ERROR_INVALID_PARAMETER == GetLastError(),
       "expected error ERROR_INVALID_PARAMETER, got %d/%x\n",GetLastError(), GetLastError());

    SetLastError(0xdeadbeef);
    bret = pCheckRemoteDebuggerPresent(GetCurrentProcess(),NULL);
    ok(!bret , "expected CheckRemoteDebuggerPresent to fail\n");
    ok(ERROR_INVALID_PARAMETER == GetLastError(),
       "expected error ERROR_INVALID_PARAMETER, got %d/%x\n",GetLastError(), GetLastError());
}

struct child_blackbox
{
    LONG failures;
};

static void doChild(int argc, char **argv)
{
    struct child_blackbox blackbox;
    const char *blackbox_file;
    HANDLE parent;
    DWORD ppid;
    BOOL debug;
    BOOL ret;

    blackbox_file = argv[4];
    sscanf(argv[3], "%08x", &ppid);

    parent = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, ppid);
    child_ok(!!parent, "OpenProcess failed, last error %#x.\n", GetLastError());

    ret = pCheckRemoteDebuggerPresent(parent, &debug);
    child_ok(ret, "CheckRemoteDebuggerPresent failed, last error %#x.\n", GetLastError());
    child_ok(!debug, "Expected debug == 0, got %#x.\n", debug);

    ret = DebugActiveProcess(ppid);
    child_ok(ret, "DebugActiveProcess failed, last error %#x.\n", GetLastError());

    ret = pCheckRemoteDebuggerPresent(parent, &debug);
    child_ok(ret, "CheckRemoteDebuggerPresent failed, last error %#x.\n", GetLastError());
    child_ok(debug, "Expected debug != 0, got %#x.\n", debug);

    ret = pDebugActiveProcessStop(ppid);
    child_ok(ret, "DebugActiveProcessStop failed, last error %#x.\n", GetLastError());

    ret = pCheckRemoteDebuggerPresent(parent, &debug);
    child_ok(ret, "CheckRemoteDebuggerPresent failed, last error %#x.\n", GetLastError());
    child_ok(!debug, "Expected debug == 0, got %#x.\n", debug);

    ret = CloseHandle(parent);
    child_ok(ret, "CloseHandle failed, last error %#x.\n", GetLastError());

    ret = pIsDebuggerPresent();
    child_ok(ret, "Expected ret != 0, got %#x.\n", ret);
    ret = pCheckRemoteDebuggerPresent(GetCurrentProcess(), &debug);
    child_ok(ret, "CheckRemoteDebuggerPresent failed, last error %#x.\n", GetLastError());
    child_ok(debug, "Expected debug != 0, got %#x.\n", debug);

    NtCurrentTeb()->Peb->BeingDebugged = FALSE;

    ret = pIsDebuggerPresent();
    child_ok(!ret, "Expected ret != 0, got %#x.\n", ret);
    ret = pCheckRemoteDebuggerPresent(GetCurrentProcess(), &debug);
    child_ok(ret, "CheckRemoteDebuggerPresent failed, last error %#x.\n", GetLastError());
    child_ok(debug, "Expected debug != 0, got %#x.\n", debug);

    NtCurrentTeb()->Peb->BeingDebugged = TRUE;

    blackbox.failures = child_failures;
    save_blackbox(blackbox_file, &blackbox, sizeof(blackbox), NULL);
}

static void test_debug_loop(int argc, char **argv)
{
    const char *arguments = " debugger child ";
    struct child_blackbox blackbox;
    char blackbox_file[MAX_PATH];
    PROCESS_INFORMATION pi;
    STARTUPINFOA si;
    BOOL debug;
    DWORD pid;
    char *cmd;
    BOOL ret;

    if (!pDebugActiveProcessStop || !pCheckRemoteDebuggerPresent)
    {
        win_skip("DebugActiveProcessStop or CheckRemoteDebuggerPresent not available, skipping test.\n");
        return;
    }

    pid = GetCurrentProcessId();
    ret = DebugActiveProcess(pid);
    ok(!ret, "DebugActiveProcess() succeeded on own process.\n");

    get_file_name(blackbox_file);
    cmd = HeapAlloc(GetProcessHeap(), 0, strlen(argv[0]) + strlen(arguments) + strlen(blackbox_file) + 2 + 10);
    sprintf(cmd, "%s%s%08x \"%s\"", argv[0], arguments, pid, blackbox_file);

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    ret = CreateProcessA(NULL, cmd, NULL, NULL, FALSE, DEBUG_PROCESS, NULL, NULL, &si, &pi);
    ok(ret, "CreateProcess failed, last error %#x.\n", GetLastError());

    HeapFree(GetProcessHeap(), 0, cmd);

    ret = pCheckRemoteDebuggerPresent(pi.hProcess, &debug);
    ok(ret, "CheckRemoteDebuggerPresent failed, last error %#x.\n", GetLastError());
    ok(debug, "Expected debug != 0, got %#x.\n", debug);

    for (;;)
    {
        DEBUG_EVENT ev;

        ret = WaitForDebugEvent(&ev, INFINITE);
        ok(ret, "WaitForDebugEvent failed, last error %#x.\n", GetLastError());
        if (!ret) break;

        if (ev.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT) break;
#if defined(__i386__) || defined(__x86_64__)
        if (ev.dwDebugEventCode == EXCEPTION_DEBUG_EVENT &&
            ev.u.Exception.ExceptionRecord.ExceptionCode == EXCEPTION_BREAKPOINT)
        {
            BYTE byte = 0;
            NtReadVirtualMemory(pi.hProcess, ev.u.Exception.ExceptionRecord.ExceptionAddress, &byte, 1, NULL);
            ok(byte == 0xcc, "got %02x\n", byte);
        }
#endif
        ret = ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, DBG_CONTINUE);
        ok(ret, "ContinueDebugEvent failed, last error %#x.\n", GetLastError());
        if (!ret) break;
    }

    ret = CloseHandle(pi.hThread);
    ok(ret, "CloseHandle failed, last error %#x.\n", GetLastError());
    ret = CloseHandle(pi.hProcess);
    ok(ret, "CloseHandle failed, last error %#x.\n", GetLastError());

    load_blackbox(blackbox_file, &blackbox, sizeof(blackbox));
    ok(!blackbox.failures, "Got %d failures from child process.\n", blackbox.failures);

    ret = DeleteFileA(blackbox_file);
    ok(ret, "DeleteFileA failed, last error %#x.\n", GetLastError());
}

static void doChildren(int argc, char **argv)
{
    const char *arguments = "debugger children last";
    struct child_blackbox blackbox;
    const char *blackbox_file, *p;
    char event_name[MAX_PATH];
    PROCESS_INFORMATION pi;
    STARTUPINFOA si;
    HANDLE event;
    char *cmd;
    BOOL ret;

    if (!strcmp(argv[3], "last")) return;

    blackbox_file = argv[3];

    run_background_thread();

    p = strrchr(blackbox_file, '\\');
    p = p ? p+1 : blackbox_file;
    strcpy(event_name, p);
    strcat(event_name, "_init");
    event = OpenEventA(EVENT_ALL_ACCESS, FALSE, event_name);
    child_ok(event != NULL, "OpenEvent failed, last error %d.\n", GetLastError());
    SetEvent(event);
    CloseHandle(event);

    p = strrchr(blackbox_file, '\\');
    p = p ? p+1 : blackbox_file;
    strcpy(event_name, p);
    strcat(event_name, "_attach");
    event = OpenEventA(EVENT_ALL_ACCESS, FALSE, event_name);
    child_ok(event != NULL, "OpenEvent failed, last error %d.\n", GetLastError());
    WaitForSingleObject(event, INFINITE);
    CloseHandle(event);

    cmd = HeapAlloc(GetProcessHeap(), 0, strlen(argv[0]) + strlen(arguments) + 2);
    sprintf(cmd, "%s %s", argv[0], arguments);

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    ret = CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
    child_ok(ret, "CreateProcess failed, last error %d.\n", GetLastError());

    child_ok(WaitForSingleObject(pi.hProcess, 10000) == WAIT_OBJECT_0,
            "Timed out waiting for the child to exit\n");

    ret = CloseHandle(pi.hThread);
    child_ok(ret, "CloseHandle failed, last error %d.\n", GetLastError());
    ret = CloseHandle(pi.hProcess);
    child_ok(ret, "CloseHandle failed, last error %d.\n", GetLastError());

    blackbox.failures = child_failures;
    save_blackbox(blackbox_file, &blackbox, sizeof(blackbox), NULL);

    HeapFree(GetProcessHeap(), 0, cmd);
}

static void test_debug_children(char *name, DWORD flag, BOOL debug_child)
{
    const char *arguments = "debugger children";
    struct child_blackbox blackbox;
    char blackbox_file[MAX_PATH], *p;
    char event_name[MAX_PATH];
    PROCESS_INFORMATION pi;
    STARTUPINFOA si;
    HANDLE event_init, event_attach;
    char *cmd;
    BOOL debug, ret;
    struct debugger_context ctx = { 0 };

    if (!pDebugActiveProcessStop || !pCheckRemoteDebuggerPresent)
    {
        win_skip("DebugActiveProcessStop or CheckRemoteDebuggerPresent not available, skipping test.\n");
        return;
    }

    get_file_name(blackbox_file);
    cmd = HeapAlloc(GetProcessHeap(), 0, strlen(name) + strlen(arguments) + strlen(blackbox_file) + 5);
    sprintf(cmd, "%s %s \"%s\"", name, arguments, blackbox_file);

    p = strrchr(blackbox_file, '\\');
    p = p ? p+1 : blackbox_file;
    strcpy(event_name, p);
    strcat(event_name, "_init");
    event_init = CreateEventA(NULL, FALSE, FALSE, event_name);
    ok(event_init != NULL, "OpenEvent failed, last error %d.\n", GetLastError());

    p = strrchr(blackbox_file, '\\');
    p = p ? p+1 : blackbox_file;
    strcpy(event_name, p);
    strcat(event_name, "_attach");
    event_attach = CreateEventA(NULL, FALSE, flag!=0, event_name);
    ok(event_attach != NULL, "CreateEvent failed, last error %d.\n", GetLastError());

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);

    ret = CreateProcessA(NULL, cmd, NULL, NULL, FALSE, flag, NULL, NULL, &si, &pi);
    ok(ret, "CreateProcess failed, last error %d.\n", GetLastError());
    HeapFree(GetProcessHeap(), 0, cmd);
    if (!flag)
    {
        WaitForSingleObject(event_init, INFINITE);
        Sleep(100);
        ret = DebugActiveProcess(pi.dwProcessId);
        ok(ret, "DebugActiveProcess failed, last error %d.\n", GetLastError());
    }

    ret = pCheckRemoteDebuggerPresent(pi.hProcess, &debug);
    ok(ret, "CheckRemoteDebuggerPresent failed, last error %d.\n", GetLastError());
    ok(debug, "Expected debug != 0, got %x.\n", debug);

    trace("starting debugger loop\n");

    if (flag)
    {
        next_event(&ctx, WAIT_EVENT_TIMEOUT);
        ok(ctx.ev.dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT, "dwDebugEventCode = %d\n", ctx.ev.dwDebugEventCode);
        ok(ctx.pid == pi.dwProcessId, "unexpected dwProcessId %x\n", ctx.ev.dwProcessId == ctx.pid);

        next_event(&ctx, WAIT_EVENT_TIMEOUT);
        ok(ctx.ev.dwDebugEventCode == LOAD_DLL_DEBUG_EVENT, "dwDebugEventCode = %d\n", ctx.ev.dwDebugEventCode);

        wait_for_breakpoint(&ctx);
        ok(ctx.dll_cnt > 2, "dll_cnt = %d\n", ctx.dll_cnt);
    }
    else
    {
        DWORD last_thread;

        process_attach_events(&ctx);
        ok(ctx.pid == pi.dwProcessId, "unexpected dwProcessId %x\n", ctx.pid);

        ret = DebugBreakProcess(pi.hProcess);
        ok(ret, "BreakProcess failed: %u\n", GetLastError());

        /* a new thread, which executes DbgBreakPoint, is created */
        next_event(&ctx, WAIT_EVENT_TIMEOUT);
        ok(ctx.ev.dwDebugEventCode == CREATE_THREAD_DEBUG_EVENT, "dwDebugEventCode = %d\n", ctx.ev.dwDebugEventCode);
        last_thread = ctx.ev.dwThreadId;

        if (ctx.ev.dwDebugEventCode == CREATE_THREAD_DEBUG_EVENT)
            next_event(&ctx, WAIT_EVENT_TIMEOUT);

        ok(ctx.ev.dwDebugEventCode == EXCEPTION_DEBUG_EVENT, "dwDebugEventCode = %d\n", ctx.ev.dwDebugEventCode);
        ok(ctx.ev.dwThreadId == last_thread, "unexpected thread\n");
        ok(ctx.ev.u.Exception.ExceptionRecord.ExceptionCode == EXCEPTION_BREAKPOINT, "ExceptionCode = %x\n",
           ctx.ev.u.Exception.ExceptionRecord.ExceptionCode);
        ok(ctx.ev.u.Exception.ExceptionRecord.ExceptionAddress == pDbgBreakPoint, "ExceptionAddress != DbgBreakPoint\n");

        ret = SetEvent(event_attach);
        ok(ret, "SetEvent failed, last error %d.\n", GetLastError());
    }

    do next_event(&ctx, WAIT_EVENT_TIMEOUT);
    while (ctx.ev.dwDebugEventCode == LOAD_DLL_DEBUG_EVENT || ctx.ev.dwDebugEventCode == UNLOAD_DLL_DEBUG_EVENT
           || ctx.ev.dwDebugEventCode == CREATE_THREAD_DEBUG_EVENT || ctx.ev.dwDebugEventCode == EXIT_THREAD_DEBUG_EVENT);

    ok(ctx.ev.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT, "dwDebugEventCode = %d\n", ctx.ev.dwDebugEventCode);
    ret = ContinueDebugEvent(ctx.ev.dwProcessId, ctx.ev.dwThreadId, DBG_CONTINUE);
    ok(ret, "ContinueDebugEvent failed, last error %d.\n", GetLastError());

    if(debug_child)
        ok(ctx.process_cnt == 2, "didn't get any child events (flag: %x).\n", flag);
    else
        ok(ctx.process_cnt == 1, "got child event (flag: %x).\n", flag);
    CloseHandle(event_init);
    CloseHandle(event_attach);

    ret = CloseHandle(pi.hThread);
    ok(ret, "CloseHandle failed, last error %d.\n", GetLastError());
    ret = CloseHandle(pi.hProcess);
    ok(ret, "CloseHandle failed, last error %d.\n", GetLastError());

    load_blackbox(blackbox_file, &blackbox, sizeof(blackbox));
    ok(!blackbox.failures, "Got %d failures from child process.\n", blackbox.failures);

    ret = DeleteFileA(blackbox_file);
    ok(ret, "DeleteFileA failed, last error %d.\n", GetLastError());
}

static void wait_debugger(HANDLE event)
{
    WaitForSingleObject(event, INFINITE);
    ExitProcess(0);
}

#define expect_event(a,b) expect_event_(__LINE__,a,b)
static void expect_event_(unsigned line, struct debugger_context *ctx, DWORD event_code)
{
    next_event(ctx, WAIT_EVENT_TIMEOUT);
    ok_(__FILE__,line)(ctx->ev.dwDebugEventCode == event_code, "dwDebugEventCode = %d expected %d\n",
                       ctx->ev.dwDebugEventCode, event_code);
}

#define expect_exception(a,b) expect_exception_(__LINE__,a,b)
static void expect_exception_(unsigned line, struct debugger_context *ctx, DWORD exception_code)
{
    expect_event_(line, ctx, EXCEPTION_DEBUG_EVENT);
    ok_(__FILE__,line)(ctx->ev.u.Exception.ExceptionRecord.ExceptionCode == exception_code, "ExceptionCode = %x expected %x\n",
                       ctx->ev.u.Exception.ExceptionRecord.ExceptionCode, exception_code);
}

#define expect_breakpoint_exception(a,b) expect_breakpoint_exception_(__LINE__,a,b)
static void expect_breakpoint_exception_(unsigned line, struct debugger_context *ctx, const void *expect_addr)
{
    struct debuggee_thread *thread;
    expect_exception_(line, ctx, EXCEPTION_BREAKPOINT);
    if (!expect_addr) return;
    ok_(__FILE__,line)(ctx->ev.u.Exception.ExceptionRecord.ExceptionAddress == expect_addr,
                       "ExceptionAddress = %p expected %p\n", ctx->ev.u.Exception.ExceptionRecord.ExceptionAddress, expect_addr);
    thread = get_debuggee_thread(ctx, ctx->ev.dwThreadId);
    fetch_thread_context(thread);
    ok_(__FILE__,line)(get_ip(&thread->ctx) == (char*)expect_addr + 1, "unexpected instruction pointer %p expected %p\n",
                       get_ip(&thread->ctx), expect_addr);
}

#define single_step(a,b,c) single_step_(__LINE__,a,b,c)
static void single_step_(unsigned line, struct debugger_context *ctx, struct debuggee_thread *thread, void *expect_addr)
{
#if defined(__i386__) || defined(__x86_64__)
    fetch_thread_context(thread);
    thread->ctx.EFlags |= 0x100;
    set_thread_context(ctx, thread);
    expect_exception_(line, ctx, EXCEPTION_SINGLE_STEP);
    ok_(__FILE__,line)(ctx->ev.u.Exception.ExceptionRecord.ExceptionAddress == expect_addr,
                       "ExceptionAddress = %p expected %p\n", ctx->ev.u.Exception.ExceptionRecord.ExceptionAddress, expect_addr);
    fetch_thread_context(thread);
    ok_(__FILE__,line)(get_ip(&thread->ctx) == expect_addr, "unexpected instruction pointer %p expected %p\n",
                       get_ip(&thread->ctx), expect_addr);
    ok_(__FILE__,line)(!(thread->ctx.EFlags & 0x100), "EFlags = %x\n", thread->ctx.EFlags);
#endif
}

static const BYTE loop_code[] = {
#if defined(__i386__) || defined(__x86_64__)
    0x90,                         /* nop */
    0x90,                         /* nop */
    0x90,                         /* nop */
    0xe9, 0xf8, 0xff, 0xff, 0xff  /* jmp $-8 */
#endif
};

static const BYTE call_debug_service_code[] = {
#ifdef __i386__
    0x53,                         /* pushl %ebx */
    0x57,                         /* pushl %edi */
    0x8b, 0x44, 0x24, 0x0c,       /* movl 12(%esp),%eax */
    0xb9, 0x11, 0x11, 0x11, 0x11, /* movl $0x11111111,%ecx */
    0xba, 0x22, 0x22, 0x22, 0x22, /* movl $0x22222222,%edx */
    0xbb, 0x33, 0x33, 0x33, 0x33, /* movl $0x33333333,%ebx */
    0xbf, 0x44, 0x44, 0x44, 0x44, /* movl $0x44444444,%edi */
    0xcd, 0x2d,                   /* int $0x2d */
    0xeb,                         /* jmp $+17 */
    0x0f, 0x1f, 0x00,             /* nop */
    0x31, 0xc0,                   /* xorl %eax,%eax */
    0xeb, 0x0c,                   /* jmp $+14 */
    0x90, 0x90, 0x90, 0x90,       /* nop */
    0x90, 0x90, 0x90, 0x90,
    0x90,
    0x31, 0xc0,                   /* xorl %eax,%eax */
    0x40,                         /* incl %eax */
    0x5f,                         /* popl %edi */
    0x5b,                         /* popl %ebx */
    0xc3,                         /* ret */
#elif defined(__x86_64__)
    0x53,                         /* push %rbx */
    0x57,                         /* push %rdi */
    0x48, 0x89, 0xc8,             /* movl %rcx,%rax */
    0x48, 0xb9, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, /* movabs $0x1111111111111111,%rcx */
    0x48, 0xba, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, /* movabs $0x2222222222222222,%rdx */
    0x48, 0xbb, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, /* movabs $0x3333333333333333,%rbx */
    0x48, 0xbf, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, /* movabs $0x4444444444444444,%rdi */
    0xcd, 0x2d,                   /* int $0x2d */
    0xeb,                         /* jmp $+17 */
    0x0f, 0x1f, 0x00,             /* nop */
    0x48, 0x31, 0xc0,             /* xor %rax,%rax */
    0xeb, 0x0e,                   /* jmp $+16 */
    0x90, 0x90, 0x90, 0x90,       /* nop */
    0x90, 0x90, 0x90, 0x90,
    0x48, 0x31, 0xc0,             /* xor %rax,%rax */
    0x48, 0xff, 0xc0,             /* inc %rax */
    0x5f,                         /* pop %rdi */
    0x5b,                         /* pop %rbx */
    0xc3,                         /* ret */
#endif
};

#if defined(__i386__) || defined(__x86_64__)
#define OP_BP 0xcc
#else
#define OP_BP 0
#endif

static void test_debugger(const char *argv0)
{
    static const char arguments[] = " debugger wait ";
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    struct debugger_context ctx = { 0 };
    PROCESS_INFORMATION pi;
    STARTUPINFOA si;
    HANDLE event, thread;
    BYTE *mem, buf[4096], *proc_code, *thread_proc, byte;
    unsigned int i, worker_cnt, exception_cnt;
    struct debuggee_thread *debuggee_thread;
    char *cmd;
    BOOL ret;

    event = CreateEventW(&sa, TRUE, FALSE, NULL);
    ok(event != NULL, "CreateEvent failed: %u\n", GetLastError());

    cmd = heap_alloc(strlen(argv0) + strlen(arguments) + 16);
    sprintf(cmd, "%s%s%x\n", argv0, arguments, (DWORD)(DWORD_PTR)event);

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    ret = CreateProcessA(NULL, cmd, NULL, NULL, TRUE, DEBUG_PROCESS, NULL, NULL, &si, &pi);
    ok(ret, "CreateProcess failed, last error %#x.\n", GetLastError());
    heap_free(cmd);

    next_event(&ctx, WAIT_EVENT_TIMEOUT);
    ok(ctx.ev.dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT, "dwDebugEventCode = %d\n", ctx.ev.dwDebugEventCode);
    wait_for_breakpoint(&ctx);
    do next_event(&ctx, POLL_EVENT_TIMEOUT);
    while(ctx.ev.dwDebugEventCode != -1);

    mem = VirtualAllocEx(pi.hProcess, NULL, sizeof(buf), MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    ok(mem != NULL, "VirtualAllocEx failed: %u\n", GetLastError());
    proc_code   = buf + 1024;
    thread_proc = mem + 1024;

    if (sizeof(loop_code) > 1)
    {
        /* test single-step exceptions */
        memset(buf, OP_BP, sizeof(buf));
        memcpy(proc_code, &loop_code, sizeof(loop_code));
        proc_code[0] = OP_BP; /* set a breakpoint */
        ret = WriteProcessMemory(pi.hProcess, mem, buf, sizeof(buf), NULL);
        ok(ret, "WriteProcessMemory failed: %u\n", GetLastError());

        thread = CreateRemoteThread(pi.hProcess, NULL, 0, (void*)thread_proc, NULL, 0, NULL);
        ok(thread != NULL, "CreateRemoteThread failed: %u\n", GetLastError());

        expect_event(&ctx, CREATE_THREAD_DEBUG_EVENT);
        debuggee_thread = get_debuggee_thread(&ctx, ctx.ev.dwThreadId);

        wait_for_breakpoint(&ctx);
        fetch_thread_context(debuggee_thread);
        ok(ctx.ev.u.Exception.ExceptionRecord.ExceptionAddress == thread_proc,
           "ExceptionAddress = %p\n", ctx.ev.u.Exception.ExceptionRecord.ExceptionAddress);
        ok(get_ip(&debuggee_thread->ctx) == thread_proc + 1, "unexpected instruction pointer %p\n",
           get_ip(&debuggee_thread->ctx));

        single_step(&ctx, debuggee_thread, thread_proc + 2);
        single_step(&ctx, debuggee_thread, thread_proc + 3);
        single_step(&ctx, debuggee_thread, thread_proc);

        byte = 0xc3; /* ret */
        ret = WriteProcessMemory(pi.hProcess, thread_proc, &byte, 1, NULL);
        ok(ret, "WriteProcessMemory failed: %u\n", GetLastError());

        expect_event(&ctx, EXIT_THREAD_DEBUG_EVENT);
    }
    else win_skip("loop_code not supported on this architecture\n");

    if (sizeof(call_debug_service_code) > 1)
    {
        /* test debug service exceptions */
        memset(buf, OP_BP, sizeof(buf));
        memcpy(proc_code, call_debug_service_code, sizeof(call_debug_service_code));
        ret = WriteProcessMemory(pi.hProcess, mem, buf, sizeof(buf), NULL);
        ok(ret, "WriteProcessMemory failed: %u\n", GetLastError());

        /* BREAKPOINT_PRINT */
        thread = CreateRemoteThread(pi.hProcess, NULL, 0, (void*)thread_proc, (void*)2, 0, NULL);
        ok(thread != NULL, "CreateRemoteThread failed: %u\n", GetLastError());
        expect_event(&ctx, CREATE_THREAD_DEBUG_EVENT);
        expect_breakpoint_exception(&ctx, NULL);
        expect_event(&ctx, EXIT_THREAD_DEBUG_EVENT);

        /* BREAKPOINT_PROMPT */
        thread = CreateRemoteThread(pi.hProcess, NULL, 0, (void*)thread_proc, (void*)1, 0, NULL);
        ok(thread != NULL, "CreateRemoteThread failed: %u\n", GetLastError());
        expect_event(&ctx, CREATE_THREAD_DEBUG_EVENT);
        next_event(&ctx, WAIT_EVENT_TIMEOUT);
        /* some 32-bit Windows versions report exception to the debugger */
        if (sizeof(void *) == 4 && ctx.ev.dwDebugEventCode == EXCEPTION_DEBUG_EVENT) next_event(&ctx, WAIT_EVENT_TIMEOUT);
        ok(ctx.ev.dwDebugEventCode == EXIT_THREAD_DEBUG_EVENT, "unexpected debug event %u\n", ctx.ev.dwDebugEventCode);
    }
    else win_skip("call_debug_service_code not supported on this architecture\n");

    if (sizeof(loop_code) > 1 && (sizeof(void*) == 8 || broken(1) /* FIXME: broken in Wine */))
    {
        memset(buf, OP_BP, sizeof(buf));
        memcpy(proc_code, &loop_code, sizeof(loop_code));
        ret = WriteProcessMemory(pi.hProcess, mem, buf, sizeof(buf), NULL);
        ok(ret, "WriteProcessMemory failed: %u\n", GetLastError());

        ctx.thread_tag = 1;

        worker_cnt = 20;
        for (i = 0; i < worker_cnt; i++)
        {
            thread = CreateRemoteThread(pi.hProcess, NULL, 0, (void*)thread_proc, NULL, 0, NULL);
            ok(thread != NULL, "CreateRemoteThread failed: %u\n", GetLastError());

            next_event(&ctx, 20000);
            ok(ctx.ev.dwDebugEventCode == CREATE_THREAD_DEBUG_EVENT, "dwDebugEventCode = %d\n", ctx.ev.dwDebugEventCode);

            ret = CloseHandle(thread);
            ok(ret, "CloseHandle failed, last error %d.\n", GetLastError());
        }

        byte = OP_BP;
        ret = WriteProcessMemory(pi.hProcess, thread_proc + 1, &byte, 1, NULL);
        ok(ret, "WriteProcessMemory failed: %u\n", GetLastError());

        expect_breakpoint_exception(&ctx, thread_proc + 1);
        exception_cnt = 1;

        debuggee_thread = ctx.current_thread;
        fetch_process_context(&ctx);
        ok(get_ip(&ctx.current_thread->ctx) == thread_proc + 2, "unexpected instruction pointer %p\n",
           get_ip(&ctx.current_thread->ctx));

        byte = 0xc3; /* ret */
        ret = WriteProcessMemory(pi.hProcess, thread_proc + 1, &byte, 1, NULL);
        ok(ret, "WriteProcessMemory failed: %u\n", GetLastError());

        for (;;)
        {
            DEBUG_EVENT ev;

            /* even when there are more pending events, they are not reported until current event is continued */
            ret = WaitForDebugEvent(&ev, 10);
            ok(GetLastError() == ERROR_SEM_TIMEOUT, "WaitForDebugEvent returned %x(%u)\n", ret, GetLastError());

            next_event(&ctx, 100);
            if (ctx.ev.dwDebugEventCode != EXCEPTION_DEBUG_EVENT) break;
            trace("exception at %p in thread %04x\n", ctx.ev.u.Exception.ExceptionRecord.ExceptionAddress, ctx.ev.dwThreadId);
            ok(ctx.ev.u.Exception.ExceptionRecord.ExceptionCode == EXCEPTION_BREAKPOINT, "ExceptionCode = %x\n",
               ctx.ev.u.Exception.ExceptionRecord.ExceptionCode);
            ok(ctx.ev.u.Exception.ExceptionRecord.ExceptionAddress == thread_proc + 1,
               "ExceptionAddress = %p\n", ctx.ev.u.Exception.ExceptionRecord.ExceptionAddress);
            ok(get_ip(&ctx.current_thread->ctx) == thread_proc + 2
               || broken(get_ip(&ctx.current_thread->ctx) == thread_proc), /* sometimes observed on win10 */
               "unexpected instruction pointer %p\n",
               get_ip(&ctx.current_thread->ctx));
            exception_cnt++;
        }

        trace("received %u exceptions\n", exception_cnt);

        for (;;)
        {
            ok(ctx.ev.dwDebugEventCode == EXIT_THREAD_DEBUG_EVENT
               || broken(ctx.ev.dwDebugEventCode == CREATE_THREAD_DEBUG_EVENT), /* sometimes happens on vista */
               "dwDebugEventCode = %d\n", ctx.ev.dwDebugEventCode);
            if (ctx.ev.dwDebugEventCode == EXIT_THREAD_DEBUG_EVENT && !--worker_cnt) break;
            next_event(&ctx, 2000);
        }
    }

    SetEvent(event);

    do
    {
        next_event(&ctx, WAIT_EVENT_TIMEOUT);
        ok (ctx.ev.dwDebugEventCode != EXCEPTION_DEBUG_EVENT, "got exception\n");
    }
    while (ctx.ev.dwDebugEventCode != EXIT_PROCESS_DEBUG_EVENT);

    ret = CloseHandle(event);
    ok(ret, "CloseHandle failed, last error %d.\n", GetLastError());
    ret = CloseHandle(pi.hThread);
    ok(ret, "CloseHandle failed, last error %d.\n", GetLastError());
    ret = CloseHandle(pi.hProcess);
    ok(ret, "CloseHandle failed, last error %d.\n", GetLastError());
}

START_TEST(debugger)
{
    HMODULE hdll;

    hdll=GetModuleHandleA("kernel32.dll");
    pCheckRemoteDebuggerPresent=(void*)GetProcAddress(hdll, "CheckRemoteDebuggerPresent");
    pDebugActiveProcessStop=(void*)GetProcAddress(hdll, "DebugActiveProcessStop");
    pDebugSetProcessKillOnExit=(void*)GetProcAddress(hdll, "DebugSetProcessKillOnExit");
    pIsDebuggerPresent=(void*)GetProcAddress(hdll, "IsDebuggerPresent");

    ntdll = GetModuleHandleA("ntdll.dll");
    pDbgBreakPoint = (void*)GetProcAddress(ntdll, "DbgBreakPoint");

    myARGC=winetest_get_mainargs(&myARGV);
    if (myARGC >= 3 && strcmp(myARGV[2], "crash") == 0)
    {
        doCrash(myARGC, myARGV);
    }
    else if (myARGC >= 3 && strncmp(myARGV[2], "dbg,", 4) == 0)
    {
        doDebugger(myARGC, myARGV);
    }
    else if (myARGC >= 5 && !strcmp(myARGV[2], "child"))
    {
        doChild(myARGC, myARGV);
    }
    else if (myARGC >= 4 && !strcmp(myARGV[2], "children"))
    {
        doChildren(myARGC, myARGV);
    }
    else if (myARGC >= 4 && !strcmp(myARGV[2], "wait"))
    {
        DWORD event;
        sscanf(myARGV[3], "%x", &event);
        wait_debugger((HANDLE)(DWORD_PTR)event);
    }
    else
    {
        test_ExitCode();
        test_RemoteDebugger();
        test_debug_loop(myARGC, myARGV);
        test_debug_children(myARGV[0], DEBUG_PROCESS, TRUE);
        test_debug_children(myARGV[0], DEBUG_ONLY_THIS_PROCESS, FALSE);
        test_debug_children(myARGV[0], DEBUG_PROCESS|DEBUG_ONLY_THIS_PROCESS, FALSE);
        test_debug_children(myARGV[0], 0, FALSE);
        test_debugger(myARGV[0]);
    }
}
