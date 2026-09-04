//! The two hooks: what Tab offers, and what the line looks like.
//!
//! # Surface
//!
//! Entry points: [`DiluviumCompleter`] and [`DiluviumHighlighter`].
//!
//! Configurable values: [`KEYWORDS`], the words painted as keywords;
//! [`COLOURS`], one per [`Class`].
//!
//! Fan-out points: [`Class`] is the closed set of things the highlighter
//! can paint, [`COLOURS`] is its colour table, and `classify`'s leading
//! `match` is where a byte chooses one. A new piece of Diluvium syntax
//! needs an arm there and nothing else.
//!
//! Both traits are synchronous and take `&self`, which is what makes this
//! shape work at all: the completer reaches through `diluvium-sys` into a
//! raw table walk that runs no metamethod and cannot fail, so it has its
//! answer before the keystroke rather than stalling the prompt mid-Tab.

use std::borrow::Cow;
use std::rc::Rc;

use ego_cli::extend::{Completer, Completion, Highlighter};
use ego_cli::style::{self, Color};

use crate::state::State;

/// What the highlighter paints as a keyword: `hlwords` in `src/dline.c`,
/// which is a slightly longer list than the one `drepl.c` completes from.
/// `case` and `default` are keywords only inside a switch body, so they are
/// worth colouring and not worth offering.
///
/// The two highlighters paint the same language, and this one exists to
/// replace that one — it is the one that can run on Windows and in a
/// browser. Completion has no list here at all; `drepl.c` owns that.
const KEYWORDS: &[&str] = &[
    "and", "break", "case", "default", "defer", "do", "else", "elseif", "end", "false", "for",
    "function", "global", "goto", "if", "in", "local", "nil", "not", "or", "repeat", "return",
    "switch", "then", "true", "until", "while",
];

/// What a byte of the line is. `dline.c`'s `HL_*` set, by another name.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum Class {
    None,
    Keyword,
    String,
    Number,
    Comment,
    /// What Diluvium adds to Lua: `$"`, `??`, `?.`, `?[`, `~function`.
    /// Its own colour, so its own syntax stands out from Lua's.
    Diluvium,
}

/// One per [`Class`], matching `hlcolour` in `src/dline.c`.
const COLOURS: [Color; 6] = [
    Color::Default,     // None
    Color::Magenta,     // Keyword
    Color::Green,       // String
    Color::Cyan,        // Number
    Color::BrightBlack, // Comment
    Color::Yellow,      // Diluvium
];

impl Class {
    fn colour(self) -> Color {
        COLOURS[self as usize]
    }
}

/// Completes an identifier through `diluvium_repl_complete`.
pub struct DiluviumCompleter {
    state: Rc<State>,
}

impl DiluviumCompleter {
    pub fn new(state: Rc<State>) -> Self {
        Self { state }
    }
}

impl Completer for DiluviumCompleter {
    fn complete(&self, line: &str, cursor: usize) -> Completion {
        let cursor = cursor.min(line.len());
        let (offset, candidates) = self.state.complete(line, cursor);
        // Nothing else to do, and that is the finding. `offset` is where the
        // replacement starts and `cursor` is where it ends, which is exactly
        // `Completion`'s range; `drepl.c` has already added the keywords,
        // already gated them to a bare word — so `string.fo` does not offer
        // `for` — and already sorted. An earlier draft did all three again on
        // top, and offering `for` after a dot is what the test caught.
        Completion::new(offset.min(cursor)..cursor, candidates)
    }
}

/// Paints Lua's syntax, and Diluvium's in its own colour.
///
/// A port of `classify` in `src/dline.c`, byte for byte in what it
/// recognises. Two implementations of one language is one too many; this
/// one exists to be the survivor, since it is the one that can run on
/// Windows and in a browser.
#[derive(Clone, Copy, Debug, Default)]
pub struct DiluviumHighlighter;

