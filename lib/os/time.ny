;; Keywords: time datetime timestamps
;; Os Time for Nytrix
module std.os.time(time, now, unix, now_ms, sleep, msleep, ticks, monotonic_ns, Instant, instant, since_ns, since_ms, Timer, timer, timer_start, elapsed_ns, elapsed_ms, elapsed_sec, reset, _is_leap, _days_in_month, _days_in_year, _pad2, _pad4, format, format_time)
use std.core
use std.os.sys
use std.core.str

fn _is_leap(int: y): int {
   ((y % 4) == 0 && ((y % 100) != 0 || (y % 400) == 0)) ? 1 : 0
}

fn _days_in_month(int: y, int: m): int {
   case m {
      1, 3, 5, 7, 8, 10, 12 -> 31
      2 -> 28 + _is_leap(y)
      _ -> 30
   }
}

fn _days_in_year(int: y): int { 365 + _is_leap(y) }

fn _pad2(int: n): str { pad_start(to_str(n), 2, "0") }

fn _pad4(int: n): str { pad_start(to_str(n), 4, "0") }

fn format_time(int: ts): str {
   "Format Unix seconds as `YYYY-MM-DD HH:MM:SS` in UTC."
   if(ts < 0){ ts = 0 }
   mut days = ts / 86400
   mut rem = ts - days * 86400
   def hour = rem / 3600
   rem = rem - hour * 3600
   def minute = rem / 60
   def second = rem - minute * 60
   mut year = 1970
   while(1){
      def diy = _days_in_year(year)
      if(days < diy){ break }
      days = days - diy
      year += 1
   }
   mut month = 1
   while(1){
      def dim = _days_in_month(year, month)
      if(days < dim){ break }
      days = days - dim
      month += 1
   }
   def day = days + 1
   f"{_pad4(year)}-{_pad2(month)}-{_pad2(day)} {_pad2(hour)}:{_pad2(minute)}:{_pad2(second)}"
}

fn time(): int {
   "Returns the current Unix timestamp(seconds since epoch) using `clock_gettime(CLOCK_REALTIME)`."
   __time_seconds()
}

fn msleep(int: ms): any {
   "Suspends execution of the current thread for `ms` milliseconds."
   __msleep_ms(ms)
}

fn sleep(int: s): any {
   "Suspends execution of the current thread for `s` seconds."
   msleep(s * 1000)
}

fn ticks(): int {
   "Returns a high-resolution monotonic tick count in nanoseconds. Useful for precise timing and benchmarking."
   __ticks_ns()
}

fn now(): int {
   "Returns the current Unix timestamp in seconds."
   time()
}

fn unix(): int {
   "Alias for now()."
   now()
}

fn now_ms(): int {
   "Returns the current Unix timestamp in milliseconds."
   __time_milliseconds()
}

fn monotonic_ns(): int {
   "Alias for ticks()."
   ticks()
}

fn Instant(): int {
   "Returns a monotonic instant token."
   ticks()
}

fn instant(): int { Instant() }

fn since_ns(int: start): int {
   "Returns monotonic nanoseconds elapsed since start."
   ticks() - start
}

fn since_ms(int: start): int {
   "Returns monotonic milliseconds elapsed since start."
   since_ns(start) / 1000000
}

fn Timer(): dict {
   "Creates a restartable timer object."
   {"start_ns": ticks()}
}

fn timer(): dict { Timer() }

fn timer_start(dict: t): int {
   "Returns a timer object's start instant."
   t.get("start_ns", ticks())
}

fn elapsed_ns(dict: t): int {
   "Returns nanoseconds elapsed for a timer object."
   ticks() - timer_start(t)
}

fn elapsed_ms(dict: t): int {
   "Returns milliseconds elapsed for a timer object."
   elapsed_ns(t) / 1000000
}

fn elapsed_sec(dict: t): int {
   "Returns seconds elapsed for a timer object."
   elapsed_ns(t) / 1000000000
}

fn reset(dict: t): dict {
   "Resets and returns a timer object."
   t.set("start_ns", ticks())
}

fn format(any: ts): str {
   "Alias for format_time(ts)."
   format_time(ts)
}
