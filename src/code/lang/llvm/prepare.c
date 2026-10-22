/*
 * LLVM compiler preparation: symbol, module, import, and link analysis used
 * before the unified NYIR-to-LLVM emitter.
 */
#include "base/util.h"
#include "code/lang/c/clang.h"
#include "code/lang/code.h"
#include "code/lang/priv.h"

/*
 * Import-reachable native-link preparation helpers (merged from
 * prepare_links.h): use-module collection, link allow-lists, and the
 * native-link discovery used by module preparation.
 * LLVM preparation helpers for import-reachable native links.
 */
static bool ny_user_use_has(codegen_t *cg, const char *mod) {
  if (!cg || !mod || !*mod)
    return false;
  for (size_t i = 0; i < cg->user_use_modules.len; i++) {
    const char *m = cg->user_use_modules.data[i];
    if (m && strcmp(m, mod) == 0)
      return true;
  }
  return false;
}

static bool ny_link_allowed_has(codegen_t *cg, const char *mod) {
  if (!cg || !mod || !*mod)
    return false;
  for (size_t i = 0; i < cg->link_allowed_modules.len; i++) {
    const char *m = cg->link_allowed_modules.data[i];
    if (m && strcmp(m, mod) == 0)
      return true;
  }
  return false;
}

static void ny_collect_use_modules_stmt(stmt_t *s, str_list *out) {
  if (!s || !out)
    return;
  if (s->kind == NY_S_USE) {
    if (ny_stmt_is_bare_core_use(s)) {
      vec_push(out, (char *)"core");
      vec_push(out, (char *)"core.os.prim");
    } else if (s->as.use.module) {
      vec_push(out, (char *)s->as.use.module);
    }
  } else if (s->kind == NY_S_LEMMA) {
  } else if (s->kind == NY_S_MODULE) {
    for (size_t i = 0; i < s->as.module.body.len; i++)
      ny_collect_use_modules_stmt(s->as.module.body.data[i], out);
  } else if (s->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < s->as.block.body.len; i++)
      ny_collect_use_modules_stmt(s->as.block.body.data[i], out);
  } else if (s->kind == NY_S_IF) {
    if (s->as.iff.conseq)
      ny_collect_use_modules_stmt(s->as.iff.conseq, out);
    if (s->as.iff.alt)
      ny_collect_use_modules_stmt(s->as.iff.alt, out);
  } else if (s->kind == NY_S_WHILE) {
    if (s->as.whl.body)
      ny_collect_use_modules_stmt(s->as.whl.body, out);
    if (s->as.whl.update)
      ny_collect_use_modules_stmt(s->as.whl.update, out);
    if (s->as.whl.init)
      ny_collect_use_modules_stmt(s->as.whl.init, out);
  } else if (s->kind == NY_S_FOR) {
    if (s->as.fr.init)
      ny_collect_use_modules_stmt(s->as.fr.init, out);
    if (s->as.fr.body)
      ny_collect_use_modules_stmt(s->as.fr.body, out);
    if (s->as.fr.update)
      ny_collect_use_modules_stmt(s->as.fr.update, out);
  } else if (s->kind == NY_S_TRY) {
    if (s->as.tr.body)
      ny_collect_use_modules_stmt(s->as.tr.body, out);
    if (s->as.tr.handler)
      ny_collect_use_modules_stmt(s->as.tr.handler, out);
  } else if (s->kind == NY_S_DEFER) {
    if (s->as.de.body)
      ny_collect_use_modules_stmt(s->as.de.body, out);
  } else if (s->kind == NY_S_MATCH) {
    for (size_t i = 0; i < s->as.match.arms.len; i++) {
      if (s->as.match.arms.data[i].conseq)
        ny_collect_use_modules_stmt(s->as.match.arms.data[i].conseq, out);
    }
    if (s->as.match.default_conseq)
      ny_collect_use_modules_stmt(s->as.match.default_conseq, out);
  }
}

