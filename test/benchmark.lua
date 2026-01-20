local MATH_ITERS = 100000000
local STR_ITERS  = 100000
local FIB_N      = 35

local function bench_math()
    local sum = 0
    for i = 0, MATH_ITERS - 1 do
        if i % 2 == 0 then
            sum = sum + (i / 2)
        else
            sum = sum + (i * 3) + 1
        end
    end
    return sum
end

local function bench_string()
    local s = ""
    for i = 0, STR_ITERS - 1 do
        if #s > 100 then
            s = ""
        end
        s = s .. tostring(i)
    end
    return #s
end

local function fib(n)
    if n <= 1 then
        return n
    end
    return fib(n - 1) + fib(n - 2)
end

local function run_bench(name, func, param)
    local start_t = os.clock()
    local res
    if param then
        res = func(param)
    else
        res = func()
    end
    local end_t = os.clock()
    print(string.format("[%s] Result: %s | Time: %.6f sec", name, res, end_t - start_t))
end

print("--- Lua Benchmark ---")
run_bench("Math (Int Ops)", bench_math)
run_bench("String (Alloc)", bench_string)
run_bench("Recursion (Fib)", fib, FIB_N)
