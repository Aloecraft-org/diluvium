/*
** dline.h
** Diluvium line editor.
**
** Replaces GNU readline. That library is GPLv3 and, linked statically,
** larger than the whole Diluvium runtime -- 336 KB plus 203 KB of
** terminfo against a 328 KB interpreter -- so it dominated both the
** licence story and the size of the binary the browser build is
** measured against. It also could not be reached from the WASM build at
** all, so the native and browser REPLs shared no editing code.
**
** This is written for Diluvium rather than vendored, so the repository
** stays under one licence, and so completion can go straight to
** 'diluvium_repl_complete' instead of through readline's global-variable
** interface.
**
** Where there is no terminal -- a pipe, a non-tty, Windows, WASI -- every
** entry point degrades to plain buffered input, which is what stock Lua
** does when it is built without readline.
*/

#ifndef dline_h
#define dline_h

#include "lua.h"

/*
** Read one line, editing it in place when the input is a terminal.
** Returns a buffer the caller frees with 'diluvium_freeline', or NULL at
** end of input. The trailing newline is not included.
**
** 'L' is used only for completion; pass NULL to disable it.
*/
char *diluvium_readline (lua_State *L, const char *prompt);
void diluvium_freeline (char *line);

/* History. 'add' ignores blank lines and immediate duplicates. */
void diluvium_history_add (const char *line);
int diluvium_history_load (const char *path);
int diluvium_history_save (const char *path);
void diluvium_history_free (void);

/* The default history file, "$HOME/.diluvium_history", or NULL if there
   is no home directory to put it in. The returned buffer is static. */
const char *diluvium_history_path (void);

#endif
