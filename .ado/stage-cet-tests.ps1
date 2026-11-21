param(
    [Parameter(Mandatory)][string] $RepoRoot,
    [Parameter(Mandatory)][string] $Destination,
    [ValidateSet('Release', 'Debug')][string] $Configuration = 'Release',
    [switch] $Sandbox
)

$ErrorActionPreference = 'Stop'
$out = Join-Path $RepoRoot "deps/nodejs/out/$Configuration"
$dst = Join-Path $Destination "deps/nodejs/out/$Configuration"
New-Item -ItemType Directory -Force $dst | Out-Null

function Copy-Required([string] $Source, [string] $Target) {
    if (-not (Test-Path -LiteralPath $Source)) { throw "Required CET payload missing: $Source" }
    Copy-Item -LiteralPath $Source -Destination $Target -Recurse -Force
}

$engine = if ($Sandbox) { 'v8jsi2sb.dll' } else { 'v8jsi2.dll' }
foreach ($file in @($engine, "$engine.pdb", 'v8jsi_test.exe', 'node_api_tests.exe', 'node_lite.exe', 'cetrun.exe')) {
    Copy-Required (Join-Path $out $file) $dst
}
foreach ($file in @('v8jsi_test.pdb', 'node_api_tests.pdb', 'node_lite.pdb', 'cetrun.pdb')) {
    Copy-Required (Join-Path $out $file) $dst
}
Copy-Required (Join-Path $out 'test-js-files') $dst
$addons = @(Get-ChildItem $out -File -Filter '*.dll' | Where-Object Name -notlike 'v8jsi2*')
if ($addons.Count -eq 0) { throw 'No Node-API addons found' }
$addons | Copy-Item -Destination $dst

# Carry the same Node runtime used by the build, without installing tools on the test host.
$tooling = Join-Path $Destination 'tooling'
$scripts = Join-Path $Destination 'scripts'
New-Item -ItemType Directory -Force $tooling, $scripts | Out-Null
$node = (Get-Command node -CommandType Application | Select-Object -First 1).Source
$version = & $node -p 'process.versions.node'
if ($LASTEXITCODE -ne 0 -or [version]$version -lt [version]'24.0.0') { throw "Node 24 required; found $version" }
Copy-Required $node (Join-Path $tooling 'node.exe')
Copy-Required (Join-Path $RepoRoot 'scripts/cetrun.test.ts') $scripts

if ($Sandbox) {
    $sandboxOut = Join-Path $RepoRoot 'deps/chromium/out/sandbox-x64'
    $sandboxDst = Join-Path $Destination 'deps/chromium/out/sandbox-x64'
    New-Item -ItemType Directory -Force $sandboxDst | Out-Null
    # Use the developer payload tested by the build, not the signed release-trust layout.
    foreach ($file in @('sbox.exe', 'v8sbhost.dll', 'v8sbhost_broker_tests.exe',
        'v8jsi2.dll', 'v8jsi2sb.dll', 'msvcp140.dll', 'msvcp140_atomic_wait.dll',
        'vcruntime140.dll', 'vcruntime140_1.dll')) {
        Copy-Required (Join-Path $sandboxOut $file) $sandboxDst
    }
    foreach ($file in @('sbox.exe.pdb', 'v8sbhost.dll.pdb', 'v8sbhost_broker_tests.exe.pdb')) {
        Copy-Required (Join-Path $sandboxOut $file) $sandboxDst
    }
}
