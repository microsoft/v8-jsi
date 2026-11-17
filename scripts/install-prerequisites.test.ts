// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { readFileSync } from "node:fs";
import { test } from "node:test";
import { fileURLToPath } from "node:url";
import { parseAcceptablePythons } from "./build-common.ts";

const script = new URL("install-prerequisites.ps1", import.meta.url);
const quote = (s: string) => `'${s.replaceAll("'", "''")}'`;
const load = `. ${quote(fileURLToPath(script))}`;

function powershell(body: string, answer?: string) {
  const result = spawnSync("powershell.exe", ["-NoProfile", "-OutputFormat", "Text", ...(answer === undefined ? ["-NonInteractive"] : []),
    "-EncodedCommand", Buffer.from(`
$ErrorActionPreference = 'Stop'
if ($PSVersionTable.PSVersion.Major -ne 5) { throw 'Windows PowerShell 5.1 required' }
${load}
${body}`, "utf16le").toString("base64")],
  { encoding: "utf8", timeout: 30_000, input: answer === undefined ? undefined : `${answer}\n` });
  assert.ifError(result.error);
  // EncodedCommand also serializes Write-Host information records to stderr.
  return { code: result.status, output: result.stdout, stderr: result.stderr };
}

interface Scenario {
  missing?: string[];
  staysMissing?: string[];
  exitCode?: number;
  nodeVersion?: string;
  brokenNasm?: boolean;
  architecture?: string;
  winget?: string;
  flags?: string;
  extra?: string;
  answer?: string;
}

function run(options: Scenario = {}) {
  const json = JSON.stringify(options);
  return powershell(`
$case = ConvertFrom-Json ${quote(json)}
$script:installed = @()
$env:PATH = 'C:\\manager;C:\\existing;C:\\EXISTING\\'
function Get-HostArchitecture { if ($case.architecture) { return $case.architecture }; return 'AMD64' }
function Test-Elevated { return $false }
function Test-Interactive { return $false }
function Get-Nasm {
    Write-Host 'PROBE NASM'
    if ($case.missing -contains 'NASM.NASM' -and
        ($script:installed -notcontains 'NASM.NASM' -or $case.staysMissing -contains 'NASM.NASM')) { return }
    $errorText = $null
    if ($case.brokenNasm) { $errorText = 'NASM version probe failed: empty or failing -v output' }
    return @{ Path = 'C:\\NASM\\nasm.exe'; Version = '3.02'; Error = $errorText }
}
function Get-Pythons($Accepted) {
    Write-Host 'PROBE Python'
    if ($case.missing -contains 'Python.Python.3.14' -and
        ($script:installed -notcontains 'Python.Python.3.14' -or $case.staysMissing -contains 'Python.Python.3.14')) { return }
    return @{ Path = 'C:\\Python\\python.exe'; Version = '3.14.0' },
        @{ Path = 'C:\\OtherPython\\python.exe'; Version = '3.13.2' }
}
function Get-Node {
    Write-Host 'PROBE Node'
    if ($case.missing -contains 'OpenJS.NodeJS.LTS' -and
        ($script:installed -notcontains 'OpenJS.NodeJS.LTS' -or $case.staysMissing -contains 'OpenJS.NodeJS.LTS')) { return }
    $version = 'v24.22.1'
    if ($case.nodeVersion) { $version = $case.nodeVersion }
    return @{ Path = 'C:\\manager\\node.exe'; Version = $version; Error = $null }
}
function Get-RegistryPath($Scope) {
    if ($Scope -eq 'Machine') { return 'c:\\EXISTING\\;C:\\machine;C:\\NASM' }
    return 'c:\\MACHINE\\;C:\\user;C:\\Python'
}
function Invoke-WinGet($Arguments) {
    Write-Host ('CALL ' + ($Arguments -join ' '))
    if ($case.winget -eq 'missing') { throw 'mock WinGet missing' }
    if ($Arguments[0] -eq '--version') {
        $version = 'v1.6.2631'
        if ($case.winget) { $version = $case.winget }
        return @{ ExitCode = 0; Output = $version }
    }
    $id = $Arguments[2]
    $script:installed += $id
    return @{ ExitCode = [int]$case.exitCode; Output = 'mock install' }
}
${options.extra ?? ""}
$code = Invoke-Prerequisites ${options.flags ?? "-Yes"}
Write-Host "FINALPATH=$env:PATH"
exit $code`, options.answer);
}