static void ny_collect_module_names_stmt(stmt_t *s, str_list *out) {
  if (!s || !out)
    return;
  if (s->kind == NY_S_MODULE) {
    if (s->as.module.name && *s->as.module.name)
      vec_push(out, (char *)s->as.module.name);
    for (size_t i = 0; i < s->as.module.body.len; i++)
      ny_collect_module_names_stmt(s->as.module.body.data[i], out);
  } else if (s->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < s->as.block.body.len; i++)
      ny_collect_module_names_stmt(s->as.block.body.data[i], out);
  } else if (s->kind == NY_S_IF) {
    if (s->as.iff.conseq)
      ny_collect_module_names_stmt(s->as.iff.conseq, out);
    if (s->as.iff.alt)
      ny_collect_module_names_stmt(s->as.iff.alt, out);
  } else if (s->kind == NY_S_WHILE) {
    if (s->as.whl.body)
      ny_collect_module_names_stmt(s->as.whl.body, out);
    if (s->as.whl.update)
      ny_collect_module_names_stmt(s->as.whl.update, out);
    if (s->as.whl.init)
      ny_collect_module_names_stmt(s->as.whl.init, out);
  } else if (s->kind == NY_S_FOR) {
    if (s->as.fr.init)
      ny_collect_module_names_stmt(s->as.fr.init, out);
    if (s->as.fr.body)
      ny_collect_module_names_stmt(s->as.fr.body, out);
    if (s->as.fr.update)
      ny_collect_module_names_stmt(s->as.fr.update, out);
  } else if (s->kind == NY_S_TRY) {
    if (s->as.tr.body)
      ny_collect_module_names_stmt(s->as.tr.body, out);
    if (s->as.tr.handler)
      ny_collect_module_names_stmt(s->as.tr.handler, out);
  } else if (s->kind == NY_S_DEFER) {
    if (s->as.de.body)
      ny_collect_module_names_stmt(s->as.de.body, out);
  } else if (s->kind == NY_S_MATCH) {
    for (size_t i = 0; i < s->as.match.arms.len; i++) {
      if (s->as.match.arms.data[i].conseq)
        ny_collect_module_names_stmt(s->as.match.arms.data[i].conseq, out);
    }
    if (s->as.match.default_conseq)
      ny_collect_module_names_stmt(s->as.match.default_conseq, out);
  }
}

static void ny_collect_module_names_prog(program_t *prog, str_list *out) {
  if (!prog || !out)
    return;
  for (size_t i = 0; i < prog->body.len; i++)
    ny_collect_module_names_stmt(prog->body.data[i], out);
}

static stmt_t *ny_find_module_stmt_any(codegen_t *cg, const char *name) {
  if (!cg || !name || !*name)
    return NULL;
  if (cg->prog) {
    for (size_t i = 0; i < cg->prog->body.len; i++) {
      stmt_t *m = find_module_stmt(cg->prog->body.data[i], name);
      if (m)
        return m;
    }
  }
  for (size_t p = 0; p < cg->extra_progs.len; p++) {
    program_t *prog = cg->extra_progs.data[p];
    if (!prog)
      continue;
    for (size_t i = 0; i < prog->body.len; i++) {
      stmt_t *m = find_module_stmt(prog->body.data[i], name);
      if (m)
        return m;
    }
  }
  return NULL;
}

