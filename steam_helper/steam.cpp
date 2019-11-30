/*
 * Copyright (c) 2019, Valve Corporation
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation and/or
 * other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its contributors
 * may be used to endorse or promote products derived from this software without
 * specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/* This is a stub steam.exe implementation for use inside Proton. It provides
 * a small subset of the actual Steam functionality for games that expect
 * Windows version of Steam running. */

#include <windows.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

#pragma push_macro("_WIN32")
#pragma push_macro("__cdecl")
#undef _WIN32
#undef __cdecl
#include "steam_api.h"
#pragma pop_macro("_WIN32")
#pragma pop_macro("__cdecl")

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(steam);

EXTERN_C HANDLE CDECL __wine_make_process_system(void);

static void set_active_process_pid(void)
{
    DWORD pid = GetCurrentProcessId();
    RegSetKeyValueA(HKEY_CURRENT_USER, "Software\\Valve\\Steam\\ActiveProcess", "pid", REG_DWORD, &pid, sizeof(pid));
}

static DWORD WINAPI create_steam_window(void *arg)
{
    static WNDCLASSEXW wndclass = { sizeof(WNDCLASSEXW) };
    static const WCHAR class_nameW[] = {'v','g','u','i','P','o','p','u','p','W','i','n','d','o','w',0};
    static const WCHAR steamW[] = {'S','t','e','a','m',0};
    MSG msg;

    wndclass.lpfnWndProc = DefWindowProcW;
    wndclass.lpszClassName = class_nameW;

    RegisterClassExW(&wndclass);
    CreateWindowW(class_nameW, steamW, WS_POPUP, 40, 40,
                  400, 300, NULL, NULL, NULL, NULL);

    while (GetMessageW(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return 0;
}

static void setup_steam_registry(void)
{
    const char *ui_lang;
    uint32 appid;
    char buf[256];
    HKEY key;
    LSTATUS status;

    if (!SteamAPI_Init())
    {
        WINE_ERR("SteamAPI_Init failed\n");
        return;
    }

    ui_lang = SteamUtils()->GetSteamUILanguage();
    WINE_TRACE("UI language: %s\n", wine_dbgstr_a(ui_lang));
    RegSetKeyValueA(HKEY_CURRENT_USER, "Software\\Valve\\Steam", "language",
                    REG_SZ, ui_lang, strlen(ui_lang) + 1);

    appid = SteamUtils()->GetAppID();
    WINE_TRACE("appid: %u\n", appid);
    sprintf(buf, "Software\\Valve\\Steam\\Apps\\%u", appid);
    status = RegCreateKeyA(HKEY_CURRENT_USER, buf, &key);
    if (!status)
    {
        DWORD value;
        value = 1;
        RegSetKeyValueA(key, NULL, "Installed", REG_DWORD, &value, sizeof(value));
        RegSetKeyValueA(key, NULL, "Running", REG_DWORD, &value, sizeof(value));
        value = 0;
        RegSetKeyValueA(key, NULL, "Updating", REG_DWORD, &value, sizeof(value));
        RegCloseKey(key);
    }
    else WINE_ERR("Could not create key: %u\n", status);

    SteamAPI_Shutdown();
}

static WCHAR *find_quote(WCHAR *str)
{
    WCHAR *end = wcschr(str, '"'), *ch;
    int odd;
    while (end)
    {
        odd = 0;
        ch = end - 1;
        while (ch >= str && *ch == '\\')
        {
            odd = !odd;
            --ch;
        }
        if (!odd)
            return end;
        end = wcschr(end + 1, '"');
    }
    return NULL;
}

struct steam_ceg_handles {
    HANDLE consume_handle;
    HANDLE produce_handle[2];
    HANDLE file_handle;
    void  *file_mapping;
};

struct steam_app_ceg_info {
    uint32_t pid;
    uint32_t active_process;
    char     startup_module[256];
    char     start_event   [256];
    char     term_event    [256];
};

static void setup_ceg_handles(steam_ceg_handles *handles) {
    SECURITY_DESCRIPTOR security_descriptor;
    SECURITY_ATTRIBUTES semaphore_attributes = {
        .nLength              = sizeof(SECURITY_ATTRIBUTES),
        .lpSecurityDescriptor = &security_descriptor,
        .bInheritHandle       = FALSE
    };

    InitializeSecurityDescriptor(&security_descriptor, 1);
    SetSecurityDescriptorDacl(&security_descriptor, 1, 0, 0);

    handles->consume_handle    = CreateSemaphoreA(&semaphore_attributes, 0, 512, "STEAM_DIPC_CONSUME");
    handles->produce_handle[0] = CreateSemaphoreA(&semaphore_attributes, 1, 512, "STEAM_DIPC_PRODUCE");
    /* Some titles listen for a typo'ed version.*/
    handles->produce_handle[1] = CreateSemaphoreA(&semaphore_attributes, 1, 512, "SREAM_DIPC_PRODUCE");
    handles->file_handle       = CreateFileMappingA(INVALID_HANDLE_VALUE, &semaphore_attributes, PAGE_READWRITE, 0, 4096, "STEAM_DRM_IPC");
    handles->file_mapping      = MapViewOfFile(handles->file_handle, 0xF001Fu, 0, 0, 0);

    WINE_TRACE("CEG: Created base CEG handles + mapping.\n");
}

static void cleanup_ceg_handles(steam_ceg_handles *handles) {
    UnmapViewOfFile(handles->file_mapping);

    CloseHandle(handles->file_handle);
    CloseHandle(handles->produce_handle[0]);
    CloseHandle(handles->produce_handle[1]);
    CloseHandle(handles->consume_handle);

    WINE_TRACE("CEG: Cleaned up CEG handles.\n");
}

static void steam_ceg_interface(steam_ceg_handles* ceg_handles) {
    steam_app_ceg_info info;
    HANDLE start_handle;

    const char* ipc_data = (const char*)ceg_handles->file_mapping;

    WINE_TRACE("CEG: Waiting for CEG interface...\n");

    /* Wait 1.5s for the game to give us their CEG data.
     * Otherwise give up. */
    if (WaitForSingleObject(ceg_handles->consume_handle, 1500) != WAIT_OBJECT_0) {
        WINE_TRACE("CEG: No CEG interface.\n");
        return;
    }

    /* Read the stuff given to us by the IPC file */
    WINE_TRACE("CEG: Parsing pid...\n");
    info.pid = *((uint32_t*)(&ipc_data));
    ipc_data += sizeof(uint32_t);
    WINE_TRACE("CEG: pid: %u\n", info.pid);

    WINE_TRACE("CEG: Parsing active_process...\n");
    info.active_process = *((uint32_t*)(&ipc_data));
    ipc_data += sizeof(uint32_t);
    WINE_TRACE("CEG: active_process: %u\n", info.active_process);

    WINE_TRACE("CEG: Parsing startup_module...\n");
    strcpy(info.startup_module, ipc_data);
    ipc_data += strlen(info.startup_module);
    WINE_TRACE("CEG: startup_module: %s\n", info.startup_module);

    WINE_TRACE("CEG: Parsing start_event...\n");
    strcpy(info.start_event, ipc_data);
    ipc_data += strlen(info.start_event);
    WINE_TRACE("CEG: start_event: %s\n", info.start_event);

    WINE_TRACE("CEG: Parsing term_event...\n");
    strcpy(info.term_event, ipc_data);
    ipc_data += strlen(info.term_event);
    WINE_TRACE("CEG: term_event: %s\n", info.term_event);

    WINE_TRACE("CEG: Deleting startup module...\n");
    /* Delete the startup file given to us */
    DeleteFileA(info.startup_module);

    if (info.start_event[0] != '\0') {
        /* Trigger the event to show that we know the game has started */
        start_handle = OpenEventA(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, info.start_event);
        if (start_handle != NULL) {
            WINE_TRACE("CEG: Triggering event.\n");
            SetEvent(start_handle);
            CloseHandle(start_handle);
        }
        else
            WINE_TRACE("CEG: Invalid start event.\n");
    }
    else
        WINE_TRACE("CEG: No start event given.\n");

    WINE_TRACE("CEG: Releasing semaphore.\n");
    ReleaseSemaphore(ceg_handles->produce_handle[0], 1, NULL);
    ReleaseSemaphore(ceg_handles->produce_handle[1], 1, NULL);
    WINE_TRACE("CEG: Released semaphore.\n");
}

static HANDLE run_process(void)
{
    WCHAR *cmdline = GetCommandLineW();
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    DWORD flags = 0;
    steam_ceg_handles ceg_handles;

    /* skip argv[0] */
    if (*cmdline == '"')
    {
        cmdline = find_quote(cmdline + 1);
        if (cmdline) cmdline++;
    }
    else
    {
        cmdline = wcschr(cmdline, ' ');
    }
    if (!cmdline)
    {
        WINE_ERR("Invalid command\n");
        return INVALID_HANDLE_VALUE;
    }
    while (*cmdline == ' ') cmdline++;

    /* convert absolute unix path to dos */
    if (cmdline[0] == '/' ||
            (cmdline[0] == '"' && cmdline[1] == '/'))
    {
        WCHAR *scratchW;
        char *scratchA;
        WCHAR *start, *end, *dos, *remainder, *new_cmdline;
        size_t argv0_len;
        int r;
        DWORD_PTR console;
        SHFILEINFOW sfi;

        static const WCHAR dquoteW[] = {'"',0};

        WINE_TRACE("Converting unix command: %s\n", wine_dbgstr_w(cmdline));

        if (cmdline[0] == '"')
        {
            start = cmdline + 1;
            end = find_quote(start);
            if (!end)
            {
                WINE_ERR("Unmatched quote? %s\n", wine_dbgstr_w(cmdline));
                goto run;
            }
            remainder = end + 1;
        }
        else
        {
            start = cmdline;
            end = wcschr(start, ' ');
            if (!end)
                end = wcschr(start, '\0');
            remainder = end;
        }

        argv0_len = end - start;

        scratchW = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, (argv0_len + 1) * sizeof(WCHAR));
        memcpy(scratchW, start, argv0_len * sizeof(WCHAR));
        scratchW[argv0_len] = '\0';

        r = WideCharToMultiByte(CP_UNIXCP, 0, scratchW, -1,
                NULL, 0, NULL, NULL);
        if (!r)
        {
            WINE_ERR("Char conversion size failed?\n");
            goto run;
        }

        scratchA = (char *)HeapAlloc(GetProcessHeap(), 0, r);

        r = WideCharToMultiByte(CP_UNIXCP, 0, scratchW, -1,
                scratchA, r, NULL, NULL);
        if (!r)
        {
            WINE_ERR("Char conversion failed?\n");
            goto run;
        }

        dos = wine_get_dos_file_name(scratchA);

        CoInitialize(NULL);

        console = SHGetFileInfoW(dos, 0, &sfi, sizeof(sfi), SHGFI_EXETYPE);
        if (console && !HIWORD(console))
            flags |= CREATE_NEW_CONSOLE;

        new_cmdline = (WCHAR *)HeapAlloc(GetProcessHeap(), 0,
                (lstrlenW(dos) + 3 + lstrlenW(remainder) + 1) * sizeof(WCHAR));
        lstrcpyW(new_cmdline, dquoteW);
        lstrcatW(new_cmdline, dos);
        lstrcatW(new_cmdline, dquoteW);
        lstrcatW(new_cmdline, remainder);

        cmdline = new_cmdline;
    }

run:
    WINE_TRACE("Running command %s\n", wine_dbgstr_w(cmdline));

    setup_ceg_handles(&ceg_handles);

    if (!CreateProcessW(NULL, cmdline, NULL, NULL, FALSE, flags, NULL, NULL, &si, &pi))
    {
        WINE_ERR("Failed to create process %s: %u\n", wine_dbgstr_w(cmdline), GetLastError());
        return INVALID_HANDLE_VALUE;
    }

    steam_ceg_interface(&ceg_handles);

    cleanup_ceg_handles(&ceg_handles);

    CloseHandle(pi.hThread);
    return pi.hProcess;
}

int main(int argc, char *argv[])
{
    HANDLE wait_handle = INVALID_HANDLE_VALUE;

    WINE_TRACE("\n");

    if (getenv("SteamGameId"))
    {
        /* do setup only for game process */
        CreateThread(NULL, 0, create_steam_window, NULL, 0, NULL);

        set_active_process_pid();
        setup_steam_registry();

        wait_handle = __wine_make_process_system();
    }

    if (argc > 1)
    {
        HANDLE child;

        child = run_process();

        if (child == INVALID_HANDLE_VALUE)
            return 1;

        if (wait_handle == INVALID_HANDLE_VALUE)
            wait_handle = child;
        else
            CloseHandle(child);
    }

    WaitForSingleObject(wait_handle, INFINITE);

    return 0;
}
