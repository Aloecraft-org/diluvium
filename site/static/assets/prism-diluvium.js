// Prism's Lua grammar, extended for what Diluvium adds, plus a small shell
// grammar for the install and DRT samples.
//
// Prism is the global the classic scripts in index.html define
// (vendor/prism-core.min.js, then vendor/prism-lua.min.js); this module runs
// after both. The interpolated-string and null-coalescing rules are carried
// over from the previous site; the rest track the language work.
//
// Contextual keywords are matched only where they can actually be one,
// because every one of them is still a valid identifier -- highlighting
// `local switch = 1` as a keyword would be a lie about the language, and the
// whole point of the design is that it is not one. Statement-openers
// (switch, case, default, defer, with, class, export, continue, const,
// static) are matched at the start of a statement; `extends` and `super`
// are matched where only they can appear.
//
// Surface:
//   installDiluviumGrammar()   extends Prism.languages.lua in place
//   installShellGrammar()      defines Prism.languages.sh

export function installDiluviumGrammar() {
  Prism.languages.insertBefore('lua', 'operator', {
    'null-coalescing': {
      pattern: /\?\?=?|\?\.|\?\[/,
      alias: 'operator',
    },
    'compound-assign': {
      // Longest first: `//=` must not be read as `/` then `/=`.
      pattern: /(?:\/\/|<<|>>|\.\.|[+\-*/%^|&])=/,
      alias: 'operator',
    },
    // Numerals with separators, and binary literals. Prism's Lua grammar
    // stops at the first `_`, which leaves `1_000_000` rendered as a number
    // and then an identifier -- two colours for one literal.
    'diluvium-number': {
      pattern: /\b0[bB][01](?:_?[01])*\b|\b0[xX][\da-fA-F](?:_?[\da-fA-F])*\b|\b\d(?:_?\d)*(?:\.\d(?:_?\d)*)?(?:[eE][+-]?\d+)?\b/,
      alias: 'number',
    },
    'secure-function': {
      pattern: /~(?=\s*function\b)/,
      alias: 'keyword',
    },
  });

  Prism.languages.insertBefore('lua', 'keyword', {
    'contextual-keyword': {
      // The lookbehind is what a statement can actually begin after: the
      // start of a line, a `;`, or one of the keywords that opens a block.
      // It used to be `(^|[\s;])`, which is any whitespace at all -- so
      // `local switch = 1` highlighted `switch` as a keyword, the one thing
      // the comment above says this must never do. It is a wider lie now
      // that ten words go through here instead of five, so it is anchored
      // properly rather than left alone.
      pattern: /(^[ \t]*|;[ \t]*|\b(?:then|do|else|repeat)[ \t]+)(?:switch|case|default|defer|with|class|export|continue|const|static)(?=[\s(]|$)/m,
      lookbehind: true,
      alias: 'keyword',
    },
    // `extends` only follows a class name, and `super` only appears as a
    // call or a lookup. Neither can be reached by an ordinary identifier in
    // those positions, so neither needs the statement-start anchor above.
    'class-keyword': {
      pattern: /\bextends\b(?=\s+[\w.])|\bsuper\b(?=\s*[.(])/,
      alias: 'keyword',
    },
    // `@name` is `self.name` and `@:m()` is `self:m()`. Highlighted as the
    // `self` it stands for rather than as punctuation.
    'self-shorthand': {
      pattern: /@:?[a-zA-Z_]\w*/,
      alias: 'variable',
    },
  });

  Prism.languages.insertBefore('lua', 'string', {
    // A backtick literal is a compiled regex, taken raw -- no escape
    // processing at all, which is the point of it: a pattern is written the
    // way every other language writes it. So there is no escape alternative
    // in this pattern, and a backslash before the closing backtick does not
    // extend the literal. `greedy` keeps a backtick inside an ordinary
    // string from opening one.
    'regex-literal': {
      pattern: /`[^`\r\n]*`/,
      greedy: true,
      alias: 'regex',
    },
    'interpolated-string': {
      pattern: /\$"(?:[^"\\{]|\\[\s\S]|\{(?:[^{}"]|"(?:[^"\\\r\n]|\\.)*")*\})*"/,
      greedy: true,
      inside: {
        'interpolation': {
          pattern: /\{(?:[^{}"]|"(?:[^"\\\r\n]|\\.)*")*\}/,
          inside: {
            'interpolation-punctuation': {
              pattern: /^\{|\}$/,
              alias: 'punctuation',
            },
            // Everything after `::` goes to string.format, so it is a
            // format spec rather than Lua to be highlighted as code.
            'format-spec': {
              pattern: /::[^{}]*(?=\}$)/,
              alias: 'attr-value',
            },
            'interpolation-content': {
              pattern: /[a-zA-Z_]\w*/,
              alias: 'variable',
            },
            rest: Prism.languages.lua,
          },
        },
        'string': /[\s\S]+/,
      },
    },
  });
}

// Just enough shell for the samples on this page: the command at the start
// of a line, its flags, strings, comments and variables. Not Prism's bash
// grammar, which is nine kilobytes for the sake of heredocs and arithmetic
// the samples never use.
export function installShellGrammar() {
  Prism.languages.sh = {
    'comment': /#.*/,
    'string': { pattern: /"(?:\\.|[^"\\])*"|'[^']*'/, greedy: true },
    'command': { pattern: /^[ \t]*[a-z][\w.-]*/m },
    'flag': { pattern: /(\s)--?[a-z][\w-]*/, lookbehind: true },
    'variable': /\$\w+|\$\{[^}]+\}/,
    'operator': /\|\||&&|\||>|</,
  };
}
