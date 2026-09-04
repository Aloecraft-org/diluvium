//! The Lua state, and the two things a prompt asks of it.
//!
//! # Surface
//!
//! Entry points: [`State::new`], [`State::eval`], [`State::complete`];
//! [`Outcome`], what an evaluation produced.
//!
//! Configurable values: [`CHUNKNAME`], what an entry is called in an error
//! message; [`TRACEBACK_LEVEL`], where a traceback starts.
//!
//! Fan-out points: [`Outcome`] is the whole result vocabulary, and `eval`'s
//! match on `diluvium_repl_load`'s three statuses is the only place a load
//! status becomes one.
//!
//! # Why this is not `Send`
//!
//! A `lua_State` is single-threaded — `dv.h` states that rule for instances
//! and it is the same rule here. The raw pointer makes this type neither
//! `Send` nor `Sync` without anything having to say so.
//!
//! # Why the methods take `&self`
//!
//! `ego_cli::extend::Completer::complete` takes `&self`, so the completer
//! holding this only ever has a shared reference — and it has to push a
//! table onto the stack. That is sound because of a property of the loop
//! rather than of the type: `Session::read_line` and `eval` are strictly
//! sequential on one thread, so a completion never runs while an evaluation
//! is on the stack. Every method here restores the stack to where it found
//! it, which is what makes "sequential" enough.
//!
//! If a second caller ever appears — an editor asking for completions while
//! a program runs — that argument stops holding, and the answer is a second
//! `lua_State` rather than a lock.
//!
//! # depth: unsafe FFI from here down

use std::ffi::CStr;
use std::os::raw::{c_char, c_int};

use diluvium_sys::lua::*;

unsafe extern "C" {
    /// Flush C's stdio. A null stream means every open stream, which is
    /// what this needs: see `flush_c_output`.
    fn fflush(stream: *mut std::ffi::c_void) -> c_int;
}

/// What an entry is called in an error message. Lua's `=` prefix means
/// "use this verbatim rather than dressing it up as a file name".
const CHUNKNAME: &CStr = c"=stdin";

/// Where `luaL_traceback` starts. 1 skips the message handler itself.
const TRACEBACK_LEVEL: c_int = 1;

/// What evaluating one entry produced.
#[derive(Clone, PartialEq, Eq, Debug)]
pub enum Outcome {
    /// It ran. The string is what the interpreter would print — empty when
    /// the entry produced no values.
    Value(String),
    /// Unfinished, not wrong: the prompt should ask for more input. This is
    /// the distinction `drepl.c` exists to make, and the one a front end
    /// cannot make for itself except by matching the text of Lua's error
    /// messages.
    Incomplete,
    /// It did not compile, or it raised.
    Error(String),
}

pub struct State {
    l: *mut lua_State,
}

impl Drop for State {
    fn drop(&mut self) {
        unsafe { lua_close(self.l) }
    }
}

impl State {
    /// A state with the standard libraries and Diluvium's own, set up the
    /// way `src/lua.c` sets one up.
    pub fn new() -> Option<Self> {
        unsafe {
            let l = luaL_newstate();
            if l.is_null() {
                return None;
            }
            // First, and protected because it raises: Lua's own check that
            // the library agrees with these bindings about sizeof
            // lua_Integer and lua_Number.
            lua_pushcfunction(l, check_version);
            if lua_pcall(l, 0, 0, 0) != LUA_OK {
                lua_close(l);
                return None;
            }
            luaL_openlibs(l);
            diluvium_openlibs(l);
            lua_gc(l, LUA_GCRESTART);
            Some(State { l })
        }
    }

    /// Compile and run one entry.
    ///
    /// `source` is the whole entry, continuation lines included: an
    /// unfinished statement is not partially compiled and resumed, it is
    /// recompiled once the rest of it has arrived.
    pub fn eval(&self, source: &str) -> Outcome {
        unsafe {
            let base = lua_gettop(self.l);

            // The message handler goes on *before* the chunk, so a
            // traceback is taken where the error happened rather than after
            // the stack has unwound. lua.c pushes it afterwards and calls
            // lua_insert; pushing first needs no insert at all.
            lua_pushcfunction(self.l, msghandler);
            let handler = base + 1;

            let status = diluvium_repl_load(
                self.l,
                source.as_ptr().cast(),
                source.len(),
                CHUNKNAME.as_ptr(),
            );
            let outcome = match status {
                // Stack unchanged by contract, so there is nothing here but
                // the handler to drop.
                DILUVIUM_REPL_INCOMPLETE => Outcome::Incomplete,
                DILUVIUM_REPL_ERROR => Outcome::Error(self.string_at(-1)),
                _ => {
                    let ok = lua_pcall(self.l, 0, LUA_MULTRET, handler) == LUA_OK;
                    // Results, or the handled error, sit above the handler.
                    let rendered = self.render_from(handler);
                    if ok {
                        Outcome::Value(rendered)
                    } else {
                        Outcome::Error(rendered)
                    }
                }
            };
            lua_settop(self.l, base);
            flush_c_output();
            outcome
        }
    }

