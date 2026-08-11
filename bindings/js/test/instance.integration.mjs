// The wasm wrapper against a real module.
//
// Not a `node --test` file, and not in `npm test`, because it needs
// `dist/diluvium_wasi.wasm` -- which needs the wasi-sdk in a container. It runs
// in the `js-binding` CI job only. See bindings/README.md for why that gap is
// stated rather than papered over.
//
// Usage: node test/instance.integration.mjs path/to/diluvium.wasm

import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";

import { Diluvium, Accepted, Busy } from "../src/index.js";

const path = process.argv[2];
if (!path) {
  console.error("usage: node test/instance.integration.mjs <diluvium.wasm>");
  process.exit(2);
}
const wasm = await readFile(path);

let checks = 0;
const check = (cond, what) => {
  checks++;
  assert.ok(cond, what);
  console.log(`[PASS] ${what}`);
};

// -- a program runs -----------------------------------------------------------
{
  const inst = await Diluvium.load(wasm, "return 1", { name: "trivial" });
  check(inst.run().done, "a program with no waits finishes");
  inst.close();
}

// -- the layout the wrapper reads is the runtime's, not a guess ---------------
{
  const inst = await Diluvium.load(wasm, "return 1", { name: "layout" });
  // If dv_layout were wrong, every struct read below would be garbage. Reading a
  // known queue's capacity is the cheapest end-to-end check of it.
  const inbox = inst.queue("inbox");
  check(inbox !== null, "inbox exists");
  check(inst.queueInfo(inbox).capacity === 64, "and its capacity reads as 64");
  check(inst.queueInfo(inbox).exported === true, "and it is exported");
  inst.close();
}

// -- errors cross with their traceback ---------------------------------------
{
  const inst = await Diluvium.load(
    wasm,
    "local function inner() error('boom', 0) end inner()",
    { name: "bad" }
  );
  let msg = "";
  try {
    inst.run();
  } catch (e) {
    msg = String(e.message);
  }
  check(msg.includes("boom"), "the message crosses linear memory");
  check(msg.includes("stack traceback"), "with its traceback");
  check(msg.includes("inner"), "naming the failing frame");
  inst.close();
}

// -- messages both ways, through a park --------------------------------------
{
  const inst = await Diluvium.load(
    wasm,
    `
    local inbox = queue.lookup("inbox")
    local outbox = queue.lookup("outbox")
    while true do
      local _, order = queue.wait({inbox})
      if order == "done" then return end
      queue.push(outbox, { id = order.id, total = order.qty * 3 })
    end
    `,
    { name: "orders" }
  );
  const inbox = inst.queue("inbox");
  const outbox = inst.queue("outbox");
  let step = inst.run();
  check(step.parked, "the program parks");
  check(step.wait.ids.length === 1 && step.wait.ids[0] === inbox,
        "on its inbox");
  check(step.wait.timeoutMs === null, "with no timeout");
  check(step.wait.forWrite === false, "waiting for a message, not for space");

  const got = [];
  for (const n of [1, 2, 3]) {
    check(inst.push(inbox, { id: n, qty: n * 2 }) === Accepted.OK,
          `push ${n} accepted`);
    step = inst.resume(inbox);
    got.push(inst.pop(outbox));
  }
  assert.deepEqual(got, [
    { id: 1, total: 6 },
    { id: 2, total: 12 },
    { id: 3, total: 18 },
  ]);
  checks++;
  console.log("[PASS] objects round-trip with their field names intact");

  // A resume that parks again must report the right wait-set: dv_resume cannot,
  // so the wrapper reads it back.
  check(step.parked && step.wait.ids[0] === inbox,
        "a resume that parks again still reports its wait-set");
  inst.push(inbox, "done");
  check(inst.resume(inbox).done, "and the program finishes when told to");
  inst.close();
}

