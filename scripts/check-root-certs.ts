// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// check-root-certs.ts -- guards the root store compiled into Node.js builds.
//
// Fails when the vendored root certificates (src/node_root_certs.h) or the
// data they are generated from (tools/certdata.txt) contain an eMudhra root,
// or when Node's block list of the compromised eMudhra intermediate keys
// (kBlockedSPKIs in src/crypto/crypto_context.cc) lacks an entry. Upstream
// Node.js still ships "emSign Root CA - G1", so a sync can bring it back.
//
// Usage: node scripts/check-root-certs.ts

import { X509Certificate } from "node:crypto";
import fs from "node:fs";
import path from "node:path";

const nodejsDir = path.resolve(import.meta.dirname, "..", "deps", "nodejs");

/** eMudhra roots that must not be trusted, by SHA-256 certificate fingerprint. */
export const deniedRoots: ReadonlyArray<{ name: string; sha256: string }> = [
  {
    name: "emSign Root CA - G1",
    sha256: "40:F6:AF:03:46:A9:9A:A1:CD:1D:55:5A:4E:9C:CE:62:C7:F9:63:46:03:EE:40:66:15:83:3D:C8:C8:D0:03:67",
  },
  {
    name: "emSign ECC Root CA - G3",
    sha256: "86:A1:EC:BA:08:9C:4A:8D:3B:BE:27:34:C6:12:BA:34:1D:81:3E:04:3C:F9:E8:A8:62:CD:5C:57:A3:6B:BE:6B",
  },
  {
    name: "emSign Root CA - C1",
    sha256: "12:56:09:AA:30:1D:A0:A2:49:B9:7A:82:39:CB:6A:34:21:6F:44:DC:AC:9F:39:54:B1:42:92:F2:E8:C8:60:8F",
  },
  {
    name: "emSign ECC Root CA - C3",
    sha256: "BC:4D:80:9B:15:18:9D:78:DB:3E:1D:8C:F4:F9:72:6A:79:5D:A1:64:3C:A5:F1:35:8E:1D:DB:0E:DC:0D:7E:B3",
  },
];

/** SHA-256 hashes of the compromised eMudhra intermediate public keys (SPKI). */
export const requiredBlockedSpkis: readonly string[] = [
  "49dd353dc2b919a3770645117d0e60c16115e7d1bc259eefe734b00ea434fe5f",
  "5c0660a2b164910d8ff589c18e14552e9e0ec237dcefe2cb736d41a4811f81a0",
  "63ecefee431d49523bc1136c4e72d0206a477086caf606cd5cf025686c109cf8",
  "92c063d67e8f14db71d288e11266ffdaf69cbf115f7bab14f3bc2f68d25e0e30",
  "ebed6475c2c2d80fcb199f3fe8dd6e6b3ce60b77eb15d4e42c3dec0c6f5b5105",
  "ec4ffa1ad0ac88081314b7aaffef6a91f97cd57291bb5ba686c5415f5eb4f6b6",
  "43bffa19d6ef651451f6841e019931e96f6f2eaeff28ab4b6f1ce4576de2717b",
  "e0254c7fcdc265e2f0f8a428dabb75ab4ae039b4872d62f5ee57f0877d5a8a41",
];

const emudhraPattern = /eMudhra|emSign/i;

export interface RootStoreSources {
  /** Text of src/node_root_certs.h. */
  header: string;
  /** Text of tools/certdata.txt. */
  certdata: string;
  /** Text of src/crypto/crypto_context.cc. */
  cryptoContext: string;
}

/** Decodes every PEM certificate in node_root_certs.h, where each one is a run of C string literals. */
export function readRootCertsHeader(text: string): X509Certificate[] {
  const literal = /"-----BEGIN CERTIFICATE-----\\n"[\s\S]*?"-----END CERTIFICATE-----"/g;
  return [...text.matchAll(literal)].map((match) => {
    const pem = [...match[0].matchAll(/"([^"]*)"/g)]
      .map((part) => part[1])
      .join("")
      .replaceAll("\\n", "\n");
    return new X509Certificate(pem);
  });
}

