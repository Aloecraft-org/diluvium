-- test_transform.lua
-- Verifies numeric spec stage 2: the 'c128' dtype, the FFT family, the
-- NTT, and the convolutions built on both.
--
-- The bit-exactness of the answers is test/numeric/corpus.lua, which is
-- diffed across targets. This file is about meaning: that an FFT is the
-- DFT, that an inverse inverts, that an exact convolution equals
-- schoolbook, and that a c128 array is kept out of the kernels that
-- could only misread it.

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

local function assert_near(actual, expected, tol, name)
    if math.abs(actual - expected) <= tol then
        print(string.format("[PASS] %s", name))
    else
        print(string.format("[FAIL] %s", name))
        print(string.format("       Expected: %.17g +- %g", expected, tol))
        print(string.format("       Actual:   %.17g", actual))
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

-- The naive transform, straight from the definition. Slow and obviously
-- right, which is the only property wanted of a reference.
local function dft(v)
    local n = #v
    local out = {}
    for k = 0, n - 1 do
        local re, im = 0.0, 0.0
        for j = 0, n - 1 do
            local th = -2.0 * math.pi * k * j / n
            re = re + v[j + 1] * math.cos(th)
            im = im + v[j + 1] * math.sin(th)
        end
        out[k + 1] = {re, im}
    end
    return out
end

-- Schoolbook convolution, likewise.
local function school(a, b)
    local out = {}
    for k = 1, #a + #b - 1 do out[k] = 0 end
    for i = 1, #a do
        for j = 1, #b do out[i + j - 1] = out[i + j - 1] + a[i] * b[j] end
    end
    return out
end

print("=== Starting Transform Tests ===\n")

print("-- 1. The c128 dtype")
local c = array.new("c128", 4)
assert_eq(array.dtype(c), "c128", "a c128 array knows its dtype")
assert_eq(array.size(c), 4, "and its size")
assert_eq(select(2, array.get(c, 1)), 0.0, "a fresh element is 0+0i")
array.set(c, 1, 3.0, 4.0)
assert_eq(select(1, array.get(c, 1)), 3.0, "set writes the real part")
assert_eq(select(2, array.get(c, 1)), 4.0, "and the imaginary one")
array.set(c, 2, 5.0)
assert_eq(select(2, array.get(c, 2)), 0.0, "a missing imaginary part is 0")
assert_eq(select(1, array.get(array.ones("c128", 2), 1)), 1.0, "'ones' is 1+0i")
assert_eq(tostring(array.zeros("c128", 3)), "array<c128>[3]", "__tostring")
-- 'from' reads pairs, 'to_table' writes them, and the two agree.
local pairsrc = array.from({{1, 2}, {3, -4}, 5}, "c128")
assert_eq(array.size(pairsrc), 3, "a table of pairs is one dimension")
assert_eq(array.to_table(pairsrc)[2][2], -4.0, "a pair round-trips")
assert_eq(array.to_table(pairsrc)[3][2], 0.0, "a bare number is real")
-- The bit format is two halves and a colon, so one element is still one
-- space-separated field.
assert_eq(array.bits(array.from({{1, 0}}, "c128")),
          "3ff0000000000000:0000000000000000", "bits prints both halves")

print("-- 2. Complex, in the five words a program has for it")
local re = array.from{3.0, -5.0}
local im = array.from{4.0, 12.0}
local z = array.complex(re, im)
assert_eq(list(array.real(z)), "3.0,-5.0", "real")
assert_eq(list(array.imag(z)), "4.0,12.0", "imag")
assert_eq(list(array.real(array.conj(z))), "3.0,-5.0", "conj keeps the real part")
assert_eq(list(array.imag(array.conj(z))), "-4.0,-12.0", "and negates the other")
assert_eq(list(array.magnitude(z)), "5.0,13.0", "magnitude")
assert_eq(list(array.imag(array.complex(re))), "0.0,0.0",
          "one array means a zero imaginary part")
assert_eq(list(array.real(array.cast(re, "c128"))), "3.0,-5.0",
          "a real array casts to c128")

