/*
 * NYR text: a human-writable source form for NYIR modules.
 *
 * The syntax follows Ny: `fn name(type param) ret { ... }`, `def`/`mut`
 * bindings, `if`/`elif`/`else`, `while`, `for`, `;;` comments.
 *
 *   extern { fn printf(ptr fmt, ...) i32 }
 *   def greeting = "fib(%ld) = %ld\n"
 *
 *   fn fib(int n) int {
 *     if n < 2 { return n }
 *     return fib(n - 1) + fib(n - 2)
 *   }
 *
 *   fn main() int {
 *     printf(greeting, 30, fib(30))
 *     return 0
 *   }
 *
 * nyr_text_parse checks and lowers every function to a verified nyr_func_t;
 * data items (strings, globals) are written as GNU assembly by
 * nyr_text_write_data. See etc/docs/spec/nyr.md for the language.
 */
#ifndef NYR_TEXT_H
#define NYR_TEXT_H

#include <nyr/nyr.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nyr_text_module nyr_text_module_t;

/* Parse, type-check and lower `source` (`length` bytes; `path` names it in
 * diagnostics). Every function is verified before the call returns. On
 * failure returns NULL and writes a `path:line:col: error: ...` diagnostic,
 * with the source line and a caret, to `err`. */
nyr_text_module_t *nyr_text_parse(const char *source, size_t length,
                                  const char *path, char *err, size_t err_len);
void nyr_text_module_free(nyr_text_module_t *module);

size_t nyr_text_function_count(const nyr_text_module_t *module);
/* Source name of function `index`. */
const char *nyr_text_function_name(const nyr_text_module_t *module,
                                   size_t index);
/* Object symbol: the plain name for `export` functions and `main`,
 * `ny_fn_<name>` (the internal-call convention) otherwise. */
const char *nyr_text_function_symbol(const nyr_text_module_t *module,
                                     size_t index);
nyr_func_t *nyr_text_function(nyr_text_module_t *module, size_t index);
/* Index of the function named `name`, or -1. */
long nyr_text_function_find(const nyr_text_module_t *module, const char *name);

/* True when the module defines strings or globals. */
bool nyr_text_has_data(const nyr_text_module_t *module);
/* Write the module's strings and globals as GNU assembly (x86-64 ELF). */
bool nyr_text_write_data(const nyr_text_module_t *module, FILE *out);

/* Print `func` in NYR text form. Instructions the text form does not cover
 * are printed as comments, so the output is readable for any function and
 * parses back when every instruction is covered. */
bool nyr_text_print_function(FILE *out, const nyr_func_t *func,
                             const char *name);

#ifdef __cplusplus
}
#endif

#endif
