# Versioning policy

The `Microsoft.JavaScript.V8` NuGet uses a Node.js-aligned
`MAJOR.MINOR.PATCH` scheme distinct from the legacy
`ReactNative.V8Jsi.Windows` scheme (which tracks React Native Windows
releases).

## Format

`MAJOR.MINOR.PATCH` — currently `26.x.y`. This follows the vendored Node.js
sources, not the Node.js 24.x runtime used to run build tools.

| Component | Meaning | Bump trigger |
|---|---|---|
| `MAJOR` | Node.js major LTS the build is on. | v8-jsi switches to a new Node.js major (e.g. v24 → v26). |
| `MINOR` | API / Node-minor checkpoint. | (a) v8-jsi-side API change (JSI / JSI-ABI / public C surface), or (b) Node.js minor-version sync (v26.M → v26.M+1). |
| `PATCH` | Node.js patch sync or bug-fix-only release. | Node.js patch-version sync without API change, or a v8-jsi bug-fix release on the current Node.js minor. |

## Version floor and CI

The authoritative committed floor is
[`versions/Microsoft.JavaScript.V8.version`](../versions/Microsoft.JavaScript.V8.version).
Update that file when starting a new version line or raising the patch floor;
keep the local `config.json.v8jsi_version` value aligned with it.

[`scripts/compute-version.ts`](../scripts/compute-version.ts) reads the version
from the committed file, adds first-parent Git height to its patch floor, and
adds a `-g<sha>` suffix on non-release branches. A version edit takes effect
after it is committed.

CI runs `node scripts\compute-version.ts --write` and overlays the computed
`config.json` on build, pack, and tag jobs. Changing only
`config.json.v8jsi_version` therefore does not change the CI version line.
The computed configuration is a build artifact, not a new committed floor.

## Fields in `config.json`

- `v8jsi_version` — the `MAJOR.MINOR.PATCH` value consumed by
  `scripts/build.ts` and the `Microsoft.JavaScript.V8.nuspec` template.
  CI computes it from the committed version floor and Git history.
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
`config.json` (and bump the committed version floor and local
`v8jsi_version` per the table above).
