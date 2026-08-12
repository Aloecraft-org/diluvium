// The swarm module against a real host, in JavaScript.
//
// Not a `node --test` file, not in `npm test`, and -- unlike
// instance.integration.mjs -- NOT yet wired into CI either: this file has
// never executed anywhere, because building its module needs the wasi-sdk
// container, and a check that has never passed does not gate anything on its
// first run (the verify_wasm lesson, doc/Messaging.md 18.3). Wire it into the
// js-binding job after its first green run.
//
// What it proves when it runs: the swarm layer is really in the module, the
// env trampolines round-trip (dvsjs_new -> dvs_step -> js_host_drive -> a
// reentrant dv_run back into the module), and a root program runs to
// completion under a host written in JavaScript -- the smallest instance of
// doc/Host.md's claim that a host is a set of duties, not a C program.
//
// Usage: node test/swarm.integration.mjs path/to/diluvium_swarm_wasi.wasm

import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";

import { instantiate, setSwarmHost } from "../src/index.js";

const path = process.argv[2];
if (!path) {
  console.error("usage: node test/swarm.integration.mjs <diluvium_swarm_wasi.wasm>");
  process.exit(2);
}
const wasm = await readFile(path);

let checks = 0;
const check = (cond, what) => {
  checks++;
  assert.ok(cond, what);
  console.log(`[PASS] ${what}`);
};

const mod = await instantiate(wasm);
const ex = mod.exports;
const mem = () => new DataView(ex.memory.buffer); // re-made per use: memory can grow

check(typeof ex.dvsjs_new === "function", "the module exports dvsjs_new");
check(typeof ex.dvs_step === "function", "and the dvs_* surface");

// dv_status values the drive loop below needs (dv.h's enum, same source the
// wrapper's constants come from).
const DV_IDLE = 6;
const DV_DONE = 7;

// A cstring in linear memory, caller frees.
const cstr = (s) => {
  const bytes = new TextEncoder().encode(s);
  const p = ex.malloc(bytes.length + 1);
  new Uint8Array(ex.memory.buffer, p, bytes.length).set(bytes);
  new Uint8Array(ex.memory.buffer, p + bytes.length, 1)[0] = 0;
  return [p, bytes.length];
};

// -- a swarm comes up under a JS host ----------------------------------------
const driven = [];
setSwarmHost(mod, {
  create(id) {
    driven.push(`create:${id}`);
    return 0;
  },
  destroy(id) {
    driven.push(`destroy:${id}`);
  },
  drive(id, inst) {
    driven.push(`drive:${id}`);
    // The minimal drive of doc/Host.md's duty 2: run it; a park would need a
    // queue decision this root never makes; anything else is finished.
    const ws = ex.malloc(64);
    try {
      const st = ex.dv_run(inst, ws);
      if (st === DV_IDLE) throw new Error("the trivial root parked, which it cannot");
      return 0; // DV_DONE or an error: either way this instance is finished
    } finally {
      ex.free(ws);
    }
  },
});

const sw = ex.dvsjs_new(8, 2);
check(sw !== 0, "dvsjs_new builds a swarm through the env trampolines");

{
  const src = "return 0";
  const [code, codeLen] = cstr(src);
  const out = ex.malloc(4);
  // No capabilities, no budget: the smallest legitimate root.
  const st = ex.dvs_root(sw, code, codeLen, 0, 0, 0n, 0n, out);
  check(st === 0, "a root program loads (DVS_OK)");
  const root = mem().getUint32(out, true);
  check(root !== 0, "and has an id");
  ex.free(out);
  ex.free(code);

  check(ex.dvs_alive(sw) === 1, "one instance alive before the step");
  ex.dvs_step(sw);
  check(driven.includes(`drive:${root}`), "dvs_step drove it through the JS trampoline");
  check(ex.dvs_alive(sw) === 0, "and the swarm drained to zero");
}

ex.dvs_free(sw);
console.log(`\n${checks} checks, 0 failed`);
