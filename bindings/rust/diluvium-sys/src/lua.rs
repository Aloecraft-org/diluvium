//! The Lua C API, narrowed to what an interactive front end needs.
//!
//! # Surface
//!
//! Entry points: [`luaL_newstate`] and [`lua_close`] bracket a state;
//! [`luaL_openselectedlibs`] and [`diluvium_openlibs`] fill it;
//! [`diluvium_repl_load`] and [`diluvium_repl_complete`] are the two
//! functions this module exists to reach. Everything else declared here is
//! the stack handling those six require, plus [`lua_sethook`] for
//! interrupting a running chunk.
//!
//! Configurable values: none. Every `LUA_*` constant below is transcribed
//! from a header, not chosen here.
//!
//! Fan-out points: none. This module is a transcription with no logic. Its
//! one departure from the headers is the numeric aliases, which are
//! `cfg`-selected -- see "The widths are not the same everywhere".
//!
//! # Why this exists next to a sealed ABI
//!
//! `dv.h` is emphatic: bytes in, bytes out, no Lua type crosses the
//! boundary. A REPL cannot be built on that. `src/drepl.c` exists precisely
//! so a front end does not have to reimplement "unfinished versus broken"
//! by matching Lua's error strings, and both of the functions it offers take
//! a `lua_State *`. Reaching them means reaching past the seal.
//!
//! That is what the `lua` feature says out loud. Turning it on is the same
//! kind of claim [`crate::DV_FLAG_UNSAFE_STDLIB`] makes -- this host's
//! programs are its own -- and it is off by default so that no one acquires
//! a `lua_State` without having said so.
//!
//! The symbols are in `libdiluvium.a` either way: `src/onelua.c` compiles
//! `lauxlib.c`, `linit.c` and `drepl.c` into every build, and `LUA_API` is
//! plain `extern` off Windows. The feature gates the declarations, not the
//! code.
//!
//! # The widths, and why they are the same everywhere now
//!
//! `lua_Integer` is `long long` and `lua_Number` is `double`, on every
//! target. That is not the platform's doing: `luaconf.h` pins both, so that
//! a compiled chunk loads wherever it is carried (doc/ROADMAP.md, "Numeric
//! types and portable bytecode").
//!
//! It was not always true, and the history is why the check below stays.
//! The browser build passes `-DLUA_USE_C89` for the wasm shim's sake, and
//! stock `luaconf.h` reads that as a statement about integer width too,
//! taking `long` -- 32 bits on wasm32. These aliases were `cfg`-selected to
//! match. When the pin landed the C changed and the `cfg` did not, so this
//! module claimed 4 bytes where the library had 8, on the one target where
//! the two ever differed.
//!
//! [`luaL_checkversion`] found it, which is the whole point of wiring it up:
//! it is Lua's own check that the library was compiled with the sizes the
//! caller believes in, and it failed loudly in a browser rather than
//! misreading a number. Call it once after [`luaL_newstate`], the way
//! `src/lua.c` does, and keep calling it even though the pin makes a
//! mismatch unlikely -- unlikely is what this was.
//!
//! # Safety
//!
//! Nothing here is safe. A `lua_State` is single-threaded, every stack index
//! is unchecked, and a function that can raise a Lua error longjmps past
//! Rust frames -- which is why anything that can raise belongs inside
//! [`lua_pcall`], and why a Rust `lua_CFunction` must not hold a value that
//! needs dropping when it does.

// The C spellings are kept: `L` for the state, `luaL_` for the auxiliary
// library. This crate exists to be diffed against the headers, and a reader
// doing that should not have to translate names on the way.
#![allow(non_snake_case)]

use std::os::raw::{c_char, c_int, c_void};

/* ====================================================================== */
/* Types                                                                  */
/* ====================================================================== */

/// An opaque Lua state. One thread at a time, exactly as `dv_instance`.
#[repr(C)]
pub struct lua_State {
    _opaque: [u8; 0],
}

/// Opaque here, deliberately.
///
/// `lua_Debug` is a real struct with a layout that depends on
/// `LUA_IDSIZE`, but the only thing this module does with one is receive a
/// pointer in a hook and hand it back. Transcribing the fields would be
/// transcribing something nothing here reads.
#[repr(C)]
pub struct lua_Debug {
    _opaque: [u8; 0],
}

