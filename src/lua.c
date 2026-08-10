/*
** $Id: lua.c $
** Lua stand-alone interpreter
** See Copyright Notice in lua.h
*/

#define lua_c

#include "lprefix.h"


#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <errno.h>
#include <signal.h>

#include "lua.h"

#include "lauxlib.h"
#include "lualib.h"
#include "analyze.h"
#include "drepl.h"
#include "dtask.h"
#include "dlibs.h"
#include "dline.h"
#include "llimits.h"


#if !defined(LUA_PROGNAME)
#define LUA_PROGNAME		"lua"
#endif

#if !defined(LUA_INIT_VAR)
#define LUA_INIT_VAR		"LUA_INIT"
#endif


#define LUA_INITVARVERSION	LUA_INIT_VAR LUA_VERSUFFIX


static lua_State *globalL = NULL;

static const char *progname = LUA_PROGNAME;


#if defined(LUA_USE_POSIX)   /* { */

/*
** Use 'sigaction' when available.
*/
static void setsignal (int sig, void (*handler)(int)) {
  struct sigaction sa;
  sa.sa_handler = handler;
  sa.sa_flags = 0;
  sigemptyset(&sa.sa_mask);  /* do not mask any signal */
  sigaction(sig, &sa, NULL);
}

#else           /* }{ */

#define setsignal            signal

#endif                               /* } */


/*
** Hook set by signal function to stop the interpreter.
*/
static void lstop (lua_State *L, lua_Debug *ar) {
  (void)ar;  /* unused arg. */
  lua_sethook(L, NULL, 0, 0);  /* reset hook */
  luaL_error(L, "interrupted!");
}


/*
** Function to be called at a C signal. Because a C signal cannot
** just change a Lua state (as there is no proper synchronization),
** this function only sets a hook that, when called, will stop the
** interpreter.
*/
static void laction (int i) {
  int flag = LUA_MASKCALL | LUA_MASKRET | LUA_MASKLINE | LUA_MASKCOUNT;
  setsignal(i, SIG_DFL); /* if another SIGINT happens, terminate process */
  lua_sethook(globalL, lstop, flag, 1);
}


static void print_usage_body (void);


static void print_usage (const char *badoption) {
  lua_writestringerror("%s: ", progname);
  if (badoption[1] == 'e' || badoption[1] == 'l')
    lua_writestringerror("'%s' needs argument\n", badoption);
  else
    lua_writestringerror("unrecognized option '%s'\n", badoption);
  print_usage_body();
}


/*
** Diluvium: the options, printed either because one was wrong or because
** '-h' asked. Interactive-only settings are grouped separately: they mean
** nothing when a script was named, and listing them together with the
** rest hides that.
*/
static void print_usage_body (void) {
  lua_writestringerror(
  "usage: %s [options] [script [args]]\n"
  "\n"
  "Options:\n"
  "  -e stat   execute string 'stat'\n"
  "  -i        enter interactive mode after executing 'script'\n"
  "  -l mod    require library 'mod' into global 'mod'\n"
  "  -l g=mod  require library 'mod' into global 'g'\n"
  "  -v        show version information\n"
  "  -E        ignore environment variables\n"
  "  -W        turn warnings on\n"
  "  -h        this message\n"
  "  --task    run coroutine-hosted, so the program can wait (not with -i)\n"
  "  --        stop handling options\n"
  "  -         stop handling options and execute stdin\n"
  "\n"
  "Interactive mode:\n"
  "  Tab completes names, and the arrow keys walk the history kept in\n"
  "  ~/.diluvium_history. Set NO_COLOR to turn off syntax highlighting.\n"
  ,
  progname);
}


/*
** Prints an error message, adding the program name in front of it
** (if present)
*/
static void l_message (const char *pname, const char *msg) {
  if (pname) lua_writestringerror("%s: ", pname);
  lua_writestringerror("%s\n", msg);
}


/*
** Check whether 'status' is not OK and, if so, prints the error
** message on the top of the stack.
*/
static int report (lua_State *L, int status) {
  if (status != LUA_OK) {
    const char *msg = lua_tostring(L, -1);
    if (msg == NULL)
      msg = "(error message not a string)";
    l_message(progname, msg);
    lua_pop(L, 1);  /* remove message */
  }
  return status;
}


