//! Host a Diluvium program as a sandboxed wasm module under wasmtime.
//!
//! # Why this is a separate crate from `diluvium`
//!
//! Both talk to the same `dv_*` ABI, and the public API here is deliberately the
//! same shape, so host code moves between them nearly unchanged. What differs is
//! everything underneath, and it differs enough that one crate serving both would
//! collapse to whichever surface is weaker:
//!
//! - **Pointers are offsets.** Every `const uint8_t *` in `dv.h` is a `u32` index
//!   into the module's linear memory, not an address. Nothing outside that memory
//!   can be passed by reference.
//! - **The host allocates inside the guest.** To pass a name or a message in, the
//!   bytes must already be *in* the module's memory, which means calling its
//!   exported `malloc`.
//! - **Struct layouts are wasm32's.** `wasm32` is ILP32, so `dv_config` — which
//!   holds a `size_t` and a pointer — is 16 bytes there and 24 on the LP64 host
//!   compiling this crate. Nothing here hardcodes an offset; `dv_layout` reports
//!   what the runtime was actually built with.
//! - **Zero-copy stops at the memory boundary.** `dv_queue_peek` still avoids the
//!   runtime's own copy, but reading the bytes into host memory is a copy. Calling
//!   that zero-copy would be a lie about performance.
//!
//! # Why you would want this over linking natively
//!
//! Two reasons, and they are the only two:
//!
//! **Containment.** A statically linked Diluvium bug is a bug in your process. A
//! wasm one is confined to the module's linear memory, which the runtime cannot
//! escape. That matters most for exactly the case Diluvium is built for —
//! programs generated and rewritten at runtime, which nobody reviewed.
//!
//! **Fuel.** `wasmtime`'s fuel metering bounds execution from *outside* the
//! guest. §9.4 of the design gives the swarm layer an instruction budget via
//! `lua_sethook`, which is a cooperative mechanism running inside the thing it is
//! limiting; fuel is not. If you are hosting untrusted programs, metering from
//! the outside is the more trustworthy side to meter from — and you get it for
//! free here. See [`Instance::set_fuel`].
//!
//! The cost is speed: a wasm Diluvium is slower than a linked one, and every
//! message crossing the boundary is a copy that the native binding does not make.

use std::collections::HashMap;

use anyhow::{anyhow, bail, Context, Result};
use serde::{de::DeserializeOwned, Serialize};
use wasmtime::{Engine, Instance as WasmInstance, Linker, Memory, Module, Store, TypedFunc};
use wasmtime_wasi::preview1::WasiP1Ctx;
use wasmtime_wasi::WasiCtxBuilder;

/// The ABI version this crate was built against.
pub const ABI_VERSION: u32 = 1;

const DV_OK: i32 = 0;
const DV_QUEUE_FULL: i32 = 1;
const DV_QUEUE_DISABLED: i32 = 2;
const DV_QUEUE_EMPTY: i32 = 4;
const DV_QUEUE_GONE: i32 = 5;
const DV_IDLE: i32 = 6;
const DV_DONE: i32 = 7;
const DV_BUSY: i32 = 11;
const DV_QUEUE_DROPPED: i32 = 13;

/// Indices into what `dv_layout` reports. Kept in step with `dv.h` by name; the
/// *values* behind them come from the runtime, never from here.
mod layout {
    pub const CONFIG_SIZE: usize = 0;
    pub const CONFIG_ABI: usize = 1;
    pub const CONFIG_FLAGS: usize = 2;
    pub const QI_SIZE: usize = 3;
    pub const QI_CAPACITY: usize = 4;
    pub const QI_LEN: usize = 5;
    pub const QI_ENABLED: usize = 6;
    pub const QI_EXPORTED: usize = 7;
    pub const WS_SIZE: usize = 10;
    pub const WS_N: usize = 11;
    pub const WS_IDS: usize = 12;
    pub const WS_TIMEOUT: usize = 13;
    pub const WS_FOR_WRITE: usize = 14;
    pub const COUNT: usize = 15;
}

