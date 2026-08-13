-- test_host.lua
-- The 'host' guest library: hostcalls without the hand-rolled queue pair.
--
-- There is no host under this interpreter, so the test plays one: it declares
-- 'host/replies' first (the library looks up before declaring), pre-pushes
-- the reply a call will want, and then reads the request the library pushed
-- off 'host/calls' to assert the wire shape. queue.wait returns immediately
-- when a message is already there, so nothing here needs to park.

local pass, fail = 0, 0
local function ok(cond, what)
    if cond then pass = pass + 1
    else fail = fail + 1; print(string.format("[FAIL] %s", what)) end
end
local function eq(got, want, what)
    ok(got == want, string.format("%s (got %s, want %s)",
        what, tostring(got), tostring(want)))
end

-- ---- surface ---------------------------------------------------------

eq(type(host), "table", "host is a table")
eq(type(host.sql), "table", "host.sql")
eq(type(host.crypto), "table", "host.crypto")
eq(type(host.sql.open), "function", "host.sql.open")
local kv = host.sql.open("kv.sqlite")
for _, f in ipairs({"exec", "query", "try_exec", "try_query"}) do
    eq(type(kv[f]), "function", "an opened db has " .. f)
end
for _, f in ipairs({"hash", "hmac", "random", "jwt_sign", "jwt_verify"}) do
    eq(type(host.crypto[f]), "function", "host.crypto." .. f)
end
for _, f in ipairs({"read", "write", "try_read", "try_write"}) do
    eq(type(host.fs[f]), "function", "host.fs." .. f)
end
eq(type(host.exec.run), "function", "host.exec.run")
eq(type(host.exec.try_run), "function", "host.exec.try_run")
eq(type(host.time), "function", "host.time")
eq(type(host.call), "function", "host.call")
eq(type(host.try), "function", "host.try")

-- ---- the played host -------------------------------------------------

local replies = queue.declare("host/replies", { capacity = 16 })
-- the library allocates tokens from a high base, disjoint from the small
-- integers a raw ask() uses; the played host answers in that space
local T = 0x40000000
local function answer(m)
    if m.tok then m.tok = T + m.tok end
    assert(queue.push(replies, m))
end
local calls  -- the library declares 'host/calls' on its first call
local function sent()
    if not calls then calls = assert(queue.lookup("host/calls")) end
    local m = queue.pop(calls)
    assert(m ~= nil, "no request on host/calls")
    return m
end

-- ---- a call round-trips, and the request has the documented shape ----

