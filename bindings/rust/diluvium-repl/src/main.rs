//! `dv-repl`: the prompt, on whatever terminal this target has.
//!
//! # Surface
//!
//! Entry points: `main`, one per platform shape. The body they share is
//! `diluvium_repl::run`.
//!
//! Configurable values: none; everything is in the library.
//!
//! Fan-out points: the `cfg` split below, which is the only place this
//! crate knows what it was compiled for.
//!
//! The browser has no `main` -- a page hands over its xterm.js terminal --
//! and is not wired up here: `wasm32-unknown-unknown` takes its libc from
//! the embedder (bindings/rust/WASM-SPIKE.md, "the browser contract"), so
//! a browser build of this needs 84 symbols supplied from Rust before it
//! needs a `main`. `diluvium_repl::run` is generic over the terminal and
//! ready for it; the embedder is the missing half.

// No async runtime. `term::platform()` is ego-cli's `BlockingNative` with
// its `runtime` feature off, and nothing it returns ever pends, so a plain
// poll loop drives the whole session. An interpreter has one thread, one
// lua_State that cannot leave it, and wants a line at exactly one point --
// there was never a reactor's worth of work here.
#[cfg(not(target_arch = "wasm32"))]
fn main() {
    let terminal = ego_cli::term::platform().expect("open terminal");
    if let Err(error) = futures_executor::block_on(diluvium_repl::run(terminal)) {
        eprintln!("dv-repl: {error}");
        std::process::exit(1);
    }
}

// WASI Preview 2 is single-threaded and cannot reach the host's termios, so
// this is the same loop reading finished lines: history is recorded, and
// there is no Up key to walk it with.
#[cfg(all(target_arch = "wasm32", target_env = "p2"))]
#[tokio::main(flavor = "current_thread")]
async fn main() {
    if let Err(error) = diluvium_repl::run(ego_cli::term::platform().expect("open terminal")).await
    {
        eprintln!("dv-repl: {error}");
    }
}

#[cfg(all(target_arch = "wasm32", target_os = "unknown"))]
fn main() {}
