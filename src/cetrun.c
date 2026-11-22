// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

// Test-only Windows x64 process-policy launcher. Never launch another cetrun.
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <intrin.h>
#include <dbghelp.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

static BOOL UserCetAvailable(void) {
  typedef BOOL(WINAPI * QueryCet)(DWORD);
  QueryCet query = (QueryCet)GetProcAddress(
      GetModuleHandleW(L"kernel32.dll"), "IsUserCetAvailableInEnvironment");
  return query && query(0);  // USER_CET_ENVIRONMENT_WIN32_PROCESS
}

static void Diagnostic(const wchar_t* mode, BOOL available, BOOL forcedUnavailable) {
  typedef LONG(WINAPI * GetVersion)(OSVERSIONINFOW*);
  GetVersion getVersion = (GetVersion)GetProcAddress(
      GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
  OSVERSIONINFOW version = {sizeof(version)};
  int regs[4];
  char brand[49] = "unknown";
  __cpuid(regs, (int)0x80000000);
  if ((unsigned)regs[0] >= 0x80000004) {
    for (int i = 0; i < 3; ++i) {
      __cpuid(regs, (int)0x80000002 + i);
      memcpy(brand + i * 16, regs, 16);
    }
    brand[48] = 0;
  }
  if (getVersion) getVersion(&version);
  fwprintf(stderr, L"cetrun: mode=%ls user_cet=%ls forced_unavailable=%d os=%lu.%lu.%lu cpu=%hs\n",
           mode, available ? L"yes" : L"no", forcedUnavailable, version.dwMajorVersion,
           version.dwMinorVersion, version.dwBuildNumber, brand);
}

// Quote every argument using the Windows CRT rules, including empty arguments,
// embedded quotes and runs of backslashes before quotes or the closing quote.
static wchar_t* CommandLine(int argc, wchar_t** argv) {
  wchar_t* line = (wchar_t*)calloc(32768, sizeof(wchar_t));
  size_t n = 0;
  if (!line) return NULL;
#define PUT(c) do { if (n == 32766) { free(line); return NULL; } line[n++] = (c); } while (0)
  for (int i = 2; i < argc; ++i) {
    if (i != 2) PUT(L' ');
    PUT(L'"');
    const wchar_t* p = argv[i];
    for (;;) {
      size_t slashes = 0;
      while (*p == L'\\') { ++slashes; ++p; }
      size_t copies = (*p == L'"' || !*p) ? slashes * 2 : slashes;
      while (copies--) PUT(L'\\');
      if (!*p) break;
      if (*p == L'"') PUT(L'\\');
      PUT(*p++);
    }
    PUT(L'"');
  }
#undef PUT
  return line;
}

// Compare opened files, not spellings: case, trailing dots, hard links and
// short/absolute paths can all name this executable. Launch the resolved path.
static BOOL FileIdentity(const wchar_t* path, BY_HANDLE_FILE_INFORMATION* info) {
  HANDLE file = CreateFileW(path, FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
  if (file == INVALID_HANDLE_VALUE) return FALSE;
  BOOL ok = GetFileInformationByHandle(file, info);
  DWORD error = GetLastError();
  CloseHandle(file);
  SetLastError(error);
  return ok;
}

static void TimeoutDump(const PROCESS_INFORMATION* child, const wchar_t* target) {
  typedef BOOL(WINAPI * WriteDump)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
      PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION,
      PMINIDUMP_CALLBACK_INFORMATION);
  wchar_t directory[MAX_PATH], path[MAX_PATH];
  HMODULE library = LoadLibraryExW(L"dbghelp.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
  WriteDump writeDump = library ? (WriteDump)GetProcAddress(library, "MiniDumpWriteDump") : NULL;
  HANDLE file = INVALID_HANDLE_VALUE;
  DWORD error = 0;
  fwprintf(stderr, L"cetrun: timed out target=%ls pid=%lu\n", target, child->dwProcessId);
  if (!writeDump) { error = GetLastError(); goto done; }
  if (!GetTempPathW(MAX_PATH, directory)) { error = GetLastError(); goto done; }
  if (swprintf(path, MAX_PATH, L"%lscetrun-%lu-%lu.dmp", directory,
        child->dwProcessId, GetTickCount()) < 0) { error = ERROR_FILENAME_EXCED_RANGE; goto done; }
  file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
  if (file == INVALID_HANDLE_VALUE) { error = GetLastError(); goto done; }
  if (!writeDump(child->hProcess, child->dwProcessId, file,
        (MINIDUMP_TYPE)(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory), NULL, NULL, NULL)) {
    error = GetLastError(); goto done;
  }
  fwprintf(stderr, L"cetrun: timeout minidump=%ls\n", path);
done:
  if (error) fwprintf(stderr, L"cetrun: timeout minidump failed: %lu\n", error);
  if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
  if (library) FreeLibrary(library);
}

int wmain(int argc, wchar_t** argv) {
  DWORD code = 3, error = 0, timeout = INFINITE;
  BOOL available = UserCetAvailable(), attributesReady = FALSE, finished = FALSE;
  // Fault injection is test-only: cetrun itself is never a shipped binary.
  BOOL forcedUnavailable = _wgetenv(L"CETRUN_TEST_NO_CET") != NULL;
  wchar_t target[32768], module[32768];
  BY_HANDLE_FILE_INFORMATION targetInfo, moduleInfo;
  int mode;
  DWORD64 policy[2] = {0, 0};
  HANDLE job = NULL;
  PROCESS_INFORMATION child = {0};
  STARTUPINFOEXW si = {0};
  SIZE_T bytes = 0;
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {0};
  wchar_t* command = NULL;
  const wchar_t* timeoutText = _wgetenv(L"CETRUN_TIMEOUT_MS");
  if (argc < 3 || ((mode = wcscmp(argv[1], L"off") == 0 ? 0 :
                           wcscmp(argv[1], L"compat") == 0 ? 1 :
                           wcscmp(argv[1], L"strict") == 0 ? 2 : -1) < 0)) {
    fwprintf(stderr, L"usage: cetrun <off|compat|strict> <exe> [args]\n");
    return 2;
  }
  DWORD length = SearchPathW(NULL, argv[2], L".exe", 32768, target, NULL);
  if (!length) goto failed;
  if (length >= 32768) { error = ERROR_FILENAME_EXCED_RANGE; goto done; }
  length = GetModuleFileNameW(NULL, module, 32768);
  if (!length) goto failed;
  if (length >= 32768) { error = ERROR_FILENAME_EXCED_RANGE; goto done; }
  if (!FileIdentity(target, &targetInfo) || !FileIdentity(module, &moduleInfo)) goto failed;
  if (targetInfo.dwVolumeSerialNumber == moduleInfo.dwVolumeSerialNumber &&
      targetInfo.nFileIndexHigh == moduleInfo.nFileIndexHigh &&
      targetInfo.nFileIndexLow == moduleInfo.nFileIndexLow) {
    fwprintf(stderr, L"cetrun: recursive launch is not allowed\n");
    return 2;
  }
  if (timeoutText) {
    wchar_t* end;
    unsigned long long value = _wcstoui64(timeoutText, &end, 10);
    if (!*timeoutText || *end || value == 0 || value >= INFINITE) {
      fwprintf(stderr, L"cetrun: invalid CETRUN_TIMEOUT_MS\n");
      return 2;
    }
    timeout = (DWORD)value;
  }
  Diagnostic(argv[1], available, forcedUnavailable);
  if ((!available || forcedUnavailable) && mode) {
    const wchar_t* requireCet = _wgetenv(L"CETRUN_REQUIRE_CET");
    if (requireCet && wcscmp(requireCet, L"1") == 0) {
      fwprintf(stderr, L"cetrun: user CET not available but CETRUN_REQUIRE_CET=1 (child not started)\n");
      return 6;
    }
    fwprintf(stderr, L"SKIP: user CET not available\n");
    return 0;
  }
  command = CommandLine(argc, argv);
  if (!command) { error = ERROR_NOT_ENOUGH_MEMORY; goto done; }
  job = CreateJobObjectW(NULL, NULL);
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                       &limits, sizeof(limits))) goto failed;
  si.StartupInfo.cb = sizeof(si);
  si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  si.StartupInfo.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  si.StartupInfo.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
  si.StartupInfo.hStdError = GetStdHandle(STD_ERROR_HANDLE);
  // Atomically attach the child to our kill-on-close job at creation: there
  // must be no orphan window if the launcher dies before the child resumes.
  DWORD attributeCount = available && !forcedUnavailable ? 2 : 1;
  InitializeProcThreadAttributeList(NULL, attributeCount, 0, &bytes);
  si.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(bytes);
  if (!si.lpAttributeList) { error = ERROR_NOT_ENOUGH_MEMORY; goto done; }
  if (!InitializeProcThreadAttributeList(si.lpAttributeList, attributeCount, 0, &bytes)) goto failed;
  attributesReady = TRUE;
  if (!UpdateProcThreadAttribute(si.lpAttributeList, 0,
        PROC_THREAD_ATTRIBUTE_JOB_LIST, &job, sizeof(job), NULL, NULL)) goto failed;
  if (available && !forcedUnavailable) {
    policy[1] = mode == 0 ? PROCESS_CREATION_MITIGATION_POLICY2_CET_USER_SHADOW_STACKS_ALWAYS_OFF :
                mode == 1 ? PROCESS_CREATION_MITIGATION_POLICY2_CET_USER_SHADOW_STACKS_ALWAYS_ON :
                            PROCESS_CREATION_MITIGATION_POLICY2_CET_USER_SHADOW_STACKS_STRICT_MODE;
    // STRICT_MODE is value 3 of the two-bit shadow-stack policy field.
    if (_wgetenv(L"CETRUN_TEST_ATTRIBUTE_FAILURE")) {
      SetLastError(ERROR_INVALID_PARAMETER); goto failed;
    }
    if (!UpdateProcThreadAttribute(si.lpAttributeList, 0,
          PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY, policy, sizeof(policy), NULL, NULL)) goto failed;
  }
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
  if (_wgetenv(L"CETRUN_TEST_CREATE_FAILURE")) {
    SetLastError(ERROR_NOT_SUPPORTED); goto failed;
  }
  if (!CreateProcessW(target, command, NULL, NULL, TRUE,
        CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
        NULL, NULL, &si.StartupInfo, &child)) goto failed;
  if (available && !forcedUnavailable) {
    PROCESS_MITIGATION_USER_SHADOW_STACK_POLICY observed = {0};
    PROCESS_MITIGATION_POLICY query = ProcessUserShadowStackPolicy;
    int expected = mode;
    if (_wgetenv(L"CETRUN_TEST_QUERY_FAILURE")) query = MaxProcessMitigationPolicy;
    if (_wgetenv(L"CETRUN_TEST_MISMATCH")) expected = mode == 0 ? 2 : 0;
    code = 4;
    if (!GetProcessMitigationPolicy(child.hProcess, query, &observed, sizeof(observed))) {
      fwprintf(stderr, L"cetrun: child policy query failed: %lu (child not resumed)\n", GetLastError());
      goto done;
    }
    fwprintf(stderr, L"cetrun: child policy shadowstack=%lu strict=%lu\n",
        (DWORD)observed.EnableUserShadowStack, (DWORD)observed.EnableUserShadowStackStrictMode);
    if (observed.EnableUserShadowStack != (DWORD)(expected != 0) ||
        observed.EnableUserShadowStackStrictMode != (DWORD)(expected == 2)) {
      fwprintf(stderr, L"cetrun: child policy mismatch (child not resumed)\n");
      goto done;
    }
  }
  code = 3;
  if (ResumeThread(child.hThread) == (DWORD)-1) goto failed;
  switch (WaitForSingleObject(child.hProcess, timeout)) {
    case WAIT_OBJECT_0:
      if (!GetExitCodeProcess(child.hProcess, &code)) goto failed;
      finished = TRUE;
      goto done;
    case WAIT_TIMEOUT:
      fwprintf(stderr, L"cetrun: timeout; terminating child job\n");
      TimeoutDump(&child, target);
      code = 5;
      if (!TerminateJobObject(job, code)) goto failed;
      goto done;
    default: goto failed;
  }
failed:
  code = 3;
  error = GetLastError();
done:
  if (error) fwprintf(stderr, L"cetrun: Windows operation failed: %lu\n", error);
  // No verification or resume failure may leave the suspended child alive.
  if (child.hProcess && !finished) {
    TerminateProcess(child.hProcess, code);
    WaitForSingleObject(child.hProcess, INFINITE);
  }
  if (job) CloseHandle(job);  // Kill descendants, including after normal exit.
  if (child.hThread) CloseHandle(child.hThread);
  if (child.hProcess) CloseHandle(child.hProcess);
  if (attributesReady) DeleteProcThreadAttributeList(si.lpAttributeList);
  free(si.lpAttributeList);
  free(command);
  ExitProcess(code);  // Preserve all 32 bits of the child's exit code.
}