answer({ tok = 1, status = "ok", value = { changes = 1, rowid = 7 } })
local r = kv.exec("INSERT INTO kv VALUES (?, ?)", "demo", 42)
eq(r.changes, 1, "exec returns the connector's value")
eq(r.rowid, 7, "exec value arrives whole")
local req = sent()
eq(req.tok, T + 1, "tokens count up from the library's base")
eq(req.call, "sql/exec", "the call is named")
eq(req.args.db, "kv.sqlite", "the opened name rides in args.db")
eq(req.args.sql, "INSERT INTO kv VALUES (?, ?)", "sql rides in args.sql")
eq(req.args.params[1], "demo", "first param")
eq(req.args.params[2], 42, "second param")
eq(#req.args.params, 2, "params is exactly the varargs")

answer({ tok = 2, status = "ok",
         value = { cols = { "v" }, rows = { { "x" } } } })
r = kv.query("SELECT v FROM kv WHERE k = ?", "demo")
eq(r.rows[1][1], "x", "query returns {cols, rows}")
req = sent()
eq(req.call, "sql/query", "query names sql/query")

-- a call with no parameters sends no params at all
answer({ tok = 3, status = "ok", value = { changes = 0, rowid = 0 } })
kv.exec("CREATE TABLE t (a)")
req = sent()
eq(req.args.params, nil, "no varargs, no params key")

-- ---- non-ok raises, with the connector's sentence in the message -----

answer({ tok = 4, status = "denied",
         detail = "this program does not hold 'host:sql/exec'" })
local okc, err = pcall(kv.exec, "DELETE FROM kv")
eq(okc, false, "a denial raises")
ok(err:find("host.sql.exec: denied: ", 1, true) ~= nil,
   "the error names the call and the status (got: " .. tostring(err) .. ")")
ok(err:find("does not hold", 1, true) ~= nil,
   "the connector's detail rides in the message")
sent()

answer({ tok = 5, status = "denied", detail = "not wired" })
okc, err = pcall(host.time)
eq(okc, false, "host.time raises on denial too")
ok(err:find("host.time: denied: not wired", 1, true) ~= nil,
   "a slashless call name still reads as host.time (got: " ..
   tostring(err) .. ")")
req = sent()
eq(req.call, "time", "time is called with no slash")
eq(req.args, nil, "and with no args")

-- ---- try_*: the expected-denial path stays expressible ---------------

answer({ tok = 6, status = "denied", detail = "outside the grant" })
local v, st, detail = kv.try_exec("DELETE FROM kv")
eq(v, nil, "try_exec: no value on denial")
eq(st, "denied", "try_exec: the status comes back")
eq(detail, "outside the grant", "try_exec: the detail comes back")
sent()

answer({ tok = 7, status = "ok", value = { changes = 1, rowid = 1 } })
v, st = kv.try_exec("INSERT INTO t VALUES (1)")
eq(v.changes, 1, "try_exec: the value on ok")
eq(st, "ok", "try_exec: and the ok status")
sent()

-- ---- replies correlate by token, in any order ------------------------

answer({ tok = 9, status = "ok", value = "second" })
answer({ tok = 8, status = "ok", value = "first" })
eq(host.call("a/b"), "first", "a call skips a reply that is not its own")
eq(host.call("a/b"), "second", "and the skipped reply answers its call")
sent(); sent()

-- a reply with no token answers nothing and is dropped
answer({ status = "malformed", detail = "no tok was readable" })
answer({ tok = 10, status = "ok", value = 42 })
eq(host.call("a/b"), 42, "a tokless reply is passed over")
sent()

-- ---- crypto: shapes, and jwt_verify never raises on invalid ----------

answer({ tok = 11, status = "ok", value = "deadbeef" })
eq(host.crypto.hash("x"), "deadbeef", "hash returns the hex")
req = sent()
eq(req.call, "crypto/hash", "hash names crypto/hash")
eq(req.args.data, "x", "data rides in args.data")

answer({ tok = 12, status = "ok", value = "ff" })
host.crypto.random(16)
req = sent()
eq(req.args.bytes, 16, "random sends args.bytes")

answer({ tok = 13, status = "ok", value = "ee" })
host.crypto.random()
req = sent()
eq(next(req.args), nil, "random() leaves the count to the host")

answer({ tok = 14, status = "ok", value = "tok.en.x" })
host.crypto.jwt_sign({ sub = "u1" }, 60)
req = sent()
eq(req.args.claims.sub, "u1", "jwt_sign sends the claims")
eq(req.args.ttl, 60, "and the ttl")

answer({ tok = 15, status = "ok",
         value = { valid = false, reason = "signature" } })
v = host.crypto.jwt_verify("bad.token.here")
eq(v.valid, false, "an invalid token is an answer, not an error")
eq(v.reason, "signature", "with the reason")
sent()

-- ---- fs and exec: the wire shapes ------------------------------------

answer({ tok = 16, status = "ok", value = "the bytes" })
eq(host.fs.read("notes/a.txt"), "the bytes", "fs.read returns the bytes")
req = sent()
eq(req.call, "fs/read", "read names fs/read")
eq(req.args.path, "notes/a.txt", "the path rides in args.path")

answer({ tok = 17, status = "ok", value = { bytes = 5 } })
local w = host.fs.write("out.txt", "12345", { append = true })
eq(w.bytes, 5, "fs.write reports its bytes")
req = sent()
eq(req.args.data, "12345", "the data rides in args.data")
eq(req.args.append, true, "and append when asked")

answer({ tok = 18, status = "ok", value = { bytes = 1 } })
host.fs.write("out.txt", "x")
req = sent()
eq(req.args.append, nil, "no opts, no append key")

answer({ tok = 19, status = "ok",
         value = { status = 0, stdout = "hi\n", stderr = "" } })
local r = host.exec.run({ "echo", "hi" }, { timeout_ms = 500 })
eq(r.status, 0, "exec.run returns the child's exit")
eq(r.stdout, "hi\n", "and its stdout")
req = sent()
eq(req.call, "exec/run", "run names exec/run")
eq(req.args.argv[1], "echo", "argv rides as a vector")
eq(req.args.timeout_ms, 500, "with the call's own deadline")

-- ---- host.try: the generic non-raising form --------------------------

answer({ tok = 20, status = "error", detail = "it broke" })
v, st, detail = host.try("frob/nicate", { n = 1 })
eq(v, nil, "host.try: nil on error")
eq(st, "error", "host.try: the status")
eq(detail, "it broke", "host.try: the detail")
req = sent()
eq(req.args.n, 1, "host.try passes args through")

-- ---- a nil parameter is refused before it can vanish -----------------

okc, err = pcall(kv.exec, "INSERT INTO t VALUES (?, ?)", "a", nil, "c")
eq(okc, false, "a nil param raises")
ok(err:find("parameter 2 is nil", 1, true) ~= nil,
   "and the message names which (got: " .. tostring(err) .. ")")
eq(queue.len(calls), 0, "refused before anything was sent")

-- ---- a raise blames the program's own line ---------------------------
-- The public functions reach the internals by proper tail call, so the
-- error level must account for the elided frame: the position must be the
-- line that called host.time, not that line's caller.

answer({ tok = 21, status = "denied", detail = "no" })
local atline = assert(load("host.time()", "=atline"))
okc, err = pcall(atline)
eq(okc, false, "the load'ed call raises")
ok(err:find("atline:1:", 1, true) ~= nil,
   "and the error is attributed to the calling line (got: " ..
   tostring(err) .. ")")
sent()

-- ---- naming the database is the program's job ------------------------

okc, err = pcall(host.sql.open)
eq(okc, false, "open with no name raises")
ok(err:find("name the database", 1, true) ~= nil,
   "and says whose job naming is")
okc, err = pcall(kv.exec, kv, "SELECT 1")  -- the db:exec(...) colon mistake
eq(okc, false, "a colon-call is caught client-side")
ok(err:find("dot%-call") ~= nil,
   "and diagnosed as one (got: " .. tostring(err) .. ")")
eq(queue.len(calls), 0, "nothing was sent for either")

print(string.format("\n%d passed, %d failed", pass, fail))
if fail > 0 then os.exit(1) end