// -- the host owns the clock -------------------------------------------------
{
  const inst = await Diluvium.load(
    wasm,
    `
    local q = queue.declare('t')
    local _, _, status = queue.wait({q}, 5000)
    local out = queue.declare('res', {exported = true})
    queue.push(out, status)
    `,
    { name: "clock" }
  );
  const step = inst.run();
  check(step.wait.timeoutMs === 5000, "the program's timeout crosses as 5000ms");
  const t0 = Date.now();
  check(inst.resumeTimeout().done, "the host says the time is up");
  check(Date.now() - t0 < 1000, "without waiting for it");
  check(inst.pop(inst.queue("res")) === "timeout", "and the program saw a timeout");
  inst.close();
}

// -- a park for space asks to be drained -------------------------------------
{
  const inst = await Diluvium.load(
    wasm,
    `
    local q = queue.declare('b', {capacity = 1, on_full = 'block', exported = true})
    queue.push(q, 1)
    queue.push(q, 2)
    `,
    { name: "backpressure" }
  );
  const step = inst.run();
  check(step.wait.forWrite === true, "for_write tells 'drain me' from 'feed me'");
  const q = step.wait.ids[0];
  check(inst.pop(q) === 1, "the host drains a slot");
  check(inst.resume(q).done, "and the parked push completes");
  inst.close();
}

// -- misuse is reported, not undefined ---------------------------------------
{
  const inst = await Diluvium.load(
    wasm,
    "local q = queue.lookup('inbox') queue.wait({q})",
    { name: "parked" }
  );
  inst.run();
  let busy = false;
  try {
    inst.run();
  } catch (e) {
    busy = e instanceof Busy;
  }
  check(busy, "running a parked program is refused");
  inst.close();
}

// -- bytecode is refused unless asked for ------------------------------------
{
  const fake = new Uint8Array([0x1b, 0x4c, 0x75, 0x61, 0x55, 0x46]);
  let refused = false;
  try {
    await Diluvium.load(wasm, fake, { name: "bin" });
  } catch {
    refused = true;
  }
  check(refused, "a precompiled chunk is refused by default");
}

// -- what a program is allowed to reach --------------------------------------
//
// A JS host had no way to set either switch until 5.5.1_build4's flags were
// plumbed through: the wrapper passed only TEXT_ONLY, so "run a program you did
// not write" was reachable from C and not from here. These run in the
// js-binding CI job only, because they need a real module -- which is also why
// the flag arithmetic in index.js is not covered by `npm test`.
{
  const inst = await Diluvium.load(
    wasm,
    "assert(os and io and package) return 1",
    { name: "open" }
  );
  check(inst.run().done, "a default instance is not sealed");
  inst.close();
}

{
  const inst = await Diluvium.load(
    wasm,
    `assert(os == nil and io == nil and package == nil)
     assert(dofile == nil and loadfile == nil)
     return 1`,
    { name: "sealed", sealed: true }
  );
  check(inst.run().done, "a sealed instance reaches nothing outside itself");
  inst.close();
}

{
  // Sealing must not come to mean "unusable".
  const inst = await Diluvium.load(
    wasm,
    `local q = queue.declare("out", {exported = true})
     queue.push(q, ("x"):rep(3) .. tostring(math.floor(2.5)))
     return 1`,
    { name: "sealed-works", sealed: true }
  );
  check(inst.run().done, "and a sealed instance still runs");
  check(
    inst.pop(inst.queue("out")) === "xxx2",
    "and still has the language and its queues"
  );
  inst.close();
}

{
  const narrow = await Diluvium.load(
    wasm,
    "assert(not pcall(debug.getregistry)) return 1",
    { name: "narrow" }
  );
  check(narrow.run().done, "debug is narrowed by default");
  narrow.close();
  const wide = await Diluvium.load(
    wasm,
    "assert(type(debug.getregistry()) == 'table') return 1",
    { name: "wide", unsafeDebug: true }
  );
  check(wide.run().done, "and unsafeDebug puts the whole library back");
  wide.close();
}

console.log(`\n${checks} checks, 0 failed`);