static void ny_build_link_allowed_modules(codegen_t *cg) {
  if (!cg)
    return;
  for (size_t i = 0; i < cg->link_allowed_modules.len; i++)
    free(cg->link_allowed_modules.data[i]);
  cg->link_allowed_modules.len = 0;

  VEC(const char *) queue;
  vec_init(&queue);
  for (size_t i = 0; i < cg->user_use_modules.len; i++) {
    const char *m = cg->user_use_modules.data[i];
    if (m && *m)
      vec_push(&queue, m);
  }
  if (queue.len == 0 && ny_codegen_current_module_is_std(cg, NULL)) {
    vec_push(&queue, cg->current_module_name);
  }
  if (queue.len == 0) {
    str_list mods = {0};
    ny_collect_module_names_prog(cg->prog, &mods);
    for (size_t p = 0; p < cg->extra_progs.len; p++) {
      program_t *prog = cg->extra_progs.data[p];
      ny_collect_module_names_prog(prog, &mods);
    }
    for (size_t i = 0; i < mods.len; i++) {
      const char *m = mods.data[i];
      if (m && *m)
        vec_push(&queue, m);
    }
    vec_free(&mods);
  }
  while (queue.len > 0) {
    const char *mod = queue.data[queue.len - 1];
    queue.len--;
    if (!mod || !*mod)
      continue;
    if (ny_link_allowed_has(cg, mod))
      continue;
    vec_push(&cg->link_allowed_modules, ny_strdup(mod));
    stmt_t *mstmt = ny_find_module_stmt_any(cg, mod);
    if (!mstmt || mstmt->kind != NY_S_MODULE)
      continue;
    str_list deps = {0};
    for (size_t i = 0; i < mstmt->as.module.body.len; i++)
      ny_collect_use_modules_stmt(mstmt->as.module.body.data[i], &deps);
    for (size_t i = 0; i < deps.len; i++) {
      const char *dep = deps.data[i];
      if (dep && *dep)
        vec_push(&queue, dep);
    }
    vec_free(&deps);
  }
  vec_free(&queue);
}

static bool ny_link_allowed_for_module(codegen_t *cg, const char *mod) {
  if (ny_env_enabled("NYTRIX_LINK_ALLOW_ALL"))
    return true;
  if (!mod || !*mod)
    return true;
  bool module_origin_known = false;
  bool module_is_std = ny_codegen_module_is_std(cg, mod, &module_origin_known);
  if (module_origin_known && module_is_std)
    return true;
  if (strncmp(mod, "lib.", 4) != 0 && strcmp(mod, "lib") != 0)
    return true;
  if (cg->link_allowed_modules.len == 0 && !cg->current_module_name)
    return true;
  if (cg->link_allowed_modules.len == 0 &&
      ny_codegen_current_module_is_std(cg, NULL))
    return true;
  if (cg->link_allowed_modules.len == 0 && cg->current_module_name &&
      strcmp(cg->current_module_name, mod) == 0)
    return true;
  if (cg->link_allowed_modules.len == 0 && ny_user_use_has(cg, mod))
    return true;
  return ny_link_allowed_has(cg, mod);
}

