-- test_localhost.lua
-- The standalone binary's own host (src/dlocalhost.c): host.* answered in
-- place, from Lua's own libraries, with no --task and no queue round trip.
-- Every call the library exposes is exercised here as a program would make
-- it. The queue transport is test_host.lua's, run with --queue-host.

local pass, fail = 0, 0
local function ok(cond, what)
    if cond then pass = pass + 1
    else fail = fail + 1; print(string.format("[FAIL] %s", what)) end
end
local function eq(got, want, what)
    ok(got == want, string.format("%s (got %s, want %s)",
        what, tostring(got), tostring(want)))
end
local function raises(f, needle, what)
    local good, err = pcall(f)
    ok(not good and tostring(err):find(needle, 1, true) ~= nil,
       string.format("%s (got %s)", what, tostring(err)))
end

local dir = os.tmpname()
os.remove(dir)
local function path(name) return dir .. "-" .. name end

-- ---- time ------------------------------------------------------------

local t = host.time()
eq(math.type(t), "integer", "host.time() is an integer")
ok(math.abs(t // 1000 - os.time()) <= 2, "host.time() agrees with os.time()")
local m1 = host.monotonic()
local m2 = host.monotonic()
eq(math.type(m1), "integer", "host.monotonic() is an integer")
ok(m1 >= 0 and m2 >= m1, "host.monotonic() does not go backwards")

-- answered by a plain call, so it works where a queue wait cannot: inside
-- a table.sort comparator, which C calls and Lua cannot yield across
local xs = { 3, 1, 2 }
table.sort(xs, function(a, b) host.time() return a < b end)
eq(table.concat(xs, ","), "1,2,3", "host calls work inside a sort comparator")

-- ---- fs --------------------------------------------------------------

local f = path("note.txt")
eq(host.fs.write(f, "hello").bytes, 5, "fs.write answers {bytes}")
eq(host.fs.write(f, " world", { append = true }).bytes, 6, "fs.write append")
eq(host.fs.read(f), "hello world", "fs.read reads what was written")
local io_f = io.open(f, "rb")
eq(io_f:read("a"), "hello world", "the same file through io")
io_f:close()
local bin = "\0\1\255\254"
host.fs.write(f, bin)
eq(host.fs.read(f), bin, "fs is byte-exact")
local v, st, detail = host.fs.try_read(path("missing"))
eq(v, nil, "fs.try_read of a missing file answers nil")
eq(st, "error", "  with status error")
ok(type(detail) == "string" and #detail > 0, "  and a detail")
raises(function() host.fs.read(path("missing")) end, "host.fs.read: error:",
       "fs.read of a missing file raises with the call's name")
raises(function() host.fs.write(f) end, "args.data must be a string",
       "fs.write without data says what it needs")
eq(host.call("fs/remove", { path = f }), nil, "fs/remove answers nil")
eq(io.open(f, "rb"), nil, "and the file is gone")

-- fs/list over a directory this test makes with nothing but the host
local made = host.exec.try_run({ "mkdir", path("d") })
if made and made.status == 0 then
    host.fs.write(path("d") .. "/b", "")
    host.fs.write(path("d") .. "/a", "")
    local names = host.call("fs/list", { path = path("d") })
    eq(table.concat(names, ","), "a,b", "fs/list answers sorted names")
    host.call("fs/remove", { path = path("d") .. "/a" })
    host.call("fs/remove", { path = path("d") .. "/b" })
    os.remove(path("d"))
end

-- ---- exec ------------------------------------------------------------

local probe = host.exec.try_run({ "sh", "-c", "exit 0" })
if probe == nil then
    -- no fork on this platform: the call says so instead of hanging
    local _, why, msg = host.exec.try_run({ "sh" })
    eq(why, "error", "exec without fork answers error")
    ok(tostring(msg):find("not built", 1, true), "  saying it is not built")
else
    local r = host.exec.run({ "sh", "-c", "echo out; echo err >&2; exit 3" })
    eq(r.status, 3, "exec.run: a nonzero exit is an answer")
    eq(r.stdout, "out\n", "exec.run: stdout")
    eq(r.stderr, "err\n", "exec.run: stderr")
    eq(host.exec.run({ "cat" }, { stdin = "fed in" }).stdout, "fed in",
       "exec.run: stdin")
    eq(host.exec.run({ "pwd" }, { cwd = "/" }).stdout, "/\n", "exec.run: cwd")
    eq(host.exec.run({ "no-such-program-here" }).status, 127,
       "exec.run: a program that cannot run is status 127")
    eq(host.exec.run({ "echo", "a b", "'c'", "$HOME" }).stdout,
       "a b 'c' $HOME\n", "exec.run: argv is a vector, no shell quoting")
    local start = host.monotonic()
    local _, st2, why = host.exec.try_run({ "sleep", "5" }, { timeout_ms = 200 })
    eq(st2, "error", "exec.run: past the deadline is an error")
    ok(tostring(why):find("deadline", 1, true), "  that names the deadline")
    ok(host.monotonic() - start < 3000, "  and the child was killed")
    -- a child that exits without reading its stdin must not kill this
    -- process with SIGPIPE
    eq(host.exec.run({ "true" }, { stdin = string.rep("x", 1 << 20) }).status,
       0, "exec.run: a child ignoring stdin is not a crash")
    raises(function() host.exec.run("ls -l") end, "argv",
           "exec.run refuses a shell string")
end

-- ---- crypto ----------------------------------------------------------

eq(host.crypto.hash("abc"),
   "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
   "crypto.hash is sha256 hex")
eq(host.call("crypto/hash", { data = "abc", alg = "sha1" }),
   "a9993e364706816aba3e25717850c26c9cd0d89d", "crypto/hash alg sha1")
local r1, r2 = host.crypto.random(), host.crypto.random(8)
eq(#r1, 64, "crypto.random() is 32 bytes of hex")
eq(#r2, 16, "crypto.random(8) is 8 bytes of hex")
ok(r1:match("^%x+$") ~= nil, "  hex digits")
ok(host.crypto.random() ~= r1, "  and not the same twice")
raises(function() host.crypto.random(0) end, "bytes must be 1..1024",
       "crypto.random bounds")

-- the keyed calls: without DILUVIUM_CRYPTO_KEY they say so; with it they
-- derive DRT's subkeys (checked against the C host's labels below)
local key = os.getenv("DILUVIUM_CRYPTO_KEY")
if key == nil then
    raises(function() host.crypto.hmac("x") end, "DILUVIUM_CRYPTO_KEY",
           "crypto.hmac without a key names the variable to set")
    raises(function() host.crypto.jwt_sign({ sub = "u" }) end,
           "DILUVIUM_CRYPTO_KEY", "crypto.jwt_sign without a key, likewise")
else
    local k_hmac = bytes.hmac_sha256(key, "diluvium/crypto/hmac/v1")
    eq(host.crypto.hmac("payload"),
       bytes.tohex(bytes.hmac_sha256(k_hmac, "payload")),
       "crypto.hmac signs under the derived hmac subkey")
    local mac = host.crypto.hmac("payload")
    eq(host.crypto.hmac("payload", { expect = mac }).valid, true,
       "crypto.hmac expect: a match")
    eq(host.crypto.hmac("other", { expect = mac }).valid, false,
       "crypto.hmac expect: a mismatch is an answer")
    raises(function() host.crypto.hmac("x", { key = "github" }) end,
           "plain diluvium has neither", "a named key is refused by name")

    local tok = host.crypto.jwt_sign({ sub = "u1", exp = 1 }, 60)
    local h, p = tok:match("^([^.]+)%.([^.]+)%.")
    eq(h, "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9", "jwt: the constant header")
    local claims = json.decode(bytes.frombase64url(p))
    eq(claims.sub, "u1", "jwt: the program's claim")
    eq(claims.exp - claims.iat, 60, "jwt: the host owns exp, from ttl")
    local k_jwt = bytes.hmac_sha256(key, "diluvium/crypto/jwt-hs256/v1")
    eq(tok:match("[^.]+$"),
       bytes.tobase64url(bytes.hmac_sha256(k_jwt, h .. "." .. p)),
       "jwt: signed under the derived jwt subkey")
    local res = host.crypto.jwt_verify(tok)
    eq(res.valid, true, "jwt_verify: its own token")
    eq(res.claims.sub, "u1", "jwt_verify: the claims")
    eq(host.crypto.jwt_verify(tok .. "x").reason, "signature",
       "jwt_verify: a bad signature is an answer")
    eq(host.crypto.jwt_verify("a.b").reason, "malformed", "jwt_verify: shape")
    local none = bytes.tobase64url('{"alg":"none","typ":"JWT"}')
    eq(host.crypto.jwt_verify(none .. "." .. p .. ".").reason, "alg",
       "jwt_verify: alg none is refused by the header compare")
end

-- ---- sql -------------------------------------------------------------

do
    local db = host.sql.open(path("kv.sqlite"))
    db.exec("create table kv (k text primary key, v, n integer)")
    local w = db.exec("insert into kv values (?, ?, ?)", "a", "one", 1)
    eq(w.changes, 1, "sql.exec answers changes")
    eq(w.rowid, 1, "  and rowid")
    db.exec("insert into kv values (?, ?, ?)", "b", host.sql.NULL, 2.5)
    db.exec("insert into kv values (?, ?, ?)", "c", "\0bin\255", true)
    local r = db.query("select k, v, n from kv order by k")
    eq(table.concat(r.cols, ","), "k,v,n", "sql.query answers cols")
    eq(#r.rows, 3, "  and rows")
    eq(r.rows[1][2], "one", "  text")
    eq(r.rows[2][2], nil, "  NULL is a hole, as off the wire")
    eq(r.rows[2][3], 2.5, "  float")
    eq(r.rows[3][2], "\0bin\255", "  bytes round-trip")
    eq(r.rows[3][3], 1, "  a boolean binds as 1")
    eq(db.query("select count(*) as n from kv where n >= ?", 1).rows[1][1], 3,
       "parameters bind in queries")
    local _, st, why = db.try_query("delete from kv")
    eq(st, "denied", "a query that writes is denied")
    ok(tostring(why):find("sql/exec", 1, true), "  pointing at sql/exec")
    eq(#db.query("select * from kv").rows, 3, "  and nothing was deleted")
    local _, st2, why2 = db.try_exec("select 1; drop table kv")
    eq(st2, "error", "two statements in one call are refused")
    ok(tostring(why2):find("one statement", 1, true), "  saying so")
    local _, st3 = db.try_exec("insert into kv values (?, ?, ?)", "d")
    eq(st3, "error", "too few parameters are refused")
    raises(function() db.query("selec nothing") end, "would not prepare",
           "a statement that does not parse raises with SQLite's reason")
    -- the same file another handle, and another process, can read
    eq(host.sql.open(path("kv.sqlite")).query("select v from kv where k = 'a'")
       .rows[1][1], "one", "a second open of the same name sees the data")
    os.remove(path("kv.sqlite"))
end

-- ---- capabilities and what is not answered here ----------------------

local fams = {}
for _, e in ipairs(host.capabilities()) do
    fams[e.name] = e
    ok(e.granted == true and e.kind == "builtin", e.name .. " is granted")
end
for _, name in ipairs({ "time", "fs", "exec", "crypto", "sql",
                        "capabilities" }) do
    ok(fams[name] ~= nil, "capabilities lists " .. name)
end

local _, rst = host.try("rest/get", { url = "https://example.com" })
eq(rst, "denied", "a family this host does not answer is denied")
raises(function() host.spawn({ code = "return 1" }) end, "no swarm",
       "host.spawn says there is no swarm")
eq(host.events(), nil, "host.events: nothing happens without a swarm")
eq(next(host.children()), nil, "host.children: none")

-- ---- the queue pair is untouched -------------------------------------

eq(queue.lookup("host/calls"), nil, "no call queue was declared")

print(string.format("\n%d passed, %d failed", pass, fail))
if fail > 0 then os.exit(1) end
