;; Keywords: query sql filter select group aggregate dsl
;; Pure-Ny SQL-like query DSL for lists of dicts.
;; Demonstrates the language as a Racket-like DSL host.
;;
;; Usage:
;;   use std.core.query
;;   def rows = [{"name": "Alice", "age": 30}, {"name": "Bob", "age": 25}]
;;   def result = run(pipe(pipe(from(rows), where(_filter)), select(["name"])))
;;
;; References:
;;   - std.core
module std.core.query(from, where, select, select_as, order_by, limit, offset,
   group_by, count_agg, sum_agg, avg_agg, min_agg, max_agg, having,
   join, run, collect, pipe)
use std.core

fn from(any rows) dict {
   "Start a query from a list of dicts (or any sequence)."
   {"_type": "query", "_rows": rows, "_steps": []}
}

fn _query_add_step(dict q, any step) dict {
   "Internal: append a step to the pipeline."
   assert(q.get("_type") == "query", "expected a query object")
   mut steps = q.get("_steps")
   steps = steps.append(step)
   q.set("_steps", steps)
}

fn where(fnptr pred) dict {
   "Filter: keep only rows where pred(row) is truthy."
   {"_step": "where", "pred": pred}
}

fn select(any cols) dict {
   "Project: keep only the specified columns."
   {"_step": "select", "cols": cols}
}

fn select_as(dict mapping) dict {
   "Project and rename: {old_name: new_name}."
   {"_step": "select_as", "mapping": mapping}
}

fn order_by(str col, bool asc=true) dict {
   "Sort by column. asc=true for ascending."
   {"_step": "order_by", "col": col, "asc": asc}
}

fn limit(int n) dict {
   "Take at most n rows."
   {"_step": "limit", "n": n}
}

fn offset(int n) dict {
   "Skip the first n rows."
   {"_step": "offset", "n": n}
}

fn group_by(str col) dict {
   "Group rows by column value. Returns a dict of key->list of rows."
   {"_step": "group_by", "col": col}
}

fn count_agg() dict {
   {"_step": "count_agg"}
}

fn sum_agg(str col) dict {
   {"_step": "sum_agg", "col": col}
}

fn avg_agg(str col) dict {
   {"_step": "avg_agg", "col": col}
}

fn min_agg(str col) dict {
   {"_step": "min_agg", "col": col}
}

fn max_agg(str col) dict {
   {"_step": "max_agg", "col": col}
}

fn having(fnptr pred) dict {
   "Post-group filter: keep groups where pred(key, rows) is truthy."
   {"_step": "having", "pred": pred}
}

fn join(any other_rows, str on_left, str on_right) dict {
   "Inner join with another row set on key equality."
   {"_step": "join", "other": other_rows, "on_left": on_left, "on_right": on_right}
}

fn pipe(dict q, dict step) dict {
   "Add a step to the query pipeline."
   _query_add_step(q, step)
}

