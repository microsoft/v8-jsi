# Copyright (c) Microsoft Corporation.
# Licensed under the MIT license.
#
# libnode_settings.gypi -- MSVC settings for the Windows libnode.dll build:
# Node.js built as a shared library (`vcbuild.bat dll`, `configure --shared`).
#
# deps/nodejs/common.gypi includes this file for Windows shared builds, so its
# `target_defaults` apply to every target of that build: libnode.dll, the
# static libraries linked into it, and the test and build-tool executables.
#
# Hybrid CRT. common.gypi and configure.py keep every object on the static C++
# runtime (/MT, /MTd), and each linked binary here swaps the static UCRT for
# the system UCRT (ucrtbase.dll). libnode.dll then imports only Windows system
# DLLs and api-ms-win-crt-*, like v8jsi.dll (src/v8jsi_settings.gypi), so apps
# need no Visual C++ redistributable. This is safe because libnode.dll exposes
# only C APIs (the embedding API and Node-API): no C++ objects or runtime state
# cross the DLL boundary. The Temporal Rust library refers to the C runtime only
# by plain symbol names, so this link decides its runtime as well.
#
# Hardening, the same as v8jsi.dll:
#   - /guard:cf (compile and link) comes from `vcbuild.bat cfg`.
#   - /GS, /DYNAMICBASE, /NXCOMPAT, and /ZH:SHA_256 are set below.
#   - Spectre: the Spectre-mitigated MSVC runtime libraries (lib\spectre\<arch>).
#     Node.js builds only with clang-cl, which ignores /Qspectre, so the
#     compiler adds no Spectre barriers to our code; neither does it for
#     v8jsi.dll or Hermes.
#   - No /CETCOMPAT: V8's heap snapshot code fails under hardware shadow
#     stacks (see src/v8jsi_settings.gypi).
{
  'target_defaults': {
    'msvs_settings': {
      'VCCLCompilerTool': {
        # SHA-256 source file hashes in the PDBs (BinSkim BA2004).
        'AdditionalOptions': [ '/ZH:SHA_256' ],
        'BufferSecurityCheck': 'true',
      },
      'VCLinkerTool': {
        # MSVS enumeration indexes: 2 is "true".
        'RandomizedBaseAddress': 2,     # /DYNAMICBASE
        'DataExecutionPrevention': 2,   # /NXCOMPAT
      },
    },
    'configurations': {
      'Release': {
        'msvs_settings': {
          'VCLinkerTool': {
            'AdditionalDependencies': [ 'ucrt.lib' ],
            # GYP's MSVS-side name; `IgnoreSpecificDefaultLibraries` is dropped.
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
    # Searched before the default library path, so the linker picks the
    # Spectre-mitigated variants of the static runtime libraries. Needs the VS
    # "C++ Spectre-mitigated libs" component.
    'conditions': [
      ['target_arch=="x64"', {
        'msvs_settings': {
          'VCLinkerTool': {
            'AdditionalLibraryDirectories': [ '$(VCToolsInstallDir)lib\\spectre\\x64' ],
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
