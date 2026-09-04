//! Embed a Diluvium program.
//!
//! A Diluvium program is an isolated instance with its own heap and its own
//! bounded message queues. It runs until it needs something, then parks and
//! tells you what it is waiting for; you decide what happens next. There is no
//! scheduler and no clock inside the runtime, which is what lets the same
//! program run under `tokio`, a browser event loop, or the blocking loop below.
//!
//! ```no_run
//! use diluvium::{Instance, Step};
//!
//! let mut inst = Instance::from_source(r#"
//!     local inbox = queue.lookup("inbox")
//!     local outbox = queue.lookup("outbox")
//!     while true do
//!         local _, msg = queue.wait({inbox})
//!         if msg == "stop" then return end
//!         queue.push(outbox, "saw " .. tostring(msg))
//!     end
//! "#, "greeter")?;
//!
//! let inbox = inst.queue("inbox").unwrap();
//! let outbox = inst.queue("outbox").unwrap();
//!
//! let mut step = inst.run()?;
//! while let Step::Parked(wait) = step {
//!     inst.push(inbox, &"hello")?;
//!     step = inst.resume(wait.ids()[0])?;
//!     if let Some(reply) = inst.pop::<String>(outbox)? {
//!         println!("{reply}");
//!     }
//!     inst.push(inbox, &"stop")?;
//!     step = inst.resume(inbox)?;
//! }
//! # Ok::<(), diluvium::Error>(())
//! ```
//!
//! # Threading
//!
//! One instance, one thread. [`Instance`] is [`Send`] but deliberately not
//! [`Sync`]: you may move it between threads, but two threads must never be
//! inside it at once. That is the ABI's contract rather than a missing lock, and
//! the type system is the cheapest place to say so.

use std::ffi::{CStr, CString};
use std::fmt;
use std::marker::PhantomData;
use std::os::raw::c_void;

use diluvium_sys as sys;
use serde::{de::DeserializeOwned, Serialize};

/// A queue handle. Valid only for the instance that issued it, and only for
/// that instance's lifetime -- these are runtime identity, never durable
/// identity, so do not store one anywhere it might outlive the program.
#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
pub struct QueueId(sys::dv_queue_id);

/// What happened to a push. A full or disabled queue is an ordinary answer, not
/// a failure: the runtime's one delivery guarantee is that a push tells you
/// whether the message was accepted.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Accepted {
    /// Queued.
    Yes,
    /// Queued, and the oldest message was evicted to make room.
    DroppedOldest,
    /// At capacity. Nothing was queued.
    Full,
    /// The program disabled this queue and is no longer accepting.
    Disabled,
}

impl Accepted {
    /// Whether the message was queued at all.
    pub fn is_accepted(self) -> bool {
        matches!(self, Accepted::Yes | Accepted::DroppedOldest)
    }
}

/// What a parked program is waiting for.
#[derive(Clone)]
pub struct Wait {
    ids: Vec<QueueId>,
    timeout: Option<std::time::Duration>,
    for_write: bool,
}

impl Wait {
    /// The queues it is watching. Any one of them firing will wake it.
    pub fn ids(&self) -> &[QueueId] {
        &self.ids
    }

    /// How long the program asked to wait, or `None` if it set no limit.
    ///
    /// You own the clock. Honour this, ignore it, or answer immediately -- the
    /// program only learns which handle you say fired.
    pub fn timeout(&self) -> Option<std::time::Duration> {
        self.timeout
    }

    /// True when the program is waiting for *space* in a full queue rather than
    /// for a message to arrive. Drain the queue rather than pushing to it.
    pub fn is_waiting_for_space(&self) -> bool {
        self.for_write
    }
}

impl fmt::Debug for Wait {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Wait")
            .field("ids", &self.ids)
            .field("timeout", &self.timeout)
            .field("for_write", &self.for_write)
            .finish()
    }
}

/// Where a program got to.
#[derive(Clone, Debug)]
pub enum Step {
    /// Parked. Decide what fired and call [`Instance::resume`].
    Parked(Wait),
    /// Ran to completion.
    Done,
}