/// `LUA_INTEGER`: `long long`, pinned in `luaconf.h` rather than inferred,
/// so it is the same on every target. Checked by [`luaL_checkversion`].
pub type lua_Integer = i64;

/// `LUA_UNSIGNED`, which `luaconf.h` defines as `unsigned LUA_INTEGER`.
pub type lua_Unsigned = u64;

/// `LUA_NUMBER`: `double`, pinned alongside the integer.
pub type lua_Number = f64;

/// `LUA_KCONTEXT`: `intptr_t`, or `ptrdiff_t` under C89. Pointer-sized
/// either way.
pub type lua_KContext = isize;

pub type lua_CFunction = unsafe extern "C" fn(L: *mut lua_State) -> c_int;
pub type lua_KFunction =
    unsafe extern "C" fn(L: *mut lua_State, status: c_int, ctx: lua_KContext) -> c_int;
pub type lua_Hook = unsafe extern "C" fn(L: *mut lua_State, ar: *mut lua_Debug);

/* ====================================================================== */
/* Constants                                                              */
/* ====================================================================== */

/// `LUA_VERSION_NUM`: major * 100 + minor.
pub const LUA_VERSION_NUM: c_int = 505;

pub const LUA_MULTRET: c_int = -1;

/// `-(INT_MAX/2 + 1000)`, computed the way `lua.h` computes it.
pub const LUA_REGISTRYINDEX: c_int = -(c_int::MAX / 2 + 1000);

/* thread status, as returned by lua_pcall and diluvium_repl_load's caller */
pub const LUA_OK: c_int = 0;
pub const LUA_YIELD: c_int = 1;
pub const LUA_ERRRUN: c_int = 2;
pub const LUA_ERRSYNTAX: c_int = 3;
pub const LUA_ERRMEM: c_int = 4;
pub const LUA_ERRERR: c_int = 5;

/* basic types */
pub const LUA_TNONE: c_int = -1;
pub const LUA_TNIL: c_int = 0;
pub const LUA_TBOOLEAN: c_int = 1;
pub const LUA_TLIGHTUSERDATA: c_int = 2;
pub const LUA_TNUMBER: c_int = 3;
pub const LUA_TSTRING: c_int = 4;
pub const LUA_TTABLE: c_int = 5;
pub const LUA_TFUNCTION: c_int = 6;
pub const LUA_TUSERDATA: c_int = 7;
pub const LUA_TTHREAD: c_int = 8;

/* garbage collection options used by a standalone interpreter */
pub const LUA_GCSTOP: c_int = 0;
pub const LUA_GCRESTART: c_int = 1;
pub const LUA_GCGEN: c_int = 7;

/* hook masks; LUA_MASKCOUNT with a count is how an interpreter interrupts */
pub const LUA_MASKCALL: c_int = 1;
pub const LUA_MASKRET: c_int = 2;
pub const LUA_MASKLINE: c_int = 4;
pub const LUA_MASKCOUNT: c_int = 8;

/* library keys for luaL_openselectedlibs, from lualib.h */
pub const LUA_GLIBK: c_int = 1;
pub const LUA_LOADLIBK: c_int = LUA_GLIBK << 1;
pub const LUA_COLIBK: c_int = LUA_LOADLIBK << 1;
pub const LUA_DBLIBK: c_int = LUA_COLIBK << 1;
pub const LUA_IOLIBK: c_int = LUA_DBLIBK << 1;
pub const LUA_MATHLIBK: c_int = LUA_IOLIBK << 1;
pub const LUA_OSLIBK: c_int = LUA_MATHLIBK << 1;
pub const LUA_STRLIBK: c_int = LUA_OSLIBK << 1;
pub const LUA_TABLIBK: c_int = LUA_STRLIBK << 1;
pub const LUA_UTF8LIBK: c_int = LUA_TABLIBK << 1;

