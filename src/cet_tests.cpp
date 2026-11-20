// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "jsi/test/testlib.h"
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#if defined(_WIN32) && defined(_M_X64)
#include <intrin.h>
#include <windows.h>
#endif

using namespace facebook::jsi;

namespace {
// Match V8 flag names without changing values or the token used in diagnostics.
std::string NormalizeFlagName(std::string flag) {
  flag.erase(flag.find('=') == std::string::npos ? flag.size() : flag.find('='));
  if (flag.empty() || flag[0] != '-') return {};
  flag.erase(0, flag.size() > 1 && flag[1] == '-' ? 2 : 1);
  for (char& ch : flag) if (ch == '_') ch = '-';
  if (flag.rfind("no", 0) == 0 && (flag.size() == 2 || flag[2] != '-'))
    flag.insert(2, "-");
  return flag;
}

// Keep exactly one out-of-line native call below the host callback for both reads.
#if defined(_WIN32) && defined(_M_X64)
#if defined(__clang__)
// clang-cl's intrinsic takes the initial register value (RDSSP is a NOP off CET).
__declspec(noinline) __attribute__((target("shstk")))
uint64_t ReadShadowStackPointer() { return _rdsspq(0); }
#else
__declspec(noinline) uint64_t ReadShadowStackPointer() { return _rdsspq(); }
#endif
#else
uint64_t ReadShadowStackPointer() { return 0; }
#endif

class CetShadowStack : public JSITestBase {
 protected:
  void SetUp() override {
#if defined(_WIN32) && defined(_M_X64)
    PROCESS_MITIGATION_USER_SHADOW_STACK_POLICY policy{};
    using QueryPolicy = BOOL(WINAPI*)(HANDLE, PROCESS_MITIGATION_POLICY, PVOID, SIZE_T);
    auto query = reinterpret_cast<QueryPolicy>(GetProcAddress(
        GetModuleHandleW(L"kernel32.dll"), "GetProcessMitigationPolicy"));
    if (!query || !query(GetCurrentProcess(), ProcessUserShadowStackPolicy,
                         &policy, sizeof(policy)) ||
        !policy.EnableUserShadowStack || !policy.EnableUserShadowStackStrictMode)
      GTEST_SKIP() << "Requires enabled CET shadow stacks in strict mode; compat repair can hide leaks";
#endif
    if (!ReadShadowStackPointer())
      GTEST_SKIP() << "Requires Windows x64 with active CET shadow stacks";
    const char* flags = std::getenv("V8JSI_TEST_V8_FLAGS");
    flags_ = flags ? flags : "";
    // Do not parse even one native intrinsic until syntax has been enabled.
    std::istringstream words(flags_);
    bool natives = false;
    for (std::string word; words >> word;) {
      const auto name = NormalizeFlagName(word);
      if (name == "allow-natives-syntax") natives = true;
      if (name == "no-allow-natives-syntax") natives = false;
    }
    if (!natives) GTEST_SKIP() << "Requires --allow-natives-syntax";
    rt.global().setProperty(rt, "__ssp", Function::createFromHostFunction(
        rt, PropNameID::forAscii(rt, "__ssp"), 0,
        [](Runtime&, const Value&, const Value*, size_t) {
          return Value(static_cast<double>(ReadShadowStackPointer()));
        }));
  }