static void process_links(codegen_t *cg, stmt_t *s, const char *cur_mod) {
  if (s->kind == NY_S_LINK) {
    if (!ny_link_allowed_for_module(cg, cur_mod))
      return;
    if (s->as.link.lib) {
      char *resolved = ny_ensure_shared_lib(s->as.link.lib);
      const char *lib = resolved ? resolved : s->as.link.lib;
      bool found = false;
      for (size_t i = 0; i < cg->links.len; i++) {
        if (strcmp(cg->links.data[i], lib) == 0 ||
            strcmp(cg->links.data[i], s->as.link.lib) == 0) {
          found = true;
          break;
        }
      }
      if (!found)
        vec_push(&cg->links, resolved ? resolved : ny_strdup(s->as.link.lib));
      else if (resolved)
        free(resolved);
    }
  } else if (s->kind == NY_S_LEMMA) {
    /*
     * Lemmas do not introduce native links
     */
  } else if (s->kind == NY_S_MODULE) {
    for (size_t i = 0; i < s->as.module.body.len; i++)
      process_links(cg, s->as.module.body.data[i], s->as.module.name);
  } else if (s->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < s->as.block.body.len; i++)
      process_links(cg, s->as.block.body.data[i], cur_mod);
  } else if (s->kind == NY_S_IF) {
    bool truthy = false;
    if (ny_eval_comptime_if(cg, s, &truthy)) {
      if (truthy) {
        if (s->as.iff.conseq)
          process_links(cg, s->as.iff.conseq, cur_mod);
      } else if (s->as.iff.alt) {
        process_links(cg, s->as.iff.alt, cur_mod);
      }
    } else {
      if (s->as.iff.conseq)
        process_links(cg, s->as.iff.conseq, cur_mod);
      if (s->as.iff.alt)
        process_links(cg, s->as.iff.alt, cur_mod);
    }
  } else if (s->kind == NY_S_WHILE) {
    if (s->as.whl.body)
      process_links(cg, s->as.whl.body, cur_mod);
    if (s->as.whl.update)
      process_links(cg, s->as.whl.update, cur_mod);
    if (s->as.whl.init)
      process_links(cg, s->as.whl.init, cur_mod);
  } else if (s->kind == NY_S_FOR) {
    if (s->as.fr.init)
      process_links(cg, s->as.fr.init, cur_mod);
    if (s->as.fr.body)
      process_links(cg, s->as.fr.body, cur_mod);
    if (s->as.fr.update)
      process_links(cg, s->as.fr.update, cur_mod);
  } else if (s->kind == NY_S_TRY) {
    if (s->as.tr.body)
      process_links(cg, s->as.tr.body, cur_mod);
    if (s->as.tr.handler)
      process_links(cg, s->as.tr.handler, cur_mod);
  } else if (s->kind == NY_S_DEFER) {
    if (s->as.de.body)
      process_links(cg, s->as.de.body, cur_mod);
  } else if (s->kind == NY_S_MATCH) {
    for (size_t i = 0; i < s->as.match.arms.len; i++) {
      if (s->as.match.arms.data[i].conseq)
        process_links(cg, s->as.match.arms.data[i].conseq, cur_mod);
    }
    if (s->as.match.default_conseq)
      process_links(cg, s->as.match.default_conseq, cur_mod);
  }
}

void codegen_collect_links(codegen_t *cg, program_t *prog) {
  if (!cg || !prog)
    return;
  NY_COMPILER_ASSERT(
      prog->body.len <= prog->body.cap,
      "codegen_collect_links program body vector len exceeds cap");
  NY_COMPILER_ASSERT(
      prog->body.data != NULL || prog->body.len == 0,
      "codegen_collect_links program body vector has len but no data");
  NY_COMPILER_ASSERT(
      cg->extra_progs.len <= cg->extra_progs.cap,
      "codegen_collect_links extra_progs vector len exceeds cap");
  NY_COMPILER_ASSERT(
      cg->extra_progs.data != NULL || cg->extra_progs.len == 0,
      "codegen_collect_links extra_progs vector has len but no data");
  if (cg->use_modules.len == 0 && cg->user_use_modules.len == 0) {
    for (size_t i = 0; i < prog->body.len; i++) {
      stmt_t *s = prog->body.data[i];
      NY_COMPILER_ASSERTF(s != NULL, "null collect-links use stmt at index %zu",
                          i);
      if (!s)
        continue;
      collect_use_modules(cg, s);
    }
  }
  ny_build_link_allowed_modules(cg);
  for (size_t i = 0; i < prog->body.len; i++) {
    stmt_t *s = prog->body.data[i];
    NY_COMPILER_ASSERTF(s != NULL,
                        "null collect-links process stmt at index %zu", i);
    if (!s)
      continue;
    process_links(cg, s, NULL);
  }

  for (size_t p = 0; p < cg->extra_progs.len; p++) {
    program_t *eprog = cg->extra_progs.data[p];
    if (!eprog)
      continue;
    NY_COMPILER_ASSERTF(
        eprog->body.len <= eprog->body.cap,
        "extra program %zu body vector len exceeds cap during link collection",
        p);
    NY_COMPILER_ASSERTF(eprog->body.data != NULL || eprog->body.len == 0,
                        "extra program %zu body vector has len but no data "
                        "during link collection",
                        p);
    for (size_t i = 0; i < eprog->body.len; i++) {
      stmt_t *s = eprog->body.data[i];
      NY_COMPILER_ASSERTF(
          s != NULL, "null extra collect-links stmt p=%zu index=%zu", p, i);
      if (!s)
        continue;
      process_links(cg, s, NULL);
    }
  }
}

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#ifndef _WIN32
#include <unistd.h>
#else
#include <process.h>
#define getpid _getpid
#endif