/*
** Message handler used to run all chunks
*/
static int msghandler (lua_State *L) {
  const char *msg = lua_tostring(L, 1);
  if (msg == NULL) {  /* is error object not a string? */
    if (luaL_callmeta(L, 1, "__tostring") &&  /* does it have a metamethod */
        lua_type(L, -1) == LUA_TSTRING)  /* that produces a string? */
      return 1;  /* that is the message */
    else
      msg = lua_pushfstring(L, "(error object is a %s value)",
                               luaL_typename(L, 1));
  }
  luaL_traceback(L, L, msg, 1);  /* append a standard traceback */
  return 1;  /* return the traceback */
}


/*
** Diluvium: '--task' runs chunks through the coroutine-hosted driver in
** dtask.c instead of 'lua_pcall', so a program can park. Off by default,
** and deliberately so: making the top level yieldable is observable from
** Lua, and the default path is the one the conformance suite holds to stock
** behaviour. See dtask.h.
*/
static int task_mode = 0;


/*
** Diluvium: keep 'globalL' on whatever is actually running.
**
** 'laction' installs a debug hook on 'globalL', and a hook is per-state. In
** task mode the chunk runs on a thread, so leaving 'globalL' on the main
** state would install the hook where no code is executing and Ctrl-C would
** stop interrupting anything at all -- silently, since the handler still
** runs and still looks like it did something.
*/
static lua_State *mainL = NULL;

static void task_running (lua_State *co, void *ud) {
  (void)ud;
  globalL = (co != NULL) ? co : mainL;
}


/*
** Diluvium: the clock, which 8.3 puts on the host side rather than in the
** runtime. This is the whole of what makes '--task' a reference host: a
** program parks, the driver hands over a duration, and this decides.
**
** For a single-threaded CLI over local queues the honest answer to an
** indefinite wait is that it can never be satisfied -- nothing else can write
** while the program is parked -- so it reports a deadlock rather than hanging
** forever. A finite wait really does let the time pass, because a program that
** measures its own timeouts should see them.
*/
#if defined(LUA_USE_POSIX)
#include <time.h>
#endif

static int task_waiting (lua_Integer ms, void *ud) {
  (void)ud;
  if (ms < 0)
    return -1;  /* nothing here can write to a local queue while we are parked */
#if defined(LUA_USE_POSIX)
  {
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000);
    ts.tv_nsec = (long)((ms % 1000) * 1000000L);
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
      ;  /* a signal is not the end of the wait; SIGINT is handled by the hook */
  }
#endif
  return 0;  /* the time passed and nothing arrived */
}


/*
** Interface to 'lua_pcall', which sets appropriate message function
** and C-signal handler. Used to run all chunks.
*/
static int docall (lua_State *L, int narg, int nres) {
  int status;
  int base = lua_gettop(L) - narg;  /* function index */
  lua_pushcfunction(L, msghandler);  /* push message handler */
  lua_insert(L, base);  /* put it under function and args */
  mainL = globalL = L;  /* to be available to 'laction' */
  setsignal(SIGINT, laction);  /* set C-signal handler */
  if (!task_mode)
    status = lua_pcall(L, narg, nres, base);
  else {
    /* The driver takes 'lua_pcall's stack contract, so only the call
       changes. It reports a park separately from an error; nothing in the
       runtime can park yet, so reaching that means the chunk yielded on
       purpose and there is no host loop to resume it. When 'queue.wait'
       lands, the wait-set drive loop belongs exactly here -- which is what
       makes this mode the reference host rather than only a switch. */
    switch (diluvium_task_call(L, narg, nres, base)) {
      case DILUVIUM_TASK_OK:    status = LUA_OK; break;
      case DILUVIUM_TASK_YIELD: /* FALLTHROUGH: report it as a failure */
      default:                  status = LUA_ERRRUN; break;
    }
  }
  setsignal(SIGINT, SIG_DFL); /* reset C-signal handler */
  globalL = mainL;
  lua_remove(L, base);  /* remove message handler from the stack */
  return status;
}


