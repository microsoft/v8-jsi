# Copyright (c) Microsoft Corporation.
# Licensed under the MIT license.
#
# node_api_tests.gyp - Build configuration for the Node-API conformance test rig.
#
# Ports the BUILD.gn-era test rig (src/node-api/test/BUILD.gn) to GYP:
#   - 34 loadable_module test addons under js-native-api/<name>/
#   - node_lite.exe — JS-runner that LoadLibrary()'s addons and runs JS tests
#   - node_api_tests.exe — GoogleTest harness that spawns node_lite per .js file
#   - copy_node_api_test_js_files — populates out/<conf>/test-js-files/
#
# This file is included from src/v8jsi.gyp; relative paths follow the same
# convention as v8jsi.gyp — anchored to the v8-jsi repo root via v8jsi_root.
{
  'variables': {
    # Path to v8-jsi repo root relative to the outer gyp (deps/nodejs/node.gyp).
    # Set as '%=' so an outer scope can override (e.g. when v8jsi.gyp has
    # already declared it).
    'v8jsi_root%': '../..',

    # Common addon settings so each loadable_module target stays terse.
    'test_addon_include_dirs': [
      '<(v8jsi_root)/src/node-api',
      '<(v8jsi_root)/src/node-api/test/js-native-api',
    ],
    # BUILDING_NODE_EXTENSION flips NAPI_EXTERN to __declspec(dllimport) in
    # the test-side js-native-api/node_api.h so napi_* resolve via v8jsi.lib.
    'test_addon_defines': [
      'BUILDING_NODE_EXTENSION',
    ],
  },

  'targets': [
    #
    # JS test files copy.
    #
    # Copies the .js test files from src/node-api/test/ into
    # <PRODUCT_DIR>/test-js-files/<rel-path>, preserving the directory
    # structure so RegisterNodeApiTests' recursive scan finds them next to
    # node_api_tests.exe / node_lite.exe.
    #
    {
      'target_name': 'copy_node_api_test_js_files',
      'type': 'none',
      'actions': [
        {
          'action_name': 'copy_test_js_files',
          'inputs': [
            '<(v8jsi_root)/src/copy_test_js_files.ts',
          ],
          # Use a stamp file as the GYP output marker since gyp wants outputs.
          'outputs': [
            '<(PRODUCT_DIR)/test-js-files/.copied.stamp',
          ],
          'action': [
            'node',
            '<(v8jsi_root)/src/copy_test_js_files.ts',
            '--source-base', '<(v8jsi_root)/src/node-api/test',
            # Pass the destination root explicitly (rather than PRODUCT_DIR)
            # so the path doesn't end in MSVS's trailing backslash, which
            # would escape the closing quote in the generated MSBuild
            # custom-build action.
            '--dest-root', '<(PRODUCT_DIR)/test-js-files',
            '--stamp', '<(PRODUCT_DIR)/test-js-files/.copied.stamp',
          ],
          'msvs_cygwin_shell': 0,
        },
      ],
    },

    #
    # node_lite.exe — JS-runner used as a child process by node_api_tests.exe.
    # Initializes a jsr_runtime, evaluates a single .js file, and dynamically
    # loads addon DLLs through JSRequire().
    #
    {
      'target_name': 'node_lite',
      'type': 'executable',
      'dependencies': [
        'v8jsi',
      ],
      'include_dirs': [
        '<(v8jsi_root)/src/node-api',
        '<(v8jsi_root)/src/node-api/test',
        '<(v8jsi_root)/src/node-api/test/js-native-api',
        '<(v8jsi_root)/src/public',
      ],
      'defines': [
        'NAPI_EXPERIMENTAL',
      ],
      'sources': [
        '<(v8jsi_root)/src/node-api/test/child_process.cpp',
        '<(v8jsi_root)/src/node-api/test/child_process.h',
        '<(v8jsi_root)/src/node-api/test/node_api_test.cpp',
        '<(v8jsi_root)/src/node-api/test/node_api_test.h',
        '<(v8jsi_root)/src/node-api/test/node_api_test_v8.cpp',
      ],
      'conditions': [
        ['OS=="win"', {
          'msvs_settings': {
            'VCCLCompilerTool': {
              'ExceptionHandling': 1,  # /EHsc
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
                  'ExceptionHandling': 1,
                  'RuntimeTypeInfo': 'true',
                },
              },
            },
            'Debug': {
              'msvs_settings': {
                'VCCLCompilerTool': {
                  'ExceptionHandling': 1,
                  'RuntimeTypeInfo': 'true',
                },
              },
            },
          },
        }],
      ],
    },

    #
    # Test addon DLLs — each is a small native module loaded into node_lite
    # via LoadLibraryA. Registered with napi_register_module_v1.
    # Mirrors the ":test_module" config + per-target settings from BUILD.gn.
    #

    {
      'target_name': '2_function_arguments',
      'type': 'loadable_module',
      'dependencies': ['v8jsi'],
      'include_dirs': ['<@(test_addon_include_dirs)'],
      'defines': [
        '<@(test_addon_defines)',
        'NODE_GYP_MODULE_NAME=2_function_arguments',
      ],
      'sources': [
        '<(v8jsi_root)/src/node-api/test/js-native-api/2_function_arguments/2_function_arguments.c',
      ],
    },

    {
      'target_name': '3_callbacks',
      'type': 'loadable_module',
      'dependencies': ['v8jsi'],
      'include_dirs': ['<@(test_addon_include_dirs)'],
      'defines': [
        '<@(test_addon_defines)',
        'NODE_GYP_MODULE_NAME=3_callbacks',
      ],
      'sources': [
        '<(v8jsi_root)/src/node-api/test/js-native-api/3_callbacks/3_callbacks.c',
      ],
    },

    {
      'target_name': '4_object_factory',
      'type': 'loadable_module',
      'dependencies': ['v8jsi'],
      'include_dirs': ['<@(test_addon_include_dirs)'],
      'defines': [
        '<@(test_addon_defines)',
        'NODE_GYP_MODULE_NAME=4_object_factory',
      ],
      'sources': [
        '<(v8jsi_root)/src/node-api/test/js-native-api/4_object_factory/4_object_factory.c',
      ],
    },

    {
      'target_name': '5_function_factory',
      'type': 'loadable_module',
      'dependencies': ['v8jsi'],
      'include_dirs': ['<@(test_addon_include_dirs)'],
      'defines': [
        '<@(test_addon_defines)',
        'NODE_GYP_MODULE_NAME=5_function_factory',
      ],
      'sources': [
        '<(v8jsi_root)/src/node-api/test/js-native-api/5_function_factory/5_function_factory.c',
      ],
    },

    {
      'target_name': '6_object_wrap',
      'type': 'loadable_module',
      'dependencies': ['v8jsi'],
      'include_dirs': ['<@(test_addon_include_dirs)'],
      'defines': [
        '<@(test_addon_defines)',
        'NODE_GYP_MODULE_NAME=6_object_wrap',
      ],
      'sources': [
        '<(v8jsi_root)/src/node-api/test/js-native-api/6_object_wrap/6_object_wrap.cc',
      ],
    },

    {
      'target_name': '7_factory_wrap',
      'type': 'loadable_module',
      'dependencies': ['v8jsi'],
      'include_dirs': ['<@(test_addon_include_dirs)'],
      'defines': [
        '<@(test_addon_defines)',
        'NODE_GYP_MODULE_NAME=7_factory_wrap',
      ],
      'sources': [
        '<(v8jsi_root)/src/node-api/test/js-native-api/7_factory_wrap/7_factory_wrap.cc',
        '<(v8jsi_root)/src/node-api/test/js-native-api/7_factory_wrap/myobject.cc',
      ],
    },

    {
      'target_name': '8_passing_wrapped',
      'type': 'loadable_module',
      'dependencies': ['v8jsi'],
      'include_dirs': ['<@(test_addon_include_dirs)'],
      'defines': [
        '<@(test_addon_defines)',
        'NODE_GYP_MODULE_NAME=8_passing_wrapped',
      ],
      'sources': [
        '<(v8jsi_root)/src/node-api/test/js-native-api/8_passing_wrapped/8_passing_wrapped.cc',
        '<(v8jsi_root)/src/node-api/test/js-native-api/8_passing_wrapped/myobject.cc',
      ],
    },

    {
      'target_name': 'test_array',
      'type': 'loadable_module',
      'dependencies': ['v8jsi'],
      'include_dirs': ['<@(test_addon_include_dirs)'],
      'defines': [
        '<@(test_addon_defines)',
        'NODE_GYP_MODULE_NAME=test_array',
      ],
      'sources': [
        '<(v8jsi_root)/src/node-api/test/js-native-api/test_array/test_array.c',
      ],
    },

    {
      'target_name': 'test_bigint',
      'type': 'loadable_module',
      'dependencies': ['v8jsi'],
      'include_dirs': ['<@(test_addon_include_dirs)'],
      'defines': [
        '<@(test_addon_defines)',
        'NODE_GYP_MODULE_NAME=test_bigint',
      ],
      'sources': [
        '<(v8jsi_root)/src/node-api/test/js-native-api/test_bigint/test_bigint.c',
      ],
    },

    {
      'target_name': 'test_cannot_run_js',
      'type': 'loadable_module',
      'dependencies': ['v8jsi'],
      'include_dirs': ['<@(test_addon_include_dirs)'],
      'defines': [
        '<@(test_addon_defines)',
        'NAPI_EXPERIMENTAL',
        'NODE_GYP_MODULE_NAME=test_cannot_run_js',
      ],
      'sources': [
        '<(v8jsi_root)/src/node-api/test/js-native-api/test_cannot_run_js/test_cannot_run_js.c',
      ],
    },

    # test_pending_exception: same source as test_cannot_run_js but pinned to
    # NAPI v8 to exercise the legacy non-experimental code path.
    {
      'target_name': 'test_pending_exception',
      'type': 'loadable_module',
      'dependencies': ['v8jsi'],
      'include_dirs': ['<@(test_addon_include_dirs)'],
      'defines': [
        '<@(test_addon_defines)',
        'NAPI_VERSION=8',
        'NODE_GYP_MODULE_NAME=test_pending_exception',
      ],
      'sources': [
        '<(v8jsi_root)/src/node-api/test/js-native-api/test_cannot_run_js/test_cannot_run_js.c',
      ],
    },

    {
      'target_name': 'test_constructor',
      'type': 'loadable_module',
      'dependencies': ['v8jsi'],
      'include_dirs': ['<@(test_addon_include_dirs)'],
      'defines': [
        '<@(test_addon_defines)',
        'NODE_GYP_MODULE_NAME=test_constructor',
      ],
      'sources': [
        '<(v8jsi_root)/src/node-api/test/js-native-api/test_constructor/test_constructor.c',
        '<(v8jsi_root)/src/node-api/test/js-native-api/test_constructor/test_null.c',
      ],
    },

    {
      'target_name': 'test_conversions',
      'type': 'loadable_module',
      'dependencies': ['v8jsi'],
      'include_dirs': ['<@(test_addon_include_dirs)'],
      'defines': [
        '<@(test_addon_defines)',
        'NODE_GYP_MODULE_NAME=test_conversions',
      ],
      'sources': [
        '<(v8jsi_root)/src/node-api/test/js-native-api/test_conversions/test_conversions.c',
        '<(v8jsi_root)/src/node-api/test/js-native-api/test_conversions/test_null.c',
      ],
    },

    {
      'target_name': 'test_dataview',
      'type': 'loadable_module',
      'dependencies': ['v8jsi'],
      'include_dirs': ['<@(test_addon_include_dirs)'],
      'defines': [
        '<@(test_addon_defines)',
        'NODE_GYP_MODULE_NAME=test_dataview',
      ],
      'sources': [
        '<(v8jsi_root)/src/node-api/test/js-native-api/test_dataview/test_dataview.c',
      ],
    },

    {
      'target_name': 'test_date',
      'type': 'loadable_module',
      'dependencies': ['v8jsi'],
      'include_dirs': ['<@(test_addon_include_dirs)'],
      'defines': [
        '<@(test_addon_defines)',
        'NODE_GYP_MODULE_NAME=test_date',
      ],
      'sources': [
        '<(v8jsi_root)/src/node-api/test/js-native-api/test_date/test_date.c',
      ],
    },

    {
      'target_name': 'test_error',
      'type': 'loadable_module',
      'dependencies': ['v8jsi'],
      'include_dirs': ['<@(test_addon_include_dirs)'],
      'defines': [
        '<@(test_addon_defines)',
        'NODE_GYP_MODULE_NAME=test_error',
      ],
      'sources': [
        '<(v8jsi_root)/src/node-api/test/js-native-api/test_error/test_error.c',
      ],
    },

    {
      'target_name': 'test_exception',
      'type': 'loadable_module',
      'dependencies': ['v8jsi'],
      'include_dirs': ['<@(test_addon_include_dirs)'],
      'defines': [
        '<@(test_addon_defines)',
        'NODE_GYP_MODULE_NAME=test_exception',
      ],
      'sources': [
        '<(v8jsi_root)/src/node-api/test/js-native-api/test_exception/test_exception.c',
      ],
    },

    # test_exceptions: lives under js-native-api/test_object/.
    {
      'target_name': 'test_exceptions',
      'type': 'loadable_module',
      'dependencies': ['v8jsi'],
      'include_dirs': ['<@(test_addon_include_dirs)'],
      'defines': [
        '<@(test_addon_defines)',
        'NODE_GYP_MODULE_NAME=test_exceptions',
      ],
      'sources': [
        '<(v8jsi_root)/src/node-api/test/js-native-api/test_object/test_exceptions.c',
      ],
    },

    {
      'target_name': 'test_finalizer',
      'type': 'loadable_module',
      'dependencies': ['v8jsi'],
      'include_dirs': ['<@(test_addon_include_dirs)'],
      'defines': [
        '<@(test_addon_defines)',
        'NAPI_EXPERIMENTAL',
        'NODE_GYP_MODULE_NAME=test_finalizer',
      ],
      'sources': [
        '<(v8jsi_root)/src/node-api/test/js-native-api/test_finalizer/test_finalizer.c',
      ],
    },

    {
      'target_name': 'test_function',
      'type': 'loadable_module',
      'dependencies': ['v8jsi'],
      'include_dirs': ['<@(test_addon_include_dirs)'],
      'defines': [
        '<@(test_addon_defines)',
        'NODE_GYP_MODULE_NAME=test_function',
      ],
      'sources': [
        '<(v8jsi_root)/src/node-api/test/js-native-api/test_function/test_function.c',
      ],
    },

    {
      'target_name': 'test_general',
      'type': 'loadable_module',
      'dependencies': ['v8jsi'],
      'include_dirs': ['<@(test_addon_include_dirs)'],
      'defines': [
        '<@(test_addon_defines)',
        'NODE_GYP_MODULE_NAME=test_general',
      ],
      'sources': [
        '<(v8jsi_root)/src/node-api/test/js-native-api/test_general/test_general.c',
      ],
    },

    {
      'target_name': 'test_handle_scope',
      'type': 'loadable_module',
      'dependencies': ['v8jsi'],
      'include_dirs': ['<@(test_addon_include_dirs)'],
      'defines': [
        '<@(test_addon_defines)',
        'NODE_GYP_MODULE_NAME=test_handle_scope',
      ],
      'sources': [
        '<(v8jsi_root)/src/node-api/test/js-native-api/test_handle_scope/test_handle_scope.c',
      ],
    },

    {
      'target_name': 'test_instance_data',
      'type': 'loadable_module',
      'dependencies': ['v8jsi'],
      'include_dirs': ['<@(test_addon_include_dirs)'],
      'defines': [
        '<@(test_addon_defines)',
        'NODE_GYP_MODULE_NAME=test_instance_data',
      ],
      'sources': [
        '<(v8jsi_root)/src/node-api/test/js-native-api/test_instance_data/test_instance_data.c',
      ],
    },

    {
      'target_name': 'test_new_target',
      'type': 'loadable_module',
      'dependencies': ['v8jsi'],
      'include_dirs': ['<@(test_addon_include_dirs)'],
      'defines': [
        '<@(test_addon_defines)',
        'NODE_GYP_MODULE_NAME=test_new_target',
      ],
      'sources': [
        '<(v8jsi_root)/src/node-api/test/js-native-api/test_new_target/test_new_target.c',
      ],
    },

    {
      'target_name': 'test_number',
      'type': 'loadable_module',
      'dependencies': ['v8jsi'],
      'include_dirs': ['<@(test_addon_include_dirs)'],
      'defines': [
        '<@(test_addon_defines)',
        'NODE_GYP_MODULE_NAME=test_number',
      ],
      'sources': [
        '<(v8jsi_root)/src/node-api/test/js-native-api/test_number/test_null.c',
        '<(v8jsi_root)/src/node-api/test/js-native-api/test_number/test_number.c',
      ],
    },

    {
      'target_name': 'test_object',
      'type': 'loadable_module',
      'dependencies': ['v8jsi'],
      'include_dirs': ['<@(test_addon_include_dirs)'],
      'defines': [
        '<@(test_addon_defines)',
        'NODE_GYP_MODULE_NAME=test_object',
      ],
      'sources': [
        '<(v8jsi_root)/src/node-api/test/js-native-api/test_object/test_null.c',
        '<(v8jsi_root)/src/node-api/test/js-native-api/test_object/test_object.c',
      ],
    },

    {
      'target_name': 'test_promise',
      'type': 'loadable_module',
      'dependencies': ['v8jsi'],
      'include_dirs': ['<@(test_addon_include_dirs)'],
      'defines': [
        '<@(test_addon_defines)',
        'NODE_GYP_MODULE_NAME=test_promise',
      ],
      'sources': [
        '<(v8jsi_root)/src/node-api/test/js-native-api/test_promise/test_promise.c',
      ],
    },

    {
      'target_name': 'test_properties',
      'type': 'loadable_module',
      'dependencies': ['v8jsi'],
      'include_dirs': ['<@(test_addon_include_dirs)'],
      'defines': [
        '<@(test_addon_defines)',
        'NODE_GYP_MODULE_NAME=test_properties',
      ],
      'sources': [
        '<(v8jsi_root)/src/node-api/test/js-native-api/test_properties/test_properties.c',
      ],
    },

    {
      'target_name': 'test_reference',
      'type': 'loadable_module',
      'dependencies': ['v8jsi'],
      'include_dirs': ['<@(test_addon_include_dirs)'],
      'defines': [
        '<@(test_addon_defines)',
        'NODE_GYP_MODULE_NAME=test_reference',
      ],
      'sources': [
        '<(v8jsi_root)/src/node-api/test/js-native-api/test_reference/test_reference.c',
      ],
    },

    # test_ref_finalizer: source is test_reference/test_finalizer.c.
    {
      'target_name': 'test_ref_finalizer',
      'type': 'loadable_module',
      'dependencies': ['v8jsi'],
      'include_dirs': ['<@(test_addon_include_dirs)'],
      'defines': [
        '<@(test_addon_defines)',
        'NODE_GYP_MODULE_NAME=test_ref_finalizer',
      ],
      'sources': [
        '<(v8jsi_root)/src/node-api/test/js-native-api/test_reference/test_finalizer.c',
      ],
    },

    {
      'target_name': 'test_reference_double_free',
      'type': 'loadable_module',
      'dependencies': ['v8jsi'],
      'include_dirs': ['<@(test_addon_include_dirs)'],
      'defines': [
        '<@(test_addon_defines)',
        'NODE_GYP_MODULE_NAME=test_reference_double_free',
      ],
      'sources': [
        '<(v8jsi_root)/src/node-api/test/js-native-api/test_reference_double_free/test_reference_double_free.c',
      ],
    },

    {
      'target_name': 'test_string',
      'type': 'loadable_module',
      'dependencies': ['v8jsi'],
      'include_dirs': ['<@(test_addon_include_dirs)'],
      'defines': [
        '<@(test_addon_defines)',
        'NAPI_EXPERIMENTAL',
        'NODE_GYP_MODULE_NAME=test_string',
      ],
      'sources': [
        '<(v8jsi_root)/src/node-api/test/js-native-api/test_string/test_null.c',
        '<(v8jsi_root)/src/node-api/test/js-native-api/test_string/test_string.c',
      ],
    },

    {
      'target_name': 'test_symbol',
      'type': 'loadable_module',
      'dependencies': ['v8jsi'],
      'include_dirs': ['<@(test_addon_include_dirs)'],
      'defines': [
        '<@(test_addon_defines)',
        'NODE_GYP_MODULE_NAME=test_symbol',
      ],
      'sources': [
        '<(v8jsi_root)/src/node-api/test/js-native-api/test_symbol/test_symbol.c',
      ],
    },

    {
      'target_name': 'test_typedarray',
      'type': 'loadable_module',
      'dependencies': ['v8jsi'],
      'include_dirs': ['<@(test_addon_include_dirs)'],
      'defines': [
        '<@(test_addon_defines)',
        'NODE_GYP_MODULE_NAME=test_typedarray',
      ],
      'sources': [
        '<(v8jsi_root)/src/node-api/test/js-native-api/test_typedarray/test_typedarray.c',
      ],
    },

    #
    # node_api_tests.exe — GoogleTest harness. Walks <PRODUCT_DIR>/test-js-files/
    # at runtime and registers one GTest per .js file. Each test spawns
    # node_lite.exe as a child process. Does NOT link against v8jsi.dll —
    # the harness is a pure black-box runner over the addon DLLs + node_lite.
    #
    {
      'target_name': 'node_api_tests',
      'type': 'executable',
      'dependencies': [
        'node_lite',
        'copy_node_api_test_js_files',
        'deps/googletest/googletest.gyp:gtest',
        # 34 addon DLLs — declared as deps so they all build before the
        # harness runs. node_api_tests.exe does not link against them; the
        # dependency is purely a runtime/spawn relationship.
        '2_function_arguments',
        '3_callbacks',
        '4_object_factory',
        '5_function_factory',
        '6_object_wrap',
        '7_factory_wrap',
        '8_passing_wrapped',
        'test_array',
        'test_bigint',
        'test_cannot_run_js',
        'test_constructor',
        'test_conversions',
        'test_dataview',
        'test_date',
        'test_error',
        'test_exception',
        'test_exceptions',
        'test_finalizer',
        'test_function',
        'test_general',
        'test_handle_scope',
        'test_instance_data',
        'test_new_target',
        'test_number',
        'test_object',
        'test_pending_exception',
        'test_promise',
        'test_properties',
        'test_ref_finalizer',
        'test_reference',
        'test_reference_double_free',
        'test_string',
        'test_symbol',
        'test_typedarray',
      ],
      'include_dirs': [
        '<(v8jsi_root)/src/node-api/test',
      ],
      'sources': [
        '<(v8jsi_root)/src/node-api/test/child_process.cpp',
        '<(v8jsi_root)/src/node-api/test/child_process.h',
        '<(v8jsi_root)/src/node-api/test/testmain.cpp',
      ],
      'conditions': [
        ['OS=="win"', {
          'msvs_settings': {
            'VCCLCompilerTool': {
              'ExceptionHandling': 1,
              'RuntimeTypeInfo': 'true',
            },
          },
          'configurations': {
            'Release': {
              'msvs_settings': {
                'VCCLCompilerTool': {
                  'ExceptionHandling': 1,
                  'RuntimeTypeInfo': 'true',
                },
              },
            },
            'Debug': {
              'msvs_settings': {
                'VCCLCompilerTool': {
                  'ExceptionHandling': 1,
                  'RuntimeTypeInfo': 'true',
                },
              },
            },
          },
        }],
      ],
    },
  ],
}