test("the script parses and runs under Windows PowerShell 5.1", () => {
  const result = powershell(`
$tokens = $null; $errors = $null
[void][System.Management.Automation.Language.Parser]::ParseFile(${quote(fileURLToPath(script))}, [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw ($errors | Out-String) }
Write-Host 'parser errors: 0'`);
  assert.equal(result.code, 0, result.output);
  assert.match(result.output, /parser errors: 0/);
});

test("the installer contains no policy bypass, self-elevation, downloader or PS7 constructs", () => {
  const text = readFileSync(script, "utf8");
  for (const forbidden of [
    /\bSet-ExecutionPolicy\b/i, /-ExecutionPolicy\b/i,
    /\bRunAs\b|\bRunAsAdministrator\b|self[- ]elevat/i,
    /\b(?:Invoke-WebRequest|Invoke-RestMethod|Start-BitsTransfer|WebClient)\b/i,
    /\bwinget(?:\.exe)?\s+settings\b/i, /\bInstall-Module\b/i,
    /\?\?|\?\.|&&|\|\|/,
  ]) {
    assert.doesNotMatch(text, forbidden);
  }
});

test("the elevation warning appears only when user-scope Python is planned", () => {
  // Elevated runs cover no installs, machine-only installs, and user-scope Python.
  for (const missing of [[], ["NASM.NASM", "OpenJS.NodeJS.LTS"], ["Python.Python.3.14"]]) {
    const result = run({ missing, extra: "function Test-Elevated { return $true }" });
    assert.equal(result.code, 0, result.output);
    const output = result.output + result.stderr;
    if (missing.includes("Python.Python.3.14")) {
      assert.match(output, /User-scope Python installs into the elevated account/);
    } else {
      assert.doesNotMatch(output, /User-scope Python/);
    }
  }
});

test("accepted Python versions match configure and include the installation version", () => {
  const actual = powershell(`Write-Host ('ACCEPTED=' + ((Get-AcceptedPythons ${quote(
    fileURLToPath(new URL("../deps/nodejs/configure", import.meta.url)))}) -join ','))`);
  assert.equal(actual.code, 0, actual.output);
  const expected = parseAcceptablePythons(readFileSync(new URL("../deps/nodejs/configure", import.meta.url), "utf8"))
    .map((v) => v.join("."));
  assert.ok(expected.includes("3.14"));
  assert.ok(actual.output.includes(`ACCEPTED=${expected.join(",")}`), actual.output);
});

test("missing configure gives a clear source-tree error and exit one", () => {
  const result = powershell(`exit (Invoke-Prerequisites -CheckOnly -ConfigurePath 'C:\\missing-source\\deps\\nodejs\\configure')`);
  assert.equal(result.code, 1, result.output);
  assert.match(result.output, /Source tree required: missing .*deps\\nodejs\\configure/);
});

test("satisfied machines keep all accepted tools without WinGet or consent on repeat runs", () => {
  const result = run({ flags: "", extra: "$first = Invoke-Prerequisites; if ($first -ne 0) { throw 'first run failed' }" });
  assert.equal(result.code, 0, result.output);
  for (const text of ["3.02 at C:\\NASM\\nasm.exe", "3.14.0 at C:\\Python\\python.exe",
    "3.13.2 at C:\\OtherPython\\python.exe", "v24.22.1 at C:\\manager\\node.exe"]) {
    assert.ok(result.output.includes(text), result.output);
  }
  assert.doesNotMatch(result.output, /CALL |Install missing/);
  assert.equal((result.output.match(/Summary: NASM=True; Python=True; Node.js=True/g) ?? []).length, 2);
});

