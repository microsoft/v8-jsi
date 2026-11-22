# Copyright (c) Microsoft Corporation.
# Licensed under the MIT license.
param(
  [ValidateSet('Release', 'Debug')][string]$Configuration = 'Release',
  [string]$TraceFile = 'out\legacy\runtime.etl'
)
$ErrorActionPreference = 'Stop'
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (!$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
  throw 'ETW capture requires an elevated PowerShell console.'
}
$exe = Join-Path $PSScriptRoot "..\..\deps\nodejs\out\$Configuration\jsitests_legacy.exe"
$trace = [IO.Path]::GetFullPath($TraceFile)
New-Item -ItemType Directory ([IO.Path]::GetDirectoryName($trace)) -Force | Out-Null
$name = "V8JsiRuntime-$PID"
& logman.exe start $name -ets -o $trace -p '{85A99459-1F1D-49BD-B3DC-E5EF2AD0C2C8}' 0xffffffffffffffff 5
if ($LASTEXITCODE) { throw 'ETW session creation failed' }
try {
  & $exe '--gtest_filter=LegacyInstrumentation.*' '--gtest_brief=1'
  if ($LASTEXITCODE) { throw 'Instrumented runtime test failed' }
} finally {
  & logman.exe stop $name -ets
  if ($LASTEXITCODE) { throw 'ETW session stop failed' }
}
& tracerpt.exe $trace -o "$trace.xml" -of XML -y
if ($LASTEXITCODE) { throw 'ETW trace decoding failed' }
if (!(Select-String -Path "$trace.xml" -Pattern 'V8::JIT|GCPrologue|GCEpilogue')) {
  throw 'No runtime JIT or GC events were captured; inspect the ETL in WPA.'
}
Write-Host "PASS: runtime JIT/GC events captured in $trace"
