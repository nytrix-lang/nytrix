;; Keywords: math stats statistics mean median mode variance stddev covariance correlation
;; Pure-Ny statistical methods and statistical data analysis.
;; Demonstrates native speed and elegant expression for numeric algorithms.
;; References:
;; - std.math
module std.math.stat(mean, median, mode, variance, stddev, cov, corr, quantile, zscore, describe)
use std.core
use std.math.float (float, sqrt)

fn mean(seq xs) f64 {
   "Returns the arithmetic mean of a sequence."
   def n = len(xs)
   if n == 0 { return 0.0 }
   mut s = 0.0
   mut i = 0
   while i < n {
      s = s + float(xs.get(i))
      i = i + 1
   }
   s / float(n)
}

fn median(seq xs) f64 {
   "Returns the sample median of a numerical sequence."
   def n = len(xs)
   if n == 0 { return 0.0 }
   mut sorted_xs = sorted(xs)
   def mid = n / 2
   if n % 2 == 1 {
      return float(sorted_xs.get(mid))
   }
   (float(sorted_xs.get(mid - 1)) + float(sorted_xs.get(mid))) / 2.0
}

fn mode(seq xs) any {
   "Returns the most frequent element in a sequence."
   def n = len(xs)
   if n == 0 { return nil }
   mut counts = dict()
   mut i = 0
   mut max_cnt = 0
   mut best_val = xs.get(0)
   while i < n {
      def v = xs.get(i)
      def cnt = dict_get(counts, v, 0) + 1
      counts.set(v, cnt)
      if cnt > max_cnt {
         max_cnt = cnt
         best_val = v
      }
      i = i + 1
   }
   best_val
}

fn variance(seq xs, bool sample=true) f64 {
   "Returns the variance of a sequence. sample=true computes sample variance(N-1)."
   def n = len(xs)
   if n <= 1 { return 0.0 }
   def m = mean(xs)
   mut ss = 0.0
   mut i = 0
   while i < n {
      def diff = float(xs.get(i)) - m
      ss = ss + (diff * diff)
      i = i + 1
   }
   def denom = sample ? float(n - 1) : float(n)
   ss / denom
}

fn stddev(seq xs, bool sample=true) f64 {
   "Returns the standard deviation of a sequence."
   sqrt(variance(xs, sample))
}

fn cov(seq xs, seq ys, bool sample=true) f64 {
   "Returns the covariance between two equal-length sequences."
   def n = len(xs)
   assert(n == len(ys), "cov expects sequences of equal length")
   if n <= 1 { return 0.0 }
   def mx = mean(xs)
   def my = mean(ys)
   mut s = 0.0
   mut i = 0
   while i < n {
      s = s + ((float(xs.get(i)) - mx) * (float(ys.get(i)) - my))
      i = i + 1
   }
   def denom = sample ? float(n - 1) : float(n)
   s / denom
}

fn corr(seq xs, seq ys) f64 {
   "Returns the Pearson correlation coefficient between two sequences."
   def sx = stddev(xs, true)
   def sy = stddev(ys, true)
   if sx == 0.0 || sy == 0.0 { return 0.0 }
   cov(xs, ys, true) / (sx * sy)
}

fn quantile(seq xs, f64 q) f64 {
   "Returns the q-th quantile(0.0 to 1.0) using linear interpolation."
   def n = len(xs)
   if n == 0 { return 0.0 }
   if n == 1 { return float(xs.get(0)) }
   mut s = sorted(xs)
   def pos = q * float(n - 1)
   def idx = __flt_floor(pos)
   def frac = pos - float(idx)
   if idx >= n - 1 { return float(s.get(n - 1)) }
   def v0 = float(s.get(idx))
   def v1 = float(s.get(idx + 1))
   v0 + frac * (v1 - v0)
}

fn zscore(seq xs) list {
   "Standardize sequence elements to z-scores(val - mean)/stddev."
   def m = mean(xs)
   def sd = stddev(xs, true)
   def n = len(xs)
   mut out = []
   mut i = 0
   while i < n {
      def z = sd == 0.0 ? 0.0 : (float(xs.get(i)) - m) / sd
      out = out.append(z)
      i = i + 1
   }
   out
}

fn describe(seq xs) dict {
   "Summary statistics dictionary for a numerical sequence."
   def n = len(xs)
   if n == 0 {
      return {"count": 0, "mean": 0.0, "std": 0.0, "min": nil, "q25": 0.0, "median": 0.0, "q75": 0.0, "max": nil}
   }
   mut s = sorted(xs)
   mut stats = dict()
   stats.set("count", n)
   stats.set("mean", mean(xs))
   stats.set("std", stddev(xs, true))
   stats.set("min", s.get(0))
   stats.set("q25", quantile(s, 0.25))
   stats.set("median", median(s))
   stats.set("q75", quantile(s, 0.75))
   stats.set("max", s.get(n - 1))
   stats
}

#main {
   def data = [10, 20, 30, 40, 50]
   assert_eq(mean(data), 30.0, "mean of [10..50]")
   assert_eq(median(data), 30.0, "median of [10..50]")
   assert_eq(variance(data, true), 250.0, "sample variance of [10..50]")
   assert(stddev(data, true) > 15.8 && stddev(data, true) < 15.9, "stddev of [10..50]")
   def even_data = [1, 2, 3, 4]
   assert_eq(median(even_data), 2.5, "median of even length sequence")
   def modes = [1, 2, 2, 3, 3, 3, 4]
   assert_eq(mode(modes), 3, "mode of sequence")
   def x = [1, 2, 3, 4, 5]
   def y = [2, 4, 6, 8, 10]
   assert(corr(x, y) > 0.999, "perfect positive linear correlation")
   def stats = describe(data)
   assert_eq(stats.get("count"), 5, "describe count")
   assert_eq(stats.get("mean"), 30.0, "describe mean")
   print("✓ std.math.stat self-test passed")
}
