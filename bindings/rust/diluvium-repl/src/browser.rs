//! The browser: an entry point, and the libc floor underneath it.
//!
//! # Surface
//!
//! Entry points: [`start`], which a page calls with its xterm.js terminal.
//!
//! Configurable values: [`HEAP_FD_LIMIT`] -- the highest descriptor this
//! answers for at all.
//!
//! Fan-out points: the `wasi` module below is the whole host interface.
//! Every function in it is a `wasi_snapshot_preview1` call wasi-libc would
//! otherwise import, and `fd_write` is the only one that does real work.
//!
//! # Why there is a libc here at all
//!
//! `wasm32-unknown-unknown` has no libc, and the Diluvium core is C. The
//! obvious reading of `bindings/rust/WASM-SPIKE.md`'s "libc from the
//! embedder" is that a page supplies 56 functions -- `malloc`, `snprintf`,
//! `strftime` and the rest -- and that is what `src/wasm_stubs_unknown.c`
//! does for the JavaScript artifact.
//!
//! It is the wrong shape for a Rust host, and not only because writing
//! `snprintf` again is unpleasant. That file's `sprintf` returns an empty
//! string and its `setjmp` traps, which is honest for a demo and wrong for
//! a runtime: `tostring(1.5)` would be empty and `pcall` would abort the
//! module.
//!
//! So `diluvium-sys` links wasi-libc into the browser build instead. Most
//! of what Lua asks for is pure computation and references no syscall, so
//! the linker takes it and leaves only what genuinely faces an operating
//! system: seventeen `wasi_snapshot_preview1` calls, which this module
//! answers. A page therefore needs the wasm-bindgen glue and nothing else
//! -- no WASI shim, no libc -- which is also what lets the tests run under
//! `wasm-bindgen-test`, since that harness would have no way to inject one.
//!
//! # depth: the WASI floor

pub use wasi::set_stdout;

use wasm_bindgen::prelude::*;

/// Descriptors above this are not ours to answer for. 0, 1 and 2 are
/// stdin, stdout and stderr; a sealed browser instance opens nothing else.
const HEAP_FD_LIMIT: i32 = 2;

/// The seventeen host calls wasi-libc could not resolve on its own.
///
/// Names are the wrappers wasi-libc references
/// (`__imported_wasi_snapshot_preview1_*`); defining them here means the
/// linker resolves them locally and the module imports nothing from
/// `wasi_snapshot_preview1` at all.
///
/// Everything that faces a filesystem returns `ENOTCAPABLE`, which is the
/// truth: a browser instance has no files, and saying so is better than
/// pretending an open succeeded. `fd_write` is the one that matters, and
/// the one Lua's `print` arrives through.
pub mod wasi {
    use super::*;
    use std::cell::RefCell;

    /// wasi errno values, from the preview1 definitions.
    const ESUCCESS: i32 = 0;
    const EBADF: i32 = 8;
    const ENOTCAPABLE: i32 = 76;

    thread_local! {
        /// The terminal `fd_write` writes to, installed by `start`.
        static STDOUT: RefCell<Option<XtermSink>> = const { RefCell::new(None) };
    }

    #[wasm_bindgen]
    extern "C" {
        pub type XtermSink;
        #[wasm_bindgen(method, js_name = write)]
        fn js_write(this: &XtermSink, data: &str);
    }

    pub fn set_stdout(handle: JsValue) {
        STDOUT.with(|slot| *slot.borrow_mut() = Some(handle.unchecked_into()));
    }

    /// Read one `__wasi_ciovec_t` array out of linear memory.
    ///
    /// # Safety
    /// `iovs` points at `len` pairs of (pointer, length), which is what
    /// wasi-libc passed; nothing else calls this.
    unsafe fn gather(iovs: i32, len: i32) -> Vec<u8> {
        let mut out = Vec::new();
        let base = iovs as *const u32;
        for index in 0..len as usize {
            let ptr = unsafe { base.add(index * 2).read_unaligned() } as *const u8;
            let count = unsafe { base.add(index * 2 + 1).read_unaligned() } as usize;
            out.extend_from_slice(unsafe { std::slice::from_raw_parts(ptr, count) });
        }
        out
    }