/// A queue handle, valid only for the instance that issued it.
#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
pub struct QueueId(pub u32);

/// What happened to a push. Full, disabled and gone are answers, not failures.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Accepted {
    Yes,
    DroppedOldest,
    Full,
    Disabled,
    /// The far end of an endpoint has closed.
    Gone,
}

impl Accepted {
    pub fn is_accepted(self) -> bool {
        matches!(self, Accepted::Yes | Accepted::DroppedOldest)
    }
}

/// What a parked program is waiting for.
#[derive(Clone, Debug)]
pub struct Wait {
    pub ids: Vec<QueueId>,
    /// What the program asked for, or `None` for no limit. You own the clock.
    pub timeout: Option<std::time::Duration>,
    /// True when it wants space rather than a message: drain, do not feed.
    pub for_write: bool,
}

/// Where a program got to.
#[derive(Clone, Debug)]
pub enum Step {
    Parked(Wait),
    Done,
}

/// How a queue is configured, and how full it is.
#[derive(Clone, Copy, Debug)]
pub struct QueueInfo {
    pub capacity: u32,
    pub len: u32,
    pub enabled: bool,
    pub exported: bool,
}

/// The wasm module, compiled once and shared by every instance made from it.
///
/// Compiling is the expensive part; keep one of these and make instances from it.
pub struct Runtime {
    engine: Engine,
    module: Module,
    linker: Linker<WasiP1Ctx>,
}

impl Runtime {
    /// Compile a `diluvium.wasm`.
    pub fn new(wasm: &[u8]) -> Result<Self> {
        let mut config = wasmtime::Config::new();
        // Fuel is opt-in per instance but has to be enabled on the engine.
        config.consume_fuel(true);
        let engine = Engine::new(&config)?;
        let module = Module::new(&engine, wasm).context("compiling diluvium.wasm")?;

        // Which module is this? The repository builds two: diluvium_wasi.wasm,
        // which imports WASI preview-1, and a wasm32-unknown-unknown one that
        // imports nothing (that is the browser's, and what bindings/js loads).
        // Adding WASI to the linker serves the first and is harmless to the
        // second, so one crate handles both rather than making the caller pick.
        //
        // What the guest actually needs WASI for is small -- clock_time_get and
        // random_get reach it through Lua's os and math libraries -- and none of
        // it is granted here beyond that: no preopened directories, no
        // environment, no stdin. A program cannot read the host's filesystem
        // because there is nothing mounted to read.
        let mut linker: Linker<WasiP1Ctx> = Linker::new(&engine);
        wasmtime_wasi::preview1::add_to_linker_sync(&mut linker, |cx| cx)?;
        Ok(Runtime { engine, module, linker })
    }

    /// Instantiate and load a program from Lua source.
    pub fn load(&self, source: &str, name: &str) -> Result<Instance> {
        self.load_bytes(source.as_bytes(), name, true)
    }

    /// Load a program that may be precompiled bytecode.
    ///
    /// Under wasmtime this is less alarming than natively: a malformed chunk that
    /// got past the loader's operand checks would still be confined to the
    /// module's memory. It is still not *safe*, only contained.
    pub fn load_bytecode(&self, code: &[u8], name: &str) -> Result<Instance> {
        self.load_bytes(code, name, false)
    }

