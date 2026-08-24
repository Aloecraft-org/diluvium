---@meta
---The guest-facing globals, as LuaCATS annotations.
---
---A sealed Diluvium instance opens these libraries beyond the stock ones:
---`queue`, `msgpack`, `endpoint`, `bytes`, `json`, `time` and `host`. This
---file never runs: it exists so lua-language-server stops flagging them as
---unknown globals and can complete and type-check guest programs. Point the
---editor at it (`workspace.library`) from any project that writes guest
---code. The *host configuration* schema is the separate `host/types/host.lua`
----- one file, one context, and this file's context is the GUEST.
---
---Doc of record: doc/Guide.md (queues, messages, endpoints, the libraries),
---doc/Hostcall.md (the protocol `host` wraps), and each library's header.

-- ---------------------------------------------------------------- queue --

---Buffers between programs, and between a program and its host. Handles are
---plain integers, never reused; a destroyed handle raises on use.
---@class diluvium.queuelib
queue = {}

---@class diluvium.QueueOpts
---@field capacity integer?  # messages held before on_full applies (default 64)
---@field on_full ("reject"|"drop_oldest"|"drop_newest"|"block")?  # default "reject"
---@field direction ("both"|"guest_write"|"guest_read")?  # default "both"
---@field exported boolean?  # true to let the host read and write it (default false)

---Declare a queue by name. Raises if the name is already declared.
---@param name string
---@param opts diluvium.QueueOpts?
---@return integer handle
function queue.declare(name, opts) end

---@param name string
---@return integer? handle  # nil when no queue has this name
function queue.lookup(name) end

---@param handle integer
function queue.destroy(handle) end

---Push a message. `false, "full"` and friends are normal outcomes, not
---errors; only a bad handle or an unencodable value raises.
---@param handle integer
---@param value any  # msgpack-encodable
---@return boolean ok
---@return "ok"|"dropped_oldest"|"full"|"disabled"|"gone" status
function queue.push(handle, value) end

---Take one message without waiting.
---@param handle integer
---@return any? value
---@return "ok"|"empty" status
function queue.pop(handle) end

---Wait on up to 32 queues. Timeout in milliseconds; -1 (the default) waits
---forever, 0 polls. Returns the ready queue's handle and the message taken
---from it, or `nil, nil, "timeout"`. Parks the program unless a message is
---already there, so it needs a host that resumes.
---@param handles integer[]
---@param timeout_ms integer?
---@return integer? handle
---@return any? value
---@return "ok"|"timeout"|"closed" status
function queue.wait(handles, timeout_ms) end

---@param handle integer
function queue.enable(handle) end

---@param handle integer
function queue.disable(handle) end

---@param handle integer
---@return integer
function queue.len(handle) end

---@param handle integer
---@return integer
function queue.capacity(handle) end

---@param handle integer
---@return "enabled"|"disabled"
function queue.state(handle) end

---@param handle integer
---@return {name: string, capacity: integer, on_full: string, exported: boolean, direction: string, len: integer, endpoint: boolean}
function queue.info(handle) end

-- -------------------------------------------------------------- msgpack --

---The wire format the queues speak, exposed for a program that needs bytes.
---@class diluvium.msgpacklib
msgpack = {}

---@param value any
---@return string bytes
function msgpack.encode(value) end

---@param bytes string
---@return any value
function msgpack.decode(bytes) end

---Force a table to encode as an array (a bare table is an array only when
---its keys are exactly 1..n).
---@param t table
---@return table wrapper
function msgpack.as_array(t) end

---Force a table to encode as a map.
---@param t table
---@return table wrapper
function msgpack.as_map(t) end

---An application ext value (codes 0x10..0x7F).
---@param code integer
---@param data string
---@return table wrapper
function msgpack.ext(code, data) end

-- ------------------------------------------------------------- endpoint --

---Names for queues outside this instance, minted by the host.
---@class diluvium.endpointlib
endpoint = {}

---Bind a host-authorised reference to a local handle.
---@param ref any     # the reference the host handed over
---@param name string # the local queue name to bind it as
---@return integer handle
function endpoint.bind(ref, name) end

---@param handle integer
---@return "live"|"gone"
function endpoint.status(handle) end

---@param handle integer
---@return boolean
function endpoint.is_endpoint(handle) end

-- ---------------------------------------------------------------- bytes --

---Flat transcoding: base64, base64url, hex, URL escaping. Pure.
---@class diluvium.byteslib
bytes = {}

---@param s string
---@return string hex
function bytes.tohex(s) end

---@param hex string
---@return string s
function bytes.fromhex(hex) end

---@param s string
---@return string b64
function bytes.tobase64(s) end

---@param b64 string
---@return string s
function bytes.frombase64(b64) end

---@param s string
---@return string b64url
function bytes.tobase64url(s) end

---@param b64url string
---@return string s
function bytes.frombase64url(b64url) end

---@param s string
---@return string escaped
function bytes.urlencode(s) end

---@param escaped string
---@return string s
function bytes.urldecode(escaped) end

-- ----------------------------------------------------------------- json --

