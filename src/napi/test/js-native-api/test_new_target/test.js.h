#include "lib/modules.h"

DEFINE_TEST_SCRIPT(test_new_target_test_js, R"JavaScript(
'use strict';

const common = require('../../common');
const assert = require('assert');
const binding = require(`./build/${common.buildType}/binding`);

class Class extends binding.BaseClass {
  constructor() {
    super();
    this.method();
  }
  method() {
    this.ok = true;
  }
}

assert.ok(new Class() instanceof binding.BaseClass);
assert.ok(new Class().ok);
assert.ok(binding.OrdinaryFunction());
assert.ok(
  new binding.Constructor(binding.Constructor) instanceof binding.Constructor);
)JavaScript");
