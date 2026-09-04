//! Raw FFI for the Diluvium instance ABI. Nothing safe lives here.
//!
//! These declarations mirror `src/dv.h` exactly, including the comments that
//! matter for correctness. The safe wrapper in the `diluvium` crate is where
//! lifetimes and the threading rule are enforced; this crate is deliberately a
//! transcription, so a reader can diff it against the header.
//!
//! # Surface
//!
//! Entry points: `dv_new` / `dv_free` bracket an instance, `dv_load` gives it
//! a program, `dv_run` / `dv_resume` drive it, and the `dv_queue_*` family
//! moves msgpack across the boundary. `dv_abi_version` is called first and its
//! answer refused on mismatch.
//!
//! Configurable values: [`DV_ABI_VERSION`] and [`DV_WAIT_MAX`], which are
//! transcribed rather than chosen, and the `DV_FLAG_*` set, which is what a
//! host actually decides.
//!
//! Fan-out points: two modules, and the reason there are two is a boundary.
//! This one is `dv.h` -- sealed, bytes in and bytes out, no Lua type crossing.
//! [`lua`] is the Lua C API, behind the off-by-default `lua` feature, for a
//! front end that has to reach `drepl.c` and therefore cannot stay sealed.
#![allow(non_camel_case_types)]

#[cfg(feature = "lua")]
pub mod lua;

use std::os::raw::{c_char, c_int, c_void};

pub const DV_ABI_VERSION: u32 = 1;
pub const DV_WAIT_MAX: usize = 32;

/// Refuse precompiled chunks in `dv_load`, accepting source only.
pub const DV_FLAG_TEXT_ONLY: u32 = 0x1;

/// Open the whole `debug` library rather than the narrowed one.
///
/// Restores the endpoint-forgery route and lets a program switch off its own
/// instruction budget. For hosts whose programs are their own.
pub const DV_FLAG_UNSAFE_DEBUG: u32 = 0x2;

/// Put `io`, `os` and `package` back, with `dofile` and `loadfile`.
///
/// Scaffolding for programs that predate the sealed default. It costs the
/// instruction budget its meaning and the swarm its replayability.
pub const DV_FLAG_UNSAFE_STDLIB: u32 = 0x4;

pub type dv_status = c_int;

pub const DV_OK: dv_status = 0;
pub const DV_QUEUE_FULL: dv_status = 1;
pub const DV_QUEUE_DISABLED: dv_status = 2;
pub const DV_QUEUE_UNKNOWN: dv_status = 3;
pub const DV_QUEUE_EMPTY: dv_status = 4;
pub const DV_QUEUE_GONE: dv_status = 5;
pub const DV_IDLE: dv_status = 6;
pub const DV_DONE: dv_status = 7;
pub const DV_ERROR: dv_status = 8;
pub const DV_ABI_MISMATCH: dv_status = 9;
pub const DV_SNAPSHOT_MISMATCH: dv_status = 10;
pub const DV_BUSY: dv_status = 11;
pub const DV_BUFFER_TOO_SMALL: dv_status = 12;
pub const DV_QUEUE_DROPPED: dv_status = 13;

pub type dv_queue_id = u32;

/// Indices into the array `dv_layout` fills: the sizes and field offsets of the
/// structs below, as the *library* was compiled. This exists for wasm -- a
/// binding reading fields out of linear memory has no `offsetof`, and wasm32 is
/// ILP32, so every struct holding a pointer or a `size_t` is laid out
/// differently there than on the LP64 host where a developer would have
/// measured it. Native bindings use it as a transcription check instead.
pub const DV_LAYOUT_CONFIG_SIZE: usize = 0;
pub const DV_LAYOUT_CONFIG_ABI: usize = 1;
pub const DV_LAYOUT_CONFIG_FLAGS: usize = 2;
pub const DV_LAYOUT_QUEUE_INFO_SIZE: usize = 3;
pub const DV_LAYOUT_QUEUE_INFO_CAPACITY: usize = 4;
pub const DV_LAYOUT_QUEUE_INFO_LEN: usize = 5;
pub const DV_LAYOUT_QUEUE_INFO_ENABLED: usize = 6;
pub const DV_LAYOUT_QUEUE_INFO_EXPORTED: usize = 7;
pub const DV_LAYOUT_QUEUE_INFO_DIRECTION: usize = 8;
pub const DV_LAYOUT_QUEUE_INFO_ON_FULL: usize = 9;
pub const DV_LAYOUT_WAITSET_SIZE: usize = 10;
pub const DV_LAYOUT_WAITSET_N: usize = 11;
pub const DV_LAYOUT_WAITSET_IDS: usize = 12;
pub const DV_LAYOUT_WAITSET_TIMEOUT: usize = 13;
pub const DV_LAYOUT_WAITSET_FOR_WRITE: usize = 14;
pub const DV_LAYOUT_COUNT: usize = 15;

