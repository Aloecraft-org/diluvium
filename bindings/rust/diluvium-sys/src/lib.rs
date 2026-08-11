//! Raw FFI for the Diluvium instance ABI. Nothing safe lives here.
//!
//! These declarations mirror `src/dv.h` exactly, including the comments that
//! matter for correctness. The safe wrapper in the `diluvium` crate is where
//! lifetimes and the threading rule are enforced; this crate is deliberately a
//! transcription, so a reader can diff it against the header.
#![allow(non_camel_case_types)]

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

    pub fn dv_set_notify(
        inst: *mut dv_instance,
        cb: Option<unsafe extern "C" fn(ud: *mut c_void, id: dv_queue_id)>,
        ud: *mut c_void,
    );
}

#[cfg(test)]
mod tests {
    /// §11.6: every binding checks the version at init and refuses a mismatch.
    /// Here that is a compile-and-link check as much as a value check -- if the
    /// constant and the library disagree, one of them was edited alone.
    #[test]
    fn version_matches_the_header() {
        assert_eq!(unsafe { super::dv_abi_version() }, super::DV_ABI_VERSION);
    }
}
