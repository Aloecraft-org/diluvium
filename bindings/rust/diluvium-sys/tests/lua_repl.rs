//! The `lua` feature's seam, exercised end to end.
//!
//! `tests/link.rs` proves the archive links. This proves the part a front end
//! actually depends on: that `drepl.c`'s two functions are reachable from
//! Rust, that "unfinished" is distinguishable from "wrong" without matching
//! error text, and that the completion offset means what a line editor needs
//! it to mean.
//!
//! Written as an integration test for the same reason as `link.rs`: it is a
//! linked artifact, so a symbol that is declared but not present fails here
//! rather than in some downstream binary.
//!
//! Run it: `cargo test -p diluvium-sys --features lua`.
#![cfg(feature = "lua")]

use std::ffi::{CStr, CString};
use std::os::raw::c_int;

use diluvium_sys::lua::*;

/// A state set up the way `src/lua.c` sets one up, minus the parts that need
/// argv: standard libraries, Diluvium's own, GC restarted.
///
/// # Safety
/// The caller closes it.
unsafe fn setup() -> *mut lua_State {
    let l = luaL_newstate();
    assert!(!l.is_null(), "luaL_newstate returned null");
    // The interpreter checks this first, inside its own protected call, and
    // so does `version_and_widths_agree` below. Here the state is fresh and a
    // mismatch would already have failed that test.
    luaL_openlibs(l);
    diluvium_openlibs(l);
    lua_gc(l, LUA_GCRESTART);
    l
}

/// Compile and run one REPL entry, returning what the interpreter would
/// print, or the load status when it did not compile.
///
/// # Safety
/// `l` must be a state from `setup`.
unsafe fn eval(l: *mut lua_State, source: &str) -> Result<String, c_int> {
    let name = CString::new("=stdin").unwrap();
    let status = diluvium_repl_load(l, source.as_ptr().cast(), source.len(), name.as_ptr());
    if status != DILUVIUM_REPL_OK {
        lua_settop(l, 0); // an error left its message; drop it
        return Err(status);
    }
    assert_eq!(
        lua_pcall(l, 0, LUA_MULTRET, 0),
        LUA_OK,
        "chunk raised: {source}"
    );
    let printed = if lua_gettop(l) == 0 {
        String::new()
    } else {
        let p = luaL_tolstring(l, -1, std::ptr::null_mut());
        CStr::from_ptr(p).to_string_lossy().into_owned()
    };
    lua_settop(l, 0);
    Ok(printed)
}

/// The candidates `diluvium_repl_complete` pushes, with the offset it
/// returned -- which is `ego_cli::extend::Completion`'s `start`, with
/// `cursor` as its `end`.
///
/// # Safety
/// `l` must be a state from `setup`.
unsafe fn complete(l: *mut lua_State, line: &str, cursor: usize) -> (usize, Vec<String>) {
    let offset = diluvium_repl_complete(l, line.as_ptr().cast(), cursor);
    assert_eq!(
        lua_type(l, -1),
        LUA_TTABLE,
        "completion did not push a table"
    );
    let n = lua_rawlen(l, -1) as usize;
    let mut out = Vec::with_capacity(n);
    for i in 1..=n {
        lua_rawgeti(l, -1, i as lua_Integer);
        let p = lua_tostring(l, -1);
        out.push(CStr::from_ptr(p).to_string_lossy().into_owned());
        lua_pop(l, 1);
    }
    lua_settop(l, 0);
    (offset, out)
}

/// `luaL_checkversion` raises, so it runs where a raise is survivable --
/// exactly as `src/lua.c` runs it, inside `pmain`.
unsafe extern "C" fn checkversion_body(l: *mut lua_State) -> c_int {
    luaL_checkversion(l);
    0
}

