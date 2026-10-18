// The Jak1KernelTest cases of test/goalc/test_goal_kernel.cpp, run in the headless wasm runtime
// (CLAUDE.MD, M4). Each case is a fresh runtime that links kernel-test.o and runs the case's
// function as the listener function (wasm/kernel_test.h); its output must match the x86 output.
//
// Build first (from the repo root):
//   build/WasmBackend/bin/goalc/goalc --game jak1 --instruction-set wasm --cmd \
//     '(begin (make-group "kernel") (asm-file "test/goalc/source_templates/kernel/kernel-test.gc" :color :write))'
//   cmake --build build-wasm
// Run: node test/goalc/wasm/kernel-test.mjs [case ...]

import { spawnSync } from "node:child_process";
import path from "node:path";
import { fileURLToPath } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../../..");
const runtime = path.join(root, "build-wasm/wasm/gk_headless.js");
const object = path.join(root, "out/jak1-wasm/obj/kernel-test.o");

const cases = {
  Basic: {
    func: "kernel-test",
    expected:
      "0\n" +
      "proc2: 0\n" +
      "TARGET FUNCTION 1 2 3\n" +
      "4 5 6\n" +
      "Stack Alignemnt 0/16\n" +
      "proc1: 0\n" +
      "proc2: 6\n" +
      "proc1: 1\n" +
      "proc2: 12\n" +
      "proc1: 2\n" +
      "proc2: 18\n" +
      "proc1: 3\n" +
      "proc2: 24\n" +
      "proc1: 4\n" +
      "proc2: 30\n" +
      "proc1: 5\n" +
      "DEACTIVATE PROC 1\n" +
      "proc2: 36\n" +
      "proc2: 42\n" +
      "proc2: 48\n" +
      "proc2: 54\n",
  },
  RunFunctionInProcess: {
    func: "kernel-test-2",
    expected:
      "0\n" +
      "Args: 1 2 3\n" +
      "4 5 6\n" +
      "Stack Alignemnt 0/16\n" +
      "run-function-in-process result: init-child-proc-result\n" +
      "Args: 0 2 3\n" +
      "4 5 6\n" +
      "Stack Alignemnt 0/16\n" +
      "run-function-in-process result: #f\n",
  },
  StateAndXmm: {
    func: "state-test",
    expected:
      "0\nenter wreck: 3 4 5 6\nwreck: 3 4 5 6\nenter check: 9 8 7 6\nrun xmm-check 12.3400 " +
      "45.6300 9 8 7 6\nwreck: 3 4 5 6\nrun xmm-check 12.3400 45.6300 9 8 7 6\nwreck: 3 4 5 6\nrun " +
      "xmm-check 12.3400 45.6300 9 8 7 6\nwreck: 3 4 5 6\n" +
      "exit check\nenter die\ntime to die!\nexit die\nexit wreck\nenter die\ntime to die!\nexit " +
      "die\n",
  },
  ThrowXmm: {
    func: "throw-backup-test",
    expected: "value now is 10.1000\nnow its 10.1000\n0\n",
  },
};

// kernel frames per case: every case is done well before this
const FRAMES = 30;
// a frame's prints (wasm/kernel_test.h)
const PRINT_PREFIX = "==== wasm-test print ";

const selected = process.argv.length > 2 ? process.argv.slice(2) : Object.keys(cases);
let failed = 0;
for (const name of selected) {
  const testCase = cases[name];
  if (!testCase) {
    console.error(`unknown case ${name}`);
    process.exit(2);
  }
  const run = spawnSync(
    process.execPath,
    [runtime, "-fakeiso", "-debug", "-nosound", "-wasm-test", object, testCase.func, String(FRAMES)],
    { cwd: root, encoding: "latin1", maxBuffer: 64 << 20 },
  );
  const out = run.stdout ?? "";
  const begin = out.indexOf("==== wasm-test begin\n");
  const end = out.indexOf("==== wasm-test end\n");
  // like the native test, only what the listener gets as prints (format #t), not format 0
  let got = null;
  if (begin >= 0 && end > begin) {
    got = out
      .slice(begin, end)
      .split("\n")
      .filter((line) => line.startsWith(PRINT_PREFIX))
      .map((line) => JSON.parse(line.slice(PRINT_PREFIX.length)))
      .join("");
  }
  if (got === testCase.expected) {
    console.log(`[ OK ] ${name}`);
  } else {
    failed++;
    console.log(`[FAIL] ${name} (exit ${run.status})`);
    console.log("--- expected\n" + testCase.expected + "--- got\n" + (got ?? "(no test output)\n"));
    if (got === null) {
      console.log("--- stdout tail\n" + out.slice(-3000) + "\n--- stderr tail\n" + (run.stderr ?? "").slice(-3000));
    }
  }
}
console.log(failed ? `${failed} of ${selected.length} failed` : `all ${selected.length} passed`);
process.exit(failed ? 1 : 0);
