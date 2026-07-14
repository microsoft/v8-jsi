// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// mkv8snapshot — offline tool that builds a V8 startup-snapshot blob from a
// pure-JS builder script. It LoadLibrary's a chosen v8jsi engine DLL
// (v8jsi.dll or the sandbox variant v8jsisb.dll) and calls its
// `v8_create_startup_snapshot` C export, then writes the blob to a file.
//
// Using the actual shipped engine DLL guarantees the blob matches that engine's
// V8 build flags (pointer-compression / cage / lite-mode / CachedDataVersionTag)
// — the blob is only loadable by an engine built identically. The same tool
// therefore serves both engines; pick with --engine.
//
// This program has NO V8 or v8jsi link dependency: it resolves the two C exports
// at runtime, so it is a tiny standalone executable.
//
// Usage:
//   mkv8snapshot --builder <file.js> --out <file.bin>
//                [--engine <dll>]        (default: v8jsisb.dll)
//                [--engine-dir <dir>]    (default: this exe's directory)
//                [--jitless | --no-jitless]
//
// --jitless forwards V8's --jitless to the snapshot creator so it matches a
// jitless consumer (the Untrusted sandbox tier runs v8jsisb jitless). When not
// specified it defaults ON for an engine whose name contains "sb", else OFF.

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

// Mirrors the engine's extern "C" exports (see src/jsi_abi/v8_jsi_config.h).
// The return is jsi_error_code; 0 == jsi_no_error == success.
using CreateSnapshotFn = int(__cdecl*)(
    const char* script_utf8,
    size_t script_size,
    const char* source_url,
    bool jitless,
    uint8_t** out_blob,
    size_t* out_blob_size);
using FreeSnapshotFn = void(__cdecl*)(uint8_t* blob);

std::wstring Widen(const std::string& s) {
  if (s.empty()) return std::wstring();
  int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w(n, L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
  return w;
}

std::wstring ExeDir() {
  wchar_t buf[MAX_PATH];
  DWORD n = ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
  std::wstring path(buf, n);
  size_t slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? L"." : path.substr(0, slash);
}

bool ReadFileBytes(const std::string& path, std::string& out) {
  FILE* f = nullptr;
  if (::fopen_s(&f, path.c_str(), "rb") != 0 || !f) return false;
  ::fseek(f, 0, SEEK_END);
  long sz = ::ftell(f);
  ::fseek(f, 0, SEEK_SET);
  if (sz < 0) { ::fclose(f); return false; }
  out.resize((size_t)sz);
  size_t got = sz > 0 ? ::fread(&out[0], 1, (size_t)sz, f) : 0;
  ::fclose(f);
  return got == (size_t)sz;
}

bool WriteFileBytes(const std::string& path, const uint8_t* data, size_t size) {
  FILE* f = nullptr;
  if (::fopen_s(&f, path.c_str(), "wb") != 0 || !f) return false;
  size_t put = size > 0 ? ::fwrite(data, 1, size, f) : 0;
  ::fclose(f);
  return put == size;
}

void Usage() {
  std::fprintf(stderr,
      "Usage: mkv8snapshot --builder <file.js> --out <file.bin>\n"
      "                    [--engine <dll>]       (default v8jsisb.dll)\n"
      "                    [--engine-dir <dir>]   (default this exe's dir)\n"
      "                    [--jitless | --no-jitless]\n");
}

} // namespace

int main(int argc, char** argv) {
  std::string builderPath, outPath, engineName = "v8jsisb.dll", engineDir;
  int jitlessOpt = -1; // -1 = derive from engine name

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : std::string(); };
    if (a == "--builder") builderPath = next();
    else if (a == "--out") outPath = next();
    else if (a == "--engine") engineName = next();
    else if (a == "--engine-dir") engineDir = next();
    else if (a == "--jitless") jitlessOpt = 1;
    else if (a == "--no-jitless") jitlessOpt = 0;
    else if (a == "-h" || a == "--help") { Usage(); return 0; }
    else { std::fprintf(stderr, "[mkv8snapshot] unknown arg: %s\n", a.c_str()); Usage(); return 2; }
  }

  if (builderPath.empty() || outPath.empty()) {
    std::fprintf(stderr, "[mkv8snapshot] --builder and --out are required\n");
    Usage();
    return 2;
  }

  const bool jitless = jitlessOpt >= 0
      ? (jitlessOpt == 1)
      : (engineName.find("sb") != std::string::npos);

  std::string builderJs;
  if (!ReadFileBytes(builderPath, builderJs)) {
    std::fprintf(stderr, "[mkv8snapshot] cannot read builder %s\n", builderPath.c_str());
    return 3;
  }

  // Resolve the engine path: --engine-dir, else this exe's directory, else as
  // given. Load by full path so we never honor the search path / allow planting.
  std::wstring enginePath;
  std::wstring engineW = Widen(engineName);
  if (!engineDir.empty()) {
    enginePath = Widen(engineDir) + L"\\" + engineW;
  } else if (engineName.find('\\') == std::string::npos &&
             engineName.find('/') == std::string::npos) {
    enginePath = ExeDir() + L"\\" + engineW;
  } else {
    enginePath = engineW;
  }

  HMODULE engine = ::LoadLibraryExW(
      enginePath.c_str(), nullptr,
      LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_APPLICATION_DIR |
          LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
  if (!engine) {
    std::fprintf(stderr, "[mkv8snapshot] LoadLibrary(%ls) failed: %lu\n",
                 enginePath.c_str(), ::GetLastError());
    return 4;
  }

  auto create = reinterpret_cast<CreateSnapshotFn>(
      ::GetProcAddress(engine, "v8_create_startup_snapshot"));
  auto freeBlob = reinterpret_cast<FreeSnapshotFn>(
      ::GetProcAddress(engine, "v8_free_startup_snapshot"));
  if (!create || !freeBlob) {
    std::fprintf(stderr,
        "[mkv8snapshot] engine %ls does not export v8_create_startup_snapshot "
        "(rebuild the engine with snapshot support)\n", enginePath.c_str());
    return 5;
  }

  std::fprintf(stderr,
      "[mkv8snapshot] engine=%ls jitless=%d builder=%s (%zu bytes)\n",
      enginePath.c_str(), jitless ? 1 : 0, builderPath.c_str(), builderJs.size());

  uint8_t* blob = nullptr;
  size_t blobSize = 0;
  int rc = create(builderJs.data(), builderJs.size(), "snapshot-builder.js",
                  jitless, &blob, &blobSize);
  if (rc != 0 || !blob || blobSize == 0) {
    std::fprintf(stderr, "[mkv8snapshot] v8_create_startup_snapshot failed (rc=%d)\n", rc);
    return 6;
  }

  bool ok = WriteFileBytes(outPath, blob, blobSize);
  freeBlob(blob);
  if (!ok) {
    std::fprintf(stderr, "[mkv8snapshot] cannot write %s\n", outPath.c_str());
    return 7;
  }

  std::fprintf(stderr, "[mkv8snapshot] wrote %s (%zu bytes)\n", outPath.c_str(), blobSize);
  return 0;
}