/// The transcription check that matters most.
///
/// Lua's own version check compares `sizeof(lua_Integer)` and
/// `sizeof(lua_Number)` against what the library was built with, so a wrong
/// alias fails here rather than silently misreading a number later. It has
/// already earned its place once: the browser target's aliases stayed at
/// 32 bits after `luaconf.h` pinned the C at 64, and this is what said so.
#[test]
fn version_and_widths_agree() {
    unsafe {
        let l = luaL_newstate();
        assert!(!l.is_null());
        lua_pushcfunction(l, checkversion_body);
        let status = lua_pcall(l, 0, 0, 0);
        if status != LUA_OK {
            let message = CStr::from_ptr(lua_tostring(l, -1))
                .to_string_lossy()
                .into_owned();
            lua_close(l);
            panic!("luaL_checkversion failed: {message}");
        }
        lua_close(l);
    }
}

#[test]
fn a_bare_expression_yields_its_value() {
    unsafe {
        let l = setup();
        // The whole point of `diluvium_repl_load`: `1 + 1` is compiled as
        // `return 1 + 1;`, so a REPL shows 2 rather than a syntax error.
        assert_eq!(eval(l, "1 + 1").unwrap(), "2");
        assert_eq!(eval(l, "('a'):upper()").unwrap(), "A");
        lua_close(l);
    }
}

#[test]
fn diluvium_syntax_compiles() {
    unsafe {
        let l = setup();
        assert_eq!(
            eval(l, r#"local n = "world"; return $"hello {n}""#).unwrap(),
            "hello world"
        );
        assert_eq!(eval(l, "nil ?? 8080").unwrap(), "8080");
        lua_close(l);
    }
}

/// The distinction a front end cannot make for itself, and the reason
/// `drepl.c` exists: unfinished input opens a continuation prompt, broken
/// input reports an error.
#[test]
fn unfinished_is_not_broken() {
    unsafe {
        let l = setup();
        assert_eq!(
            eval(l, "function f()").unwrap_err(),
            DILUVIUM_REPL_INCOMPLETE,
            "an unclosed function should ask for more input"
        );
        assert_eq!(
            eval(l, "local 1").unwrap_err(),
            DILUVIUM_REPL_ERROR,
            "a real syntax error should not ask for more input"
        );
        lua_close(l);
    }
}

#[test]
fn completion_offers_globals_and_keywords() {
    unsafe {
        let l = setup();
        let (offset, items) = complete(l, "prin", 4);
        assert_eq!(
            offset, 0,
            "the replacement starts at the token, not the cursor"
        );
        assert!(
            items.iter().any(|c| c == "print"),
            "no 'print' in {items:?}"
        );

        // A Diluvium contextual keyword: not a value, so no table walk finds
        // it, and `drepl.c` carries the list for exactly that reason.
        let (_, items) = complete(l, "swit", 4);
        assert!(
            items.iter().any(|c| c == "switch"),
            "no 'switch' in {items:?}"
        );
        lua_close(l);
    }
}

/// The offset is what makes this a `Completion` rather than a word list: a
/// line editor replaces `line[start..cursor]`, so for `string.f` the answer
/// has to start after the dot.
#[test]
fn completion_offset_points_past_the_dot() {
    unsafe {
        let l = setup();
        let (offset, items) = complete(l, "string.f", 8);
        assert_eq!(offset, 7, "'string.f' should replace from index 7, not 0");
        assert!(
            items.iter().any(|c| c == "format"),
            "no 'format' in {items:?}"
        );
        lua_close(l);
    }
}

/// `drepl.h`: "Resolution is entirely raw: no metamethod runs, so completing
/// inside a table with an active `__index` has no side effects and cannot
/// fail." That is the property that lets `ego_cli::extend::Completer` stay
/// synchronous and take `&self`, so this test states it.
#[test]
fn completion_runs_no_metamethod() {
    unsafe {
        let l = setup();
        eval(
            l,
            "tricky = setmetatable({}, {__index = function() error('ran') end})",
        )
        .unwrap();
        let (offset, items) = complete(l, "tricky.any", 10);
        assert_eq!(offset, 7);
        assert!(
            items.is_empty(),
            "a raw walk should find nothing: {items:?}"
        );
        // The state is still usable, which is the half that matters.
        assert_eq!(eval(l, "1 + 1").unwrap(), "2");
        lua_close(l);
    }
}
