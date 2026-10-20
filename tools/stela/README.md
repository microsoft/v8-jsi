# Stela

Stela checks C ABI headers. It provides the annotation vocabulary, the native
extractor, `stela vendor`, the merged `stela.model/1`, the header-test and
layout-test generators, C ABI convention lint, ABI diff against committed
baselines, and API manifests embedded in the binaries that implement the
headers. It incubates here with no
dependency on v8-jsi code, so it can move to its own repository unchanged.

Status: stage S6, embedded metadata.

## Requirements

- Node.js 24 or later. Stela runs TypeScript directly; there is no build step.
- Visual Studio with the C++ tools and the "C++ Clang Compiler for Windows"
  component (`Microsoft.VisualStudio.Component.VC.Llvm.Clang`). `extract` (and
  so `gen embed`, `check`, and the v8-jsi builds that embed manifests) needs
  its libclang; the compile-matrix tests need its clang.

## Extractor

`extract/stela_extract.c` is a C17 libclang client. It parses one target and
writes deterministic `stela.raw/1` JSON. The Node helper in
`src/extractor.ts` builds it on demand with the host-native MSVC and libclang,
keys the output by the sources and libclang binary, and supplies the selected
target's MSVC and Windows SDK include paths.

The native command is:

```text
stela-extract --target <triple> --root <dir> --out <file.json>
              [--include <dir>]... [--define <NAME[=VALUE]>]...
              [--owns <path>]... <header>...
```

The default owned paths are the entry headers. Declarations are assigned by
their macro expansion location. Any parse error exits with code 2 and does not
replace an existing output.

The vendored libclang C API headers are pinned to LLVM 22.1.3. Set
`STELA_LIBCLANG_DIR` to override the host-native directory containing
`libclang.dll`; the matching import library must be in the sibling `lib`
directory.

## The annotation template

[`include/stela.h`](include/stela.h) is a template; nothing includes it.
`stela vendor` generates one `<lib>_stela.h` per library from it, with the
library's macro prefix:

- `STELA_<NAME>` becomes `<LIB>_<NAME>`: `V8HOST_NULLABLE`, `V8HOST_ENUM`.
- The internal helpers become `<LIB>_STELA_VERSION`, `<LIB>_STELA_ANNOTATE_`,
  and `<LIB>_STELA_FIXED_ENUMS_`.
- `STELA_EXTRACT` keeps its name. It's the extractor's command-line switch.
- A plain `<LIB>_STELA_H_` include guard replaces the template's version
  guard.

Outside the extractor every annotation expands to nothing, so the generated
file costs nothing at compile time. Each library's names carry its own prefix,
so any mix of libraries and Stela versions can meet in one translation unit.
Never edit a generated file; change the template or the configuration and
regenerate.

## Usage

```powershell
node tools\stela\src\stela.ts vendor            # write each library's <lib>_stela.h
node tools\stela\src\stela.ts vendor --check    # exit 1 if any file is missing or stale
node tools\stela\src\stela.ts extract           # write out\stela\stela.model.json
node tools\stela\src\stela.ts lint              # check the extracted model
node tools\stela\src\stela.ts lint --report-only # report without failing
node tools\stela\src\stela.ts gen layout-tests --out out\stela\tests
node tools\stela\src\stela.ts diff              # compare the model with the baselines
node tools\stela\src\stela.ts diff --base old.model.json --head new.model.json
node tools\stela\src\stela.ts baseline          # refresh every configured baseline
node tools\stela\src\stela.ts baseline --out pkg\stela sbox   # write pkg\stela\sbox.model.json
node tools\stela\src\stela.ts gen embed --binary v8host.dll --target x86_64-pc-windows-msvc --out out\stela
node tools\stela\src\stela.ts show v8host.dll   # print the API a binary's manifest describes
node tools\stela\src\stela.ts verify v8host.dll sbox.exe   # exports against manifests
node tools\stela\src\stela.ts check v8host.dll --config stela.config.json
node tools\stela\src\stela.ts check v8host.dll --headers include\sbox.h include\v8host_client.h
node tools\stela\src\stela.ts diff --base old\v8host.dll --head v8host.dll
```

Options: `--config <file>` (default `stela.config.json` in the current
directory), `--template <file>` (`vendor`; default `include/stela.h`),
`--out <file>` (`extract`; default `out/stela/<config name>.model.json` next to
the configuration), `--model <file>` (`lint`, `gen`, and `baseline`; default
the `extract` output), `--base` and `--head <file>` (`diff`; default the
configured baselines and the `extract` output; either may be a binary), and
`--format text|json` (`lint`, `diff`, and `check`). Paths in the configuration
are relative to the configuration file. Every command writes a file only when
its content changes.

Exit codes: 0 success; 1 check failures (lint errors, ABI breaks, out-of-date
files, a declaration that differs between targets, or exports that don't match
a manifest); 2 usage, configuration, or tool errors, including a binary with
no or a corrupt manifest. `lint --report-only` reports errors but exits 0.

