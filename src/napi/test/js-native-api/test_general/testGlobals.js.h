#include "lib/modules.h"

DEFINE_TEST_SCRIPT(test_general_testGlobals_js, R"JavaScript(
'use strict';
const common = require('../../common');
const assert = require('assert');

const test_globals = require(`./build/${common.buildType}/test_general`);

assert.strictEqual(test_globals.getUndefined(), undefined);
assert.strictEqual(test_globals.getNull(), null);
)JavaScript");
