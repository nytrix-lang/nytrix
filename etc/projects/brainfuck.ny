#!/usr/bin/env ny
;; Keywords: compiler brainfuck nyr
;; Brainfuck frontend built directly on the public Nyr API.
use core
use core.str as str
use core.os (file_read, file_remove, temp_dir, pid, exit)
use core.os.cli as cli

#include <nyr/nyr.h>
#link "../../build/release/libnytrixrt.so"

extern "../../build/release/libnytrixrt.so" {
  fn api_func_create() ptr as "nyr_func_create"
  fn api_func_free(ptr func) as "nyr_func_free"
  fn api_emit_const_i64(ptr func, int value, ptr value_id, ptr err, int err_len) bool as "nyr_emit_const_i64"
  fn api_emit_binary(ptr func, int op, int lhs, int rhs, int scalar_class, ptr value_id, ptr err, int err_len) bool as "nyr_emit_binary"
  fn api_emit_load_local(ptr func, int slot, ptr value_id, ptr err, int err_len) bool as "nyr_emit_load_local"
  fn api_emit_store_local(ptr func, int slot, int value, ptr err, int err_len) bool as "nyr_emit_store_local"
  fn api_emit_load_i64(ptr func, int address, int offset, ptr value_id, ptr err, int err_len) bool as "nyr_emit_load_i64"
  fn api_emit_store_i64(ptr func, int address, int value, int offset, ptr err, int err_len) bool as "nyr_emit_store_i64"
  fn api_emit_extern_call(ptr func, str symbol, ptr args, int argc, int scalar_class, ptr value_id, ptr err, int err_len) bool as "nyr_emit_extern_call"
  fn api_emit_compare_i64(ptr func, int lhs, int rhs, int cmp, ptr value_id, ptr err, int err_len) bool as "nyr_emit_compare_i64"
  fn api_emit_label(ptr func, int label, ptr err, int err_len) bool as "nyr_emit_label"
  fn api_emit_branch(ptr func, int label, ptr err, int err_len) bool as "nyr_emit_branch"
  fn api_emit_branch_if(ptr func, int condition, int label, ptr err, int err_len) bool as "nyr_emit_branch_if"
  fn api_emit_return(ptr func, int value, ptr err, int err_len) bool as "nyr_emit_return"
  fn api_verify(ptr func, ptr err, int err_len) bool as "nyr_verify"
  fn api_optimize(ptr func, int opt_level, ptr err, int err_len) bool as "nyr_optimize"
  fn api_emit_assembly(ptr func, str path, str symbol, ptr err, int err_len) bool as "nyr_emit_assembly"
  fn api_jit_run(ptr func, ptr result, ptr err, int err_len) bool as "nyr_jit_run"
}

def TAPE_SIZE = 30000
def BASE_LOCAL = 0
def POINTER_LOCAL = 1
def BOUNDS_LABEL = 9000000000000000
def ALLOC_LABEL = 9000000000000001
def IO_LABEL_BASE = 8000000000000000
;; Ny currently cannot lower imported C enum values through extern calls.
def N_ADD_I64 = 4
def N_MUL_I64 = 6
def N_AND_I64 = 9
def N_OR_I64 = 10
def N_CMP_NE = 1
def N_CMP_LT = 2
def N_CMP_GE = 5
def N_SCALAR_I64_RAW = 1
def N_SCALAR_POINTER = 8
def N_OPT_O2 = 2

mut ptr API_ERROR = 0

fn _check_api(bool ok, str operation) {
  if ok { return }
  def detail = API_ERROR ? str.cstr_to_str(API_ERROR) : ""
  eprint("brainfuck: " + operation +
         (detail.len > 0 ? ": " + detail : ""))
  exit(1)
}

fn _emit_const(ptr func, ptr value_id, int value) int {
  store32(value_id, -1, 0)
  _check_api(api_emit_const_i64(func, value, value_id, API_ERROR, 512),
             "emit Nyr constant")
  load32(value_id, 0)
}

fn _emit_binary(ptr func, ptr value_id, int op, int lhs, int rhs, int scalar_class) int {
  store32(value_id, -1, 0)
  _check_api(api_emit_binary(func, op, lhs, rhs, scalar_class, value_id,
                             API_ERROR, 512),
             "emit Nyr binary operation")
  load32(value_id, 0)
}