static void print_version (void) {
  lua_writestring(LUA_COPYRIGHT, strlen(LUA_COPYRIGHT));
  lua_writeline();
}


/*
** Diluvium: what an interactive session prints before the first prompt.
** The version line alone said nothing about how to get out again or what
** the editor can do, which is the first thing anyone meeting a REPL
** wants to know.
**
** 'help' is defined only for interactive sessions, and only when nothing
** of that name exists already, so a script's own 'help' is never
** shadowed.
*/
static int diluvium_help (lua_State *L) {
  static const char msg[] =
    "Diluvium extends Lua with:\n"
    "  $\"text {expr}\"       string interpolation, {x::%.2f} to format\n"
    "  a ?? b               b when a is nil\n"
    "  a?.b   a?[k]         nil when a is nil, skipping the rest\n"
    "  x += 1               and -= *= /= //= %= ^= ..= |= &= <<= >>= ?\?=\n"
    "  switch x do ... end  case a, b then ... default ... end\n"
    "  defer stat           runs however the block is left\n"
    "  ~function f() end    obfuscated at rest\n"
    "\n"
    "In this REPL:\n"
    "  Tab       complete a name\n"
    "  Up/Down   history, kept in ~/.diluvium_history\n"
    "  Ctrl-A/E  start/end of line     Ctrl-W  delete word\n"
    "  Ctrl-C    abandon the line      Ctrl-D  exit\n"
    "\n"
    "Set NO_COLOR to turn off syntax highlighting.\n";
  (void)L;
  lua_writestring(msg, sizeof(msg) - 1);
  return 0;
}


static void repl_intro (lua_State *L) {
  lua_getglobal(L, "help");
  if (lua_isnil(L, -1)) {  /* nothing called 'help' already? */
    lua_pushcfunction(L, diluvium_help);
    lua_setglobal(L, "help");
    lua_writestringerror("%s\n", "type 'help()' for what Diluvium adds, "
                                  "Ctrl-D to exit");
  }
  lua_pop(L, 1);
}


/*
** Create the 'arg' table, which stores all arguments from the
** command line ('argv'). It should be aligned so that, at index 0,
** it has 'argv[script]', which is the script name. The arguments
** to the script (everything after 'script') go to positive indices;
** other arguments (before the script name) go to negative indices.
** If there is no script name, assume interpreter's name as base.
** (If there is no interpreter's name either, 'script' is -1, so
** table sizes are zero.)
*/
static void createargtable (lua_State *L, char **argv, int argc, int script) {
  int i, narg;
  narg = argc - (script + 1);  /* number of positive indices */
  lua_createtable(L, narg, script + 1);
  for (i = 0; i < argc; i++) {
    lua_pushstring(L, argv[i]);
    lua_rawseti(L, -2, i - script);
  }
  lua_setglobal(L, "arg");
}


static int dochunk (lua_State *L, int status) {
  if (status == LUA_OK) status = docall(L, 0, 0);
  return report(L, status);
}


static int dofile (lua_State *L, const char *name) {
  return dochunk(L, luaL_loadfilex(L, name, "bt"));
}


static int dostring (lua_State *L, const char *s, const char *name) {
  return dochunk(L, luaL_loadbufferx(L, s, strlen(s), name, "t"));
}


/*
** Receives 'globname[=modname]' and runs 'globname = require(modname)'.
** If there is no explicit modname and globname contains a '-', cut
** the suffix after '-' (the "version") to make the global name.
*/
static int dolibrary (lua_State *L, char *globname) {
  int status;
  char *suffix = NULL;
  char *modname = strchr(globname, '=');
  if (modname == NULL) {  /* no explicit name? */
    modname = globname;  /* module name is equal to global name */
    suffix = strchr(modname, *LUA_IGMARK);  /* look for a suffix mark */
  }
  else {
    *modname = '\0';  /* global name ends here */
    modname++;  /* module name starts after the '=' */
  }
  lua_getglobal(L, "require");
  lua_pushstring(L, modname);
  status = docall(L, 1, 1);  /* call 'require(modname)' */
  if (status == LUA_OK) {
    if (suffix != NULL)  /* is there a suffix mark? */
      *suffix = '\0';  /* remove suffix from global name */
    lua_setglobal(L, globname);  /* globname = require(modname) */
  }
  return report(L, status);
}


