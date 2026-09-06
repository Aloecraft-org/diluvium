-- test_regex.lua
-- The 'regex' library and the `...` literal.
--
-- Three things this asserts that are easy to lose and hard to notice:
--   * the scanning rule is 'string.gsub''s, to the letter (section 9), so a
--     Lua programmer's habits carry over between the two;
--   * a pattern that makes a backtracker take exponential time takes linear
--     time here (section 10), which is the reason this engine exists;
--   * the literal is a syntax error in stock Lua (section 11), which is the
--     fork's standing promise about every construct it adds.

local failed = 0
local checks = 0

local function ok (cond, name)
    checks = checks + 1
    if cond then
        print(string.format("[PASS] %s", name))
    else
        print(string.format("[FAIL] %s", name))
        failed = failed + 1
    end
end

local function eq (actual, expected, name)
    checks = checks + 1
    if actual == expected then
        print(string.format("[PASS] %s", name))
    else
        print(string.format("[FAIL] %s", name))
        print(string.format("       expected: %s", tostring(expected)))
        print(string.format("       actual:   %s", tostring(actual)))
        failed = failed + 1
    end
end

-- A match, flattened, so a whole result can be compared as one string.
local function flat (...)
    local t = table.pack(...)
    local out = {}
    for i = 1, t.n do
        local v = t[i]
        out[i] = (v == nil) and "nil" or ((v == false) and "false" or tostring(v))
    end
    return table.concat(out, "|")
end

-- Compiling 'src' must fail: the stock-Lua compatibility checks.
local function nocompile (src, name)
    checks = checks + 1
    if load(src) == nil then
        print(string.format("[PASS] %s", name))
    else
        print(string.format("[FAIL] %s (compiled, expected a syntax error)", name))
        failed = failed + 1
    end
end

-- A pattern that must be refused, with the reason mentioning 'why'.
local function refused (pat, why, name)
    checks = checks + 1
    local okc, err = pcall(regex.compile, pat)
    if not okc and string.find(err, why, 1, true) then
        print(string.format("[PASS] %s", name))
    else
        print(string.format("[FAIL] %s", name))
        print(string.format("       got: %s", tostring(err)))
        failed = failed + 1
    end
end

print("=== regex ===\n")

print("-- 1. the library is there, and is a library")
ok(type(regex) == "table", "regex is a global table")
ok(type(regex.compile) == "function", "regex.compile")
ok(type(regex.find) == "function", "regex.find")
ok(type(regex.escape) == "function", "regex.escape")

print("-- 2. compiling")
local re = regex.compile("(\\d+)-(\\d+)")
eq(re.source, "(\\d+)-(\\d+)", "the object keeps its source")
eq(re.ngroups, 2, "it counts its capture groups")
eq(re.flags, "", "no flags by default")
eq(tostring(re), "regex((\\d+)-(\\d+))", "tostring names the pattern")
ok(regex.compile(re) == re, "compiling a compiled regex returns it")
ok(regex.compile("(\\d+)-(\\d+)") == re, "the same pattern compiles once")

print("-- 3. find and match")
eq(flat(re:find("order 12-34 shipped")), "7|11|12|34", "find: span then captures")
eq(flat(re:match("order 12-34 shipped")), "12|34", "match: the captures")
eq(flat(regex.match("[a-z]+", "Hello World")), "ello", "no groups: the whole match")
eq(flat(regex.find("zz", "abc")), "nil", "no match is nil")
eq(flat(regex.find("b", "abcabc", 3)), "5|5", "init skips ahead")
eq(flat(regex.find("b", "abcabc", -2)), "5|5", "a negative init counts back")
eq(flat(regex.match("(a)|(b)", "b")), "false|b", "a group that did not take part is false")