print("-- 3. What a c128 array is not allowed into")
local zc = array.from({{1, 1}}, "c128")
assert_err(function() return array.sum(zc) end, "real array", "sum refuses it")
assert_err(function() return zc + zc end, "real array", "and so does '+'")
assert_err(function() return array.sort(zc) end, "real array", "and sort")
assert_err(function() return array.dot(zc, zc) end, "real array", "and dot")
assert_err(function() return zc[1] end, "two numbers", "'a[i]' refuses it")
assert_err(function() zc[1] = 2 end, "two numbers", "and 'a[i] = v'")
assert_err(function() return array.cast(zc, "f64") end, "array.real",
           "and casting away the imaginary part")
-- A view of one is still one, and the width is right: element 2 of a
-- slice from 2 is element 3 of the base.
local seq = array.from({{1, 10}, {2, 20}, {3, 30}, {4, 40}}, "c128")
local view = array.slice(seq, 2, 4)
assert_eq(array.size(view), 3, "a c128 slice has the right length")
assert_eq(select(2, array.get(view, 2)), 30.0, "and the right elements")
assert_eq(array.isview(view), true, "and is a view")
assert_eq(select(2, array.get(array.copy(view), 1)), 20.0, "copy copies pairs")

print("-- 4. The FFT is the DFT")
local sig = {}
for k = 1, 16 do sig[k] = math.sin(k * 1.7) * 3.0 + k * 0.25 end
local F = array.fft(array.from(sig))
local D = dft(sig)
local worst = 0.0
for k = 1, 16 do
    local a, b = array.get(F, k)
    worst = math.max(worst, math.abs(a - D[k][1]), math.abs(b - D[k][2]))
end
assert_eq(worst < 1e-9, true, "16 bins agree with the naive DFT (" .. worst .. ")")
assert_eq(array.dtype(F), "c128", "and the result is complex")
-- The bin every reader can check by eye.
local dcr, dci = array.get(array.fft(array.ones("f64", 8)), 1)
assert_eq(dcr .. "," .. dci, "8.0,0.0", "a constant signal is all DC")
assert_near(select(1, array.get(array.fft(array.ones("f64", 8)), 5)), 0.0,
            1e-12, "and nothing anywhere else")
-- Length 1 is a power of two and the transform is the identity.
assert_eq(select(1, array.get(array.fft(array.from{7.0}), 1)), 7.0,
          "a one-point transform is the identity")

print("-- 5. The inverses invert")
local x = array.from{1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0}
local rt = array.real(array.ifft(array.fft(x)))
worst = 0.0
for k = 1, 8 do worst = math.max(worst, math.abs(array.get(rt, k) - array.get(x, k))) end
assert_eq(worst < 1e-12, true, "ifft(fft(x)) is x (" .. worst .. ")")
local half = array.rfft(x)
assert_eq(array.size(half), 5, "a half spectrum of 8 has 5 bins")
assert_eq(list(array.irfft(half)), "1.0,2.0,3.0,4.0,5.0,6.0,7.0,8.0",
          "irfft(rfft(x)) is x, exactly here")
-- The half spectrum is the first half of the full one, which is what
-- makes the two functions describe the same transform.
local full = array.fft(x)
worst = 0.0
for k = 1, 5 do
    local a1, b1 = array.get(full, k)
    local a2, b2 = array.get(half, k)
    worst = math.max(worst, math.abs(a1 - a2), math.abs(b1 - b2))
end
assert_eq(worst, 0.0, "rfft is fft truncated, bit for bit")
assert_eq(array.size(array.irfft(half, 8)), 8, "irfft takes the length back")

print("-- 6. Power-of-two sizes, and the refusal that says so")
assert_err(function() return array.fft(array.zeros("f64", 3)) end,
           "pad to 4", "3 is refused with the length it needed")
assert_err(function() return array.fft(array.zeros("f64", 100)) end,
           "pad to 128", "and so is 100")
assert_err(function() return array.ntt(array.zeros("i64", 6)) end,
           "power of two", "the NTT wants one too")
assert_err(function() return array.fft(array.new("f64", 2, 2)) end,
           "1D array", "and a 2D array has no transform here")

print("-- 7. The NTT is exact")
local iv = array.from{5, 7, 11, 13, 17, 19, 23, 29}
assert_eq(list(array.intt(array.ntt(iv))), list(iv), "intt(ntt(x)) is x exactly")
assert_eq(array.dtype(array.ntt(iv)), "i64", "and stays an integer array")
assert_err(function() return array.ntt(array.from{-1, 0, 0, 0}) end,
           "residue", "a negative value is not a residue")
