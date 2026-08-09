-- Load-time bytecode verifier (src/lverify.c).
--
-- The verifier runs from luai_verifycode, which luaU_undump calls and
-- nothing else does. That means a test written the ordinary way -- source
-- in, behaviour out -- never reaches it: the whole suite can pass with the
-- verifier rejecting every numeric for loop in the language, which is
-- exactly what happened while this was being written.
--
-- So the shape of this file is deliberate. Everything here goes through
-- string.dump and back, because that is the only path that runs the code
-- under test.

print("testing bytecode verifier")

local function roundtrip (src)
  local f = assert(load(src), "source did not compile: " .. src)
  local chunk = string.dump(f)
  local g, err = load(chunk, "verify", "b")
  assert(g, "verifier rejected legal bytecode: " .. src .. "\n  " ..
            tostring(err))
  return g
end


-- ---------------------------------------------------------------------
-- Every construct the compiler emits has to survive a round trip. A false
-- rejection here is worse than a missed check: it breaks working programs.
-- ---------------------------------------------------------------------

-- The loop forms are listed one variable at a time because the operand
-- bound is a function of the variable count, and an off-by-one only shows
-- up when the frame ends exactly at the last slot the loop touches.
local shapes = {
  -- numeric for: a three-slot control block in 5.5, four in 5.4
  "for i = 1, 10 do end",
  "for i = 1, 10, 2 do end",
  "for i = 10, 1, -1 do end",
  "for i = 1.5, 3.5, 0.5 do end",
  "local s = 0 for i = 1, 10 do s = s + i end return s",
  "for i = 1, 3 do for j = 1, 3 do end end",
  -- generic for, by number of loop variables
  "for a in next, {} do end",
  "for a, b in next, {} do end",
  "for a, b, c in next, {} do end",
  "for a, b, c, d in next, {} do end",
  "for a, b, c, d, e in next, {} do end",
  "for a, b, c, d, e, g, h in next, {} do end",
  "for k, v in pairs({1, 2, 3}) do end",
  "for _ in ipairs({}) do end",
  -- tests and jumps: every comparison has a jump behind it
  "local a, b = 1, 2 if a == b then return 1 end",
  "local a, b = 1, 2 if a < b then return 1 else return 2 end",
  "local a = 1 if a ~= 1 then return 1 end",
  "local a = 1 while a < 10 do a = a + 1 end return a",
  "local a = 1 repeat a = a + 1 until a > 5 return a",
  "local a = 1 return a and 2 or 3",
  "local a = nil return a or 1",
  "local a = 1 return not a",
  "do goto done ::done:: end",
  -- calls and returns, fixed and multiple
  "local function f() return 1, 2, 3 end return f()",
  "local function f(...) return ... end return f(1, 2)",
  "local function f(a, ...) return a, ... end return f(1, 2, 3)",
  "local function f() return select('#') end return f()",
  "local function f(a) return f(a) end",              -- tail call
  "local function f(...) return (...) end return f(1)",
  "return (function(...) local a, b = ... return a end)(1, 2)",
  -- table constructors: SETLIST with and without an extra argument
  "return {}",
  "return {1, 2, 3}",
  "return {a = 1, b = 2}",
  "return {1, 2, a = 3}",
  "local t = {} for i = 1, 100 do t[i] = i end return t",
  "return {(function() return 1, 2 end)()}",           -- open-ended SETLIST
  -- upvalues, closures and nesting
  "local x = 1 return function() return x end",
  "local x = 1 return function() return function() return x end end",
  "local x = 1 local function f() x = x + 1 end f() return x",
  "return function(a) return function(b) return function(c) return a+b+c end end end",
  -- to-be-closed
  "local t = setmetatable({}, {__close = function() end}) do local x <close> = t end",
  -- arithmetic, so every MMBIN variant appears
  "local a, b = 1, 2 return a + b, a - b, a * b, a / b, a // b, a % b, a ^ b",
  "local a = 1 return a + 1, a - 1, a * 2, a // 2, a % 2",
  "local a, b = 1, 2 return a & b, a | b, a ~ b, a << b, a >> b, ~a",
  "local a = 1 return a << 1, a >> 1",
  "return 1 .. 2 .. 3",
  "local t = {} return #t",
  -- constants past the operand limit, forcing LOADKX/EXTRAARG
  (function ()
    -- distinct constants, not distinct locals: past 256 the compiler has
    -- to reach for LOADKX and an EXTRAARG word
    local parts = {"local t = {}"}
    for i = 1, 300 do
      parts[#parts + 1] = string.format("t.f%d = %q", i, "konstant" .. i)
    end
    parts[#parts + 1] = "return t"
    return table.concat(parts, " ")
  end)(),
  -- field access and method calls
  "local t = {f = function() end} t.f() t:f()",
  "local t = {} t.a = 1 t['b'] = 2 t[1] = 3 return t.a, t.b, t[1]",
  -- Diluvium's own syntax, which desugars before the verifier sees it
  "local name = 'x' return $\"hello {name}\"",
  "local x = nil return x ?? 1",
  "local t = nil return t?.a",
  "local n = 1 n += 1 n *= 2 return n",
  "local x = 1 switch x do case 1 then return 'one' default return 'other' end",
  "do defer print('x') end",
  "local t = setmetatable({}, {__close = function() end}) with a = t do end",
  "~function secret() return 'hidden' end return secret",
  -- a long straight-line body, to force many line-info and absolute-line
  -- entries: the debug-array checks only mean anything on a function big
  -- enough to have more than one absolute entry (MAXIWTHABS apart)
  (function ()
    local parts = {"local x = 0"}
    for i = 1, 400 do parts[#parts + 1] = "x = x + " .. i end
    parts[#parts + 1] = "return x"
    return table.concat(parts, "\n")   -- newlines, so line info advances
  end)(),
  -- a big table with hash and array parts, to push NEWTABLE's sizes
  (function ()
    local parts = {"return {"}
    for i = 1, 200 do parts[#parts + 1] = i .. "," end
    for i = 1, 200 do parts[#parts + 1] = "k" .. i .. " = " .. i .. "," end
    parts[#parts + 1] = "}"
    return table.concat(parts, "\n")
  end)(),
  -- an array past 1023 elements, which is what makes NEWTABLE set its k
  -- flag and spill the array size into an OP_EXTRAARG: the case the
  -- verifier's k-and-extra-argument check has to accept
  (function ()
    local parts = {"return {"}
    for i = 1, 1500 do parts[#parts + 1] = i .. "," end
    parts[#parts + 1] = "}"
    return table.concat(parts, "\n")
  end)(),
}

for _, src in ipairs(shapes) do
  roundtrip(src)
end
print("+")


-- Real compiler output at scale: every file the suite already has is a
-- corpus of legal bytecode, and a verifier that rejects any of it is wrong
-- about the compiler rather than about the chunk.
do
  local files = {
    "bitwise.lua", "calls.lua", "closure.lua", "constructs.lua",
    "events.lua", "literals.lua", "locals.lua", "math.lua", "nextvar.lua",
    "sort.lua", "strings.lua", "tpack.lua", "utf8.lua", "vararg.lua",
    "test_interop.lua", "test_switch.lua", "test_defer.lua",
    "test_fstrings.lua", "test_safenav.lua", "test_compound.lua",
  }
  local checked = 0
  for _, name in ipairs(files) do
    local f = loadfile(name)
    if f then  -- absent files are not this test's problem
      local chunk = string.dump(f)
      local g, err = load(chunk, name, "b")
      assert(g, "verifier rejected compiled " .. name .. ": " .. tostring(err))
      -- and stripped, which drops the debug arrays the verifier also sizes
      local stripped = string.dump(f, true)
      local h, serr = load(stripped, name, "b")
      assert(h, "verifier rejected stripped " .. name .. ": " .. tostring(serr))
      checked = checked + 1
    end
  end
  assert(checked > 0, "no suite files were available to round-trip")
end
print("+")


-- ---------------------------------------------------------------------
-- The other half: a corrupt chunk must be refused, and refused as an
-- error rather than by dying. This cannot assert on *which* byte does
-- what -- that is the fuzzer's job (script/fuzz_exec.py) -- only that
-- the process survives to report it.
-- ---------------------------------------------------------------------

-- This half deliberately LOADS every mutant and runs none of them.
-- Loading is where the verifier works, and it is safe in-process: it
-- either accepts or raises. Running a mutant is not safe in-process --
-- a chunk that survives the verifier and then crashes takes the whole
-- suite down with it, which is exactly why script/fuzz_exec.py runs each
-- mutant in its own process. Execution-time survival is measured there;
-- what is measured here is that the loader never dies on the way in, and
-- that it is actually refusing things rather than waving them through.
do
  local base = string.dump(assert(load(
    "local function f(a, b) local t = {a, b} return t[1] + t[2] + #t end " ..
    "return f(1, 2)")))
  local refused, accepted = 0, 0
  -- Walk the whole dump rather than sampling: deterministic, and it
  -- covers the header, the code array, the constants and the debug
  -- arrays alike.
  for pos = 1, #base do
    for _, byte in ipairs({0, 1, 0x7f, 0xff}) do
      local mutant = base:sub(1, pos - 1) .. string.char(byte) ..
                     base:sub(pos + 1)
      if mutant ~= base then
        if load(mutant, "mutant", "b") == nil then
          refused = refused + 1
        else
          accepted = accepted + 1
        end
      end
    end
  end
  assert(refused > 0, "no mutation of the dump was refused")
  -- A verifier that accepted everything would still pass the assert
  -- above, because the loader refuses plenty on its own, so pin the rate
  -- as well. Measured on this dump: the loader alone refuses 19.2%
  -- (238/1241) and the loader plus the verifier refuses 32.9%
  -- (408/1241). 25% sits between them, so this fails if luai_verifycode
  -- is ever quietly emptied again rather than only when the fuzzer runs.
  -- It is a floor, not a target -- most single-byte mutations land in
  -- string or debug data and produce a chunk that is genuinely fine.
  local rate = refused / (refused + accepted)
  assert(rate > 0.25, string.format(
    "only %.1f%% of mutants were refused; is luai_verifycode wired in?",
    rate * 100))
  print(string.format("+ (%d refused, %d accepted, %.1f%% refused)",
                      refused, accepted, rate * 100))
end

print("OK")
