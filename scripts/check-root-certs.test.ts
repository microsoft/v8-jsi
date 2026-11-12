// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Tests for check-root-certs.ts: node --test scripts/check-root-certs.test.ts

import assert from "node:assert/strict";
import { X509Certificate } from "node:crypto";
import fs from "node:fs";
import path from "node:path";
import { test } from "node:test";
import {
  checkRootStore,
  readBlockedSpkis,
  readCertdata,
  readRootCertsHeader,
  requiredBlockedSpkis,
  type RootStoreSources,
} from "./check-root-certs.ts";

const nodejsDir = path.resolve(import.meta.dirname, "..", "deps", "nodejs");

function vendoredSources(): RootStoreSources {
  const read = (relative: string): string => fs.readFileSync(path.join(nodejsDir, relative), "utf8");
  return {
    header: read("src/node_root_certs.h"),
    certdata: read("tools/certdata.txt"),
    cryptoContext: read("src/crypto/crypto_context.cc"),
  };
}

// emSign Root CA - G1, the eMudhra root that upstream Node.js 26.13.1 still ships.
const g1Pem = `-----BEGIN CERTIFICATE-----
MIIDlDCCAnygAwIBAgIKMfXkYgxsWO3W2DANBgkqhkiG9w0BAQsFADBnMQswCQYDVQQGEwJJ
TjETMBEGA1UECxMKZW1TaWduIFBLSTElMCMGA1UEChMcZU11ZGhyYSBUZWNobm9sb2dpZXMg
TGltaXRlZDEcMBoGA1UEAxMTZW1TaWduIFJvb3QgQ0EgLSBHMTAeFw0xODAyMTgxODMwMDBa
Fw00MzAyMTgxODMwMDBaMGcxCzAJBgNVBAYTAklOMRMwEQYDVQQLEwplbVNpZ24gUEtJMSUw
IwYDVQQKExxlTXVkaHJhIFRlY2hub2xvZ2llcyBMaW1pdGVkMRwwGgYDVQQDExNlbVNpZ24g
Um9vdCBDQSAtIEcxMIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAk0u76WaK7p1b
1TST0Bsew+eeuGQzf2N4aLTNLnF115sgxk0pvLZoYIr3IZpWNVrzdr3YzZr/k1ZLpVkGoZM0
Kd0WNHVO8oG0x5ZOrRkVUkr+PHB1cM2vK6sVmjM8qrOLqs1D/fXqcP/tzxE7lM5OMhbTI0Aq
d7OvPAEsbO2ZLIvZTmmYsvePQbAyeGHWDV/D+qJAkh1cF+ZwPjXnorfCYuKrpDhMtTk1b+oD
afo6VGiFbdbyL0NVHpENDtjVaqSW0RM8LHhQ6DqS0hdW5TUaQBw+jSztOd9C4INBdN+jzcKG
YEho42kLVACL5HZpIQ15TjQIXhTCzLG3rdd8cIrHhQIDAQABo0IwQDAdBgNVHQ4EFgQU++8N
hp6w492pufEhF38+/PB3KxowDgYDVR0PAQH/BAQDAgEGMA8GA1UdEwEB/wQFMAMBAf8wDQYJ
KoZIhvcNAQELBQADggEBAFn/8oz1h31xPaOfG1vR2vjTnGs2vZupYeveFix0PZ7mddrXuqe8
QhfnPZHr5X3dPpzxz5KsbEjMwiI/aTvFthUvozXGaCocV685743QNcMYDHsAVhzNixl03r4P
EuDQqqE/AjSxcM6dGNYIAwlG7mDgfrbESQRRfXBgvKqy/3lyeqYdPV8q+Mri/Tm3R7nrft8E
I6/6nAYH6ftjk4BAtcZsCjEozgyfz7MjNYBBjWzEN3uBL4ChQEKF6dk4jeihU80Bv2noWgby
RQuQ+q7hv53yrlc8pa6yVvSLZUDp/TGBLPQ5Cdjua6e0ph0VpZj3AYHYhX3zUVxxiN66zB+A
fko=
-----END CERTIFICATE-----`;

// Formats a PEM certificate the way node_root_certs.h stores it.
function headerEntry(pem: string): string {
  const lines = pem.trim().split("\n");
  const body = lines.slice(0, -1).map((line) => `"${line}\\n"`);
  return `\n/* planted */\n${body.join("\n")}\n"${lines[lines.length - 1]}",\n`;
}

// Formats a DER certificate the way certdata.txt stores it.
function certdataObject(der: Buffer): string {
  const octal = [...der].map((byte) => `\\${byte.toString(8).padStart(3, "0")}`);
  const rows: string[] = [];
  for (let i = 0; i < octal.length; i += 16) rows.push(octal.slice(i, i + 16).join(""));
  return [
    "",
    "#",
    '# Certificate "planted"',
    "#",
    "CKA_CLASS CK_OBJECT_CLASS CKO_CERTIFICATE",
    "CKA_VALUE MULTILINE_OCTAL",
    ...rows,
    "END",
    "",
  ].join("\n");
}

test("the vendored root store has no eMudhra root and blocks every compromised key", () => {
  assert.deepEqual(checkRootStore(vendoredSources()), []);
});

test("both root lists and the key block list parse", () => {
  const sources = vendoredSources();
  assert.ok(readRootCertsHeader(sources.header).length > 100);
  assert.ok(readCertdata(sources.certdata).length > 100);
  assert.equal(readBlockedSpkis(sources.cryptoContext).length, requiredBlockedSpkis.length);
});

test("a planted G1 root in node_root_certs.h is reported", () => {
  const sources = vendoredSources();
  const problems = checkRootStore({ ...sources, header: sources.header + headerEntry(g1Pem) });
  assert.ok(
    problems.some((problem) => problem.startsWith("src/node_root_certs.h") && problem.includes("emSign Root CA - G1")),
    problems.join("\n"),
  );
});

test("a planted G1 root in certdata.txt is reported", () => {
  const sources = vendoredSources();
  const der = new X509Certificate(g1Pem).raw;
  const problems = checkRootStore({ ...sources, certdata: sources.certdata + certdataObject(der) });
  assert.ok(
    problems.some((problem) => problem.startsWith("tools/certdata.txt") && problem.includes("emSign Root CA - G1")),
    problems.join("\n"),
  );
});

test("a missing blocked key is reported", () => {
  const sources = vendoredSources();
  const changed = sources.cryptoContext.replace("{0x49, 0xdd, 0x35", "{0x00, 0xdd, 0x35");
  assert.notEqual(changed, sources.cryptoContext);
  const problems = checkRootStore({ ...sources, cryptoContext: changed });
  assert.ok(problems.some((problem) => problem.includes(requiredBlockedSpkis[0])), problems.join("\n"));
});

test("an unparsable root store is reported instead of passing", () => {
  const sources = vendoredSources();
  const problems = checkRootStore({ ...sources, header: "/* empty */" });
  assert.ok(problems.some((problem) => problem.includes("no certificates found")), problems.join("\n"));
});