static void ny_register_impl_types_stmt(codegen_t *cg, stmt_t *s) {
  if (!cg || !s)
    return;
  switch (s->kind) {
  case NY_S_IMPL:
    ny_register_tagged_type(cg, s->as.impl.type_name, s->tok);
    break;
  case NY_S_LEMMA:
    break;
  case NY_S_MODULE:
    for (size_t i = 0; i < s->as.module.body.len; i++)
      ny_register_impl_types_stmt(cg, s->as.module.body.data[i]);
    break;
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; i++)
      ny_register_impl_types_stmt(cg, s->as.block.body.data[i]);
    break;
  case NY_S_IF:
    ny_register_impl_types_stmt(cg, s->as.iff.conseq);
    ny_register_impl_types_stmt(cg, s->as.iff.alt);
    break;
  case NY_S_GUARD:
    ny_register_impl_types_stmt(cg, s->as.guard.fallback);
    break;
  default:
    break;
  }
}

void codegen_prepare(codegen_t *cg) {
  if (!cg || !cg->prog || cg->is_preparing)
    return;

  cg->is_preparing = true;
  NY_COMPILER_ASSERT(cg->module != NULL, "codegen_prepare missing LLVM module");
  NY_COMPILER_ASSERT(cg->builder != NULL,
                     "codegen_prepare missing LLVM builder");
  NY_COMPILER_ASSERT(cg->prog->body.len <= cg->prog->body.cap,
                     "codegen_prepare program body vector len exceeds cap");
  NY_COMPILER_ASSERT(cg->prog->body.data != NULL || cg->prog->body.len == 0,
                     "codegen_prepare program body vector has len but no data");
  NY_COMPILER_ASSERT(cg->extra_progs.len <= cg->extra_progs.cap,
                     "codegen_prepare extra_progs vector len exceeds cap");
  NY_COMPILER_ASSERT(cg->extra_progs.data != NULL || cg->extra_progs.len == 0,
                     "codegen_prepare extra_progs vector has len but no data");

  if (cg->debug_symbols) {
    stmt_t *first_stmt =
        (cg->prog->body.len > 0) ? cg->prog->body.data[0] : NULL;
    NY_COMPILER_ASSERT(first_stmt != NULL || cg->prog->body.len == 0,
                       "codegen_prepare first top-level statement is null");
    const char *main_file =
        (cg->debug_main_file && *cg->debug_main_file)
            ? cg->debug_main_file
            : (first_stmt ? first_stmt->tok.filename : "<inline>");

    bool inline_source = !main_file || !*main_file || main_file[0] == '<' ||
                         strcmp(main_file, "-") == 0;
    if (inline_source && cg->user_source && cg->user_source_len > 0) {
      char inline_file[PATH_MAX];
      char inline_name[64];
      snprintf(inline_name, sizeof(inline_name), "ny_inline_%ld.ny",
               (long)getpid());
      ny_join_path(inline_file, sizeof(inline_file), ny_get_temp_dir(),
                   inline_name);
      FILE *f = fopen(inline_file, "w");
      if (f) {
        fwrite(cg->user_source, 1, cg->user_source_len, f);
        fclose(f);

        static char abs_inline[4096];
        if (ny_realpath(inline_file, abs_inline)) {
          main_file = abs_inline;
        } else {
          main_file = inline_file;
        }

        size_t start = cg->prog->body.len > 20 ? cg->prog->body.len - 20 : 0;
        for (size_t i = start; i < cg->prog->body.len; i++) {
          stmt_t *s = cg->prog->body.data[i];
          NY_COMPILER_ASSERTF(s != NULL,
                              "null debug inline-source stmt at index %zu", i);
          if (!s)
            continue;
          const char *fn = s->tok.filename;
          if (fn) {
            s->tok.filename = main_file;
          }
        }
      }
    }
    codegen_debug_init(cg, main_file);
  }

  for (size_t i = 0; i < cg->prog->body.len; i++) {
    cg->current_module_name = NULL;
    stmt_t *s = cg->prog->body.data[i];
    NY_COMPILER_ASSERTF(s != NULL, "null top-level statement at index %zu", i);
    if (!s)
      continue;
    collect_use_aliases(cg, s);
    collect_use_modules(cg, s);
  }

  for (size_t i = 0; i < cg->prog->body.len; i++) {
    stmt_t *s = cg->prog->body.data[i];
    NY_COMPILER_ASSERTF(s != NULL, "null impl registration stmt at index %zu",
                        i);
    if (!s)
      continue;
    ny_register_impl_types_stmt(cg, s);
  }

  {
    struct timespec ts;
    int64_t deadline = 0;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
      deadline = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec + 15000000000LL;
    ny_ffi_set_global_deadline(deadline);
  }
  for (size_t i = 0; i < cg->prog->body.len; i++) {
    cg->current_module_name = NULL;
    stmt_t *s = cg->prog->body.data[i];
    NY_COMPILER_ASSERTF(s != NULL,
                        "null signature collection stmt at index %zu", i);
    if (!s)
      continue;
    collect_sigs(cg, s);
  }
  ny_ffi_set_global_deadline(0);

  process_default_core_imports(cg);

  for (size_t i = 0; i < cg->prog->body.len; i++) {
    cg->current_module_name = NULL;
    stmt_t *s = cg->prog->body.data[i];
    NY_COMPILER_ASSERTF(s != NULL, "null import processing stmt at index %zu",
                        i);
    if (!s)
      continue;
    process_use_imports(cg, s);
  }

  for (size_t i = 0; i < cg->prog->body.len; i++) {
    cg->current_module_name = NULL;
    stmt_t *s = cg->prog->body.data[i];
    NY_COMPILER_ASSERTF(s != NULL, "null export processing stmt at index %zu",
                        i);
    if (!s)
      continue;
    process_exports(cg, s);
  }

  ny_build_link_allowed_modules(cg);
  for (size_t i = 0; i < cg->prog->body.len; i++) {
    stmt_t *s = cg->prog->body.data[i];
    NY_COMPILER_ASSERTF(s != NULL, "null link processing stmt at index %zu", i);
    if (!s)
      continue;
    process_links(cg, s, NULL);
  }
  for (size_t p = 0; p < cg->extra_progs.len; p++) {
    program_t *prog = cg->extra_progs.data[p];
    if (!prog)
      continue;
    NY_COMPILER_ASSERTF(prog->body.len <= prog->body.cap,
                        "extra program %zu body vector len exceeds cap", p);
    NY_COMPILER_ASSERTF(prog->body.data != NULL || prog->body.len == 0,
                        "extra program %zu body vector has len but no data", p);
    for (size_t i = 0; i < prog->body.len; i++) {
      stmt_t *s = prog->body.data[i];
      NY_COMPILER_ASSERTF(s != NULL,
                          "null extra-program link stmt p=%zu index=%zu", p, i);
      if (!s)
        continue;
      process_links(cg, s, NULL);
    }
  }

  if (cg->effect_interfaces.len > 0) {
    char effect_error[256] = {0};
    if (!ny_effect_interface_apply(cg, effect_error, sizeof(effect_error))) {
      NY_LOG_ERR("Effect interface import failed: %s\n", effect_error);
      cg->had_error = 1;
    }
  }

  infer_pure_functions(cg);

  cg->is_preparing = false;
}