test("consented installs use exact commands and recheck all three tools", () => {
  const result = run({ missing: ["NASM.NASM", "Python.Python.3.14", "OpenJS.NodeJS.LTS"] });
  assert.equal(result.code, 0, result.output);
  const calls = [...result.output.matchAll(/^CALL (install .+)$/gm)].map((m) => m[1].trim());
  const suffix = " --accept-source-agreements --accept-package-agreements";
  assert.deepEqual(calls, [
    "install --id NASM.NASM --exact --source winget --no-upgrade" + suffix,
    "install --id Python.Python.3.14 --exact --source winget --no-upgrade --scope user --installer-type burn" + suffix,
    "install --id OpenJS.NodeJS.LTS --exact --source winget --no-upgrade --version 24.22.1" + suffix,
  ]);
  for (const name of ["NASM", "Python", "Node"]) {
    assert.equal((result.output.match(new RegExp(`PROBE ${name}`, "g")) ?? []).length, 2);
  }
  assert.doesNotMatch(result.output, /--architecture/);
});

test("a successful WinGet exit with a missing tool fails only that item and continues", () => {
  // Each tool can fail detection independently without preventing the other installs.
  for (const [id, message, summary] of [
    ["NASM.NASM", "NASM still missing", "NASM=False; Python=True; Node.js=True"],
    ["Python.Python.3.14", "Accepted Python still missing", "NASM=True; Python=False; Node.js=True"],
    ["OpenJS.NodeJS.LTS", "Node.js still missing", "NASM=True; Python=True; Node.js=False"],
  ]) {
    const result = run({ missing: ["NASM.NASM", "Python.Python.3.14", "OpenJS.NodeJS.LTS"], staysMissing: [id] });
    assert.equal(result.code, 1, result.output);
    assert.ok(result.output.includes(message), result.output);
    assert.ok(result.output.includes(`Summary: ${summary}`), result.output);
    assert.equal((result.output.match(/^CALL install/gm) ?? []).length, 3);
  }
});

test("every nonzero WinGet exit fails even when detection succeeds", () => {
  // Representative cancellation and failure codes must never become success whitelists.
  for (const exitCode of [1, -1978335189]) {
    const result = run({ missing: ["NASM.NASM", "Python.Python.3.14"], exitCode });
    assert.equal(result.code, 1, result.output);
    assert.ok(result.output.includes(`FAILED: exit ${exitCode}: winget install --id NASM.NASM`));
    assert.ok(result.output.includes("NASM: 3.02 at C:\\NASM\\nasm.exe"));
    assert.ok(result.output.includes("Python: 3.14.0 at C:\\Python\\python.exe"));
    assert.match(result.output, /Summary: NASM=False; Python=False; Node.js=True/);
  }
});

test("an installed NASM with a failed version probe is not accepted", () => {
  const result = run({ brokenNasm: true });
  assert.equal(result.code, 1, result.output);
  assert.match(result.output, /NASM version probe failed/);
  assert.doesNotMatch(result.output, /CALL /);
});

test("an older first-on-PATH Node fails without attempting replacement; newer Node is kept", () => {
  const old = run({ nodeVersion: "v22.1.0" });
  assert.equal(old.code, 1, old.output);
  assert.ok(old.output.includes("v22.1.0 at C:\\manager\\node.exe"));
  assert.match(old.output, /Update it \(or its version manager\) to 24.x or remove it and rerun/);
  assert.doesNotMatch(old.output, /CALL /);
  const newer = run({ nodeVersion: "v26.1.0" });
  assert.equal(newer.code, 0, newer.output);
  assert.doesNotMatch(newer.output, /CALL /);
});

