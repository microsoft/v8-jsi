# Copyright (c) Microsoft Corporation.
# Licensed under the MIT license.
param(
    [Parameter(Mandatory)][string] $RepoRoot,
    [Parameter(Mandatory)][string] $Destination,
    [Parameter(Mandatory)][string] $Archive,
    [ValidateSet('Release', 'Debug')][string] $Configuration = 'Release',
    [switch] $Sandbox,
    [switch] $Legacy
)

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
function Copy-Required([string] $Source, [string] $Target) {
    if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) { throw "Required CET payload missing: $Source" }
    New-Item -ItemType Directory -Force (Split-Path $Target) | Out-Null
    Copy-Item -LiteralPath $Source -Destination $Target
}

if ($MyInvocation.InvocationName -ne '.') {
    if ($Sandbox -and $Legacy) { throw 'Sandbox and legacy payloads must be isolated' }
    if ((Test-Path $Destination) -and (Get-ChildItem $Destination -Force)) { throw 'CET staging directory must be empty' }
    $out = Join-Path $RepoRoot "deps/nodejs/out/$Configuration"
    if ($Legacy) {
        foreach ($file in 'cetrun_legacy.exe', 'v8jsi_legacy_startup_tests.exe') {
            Copy-Required "$out/$file" "$Destination/legacy/$Configuration/$file"
        }
    } else {
        # Resolve once: images can have more than one Node application on PATH.
        $node = (Get-Command node -CommandType Application | Select-Object -First 1).Source
        $version = & $node -p 'process.versions.node'
        if ($LASTEXITCODE -ne 0 -or $version -notmatch '^24\.') { throw "Node 24.x required; found $version" }
        Copy-Required $node "$Destination/tooling/node.exe"
        $dst = "$Destination/deps/nodejs/out/$Configuration"
        $engine = if ($Sandbox) { 'v8jsi2sb.dll' } else { 'v8jsi2.dll' }
        foreach ($file in $engine, 'v8jsi_test.exe', 'node_api_tests.exe', 'node_lite.exe', 'cetrun.exe') {
            Copy-Required "$out/$file" "$dst/$file"
        }
        # Derive the complete addon and fixture inventories from the build's inputs.
        $source = "$RepoRoot/src/node-api/test"
        $gyp = Get-Content "$source/node_api_tests.gyp" -Raw
        $addons = [regex]::Matches($gyp, "'target_name': '([^']+)',\s*'type': 'loadable_module'")
        $fixtures = @(Get-ChildItem $source -Recurse -File -Filter '*.js')
        if (-not $addons.Count -or -not $fixtures.Count) { throw 'Empty Node-API test inventory' }
        foreach ($addon in $addons) {
            $file = $addon.Groups[1].Value + '.dll'
            Copy-Required "$out/$file" "$dst/$file"
        }
        foreach ($fixture in $fixtures) {
            $relative = [IO.Path]::GetRelativePath($source, $fixture.FullName)
            Copy-Required "$out/test-js-files/$relative" "$dst/test-js-files/$relative"
        }
        if ($Sandbox) {
            # Use the tested developer payload, not the signed release-trust layout.
            $relative = 'deps/chromium/out/sandbox-x64'
            foreach ($file in 'sbox.exe', 'v8sbhost.dll', 'v8sbhost_broker_tests.exe',
                'v8jsi2.dll', 'v8jsi2sb.dll', 'msvcp140.dll', 'msvcp140_atomic_wait.dll',
                'vcruntime140.dll', 'vcruntime140_1.dll') {
                Copy-Required "$RepoRoot/$relative/$file" "$Destination/$relative/$file"
            }
        }
    }
    New-Item -ItemType Directory -Force (Split-Path $Archive) | Out-Null
    & tar.exe -cf $Archive -C $Destination .
    if ($LASTEXITCODE -ne 0) { throw "CET tar failed (exit $LASTEXITCODE)" }
    $bytes = (Get-ChildItem $Destination -Recurse -File | Measure-Object Length -Sum).Sum
    Write-Output "CET payload: $bytes bytes; archive: $((Get-Item $Archive).Length) bytes"
}
