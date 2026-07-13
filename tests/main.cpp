// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Real-consumer smoke build. Validates the .targets-driven consumer path
// end-to-end: this TU and the consumer-side V8JsiRuntime.cpp / JsiAbiRuntime.cpp
// / jsi.cpp (pulled in by Microsoft.JavaScript.V8.<rid>.targets) all compile
// against the shipped header layout, link against v8jsi.dll.lib, and run against
// v8jsi.dll at load time.
//
// The default build also exercises the Hybrid CRT cross-CRT boundary: when built
// /MDd (Debug-CRT) and linked against a Release Hybrid-CRT v8jsi.dll, every
// allocation/free below stays on the consumer-CRT side. The DLL sees only the C
// ABI vtable; STL types (std::string, jsi::JSError, etc.) never cross it.
//
// When built with SMOKE_SANDBOX defined (the consumer .vcxproj sets it when
// UseV8Sandbox=true), the harness additionally validates the sandbox package
// surface: it drives the jitless sandbox engine v8jsisb.dll through the same JSI
// C++ API (via a delay-load redirect) and confirms the broker ABI is consumable
// (compiles against sbox.h + links sbox.dll.lib + loads sbox.dll).
//
// Scenario selection (argv):
//   (no args)                  cross-CRT / sandbox-surface smoke (runJsiChecks).
//   --snapshot <blob>          create a runtime FROM a startup-snapshot blob and
//                              assert the baked-in global is restored with NO
//                              script evaluated (the public
//                              V8RuntimeArgs::startupSnapshotBlob surface).
//   --snapshot-reject <blob>   feed an INCOMPATIBLE (cross-engine) blob; assert
//                              the engine rejects it and falls back to a normal
//                              runtime (global absent), never crashing.
// The same exe serves both engines: default links v8jsi.dll; SMOKE_SANDBOX
// redirects to v8jsisb.dll — so each snapshot scenario runs against whichever
// engine this build targets.
//
// Exit code:
//   0 = all checks passed.
//   1 = any failure (runtime construction, evaluation, type or value
//       mismatch, exception not propagated, etc.).

#include <V8JsiRuntime.h>
#include <jsi/jsi.h>

#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <string>

#ifdef SMOKE_SANDBOX
#include <sbox.h>

#include <windows.h>
#include <delayimp.h>
#include <string.h>  // _stricmp
#endif

namespace jsi = facebook::jsi;

#define FAIL(msg)                                       \
  do {                                                  \
    std::fprintf(stderr, "test-harness: %s\n", (msg));  \
    return 1;                                           \
  } while (0)

#ifdef SMOKE_SANDBOX
// Redirect the delay-loaded "v8jsi.dll" import to the jitless sandbox engine
// v8jsisb.dll, so the JSI C++ API (makeV8Runtime) drives the sandbox engine
// in-process. v8jsisb.dll exposes the same JSI ABI as v8jsi.dll, so every
// import resolves from it. delayimp.lib reads __pfnDliNotifyHook2 on the first
// delay-load; it is declared `const` in <delayimp.h>, so we install the hook by
// defining that variable (the linker takes our definition over the library
// default) rather than assigning it at runtime.
static FARPROC WINAPI SmokeDelayHook(unsigned notify, PDelayLoadInfo info) {
  if (notify == dliNotePreLoadLibrary && info->szDll &&
      _stricmp(info->szDll, "v8jsi.dll") == 0) {
    return reinterpret_cast<FARPROC>(::LoadLibraryW(L"v8jsisb.dll"));
  }
  return nullptr;
}

extern "C" const PfnDliHook __pfnDliNotifyHook2 = SmokeDelayHook;

// Reference an sbox export so the linker pulls in sbox.dll.lib: the build fails
// with an unresolved external if UseV8Sandbox did not wire the import lib. This
// only forces the link; it never calls into the broker (no target is spawned).
static void* volatile g_sboxLinkProbe =
    reinterpret_cast<void*>(&sbox_broker_run);
#endif