---A Lua value to JSON text and back. Pure, strict, bounded.
---@class diluvium.jsonlib
json = {}

---@param value any
---@return string text
function json.encode(value) end

---@param text string
---@return any value
function json.decode(text) end

-- ----------------------------------------------------------------- time --

---Calendar arithmetic on Unix epoch seconds, all UTC. Pure -- reading the
---current time is `host.time()`, which answers in milliseconds.
---@class diluvium.timelib
time = {}

---@param sec integer
---@return string iso  # "YYYY-MM-DDThh:mm:ssZ"
function time.iso(sec) end

---@param iso string
---@return integer sec
function time.parse(iso) end

---@param sec integer
---@return {year: integer, month: integer, day: integer, hour: integer, min: integer, sec: integer, wday: integer, yday: integer}
function time.fields(sec) end

---@param fields {year: integer, month: integer?, day: integer?, hour: integer?, min: integer?, sec: integer?}
---@return integer sec
function time.of(fields) end

-- ----------------------------------------------------------------- host --

---Hostcalls without the hand-rolled queue pair (doc/Hostcall.md). Every
---call is one connector round-trip; a non-`ok` status raises with the
---connector's own sentence in the message, and the `try` forms return it
---instead for a program that expects a denial. Which calls actually answer
---is the deployment's business: the connector must be wired and the grant
---(`host:sql/*`, `host:time`, ...) held, or the reply is `denied`.
---@class diluvium.hostlib
---@field sql diluvium.hostsql
---@field crypto diluvium.hostcrypto
---@field fs diluvium.hostfs
---@field exec diluvium.hostexec
host = {}

