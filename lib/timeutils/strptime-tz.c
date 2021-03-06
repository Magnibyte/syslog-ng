/*
 * Copyright (c) 2018 Balabit
 * Copyright (c) 2018 Balazs Scheidler
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * As an additional exemption you are allowed to compile & link against the
 * OpenSSL libraries as published by the OpenSSL project. See the file
 * COPYING for details.
 *
 */
/*  $NetBSD: strptime.c,v 1.49 2015/10/09 17:21:45 christos Exp $ */

/*-
 * Copyright (c) 1997, 1998, 2005, 2008 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code was contributed to The NetBSD Foundation by Klaus Klein.
 * Heavily optimised by David Laight
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "strptime-tz.h"
#include "timeutils/timeutils.h"
#include "timeutils/cache.h"
#include <time.h>
#include <ctype.h>
#include <string.h>
#include <stdint.h>

typedef unsigned char _u_char;
typedef unsigned int _uint;

#define __UNCONST(a)    ((void *)(unsigned long)(const void *)(a))

#define TM_YEAR_BASE 1900
#define TM_SUNDAY 0
#define TM_MONDAY 1
#define TM_TUESDAY  2
#define TM_WEDNESDAY  3
#define TM_THURSDAY 4
#define TM_FRIDAY 5
#define TM_SATURDAY 6

#define TM_JANUARY  0
#define TM_FEBRUARY 1
#define TM_MARCH  2
#define TM_APRIL  3
#define TM_MAY    4
#define TM_JUNE   5
#define TM_JULY   6
#define TM_AUGUST 7
#define TM_SEPTEMBER  8
#define TM_OCTOBER  9
#define TM_NOVEMBER 10
#define TM_DECEMBER 11

#define isleap(y) ((((y) % 4) == 0 && ((y) % 100) != 0) || ((y) % 400) == 0)
#define isleap_sum(a, b)  isleap((a) % 400 + (b) % 400)


typedef struct
{
  const char *abday[7];
  const char *day[7];
  const char *abmon[12];
  const char *mon[12];
  const char *am_pm[2];
  const char *d_t_fmt;
  const char *d_fmt;
  const char *t_fmt;
  const char *t_fmt_ampm;
} _TimeLocale;

static const _TimeLocale _DefaultTimeLocale =
{
  /* abbreviated day */
  {
    "Sun","Mon","Tue","Wed","Thu","Fri","Sat",
  },
  /* days */
  {
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday",
    "Friday", "Saturday"
  },
  /* abbreviated month */
  {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
  },
  /* month */
  {
    "January", "February", "March", "April", "May", "June", "July",
    "August", "September", "October", "November", "December"
  },
  /* ampm */
  {
    "AM", "PM"
  },
  "%a %b %e %H:%M:%S %Y",
  "%m/%d/%y",
  "%H:%M:%S",
  "%I:%M:%S %p"
};

#define _TIME_LOCALE(loc) \
  (&_DefaultTimeLocale)

static const _u_char *conv_num(const unsigned char *, int *, _uint, _uint);
static const _u_char *find_string(const _u_char *, int *, const char *const *,
                                  const char *const *, int);


/*
 * We do not implement alternate representations. However, we always
 * check whether a given modifier is allowed for a certain conversion.
 */
#define ALT_E     0x01
#define ALT_O     0x02
#define LEGAL_ALT(x)    { if (alt_format & ~(x)) return NULL; }

#define S_YEAR      (1 << 0)
#define S_MON     (1 << 1)
#define S_YDAY      (1 << 2)
#define S_MDAY      (1 << 3)
#define S_WDAY      (1 << 4)
#define S_HOUR      (1 << 5)

#define HAVE_MDAY(s)    (s & S_MDAY)
#define HAVE_MON(s)   (s & S_MON)
#define HAVE_WDAY(s)    (s & S_WDAY)
#define HAVE_YDAY(s)    (s & S_YDAY)
#define HAVE_YEAR(s)    (s & S_YEAR)
#define HAVE_HOUR(s)    (s & S_HOUR)

static char gmt[] = { "GMT" };
static char utc[] = { "UTC" };
/* RFC-822/RFC-2822 */
static const char *const nast[5] =
{
  "EST",    "CST",    "MST",    "PST",    "\0\0\0"
};
static const char *const nadt[5] =
{
  "EDT",    "CDT",    "MDT",    "PDT",    "\0\0\0"
};

/*
 * Table to determine the ordinal date for the start of a month.
 * Ref: http://en.wikipedia.org/wiki/ISO_week_date
 */