// The shared JSI-ABI exercises. Run identically against v8jsi.dll (default) and
// v8jsisb.dll (SMOKE_SANDBOX) — all are jitless-compatible (no JIT required).
static int runJsiChecks(jsi::Runtime &runtime) {
  // 1) Baseline number evaluation.
  {
    auto src = std::make_shared<jsi::StringBuffer>("1 + 2");
    auto r = runtime.evaluateJavaScript(src, "smoke-num.js");
    if (!r.isNumber() || r.getNumber() != 3.0) FAIL("1 + 2 != 3");
  }

  // 2) utf8 round-trip. JSI String allocated DLL-side via the C ABI is
  //    surfaced to the consumer through Runtime::utf8 (defined in jsi.cpp,
  //    consumer-compiled). The returned std::string lives entirely in the
  //    consumer's CRT heap; the DLL never sees the std::string object.
  {
    auto src = std::make_shared<jsi::StringBuffer>("'hello, ' + 'world'");
    auto r = runtime.evaluateJavaScript(src, "smoke-str.js");
    if (!r.isString()) FAIL("expected string result");
    std::string utf8 = r.asString(runtime).utf8(runtime);
    if (utf8 != "hello, world") FAIL("utf8 round-trip mismatch");
  }

  // 3) Global property set/get. createPropNameIDFromAscii inside jsi.cpp
  //    allocates a PropNameID handle through the C ABI; setProperty /
  //    getProperty round-trip a number value.
  {
    runtime.global().setProperty(runtime, "x", 42);
    auto v = runtime.global().getProperty(runtime, "x");
    if (!v.isNumber() || v.getNumber() != 42.0) FAIL("global.x != 42");
  }

  // 4) HostFunction create + call. The std::function lambda captures
  //    consumer-side state (the call counter); the engine invokes it through
  //    the C ABI host-function callback.
  {
    int calls = 0;
    auto fn = jsi::Function::createFromHostFunction(
        runtime,
        jsi::PropNameID::forAscii(runtime, "add"),
        2,
        [&calls](jsi::Runtime &rt, const jsi::Value &, const jsi::Value *a,
                 size_t n) -> jsi::Value {
          ++calls;
          if (n != 2 || !a[0].isNumber() || !a[1].isNumber())
            throw jsi::JSError(rt, "bad args");
          return jsi::Value(a[0].getNumber() + a[1].getNumber());
        });
    auto r = fn.call(runtime, jsi::Value(7.0), jsi::Value(35.0));
    if (!r.isNumber() || r.getNumber() != 42.0) FAIL("host fn result != 42");
    if (calls != 1) FAIL("host fn call counter != 1");
  }

  // 5) JS exception caught consumer-side. JSError is constructed inside
  //    jsi.cpp (consumer-compiled) when the C ABI signals a pending exception
  //    via jsi_status; thrown and caught entirely in the consumer's CRT. The
  //    engine never participates in the exception unwinding.
  {
    auto src = std::make_shared<jsi::StringBuffer>("throw new Error('boom')");
    bool caught = false;
    try {
      runtime.evaluateJavaScript(src, "smoke-throw.js");
    } catch (const jsi::JSError &) {
      caught = true;
    }
    if (!caught) FAIL("JSError was not propagated to consumer");
  }

  return 0;
}

// Read a whole file into a byte string. Returns false on any open/read failure.
static bool readFileBytes(const char *path, std::string &out) {
  FILE *f = nullptr;
  if (::fopen_s(&f, path, "rb") != 0 || !f) return false;
  ::fseek(f, 0, SEEK_END);
  long sz = ::ftell(f);
  ::fseek(f, 0, SEEK_SET);
  if (sz < 0) {
    ::fclose(f);
    return false;
  }
  out.resize(static_cast<size_t>(sz));
  size_t got = sz > 0 ? ::fread(&out[0], 1, static_cast<size_t>(sz), f) : 0;
  ::fclose(f);
  return got == static_cast<size_t>(sz);
}