/// Everything that can go wrong.
#[derive(Debug)]
pub enum Error {
    /// The library and this wrapper disagree about the ABI. Refusing to start is
    /// the point: a wrapper that guessed could misread a message instead.
    AbiMismatch { library: u32, wrapper: u32 },
    /// Could not create an instance.
    OutOfMemory,
    /// The program did not compile, or raised. Carries the message and, for a
    /// runtime error, the traceback.
    Program(String),
    /// A handle that this instance never issued, or has destroyed.
    UnknownQueue(QueueId),
    /// Called in the wrong order -- running a parked program, or resuming one
    /// that is not parked. The message says which.
    Busy(String),
    /// A message could not be encoded or decoded.
    Codec(String),
    /// A snapshot's header was refused: a different runtime build, a different
    /// permanents or capability set, or the wrong host stamp. Distinct from
    /// [`Error::Program`] on purpose -- a mismatch means "wrong place", not
    /// "corrupt", and a supervisor routes the two differently.
    SnapshotMismatch(String),
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Error::AbiMismatch { library, wrapper } => write!(
                f,
                "Diluvium ABI mismatch: the library is version {library}, this \
                 wrapper expects {wrapper}"
            ),
            Error::OutOfMemory => write!(f, "could not create a Diluvium instance"),
            Error::Program(m) => write!(f, "{m}"),
            Error::UnknownQueue(q) => write!(f, "no such queue: {q:?}"),
            Error::Busy(m) => write!(f, "{m}"),
            Error::Codec(m) => write!(f, "msgpack: {m}"),
            Error::SnapshotMismatch(m) => write!(f, "{m}"),
        }
    }
}

impl std::error::Error for Error {}

/// What a program loaded into an instance is allowed to reach.
///
/// [`Instance::from_source`] and [`Instance::from_bytecode`] cover the common
/// cases; this is here for the two switches that have no business being
/// positional booleans, and because a host that cannot set them has no way to
/// run a program it did not write.
///
/// ```no_run
/// # use diluvium::Config;
/// // A program you wrote yourself, that predates the sealed default and still
/// // calls `os.time`. Nothing else needs this.
/// let inst = Config::new()
///     .unsafe_stdlib(true)
///     .load_source("return os.time()", "=legacy")?;
/// # Ok::<(), diluvium::Error>(())
/// ```
///
/// `Default` is what `from_source` uses, and it is sealed: an instance has no
/// `io`, `os` or `package`, and reaches outside itself only by yielding a
/// request its host answers.
#[derive(Clone, Copy, Debug)]
pub struct Config {
    text_only: bool,
    unsafe_stdlib: bool,
    unsafe_debug: bool,
    budget: Option<(u64, u64)>,
}

impl Default for Config {
    /// Written out rather than derived, so that flipping a default is an edit
    /// to a line that says what it means.
    fn default() -> Self {
        Config {
            text_only: false,
            unsafe_stdlib: false,
            unsafe_debug: false,
            budget: None,
        }
    }
}

impl Config {
    /// A configuration with every switch at its default.
    pub fn new() -> Self {
        Self::default()
    }

    /// Refuse precompiled chunks, accepting source only.
    ///
    /// Worth setting whenever the bytes did not come from your own compiler.
    /// Note it binds `dv_load` and not the program's own `load()`.
    pub fn text_only(mut self, yes: bool) -> Self {
        self.text_only = yes;
        self
    }

    /// Put `io`, `os` and `package` back, with `dofile` and `loadfile`.
    ///
    /// **Scaffolding, not a supported configuration.** An instance is sealed by
    /// default and reaches outside itself by yielding a request its host
    /// answers. Setting this costs two things beyond the obvious: the
    /// instruction budget stops meaning anything, because a subprocess started
    /// by `os.execute` costs no VM instructions; and the swarm stops being
    /// replayable, because inputs arrive somewhere other than the message log.
    ///
    /// A snapshot does not cross this switch -- the permanents fingerprint
    /// covers names -- so a sealed instance's snapshot restores only into
    /// another sealed one.
    pub fn unsafe_stdlib(mut self, yes: bool) -> Self {
        self.unsafe_stdlib = yes;
        self
    }

    /// Open the whole `debug` library rather than the narrowed one.
    ///
    /// This restores the endpoint-forgery route and lets the program switch off
    /// its own instruction budget. Only for programs you wrote yourself.
    pub fn unsafe_debug(mut self, yes: bool) -> Self {
        self.unsafe_debug = yes;
        self
    }

