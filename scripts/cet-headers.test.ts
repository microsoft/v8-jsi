// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

import assert from "node:assert/strict";
import { test } from "node:test";
import { checkCetHeaders, pickNewestMsvcTool } from "./build-common.ts";

// Reduced from Release dumpbin /headers output; paths and unrelated fields omitted.
const ordinary = `Microsoft (R) COFF/PE Dumper Version 14.50.35717.0

FILE HEADER VALUES
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
    6B00A136 cv            20 020427E0  20417E0
    6B00A136 repro         24 020427C0  20417C0
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
      assert.throws(() => checkCetHeaders(headers, expected, "tool", "C:/VS/dumpbin.exe"), (error: Error) => {
        assert.match(error.message, /invalid dumpbin|no recognizable DLL characteristics block/);
        assert.match(error.message, /dumpbin: C:\/VS\/dumpbin.exe/);
        return true;
      });
    }
  }
});

test("newest MSVC tool wins regardless of discovery order", () => {
  const tool = (version: string) => `C:/VS/VC/Tools/MSVC/${version}/bin/Hostx64/x64/dumpbin.exe`;
  const old = tool("14.16.27023"), middle = tool("14.29.30133"), newest = tool("14.50.35717");
  // Opposite discovery orders must select the same toolset with either separator.
  for (const paths of [[old, middle, newest], [newest, middle, old]]) {
    assert.equal(pickNewestMsvcTool(paths), newest);
    assert.equal(pickNewestMsvcTool(paths.map((p) => p.replaceAll("/", "\\"))), newest.replaceAll("/", "\\"));
  }
  assert.equal(pickNewestMsvcTool([tool("14.9.99999"), tool("14.10.1")]), tool("14.10.1"));
  assert.equal(pickNewestMsvcTool([]), undefined);
});

test("paths without a parseable MSVC version are ignored before ranking", () => {
  const invalid = ["C:/VS/LLVM/bin/dumpbin.exe", "C:/VS/MSVC/not-a-version/bin/dumpbin.exe",
    "C:/VS/MSVC/14.50/bin/dumpbin.exe"];
  const valid = "C:/VS/MSVC/14.10.1/bin/dumpbin.exe";
  assert.equal(pickNewestMsvcTool(invalid), undefined);
  // Invalid entries on either side cannot displace a valid MSVC toolset.
  for (const paths of [[...invalid, valid], [valid, ...invalid]]) {
    assert.equal(pickNewestMsvcTool(paths), valid);
  }
});

test("CET mismatch reports the selected tool, banner, characteristics and debug rows", () => {
  // Cover absent CET metadata as well as an unexpected positive marking.
  for (const [headers, expected] of [[ordinary, true], [extendedCet, false],
    [extendedCet.replace("CET compatible", "Reserved"), true]] as const) {
    assert.throws(() => checkCetHeaders(headers, expected, "engine", "C:/VS/dumpbin.exe"), (error: Error) => {
      assert.match(error.message, /dumpbin: C:\/VS\/dumpbin.exe/);
      assert.match(error.message, /Microsoft \(R\) COFF\/PE Dumper Version 14\.50\.35717\.0/);
      assert.match(error.message, /C160 DLL characteristics/);
      assert.match(error.message, /NX compatible/);
      assert.doesNotMatch(error.message, /size of stack reserve|SECTION HEADER/);
      if (headers !== ordinary) {
        assert.match(error.message, /Debug Directories \(Time Type Size RVA Pointer\):/);
        for (const row of ["6B00A136 cv            20 020427E0  20417E0",
          "6B00A136 repro         24 020427C0  20417C0", "6B00A136 dllchar        4 02042804  2041804"]) {
          assert.ok(error.message.includes(row));
        }
      }
      if (!expected) assert.match(error.message, /00000001 extended DLL characteristics; CET compatible/);
      assert.ok(error.message.split("\n").length <= 8);
      return true;
    });
  }
});
