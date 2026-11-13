# Copyright (c) Microsoft Corporation.
# Licensed under the MIT license.
#
# libnode.gyp -- v8-jsi additions to the Windows libnode.dll build.
#
# deps/nodejs/node.gyp includes this file for Windows shared builds. GYP rebases
# plain relative paths here against this directory, so each path here starts
# with a variable: <(libnode_root) is the repository root, relative to
# deps/nodejs.
{
  'variables': {
    # The v8-jsi repository root, relative to deps/nodejs.
    'libnode_root': '../..',
  },

  'targets': [
    {
      # The embedded Stela manifest (tools/stela/README.md): extracts the
      # embedding API (stela.libnode.config.json) and writes
      # libnode.dll.stela.gz plus a .rc that links it as resource STELA 1, which
      # `stela show`, `verify`, and `check` read back from the DLL.
      'target_name': 'libnode_stela_gen',
      'type': 'none',
      'variables': {
        'conditions': [
          ['target_arch=="arm64"', {
            'libnode_stela_target': 'aarch64-pc-windows-msvc',
          }, {
            'libnode_stela_target': 'x86_64-pc-windows-msvc',
          }],
        ],
        'libnode_stela_out': '<(SHARED_INTERMEDIATE_DIR)/libnode_stela',
      },
      'actions': [
        {
          'action_name': 'generate_stela_manifest',
          'inputs': [
            '<(libnode_root)/stela.libnode.config.json',
            '<(libnode_root)/deps/nodejs/src/node_embedding_api.h',
            '<(libnode_root)/deps/nodejs/src/node_api.h',
            '<(libnode_root)/deps/nodejs/src/node_api_types.h',
            '<(libnode_root)/deps/nodejs/src/js_native_api.h',
            '<(libnode_root)/deps/nodejs/src/js_native_api_types.h',
            '<(libnode_root)/tools/stela/extract/stela_extract.c',
            '<(libnode_root)/tools/stela/schema/stela.embedded.schema.json',
            '<(libnode_root)/tools/stela/schema/stela.model.schema.json',
            '<(libnode_root)/tools/stela/schema/stela.raw.schema.json',
            '<(libnode_root)/tools/stela/src/config.ts',
            '<(libnode_root)/tools/stela/src/diff.ts',
            '<(libnode_root)/tools/stela/src/embed.ts',
            '<(libnode_root)/tools/stela/src/extract.ts',
            '<(libnode_root)/tools/stela/src/extractor.ts',
            '<(libnode_root)/tools/stela/src/model.ts',
            '<(libnode_root)/tools/stela/src/pe.ts',
            '<(libnode_root)/tools/stela/src/schema.ts',
            '<(libnode_root)/tools/stela/src/stela.ts',
          ],
          # Only the stamp is declared, for the reason src/v8jsi.gyp gives:
          # `gen embed` leaves unchanged outputs alone, so declaring them would
          # make MSBuild rerun the action on every build.
          'outputs': [
            '<(libnode_stela_out)/libnode.dll.stela.stamp',
          ],
          'action': [
            'node',
            '<(libnode_root)/tools/stela/src/stela.ts',
            'gen', 'embed',
            '--config', '<(libnode_root)/stela.libnode.config.json',
            '--extract',
            '--binary', 'libnode.dll',
            '--target', '<(libnode_stela_target)',
            '--out', '<(libnode_stela_out)',
            '--stamp', '<(libnode_stela_out)/libnode.dll.stela.stamp',
          ],
        },
      ],
      'direct_dependent_settings': {
        'sources': [
          '<(libnode_stela_out)/libnode.dll.stela.rc',
        ],
      },
    },  # libnode_stela_gen
  ],
}
