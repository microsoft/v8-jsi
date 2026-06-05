# Copyright (c) Microsoft Corporation.
# Licensed under the MIT license.
#
# v8jsi_settings.gypi -- shared MSVC settings for the v8jsi targets.
#
# This file is the GYP forward-port of PR #233 (commits c3d219a9d1c and
# a1b7255b342) onto the Node.js-sources/GYP build pipeline. PR #233 lands two
# related hardening blocks: the Hybrid CRT swap (Stage B1) and the BinSkim
# compliance flags (Stage B3). Both live here so v8jsi.dll's "shipping
# substrate" -- CRT layout + compliance flags -- is set in one place.
#
# Hybrid CRT (Stage B1)
# ---------------------
# Node.js's deps/nodejs/common.gypi defaults `MSVC_runtimeType` to /MT[d]
# (`RuntimeLibrary = 0/1`) whenever `node_shared != "true"`, which is the
# case for our `vcbuild.bat ... v8jsi without-intl` invocation. So the App
# CRT (vcruntime, libcpmt) is already statically linked into v8jsi.dll by
# default -- `dumpbin /DEPENDENTS v8jsi.dll` shows no vcruntime140.dll /
# msvcp140.dll on the pre-B1 baseline.
#
# What `/MT[d]` also pulls in by default is the **static UCRT**
# (libucrt.lib / libucrtd.lib). The Hybrid CRT model swaps that static UCRT
# for the dynamic system UCRT (ucrtbase.dll), which has been an OS
# component since Windows 10 and is CRT-version-agnostic. The net effect:
# v8jsi.dll keeps its private static App CRT (so STL/allocator state never
# crosses the DLL boundary) but binds to the same shared ucrtbase.dll that
# every other binary on the box uses (so Debug-CRT consumers and a Release
# Hybrid-CRT v8jsi.dll agree on UCRT-managed state -- the basis for B2's
# Debug-drop).
#
# The swap is two linker flags per config:
#   Release: /NODEFAULTLIB:libucrt.lib  + /DEFAULTLIB:ucrt.lib
#   Debug:   /NODEFAULTLIB:libucrtd.lib + /DEFAULTLIB:ucrtd.lib
# In GYP terms those are `IgnoreSpecificDefaultLibraries` +
# `AdditionalDependencies` on VCLinkerTool. (GYP wants the MSVS-side name
# `IgnoreDefaultLibraryNames`; using `IgnoreSpecificDefaultLibraries` makes
# GYP drop the setting silently.)
#
# BinSkim compliance (Stage B3)
# -----------------------------
# Phase-1 master-plan gate #5 requires v8jsi.dll to pass BinSkim with CFG +
# Spectre verified. PR #233's `:win_msvc_cfg` GN config gives the
# canonical flag set: `/guard:cf` (compile + link), `/Qspectre` (compile),
# `/W3` (already common.gypi default).
#
# We extend that with two defensive additions for BinSkim coverage:
#   - `/CETCOMPAT` (link) -- BA2025 EnableShadowStack. Arch-independent
#     metadata; the linker accepts the flag on arm64/x86 and the bit just
#     isn't honored at runtime there.
#   - Pin `RandomizedBaseAddress` (/DYNAMICBASE) and
#     `DataExecutionPrevention` (/NXCOMPAT) explicitly. MSBuild v143 +
#     x64 defaults both to true; pinning means BinSkim sees the flags in
#     the .vcxproj regardless of toolchain defaults.
#
# `/HIGHENTROPYVA` is left at the v143 x64 default (on); add it to
# `AdditionalOptions` if BinSkim ever flags it missing.
# `BufferSecurityCheck` (/GS) is already set globally by Node.js's
# common.gypi at line 326; we pin it here for clarity.
# `/Qspectre` automatically links Spectre-mitigated runtime libraries when
# the VS "Spectre Mitigations" component is installed (CI image has it),
# so we don't need a separate `-vcvars_spectre_libs=spectre` step.
#
# GYP key notes
# -------------
# GYP's MSVSSettings.py has first-class keys for `RandomizedBaseAddress`,
# `DataExecutionPrevention`, and `BufferSecurityCheck`. It does NOT have
# first-class keys for `ControlFlowGuard`, `SpectreMitigation`, `CETCompat`,
# or `HighEntropyVA` -- those flow through `AdditionalOptions` on the
# appropriate tool. PR #233's GN config uses raw `cflags` / `ldflags` for
# the same reason.
#
# Scope
# -----
# Included at the top of src/v8jsi.gyp via `'includes': ['v8jsi_settings.gypi']`,
# so `target_defaults` merges into every target defined in v8jsi.gyp -- the
# v8jsi DLL, the v8jsi_test EXE, and the no-op v8jsi_version_gen action
# target (which has no compilation, so the compile/link blocks are moot
# for it).
#
# Not included from src/node-api/test/node_api_tests.gyp. The N-API
# conformance addon DLLs and node_lite/node_api_tests EXEs are test-only
# binaries that don't ship; BinSkim compliance is a *shipping* gate, so
# applying it to test-only targets would expand blast radius for no
# acceptance-gate benefit. (PR #233 did extend the Hybrid CRT swap to
# the test addons; we diverge here intentionally to match B1's scope
# decision.)
{
  'target_defaults': {
    'msvs_settings': {
      'VCCLCompilerTool': {
        # /guard:cf -- Control Flow Guard (BA2008).
        # /Qspectre  -- Spectre v1 mitigation (BA2024). Auto-pulls
        #               Spectre-mitigated runtime libs when the VS
        #               "Spectre Mitigations" component is installed.
        'AdditionalOptions': [ '/guard:cf', '/Qspectre' ],
        # /GS -- buffer security cookies (BA2011). Already true via
        # common.gypi globally; pinned here for clarity.
        'BufferSecurityCheck': 'true',
      },
      'VCLinkerTool': {
        # /guard:cf -- Control Flow Guard link-side instrumentation.
        # NOTE: /CETCOMPAT was tried and reverted -- V8's heap-snapshot
        # serialization calls a virtual OutputStream::WriteAsciiChunk
        # through an indirect call that lands in code V8 doesn't mark as
        # shadow-stack-safe; CET enforcement triggers a FAST_FAIL
        # (0xC0000409 STATUS_STACK_BUFFER_OVERRUN) in
        # JSITestExt.V8Instrumentation_CreateHeapSnapshotToFile.
        # BA2025 (EnableShadowStack) accepted as a residual finding --
        # blocked by V8 upstream; see impl-b3-results.md.
        'AdditionalOptions': [ '/guard:cf' ],
        # /DYNAMICBASE -- ASLR opt-in (BA2009).
        'RandomizedBaseAddress': 'true',
        # /NXCOMPAT -- DEP opt-in (BA2016).
        'DataExecutionPrevention': 'true',
      },
    },
    'configurations': {
      'Release': {
        'msvs_settings': {
          'VCLinkerTool': {
            'AdditionalDependencies': [ 'ucrt.lib' ],
            # GYP wants the MSVS-side name `IgnoreDefaultLibraryNames`. The
            # MSBuild-side name `IgnoreSpecificDefaultLibraries` is silently
            # dropped by GYP's MSVSSettings.py emit pass -- linker then sees
            # both libucrt.lib and ucrt.lib and fails with duplicate symbols.
            'IgnoreDefaultLibraryNames': [ 'libucrt.lib' ],
          },
        },
      },
      'Debug': {
        'msvs_settings': {
          'VCLinkerTool': {
            'AdditionalDependencies': [ 'ucrtd.lib' ],
            'IgnoreDefaultLibraryNames': [ 'libucrtd.lib' ],
          },
        },
      },
    },
  },
}