fn _apply_step(any rows, dict step) any {
   def kind = step.get("_step", "")
   if kind == "where" {
      def pred = step.get("pred")
      mut out = []
      mut i = 0
      while i < len(rows) {
         if pred(rows.get(i)) {
            out = out.append(rows.get(i))
         }
         i = i + 1
      }
      return out
   }
   if kind == "select" {
      def cols = step.get("cols")
      mut out = []
      mut i = 0
      while i < len(rows) {
         def row = rows.get(i)
         mut r = dict()
         mut j = 0
         while j < len(cols) {
            def c = cols.get(j)
            if dict_has(row, c) {
               r = r.set(c, row.get(c))
            }
            j = j + 1
         }
         out = out.append(r)
         i = i + 1
      }
      return out
   }
   if kind == "select_as" {
      def mapping = step.get("mapping")
      def ks = keys(mapping)
      mut out = []
      mut i = 0
      while i < len(rows) {
         def row = rows.get(i)
         mut r = dict()
         mut j = 0
         while j < len(ks) {
            def old_k = ks.get(j)
            def new_k = mapping.get(old_k)
            if dict_has(row, old_k) {
               r = r.set(new_k, row.get(old_k))
            }
            j = j + 1
         }
         out = out.append(r)
         i = i + 1
      }
      return out
   }
   if kind == "limit" {
      def n = step.get("n")
      mut out = []
      mut i = 0
      while i < n && i < len(rows) {
         out = out.append(rows.get(i))
         i = i + 1
      }
      return out
   }
   if kind == "offset" {
      def n = step.get("n")
      mut out = []
      mut i = n
      while i < len(rows) {
         out = out.append(rows.get(i))
         i = i + 1
      }
      return out
   }
   if kind == "group_by" {
      def col = step.get("col")
      mut groups = dict()
      mut i = 0
      while i < len(rows) {
         def row = rows.get(i)
         def key = row.get(col, "")
         if dict_has(groups, key) {
            groups.set(key, groups.get(key).append(row))
         } else {
            groups.set(key, [row])
         }
         i = i + 1
      }
      return groups
   }
   if kind == "count_agg" {
      if is_dict(rows) {
         mut out = dict()
         def ks = keys(rows)
         mut i = 0
         while i < len(ks) {
            def k = ks.get(i)
            out.set(k, len(rows.get(k)))
            i = i + 1
         }
         return out
      }
      return len(rows)
   }
   if kind == "sum_agg" {
      def col = step.get("col")
      if is_dict(rows) {
         mut out = dict()
         def ks = keys(rows)
         mut i = 0
         while i < len(ks) {
            def k = ks.get(i)
            def grp = rows.get(k)
            mut s = 0
            mut j = 0
            while j < len(grp) {
               s = s + grp.get(j).get(col, 0)
               j = j + 1
            }
            out.set(k, s)
            i = i + 1
         }
         return out
      }
      mut s = 0
      mut i = 0
      while i < len(rows) {
         s = s + rows.get(i).get(col, 0)
         i = i + 1
      }
      return s
   }
   if kind == "join" {
      def other = step.get("other")
      def on_left = step.get("on_left")
      def on_right = step.get("on_right")
      mut out = []
      mut i = 0
      while i < len(rows) {
         def row = rows.get(i)
         def kv = row.get(on_left, nil)
         mut j = 0
         while j < len(other) {
            def other_row = other.get(j)
            if eq(other_row.get(on_right, nil), kv) {
               def oks = keys(other_row)
               mut merged = row
               mut k = 0
               while k < len(oks) {
                  def ok = oks.get(k)
                  if !dict_has(merged, ok) {
                     merged = merged.set(ok, other_row.get(ok))
                  }
                  k = k + 1
               }
               out = out.append(merged)
            }
            j = j + 1
         }
         i = i + 1
      }
      return out
   }
   rows
}

fn run(dict q) any {
   "Execute a query and return the result."
   assert(q.get("_type") == "query", "run: expected a query object")
   mut rows = q.get("_rows")
   def steps = q.get("_steps")
   mut i = 0
   while i < len(steps) {
      rows = _apply_step(rows, steps.get(i))
      i = i + 1
   }
   rows
}

fn collect(dict q) list {
   "Alias for run() returning a list."
   def result = run(q)
   if is_list(result) { return result }
   [result]
}

fn _filter_salary_90k(any r) bool {
   r.get("salary") > 90000
}

#main {
   def rows = [
      {"name": "Alice",   "dept": "eng",  "salary": 120000},
      {"name": "Bob",     "dept": "eng",  "salary": 95000},
      {"name": "Carol",   "dept": "mkt",  "salary": 85000},
      {"name": "Dave",    "dept": "mkt",  "salary": 75000},
      {"name": "Eve",     "dept": "eng",  "salary": 110000}
   ]

   def q1 = from(rows)
   def q1b = pipe(q1, where(_filter_salary_90k))
   def q1c = pipe(q1b, select(["name", "salary"]))
   def result1 = run(q1c)
   assert_eq(len(result1), 3, "filter: 3 employees with salary > 90k")

   def q3 = pipe(pipe(from(rows), group_by("dept")), count_agg())
   def counts = run(q3)
   assert_eq(counts.get("eng"), 3, "eng dept has 3")
   assert_eq(counts.get("mkt"), 2, "mkt dept has 2")

   def q4 = pipe(pipe(from(rows), group_by("dept")), sum_agg("salary"))
   def sums = run(q4)
   assert_eq(sums.get("eng"), 325000, "eng total salary")

   def depts = [{"dept": "eng", "head": "Frank"}, {"dept": "mkt", "head": "Grace"}]
   def q5 = pipe(pipe(from(rows), join(depts, "dept", "dept")), select(["name", "head"]))
   def joined = run(q5)
   assert_eq(len(joined), 5, "join: all 5 rows matched")
   assert(joined.get(0).get("head") != nil, "join: head column present")

   print("✓ std.core.query self-test passed")
}