## Lint

`stela lint` applies the v1 rules from the Stela design to every declaration in
the model. It also uses the configuration to verify generated Stela headers and
to reject definitions or preprocessor tests of reserved Stela macros. Text
output is deterministic and compiler-shaped; JSON output is an array of finding
objects.

Errors fail the command. `bool_type` and `macro_constants` default to warnings,
and `annotation_redundant` defaults to informational. Override a rule with
`lint.rules` in `stela.config.json`, using `error`, `warning`, `info`, or `off`.
List intentional integer macros in `lint.macroConstants`. A declaration can
suppress one rule with `<LIB>_ALLOW(rule)`.

In v8-jsi, lint is a gate: `scripts/header-check.ts` runs it on the extracted
model and fails on any finding, at any severity.

## Diff

`stela diff` compares two models and classifies each change as a break, an
addition, or a note. Breaks include:

- a declaration or target removed;
- a changed signature, calling convention, typedef, enumerator value, or
  macro value;
- a changed size, alignment, or offset;
- a table slot reordered, removed, or inserted;
- a changed contract annotation, such as `transfer` or `count`.

Additions are new declarations, enumerators, and targets, and fields appended
to a struct that starts with `struct_size` or to a function table. Notes cover
docs, `deprecated`, parameter names, and a raised `*_ABI_VERSION`.

Annotations are compared in effect. One that only restates what a name
implies, such as `out` on an `out_` parameter, is no change, and references
such as `count=n` compare by position.

`nullable` is a break when it narrows what a caller may pass or widens what it
may get back. On callbacks and struct fields, either side may implement them,
so any `nullable` change is a break.

A library with additions must raise its `*_ABI_VERSION`. An extension library
with no version macro of its own uses the one from the library that owns its
status type. Without a raise, `diff` reports `version_not_bumped` as a break.

Breaks in a declaration annotated `experimental`, or in a header listed in
`diff.experimental`, are reported as notes. The baseline decides: marking a
stable declaration experimental, or moving it into such a header, doesn't
excuse breaking it.

Text output is one compiler-shaped line per change, followed by a summary
line. JSON output is an array of finding objects. The exit code is 1 if any
break remains.

`stela diff` with no `--base` compares the model with the configured
`diff.baselines`, covering only the libraries that have one. `stela baseline`
writes each baseline from the current model, one library per file, as
`stela.model/1`.

In v8-jsi the baselines are `api/<library>.model.json`, and
`scripts/header-check.ts` fails on any break. A deliberate break refreshes the
affected baseline in the same change.

`baseline --out <dir>` writes `<dir>/<library>.model.json` instead, for any
configured library (default: all of them); v8-jsi uses it to ship each public
header's model in the NuGet package.

## Embedded manifests (`stela.embedded/1`)

A binary that implements a C ABI carries the API it was built against: a
gzipped `stela.embedded/1` JSON document in the PE resource `STELA` 1.
[`schema/stela.embedded.schema.json`](schema/stela.embedded.schema.json)
describes the format. A manifest names the binary, its targets, and the
producer (Stela and libclang versions). Per library, it has the library's
configuration, the `modelSha256` of its `stela.model/1` (all targets, so the
hash equals the shipped or baseline `<library>.model.json`), its evaluated
`*_ABI_VERSION` macros, the functions the binary exports, and the
declarations narrowed to the binary's targets.

`stela gen embed --binary <name> --target <triple>... --out <dir>` writes
`<name>.stela.gz` and `<name>.stela.rc`, which compiles it with `rc.exe`; the
binary links the `.res`. The binary's entry in the configuration's `binaries`
lists its libraries. `--extract` extracts just those libraries instead of
reading `--model`. The gzip has a fixed header (no time stamp or name), so the
same headers give the same bytes. Unchanged outputs aren't rewritten, so
their times don't move; `--stamp <file>` also writes `<file>` on every run,
for a build system that would otherwise rerun the command until its outputs
are newer than its inputs (the v8jsi gyp action declares only the stamp).

Reading never loads the binary: Stela parses the PE file with bounds checks,
caps the resource and the inflated manifest, and validates the document
against the schema before using it. The commands that read manifests:

- `stela show <binary>` prints the API as declarations; `--json` prints the
  manifest.
- `stela verify <binary>...` checks the export table. Every function a
  manifest lists as exported must be exported. An export not listed is a
  failure if `otherExports` is `false` or if it starts with a library's
  `<prefix>_`; other exports (C++ symbols, other APIs) are allowed when
  `otherExports` is `true`.
- `stela check <binary> --config <file>` extracts the binary's libraries for
  its targets and diffs them (base) against the manifest (head), so a binary
  built from older or different headers shows as breaks or additions.
  `--headers <header>...` uses those headers instead of a configuration; each
  header is matched to a library by its include spelling, then by its file
  name.
