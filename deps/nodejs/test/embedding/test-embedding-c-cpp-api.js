'use strict';

// Tests the C++ wrapper over the libnode embedding C API
// (src/node_embedding_api_cpp.h) through embedtest_c_cpp_api*.cc.

require('../common');
require('./embedding-api-test-cases').runTests('c-cpp-api');
