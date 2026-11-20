// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "jsi/test/testlib.h"
#include "jsi_abi/v8_jsi_abi.h"
#include <cwchar>
#include <thread>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>
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

#if defined(_WIN32) && defined(_M_X64)
bool QueryShadowStackPolicy(PROCESS_MITIGATION_USER_SHADOW_STACK_POLICY& policy) {
  using QueryPolicy = BOOL(WINAPI*)(HANDLE, PROCESS_MITIGATION_POLICY, PVOID, SIZE_T);
  auto query = reinterpret_cast<QueryPolicy>(GetProcAddress(
      GetModuleHandleW(L"kernel32.dll"), "GetProcessMitigationPolicy"));
  return query && query(GetCurrentProcess(), ProcessUserShadowStackPolicy,
                        &policy, sizeof(policy));
}
#endif

class CetShadowStack : public JSITestBase {
 protected:
  void SetUp() override {
    const char* flags = std::getenv("V8JSI_TEST_V8_FLAGS");
    flags_ = flags ? flags : "";
  }

  void requireOracle() {
#if defined(_WIN32) && defined(_M_X64)
    PROCESS_MITIGATION_USER_SHADOW_STACK_POLICY policy{};
    if (!QueryShadowStackPolicy(policy) ||
        !policy.EnableUserShadowStack || !policy.EnableUserShadowStackStrictMode)
      GTEST_SKIP() << "Requires enabled CET shadow stacks in strict mode; compat repair can hide leaks";
#endif
    if (!ReadShadowStackPointer())
      GTEST_SKIP() << "Requires Windows x64 with active CET shadow stacks";
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

  // Declare every catch tier before executing any tier-forcing intrinsic.
  void run(const char* body, bool interpreted = false, int depth = 0,
           int expectedCatches = 1, bool async = false, bool needsTurbofan = false) {
    requireOracle();
    if (IsSkipped()) return;
    // Check restrictions before tier intrinsics: CompileBaseline can trap when filtered out.
    for (const char* tier : {"sparkplug", "turbofan"}) {
      if ((interpreted || std::string(tier) != "sparkplug") &&
          (!needsTurbofan || std::string(tier) != "turbofan")) continue;
      std::istringstream words(flags_);
      for (std::string flag; words >> flag;) {
        const auto name = NormalizeFlagName(flag);
        if (name == "jitless" || name == "lite-mode" || name == std::string("no-") + tier ||
            name == std::string(std::string(tier) == "turbofan" ? "turbo" : tier) + "-filter")
          GTEST_SKIP() << "Required tier " << tier << " disabled or restricted by flag " << flag
                       << (needsTurbofan ? "; genuine deopt/materialized catch requires an optimizer" : "");
      }
    }
    if (interpreted) {
      std::istringstream words(flags_);
      for (std::string flag; words >> flag;) {
        const auto name = NormalizeFlagName(flag);
        if (name == "always-sparkplug" || name == "no-baseline-batch-compilation" ||
            name == "jit-fuzzing")
          GTEST_SKIP() << "Required tier Ignition incompatible with forced baseline compilation by flag " << flag;
      }
    }
    const bool sparkplug = eval("%IsSparkplugEnabled()").getBool();
    if (!interpreted && !sparkplug)
      GTEST_SKIP() << "Required tier Sparkplug disabled by flags/build: " << flags_;
    if (interpreted && sparkplug && !needsTurbofan)
      GTEST_SKIP() << "Required tier Ignition needs --no-sparkplug or --jitless: " << flags_;
    if (needsTurbofan && !eval("%IsTurbofanEnabled()").getBool())
      GTEST_SKIP() << "Required tier TurboFan (genuine deopt/materialized catch) disabled by flags/build: " << flags_;
    rt.global().setProperty(rt, "depth", depth);
    rt.global().setProperty(rt, "interpreted", interpreted);
    // Status bits from V8 runtime.h. Pin before entry, verify each live handler
    // before throwing. Ordinary handlers are pinned; materialized/mixed cases
    // explicitly require TurboFan and verify live tiers across deoptimization.
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

  // Probe scenarios pin every ordinary handler to the row's unoptimized tier.
  // Optimizing scenarios separately declare TurboFan/Maglev before parsing JS.
  void scenario(const char* body, const char* optimizedTier = nullptr,
                bool async = false, const char* feature = nullptr) {
    requireOracle();
    if (IsSkipped()) return;
    bool focused = false;
    std::istringstream words(flags_);
    for (std::string flag; words >> flag;) {
      const auto name = NormalizeFlagName(flag);
      if (name == "jitless" || name == "no-sparkplug") focused = true;
      if (feature && (name == std::string("no-") + feature ||
                      (std::string(feature) == "maglev-osr" && name == "no-use-osr")))
        GTEST_SKIP() << "Required feature " << feature << " disabled by flag " << flag;
      if (optimizedTier &&
          (name == "jitless" || name == "lite-mode" ||
           name == std::string("no-") + optimizedTier ||
           name == std::string(std::string(optimizedTier) == "turbofan" ? "turbo" : optimizedTier) + "-filter"))
        GTEST_SKIP() << "Required tier " << optimizedTier << " disabled or restricted by flag " << flag;
    }
    if (optimizedTier && !eval(std::string(optimizedTier) == "maglev" ?
        "%IsMaglevEnabled()" : "%IsTurbofanEnabled()").getBool())
      GTEST_SKIP() << "Required tier " << optimizedTier << " disabled by flags/build: " << flags_;
    rt.global().setProperty(rt, "focused", focused);
    rt.global().setProperty(rt, "maglev", optimizedTier && std::string(optimizedTier) == "maglev");
    eval(R"JS(
      var finished = 0, checks = 0, observed = 0, minDelta = 0, maxDelta = 0;
      var statuses = [];
      var baseline = %IsSparkplugEnabled();
      var catchTier = baseline ? (1 << 16) : (1 << 15);
      var optimizedBit = maglev ? (1 << 4) : (1 << 5);
      var liveOptimizedBit = maglev ? (1 << 18) : (1 << 11);
      function check(actual, expected, what = 'result') {
        ++checks;
        if (actual !== expected) throw new Error(what + ': expected ' + expected + ', got ' + actual);
      }
      function tier(f, bit) {
        const status = %GetOptimizationStatus(f);
        statuses.push([status, bit]);
        check((status & bit) !== 0, true, 'required tier, status=' + status);
      }
      function pin(f) {
        %NeverOptimizeFunction(f);
        if (baseline) %CompileBaseline(f);
      }
      function observe(delta) {
        ++observed;
        minDelta = Math.min(minDelta, delta);
        maxDelta = Math.max(maxDelta, delta);
        check(delta, 0, 'SSP entries');
      }
      function optimize(f) {
        if (maglev) %OptimizeMaglevOnNextCall(f);
        else %OptimizeFunctionOnNextCall(f);
      }
    )JS");
    try {
      // Keep the measuring frame out of OSR/tier-up while its callees transition.
      const std::string script = std::string("function exercise() { tier(exercise, catchTier);\n") + body +
          "\n} pin(exercise); exercise();";
      eval(script.c_str());
    } catch (const JSIException& error) {
      FAIL() << error.what();
    }
    if (async) {
      // All jobs are local promises: bounded draining replaces probe timers.
      for (int i = 0; i < 8; ++i) rt.drainMicrotasks();
    }
    ASSERT_EQ(eval("finished").getNumber(), 1) << "Scenario and all continuations must return";
    ASSERT_GT(eval("checks").getNumber(), 0);
    ASSERT_GT(eval("statuses.length").getNumber(), 0);
    ASSERT_GT(eval("observed").getNumber(), 0);
    EXPECT_EQ(eval("minDelta").getNumber(), 0);
    EXPECT_EQ(eval("maxDelta").getNumber(), 0);
    std::cout << "Scenario observations=" << eval("observed").getNumber()
              << " min/max SSP entries=" << eval("minDelta").getNumber()
              << '/' << eval("maxDelta").getNumber()
              << " tiers=" << eval("JSON.stringify(statuses)").getString(rt).utf8(rt) << '\n';
  }
  void requireWasm(bool tiering = false) {
#ifndef V8JSI_TEST_WEBASSEMBLY
    GTEST_SKIP() << "Requires WebAssembly; lite/sandbox build excludes Wasm";
#endif
    bool natives = false;
    std::istringstream words(flags_);
    for (std::string flag; words >> flag;) {
      const auto name = NormalizeFlagName(flag);
      if (name == "jitless" || name == "lite-mode" || name == "wasm-jitless")
        GTEST_SKIP() << "Requires Wasm JIT; disabled by flag " << flag;
      if (name == "allow-natives-syntax") natives = true;
      if (name == "no-allow-natives-syntax") natives = false;
    }
    if (tiering && !natives) GTEST_SKIP() << "Wasm tier assertions require --allow-natives-syntax";
    ASSERT_TRUE(eval("typeof WebAssembly === 'object'").getBool());
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

const char* kNestedCatch = R"JS(
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
  )JS";
TEST_P(CetShadowStack, CompiledNestedCatch) { run(kNestedCatch, false, 2, 2); }
TEST_P(CetShadowStack, InterpretedNestedCatch) { run(kNestedCatch, true, 2, 2); }

const char* kFinally = R"JS(
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
  )JS";
TEST_P(CetShadowStack, CompiledFinally) { run(kFinally, false, 2, 2); }
TEST_P(CetShadowStack, InterpretedFinally) { run(kFinally, true, 2, 2); }

const char* kRethrow = R"JS(
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
  )JS";
TEST_P(CetShadowStack, CompiledRethrow) { run(kRethrow, false, 2, 2); }
TEST_P(CetShadowStack, InterpretedRethrow) { run(kRethrow, true, 2, 2); }

// Ignition-only oracles; the Compiled* variants above require Sparkplug.
TEST_P(CetShadowStack, OracleInterpretedDepth0) { run(kCatch, true); }
TEST_P(CetShadowStack, OracleInterpretedDepth2) { run(kCatch, true, 2); }
TEST_P(CetShadowStack, OracleInterpretedDepth300) { run(kCatch, true, 300); }
TEST_P(CetShadowStack, OracleInterpretedDepth1000) { run(kCatch, true, 1000); }

TEST_P(CetShadowStack, GeneratorCatch) {
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

TEST_P(CetShadowStack, AsyncCatch) {
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

// Deopt materializes Ignition frames even when Sparkplug is enabled.
// MaterializedCatch/MixedCatch require genuine TurboFan deopt, not just
// interpreted execution; optimizer-disabled rows skip before optimization.
TEST_P(CetShadowStack, MaterializedCatch) {
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

TEST_P(CetShadowStack, MixedCatch) {
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

TEST_P(CetShadowStack, ArithmeticReturns) {
  scenario(R"JS(
    function sum() {
      tier(sum, catchTier);
      let s = 0;
      for (let i = 0; i < 1000; ++i) s += i;
      return s;
    }
    pin(sum);
    const before = __ssp();
    check(sum(), 499500);
    observe((before - __ssp()) / 8);
    finished = 1;
  )JS");
}

TEST_P(CetShadowStack, AlternatingExceptions) {
  scenario(R"JS(
    function thrower(i) {
      if (i % 2) throw new Error('odd ' + i);
      return i;
    }
    function catcher(n) {
      tier(catcher, catchTier);
      let caught = 0, sum = 0;
      const before = __ssp();
      for (let i = 0; i < n; ++i) {
        try { sum += thrower(i); }
        catch (e) {
          observe((before - __ssp()) / 8);
          check(e.message, 'odd ' + i);
          ++caught;
        }
      }
      check(sum, 24995000);
      return caught;
    }
    pin(thrower); pin(catcher);
    let total = 0;
    for (let round = 0; round < 20; ++round) total += catcher(10000);
    check(total, 100000);
    finished = 1;
  )JS");
}

TEST_P(CetShadowStack, ThrowLoop300k) {
  scenario(R"JS(
    function thrower(i) { throw i; }
    function middle(i) { return thrower(i) + 1; }
    function loop() {
      tier(loop, catchTier);
      let caught = 0, selfCaught = 0;
      const before = __ssp();
      for (let i = 0; i < 300000; ++i) {
        try { middle(i); }
        catch (e) { observe((before - __ssp()) / 8); check(e, i); ++caught; }
        try { throw i; }
        catch (e) { observe((before - __ssp()) / 8); check(e, i); ++selfCaught; }
      }
      check(caught, 300000);
      check(selfCaught, 300000);
      return 17;
    }
    pin(thrower); pin(middle); pin(loop);
    check(loop(), 17);
    finished = 1;
  )JS");
}

// Two depths exercise the probe's chunked unwinds; focused rows use a small repeat count.
const char* kDeepThrowLoop = R"JS(
  function rec(n) {
    if (n === 0) throw 42;
    return rec(n - 1) + 1;
  }
  function loop() {
    tier(loop, catchTier);
    const before = __ssp(), iterations = focused ? 3 : 3000;
    let caught = 0;
    for (let i = 0; i < iterations; ++i) {
      try { rec(depth); }
      catch (e) { observe((before - __ssp()) / 8); check(e, 42); ++caught; }
    }
    check(caught, iterations);
    return 17;
  }
  pin(rec); pin(loop);
  check(loop(), 17);
  finished = 1;
)JS";
TEST_P(CetShadowStack, DeepThrowLoop200) {
  rt.global().setProperty(rt, "depth", 200);
  scenario(kDeepThrowLoop);
}
TEST_P(CetShadowStack, DeepThrowLoop1000) {
  rt.global().setProperty(rt, "depth", 1000);
  scenario(kDeepThrowLoop);
}

TEST_P(CetShadowStack, StackOverflowIsRangeError) {
  scenario(R"JS(
    function rec(n) { return rec(n + 1) + 1; }
    function catcher() {
      tier(catcher, catchTier);
      let caught = 0;
      const before = __ssp();
      for (let i = 0; i < 5; ++i) {
        try { rec(0); }
        catch (e) {
          observe((before - __ssp()) / 8);
          check(e instanceof RangeError, true);
          ++caught;
        }
      }
      return caught;
    }
    pin(rec); pin(catcher);
    check(catcher(), 5);
    check(6 * 7, 42, 'process survives overflow');
    finished = 1;
  )JS");
}

TEST_P(CetShadowStack, AsyncRejectionsAndReturns) {
  scenario(R"JS(
    async function step(i) {
      await null;
      if (i % 3 === 0) throw new Error('x');
      return i;
    }
    async function run() {
      let sum = 0, caught = 0;
      for (let i = 0; i < 3000; ++i) {
        try { sum += await step(i); }
        catch (e) { check(e.message, 'x'); --sum; ++caught; }
        // Observe an explicit live catch after resumption, at equal native depth.
        if (i === 0) tier(run, catchTier);
        const before = __ssp();
        try { throw 42; }
        catch (e) { observe((before - __ssp()) / 8); check(e, 42); }
      }
      check(caught, 1000);
      return sum;
    }
    pin(step); pin(run);
    run().then(sum => { check(sum, 2999000); finished = 1; });
  )JS", nullptr, true);
}

// Shared bodies exercise identical transitions in TurboFan and Maglev.
const char* kEagerDeopt = R"JS(
  function getX(o) { return o.x + 1; }
  function sum(a) {
    let s = 0;
    for (let i = 0; i < a.length; ++i) s += a[i];
    return s;
  }
  const ints = Array.from({length: 64}, (_, i) => i);
  const doubles = ints.map(x => x + 0.5);
  let total = 0;
  for (let round = 0; round < 50; ++round) {
    %DeoptimizeFunction(getX); %ClearFunctionFeedback(getX);
    %PrepareFunctionForOptimization(getX);
    getX({x: 1}); getX({x: 2});
    optimize(getX);
    check(getX({x: 3}), 4);
    tier(getX, optimizedBit);
    const before = __ssp();
    total += getX({y: 1, x: 4});
    observe((before - __ssp()) / 8);
    check(%GetOptimizationStatus(getX) & optimizedBit, 0, 'eager map deopt');
    %DeoptimizeFunction(sum); %ClearFunctionFeedback(sum);
    %PrepareFunctionForOptimization(sum);
    sum(ints); sum(ints);
    optimize(sum);
    check(sum(ints), 2016);
    tier(sum, optimizedBit);
    total += sum(doubles);
    observe((before - __ssp()) / 8);
    check(%GetOptimizationStatus(sum) & optimizedBit, 0, 'elements-kind deopt');
  }
  check(total, 50 * (5 + 2048));
  finished = 1;
)JS";
// Under strict CET this also proves the startup flag stays on: map/elements-type
// changes force deopt continuations that cannot return safely with the flag off.
TEST_P(CetShadowStack, TurboFanEagerDeopt) { scenario(kEagerDeopt, "turbofan"); }
TEST_P(CetShadowStack, MaglevEagerDeopt) { scenario(kEagerDeopt, "maglev"); }

const char* kLazyDeopt = R"JS(
  function invalidate(go) {
    if (go) { tier(g, liveOptimizedBit); %DeoptimizeFunction(g); }
    return go ? 2 : 1;
  }
  function g(cb, go) { return cb(go) * 3; }
  pin(invalidate);
  let total = 0;
  for (let round = 0; round < 50; ++round) {
    %PrepareFunctionForOptimization(g);
    g(invalidate, false); g(invalidate, false);
    optimize(g);
    check(g(invalidate, false), 3);
    tier(g, optimizedBit);
    const before = __ssp();
    total += g(invalidate, true);
    observe((before - __ssp()) / 8);
    check(%GetOptimizationStatus(g) & optimizedBit, 0, 'lazy deopt');
  }
  check(total, 300);
  finished = 1;
)JS";
TEST_P(CetShadowStack, TurboFanLazyDeopt) { scenario(kLazyDeopt, "turbofan"); }
TEST_P(CetShadowStack, MaglevLazyDeopt) { scenario(kLazyDeopt, "maglev"); }

TEST_P(CetShadowStack, PrototypeInvalidationDeoptsCaller) {
  scenario(R"JS(
    let total = 0;
    for (let round = 0; round < 10; ++round) {
      class P { get() { return 1; } }
      function mutate(go) {
        if (go) {
          tier(g, liveOptimizedBit);
          P.prototype.get = function() { return 100; };
        }
      }
      function g(p, go) {
        const a = p.get();
        mutate(go);
        return a + p.get();
      }
      pin(mutate);
      // Closures share feedback across rounds; retain a fresh prototype dependency.
      %ClearFunctionFeedback(g);
      %PrepareFunctionForOptimization(g);
      const p = new P();
      g(p, false); g(p, false);
      optimize(g);
      check(g(p, false), 2);
      tier(g, optimizedBit);
      const before = __ssp();
      total += g(p, true);
      observe((before - __ssp()) / 8);
      check(%GetOptimizationStatus(g) & optimizedBit, 0, 'prototype lazy deopt');
    }
    check(total, 1010);
    finished = 1;
  )JS", "turbofan");
}

TEST_P(CetShadowStack, TurboFanInlinedDeopt) {
  scenario(R"JS(
    let inlined = 0, total = 0;
    function inner(o) {
      const optimized = (%GetOptimizationStatus(outer) & liveOptimizedBit) !== 0;
      // Exercise the instrumentation during warmup too, avoiding a cold-path deopt.
      // A physical inner frame sets kIsExecuting; its absence proves inlining.
      check((%GetOptimizationStatus(inner) & (1 << 10)) === 0, optimized, 'inlined inner');
      inlined += optimized ? 1 : 0;
      return o.v + 1;
    }
    function outer(o) { return inner(o) * 2; }
    for (let round = 0; round < 50; ++round) {
      %DeoptimizeFunction(outer);
      // Reset map feedback, not outer's call count: preserve its inlining frequency.
      %ClearFunctionFeedback(inner);
      %PrepareFunctionForOptimization(inner);
      %PrepareFunctionForOptimization(outer);
      outer({v: 1}); outer({v: 2});
      optimize(outer);
      check(outer({v: 3}), 8);
      tier(outer, optimizedBit);
      const before = __ssp();
      total += outer({w: 0, v: 4});
      observe((before - __ssp()) / 8);
      check(%GetOptimizationStatus(outer) & optimizedBit, 0, 'inlined eager deopt');
    }
    // Both optimized calls reach inner before the second call's map bailout.
    check(inlined, 100);
    check(total, 500);
    finished = 1;
  )JS", "turbofan", false, "turbo-inlining");
}

// Genuine TurboFan deopt/inlining is required, so optimizer-disabled rows skip.
TEST_P(CetShadowStack, InlinedMaterializedCatch) {
  scenario(R"JS(
    let caught = 0;
    function invalidate(go) {
      if (go) {
        tier(outer, liveOptimizedBit);
        check(%GetOptimizationStatus(inner) & (1 << 10), 0, 'inlined catching frame');
        %DeoptimizeFunction(outer);
      }
    }
    function rec(n) { if (n === 0) throw 42; return rec(n - 1) + 1; }
    function inner(go) {
      invalidate(go);
      if (!go) return 17;
      tier(inner, 1 << 15);
      const before = __ssp();
      try { rec(2); }
      catch (e) { observe((before - __ssp()) / 8); check(e, 42); ++caught; }
      return 17;
    }
    function outer(go) { return inner(go) + 1; }
    pin(invalidate); pin(rec);
    %PrepareFunctionForOptimization(inner);
    %PrepareFunctionForOptimization(outer);
    outer(false); outer(false);
    optimize(outer);
    check(outer(false), 18);
    tier(outer, optimizedBit);
    check(outer(true), 18);
    check(caught, 1);
    finished = 1;
  )JS", "turbofan", false, "turbo-inlining");
}

// Genuine TurboFan deopt must materialize both frames; optimizer-disabled rows skip.
TEST_P(CetShadowStack, NonTopmostMaterializedCatch) {
  scenario(R"JS(
    let caught = 0;
    function invalidate(go) {
      if (go) {
        tier(outer, liveOptimizedBit);
        check(%GetOptimizationStatus(inner) & (1 << 10), 0, 'inlined callee');
        %DeoptimizeFunction(outer);
      }
    }
    function inner(go) {
      invalidate(go);
      if (!go) return 17;
      // Both materialized frames are still on stack when the callee throws.
      tier(inner, 1 << 15);
      tier(outer, 1 << 15);
      throw 42;
    }
    function outer(go) {
      const before = __ssp();
      try { return inner(go) + 1; }
      catch (e) {
        tier(outer, 1 << 15);
        observe((before - __ssp()) / 8);
        check(e, 42);
        ++caught;
        return 18;
      }
    }
    pin(invalidate);
    %PrepareFunctionForOptimization(inner);
    %PrepareFunctionForOptimization(outer);
    outer(false); outer(false);
    optimize(outer);
    check(outer(false), 18);
    tier(outer, optimizedBit);
    check(outer(true), 18);
    check(caught, 1);
    finished = 1;
  )JS", "turbofan", false, "turbo-inlining");
}

const char* kOsr = R"JS(
  function loop(n, go, expected) {
    let sum = 0;
    for (let i = 0; i < n; ++i) {
      sum += i;
      if (go && (i === 10 || (!maglev && i === 30))) %OptimizeOsr();
      if (i === 80) tier(loop, expected);
    }
    return sum;
  }
  for (let round = 0; round < 20; ++round) {
    %DeoptimizeFunction(loop);
    // Discard cached OSR code/urgency before asserting the next baseline entry.
    %ClearFunctionFeedback(loop);
    %PrepareFunctionForOptimization(loop);
    if (baseline) %CompileBaseline(loop);
    check(loop(100, false, catchTier), 4950);
    const before = __ssp();
    check(loop(100, true, liveOptimizedBit), 4950);
    observe((before - __ssp()) / 8);
  }
  finished = 1;
)JS";
TEST_P(CetShadowStack, TurboFanOnStackReplacement) {
  scenario(kOsr, "turbofan", false, "use-osr");
}
TEST_P(CetShadowStack, MaglevOnStackReplacement) {
  scenario(kOsr, "maglev", false, "maglev-osr");
}

TEST_P(CetShadowStack, TurboFanThrowAndReturn) {
  scenario(R"JS(
    function thrower(x) { if (x > 0) throw new Error('x'); return x; }
    function catcher(x) {
      try { return thrower(x); }
      catch (e) { check(e.message, 'x'); return -1; }
    }
    for (let round = 0; round < 50; ++round) {
      %DeoptimizeFunction(catcher);
      %PrepareFunctionForOptimization(thrower);
      %PrepareFunctionForOptimization(catcher);
      check(catcher(0), 0); check(catcher(1), -1);
      optimize(catcher);
      check(catcher(1), -1);
      tier(catcher, optimizedBit);
      const before = __ssp();
      check(catcher(1), -1); check(catcher(0), 0);
      observe((before - __ssp()) / 8);
    }
    finished = 1;
  )JS", "turbofan");
}

#if defined(_WIN32) && defined(_M_X64) && defined(V8JSI_TEST_CET_STARTUP_CHECK)
// Unlike death-test children, these children get an explicit process policy.
TEST(CetShadowStack, InitOnly) {
  auto runtime = runtimeGenerators()[1](); // One runtime, no JavaScript (safe even with CET opted out).
}

// Only the normal Windows x64 engine has the process-dependent startup default.
void CheckStartupFlags(const wchar_t* mode, const wchar_t* flags, const char* expected,
                       const wchar_t* test = L"InitOnly",
                       uint32_t* snapshotTag = nullptr, bool snapshotFirst = false) {
  wchar_t exe[32768];
  const DWORD length = GetModuleFileNameW(nullptr, exe, 32768);
  ASSERT_GT(length, 0u);
  ASSERT_LT(length, 32768u);
  const std::wstring target(exe);
  const auto launcher = target.substr(0, target.find_last_of(L"\\") + 1) + L"cetrun.exe";
  std::wstring command = L"\"" + launcher + L"\" " + mode + L" \"" + target +
      L"\" " + (snapshotFirst ? L"--cet-snapshot-first " : L"") +
      L"--gtest_color=no --gtest_filter=CetShadowStack." + test;
  // Build a child-only environment; never race V8's worker threads by mutating ours.
  const auto inherited = GetEnvironmentStringsW();
  ASSERT_NE(inherited, nullptr);
  std::vector<std::wstring> entries;
  for (const wchar_t* entry = inherited; *entry; entry += wcslen(entry) + 1) {
    if (_wcsnicmp(entry, L"V8JSI_TEST_V8_FLAGS=", 20) &&
        _wcsnicmp(entry, L"V8JSI_TEST_CET_CHILD=", 21) &&
        _wcsnicmp(entry, L"CETRUN_TIMEOUT_MS=", 18)) entries.emplace_back(entry);
  }
  FreeEnvironmentStringsW(inherited);
  // Diagnostic flags also enter V8's hash; omit them for tag comparisons.
  entries.emplace_back(std::wstring(L"V8JSI_TEST_V8_FLAGS=") +
      (snapshotTag ? L"" : L"--print-flag-values ") + flags);
  entries.emplace_back(L"V8JSI_TEST_CET_CHILD=1");
  entries.emplace_back(L"CETRUN_TIMEOUT_MS=60000");
  std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
    return _wcsicmp(a.c_str(), b.c_str()) < 0;
  });
  std::vector<wchar_t> environment;
  for (const auto& entry : entries) {
    environment.insert(environment.end(), entry.begin(), entry.end());
    environment.push_back(0);
  }
  environment.push_back(0);
  SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
  HANDLE read{}, write{};
  ASSERT_TRUE(CreatePipe(&read, &write, &security, 0));
  std::unique_ptr<void, decltype(&CloseHandle)> readOwner(read, CloseHandle), writeOwner(write, CloseHandle);
  ASSERT_TRUE(SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0));
  STARTUPINFOW startup{sizeof(startup)};
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  startup.hStdOutput = startup.hStdError = write;
  PROCESS_INFORMATION child{};
  ASSERT_TRUE(CreateProcessW(launcher.c_str(), command.data(), nullptr, nullptr, TRUE,
                            CREATE_UNICODE_ENVIRONMENT, environment.data(), nullptr, &startup, &child))
      << GetLastError();
  CloseHandle(child.hThread);
  std::unique_ptr<void, decltype(&CloseHandle)> process(child.hProcess, CloseHandle);
  writeOwner.reset();
  std::string output;
  std::thread reader([&] {
    char buffer[4096];
    DWORD bytes;
    while (ReadFile(read, buffer, sizeof(buffer), &bytes, nullptr) && bytes)
      output.append(buffer, bytes);
  });
  const DWORD wait = WaitForSingleObject(child.hProcess, 90000);
  if (wait != WAIT_OBJECT_0) {
    TerminateProcess(child.hProcess, 1); // Closing cetrun also kills its job's child.
    WaitForSingleObject(child.hProcess, INFINITE);
  }
  reader.join();
  ASSERT_EQ(wait, WAIT_OBJECT_0) << output;
  DWORD exit{};
  ASSERT_TRUE(GetExitCodeProcess(child.hProcess, &exit));
  ASSERT_EQ(exit, 0u) << output;
  if (output.find("SKIP: user CET not available") != std::string::npos)
    GTEST_SKIP() << "user CET not available; strict process cannot be tested";
  ASSERT_NE(output.find("[  PASSED  ] 1 test."), std::string::npos) << output;
  std::istringstream lines(output);
  int cetLines = 0;
  for (std::string line; std::getline(lines, line);) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line != "--cet-compatible" && line != "--no-cet-compatible") continue;
    ++cetLines;
    EXPECT_EQ(line, expected);
  }
  EXPECT_EQ(cetLines, snapshotTag ? 0 : 1) << output;
  if (snapshotTag) {
    const auto tag = output.find("snapshot-version=");
    ASSERT_NE(tag, std::string::npos) << output;
    *snapshotTag = static_cast<uint32_t>(std::stoul(output.substr(tag + 17)));
  }
  std::wcout << L"Startup mode=" << mode << L" host=" << flags << L" child=" << test;
  std::cout << " => " << expected << '\n';
}

