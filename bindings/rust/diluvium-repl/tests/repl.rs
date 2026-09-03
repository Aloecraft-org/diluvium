//! The whole prompt, driven through `MemTerminal` — no tty anywhere.
//!
//! This is the spike's actual claim: that `ego_cli`'s two hooks fit what
//! `drepl.c` offers, closely enough that the seam needs no adapter. So the
//! tests are about the seam rather than about either side of it — what Tab
//! inserts, where a replacement starts, what an unfinished statement does,
//! and that highlighting adds only colour.
//!
//! `MemTerminal` ships in `ego_cli` rather than in its test suite for
//! exactly this: a host writing a completer needs to assert what Tab does
//! without owning a terminal.

use std::rc::Rc;

use diluvium_repl::extend::{DiluviumCompleter, DiluviumHighlighter};
use diluvium_repl::state::{Outcome, State};
use ego_cli::extend::{Completer, Highlighter};
use ego_cli::style;
use ego_cli::term::mem::MemTerminal;
use ego_cli::{ReadOutcome, Session, Size};
use futures_executor::block_on;

fn state() -> Rc<State> {
    Rc::new(State::new().expect("a Diluvium state"))
}

/// A session with the real completer and highlighter, fed `input` the way a
/// terminal would send it.
fn session(state: &Rc<State>, input: &str) -> Session<MemTerminal> {
    let mut term = MemTerminal::raw(Size::new(80, 24));
    term.push_input(input);
    let mut session = Session::new(term);
    session.set_completer(DiluviumCompleter::new(Rc::clone(state)));
    session.set_highlighter(DiluviumHighlighter);
    session
}

/// One line, read to completion. `block_on` rather than a runtime: with
/// ego-cli's `runtime` feature off there is nothing to run.
fn read(state: &Rc<State>, input: &str) -> ReadOutcome {
    block_on(session(state, input).read_line()).unwrap()
}

/* ====================================================================== */
/* The state                                                              */
/* ====================================================================== */

#[test]
fn a_bare_expression_yields_its_value() {
    let s = state();
    assert_eq!(s.eval("1 + 1"), Outcome::Value("2".into()));
    assert_eq!(s.eval("('a'):upper()"), Outcome::Value("A".into()));
}

