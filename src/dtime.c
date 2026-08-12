/*
** dtime.c
** The 'time' guest library. See dtime.h for why the clock is elsewhere.
**
** The calendar arithmetic is Howard Hinnant's days-from-civil / civil-from-
** days, which is exact for the whole range of a 64-bit second count and needs
** no libc time functions -- so it behaves the same on glibc, musl and wasm32,
** and has no time zone or locale to reach. All UTC.
*/

#define dtime_c

#include "lprefix.h"

#include <stddef.h>

#include "lua.h"

#include "lauxlib.h"
#include "dtime.h"

#define SECS_PER_DAY 86400

/* Past this the epoch-second result would overflow int64. The bound has room
   to spare -- 250 billion years each way, past any real use and most of the
   int64 second range time.fields can hand back -- and unlike the other fields
   'year' is otherwise unbounded, so time.of must check it before the date
   arithmetic multiplies it up. time.parse needs no such guard: it reads at
   most four year digits. */
#define YEAR_ABS_MAX 250000000000LL


/* Days since 1970-01-01 for a proleptic-Gregorian y-m-d (m in 1..12, d in
   1..31). Hinnant, public domain. */
static long long days_from_civil (long long y, int m, int d) {
  y -= (m <= 2);
  {
    long long era = (y >= 0 ? y : y - 399) / 400;
    long long yoe = y - era * 400;                         /* [0, 399] */
    long long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1; /* [0,365] */
    long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;  /* [0, 146096] */
    return era * 146097 + doe - 719468;
  }
}

/* The inverse: a day count to y-m-d. */
static void civil_from_days (long long z, long long *y, int *m, int *d) {
  long long era, doe, yoe, doy, mp, yy;
  z += 719468;
  era = (z >= 0 ? z : z - 146096) / 146097;
  doe = z - era * 146097;                                  /* [0, 146096] */
  yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; /* [0, 399] */
  yy = yoe + era * 400;
  doy = doe - (365 * yoe + yoe / 4 - yoe / 100);           /* [0, 365] */
  mp = (5 * doy + 2) / 153;                                /* [0, 11] */
  *d = (int)(doy - (153 * mp + 2) / 5 + 1);                /* [1, 31] */
  *m = (int)(mp + (mp < 10 ? 3 : -9));                     /* [1, 12] */
  *y = yy + (*m <= 2);
}