    fn load_bytes(&self, code: &[u8], name: &str, text_only: bool) -> Result<Instance> {
        // Deliberately bare: stdout and stderr so a program's own diagnostics
        // reach the terminal, and nothing else. No directories, no environment,
        // no arguments -- a sandbox that mounted the host's filesystem would
        // give away the reason to use one.
        let wasi = WasiCtxBuilder::new().inherit_stdout().inherit_stderr().build_p1();
        let mut store = Store::new(&self.engine, wasi);
        // Without fuel the store refuses to run at all once consume_fuel is on,
        // so start with a generous allowance and let the caller narrow it.
        store.set_fuel(u64::MAX / 2)?;
        let inst = self
            .linker
            .instantiate(&mut store, &self.module)
            .context("instantiating diluvium.wasm")?;

        let memory = inst
            .get_memory(&mut store, "memory")
            .ok_or_else(|| anyhow!("the module exports no memory"))?;

        let f = Exports::new(&mut store, &inst)?;

        // The wasm build is not a WASI reactor, so its constructors have to be
        // run by hand -- the same thing doc/repl-reference.html records about
        // run_lua.
        if let Ok(ctors) = inst.get_typed_func::<(), ()>(&mut store, "__wasm_call_ctors") {
            ctors.call(&mut store, ())?;
        }

        let library = f.abi_version.call(&mut store, ())? as u32;
        if library != ABI_VERSION {
            bail!(
                "Diluvium ABI mismatch: the module is version {library}, this crate \
                 expects {ABI_VERSION}"
            );
        }

        let mut me = Instance {
            store,
            f,
            memory,
            layout: [0u32; layout::COUNT],
            ptr: 0,
            waitset: 0,
        };
        me.read_layout()?;

        // dv_config, sized and laid out as the *module* has it.
        let cfg_size = me.layout[layout::CONFIG_SIZE] as usize;
        let cfg = me.alloc(cfg_size)?;
        me.zero(cfg, cfg_size)?;
        me.write_u32(cfg + me.layout[layout::CONFIG_ABI], ABI_VERSION)?;
        me.write_u32(
            cfg + me.layout[layout::CONFIG_FLAGS],
            if text_only { 1 } else { 0 },
        )?;
        let ptr = me.f.new_.call(&mut me.store, cfg as i32)? as u32;
        me.free(cfg)?;
        if ptr == 0 {
            bail!("could not create an instance inside the module");
        }
        me.ptr = ptr;
        me.waitset = me.alloc(me.layout[layout::WS_SIZE] as usize)?;

        let code_ptr = me.write_bytes(code)?;
        let name_ptr = me.write_cstring(name)?;
        let st = me.f.load.call(
            &mut me.store,
            (ptr as i32, code_ptr as i32, code.len() as i32, name_ptr as i32),
        )?;
        me.free(code_ptr)?;
        me.free(name_ptr)?;
        if st != DV_OK {
            let msg = me.last_error()?;
            bail!("{msg}");
        }
        Ok(me)
    }
}

/// Every `dv_*` function this crate calls, resolved once.
struct Exports {
    malloc: TypedFunc<i32, i32>,
    free: TypedFunc<i32, ()>,
    abi_version: TypedFunc<(), i32>,
    layout: TypedFunc<(i32, i32), i32>,
    new_: TypedFunc<i32, i32>,
    free_inst: TypedFunc<i32, ()>,
    load: TypedFunc<(i32, i32, i32, i32), i32>,
    last_error: TypedFunc<i32, i32>,
    queue_lookup: TypedFunc<(i32, i32), i32>,
    queue_state: TypedFunc<(i32, i32, i32), i32>,
    queue_push: TypedFunc<(i32, i32, i32, i32), i32>,
    queue_peek: TypedFunc<(i32, i32, i32, i32), i32>,
    queue_release: TypedFunc<(i32, i32), ()>,
    run: TypedFunc<(i32, i32), i32>,
    resume: TypedFunc<(i32, i32), i32>,
    waitset_get: TypedFunc<(i32, i32), i32>,
    endpoint_queue: TypedFunc<(i32, i32), i32>,
    endpoint_close: TypedFunc<(i32, i32), i32>,
    endpoint_allow: TypedFunc<(i32, i32, i32, i32), ()>,
}