    /// Limit what the program may spend: a VM instruction count and a heap cap
    /// in kilobytes. Zero for either means no limit on that axis.
    ///
    /// The budget **aborts, it does not schedule**: past the instruction limit
    /// the program fails with an error, and past the memory limit an
    /// allocation fails as an ordinary out-of-memory the program can even
    /// catch. A guest cannot meaningfully limit itself -- a runaway loop never
    /// yields -- so the limit lives out here. It applies at load and at
    /// [`Config::restore`] alike, which is the only order that exists: the
    /// runtime refuses a budget set on an instance that has started, and a
    /// restored instance has.
    pub fn budget(mut self, instructions: u64, memory_kb: u64) -> Self {
        self.budget = Some((instructions, memory_kb));
        self
    }

    fn flags(self) -> u32 {
        let mut f = 0;
        if self.text_only {
            f |= sys::DV_FLAG_TEXT_ONLY;
        }
        if self.unsafe_stdlib {
            f |= sys::DV_FLAG_UNSAFE_STDLIB;
        }
        if self.unsafe_debug {
            f |= sys::DV_FLAG_UNSAFE_DEBUG;
        }
        f
    }

    /// Load source under this configuration.
    pub fn load_source(self, source: &str, name: &str) -> Result<Instance, Error> {
        Instance::load_with(self, source.as_bytes(), name)
    }

    /// Load a program that may be precompiled bytecode, under this
    /// configuration. Refused when [`Config::text_only`] is set.
    pub fn load_bytecode(self, code: &[u8], name: &str) -> Result<Instance, Error> {
        Instance::load_with(self, code, name)
    }

    /// Wake a snapshot into a fresh instance under this configuration.
    ///
    /// `host` is the identity stamp: a stamped snapshot restores only under
    /// the same string, and a host that supplies one refuses a snapshot
    /// without it -- or stamping would be advisory. Pass `None` to accept only
    /// unstamped snapshots.
    ///
    /// On success the instance is parked exactly as the snapshot's was, so the
    /// next calls are [`Instance::current_wait`] to learn what it is waiting
    /// for and then [`Instance::resume`] -- not [`Instance::run`]: the program
    /// is not starting, it is continuing. The configuration must match the
    /// captured instance's -- a snapshot does not cross the `unsafe_stdlib`
    /// switch, because a program captured holding `io.open` cannot wake
    /// somewhere there is none.
    ///
    /// Restoring refuses rather than raising, on *any* input: every field is
    /// range-checked before anything is written, so a malformed snapshot
    /// cannot leave a half-built instance. [`Error::SnapshotMismatch`] means
    /// the header was refused; [`Error::Program`] means the header matched and
    /// the payload did not survive reading.
    pub fn restore(self, snapshot: &[u8], host: Option<&str>) -> Result<Instance, Error> {
        let inst = Instance::fresh(self)?;
        let chost =
            host.map(|h| CString::new(h).unwrap_or_else(|_| CString::new("(host)").unwrap()));
        let st = unsafe {
            sys::dv_restore(
                inst.raw,
                chost.as_ref().map_or(std::ptr::null(), |c| c.as_ptr()),
                snapshot.as_ptr(),
                snapshot.len(),
            )
        };
        match st {
            sys::DV_OK => Ok(inst),
            sys::DV_SNAPSHOT_MISMATCH => Err(Error::SnapshotMismatch(inst.last_error())),
            _ => Err(Error::Program(inst.last_error())),
        }
    }

    fn load(self, code: &[u8], name: &str) -> Result<Instance, Error> {
        Instance::load_with(self, code, name)
    }
}

/// One Diluvium program, with its own heap, queues and fate.
pub struct Instance {
    raw: *mut sys::dv_instance,
    /// `!Sync`: see the threading note in the crate docs.
    _not_sync: PhantomData<std::cell::Cell<()>>,
}

// Moving an instance between threads is fine; being inside it from two at once
// is not. `Send` without `Sync` says exactly that.
unsafe impl Send for Instance {}

impl Instance {
    /// Compile Lua source into a fresh instance.
    ///
    /// `name` appears in error messages and tracebacks. Give it something a
    /// human can act on.
    pub fn from_source(source: &str, name: &str) -> Result<Self, Error> {
        Self::load(source.as_bytes(), name, true)
    }