/*
** Push on the stack the contents of table 'arg' from 1 to #arg
*/
static int pushargs (lua_State *L) {
  int i, n;
  if (lua_getglobal(L, "arg") != LUA_TTABLE)
    luaL_error(L, "'arg' is not a table");
  n = (int)luaL_len(L, -1);
  luaL_checkstack(L, n + 3, "too many arguments to script");
  for (i = 1; i <= n; i++)
    lua_rawgeti(L, -i, i);
  lua_remove(L, -i);  /* remove table from the stack */
  return n;
}


static int handle_script (lua_State *L, char **argv) {
  int status;
  const char *fname = argv[0];
  if (strcmp(fname, "-") == 0 && strcmp(argv[-1], "--") != 0)
    fname = NULL;  /* stdin */
  status = luaL_loadfilex(L, fname, "bt");
  if (status == LUA_OK) {
    int n = pushargs(L);  /* push arguments to script */
    status = docall(L, n, LUA_MULTRET);
  }
  return report(L, status);
}


/* bits of various argument indicators in 'args' */
#define has_error	1	/* bad option */
#define has_i		2	/* -i */
#define has_v		4	/* -v */
#define has_e		8	/* -e */
#define has_E		16	/* -E */
#define has_h		32	/* -h (Diluvium) */
#define has_task	64	/* --task (Diluvium) */


/*
** Traverses all arguments from 'argv', returning a mask with those
** needed before running any Lua code or an error code if it finds any
** invalid argument. In case of error, 'first' is the index of the bad
** argument.  Otherwise, 'first' is -1 if there is no program name,
** 0 if there is no script name, or the index of the script name.
*/
static int collectargs (char **argv, int *first) {
  int args = 0;
  int i;
  if (argv[0] != NULL) {  /* is there a program name? */
    if (argv[0][0])  /* not empty? */
      progname = argv[0];  /* save it */
  }
  else {  /* no program name */
    *first = -1;
    return 0;
  }
  for (i = 1; argv[i] != NULL; i++) {  /* handle arguments */
    *first = i;
    if (argv[i][0] != '-')  /* not an option? */
        return args;  /* stop handling options */
    switch (argv[i][1]) {  /* else check option */
      case '-':  /* '--' or a long option */
        /* Diluvium: '--task' is the only long option, so this case does not
           need a general parser yet. Anything else after '--' is still an
           error, as upstream has it. */
        if (strcmp(argv[i], "--task") == 0) {
          args |= has_task;
          break;
        }
        if (argv[i][2] != '\0')  /* extra characters after '--'? */
          return has_error;  /* invalid option */
        /* if there is a script name, it comes after '--' */
        *first = (argv[i + 1] != NULL) ? i + 1 : 0;
        return args;
      case '\0':  /* '-' */
        return args;  /* script "name" is '-' */
      case 'E':
        if (argv[i][2] != '\0')  /* extra characters? */
          return has_error;  /* invalid option */
        args |= has_E;
        break;
      case 'W':
        if (argv[i][2] != '\0')  /* extra characters? */
          return has_error;  /* invalid option */
        break;
      case 'h':  /* Diluvium: print the options and stop */
        if (argv[i][2] != '\0')  /* extra characters? */
          return has_error;  /* invalid option */
        args |= has_h;
        break;
      case 'i':
        args |= has_i;  /* (-i implies -v) *//* FALLTHROUGH */
      case 'v':
        if (argv[i][2] != '\0')  /* extra characters? */
          return has_error;  /* invalid option */
        args |= has_v;
        break;
      case 'e':
        args |= has_e;  /* FALLTHROUGH */
      case 'l':  /* both options need an argument */
        if (argv[i][2] == '\0') {  /* no concatenated argument? */
          i++;  /* try next 'argv' */
          if (argv[i] == NULL || argv[i][0] == '-')
            return has_error;  /* no next argument or it is another option */
        }
        break;
      default:  /* invalid option */
        return has_error;
    }
  }
  *first = 0;  /* no script name */
  return args;
}


