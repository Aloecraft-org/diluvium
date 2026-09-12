// web/test.mjs -- exercise the browser build through its own loader.
//
//   make build_browser && node web/test.mjs
//
// Uses only Node built-ins and web/diluvium.js (no npm). It is the same
// surface a page uses, so a break here is a break in the shipped artifact.
// Pass a different module path as the first argument.

import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { loadDiluvium } from "./diluvium.js";

const wasm = process.argv[2] || fileURLToPath(new URL("./diluvium_browser.wasm", import.meta.url));
const dl = await loadDiluvium(readFileSync(wasm));

let pass = 0, fail = 0;
const ok = (cond, name, detail = "") => {
  if (cond) { pass++; console.log("[PASS]", name, detail); }
  else { fail++; console.log("[FAIL]", name, detail); }
};
const out = (src) => dl.eval(src);

// -- version ---------------------------------------------------------------
ok(/^5\.5\.1/.test(dl.version()), "version() is a 5.5.1 build", dl.version());

// -- real libc: numbers format through wasi-libc's snprintf ----------------
let r = out('print(1/3, ("%.4f"):format(math.pi))');
ok(r.ok && r.output.includes("0.33333") && r.output.includes("3.1416"),
   "real snprintf (not the old empty stub)", JSON.stringify(r.output.trim()));

// -- the language, in the browser ------------------------------------------
r = out('local t={} for i=1,5 do if i%2==0 then continue end t[#t+1]=$"{i}" end print(table.concat(t,","))');
ok(r.ok && r.output.trim() === "1,3,5", "continue + string interpolation");

r = out('print((`(\\d+)-(\\d+)`):match("order 12-34"))');
ok(r.ok && r.output.trim() === "12\t34", "regex literal");

// -- real allocator: allocate well past any fixed 8 MB heap ----------------
r = out('local t={} for i=1,200000 do t[i]=tostring(i) end print(#table.concat(t))');
ok(r.ok && /^\d+$/.test(r.output.trim()), "200k-element join (past a bump heap)", r.output.trim());

// -- errors are caught, not traps ------------------------------------------
r = out('print(pcall(function() error("boom") end))');
ok(r.ok && /false/.test(r.output) && /boom/.test(r.output), "pcall catches an error (EH works)");

r = out('local ok,e = pcall(function() return nil + 1 end) print(ok, e)');
ok(r.ok && /false/.test(r.output), "pcall catches an arithmetic error");

r = out('error("top level")');
ok(!r.ok && /top level/.test(r.output), "a top-level error is reported");
r = out('print("alive: " .. (2+2))');
ok(r.ok && /alive: 4/.test(r.output), "the module is usable after an error");

// -- compile: source -> bytecode -------------------------------------------
const bc = dl.compile("local x = 41 return x + 1");
ok(bc instanceof Uint8Array && bc.length > 8, "compile returns bytecode", bc ? bc.length + " bytes" : "null");
ok(bc && bc[0] === 0x1b && bc[1] === 0x4c && bc[2] === 0x75 && bc[3] === 0x61,
   "bytecode carries the Lua signature (1b 4c 75 61)");
ok(dl.compile("local x = = =") === null, "a syntax error compiles to null");

// -- analyze: source -> the JSON report ------------------------------------
const rep = dl.analyze("local function add(a, b) return a + b end\nreturn add");
ok(rep && Array.isArray(rep.functions), "analyze returns a report");
ok(rep && rep.functions.some(f => Array.isArray(f.param_names) && f.param_names.join(",") === "a,b"),
   "the report sees add(a, b)'s parameters");

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
