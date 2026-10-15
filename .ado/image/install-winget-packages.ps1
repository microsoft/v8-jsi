# Installs WinGet packages machine-wide during a CloudTest managed-image build.
# Embedded base64-encoded as the "Script" of the windows-powershell-invokecommand
# artifact in windows-2025-1espt-arm64.json; see README.md, "WinGet during image
# builds", for why each step is needed and how to regenerate the JSON entry.
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

# Package id -> extra "winget install" arguments.
$Packages = [ordered]@{
    '7zip.7zip'          = ''
    # Native Python inside the Node.js configure range (3.9-3.14). The floating
    # windows-install-python release on PATH can be newer than configure
    # accepts, and the tool-cache Python 3.13 is x64 (emulated), which makes
    # configure see an x64 host. scripts/build.ts finds this copy through PEP 514.
    # The manifest's machine installer sets PrependPath=1, so override it: keep
    # this copy off PATH and leave the py launcher and file associations to
    # windows-install-python.
    'Python.Python.3.14' = '--architecture arm64 --override "/quiet InstallAllUsers=1 PrependPath=0 Include_launcher=0 AssociateFiles=0 Include_test=0"'
}
$FamilyName = 'Microsoft.DesktopAppInstaller_8wekyb3d8bbwe'
$SourceMsixUrls = @(
    'https://cdn.winget.microsoft.com/cache/source.msix',
    'https://cdn.winget.microsoft.com/cache/source2.msix'
)

function Invoke-WinGet([string] $Arguments, [int] $TimeoutSec = 900) {
    $out = [IO.Path]::GetTempFileName()
    $err = [IO.Path]::GetTempFileName()
    try {
        $p = Start-Process -FilePath $WinGet -ArgumentList $Arguments -NoNewWindow -PassThru `
            -RedirectStandardOutput $out -RedirectStandardError $err
        $null = $p.Handle
        if (-not $p.WaitForExit($TimeoutSec * 1000)) {
            $p.Kill()
            throw "winget $Arguments timed out after $TimeoutSec s"
        }
        $text = "$(Get-Content $out -Raw)$(Get-Content $err -Raw)" -replace '[^\x20-\x7E\r\n]', ''
        Write-Host ("winget {0} -> 0x{1:X8}`n{2}" -f $Arguments, $p.ExitCode, $text.Trim())
        return $p.ExitCode
    } finally {
        Remove-Item $out, $err -Force -ErrorAction SilentlyContinue
    }
}

function Test-Catalog {
    (Invoke-WinGet "show --id $(@($Packages.Keys)[0]) --exact --source winget --accept-source-agreements --disable-interactivity" 300) -eq 0
}

# 1. The build user (a service logon in session 0) has winget provisioned but not
#    registered, and Windows refuses to start winget.exe for an unregistered user.
#    Registering the provisioned package creates the WindowsApps alias.
Add-AppxPackage -RegisterByFamilyName -MainPackage $FamilyName
$WinGet = Join-Path $env:LOCALAPPDATA 'Microsoft\WindowsApps\winget.exe'
for ($i = 0; $i -lt 30 -and -not (Test-Path $WinGet); $i++) { Start-Sleep -Seconds 1 }
if (-not (Test-Path $WinGet)) { throw "WinGet alias $WinGet not created after registering $FamilyName" }
$null = Invoke-WinGet '--version' 60

# 2. In this session winget cannot deploy its own catalog package
#    (0x8A15000F, source data missing), but Add-AppxPackage can. The package is
#    signed by Microsoft and verified by the deployment stack.
if (-not (Test-Catalog)) {
    $catalogReady = $false
    foreach ($url in $SourceMsixUrls) {
        $msix = Join-Path $env:TEMP ([IO.Path]::GetFileName($url))
        try {
            Invoke-WebRequest $url -OutFile $msix -UseBasicParsing
            Add-AppxPackage -Path $msix
            Write-Host "Deployed WinGet catalog package $url"
        } catch {
            Write-Host "Could not deploy WinGet catalog package ${url}: $($_.Exception.Message)"
        } finally {
            Remove-Item $msix -Force -ErrorAction SilentlyContinue
        }
        if (Test-Catalog) { $catalogReady = $true; break }
    }
    if (-not $catalogReady) { throw 'WinGet catalog is unavailable after deploying its source package' }
}

# 3. winget selects the installer for the real OS architecture and verifies its
#    SHA256 against the community manifest.
foreach ($id in $Packages.Keys) {
    $code = -1
    for ($attempt = 1; $attempt -le 3 -and $code -ne 0; $attempt++) {
        $code = Invoke-WinGet "install --id $id --exact --source winget --scope machine --silent --accept-package-agreements --accept-source-agreements --disable-interactivity $($Packages[$id])"
        if ($code -ne 0) { Start-Sleep -Seconds (15 * $attempt) }
    }
    if ($code -ne 0) { throw ('winget install {0} failed with 0x{1:X8}' -f $id, $code) }
}

$sevenZip = Join-Path $env:ProgramFiles '7-Zip\7z.exe'
if (-not (Test-Path $sevenZip)) { throw "7-Zip install reported success but $sevenZip is missing" }
& $sevenZip i | Select-Object -First 3 | ForEach-Object { Write-Host $_ }

# PowerShell is emulated x64 here, but x64 shares the native 64-bit registry
# view on ARM64, so the arm64 Python's HKLM PEP 514 key is visible.
$pyKey = Get-ChildItem 'HKLM:\SOFTWARE\Python\PythonCore' -ErrorAction SilentlyContinue |
    Where-Object PSChildName -like '3.14*' | Select-Object -First 1
if (-not $pyKey) { throw 'Python 3.14 install reported success but is not registered under HKLM\SOFTWARE\Python\PythonCore' }
$python = Join-Path (Get-Item "$($pyKey.PSPath)\InstallPath").GetValue('') 'python.exe'
$pyInfo = & $python -c 'import sys, sysconfig; print(sys.version.split()[0], sysconfig.get_platform())'
Write-Host "Python: $python -> $pyInfo"
if ($pyInfo -notmatch '^3\.14\.\d+ win-arm64$') { throw "Expected native arm64 Python 3.14, got: $pyInfo" }

# 4. Sysprep fails (0x80073cf2) on per-user packages that are not provisioned, so
#    remove the catalog packages. Keep the DesktopAppInstaller registration: it is
#    the provisioned version, and ImageFactory's PrepareSysprep is told not to
#    remove it (removing it on Server 2025 leaves sysprep hanging).
Get-AppxPackage -Name 'Microsoft.Winget.*' | ForEach-Object {
    Remove-AppxPackage -Package $_.PackageFullName
    Write-Host "Removed per-user $($_.PackageFullName)"
}
$flagKey = 'HKLM:\SOFTWARE\ImageFactory'
if (-not (Test-Path $flagKey)) { New-Item -Path $flagKey -Force | Out-Null }
New-ItemProperty -Path $flagKey -Name 'SkipDesktopAppInstallerRemoval' -Value 1 -PropertyType DWord -Force | Out-Null

$leftovers = @(Get-AppxPackage -Name 'Microsoft.Winget.*')
if ($leftovers.Count) { throw "Per-user packages would break sysprep: $(($leftovers | ForEach-Object { $_.PackageFullName }) -join ', ')" }
Write-Host "Installed $(@($Packages.Keys) -join ', ') with WinGet"
