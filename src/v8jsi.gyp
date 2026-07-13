# Copyright (c) Microsoft Corporation.
# Licensed under the MIT license.
#
# v8jsi.gyp - Build configuration for v8jsi shared library
# This builds v8jsi.dll using V8 from Node.js
#
# Note: When this file is included via node.gyp's 'includes' directive,
# all relative paths are resolved from node.gyp's directory (deps/nodejs),
# NOT from this file's directory.
#
# This file lives in the v8-jsi repo src folder.
# The v8jsi_root variable points to the v8-jsi repo root (../../ from deps/nodejs).
{
  # v8jsi_settings.gypi carries shared MSVC settings (Hybrid CRT in B1;
  # BinSkim/CFG/Spectre will land here in B3). Included at the top level so
  # its `target_defaults` merges into every target in this file.
  'includes': [ 'v8jsi_settings.gypi' ],

  'variables': {
    # Path to v8-jsi repo root relative to deps/nodejs
    'v8jsi_root': '../..',

    # The sandbox build variant (--build-v8jsisb / configure.py). When 1, the
    # v8jsi target is emitted as v8jsisb.dll and is compiled with the V8
    # in-process sandbox + pointer-compression shared cage + jitless/lite mode
    # (the v8_enable_* variables are set in configure.py). Defaulted here so the
    # condition below is valid even on a mainline (v8jsi.dll) build.
    'build_v8jsisb%': 0,

    # Enable inspector support (sets V8JSI_ENABLE_INSPECTOR).
    # v8-jsi must always ship with inspector ON — Office's enableDebugging and
    # RNW's useDirectDebugger both depend on it. Setting this to 0 is only
    # valid as a transient state during V8 13.x modernization.
    'v8jsi_enable_inspector%': 1,

    # Enable test-only ABI hooks (sets JSI_TESTING_ONLY). When 1 (default),
    # v8jsi.dll exports `v8_jsi_test_post_foreground_task` and any other
    # symbols guarded by `#ifdef JSI_TESTING_ONLY`. Production / release
    # builds can flip this to 0 to drop those exports. Note: with this 0,
    # the v8jsi_test target will fail to link any test that references a
    # gated symbol.
    'v8jsi_test_hooks%': 1,

    # Shared V8 core infrastructure (used by legacy code and ABI)
    'v8jsi_core_sources': [
      '<(v8jsi_root)/src/v8_core.h',
      '<(v8jsi_root)/src/v8_core.cpp',
    ],

    # Core v8jsi sources. Now there is no legacy V8Runtime class — only
    # JSI core, V8 instrumentation, MurmurHash, and the public C++ headers
    # consumers depend on.
    'v8jsi_sources': [
      '<(v8jsi_root)/src/v8jsi.cpp',
      '<(v8jsi_root)/src/MurmurHash.cpp',
      '<(v8jsi_root)/src/MurmurHash.h',
      '<(v8jsi_root)/src/V8Instrumentation.cpp',
      '<(v8jsi_root)/src/V8Instrumentation.h',
      '<(v8jsi_root)/src/jsi/JSIDynamic.h',
      '<(v8jsi_root)/src/jsi/decorator.h',
      '<(v8jsi_root)/src/jsi/instrumentation.h',
      '<(v8jsi_root)/src/jsi/jsi-inl.h',
      '<(v8jsi_root)/src/jsi/jsi.cpp',
      '<(v8jsi_root)/src/jsi/jsi.h',
      '<(v8jsi_root)/src/jsi/jsilib.h',
      '<(v8jsi_root)/src/jsi/threadsafe.h',
      '<(v8jsi_root)/src/public/ScriptStore.h',
      '<(v8jsi_root)/src/public/V8JsiRuntime.h',
    ],

    # JSI ABI sources — exception-free code (uses TryCatch + Maybe instead of
    # throw/catch). Node.js default is /EHs-c- (no exceptions), but these
    # are compiled with /EHsc because they share the v8jsi target with
    # legacy code that uses C++ exceptions. Separating into a static lib
    # with the default no-exception flags would require solving MSVC DLL
    # symbol export issues (/WHOLEARCHIVE or explicit forwarding).
    'v8jsi_abi_sources': [
      '<(v8jsi_root)/src/jsi_abi/jsi_abi.h',
      '<(v8jsi_root)/src/jsi_abi/jsi_abi_helpers.h',
      '<(v8jsi_root)/src/jsi_abi/jsi_abi_v8.cpp',
      '<(v8jsi_root)/src/jsi_abi/jsi_abi_v8_internal.h',
      '<(v8jsi_root)/src/jsi_abi/v8_jsi_config.h',
      '<(v8jsi_root)/src/jsi_abi/v8_snapshot_container.h',
      '<(v8jsi_root)/src/jsi_abi/v8_node_api_attach.h',
    ],

    # Node-API sources — restored to the GYP build after being lost in the
    # GN→GYP migration. Always-on; v8jsi.dll ships with Node-API support.
    # Office, RNW, and other consumers depend on the napi_* / jsr_* exports.
    'v8jsi_node_api_sources': [
      '<(v8jsi_root)/src/node-api/env-inl.h',
      '<(v8jsi_root)/src/node-api/js_native_api.h',
      '<(v8jsi_root)/src/node-api/js_native_api_types.h',
      '<(v8jsi_root)/src/node-api/js_native_api_v8.cc',
      '<(v8jsi_root)/src/node-api/js_native_api_v8.h',
      '<(v8jsi_root)/src/node-api/js_native_api_v8_internals.h',
      '<(v8jsi_root)/src/node-api/js_native_api_v8_internals_abi.h',
      '<(v8jsi_root)/src/node-api/js_runtime_api.h',
      '<(v8jsi_root)/src/node-api/util-inl.h',
      '<(v8jsi_root)/src/node-api/v8_api_abi.cpp',
      '<(v8jsi_root)/src/public/compat.h',
      '<(v8jsi_root)/src/public/v8_api.h',
    ],

    # Windows-specific sources
    'v8jsi_sources_win': [
      '<(v8jsi_root)/src/jsi/jsilib-windows.cpp',
      '<(v8jsi_root)/src/etw/tracing.cpp',
      '<(v8jsi_root)/src/etw/tracing.h',
      # New-package VERSIONINFO (Microsoft.JavaScript.V8). The legacy
      # ReactNative.V8Jsi.Windows build uses src/version.rc and is untouched.
      '<(v8jsi_root)/src/version_info.rc',
    ],

    # Inspector sources (Windows only, when v8jsi_enable_inspector is set)
    'v8jsi_inspector_sources': [
      '<(v8jsi_root)/src/inspector/inspector_agent.cpp',
      '<(v8jsi_root)/src/inspector/inspector_agent.h',
      '<(v8jsi_root)/src/inspector/inspector_socket.cpp',
      '<(v8jsi_root)/src/inspector/inspector_socket.h',
      '<(v8jsi_root)/src/inspector/inspector_socket_server.cpp',
      '<(v8jsi_root)/src/inspector/inspector_socket_server.h',
      '<(v8jsi_root)/src/inspector/inspector_tcp.cpp',
      '<(v8jsi_root)/src/inspector/inspector_tcp.h',
      '<(v8jsi_root)/src/inspector/inspector_utils.cpp',
      '<(v8jsi_root)/src/inspector/inspector_utils.h',
      '<(v8jsi_root)/src/inspector/llhttp.c',
      '<(v8jsi_root)/src/inspector/llhttp.h',
      '<(v8jsi_root)/src/inspector/llhttp_api.c',
      '<(v8jsi_root)/src/inspector/llhttp_http.c',
    ],

    # Posix-specific sources
    'v8jsi_sources_posix': [
      '<(v8jsi_root)/src/jsi/jsilib-posix.cpp',
    ],
  },

  'targets': [
    {
      'target_name': 'v8jsi_version_gen',
      'type': 'none',
      'actions': [
        {
          'action_name': 'generate_version_info',
          'inputs': [
            '<(v8jsi_root)/config.json',
            '<(v8jsi_root)/src/generate_version_info.ts',
          ],
          'outputs': [
            '<(v8jsi_root)/src/version_info_gen.h',
          ],
          'action': [
            'node',
            '<(v8jsi_root)/src/generate_version_info.ts',
            '--config', '<(v8jsi_root)/config.json',
            '--output', '<(v8jsi_root)/src/version_info_gen.h',
          ],
        },
      ],
    },  # v8jsi_version_gen target

    {
      'target_name': 'v8jsi',
      'type': 'shared_library',

      'dependencies': [
        # V8 dependencies from Node.js
        # v8_snapshot provides the V8 embedded blob (required for V8 to work)
        # Use --without-intl to reduce size from ~69MB to ~33MB
        'tools/v8_gypfiles/v8.gyp:v8_snapshot',
        'tools/v8_gypfiles/v8.gyp:v8_libplatform',
        # Generate version_gen.rc before building
        'v8jsi_version_gen',
      ],

      'include_dirs': [
        'deps/v8/include',
        '<(v8jsi_root)/src',
        '<(v8jsi_root)/src/jsi',
        '<(v8jsi_root)/src/jsi_abi',
        '<(v8jsi_root)/src/node-api',
        '<(v8jsi_root)/src/public',
        '<(v8jsi_root)/include',
      ],

      'sources': [
        '<@(v8jsi_core_sources)',
        '<@(v8jsi_sources)',
        '<@(v8jsi_abi_sources)',
        '<@(v8jsi_node_api_sources)',
      ],

      'defines': [
        'BUILDING_V8JSI_SHARED',
        'JSI_ABI_BUILDING',
        'V8JSI_ENABLE_NODE_API',
      ],

      'conditions': [
        # Sandbox variant: emit v8jsisb.dll instead of v8jsi.dll. The gyp target
        # name stays 'v8jsi' so v8jsi_test / node_api_tests keep depending on it
        # (gyp links them against the renamed import library automatically); only
        # the output product name changes. The V8 sandbox/lite preprocessor
        # defines (V8_ENABLE_SANDBOX, V8_COMPRESS_POINTERS, ...) reach these
        # sources via common.gypi from configure.py's v8_enable_* variables.
        ['build_v8jsisb==1', {
          'product_name': 'v8jsisb',
          # VER_BINARY selects the per-binary strings in version_info.rc.
          'msvs_settings': {
            'VCResourceCompilerTool': {
              'PreprocessorDefinitions': ['VER_BINARY=2'],
            },
          },
        }, {  # build_v8jsisb==0 -> mainline v8jsi.dll
          'msvs_settings': {
            'VCResourceCompilerTool': {
              'PreprocessorDefinitions': ['VER_BINARY=1'],
            },
          },
        }],
        ['v8jsi_test_hooks==1', {
          'defines': [
            'JSI_TESTING_ONLY',
          ],
        }],
        ['OS=="win"', {
          'sources': [
            '<@(v8jsi_sources_win)',
          ],
          'msvs_settings': {
            'VCCLCompilerTool': {
              # Enable C++ exceptions (/EHsc)
              'ExceptionHandling': 1,
            },
            'VCLinkerTool': {
              'AdditionalDependencies': [
                'dbghelp.lib',
                'bcrypt.lib',
                'shlwapi.lib',
                'winmm.lib',
              ],
            },
          },
          'conditions': [
            ['v8jsi_enable_inspector==1', {
              'sources': [
                '<@(v8jsi_inspector_sources)',
              ],
              'include_dirs': [
                '<(v8jsi_root)/deps/asio/include',
              ],
              'defines': [
                'V8JSI_ENABLE_INSPECTOR',
                # asio standalone mode — no Boost dependency.
                'ASIO_STANDALONE',
                # asio's Win32 SDK target — required when ASIO_STANDALONE is set
                # and not provided by the toolchain. Windows 7 (0x0601) matches
                # the rest of v8-jsi.
                '_WIN32_WINNT=0x0601',
              ],
            }],
          ],
        }],
        ['OS=="linux" or OS=="freebsd" or OS=="openbsd" or OS=="solaris"', {
          'sources': [
            '<@(v8jsi_sources_posix)',
          ],
          'ldflags': [
            '-Wl,--whole-archive',
            '<(obj_dir)/tools/v8_gypfiles/<(STATIC_LIB_PREFIX)v8_base_without_compiler<(STATIC_LIB_SUFFIX)',
            '-Wl,--no-whole-archive',
            '-Wl,--allow-multiple-definition',
            '-Wl,--version-script=<(v8jsi_root)/src/makev8jsi.lst',
          ],
        }],
        ['OS=="mac"', {
          'sources': [
            '<@(v8jsi_sources_posix)',
          ],
          'xcode_settings': {
            'OTHER_LDFLAGS': [
              '-Wl,-force_load,<(PRODUCT_DIR)/<(STATIC_LIB_PREFIX)v8_base_without_compiler<(STATIC_LIB_SUFFIX)',
            ],
          },
        }],
      ],
    },  # v8jsi target

    {
      'target_name': 'v8jsi_test',
      'type': 'executable',

      'dependencies': [
        'v8jsi',
        'deps/googletest/googletest.gyp:gtest',
        'tools/v8_gypfiles/v8.gyp:v8_libplatform',
      ],

      'include_dirs': [
        'deps/v8/include',
        '<(v8jsi_root)/src',
        '<(v8jsi_root)/src/jsi',
        '<(v8jsi_root)/src/jsi_abi',
        '<(v8jsi_root)/src/node-api',
        '<(v8jsi_root)/src/public',
      ],

      'defines': [
        'JSI_V8_IMPL',
        'JSI_SUPPORT_MICROTASKS',
      ],

      'sources': [
        # Test main with runtime factory (no Node-API)
        '<(v8jsi_root)/src/jsitests_main.cpp',
        # JSI test library
        '<(v8jsi_root)/src/jsi/test/testlib.cpp',
        '<(v8jsi_root)/src/jsi/test/testlib.h',
        '<(v8jsi_root)/src/jsi/test/testlib_ext.cpp',
        '<(v8jsi_root)/src/jsi/test/testlib_abi.h',
        # JSI headers needed for tests
        '<(v8jsi_root)/src/jsi/JSIDynamic.h',
        '<(v8jsi_root)/src/jsi/decorator.h',
        '<(v8jsi_root)/src/jsi/instrumentation.h',
        '<(v8jsi_root)/src/jsi/jsi-inl.h',
        '<(v8jsi_root)/src/jsi/jsi.cpp',
        '<(v8jsi_root)/src/jsi/jsi.h',
        '<(v8jsi_root)/src/jsi/jsilib.h',
        '<(v8jsi_root)/src/jsi/threadsafe.h',
        '<(v8jsi_root)/src/public/compat.h',
        # JSI ABI runtime wrapper for tests
        '<(v8jsi_root)/src/jsi_abi/JsiAbiRuntime.h',
        '<(v8jsi_root)/src/jsi_abi/JsiAbiRuntime.cpp',
        '<(v8jsi_root)/src/jsi_abi/jsi_abi.h',
        '<(v8jsi_root)/src/jsi_abi/jsi_abi_helpers.h',
        '<(v8jsi_root)/src/jsi_abi/v8_jsi_config.h',
        # Consumer-side definition of makeV8Runtime. Ships as source in
        # the NuGet; compiled here to prove the round-trip works end-to-end.
        '<(v8jsi_root)/src/public/V8JsiRuntime.cpp',
      ],

      'conditions': [
        ['OS=="win"', {
          'sources': [
            '<(v8jsi_root)/src/jsi/jsilib-windows.cpp',
          ],
          # Enable RTTI - needed for dynamic_pointer_cast in tests
          'cflags_cc': [ '-frtti' ],
          'cflags_cc!': [ '-fno-rtti' ],
          'msvs_settings': {
            'VCCLCompilerTool': {
              'ExceptionHandling': 1,
              'RuntimeTypeInfo': 'true',
            },
            'VCLinkerTool': {
              'AdditionalDependencies': [
                'dbghelp.lib',
                'winmm.lib',
              ],
            },
          },
          'configurations': {
            'Release': {
              'msvs_settings': {
                'VCCLCompilerTool': {
                  'RuntimeTypeInfo': 'true',
                  'ExceptionHandling': 1,
                },
              },
            },
            'Debug': {
              'msvs_settings': {
                'VCCLCompilerTool': {
                  'RuntimeTypeInfo': 'true',
                  'ExceptionHandling': 1,
                },
              },
            },
          },
        }],
        ['OS=="win" and v8jsi_enable_inspector==1', {
          'defines': [
            'V8JSI_ENABLE_INSPECTOR',
          ],
        }],
        ['OS=="linux" or OS=="freebsd" or OS=="openbsd" or OS=="solaris" or OS=="mac"', {
          'sources': [
            '<(v8jsi_root)/src/jsi/jsilib-posix.cpp',
          ],
          'cflags_cc': [ '-frtti' ],
          'cflags_cc!': [ '-fno-rtti' ],
        }],
      ],
    },  # v8jsi_test target
    {
      'target_name': 'mkv8snapshot',
      'type': 'executable',

      # Offline startup-snapshot builder. It LoadLibrary's the engine DLL
      # (v8jsi.dll / v8jsisb.dll) and resolves v8_create_startup_snapshot at
      # runtime, so it links NO V8 and has NO build dependency on the v8jsi
      # target: the SAME exe drives either engine, and using the actual shipped
      # DLL guarantees the blob matches that engine's V8 build flags. Built
      # alongside the engine (rather than the gn sandbox tree) so it ships in the
      # same per-RID package and is available wherever the engine is. Windows-
      # only (Win32 LoadLibrary); the snapshot scenario is Windows-only.
      #
      # Depends on v8jsi_version_gen only to order the version header generation;
      # it does not link anything from it (a 'none' target).
      'dependencies': [
        'v8jsi_version_gen',
      ],

      'sources': [
        '<(v8jsi_root)/tools/mksnapshot/mkv8snapshot.cpp',
      ],

      'conditions': [
        ['OS=="win"', {
          'sources': [
            '<(v8jsi_root)/src/version_info.rc',
          ],
          'msvs_settings': {
            'VCLinkerTool': {
              'SubSystem': '1',  # console (main + fprintf diagnostics)
            },
            'VCResourceCompilerTool': {
              'PreprocessorDefinitions': ['VER_BINARY=3'],
            },
          },
        }],
        ['OS=="win" and target_arch=="ia32"', {
          # Unlike the V8 DLL, this standalone tool links no V8 or hand-written
          # assembly, so its complete object closure supports x86 SafeSEH.
          'msvs_settings': {
            'VCLinkerTool': {
              'ImageHasSafeExceptionHandlers': 'true',
            },
          },
        }],
      ],
    },  # mkv8snapshot target
  ],
}