- `stela diff` accepts a binary for `--base` or `--head`. The model on the
  other side is cut to the binary's libraries and targets first; two binaries
  compare the libraries and targets they share, and fail if they share none.

## The model (`stela.model/1`)

`stela extract` runs the extractor for every library on each of its targets,
in parallel, and merges the results. Facts that must agree on every target
(types, fields, enumerators, documentation, annotations) appear once; sizes,
alignments, offsets, and enumerator values go under `perTarget`. A declaration
missing on some targets lists the ones it has in `targets`. Any other
difference between targets is an error: `extract` prints the first differing
path for each declaration, exits 1, and writes nothing.

Declarations keep source order. Keys have a fixed order and the JSON is
compact (a container that fits in 100 columns stays on one line), so the same
headers always give the same bytes. Opaque records carry `incomplete: true`
and no layout; a record defined without a tag carries `tagless: true`. A field
whose type has neither a tag nor a typedef name (`union { … } data;`) carries
that type inline, as `{"union": {"fields": [...]}}`, so no absolute path from
Clang's spelling reaches the model.
[`schema/stela.model.schema.json`](schema/stela.model.schema.json) describes
the format.

## Generators

`stela gen <generator> --out <dir>` writes one directory per library:

- `header-tests`: `<header>.header.c` and `.cpp` per entry header, and per
  generated `<lib>_stela.h`. Each defines the library's defines and includes
  its header alone, twice, to check the include guard. The stela-header tests
  prove the file stands alone, without the `<stdint.h>` and similar includes
  an entry header puts before it.
- `layout-tests`: `layout.c` and `layout.cpp`. Static asserts of each record's
  size, alignment, and field offsets (except bit-fields and anonymous members),
  each object typedef's size and alignment, and each enumerator's value. Layout
  checks are grouped by target under the compiler's architecture macros.

Both compile with clang (`-ffreestanding -nostdlibinc`, warnings as errors)
and MSVC (`/W4 /WX`) for each target, as C and C++. Header tests keep the
C99 floor; layout tests need C11 for `_Static_assert`. v8-jsi runs both in
`scripts/header-check.ts`.

## Configuration (`stela.config.json`)

```json
{
  "schema": "stela.config/1",
  "libraries": [
    {
      "name": "v8host",
      "prefix": "v8host",
      "macroPrefix": "V8HOST",
      "headers": ["src/public/v8host_client.h"],
      "stelaHeader": "src/public/v8host_stela.h"
    }
  ]
}
```

- `stelaHeader` defaults to `<macroPrefix in lowercase>_stela.h` next to the
  first header. An extension library sets it to `false` and uses its base
  library's file.
- `stelaNames` gives one annotation a longer name if it clashes with a library
  macro that can't change: `{ "DEPRECATED": "V8HOST_STELA_DEPRECATED" }`.
- `status` names the library's status type (default `<prefix>_status`).
- `targets` (top level) lists every target the model covers, as clang
  triples; a library's own `targets` narrows it to a subset, in the same
  order.
- `includeDirs` lists include directories as a consumer sets them (the
  generated tests spell each `#include` relative to the first one that holds
  the header); `owns` lists the files or directories whose declarations belong
  to the library (default: the entry headers); `defines` are `NAME` or
  `NAME=VALUE`.
- `lint` sets rule severities (`rules`) and intentional integer macros
  (`macroConstants`).
- `diff.baselines` maps library names to baseline model files, and
  `diff.experimental` lists headers whose breaks `diff` reports as notes.
- `binaries` lists the binaries that embed a manifest: `name` (a `.dll` or
  `.exe` file name), `libraries`, `exports` (`all`, the default, or `none`
  for a binary that implements no functions, such as an executable), and
  `otherExports` (default `true`; `false` makes `verify` reject any export
  the manifest doesn't list).
- [`schema/stela.config.schema.json`](schema/stela.config.schema.json)
  describes the format.

## Tests

```powershell
cd tools\stela
npm test            # node --test: unit, CLI, and compile-matrix tests
npm run typecheck   # needs `npm install` once
```

The compile matrix generates two libraries' files (one from a newer template
version) and compiles them alone and together, in both include orders:

- clang 9 language modes (C99–C23, C++11–C++23) × 6 targets (Windows x64,
  x86, ARM64, ARM64EC; Linux x64, ARM64), with
  `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wdocumentation -Werror`;
- MSVC 8 language modes × x64, x86, and ARM64, with `/W4 /WX`;
- before and after `<windows.h>`;
- extractor mode (`-DSTELA_EXTRACT`), where every annotation must reach the
  AST, and default mode, where none may.

Without Visual Studio the compile-matrix, extractor, and embedded-manifest
binary tests are skipped. The binary tests build small DLLs and executables
with MSVC, embed manifests in them, and run `show`, `verify`, `check`, and
`diff` against them.

## License

MIT. Each generated file carries the same notice.
