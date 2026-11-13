'use strict';

// Test cases for the libnode embedding API (src/node_embedding_api.h): the C
// API (embedtest_c_api*.c) and the C++ wrapper over it
// (embedtest_c_cpp_api*.cc). The api argument is the embedtest prefix that
// selects them: 'c-api' or 'c-cpp-api'.

const common = require('../common');
const fixtures = require('../common/fixtures');
const path = require('path');
const {
  spawnSyncAndAssert,
  spawnSyncAndExit,
  spawnSyncAndExitWithoutError,
} = require('../common/child_process');

const embedtest = common.resolveBuiltBinary('embedtest');

function readFileCode(filePath) {
  return `(require("fs").readFileSync(${JSON.stringify(filePath)}, "utf8"))`;
}

const incMyCountScript =
  'myCount = 0; ' +
  'function incMyCount() { ' +
  '  ++myCount; ' +
  '  if (myCount < 5) setTimeout(incMyCount, 1); ' +
  '}';

function runLoadTests(api) {
  spawnSyncAndAssert(embedtest, [api, 'console.log(42)'], {
    trim: true,
    stdout: '42',
  });

  spawnSyncAndAssert(embedtest, [api, 'console.log(embedVars.nön_ascıı)'], {
    trim: true,
    stdout: '🏳️‍🌈',
  });

  spawnSyncAndExit(embedtest, [api, 'throw new Error()'], {
    status: 1,
    signal: null,
  });

  spawnSyncAndExit(embedtest, [api, 'require("lib/internal/test/binding")'], {
    status: 1,
    signal: null,
  });

  spawnSyncAndExit(embedtest, [api, 'process.exitCode = 8'], {
    status: 8,
    signal: null,
  });

  const fixturePath = JSON.stringify(fixtures.path('exit.js'));
  spawnSyncAndExit(embedtest, [api, `require(${fixturePath})`, 92], {
    status: 92,
    signal: null,
  });

  spawnSyncAndExit(embedtest, [api, '0syntax_error'], {
    status: 1,
    stderr: /SyntaxError: Invalid or unexpected token/,
  });

  // The test's platform sets node_embedding_platform_flags_disable_node_options_env.
  if (!process.config.variables.node_without_node_options) {
    spawnSyncAndExit(
      embedtest,
      [api, 'require("os")'],
      {
        env: {
          ...process.env,
          NODE_REPL_EXTERNAL_MODULE: 'fs',
        },
      },
      {
        status: 9,
        signal: null,
        trim: true,
        stderr:
          `${embedtest}: NODE_REPL_EXTERNAL_MODULE can't be used with ` +
          'kDisableNodeOptionsEnv',
      });
  }
}

function runRuntimeTests(api) {
  spawnSyncAndAssert(
    embedtest,
    [`${api}-nodejs-main`, '--eval', 'console.log("Hello World")'],
    {
      trim: true,
      stdout: 'Hello World',
    });

  spawnSyncAndAssert(
    embedtest,
    [api, 'function callMe(text) { return text + " you"; }'],
    { stdout: 'called you' });

  spawnSyncAndAssert(
    embedtest,
    [api, 'function waitMe(text, cb) { setTimeout(() => cb(text + " you"), 1); }'],
    { stdout: 'waited you' });

  spawnSyncAndAssert(
    embedtest,
    [
      api,
      'function waitPromise(text) { ' +
        'return new Promise((res) => ' +
        '  setTimeout(() => res(text + " with cheese"), 1)); ' +
        '}',
    ],
    { stdout: 'waited with cheese' });

  spawnSyncAndAssert(
    embedtest,
    [
      api,
      'function waitPromise(text) { ' +
        'return new Promise((res, rej) => ' +
        '  setTimeout(() => rej(text + " without cheese"), 1)); ' +
        '}',
    ],
    { stdout: 'waited without cheese' });

  // 12 runtimes, each in its own thread.
  spawnSyncAndAssert(
    embedtest,
    [`${api}-threading-runtime-per-thread`, 'myCount = 1'],
    {
      trim: true,
      stdout: '12',
    });

  // 12 runtimes in one thread.
  spawnSyncAndAssert(
    embedtest,
    [`${api}-threading-several-runtimes-per-thread`, incMyCountScript],
    {
      trim: true,
      stdout: '60',
    });

  // One runtime used from several threads.
  spawnSyncAndAssert(
    embedtest,
    [`${api}-threading-runtime-in-several-threads`, incMyCountScript],
    {
      trim: true,
      stdout: '5',
    });

  // One runtime driven by the task runner of a UI thread.
  spawnSyncAndAssert(
    embedtest,
    [`${api}-threading-runtime-in-ui-thread`, incMyCountScript],
    {
      trim: true,
      stdout: '5',
    });

  const preloadScriptPath = path.join(__dirname, 'preload-with-worker.js');
  spawnSyncAndAssert(
    embedtest,
    [`${api}-preload`, `eval(${readFileCode(preloadScriptPath)})`],
    { cwd: __dirname },
    {
      trim: true,
      stdout: 'preloadValue=42; worker preloadValue=42',
    });

  const linkedModulesScriptPath = path.join(__dirname, 'use-linked-modules.js');
  spawnSyncAndAssert(
    embedtest,
    [
      `${api}-linked-modules`,
      `eval(${readFileCode(linkedModulesScriptPath)})`,
      2, // Expected number of greeter module initializations.
      2, // Expected number of replicator module initializations.
    ],
    { cwd: __dirname },
    {
      trim: true,
      stdout: 'main=Hello, World World; worker=Hello, Node Node',
    });
}

function runEnvironmentTests(api) {
  // The script requires ./test/common/globals relative to the working
  // directory.
  const nodeRoot = path.resolve(__dirname, '..', '..');
  spawnSyncAndExitWithoutError(
    embedtest,
    [`${api}-env-no-browser-globals`],
    { cwd: nodeRoot });

  spawnSyncAndExitWithoutError(embedtest, [`${api}-env-with-esm-loader`]);

  // vm.SourceTextModule needs --experimental-vm-modules, which this case
  // does not pass.
  spawnSyncAndExit(embedtest, [`${api}-env-with-no-esm-loader`], {
    status: 1,
    signal: null,
  });
}

function runTests(api) {
  runLoadTests(api);
  runRuntimeTests(api);
  runEnvironmentTests(api);
}

module.exports = { runTests };