#[test]
fn diluvium_syntax_evaluates() {
    let s = state();
    assert_eq!(
        s.eval(r#"local n = "world"; return $"hello {n}""#),
        Outcome::Value("hello world".into())
    );
    assert_eq!(s.eval("nil ?? 8080"), Outcome::Value("8080".into()));
}

#[test]
fn unfinished_is_not_broken() {
    let s = state();
    assert_eq!(s.eval("function f()"), Outcome::Incomplete);
    // ...and the same text, finished, compiles.
    assert_eq!(
        s.eval("function f()\nreturn 7\nend"),
        Outcome::Value(String::new())
    );
    assert_eq!(s.eval("f()"), Outcome::Value("7".into()));
    // A real syntax error is not a request for more input.
    assert!(matches!(s.eval("local 1"), Outcome::Error(_)));
}

#[test]
fn a_raise_carries_a_traceback() {
    let s = state();
    match s.eval("error('boom')") {
        Outcome::Error(message) => {
            assert!(message.contains("boom"), "{message}");
            assert!(message.contains("stack traceback"), "{message}");
        }
        other => panic!("expected an error, got {other:?}"),
    }
}

/* ====================================================================== */
/* The completion seam                                                    */
/* ====================================================================== */

#[test]
fn tab_completes_a_global() {
    // "prin", Tab, Enter. One candidate, so Tab inserts it outright.
    let s = state();
    assert_eq!(read(&s, "prin\t\r"), ReadOutcome::Line("print".into()));
}

/// The property that makes this a `Completion` and not a word list:
/// `diluvium_repl_complete` returns where the replacement starts, so
/// completing `string.f` rewrites after the dot and not from the line's
/// beginning.
#[test]
fn tab_replaces_from_the_token_not_the_line() {
    let s = state();
    let completer = DiluviumCompleter::new(Rc::clone(&s));
    let completion = completer.complete("string.f", 8);
    assert_eq!(completion.start, 7, "{completion:?}");
    assert_eq!(completion.end, 8, "{completion:?}");
    assert!(
        completion.candidates.iter().any(|c| c == "format"),
        "{completion:?}"
    );

    // And through a whole session: "string.fo", Tab, Enter.
    assert_eq!(
        read(&s, "string.fo\t\r"),
        ReadOutcome::Line("string.format".into())
    );
}

/// Keywords are not values, so no table walk finds them. `drepl.c` carries
/// its own list for that reason, and the completer adds nothing to it.
#[test]
fn tab_offers_diluvium_keywords() {
    let s = state();
    let completer = DiluviumCompleter::new(Rc::clone(&s));
    let completion = completer.complete("swit", 4);
    assert!(
        completion.candidates.iter().any(|c| c == "switch"),
        "{completion:?}"
    );
    let completion = completer.complete("defe", 4);
    assert!(
        completion.candidates.iter().any(|c| c == "defer"),
        "{completion:?}"
    );
}

/// `drepl.h`: resolution is raw, so completing inside a table with an
/// active `__index` has no side effects and cannot fail. That is what
/// licenses a synchronous `&self` completer, so it is asserted rather than
/// assumed.
#[test]
fn completion_runs_no_metamethod() {
    let s = state();
    s.eval("tricky = setmetatable({}, {__index = function() error('ran') end})");
    let completer = DiluviumCompleter::new(Rc::clone(&s));
    let completion = completer.complete("tricky.any", 10);
    assert_eq!(completion.start, 7, "{completion:?}");
    assert!(completion.candidates.is_empty(), "{completion:?}");
    // The state is still usable, which is the half that matters.
    assert_eq!(s.eval("1 + 1"), Outcome::Value("2".into()));
}

/* ====================================================================== */
/* The highlighting seam                                                  */
/* ====================================================================== */

/// The bargain `ego_cli` asks of a highlighter: same printable characters,
/// escapes added. Break it and the cursor lands in the wrong column,
/// because the cursor is measured against the line the editor holds.
#[test]
fn highlighting_adds_only_colour() {
    let lines = [
        "local x = 1",
        r#"local s = $"a {b} c""#,
        "-- a comment",
        "x ??= 8080",
        "~function secret() end",
        "t?.field",
        "1e-3 + 0xff",
        "local emoji = \"héllo ✨\"",
        "",
    ];
    for line in lines {
        let painted = DiluviumHighlighter.highlight(line);
        assert_eq!(style::strip(&painted), line, "painting changed {line:?}");
    }
}

#[test]
fn diluvium_syntax_gets_its_own_colour() {
    let yellow = style::fg(ego_cli::style::Color::Yellow);
    let magenta = style::fg(ego_cli::style::Color::Magenta);

    let painted = DiluviumHighlighter.highlight("local x = 1");
    assert!(painted.contains(magenta), "no keyword colour: {painted:?}");

    for source in [r#"$"hi""#, "a ?? b", "t?.f", "~function f() end"] {
        let painted = DiluviumHighlighter.highlight(source);
        assert!(
            painted.contains(yellow),
            "Diluvium syntax unpainted in {source:?}: {painted:?}"
        );
    }

    // '~' alone is the bitwise operator, not a secure function.
    let painted = DiluviumHighlighter.highlight("a = ~b");
    assert!(
        !painted.contains(yellow),
        "'~b' painted as Diluvium: {painted:?}"
    );
}

/* ====================================================================== */
/* The loop                                                               */
/* ====================================================================== */

#[test]
fn the_prompt_and_the_line_are_drawn() {
    let s = state();
    let mut session = session(&s, "1 + 1\r");
    session.set_prompt("dv> ");
    block_on(session.read_line()).unwrap();
    let output = session.terminal().output();
    assert!(output.contains("dv> "), "{output:?}");
}

/// Editing still works with a completer and a highlighter installed --
/// the two hooks are not allowed to cost the editor anything.
#[test]
fn the_editor_still_edits() {
    let s = state();
    // "ac", Left, "b" -> "abc"
    assert_eq!(read(&s, "ac\x1b[Db\r"), ReadOutcome::Line("abc".into()));
    // "one two", Ctrl+Left, "X" -> "one Xtwo"
    assert_eq!(
        read(&s, "one two\x1b[1;5DX\r"),
        ReadOutcome::Line("one Xtwo".into())
    );
}

/// Ctrl+C at the prompt is a key press, not a signal: the terminal is in
/// raw mode there. It abandons the line and the session stays good.
#[test]
fn ctrl_c_abandons_the_line() {
    let s = state();
    assert_eq!(read(&s, "half a line\x03"), ReadOutcome::Interrupted);
}

/// Running out of scripted input is end of input, which is how a piped
/// session ends too.
#[test]
fn exhausted_input_is_eof() {
    let s = state();
    assert_eq!(read(&s, ""), ReadOutcome::Eof);
}

/* ====================================================================== */
/* Two writers, one file descriptor                                       */
/* ====================================================================== */

/// Lua's `print` and `io.write` go through C stdio; the session writes
/// through Rust. On a tty C stdio is line buffered and the two interleave
/// correctly by luck. On a pipe it is fully buffered, so an `io.write` with
/// no newline stayed in C's buffer until the process exited and came out
/// after everything Rust had written since -- which is what this asserts is
/// no longer true.
///
/// Spawns the real binary, because that is the only way to have a real pipe
/// on the other end. Native and non-Windows: wasm cannot spawn at all, and
/// the Windows test binary runs under wine where the path to a Unix-built
/// `dv-repl` means nothing.
#[cfg(all(not(target_arch = "wasm32"), not(windows)))]
#[test]
fn lua_output_and_prompt_output_stay_in_order() {
    use std::io::Write;
    use std::process::{Command, Stdio};

    let mut child = Command::new(env!("CARGO_BIN_EXE_dv-repl"))
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()
        .expect("spawn dv-repl");
    child
        .stdin
        .take()
        .expect("stdin")
        .write_all(
            b"print('A-from-lua')\n\
              return 'B-from-rust'\n\
              io.write('C-no-newline')\n\
              return 'D-from-rust'\n",
        )
        .expect("write the script");
    let out = child.wait_with_output().expect("run to completion");
    let text = String::from_utf8_lossy(&out.stdout);

    let at = |needle: &str| {
        text.find(needle)
            .unwrap_or_else(|| panic!("{needle:?} missing from {text:?}"))
    };
    assert!(at("A-from-lua") < at("B-from-rust"), "{text:?}");
    // The one that used to fail: no newline, so nothing flushed it.
    assert!(at("C-no-newline") < at("D-from-rust"), "{text:?}");
}
