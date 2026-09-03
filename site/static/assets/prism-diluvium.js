// Prism's Lua grammar, extended for what Diluvium adds, plus a small shell
// grammar for the install and DRT samples.
//
// Prism is the global the classic scripts in index.html define
// (vendor/prism-core.min.js, then vendor/prism-lua.min.js); this module runs
// after both. The interpolated-string and null-coalescing rules are carried
// over from the previous site; the rest track the 5.5 language work.
// Contextual keywords (switch, case, default, defer, with) are matched only
// where they open a statement, because they are still valid identifiers --
// highlighting `local switch = 1` as a keyword would be a lie about the
// language.
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
    'secure-function': {
      pattern: /~(?=\s*function\b)/,
      alias: 'keyword',
    },
  });

  Prism.languages.insertBefore('lua', 'keyword', {
    'contextual-keyword': {
      pattern: /(^|[\s;])(?:switch|case|default|defer|with)(?=[\s(]|$)/m,
      lookbehind: true,
      alias: 'keyword',
    },
  });

  Prism.languages.insertBefore('lua', 'string', {
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
