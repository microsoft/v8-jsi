#include "lib/modules.h"

DEFINE_TEST_SCRIPT(test_2_function_arguments_test_js, R"JavaScript(
'use strict';
const common = require('../../common');
const assert = require('assert');
const addon = require(`./build/${common.buildType}/binding`);

assert.strictEqual(addon.add(3, 5), 8);
)JavaScript");
