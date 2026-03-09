;; Keywords: error
;; Error Handling and Panic Utilities for Nytrix
module std.core.error(panic, ok, err, is_ok, is_err, unwrap, unwrap_or, format_backtrace, ERR, ERR_ASSERT, ERR_ATTR, ERR_TYPE, ERR_VALUE, ERR_NAME, ERR_RUNTIME, ERR_NOT_IMPL, ERR_ARITH, ERR_DIV_ZERO, ERR_OVERFLOW, ERR_LOOKUP, ERR_INDEX, ERR_KEY, ERR_SYNTAX, ERR_IMPORT, ERR_MODULE, ERR_IO, ERR_NOT_FOUND, ERR_PERMISSION, ERR_TIMEOUT, ERR_EOF, ERR_INTERRUPT, WARN, WARN_DEPRECATED, WARN_SYNTAX, WARN_RUNTIME, WARN_FUTURE, WARN_IMPORT, WARN_RESOURCE, exception, warning, error_kind, error_message, is_error, raise_error)
use std.core

def ERR = "err"
def ERR_ASSERT = "err.assert"
def ERR_ATTR = "err.attr"
def ERR_TYPE = "err.type"
def ERR_VALUE = "err.value"
def ERR_NAME = "err.name"
def ERR_RUNTIME = "err.runtime"
def ERR_NOT_IMPL = "err.not_impl"
def ERR_ARITH = "err.arith"
def ERR_DIV_ZERO = "err.div_zero"
def ERR_OVERFLOW = "err.overflow"
def ERR_LOOKUP = "err.lookup"
def ERR_INDEX = "err.index"
def ERR_KEY = "err.key"
def ERR_SYNTAX = "err.syntax"
def ERR_IMPORT = "err.import"
def ERR_MODULE = "err.module"
def ERR_IO = "err.io"
def ERR_NOT_FOUND = "err.not_found"
def ERR_PERMISSION = "err.permission"
def ERR_TIMEOUT = "err.timeout"
def ERR_EOF = "err.eof"
def ERR_INTERRUPT = "err.interrupt"
def WARN = "warn"
def WARN_DEPRECATED = "warn.deprecated"
def WARN_SYNTAX = "warn.syntax"
def WARN_RUNTIME = "warn.runtime"
def WARN_FUTURE = "warn.future"
def WARN_IMPORT = "warn.import"
def WARN_RESOURCE = "warn.resource"

@returns_owned
fn exception(str: kind=ERR, str: message="", any: data=0): dict {
   "Creates a structured exception payload without changing panic/catch semantics."
   mut e = {"kind": kind, "message": message}
   if(data != 0){ e["data"] = data }
   return e
}

@returns_owned
fn warning(str: kind=WARN, str: message="", any: data=0): dict {
   "Creates a structured warning payload."
   mut w = {"kind": kind, "message": message}
   if(data != 0){ w["data"] = data }
   return w
}

fn error_kind(any: e): str {
   "Returns the symbolic error kind for structured errors, or ERR for raw panic payloads."
   if(is_dict(e)){ return e.get("kind", ERR) }
   return ERR
}

fn error_message(any: e): str {
   "Returns the message string for structured errors, or the string form of a raw payload."
   if(is_dict(e)){ return __to_str(e.get("message", "")) }
   return __to_str(e)
}

fn is_error(any: e, str: kind): bool {
   "Returns true when a structured error has the requested symbolic kind."
   return error_kind(e) == kind
}

fn raise_error(str: kind=ERR, str: message="", any: data=0): any {
   "Panics with a structured exception payload."
   panic(exception(kind, message, data))
}

@returns_owned
fn format_backtrace(seq: entries): str {
   "Returns a formatted string of the backtrace entries(list of [file, line, col, fn])."
   mut out = ""
   for f in entries {
      def file = f.get(0)
      def line = f.get(1)
      def col = f.get(2)
      def f_name = f.get(3)
      out = f"{out}  at {file}:{line}:{col} (fn {f_name})\n"
   }
   out
}

fn panic(any: msg): any {
   "Raises a panic: jumps to the nearest surrounding catch handler  if none, prints the message to stderr and exits."
   return __panic(msg)
}

fn ok(any: v): any {
   "Creates an **Ok** result."
   return __result_ok(v)
}

fn err(any: e): any {
   "Creates an **Err** result."
   return __result_err(e)
}

fn is_ok(any: v): bool {
   "Returns **true** if `v` is an **Ok** result."
   return __is_ok(v)
}

fn is_err(any: v): bool {
   "Returns **true** if `v` is an **Err** result."
   return __is_err(v)
}

fn unwrap(any: v): any {
   "Unwraps a Result or returns the value. Panics if **Err**."
   if(is_err(v)){ panic("unwrapped an Err: " + __to_str(__unwrap(v))) }
   return __unwrap(v)
}

fn unwrap_or(any: v, any: default): any {
   "Unwraps a Result or returns the default value."
   if(is_ok(v)){ return __unwrap(v) }
   return default
}
