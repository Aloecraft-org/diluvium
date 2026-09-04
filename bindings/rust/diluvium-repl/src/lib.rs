//! An interactive Diluvium prompt, on whatever terminal the platform has.
//!
//! # Surface
//!
//! Entry points: [`run`], the loop; [`state::State`], the runtime behind
//! it; [`extend::DiluviumCompleter`] and [`extend::DiluviumHighlighter`],
//! the two hooks it installs.
//!
//! Configurable values: [`PROMPT`] and [`CONTINUATION`], the two prompts;
//! [`GREETING`].
//!
//! Fan-out points: [`run`]'s match on [`ego_cli::ReadOutcome`] decides what
//! a read means, and its match on [`state::Outcome`] decides what an
//! evaluation means. Those two are the whole control flow.
//!
//! # What this is
//!
//! A spike. It answers whether `ego_cli`'s hooks fit what `drepl.c` offers
//! — they do, and `Completion`'s `start..end` is `diluvium_repl_complete`'s
//! offset and cursor with nothing in between — and what a Rust prompt
//! costs. It is not `src/lua.c`: no argument handling, no script running,
//! no `--task`, no `LUA_INIT`.
//!
//! # Raw mode, and why Ctrl+C works
//!
//! `Session::read_line` sets raw mode on entry and restores it before
//! returning, on the error path included. So evaluation always happens in
//! cooked mode: a real SIGINT is delivered during a chunk (where `lua.c`'s
//! hook interrupts it), and Lua's own `print` reaches a terminal with its
//! line discipline on. At the prompt, where raw mode is on, Ctrl+C is a key
//! press and arrives as [`ego_cli::ReadOutcome::Interrupted`]. The two
//! meanings of Ctrl+C land in the two places that want them without either
//! side arranging it.

#[cfg(all(target_arch = "wasm32", target_os = "unknown"))]
pub mod browser;
pub mod extend;
pub mod state;

use std::rc::Rc;

use ego_cli::{ReadOutcome, Session, Terminal};

use crate::extend::{DiluviumCompleter, DiluviumHighlighter};
use crate::state::{Outcome, State};

pub const PROMPT: &str = "dv> ";
pub const CONTINUATION: &str = ">>> ";
pub const GREETING: &str = "Diluvium. Tab completes, Ctrl+D exits.\n";

/// Read, evaluate, print, until end of input.
///
/// Generic over the terminal so the same loop serves a tty, a pipe, WASI's
/// line-at-a-time stdin and a browser's xterm.js — which is the whole
/// argument for this crate existing.
pub async fn run<T: Terminal>(term: T) -> ego_cli::Result<()> {
    let state = Rc::new(State::new().expect("cannot create a Diluvium state"));
    let mut session = Session::new(term);
    session.set_prompt(PROMPT);
    session.set_completer(DiluviumCompleter::new(Rc::clone(&state)));
    session.set_highlighter(DiluviumHighlighter);
    session.print(GREETING).await?;

    // An unfinished statement is held here and recompiled whole when the
    // rest arrives, rather than compiled in pieces: 'diluvium_repl_load'
    // reports INCOMPLETE precisely so a front end can do that.
    let mut pending = String::new();

    loop {
        match session.read_line().await? {
            ReadOutcome::Line(line) => {
                let source = if pending.is_empty() {
                    line
                } else {
                    format!("{pending}\n{line}")
                };
                match state.eval(&source) {
                    Outcome::Incomplete => {
                        pending = source;
                        session.set_prompt(CONTINUATION);
                        continue;
                    }
                    Outcome::Value(text) => {
                        if !text.is_empty() {
                            session.print(&format!("{text}\n")).await?;
                        }
                    }
                    Outcome::Error(message) => {
                        session.print(&format!("{message}\n")).await?;
                    }
                }
                pending.clear();
                session.set_prompt(PROMPT);
            }
            // Ctrl+C abandons whatever was being typed, the continuation
            // included -- otherwise there is no way out of a statement you
            // have changed your mind about.
            ReadOutcome::Interrupted => {
                pending.clear();
                session.set_prompt(PROMPT);
            }
            ReadOutcome::Eof => break,
        }
    }
    Ok(())
}