  // All handlers (including finally) in enabled cases require Sparkplug.
  void run(const char* body, bool interpreted = false, int depth = 0,
           int expectedCatches = 1, bool async = false, bool needsTurbofan = false) {
    // Check restrictions before tier intrinsics: CompileBaseline can trap when filtered out.
    for (const char* tier : {"sparkplug", "turbofan"}) {
      if ((interpreted || std::string(tier) != "sparkplug") &&
          (!needsTurbofan || std::string(tier) != "turbofan")) continue;
      std::istringstream words(flags_);
      for (std::string flag; words >> flag;) {
        const auto name = NormalizeFlagName(flag);
        if (name == "jitless" || name == "lite-mode" || name == std::string("no-") + tier ||
            name == std::string(std::string(tier) == "turbofan" ? "turbo" : tier) + "-filter")
          GTEST_SKIP() << "Required tier " << tier << " disabled or restricted by flag " << flag;
      }
    }
    const bool sparkplug = eval("%IsSparkplugEnabled()").getBool();
    if (!interpreted && !sparkplug)
      GTEST_SKIP() << "Required tier Sparkplug disabled by flags/build: " << flags_;
    if (interpreted && sparkplug)
      GTEST_SKIP() << "Required tier Ignition needs --no-sparkplug or --jitless: " << flags_;
    if (needsTurbofan && !eval("%IsTurbofanEnabled()").getBool())
      GTEST_SKIP() << "Required tier TurboFan disabled by flags/build: " << flags_;
    rt.global().setProperty(rt, "depth", depth);
    rt.global().setProperty(rt, "interpreted", interpreted);
    // Status bits from V8 runtime.h. Pin before entry, verify each live handler
    // before throwing; no warmup exceptions, optimizing tier-up or deopt fallback.
    eval(R"JS(
      var deltas = [], tiers = [], caught = 0, finished = 0, finalized = 0;
      var required = interpreted ? (1 << 15) : (1 << 16);
      function tier(f, expected = required) {
        const status = %GetOptimizationStatus(f);
        tiers.push([status, expected]);
        return (status & expected) !== 0;
      }
      function pin(f) {
        %NeverOptimizeFunction(f);
        if (!interpreted) %CompileBaseline(f);
      }
      function rec(n) {
        if (n === 0) {
          if (!tier(rec)) return;
          throw 42;
        }
        return rec(n - 1) + 1;
      }
      pin(rec);
    )JS");
    eval(body);
    if (async) {
      // A bounded drain, not a timer or an unbounded promise polling loop.
      for (int i = 0; i < 4; ++i) rt.drainMicrotasks();
    }
    auto tiers = eval("tiers").getObject(rt).getArray(rt);
    ASSERT_GT(tiers.size(rt), 0u);
    for (size_t i = 0; i < tiers.size(rt); ++i) {
      auto check = tiers.getValueAtIndex(rt, i).getObject(rt).getArray(rt);
      const int status = static_cast<int>(check.getValueAtIndex(rt, 0).getNumber());
      const int expected = static_cast<int>(check.getValueAtIndex(rt, 1).getNumber());
      ASSERT_NE(status & expected, 0)
          << "Wrong live frame tier, status=" << status;
    }
    ASSERT_EQ(eval("finished").getNumber(), 1) << "All JS frames must return normally";
    ASSERT_EQ(eval("caught").getNumber(), expectedCatches);
    auto deltas = eval("deltas").getObject(rt).getArray(rt);
    ASSERT_EQ(deltas.size(rt), static_cast<size_t>(expectedCatches));
    for (size_t i = 0; i < deltas.size(rt); ++i) {
      const double entries = deltas.getValueAtIndex(rt, i).getNumber();
      std::cout << "SSP depth=" << depth << " catch=" << i << " entries=" << entries << '\n';
      EXPECT_EQ(entries, 0) << "Live catch shadow-stack difference (entries), depth=" << depth;
    }
  }
  std::string flags_;
};

const char* kCatch = R"JS(
  function catcher() {
    if (!tier(catcher)) return;
    const before = __ssp();
    try { rec(depth); }
    catch (e) { deltas.push((before - __ssp()) / 8); caught += e === 42; }
    return 17;
  }
  pin(catcher);
  finished = catcher() === 17 ? 1 : 0;
)JS";

// Each depth gets its own test/runtime so a leak cannot contaminate another read.
TEST_P(CetShadowStack, CompiledThrowDepth0) { run(kCatch); }
TEST_P(CetShadowStack, CompiledThrowDepth2) { run(kCatch, false, 2); }
TEST_P(CetShadowStack, CompiledThrowDepth300) { run(kCatch, false, 300); }
TEST_P(CetShadowStack, CompiledThrowDepth1000) { run(kCatch, false, 1000); }
// rec(n) contributes n+1 calls, plus the runtime exit frame: n+2 entries.
TEST_P(CetShadowStack, CompiledUnwind255Entries) { run(kCatch, false, 253); }
TEST_P(CetShadowStack, CompiledUnwind256Entries) { run(kCatch, false, 254); }

TEST_P(CetShadowStack, CompiledNestedCatch) {
  run(R"JS(
    function inner() {
      if (!tier(inner)) return;
      const before = __ssp();
      try { rec(2); }
      catch (e) { deltas.push((before - __ssp()) / 8); caught += e === 42; }
      return 19;
    }
    function outer() {
      if (!tier(outer)) return;
      const before = __ssp();
      try { if (inner() !== 19) return; rec(2); }
      catch (e) { deltas.push((before - __ssp()) / 8); caught += e === 42; }
      return 17;
    }
    pin(inner); pin(outer);
    finished = outer() === 17 ? 1 : 0;
  )JS", false, 2, 2);
}

