'use strict';
const common = require('../../common');
const assert = require('assert');
const binding = require(`./build/${common.buildType}/test_string`);

binding.TestStringBufferBoundaries();

const embeddedNull = 'a\0b';
assert.strictEqual(binding.TestLatin1(embeddedNull), embeddedNull);
assert.strictEqual(binding.TestUtf8(embeddedNull), embeddedNull);
assert.strictEqual(binding.TestUtf16(embeddedNull), embeddedNull);
assert.strictEqual(binding.Utf8Length(embeddedNull), 3);
assert.strictEqual(binding.Utf16Length(embeddedNull), 3);

assert.strictEqual(binding.TestUtf8('\ud800'), '\ufffd');
assert.strictEqual(binding.TestUtf8('\udc00'), '\ufffd');
assert.strictEqual(binding.TestUtf16('\ud800'), '\ud800');
assert.strictEqual(binding.TestUtf16('\udc00'), '\udc00');
assert.strictEqual(binding.TestUtf8Insufficient('\ud83d\ude00'), '');