TEST(CetStartupFlags, OffUsesCurrentCodeGeneration) {
  CheckStartupFlags(L"off", L"", "--no-cet-compatible");
}
TEST(CetStartupFlags, OffHonorsHostOptIn) {
  CheckStartupFlags(L"off", L"--cet-compatible", "--cet-compatible");
}
TEST(CetStartupFlags, StrictDefaultsToCompatibleCode) {
  CheckStartupFlags(L"strict", L"", "--cet-compatible");
}
TEST(CetStartupFlags, StrictHonorsHostOptOut) {
  CheckStartupFlags(L"strict", L"--no-cet-compatible", "--no-cet-compatible");
}
TEST(CetStartupFlags, RepeatedHostFlagsBeforeRuntimeWin) {
  CheckStartupFlags(L"off", L"", "--cet-compatible", L"RepeatedHostFlags");
}
// Separate processes avoid V8's immutable flags and shared read-only heap.
void CheckSnapshotFirstDefault(const wchar_t* mode, const wchar_t* flag, const char* expected) {
  uint32_t explicitTag = 0, defaultTag = 0;
  CheckStartupFlags(mode, flag, expected, L"SnapshotFirst", &explicitTag);
  if (testing::Test::IsSkipped() || testing::Test::HasFatalFailure()) return;
  CheckStartupFlags(mode, L"", expected, L"SnapshotFirst", &defaultTag, true);
  if (testing::Test::IsSkipped() || testing::Test::HasFatalFailure()) return;
  EXPECT_NE(defaultTag, 0u);
  EXPECT_EQ(defaultTag, explicitTag);
}
TEST(CetStartupFlags, SnapshotFirstOffMatchesExplicitOptOut) {
  CheckSnapshotFirstDefault(L"off", L"--no-cet-compatible", "--no-cet-compatible");
}
TEST(CetStartupFlags, SnapshotFirstStrictMatchesExplicitOptIn) {
  CheckSnapshotFirstDefault(L"strict", L"--cet-compatible", "--cet-compatible");
}
TEST(CetStartupFlags, SnapshotBeforeRuntimeKeepsHostOverride) {
  CheckStartupFlags(L"off", L"--cet-compatible", "--cet-compatible", L"SnapshotFirst");
}

