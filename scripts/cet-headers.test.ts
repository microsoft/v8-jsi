// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

import assert from "node:assert/strict";
import { test } from "node:test";
import { checkCetHeaders } from "./build-common.ts";

// Reduced from Release dumpbin /headers output; paths and unrelated fields omitted.
const ordinary = `FILE HEADER VALUES
            8664 machine (x64)

OPTIONAL HEADER VALUES
             20B magic # (PE32+)
            C160 DLL characteristics
                   High Entropy Virtual Addresses
                   Dynamic base
                   NX compatible
                   Control Flow Guard
                   Terminal Server Aware
          100000 size of stack reserve
`;
const debug = `
SECTION HEADER #2
  .rdata name

  Debug Directories

        Time Type        Size      RVA  Pointer
    -------- ------- -------- -------- --------
    6B00A136 dllchar        4 02042804  2041804

        00000001 extended DLL characteristics
                   CET compatible

SECTION HEADER #3
   .data name
`;
const extendedCet = ordinary + debug;
// Preserve the real list indentation when exercising the optional-header form.
const optionalCet = ordinary.replace("          100000 size of stack reserve",
  "                   CET compatible\n          100000 size of stack reserve");

// Both dumpbin layouts must enforce positive and negative expectations.
for (const [layout, headers, expected] of [
  ["ordinary optional header", ordinary, false],
  ["CET optional header", optionalCet, true],
  ["CET extended characteristics", extendedCet, true],
  ["unmarked extended characteristics", extendedCet.replace("CET compatible", "Reserved"), false],
] as const) {
  test(`${layout} enforces the requested CET polarity`, () => {
    assert.doesNotThrow(() => checkCetHeaders(headers, expected, "engine"));
    assert.throws(() => checkCetHeaders(headers, !expected, "engine"), /must .*be CET compatible/);
  });
}

test("CET text outside a characteristics list does not mark a binary", () => {
  // Cover list boundaries and both misplaced characteristics-block forms.
  for (const stray of [
    "CET compatible\n",
    "                   CET compatible\n",
    "CET compatible.exe\n",
    "\n                   CET compatible\n",
    "SECTION HEADER #2\n                   CET compatible\n",
    "  Debug Directories\n                   CET compatible\n",
    "        00000001 extended DLL characteristics\n                   CET compatible\n",
    "  Debug Directories\n        4160 DLL characteristics\n                   CET compatible\n",
    debug.replace("CET compatible", "Reserved") + "                   CET compatible\n",
  ]) {
    assert.doesNotThrow(() => checkCetHeaders(ordinary + stray, false, "tool"));
    assert.throws(() => checkCetHeaders(ordinary + stray, true, "tool"), /must be CET compatible/);
  }
});

test("missing recognizable header blocks fail even when CET is not expected", () => {
  // Malformed output must not silently satisfy either header contract.
  for (const headers of ["", "FILE HEADER VALUES\n", "FILE HEADER VALUES\nCET compatible\n",
    "FILE HEADER VALUES\nOPTIONAL HEADER VALUES\n                   CET compatible\n",
    "FILE HEADER VALUES\n        00000001 extended DLL characteristics\n                   CET compatible\n"]) {
    for (const expected of [false, true]) {
      assert.throws(() => checkCetHeaders(headers, expected, "tool"), /invalid dumpbin|no recognizable DLL characteristics block/);
    }
  }
});
