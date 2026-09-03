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

/// Take over `terminal` and run the prompt. From a page:
///
/// ```js
/// import init, { start } from './dv_repl.js';
/// await init();
/// const term = new Terminal();
/// term.open(document.getElementById('terminal'));
/// await start(term);
/// ```
///
/// Returns nothing, deliberately. A `#[wasm_bindgen]` export that returns
/// `Result<_, JsValue>` makes wasm-bindgen generate a *catch wrapper*, and
/// it refuses to generate one for this module: the C core is compiled with
/// the wasm exception-handling proposal, so Lua's setjmp/longjmp -- and
/// therefore `pcall` -- work rather than trapping, which leaves a `tag`
/// section in the module. wasm-bindgen sees that, takes its exception-aware
/// path, and asks for an `__instance_terminated` global that only a
/// Rust-side EH build emits. "failed to generate catch wrappers" is what
/// that looks like.
///
/// Nothing is lost. A prompt that cannot start has one useful place to say
/// so, and it is the terminal the caller just handed over.
#[cfg(all(target_arch = "wasm32", target_os = "unknown"))]
#[wasm_bindgen::prelude::wasm_bindgen]
pub fn start(terminal: wasm_bindgen::JsValue) {
    wasm_bindgen_futures::spawn_local(run_prompt(terminal));
}

#[cfg(all(target_arch = "wasm32", target_os = "unknown"))]
async fn run_prompt(terminal: wasm_bindgen::JsValue) {
    use ego_cli::term::browser::XtermTerminal;

    let terminal = XtermTerminal::attach(terminal);
    // Lua's own output goes through C stdio, which lands in the WASI floor
    // in `diluvium_repl::browser`; point that at the same terminal before
    // anything can print.
    diluvium_repl::browser::set_stdout(terminal.handle().clone());
    if let Err(error) = diluvium_repl::run(terminal).await {
        // `run` owns the terminal, so there is nothing left to print on.
        // Throwing is not a catch wrapper -- that is a property of the
        // return type -- so this stays out of the trouble described above.
        wasm_bindgen::throw_str(&format!("dv-repl: {error}"));
    }
}
