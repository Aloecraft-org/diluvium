// Line editing over xterm.js.
//
// Adapted from doc/repl-reference.html in the diluvium tree. The line editor
// lives here rather than in the runtime because the runtime's own editor
// (dline.c) is for the native binary and reads a real tty; in a page xterm
// delivers keys and this stands in for it.
//
// The one behaviour worth naming: repl_eval returning 1 means "unfinished",
// not "wrong". That is what lets `function f()` open a continuation prompt
// instead of raising, and it is the reason this drives repl_eval rather than
// run_lua.

const MAX_OUTPUT_CHARS = 200000;   // one runaway loop should not kill the tab

export class Repl {
  constructor(term, rt) {
    this.term = term;
    this.rt = rt;
    this.line = '';
    this.pos = 0;
    this.pending = '';             // lines of an unfinished statement
    this.history = [];
    this.hindex = 0;
    this.outputChars = 0;
    this.truncated = false;
  }

  get prompt() { return this.pending ? '>> ' : '> '; }

  write(s) { this.term.write(s.replace(/\n/g, '\r\n')); }

  // Called from the WASI fd_write hook, so it is the one place a runaway
  // program can flood the page.
  output(text) {
    if (this.truncated) return;
    this.outputChars += text.length;
    if (this.outputChars > MAX_OUTPUT_CHARS) {
      this.truncated = true;
      this.write('\n-- output truncated; restart the kernel to continue --\n');
      return;
    }
    this.write(text);
  }

  redraw() {
    this.term.write('\r\x1b[K' + this.prompt + this.line);
    const back = this.line.length - this.pos;
    if (back > 0) this.term.write(`\x1b[${back}D`);
  }

  submit() {
    this.write('\n');
    const source = this.pending + this.line;
    if (this.line.trim() !== '') {
      this.history.push(this.line);
      this.hindex = this.history.length;
    }

    let status = 0;
    if (source.trim() !== '') {
      this.outputChars = 0;
      this.truncated = false;
      try {
        status = this.rt.eval(source);
      } catch (err) {
        // A trap unwinds the WASM stack and leaves the Lua state
        // unusable; there is no recovering it in place.
        this.write(`\nkernel fault: ${err.message}\nrestart the kernel to continue\n`);
        status = 2;
      }
    }

    this.pending = status === 1 ? source + '\n' : '';
    this.line = '';
    this.pos = 0;
    this.redraw();
  }

  tab() {
    const { offset, items } = this.rt.complete(this.line, this.pos);
    if (items.length === 0) return;
    const typed = this.line.slice(offset, this.pos);
    let common = items[0];
    for (const it of items) {
      while (!it.startsWith(common)) common = common.slice(0, -1);
    }
    if (common.length > typed.length) {
      const add = common.slice(typed.length);
      this.line = this.line.slice(0, this.pos) + add + this.line.slice(this.pos);
      this.pos += add.length;
    } else if (items.length > 1) {
      this.write('\n' + items.join('  ') + '\n');
    }
    this.redraw();
  }

  clear() {
    this.term.write('\x1b[2J\x1b[H');
    this.redraw();
  }

  reset() {
    this.rt.reset();
    this.pending = '';
    this.line = '';
    this.pos = 0;
    this.outputChars = 0;
    this.truncated = false;
    this.write('\n-- kernel restarted --\n');
    this.redraw();
  }

  key(data) {
    for (const ch of data) {
      const c = ch.charCodeAt(0);
      if (ch === '\r' || ch === '\n') { this.submit(); }
      else if (ch === '\t') { this.tab(); }
      else if (c === 127) {                                   // backspace
        if (this.pos > 0) {
          this.line = this.line.slice(0, this.pos - 1) + this.line.slice(this.pos);
          this.pos--;
          this.redraw();
        }
      }
      else if (ch === '\x03') {                               // ^C
        this.write('^C\n');
        this.line = ''; this.pos = 0; this.pending = '';
        this.redraw();
      }
      else if (ch === '\x0c') { this.clear(); }               // ^L
      else if (ch === '\x01') { this.pos = 0; this.redraw(); }              // ^A
      else if (ch === '\x05') { this.pos = this.line.length; this.redraw(); } // ^E
      else if (ch === '\x15') {                                             // ^U
        this.line = this.line.slice(this.pos);
        this.pos = 0;
        this.redraw();
      }
      else if (ch === '\x1b') { /* escape sequences handled in seq() */ }
      else if (c >= 32) {
        this.line = this.line.slice(0, this.pos) + ch + this.line.slice(this.pos);
        this.pos++;
        this.redraw();
      }
    }
  }

  seq(data) {
    if (data === '\x1b[A' || data === '\x1b[B') {
      if (this.history.length === 0) return true;
      this.hindex += (data === '\x1b[A') ? -1 : 1;
      this.hindex = Math.max(0, Math.min(this.history.length, this.hindex));
      this.line = this.history[this.hindex] ?? '';
      this.pos = this.line.length;
      this.redraw();
      return true;
    }
    if (data === '\x1b[D') { if (this.pos > 0) { this.pos--; this.redraw(); } return true; }
    if (data === '\x1b[C') { if (this.pos < this.line.length) { this.pos++; this.redraw(); } return true; }
    if (data === '\x1b[H') { this.pos = 0; this.redraw(); return true; }
    if (data === '\x1b[F') { this.pos = this.line.length; this.redraw(); return true; }
    return false;
  }

  // A paste arrives as one onData call carrying newlines. Splitting on them
  // and submitting each line is what makes pasting a function definition
  // behave the way typing it does.
  feed(data) {
    if (data.length > 1 && /[\r\n]/.test(data)) {
      const lines = data.split(/\r\n|\r|\n/);
      lines.forEach((l, i) => {
        this.key(l);
        if (i < lines.length - 1) this.submit();
      });
      return;
    }
    if (!this.seq(data)) this.key(data);
  }
}