TEST(CetShadowStack, RepeatedHostFlags) {
  if (!std::getenv("V8JSI_TEST_CET_CHILD"))
    GTEST_SKIP() << "Requires fresh explicitly launched startup child";
  char program[] = "v8jsi", on[] = "--cet-compatible", off[] = "--no-cet-compatible";
  // Alternate values, then a no-op call: defaults must not reset any earlier host call.
  for (char* flag : {on, off, on, static_cast<char*>(nullptr)}) {
    char* argv[] = {program, flag};
    size_t argc = flag ? 2 : 1;
    ASSERT_EQ(v8_jsi_abi_set_v8_flags(&argc, argv, false), jsi_status_ok);
  }
  auto runtime = runtimeGenerators()[1]();
}

TEST(CetShadowStack, SnapshotFirst) {
  if (!std::getenv("V8JSI_TEST_CET_CHILD"))
    GTEST_SKIP() << "Requires fresh explicitly launched startup child";
  uint8_t* blob = nullptr;
  size_t size = 0;
  ASSERT_EQ(v8_jsi_abi_snapshot_create("", 0, "empty.js", false, &blob, &size), jsi_status_ok);
  std::unique_ptr<uint8_t, decltype(&v8_jsi_abi_snapshot_destroy)> owner(blob, v8_jsi_abi_snapshot_destroy);
  ASSERT_GE(size, 24u);
  // CachedDataVersionTag in the container header, finalized after V8 init.
  uint32_t tag;
  std::memcpy(&tag, blob + 8, sizeof(tag));
  std::cout << "snapshot-version=" << tag << '\n';
  EXPECT_EQ(v8_jsi_abi_snapshot_check(blob, size), v8_jsi_abi_snapshot_compat_ok);
  // This checks initialization order, not custom-blob consumption (shared RO heap).
  auto runtime = runtimeGenerators()[1]();
  char program[] = "v8jsi", flag[] = "--cet-compatible";
  char* argv[] = {program, flag};
  size_t argc = 2;
  EXPECT_EQ(v8_jsi_abi_set_v8_flags(&argc, argv, false), jsi_status_invalid_arg);
}
#endif