/* results of diluvium_repl_load, from drepl.h */
pub const DILUVIUM_REPL_OK: c_int = 0;
/// Needs more input; the stack is unchanged. This is the value that lets a
/// front end open a continuation prompt instead of reporting an error, and
/// the reason `drepl.c` exists.
pub const DILUVIUM_REPL_INCOMPLETE: c_int = 1;
/// The message is on the stack.
pub const DILUVIUM_REPL_ERROR: c_int = 2;

/* ====================================================================== */
/* Functions                                                              */
/* ====================================================================== */

extern "C" {
    /* --- state --- */

    /// `lauxlib.h`. Null when the allocation fails.
    pub fn luaL_newstate() -> *mut lua_State;
    pub fn lua_close(L: *mut lua_State);

    /// Behind the `luaL_checkversion` macro. Prefer [`luaL_checkversion`].
    pub fn luaL_checkversion_(L: *mut lua_State, ver: lua_Number, sz: usize);

    /// `lualib.h`. `load` and `preload` are masks of the `LUA_*LIBK` keys;
    /// `luaL_openselectedlibs(L, !0, 0)` is `luaL_openlibs`.
    pub fn luaL_openselectedlibs(L: *mut lua_State, load: c_int, preload: c_int);

    /// `dlibs.h`. Diluvium's own libraries, into globals. Call once, after
    /// the standard libraries.
    pub fn diluvium_openlibs(L: *mut lua_State);

    /// Variadic in `lua.h` (`int lua_gc(lua_State *, int what, ...)`). The
    /// options a standalone interpreter uses -- `LUA_GCRESTART`, `LUA_GCGEN`
    /// -- take no further arguments.
    pub fn lua_gc(L: *mut lua_State, what: c_int, ...) -> c_int;

    /* --- the REPL intelligence, from drepl.h --- */

    /// Compile one REPL entry, a bare expression as though it read
    /// `return <it>;`. Returns `DILUVIUM_REPL_OK` with the chunk on the
    /// stack, `DILUVIUM_REPL_INCOMPLETE` with the stack unchanged, or
    /// `DILUVIUM_REPL_ERROR` with the message on the stack.
    pub fn diluvium_repl_load(
        L: *mut lua_State,
        code: *const c_char,
        len: usize,
        chunkname: *const c_char,
    ) -> c_int;

    /// Push a table of candidates for the identifier ending at `cursor`, and
    /// return the offset the replacement starts at: a front end replaces
    /// `buf[offset..cursor)` with a candidate, which is exactly
    /// `ego_cli::extend::Completion { start, end, .. }`.
    ///
    /// Resolution is raw -- no metamethod runs -- so this cannot fail and
    /// has no side effects, which is why a synchronous `&self` completer can
    /// answer from it before the keystroke.
    pub fn diluvium_repl_complete(L: *mut lua_State, buf: *const c_char, cursor: usize) -> usize;

    /* --- stack --- */

    pub fn lua_gettop(L: *mut lua_State) -> c_int;
    pub fn lua_settop(L: *mut lua_State, idx: c_int);
    pub fn lua_type(L: *mut lua_State, idx: c_int) -> c_int;
    pub fn lua_tolstring(L: *mut lua_State, idx: c_int, len: *mut usize) -> *const c_char;
    pub fn lua_rawlen(L: *mut lua_State, idx: c_int) -> lua_Unsigned;
    pub fn lua_rawgeti(L: *mut lua_State, idx: c_int, n: lua_Integer) -> c_int;
    pub fn lua_pushlstring(L: *mut lua_State, s: *const c_char, len: usize) -> *const c_char;
    pub fn lua_pushstring(L: *mut lua_State, s: *const c_char) -> *const c_char;
    pub fn lua_pushcclosure(L: *mut lua_State, f: lua_CFunction, n: c_int);
    pub fn lua_pushlightuserdata(L: *mut lua_State, p: *mut c_void);
    pub fn lua_touserdata(L: *mut lua_State, idx: c_int) -> *mut c_void;

    /// `luaL_tolstring` runs `__tostring`, so it is what prints a REPL
    /// result the way the interpreter does. It can raise; keep it protected.
    pub fn luaL_tolstring(L: *mut lua_State, idx: c_int, len: *mut usize) -> *const c_char;

    /* --- calling and errors --- */

    /// Behind the `lua_pcall` macro. Prefer [`lua_pcall`].
    pub fn lua_pcallk(
        L: *mut lua_State,
        nargs: c_int,
        nresults: c_int,
        errfunc: c_int,
        ctx: lua_KContext,
        k: Option<lua_KFunction>,
    ) -> c_int;

    /// Raises the value on top of the stack. Never returns: it longjmps, so
    /// no Rust frame between here and the enclosing [`lua_pcall`] may own
    /// anything that needs dropping.
    pub fn lua_error(L: *mut lua_State) -> c_int;

    /// Push a traceback for `L1`. The message handler an interpreter
    /// installs as `lua_pcall`'s `errfunc`.
    pub fn luaL_traceback(L: *mut lua_State, L1: *mut lua_State, msg: *const c_char, level: c_int);

    /* --- interrupting a running chunk --- */

    /// With `LUA_MASKCOUNT` and a count, this is how a standalone
    /// interpreter turns Ctrl+C into an error inside a running chunk --
    /// `src/lua.c`'s `laction`/`lstop`. Pass `None` to clear.
    pub fn lua_sethook(L: *mut lua_State, f: Option<lua_Hook>, mask: c_int, count: c_int);
}