test("PATH merging preserves process-only Node and appends only new registry entries", () => {
  const result = run({ missing: ["NASM.NASM", "Python.Python.3.14"] });
  assert.equal(result.code, 0, result.output);
  assert.ok(result.output.includes("FINALPATH=C:\\manager;C:\\existing;C:\\EXISTING\\;C:\\machine;C:\\NASM;C:\\user;C:\\Python"));
  assert.match(result.output, /Node.js: v24.22.1 at C:\\manager\\node.exe/);
  assert.doesNotMatch(result.output, /CALL install --id OpenJS/);
});

test("ARM64 still installs NASM and a 32-bit Windows host is unsupported", () => {
  const arm = run({ architecture: "ARM64", missing: ["NASM.NASM"] });
  assert.equal(arm.code, 0, arm.output);
  assert.match(arm.output, /CALL install --id NASM.NASM/);
  const x86 = run({ architecture: "x86" });
  assert.equal(x86.code, 1, x86.output);
  assert.match(x86.output, /Unsupported host/);
  assert.doesNotMatch(x86.output, /CALL /);
});

test("missing or older WinGet fails clearly without installing", () => {
  for (const winget of ["missing", "v1.5.2201"]) {
    const result = run({ winget, missing: ["NASM.NASM"] });
    assert.equal(result.code, 1, result.output);
    assert.match(result.output, /https:\/\/learn.microsoft.com\/windows\/package-manager\/winget\//);
    assert.doesNotMatch(result.output, /CALL install/);
  }
});

test("WinGet preview and suffixed versions use the leading numeric version", () => {
  // Suffixes do not change minimum-version acceptance or rejection.
  for (const winget of ["v1.12.3-preview", "1.6.2631-dev", "v1.5.2201-preview"]) {
    const result = run({ winget, missing: ["NASM.NASM"] });
    assert.equal(result.code, winget.includes("1.5.") ? 1 : 0, result.output);
    if (result.code === 0) {
      assert.match(result.output, /CALL install --id NASM.NASM/);
    } else {
      assert.match(result.output, /WinGet 1.6.2631 or newer is required/);
      assert.doesNotMatch(result.output, /CALL install/);
    }
  }
});

test("check-only never calls WinGet and missing non-interactive consent fails immediately", () => {
  const check = run({ missing: ["NASM.NASM"], winget: "missing", flags: "-CheckOnly" });
  assert.equal(check.code, 1, check.output);
  assert.doesNotMatch(check.output, /CALL /);
  const noConsent = run({ missing: ["NASM.NASM"], flags: "" });
  assert.equal(noConsent.code, 1, noConsent.output);
  assert.match(noConsent.output, /Non-interactive session: rerun with -Yes/);
  assert.doesNotMatch(noConsent.output, /CALL /);
});

test("one consent question covers all installs without accepting WinGet agreements implicitly", () => {
  const options = { missing: ["NASM.NASM", "Python.Python.3.14"], flags: "",
    extra: "function Test-Interactive { return $true }" };
  const declined = run({ ...options, answer: "n" });
  assert.equal(declined.code, 1, declined.output + declined.stderr);
  assert.match(declined.output, /Installation declined/);
  assert.doesNotMatch(declined.output, /CALL /);
  const accepted = run({ ...options, answer: "" });
  assert.equal(accepted.code, 0, accepted.output + accepted.stderr);
  // Read-Host suppresses prompt text on redirected input; inspect its single call too.
  const text = readFileSync(script, "utf8");
  assert.equal((text.match(/Read-Host 'Install missing prerequisites\? \[Y\/n\]'/g) ?? []).length, 1);
  assert.equal((text.match(/\bRead-Host\b/g) ?? []).length, 1);
  assert.equal((accepted.output.match(/^CALL install/gm) ?? []).length, 2);
  assert.doesNotMatch(accepted.output, /--accept-source-agreements|--accept-package-agreements/);
});