impl Exports {
    fn new(store: &mut Store<WasiP1Ctx>, inst: &WasmInstance) -> Result<Self> {
        macro_rules! f {
            ($name:literal) => {
                inst.get_typed_func(&mut *store, $name)
                    .with_context(|| format!("the module does not export {}", $name))?
            };
        }
        Ok(Exports {
            malloc: f!("malloc"),
            free: f!("free"),
            abi_version: f!("dv_abi_version"),
            layout: f!("dv_layout"),
            new_: f!("dv_new"),
            free_inst: f!("dv_free"),
            load: f!("dv_load"),
            last_error: f!("dv_last_error"),
            queue_lookup: f!("dv_queue_lookup"),
            queue_state: f!("dv_queue_state"),
            queue_push: f!("dv_queue_push"),
            queue_peek: f!("dv_queue_peek"),
            queue_release: f!("dv_queue_release"),
            run: f!("dv_run"),
            resume: f!("dv_resume"),
            waitset_get: f!("dv_waitset_get"),
            endpoint_queue: f!("dv_endpoint_queue"),
            endpoint_close: f!("dv_endpoint_close"),
            endpoint_allow: f!("dv_endpoint_allow"),
        })
    }
}

/// One Diluvium program, sandboxed inside a wasm module.
pub struct Instance {
    store: Store<WasiP1Ctx>,
    f: Exports,
    memory: Memory,
    layout: [u32; layout::COUNT],
    ptr: u32,
    waitset: u32,
}

impl Instance {
    // -- linear memory -----------------------------------------------------

    fn alloc(&mut self, n: usize) -> Result<u32> {
        let p = self.f.malloc.call(&mut self.store, n.max(1) as i32)? as u32;
        if p == 0 {
            bail!("out of memory inside the module");
        }
        Ok(p)
    }

    fn free(&mut self, p: u32) -> Result<()> {
        self.f.free.call(&mut self.store, p as i32)?;
        Ok(())
    }

    fn zero(&mut self, at: u32, n: usize) -> Result<()> {
        let zeros = vec![0u8; n];
        self.memory.write(&mut self.store, at as usize, &zeros)?;
        Ok(())
    }

    fn write_bytes(&mut self, src: &[u8]) -> Result<u32> {
        let p = self.alloc(src.len())?;
        self.memory.write(&mut self.store, p as usize, src)?;
        Ok(p)
    }

    fn write_cstring(&mut self, s: &str) -> Result<u32> {
        let mut b = s.as_bytes().to_vec();
        b.push(0);
        self.write_bytes(&b)
    }

    fn write_u32(&mut self, at: u32, v: u32) -> Result<()> {
        self.memory
            .write(&mut self.store, at as usize, &v.to_le_bytes())?;
        Ok(())
    }

    fn read_u32(&mut self, at: u32) -> Result<u32> {
        let mut b = [0u8; 4];
        self.memory.read(&self.store, at as usize, &mut b)?;
        Ok(u32::from_le_bytes(b))
    }

    fn read_u8(&mut self, at: u32) -> Result<u8> {
        let mut b = [0u8; 1];
        self.memory.read(&self.store, at as usize, &mut b)?;
        Ok(b[0])
    }

    fn read_i64(&mut self, at: u32) -> Result<i64> {
        let mut b = [0u8; 8];
        self.memory.read(&self.store, at as usize, &mut b)?;
        Ok(i64::from_le_bytes(b))
    }

    fn read_vec(&mut self, at: u32, n: usize) -> Result<Vec<u8>> {
        let mut out = vec![0u8; n];
        self.memory.read(&self.store, at as usize, &mut out)?;
        Ok(out)
    }

    fn read_layout(&mut self) -> Result<()> {
        let p = self.alloc(layout::COUNT * 4)?;
        let n = self
            .f
            .layout
            .call(&mut self.store, (p as i32, layout::COUNT as i32))? as usize;
        for i in 0..n.min(layout::COUNT) {
            self.layout[i] = self.read_u32(p + (i as u32) * 4)?;
        }
        self.free(p)?;
        Ok(())
    }