    /// The one that does real work: whatever the program printed, onto the
    /// terminal. xterm.js has no line discipline, so a bare newline has to
    /// become a carriage return and a newline or the next line starts in
    /// the wrong column.
    #[unsafe(no_mangle)]
    pub extern "C" fn __imported_wasi_snapshot_preview1_fd_write(
        fd: i32,
        iovs: i32,
        iovs_len: i32,
        nwritten: i32,
    ) -> i32 {
        if fd > HEAP_FD_LIMIT || fd == 0 {
            return EBADF;
        }
        let bytes = unsafe { gather(iovs, iovs_len) };
        let text = String::from_utf8_lossy(&bytes).replace('\n', "\r\n");
        STDOUT.with(|slot| {
            if let Some(sink) = slot.borrow().as_ref() {
                sink.js_write(&text);
            }
        });
        // Report every byte written even when there is no terminal yet:
        // a short write would send stdio into a retry loop.
        unsafe { (nwritten as *mut u32).write_unaligned(bytes.len() as u32) };
        ESUCCESS
    }

    /// Nothing to read. The prompt owns input; this is the C side's stdin,
    /// which a browser instance does not have. Zero bytes is end of input.
    #[unsafe(no_mangle)]
    pub extern "C" fn __imported_wasi_snapshot_preview1_fd_read(
        _fd: i32,
        _iovs: i32,
        _iovs_len: i32,
        nread: i32,
    ) -> i32 {
        unsafe { (nread as *mut u32).write_unaligned(0) };
        ESUCCESS
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn __imported_wasi_snapshot_preview1_clock_time_get(
        _id: i32,
        _precision: i64,
        time: i32,
    ) -> i32 {
        // Milliseconds since the epoch, in nanoseconds. `Date.now` rather
        // than `performance.now` because os.time wants a wall clock, and
        // browsers coarsen both anyway.
        let nanos = (js_sys::Date::now() * 1.0e6) as u64;
        unsafe { (time as *mut u64).write_unaligned(nanos) };
        ESUCCESS
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn __imported_wasi_snapshot_preview1_environ_sizes_get(
        count: i32,
        size: i32,
    ) -> i32 {
        unsafe {
            (count as *mut u32).write_unaligned(0);
            (size as *mut u32).write_unaligned(0);
        }
        ESUCCESS
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn __imported_wasi_snapshot_preview1_environ_get(_env: i32, _buf: i32) -> i32 {
        ESUCCESS
    }

    /// `os.exit` in a page. There is no process to end, so this raises into
    /// JavaScript rather than pretending to succeed.
    #[unsafe(no_mangle)]
    pub extern "C" fn __imported_wasi_snapshot_preview1_proc_exit(code: i32) -> ! {
        wasm_bindgen::throw_str(&format!("the program called exit({code})"));
    }

    // The rest face a filesystem this instance does not have. ENOTCAPABLE
    // is preview1's "the capability was never granted", which is exactly
    // the case, and it is what makes io.open return an error rather than a
    // file that then misbehaves.

    macro_rules! refuse {
        ($($name:ident($($arg:ident: $ty:ty),*);)*) => {$(
            #[unsafe(no_mangle)]
            pub extern "C" fn $name($(_: $ty),*) -> i32 { ENOTCAPABLE }
        )*};
    }

    refuse! {
        __imported_wasi_snapshot_preview1_fd_close(fd: i32);
        __imported_wasi_snapshot_preview1_fd_fdstat_get(fd: i32, buf: i32);
        __imported_wasi_snapshot_preview1_fd_fdstat_set_flags(fd: i32, flags: i32);
        __imported_wasi_snapshot_preview1_fd_prestat_get(fd: i32, buf: i32);
        __imported_wasi_snapshot_preview1_fd_prestat_dir_name(fd: i32, path: i32, len: i32);
        __imported_wasi_snapshot_preview1_fd_renumber(from: i32, to: i32);
        __imported_wasi_snapshot_preview1_path_remove_directory(fd: i32, path: i32, len: i32);
        __imported_wasi_snapshot_preview1_path_unlink_file(fd: i32, path: i32, len: i32);
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn __imported_wasi_snapshot_preview1_fd_seek(
        _fd: i32,
        _offset: i64,
        _whence: i32,
        _newoffset: i32,
    ) -> i32 {
        ENOTCAPABLE
    }

    #[unsafe(no_mangle)]
    #[allow(clippy::too_many_arguments)]
    pub extern "C" fn __imported_wasi_snapshot_preview1_path_open(
        _fd: i32,
        _dirflags: i32,
        _path: i32,
        _path_len: i32,
        _oflags: i32,
        _rights: i64,
        _rights_inheriting: i64,
        _fdflags: i32,
        _opened: i32,
    ) -> i32 {
        ENOTCAPABLE
    }

    #[unsafe(no_mangle)]
    #[allow(clippy::too_many_arguments)]
    pub extern "C" fn __imported_wasi_snapshot_preview1_path_rename(
        _fd: i32,
        _old: i32,
        _old_len: i32,
        _new_fd: i32,
        _new: i32,
        _new_len: i32,
    ) -> i32 {
        ENOTCAPABLE
    }
}
