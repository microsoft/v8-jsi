# Copyright (c) Microsoft Corporation.
# Licensed under the MIT license.
param(
  [Parameter(Mandatory)][string]$PkgDir,
  [Parameter(Mandatory)][string]$BaselinePackageDir,
  [ValidateSet('x64', 'x86', 'arm64')][string]$Platform = 'x64',
  [ValidateSet('Release', 'Debug')][string]$Configuration = 'Release',
  [string]$ModernDll,
  [string]$WorkDir = 'out\legacy\acceptance'
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$hostArch = [Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString().ToLowerInvariant()
if ($Platform -eq 'arm64' -and $hostArch -ne 'arm64') { throw 'ARM64 acceptance must run natively on an ARM64 host.' }
$PkgDir = (Resolve-Path $PkgDir).Path
$baseline = Get-ChildItem $BaselinePackageDir -Filter '*0.79.5.nupkg' | Select-Object -First 1
$candidate = Get-ChildItem $PkgDir -Filter 'ReactNative.V8Jsi.Windows.*.nupkg' | Select-Object -First 1
if (!$baseline -or !$candidate) { throw 'Both baseline and candidate packages are required' }
$version = $candidate.Name -replace '^ReactNative.V8Jsi.Windows\.', '' -replace '\.nupkg$', ''
if (!$ModernDll) { $ModernDll = "$PSScriptRoot\..\..\deps\nodejs\out\$Configuration\v8jsi2.dll" }
if (!(Test-Path -LiteralPath $ModernDll -PathType Leaf)) {
  throw 'Modern DLL not found. Supply -ModernDll with a v8jsi2.dll built for the target architecture.'
}
$ModernDll = (Resolve-Path -LiteralPath $ModernDll).Path
$pe = [IO.File]::ReadAllBytes($ModernDll)
$machine = 0
if ($pe.Length -ge 64 -and [BitConverter]::ToUInt16($pe, 0) -eq 0x5a4d) {
  $offset = [BitConverter]::ToInt32($pe, 60)
  if ($offset -ge 0 -and $offset -le $pe.Length - 6 -and [BitConverter]::ToUInt32($pe, $offset) -eq 0x4550) {
    $machine = [BitConverter]::ToUInt16($pe, $offset + 4)
  }
}
$expectedMachine = switch ($Platform) { 'x86' { 0x14c } 'arm64' { 0xaa64 } default { 0x8664 } }
if ($machine -ne $expectedMachine) {
  throw ('Modern DLL PE machine 0x{0:x} does not match {1}. Supply -ModernDll with a matching v8jsi2.dll.' -f $machine, $Platform)
}
$work = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($WorkDir)
if (Test-Path $work) { throw 'Acceptance work directory must not already exist' }
New-Item -ItemType Directory $work | Out-Null
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$requiredTools = if ($hostArch -eq 'arm64') { 'Microsoft.VisualStudio.Component.VC.Tools.ARM64' } else { 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64' }
$vs = & $vswhere -latest -products '*' -requires $requiredTools -property installationPath
if (!$vs) { throw "Visual Studio with $requiredTools is required" }
$msbuild = Join-Path $vs 'MSBuild\Current\Bin\MSBuild.exe'
$toolsetArguments = if (Test-Path (Join-Path $vs 'VC\Auxiliary\Build\Microsoft.VCToolsVersion.v143.default.txt')) { @('/p:PlatformToolset=v143') } else { @() }
$msplatform = switch ($Platform) { 'x86' { 'Win32' } 'arm64' { 'ARM64' } default { 'x64' } }
function Invoke-Checked([string]$Exe, [string[]]$Arguments) {
  & $Exe @Arguments | Out-Host
  if ($LASTEXITCODE) { throw "$Exe failed: $LASTEXITCODE" }
}
function Run-Consumer([string]$Exe, [string[]]$Arguments, [string]$ExpectedFailureMessage) {
  $options = [Diagnostics.ProcessStartInfo]::new($Exe)
  $options.UseShellExecute = $false
  $options.RedirectStandardOutput = $true
  $options.RedirectStandardError = $true
  foreach ($argument in $Arguments) { $options.ArgumentList.Add($argument) }
  $child = [Diagnostics.Process]::new()
  $child.StartInfo = $options
  try {
    if (!$child.Start()) { throw 'Consumer did not start' }
    $stdout = $child.StandardOutput.ReadToEndAsync()
    $stderr = $child.StandardError.ReadToEndAsync()
    $exited = $child.WaitForExit(60000)
    if (!$exited) { Stop-Process -Id $child.Id; $child.WaitForExit() }
    $output = $stdout.GetAwaiter().GetResult()
    $errors = $stderr.GetAwaiter().GetResult()
    Write-Host $output -NoNewline
    Write-Host $errors -NoNewline
    if (!$exited) { throw 'Consumer timed out' }
    if ($ExpectedFailureMessage) {
      if ($child.ExitCode -ne 1 -or !($errors -split '\r?\n' -contains $ExpectedFailureMessage)) {
        throw "Negative control did not report '$ExpectedFailureMessage' (exit $($child.ExitCode))"
      }
      Write-Host "PASS: expected control failure for $Arguments`: $ExpectedFailureMessage"
    } elseif ($child.ExitCode) { throw "Consumer failed: $($child.ExitCode)" }
  } finally { $child.Dispose() }
}
function Restore-Package([string]$Name, [string]$Version, [string]$Package) {
  $dir = Join-Path $work $Name
  New-Item -ItemType Directory $dir, "$dir\feed" | Out-Null
  Copy-Item $Package "$dir\feed"
  [IO.File]::WriteAllText("$dir\NuGet.Config", '<configuration><packageSources><clear/><add key="local" value="feed"/></packageSources></configuration>')
  [IO.File]::WriteAllText("$dir\packages.config", "<packages><package id=`"ReactNative.V8Jsi.Windows`" version=`"$Version`" /></packages>")
  Invoke-Checked 'nuget.exe' @('restore', "$dir\packages.config", '-ConfigFile', "$dir\NuGet.Config", '-PackagesDirectory', "$dir\packages", '-NoHttpCache', '-NonInteractive')
  return "$dir\packages\ReactNative.V8Jsi.Windows.$Version"
}
function Build-Consumer([string]$PackageRoot, [string]$Output) {
  Invoke-Checked $msbuild (@("$PSScriptRoot\consumer.vcxproj", '/t:Rebuild', '/m', '/v:minimal', "/p:Configuration=$Configuration", "/p:Platform=$msplatform", "/p:PackageRoot=$PackageRoot", "/p:OutDir=$Output\") + $toolsetArguments)
}
$old = Restore-Package 'baseline' '0.79.5' $baseline.FullName
$new = Restore-Package 'candidate' $version $candidate.FullName
$toolset = & $msbuild "$PSScriptRoot\consumer.vcxproj" '/getProperty:PlatformToolset' "/p:PackageRoot=$old" "/p:Configuration=$Configuration" "/p:Platform=$msplatform" @toolsetArguments
if ($LASTEXITCODE) { throw "Toolset evaluation failed: $LASTEXITCODE" }
Write-Host "Acceptance PlatformToolset: $toolset"
$dropin = "$work\dropin"
Build-Consumer $old $dropin
Run-Consumer "$dropin\consumer.exe" @()
$cache = "$work\v8-12.1.cache"
Run-Consumer "$dropin\consumer.exe" @('--produce-cache', $cache)
Run-Consumer "$dropin\consumer.exe" @('--accept-cache', $cache)
Run-Consumer "$dropin\consumer.exe" @('--finalizer-drain') -ExpectedFailureMessage 'teardown did not drain pending finalizer'
# The candidate proves queued inspector dispatch is safe after teardown; 0.79.5 disconnect does not queue it.
$hash = (Get-FileHash "$dropin\consumer.exe").Hash
Copy-Item "$new\lib\win32\$Configuration\$Platform\v8jsi.dll" "$dropin\v8jsi.dll" -Force
if ((Get-FileHash "$dropin\consumer.exe").Hash -ne $hash) { throw 'DLL swap changed the consumer executable' }
Run-Consumer "$dropin\consumer.exe" @()
Run-Consumer "$dropin\consumer.exe" @('--reject-cache', $cache)
Run-Consumer "$dropin\consumer.exe" @('--finalizer-drain')
Run-Consumer "$dropin\consumer.exe" @('--late-tasks')
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvarsall.bat'
$arch = if ($hostArch -eq 'arm64') {
  switch ($Platform) { 'x86' { 'arm64_x86' } 'arm64' { 'arm64' } default { 'arm64_x64' } }
} else {
  switch ($Platform) { 'x86' { 'x64_x86' } 'arm64' { 'x64_arm64' } default { 'x64' } }
}
Invoke-Checked $env:ComSpec @('/c', "call `"$vcvars`" $arch && cl /nologo /std:c++17 /EHsc /W4 /WX /I`"$PSScriptRoot\..\..\src`" /I`"$PSScriptRoot\..\..\src\public`" /I`"$PSScriptRoot\..\..\src\jsi_abi`" `"$PSScriptRoot\modern-cache.cpp`" /Fo`"$work\modern-cache.obj`" /Fe`"$work\modern-cache.exe`"")
Run-Consumer "$work\modern-cache.exe" @($ModernDll, "$work\modern-same-source.cache", 'same-source')
Run-Consumer "$dropin\consumer.exe" @('--same-source-cache', "$work\modern-same-source.cache")
Run-Consumer "$work\modern-cache.exe" @($ModernDll, "$work\modern-flag-mismatch.cache", 'flag-mismatch')
Run-Consumer "$dropin\consumer.exe" @('--reject-cache', "$work\modern-flag-mismatch.cache")
$rebuilt = "$work\rebuilt"
Build-Consumer $new $rebuilt
Run-Consumer "$rebuilt\consumer.exe" @()
Run-Consumer "$rebuilt\consumer.exe" @('--reject-cache', $cache)
Run-Consumer "$rebuilt\consumer.exe" @('--reject-cache', "$work\modern-flag-mismatch.cache")
Run-Consumer "$rebuilt\consumer.exe" @('--same-source-cache', "$work\modern-same-source.cache")
Run-Consumer "$rebuilt\consumer.exe" @('--finalizer-drain')
Run-Consumer "$rebuilt\consumer.exe" @('--late-tasks')
Write-Host "PASS: 0.79.5 control, DLL-only swap (EXE SHA256 $hash), and candidate rebuild"

$coexistence = "$work\coexistence"
Invoke-Checked $msbuild (@("$PSScriptRoot\consumer.vcxproj", '/t:Rebuild', '/m', '/v:minimal', "/p:Configuration=$Configuration", "/p:Platform=$msplatform", "/p:PackageRoot=$new", "/p:OutDir=$coexistence\", '/p:Coexistence=true') + $toolsetArguments)
Copy-Item -LiteralPath $ModernDll -Destination "$coexistence\v8jsi2.dll"
foreach ($loadOrder in @('legacy-first', 'modern-first')) {
  foreach ($destroyOrder in @('legacy-first', 'modern-first')) {
    Run-Consumer "$coexistence\coexistence.exe" @($loadOrder, $destroyOrder)
  }
}