TEST_P(CetShadowStack, CompiledFinally) {
  run(R"JS(
    function catcher() {
      if (!tier(catcher)) return;
      const before = __ssp();
      try {
        try { rec(2); }
        finally { deltas.push((before - __ssp()) / 8); ++finalized; }
      } catch (e) { deltas.push((before - __ssp()) / 8); caught += e === 42; }
      return finalized === 1 ? 17 : 0;
    }
    pin(catcher);
    finished = catcher() === 17 ? 1 : 0;
    // Include the finally handler as an oracle observation, not an extra throw.
    caught += finalized;
  )JS", false, 2, 2);
}

TEST_P(CetShadowStack, CompiledRethrow) {
  run(R"JS(
    function inner() {
      if (!tier(inner)) return;
      const before = __ssp();
      try { rec(2); }
      catch (e) { deltas.push((before - __ssp()) / 8); ++caught; throw e; }
    }
    function outer() {
      if (!tier(outer)) return;
      const before = __ssp();
      try { inner(); }
      catch (e) { deltas.push((before - __ssp()) / 8); caught += e === 42; }
      return 17;
    }
    pin(inner); pin(outer);
    finished = outer() === 17 ? 1 : 0;
  )JS", false, 2, 2);
}

// Interpreted-capable handlers stay disabled until interpreter unwinding is fixed.
// Forced-interpreter runs use Ignition; no compiled test run enables these.
TEST_P(CetShadowStack, DISABLED_OracleInterpretedDepth0) { run(kCatch, true); }
TEST_P(CetShadowStack, DISABLED_OracleInterpretedDepth2) { run(kCatch, true, 2); }
TEST_P(CetShadowStack, DISABLED_OracleInterpretedDepth300) { run(kCatch, true, 300); }
TEST_P(CetShadowStack, DISABLED_OracleInterpretedDepth1000) { run(kCatch, true, 1000); }

TEST_P(CetShadowStack, DISABLED_GeneratorCatch) {
  run(R"JS(
    function* catcher() {
      yield 1;
      if (!tier(catcher)) return;
      const before = __ssp();
      try { rec(2); }
      catch (e) { deltas.push((before - __ssp()) / 8); caught += e === 42; }
      return 17;
    }
    pin(catcher);
    const g = catcher();
    const first = g.next(), last = g.next();
    finished = first.value === 1 && last.done && last.value === 17 ? 1 : 0;
  )JS", true, 2);
}

TEST_P(CetShadowStack, DISABLED_AsyncCatch) {
  run(R"JS(
    async function catcher() {
      await 1;
      if (!tier(catcher)) return;
      const before = __ssp();
      try { rec(2); }
      catch (e) { deltas.push((before - __ssp()) / 8); caught += e === 42; }
      return 17;
    }
    pin(catcher);
    catcher().then(value => { finished = value === 17 ? 1 : 0; });
  )JS", true, 2, 1, true);
}

// These require --no-sparkplug with TurboFan enabled; they are not matrix rows.
TEST_P(CetShadowStack, DISABLED_MaterializedCatch) {
  run(R"JS(
    function catcher(go) {
      if (!go) return 17;
      if (!tier(catcher, 1 << 11)) return;
      %DeoptimizeFunction(catcher);
      if (!tier(catcher)) return;
      const before = __ssp();
      try { rec(2); }
      catch (e) { deltas.push((before - __ssp()) / 8); caught += e === 42; }
      return 17;
    }
    %PrepareFunctionForOptimization(catcher);
    catcher(false); catcher(false);
    %OptimizeFunctionOnNextCall(catcher);
    catcher(false);
    finished = catcher(true) === 17 ? 1 : 0;
  )JS", true, 2, 1, false, true);
}

TEST_P(CetShadowStack, DISABLED_MixedCatch) {
  run(R"JS(
    function inner(go) {
      if (!go) return 17;
      if (!tier(inner)) return;
      const before = __ssp();
      try { rec(2); }
      catch (e) { deltas.push((before - __ssp()) / 8); ++caught; throw e; }
    }
    function outer(go) {
      if (!go) return inner(go);
      if (!tier(outer, 1 << 11)) return;
      const before = __ssp();
      try { inner(go); }
      catch (e) { deltas.push((before - __ssp()) / 8); caught += e === 42; }
      return 17;
    }
    pin(inner);
    %PrepareFunctionForOptimization(outer);
    outer(false); outer(false);
    %OptimizeFunctionOnNextCall(outer);
    outer(false);
    finished = outer(true) === 17 ? 1 : 0;
  )JS", true, 2, 2, false, true);
}

INSTANTIATE_TEST_SUITE_P(Runtimes, CetShadowStack,
                         ::testing::ValuesIn(runtimeGenerators()));
} // namespace