    /// Load a program that may be precompiled bytecode.
    ///
    /// Prefer [`Instance::from_source`] for anything you did not compile
    /// yourself. The loader checks a chunk's operands against the prototype
    /// that owns them and refuses malformed bytecode rather than crashing, but
    /// that is a smaller claim than "bytecode is safe" -- Lua 5.1 shipped a
    /// fuller checker and still had escapes.
    pub fn from_bytecode(code: &[u8], name: &str) -> Result<Self, Error> {
        Self::load(code, name, false)
    }

    /// Wake a snapshot taken by [`Instance::snapshot`], under the default
    /// configuration. See [`Config::restore`] for the whole story, including
    /// the `host` stamp and what to call next.
    pub fn restore(snapshot: &[u8], host: Option<&str>) -> Result<Self, Error> {
        Config::new().restore(snapshot, host)
    }

    fn load(code: &[u8], name: &str, text_only: bool) -> Result<Self, Error> {
        Config::new().text_only(text_only).load(code, name)
    }

    /// A fresh instance with the configuration's flags and budget applied and
    /// nothing loaded: the shape [`Config::restore`] needs, and the first half
    /// of a load. The budget goes on here because the runtime refuses one once
    /// the instance has started, and a restored instance has.
    fn fresh(cfg: Config) -> Result<Self, Error> {
        let library = unsafe { sys::dv_abi_version() };
        if library != sys::DV_ABI_VERSION {
            return Err(Error::AbiMismatch {
                library,
                wrapper: sys::DV_ABI_VERSION,
            });
        }
        let raw_cfg = sys::dv_config {
            flags: cfg.flags(),
            ..Default::default()
        };
        let raw = unsafe { sys::dv_new(&raw_cfg) };
        if raw.is_null() {
            return Err(Error::OutOfMemory);
        }
        let mut inst = Instance {
            raw,
            _not_sync: PhantomData,
        };
        if let Some((instructions, memory_kb)) = cfg.budget {
            inst.set_budget(instructions, memory_kb)?;
        }
        Ok(inst)
    }

    fn load_with(cfg: Config, code: &[u8], name: &str) -> Result<Self, Error> {
        let inst = Self::fresh(cfg)?;
        let cname = CString::new(name).unwrap_or_else(|_| CString::new("=(program)").unwrap());
        let st = unsafe { sys::dv_load(inst.raw, code.as_ptr(), code.len(), cname.as_ptr()) };
        if st != sys::DV_OK {
            return Err(Error::Program(inst.last_error()));
        }
        Ok(inst)
    }

    fn last_error(&self) -> String {
        let p = unsafe { sys::dv_last_error(self.raw) };
        if p.is_null() {
            "(no message)".to_string()
        } else {
            unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned()
        }
    }

    /// Find a queue the program declared, or one of the `inbox`/`outbox` pair
    /// that every instance starts with.
    ///
    /// Queues belong to the program, not to you: this finds one that exists and
    /// never creates one. `None` is also how you learn that the program you
    /// loaded is not the program you expected.
    pub fn queue(&self, name: &str) -> Option<QueueId> {
        let cname = CString::new(name).ok()?;
        let id = unsafe { sys::dv_queue_lookup(self.raw, cname.as_ptr()) };
        (id != 0).then_some(QueueId(id))
    }

    /// Capacity, current length, and how the queue is configured.
    pub fn queue_info(&self, id: QueueId) -> Result<QueueInfo, Error> {
        let mut raw = sys::dv_queue_info::default();
        let st = unsafe { sys::dv_queue_state(self.raw, id.0, &mut raw) };
        if st != sys::DV_OK {
            return Err(Error::UnknownQueue(id));
        }
        Ok(QueueInfo {
            capacity: raw.capacity,
            len: raw.len,
            enabled: raw.enabled != 0,
            exported: raw.exported != 0,
        })
    }