fn _emit_compare(ptr func, ptr value_id, int lhs, int rhs, int cmp) int {
  store32(value_id, -1, 0)
  _check_api(api_emit_compare_i64(func, lhs, rhs, cmp, value_id, API_ERROR,
                                  512),
             "emit Nyr comparison")
  load32(value_id, 0)
}

fn _emit_load_local(ptr func, ptr value_id, int slot) int {
  store32(value_id, -1, 0)
  _check_api(api_emit_load_local(func, slot, value_id, API_ERROR, 512),
             "load Nyr local")
  load32(value_id, 0)
}

fn _emit_call(ptr func, ptr value_id, str symbol, ptr args, int argc, int scalar_class) int {
  store32(value_id, -1, 0)
  _check_api(api_emit_extern_call(func, symbol, args, argc, scalar_class,
                                  value_id, API_ERROR, 512),
             "emit Nyr external call")
  load32(value_id, 0)
}

fn _cell_address(ptr func, ptr value_id) int {
  def base = _emit_load_local(func, value_id, BASE_LOCAL)
  def pointer = _emit_load_local(func, value_id, POINTER_LOCAL)
  def stride = _emit_const(func, value_id, 8)
  def offset = _emit_binary(func, value_id, N_MUL_I64, pointer, stride, N_SCALAR_I64_RAW)
  _emit_binary(func, value_id, N_ADD_I64, base, offset, N_SCALAR_POINTER)
}

fn _load_cell(ptr func, ptr value_id) int {
  def address = _cell_address(func, value_id)
  store32(value_id, -1, 0)
  _check_api(api_emit_load_i64(func, address, 0, value_id, API_ERROR, 512),
             "load Nyr memory")
  load32(value_id, 0)
}

fn _store_cell(ptr func, ptr value_id, int value) {
  def address = _cell_address(func, value_id)
  _check_api(api_emit_store_i64(func, address, value, 0, API_ERROR, 512),
             "store Nyr memory")
}

fn _emit_label(ptr func, int label) {
  _check_api(api_emit_label(func, label, API_ERROR, 512), "emit Nyr label")
}

fn _emit_branch(ptr func, int label) {
  _check_api(api_emit_branch(func, label, API_ERROR, 512), "emit Nyr branch")
}

fn _emit_branch_if(ptr func, int condition, int label) {
  _check_api(api_emit_branch_if(func, condition, label, API_ERROR, 512),
             "emit conditional Nyr branch")
}

fn _emit_return(ptr func, ptr value_id, int value) {
  _check_api(api_emit_return(func, value, API_ERROR, 512), "emit Nyr return")
}

fn _adjust_cell(ptr func, ptr value_id, int delta) {
  def amount = (delta % 256 + 256) % 256
  if amount == 0 { return }
  def current = _load_cell(func, value_id)
  def change = _emit_const(func, value_id, amount)
  def sum = _emit_binary(func, value_id, N_ADD_I64, current, change, N_SCALAR_I64_RAW)
  def mask = _emit_const(func, value_id, 255)
  def wrapped = _emit_binary(func, value_id, N_AND_I64, sum, mask, N_SCALAR_I64_RAW)
  _store_cell(func, value_id, wrapped)
}

fn _move_pointer(ptr func, ptr value_id, int delta) {
  if delta == 0 { return }
  def pointer = _emit_load_local(func, value_id, POINTER_LOCAL)
  def change = _emit_const(func, value_id, delta)
  def moved = _emit_binary(func, value_id, N_ADD_I64, pointer, change, N_SCALAR_I64_RAW)
  def zero_index = _emit_const(func, value_id, 0)
  def below = _emit_compare(func, value_id, moved, zero_index, N_CMP_LT)
  def tape_end = _emit_const(func, value_id, TAPE_SIZE)
  def past_end = _emit_compare(func, value_id, moved, tape_end, N_CMP_GE)
  def invalid = _emit_binary(func, value_id, N_OR_I64, below, past_end, N_SCALAR_I64_RAW)
  _emit_branch_if(func, invalid, BOUNDS_LABEL)
  _check_api(api_emit_store_local(func, POINTER_LOCAL, moved, API_ERROR, 512),
             "move tape pointer")
}

fn _emit_loop_start(ptr func, ptr value_id, int index) {
  def start_label = 3 * index + 1
  def body_label = start_label + 1
  def end_label = start_label + 2
  _emit_label(func, start_label)
  def current = _load_cell(func, value_id)
  def cell_zero = _emit_const(func, value_id, 0)
  def nonzero = _emit_compare(func, value_id, current, cell_zero, N_CMP_NE)
  _emit_branch_if(func, nonzero, body_label)
  _emit_branch(func, end_label)
  _emit_label(func, body_label)
}