/*
** Processes options 'e' and 'l', which involve running Lua code, and
** 'W', which also affects the state.
** Returns 0 if some code raises an error.
*/
static int runargs (lua_State *L, char **argv, int n) {
  int i;
  lua_warning(L, "@off", 0);  /* by default, Lua stand-alone has warnings off */
  for (i = 1; i < n; i++) {
    int option = argv[i][1];
    lua_assert(argv[i][0] == '-');  /* already checked */
    switch (option) {
      case 'e':  case 'l': {
        int status;
        char *extra = argv[i] + 2;  /* both options need an argument */
        if (*extra == '\0') extra = argv[++i];
        lua_assert(extra != NULL);
        status = (option == 'e')
                 ? dostring(L, extra, "=(command line)")
                 : dolibrary(L, extra);
        if (status != LUA_OK) return 0;
        break;
      }
      case 'W':
        lua_warning(L, "@on", 0);  /* warnings on */
        break;
    }
  }
  return 1;
}


static char *(*l_getenv)(const char *name);

/* Function to ignore environment variables, used by option -E */
static char *no_getenv (const char *name) {
  UNUSED(name);
  return NULL;
}


static int handle_luainit (lua_State *L) {
  const char *name = "=" LUA_INITVARVERSION;
  const char *init = l_getenv(name + 1);
  if (init == NULL) {
    name = "=" LUA_INIT_VAR;
    init = l_getenv(name + 1);  /* try alternative name */
  }
  if (init == NULL) return LUA_OK;
  else if (init[0] == '@')
    return dofile(L, init+1);
  else
    return dostring(L, init, name);
}


/*
** {==================================================================
** Read-Eval-Print Loop (REPL)
** ===================================================================
*/

#if !defined(LUA_PROMPT)
#define LUA_PROMPT		"> "
#define LUA_PROMPT2		">> "
#endif

#if !defined(LUA_MAXINPUT)
#define LUA_MAXINPUT		512
#endif


/*
** lua_stdin_is_tty detects whether the standard input is a 'tty' (that
** is, whether we're running lua interactively).
*/
#if !defined(lua_stdin_is_tty)	/* { */

#if defined(LUA_USE_POSIX)	/* { */

#include <unistd.h>
#define lua_stdin_is_tty()	isatty(0)

#elif defined(LUA_USE_WINDOWS)	/* }{ */

#include <io.h>
#include <windows.h>

#define lua_stdin_is_tty()	_isatty(_fileno(stdin))

#else				/* }{ */

/* ISO C definition */
#define lua_stdin_is_tty()	1  /* assume stdin is a tty */

#endif				/* } */

#endif				/* } */

/*
** Diluvium: line editing is dline.c rather than GNU readline. See
** dline.h for why -- licence and size, and the fact that readline could
** never be reached from the WASM build, so the native and browser REPLs
** shared no editing code at all.
**
** Where there is no terminal, dline falls back to plain buffered input,
** which is how stock Lua behaves when built without readline.
*/
#define lua_saveline(line)	diluvium_history_add(line)


/*
** Return the string to be used as a prompt by the interpreter. Leave
** the string (or nil, if using the default value) on the stack, to keep
** it anchored.
*/
static const char *get_prompt (lua_State *L, int firstline) {
  if (lua_getglobal(L, firstline ? "_PROMPT" : "_PROMPT2") == LUA_TNIL)
    return (firstline ? LUA_PROMPT : LUA_PROMPT2);  /* use the default */
  else {  /* apply 'tostring' over the value */
    const char *p = luaL_tolstring(L, -1, NULL);
    lua_remove(L, -2);  /* remove original value */
    return p;
  }
}



