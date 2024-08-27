'use strict';
const common = require('../../common');

if (process.argv[2] === 'child') {
  const binding = require(`./build/${common.buildType}/test_ref_finalizer`);

  (async function() {
    {
      binding.createExternalWithJsFinalize(
        common.mustCall(() => {
          throw new Error('finalizer error');
        }));
    }
    global.gc();
  })().then(common.mustCall());
  return;
}

const assert = require('assert');
const { spawnSync } = require('child_process');
const child = spawnSync(process.execPath, [
  '--expose-gc', __filename, 'child',
]);
assert(common.nodeProcessAborted(child.status, child.signal));
assert.match(child.stderr.toString(), /finalizer error/);