fn _emit_loop_end(ptr func, int open_index) {
  _emit_branch(func, 3 * open_index + 1)
  _emit_label(func, 3 * open_index + 3)
}

fn _emit_output(ptr func, ptr value_id, ptr call_args) {
  def current = _load_cell(func, value_id)
  store32(call_args, current, 0)
  _emit_call(func, value_id, "putchar", call_args, 1, N_SCALAR_I64_RAW)
}

fn _emit_input(ptr func, ptr value_id, int index) {
  def input = _emit_call(func, value_id, "getchar", nil, 0, N_SCALAR_I64_RAW)
  def eof = _emit_const(func, value_id, 0)
  def has_byte = _emit_compare(func, value_id, input, eof, N_CMP_GE)
  def store_label = IO_LABEL_BASE + 2 * index
  def after_label = store_label + 1
  _emit_branch_if(func, has_byte, store_label)
  _emit_branch(func, after_label)
  _emit_label(func, store_label)
  _store_cell(func, value_id, input)
  _emit_label(func, after_label)
}

fn _release_tape(ptr func, ptr value_id, ptr call_args) {
  def tape = _emit_load_local(func, value_id, BASE_LOCAL)
  store32(call_args, tape, 0)
  _emit_call(func, value_id, "free", call_args, 1, N_SCALAR_POINTER)
}

fn compile_code(str source, ptr func, ptr value_id, ptr call_args) {
  def cell_count = _emit_const(func, value_id, TAPE_SIZE)
  def cell_size = _emit_const(func, value_id, 8)
  store32(call_args, cell_count, 0)
  store32(call_args, cell_size, 4)
  def tape = _emit_call(func, value_id, "calloc", call_args, 2, N_SCALAR_POINTER)
  def zero = _emit_const(func, value_id, 0)
  _check_api(api_emit_store_local(func, BASE_LOCAL, tape, API_ERROR, 512),
             "store tape pointer")
  _check_api(api_emit_store_local(func, POINTER_LOCAL, zero, API_ERROR, 512),
             "initialize tape pointer")

  def allocated = _emit_compare(func, value_id, tape, zero, N_CMP_NE)
  _emit_branch_if(func, allocated, ALLOC_LABEL)
  def alloc_failure = _emit_const(func, value_id, 1)
  _emit_return(func, value_id, alloc_failure)
  _emit_label(func, ALLOC_LABEL)

  mut int stack_capacity = int(source.len)
  if stack_capacity < 1 { stack_capacity = 1 }
  def int stack_bytes = stack_capacity * 8
  mut ptr stack = malloc(stack_bytes)
  assert(stack != 0, "allocate Brainfuck loop stack")
  defer { free(stack) }
  mut int depth = 0
  mut int i = 0
  while i < source.len {
    def int op = load8(source, i)

    if op == 43 || op == 45 {
      mut int delta = 0
      while i < source.len {
        def int next_op = load8(source, i)
        if next_op != 43 && next_op != 45 { break }
        delta += next_op == 43 ? 1 : -1
        i += 1
      }
      _adjust_cell(func, value_id, delta)

    } else if op == 62 || op == 60 {
      mut int delta = 0
      while i < source.len {
        def int next_op = load8(source, i)
        if next_op != 62 && next_op != 60 { break }
        delta += next_op == 62 ? 1 : -1
        i += 1
      }
      _move_pointer(func, value_id, delta)

    } else if op == 91 {
      if i + 2 < source.len && (load8(source, i + 1) == 43 || load8(source, i + 1) == 45) && load8(source, i + 2) == 93 {
        def cell_zero = _emit_const(func, value_id, 0)
        _store_cell(func, value_id, cell_zero)
        i += 3
      } else {
        store64_i(stack, i, depth * 8)
        depth += 1
        _emit_loop_start(func, value_id, i)
        i += 1
      }

    } else if op == 93 {
      if depth == 0 {
        eprint("brainfuck: unexpected ']'")
        exit(2)
      }
      depth -= 1
      def int open_index = load64_i(stack, depth * 8)
      _emit_loop_end(func, open_index)
      i += 1

    } else if op == 46 {
      _emit_output(func, value_id, call_args)
      i += 1

    } else if op == 44 {
      _emit_input(func, value_id, i)
      i += 1

    } else {
      i += 1
    }
  }

  if depth != 0 {
    eprint("brainfuck: missing ']'")
    exit(2)
  }
  _release_tape(func, value_id, call_args)
  def success = _emit_const(func, value_id, 0)
  _emit_return(func, value_id, success)
  _emit_label(func, BOUNDS_LABEL)
  _release_tape(func, value_id, call_args)
  def bounds_error = _emit_const(func, value_id, 1)
  _emit_return(func, value_id, bounds_error)
}

