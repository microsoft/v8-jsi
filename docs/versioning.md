# Versioning policy

The `Microsoft.JavaScript.V8` NuGet uses a Node.js-aligned
`MAJOR.MINOR.PATCH` scheme distinct from the legacy
`ReactNative.V8Jsi.Windows` scheme (which tracks React Native Windows
releases).

## Format

`MAJOR.MINOR.PATCH` — currently `24.x.y`.

| Component | Meaning | Bump trigger |
|---|---|---|
| `MAJOR` | Node.js major LTS the build is on. | v8-jsi switches to a new Node.js major (e.g. v24 → v26). |
| `MINOR` | API / Node-minor checkpoint. | (a) v8-jsi-side API change (JSI / JSI-ABI / public C surface), or (b) Node.js minor-version sync (v24.M → v24.M+1). |
| `PATCH` | Node.js patch sync or bug-fix-only release. | Node.js patch-version sync without API change, or a v8-jsi bug-fix release on the current Node.js minor. |

## Fields in `config.json`

- `v8jsi_version` — the `MAJOR.MINOR.PATCH` value consumed by
  `scripts/build.ts` and the `Microsoft.JavaScript.V8.nuspec` template.
- `nodejs_version` — the upstream Node.js version that `deps/nodejs/`
  is currently synced to. Authoritative source of the actual vendored
  bits remains `deps/nodejs/sync-config.json` (commit + branch);
  `nodejs_version` mirrors it as a human-readable label.

## Sync check

`scripts/build.ts` reads `NODE_MAJOR_VERSION` / `NODE_MINOR_VERSION` /
`NODE_PATCH_VERSION` from `deps/nodejs/src/node_version.h` and compares
the assembled version string against `config.json.nodejs_version`. A
mismatch fails the build with a clear message: re-run the fork-sync to
pick up the new Node.js bits, then update `nodejs_version` in
`config.json` (and bump `v8jsi_version` per the table above).
