;; Keywords: math timefmt
;; Math Timefmt module.

use std.core
use std.strings.str

module std.math.timefmt (
   format_time, gmtime
)

fn is_leap(y) {
   (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0)
}

fn days_in_year(y) {
   if (is_leap(y)) { 366 }
   else { 365 }
}

fn pad_z(v, n) {
   def s = to_str(v)
   while (len(s) < n) {
      s = f"0{s}"
   }
   return s
}

fn gmtime(ts) {
   "Breaks down Unix timestamp `ts` into its UTC components. Returns a dictionary with 'year', 'month', 'day', 'hour', 'min', 'sec'."
   def res = dict(16)
   def seconds = ts

   def sec = seconds % 60
   def minutes = seconds / 60
   def m = minutes % 60
   def hours = minutes / 60
   def h = hours % 24
   def days = hours / 24

   def year = 1970
   while (days >= days_in_year(year)) {
      days = days - days_in_year(year)
      year = year + 1
   }

   def month = 1
   def mdays = [31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31]
   if (is_leap(year)) { set_idx(mdays, 1, 29) }

   while (days >= get(mdays, month - 1)) {
      days = days - get(mdays, month - 1)
      month = month + 1
   }

   dict_set(res, "year", year)
   dict_set(res, "month", month)
   dict_set(res, "day", days + 1)
   dict_set(res, "hour", h)
   dict_set(res, "min", m)
   dict_set(res, "sec", sec)
   return res
}

fn format_time(ts) {
   "Formats Unix timestamp `ts` into a UTC string (YYYY-MM-DD HH:MM:SS) using pure Nytrix logic."
   def t = gmtime(ts)
   def y = get(t, "year")
   def mo = get(t, "month")
   def d = get(t, "day")
   def h = get(t, "hour")
   def mi = get(t, "min")
   def s = get(t, "sec")

   return f"{pad_z(y, 4)}-{pad_z(mo, 2)}-{pad_z(d, 2)} {pad_z(h, 2)}:{pad_z(mi, 2)}:{pad_z(s, 2)}"
}