    fn last_error(&mut self) -> Result<String> {
        let p = self.f.last_error.call(&mut self.store, self.ptr as i32)? as u32;
        if p == 0 {
            return Ok("(no message)".into());
        }
        // No length, so scan for the NUL -- in chunks, because reading one byte
        // at a time across the boundary for a traceback is needlessly slow.
        let mut out = Vec::new();
        let mut at = p;
        loop {
            let chunk = self.read_vec(at, 64).unwrap_or_default();
            if chunk.is_empty() {
                break;
            }
            if let Some(i) = chunk.iter().position(|&b| b == 0) {
                out.extend_from_slice(&chunk[..i]);
                break;
            }
            out.extend_from_slice(&chunk);
            at += 64;
        }
        Ok(String::from_utf8_lossy(&out).into_owned())
    }

    // -- fuel --------------------------------------------------------------

    /// Bound how much the program may execute, from outside it.
    ///
    /// This is the reason to host under wasmtime rather than link natively. §9.4
    /// gives the swarm layer an instruction budget through `lua_sethook`, which
    /// runs *inside* the thing it limits; fuel does not, so a program cannot
    /// disable it, outlast it, or spin in C where a hook never fires.
    ///
    /// A program that runs out of fuel makes the next call fail; the instance is
    /// then unusable, which is the intended shape of a budget rather than a
    /// pause.
    pub fn set_fuel(&mut self, fuel: u64) -> Result<()> {
        self.store.set_fuel(fuel)?;
        Ok(())
    }

    /// How much fuel is left, if any was set.
    pub fn fuel(&self) -> Option<u64> {
        self.store.get_fuel().ok()
    }

    // -- queues ------------------------------------------------------------

    /// Find a queue the program declared, or one of the `inbox`/`outbox` pair.
    pub fn queue(&mut self, name: &str) -> Result<Option<QueueId>> {
        let p = self.write_cstring(name)?;
        let id = self
            .f
            .queue_lookup
            .call(&mut self.store, (self.ptr as i32, p as i32))? as u32;
        self.free(p)?;
        Ok((id != 0).then_some(QueueId(id)))
    }

    /// Capacity, current length, and how the queue is configured.
    pub fn queue_info(&mut self, id: QueueId) -> Result<QueueInfo> {
        let size = self.layout[layout::QI_SIZE] as usize;
        let p = self.alloc(size)?;
        let st = self
            .f
            .queue_state
            .call(&mut self.store, (self.ptr as i32, id.0 as i32, p as i32))?;
        if st != DV_OK {
            self.free(p)?;
            bail!("no such queue: {id:?}");
        }
        let info = QueueInfo {
            capacity: self.read_u32(p + self.layout[layout::QI_CAPACITY])?,
            len: self.read_u32(p + self.layout[layout::QI_LEN])?,
            enabled: self.read_u8(p + self.layout[layout::QI_ENABLED])? != 0,
            exported: self.read_u8(p + self.layout[layout::QI_EXPORTED])? != 0,
        };
        self.free(p)?;
        Ok(info)
    }

    /// Send a message, encoding it to msgpack.
    ///
    /// Uses `to_vec_named`, so a struct crosses as a map keyed by field name.
    /// Every binding must agree on that: the compact form would encode a struct
    /// as an array of values and couple the guest to Rust's field order.
    pub fn push<T: Serialize>(&mut self, id: QueueId, value: &T) -> Result<Accepted> {
        let bytes = rmp_serde::to_vec_named(value)?;
        self.push_raw(id, &bytes)
    }

    /// Send msgpack bytes you already have.
    pub fn push_raw(&mut self, id: QueueId, msgpack: &[u8]) -> Result<Accepted> {
        let p = self.write_bytes(msgpack)?;
        let st = self.f.queue_push.call(
            &mut self.store,
            (self.ptr as i32, id.0 as i32, p as i32, msgpack.len() as i32),
        )?;
        self.free(p)?;
        Ok(match st {
            DV_OK => Accepted::Yes,
            DV_QUEUE_DROPPED => Accepted::DroppedOldest,
            DV_QUEUE_FULL => Accepted::Full,
            DV_QUEUE_DISABLED => Accepted::Disabled,
            DV_QUEUE_GONE => Accepted::Gone,
            _ => bail!("no such queue: {id:?}"),
        })
    }