    /// Send a message. Encodes to msgpack, which is what queues hold.
    ///
    /// Returns whether it was accepted; a full or disabled queue is a normal
    /// answer. Accepting never waits on anything, so a program's
    /// `on_full = "block"` reads as `Full` from here -- watch
    /// [`QueueInfo::len`] against [`QueueInfo::capacity`] if you want
    /// backpressure.
    pub fn push<T: Serialize>(&mut self, id: QueueId, value: &T) -> Result<Accepted, Error> {
        // `to_vec_named`, not `to_vec`. rmp-serde's compact form encodes a
        // struct as an *array* of its field values, which would arrive in Lua
        // as `{1, "widget", 2}` and make every guest depend on the declaration
        // order of a Rust struct -- a coupling nobody would choose and nobody
        // would notice until a field moved. Named costs a few bytes per message
        // and buys `msg.qty` on the other side.
        //
        // Every binding must agree on this. Field names cross the boundary.
        let bytes = rmp_serde::to_vec_named(value).map_err(|e| Error::Codec(e.to_string()))?;
        self.push_raw(id, &bytes)
    }

    /// Send msgpack bytes you already have, with no re-encoding.
    pub fn push_raw(&mut self, id: QueueId, msgpack: &[u8]) -> Result<Accepted, Error> {
        let st = unsafe { sys::dv_queue_push(self.raw, id.0, msgpack.as_ptr(), msgpack.len()) };
        match st {
            sys::DV_OK => Ok(Accepted::Yes),
            sys::DV_QUEUE_DROPPED => Ok(Accepted::DroppedOldest),
            sys::DV_QUEUE_FULL => Ok(Accepted::Full),
            sys::DV_QUEUE_DISABLED => Ok(Accepted::Disabled),
            _ => Err(Error::UnknownQueue(id)),
        }
    }

    /// Take the oldest message and decode it. `None` when the queue is empty.
    ///
    /// Uses the borrow-and-release path, so nothing is copied except into your
    /// own type.
    pub fn pop<T: DeserializeOwned>(&mut self, id: QueueId) -> Result<Option<T>, Error> {
        let mut ptr: *const u8 = std::ptr::null();
        let mut len: usize = 0;
        let st = unsafe { sys::dv_queue_peek(self.raw, id.0, &mut ptr, &mut len) };
        match st {
            sys::DV_OK => {}
            sys::DV_QUEUE_EMPTY => return Ok(None),
            _ => return Err(Error::UnknownQueue(id)),
        }
        // Safe because nothing between the peek and the release can run the
        // program, and the runtime anchors the bytes for exactly this window.
        let bytes = unsafe { std::slice::from_raw_parts(ptr, len) };
        let decoded = rmp_serde::from_slice::<T>(bytes).map_err(|e| Error::Codec(e.to_string()));
        unsafe { sys::dv_queue_release(self.raw, id.0) };
        decoded.map(Some)
    }

    /// Take the oldest message as raw msgpack. `None` when the queue is empty.
    pub fn pop_raw(&mut self, id: QueueId) -> Result<Option<Vec<u8>>, Error> {
        let mut ptr: *const u8 = std::ptr::null();
        let mut len: usize = 0;
        let st = unsafe { sys::dv_queue_peek(self.raw, id.0, &mut ptr, &mut len) };
        match st {
            sys::DV_OK => {}
            sys::DV_QUEUE_EMPTY => return Ok(None),
            _ => return Err(Error::UnknownQueue(id)),
        }
        let owned = unsafe { std::slice::from_raw_parts(ptr, len) }.to_vec();
        unsafe { sys::dv_queue_release(self.raw, id.0) };
        Ok(Some(owned))
    }

    /// Start the program, or continue it after it finished a step.
    pub fn run(&mut self) -> Result<Step, Error> {
        let mut ws = sys::dv_waitset::default();
        let st = unsafe { sys::dv_run(self.raw, &mut ws) };
        self.settle(st, ws)
    }

    /// Answer a park: say which queue fired.
    ///
    /// Use [`Instance::resume_timeout`] to say the time ran out instead. What a
    /// fired handle means -- a message, or a queue that has gone away -- is
    /// worked out from the queue, so you never have to model the difference.
    pub fn resume(&mut self, fired: QueueId) -> Result<Step, Error> {
        let st = unsafe { sys::dv_resume(self.raw, fired.0) };
        self.settle_after_resume(st)
    }

    /// Answer a park by saying its timeout elapsed.
    pub fn resume_timeout(&mut self) -> Result<Step, Error> {
        let st = unsafe { sys::dv_resume(self.raw, 0) };
        self.settle_after_resume(st)
    }

