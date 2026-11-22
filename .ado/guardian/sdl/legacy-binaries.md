# Legacy binary findings

Legacy cells stage `out/pkg-staging/lib/win32/<Configuration>/<arch>/v8jsi.dll`
under the build staging directory. The existing BinSkim target includes all six
cells; do not exclude them. Full PDBs and the matching signed DLL are retained in
`<Name>-legacy` under `symbols/<arch>-<Configuration>`. The script's SARIF is in
`diagnostics/v8jsi.binskim.sarif` for comparison with Guardian's scan.

Add `.gdnsuppress` entries only from Guardian's observed signatures, with the
exact staged target, rule, architecture, configuration, and technical justification.
Do not derive signatures from another binary or use wildcards. Local BinSkim SARIF
is not a Guardian suppression signature. Leave unexpected findings failing.

Expected residuals to confirm individually include ARM64 BA2014 (V8's compiler-
generated buffer-less constructor closures) and x86/ARM64 BA2025 (CET shadow stacks
are x64-only). A finding on x64 CET must not be suppressed. Other rule findings
need their own observed evidence and review; existing modern suppressions and
baselines do not authorize suppressing a legacy image.