#[repr(C)]
pub struct dv_instance {
    _opaque: [u8; 0],
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct dv_config {
    pub abi_version: u32,
    pub flags: u32,
    pub memory_limit: usize,
    pub reserved: *mut c_void,
}

impl Default for dv_config {
    fn default() -> Self {
        dv_config {
            abi_version: DV_ABI_VERSION,
            flags: 0,
            memory_limit: 0,
            reserved: std::ptr::null_mut(),
        }
    }
}

#[repr(C)]
#[derive(Clone, Copy, Default, Debug)]
pub struct dv_queue_info {
    pub capacity: u32,
    pub len: u32,
    pub enabled: u8,
    pub exported: u8,
    pub direction: u8,
    pub on_full: u8,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct dv_waitset {
    pub n: u32,
    pub ids: [dv_queue_id; DV_WAIT_MAX],
    pub timeout_ms: i64,
    pub for_write: u8,
}

impl Default for dv_waitset {
    fn default() -> Self {
        dv_waitset {
            n: 0,
            ids: [0; DV_WAIT_MAX],
            timeout_ms: -1,
            for_write: 0,
        }
    }
}

extern "C" {
    pub fn dv_abi_version() -> u32;
    pub fn dv_status_name(s: dv_status) -> *const c_char;

    pub fn dv_new(cfg: *const dv_config) -> *mut dv_instance;
    pub fn dv_free(inst: *mut dv_instance);
    pub fn dv_load(
        inst: *mut dv_instance,
        code: *const u8,
        len: usize,
        name: *const c_char,
    ) -> dv_status;
    pub fn dv_last_error(inst: *mut dv_instance) -> *const c_char;

    pub fn dv_queue_lookup(inst: *mut dv_instance, name: *const c_char) -> dv_queue_id;
    pub fn dv_queue_state(
        inst: *mut dv_instance,
        id: dv_queue_id,
        out: *mut dv_queue_info,
    ) -> dv_status;
    pub fn dv_queue_push(
        inst: *mut dv_instance,
        id: dv_queue_id,
        msgpack: *const u8,
        len: usize,
    ) -> dv_status;
    pub fn dv_queue_pop(
        inst: *mut dv_instance,
        id: dv_queue_id,
        buf: *mut u8,
        cap: usize,
        out_len: *mut usize,
    ) -> dv_status;
    pub fn dv_queue_peek(
        inst: *mut dv_instance,
        id: dv_queue_id,
        ptr: *mut *const u8,
        out_len: *mut usize,
    ) -> dv_status;
    pub fn dv_queue_release(inst: *mut dv_instance, id: dv_queue_id);

    pub fn dv_run(inst: *mut dv_instance, out_waitset: *mut dv_waitset) -> dv_status;
    pub fn dv_resume(inst: *mut dv_instance, fired: dv_queue_id) -> dv_status;
    pub fn dv_waitset_get(inst: *mut dv_instance, out: *mut dv_waitset) -> dv_status;

    /// Answer a guest binding an endpoint reference: set `*token` and return 1,
    /// or return 0 to refuse. References registered with `dv_endpoint_allow`
    /// are consulted before any handler installed here, so the two compose.
    pub fn dv_set_endpoint_handler(
        inst: *mut dv_instance,
        bind: Option<
            unsafe extern "C" fn(
                ud: *mut c_void,
                r: *const u8,
                len: usize,
                token: *mut u32,
            ) -> c_int,
        >,
        ud: *mut c_void,
    );
    /// Pre-authorise a reference, mapping bytes to a token, with no callback.
    /// Prefer this; it is also the only shape reachable through wasm.
    pub fn dv_endpoint_allow(inst: *mut dv_instance, r: *const u8, len: usize, token: u32);
    /// The queue handle a token was bound to, or 0 if nothing bound it. This is
    /// the buffer to drain: `dv_queue_pop` on it is how messages leave.
    pub fn dv_endpoint_queue(inst: *mut dv_instance, token: u32) -> dv_queue_id;
    /// Say the far end has closed. Once gone it stays gone.
    pub fn dv_endpoint_close(inst: *mut dv_instance, id: dv_queue_id) -> dv_status;

    /// Set before `dv_run` or `dv_restore`; refused (DV_BUSY) once the instance
    /// has started. `instructions` is a VM instruction count, `memory_kb` a
    /// heap cap; 0 means no limit. The budget aborts, it does not schedule.
    pub fn dv_set_budget(inst: *mut dv_instance, instructions: u64, memory_kb: u64) -> dv_status;
    /// What the instance has spent. `memory_kb` is the high-water mark. Either
    /// pointer may be NULL.
    pub fn dv_usage(
        inst: *mut dv_instance,
        instructions: *mut u64,
        memory_kb: *mut u64,
    ) -> dv_status;
    /// What the instance holds *now*, in bytes, beside the same peak before it
    /// is divided into kilobytes. Either pointer may be NULL.
    pub fn dv_memory(
        inst: *mut dv_instance,
        bytes_now: *mut u64,
        bytes_peak: *mut u64,
    ) -> dv_status;
    /// Did this instance stop because it ran out of budget?
    pub fn dv_exceeded(inst: *mut dv_instance) -> c_int;

    /// Write a parked instance's whole state into `out`; pass out == NULL to
    /// ask only for the size. `host` is the identity stamp, or NULL for none.
    pub fn dv_snapshot(
        inst: *mut dv_instance,
        host: *const c_char,
        out: *mut u8,
        cap: usize,
        len: *mut usize,
    ) -> dv_status;
    /// Restore into a *fresh* instance. After DV_OK it is parked exactly as the
    /// snapshot's was: read the park with `dv_waitset_get`, then `dv_resume` --
    /// not `dv_run`. Refuses on any malformed input rather than raising.
    pub fn dv_restore(
        inst: *mut dv_instance,
        host: *const c_char,
        s: *const u8,
        len: usize,
    ) -> dv_status;
    /// Register a shared chunk so snapshots carry a 32-byte hash in its place.
    pub fn dv_register_code(
        inst: *mut dv_instance,
        code: *const u8,
        len: usize,
        name: *const c_char,
    ) -> dv_status;

    /// Fill `out` with up to `n` of the DV_LAYOUT_COUNT size/offset values, in
    /// DV_LAYOUT_* order; returns how many were written.
    pub fn dv_layout(out: *mut u32, n: usize) -> u32;

    pub fn dv_set_notify(
        inst: *mut dv_instance,
        cb: Option<unsafe extern "C" fn(ud: *mut c_void, id: dv_queue_id)>,
        ud: *mut c_void,
    );
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::mem::{offset_of, size_of};

    /// §11.6: every binding checks the version at init and refuses a mismatch.
    /// Here that is a compile-and-link check as much as a value check -- if the
    /// constant and the library disagree, one of them was edited alone.
    #[test]
    fn version_matches_the_header() {
        assert_eq!(unsafe { dv_abi_version() }, DV_ABI_VERSION);
    }

    /// The transcription check `dv_layout` makes possible on a native target:
    /// the library reports the sizes and offsets it was compiled with, and the
    /// `#[repr(C)]` structs here must land every field in the same place. A
    /// wasm binding *reads* these numbers; this crate gets to *assert* them.
    #[test]
    fn struct_layout_matches_the_library() {
        let mut out = [0u32; DV_LAYOUT_COUNT];
        let n = unsafe { dv_layout(out.as_mut_ptr(), out.len()) } as usize;
        assert_eq!(n, DV_LAYOUT_COUNT, "the library knows entries we do not");

        assert_eq!(out[DV_LAYOUT_CONFIG_SIZE] as usize, size_of::<dv_config>());
        assert_eq!(
            out[DV_LAYOUT_CONFIG_ABI] as usize,
            offset_of!(dv_config, abi_version)
        );
        assert_eq!(
            out[DV_LAYOUT_CONFIG_FLAGS] as usize,
            offset_of!(dv_config, flags)
        );

        assert_eq!(
            out[DV_LAYOUT_QUEUE_INFO_SIZE] as usize,
            size_of::<dv_queue_info>()
        );
        assert_eq!(
            out[DV_LAYOUT_QUEUE_INFO_CAPACITY] as usize,
            offset_of!(dv_queue_info, capacity)
        );
        assert_eq!(
            out[DV_LAYOUT_QUEUE_INFO_LEN] as usize,
            offset_of!(dv_queue_info, len)
        );
        assert_eq!(
            out[DV_LAYOUT_QUEUE_INFO_ENABLED] as usize,
            offset_of!(dv_queue_info, enabled)
        );
        assert_eq!(
            out[DV_LAYOUT_QUEUE_INFO_EXPORTED] as usize,
            offset_of!(dv_queue_info, exported)
        );
        assert_eq!(
            out[DV_LAYOUT_QUEUE_INFO_DIRECTION] as usize,
            offset_of!(dv_queue_info, direction)
        );
        assert_eq!(
            out[DV_LAYOUT_QUEUE_INFO_ON_FULL] as usize,
            offset_of!(dv_queue_info, on_full)
        );

        assert_eq!(
            out[DV_LAYOUT_WAITSET_SIZE] as usize,
            size_of::<dv_waitset>()
        );
        assert_eq!(out[DV_LAYOUT_WAITSET_N] as usize, offset_of!(dv_waitset, n));
        assert_eq!(
            out[DV_LAYOUT_WAITSET_IDS] as usize,
            offset_of!(dv_waitset, ids)
        );
        assert_eq!(
            out[DV_LAYOUT_WAITSET_TIMEOUT] as usize,
            offset_of!(dv_waitset, timeout_ms)
        );
        assert_eq!(
            out[DV_LAYOUT_WAITSET_FOR_WRITE] as usize,
            offset_of!(dv_waitset, for_write)
        );
    }
}
