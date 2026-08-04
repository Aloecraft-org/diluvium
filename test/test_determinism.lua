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

print("=== all determinism tests passed ===")
