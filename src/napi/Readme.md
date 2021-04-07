# NAPI for V8 implementation

This folder contains implementation of N-API (Node.js native JavaScript API)
implemented for the V8 JavaScript engine. The implementation and the related
tests are adapted from the Node.js project. The documentation about N-API can
be found here: https://nodejs.org/api/n-api.html

Our main goal of adopting N-API is to have it as the core of the ABI safe API
for the V8 JavaScript engine DLL. We think that rather than inventing a new set
of ABI-safe APIs we must adopt the existing API set which is the most widely
used in industry.

While working on the adoption the N-API implementation for V8 from the Node.js
project we want to keep the most Node.js files "as-is". The preference is to
make any required changes outside of the core files taken from Node.js. It
should help us adopting future changes from the Node.js project.

Below is the list of files that we use for the N-API implementation and its
unit tests.

## Public files in the root 'public' folder

- js_native_api.h - the N-API interface declaration. It is based on "C" calling
  conventions. It is taken from Node.js without changes.
- js_native_api_types.h - types used by the js_native_api.h. It is taken from
  Node.js without changes.
- js_native_ext_api.h - extensions to the N-API to host JavaScript engine. It
  is not part of Node.js code.

## NAPI implementation files

- js_native_api_v8.cc - N-API implementation file. It is taken from Node.js
  without changes.
- js_native_api_v8.h - N-API implementation header file. It is taken from
  Node.js without changes.
- js_native_api_v8_internals.h - code needed to support the N-API
  implementation. It is a mixture of our and Node.js code.
- env-inl.h - the file referenced by the N-API implementation. We replaced its
  content with some common macro definitions because it is included before
  other files.
- util-inl.h - the file referenced by the N-API implementation. We only keep a
  subset of utilities from the util.h and util-inl.h files needed for the N-API
  implementation.
- js_native_ext_api_v8.cpp - mostly implementation of the js_native_ext_api.h
  and some other functions that we want to have in a .cpp file. Some code is
  borrowed from Node.js project.

In addition, the files in the parent folder: V8Platform.h, V8Platform.cpp,
V8JsiRuntime.cpp, and V8JsiRuntime_impl.h are modified to support the N-API
environment. Some of the code in these files are borrowed from the Node.js
project and some other code is from the d8 utility from the V8 JavaScript
engine project (e.g. to manage unhandled promise rejections).

## The NAPI unit tests

The goals here are:
- run the JavaScript N-API tests as they written in Node.js project;
- run tests using GTest;
- tests can be run against different N-API implementations;
- be able to add custom tests.

- js-native-api folder contains code from the corresponding N-API folder in
  Node.js: https://github.com/nodejs/node/tree/master/test/js-native-api
  All .js files in this folder were converted to .js.h files to use in C++ code.
  Some code was changed to add expicit cast from void* to a pointer of
  specific type. It is caused by the difference between C and C++.
- lib folder has wrappers for assert.js and common.js files that N-API tests
  use. Contents of these files was changed to make code smaller but still having
  the functinality used by the N-API tests.
- napitest.h and napitest.cpp files contain the base classes and helpers used
  by all tests.
- test_*.cpp are the test files that either wrap up the tests from the
  js-native-api folder, or implement additional tests.
- napitest_v8.cpp - the adaptation of the N-API tests for the V8 engine.

## Copyright notes

Files in this folder and some files in the public folder are fully or partially
copied from the Node.js project. The Node.js license file can be found here:
https://github.com/nodejs/node.
If any file in this folder, subfolder, or the 'public' folder has no explictly
stated copyright notice, then it is covered by the Node.js license.

The unhandled promise rejection is based on the d8 code that is found in the V8
project: https://github.com/v8/v8/tree/master/src/d8.
The V8 license file can be found here: https://github.com/v8/v8
