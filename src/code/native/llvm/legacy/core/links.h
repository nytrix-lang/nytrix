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
    if (ny_stmt_is_bare_std_use(s)) {
      vec_push(out, (char *)"std.core");
      vec_push(out, (char *)"std.os.prim");
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
  if (queue.len == 0 && cg->current_module_name &&
      strncmp(cg->current_module_name, "std.", 4) == 0) {
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
  if (strncmp(mod, "std.", 4) == 0)
    return true;
  if (strncmp(mod, "lib.", 4) != 0)
    return true;
  if (cg->link_allowed_modules.len == 0 && !cg->current_module_name)
    return true;
  if (cg->link_allowed_modules.len == 0 && cg->current_module_name &&
      strncmp(cg->current_module_name, "std.", 4) == 0)
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
