#include "lib/modules.h"

DEFINE_TEST_SCRIPT(test_4_object_factory_test_js, R"JavaScript(
'use strict';
const common = require('../../common');
const assert = require('assert');
const addon = require(`./build/${common.buildType}/binding`);

const obj1 = addon('hello');
const obj2 = addon('world');
assert.strictEqual(`${obj1.msg} ${obj2.msg}`, 'hello world');
)JavaScript");
