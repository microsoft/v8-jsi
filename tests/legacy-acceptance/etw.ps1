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
$providers = "$trace.providers.txt"
[IO.File]::WriteAllLines($providers, @(
  '{85A99459-1F1D-49BD-B3DC-E5EF2AD0C2C8} 0xffffffffffffffff 5',
  '{57277741-3638-4A4B-BDBA-0AC6E45DA56C} 0x1 5'
))
try {
  & logman.exe start $name -ets -o $trace -pf $providers
  if ($LASTEXITCODE) { throw 'ETW session creation failed' }
} finally { Remove-Item -LiteralPath $providers }
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
[xml]$events = Get-Content "$trace.xml" -Raw
$engineEvents = $events.SelectNodes("//*[local-name()='Event']") | Where-Object {
  $_.OuterXml -match '(?i)57277741-3638-4a4b-bdba-0ac6e45da56c' -and
  $_.OuterXml -match 'MethodLoad|SourceLoad'
}
if (!$engineEvents) {
  throw 'No V8.js MethodLoad or SourceLoad events were captured; engine ETW flags may be inactive.'
}
Write-Host "PASS: runtime JIT/GC and V8.js MethodLoad/SourceLoad events captured in $trace"