static int is_leap (long long y) {
  return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

static int days_in_month (long long y, int m) {
  static const int dm[] = {31,28,31,30,31,30,31,31,30,31,30,31};
  if (m == 2 && is_leap(y)) return 29;
  return dm[m - 1];
}

/* floor division and modulo, so a pre-1970 epoch splits into a day count and a
   0..86399 second-of-day rather than a negative remainder. */
static long long floordiv (long long a, long long b) {
  long long q = a / b, r = a % b;
  if (r != 0 && ((r < 0) != (b < 0))) q--;
  return q;
}


/* ---- time.fields(sec) -> a broken-down table ------------------------- */

static int t_fields (lua_State *L) {
  long long sec = (long long)luaL_checkinteger(L, 1);
  long long days = floordiv(sec, SECS_PER_DAY);
  /* Second-of-day by modulo, not sec - days*SECS_PER_DAY: that wide multiply
     overflows int64 for a sec near LLONG_MIN, where the floored 'days' makes
     the product more negative than sec. The modulo cannot overflow. */
  long long rem = sec % SECS_PER_DAY;
  int sod = (int)(rem < 0 ? rem + SECS_PER_DAY : rem);     /* [0, 86399] */
  long long y;
  int m, d;
  long long jan1;
  civil_from_days(days, &y, &m, &d);
  jan1 = days_from_civil(y, 1, 1);
  lua_createtable(L, 0, 8);
  lua_pushinteger(L, (lua_Integer)y);     lua_setfield(L, -2, "year");
  lua_pushinteger(L, m);                  lua_setfield(L, -2, "month");
  lua_pushinteger(L, d);                  lua_setfield(L, -2, "day");
  lua_pushinteger(L, sod / 3600);         lua_setfield(L, -2, "hour");
  lua_pushinteger(L, (sod / 60) % 60);    lua_setfield(L, -2, "min");
  lua_pushinteger(L, sod % 60);           lua_setfield(L, -2, "sec");
  /* os.date's conventions: wday 1..7 with Sunday = 1, yday 1..366. 1970-01-01
     was a Thursday (wday 5). */
  lua_pushinteger(L, (int)(((days % 7) + 4 + 7) % 7) + 1);
  lua_setfield(L, -2, "wday");
  lua_pushinteger(L, (lua_Integer)(days - jan1) + 1);
  lua_setfield(L, -2, "yday");
  return 1;
}


/* ---- time.of(fields) -> sec ------------------------------------------ */

static int field_int (lua_State *L, const char *key, int required,
                      int def, int lo, int hi) {
  int v = def;
  if (lua_getfield(L, 1, key) != LUA_TNIL) {
    if (!lua_isinteger(L, -1))
      luaL_error(L, "time.of: '%s' must be an integer", key);
    v = (int)lua_tointeger(L, -1);
    if (v < lo || v > hi)
      luaL_error(L, "time.of: '%s' is out of range", key);
  }
  else if (required)
    luaL_error(L, "time.of: '%s' is required", key);
  lua_pop(L, 1);
  return v;
}

static int t_of (lua_State *L) {
  long long y;
  int mon, day, hour, min, sec;
  luaL_checktype(L, 1, LUA_TTABLE);
  /* year has the full range; the rest are bounded, and day is bounded to the
     month's real length so nothing normalizes silently (Feb 30 is refused,
     not turned into March). */
  lua_getfield(L, 1, "year");
  if (!lua_isinteger(L, -1))
    return luaL_error(L, "time.of: 'year' is required and must be an integer");
  y = (long long)lua_tointeger(L, -1);
  lua_pop(L, 1);
  if (y < -YEAR_ABS_MAX || y > YEAR_ABS_MAX)
    return luaL_error(L, "time.of: 'year' is out of the representable range");
  mon  = field_int(L, "month", 1, 0, 1, 12);
  day  = field_int(L, "day", 1, 0, 1, 31);
  hour = field_int(L, "hour", 0, 0, 0, 23);
  min  = field_int(L, "min", 0, 0, 0, 59);
  sec  = field_int(L, "sec", 0, 0, 0, 59);
  if (day > days_in_month(y, mon))
    return luaL_error(L, "time.of: day %d is past the end of month %d", day,
                      mon);
  lua_pushinteger(L, (lua_Integer)(days_from_civil(y, mon, day) * SECS_PER_DAY
                                   + hour * 3600 + min * 60 + sec));
  return 1;
}


/* ---- time.iso(sec) -> "YYYY-MM-DDThh:mm:ssZ" ------------------------- */

static int t_iso (lua_State *L) {
  long long sec = (long long)luaL_checkinteger(L, 1);
  long long days = floordiv(sec, SECS_PER_DAY);
  long long rem = sec % SECS_PER_DAY;                      /* see t_fields */
  int sod = (int)(rem < 0 ? rem + SECS_PER_DAY : rem);
  long long y;
  int m, d;
  char buf[40];
  int n;
  civil_from_days(days, &y, &m, &d);
  /* Zero-pad the year to four digits: ISO 8601 requires it, and time.parse
     reads exactly four, so "0001-01-01T00:00:00Z" round-trips where "1-..."
     would not. Years past 9999 print wider; that is out of the standard's
     basic form but stays parseable here. */
  n = snprintf(buf, sizeof(buf), "%04lld-%02d-%02dT%02d:%02d:%02dZ",
               y, m, d, sod / 3600, (sod / 60) % 60, sod % 60);
  lua_pushlstring(L, buf, (size_t)n);
  return 1;
}


/* ---- time.parse("...") -> sec ---------------------------------------- */

/* Read exactly n decimal digits at *p (bounded by end), advance *p, and store
   the value. Returns 0 on success, -1 if there are not n digits. */
static int read_n (const char **p, const char *end, int n, long long *out) {
  long long v = 0;
  int i;
  for (i = 0; i < n; i++) {
    if (*p >= end || **p < '0' || **p > '9') return -1;
    v = v * 10 + (**p - '0');
    (*p)++;
  }
  *out = v;
  return 0;
}

/*
** ISO 8601, the subset a machine actually exchanges:
**   YYYY-MM-DD                                   (a date, midnight UTC)
**   YYYY-MM-DD(T| )hh:mm:ss[.frac][Z|±hh[:]mm]   (a time; no zone means UTC)
** The fractional part is accepted and dropped -- the result is integer
** seconds. An offset is applied, so 12:00:00+01:00 is 11:00:00Z.
*/
static int t_parse (lua_State *L) {
  size_t len;
  const char *s = luaL_checklstring(L, 1, &len);
  const char *p = s, *end = s + len;
  long long yr, mo, dy, hh = 0, mm = 0, ss = 0;
  long long off = 0;                    /* seconds to subtract to reach UTC */
  if (read_n(&p, end, 4, &yr) != 0 || p >= end || *p++ != '-' ||
      read_n(&p, end, 2, &mo) != 0 || p >= end || *p++ != '-' ||
      read_n(&p, end, 2, &dy) != 0)
    return luaL_error(L, "time.parse: not an ISO 8601 date (want YYYY-MM-DD)");
  if (mo < 1 || mo > 12 || dy < 1 || dy > 31)
    return luaL_error(L, "time.parse: the date's month or day is out of range");
  if (p < end) {
    if (*p != 'T' && *p != ' ')
      return luaL_error(L, "time.parse: a date and time are joined by 'T'");
    p++;
    if (read_n(&p, end, 2, &hh) != 0 || p >= end || *p++ != ':' ||
        read_n(&p, end, 2, &mm) != 0 || p >= end || *p++ != ':' ||
        read_n(&p, end, 2, &ss) != 0)
      return luaL_error(L, "time.parse: the time is not hh:mm:ss");
    if (hh > 23 || mm > 59 || ss > 59)
      return luaL_error(L, "time.parse: the time is out of range");
    if (p < end && *p == '.') {         /* fractional seconds: accept, drop */
      p++;
      if (p >= end || *p < '0' || *p > '9')
        return luaL_error(L, "time.parse: a '.' with no fractional digits");
      while (p < end && *p >= '0' && *p <= '9') p++;
    }
    if (p < end) {                      /* a zone */
      if (*p == 'Z') { p++; }
      else if (*p == '+' || *p == '-') {
        int neg = (*p == '-');
        long long oh, om;
        p++;
        if (read_n(&p, end, 2, &oh) != 0)
          return luaL_error(L, "time.parse: a zone offset needs two hour "
                            "digits");
        if (p < end && *p == ':') p++;  /* the ':' in ±hh:mm is optional */
        if (read_n(&p, end, 2, &om) != 0)
          return luaL_error(L, "time.parse: a zone offset needs two minute "
                            "digits");
        if (oh > 23 || om > 59)
          return luaL_error(L, "time.parse: the zone offset is out of range");
        off = (oh * 3600 + om * 60) * (neg ? -1 : 1);
      }
      else
        return luaL_error(L, "time.parse: trailing bytes after the time");
    }
  }
  if (dy > days_in_month(yr, (int)mo))
    return luaL_error(L, "time.parse: day %d is past the end of month %d",
                      (int)dy, (int)mo);
  if (p != end)
    return luaL_error(L, "time.parse: trailing bytes after the timestamp");
  lua_pushinteger(L, (lua_Integer)(days_from_civil(yr, (int)mo, (int)dy)
                                   * SECS_PER_DAY + hh * 3600 + mm * 60 + ss
                                   - off));
  return 1;
}


/* ---- registration ---------------------------------------------------- */

static const luaL_Reg time_lib[] = {
  {"iso",    t_iso},
  {"parse",  t_parse},
  {"fields", t_fields},
  {"of",     t_of},
  {NULL, NULL}
};

LUAMOD_API int luaopen_dtime (lua_State *L) {
  luaL_newlib(L, time_lib);
  return 1;
}
