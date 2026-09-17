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
