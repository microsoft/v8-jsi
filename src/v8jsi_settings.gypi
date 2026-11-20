# Copyright (c) Microsoft Corporation.
# Licensed under the MIT license.
#
# v8jsi_settings.gypi -- shared MSVC settings for the v8jsi targets.
#
# This file is the GYP forward-port of PR #233 (commits c3d219a9d1c and
# a1b7255b342) onto the Node.js-sources/GYP build pipeline. PR #233 lands two
# related hardening blocks: the Hybrid CRT swap and the BinSkim
# compliance flags. Both live here so v8jsi.dll's "shipping
# substrate" -- CRT layout + compliance flags -- is set in one place.
#
# Hybrid CRT
# ----------
# Node.js's deps/nodejs/common.gypi defaults `MSVC_runtimeType` to /MT[d]
# (`RuntimeLibrary = 0/1`) whenever `node_shared != "true"`, which is the
# case for our `vcbuild.bat ... v8jsi without-intl` invocation. So the App
# CRT (vcruntime, libcpmt) is already statically linked into v8jsi.dll by
# default -- `dumpbin /DEPENDENTS v8jsi.dll` shows no vcruntime140.dll /
# msvcp140.dll before the UCRT swap.
#
# What `/MT[d]` also pulls in by default is the **static UCRT**
# (libucrt.lib / libucrtd.lib). The Hybrid CRT model swaps that static UCRT
# for the dynamic system UCRT (ucrtbase.dll), which has been an OS
# component since Windows 10 and is CRT-version-agnostic. The net effect:
# v8jsi.dll keeps its private static App CRT (so STL/allocator state never
# crosses the DLL boundary) but binds to the same shared ucrtbase.dll that
# every other binary on the box uses (so Debug-CRT consumers and a Release
# Hybrid-CRT v8jsi.dll agree on UCRT-managed state).
#
# The swap is two linker flags per config:
#   Release: /NODEFAULTLIB:libucrt.lib  + /DEFAULTLIB:ucrt.lib
#   Debug:   /NODEFAULTLIB:libucrtd.lib + /DEFAULTLIB:ucrtd.lib
# In GYP terms those are `IgnoreSpecificDefaultLibraries` +
# `AdditionalDependencies` on VCLinkerTool. (GYP wants the MSVS-side name
# `IgnoreDefaultLibraryNames`; using `IgnoreSpecificDefaultLibraries` makes
# GYP drop the setting silently.)
#
# BinSkim compliance
# ------------------
# v8jsi.dll must pass BinSkim with CFG +
# Spectre verified. PR #233's `:win_msvc_cfg` GN config gives the
# canonical flag set: `/guard:cf` (compile + link) and `/W3` (already
# common.gypi default). Its `/Qspectre` is not used: clang-cl ignores it
# (warning "argument unused during compilation") and inserts no Spectre
# barriers. Spectre mitigation (BA2024) comes from the Spectre-mitigated
# runtime libraries linked below, as in Hermes.
#
# We extend that with two defensive additions for BinSkim coverage:
#   - v8-jsi: `/CETCOMPAT` (link) -- BA2025 EnableShadowStack, x64 only
#     (see the x64 block in target_defaults). The x64 engine is built with
#     V8's CET switch for deopt/unwind shadow-stack adjustments;
#     arm64/x86 stay non-CETCOMPAT.
#   - Pin `RandomizedBaseAddress` (/DYNAMICBASE) and
#     `DataExecutionPrevention` (/NXCOMPAT) explicitly. MSBuild v143 +
#     x64 defaults both to true; pinning means BinSkim sees the flags in
#     the .vcxproj regardless of toolchain defaults.
#
# `/HIGHENTROPYVA` is left at the v143 x64 default (on); add it to
# `AdditionalOptions` if BinSkim ever flags it missing.
# `BufferSecurityCheck` (/GS) is already set globally by Node.js's
# common.gypi at line 326; we pin it here for clarity.
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
# the test addons; those do not need shipping BinSkim settings.)
# node.gyp's target_defaults can still propagate these settings to test
# targets; node_lite explicitly overrides /CETCOMPAT as described below.
{
  'target_defaults': {
    'msvs_settings': {
      'VCCLCompilerTool': {
        # /guard:cf -- Control Flow Guard (BA2008).
        # /ZH:SHA_256 -- secure source hashing (BA2004). clang-cl defaults to
        #               MD5 in CodeView; this switches to SHA-256. (Vendored V8
        #               libs stay MD5 -- a sub-threshold BA2004 Note, not error.)
        'AdditionalOptions': [ '/guard:cf', '/ZH:SHA_256' ],
        # /GS -- buffer security cookies (BA2011). Already true via
        # common.gypi globally; pinned here for clarity.
        'BufferSecurityCheck': 'true',
      },
      'VCLinkerTool': {
        # /guard:cf -- Control Flow Guard link-side instrumentation.
        # v8-jsi: x64 /CETCOMPAT below requires V8's CET switch so deopt
        # and unwinding adjust the shadow stack. node_lite (ordinary test
        # host) and mkv8snapshot (ordinary-host blobs) override it with
        # /CETCOMPAT:NO in their target settings.
        'AdditionalOptions': [ '/guard:cf' ],
        # /DYNAMICBASE -- ASLR opt-in (BA2009). GYP takes the MSVS
        # enumeration index here: 2 is "true".
        'RandomizedBaseAddress': 2,
        # /NXCOMPAT -- DEP opt-in (BA2016).
        'DataExecutionPrevention': 2,
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
    # Link the Spectre-mitigated MSVC CRT (BA2024). BinSkim flags the static CRT
    # objects (libcmt/libcpmt/libvcruntime) -- not our clang-cl code -- as lacking
    # Spectre mitigation, and clang-cl does not auto-link the Spectre CRT the way
    # cl/link does. Prepending lib\spectre\<arch> makes the linker pick the
    # mitigated variants of the same-named CRT libs. Needs the VS
    # "C++ Spectre-mitigated libs" component (present on the CI pool).
    'conditions': [
      ['target_arch=="x64"', {
        'msvs_settings': {
          'VCLinkerTool': {
            'AdditionalOptions': [ '/CETCOMPAT' ],
            'AdditionalLibraryDirectories': [ '$(VCToolsInstallDir)lib\\spectre\\x64' ],
          },
        },
      }],
      ['target_arch=="ia32"', {
        'msvs_settings': {
          'VCLinkerTool': {
            'AdditionalLibraryDirectories': [ '$(VCToolsInstallDir)lib\\spectre\\x86' ],
          },
        },
      }],
      ['target_arch=="arm64"', {
        'msvs_settings': {
          'VCLinkerTool': {
            'AdditionalLibraryDirectories': [ '$(VCToolsInstallDir)lib\\spectre\\arm64' ],
          },
        },
      }],
    ],
  },
}