void codegen_repopulate_interns(codegen_t *cg) {
  if (!cg || !cg->module)
    return;

  for (LLVMValueRef g = LLVMGetFirstGlobal(cg->module); g;
       g = LLVMGetNextGlobal(g)) {
    const char *name = LLVMGetValueName(g);
    if (!name)
      continue;

    if (strncmp(name, ".str.runtime.", 13) == 0) {
      const char *suffix = name + 13;
      char data_name[256];
      snprintf(data_name, sizeof(data_name), ".str.data.%s", suffix);
      LLVMValueRef dg = ny_get_global(cg, data_name);
      if (!dg) {
        const char *dot = strchr(suffix, '.');
        if (dot && dot > suffix) {
          size_t base_len = (size_t)(dot - suffix);
          if (base_len < sizeof(data_name) - sizeof(".str.data.")) {
            snprintf(data_name, sizeof(data_name), ".str.data.%.*s",
                     (int)base_len, suffix);
            dg = ny_get_global(cg, data_name);
          }
        }
      }
      if (dg) {
        bool exists = false;
        for (size_t i = 0; i < cg->interns.len; i++) {
          string_intern *old = &cg->interns.data[i];
          if (old->val == g) {
            exists = true;
            break;
          }
        }
        if (exists)
          continue;
        string_intern in = {0};
        in.gv = dg;
        in.val = g;
        in.module = cg->module;
        vec_push(&cg->interns, in);
      }
    }
  }
}

