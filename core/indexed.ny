;; Keywords: vec fin length index dependent family total head tail
;; Length-indexed vectors and bounded indices over the kernel's inductive
;; families, written in Nytrix.
;;
;; `Vec<int, n>` is a value-indexed family: its constructors fix the index
;; (`Nil` has length 0, `Cons` adds one), and the functions here carry the
;; length in their types. `vhead`/`vtail` take `Vec<int, n + 1>`, so applying
;; them to an empty vector is a type error, not a runtime check; `vlength`
;; reads the length from the type through an implicit binder. Elements are
;; ints: a dynamic element type does not keep its payload tags through the
;; module boundary yet.
;; References:
;; - etc/docs/spec/kernel.md (value-indexed families, implicit binders)
module core.indexed(Vec, vnil, vcons, vlength, vhead, vtail, vto_list, vsum,
   fin_bound, fin_value)
use core

enum Vec<A, n> {
   Nil where n is 0,
   Cons(A head, Vec<A, k> tail) where n is k + 1
}

;; Constructors: the result's length is computed from the argument's.
fn vnil() Vec<int, 0> { Vec.Nil }

fn vcons{int n}(int h, Vec<int, n> t) Vec<int, n + 1> {
   "Prepends `h` to `t`; the result is one longer."
   Vec.Cons(h, t)
}

;; The length is a static index, filled in at the call.
fn vlength{int n}(Vec<int, n> xs) int { n }

;; Total on non-empty vectors; `Nil` cannot reach these matches.
fn vhead{int n}(Vec<int, n + 1> xs) int {
   match xs {
      Vec.Cons(h, t) -> h
   }
}

fn vtail{int n}(Vec<int, n + 1> xs) Vec<int, n> {
   "Returns everything after the first element of a non-empty vector."
   match xs {
      Vec.Cons(h, t) -> t
   }
}

;; Elements in order, as a plain list. (Each traversal keeps its typed
;; scrutinee: a `Vec` passed through an `any` helper loses its payload tags.)
fn vto_list{int n}(Vec<int, n> xs) list {
   mut out = []
   mut cur = xs
   mut go = true
   while go {
      match cur {
         Vec.Nil -> { go = false }
         Vec.Cons(h, t) -> {
            ;; `h + 0` stores a fresh int: the payload word itself is
            ;; appended untagged inside a stdlib module.
            out = out.append(h + 0)
            cur = t
         }
      }
   }
   out
}

fn vsum{int n}(Vec<int, n> xs) int {
   "Returns the sum of the elements."
   mut total = 0
   mut cur = xs
   mut go = true
   while go {
      match cur {
         Vec.Nil -> { go = false }
         Vec.Cons(h, t) -> {
            total = total + h
            cur = t
         }
      }
   }
   total
}

;; A bounded index knows its bound statically.
fn fin_bound{int n}(Fin<n> i) int { n }

fn fin_value{int n}(Fin<n> i) int {
   "Returns the index as a plain int."
   i
}
