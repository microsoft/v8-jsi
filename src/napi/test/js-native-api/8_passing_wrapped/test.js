'use strict';
// Flags: --expose-gc

const common = require('../../common');
const assert = require('assert');
const addon = require(`./build/${common.buildType}/binding`);

async function runTest() {
  (() => {
    let obj1 = addon.createObject(10);
    let obj2 = addon.createObject(20);
    const result = addon.add(obj1, obj2);
    assert.strictEqual(result, 30);
  })();
  await common.gcUntil('8_passing_wrapped',
                       () => (addon.finalizeCount() === 2));
}
runTest();
