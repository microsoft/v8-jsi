# Copyright (c) Microsoft Corporation.
# Licensed under the MIT license.

<#
.SYNOPSIS
  Developer task runner for v8-jsi.

.DESCRIPTION
  A single entry point for common development tasks.
  Run without arguments or with -? to see available commands.

.EXAMPLE
  .\dev fork-sync --dep nodejs --status
#>

param(
    [Parameter(Position = 0)]
    [string]$Command,

    [Parameter(Position = 1, ValueFromRemainingArguments)]
    [string[]]$Args
)

$ErrorActionPreference = 'Stop'
$ScriptDir = $PSScriptRoot

# =============================================================================
# Helpers
# =============================================================================

function Ensure-ScriptDeps {
    $stamp = Join-Path $ScriptDir 'scripts\fork-sync\node_modules\.package-lock.json'
    $pkg = Join-Path $ScriptDir 'scripts\fork-sync\package.json'

    if (-not (Test-Path $stamp) -or
        (Get-Item $pkg).LastWriteTime -gt (Get-Item $stamp).LastWriteTime) {
        Write-Host 'Installing script dependencies...'
        Push-Location (Join-Path $ScriptDir 'scripts\fork-sync')
        try { npm install } finally { Pop-Location }
    }
}

function Show-Help {
    Write-Host ''
    Write-Host '  v8-jsi developer tasks'
    Write-Host '  ======================'
    Write-Host ''
    Write-Host '  .\dev fork-sync [args]          Run fork-sync tool'
    Write-Host ''
    Write-Host '  Examples:'
    Write-Host '    .\dev fork-sync --dep nodejs --status'
    Write-Host ''
}

# =============================================================================
# Commands
# =============================================================================

switch ($Command) {
    'fork-sync' {
        Ensure-ScriptDeps
        & node (Join-Path $ScriptDir 'scripts\fork-sync\node_modules\@rnx-kit\fork-sync\lib\sync.js') @Args
    }

    default {
        Show-Help
    }
}

exit $LASTEXITCODE
