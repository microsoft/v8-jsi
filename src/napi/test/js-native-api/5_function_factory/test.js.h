#include "lib/modules.h"

DEFINE_TEST_SCRIPT(test_5_function_factory_test_js, R"JavaScript(
'use strict';
const common = require('../../common');
const assert = require('assert');
const addon = require(`./build/${common.buildType}/binding`);

const fn = addon();
assert.strictEqual(fn(), 'hello world'); // 'hello world'
)JavaScript");