impl Highlighter for DiluviumHighlighter {
    fn highlight<'l>(&self, line: &'l str) -> Cow<'l, str> {
        let classes = classify(line.as_bytes());
        if classes.iter().all(|c| *c == Class::None) {
            return Cow::Borrowed(line);
        }
        let mut out = String::with_capacity(line.len() + 32);
        let mut current: Option<Class> = None;
        // By character, not by byte. Classification is byte-oriented, but
        // reassembly cannot be: `push(byte as char)` reads each byte as a
        // codepoint, which turns "héllo" into "hÃ©llo". A character takes the
        // class of its first byte, which is the only byte of it any arm of
        // the classifier can match.
        for (i, ch) in line.char_indices() {
            let class = classes[i];
            if current != Some(class) {
                out.push_str(style::fg(class.colour()));
                current = Some(class);
            }
            out.push(ch);
        }
        if current != Some(Class::None) {
            out.push_str(style::RESET);
        }
        Cow::Owned(out)
    }
}

// depth: the classifier, one byte at a time

fn is_digit(b: u8) -> bool {
    b.is_ascii_digit()
}

fn is_name(b: u8) -> bool {
    b.is_ascii_alphanumeric() || b == b'_'
}

/// One [`Class`] per byte of `s`.
///
/// Byte-oriented like the C it comes from, and safe on UTF-8 because every
/// byte of a multi-byte character has its high bit set: none of them match
/// any arm below, so a character is left `None` as a unit rather than split
/// across classes.
fn classify(s: &[u8]) -> Vec<Class> {
    let mut cls = vec![Class::None; s.len()];
    let len = s.len();
    let mut i = 0;
    while i < len {
        match s[i] {
            // A comment runs to the end of the line, and a REPL entry's
            // line is all there is.
            b'-' if i + 1 < len && s[i + 1] == b'-' => {
                while i < len {
                    cls[i] = Class::Comment;
                    i += 1;
                }
            }
            q @ (b'"' | b'\'') => {
                cls[i] = Class::String;
                i += 1;
                while i < len {
                    if s[i] == b'\\' && i + 1 < len {
                        cls[i] = Class::String;
                        cls[i + 1] = Class::String;
                        i += 2;
                        continue;
                    }
                    cls[i] = Class::String;
                    i += 1;
                    if s[i - 1] == q {
                        break; // the closing quote
                    }
                }
            }
            // The '$' of an interpolated string is Diluvium's; the string
            // itself is a string.
            b'$' if i + 1 < len && (s[i + 1] == b'"' || s[i + 1] == b'\'') => {
                cls[i] = Class::Diluvium;
                i += 1;
            }
            b'?' if i + 1 < len && matches!(s[i + 1], b'?' | b'.' | b'[') => {
                cls[i] = Class::Diluvium;
                i += 1;
                // '?[' paints only the '?': the bracket is ordinary syntax.
                if i < len && s[i] != b'[' {
                    cls[i] = Class::Diluvium;
                    i += 1;
                }
            }
            // A secure '~function', and only that, so '~x' stays the
            // bitwise operator it is.
            b'~' if s[i + 1..].starts_with(b"function") => {
                cls[i] = Class::Diluvium;
                i += 1;
            }
            b'.' if i + 1 < len && is_digit(s[i + 1]) => {
                i = number(s, i, &mut cls);
            }
            b if is_digit(b) => {
                i = number(s, i, &mut cls);
            }
            b if is_name(b) => {
                let word = i;
                while i < len && is_name(s[i]) {
                    i += 1;
                }
                if KEYWORDS.iter().any(|k| k.as_bytes() == &s[word..i]) {
                    cls[word..i].fill(Class::Keyword);
                }
            }
            _ => i += 1,
        }
    }
    cls
}

/// Paint the number starting at `i`, and say where it ended. Exponent signs
/// count as part of it, so `1e-3` is one number and not two and a minus.
fn number(s: &[u8], mut i: usize, cls: &mut [Class]) -> usize {
    while i < s.len()
        && (is_name(s[i])
            || s[i] == b'.'
            || ((s[i] == b'-' || s[i] == b'+') && i > 0 && (s[i - 1] == b'e' || s[i - 1] == b'E')))
    {
        cls[i] = Class::Number;
        i += 1;
    }
    i
}