    /// Take the oldest message and decode it. `None` when the queue is empty.
    pub fn pop<T: DeserializeOwned>(&mut self, id: QueueId) -> Result<Option<T>> {
        match self.pop_raw(id)? {
            None => Ok(None),
            Some(bytes) => Ok(Some(rmp_serde::from_slice(&bytes)?)),
        }
    }

    /// Take the oldest message as raw msgpack. `None` when the queue is empty.
    ///
    /// Uses peek/release, which avoids the runtime's own copy. Reading the bytes
    /// out of linear memory is a copy that a natively linked host does not make;
    /// that is the price of the sandbox.
    pub fn pop_raw(&mut self, id: QueueId) -> Result<Option<Vec<u8>>> {
        let pp = self.alloc(4)?;
        let plen = self.alloc(4)?;
        let st = self.f.queue_peek.call(
            &mut self.store,
            (self.ptr as i32, id.0 as i32, pp as i32, plen as i32),
        )?;
        let out = if st == DV_QUEUE_EMPTY {
            None
        } else if st == DV_OK {
            let at = self.read_u32(pp)?;
            let len = self.read_u32(plen)? as usize;
            let bytes = self.read_vec(at, len)?;
            self.f
                .queue_release
                .call(&mut self.store, (self.ptr as i32, id.0 as i32))?;
            Some(bytes)
        } else {
            self.free(pp)?;
            self.free(plen)?;
            bail!("no such queue: {id:?}");
        };
        self.free(pp)?;
        self.free(plen)?;
        Ok(out)
    }

    // -- endpoints ---------------------------------------------------------

    /// Pre-authorise a reference, mapping bytes to a token.
    ///
    /// This is how a wasm host binds endpoints at all. The ABI also takes a C
    /// callback, but a function pointer in wasm is an index into the module's
    /// function table -- there is no way to hand one in from out here -- so
    /// `dv_endpoint_allow` exists, and this is it. Call it before the program
    /// tries to bind.
    pub fn allow_endpoint(&mut self, reference: &[u8], token: u32) -> Result<()> {
        if token == 0 {
            bail!("0 is not a valid endpoint token");
        }
        let p = self.write_bytes(reference)?;
        self.f.endpoint_allow.call(
            &mut self.store,
            (self.ptr as i32, p as i32, reference.len() as i32, token as i32),
        )?;
        self.free(p)?;
        Ok(())
    }

    /// The queue a token was bound to, or `None`. This is the buffer to drain.
    pub fn endpoint_queue(&mut self, token: u32) -> Result<Option<QueueId>> {
        let id = self
            .f
            .endpoint_queue
            .call(&mut self.store, (self.ptr as i32, token as i32))? as u32;
        Ok((id != 0).then_some(QueueId(id)))
    }

    /// Say an endpoint's far end has closed. Pushes answer `Gone` from then on.
    pub fn endpoint_close(&mut self, id: QueueId) -> Result<()> {
        let st = self
            .f
            .endpoint_close
            .call(&mut self.store, (self.ptr as i32, id.0 as i32))?;
        if st != DV_OK {
            bail!("no such endpoint: {id:?}");
        }
        Ok(())
    }

    // -- scheduling --------------------------------------------------------

    /// Start the program, or continue it after a completed step.
    pub fn run(&mut self) -> Result<Step> {
        let st = self
            .f
            .run
            .call(&mut self.store, (self.ptr as i32, self.waitset as i32))?;
        self.settle(st, true)
    }

    /// Answer a park: say which queue fired.
    pub fn resume(&mut self, fired: QueueId) -> Result<Step> {
        let st = self
            .f
            .resume
            .call(&mut self.store, (self.ptr as i32, fired.0 as i32))?;
        self.settle(st, false)
    }

    /// Answer a park by saying its timeout elapsed.
    pub fn resume_timeout(&mut self) -> Result<Step> {
        let st = self.f.resume.call(&mut self.store, (self.ptr as i32, 0))?;
        self.settle(st, false)
    }