    /// `dv_resume` has nowhere to put a wait-set, so when it parks again the
    /// current one is read back explicitly. A zeroed struct would look like a
    /// park on no queues at all, which is the kind of wrong that reads as
    /// working.
    fn settle_after_resume(&mut self, st: sys::dv_status) -> Result<Step, Error> {
        let mut ws = sys::dv_waitset::default();
        if st == sys::DV_IDLE {
            unsafe { sys::dv_waitset_get(self.raw, &mut ws) };
        }
        self.settle(st, ws)
    }

    fn settle(&mut self, st: sys::dv_status, ws: sys::dv_waitset) -> Result<Step, Error> {
        match st {
            sys::DV_DONE => Ok(Step::Done),
            sys::DV_IDLE => Ok(Step::Parked(Self::wait_from(ws))),
            sys::DV_BUSY => Err(Error::Busy(self.last_error())),
            _ => Err(Error::Program(self.last_error())),
        }
    }

    fn wait_from(ws: sys::dv_waitset) -> Wait {
        let n = ws.n as usize;
        Wait {
            ids: ws.ids[..n.min(sys::DV_WAIT_MAX)]
                .iter()
                .map(|&i| QueueId(i))
                .collect(),
            timeout: (ws.timeout_ms >= 0)
                .then(|| std::time::Duration::from_millis(ws.timeout_ms as u64)),
            for_write: ws.for_write != 0,
        }
    }

    /// What the program is waiting for right now, or `None` when it is not
    /// parked.
    ///
    /// [`Instance::run`] and [`Instance::resume`] already hand back the park as
    /// [`Step::Parked`]; this is for the moment neither has run yet -- above
    /// all a freshly [restored](Config::restore) instance, which is parked
    /// exactly as the snapshot's was and whose park was never returned by
    /// anything.
    pub fn current_wait(&self) -> Option<Wait> {
        let mut ws = sys::dv_waitset::default();
        let st = unsafe { sys::dv_waitset_get(self.raw, &mut ws) };
        (st == sys::DV_OK).then(|| Self::wait_from(ws))
    }

    /// Limit what the program may spend, before it starts.
    ///
    /// See [`Config::budget`], which is the same switch applied at
    /// construction and reads better; this exists for a host that decides the
    /// numbers after loading. Refused with [`Error::Busy`] once the instance
    /// has started -- a budget that changed mid-flight would make "exceeded"
    /// mean nothing.
    pub fn set_budget(&mut self, instructions: u64, memory_kb: u64) -> Result<(), Error> {
        let st = unsafe { sys::dv_set_budget(self.raw, instructions, memory_kb) };
        match st {
            sys::DV_OK => Ok(()),
            sys::DV_BUSY => Err(Error::Busy(self.last_error())),
            _ => Err(Error::Program(self.last_error())),
        }
    }

    /// What the instance has spent: exact instructions, and its memory
    /// high-water mark in kilobytes. The peak rather than the current figure,
    /// because a supervisor deciding whether a child needs a larger budget
    /// wants the peak; [`Instance::memory`] answers the other question.
    pub fn usage(&self) -> Usage {
        let mut instructions = 0u64;
        let mut memory_kb = 0u64;
        unsafe { sys::dv_usage(self.raw, &mut instructions, &mut memory_kb) };
        Usage {
            instructions,
            memory_kb_peak: memory_kb,
        }
    }

    /// What the instance is holding *now*, in bytes, beside the same peak
    /// before it is divided into kilobytes.
    ///
    /// This is the number a host asks about a swarm -- what a thing costs at
    /// rest -- where [`Instance::usage`] answers a supervisor's sizing
    /// question. `bytes_now` below `bytes_peak` is the collector having run,
    /// not a disagreement between two counters.
    pub fn memory(&self) -> Memory {
        let mut bytes_now = 0u64;
        let mut bytes_peak = 0u64;
        unsafe { sys::dv_memory(self.raw, &mut bytes_now, &mut bytes_peak) };
        Memory {
            bytes_now,
            bytes_peak,
        }
    }

