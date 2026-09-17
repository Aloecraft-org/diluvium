-- test_array.lua
-- Verifies the 'array' library (numeric spec stage 0): the value's shape
-- and dtypes, the kernels, and the four rules that make every answer here
-- the same on every target -- the canonical reduction order, the total
-- ordering with NaN last, first-appearance group ids, and a fixed hash.
--
-- The bit-exactness of the answers is test/numeric/corpus.lua, which is
-- diffed across targets. This file is about meaning: that 'sum' sums, that
-- a view sees the buffer, that '//' matches Lua's own.

local function assert_eq(actual, expected, name)
    if actual == expected then
        print(string.format("[PASS] %s", name))
    else
        print(string.format("[FAIL] %s", name))
        print(string.format("       Expected: '%s'", tostring(expected)))
        print(string.format("       Actual:   '%s'", tostring(actual)))
        os.exit(1)
    end
end

local function assert_err(fn, want, name)
    local ok, err = pcall(fn)
    if ok then
        print(string.format("[FAIL] %s (no error raised)", name))
        os.exit(1)
    elseif want ~= nil and not tostring(err):find(want, 1, true) then
        print(string.format("[FAIL] %s", name))
        print(string.format("       Expected an error containing: '%s'", want))
        print(string.format("       Got: '%s'", tostring(err)))
        os.exit(1)
    else
        print(string.format("[PASS] %s", name))
    end
end

local function list(a) return table.concat(array.to_table(a), ",") end

print("=== Starting Array Tests ===\n")

print("-- 1. Construction and dtypes")
assert_eq(array.dtype(array.from{1.0, 2.0}), "f64", "a table of floats is f64")
assert_eq(array.dtype(array.from{1, 2}), "i64", "a table of integers is i64")
assert_eq(array.dtype(array.from{1, 2.0}), "f64", "one float makes it f64")
assert_eq(array.dtype(array.from({1, 2}, "u8")), "u8", "an explicit dtype wins")
assert_eq(list(array.from{1, 2, 3}), "1,2,3", "values round-trip")
assert_eq(array.size(array.new("f64", 3, 4)), 12, "a 2D array's size")
assert_eq(list(array.zeros("i64", 3)), "0,0,0", "zeros")
assert_eq(list(array.ones("i64", 3)), "1,1,1", "ones")
assert_eq(list(array.arange("i64", 0, 5)), "0,1,2,3,4", "arange is half-open")
assert_eq(list(array.arange("i64", 5, 0, -2)), "5,3,1", "arange counts down")
assert_eq(list(array.arange("i64", 5, 5)), "", "an empty range is empty")
assert_eq(list(array.linspace(0, 1, 3)), "0.0,0.5,1.0", "linspace includes both ends")
assert_eq(array.get(array.linspace(0, 7, 8), 8), 7.0,
          "and its last element is the endpoint exactly")
assert_err(function() return array.new("f32", 1) end, "unknown dtype",
           "an unknown dtype is refused")
assert_eq(list(array.cast(array.from{1.9, -1.9}, "i64")), "1,-1",
          "casting to i64 truncates toward zero, as Lua does")

