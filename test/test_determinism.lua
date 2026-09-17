-- Determinism tests.
--
-- Diluvium fixes the string hash seed (see luai_makeseed in luaconf.h),
-- so iteration order over string keys is identical across processes.
-- Stock Lua derives the seed from ASLR addresses and the clock, making
-- 'pairs' order vary run to run -- unacceptable for consensus execution.

print("=== determinism tests ===")

local function key_order()
  local t = {}
  for _, k in ipairs{"alpha", "beta", "gamma", "delta", "epsilon",
                     "zeta", "eta", "theta", "iota", "kappa"} do
    t[k] = true
  end
  local o = {}
  for k in pairs(t) do o[#o + 1] = k end
  return table.concat(o, ",")
end

-- Within one process, rebuilding the same table gives the same order.
local first = key_order()
for i = 1, 5 do
  assert(key_order() == first, "iteration order unstable within a process")
end

-- Across processes: spawn this interpreter twice with a fresh state each
-- time and compare. This is the assertion stock Lua fails.
local interp = arg and arg[-1]
if interp and io.popen then
  local snippet =
    'local t={} for _,k in ipairs{"alpha","beta","gamma","delta",' ..
    '"epsilon","zeta","eta","theta","iota","kappa"} do t[k]=true end ' ..
    'local o={} for k in pairs(t) do o[#o+1]=k end print(table.concat(o,","))'
  local function spawn()
    local p = assert(io.popen(string.format("%q -e %q", interp, snippet)))
    local out = p:read("l")
    p:close()
    return out
  end
  local a, b = spawn(), spawn()
  assert(a ~= nil and a ~= "", "child interpreter produced no output")
  assert(a == b, "iteration order differs between processes:\n  " ..
                 a .. "\n  " .. tostring(b))
  assert(a == first, "child process order differs from this process")
else
  print("  (no io.popen or arg[-1]; cross-process check skipped)")
end

-- Keys that are tables, closures, coroutines or userdata hash by a
-- per-state creation counter (ltable.c), not by their address, so the
-- order 'pairs' visits them in is a property of the program rather than
-- of where the allocator put the objects. Stock Lua hashes them by
-- address, and this is the assertion it fails under ASLR: two fresh
-- processes running the same program visit the same object keys in a
-- different order. Compared child against child, because the order is a
-- function of creation order and this process has a history of its own.
local objkeys =
  'local t, o, ks = {}, {}, {} ' ..
  'for i = 1, 4 do ks[#ks + 1] = {} end ' ..
  'for i = 1, 4 do ks[#ks + 1] = function() return i end end ' ..
  'for i = 1, 3 do ks[#ks + 1] = coroutine.create(print) end ' ..
  'if io and io.tmpfile then for i = 1, 2 do ks[#ks + 1] = io.tmpfile() end end ' ..
  'for i, k in ipairs(ks) do t[k] = i end ' ..
  'for _, v in pairs(t) do o[#o + 1] = v end ' ..
  'io.write(table.concat(o, ","), "\\n")'
if interp and io.popen then
  local function spawn()
    local p = assert(io.popen(string.format("%q -e %q", interp, objkeys)))
    local out = p:read("l")
    p:close()
    return out
  end
  local a, b = spawn(), spawn()
  assert(a ~= nil and a ~= "", "child interpreter produced no object-key order")
  assert(select(2, a:gsub(",", ",")) >= 10, "expected at least eleven keys: " .. a)
  assert(a == b, "iteration order over object keys differs between processes:\n  "
                 .. a .. "\n  " .. tostring(b))
end

-- math.random is seeded from the same constant. lmathlib.c seeds its
-- generator at open time with luaL_makeseed, and lauxlib.c's luaL_makeseed
-- returns luai_makeseed() -- which luaconf.h pins to "DILU" for the string
-- hash. So the first draw from a fresh state is the same number in every
-- process on every platform, and math.randomseed() with no arguments
-- reseeds from the same constant rather than from the clock. Nothing in
-- the upstream suite pins the default seed (it always seeds explicitly),
-- so this is where a change to luaconf.h that handed entropy back would
-- show up. The two draws below were taken from a fresh interpreter -- the
-- first as raw IEEE bits, so no libc's %a spelling is involved -- and they
-- are the "DILU"-seeded xoshiro256** stream: they move only if the seed,
-- the seeding, or the generator does.
do
  local s1 = math.randomseed()
  assert(s1 == 0x44494C55,
         string.format("math.randomseed() reseeds from %#x, not \"DILU\"", s1))
  if interp and io.popen then
    local function firstdraws()
      local p = assert(io.popen(string.format(
        '%q -e "io.write((\'%%016x\'):format(string.unpack(\'<I8\', string.pack(\'<d\', math.random()))), \' \', math.random(1, 1000000))"',
        interp)))
      local out = p:read("l")
      p:close()
      return out
    end
    local a, b = firstdraws(), firstdraws()
    assert(a ~= nil and a ~= "", "child interpreter produced no draws")
    assert(a == b, "math.random's first draws differ between processes:\n  "
                   .. a .. "\n  " .. tostring(b))
    assert(a == "3fe1307f39ad595c 360252",
           "math.random's default stream moved: " .. a)
  end
end

print("=== all determinism tests passed ===")
