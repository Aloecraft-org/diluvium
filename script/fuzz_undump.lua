-- Fuzz the bytecode loader.
--
-- Run:  diluvium_debug script/fuzz_undump.lua [rounds] [seed]
--
-- Loading a corrupt chunk must fail, not misbehave. Stock Lua's position is
-- that bytecode is trusted input, but Diluvium's is not quite that: secure
-- functions and the analysis report exist so a chunk you did not compile
-- can be handed to you and inspected, which means the loader is reachable
-- from things a user did not write.
--
-- This is a smoke-level fuzzer, not a campaign. Its value is that it runs
-- on every build under the debug binary, whose allocator asserts on a leak
-- at exit and whose GC checks are on -- so the failure modes it catches are
-- the quiet ones. Both loader bugs fixed in 5.5.1_build2 were found this
-- way: a scratch buffer leaked whenever a truncated chunk raised part-way
-- through a string, and a corrupt size field underflowed to a huge length.
--
-- What counts as a pass: 'load' either returns a function or returns nil
-- and a message. Anything else -- a crash, an assertion, a hang, a non-nil
-- non-function -- is a failure. A mutated chunk that happens to stay valid
-- and loads is fine and expected; it is not evidence of anything.

local rounds = tonumber(arg and arg[1]) or 2000
local seed = tonumber(arg and arg[2]) or 20260808

math.randomseed(seed)

-- Sources chosen to put variety into the dump: secure functions (scrambled
-- strings and code), nested closures, upvalues, long strings past the
-- short-string boundary, many constants, and varied debug info.
local SOURCES = {
    "return 1",
    "local x = 'hello' return x",
    "~function secret() return 'hidden-value' end return secret",
    [[
        local shared = "shared-literal";
        ~function a() return shared .. "-a" end
        local function b() return shared .. "-b" end
        return a, b
    ]],
    [[
        local t = {}
        for i = 1, 40 do t[i] = "constant-number-" .. i end
        local function outer(p)
            local q = p * 2
            return function (r) return q + r + #t end
        end
        return outer, t
    ]],
    [[
        local long = "]] .. string.rep("x", 300) .. [[";
        ~function keeper() return long end
        return keeper
    ]],
    [[
        local function f(a, b, ...)
            local c <const> = a + b
            local d <close> = setmetatable({}, {__close = function () end})
            switch c do
                case 1 then return "one"
                case 2, 3 then return "few"
                default return $"many {c}"
            end
        end
        return f
    ]],
}

local dumps = {}
for i, src in ipairs(SOURCES) do
    local f = assert(load(src, "=fuzz" .. i))
    dumps[#dumps + 1] = string.dump(f)
    dumps[#dumps + 1] = string.dump(f, true)   -- stripped: different layout
end

-- Mutations. Truncation and single-byte edits between them reach both the
-- "ran out of input" paths and the "field says something absurd" paths.
local function truncate(s)
    if #s < 2 then return s end
    return s:sub(1, math.random(1, #s - 1))
end

local function poke(s)
    local i = math.random(1, #s)
    local byte
    -- Bias towards values that are structurally meaningful -- 0, 1, small
    -- counts and 0xff -- rather than uniform noise, which mostly lands in
    -- string payloads where any byte is legal.
    local r = math.random(1, 10)
    if r <= 6 then byte = ({0, 1, 2, 3, 0x7f, 0xff})[math.random(1, 6)]
    else byte = math.random(0, 255) end
    return s:sub(1, i - 1) .. string.char(byte) .. s:sub(i + 1)
end

local function splice(s)
    if #s < 8 then return s end
    local a = math.random(1, #s - 4)
    local b = math.random(1, #s - 4)
    local n = math.random(1, 4)
    return s:sub(1, a - 1) .. s:sub(b, b + n - 1) .. s:sub(a + n)
end

local MUTATORS = {truncate, poke, splice}

local loaded, rejected, errors = 0, 0, {}

for round = 1, rounds do
    local s = dumps[math.random(1, #dumps)]
    -- Sometimes stack two mutations: one edit often lands somewhere inert.
    local n = (math.random(1, 4) == 1) and 2 or 1
    for _ = 1, n do
        s = MUTATORS[math.random(1, #MUTATORS)](s)
    end

    local ok, f, err = pcall(load, s, "=mutant")
    if not ok then
        -- load() itself raising, rather than returning nil, is a failure:
        -- the documented contract is that it reports errors by return.
        print(string.format("FAIL round %d: load() raised: %s", round, tostring(f)))
        os.exit(1)
    elseif f == nil then
        rejected = rejected + 1
        local key = tostring(err):match("%(([^)]+)%)$") or tostring(err)
        errors[key] = (errors[key] or 0) + 1
    elseif type(f) == "function" then
        -- Still valid after mutation. Do not call it: a mutated chunk can
        -- be well-formed and still describe nonsense, and executing it is
        -- outside what the loader promises.
        loaded = loaded + 1
    else
        print(string.format("FAIL round %d: load returned a %s", round, type(f)))
        os.exit(1)
    end
end

print(string.format("fuzz_undump: %d rounds, seed %d", rounds, seed))
print(string.format("  rejected %d, still-valid %d", rejected, loaded))

local keys = {}
for k in pairs(errors) do keys[#keys + 1] = k end
table.sort(keys, function (a, b) return errors[a] > errors[b] end)
for i = 1, math.min(#keys, 12) do
    print(string.format("  %6d  %s", errors[keys[i]], keys[i]))
end

-- A run where nothing was rejected would mean the mutations are landing
-- somewhere inert and the fuzzer is testing nothing.
if rejected == 0 then
    print("FAIL: no mutant was rejected; the mutations are not reaching the loader")
    os.exit(1)
end

print("fuzz_undump: OK")
