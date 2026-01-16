#include "base/loader.h"
#include "parse/parser.h"
#include "priv.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void doclist_set(doc_list_t *dl, const char *name, const char *doc,
                 const char *def, const char *src, int kind) {
  if (!dl || !name)
    return;
  for (size_t i = 0; i < dl->len; ++i) {
    if (strcmp(dl->data[i].name, name) == 0) {
      if (doc) {
        free(dl->data[i].doc);
        dl->data[i].doc = strdup(doc);
      }
      if (def) {
        free(dl->data[i].def);
        dl->data[i].def = strdup(def);
      }
      if (src) {
        free(dl->data[i].src);
        dl->data[i].src = strdup(src);
      }
      if (kind != 0)
        dl->data[i].kind = kind;
      return;
    }
  }
  if (dl->len == dl->cap) {
    size_t new_cap = dl->cap ? dl->cap * 2 : 64;
    ny_doc_entry *nd = realloc(dl->data, new_cap * sizeof(ny_doc_entry));
    if (!nd)
      return;
    memset(nd + dl->len, 0, (new_cap - dl->len) * sizeof(ny_doc_entry));
    dl->data = nd;
    dl->cap = new_cap;
  }
  dl->data[dl->len].name = strdup(name);
  dl->data[dl->len].doc = doc ? strdup(doc) : NULL;
  dl->data[dl->len].def = def ? strdup(def) : NULL;
  dl->data[dl->len].src = src ? strdup(src) : NULL;
  dl->data[dl->len].kind = kind;
  dl->len += 1;
}

void doclist_add_recursive(doc_list_t *dl, ny_stmt_list *body,
                           const char *prefix) {
  for (size_t i = 0; i < body->len; ++i) {
    stmt_t *s = body->data[i];
    if (s->kind == NY_S_FUNC) {
      char qname[512];
      const char *name = s->as.fn.name;
      if (prefix && *prefix && strncmp(name, prefix, strlen(prefix)) != 0) {
        snprintf(qname, sizeof(qname), "%s.%s", prefix, name);
        name = qname;
      }
      char def_buf[512];
      int n = snprintf(def_buf, sizeof(def_buf), "fn %s(", name);
      for (size_t j = 0; j < s->as.fn.params.len; ++j) {
        const char *sep = (j + 1 < s->as.fn.params.len) ? ", " : "";
        int written = snprintf(def_buf + n, sizeof(def_buf) - (size_t)n, "%s%s",
                               s->as.fn.params.data[j].name, sep);
        if (written > 0)
          n += written;
      }
      snprintf(def_buf + n, sizeof(def_buf) - (size_t)n, ")");
      char *src = NULL;
      if (s->as.fn.src_start && s->as.fn.src_end > s->as.fn.src_start) {
        src = ny_strndup(s->as.fn.src_start,
                         (size_t)(s->as.fn.src_end - s->as.fn.src_start));
      }
      doclist_set(dl, name, s->as.fn.doc, def_buf, src, 3); // 3 = FN
      if (src)
        free(src);
    } else if (s->kind == NY_S_MODULE) {
      char qname[512];
      const char *name = s->as.module.name;
      if (prefix && *prefix && strncmp(name, prefix, strlen(prefix)) != 0) {
        snprintf(qname, sizeof(qname), "%s.%s", prefix, name);
        name = qname;
      }
      char *src = NULL;
      if (s->as.module.src_start &&
          s->as.module.src_end > s->as.module.src_start) {
        src =
            ny_strndup(s->as.module.src_start,
                       (size_t)(s->as.module.src_end - s->as.module.src_start));
      }
      doclist_set(dl, name, "Module", "module", src, 2); // 2 = MOD
      if (src)
        free(src);
      doclist_add_recursive(dl, &s->as.module.body, name);
    }
  }
}

void doclist_add_from_prog(doc_list_t *dl, program_t *prog) {
  if (!dl || !prog)
    return;
  doclist_add_recursive(dl, &prog->body, NULL);
}