/* ====================================================================== */
/* The macros lua.h defines, as functions                                 */
/* ====================================================================== */
/* Each is a macro in the C headers, so there is no symbol to link; these
are the same expansions, kept here rather than left to every caller. */

/// `lua_pcall(L, n, r, f)` -- `lua_pcallk` with no continuation.
///
/// # Safety
/// `nargs + 1` values must be on the stack, and `errfunc` must be 0 or a
/// valid index holding a function.
#[inline]
pub unsafe fn lua_pcall(L: *mut lua_State, nargs: c_int, nresults: c_int, errfunc: c_int) -> c_int {
    lua_pcallk(L, nargs, nresults, errfunc, 0, None)
}

/// `lua_pop(L, n)` -- `lua_settop(L, -(n)-1)`.
///
/// # Safety
/// At least `n` values must be on the stack.
#[inline]
pub unsafe fn lua_pop(L: *mut lua_State, n: c_int) {
    lua_settop(L, -n - 1);
}

/// `lua_tostring(L, i)` -- `lua_tolstring` with no length out-parameter.
///
/// # Safety
/// `idx` must be a valid stack index. The pointer is owned by Lua and is
/// invalid once the value leaves the stack.
#[inline]
pub unsafe fn lua_tostring(L: *mut lua_State, idx: c_int) -> *const c_char {
    lua_tolstring(L, idx, std::ptr::null_mut())
}

/// `lua_pushcfunction(L, f)` -- a closure with no upvalues.
///
/// # Safety
/// `f` must not unwind into C; a Rust panic across the FFI is undefined.
#[inline]
pub unsafe fn lua_pushcfunction(L: *mut lua_State, f: lua_CFunction) {
    lua_pushcclosure(L, f, 0);
}

/// `luaL_openlibs(L)` -- every standard library, none preloaded.
///
/// # Safety
/// `L` must be a fresh state from [`luaL_newstate`].
#[inline]
pub unsafe fn luaL_openlibs(L: *mut lua_State) {
    luaL_openselectedlibs(L, !0, 0);
}

/// `luaL_checkversion(L)` -- assert that the linked library agrees with this
/// crate about `LUA_VERSION_NUM`, `sizeof(lua_Integer)` and
/// `sizeof(lua_Number)`.
///
/// This is the check that catches a wrong [`lua_Integer`] alias, which is a
/// live hazard on the browser target; see the module docs. It raises rather
/// than returning a status, so call it where a raise is survivable -- the
/// interpreter calls it inside its own `lua_pcall`.
///
/// # Safety
/// `L` must be a valid state, and a raise here longjmps.
#[inline]
pub unsafe fn luaL_checkversion(L: *mut lua_State) {
    // LUAL_NUMSIZES == sizeof(lua_Integer) * 16 + sizeof(lua_Number)
    const NUMSIZES: usize =
        std::mem::size_of::<lua_Integer>() * 16 + std::mem::size_of::<lua_Number>();
    luaL_checkversion_(L, lua_Number::from(LUA_VERSION_NUM), NUMSIZES);
}