void codegen_rebind_llvm_symbols(codegen_t *cg) {
  if (!cg || !cg->module)
    return;
  for (size_t i = 0; i < cg->fun_sigs.len; i++) {
    fun_sig *sig = &cg->fun_sigs.data[i];
    const char *llvm_name =
        (sig->llvm_name && *sig->llvm_name)
            ? sig->llvm_name
            : ((sig->link_name && *sig->link_name) ? sig->link_name
                                                   : sig->name);
    if (!llvm_name || !*llvm_name)
      continue;
    LLVMValueRef fn = LLVMGetNamedFunction(cg->module, llvm_name);
    if (!fn)
      continue;
    sig->value = fn;
    sig->type = LLVMGlobalGetValueType(fn);
  }
  for (size_t i = 0; i < cg->global_vars.len; i++) {
    binding *b = &cg->global_vars.data[i];
    if (!b->name || !*b->name)
      continue;
    LLVMValueRef gv = LLVMGetNamedGlobal(cg->module, b->name);
    if (gv)
      b->value = gv;
  }
}

void codegen_export_extern_link_names(codegen_t *cg) {
  /*
   * Extern declarations declared with `as "cname"` carry their linker symbol
   * in link_name, but the LLVM value is emitted under the Nytrix name (for
   * example `core.os.disasm._cs_version`). Calls already reference the value
   * handle, so renaming the value right before object emission makes the
   * emitted relocation match the C symbol that the linked library exports.
   * Skip a rename when the linker name is already claimed by a different
   * value; the old (broken) mapping is kept instead of inventing a new one.
   */
  if (!cg || !cg->module)
    return;
  for (size_t i = 0; i < cg->fun_sigs.len; i++) {
    fun_sig *sig = &cg->fun_sigs.data[i];
    if (!sig->is_extern || !sig->link_name || !*sig->link_name)
      continue;
    const char *llvm_name =
        (sig->llvm_name && *sig->llvm_name) ? sig->llvm_name : sig->name;
    if (!llvm_name || !*llvm_name || strcmp(llvm_name, sig->link_name) == 0)
      continue;
    LLVMValueRef fn = LLVMGetNamedFunction(cg->module, llvm_name);
    if (!fn)
      continue;
    LLVMValueRef clash = LLVMGetNamedFunction(cg->module, sig->link_name);
    if (clash && clash != fn)
      continue;
    LLVMSetValueName2(fn, sig->link_name, strlen(sig->link_name));
  }
}