    /// The candidates for the identifier ending at `cursor`, and the byte
    /// offset the replacement starts at — `line[offset..cursor]` is what a
    /// line editor replaces.
    ///
    /// Resolution is raw: no metamethod runs, so this cannot fail and has
    /// no side effects, which is what lets it answer a keystroke rather
    /// than defer one.
    pub fn complete(&self, line: &str, cursor: usize) -> (usize, Vec<String>) {
        unsafe {
            let base = lua_gettop(self.l);
            let offset = diluvium_repl_complete(self.l, line.as_ptr().cast(), cursor);
            let mut candidates = Vec::new();
            if lua_type(self.l, -1) == LUA_TTABLE {
                let n = lua_rawlen(self.l, -1) as usize;
                candidates.reserve(n);
                for i in 1..=n {
                    lua_rawgeti(self.l, -1, i as lua_Integer);
                    if let Some(s) = owned(lua_tostring(self.l, -1)) {
                        candidates.push(s);
                    }
                    lua_pop(self.l, 1);
                }
            }
            lua_settop(self.l, base);
            (offset, candidates)
        }
    }

    /// Everything above `from`, rendered the way the interpreter prints
    /// results: tab separated, `__tostring` honoured.
    ///
    /// # Safety
    /// `from` must be a valid absolute stack index.
    unsafe fn render_from(&self, from: c_int) -> String {
        let top = lua_gettop(self.l);
        let mut parts = Vec::new();
        for i in (from + 1)..=top {
            // luaL_tolstring pushes above `top`, so the indices being
            // walked stay valid as this goes.
            if let Some(s) = owned(luaL_tolstring(self.l, i, std::ptr::null_mut())) {
                parts.push(s);
            }
            lua_pop(self.l, 1);
        }
        parts.join("\t")
    }

    /// # Safety
    /// `idx` must hold a string or a number.
    unsafe fn string_at(&self, idx: c_int) -> String {
        owned(lua_tostring(self.l, idx)).unwrap_or_default()
    }
}

/// `luaL_checkversion` raises rather than returning a status, so it runs as
/// a Lua function inside a `lua_pcall` — the same shape `lua.c` uses in
/// `pmain`.
unsafe extern "C" fn check_version(l: *mut lua_State) -> c_int {
    luaL_checkversion(l);
    0
}

/// Turn whatever was raised into a string and append a traceback taken at
/// the point of the error.
///
/// `lua.c` reaches for `__tostring` by hand through `luaL_callmeta`;
/// `luaL_tolstring` does the same thing and always produces a string, so
/// this is that function with the special cases already handled. It can
/// itself raise, on a `__tostring` that does — which `lua_pcall` reports as
/// LUA_ERRERR, and which is the correct outcome for a program whose error
/// values are hostile.
unsafe extern "C" fn msghandler(l: *mut lua_State) -> c_int {
    let msg = luaL_tolstring(l, 1, std::ptr::null_mut());
    luaL_traceback(l, l, msg, TRACEBACK_LEVEL);
    1
}

/// Push whatever the chunk printed out of C's buffer, so it lands before
/// whatever the prompt writes next.
///
/// Two writers share fd 1: Lua's `print` and `io.write` go through C stdio,
/// and the session writes through Rust. On a tty C stdio is line buffered
/// and the two happen to interleave correctly; on a pipe it is fully
/// buffered, so `io.write("x")` with no newline sat in C's buffer until the
/// process exited and came out *after* everything Rust had written since.
/// Found by piping the prompt's output and reading it back.
///
/// Flushing after each evaluation is the whole fix: the prompt writes at
/// exactly one point, right after this.
fn flush_c_output() {
    // Null flushes every stream. It cannot fail in a way that matters here
    // -- a closed stdout is the caller's problem, not this function's.
    unsafe { fflush(std::ptr::null_mut()) };
}

/// A string Lua owns, copied out.
///
/// # Safety
/// `p` is null or points at a NUL-terminated string Lua has not yet
/// collected.
unsafe fn owned(p: *const c_char) -> Option<String> {
    if p.is_null() {
        None
    } else {
        Some(CStr::from_ptr(p).to_string_lossy().into_owned())
    }
}
