# Copyright (c) Microsoft Corporation.
# Licensed under the MIT license.
{
  'includes': ['../v8jsi_settings.gypi'],
  'variables': {'v8jsi_root': '../..'},
  'targets': [
    {
      'target_name': 'v8jsi_legacy_probe_source_link',
      'type': 'none',
      'actions': [{
        'action_name': 'source_link',
        'inputs': ['<(v8jsi_root)/src/generate_source_link.ts'],
        'outputs': ['<(v8jsi_source_link_json)'],
        'action': ['node', '<(v8jsi_root)/src/generate_source_link.ts',
                   '--output', '<(v8jsi_source_link_json)'],
      }],
    },
    {
      'target_name': 'v8jsi_legacy_probe',
      'type': 'shared_library',
      'dependencies': [
        'tools/v8_gypfiles/v8.gyp:v8_snapshot',
        'tools/v8_gypfiles/v8.gyp:v8_libplatform',
        'v8jsi_legacy_probe_source_link',
      ],
      'include_dirs': ['deps/v8/include'],
      'sources': ['<(v8jsi_root)/src/legacy/engine_probe.cpp'],
      'msvs_settings': {
        'VCLinkerTool': {
          'AdditionalDependencies': ['dbghelp.lib', 'winmm.lib'],
          'AdditionalOptions': ['/DEBUG:FULL', '/pdbstream:sourcelink=<(v8jsi_source_link_json)'],
        },
      },
    },
    {
      'target_name': 'v8jsi_legacy_probe_runner',
      'type': 'executable',
      'dependencies': ['v8jsi_legacy_probe'],
      'sources': ['<(v8jsi_root)/src/legacy/engine_probe.cpp'],
      'defines': ['V8JSI_PROBE_RUNNER'],
    },
  ],
}