void doclist_free(doc_list_t *dl) {
  if (!dl || !dl->data)
    return;
  for (size_t i = 0; i < dl->len; ++i) {
    free(dl->data[i].name);
    if (dl->data[i].doc)
      free(dl->data[i].doc);
    if (dl->data[i].def)
      free(dl->data[i].def);
    if (dl->data[i].src)
      free(dl->data[i].src);
  }
  free(dl->data);
}

int doclist_print(const doc_list_t *dl, const char *name) {
  if (!dl || !name || !*name)
    return 0;

  int found_idx = -1;

  // 1. Try exact match
  for (size_t i = 0; i < dl->len; ++i) {
    if (strcmp(dl->data[i].name, name) == 0) {
      found_idx = (int)i;
      goto found;
    }
  }

  // 2. Try suffix match (e.g. "sys_write" matching "std.io.sys_write")
  int match_idx = -1;
  int match_count = 0;
  size_t name_len = strlen(name);

  for (size_t i = 0; i < dl->len; ++i) {
    const char *entry_name = dl->data[i].name;
    size_t entry_len = strlen(entry_name);

    if (entry_len > name_len && entry_name[entry_len - name_len - 1] == '.' &&
        strcmp(entry_name + entry_len - name_len, name) == 0) {
      match_idx = (int)i;
      match_count++;
    } else if (name_len > entry_len && name[name_len - entry_len - 1] == '.' &&
               strcmp(name + name_len - entry_len, entry_name) == 0) {
      match_idx = (int)i;
      match_count++;
    }
  }

  if (match_count == 1) {
    found_idx = match_idx;
    goto found;
  } else if (match_count > 1) {
    printf("%sMultiple matches found for '%s':%s\n", clr(NY_CLR_YELLOW), name,
           clr(NY_CLR_RESET));
    for (size_t i = 0; i < dl->len; ++i) {
      const char *en = dl->data[i].name;
      size_t el = strlen(en);
      if ((el > name_len && en[el - name_len - 1] == '.' &&
           strcmp(en + el - name_len, name) == 0) ||
          (name_len > el && name[name_len - el - 1] == '.' &&
           strcmp(name + name_len - el, en) == 0)) {
        printf("  - %s\n", en);
      }
    }
    return 1;
  }

  return 0;

found: {
  ny_doc_entry *e = &dl->data[found_idx];

  const char *k_name = "Symbol";
  if (e->kind == 1)
    k_name = "Package";
  else if (e->kind == 2)
    k_name = "Module";
  else if (e->kind == 3)
    k_name = "Function";

  printf("\n%s%s%s %s%s%s\n", clr(NY_CLR_BOLD), clr(NY_CLR_MAGENTA), k_name,
         clr(NY_CLR_CYAN), e->name, clr(NY_CLR_RESET));

  if (e->def) {
    printf("%s %s%s%s\n", clr(NY_CLR_GRAY), clr(NY_CLR_GREEN), e->def,
           clr(NY_CLR_RESET));
  }

  printf("%s%s%s\n", clr(NY_CLR_GRAY),
         "--------------------------------------------------",
         clr(NY_CLR_RESET));

  if (e->doc && strlen(e->doc) > 0) {
    printf("%s\n", e->doc);
  } else {
    printf("%s(No documentation string available)%s\n", clr(NY_CLR_GRAY),
           clr(NY_CLR_RESET));
  }

  if (e->src) {
    printf("\n%sImplementation:%s\n", clr(NY_CLR_BOLD), clr(NY_CLR_RESET));
    const char *s = e->src;
    while (*s && isspace((unsigned char)*s))
      s++;
    repl_highlight_line(s);
    printf("\n");
  }
  printf("\n");
  return 1;
}
}