assert_err(function() return array.ntt(array.from{2013265921, 0, 0, 0}) end,
           "residue", "and neither is the prime itself")
assert_eq(list(array.ntt(array.zeros("i64", 4))), "0,0,0,0",
          "the transform of nothing is nothing")

print("-- 8. Convolution")
local A, B = {1, -2, 3, 4, 5}, {7, 8, -9}
local Ci = array.convolve(array.from(A), array.from(B))
local S = school(A, B)
assert_eq(array.size(Ci), #S, "the result is #x + #y - 1 long")
assert_eq(list(Ci), table.concat(S, ","), "and equals schoolbook, exactly")
assert_eq(array.dtype(Ci), "i64", "an integer convolution stays integer")
-- Signs both ways, which is what the centred representative is for.
local N1, N2 = {-3, -4}, {-5, 6}
assert_eq(list(array.convolve(array.from(N1), array.from(N2))),
          table.concat(school(N1, N2), ","), "negative operands")
-- A float operand takes the FFT path instead.
local Cf = array.convolve(array.from{1.5, -2.5, 3.5}, array.from{0.5, 0.25})
assert_eq(array.dtype(Cf), "f64", "a float convolution is f64")
local Sf = school({1.5, -2.5, 3.5}, {0.5, 0.25})
worst = 0.0
for k = 1, #Sf do worst = math.max(worst, math.abs(array.get(Cf, k) - Sf[k])) end
assert_eq(worst < 1e-12, true, "and agrees with schoolbook (" .. worst .. ")")
-- The bound is checked before any work, and names what it is.
assert_err(function()
    return array.convolve(array.from{1000000, 1000000}, array.from{1000000, 1})
end, "past the exact range", "a product past the ring is refused")
assert_err(function() return array.convolve(array.from{1}, array.zeros("i64", 0)) end,
           "empty", "an empty operand has no convolution")
-- 'math.mininteger' has no positive counterpart, so the bound is tested
-- before anything negates it.
assert_err(function()
    return array.convolve(array.from{math.mininteger}, array.from{1})
end, "too large for an exact convolution",
   "the smallest integer is refused rather than negated")

print("-- 9. Correlation")
-- A delta at lag 0: correlating with {0,1,0} picks out the middle.
local Cc = array.correlate(array.from{1, 2, 3}, array.from{0, 1, 0})
assert_eq(array.size(Cc), 5, "the full correlation is #x + #y - 1 long")
assert_eq(list(Cc), "0,1,2,3,0", "and is x, placed at the lag")
-- Autocorrelation peaks at zero lag, which is index #y.
local sq = array.from{1, 2, 3, 4}
local ac = array.correlate(sq, sq)
local peak = array.get(ac, 4)
for k = 1, array.size(ac) do
    assert_eq(array.get(ac, k) <= peak, true, "autocorrelation peak, bin " .. k)
end
assert_eq(peak, 30, "and the peak is the sum of squares")

print("-- 10. The transforms meet the rest of the library")
-- Parseval: the energy of a signal equals the energy of its spectrum
-- divided by n. A cheap end-to-end check that nothing is scaled wrong.
local p = array.from{2.0, -1.0, 0.5, 3.0, -2.5, 1.0, 0.0, 4.0}
local energy = array.sum(p * p)
local spectrum = array.magnitude(array.fft(p))
assert_near(array.sum(spectrum * spectrum) / 8.0, energy, 1e-9, "Parseval")
-- A strided view transforms as its elements, not as its buffer.
local wide = array.from{1.0, 99.0, 2.0, 99.0, 3.0, 99.0, 4.0, 99.0}
local rows = array.new("f64", 4, 2)
for k = 1, 4 do array.set(rows, k, 1, array.get(wide, 2 * k - 1)) end
local col = array.row(array.transpose(rows), 1)
assert_eq(array.isview(col), true, "the column is a view")
local sr = array.get(array.fft(col), 1)
assert_eq(sr, 10.0, "and its transform sees 1..4, not the buffer")

print("\n=== All Transform Tests Passed ===")
