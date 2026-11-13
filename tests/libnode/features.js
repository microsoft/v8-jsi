// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

// Checks that libnode has every feature of a full Node.js build. The package
// smoke test runs it through tests/libnode/smoke.c:
//   libnode_smoke features.js --expect-version <x.y.z> --expect-roots <count>
// It prints one line per feature and exits with 1 if any of them fails.

'use strict';

const assert = require('node:assert');
const crypto = require('node:crypto');
const tls = require('node:tls');
const { parseArgs } = require('node:util');

const { values: expected } = parseArgs({
  options: {
    'expect-version': { type: 'string' },
    'expect-roots': { type: 'string' },
  },
});
assert.ok(expected['expect-version'], '--expect-version is required');
assert.ok(expected['expect-roots'], '--expect-roots is required');

// process.config keeps some gyp variables as "true" / "false" strings.
function configFlag(name) {
  const value = process.config.variables[name];
  assert.ok(
    value === true || value === false || value === 'true' || value === 'false',
    `process.config.variables.${name} is ${value}`);
  return value === true || value === 'true';
}

const features = {
  'Node.js version': () => {
    assert.strictEqual(process.version, `v${expected['expect-version']}`);
  },

  'full ICU': () => {
    assert.strictEqual(configFlag('icu_small'), false);
    const date = new Date(Date.UTC(2026, 0, 2));
    const options = { timeZone: 'UTC', dateStyle: 'long' };
    assert.strictEqual(
      new Intl.DateTimeFormat('ja-JP', options).format(date), '2026年1月2日');
    assert.strictEqual(
      new Intl.NumberFormat('de-DE').format(1234567.891), '1.234.567,891');
  },

  'bundled OpenSSL': () => {
    assert.strictEqual(configFlag('node_shared_openssl'), false);
    assert.strictEqual(
      crypto.createHash('sha3-256').update('abc').digest('hex'),
      '3a985da74fe225b2045c172d6bd390bd855f086e3e9d525b46bfe24511431532');
    const key = crypto.randomBytes(32);
    const iv = crypto.randomBytes(12);
    const cipher = crypto.createCipheriv('aes-256-gcm', key, iv);
    const encrypted = Buffer.concat([cipher.update('libnode'), cipher.final()]);
    const decipher = crypto.createDecipheriv('aes-256-gcm', key, iv);
    decipher.setAuthTag(cipher.getAuthTag());
    const decrypted = Buffer.concat([decipher.update(encrypted), decipher.final()]);
    assert.strictEqual(decrypted.toString(), 'libnode');
  },

  'root certificates': () => {
    const roots = tls.rootCertificates;
    assert.strictEqual(roots.length, Number(expected['expect-roots']));
    const eMudhraRoots = roots
      .map((pem) => new crypto.X509Certificate(pem))
      .filter((cert) => /eMudhra|emSign/i.test(cert.subject))
      .map((cert) => cert.subject);
    assert.deepStrictEqual(eMudhraRoots, []);
  },

  'Temporal': () => {
    assert.ok(Temporal.Now.instant() instanceof Temporal.Instant);
  },

  'single executable applications': () => {
    assert.strictEqual(configFlag('node_use_lief'), true);
    assert.strictEqual(typeof require('node:sea').isSea, 'function');
  },

  'SQLite': () => {
    const { DatabaseSync } = require('node:sqlite');
    const db = new DatabaseSync(':memory:');
    try {
      assert.strictEqual(db.prepare('SELECT 40 + 2 AS answer').get().answer, 42);
    } finally {
      db.close();
    }
  },

  'FFI': () => {
    assert.strictEqual(configFlag('node_use_ffi'), true);
  },

  'inspector': () => {
    assert.strictEqual(process.features.inspector, true);
    assert.strictEqual(typeof require('node:inspector').open, 'function');
  },

  'fetch': () => {
    assert.strictEqual(typeof fetch, 'function');
  },

  'TypeScript type stripping': () => {
    assert.strictEqual(configFlag('node_use_amaro'), true);
  },

  'startup snapshot': () => {
    assert.strictEqual(configFlag('node_use_node_snapshot'), true);
  },
};

let failed = 0;
for (const [name, check] of Object.entries(features)) {
  try {
    check();
    console.log(`ok   ${name}`);
  } catch (err) {
    failed++;
    console.log(`FAIL ${name}: ${err.message}`);
  }
}
if (failed > 0) {
  console.log(`${failed} feature(s) failed`);
  process.exitCode = 1;
}