void add_builtin_docs(doc_list_t *docs) {
  // Memory
  doclist_set(docs, "__malloc", "Allocates n bytes of memory on the heap.",
              "fn __malloc(n)", NULL, 3);
  doclist_set(docs, "__free", "Frees memory previously allocated by __malloc.",
              "fn __free(p)", NULL, 3);
  doclist_set(docs, "__realloc", "Reallocates memory to a new size.",
              "fn __realloc(p, n)", NULL, 3);

  // Low level memory access
  doclist_set(docs, "__load8", "Loads a single byte from memory address p.",
              "fn __load8(p)", NULL, 3);
  doclist_set(docs, "__store8", "Stores byte v at memory address p.",
              "fn __store8(p, v)", NULL, 3);
  doclist_set(docs, "__load16", "Loads a 16-bit integer from memory address p.",
              "fn __load16(p)", NULL, 3);
  doclist_set(docs, "__store16", "Stores 16-bit integer v at memory address p.",
              "fn __store16(p, v)", NULL, 3);
  doclist_set(docs, "__load32", "Loads a 32-bit integer from memory address p.",
              "fn __load32(p)", NULL, 3);
  doclist_set(docs, "__store32", "Stores 32-bit integer v at memory address p.",
              "fn __store32(p, v)", NULL, 3);
  doclist_set(docs, "__load64", "Loads a 64-bit integer from memory address p.",
              "fn __load64(p)", NULL, 3);
  doclist_set(docs, "__store64", "Stores 64-bit integer v at memory address p.",
              "fn __store64(p, v)", NULL, 3);

  // Pointer arithmetic
  doclist_set(docs, "__ptr_add", "Adds offset to pointer.",
              "fn __ptr_add(p, offset)", NULL, 3);
  doclist_set(docs, "__ptr_sub",
              "Subtracts offset from pointer or returns difference.",
              "fn __ptr_sub(p, b)", NULL, 3);

  // Syscall
  doclist_set(docs, "__syscall", "Executes a raw Linux system call.",
              "fn __syscall(n, a1, a2, a3, a4, a5, a6)", NULL, 3);
  doclist_set(docs, "__exit", "Exits the program_t with status code.",
              "fn __exit(code)", NULL, 3);
  doclist_set(docs, "__errno", "Returns the last error number.", "fn __errno()",
              NULL, 3);

  // Math
  doclist_set(docs, "__add", "Integer addition.", "fn __add(a, b)", NULL, 3);
  doclist_set(docs, "__sub", "Integer subtraction.", "fn __sub(a, b)", NULL, 3);
  doclist_set(docs, "__mul", "Integer multiplication.", "fn __mul(a, b)", NULL,
              3);
  doclist_set(docs, "__div", "Integer division.", "fn __div(a, b)", NULL, 3);
  doclist_set(docs, "__mod", "Integer modulus.", "fn __mod(a, b)", NULL, 3);
  doclist_set(docs, "__and", "Bitwise AND.", "fn __and(a, b)", NULL, 3);
  doclist_set(docs, "__or", "Bitwise OR.", "fn __or(a, b)", NULL, 3);
  doclist_set(docs, "__xor", "Bitwise XOR.", "fn __xor(a, b)", NULL, 3);
  doclist_set(docs, "__not", "Bitwise NOT.", "fn __not(a)", NULL, 3);
  doclist_set(docs, "__shl", "Bitwise shift left.", "fn __shl(a, b)", NULL, 3);
  doclist_set(docs, "__shr", "Bitwise shift right.", "fn __shr(a, b)", NULL, 3);

  // Float
  doclist_set(docs, "__flt_add", "Float addition.", "fn __flt_add(a, b)", NULL,
              3);
  doclist_set(docs, "__flt_sub", "Float subtraction.", "fn __flt_sub(a, b)",
              NULL, 3);
  doclist_set(docs, "__flt_mul", "Float multiplication.", "fn __flt_mul(a, b)",
              NULL, 3);
  doclist_set(docs, "__flt_div", "Float division.", "fn __flt_div(a, b)", NULL,
              3);
  doclist_set(docs, "__flt_from_int", "Convert int to float.",
              "fn __flt_from_int(i)", NULL, 3);
  doclist_set(docs, "__flt_to_int", "Convert float to int.",
              "fn __flt_to_int(f)", NULL, 3);

  // Type checks
  doclist_set(docs, "__is_int", "Checks if value is an integer.",
              "fn __is_int(v)", NULL, 3);
  doclist_set(docs, "__is_ptr", "Checks if value is a pointer.",
              "fn __is_ptr(v)", NULL, 3);
  doclist_set(docs, "__is_str", "Checks if value is a string.",
              "fn __is_str(v)", NULL, 3);
  doclist_set(docs, "__is_flt", "Checks if value is a float.", "fn __is_flt(v)",
              NULL, 3);

  // Strings
  doclist_set(docs, "__init_str", "Initializes a string.", "fn __init_str(s)",
              NULL, 3);
  doclist_set(docs, "__to_str", "Converts primitive to string.",
              "fn __to_str(v)", NULL, 3);
  doclist_set(docs, "__str_concat", "Concatenates two strings.",
              "fn __str_concat(a, b)", NULL, 3);

  // Dynamic Linking
  doclist_set(docs, "__dlopen", "Opens a dynamic library.",
              "fn __dlopen(path, flags)", NULL, 3);
  doclist_set(docs, "__dlsym", "Resolves a symbol in a library.",
              "fn __dlsym(handle, symbol)", NULL, 3);
  doclist_set(docs, "__dlclose", "Closes a dynamic library.",
              "fn __dlclose(handle)", NULL, 3);
  doclist_set(docs, "__dlerror", "Returns the last dynamic linking error.",
              "fn __dlerror()", NULL, 3);

  // Threads
  doclist_set(docs, "__thread_spawn", "Spawns a new thread.",
              "fn __thread_spawn(fn_ptr, arg)", NULL, 3);
  doclist_set(docs, "__thread_join", "Joins a thread.",
              "fn __thread_join(thread)", NULL, 3);
  doclist_set(docs, "__mutex_new", "Creates a new mutex.", "fn __mutex_new()",
              NULL, 3);
  doclist_set(docs, "__mutex_lock64", "Locks a mutex.", "fn __mutex_lock64(m)",
              NULL, 3);
  doclist_set(docs, "__mutex_unlock64", "Unlocks a mutex.",
              "fn __mutex_unlock64(m)", NULL, 3);
  doclist_set(docs, "__mutex_free", "Frees a mutex.", "fn __mutex_free(m)",
              NULL, 3);

  // Misc
  doclist_set(docs, "__sleep", "Sleeps for n milliseconds.", "fn __sleep(ms)",
              NULL, 3);
  doclist_set(docs, "__panic", "Panics with a message.", "fn __panic(msg)",
              NULL, 3);
  doclist_set(docs, "__argc", "Returns argument count.", "fn __argc()", NULL,
              3);
  doclist_set(docs, "__argv", "Returns argument vector.", "fn __argv(i)", NULL,
              3);
  doclist_set(docs, "__envp", "Returns environment variable at index.",
              "fn __envp(i)", NULL, 3);
  doclist_set(docs, "__envc", "Returns environment variable count.",
              "fn __envc()", NULL, 3);
}

void repl_load_module_docs(doc_list_t *docs, const char *name) {
  int idx = ny_std_find_module_by_name(name);
  if (idx < 0)
    return;
  const char *path = ny_std_module_path((size_t)idx);
  char *src = repl_read_file(path);
  if (!src)
    return;
  parser_t ps;
  parser_init(&ps, src, path);
  program_t pr = parse_program(&ps);
  if (!ps.had_error) {
    if (pr.doc)
      doclist_set(docs, name, pr.doc, "module", NULL, 2); // 2 = MOD
    doclist_add_recursive(docs, &pr.body, name);
  }
  program_free(&pr, ps.arena);
  free(src);
}