/*
** Prompt the user, read a line, and push it into the Lua stack.
*/
static int pushline (lua_State *L, int firstline) {
  size_t l;
  const char *prmt = get_prompt(L, firstline);
  char *b = diluvium_readline(L, prmt);
  lua_pop(L, 1);  /* remove prompt */
  if (b == NULL)
    return 0;  /* no input */
  l = strlen(b);
  if (l > 0 && b[l-1] == '\n')  /* line ends with newline? */
    b[--l] = '\0';  /* remove it */
  lua_pushlstring(L, b, l);
  diluvium_freeline(b);
  return 1;
}


static void checklocal (const char *line) {
  static const size_t szloc = sizeof("local") - 1;
  static const char space[] = " \t";
  line += strspn(line, space);  /* skip spaces */
  if (strncmp(line, "local", szloc) == 0 &&  /* "local"? */
      strchr(space, *(line + szloc)) != NULL) {  /* followed by a space? */
    lua_writestringerror("%s\n",
      "warning: locals do not survive across lines in interactive mode");
  }
}


/*
** Read a line, and keep reading while it is an unfinished statement.
**
** Diluvium: what to make of each attempt -- a value, more input needed,
** or a real error -- comes from 'diluvium_repl_load', which every other
** front end shares (see drepl.h). What is left here is only the part
** specific to a terminal: prompting for the next line and joining it on.
**
** Returns LUA_OK with the compiled chunk at index 1, or an error status
** with the message there.
*/
static int loadline (lua_State *L) {
  const char *line;
  size_t len;
  int status;
  lua_settop(L, 0);
  if (!pushline(L, 1))
    return -1;  /* no input */
  checklocal(lua_tostring(L, 1));
  for (;;) {
    line = lua_tolstring(L, 1, &len);
    status = diluvium_repl_load(L, line, len, "=stdin");
    if (status != DILUVIUM_REPL_INCOMPLETE)
      break;
    if (!pushline(L, 0)) {  /* input ended inside an unfinished statement */
      status = luaL_loadbufferx(L, line, len, "=stdin", "t");
      break;  /* report it as the syntax error it is */
    }
    lua_pushliteral(L, "\n");  /* join the lines... */
    lua_insert(L, -2);          /* ...with a newline between them */
    lua_concat(L, 3);
  }
  line = lua_tostring(L, 1);
  if (line[0] != '\0')  /* non empty? */
    lua_saveline(line);  /* keep history */
  lua_remove(L, 1);  /* remove line from the stack */
  lua_assert(lua_gettop(L) == 1);
  return (status == DILUVIUM_REPL_OK) ? LUA_OK : status;
}


/*
** Prints (calling the Lua 'print' function) any values on the stack
*/
static void l_print (lua_State *L) {
  int n = lua_gettop(L);
  if (n > 0) {  /* any result to be printed? */
    luaL_checkstack(L, LUA_MINSTACK, "too many results to print");
    lua_getglobal(L, "print");
    lua_insert(L, 1);
    if (lua_pcall(L, n, 0, 0) != LUA_OK)
      l_message(progname, lua_pushfstring(L, "error calling 'print' (%s)",
                                             lua_tostring(L, -1)));
  }
}


/*
** Do the REPL: repeatedly read (load) a line, evaluate (call) it, and
** print any results.
*/
static void doREPL (lua_State *L) {
  int status;
  const char *oldprogname = progname;
  const char *histfile = diluvium_history_path();
  progname = NULL;  /* no 'progname' on errors in interactive mode */
  diluvium_history_load(histfile);  /* absent or unreadable is fine */
  while ((status = loadline(L)) != -1) {
    if (status == LUA_OK)
      status = docall(L, 0, LUA_MULTRET);
    if (status == LUA_OK) l_print(L);
    else report(L, status);
  }
  diluvium_history_save(histfile);
  diluvium_history_free();
  lua_settop(L, 0);  /* clear stack */
  lua_writeline();
  progname = oldprogname;
}

/* }================================================================== */

#if !defined(luai_openlibs)
#if defined(LUA_NODEBUGLIB)
/* With this option, code must require the debug library before using it */
#define luai_openlibs(L)  luaL_openselectedlibs(L, ~LUA_DBLIBK, LUA_DBLIBK)
#else
/* The default is to open all standard libraries */
#define luai_openlibs(L)  luaL_openselectedlibs(L, ~0, 0)
#endif
#endif