    fn settle(&mut self, st: i32, filled: bool) -> Result<Step> {
        match st {
            DV_DONE => Ok(Step::Done),
            DV_IDLE => {
                // dv_resume has nowhere to put a wait-set, and a program looping
                // on queue.wait parks again at once, so read the current one back
                // rather than trusting whatever is in the buffer.
                if !filled {
                    self.f
                        .waitset_get
                        .call(&mut self.store, (self.ptr as i32, self.waitset as i32))?;
                }
                let base = self.waitset;
                let n = self.read_u32(base + self.layout[layout::WS_N])? as usize;
                let ids_at = base + self.layout[layout::WS_IDS];
                let mut ids = Vec::with_capacity(n);
                for i in 0..n {
                    ids.push(QueueId(self.read_u32(ids_at + (i as u32) * 4)?));
                }
                let ms = self.read_i64(base + self.layout[layout::WS_TIMEOUT])?;
                let for_write = self.read_u8(base + self.layout[layout::WS_FOR_WRITE])? != 0;
                Ok(Step::Parked(Wait {
                    ids,
                    timeout: (ms >= 0).then(|| std::time::Duration::from_millis(ms as u64)),
                    for_write,
                }))
            }
            DV_BUSY => {
                let m = self.last_error()?;
                bail!("{m}")
            }
            _ => {
                let m = self.last_error()?;
                bail!("{m}")
            }
        }
    }
}

impl Drop for Instance {
    fn drop(&mut self) {
        // Best effort: a store that has trapped or run out of fuel cannot be
        // called into, and there is nothing useful to do about it here -- the
        // whole linear memory goes when the store does.
        let _ = self.f.free_inst.call(&mut self.store, self.ptr as i32);
    }
}

impl std::fmt::Debug for Instance {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Instance")
            .field("ptr", &self.ptr)
            .finish_non_exhaustive()
    }
}

/// A relay that moves messages between two sandboxed instances.
///
/// Here so that §7.4's argument is demonstrable rather than only asserted:
/// routing is not a runtime feature. This is the entirety of a host's part in
/// getting a message from one instance to another — drain a bounded endpoint
/// buffer, push into an inbox — and it knows nothing about what the messages
/// mean.
pub fn relay(
    from: &mut Instance,
    endpoint: QueueId,
    to: &mut Instance,
    inbox: QueueId,
) -> Result<usize> {
    let mut moved = 0;
    while let Some(bytes) = from.pop_raw(endpoint)? {
        if !to.push_raw(inbox, &bytes)?.is_accepted() {
            // Not an error: a full inbox is an answer, and putting the message
            // back is the relay's business rather than the runtime's.
            break;
        }
        to.resume(inbox)?;
        moved += 1;
    }
    Ok(moved)
}

/// Tokens a host has handed out, if it wants somewhere to keep them.
///
/// Not required by anything; a convenience for the common case where a reference
/// is just an index into a table of instances the host already has.
#[derive(Default, Debug)]
pub struct Endpoints {
    next: u32,
    names: HashMap<u32, String>,
}

impl Endpoints {
    /// Mint a token for a named destination, and the reference bytes to send.
    pub fn mint(&mut self, name: &str) -> (u32, Vec<u8>) {
        self.next += 1;
        let token = self.next;
        self.names.insert(token, name.to_string());
        (token, token.to_string().into_bytes())
    }

    /// What a token was minted for.
    pub fn name(&self, token: u32) -> Option<&str> {
        self.names.get(&token).map(|s| s.as_str())
    }

    /// msgpack for an endpoint reference carrying these bytes: ext 0x02.
    pub fn as_message(bytes: &[u8]) -> Vec<u8> {
        let mut out = Vec::with_capacity(bytes.len() + 3);
        match bytes.len() {
            1 => out.push(0xd4),
            2 => out.push(0xd5),
            4 => out.push(0xd6),
            8 => out.push(0xd7),
            16 => out.push(0xd8),
            n => {
                out.push(0xc7);
                out.push(n as u8);
            }
        }
        out.push(0x02);
        out.extend_from_slice(bytes);
        out
    }
}
