-- test_time.lua
-- The 'time' guest library: epoch seconds <-> fields <-> ISO 8601, all UTC.
--
-- Fixed reference moments first (a value a second implementation agrees on),
-- then round-trips across the range that finds the calendar arithmetic wrong:
-- a leap day, a century year, the far past and the far future.

local pass, fail = 0, 0
local function ok(cond, what)
    if cond then pass = pass + 1
    else fail = fail + 1; print(string.format("[FAIL] %s", what)) end
end
local function eq(got, want, what)
    ok(got == want, string.format("%s (got %s, want %s)",
        what, tostring(got), tostring(want)))
end

-- ---- iso: known moments ----------------------------------------------

eq(time.iso(0), "1970-01-01T00:00:00Z", "the epoch")
eq(time.iso(-1), "1969-12-31T23:59:59Z", "one second before the epoch")
eq(time.iso(1786554000), "2026-08-12T17:00:00Z", "a moment in 2026")
eq(time.iso(951782400), "2000-02-29T00:00:00Z", "the year-2000 leap day")
eq(time.iso(-62135596800), "0001-01-01T00:00:00Z", "year one, zero-padded")

-- ---- parse: the forms a machine exchanges ----------------------------

eq(time.parse("2026-08-12T17:00:00Z"), 1786554000, "parse Z")
eq(time.parse("2026-08-12T18:00:00+01:00"), 1786554000, "parse a +01:00 offset")
eq(time.parse("2026-08-12T16:00:00-01:00"), 1786554000, "parse a -01:00 offset")
eq(time.parse("2026-08-12T18:30:00+0130"), 1786554000, "parse a colon-less offset")
eq(time.parse("2026-08-12T17:00:00.123Z"), 1786554000, "parse and drop a fraction")
eq(time.parse("2026-08-12 17:00:00Z"), 1786554000, "parse a space separator")
eq(time.parse("1970-01-01"), 0, "parse a date alone as midnight")

-- ---- fields ----------------------------------------------------------

do
    local f = time.fields(1786554000)   -- 2026-08-12T17:00:00Z, a Wednesday
    ok(f.year == 2026 and f.month == 8 and f.day == 12, "fields: the date")
    ok(f.hour == 17 and f.min == 0 and f.sec == 0, "fields: the time")
    eq(f.wday, 4, "fields: wday is 1=Sunday..7=Saturday (Wednesday = 4)")
    eq(f.yday, 224, "fields: yday counts from 1")
end
eq(time.fields(0).wday, 5, "the epoch was a Thursday (wday 5)")

-- ---- of --------------------------------------------------------------

eq(time.of({year = 2026, month = 8, day = 12, hour = 17}), 1786554000,
   "of: fields to epoch, missing parts default to zero")
eq(time.of({year = 1970, month = 1, day = 1}), 0, "of: the epoch")

ok(pcall(time.of, {month = 1, day = 1}) == false, "of refuses a missing year")
ok(pcall(time.of, {year = 2021, month = 2, day = 29}) == false,
   "of refuses Feb 29 in a common year")
ok(pcall(time.of, {year = 2021, month = 13, day = 1}) == false,
   "of refuses a month past 12")
ok(pcall(time.of, {year = 2021, month = 1, day = 1, hour = 24}) == false,
   "of refuses hour 24")

-- ---- parse refusals --------------------------------------------------

ok(pcall(time.parse, "2026-13-01T00:00:00Z") == false, "parse refuses month 13")
ok(pcall(time.parse, "2021-02-29") == false, "parse refuses a non-leap Feb 29")
ok(pcall(time.parse, "2026/08/12") == false, "parse refuses '/' separators")
ok(pcall(time.parse, "2026-08-12T17:00Z") == false, "parse refuses a missing :ss")
ok(pcall(time.parse, "2026-08-12T17:00:00Zjunk") == false,
   "parse refuses trailing bytes")

-- ---- round-trips over a hard range -----------------------------------

for _, e in ipairs({
    0, 1, -1, 1786554000, 951782400, 946684800, -62135596800, 253402300799,
    1234567890, -2208988800,   -- 1900-01-01
}) do
    eq(time.of(time.fields(e)), e, "fields round-trip at " .. e)
    eq(time.parse(time.iso(e)), e, "iso round-trip at " .. e)
end

-- the connector answers milliseconds; the library speaks seconds. Prove the
-- documented boundary conversion is all it takes.
eq(time.iso(1786554000123 // 1000), "2026-08-12T17:00:00Z",
   "host:time ms / 1000 formats as seconds")

print(string.format("\n%d passed, %d failed", pass, fail))
if fail > 0 then os.exit(1) end