/*
** Main body of stand-alone interpreter (to be called in protected mode).
** Reads the options and handles them all.
*/
static int pmain (lua_State *L) {
  int argc = (int)lua_tointeger(L, 1);
  char **argv = (char **)lua_touserdata(L, 2);
  int script;
  int args = collectargs(argv, &script);
  int optlim = (script > 0) ? script : argc; /* first argv not an option */
  luaL_checkversion(L);  /* check that interpreter has correct version */
  if (args == has_error) {  /* bad arg? */
    print_usage(argv[script]);  /* 'script' has index of bad arg. */
    return 0;
  }
  if (args & has_h) {  /* Diluvium: option '-h'? */
    print_usage_body();
    return 0;
  }
  if ((args & has_task) && (args & has_i)) {
    /* Diluvium: a parked prompt is undesigned -- what an interactive session
       should show while a program waits is a real question and not one to
       answer by accident. Refuse the combination rather than pick. */
    l_message(progname, "'--task' and '-i' cannot be combined yet: "
                        "an interactive session cannot park a program");
    return 0;
  }
  if (args & has_task) {  /* Diluvium: run chunks coroutine-hosted */
    task_mode = 1;
    diluvium_task_sethook(task_running, NULL);
    diluvium_task_setwait(task_waiting, NULL);
  }
  if (args & has_v)  /* option '-v'? */
    print_version();
  if (args & has_E) {  /* option '-E'? */
    l_getenv = &no_getenv;  /* program will ignore environment variables */
    lua_pushboolean(L, 1);  /* signal for libraries to ignore env. vars. */
    lua_setfield(L, LUA_REGISTRYINDEX, "LUA_NOENV");
  }
  else
    l_getenv = &getenv;
  luai_openlibs(L);  /* open standard libraries */
  diluvium_openlibs(L);  /* Diluvium: msgpack, and what follows it */
  createargtable(L, argv, argc, script);  /* create table 'arg' */
  lua_gc(L, LUA_GCRESTART);  /* start GC... */
  lua_gc(L, LUA_GCGEN);  /* ...in generational mode */
  if (handle_luainit(L) != LUA_OK)  /* run LUA_INIT */
    return 0;  /* error running LUA_INIT */
  if (!runargs(L, argv, optlim))  /* execute arguments -e, -l, and -W */
    return 0;  /* something failed */
  if (script > 0) {  /* execute main script (if there is one) */
    if (handle_script(L, argv + script) != LUA_OK)
      return 0;  /* interrupt in case of error */
  }
  if (args & has_i) {  /* -i option? */
    repl_intro(L);  /* '-i' already printed the version, via has_v */
    doREPL(L);  /* do read-eval-print loop */
  }
  else if (script < 1 && !(args & (has_e | has_v))) { /* no active option? */
    if (lua_stdin_is_tty()) {  /* running in interactive mode? */
      print_version();
      repl_intro(L);
      doREPL(L);  /* do read-eval-print loop */
    }
    else dofile(L, NULL);  /* executes stdin as a file */
  }
  lua_pushboolean(L, 1);  /* signal no errors */
  return 1;
}


#ifndef DILUVIUM_AS_LIBRARY
int main (int argc, char **argv) {
  int status, result;
  lua_State *L = luaL_newstate();  /* create state */
  if (L == NULL) {
    l_message(argv[0], "cannot create state: not enough memory");
    return EXIT_FAILURE;
  }
  lua_gc(L, LUA_GCSTOP);  /* stop GC while building state */
  lua_pushcfunction(L, &pmain);  /* to call 'pmain' in protected mode */
  lua_pushinteger(L, argc);  /* 1st argument */
  lua_pushlightuserdata(L, argv); /* 2nd argument */
  status = lua_pcall(L, 2, 1, 0);  /* do the call */
  result = lua_toboolean(L, -1);  /* get result */
  report(L, status);
  lua_close(L);
  return (result && status == LUA_OK) ? EXIT_SUCCESS : EXIT_FAILURE;
}
#endif