// Hand-assembled: (import "m" "s" (func (result i32))), f() = s() + 1.
const char* kJspiModule = R"JS(
  var module = new WebAssembly.Module(new Uint8Array([
    0,97,115,109,1,0,0,0, 1,5,1,96,0,1,127,
    2,7,1,1,109,1,115,0,0, 3,2,1,0, 7,5,1,1,102,0,1,
    10,9,1,7,0,16,0,65,1,106,11
  ]));
)JS";

TEST_P(CetShadowStack, JspiFollowsProcessPolicy) {
  requireWasm();
  if (IsSkipped() || HasFatalFailure()) return;
#if defined(_WIN32) && defined(_M_X64)
  PROCESS_MITIGATION_USER_SHADOW_STACK_POLICY policy{};
  const bool shadowStacks = QueryShadowStackPolicy(policy) && policy.EnableUserShadowStack;
  EXPECT_EQ(ReadShadowStackPointer() != 0, shadowStacks);
#else
  const bool shadowStacks = false;
#endif
  // JSI owns one context per runtime. Keep the initial context alive while an
  // additional factory-created runtime/context exercises the same contract.
  auto additional = factory();
  for (Runtime* context : {&rt, additional.get()}) {
    auto evaluate = [context](const char* js) {
      return context->evaluateJavaScript(std::make_unique<StringBuffer>(js), "wasm-jspi.js");
    };
    for (const char* name : {"Suspending", "promising", "SuspendError"}) {
      SCOPED_TRACE(name);
      const std::string js = std::string("typeof WebAssembly.") + name;
      EXPECT_EQ(evaluate(js.c_str()).getString(*context).utf8(*context),
                shadowStacks ? "undefined" : "function");
    }
    if (HasFailure()) return; // Never attempt stack switching after a contract failure.
    evaluate(kJspiModule);
    if (shadowStacks) {
      // A missing wrapper is an invalid import; construction itself is also a
      // catchable TypeError, rather than a stack-switching process termination.
      EXPECT_TRUE(evaluate(R"JS(
        var errors = 0;
        try { new WebAssembly.Suspending(() => Promise.resolve(41)); }
        catch (e) { if (e instanceof TypeError) ++errors; else throw e; }
        try { new WebAssembly.Instance(module, {m: {s: WebAssembly.Suspending}}); }
        catch (e) { if (e instanceof WebAssembly.LinkError) ++errors; else throw e; }
        errors === 2
      )JS").getBool());
    } else {
      evaluate(R"JS(
        var calls = 0, resolved = 0, result = 0, rejected = 0, resume;
        var pending = new Promise(resolve => { resume = resolve; });
        var s = new WebAssembly.Suspending(() => { ++calls; return pending; });
        var instance = new WebAssembly.Instance(module, {m: {s}});
        var promise = WebAssembly.promising(instance.exports.f)();
        promise.then(value => { result = value; ++resolved; }, () => { ++rejected; });
      )JS");
      EXPECT_TRUE(evaluate("promise instanceof Promise && calls === 1 && resolved === 0").getBool());
      context->drainMicrotasks();
      EXPECT_EQ(evaluate("resolved").getNumber(), 0); // Still suspended on our unresolved promise.
      evaluate("resume(41)");
      for (int i = 0; i < 4; ++i) context->drainMicrotasks();
      EXPECT_TRUE(evaluate("calls === 1 && resolved === 1 && result === 42 && rejected === 0").getBool());
    }
  }
  // v8::Isolate::InstallConditionalFeatures is unreachable through JSI/ABI:
  // v8jsi2.dll does not export the V8 C++ API. That site is covered by the
  // identical process-policy guard, not by this test.
}

// (memory 1), add(i32,i32)->i32 and load(i32)->i32; no external fixture.
const char* kAddLoadModule = R"JS(
  var instance = new WebAssembly.Instance(new WebAssembly.Module(new Uint8Array([
    0,97,115,109,1,0,0,0,
    1,12,2,96,2,127,127,1,127,96,1,127,1,127,
    3,3,2,0,1, 5,3,1,0,1,
    7,14,2,3,97,100,100,0,0,4,108,111,97,100,0,1,
    10,17,2,7,0,32,0,32,1,106,11,7,0,32,0,40,2,0,11
  ])));
  var add = instance.exports.add, load = instance.exports.load;
)JS";

