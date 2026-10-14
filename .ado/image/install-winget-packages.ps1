# Installs WinGet packages machine-wide during a CloudTest managed-image build.
# Embedded base64-encoded as the "Script" of the windows-powershell-invokecommand
# artifact in windows-2025-1espt-arm64.json; see README.md, "WinGet during image
# builds", for why each step is needed and how to regenerate the JSON entry.
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$Packages = @('7zip.7zip')
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
    (Invoke-WinGet "show --id $($Packages[0]) --exact --source winget --accept-source-agreements --disable-interactivity" 300) -eq 0
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
foreach ($id in $Packages) {
    $code = -1
    for ($attempt = 1; $attempt -le 3 -and $code -ne 0; $attempt++) {
        $code = Invoke-WinGet "install --id $id --exact --source winget --scope machine --silent --accept-package-agreements --accept-source-agreements --disable-interactivity"
        if ($code -ne 0) { Start-Sleep -Seconds (15 * $attempt) }
    }
    if ($code -ne 0) { throw ('winget install {0} failed with 0x{1:X8}' -f $id, $code) }
}

$sevenZip = Join-Path $env:ProgramFiles '7-Zip\7z.exe'
if (-not (Test-Path $sevenZip)) { throw "7-Zip install reported success but $sevenZip is missing" }
& $sevenZip i | Select-Object -First 3 | ForEach-Object { Write-Host $_ }

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
Write-Host "Installed $($Packages -join ', ') with WinGet"