print("-- 4. the syntax")
eq(flat(regex.match("colou?r", "colour")), "colour", "optional")
eq(flat(regex.match("a{2,3}", "aaaa")), "aaa", "bounded repetition is greedy")
eq(flat(regex.match("a{2,3}?", "aaaa")), "aa", "and lazy with '?'")
eq(flat(regex.match("<(.+?)>", "<a><b>")), "a", "lazy quantifier")
eq(flat(regex.match("<(.+)>", "<a><b>")), "a><b", "greedy quantifier")
eq(flat(regex.match("(?:ab)+", "ababab")), "ababab", "a plain group")
eq(flat(regex.match("^\\w+", "one two")), "one", "anchor and \\w")
eq(flat(regex.find("\\bfoo\\b", "a foo b")), "3|5", "word boundaries")
eq(flat(regex.find("\\Bfoo", "buffoon")), "4|6", "and their complement")
eq(flat(regex.match("[^aeiou]+", "xyzabc")), "xyz", "a negated class")
eq(flat(regex.match("[[:digit:]]+", "ab123")), "123", "a POSIX class")
eq(flat(regex.match("[\\d.]+", "v1.22x")), "1.22", "a class holding an escape")
eq(flat(regex.match("a\\.c", "abc a.c")), "a.c", "an escaped metacharacter")
eq(flat(regex.match("\\x41+", "zAAA")), "AAA", "a hex escape")
eq(flat(regex.match("a|ab", "ab")), "a", "alternation prefers the left branch")
eq(flat(regex.match("(?i)hello (\\w+)", "HELLO World")), "World", "(?i)")
eq(flat(regex.match("(?s).+", "a\nb")), "a\nb", "(?s) lets '.' cross a line")
eq(flat(regex.find("(?m)^b", "a\nb")), "3|3", "(?m) makes '^' a line anchor")
eq(flat(regex.find("$", "ab\n")), "4|3", "'$' is the end of the subject, not of a line")
eq(flat(regex.match("x{2}", "axxb")), "xx", "an exact count")
eq(flat(regex.match("{2}", "a{2}b")), "{2}", "a brace that is not a count is a brace")

print("-- 5. named groups")
local d = regex.compile("(?<year>\\d{4})-(?<mon>\\d{2})")
eq(d.names.year, 1, "a named group knows its number")
eq(d.names.mon, 2, "and so does the next")
eq(flat(d:match("on 2026-09-06")), "2026|09", "named groups still capture by position")
eq(regex.compile("(?P<a>x)").names.a, 1, "(?P<name>) is the same thing")
ok(regex.compile("(x)").names == nil, "no names, no table")

print("-- 6. flags as an argument")
local ci = regex.compile("abc", "i")
eq(ci.flags, "i", "the flags are recorded")
eq(flat(ci:match("xxABCxx")), "ABC", "and applied")
eq(flat(regex.match("[a-c]+", "ABC")), "nil", "without them, case matters")

print("-- 7. gsub")
eq(flat(regex.gsub("(\\w+)@(\\w+)", "a@b and c@d", "%2.%1")), "b.a and d.c|2",
   "a replacement string with capture references")
eq(flat(regex.gsub("\\s+", "a  b   c", "-")), "a-b-c|2", "a plain replacement")
eq(flat(regex.gsub("\\d", "a1b2", "[%0]")), "a[1]b[2]|2", "%0 is the whole match")
eq(flat(regex.gsub("a", "aaa", "b", 2)), "bba|2", "a replacement count")
eq(flat(regex.gsub("(\\w+)", "hi you", function (w) return w:upper() end)),
   "HI YOU|2", "a function replacement")
eq(flat(regex.gsub("(\\w+)", "one two", { one = "1" })), "1 two|2",
   "a table replacement, keyed by the first capture")
eq(flat(regex.gsub("(a)|(b)", "ab", "[%1%2]")), "[a][b]|2",
   "an absent group contributes nothing")

