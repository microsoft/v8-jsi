// memprobe.exe — footprint sampler (measurement-only; not shipped).
//
// Given one or more PIDs, prints a JSON object per PID with the memory metrics
// that decide the sandbox-vs-WebView2 RAM story:
//   * working set: total / private / shareable (from QueryWorkingSet page flags)
//   * private commit (PrivateUsage from PROCESS_MEMORY_COUNTERS_EX)
//   * address space: reserved vs committed (VirtualQueryEx walk) + the single
//     largest reserved region (the V8 pointer-compression cage reserves a huge
//     range but commits little — reserve is cheap, commit is what counts).
//
// Usage:  memprobe.exe <pid> [<pid> ...]
//         memprobe.exe --name v8host.exe        (sum all processes by image name)
//         memprobe.exe --tree <root_pid>        (root + all descendants by PPID)
//
// Output: one JSON object per line; a final {"summary":...} line aggregating all.
// All sizes in bytes. A normal/elevated harness can query lower-integrity
// sandboxed children (query is allowed up the integrity ladder).

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <psapi.h>
#include <tlhelp32.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "psapi.lib")

namespace {

struct Metrics {
  DWORD pid = 0;
  std::wstring name;
  bool opened = false;
  uint64_t ws_total = 0;     // GetProcessMemoryInfo WorkingSetSize
  uint64_t ws_private = 0;   // QueryWorkingSet: pages with Shared==0
  uint64_t ws_shareable = 0; // QueryWorkingSet: pages with Shared==1
  uint64_t priv_commit = 0;  // PrivateUsage (private bytes / commit charge)
  uint64_t reserved = 0;     // sum of regions State != FREE
  uint64_t committed = 0;    // sum of regions State == COMMIT
  uint64_t largest_reserve = 0;  // largest single MEM_RESERVE region (the cage)
  uint64_t pagefile_commit = 0;  // PagefileUsage
  uint32_t regions = 0;
};

// QueryWorkingSet returns an array whose first element is the count, followed by
// PSAPI_WORKING_SET_BLOCK values. Bit 8 (0x100) of each block is the Shared flag.
void CollectWorkingSet(HANDLE h, Metrics* m) {
  // Size the buffer generously; retry on ERROR_BAD_LENGTH with the returned count.
  std::vector<ULONG_PTR> buf(1 << 16, 0);
  for (int attempt = 0; attempt < 6; ++attempt) {
    if (QueryWorkingSet(h, buf.data(), (DWORD)(buf.size() * sizeof(ULONG_PTR)))) {
      break;
    }
    if (GetLastError() != ERROR_BAD_LENGTH)
      return;  // can't read WS (rights); leave private/shareable at 0
    // buf[0] holds the required entry count on ERROR_BAD_LENGTH.
    size_t needed = buf[0] + 16;
    buf.assign(needed, 0);
  }
  ULONG_PTR count = buf[0];
  const ULONG_PTR kShared = 0x100;  // PSAPI_WORKING_SET_BLOCK.Shared bit
  SYSTEM_INFO si = {};
  GetSystemInfo(&si);
  const uint64_t page = si.dwPageSize ? si.dwPageSize : 4096;
  uint64_t shared_pages = 0, private_pages = 0;
  for (ULONG_PTR i = 1; i <= count && i < buf.size(); ++i) {
    if (buf[i] & kShared)
      ++shared_pages;
    else
      ++private_pages;
  }
  m->ws_private = private_pages * page;
  m->ws_shareable = shared_pages * page;
}

void CollectVirtualWalk(HANDLE h, Metrics* m) {
  MEMORY_BASIC_INFORMATION mbi = {};
  uintptr_t addr = 0;
  // User address space upper bound on x64.
  const uintptr_t kMax = (uintptr_t)0x7FFFFFFF0000ULL;
  uint32_t regions = 0;
  while (addr < kMax) {
    SIZE_T got = VirtualQueryEx(h, (LPCVOID)addr, &mbi, sizeof(mbi));
    if (got == 0)
      break;
    if (mbi.State == MEM_RESERVE || mbi.State == MEM_COMMIT) {
      m->reserved += mbi.RegionSize;
      ++regions;
      if (mbi.State == MEM_COMMIT)
        m->committed += mbi.RegionSize;
      if (mbi.State == MEM_RESERVE && mbi.RegionSize > m->largest_reserve)
        m->largest_reserve = mbi.RegionSize;
    }
    uintptr_t next = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    if (next <= addr)
      break;  // guard against wrap
    addr = next;
  }
  m->regions = regions;
}

bool Probe(DWORD pid, const std::wstring& name, Metrics* out) {
  out->pid = pid;
  out->name = name;
  HANDLE h = OpenProcess(
      PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
  if (!h) {
    h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h)
      return false;
  }
  out->opened = true;
  PROCESS_MEMORY_COUNTERS_EX pmc = {};
  pmc.cb = sizeof(pmc);
  if (GetProcessMemoryInfo(h, (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) {
    out->ws_total = pmc.WorkingSetSize;
    out->priv_commit = pmc.PrivateUsage;
    out->pagefile_commit = pmc.PagefileUsage;
  }
  CollectWorkingSet(h, out);
  CollectVirtualWalk(h, out);
  CloseHandle(h);
  return true;
}

void PrintJson(const Metrics& m) {
  // Escape-free: image names have no JSON-special chars here.
  char namebuf[256] = {};
  WideCharToMultiByte(CP_UTF8, 0, m.name.c_str(), -1, namebuf, sizeof(namebuf) - 1,
                      nullptr, nullptr);
  printf(
      "{\"pid\":%lu,\"name\":\"%s\",\"opened\":%s,"
      "\"ws_total\":%llu,\"ws_private\":%llu,\"ws_shareable\":%llu,"
      "\"priv_commit\":%llu,\"pagefile_commit\":%llu,"
      "\"reserved\":%llu,\"committed\":%llu,\"largest_reserve\":%llu,"
      "\"regions\":%lu}\n",
      m.pid, namebuf, m.opened ? "true" : "false",
      (unsigned long long)m.ws_total, (unsigned long long)m.ws_private,
      (unsigned long long)m.ws_shareable, (unsigned long long)m.priv_commit,
      (unsigned long long)m.pagefile_commit, (unsigned long long)m.reserved,
      (unsigned long long)m.committed, (unsigned long long)m.largest_reserve,
      m.regions);
}

struct ProcRow {
  DWORD pid;
  DWORD ppid;
  std::wstring name;
};

std::vector<ProcRow> SnapshotProcs() {
  std::vector<ProcRow> rows;
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE)
    return rows;
  PROCESSENTRY32W pe = {};
  pe.dwSize = sizeof(pe);
  if (Process32FirstW(snap, &pe)) {
    do {
      rows.push_back({pe.th32ProcessID, pe.th32ParentProcessID, pe.szExeFile});
    } while (Process32NextW(snap, &pe));
  }
  CloseHandle(snap);
  return rows;
}

std::vector<std::pair<DWORD, std::wstring>> FindByName(const std::wstring& want) {
  std::vector<std::pair<DWORD, std::wstring>> out;
  for (auto& r : SnapshotProcs())
    if (_wcsicmp(r.name.c_str(), want.c_str()) == 0)
      out.emplace_back(r.pid, r.name);
  return out;
}

// Root + all transitive descendants by parent-PID. PPIDs from the snapshot are a
// point-in-time view; sufficient for sampling a held, quiescent process tree.
std::vector<std::pair<DWORD, std::wstring>> FindTree(DWORD root) {
  std::vector<ProcRow> rows = SnapshotProcs();
  std::vector<std::pair<DWORD, std::wstring>> out;
  std::vector<DWORD> frontier = {root};
  // Include the root itself (find its name).
  for (auto& r : rows)
    if (r.pid == root)
      out.emplace_back(r.pid, r.name);
  while (!frontier.empty()) {
    DWORD cur = frontier.back();
    frontier.pop_back();
    for (auto& r : rows) {
      if (r.ppid == cur && r.pid != cur && r.pid != 0) {
        // Avoid revisiting (guard against PID-reuse cycles).
        bool seen = false;
        for (auto& o : out)
          if (o.first == r.pid) { seen = true; break; }
        if (seen)
          continue;
        out.emplace_back(r.pid, r.name);
        frontier.push_back(r.pid);
      }
    }
  }
  return out;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: memprobe <pid>... | --name <image.exe>\n");
    return 2;
  }
  std::vector<std::pair<DWORD, std::wstring>> targets;
  if (wcscmp(argv[1], L"--name") == 0 && argc >= 3) {
    targets = FindByName(argv[2]);
  } else if (wcscmp(argv[1], L"--tree") == 0 && argc >= 3) {
    targets = FindTree((DWORD)_wtoi(argv[2]));
  } else {
    for (int i = 1; i < argc; ++i)
      targets.emplace_back((DWORD)_wtoi(argv[i]), L"");
  }

  Metrics sum = {};
  sum.name = L"SUMMARY";
  uint32_t counted = 0;
  for (auto& t : targets) {
    Metrics m = {};
    if (!Probe(t.first, t.second, &m)) {
      m.pid = t.first;
      m.name = t.second;
      PrintJson(m);  // opened=false
      continue;
    }
    PrintJson(m);
    sum.ws_total += m.ws_total;
    sum.ws_private += m.ws_private;
    sum.ws_shareable += m.ws_shareable;
    sum.priv_commit += m.priv_commit;
    sum.pagefile_commit += m.pagefile_commit;
    sum.reserved += m.reserved;
    sum.committed += m.committed;
    if (m.largest_reserve > sum.largest_reserve)
      sum.largest_reserve = m.largest_reserve;
    sum.regions += m.regions;
    ++counted;
  }
  sum.opened = counted > 0;
  // Reuse PrintJson but tag as summary with a process count via pid field = count.
  sum.pid = counted;
  printf("{\"summary\":true,\"count\":%u,"
         "\"ws_total\":%llu,\"ws_private\":%llu,\"ws_shareable\":%llu,"
         "\"priv_commit\":%llu,\"pagefile_commit\":%llu,"
         "\"reserved\":%llu,\"committed\":%llu,\"largest_reserve\":%llu,"
         "\"regions\":%lu}\n",
         counted, (unsigned long long)sum.ws_total,
         (unsigned long long)sum.ws_private, (unsigned long long)sum.ws_shareable,
         (unsigned long long)sum.priv_commit,
         (unsigned long long)sum.pagefile_commit,
         (unsigned long long)sum.reserved, (unsigned long long)sum.committed,
         (unsigned long long)sum.largest_reserve, sum.regions);
  return 0;
}