TEST_P(CetShadowStack, WasmAddLoadAndTrap) {
  requireWasm();
  if (IsSkipped() || HasFatalFailure()) return;
  eval(kAddLoadModule);
  EXPECT_EQ(eval("add(20, 22)").getNumber(), 42);
  EXPECT_EQ(eval("load(0)").getNumber(), 0);
  EXPECT_EQ(eval("load(65532)").getNumber(), 0);
  eval(R"JS(
    var sum = 0, caught = 0;
    for (let i = 0; i < 2000; ++i) {
      sum = add(sum, 1);
      try { load(70000); }
      catch (e) { if (e instanceof WebAssembly.RuntimeError) ++caught; else throw e; }
    }
  )JS");
  EXPECT_EQ(eval("sum").getNumber(), 2000);
  EXPECT_EQ(eval("caught").getNumber(), 2000);
  EXPECT_EQ(eval("add(load(4), 42)").getNumber(), 42);
}

TEST_P(CetShadowStack, WasmLiftoffToTurboFan) {
  requireWasm(true);
  if (IsSkipped() || HasFatalFailure()) return;
  eval(kAddLoadModule);
  EXPECT_EQ(eval("add(1, 2)").getNumber(), 3); // Compile lazy functions before checking tiers.
  EXPECT_EQ(eval("load(0)").getNumber(), 0);
  ASSERT_TRUE(eval("%IsLiftoffFunction(add) && %IsLiftoffFunction(load)").getBool());
  eval("%WasmTierUpFunction(add); %WasmTierUpFunction(load)");
  ASSERT_TRUE(eval("%IsTurboFanFunction(add) && %IsTurboFanFunction(load)").getBool());
  EXPECT_EQ(eval("var total = 0; for (let i = 0; i < 10000; ++i) total = add(total, 1); total").getNumber(), 10000);
  EXPECT_EQ(eval("load(65532)").getNumber(), 0);
  EXPECT_TRUE(eval(R"JS(
    var trapped = false;
    try { load(70000); } catch (e) { if (e instanceof WebAssembly.RuntimeError) trapped = true; else throw e; }
    trapped
  )JS").getBool());
}

INSTANTIATE_TEST_SUITE_P(Runtimes, CetShadowStack,
                         ::testing::ValuesIn(runtimeGenerators()));
} // namespace
