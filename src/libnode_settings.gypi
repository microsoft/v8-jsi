# Copyright (c) Microsoft Corporation.
# Licensed under the MIT license.
#
# libnode_settings.gypi -- settings for the libnode builds: Node.js built as a
# shared library (`configure --shared`, `vcbuild.bat dll`).
#
# deps/nodejs/common.gypi includes this file for shared builds, so its
# `target_defaults` apply to every target of that build: the library, the
# static libraries linked into it, and the test and build-tool executables.
#
# Windows: Hybrid CRT. common.gypi and configure.py keep every object on the
# static C++ runtime (/MT, /MTd), and each linked binary here swaps the static
# UCRT for the system UCRT (ucrtbase.dll). libnode.dll then imports only
# Windows system DLLs and api-ms-win-crt-*, like v8jsi.dll
# (src/v8jsi_settings.gypi), so apps need no Visual C++ redistributable. This
# is safe because libnode.dll exposes only C APIs (the embedding API and
# Node-API): no C++ objects or runtime state cross the DLL boundary. The
# Temporal Rust library refers to the C runtime only by plain symbol names, so
# this link decides its runtime as well.
#
# Windows hardening, the same as v8jsi.dll:
#   - /guard:cf (compile and link) comes from `vcbuild.bat cfg`.
#   - /GS, /DYNAMICBASE, /NXCOMPAT, and /ZH:SHA_256 are set below.
#   - Spectre: the Spectre-mitigated MSVC runtime libraries (lib\spectre\<arch>).
#     Node.js builds only with clang-cl, which ignores /Qspectre, so the
#     compiler adds no Spectre barriers to our code; neither does it for
#     v8jsi.dll or Hermes.
#   - No /CETCOMPAT: libnode x64 does not enable V8's CET switch, which
#     supplies the shadow-stack adjustments needed for deopt and unwinding.
#
# Linux: the Linux counterpart of the Hybrid CRT. The build image
# (.ado/image/libnode-linux-build.Dockerfile) has a newer libstdc++ than the
# Ubuntu 22.04 that libnode.so supports, so libnode.so links the C++ runtime,
# libgcc, and libatomic statically and keeps them private: --exclude-libs
# hides the symbols of those archives and of the Temporal Rust library, and
# src/libnode.version hides the C++ runtime code that Node.js instantiates
# from its headers. libnode.so then needs only glibc.
#
# Linux and macOS: the packages ship the library as libnode.so and
# libnode.dylib, and a C consumer that links against them must load them
# under the same name, so the library is built with that name (SONAME and
# install name) instead of Node.js's libnode.so.<NODE_MODULE_VERSION> and
# libnode.<NODE_MODULE_VERSION>.dylib. target_conditions run after node.gyp's
# conditions, which set the versioned name.
#
# macOS: Release objects otherwise carry full DWARF that no package ships,
# and its copies in static archives fill the CI disk. Disable the default
# debug information, as tools/v8_gypfiles/v8.gyp does for v8_snapshot.
{
  'conditions': [
    ['OS=="win"', {
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
    }],
    ['OS=="linux"', {
      'variables': {
        # Absolute, because the linker runs in the build output folder.
        'libnode_version_script': '<!(cd ../.. && pwd)/src/libnode.version',
      },
      'target_defaults': {
        # libnode is node.gyp's node_lib_target_name, which the other .gyp
        # files that include this one don't define.
        'target_conditions': [
          ['_target_name=="libnode"', {
            'product_extension': 'so',
            # node.gyp links Clang builds with libatomic, which not every
            # system has; libnode.so links its static library instead.
            'libraries!': [ '-latomic' ],
            'libraries': [ '-l:libatomic.a' ],
            'ldflags': [
              '-static-libstdc++',
              '-static-libgcc',
              '-Wl,--exclude-libs=libatomic.a:libgcc.a:libgcc_eh.a:libnode_crates.a:libstdc++.a',
              '-Wl,--version-script=<(libnode_version_script)',
              '-Wl,-z,noexecstack',
            ],
          }],
        ],
      },
    }],
    ['OS=="mac"', {
      'target_defaults': {
        'configurations': {
          'Release': {
            'xcode_settings': {
              'GCC_GENERATE_DEBUGGING_SYMBOLS': 'NO',
            },
          },
        },
        'target_conditions': [
          ['_target_name=="libnode"', {
            'product_extension': 'dylib',
            'xcode_settings': {
              'LD_DYLIB_INSTALL_NAME': '@rpath/libnode.dylib',
            },
          }],
        ],
      },
    }],
  ],
}
