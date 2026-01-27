;; Keywords: strings timefmt
;; Strings Timefmt module.

use std.core *
use std.math *
use std.str *
module std.math.timefmt (
   _is_leap, _days_in_month, _days_in_year, _pad2, _pad4, format_time
)

fn _is_leap(y){
   "Return true if year y is a leap year."
   if((y % 4) != 0){ return 0 }
   if((y % 100) != 0){ return 1 }
   if((y % 400) != 0){ return 0 }
   return 1
}

fn _days_in_month(y, m){
   "Internal: days in month m for year y."
   if(m==1 || m==3 || m==5 || m==7 || m==8 || m==10 || m==12){ return 31  }
   if(m==4 || m==6 || m==9 || m==11){ return 30  }
   if(m==2){ return 28 + _is_leap(y)  }
   return 30
}

fn _days_in_year(y){
   "Internal: days in year y."
   return 365 + _is_leap(y)
}

fn _pad2(n){
   "Internal: zero-pad integer to 2 digits."
   return pad_start(to_str(n), 2, "0")
}

fn _pad4(n){
   "Internal: zero-pad integer to 4 digits."
   return pad_start(to_str(n), 4, "0")
}

fn format_time(ts){
   "Format unix seconds to YYYY-MM-DD HH:MM:SS (UTC)."
   if(ts < 0){ ts = 0  }
   mut days = ts / 86400
   mut rem = ts - days*86400
   def hour = rem / 3600
   rem = rem - hour*3600
   def minute = rem / 60
   def second = rem - minute*60
   mut year = 1970
   while(days >= _days_in_year(year)){
      days = days - _days_in_year(year)
      year = year + 1
   }
   mut month = 1
   while(days >= _days_in_month(year, month)){
      days = days - _days_in_month(year, month)
      month = month + 1
   }
   def day = days + 1
   return f"{_pad4(year)}-{_pad2(month)}-{_pad2(day)} {_pad2(hour)}:{_pad2(minute)}:{_pad2(second)}"
}
