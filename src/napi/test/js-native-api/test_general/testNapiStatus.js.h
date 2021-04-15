#include "lib/modules.h"

DEFINE_TEST_SCRIPT(test_general_testNapiStatus_js, R"JavaScript(
'use strict';

const common = require('../../common');
const addon = require(`./build/${common.buildType}/test_general`);
const assert = require('assert');

addon.createNapiError();
assert(addon.testNapiErrorCleanup(), 'napi_status cleaned up for second call');
)JavaScript");