    /// Did this instance stop because it ran out of budget?
    ///
    /// Distinguishes "the program failed" from "we stopped it", which arrive
    /// as the same [`Error::Program`]: a supervisor restarts one and grows the
    /// other's budget.
    pub fn exceeded(&self) -> bool {
        unsafe { sys::dv_exceeded(self.raw) != 0 }
    }

    /// Hibernate: write the instance's whole state -- the parked program, its
    /// call chain, its reachable values, and every queue with its contents --
    /// into bytes that [`Config::restore`] can wake.
    ///
    /// The instance must be *parked*, which is the only state in which all of
    /// it is written down; snapshotting a running or finished program is an
    /// [`Error::Program`] that says so. `host` is the identity stamp: a
    /// snapshot with no stamp restores anywhere, a stamped one only under the
    /// same string.
    pub fn snapshot(&mut self, host: Option<&str>) -> Result<Vec<u8>, Error> {
        let chost =
            host.map(|h| CString::new(h).unwrap_or_else(|_| CString::new("(host)").unwrap()));
        let hostp = chost.as_ref().map_or(std::ptr::null(), |c| c.as_ptr());
        let mut len: usize = 0;
        let st = unsafe { sys::dv_snapshot(self.raw, hostp, std::ptr::null_mut(), 0, &mut len) };
        // Asking for the size answers DV_OK with `*len` set; anything else is
        // the refusal itself and the buffer would not have helped.
        if st != sys::DV_OK && st != sys::DV_BUFFER_TOO_SMALL {
            return Err(Error::Program(self.last_error()));
        }
        let mut buf = vec![0u8; len];
        let st =
            unsafe { sys::dv_snapshot(self.raw, hostp, buf.as_mut_ptr(), buf.len(), &mut len) };
        if st != sys::DV_OK {
            return Err(Error::Program(self.last_error()));
        }
        buf.truncate(len);
        Ok(buf)
    }

    /// Register a chunk shared across a fleet, so every snapshot of an
    /// instance using it carries a 32-byte hash in place of the code.
    ///
    /// A host that registers nothing gets self-contained snapshots, which is
    /// the right default; this is for the swarm case where one program has ten
    /// thousand snapshots. The same registration must exist wherever those
    /// snapshots wake.
    pub fn register_code(&mut self, code: &[u8], name: &str) -> Result<(), Error> {
        let cname = CString::new(name).unwrap_or_else(|_| CString::new("=(code)").unwrap());
        let st =
            unsafe { sys::dv_register_code(self.raw, code.as_ptr(), code.len(), cname.as_ptr()) };
        if st != sys::DV_OK {
            return Err(Error::Program(self.last_error()));
        }
        Ok(())
    }

    /// Pre-authorise an endpoint reference: when the program binds a reference
    /// with exactly these bytes, it gets an endpoint identified to you by
    /// `token`.
    ///
    /// What the bytes mean is entirely yours -- an index into your own tables,
    /// an address, a name. The runtime carries them and does not read them.
    /// Prefer this over [`Instance::on_endpoint_bind`]: a host almost always
    /// knows what its own references mean, and saying so up front is simpler
    /// than answering a question later.
    pub fn endpoint_allow(&mut self, reference: &[u8], token: u32) {
        unsafe { sys::dv_endpoint_allow(self.raw, reference.as_ptr(), reference.len(), token) };
    }

    /// The queue behind a bound endpoint, by the token you chose -- or `None`
    /// while nothing has bound it.
    ///
    /// An endpoint is a real bounded local queue, so [`Instance::pop`] on this
    /// handle is how messages leave the instance. The buffer being bounded is
    /// the point: a host-side buffer that grew without limit would break the
    /// accept-is-O(1) guarantee rather than extend it.
    pub fn endpoint_queue(&self, token: u32) -> Option<QueueId> {
        let id = unsafe { sys::dv_endpoint_queue(self.raw, token) };
        (id != 0).then_some(QueueId(id))
    }

    /// Say that an endpoint's far end has closed. Pushes to it answer "gone"
    /// from then on, immediately -- and once gone it stays gone: something new
    /// at the same address is a new endpoint.
    pub fn endpoint_close(&mut self, id: QueueId) -> Result<(), Error> {
        let st = unsafe { sys::dv_endpoint_close(self.raw, id.0) };
        if st != sys::DV_OK {
            return Err(Error::UnknownQueue(id));
        }
        Ok(())
    }
}