print("-- 2. Shape, indexing and length")
local m = array.from{{1, 2, 3}, {4, 5, 6}}
assert_eq(#m, 2, "'#' is the first dimension")
assert_eq(select(2, array.shape(m)), 3, "shape reports both")
assert_eq(array.get(m, 2, 3), 6, "get with two indices")
assert_eq(m[2][3], 6, "a 2D array indexes to a row view")
assert_eq(#array.from{1, 2, 3, 4}, 4, "'#' of a 1D array is its length")
assert_eq(array.from{7, 8, 9}[2], 8, "a 1D array indexes to an element")
assert_err(function() return array.from{1}[2] end, "outside",
           "an out-of-range index is refused")
local w = array.from{1, 2, 3}
w[2] = 20
assert_eq(list(w), "1,20,3", "assignment writes an element")
array.set(m, 1, 1, 99)
assert_eq(array.get(m, 1, 1), 99, "and 'set' writes a 2D one")
assert_err(function() return array.dot(array.from{1.0}, array.from{1.0, 2.0}) end,
           "lengths differ", "a length mismatch is refused")
assert_err(function() return array.from{1, 2} + array.from{1, 2, 3} end,
           "shape mismatch", "a shape mismatch is refused")

print("-- 3. Elementwise, and the scalar broadcast")
local a = array.from{1.0, 2.0, 3.0, 4.0}
assert_eq(list(a + a), "2.0,4.0,6.0,8.0", "add")
assert_eq(list(a - a), "0.0,0.0,0.0,0.0", "sub")
assert_eq(list(a * 2), "2.0,4.0,6.0,8.0", "a scalar broadcasts")
assert_eq(list(2 * a), "2.0,4.0,6.0,8.0", "on either side")
assert_eq(list(-a), "-1.0,-2.0,-3.0,-4.0", "unary minus")
assert_eq(array.dtype(array.from{1, 2} / array.from{1, 2}), "f64",
          "'/' is a float operator, as in Lua")
assert_eq(array.dtype(array.from{1, 2} ^ array.from{1, 2}), "f64",
          "and so is '^'")
assert_eq(array.dtype(array.from{1, 2} + array.from{1, 2}), "i64",
          "integer arithmetic stays integer")
-- The integer operators are Lua's, not C's: floor division and a modulo
-- that takes the divisor's sign.
local xs, ys = {-7, 7, -7, 7}, {2, 2, -2, -2}
for k = 1, #xs do
    assert_eq(array.get(array.idiv(array.from(xs), array.from(ys)), k),
              xs[k] // ys[k], string.format("%d // %d", xs[k], ys[k]))
    assert_eq(array.get(array.mod(array.from(xs), array.from(ys)), k),
              xs[k] % ys[k], string.format("%d %% %d", xs[k], ys[k]))
end
assert_err(function() return array.idiv(array.from{1}, array.from{0}) end,
           "n//0", "integer division by zero raises, as Lua does")
assert_eq(array.dtype(array.gt(a, 2.0)), "u8", "a comparison returns a mask")
assert_eq(list(array.gt(a, 2.0)), "0,0,1,1", "and the mask is right")
assert_eq(list(array.eq(array.from{1, 2, 3}, array.from{1, 5, 3})), "1,0,1", "eq")

print("-- 4. Reductions, in the canonical order")
assert_eq(array.sum(a), 10.0, "sum")
assert_eq(array.mean(a), 2.5, "mean")
assert_eq(array.min(a), 1.0, "min")
assert_eq(array.max(a), 4.0, "max")
assert_eq(array.prod(a), 24.0, "prod")
assert_eq(array.var(a), 1.25, "var is the population variance")
assert_eq(array.std(a), math.sqrt(1.25), "std is its square root")
assert_eq(array.argmin(a), 1, "argmin is 1-based")
assert_eq(array.argmax(a), 4, "argmax too")
assert_eq(array.argmin(array.from{2, 1, 1}), 2, "and ties keep the lowest index")
assert_eq(list(array.cumsum(a)), "1.0,3.0,6.0,10.0", "cumsum")
assert_eq(array.sum(array.from{1, 2, 3}), 6, "an integer sum stays exact")
assert_eq(array.sum(array.from{}), 0, "an empty sum is zero")
assert_eq(array.min(array.from{}), nil, "an empty min has no answer")
-- The order is not the naive one, and this is the test that says so: a
-- left-to-right sum absorbs the ones into the 1e16 and answers 1e16.
local hard = {1e16}
for _ = 1, 8 do hard[#hard + 1] = 1.0 end
local naive = 0.0
for _, v in ipairs(hard) do naive = naive + v end
assert_eq(array.sum(array.from(hard)) ~= naive, true,
          "the canonical order is not a left-to-right sum")
assert_eq(array.sum(array.from(hard)), 1e16 + 6,
          "eight accumulators, combined pairwise")
-- An axis collapses one dimension of a 2D array.
local g = array.from{{1.0, 2.0}, {10.0, 20.0}}
assert_eq(list(array.sum(g, 1)), "11.0,22.0", "axis 1 collapses rows")
assert_eq(list(array.sum(g, 2)), "3.0,30.0", "axis 2 collapses columns")

print("-- 5. Ordering: stable, total, NaN last")
local nan = 0.0 / 0.0
local s = array.sort(array.from{3.0, nan, 1.0, 2.0})
assert_eq(array.get(s, 1), 1.0, "sorted ascending")
assert_eq(array.get(s, 3), 3.0, "with the values in order")
assert_eq(array.get(s, 4) ~= array.get(s, 4), true, "and NaN last")
-- 'min' and 'max' are the first and last of the sorted order, and NaN
-- sorts last: so a NaN never wins 'min' and always wins 'max'. Stated as
-- one rule rather than two conveniences, because the alternative -- a
-- 'max' that skipped NaN -- would disagree with 'sort' about which
-- element is the largest.
local withnan = array.from{nan, 2.0, 1.0}
assert_eq(array.min(withnan), 1.0, "NaN never wins 'min'")
assert_eq(array.max(withnan) ~= array.max(withnan), true,
          "and always wins 'max', because it sorts last")
local sorted = array.sort(withnan)
assert_eq(array.min(withnan), array.get(sorted, 1),
          "'min' is the first element of the sorted order")
assert_eq(array.bits(array.from{array.max(withnan)}),
          array.bits(array.from{array.get(sorted, 3)}),
          "and 'max' is the last")
assert_eq(array.argmax(withnan), 1, "argmax names the NaN's position")
assert_eq(list(array.argsort(array.from{2, 1, 2, 1, 2, 1})), "2,4,6,1,3,5",
          "argsort is stable: equal keys keep their input order")
assert_eq(list(array.sort(array.from{3, 1, 2})), "1,2,3", "an integer sort")

print("-- 6. Masks")
local mask = array.gt(a, 2.0)
assert_eq(list(array.select(a, mask)), "3.0,4.0", "select compacts")
assert_eq(list(array.where(mask, a, -1)), "-1.0,-1.0,3.0,4.0", "where selects")
assert_eq(list(array.where(mask, 1, 0)), "0,0,1,1",
          "where takes scalars on both sides")
assert_eq(array.dtype(array.where(mask, 1, 0)), "i64",
          "and stays integer when both are")

print("-- 7. Grouping")
local keys = array.from{10, 20, 10, 30, 20}
local ids, ng = array.group_index(keys)
assert_eq(list(ids), "1,2,1,3,2", "ids are assigned in first-appearance order")
assert_eq(ng, 3, "and counted")
local vals = array.from{1.0, 2.0, 4.0, 8.0, 16.0}
assert_eq(list(array.group_sum(vals, ids, ng)), "5.0,18.0,8.0", "group_sum")
assert_eq(list(array.group_count(vals, ids, ng)), "2,2,1", "group_count")
assert_eq(list(array.group_mean(vals, ids, ng)), "2.5,9.0,8.0", "group_mean")
assert_eq(list(array.group_min(vals, ids, ng)), "1.0,2.0,8.0", "group_min")
assert_eq(list(array.group_max(vals, ids, ng)), "4.0,16.0,8.0", "group_max")
-- The two canonicalisations that make a group id agree with '=='.
local zk, zn = array.group_index(array.from{0.0, -0.0, nan, nan, 1.0})
assert_eq(list(zk), "1,1,2,2,3", "-0.0 and +0.0 are one key, and NaN is one key")
assert_eq(zn, 3, "so there are three groups")
assert_err(function() return array.group_sum(vals, array.from{1, 9, 1, 1, 1}, ng) end,
           "outside", "a group id out of range is refused")

print("-- 8. Linear algebra")
assert_eq(array.dot(array.from{1.0, 2.0, 3.0}, array.from{4.0, 5.0, 6.0}), 32.0,
          "dot")
assert_eq(array.dot(array.from{1, 2, 3}, array.from{4, 5, 6}), 32,
          "an integer dot stays exact")
local p = array.matmul(array.from{{1.0, 2.0}, {3.0, 4.0}},
                       array.from{{5.0, 6.0}, {7.0, 8.0}})
assert_eq(array.get(p, 1, 1), 19.0, "matmul [1,1]")
assert_eq(array.get(p, 1, 2), 22.0, "matmul [1,2]")
assert_eq(array.get(p, 2, 1), 43.0, "matmul [2,1]")
assert_eq(array.get(p, 2, 2), 50.0, "matmul [2,2]")

print("-- 9. Views share the buffer")
local base = array.from{1.0, 2.0, 3.0, 4.0, 5.0}
local sl = array.slice(base, 2, 4)
assert_eq(array.isview(sl), true, "a slice is a view")
assert_eq(list(sl), "2.0,3.0,4.0", "and reads the right elements")
sl[1] = 99.0
assert_eq(array.get(base, 2), 99.0, "writing through it writes the base")
base[2] = 2.0
assert_eq(array.get(sl, 1), 2.0, "and the base is what it reads back")
local big = array.from{{1.0, 2.0}, {3.0, 4.0}}
assert_eq(list(array.row(big, 2)), "3.0,4.0", "a row is a view")
local t = array.transpose(big)
assert_eq(array.get(t, 1, 2), 3.0, "transpose swaps the axes")
assert_eq(array.isview(t), true, "without copying")
assert_eq(array.sum(t), array.sum(big), "and a kernel over it agrees")
assert_eq(array.isview(array.copy(t)), false, "copy makes it dense again")
assert_eq(list(array.copy(t)[1]), "1.0,3.0", "with the elements in view order")

print("-- 10. Printing never shows a decimal")
-- 3.3: an example that printed '%g' would be right on glibc and wrong
-- somewhere else, so '__tostring' names the array and 'bits' is how its
-- contents are read.
assert_eq(tostring(array.from{1.5}), "array<f64>[1]", "__tostring is a shape")
assert_eq(tostring(array.zeros("i64", 2, 3)), "array<i64>[2,3]", "for 2D too")
assert_eq(array.bits(array.from{1.0}), "3ff0000000000000", "bits are 16 digits")
assert_eq(array.bits(array.from({1}, "u8")), "01", "and a u8 is two")
assert_eq(array.bits(array.from{{1.0, 1.0}, {1.0, 1.0}}),
          "3ff0000000000000 3ff0000000000000\n" ..
          "3ff0000000000000 3ff0000000000000",
          "a row per line")
assert_eq(getmetatable(array.from{1}), "array",
          "the metatable is not reachable")

print("\n=== All Array Tests Passed ===")
