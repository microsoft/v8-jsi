'use strict';

// Tests the libnode embedding C API (src/node_embedding_api.h) through
// embedtest_c_api*.c.

require('../common');
require('./embedding-api-test-cases').runTests('c-api');