#main {
  mut spec = cli.app("brainfuck", "Run Brainfuck source with the Nyr JIT")
  spec = cli.add(spec,
    cli.optional_option("asm", "a", "Emit assembly to stderr or PATH", "path",
                        "", "stderr"))
  spec = cli.add(spec,
    cli.option("code", "c", "Use inline Brainfuck CODE", "str", nil))
  spec = cli.add(spec,
    cli.positional("source", "Brainfuck source file", "path", false))
  spec = cli.add(spec, cli.rest("args"))

  def parsed = cli.parse(spec)
  if !bool(parsed.get("ok", false)) {
    eprint(cli.format_error(parsed))
    exit(2)
  }
  if bool(parsed.get("help", false)) {
    print(to_str(parsed.get("text", "")))
    exit(0)
  }

  def values = parsed.get("values", {})
  def inline_source = values.get("code", nil)
  def has_inline_source = parsed.get("provided", {}).contains("code")
  def source_path = to_str(values.get("source", ""))
  if !has_inline_source && source_path.len == 0 {
    eprint("brainfuck: expected -c CODE or SOURCE.bf")
    exit(2)
  }
  if has_inline_source && source_path.len > 0 {
    eprint("brainfuck: -c CODE and SOURCE.bf are mutually exclusive")
    exit(2)
  }
  mut str source = ""
  if has_inline_source {
    source = to_str(inline_source)
  } else {
    def source_result = file_read(source_path)
    if !is_ok(source_result) {
      eprint("brainfuck: cannot read source '" + source_path + "'")
      exit(1)
    }
    source = to_str(unwrap(source_result))
  }

  mut str requested_assembly = ""
  if parsed.get("provided", {}).contains("asm") {
    requested_assembly = to_str(values.get("asm", ""))
  }
  def assembly_to_stderr =
    requested_assembly == "stderr" || requested_assembly == "-"
  mut str assembly_path = requested_assembly
  if assembly_to_stderr {
    assembly_path = temp_dir() + "/ny-brainfuck-" + to_str(pid()) + ".s"
  }

  def func = api_func_create()
  def value_id = malloc(4)
  def call_args = malloc(8)
  def jit_result = malloc(8)
  API_ERROR = malloc(512)
  assert(func != 0 && value_id != 0 && call_args != 0 && jit_result != 0 &&
         API_ERROR != 0,
    "allocate Nyr frontend state")
  mut int error_byte = 0
  while error_byte < 512 {
    store8(API_ERROR, 0, error_byte)
    error_byte += 1
  }
  defer {
    api_func_free(func)
    free(value_id)
    free(call_args)
    free(jit_result)
    free(API_ERROR)
    API_ERROR = 0
  }

  compile_code(source, func, value_id, call_args)
  _check_api(api_verify(func, API_ERROR, 512), "verify Brainfuck NYIR")
  _check_api(api_optimize(func, N_OPT_O2, API_ERROR, 512),
             "optimize Brainfuck NYIR")
  _check_api(api_verify(func, API_ERROR, 512), "verify optimized Brainfuck NYIR")
  if assembly_path != "" {
    _check_api(api_emit_assembly(func, assembly_path, "rt_main", API_ERROR, 512),
               "emit Nyr assembly to '" + assembly_path + "'")
    if assembly_to_stderr {
      def assembly = file_read(assembly_path)
      if !is_ok(assembly) {
        file_remove(assembly_path)
        eprint("brainfuck: cannot read generated assembly")
        exit(1)
      }
      eprint(to_str(unwrap(assembly)))
      file_remove(assembly_path)
    }
  }
  _check_api(api_jit_run(func, jit_result, API_ERROR, 512),
             "run Brainfuck with Nyr JIT")
  if load64_i(jit_result, 0) != 0 {
    eprint("Brainfuck runtime error")
    exit(1)
  }
}