impl Drop for Instance {
    fn drop(&mut self) {
        unsafe { sys::dv_free(self.raw) };
    }
}

// Written by hand rather than derived: the pointer is not information a host
// wants, and without any Debug at all an Instance cannot be a field of a struct
// that derives it -- which is where most hosts will want to keep one.
impl fmt::Debug for Instance {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Instance").finish_non_exhaustive()
    }
}

/// What an instance has spent, against the budget it was given.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Usage {
    /// VM instructions consumed, exact to the counting hook's granularity.
    pub instructions: u64,
    /// The heap's high-water mark, in kilobytes.
    pub memory_kb_peak: u64,
}

/// What an instance is holding, in bytes.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Memory {
    /// Held right now. This is what an idle agent costs.
    pub bytes_now: u64,
    /// The high-water mark [`Usage::memory_kb_peak`] is derived from.
    pub bytes_peak: u64,
}

/// How a queue is configured, and how full it is.
#[derive(Clone, Copy, Debug)]
pub struct QueueInfo {
    /// The most messages it will hold. Always bounded; there is no unbounded
    /// option anywhere in Diluvium.
    pub capacity: u32,
    /// How many it holds now.
    pub len: u32,
    /// False once the program has stopped accepting on it.
    pub enabled: bool,
    /// Whether the program meant this queue to be visible outside itself.
    pub exported: bool,
}

/// The ABI version this wrapper was built against.
pub fn abi_version() -> u32 {
    sys::DV_ABI_VERSION
}

/// The ABI version the linked library reports.
pub fn library_abi_version() -> u32 {
    unsafe { sys::dv_abi_version() }
}

/// Set a callback for when the program pushes to a queue it exported, so a host
/// need not poll.
///
/// The callback fires synchronously inside `run` or `resume` and **must not
/// touch the instance** -- it is mid-step when your code runs. Record the id and
/// act on it after the step returns.
pub struct Notifier {
    _private: (),
}

impl Instance {
    /// Install a notification callback. See [`Notifier`].
    ///
    /// The closure must be `'static` because the runtime keeps it for the life
    /// of the instance.
    pub fn on_export<F>(&mut self, f: F) -> Notifier
    where
        F: FnMut(QueueId) + Send + 'static,
    {
        unsafe extern "C" fn trampoline<F: FnMut(QueueId)>(ud: *mut c_void, id: sys::dv_queue_id) {
            let f = &mut *(ud as *mut F);
            f(QueueId(id));
        }
        // Leaked on purpose: the runtime holds this pointer until the instance
        // is freed, and there is no later moment at which reclaiming it would
        // be sound. One closure per instance is not a leak worth machinery.
        let boxed: *mut F = Box::into_raw(Box::new(f));
        unsafe {
            sys::dv_set_notify(self.raw, Some(trampoline::<F>), boxed as *mut c_void);
        }
        Notifier { _private: () }
    }

    /// Answer endpoint binds with a callback: the program hands over reference
    /// bytes, and you return the token to know the endpoint by -- or `None` to
    /// refuse, which raises in the program, because it asked for one specific
    /// destination and did not get it.
    ///
    /// References registered with [`Instance::endpoint_allow`] are consulted
    /// first, so the two compose: allow the ones you know, answer the rest
    /// here. The callback fires inside `run` or `resume` and **must not touch
    /// the instance** -- same rule as [`Instance::on_export`], same reason.
    pub fn on_endpoint_bind<F>(&mut self, f: F)
    where
        F: FnMut(&[u8]) -> Option<u32> + Send + 'static,
    {
        unsafe extern "C" fn trampoline<F: FnMut(&[u8]) -> Option<u32>>(
            ud: *mut c_void,
            r: *const u8,
            len: usize,
            token: *mut u32,
        ) -> std::os::raw::c_int {
            let f = &mut *(ud as *mut F);
            match f(std::slice::from_raw_parts(r, len)) {
                Some(t) => {
                    *token = t;
                    1
                }
                None => 0,
            }
        }
        // Leaked for the same reason as `on_export`.
        let boxed: *mut F = Box::into_raw(Box::new(f));
        unsafe {
            sys::dv_set_endpoint_handler(self.raw, Some(trampoline::<F>), boxed as *mut c_void);
        }
    }
}