// Snapshot scenario: build a runtime FROM a startup-snapshot blob via the public
// V8RuntimeArgs::startupSnapshotBlob field — the consumer-facing surface — and
// check the baked-in global. expectMarker=true: a MATCHING blob, so
// globalThis.snapshotMarker must be 42 with NO script evaluated (it can only
// have come from the snapshot). expectMarker=false: an INCOMPATIBLE
// (cross-engine) blob, which the engine must REJECT and fall back to a normal
// runtime (marker absent) — never crash. Either way the runtime must still run
// JS afterwards.
static int runSnapshotScenario(const char *blobPath, bool expectMarker) {
  std::string blob;
  if (!readFileBytes(blobPath, blob)) {
    std::fprintf(stderr, "test-harness: cannot read snapshot blob %s\n",
                 blobPath);
    return 1;
  }
  v8runtime::V8RuntimeArgs args;
  args.startupSnapshotBlob =
      std::make_shared<jsi::StringBuffer>(std::move(blob));
  auto runtime = v8runtime::makeV8Runtime(std::move(args));
  if (!runtime) FAIL("makeV8Runtime(snapshot) returned null");

  // Read the baked-in global BEFORE evaluating anything: if present it can only
  // have come from the snapshot, proving no parse/execute happened here.
  auto marker = runtime->global().getProperty(*runtime, "snapshotMarker");
  if (expectMarker) {
    if (!marker.isNumber() || marker.getNumber() != 42.0)
      FAIL("snapshotMarker != 42 — snapshot was not restored");
  } else {
    if (!marker.isUndefined())
      FAIL("snapshotMarker present — an incompatible snapshot was NOT rejected");
  }

  // Liveness: the runtime must still evaluate JS in either case.
  auto r = runtime->evaluateJavaScript(
      std::make_shared<jsi::StringBuffer>("1 + 1"), "snapshot-live.js");
  if (!r.isNumber() || r.getNumber() != 2.0)
    FAIL("runtime not live after the snapshot path");

  std::printf("test-harness: snapshot %s OK\n",
              expectMarker ? "consume (marker restored)"
                           : "reject (incompatible blob fell back)");
  return 0;
}

int main(int argc, char **argv) {
  // Scenario dispatch. Snapshot modes return before the default smoke checks.
  const char *snapshotPath = nullptr;
  bool expectMarker = true;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--snapshot") == 0 && i + 1 < argc) {
      snapshotPath = argv[++i];
      expectMarker = true;
    } else if (std::strcmp(argv[i], "--snapshot-reject") == 0 && i + 1 < argc) {
      snapshotPath = argv[++i];
      expectMarker = false;
    }
  }

  try {
    if (snapshotPath)
      return runSnapshotScenario(snapshotPath, expectMarker);

    // The delay-load redirect to v8jsisb.dll is installed by the file-scope
    // __pfnDliNotifyHook2 definition above (SMOKE_SANDBOX builds only), so it is
    // already in effect before the first v8jsi.dll delay-import below.
    v8runtime::V8RuntimeArgs args;
    auto runtime = v8runtime::makeV8Runtime(std::move(args));
    if (!runtime) FAIL("makeV8Runtime returned null");

    if (int rc = runJsiChecks(*runtime)) return rc;

#ifdef SMOKE_SANDBOX
    // Confirm v8jsisb.dll actually backed the runtime (the redirect resolved).
    if (!::GetModuleHandleW(L"v8jsisb.dll"))
      FAIL("v8jsisb.dll was not loaded — delay-load redirect did not take");

    // Prove sbox.dll is shipped, loadable, and exports the broker C ABI —
    // without spawning a sandboxed target (the full broker/target path is
    // covered by the lockdown tier, not this surface smoke). g_sboxLinkProbe
    // already forced the sbox.dll.lib import at link time.
    (void)g_sboxLinkProbe;
    HMODULE sb = ::LoadLibraryW(L"sbox.dll");
    if (!sb) FAIL("LoadLibrary(sbox.dll) failed");
    if (!::GetProcAddress(sb, "sbox_broker_run"))
      FAIL("sbox.dll missing the sbox_broker_run export");

    std::printf(
        "test-harness: sandbox smoke OK "
        "(v8jsisb.dll via the JSI C++ API + sbox.dll broker surface)\n");
#else
    std::printf("test-harness: cross-CRT smoke OK\n");
#endif
    return 0;
  } catch (const std::exception &ex) {
    std::fprintf(stderr, "test-harness: unexpected exception: %s\n", ex.what());
    return 1;
  } catch (...) {
    std::fprintf(stderr, "test-harness: unknown exception\n");
    return 1;
  }
}