/** Decodes the certificate objects (CKO_CERTIFICATE) in NSS certdata.txt. */
export function readCertdata(text: string): X509Certificate[] {
  const certs: X509Certificate[] = [];
  const lines = text.split(/\r?\n/);
  let inCertificate = false;
  for (let i = 0; i < lines.length; i++) {
    const line = lines[i];
    if (line.startsWith("CKA_CLASS ")) {
      inCertificate = line.endsWith(" CKO_CERTIFICATE");
    } else if (inCertificate && line === "CKA_VALUE MULTILINE_OCTAL") {
      const bytes: number[] = [];
      for (i++; lines[i] !== "END"; i++) {
        if (i >= lines.length) throw new Error("certdata.txt: CKA_VALUE without END");
        for (const octal of lines[i].matchAll(/\\([0-7]{3})/g)) {
          bytes.push(parseInt(octal[1], 8));
        }
      }
      certs.push(new X509Certificate(Buffer.from(bytes)));
    }
  }
  return certs;
}

/** Returns the hex SHA-256 SPKI hashes listed in kBlockedSPKIs. */
export function readBlockedSpkis(text: string): string[] {
  const start = text.indexOf("kBlockedSPKIs[] = {");
  if (start < 0) return [];
  const body = text.slice(start, text.indexOf("};", start));
  return [...body.matchAll(/\{([^{}]*)\}/g)].map((entry) =>
    [...entry[1].matchAll(/0x([0-9a-fA-F]{2})/g)].map((byte) => byte[1].toLowerCase()).join(""),
  );
}

/** Returns one message per problem; an empty list means the root store is clean. */
export function checkRootStore(sources: RootStoreSources): string[] {
  const problems: string[] = [];
  const denied = new Map(deniedRoots.map((root) => [root.sha256, root.name]));

  const inspect = (file: string, text: string, certs: X509Certificate[]): void => {
    if (certs.length === 0) problems.push(`${file}: no certificates found; the format may have changed`);
    for (const cert of certs) {
      const name = denied.get(cert.fingerprint256);
      if (name !== undefined) {
        problems.push(`${file}: contains the eMudhra root "${name}"`);
      } else if (emudhraPattern.test(cert.subject) || emudhraPattern.test(cert.issuer)) {
        problems.push(`${file}: contains an eMudhra certificate (${cert.subject.replaceAll("\n", ", ")})`);
      }
    }
    if (emudhraPattern.test(text)) problems.push(`${file}: mentions eMudhra or emSign`);
  };
  inspect("src/node_root_certs.h", sources.header, readRootCertsHeader(sources.header));
  inspect("tools/certdata.txt", sources.certdata, readCertdata(sources.certdata));

  const blocked = new Set(readBlockedSpkis(sources.cryptoContext));
  for (const spki of requiredBlockedSpkis) {
    if (!blocked.has(spki)) problems.push(`src/crypto/crypto_context.cc: kBlockedSPKIs lacks ${spki}`);
  }
  return problems;
}

function main(): void {
  const read = (relative: string): string => fs.readFileSync(path.join(nodejsDir, relative), "utf8");
  const sources: RootStoreSources = {
    header: read("src/node_root_certs.h"),
    certdata: read("tools/certdata.txt"),
    cryptoContext: read("src/crypto/crypto_context.cc"),
  };
  const problems = checkRootStore(sources);
  if (problems.length > 0) {
    for (const problem of problems) console.error(`ERROR: ${problem}`);
    process.exit(1);
  }
  const roots = readRootCertsHeader(sources.header).length;
  console.log(
    `Root store OK: ${roots} roots, no eMudhra roots, all ${requiredBlockedSpkis.length} compromised eMudhra keys blocked.`,
  );
}

if (import.meta.main) {
  main();
}