---Wall-clock milliseconds from the `time` connector. (Calendar arithmetic
---on the result is the pure `time` library's job.)
---@return integer ms
function host.time() end

---Any connector call by name: `host.call("sql/exec", {sql = ...})`. The
---typed wrappers below are this, spelled out; this form exists for a
---connector the deployment wires that this runtime predates.
---@param name string  # "prefix/op", or a bare name like "time"
---@param args any?
---@return any value
function host.call(name, args) end

---`host.call` without the raise: returns the reply's status instead.
---@param name string
---@param args any?
---@return any? value
---@return string status  # "ok", "denied", "error", "malformed", ...
---@return string? detail # the connector's sentence, on non-ok
function host.try(name, args) end

---Spawn a child instance (build10). The library owns the lifecycle queue,
---the op shape, and the outcome wait; the caller holds a handle. Needs the
---`lifecycle` capability, and the child's `caps` must attenuate -- a name
---this instance does not hold is a raised denial, with the swarm's own
---sentence (the refused capability) in it. Requests are sequential through
---this library: one spawn or kill outcome in flight at a time.
---
---A handle is NOT a channel: the swarm does not yet deliver endpoint
---references between instances, so there is no `child.push`. Parent and
---child coordinate through whatever the deployment wires (a shared
---connector), or the child works autonomously and its exit reaches
---`host.events`.
---@param spec diluvium.SpawnSpec
---@return diluvium.hostchild child
function host.spawn(spec) end

---@class diluvium.SpawnSpec
---@field code string             # the child's chunk; source, or a compiled
---                               # dump (a secure function travels intact)
---@field caps string[]?          # the child's grants; subset of this
---                               # instance's or the spawn is denied
---@field budget {instructions: integer?, memory_kb: integer?}?
---@field wake_on_message boolean?  # hibernated child wakes on delivery
---@field sealed boolean?         # drop UNSAFE_STDLIB from the child even
---                               # if this instance holds it
---@field waitms integer?         # outcome wait bound (default 10000)

---@class diluvium.hostchild
---@field id integer              # the swarm id, unique host-wide
local hostchild = {}

---Kill the child and its whole subtree. Ancestor-only, enforced by the
---swarm; returns true once the swarm's `exited` lands, raises on a denial
---("no such instance" once it already died).
---@param waitms integer?
---@return boolean
function hostchild.kill(waitms) end

---The children spawned through this library and not yet seen die, id ->
---handle, a fresh table each call. This ledger resets with the library's
---other upvalues on a snapshot restore: the authoritative record of a
---supervision tree is the supervisor program's own state, which does ride
---the snapshot.
---@return table<integer, diluvium.hostchild>
function host.children() end

---The next lifecycle event about this instance's children: `exited`,
---`faulted`, `exceeded`, `denied`, `status` -- `{event, id, detail?}`, the
---shape Messaging.md 9.2 fixes. Buffered events first (an outcome wait
---never eats a death notice), then the events queue. `waitms` nil or 0
---polls, which is what a supervisor already parked on its own wait-set
---wants; positive parks here.
---@param waitms integer?
---@return {event: string, id: integer, detail: string?}? ev
function host.events(waitms) end

---@class diluvium.hostsql
local hostsql = {}

---Name a database inside the deployment's granted scope and get its handle.
---The name is a filename, never a path -- a separator or a name resolving
---outside the scope is denied host-side. Client-side sugar: the handle's
---calls carry the name as `args.db`. Handle calls are DOT-calls
---(`db.exec(...)`, not `db:exec(...)`).
---@param name string
---@return diluvium.hostdb db
function hostsql.open(name) end

---@class diluvium.hostdb
local hostdb = {}

---One writing statement (`host:sql/exec`; wired only when the deployment
---grants access "readwrite"). Parameters bind `?` markers in order; SQL
---NULL is spelled NULL in the statement, not nil in the list.
---@param sql string
---@param ... boolean|number|string
---@return {changes: integer, rowid: integer}
function hostdb.exec(sql, ...) end

---One reading statement (`host:sql/query`). Refuses writes.
---@param sql string
---@param ... boolean|number|string
---@return {cols: string[], rows: any[][]}
function hostdb.query(sql, ...) end

---`exec` without the raise.
---@param sql string
---@param ... boolean|number|string
---@return {changes: integer, rowid: integer}? value
---@return string status
---@return string? detail
function hostdb.try_exec(sql, ...) end

---`query` without the raise.
---@param sql string
---@param ... boolean|number|string
---@return {cols: string[], rows: any[][]}? value
---@return string status
---@return string? detail
function hostdb.try_query(sql, ...) end

---@class diluvium.hostfs
local hostfs = {}

---Read a file inside the deployment's granted scope (`host:fs/read`). The
---path is relative and may descend (`notes/a.txt`); `..`, absolute paths,
---and anything resolving outside the scope are denied. Refuses past the
---deployment's byte cap.
---@param path string
---@return string bytes
function hostfs.read(path) end

---Write a file inside the granted scope (`host:fs/write`; wired only under
---access "readwrite"). Directories are not created on the way. Refuses
---past the byte cap.
---@param path string
---@param data string
---@param opts {append: boolean?}?
---@return {bytes: integer}
function hostfs.write(path, data, opts) end

---`read` without the raise.
---@param path string
---@return string? bytes
---@return string status
---@return string? detail
function hostfs.try_read(path) end

---`write` without the raise.
---@param path string
---@param data string
---@param opts {append: boolean?}?
---@return {bytes: integer}? value
---@return string status
---@return string? detail
function hostfs.try_write(path, data, opts) end

---@class diluvium.hostexec
local hostexec = {}

---Run a subprocess (`host:exec/run`) -- the honest escape hatch: granting
---it is leaving the sandbox. `argv` is a vector, so there is no shell
---unless the program names one. A nonzero exit is an *answer*
---(`{status = 1}`); the raise is for the call itself failing -- the
---deadline (capped by the deployment's ceiling) or an output stream past
---the byte cap, either of which kills the child.
---@param argv string[]
---@param opts {stdin: string?, timeout_ms: integer?, cwd: string?}?
---@return {status: integer, stdout: string, stderr: string}
function hostexec.run(argv, opts) end

---`run` without the raise.
---@param argv string[]
---@param opts {stdin: string?, timeout_ms: integer?, cwd: string?}?
---@return {status: integer, stdout: string, stderr: string}? value
---@return string status
---@return string? detail
function hostexec.try_run(argv, opts) end

---@class diluvium.hostcrypto
local hostcrypto = {}

---SHA-256, as lowercase hex.
---@param data string
---@return string hex
function hostcrypto.hash(data) end

---HMAC-SHA256, as lowercase hex. Without options, under the host's derived
---key. `opts.key` selects a named raw secret from the deployment's
---`crypto.secrets` (webhook interop -- the peer holds the same bytes);
---`opts.expect` (hex) turns the call into a constant-time verification and
---the return into `{valid = boolean}` -- an answer, never a raise, the
---jwt_verify convention.
---@param data string
---@param opts {key: string?, expect: string?}?
---@return string|{valid: boolean}
function hostcrypto.hmac(data, opts) end

---Host-quality randomness, as hex (two characters per byte).
---@param nbytes integer?  # default is the host's (32)
---@return string hex
function hostcrypto.random(nbytes) end

---An HS256 JWT. The host owns `iat` and `exp`: a guest-supplied expiry is
---dropped and `ttl` (seconds, capped by the deployment) sets the real one.
---@param claims table
---@param ttl integer?  # default is the deployment's default_ttl
---@return string token
function hostcrypto.jwt_sign(claims, ttl) end

---Verify an HS256 JWT. An invalid token is an *answer*, not an error:
---`{valid = false, reason = ...}` arrives with status `ok` and no raise.
---@param token string
---@return {valid: boolean, claims: table?, reason: string?}
function hostcrypto.jwt_verify(token) end

---A TURN REST credential (coturn's use-auth-secret): `username` is
---"<expiry>:<user>", `password` is base64 of HMAC-SHA1 over it under the
---deployment's shared secret. The host owns the expiry: `ttl` (seconds,
---default the deployment's turn.ttl) sets it, a guest never names a
---timestamp. When the deployment lists `turn.uris`, they arrive verbatim in
---`uris`, so the reply is a complete ICE server entry. Denied unless the
---deployment configures `crypto.turn`.
---@param user string  # 1..256 bytes, no NUL
---@param ttl integer?
---@return {username: string, password: string, expires: integer, uris: string[]?}
function hostcrypto.turn_credential(user, ttl) end