static const int start_of_month[2][13] =
{
  /* non-leap year */
  { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365 },
  /* leap year */
  { 0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335, 366 }
};

/*
 * Calculate the week day of the first day of a year. Valid for
 * the Gregorian calendar, which began Sept 14, 1752 in the UK
 * and its colonies. Ref:
 * http://en.wikipedia.org/wiki/Determination_of_the_day_of_the_week
 */

static int
first_wday_of(int yr)
{
  return ((2 * (3 - (yr / 100) % 4)) + (yr % 100) + ((yr % 100) /  4) +
          (isleap(yr) ? 6 : 0) + 1) % 7;
}

/* standard strptime() doesn't support %z / %Z properly on all
 * platforms, especially those that don't have tm_gmtoff/tm_zone in
 * their struct tm. This is a slightly modified NetBSD strptime() with
 * some modifications and explicit zone related parameters. */
char *
strptime_with_tz(const char *buf, const char *fmt, struct tm *tm, long *tm_gmtoff, const char **tm_zone)
{
  unsigned char c;
  const unsigned char *bp, *ep;
  int alt_format, i, split_year = 0, neg = 0, state = 0,
                     day_offset = -1, week_offset = 0, offs;
  const char *new_fmt;

  bp = (const _u_char *)buf;

  while (bp != NULL && (c = *fmt++) != '\0')
    {
      /* Clear `alternate' modifier prior to new conversion. */
      alt_format = 0;
      i = 0;

      /* Eat up white-space. */
      if (isspace(c))
        {
          while (isspace(*bp))
            bp++;
          continue;
        }

      if (c != '%')
        goto literal;


again:
      switch (c = *fmt++)
        {
        case '%': /* "%%" is converted to "%". */
literal:
          if (c != *bp++)
            return NULL;
          LEGAL_ALT(0);
          continue;

        /*
         * "Alternative" modifiers. Just set the appropriate flag
         * and start over again.
         */
        case 'E': /* "%E?" alternative conversion modifier. */
          LEGAL_ALT(0);
          alt_format |= ALT_E;
          goto again;

        case 'O': /* "%O?" alternative conversion modifier. */
          LEGAL_ALT(0);
          alt_format |= ALT_O;
          goto again;

        /*
         * "Complex" conversion rules, implemented through recursion.
         */
        case 'c': /* Date and time, using the locale's format. */
          new_fmt = _TIME_LOCALE(loc)->d_t_fmt;
          state |= S_WDAY | S_MON | S_MDAY | S_YEAR;
          goto recurse;

        case 'D': /* The date as "%m/%d/%y". */
          new_fmt = "%m/%d/%y";
          LEGAL_ALT(0);
          state |= S_MON | S_MDAY | S_YEAR;
          goto recurse;

        case 'F': /* The date as "%Y-%m-%d". */
          new_fmt = "%Y-%m-%d";
          LEGAL_ALT(0);
          state |= S_MON | S_MDAY | S_YEAR;
          goto recurse;

        case 'R': /* The time as "%H:%M". */
          new_fmt = "%H:%M";
          LEGAL_ALT(0);
          goto recurse;

        case 'r': /* The time in 12-hour clock representation. */
          new_fmt = _TIME_LOCALE(loc)->t_fmt_ampm;
          LEGAL_ALT(0);
          goto recurse;

        case 'T': /* The time as "%H:%M:%S". */
          new_fmt = "%H:%M:%S";
          LEGAL_ALT(0);
          goto recurse;

        case 'X': /* The time, using the locale's format. */
          new_fmt = _TIME_LOCALE(loc)->t_fmt;
          goto recurse;

        case 'x': /* The date, using the locale's format. */
          new_fmt = _TIME_LOCALE(loc)->d_fmt;
          state |= S_MON | S_MDAY | S_YEAR;
recurse:
          bp = (const _u_char *)strptime_with_tz((const char *)bp,
                                                 new_fmt, tm, tm_gmtoff, tm_zone);
          LEGAL_ALT(ALT_E);
          continue;

        /*
         * "Elementary" conversion rules.
         */
        case 'A': /* The day of week, using the locale's form. */
        case 'a':
          bp = find_string(bp, &tm->tm_wday,
                           _TIME_LOCALE(loc)->day, _TIME_LOCALE(loc)->abday, 7);
          LEGAL_ALT(0);
          state |= S_WDAY;
          continue;

        case 'B': /* The month, using the locale's form. */
        case 'b':
        case 'h':
          bp = find_string(bp, &tm->tm_mon,
                           _TIME_LOCALE(loc)->mon, _TIME_LOCALE(loc)->abmon,
                           12);
          LEGAL_ALT(0);
          state |= S_MON;
          continue;

        case 'C': /* The century number. */
          i = 20;
          bp = conv_num(bp, &i, 0, 99);

          i = i * 100 - TM_YEAR_BASE;
          if (split_year)
            i += tm->tm_year % 100;
          split_year = 1;
          tm->tm_year = i;
          LEGAL_ALT(ALT_E);
          state |= S_YEAR;
          continue;

        case 'd': /* The day of month. */
        case 'e':
          bp = conv_num(bp, &tm->tm_mday, 1, 31);
          LEGAL_ALT(ALT_O);
          state |= S_MDAY;
          continue;

        case 'k': /* The hour (24-hour clock representation). */
          LEGAL_ALT(0);
        /* FALLTHROUGH */
        case 'H':
          bp = conv_num(bp, &tm->tm_hour, 0, 23);
          LEGAL_ALT(ALT_O);
          state |= S_HOUR;
          continue;

        case 'l': /* The hour (12-hour clock representation). */
          LEGAL_ALT(0);
        /* FALLTHROUGH */
        case 'I':
          bp = conv_num(bp, &tm->tm_hour, 1, 12);
          if (tm->tm_hour == 12)
            tm->tm_hour = 0;
          LEGAL_ALT(ALT_O);
          state |= S_HOUR;
          continue;

        case 'j': /* The day of year. */
          i = 1;
          bp = conv_num(bp, &i, 1, 366);
          tm->tm_yday = i - 1;
          LEGAL_ALT(0);
          state |= S_YDAY;
          continue;

        case 'M': /* The minute. */
          bp = conv_num(bp, &tm->tm_min, 0, 59);
          LEGAL_ALT(ALT_O);
          continue;

        case 'm': /* The month. */
          i = 1;
          bp = conv_num(bp, &i, 1, 12);
          tm->tm_mon = i - 1;
          LEGAL_ALT(ALT_O);
          state |= S_MON;
          continue;

        case 'p': /* The locale's equivalent of AM/PM. */
          bp = find_string(bp, &i, _TIME_LOCALE(loc)->am_pm,
                           NULL, 2);
          if (HAVE_HOUR(state) && tm->tm_hour > 11)
            return NULL;
          tm->tm_hour += i * 12;
          LEGAL_ALT(0);
          continue;

        case 'S': /* The seconds. */
          bp = conv_num(bp, &tm->tm_sec, 0, 61);
          LEGAL_ALT(ALT_O);
          continue;

#ifndef TIME_MAX
#define TIME_MAX  INT64_MAX
#endif
        case 's': /* seconds since the epoch */
        {
          time_t sse = 0;
          uint64_t rulim = TIME_MAX;

          if (*bp < '0' || *bp > '9')
            {
              bp = NULL;
              continue;
            }

          do
            {
              sse *= 10;
              sse += *bp++ - '0';
              rulim /= 10;
            }
          while ((sse * 10 <= TIME_MAX) &&
                 rulim && *bp >= '0' && *bp <= '9');

          if (sse < 0 || (uint64_t)sse > TIME_MAX)
            {
              bp = NULL;
              continue;
            }

          cached_localtime(&sse, tm);
          state |= S_YDAY | S_WDAY |
                   S_MON | S_MDAY | S_YEAR;
        }
        continue;

        case 'U': /* The week of year, beginning on sunday. */
        case 'W': /* The week of year, beginning on monday. */
          /*
           * XXX This is bogus, as we can not assume any valid
           * information present in the tm structure at this
           * point to calculate a real value, so just check the
           * range for now.
           */
          bp = conv_num(bp, &i, 0, 53);
          LEGAL_ALT(ALT_O);
          if (c == 'U')
            day_offset = TM_SUNDAY;
          else
            day_offset = TM_MONDAY;
          week_offset = i;
          continue;

        case 'w': /* The day of week, beginning on sunday. */
          bp = conv_num(bp, &tm->tm_wday, 0, 6);
          LEGAL_ALT(ALT_O);
          state |= S_WDAY;
          continue;

        case 'u': /* The day of week, monday = 1. */
          bp = conv_num(bp, &i, 1, 7);
          tm->tm_wday = i % 7;
          LEGAL_ALT(ALT_O);
          state |= S_WDAY;
          continue;

        case 'g': /* The year corresponding to the ISO week
         * number but without the century.
         */
          bp = conv_num(bp, &i, 0, 99);
          continue;

        case 'G': /* The year corresponding to the ISO week
         * number with century.
         */
          do
            bp++;
          while (isdigit(*bp));
          continue;

        case 'V': /* The ISO 8601:1988 week number as decimal */
          bp = conv_num(bp, &i, 0, 53);
          continue;

        case 'Y': /* The year. */
          i = TM_YEAR_BASE; /* just for data sanity... */
          bp = conv_num(bp, &i, 0, 9999);
          tm->tm_year = i - TM_YEAR_BASE;
          LEGAL_ALT(ALT_E);
          state |= S_YEAR;
          continue;

        case 'y': /* The year within 100 years of the epoch. */
          /* LEGAL_ALT(ALT_E | ALT_O); */
          bp = conv_num(bp, &i, 0, 99);

          if (split_year)
            /* preserve century */
            i += (tm->tm_year / 100) * 100;
          else
            {
              split_year = 1;
              if (i <= 68)
                i = i + 2000 - TM_YEAR_BASE;
              else
                i = i + 1900 - TM_YEAR_BASE;
            }
          tm->tm_year = i;
          state |= S_YEAR;
          continue;

        case 'Z':
          tzset();
          if (strncmp((const char *)bp, gmt, 3) == 0 ||
              strncmp((const char *)bp, utc, 3) == 0)
            {
              tm->tm_isdst = 0;
              *tm_gmtoff = 0;
              *tm_zone = gmt;
              bp += 3;
            }
          else
            {
              ep = find_string(bp, &i, (const char *const *)tzname, NULL, 2);
              if (ep != NULL)
                {
                  tm->tm_isdst = i;
#ifdef SYSLOG_NG_HAVE_TIMEZONE
                  *tm_gmtoff = -(timezone);
#endif
                  *tm_zone = tzname[i];
                }
              bp = ep;
            }
          continue;

        case 'z':
          /*
           * We recognize all ISO 8601 formats:
           * Z  = Zulu time/UTC
           * [+-]hhmm
           * [+-]hh:mm
           * [+-]hh
           * We recognize all RFC-822/RFC-2822 formats:
           * UT|GMT
           *          North American : UTC offsets
           * E[DS]T = Eastern : -4 | -5
           * C[DS]T = Central : -5 | -6
           * M[DS]T = Mountain: -6 | -7
           * P[DS]T = Pacific : -7 | -8
           *          Military
           * [A-IL-M] = -1 ... -9 (J not used)
           * [N-Y]  = +1 ... +12
           */
          while (isspace(*bp))
            bp++;

          switch (*bp++)
            {
            case 'G':
              if (*bp++ != 'M')
                return NULL;
            /*FALLTHROUGH*/
            case 'U':
              if (*bp++ != 'T')
                return NULL;
            /*FALLTHROUGH*/
            case 'Z':
              tm->tm_isdst = 0;
              *tm_gmtoff = 0;
              *tm_zone = utc;
              continue;
            case '+':
              neg = 0;
              break;
            case '-':
              neg = 1;
              break;
            default:
              --bp;
              ep = find_string(bp, &i, nast, NULL, 4);
              if (ep != NULL)
                {
                  *tm_gmtoff = (-5 - i) * 3600;
                  *tm_zone = __UNCONST(nast[i]);
                  bp = ep;
                  continue;
                }
              ep = find_string(bp, &i, nadt, NULL, 4);
              if (ep != NULL)
                {
                  tm->tm_isdst = 1;
                  *tm_gmtoff = (-4 - i) * 3600;
                  *tm_zone = __UNCONST(nadt[i]);
                  bp = ep;
                  continue;
                }

              if ((*bp >= 'A' && *bp <= 'I') ||
                  (*bp >= 'L' && *bp <= 'Y'))
                {
                  /* Argh! No 'J'! */
                  if (*bp >= 'A' && *bp <= 'I')
                    *tm_gmtoff =
                      (('A' - 1) - (int)*bp) * 3600;
                  else if (*bp >= 'L' && *bp <= 'M')
                    *tm_gmtoff = ('A' - (int)*bp) * 3600;
                  else if (*bp >= 'N' && *bp <= 'Y')
                    *tm_gmtoff = ((int)*bp - 'M') * 3600;
                  *tm_zone = utc; /* XXX */
                  bp++;
                  continue;
                }
              return NULL;
            }
          offs = 0;
          for (i = 0; i < 4; )
            {
              if (isdigit(*bp))
                {
                  offs = offs * 10 + (*bp++ - '0');
                  i++;
                  continue;
                }
              if (i == 2 && *bp == ':')
                {
                  bp++;
                  continue;
                }
              break;
            }
          switch (i)
            {
            case 2:
              offs *= 100;
              break;
            case 4:
              i = offs % 100;
              if (i >= 60)
                return NULL;
              /* Convert minutes into decimal */
              offs = (offs / 100) * 100 + (i * 50) / 30;
              break;
            default:
              return NULL;
            }
          if (neg)
            offs = -offs;
          tm->tm_isdst = 0; /* XXX */
          *tm_gmtoff = (offs * 3600) / 100;
          *tm_zone = utc; /* XXX */
          continue;

        /*
         * Miscellaneous conversions.
         */
        case 'n': /* Any kind of white-space. */
        case 't':
          while (isspace(*bp))
            bp++;
          LEGAL_ALT(0);
          continue;


        default:  /* Unknown/unsupported conversion. */
          return NULL;
        }
    }

  if (!HAVE_YDAY(state) && HAVE_YEAR(state))
    {
      if (HAVE_MON(state) && HAVE_MDAY(state))
        {
          /* calculate day of year (ordinal date) */
          tm->tm_yday =  start_of_month[isleap_sum(tm->tm_year,
                                                   TM_YEAR_BASE)][tm->tm_mon] + (tm->tm_mday - 1);
          state |= S_YDAY;
        }
      else if (day_offset != -1)
        {
          /*
           * Set the date to the first Sunday (or Monday)
           * of the specified week of the year.
           */
          if (!HAVE_WDAY(state))
            {
              tm->tm_wday = day_offset;
              state |= S_WDAY;
            }
          tm->tm_yday = (7 -
                         first_wday_of(tm->tm_year + TM_YEAR_BASE) +
                         day_offset) % 7 + (week_offset - 1) * 7 +
                        tm->tm_wday  - day_offset;
          state |= S_YDAY;
        }
    }

  if (HAVE_YDAY(state) && HAVE_YEAR(state))
    {
      int isleap;

      if (!HAVE_MON(state))
        {
          /* calculate month of day of year */
          i = 0;
          isleap = isleap_sum(tm->tm_year, TM_YEAR_BASE);
          while (tm->tm_yday >= start_of_month[isleap][i])
            i++;
          if (i > 12)
            {
              i = 1;
              tm->tm_yday -= start_of_month[isleap][12];
              tm->tm_year++;
            }
          tm->tm_mon = i - 1;
          state |= S_MON;
        }

      if (!HAVE_MDAY(state))
        {
          /* calculate day of month */
          isleap = isleap_sum(tm->tm_year, TM_YEAR_BASE);
          tm->tm_mday = tm->tm_yday -
                        start_of_month[isleap][tm->tm_mon] + 1;
          state |= S_MDAY;
        }

      if (!HAVE_WDAY(state))
        {
          /* calculate day of week */
          i = 0;
          week_offset = first_wday_of(tm->tm_year);
          while (i++ <= tm->tm_yday)
            {
              if (week_offset++ >= 6)
                week_offset = 0;
            }
          tm->tm_wday = week_offset;
          state |= S_WDAY;
        }
    }

  return __UNCONST(bp);
}


static const _u_char *
conv_num(const unsigned char *buf, int *dest, _uint llim, _uint ulim)
{
  _uint result = 0;
  unsigned char ch;

  /* The limit also determines the number of valid digits. */
  _uint rulim = ulim;

  ch = *buf;
  if (ch < '0' || ch > '9')
    return NULL;

  do
    {
      result *= 10;
      result += ch - '0';
      rulim /= 10;
      ch = *++buf;
    }
  while ((result * 10 <= ulim) && rulim && ch >= '0' && ch <= '9');

  if (result < llim || result > ulim)
    return NULL;

  *dest = result;
  return buf;
}

static const _u_char *
find_string(const _u_char *bp, int *tgt, const char *const *n1,
            const char *const *n2, int c)
{
  int i;
  size_t len;

  /* check full name - then abbreviated ones */
  for (; n1 != NULL; n1 = n2, n2 = NULL)
    {
      for (i = 0; i < c; i++, n1++)
        {
          len = strlen(*n1);
          if (strncasecmp(*n1, (const char *)bp, len) == 0)
            {
              *tgt = i;
              return bp + len;
            }
        }
    }

  /* Nothing matched */
  return NULL;
}