print("-- 8. gmatch and split")
local words = {}
for w in regex.gmatch("[a-z]+", "one two three") do words[#words + 1] = w end
eq(table.concat(words, ","), "one,two,three", "gmatch yields every match")
local pairs_ = {}
for k, v in regex.gmatch("(\\w+)=(\\w+)", "a=1 b=2") do
    pairs_[#pairs_ + 1] = k .. ":" .. v
end
eq(table.concat(pairs_, ","), "a:1,b:2", "gmatch yields the captures")
eq(table.concat(regex.split(",\\s*", "a, b,c"), "|"), "a|b|c", "split")
eq(table.concat(regex.split(",", "a,b,c", 2), "|"), "a|b,c", "split honours a limit")
eq(table.concat(regex.split("x*", "abc"), "|"), "abc",
   "a separator that matches nothing separates nothing")
eq(regex.escape("a.b*c"), "a\\.b\\*c", "escape quotes the metacharacters")
eq(flat(regex.match(regex.escape("1+1=2"), "so 1+1=2 then")), "1+1=2",
   "and what it produces matches literally")

print("-- 9. the scanning rule is string.gsub's, to the letter")
for _, c in ipairs{
    { "c*", "c*", "bcbb" }, { "a*", "a*", "aaa" }, { "x*", "x*", "" },
    { "[0-9]*", "%d*", "a1b22c" }, { "", "", "abc" }, { "b*", "b*", "abcb" },
    { "[0-9]+", "%d+", "a1b22c" }, { "a", "a", "banana" },
} do
    local rgot, rn = regex.gsub(c[1], c[3], "<%0>")
    local lgot, ln = string.gsub(c[3], c[2], "<%0>")
    eq(rgot .. "/" .. rn, lgot .. "/" .. ln,
       string.format("gsub %q agrees with string.gsub %q", c[1], c[2]))
    local a, b = {}, {}
    for m in regex.gmatch(c[1], c[3]) do a[#a + 1] = m end
    for m in string.gmatch(c[3], c[2]) do b[#b + 1] = m end
    eq(table.concat(a, ","), table.concat(b, ","),
       string.format("gmatch %q agrees with string.gmatch %q", c[1], c[2]))
end

-- The one place the two deliberately part company. Lua's manual: a caret in
-- gmatch "does not work as an anchor, as this would prevent the iteration",
-- so string.gmatch reads it as a literal '^'. A regex anchor is an anchor.
do
    local a = {}
    for m in regex.gmatch("^a+", "aa b aa") do a[#a + 1] = m end
    eq(table.concat(a, ","), "aa", "'^' in gmatch anchors, unlike string.gmatch")
    local b = {}
    for m in regex.gmatch("\\^a", "^a x ^a") do b[#b + 1] = m end
    eq(table.concat(b, ","), "^a,^a", "and a literal caret is written '\\^'")
end

print("-- 10. linear time, which is the whole point")
-- A backtracker needs 2^n steps for these. If this test ever hangs, the
-- engine stopped being a Thompson simulation.
local subject = string.rep("a", 64)
for _, p in ipairs{ "(a+)+b", "(a|a)*b", "(a*)*b", "(a|aa)+c", "((a)*)*b" } do
    local t0 = os.clock()
    local hit = regex.find(p, subject)
    ok(hit == nil and os.clock() - t0 < 1.0,
       string.format("%q over 64 a's: no match, no explosion", p))
end
eq(flat(regex.match("(a*)*", "aaa")), "aaa", "an empty-capable loop still matches")
do  -- what such a group *captures* is where a linear-time engine and a
    -- backtracker are allowed to differ (dregex.h); the span is not.
    local a, b = regex.find("(a?)*b", "aaab")
    eq(a .. "|" .. b, "1|4", "and one whose body can match nothing")
end

print("-- 11. the literal")
local lit = `(\d+)-(\d+)`
eq(lit.source, "(\\d+)-(\\d+)", "a literal is raw: no doubled backslashes")
ok(lit == re, "and it is the same object the same pattern compiled to")
eq(flat(`[a-z]+`:match("Hello World")), "ello", "a method call needs no parentheses")
eq(`a``b`.source, "a`b", "'``' is one backtick")
eq(flat(`\s+`:gsub("a  b", "-")), "a-b|1", "a literal in an expression")
local n = 0
for _ in `\w+`:gmatch("one two three") do n = n + 1 end
eq(n, 3, "a literal driving a for loop")
-- That a literal is a syntax error in *stock* Lua cannot be asserted from
-- inside Diluvium, whose own 'load' is the fork's parser. It rests on the
-- backtick appearing nowhere in upstream's lexer, which the patch series
-- (script/patch_series.sh) is what keeps honest.
-- Every position an expression can take, since it is a primary expression
-- and not a simple one: the grammar has to hold in all of them.
do
    local keyed = { [`a`] = 1 }
    eq(#{ `a`, `b` }, 2, "a literal in a table constructor")
    eq(({ `x` })[1].source, "x", "and indexed straight out of one")
    eq((function (r) return r.source end)(`z`), "z", "as a call argument")
    eq(select("#", `a`, `b`), 2, "in a value list")
    eq(next(keyed).source, "a", "and as a table key")
end
ok(load("return `a`(1)") ~= nil, "calling one compiles (it fails at run time)")
nocompile("`a` = 1", "a literal is not an assignment target")
nocompile("`a`", "and not a statement on its own")
nocompile("local x = `abc", "an unfinished literal is refused")
nocompile("local x = `ab\nc`", "and so is one that crosses a line")

-- A chunk holding a literal is ordinary bytecode: it dumps, reloads, and
-- passes the load-time verifier, because the literal compiles to a global
-- call and introduces no instruction the compiler did not already emit.
do
    local chunk = load("local re = `(\\d+)x` return re:match('12x')")
    local reloaded = load(string.dump(chunk))
    ok(reloaded ~= nil and reloaded() == "12", "a literal survives dump and reload")
    local stripped = load(string.dump(chunk, true))
    ok(stripped ~= nil and stripped() == "12", "and survives being stripped")
end

print("-- 12. what is refused, and why the message says so")
refused("(a", "missing ')'", "an unclosed group")
refused("[a", "unfinished character class", "an unclosed class")
refused("*a", "nothing to repeat", "a quantifier with nothing to quantify")
refused("(a)\\1", "backreferences are not supported", "a backreference")
refused("(?=a)", "lookahead is not supported", "lookahead")
refused("(?<=a)", "lookbehind is not supported", "lookbehind")
refused("\\p{L}", "Unicode property", "a Unicode property class")
refused("a**", "one quantifier applies to one piece", "a doubled quantifier")
refused("a*+", "possessive", "a possessive quantifier")
refused("[z-a]", "reversed range", "a reversed range")
refused("a(?i)", "flags must come first", "flags after the first atom")
refused("\\q", "unknown escape", "an unknown escape")
refused("a)", "unmatched ')'", "a stray ')'")
refused(string.rep("(?:", 60) .. string.rep(")", 60), "nests too deeply",
        "a pattern nested past the limit")
refused(string.rep("(", 30) .. string.rep(")", 30), "too many capturing groups",
        "more capture groups than there are slots")
refused(string.rep("a{200}", 4), "too complex", "a pattern past the size limit")
-- Overflowing inside an alternation used to be the interesting case: the
-- patch chain runs through the instruction stream, so an instruction that
-- was never emitted must not become a link that points at itself.
refused("(" .. string.rep("abcdefghij|", 60) .. "z)", "too complex",
        "an alternation past the size limit")
refused(string.rep("(?:a|b)", 145), "too complex",
        "a concatenation past the size limit")

print("-- 13. a compiled regex is ordinary data")
-- No userdata anywhere in it: dsnap.c refuses to capture one, so an agent
-- that parks holding a regex must still be capturable. See dregex.h.
local function nouserdata (v, seen)
    seen = seen or {}
    if type(v) == "userdata" then return false end
    if type(v) ~= "table" or seen[v] then return true end
    seen[v] = true
    for k, x in pairs(v) do
        if not nouserdata(k, seen) or not nouserdata(x, seen) then return false end
    end
    return true
end
ok(nouserdata(re), "a compiled regex holds no userdata")
ok(type(rawget(re, 1)) == "string", "its program is a byte string")
ok(getmetatable(re) == getmetatable(regex.compile("zz")),
   "every regex shares one metatable")

print("-- 14. a forged object fails to match rather than reaching memory")
local forged = setmetatable({ "DRX\1 not a program at all" }, getmetatable(re))
ok(not pcall(function () return forged:find("abc") end),
   "a made-up program is refused")
local forged2 = setmetatable({ 42 }, getmetatable(re))
ok(not pcall(function () return forged2:find("abc") end),
   "so is a program that is not even a string")

print("")
print(string.format("=== %d checks, %d failed ===", checks, failed))
-- 'error' rather than 'os.exit': os.exit leaves the state unclosed, and the
-- debug build's checkfinalmem assertion then fires instead of the failure
-- being reported. test/run_tests.sh says the same thing about its guards.
if failed > 0 then error(string.format("%d regex checks failed", failed), 0) end
