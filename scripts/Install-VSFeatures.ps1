param (
	[Parameter(Mandatory=$true)]
	[string[]] $Components,
	[uri] $InstallerUri = "https://aka.ms/vs/16/release/vs_enterprise.exe",
	[string] $VsInstaller = "${env:System_DefaultWorkingDirectory}\vs_Enterprise.exe",
	[string] $VsInstallOutputDir = "${env:System_DefaultWorkingDirectory}\vs",
	[System.IO.FileInfo] $VsInstallPath = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2019\Enterprise",
	[System.IO.FileInfo] $VsInstallerPath = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer",
	[switch] $Collect = $false,
	[switch] $Cleanup = $false,
	[switch] $UseWebInstaller = $false,
	[string] $OutputPath = "$PSScriptRoot\out"
)

$Components | ForEach-Object {
	$componentList += '--add', $_
}

$LocalVsInstaller = "$VsInstallerPath\vs_installershell.exe"

$UseWebInstaller = $UseWebInstaller -or -not (Test-Path -Path "$LocalVsInstaller")

if ($UseWebInstaller) {
	Write-Host "Downloading web installer..."

	Invoke-WebRequest -Method Get `
		-Uri $InstallerUri `
		-OutFile $VsInstaller

	New-Item -ItemType directory -Path $VsInstallOutputDir

	Write-Host "Running web installer to download requested components..."

	Start-Process `
		-FilePath "$VsInstaller" `
		-ArgumentList ( `
			'--layout', "$VsInstallOutputDir",
			'--wait',
			'--norestart',
			'--quiet' + `
			$componentList
		) `
		-Wait `
		-PassThru

	Write-Host "Running downloaded VS installer to add requested components..."

	Start-Process `
		-FilePath "$VsInstallOutputDir\vs_Enterprise.exe" `
		-ArgumentList (
			'modify',
			'--installPath', "`"$VsInstallPath`"" ,
			'--wait',
			'--norestart',
			'--quiet' + `
			$componentList
		) `
		-Wait `
		-PassThru `
		-OutVariable returnCode

	if ($Cleanup) {
		Write-Host "Cleaning up..."

		Remove-Item -Path $VsInstaller
		Remove-Item -Path $VsInstallOutputDir -Recurse
	}
	
} else {
	Write-Host "Downloading latest Bootstrapper to update local installer..."

	Invoke-WebRequest -Method Get -Uri $InstallerUri -OutFile $VsInstaller

	Start-Process `
		-FilePath "$VsInstaller" `
		-ArgumentList ( '--update', '--wait', '--quiet' ) `
		-Wait `
		-PassThru

	Write-Host "Running local installer to add requested components..."

	Start-Process `
		-FilePath "$LocalVsInstaller" `
		-ArgumentList (
			'modify',
			'--installPath', "`"$VsInstallPath`"" ,
			'--norestart',
			'--quiet' + `
			$componentList
		) `
		-Wait `
		-OutVariable returnCode
}

if ($Collect) {
	Invoke-WebRequest -Method Get `
		-Uri 'https://download.microsoft.com/download/8/3/4/834E83F6-C377-4DCE-A757-69A418B6C6DF/Collect.exe' `
		-OutFile ${env:System_DefaultWorkingDirectory}\Collect.exe

	# Should generate ${env:Temp}\vslogs.zip
	Start-Process `
		-FilePath "${env:System_DefaultWorkingDirectory}\Collect.exe" `
		-Wait `
		-PassThru

	New-Item -ItemType Directory -Force ${env:System_DefaultWorkingDirectory}\vslogs
	Expand-Archive -Path ${env:TEMP}\vslogs.zip -DestinationPath ${env:System_DefaultWorkingDirectory}\vslogs\
	Copy-Item -Path ${env:TEMP}\vslogs.zip -Destination $OutputPath

	Write-Host "VC versions after installation:"
	Get-ChildItem -Name "$VsInstallPath\VC\Tools\MSVC\"
}
