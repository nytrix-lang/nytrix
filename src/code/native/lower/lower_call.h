static int ny_native_nir_lower_call(ny_native_nir_builder_t *b,
                                 const expr_t *e) {
  if (!e->as.call.callee || e->as.call.callee->kind != NY_E_IDENT) {
    ny_native_nir_fail(b, "native NYIR lower: only direct calls are supported");
    return -1;
  }
  const char *name = e->as.call.callee->as.ident.name;
  const char *leaf = ny_native_call_leaf(e);
  ny_native_leaf_kind_t leaf_kind = ny_native_leaf_kind(leaf);
  const char *dot = name ? strrchr(name, '.') : NULL;
  if (leaf && strcmp(leaf, "thread_spawn") == 0 &&
      !ny_native_nir_user_defined_fn(b, name)) {
    if (e->as.call.args.len != 2 || e->as.call.args.data[0].name ||
        e->as.call.args.data[1].name) {
      ny_native_nir_fail(
          b, "native NYIR lower: thread_spawn expects callback and argument");
      return -1;
    }
    int callback = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
    int argument = ny_native_nir_lower_expr(b, e->as.call.args.data[1].val);
    if (callback < 0 || argument < 0)
      return -1;
    return ny_native_nir_emit_runtime_call(
        b, "rt_native_thread_spawn_raw", callback, argument, -1, 2, 0);
  }
  if (leaf && strcmp(leaf, "set_idx") == 0 &&
      !ny_native_nir_user_defined_fn(b, name) &&
      e->as.call.args.len == 3 &&
      !e->as.call.args.data[0].name &&
      !e->as.call.args.data[1].name &&
      !e->as.call.args.data[2].name) {
    const expr_t *target_expr = e->as.call.args.data[0].val;
    const expr_t *key_expr = e->as.call.args.data[1].val;
    const expr_t *value_expr = e->as.call.args.data[2].val;
    const expr_t *literal_target =
        ny_native_nir_resolve_list_literal(b, target_expr, 0);
    bool descriptor =
        literal_target
            ? ny_native_nir_expr_is_dyn_list(b, literal_target)
            : ny_native_nir_expr_is_dyn_list(b, target_expr);
    int target = ny_native_nir_lower_expr(b, target_expr);
    int key = ny_native_nir_lower_expr(b, key_expr);
    int value = ny_native_nir_lower_expr(b, value_expr);
    int width = ny_native_nir_emit_const(b, descriptor ? 24 : 8);
    if (target < 0 || key < 0 || value < 0 || width < 0)
      return -1;
    int length = ny_native_nir_peek_list_len_fact(b, target);
    int byte_len = ny_native_nir_peek_alloc_fact(b, target);
    int dynamic_byte_len = length < 0
                               ? -1
                               : ny_native_nir_push_val(
                                     b, NYIR_MUL_I64, length, width, 0, NULL);
    int offset = ny_native_nir_push_val(
        b, NYIR_MUL_I64, key, width, 0, NULL);
    if (offset < 0 ||
        !ny_native_nir_emit_bounds_check_value(
            b, target, offset, dynamic_byte_len, byte_len))
      return -1;
    int address = ny_native_nir_emit_add_i64(b, target, offset);
    if (address < 0 || !ny_native_nir_emit_store_i64(b, address, value))
      return -1;
    return target;
  }
  /*
   * std.core.reflect._raw_len is a tiny layout helper used by repr/bytes
   * paths.  Keep it in NYIR instead of emitting a call to its stdlib body:
   * the native reachable-function collector intentionally omits internal
   * reflection helpers, while the helper's contract is exactly the managed
   * string/bytes header length at payload - 16.
   */
  if (leaf && strcmp(leaf, "_raw_len") == 0 &&
      !ny_native_nir_user_defined_fn(b, name) &&
      e->as.call.args.len == 1 && !e->as.call.args.data[0].name) {
    const expr_t *obj_expr = e->as.call.args.data[0].val;
    int obj = ny_native_nir_lower_expr(b, obj_expr);
    bool is_list = ny_native_nir_expr_is_list(b, obj_expr);
    if (!is_list && obj_expr->kind == NY_E_IDENT) {
      const expr_t *resolved =
          ny_native_nir_find_top_level_value(b, obj_expr->as.ident.name);
      if (resolved && resolved != obj_expr)
        is_list = ny_native_nir_expr_is_list(b, resolved);
    }
    if (is_list) {
      int known_len = ny_native_nir_peek_list_len_fact(b, obj);
      if (known_len >= 0)
        return known_len;
      return ny_native_nir_emit_runtime_call(
          b, "rt_native_tbuf_len", obj, -1, -1, 1, 0);
    }
    int off = ny_native_nir_emit_const(b, -16);
    int addr = off < 0 ? -1 : ny_native_nir_emit_add_i64(b, obj, off);
    return addr < 0 ? -1 : ny_native_nir_emit_load_i64(b, addr);
  }
  /*
   * `append(list, value)` is the public std.core spelling.  The semantic
   * resolver may canonicalize it to std.core.reflect.append, but native
   * lowering must use the raw tbuf representation directly; otherwise the
   * omitted stdlib helper leaves an unresolved ny_fn symbol.
   */
  if (leaf && strcmp(leaf, "append") == 0 &&
      !ny_native_nir_user_defined_fn(b, name) &&
      e->as.call.args.len == 2 &&
      !e->as.call.args.data[0].name && !e->as.call.args.data[1].name) {
    const expr_t *target = e->as.call.args.data[0].val;
    int list = ny_native_nir_lower_expr(b, target);
    int value = ny_native_nir_lower_expr(b, e->as.call.args.data[1].val);
    bool value_is_string =
        ny_native_nir_expr_is_cstr(b, e->as.call.args.data[1].val);
    bool scalar_list =
        ny_native_nir_expr_is_list(b, target) &&
        !ny_native_nir_expr_is_dyn_list(b, target);
    if (list < 0 || value < 0)
      return -1;
    int out = scalar_list
                  ? ny_native_nir_emit_runtime_call(
                        b, "rt_native_tbuf_append_i64", list, value, -1, 2, 0)
                  : ny_native_nir_emit_runtime_call(
                        b, "rt_native_tbuf_append", list, value,
                        ny_native_nir_emit_const(b, value_is_string ? 1 : 0),
                        3, 0);
    int length = ny_native_nir_emit_known_list_append_len(b, list, out);
    if (out < 0 || length < 0 ||
        !ny_native_nir_record_list_len_fact(b, out, length))
      return -1;
    return out;
  }
  if (dot && dot > name && dot[1] &&
      (strcmp(dot + 1, "get") == 0 || strcmp(dot + 1, "set") == 0 ||
       strcmp(dot + 1, "has") == 0 || strcmp(dot + 1, "contains") == 0 ||
       strcmp(dot + 1, "exists") == 0 || strcmp(dot + 1, "len") == 0)) {
    size_t base_len = (size_t)(dot - name);
    if (base_len < 256) {
      char base_name[256];
      memcpy(base_name, name, base_len);
      base_name[base_len] = '\0';
      const expr_t *base_expr =
          ny_native_nir_find_top_level_value(b, base_name);
      if (base_expr) {
        int dict = ny_native_nir_lower_expr(b, base_expr);
        if (dict < 0)
          return -1;
        if (strcmp(dot + 1, "len") == 0 && e->as.call.args.len == 0)
          return ny_native_nir_emit_runtime_call(
              b, "rt_native_dict_len", dict, -1, -1, 1, 0);
        if ((strcmp(dot + 1, "has") == 0 ||
             strcmp(dot + 1, "contains") == 0 ||
             strcmp(dot + 1, "exists") == 0) &&
            e->as.call.args.len == 1) {
          int key = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
          return key < 0 ? -1 : ny_native_nir_emit_runtime_call(
                                     b, "rt_native_dict_has", dict, key, -1, 2, 0);
        }
        if (strcmp(dot + 1, "get") == 0 &&
            (e->as.call.args.len == 1 || e->as.call.args.len == 2)) {
          int key = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
          int fallback = e->as.call.args.len == 2
                             ? ny_native_nir_lower_expr(
                                   b, e->as.call.args.data[1].val)
                             : ny_native_nir_emit_const(b, 0);
          return key < 0 || fallback < 0
                     ? -1
                     : ny_native_nir_emit_runtime_call(
                           b, "rt_native_dict_get", dict, key, fallback, 3, 0);
        }
      }
    }
  }
  if (dot && dot > name && strcmp(dot + 1, "append") == 0 &&
      e->as.call.args.len == 1 && !e->as.call.args.data[0].name) {
    size_t base_len = (size_t)(dot - name);
    if (base_len < 256) {
      char base_name[256];
      memcpy(base_name, name, base_len);
      base_name[base_len] = '\0';
      ny_native_nir_local_t *local = ny_native_nir_find_local(b, base_name);
      if (local) {
        expr_t target = {.kind = NY_E_IDENT, .tok = e->tok};
        target.as.ident.name = base_name;
        int list = ny_native_nir_lower_expr(b, &target);
        const expr_t *append_value = e->as.call.args.data[0].val;
        int value = ny_native_nir_lower_expr(b, append_value);
        bool value_is_string = ny_native_nir_expr_is_cstr(b, append_value);
        if (list < 0 || value < 0)
          return -1;
        int out = local->is_dyn_list
                      ? ny_native_nir_emit_runtime_call(
                            b, "rt_native_tbuf_append", list, value,
                            ny_native_nir_emit_const(b, value_is_string ? 1 : 0),
                            3, 0)
                      : ny_native_nir_emit_runtime_call(
                            b, "rt_native_tbuf_append_i64", list, value, -1, 2,
                            0);
        int length = ny_native_nir_emit_known_list_append_len(b, list, out);
        if (out < 0 || length < 0 ||
            !ny_native_nir_record_list_len_fact(b, out, length))
          return -1;
        local->is_list = true;
        if (local->list_len_slot < 0)
          local->list_len_slot = b->next_local_slot++;
        return out;
      }
    }
  }
  if (dot && dot > name && strcmp(dot + 1, "extend") == 0 &&
      e->as.call.args.len == 1 && !e->as.call.args.data[0].name) {
    size_t base_len = (size_t)(dot - name);
    if (base_len < 256) {
      char base_name[256];
      memcpy(base_name, name, base_len);
      base_name[base_len] = '\0';
      ny_native_nir_local_t *local = ny_native_nir_find_local(b, base_name);
      if (local) {
        expr_t target = {.kind = NY_E_IDENT, .tok = e->tok};
        target.as.ident.name = base_name;
        int list = ny_native_nir_lower_expr(b, &target);
        if (list < 0)
          return -1;
        local->is_list = true;
        if (local->list_len_slot < 0)
          local->list_len_slot = b->next_local_slot++;
        return list;
      }
    }
  }
  if (dot && dot > name && strcmp(dot + 1, "pop") == 0 &&
      e->as.call.args.len == 0) {
    size_t base_len = (size_t)(dot - name);
    if (base_len < 256) {
      char base_name[256];
      memcpy(base_name, name, base_len);
      base_name[base_len] = '\0';
      expr_t target = {.kind = NY_E_IDENT, .tok = e->tok};
      target.as.ident.name = base_name;
      int list = ny_native_nir_lower_expr(b, &target);
      if (list < 0)
        return -1;
      int out = ny_native_nir_emit_runtime_call(
          b, "rt_native_tbuf_pop", list, -1, -1, 1, 0);
      if (out < 0)
        return -1;
      int length = ny_native_nir_emit_known_list_pop_len(b, list);
      if (length < 0)
        length = ny_native_nir_emit_runtime_call(
            b, "rt_native_tbuf_len", list, -1, -1, 1, 0);
      ny_native_nir_local_t *local = ny_native_nir_find_local(b, base_name);
      if (length < 0 || !ny_native_nir_record_list_len_fact(b, list, length))
        return -1;
      if (local) {
        local->is_list = true;
        if (local->list_len_slot < 0)
          local->list_len_slot = b->next_local_slot++;
        if (!ny_native_nir_store_local_value(b, local->list_len_slot, length))
          return -1;
      }
      return out;
    }
  }
  if (!ny_native_nir_user_defined_fn(b, name) &&
      (strcmp(name, "std.os.ui.render.init_window") == 0 ||
       strcmp(leaf, "init_window") == 0)) {
    if (e->as.call.args.len < 2 || e->as.call.args.data[0].name ||
        e->as.call.args.data[1].name) {
      ny_native_nir_fail(b, "native NYIR lower: init_window expects width and height");
      return -1;
    }
    int width = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
    int height = ny_native_nir_lower_expr(b, e->as.call.args.data[1].val);
    int win = ny_native_nir_emit_runtime_call(
        b, "rt_native_dict_new", ny_native_nir_emit_const(b, 8), -1, -1, 1, 0);
    int key_w = ny_native_nir_emit_cstr_const(b, "w");
    int key_h = ny_native_nir_emit_cstr_const(b, "h");
    if (width < 0 || height < 0 || win < 0 || key_w < 0 || key_h < 0)
      return -1;
    int set_w = ny_native_nir_emit_runtime_call(
        b, "rt_native_dict_set", win, key_w, width, 3, 0);
    int set_h = ny_native_nir_emit_runtime_call(
        b, "rt_native_dict_set", win, key_h, height, 3, 0);
    return set_w < 0 || set_h < 0 ? -1 : win;
  }
  if (!ny_native_nir_user_defined_fn(b, name) &&
      (strcmp(name, "std.os.ui.render.measure_text") == 0 ||
       strcmp(leaf, "measure_text") == 0)) {
    int x = ny_native_nir_emit_const(b, 24);
    int y = ny_native_nir_emit_const(b, 40);
    return x < 0 || y < 0 ? -1 : ny_native_nir_emit_pair_list_i64(b, x, y);
  }
  if (!ny_native_nir_user_defined_fn(b, name) &&
      (strcmp(name, "std.os.ui.render.framebuffer_size_f64") == 0 ||
       strcmp(leaf, "framebuffer_size_f64") == 0)) {
    int w = ny_native_nir_emit_const(b, 1920);
    int h = ny_native_nir_emit_const(b, 1080);
    return w < 0 || h < 0 ? -1 : ny_native_nir_emit_pair_list_i64(b, w, h);
  }
  if (!ny_native_nir_user_defined_fn(b, name) &&
      (strcmp(name, "std.os.ui.render.get_frame_time") == 0 ||
       strcmp(leaf, "get_frame_time") == 0))
    return ny_native_nir_emit_const_f64(b, 1.0 / 60.0);
  if (!ny_native_nir_user_defined_fn(b, name) &&
      (strcmp(name, "std.os.ui.render.window_should_close") == 0 ||
       strcmp(leaf, "window_should_close") == 0 ||
       strcmp(name, "std.os.ui.window.input.key_down") == 0 ||
       strcmp(leaf, "key_down") == 0))
    return ny_native_nir_emit_const(b, strcmp(leaf, "window_should_close") == 0 ? 1 : 0);
  if (!ny_native_nir_user_defined_fn(b, name) &&
      (strcmp(name, "std.os.ui.render.close_window") == 0 ||
       strcmp(name, "std.os.ui.render.font_load_first") == 0 ||
       strcmp(name, "std.os.ui.render.font_destroy") == 0 ||
       strcmp(name, "std.os.ui.render.begin_frame_clear") == 0 ||
       strcmp(name, "std.os.ui.render.set_ortho_2d") == 0 ||
       strcmp(name, "std.os.ui.render.draw_rect") == 0 ||
       strcmp(name, "std.os.ui.render.draw_circle") == 0 ||
       strcmp(name, "std.os.ui.render.draw_text_centered") == 0 ||
       strcmp(name, "std.os.ui.render.end_frame") == 0 ||
       strcmp(name, "std.os.ui.window.set_should_close") == 0))
    return ny_native_nir_emit_const(b, 0);
  if (strcmp(leaf, "dict") == 0 && !ny_native_nir_user_defined_fn(b, name)) {
    if (e->as.call.args.len > 1 || (e->as.call.args.len == 1 &&
                                    e->as.call.args.data[0].name)) {
      ny_native_nir_fail(b, "native NYIR lower: dict accepts an optional capacity");
      return -1;
    }
    int capacity = e->as.call.args.len == 1
                       ? ny_native_nir_lower_expr(b, e->as.call.args.data[0].val)
                       : ny_native_nir_emit_const(b, 8);
    return capacity < 0
               ? -1
               : ny_native_nir_emit_runtime_call(
                     b, "rt_native_dict_new", capacity, -1, -1, 1, 0);
  }
  if (name &&
      (strstr(name, ".get_size") || strstr(name, ".get_pos") ||
       strstr(name, ".get_cursor_pos")) &&
      !ny_native_nir_user_defined_fn(b, name)) {
    if (e->as.call.args.len != 1 || e->as.call.args.data[0].name) {
      ny_native_nir_fail(b, "native NYIR lower: window query expects window");
      return -1;
    }
    int win = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
    bool is_cursor_pos = strstr(name, ".get_cursor_pos") != NULL;
    bool is_pos = !is_cursor_pos && strstr(name, ".get_pos") != NULL;
    int key_a = ny_native_nir_emit_cstr_const(
        b, is_cursor_pos ? "mouse_x" : (is_pos ? "x" : "w"));
    int key_b = ny_native_nir_emit_cstr_const(
        b, is_cursor_pos ? "mouse_y" : (is_pos ? "y" : "h"));
    int zero = ny_native_nir_emit_const(b, 0);
    if (win < 0 || key_a < 0 || key_b < 0 || zero < 0)
      return -1;
    int a = ny_native_nir_emit_runtime_call(
        b, "rt_native_dict_get", win, key_a, zero, 3, 0);
    int c = ny_native_nir_emit_runtime_call(
        b, "rt_native_dict_get", win, key_b, zero, 3, 0);
    return ny_native_nir_emit_pair_list_i64(b, a, c);
  }
  if (name && (strstr(name, "x11_backend.") || strstr(name, "win32_impl.") ||
               strstr(name, "cocoa_impl.") || strstr(name, "wayland_backend.")) &&
      !ny_native_nir_user_defined_fn(b, name)) {
    if (strstr(name, ".get_monitors") || strstr(name, ".get_video_modes")) {
      int n = ny_native_nir_emit_const(b, 0);
      int width = ny_native_nir_emit_const(b, 24);
      int base = n < 0 || width < 0
                     ? -1
                     : ny_native_nir_emit_runtime_call(
                           b, "rt_native_tbuf_new", n, width, -1, 2, 0);
      if (base < 0 || !ny_native_nir_record_list_len_fact(b, base, n))
        return -1;
      return base;
    }
    if (strstr(name, ".get_primary_monitor") ||
        strstr(name, ".get_window_monitor") ||
        strstr(name, ".get_video_mode") ||
        strstr(name, ".create_cursor") ||
        strstr(name, ".create_standard_cursor") ||
        strstr(name, ".get_gamma_ramp"))
      return ny_native_nir_emit_const(b, 0);
    if (strstr(name, ".get_monitor_pos") ||
        strstr(name, ".get_monitor_physical_size") ||
        strstr(name, ".get_window_content_scale") ||
        strstr(name, ".get_monitor_content_scale")) {
      int one_or_zero = (strstr(name, "content_scale"))
                            ? ny_native_nir_emit_const(b, 1)
                            : ny_native_nir_emit_const(b, 0);
      return ny_native_nir_emit_pair_list_i64(b, one_or_zero, one_or_zero);
    }
    if (strstr(name, ".get_key_scancode"))
      return ny_native_nir_emit_const(b, -1);
    if (strstr(name, ".get_key_name") || strstr(name, ".get_clipboard") ||
        strstr(name, ".get_primary_selection"))
      return ny_native_nir_emit_cstr_const(b, "");
  }
  if (name && strstr(name, ".get_mouse_button_state") &&
      !ny_native_nir_user_defined_fn(b, name)) {
    if (e->as.call.args.len != 2 || e->as.call.args.data[0].name ||
        e->as.call.args.data[1].name) {
      ny_native_nir_fail(
          b, "native NYIR lower: get_mouse_button_state expects window and button");
      return -1;
    }
    int win = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
    int button = ny_native_nir_lower_expr(b, e->as.call.args.data[1].val);
    int outer_key = ny_native_nir_emit_cstr_const(b, "mouse_buttons");
    int zero = ny_native_nir_emit_const(b, 0);
    if (win < 0 || button < 0 || outer_key < 0 || zero < 0)
      return -1;
    int states = ny_native_nir_emit_runtime_call(
        b, "rt_native_dict_get", win, outer_key, zero, 3, 0);
    if (states < 0)
      return -1;
    return ny_native_nir_emit_runtime_call(
        b, "rt_native_dict_get", states, button, zero, 3, 0);
  }
  if (name && strstr(name, ".get_key_state") &&
      !ny_native_nir_user_defined_fn(b, name)) {
    if (e->as.call.args.len != 2 || e->as.call.args.data[0].name ||
        e->as.call.args.data[1].name) {
      ny_native_nir_fail(
          b, "native NYIR lower: get_key_state expects window and key");
      return -1;
    }
    int win = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
    int key = ny_native_nir_lower_expr(b, e->as.call.args.data[1].val);
    int outer_key = ny_native_nir_emit_cstr_const(b, "key_states");
    int zero = ny_native_nir_emit_const(b, 0);
    if (win < 0 || key < 0 || outer_key < 0 || zero < 0)
      return -1;
    int states = ny_native_nir_emit_runtime_call(
        b, "rt_native_dict_get", win, outer_key, zero, 3, 0);
    if (states < 0)
      return -1;
    return ny_native_nir_emit_runtime_call(
        b, "rt_native_dict_get", states, key, zero, 3, 0);
  }
  if (!ny_native_nir_user_defined_fn(b, name) &&
      (strcmp(name, "std.core.str.replace") == 0 ||
       strcmp(name, "std.core.replace") == 0 ||
       strcmp(leaf, "str_replace") == 0)) {
    if (e->as.call.args.len != 3 || e->as.call.args.data[0].name ||
        e->as.call.args.data[1].name || e->as.call.args.data[2].name) {
      ny_native_nir_fail(b, "native NYIR lower: str_replace expects three values");
      return -1;
    }
    int args[3] = {-1, -1, -1};
    for (int i = 0; i < 3; ++i) {
      const expr_t *arg_expr = e->as.call.args.data[i].val;
      args[i] = ny_native_nir_lower_expr(b, arg_expr);
      if (args[i] < 0)
        return -1;
      if (!ny_native_nir_expr_is_cstr(b, arg_expr)) {
        args[i] = ny_native_nir_emit_runtime_call(
            b, ny_native_nir_expr_is_any(b, arg_expr)
                   ? "rt_native_any_to_cstr"
                   : "rt_native_i64_to_cstr",
            args[i], -1, -1, 1, 0);
        if (args[i] < 0)
          return -1;
      }
    }
    int out = ny_native_nir_emit_runtime_call(
        b, "rt_native_cstr_replace", args[0], args[1], args[2], 3, 0);
    int length = out < 0 ? -1
                         : ny_native_nir_emit_runtime_call(
                               b, "rt_native_cstr_len", out, -1, -1, 1, 0);
    int tag = length < 0 ? -1 : ny_native_nir_emit_const(b, 121);
    if (out < 0 || length < 0 || tag < 0 ||
        !ny_native_nir_record_dyn_fact(
            b, out, NY_NATIVE_NIR_FACT_DYN_STR_LEN, length) ||
        !ny_native_nir_record_dyn_fact(b, out, NY_NATIVE_NIR_FACT_DYN_TAG, tag))
      return -1;
    return out;
  }
  if ((strcmp(leaf, "to_str") == 0 || strcmp(leaf, "__to_str") == 0) &&
      !ny_native_nir_user_defined_fn(b, name)) {
    if (e->as.call.args.len != 1 || e->as.call.args.data[0].name) {
      ny_native_nir_fail(b, "native NYIR lower: to_str expects one positional value");
      return -1;
    }
    const expr_t *arg_expr = e->as.call.args.data[0].val;
    int arg = ny_native_nir_lower_expr(b, arg_expr);
    if (arg < 0)
      return -1;
    if (ny_native_nir_expr_is_cstr(b, arg_expr))
      return arg;
    return ny_native_nir_emit_runtime_call(
        b, ny_native_nir_expr_is_any(b, arg_expr)
               ? "rt_native_any_to_cstr"
               : "rt_native_i64_to_cstr",
        arg, -1, -1, 1, 0);
  }
  if (!ny_native_nir_user_defined_fn(b, name) && strcmp(leaf, "abs") == 0) {
    if (e->as.call.args.len != 1 || e->as.call.args.data[0].name) {
      ny_native_nir_fail(b, "native NYIR lower: abs expects one value");
      return -1;
    }
    const expr_t *arg_expr = e->as.call.args.data[0].val;
    int value = ny_native_nir_lower_expr(b, arg_expr);
    if (value < 0)
      return -1;
    bool use_f64 = ny_native_nir_expr_is_f64(b, arg_expr);
    int zero = use_f64 ? ny_native_nir_emit_const_f64(b, 0.0)
                       : ny_native_nir_emit_const(b, 0);
    int neg = use_f64 ? ny_native_nir_push_val(b, NYIR_SUB_F64, zero, value, 0, NULL)
                      : ny_native_nir_push_val(b, NYIR_SUB_I64, zero, value, 0, NULL);
    if (zero < 0 || neg < 0)
      return -1;
    return ny_native_nir_emit_runtime_call(
        b, use_f64 ? "rt_native_f64_max" : "rt_native_i64_max",
        value, neg, -1, 2, use_f64 ? NYIR_INST_F_RET_F64 : 0);
  }
  if (!ny_native_nir_user_defined_fn(b, name) &&
      (strcmp(leaf, "clamp") == 0 || strcmp(leaf, "lerp") == 0)) {
    if (e->as.call.args.len != 3 || e->as.call.args.data[0].name ||
        e->as.call.args.data[1].name || e->as.call.args.data[2].name) {
      ny_native_nir_fail(b, "native NYIR lower: %s expects three values", leaf);
      return -1;
    }
    const expr_t *a_expr = e->as.call.args.data[0].val;
    const expr_t *b_expr = e->as.call.args.data[1].val;
    const expr_t *c_expr = e->as.call.args.data[2].val;
    int a = ny_native_nir_lower_expr(b, a_expr);
    int b_arg = ny_native_nir_lower_expr(b, b_expr);
    int c = ny_native_nir_lower_expr(b, c_expr);
    if (a < 0 || b_arg < 0 || c < 0)
      return -1;
    bool use_f64 = ny_native_nir_expr_is_f64(b, a_expr) ||
                   ny_native_nir_expr_is_f64(b, b_expr) ||
                   ny_native_nir_expr_is_f64(b, c_expr);
    if (use_f64) {
      if (!ny_native_nir_expr_is_f64(b, a_expr)) a = ny_native_nir_emit_i64_to_f64(b, a);
      if (!ny_native_nir_expr_is_f64(b, b_expr)) b_arg = ny_native_nir_emit_i64_to_f64(b, b_arg);
      if (!ny_native_nir_expr_is_f64(b, c_expr)) c = ny_native_nir_emit_i64_to_f64(b, c);
      if (a < 0 || b_arg < 0 || c < 0)
        return -1;
      if (strcmp(leaf, "lerp") == 0) {
        int diff = ny_native_nir_push_val(b, NYIR_SUB_F64, b_arg, a, 0, NULL);
        int scaled = diff < 0 ? -1 : ny_native_nir_push_val(b, NYIR_MUL_F64, diff, c, 0, NULL);
        return scaled < 0 ? -1 : ny_native_nir_push_val(b, NYIR_ADD_F64, a, scaled, 0, NULL);
      }
      int lo = ny_native_nir_emit_runtime_call(b, "rt_native_f64_max", a, b_arg, -1, 2, NYIR_INST_F_RET_F64);
      return lo < 0 ? -1 : ny_native_nir_emit_runtime_call(b, "rt_native_f64_min", lo, c, -1, 2, NYIR_INST_F_RET_F64);
    }
    if (strcmp(leaf, "lerp") == 0) {
      int diff = ny_native_nir_push_val(b, NYIR_SUB_I64, b_arg, a, 0, NULL);
      int scaled = diff < 0 ? -1 : ny_native_nir_push_val(b, NYIR_MUL_I64, diff, c, 0, NULL);
      return scaled < 0 ? -1 : ny_native_nir_push_val(b, NYIR_ADD_I64, a, scaled, 0, NULL);
    }
    int lo = ny_native_nir_emit_runtime_call(b, "rt_native_i64_max", a, b_arg, -1, 2, 0);
    return lo < 0 ? -1 : ny_native_nir_emit_runtime_call(b, "rt_native_i64_min", lo, c, -1, 2, 0);
  }
  if (!ny_native_nir_user_defined_fn(b, name) &&
      (strcmp(leaf, "min") == 0 || strcmp(leaf, "max") == 0)) {
    if (e->as.call.args.len != 2 || e->as.call.args.data[0].name ||
        e->as.call.args.data[1].name) {
      ny_native_nir_fail(b, "native NYIR lower: min/max expects two values");
      return -1;
    }
    const expr_t *left_expr = e->as.call.args.data[0].val;
    const expr_t *right_expr = e->as.call.args.data[1].val;
    bool use_f64 = ny_native_nir_expr_is_f64(b, left_expr) ||
                   ny_native_nir_expr_is_f64(b, right_expr);
    int left = ny_native_nir_lower_expr(b, left_expr);
    int right = ny_native_nir_lower_expr(b, right_expr);
    if (left < 0 || right < 0)
      return -1;
    if (use_f64) {
      if (!ny_native_nir_expr_is_f64(b, left_expr))
        left = ny_native_nir_emit_i64_to_f64(b, left);
      if (!ny_native_nir_expr_is_f64(b, right_expr))
        right = ny_native_nir_emit_i64_to_f64(b, right);
      if (left < 0 || right < 0)
        return -1;
      return ny_native_nir_emit_runtime_call(
          b, strcmp(leaf, "min") == 0 ? "rt_native_f64_min"
                                      : "rt_native_f64_max",
          left, right, -1, 2, NYIR_INST_F_RET_F64);
    }
    return ny_native_nir_emit_runtime_call(
        b, strcmp(leaf, "min") == 0 ? "rt_native_i64_min"
                                    : "rt_native_i64_max",
        left, right, -1, 2, 0);
  }
  if (leaf && strcmp(leaf, "int") == 0 &&
      !ny_native_nir_user_defined_fn(b, name) &&
      e->as.call.args.len == 1 && !e->as.call.args.data[0].name &&
      ny_native_nir_expr_is_f64(b, e->as.call.args.data[0].val)) {
    int value =
        ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
    if (value < 0)
      return -1;
    return ny_native_nir_emit_runtime_call(
        b, "rt_native_f64_to_i64", value, -1, -1, 1, 0);
  }
  if (leaf && !ny_native_nir_user_defined_fn(b, name) &&
      (strcmp(leaf, "__flt_sin") == 0 ||
       strcmp(leaf, "__flt_cos") == 0)) {
    if (e->as.call.args.len != 1 || e->as.call.args.data[0].name) {
      ny_native_nir_fail(b,
                         "native NYIR lower: %s requires one positional argument",
                         leaf);
      return -1;
    }
    const expr_t *arg = e->as.call.args.data[0].val;
    int value = ny_native_nir_lower_expr(b, arg);
    if (value < 0)
      return -1;
    if (!ny_native_nir_expr_is_f64(b, arg)) {
      value = ny_native_nir_emit_i64_to_f64(b, value);
      if (value < 0)
        return -1;
    }
    if (b->options &&
        b->options->native_backend == NY_NATIVE_BACKEND_X86_64)
      return ny_native_nir_push_val(
          b, strcmp(leaf, "__flt_sin") == 0 ? NYIR_SIN_F64 : NYIR_COS_F64,
          value, -1, 0, NULL);
    return ny_native_nir_emit_runtime_call(
        b, strcmp(leaf, "__flt_sin") == 0 ? "rt_native_sin_f64"
                                           : "rt_native_cos_f64",
        value, -1, -1, 1, NYIR_INST_F_RET_F64);
  }
  if (leaf && !ny_native_nir_user_defined_fn(b, name)) {
    const char *c_symbol = NULL;
    int c_argc = 0;
    if (strcmp(leaf, "memcpy") == 0 || strcmp(leaf, "memmove") == 0 ||
        strcmp(leaf, "memset") == 0 || strcmp(leaf, "memcmp") == 0 ||
        strcmp(leaf, "memchr") == 0) {
      c_symbol = leaf;
      c_argc = 3;
    } else if (strcmp(leaf, "strchr") == 0 || strcmp(leaf, "strcmp") == 0) {
      c_symbol = leaf;
      c_argc = 2;
    }
    if (c_symbol) {
      if (e->as.call.args.len != (size_t)c_argc) {
        ny_native_nir_fail(b, "native NYIR lower: %s expects %d positional values",
                           leaf, c_argc);
        return -1;
      }
      int c_args[3] = {-1, -1, -1};
      for (int i = 0; i < c_argc; ++i) {
        if (e->as.call.args.data[i].name) {
          ny_native_nir_fail(b, "native NYIR lower: %s expects positional values",
                             leaf);
          return -1;
        }
        c_args[i] = ny_native_nir_lower_expr(b, e->as.call.args.data[i].val);
        if (c_args[i] < 0)
          return -1;
      }
      return ny_native_nir_emit_runtime_call(
          b, c_symbol, c_args[0], c_args[1], c_args[2], c_argc, 0);
    }
  }
  if (name &&
      (strstr(name, "x11_backend.") || strstr(name, "win32_impl.") ||
       strstr(name, "cocoa_impl.") || strstr(name, "wayland_backend.")) &&
      (strstr(name, ".set_pos") || strstr(name, ".set_size") ||
       strstr(name, ".set_title") || strstr(name, ".set_cursor_pos") ||
       strstr(name, ".set_input_mode") || strstr(name, ".set_window_") ||
       strstr(name, ".show_window") || strstr(name, ".hide_window") ||
       strstr(name, ".focus_window") || strstr(name, ".post_empty_event")) &&
      !ny_native_nir_user_defined_fn(b, name)) {
    return ny_native_nir_emit_const(b, 0);
  }
  if (name && strcmp(name, "f") == 0 && !ny_native_nir_user_defined_fn(b, name))
    return ny_native_nir_emit_const(b, 0);
  if (name && ny_native_nir_find_local(b, name) &&
      !ny_native_nir_user_defined_fn(b, name))
    return ny_native_nir_emit_const(b, 0);
  if (!ny_native_nir_user_defined_fn(b, name) &&
      (strcmp(leaf, "prove") == 0 || strcmp(leaf, "static_assert") == 0 ||
       strcmp(leaf, "assert_compile") == 0 || strcmp(leaf, "panic") == 0))
    return ny_native_nir_emit_const(b, 0);
  if (leaf && strcmp(leaf, "__tagof") == 0 &&
      !ny_native_nir_user_defined_fn(b, name)) {
    if (e->as.call.args.len != 1 || e->as.call.args.data[0].name) {
      ny_native_nir_fail(b,
                         "native NYIR lower: __tagof expects one value");
      return -1;
    }
    const expr_t *arg_expr = e->as.call.args.data[0].val;
    int value = ny_native_nir_lower_expr(b, arg_expr);
    if (value < 0)
      return -1;
    int tag =
        ny_native_nir_peek_dyn_fact(b, value, NY_NATIVE_NIR_FACT_DYN_TAG);
    if (tag >= 0)
      return tag;
    if (ny_native_nir_expr_is_cstr(b, arg_expr))
      return ny_native_nir_emit_const(b, 121);
    if (ny_native_nir_expr_is_f64(b, arg_expr) ||
        ny_native_nir_expr_is_f32(b, arg_expr))
      return ny_native_nir_emit_const(b, TAG_FLOAT);
    if (!ny_native_nir_expr_is_any(b, arg_expr))
      return ny_native_nir_emit_const(b, 3);
    const char *tag_symbol = ny_native_runtime_symbol(leaf);
    if (!tag_symbol)
      return -1;
    return ny_native_nir_emit_runtime_call(
        b, tag_symbol, value, -1, -1, 1, 0);
  }
  if (leaf && strcmp(leaf, "_big_is_rt") == 0 &&
      e->as.call.args.len == 1 && !e->as.call.args.data[0].name) {
    int value =
        ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
    int tag = ny_native_nir_emit_const(b, TAG_BIGINT);
    if (value < 0 || tag < 0)
      return -1;
    return ny_native_nir_emit_runtime_call(
        b, "rt_native_has_tag", value, tag, -1, 2, 0);
  }
  if (leaf_kind == NY_NATIVE_LEAF_IS_STR && !ny_native_nir_user_defined_fn(b, name)) {
    if (e->as.call.args.len != 1 || e->as.call.args.data[0].name) { ny_native_nir_fail(b, "native NYIR lower: is_str expects one value"); return -1; }
    int value=ny_native_nir_lower_expr(b,e->as.call.args.data[0].val); if(value<0)return -1;
    int tag=ny_native_nir_peek_dyn_fact(b,value,NY_NATIVE_NIR_FACT_DYN_TAG); if(tag<0)return ny_native_nir_emit_const(b,ny_native_nir_expr_is_cstr(b,e->as.call.args.data[0].val)?1:0);
    int want=ny_native_nir_emit_const(b,121); return want<0?-1:nyir_emit(&b->nyir,(nyir_inst_t){.op=NYIR_CMP_I64,.dst=-1,.a=tag,.b=want,.cmp=NYIR_CMP_EQ});
  }
  if (leaf_kind == NY_NATIVE_LEAF_INTRINSIC) {
    /*
     * Capability-gated portable intrinsics: lower a small set to pure NYIR
     * (SWAR), not LLVM. Unknown names still fail explicitly.
     */
    if (e->as.call.args.len < 1 || !e->as.call.args.data[0].val ||
        e->as.call.args.data[0].val->kind != NY_E_LITERAL ||
        e->as.call.args.data[0].val->as.literal.kind != NY_LIT_STR) {
      ny_native_nir_fail(
          b, "native NYIR lower: intrinsic(...) first argument must be a string literal name");
      return -1;
    }
    const char *iname = e->as.call.args.data[0].val->as.literal.as.s.data;
    size_t iname_len = e->as.call.args.data[0].val->as.literal.as.s.len;
    if (!iname || iname_len == 0) {
      ny_native_nir_fail(b, "native NYIR lower: empty intrinsic name");
      return -1;
    }
    if (iname_len == 9 && memcmp(iname, "ctpop.i64", 9) == 0) {
      if (e->as.call.args.len != 2 || e->as.call.args.data[1].name) {
        ny_native_nir_fail(b, "native NYIR lower: ctpop.i64 expects one value");
        return -1;
      }
      int x = ny_native_nir_lower_expr(b, e->as.call.args.data[1].val);
      if (x < 0)
        return -1;
      /*
       * SWAR popcount with logical shifts (SAR + clear high fill bits).
       */
      int c1 = ny_native_nir_emit_const(b, (int64_t)0x5555555555555555LL);
      int c2 = ny_native_nir_emit_const(b, (int64_t)0x3333333333333333LL);
      int c4 = ny_native_nir_emit_const(b, (int64_t)0x0f0f0f0f0f0f0f0fLL);
      int one = ny_native_nir_emit_const(b, 1);
      int two = ny_native_nir_emit_const(b, 2);
      int four = ny_native_nir_emit_const(b, 4);
      int eight = ny_native_nir_emit_const(b, 8);
      int sixteen = ny_native_nir_emit_const(b, 16);
      int thirtytwo = ny_native_nir_emit_const(b, 32);
      int mask7f = ny_native_nir_emit_const(b, 0x7f);
      /*
       * Logical-shift masks: (1<<(64-n))-1
       */
      int m1 = ny_native_nir_emit_const(b, (int64_t)0x7fffffffffffffffLL);
      int m2 = ny_native_nir_emit_const(b, (int64_t)0x3fffffffffffffffLL);
      int m4 = ny_native_nir_emit_const(b, (int64_t)0x0fffffffffffffffLL);
      int m8 = ny_native_nir_emit_const(b, (int64_t)0x00ffffffffffffffLL);
      int m16 = ny_native_nir_emit_const(b, (int64_t)0x0000ffffffffffffLL);
      int m32 = ny_native_nir_emit_const(b, (int64_t)0x00000000ffffffffLL);
      if (c1 < 0 || c2 < 0 || c4 < 0 || one < 0 || two < 0 || four < 0 ||
          eight < 0 || sixteen < 0 || thirtytwo < 0 || mask7f < 0 || m1 < 0 ||
          m2 < 0 || m4 < 0 || m8 < 0 || m16 < 0 || m32 < 0)
        return -1;
#define NY_LSHR(outv, src, sh, msk)                                            \
  do {                                                                         \
  int _t = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SAR_I64,        \
                                                .dst = -1,                   \
                                                .a = (src),                  \
                                                .b = (sh)});                 \
  if (_t < 0)                                                                \
    return -1;                                                               \
  (outv) = nyir_emit(&b->nyir, (nyir_inst_t){                             \
                                    .op = NYIR_AND_I64,                    \
                                    .dst = -1,                               \
                                    .a = _t,                                 \
                                    .b = (msk)});                            \
  if ((outv) < 0)                                                            \
    return -1;                                                               \
  } while (0)
      int t;
      NY_LSHR(t, x, one, m1);
      t = nyir_emit(&b->nyir, (nyir_inst_t){
                                   .op = NYIR_AND_I64, .dst = -1, .a = t, .b = c1});
      if (t < 0)
        return -1;
      x = nyir_emit(&b->nyir, (nyir_inst_t){
                                   .op = NYIR_SUB_I64, .dst = -1, .a = x, .b = t});
      if (x < 0)
        return -1;
      int a = nyir_emit(&b->nyir, (nyir_inst_t){
                                       .op = NYIR_AND_I64, .dst = -1, .a = x, .b = c2});
      int b2;
      NY_LSHR(b2, x, two, m2);
      if (a < 0)
        return -1;
      b2 = nyir_emit(&b->nyir, (nyir_inst_t){
                                    .op = NYIR_AND_I64, .dst = -1, .a = b2, .b = c2});
      if (b2 < 0)
        return -1;
      x = nyir_emit(&b->nyir, (nyir_inst_t){
                                   .op = NYIR_ADD_I64, .dst = -1, .a = a, .b = b2});
      if (x < 0)
        return -1;
      NY_LSHR(t, x, four, m4);
      x = nyir_emit(&b->nyir, (nyir_inst_t){
                                   .op = NYIR_ADD_I64, .dst = -1, .a = x, .b = t});
      if (x < 0)
        return -1;
      x = nyir_emit(&b->nyir, (nyir_inst_t){
                                   .op = NYIR_AND_I64, .dst = -1, .a = x, .b = c4});
      if (x < 0)
        return -1;
      NY_LSHR(t, x, eight, m8);
      x = nyir_emit(&b->nyir, (nyir_inst_t){
                                   .op = NYIR_ADD_I64, .dst = -1, .a = x, .b = t});
      if (x < 0)
        return -1;
      NY_LSHR(t, x, sixteen, m16);
      x = nyir_emit(&b->nyir, (nyir_inst_t){
                                   .op = NYIR_ADD_I64, .dst = -1, .a = x, .b = t});
      if (x < 0)
        return -1;
      NY_LSHR(t, x, thirtytwo, m32);
      x = nyir_emit(&b->nyir, (nyir_inst_t){
                                   .op = NYIR_ADD_I64, .dst = -1, .a = x, .b = t});
      if (x < 0)
        return -1;
#undef NY_LSHR
      return nyir_emit(&b->nyir, (nyir_inst_t){
                                      .op = NYIR_AND_I64, .dst = -1, .a = x, .b = mask7f});
    }
    if ((iname_len == 8 && memcmp(iname, "cttz.i64", 8) == 0) ||
        (iname_len == 8 && memcmp(iname, "ctlz.i64", 8) == 0)) {
      int is_ctlz = (iname_len == 8 && memcmp(iname, "ctlz.i64", 8) == 0);
      /*
       * cttz via ctpop((x & -x) - 1); zero input yields 64.
       * ctlz via cttz(bitreverse(x)).
       */
      if (e->as.call.args.len < 2 || e->as.call.args.len > 3 ||
          e->as.call.args.data[1].name) {
        ny_native_nir_fail(b, "native NYIR lower: cttz/ctlz.i64 expects a value");
        return -1;
      }
      int x = ny_native_nir_lower_expr(b, e->as.call.args.data[1].val);
      if (x < 0)
        return -1;
      if (is_ctlz) {
        /*
         * Bitreverse x via SWAR so cttz on the result yields ctlz.
         */
        int bc1 = ny_native_nir_emit_const(b, (int64_t)0x5555555555555555LL);
        int bc2 = ny_native_nir_emit_const(b, (int64_t)0x3333333333333333LL);
        int bc4 = ny_native_nir_emit_const(b, (int64_t)0x0f0f0f0f0f0f0f0fLL);
        int bc8 = ny_native_nir_emit_const(b, (int64_t)0x00ff00ff00ff00ffLL);
        int bc16 = ny_native_nir_emit_const(b, (int64_t)0x0000ffff0000ffffLL);
        int b1 = ny_native_nir_emit_const(b, 1);
        int b2c = ny_native_nir_emit_const(b, 2);
        int b4c = ny_native_nir_emit_const(b, 4);
        int b8 = ny_native_nir_emit_const(b, 8);
        int b16 = ny_native_nir_emit_const(b, 16);
        int b32 = ny_native_nir_emit_const(b, 32);
        if (bc1 < 0 || bc2 < 0 || bc4 < 0 || bc8 < 0 || bc16 < 0 ||
            b1 < 0 || b2c < 0 || b4c < 0 || b8 < 0 || b16 < 0 || b32 < 0)
          return -1;
        int bm63 = ny_native_nir_emit_const(b, (int64_t)0x7fffffffffffffffLL);
        int bm62 = ny_native_nir_emit_const(b, (int64_t)0x3fffffffffffffffLL);
        int bm60 = ny_native_nir_emit_const(b, (int64_t)0x0fffffffffffffffLL);
        int bm56 = ny_native_nir_emit_const(b, (int64_t)0x00ffffffffffffffLL);
        int bm48 = ny_native_nir_emit_const(b, (int64_t)0x0000ffffffffffffLL);
        if (bm63 < 0 || bm62 < 0 || bm60 < 0 || bm56 < 0 || bm48 < 0)
          return -1;
        /*
         * SWAR bit-reverse steps.
         */
        int br;
        br = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SAR_I64, .dst = -1, .a = x, .b = b1});
        if (br < 0) return -1;
        br = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = br, .b = bm63});
        if (br < 0) return -1;
        br = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = br, .b = bc1});
        if (br < 0) return -1;
        int bl = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = x, .b = bc1});
        if (bl < 0) return -1;
        bl = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SHL_I64, .dst = -1, .a = bl, .b = b1});
        if (bl < 0) return -1;
        x = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_OR_I64, .dst = -1, .a = br, .b = bl});
        if (x < 0) return -1;
        br = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SAR_I64, .dst = -1, .a = x, .b = b2c});
        if (br < 0) return -1;
        br = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = br, .b = bm62});
        if (br < 0) return -1;
        br = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = br, .b = bc2});
        if (br < 0) return -1;
        bl = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = x, .b = bc2});
        if (bl < 0) return -1;
        bl = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SHL_I64, .dst = -1, .a = bl, .b = b2c});
        if (bl < 0) return -1;
        x = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_OR_I64, .dst = -1, .a = br, .b = bl});
        if (x < 0) return -1;
        br = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SAR_I64, .dst = -1, .a = x, .b = b4c});
        if (br < 0) return -1;
        br = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = br, .b = bm60});
        if (br < 0) return -1;
        br = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = br, .b = bc4});
        if (br < 0) return -1;
        bl = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = x, .b = bc4});
        if (bl < 0) return -1;
        bl = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SHL_I64, .dst = -1, .a = bl, .b = b4c});
        if (bl < 0) return -1;
        x = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_OR_I64, .dst = -1, .a = br, .b = bl});
        if (x < 0) return -1;
        br = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SAR_I64, .dst = -1, .a = x, .b = b8});
        if (br < 0) return -1;
        br = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = br, .b = bm56});
        if (br < 0) return -1;
        br = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = br, .b = bc8});
        if (br < 0) return -1;
        bl = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = x, .b = bc8});
        if (bl < 0) return -1;
        bl = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SHL_I64, .dst = -1, .a = bl, .b = b8});
        if (bl < 0) return -1;
        x = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_OR_I64, .dst = -1, .a = br, .b = bl});
        if (x < 0) return -1;
        br = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SAR_I64, .dst = -1, .a = x, .b = b16});
        if (br < 0) return -1;
        br = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = br, .b = bm48});
        if (br < 0) return -1;
        br = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = br, .b = bc16});
        if (br < 0) return -1;
        bl = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = x, .b = bc16});
        if (bl < 0) return -1;
        bl = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SHL_I64, .dst = -1, .a = bl, .b = b16});
        if (bl < 0) return -1;
        x = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_OR_I64, .dst = -1, .a = br, .b = bl});
        if (x < 0) return -1;
        /*
         * final 32-bit swap
         */
        br = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SAR_I64, .dst = -1, .a = x, .b = b32});
        if (br < 0) return -1;
        x = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = x, .b = ny_native_nir_emit_const(b, (int64_t)0x00000000FFFFFFFFLL)});
        if (x < 0) return -1;
        x = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SHL_I64, .dst = -1, .a = x, .b = b32});
        if (x < 0) return -1;
        x = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_OR_I64, .dst = -1, .a = x, .b = br});
        if (x < 0) return -1;
      }
      int zero = ny_native_nir_emit_const(b, 0);
      int one = ny_native_nir_emit_const(b, 1);
      int sixtyfour = ny_native_nir_emit_const(b, 64);
      if (zero < 0 || one < 0 || sixtyfour < 0)
        return -1;
      /*
       * is_zero = (x == 0)
       */
      int is_zero = nyir_emit(
          &b->nyir, (nyir_inst_t){.op = NYIR_CMP_I64,
                                  .dst = -1,
                                  .a = x,
                                  .b = zero,
                                  .cmp = NYIR_CMP_EQ});
      if (is_zero < 0)
        return -1;
      /*
       * lowest = x & -x  (0 - x for negate)
       */
      int neg = nyir_emit(&b->nyir, (nyir_inst_t){
                                         .op = NYIR_SUB_I64, .dst = -1, .a = zero, .b = x});
      if (neg < 0)
        return -1;
      int lowest = nyir_emit(&b->nyir, (nyir_inst_t){
                                            .op = NYIR_AND_I64, .dst = -1, .a = x, .b = neg});
      if (lowest < 0)
        return -1;
      int lm1 = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SUB_I64,
                                                     .dst = -1,
                                                     .a = lowest,
                                                     .b = one});
      if (lm1 < 0)
        return -1;
      /*
       * Inline SWAR popcount of lm1 (same as ctpop).
       */
      int c1 = ny_native_nir_emit_const(b, (int64_t)0x5555555555555555LL);
      int c2 = ny_native_nir_emit_const(b, (int64_t)0x3333333333333333LL);
      int c4 = ny_native_nir_emit_const(b, (int64_t)0x0f0f0f0f0f0f0f0fLL);
      int two = ny_native_nir_emit_const(b, 2);
      int four = ny_native_nir_emit_const(b, 4);
      int eight = ny_native_nir_emit_const(b, 8);
      int sixteen = ny_native_nir_emit_const(b, 16);
      int thirtytwo = ny_native_nir_emit_const(b, 32);
      int mask7f = ny_native_nir_emit_const(b, 0x7f);
      if (c1 < 0 || c2 < 0 || c4 < 0 || two < 0 || four < 0 || eight < 0 ||
          sixteen < 0 || thirtytwo < 0 || mask7f < 0)
        return -1;
      int t = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SAR_I64,
                                                   .dst = -1,
                                                   .a = lm1,
                                                   .b = one});
      if (t < 0)
        return -1;
      t = nyir_emit(&b->nyir, (nyir_inst_t){
                                   .op = NYIR_AND_I64, .dst = -1, .a = t, .b = c1});
      if (t < 0)
        return -1;
      int px = nyir_emit(&b->nyir, (nyir_inst_t){
                                        .op = NYIR_SUB_I64, .dst = -1, .a = lm1, .b = t});
      if (px < 0)
        return -1;
      int a = nyir_emit(&b->nyir, (nyir_inst_t){
                                       .op = NYIR_AND_I64, .dst = -1, .a = px, .b = c2});
      int b2 = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SAR_I64,
                                                    .dst = -1,
                                                    .a = px,
                                                    .b = two});
      if (a < 0 || b2 < 0)
        return -1;
      b2 = nyir_emit(&b->nyir, (nyir_inst_t){
                                    .op = NYIR_AND_I64, .dst = -1, .a = b2, .b = c2});
      if (b2 < 0)
        return -1;
      px = nyir_emit(&b->nyir, (nyir_inst_t){
                                    .op = NYIR_ADD_I64, .dst = -1, .a = a, .b = b2});
      if (px < 0)
        return -1;
      t = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SAR_I64,
                                               .dst = -1,
                                               .a = px,
                                               .b = four});
      if (t < 0)
        return -1;
      px = nyir_emit(&b->nyir, (nyir_inst_t){
                                    .op = NYIR_ADD_I64, .dst = -1, .a = px, .b = t});
      if (px < 0)
        return -1;
      px = nyir_emit(&b->nyir, (nyir_inst_t){
                                    .op = NYIR_AND_I64, .dst = -1, .a = px, .b = c4});
      if (px < 0)
        return -1;
      t = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SAR_I64,
                                               .dst = -1,
                                               .a = px,
                                               .b = eight});
      if (t < 0)
        return -1;
      px = nyir_emit(&b->nyir, (nyir_inst_t){
                                    .op = NYIR_ADD_I64, .dst = -1, .a = px, .b = t});
      if (px < 0)
        return -1;
      t = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SAR_I64,
                                               .dst = -1,
                                               .a = px,
                                               .b = sixteen});
      if (t < 0)
        return -1;
      px = nyir_emit(&b->nyir, (nyir_inst_t){
                                    .op = NYIR_ADD_I64, .dst = -1, .a = px, .b = t});
      if (px < 0)
        return -1;
      t = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SAR_I64,
                                               .dst = -1,
                                               .a = px,
                                               .b = thirtytwo});
      if (t < 0)
        return -1;
      px = nyir_emit(&b->nyir, (nyir_inst_t){
                                    .op = NYIR_ADD_I64, .dst = -1, .a = px, .b = t});
      if (px < 0)
        return -1;
      int pop = nyir_emit(&b->nyir, (nyir_inst_t){
                                         .op = NYIR_AND_I64, .dst = -1, .a = px, .b = mask7f});
      if (pop < 0)
        return -1;
      /*
       * result = is_zero ? 64 : pop  — use select via arithmetic:
       * is_zero * 64 + (1-is_zero)*pop, but we only have binary ops.
       * (is_zero * 64) | ((is_zero ^ 1) * pop)
       */
      int notz = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_XOR_I64,
                                                      .dst = -1,
                                                      .a = is_zero,
                                                      .b = one});
      if (notz < 0)
        return -1;
      int term0 = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_MUL_I64,
                                                       .dst = -1,
                                                       .a = is_zero,
                                                       .b = sixtyfour});
      int term1 = nyir_emit(&b->nyir, (nyir_inst_t){
                                           .op = NYIR_MUL_I64, .dst = -1, .a = notz, .b = pop});
      if (term0 < 0 || term1 < 0)
        return -1;
      return nyir_emit(&b->nyir, (nyir_inst_t){
                                      .op = NYIR_ADD_I64, .dst = -1, .a = term0, .b = term1});
    }
    if ((iname_len == 8 && memcmp(iname, "umax.i64", 8) == 0) ||
        (iname_len == 8 && memcmp(iname, "umin.i64", 8) == 0)) {
      /*
       * Treat as signed for Nytrix i64 values in native path (same select).
       */
      if (e->as.call.args.len != 3 || e->as.call.args.data[1].name ||
          e->as.call.args.data[2].name) {
        ny_native_nir_fail(b, "native NYIR lower: umax/umin.i64 expects two values");
        return -1;
      }
      bool is_max = iname[1] == 'm' && iname[2] == 'a';
      int x = ny_native_nir_lower_expr(b, e->as.call.args.data[1].val);
      int y = ny_native_nir_lower_expr(b, e->as.call.args.data[2].val);
      if (x < 0 || y < 0)
        return -1;
      int c = nyir_emit(&b->nyir, (nyir_inst_t){
                                      .op = NYIR_CMP_I64,
                                      .dst = -1,
                                      .a = x,
                                      .b = y,
                                      .cmp = is_max ? NYIR_CMP_GT : NYIR_CMP_LT});
      int one = ny_native_nir_emit_const(b, 1);
      if (c < 0 || one < 0)
        return -1;
      int nc = nyir_emit(&b->nyir, (nyir_inst_t){
                                        .op = NYIR_XOR_I64, .dst = -1, .a = c, .b = one});
      if (nc < 0)
        return -1;
      int t0 = nyir_emit(&b->nyir, (nyir_inst_t){
                                        .op = NYIR_MUL_I64, .dst = -1, .a = c, .b = x});
      int t1 = nyir_emit(&b->nyir, (nyir_inst_t){
                                        .op = NYIR_MUL_I64, .dst = -1, .a = nc, .b = y});
      if (t0 < 0 || t1 < 0)
        return -1;
      return nyir_emit(&b->nyir, (nyir_inst_t){
                                      .op = NYIR_ADD_I64, .dst = -1, .a = t0, .b = t1});
    }
    if ((iname_len == 8 && memcmp(iname, "smax.i64", 8) == 0) ||
        (iname_len == 8 && memcmp(iname, "smin.i64", 8) == 0)) {
      if (e->as.call.args.len != 3 || e->as.call.args.data[1].name ||
          e->as.call.args.data[2].name) {
        ny_native_nir_fail(b, "native NYIR lower: smax/smin.i64 expects two values");
        return -1;
      }
      bool is_max = iname_len >= 4 && iname[1] == 'm' && iname[2] == 'a';
      int x = ny_native_nir_lower_expr(b, e->as.call.args.data[1].val);
      int y = ny_native_nir_lower_expr(b, e->as.call.args.data[2].val);
      if (x < 0 || y < 0)
        return -1;
      /*
       * result = (x > y) ? x : y  via arithmetic select:
       * c = (x > y); c*x + (1-c)*y
       */
      int c = nyir_emit(&b->nyir, (nyir_inst_t){
                                      .op = NYIR_CMP_I64,
                                      .dst = -1,
                                      .a = x,
                                      .b = y,
                                      .cmp = is_max ? NYIR_CMP_GT : NYIR_CMP_LT});
      int one = ny_native_nir_emit_const(b, 1);
      if (c < 0 || one < 0)
        return -1;
      int nc = nyir_emit(&b->nyir, (nyir_inst_t){
                                        .op = NYIR_XOR_I64, .dst = -1, .a = c, .b = one});
      if (nc < 0)
        return -1;
      int t0 = nyir_emit(&b->nyir, (nyir_inst_t){
                                        .op = NYIR_MUL_I64, .dst = -1, .a = c, .b = x});
      int t1 = nyir_emit(&b->nyir, (nyir_inst_t){
                                        .op = NYIR_MUL_I64, .dst = -1, .a = nc, .b = y});
      if (t0 < 0 || t1 < 0)
        return -1;
      return nyir_emit(&b->nyir, (nyir_inst_t){
                                      .op = NYIR_ADD_I64, .dst = -1, .a = t0, .b = t1});
    }
    if (iname_len == 14 && memcmp(iname, "bitreverse.i64", 14) == 0) {
      if (e->as.call.args.len != 2 || e->as.call.args.data[1].name) {
        ny_native_nir_fail(b,
                           "native NYIR lower: bitreverse.i64 expects one value");
        return -1;
      }
      int x = ny_native_nir_lower_expr(b, e->as.call.args.data[1].val);
      if (x < 0)
        return -1;
      /*
       * Portable bit reverse via parallel SWAR.
       */
      int c1 = ny_native_nir_emit_const(b, (int64_t)0x5555555555555555LL);
      int c2 = ny_native_nir_emit_const(b, (int64_t)0x3333333333333333LL);
      int c4 = ny_native_nir_emit_const(b, (int64_t)0x0f0f0f0f0f0f0f0fLL);
      int c8 = ny_native_nir_emit_const(b, (int64_t)0x00ff00ff00ff00ffLL);
      int c16 = ny_native_nir_emit_const(b, (int64_t)0x0000ffff0000ffffLL);
      int one = ny_native_nir_emit_const(b, 1);
      int two = ny_native_nir_emit_const(b, 2);
      int four = ny_native_nir_emit_const(b, 4);
      int eight = ny_native_nir_emit_const(b, 8);
      int sixteen = ny_native_nir_emit_const(b, 16);
      int thirtytwo = ny_native_nir_emit_const(b, 32);
      if (c1 < 0 || c2 < 0 || c4 < 0 || c8 < 0 || c16 < 0 || one < 0 ||
          two < 0 || four < 0 || eight < 0 || sixteen < 0 || thirtytwo < 0)
        return -1;
      /*
       * x = ((x >> 1) & c1) | ((x & c1) << 1) etc. Use SAR+mask for >>
       */
      int m63 = ny_native_nir_emit_const(b, (int64_t)0x7fffffffffffffffLL);
      int m62 = ny_native_nir_emit_const(b, (int64_t)0x3fffffffffffffffLL);
      int m60 = ny_native_nir_emit_const(b, (int64_t)0x0fffffffffffffffLL);
      int m56 = ny_native_nir_emit_const(b, (int64_t)0x00ffffffffffffffLL);
      int m48 = ny_native_nir_emit_const(b, (int64_t)0x0000ffffffffffffLL);
      int m32 = ny_native_nir_emit_const(b, (int64_t)0x00000000ffffffffLL);
      if (m63 < 0 || m62 < 0 || m60 < 0 || m56 < 0 || m48 < 0 || m32 < 0)
        return -1;
#define NY_BREV_STEP(xin, sh, msk, cm)                                         \
  do {                                                                         \
  int _r = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SAR_I64,            \
                                                .dst = -1,                   \
                                                .a = (xin),                  \
                                                .b = (sh)});                 \
  if (_r < 0)                                                                \
    return -1;                                                               \
  _r = nyir_emit(&b->nyir, (nyir_inst_t){                                    \
                                .op = NYIR_AND_I64, .dst = -1, .a = _r,      \
                                .b = (msk)});                                \
  if (_r < 0)                                                                \
    return -1;                                                               \
  _r = nyir_emit(&b->nyir, (nyir_inst_t){                                    \
                                .op = NYIR_AND_I64, .dst = -1, .a = _r,      \
                                .b = (cm)});                                 \
  if (_r < 0)                                                                \
    return -1;                                                               \
  int _l = nyir_emit(&b->nyir, (nyir_inst_t){                                \
                                    .op = NYIR_AND_I64,                      \
                                    .dst = -1,                               \
                                    .a = (xin),                              \
                                    .b = (cm)});                             \
  if (_l < 0)                                                                \
    return -1;                                                               \
  _l = nyir_emit(&b->nyir, (nyir_inst_t){                                    \
                                .op = NYIR_SHL_I64, .dst = -1, .a = _l,      \
                                .b = (sh)});                                 \
  if (_l < 0)                                                                \
    return -1;                                                               \
  (xin) = nyir_emit(&b->nyir, (nyir_inst_t){                                 \
                                   .op = NYIR_OR_I64, .dst = -1, .a = _r,    \
                                   .b = _l});                                \
  if ((xin) < 0)                                                             \
    return -1;                                                               \
  } while (0)
      NY_BREV_STEP(x, one, m63, c1);
      NY_BREV_STEP(x, two, m62, c2);
      NY_BREV_STEP(x, four, m60, c4);
      NY_BREV_STEP(x, eight, m56, c8);
      NY_BREV_STEP(x, sixteen, m48, c16);
      /*
       * final 32-bit swap
       */
      {
        int r = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SAR_I64,
                                                     .dst = -1,
                                                     .a = x,
                                                     .b = thirtytwo});
        if (r < 0)
          return -1;
        r = nyir_emit(&b->nyir, (nyir_inst_t){
                                     .op = NYIR_AND_I64, .dst = -1, .a = r, .b = m32});
        if (r < 0)
          return -1;
        int l = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SHL_I64,
                                                     .dst = -1,
                                                     .a = x,
                                                     .b = thirtytwo});
        if (l < 0)
          return -1;
        x = nyir_emit(&b->nyir, (nyir_inst_t){
                                     .op = NYIR_OR_I64, .dst = -1, .a = r, .b = l});
        if (x < 0)
          return -1;
      }
#undef NY_BREV_STEP
      return x;
    }
    if (iname_len == 7 && memcmp(iname, "abs.i64", 7) == 0) {
      if (e->as.call.args.len != 2 || e->as.call.args.data[1].name) {
        ny_native_nir_fail(b, "native NYIR lower: abs.i64 expects one value");
        return -1;
      }
      int x = ny_native_nir_lower_expr(b, e->as.call.args.data[1].val);
      if (x < 0)
        return -1;
      int zero = ny_native_nir_emit_const(b, 0);
      int sixtythree = ny_native_nir_emit_const(b, 63);
      if (zero < 0 || sixtythree < 0)
        return -1;
      /*
       * abs via (x ^ (x>>63)) - (x>>63) arithmetic.
       */
      int s = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SAR_I64,
                                                   .dst = -1,
                                                   .a = x,
                                                   .b = sixtythree});
      if (s < 0)
        return -1;
      int y = nyir_emit(&b->nyir, (nyir_inst_t){
                                       .op = NYIR_XOR_I64, .dst = -1, .a = x, .b = s});
      if (y < 0)
        return -1;
      return nyir_emit(&b->nyir, (nyir_inst_t){
                                      .op = NYIR_SUB_I64, .dst = -1, .a = y, .b = s});
    }
    if (iname_len == 9 && memcmp(iname, "bswap.i64", 9) == 0) {
      if (e->as.call.args.len != 2 || e->as.call.args.data[1].name) {
        ny_native_nir_fail(b, "native NYIR lower: bswap.i64 expects one value");
        return -1;
      }
      int x = ny_native_nir_lower_expr(b, e->as.call.args.data[1].val);
      if (x < 0)
        return -1;
      /*
       * Portable byte swap via shifts and masks (no host asm).
       */
      int c8 = ny_native_nir_emit_const(b, 8);
      int c16 = ny_native_nir_emit_const(b, 16);
      int c24 = ny_native_nir_emit_const(b, 24);
      int c32 = ny_native_nir_emit_const(b, 32);
      int c40 = ny_native_nir_emit_const(b, 40);
      int c48 = ny_native_nir_emit_const(b, 48);
      int c56 = ny_native_nir_emit_const(b, 56);
      int mff = ny_native_nir_emit_const(b, (int64_t)0xff);
      if (c8 < 0 || c16 < 0 || c24 < 0 || c32 < 0 || c40 < 0 || c48 < 0 ||
          c56 < 0 || mff < 0)
        return -1;
      int acc = -1;
      int shifts[] = {c56, c48, c40, c32, c24, c16, c8, -1};
      int rshifts[] = {0, 8, 16, 24, 32, 40, 48, 56};
      for (int bi = 0; bi < 8; ++bi) {
        int piece = x;
        if (rshifts[bi] > 0) {
          int rs = ny_native_nir_emit_const(b, rshifts[bi]);
          if (rs < 0)
            return -1;
          piece = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SAR_I64,
                                                       .dst = -1,
                                                       .a = x,
                                                       .b = rs});
          if (piece < 0)
            return -1;
          /*
           * Logical mask after SAR for high bytes
           */
          if (rshifts[bi] >= 32) {
            int m = ny_native_nir_emit_const(
                b, (int64_t)((1ULL << (64 - rshifts[bi])) - 1ULL));
            if (m < 0)
              return -1;
            piece = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_AND_I64,
                                                         .dst = -1,
                                                         .a = piece,
                                                         .b = m});
            if (piece < 0)
              return -1;
          }
        }
        piece = nyir_emit(&b->nyir, (nyir_inst_t){
                                         .op = NYIR_AND_I64, .dst = -1, .a = piece, .b = mff});
        if (piece < 0)
          return -1;
        if (shifts[bi] >= 0) {
          piece = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SHL_I64,
                                                       .dst = -1,
                                                       .a = piece,
                                                       .b = shifts[bi]});
          if (piece < 0)
            return -1;
        }
        if (acc < 0)
          acc = piece;
        else {
          acc = nyir_emit(&b->nyir, (nyir_inst_t){
                                         .op = NYIR_OR_I64, .dst = -1, .a = acc, .b = piece});
          if (acc < 0)
            return -1;
        }
      }
      return acc;
    }
    if ((iname_len == 8 && memcmp(iname, "fshl.i64", 8) == 0) ||
        (iname_len == 8 && memcmp(iname, "rotl.i64", 8) == 0)) {
      bool is_rotl = (iname_len == 8 && memcmp(iname, "rotl.i64", 8) == 0);
      size_t expected_args = is_rotl ? 2u : 3u;
      if (e->as.call.args.len != expected_args + 1) {
        ny_native_nir_fail(b, "native NYIR lower: %.*s argument count mismatch", (int)iname_len, iname);
        return -1;
      }
      int a_val = ny_native_nir_lower_expr(b, e->as.call.args.data[1].val);
      int b_val = is_rotl ? a_val : ny_native_nir_lower_expr(b, e->as.call.args.data[2].val);
      int sh_val = ny_native_nir_lower_expr(b, e->as.call.args.data[is_rotl ? 2 : 3].val);
      if (a_val < 0 || b_val < 0 || sh_val < 0)
        return -1;
      int c63 = ny_native_nir_emit_const(b, 63);
      int c64 = ny_native_nir_emit_const(b, 64);
      if (c63 < 0 || c64 < 0)
        return -1;
      int s = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = sh_val, .b = c63});
      int inv_s = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SUB_I64, .dst = -1, .a = c64, .b = sh_val});
      int rsh = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = inv_s, .b = c63});
      if (s < 0 || inv_s < 0 || rsh < 0)
        return -1;
      int left = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SHL_I64, .dst = -1, .a = a_val, .b = s});
      int right = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SAR_I64, .dst = -1, .a = b_val, .b = rsh});
      if (left < 0 || right < 0)
        return -1;
      return nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_OR_I64, .dst = -1, .a = left, .b = right});
    }
    if ((iname_len == 8 && memcmp(iname, "fshr.i64", 8) == 0) ||
        (iname_len == 8 && memcmp(iname, "rotr.i64", 8) == 0)) {
      bool is_rotr = (iname_len == 8 && memcmp(iname, "rotr.i64", 8) == 0);
      size_t expected_args = is_rotr ? 2u : 3u;
      if (e->as.call.args.len != expected_args + 1) {
        ny_native_nir_fail(b, "native NYIR lower: %.*s argument count mismatch", (int)iname_len, iname);
        return -1;
      }
      int a_val = ny_native_nir_lower_expr(b, e->as.call.args.data[1].val);
      int b_val = is_rotr ? a_val : ny_native_nir_lower_expr(b, e->as.call.args.data[2].val);
      int sh_val = ny_native_nir_lower_expr(b, e->as.call.args.data[is_rotr ? 2 : 3].val);
      if (a_val < 0 || b_val < 0 || sh_val < 0)
        return -1;
      int c63 = ny_native_nir_emit_const(b, 63);
      int c64 = ny_native_nir_emit_const(b, 64);
      if (c63 < 0 || c64 < 0)
        return -1;
      int s = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = sh_val, .b = c63});
      int inv_s = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SUB_I64, .dst = -1, .a = c64, .b = sh_val});
      int lsh = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = inv_s, .b = c63});
      if (s < 0 || inv_s < 0 || lsh < 0)
        return -1;
      int right = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SAR_I64, .dst = -1, .a = a_val, .b = s});
      int left = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SHL_I64, .dst = -1, .a = b_val, .b = lsh});
      if (left < 0 || right < 0)
        return -1;
      return nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_OR_I64, .dst = -1, .a = left, .b = right});
    }
    ny_native_nir_fail(
        b,
        "native NYIR lower: intrinsic(\"%.*s\") is not supported by the Nytrix-owned native backend; use ordinary Nytrix operations or std.math.bin",
        (int)iname_len, iname);
    return -1;
  }
  /*
   * NYIR integer values are raw i64s.  Keep typed print on the raw-i64
   * runtime entry point; rt_print_value is a dynamic NyValue ABI and must
   * only be used after an explicit box operation exists.
   */
  if (leaf_kind == NY_NATIVE_LEAF_ASSERT &&
      !ny_native_nir_user_defined_fn(b, name)) {
    if (e->as.call.args.len < 1 || e->as.call.args.len > 2 ||
        e->as.call.args.data[0].name ||
        (e->as.call.args.len == 2 && e->as.call.args.data[1].name)) {
      ny_native_nir_fail(b,
                         "native NYIR lower: assert expects condition and optional string message");
      return -1;
    }
    int condition = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
    int message = -1;
    if (condition < 0)
      return -1;
    if (e->as.call.args.len == 2) {
      if (!ny_native_nir_expr_is_cstr(b, e->as.call.args.data[1].val)) {
        ny_native_nir_fail(b,
                           "native NYIR lower: assert message must be a string");
        return -1;
      }
      message = ny_native_nir_lower_expr(b, e->as.call.args.data[1].val);
    } else {
      message = ny_native_nir_emit_const(b, 0);
    }
    if (message < 0)
      return -1;
    int result = ny_native_nir_emit_runtime_call(
        b, "rt_native_assert_cstr", condition, message, -1, 2, 0);
    return result < 0 ? -1 : ny_native_nir_emit_const(b, 0);
  }
  if (leaf_kind == NY_NATIVE_LEAF_PRINT &&
      !ny_native_nir_user_defined_fn(b, name)) {
    if (e->as.call.args.len == 0) {
      ny_native_nir_fail(
          b, "native NYIR lower: print requires at least one positional argument");
      return -1;
    }
    for (size_t i = 0; i < e->as.call.args.len; ++i) {
      if (e->as.call.args.data[i].name) {
        ny_native_nir_fail(b, "native NYIR lower: print accepts positional arguments only");
        return -1;
      }
      const expr_t *arg = e->as.call.args.data[i].val;
      nyir_cmp_t ignored_cmp;
      bool is_bool =
          (arg && arg->kind == NY_E_BINARY &&
           ny_native_nir_cmp(arg->as.binary.op, &ignored_cmp)) ||
          (arg && arg->kind == NY_E_LOGICAL) ||
          (arg && arg->kind == NY_E_UNARY && arg->as.unary.op &&
           strcmp(arg->as.unary.op, "!") == 0);
      if (i > 0) {
        int space_ptr = ny_native_nir_emit_cstr_const(b, " ");
        if (space_ptr >= 0)
          (void)ny_native_nir_emit_runtime_call(b, "rt_print_cstr", space_ptr,
                                                -1, -1, 1, 0);
      }
      bool is_f64_arg = !is_bool && ny_native_nir_expr_is_f64(b, arg);
      bool is_f32_arg = !is_bool && ny_native_nir_expr_is_f32(b, arg);
      int raw = ny_native_nir_lower_expr(b, arg);
      if (raw < 0)
        return -1;
      if (is_bool) {
        raw = ny_native_nir_emit_runtime_call(b, "rt_native_bool_to_cstr", raw,
                                              -1, -1, 1, 0);
        if (raw < 0)
          return -1;
      } else if (is_f32_arg) {
        raw = ny_native_nir_emit_f32_to_f64(b, raw);
        if (raw < 0)
          return -1;
      }
      const char *print_sym =
          is_bool || ny_native_nir_expr_is_cstr(b, arg)
              ? "rt_print_cstr"
              : (is_f64_arg || is_f32_arg) ? "rt_print_f64_raw"
                                           : "rt_print_i64_raw";
      if (ny_native_nir_emit_runtime_call(b, print_sym, raw, -1, -1, 1, 0) < 0)
        return -1;
    }
    if (nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_CALL,
                                             .dst = -1,
                                             .a = -1,
                                             .b = -1,
                                             .c = -1,
                                             .imm = 0,
                                             .flags = NYIR_INST_F_EXTERN,
                                             .symbol = "rt_print_newline"}) < 0) {
      ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
      return -1;
    }
    return ny_native_nir_emit_const(b, 0);
  }
  if (leaf_kind == NY_NATIVE_LEAF_FLOAT) {
    if (e->as.call.args.len != 1 || e->as.call.args.data[0].name) {
      ny_native_nir_fail(b, "native NYIR lower: float requires one positional argument");
      return -1;
    }
    const expr_t *arg = e->as.call.args.data[0].val;
    int value = ny_native_nir_lower_expr(b, arg);
    if (value < 0)
      return -1;
    return ny_native_nir_expr_is_f64(b, arg) ? value :
        ny_native_nir_emit_i64_to_f64(b, value);
  }
  if (leaf_kind == NY_NATIVE_LEAF_ARGC &&
      !ny_native_nir_user_defined_fn(b, name)) {
    if (e->as.call.args.len != 0) {
      ny_native_nir_fail(b, "native NYIR lower: %s takes no arguments", leaf);
      return -1;
    }
    /*
     * rt_argc returns the tagged VM integer; native NYIR integers are raw i64.
     */
    int tagged = ny_native_nir_emit_runtime_call(b, "rt_argc", -1, -1, -1, 0, 0);
    int one = tagged < 0 ? -1 : ny_native_nir_emit_const(b, 1);
    return one < 0 ? -1 :
        nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SAR_I64, .dst = -1,
                                          .a = tagged, .b = one});
  }
  if (leaf_kind == NY_NATIVE_LEAF_TICKS) {
    if (e->as.call.args.len != 0) {
      ny_native_nir_fail(b, "native NYIR lower: ticks takes no arguments");
      return -1;
    }
    return ny_native_nir_emit_runtime_call(b, "rt_ticks_ns", -1, -1, -1,
                                           0, 0);
  }
  if (leaf_kind == NY_NATIVE_LEAF_FLT_SQRT) {
    if (e->as.call.args.len != 1 || e->as.call.args.data[0].name) {
      ny_native_nir_fail(b, "native NYIR lower: __flt_sqrt requires one positional argument");
      return -1;
    }
    const expr_t *arg = e->as.call.args.data[0].val;
    int value = ny_native_nir_lower_expr(b, arg);
    if (value < 0)
      return -1;
    if (!ny_native_nir_expr_is_f64(b, arg)) {
      value = ny_native_nir_emit_i64_to_f64(b, value);
      if (value < 0)
        return -1;
    }
    return ny_native_nir_push_val(b, NYIR_SQRT_F64, value, -1, 0, NULL);
  }
  if (leaf_kind == NY_NATIVE_LEAF_ADDR &&
      leaf && strcmp(leaf, "borrow") == 0) {
    if (e->as.call.args.len != 1 || e->as.call.args.data[0].name ||
        !e->as.call.args.data[0].val) {
      ny_native_nir_fail(b, "native NYIR lower: borrow requires one expression");
      return -1;
    }
    /*
     * `&x` parses to borrow(x).  A borrow of a dereferenced pointer, of a
     * heap reference (list/str/any), or of a non-lvalue expression is the
     * identity and yields the reference itself.  A borrow of a scalar local
     * or global is the value's address and falls through to the shared
     * addr_of handling below.
     */
    const expr_t *target = e->as.call.args.data[0].val;
    if (target->kind == NY_E_DEREF)
      return ny_native_nir_lower_expr(b, target->as.deref.target);
    if (target->kind == NY_E_IDENT) {
      ny_native_nir_local_t *l =
          ny_native_nir_find_local(b, target->as.ident.name);
      if (l && (l->is_list || l->is_cstr || l->is_any))
        return ny_native_nir_lower_expr(b, target);
    } else {
      return ny_native_nir_lower_expr(b, target);
    }
  }
  if (leaf_kind == NY_NATIVE_LEAF_ADDR) {
    const expr_t *target = e->as.call.args.data[0].val;
    if (target->kind == NY_E_DEREF)
      return ny_native_nir_lower_expr(b, target->as.deref.target);
    if (target->kind != NY_E_IDENT) {
      ny_native_nir_fail(
          b,
          "native NYIR lower: %s supports local and dereferenced pointer lvalues, not expression kind %d at %s:%d in %s",
          leaf, (int)target->kind,
          target->tok.filename ? target->tok.filename : "<source>",
          target->tok.line,
          b->current_fn_name ? b->current_fn_name : "<unknown>");
      return -1;
    }
    const char *local_name = target->as.ident.name;
    ny_native_nir_local_t *l = ny_native_nir_find_local(b, local_name);
    if (!l) {
      int v = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_ADDR_SYMBOL,
                                                   .dst = -1,
                                                   .a = -1,
                                                   .b = -1,
                                                   .imm = 0,
                                                   .symbol = local_name});
      if (v < 0)
        ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
      return v;
    }
    return ny_native_nir_emit_addr_local(b, l->slot, local_name);
  }
  if (leaf_kind == NY_NATIVE_LEAF_F64BUF_NEW) {
    if (e->as.call.args.len != 1 || e->as.call.args.data[0].name) {
      ny_native_nir_fail(b, "native NYIR lower: f64buf_new requires one positional length");
      return -1;
    }
    int count = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
    int width = ny_native_nir_emit_const(b, 8);
    if (count < 0 || width < 0)
      return -1;
    int out = ny_native_nir_emit_runtime_call(b, "rt_native_tbuf_new", count, width,
                                              -1, 2, 0);
    int64_t const_count = 0;
    if (ny_native_nir_fold_top_level_int(b->prog, e->as.call.args.data[0].val,
                                         &const_count, 0) &&
        const_count > 0 && out >= 0)
      ny_native_nir_record_alloc_fact(b, out, const_count * 8);
    return out;
  }
  if (leaf_kind == NY_NATIVE_LEAF_F64BUF_LOAD) {
    if (e->as.call.args.len != 2 || e->as.call.args.data[0].name ||
        e->as.call.args.data[1].name) {
      ny_native_nir_fail(b, "native NYIR lower: f64buf_load requires buffer and index");
      return -1;
    }
    /*
     * Fin-typed index elision: skip bounds check when index is Fin<N>
     * and N <= buffer byte length (comptime-known).
     */
    int64_t buf_byte_len = ny_native_nir_resolve_buf_byte_len(b, e);
    int data = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
    int index = ny_native_nir_lower_expr(b, e->as.call.args.data[1].val);
    if (data < 0 || index < 0)
      return -1;
    int shift3 = ny_native_nir_emit_const(b, 3);
    int offset = shift3 < 0 ? -1 : ny_native_nir_push_val(b, NYIR_SHL_I64,
                                                            index, shift3, 0, NULL);
    if (offset < 0)
      return -1;
    if (!ny_native_nir_index_fin_bound_elision(b, e, 1, buf_byte_len))
      ny_native_nir_emit_bounds_check(b, data, offset, buf_byte_len);
    int addr = ny_native_nir_emit_add_i64(b, data, offset);
    return addr < 0 ? -1 : ny_native_nir_emit_load_f64(b, addr);
  }
  if (leaf_kind == NY_NATIVE_LEAF_F64BUF_STORE) {
    if (e->as.call.args.len != 3 || e->as.call.args.data[0].name ||
        e->as.call.args.data[1].name || e->as.call.args.data[2].name) {
      ny_native_nir_fail(b, "native NYIR lower: f64buf_store requires buffer, index, and value");
      return -1;
    }
    /*
     * Fin-typed index elision.
     */
    int64_t buf_byte_len = ny_native_nir_resolve_buf_byte_len(b, e);
    int data = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
    int index = ny_native_nir_lower_expr(b, e->as.call.args.data[1].val);
    int value = ny_native_nir_lower_expr(b, e->as.call.args.data[2].val);
    if (data < 0 || index < 0 || value < 0)
      return -1;
    int shift3 = ny_native_nir_emit_const(b, 3);
    int offset = shift3 < 0 ? -1 : ny_native_nir_push_val(b, NYIR_SHL_I64,
                                                            index, shift3, 0, NULL);
    if (offset < 0)
      return -1;
    if (!ny_native_nir_index_fin_bound_elision(b, e, 1, buf_byte_len))
      ny_native_nir_emit_bounds_check(b, data, offset, buf_byte_len);
    int addr = ny_native_nir_emit_add_i64(b, data, offset);
    return addr < 0 ? -1 :
        (ny_native_nir_emit_store_f64(b, addr, value) ? value : -1);
  }
  if (leaf_kind == NY_NATIVE_LEAF_I64BUF_NEW) {
    if (e->as.call.args.len != 1 || e->as.call.args.data[0].name) {
      ny_native_nir_fail(b, "native NYIR lower: i64buf_new requires one positional length");
      return -1;
    }
    int count = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
    int width = ny_native_nir_emit_const(b, 8);
    if (count < 0 || width < 0)
      return -1;
    int out = ny_native_nir_emit_runtime_call(b, "rt_native_tbuf_new", count, width,
                                              -1, 2, 0);
    int64_t const_count = 0;
    if (ny_native_nir_fold_top_level_int(b->prog, e->as.call.args.data[0].val,
                                         &const_count, 0) &&
        const_count > 0 && out >= 0)
      ny_native_nir_record_alloc_fact(b, out, const_count * 8);
    return out;
  }
  if (leaf_kind == NY_NATIVE_LEAF_I64BUF_LOAD) {
    if (e->as.call.args.len != 2 || e->as.call.args.data[0].name ||
        e->as.call.args.data[1].name) {
      ny_native_nir_fail(b, "native NYIR lower: i64buf_load requires buffer and index");
      return -1;
    }
    int64_t buf_byte_len = ny_native_nir_resolve_buf_byte_len(b, e);
    int data = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
    int index = ny_native_nir_lower_expr(b, e->as.call.args.data[1].val);
    if (data < 0 || index < 0)
      return -1;
    int shift3 = ny_native_nir_emit_const(b, 3);
    int offset = shift3 < 0 ? -1 : ny_native_nir_push_val(
        b, NYIR_SHL_I64, index, shift3, 0, NULL);
    if (offset < 0)
      return -1;
    if (!ny_native_nir_index_fin_bound_elision(b, e, 1, buf_byte_len) &&
        !ny_native_nir_emit_bounds_check(b, data, offset, buf_byte_len))
      return -1;
    int addr = ny_native_nir_emit_add_i64(b, data, offset);
    return addr < 0 ? -1 : ny_native_nir_emit_load_i64(b, addr);
  }
  if (leaf_kind == NY_NATIVE_LEAF_I64BUF_STORE) {
    if (e->as.call.args.len != 3 || e->as.call.args.data[0].name ||
        e->as.call.args.data[1].name || e->as.call.args.data[2].name) {
      ny_native_nir_fail(b, "native NYIR lower: i64buf_store requires buffer, index, and value");
      return -1;
    }
    int64_t buf_byte_len = ny_native_nir_resolve_buf_byte_len(b, e);
    int data = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
    int index = ny_native_nir_lower_expr(b, e->as.call.args.data[1].val);
    int value = ny_native_nir_lower_expr(b, e->as.call.args.data[2].val);
    if (data < 0 || index < 0 || value < 0)
      return -1;
    int shift3 = ny_native_nir_emit_const(b, 3);
    int offset = shift3 < 0 ? -1 : ny_native_nir_push_val(
        b, NYIR_SHL_I64, index, shift3, 0, NULL);
    if (offset < 0)
      return -1;
    if (!ny_native_nir_index_fin_bound_elision(b, e, 1, buf_byte_len) &&
        !ny_native_nir_emit_bounds_check(b, data, offset, buf_byte_len))
      return -1;
    int addr = ny_native_nir_emit_add_i64(b, data, offset);
    return addr < 0 || !ny_native_nir_emit_store_i64(b, addr, value) ? -1
                                                                      : value;
  }
  if (leaf_kind == NY_NATIVE_LEAF_LOAD8) {
    if (e->as.call.args.len != 2 || e->as.call.args.data[0].name ||
        e->as.call.args.data[1].name) {
      ny_native_nir_fail(b, "load8 requires pointer and byte offset");
      return -1;
    }
    int addr = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
    int index = ny_native_nir_lower_expr(b, e->as.call.args.data[1].val);
    int effective = (addr < 0 || index < 0)
                        ? -1
                        : ny_native_nir_emit_add_i64(b, addr, index);
    return effective < 0 ? -1 : ny_native_nir_emit_load8(b, effective);
  }
  if (leaf_kind == NY_NATIVE_LEAF_STORE8) {
    if (e->as.call.args.len != 3 || e->as.call.args.data[0].name ||
        e->as.call.args.data[1].name || e->as.call.args.data[2].name) {
      ny_native_nir_fail(b, "store8 requires pointer, value, and byte offset");
      return -1;
    }
    int addr = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
    int value = ny_native_nir_lower_expr(b, e->as.call.args.data[1].val);
    int index = ny_native_nir_lower_expr(b, e->as.call.args.data[2].val);
    int effective = (addr < 0 || index < 0)
                        ? -1
                        : ny_native_nir_emit_add_i64(b, addr, index);
    return effective < 0 || value < 0 ||
                   !ny_native_nir_emit_store8(b, effective, value)
               ? -1
               : value;
  }
  if (leaf_kind == NY_NATIVE_LEAF_LOAD64 ||
      leaf_kind == NY_NATIVE_LEAF_LOAD64_IDX) {
    if (e->as.call.args.len < 1 || e->as.call.args.len > 2 ||
        e->as.call.args.data[0].name ||
        (e->as.call.args.len > 1 && e->as.call.args.data[1].name)) {
      ny_native_nir_fail(b, "native NYIR lower: load64/load64_i/load64_h require positional pointer and optional offset");
      return -1;
    }
    /*
     * Fin-typed offset elision: resolve buffer byte length from the ptr
     * argument, then check whether the byte-offset arg has a Fin type.
     */
    int64_t buf_byte_len = ny_native_nir_resolve_buf_byte_len(b, e);
    int addr = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
    if (addr < 0)
      return -1;
    if (e->as.call.args.len > 1) {
      int off = ny_native_nir_lower_expr(b, e->as.call.args.data[1].val);
      if (off < 0)
        return -1;
      if (!ny_native_nir_index_fin_bound_elision(b, e, 1, buf_byte_len))
        ny_native_nir_emit_bounds_check(b, addr, off, buf_byte_len);
      addr = ny_native_nir_emit_add_i64(b, addr, off);
      if (addr < 0)
        return -1;
    }
    return ny_native_nir_emit_load_i64(b, addr);
  }
  if (leaf_kind == NY_NATIVE_LEAF_STORE64 ||
      leaf_kind == NY_NATIVE_LEAF_STORE64_H ||
      leaf_kind == NY_NATIVE_LEAF_STORE64_IDX) {
    bool intrinsic_order = leaf_kind == NY_NATIVE_LEAF_STORE64_H ||
                           leaf_kind == NY_NATIVE_LEAF_STORE64_IDX;
    if (e->as.call.args.len < 2 || e->as.call.args.len > 3 ||
        (intrinsic_order && e->as.call.args.len != 3) ||
        e->as.call.args.data[0].name || e->as.call.args.data[1].name ||
        (e->as.call.args.len > 2 && e->as.call.args.data[2].name)) {
      ny_native_nir_fail(b, "native NYIR lower: store64_i/store64_h require positional pointer, value, and optional offset");
      return -1;
    }
    size_t val_idx = intrinsic_order ? 2u : 1u;
    size_t off_idx = intrinsic_order ? 1u : 2u;
    /*
     * Fin-typed offset elision.
     */
    int64_t buf_byte_len = ny_native_nir_resolve_buf_byte_len(b, e);
    int addr = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
    int value = ny_native_nir_lower_expr(b, e->as.call.args.data[val_idx].val);
    if (addr < 0 || value < 0)
      return -1;
    if (e->as.call.args.len > off_idx) {
      int off = ny_native_nir_lower_expr(b, e->as.call.args.data[off_idx].val);
      if (off < 0)
        return -1;
      if (!ny_native_nir_index_fin_bound_elision(b, e, off_idx, buf_byte_len))
        ny_native_nir_emit_bounds_check(b, addr, off, buf_byte_len);
      addr = ny_native_nir_emit_add_i64(b, addr, off);
      if (addr < 0)
        return -1;
    }
    if (!ny_native_nir_emit_store_i64(b, addr, value))
      return -1;
    return ny_native_nir_emit_const(b, 0);
  }
  if (e->as.call.args.len > NYIR_CALL_MAX_ARGS) {
    ny_native_nir_fail(b,
                       "native NYIR lower: call exceeds the maximum supported argument count (%d)",
                       NYIR_CALL_MAX_ARGS);
    return -1;
  }
  const stmt_t *callee_fn = ny_native_nir_find_user_function(b, name);
  const stmt_t *callee_ext_decl = NULL;
  for (size_t i = 0; b->prog && i < b->prog->body.len; ++i) {
    callee_ext_decl =
        ny_native_nir_find_extern_decl_in_stmt(b->prog->body.data[i], name, 0);
    if (callee_ext_decl)
      break;
  }
  /*
   * The native-only pipeline compiles bodies only for top-level functions
   * (ny_native_build_nir's flat scan); a module/library method found by the
   * recursive finder has no compiled body, so emitting the call would leave
   * an undefined ny_fn_* symbol at link time.  Reject it with a clean
   * compile error instead (R3).  Builtin C allocators and externs bypass
   * this: they are resolved by symbol and never need a compiled body.
   */
  bool callee_is_extern =
      (callee_fn && (callee_fn->as.fn.is_extern || callee_fn->as.fn.link_name)) ||
      callee_ext_decl != NULL;
  bool runtime_builtin =
      ny_native_runtime_symbol(name) || ny_native_runtime_symbol(leaf);
  bool ffi_builtin = ny_native_ffi_symbol_name(name);
  bool declared_external =
      (b->externs && ny_extern_table_lookup(b->externs, name)) ||
      callee_ext_decl != NULL;
  if (!callee_fn && !runtime_builtin && !ffi_builtin && !declared_external &&
      ny_builtin_alloc_kind(leaf) == NY_BUILTIN_ALLOC_NONE) {
    ny_native_nir_fail(
        b,
        "native NYIR lower: call to '%s' has no native body or supported external "
        "symbol",
        name);
    return -1;
  }
  if (callee_fn && !ny_native_nir_user_fn_is_top_level(b, name) &&
      !callee_is_extern &&
      !(b->externs && ny_extern_table_lookup(b->externs, name)) &&
      ny_builtin_alloc_kind(leaf) == NY_BUILTIN_ALLOC_NONE) {
    ny_native_nir_fail(b,
                       "native NYIR lower: call to '%s' has no compiled native "
                       "body (only top-level functions are compiled in the "
                       "native-only path; stdlib/module methods are not supported)",
                       name);
    return -1;
  }
  int args[NYIR_CALL_MAX_ARGS];
  size_t lowered_argc = 0;
  for (size_t i = 0; i < e->as.call.args.len; ++i) {
    if (e->as.call.args.data[i].name) {
      ny_native_nir_fail(b, "native NYIR lower: named call args are not supported");
      return -1;
    }
    const expr_t *arg_expr = e->as.call.args.data[i].val;
    bool arg_expr_f64 = ny_native_nir_expr_is_f64(b, arg_expr);
    bool arg_expr_f32 = ny_native_nir_expr_is_f32(b, arg_expr);
    int arg = ny_native_nir_lower_expr(b, arg_expr);
    if (arg < 0)
      return -1;
    const ny_param_list *callee_params =
        callee_fn ? &callee_fn->as.fn.params
                  : callee_ext_decl ? &callee_ext_decl->as.ext.params : NULL;
    const char *param_type =
        callee_params && i < callee_params->len
            ? callee_params->data[i].type
            : NULL;
    if (ny_native_type_name_is_f32(param_type) && !arg_expr_f32) {
      arg = arg_expr_f64 ? ny_native_nir_emit_f64_to_f32(b, arg)
                         : ny_native_nir_emit_i64_to_f32(b, arg);
    } else if (ny_native_type_name_is_f64(param_type) && !arg_expr_f64) {
      arg = arg_expr_f32 ? ny_native_nir_emit_f32_to_f64(b, arg)
                         : ny_native_nir_emit_i64_to_f64(b, arg);
    }
    if (arg < 0)
      return -1;
    if (lowered_argc >= NYIR_CALL_MAX_ARGS) {
      ny_native_nir_fail(
          b, "native NYIR lower: expanded call exceeds the maximum supported argument count (%d)",
          NYIR_CALL_MAX_ARGS);
      return -1;
    }
    args[lowered_argc++] = arg;
    if (ny_native_type_name_is_list(param_type)) {
      int length = ny_native_nir_take_list_len_fact(b, arg);
      if (length < 0)
        length = ny_native_nir_emit_runtime_call(
            b, "rt_native_tbuf_len", arg, -1, -1, 1, 0);
      if (length < 0) {
        ny_native_nir_fail(
            b, "native NYIR lower: list argument %zu to '%s' length query failed",
            i + 1, name);
        return -1;
      }
      if (lowered_argc >= NYIR_CALL_MAX_ARGS) {
        ny_native_nir_fail(
            b, "native NYIR lower: expanded call exceeds the maximum supported argument count (%d)",
            NYIR_CALL_MAX_ARGS);
        return -1;
      }
      args[lowered_argc++] = length;
    } else if (ny_native_type_name_is_str(param_type) || ny_native_type_name_is_any(param_type)) {
      int length = ny_native_nir_take_dyn_fact(
          b, arg, NY_NATIVE_NIR_FACT_DYN_STR_LEN);
      int tag =
          ny_native_nir_take_dyn_fact(b, arg, NY_NATIVE_NIR_FACT_DYN_TAG);
      if (length < 0)
        length = ny_native_nir_emit_const(b, 0);
      if (tag < 0) {
        int64_t static_tag = ny_native_nir_expr_is_cstr(b, arg_expr)
                                 ? 121
                                 : (arg_expr_f64 || arg_expr_f32)
                                       ? TAG_FLOAT
                                       : !ny_native_nir_expr_is_any(b, arg_expr)
                                             ? 3
                                             : 0;
        tag = ny_native_nir_emit_const(b, static_tag);
      }
      if (lowered_argc + 1 >= NYIR_CALL_MAX_ARGS) {
        ny_native_nir_fail(
            b, "native NYIR lower: expanded call exceeds the maximum supported argument count (%d)",
            NYIR_CALL_MAX_ARGS);
        return -1;
      }
      args[lowered_argc++] = length;
      args[lowered_argc++] = tag;
    }
  }
  const ny_extern_entry_t *ext =
      b->externs ? ny_extern_table_lookup(b->externs, name) : NULL;
  bool has_aggregate_return = ext && ext->ret_aggregate_size > 0;
  bool has_sret = has_aggregate_return &&
                  ext->ret_aggregate_classes[0] == NY_SYSV_AGG_MEMORY;
  if (has_aggregate_return && !has_sret &&
      ext->ret_aggregate_classes[0] != NY_SYSV_AGG_INTEGER &&
      ext->ret_aggregate_classes[0] != NY_SYSV_AGG_SSE &&
      ext->ret_aggregate_classes[0] != NY_SYSV_AGG_HFA_F32 &&
      ext->ret_aggregate_classes[0] != NY_SYSV_AGG_HFA_F64 &&
      ext->ret_aggregate_classes[0] != NY_SYSV_AGG_HVA_V128) {
    ny_native_nir_fail(
        b, "native NYIR lower: aggregate return class is not represented for the selected ABI");
    return -1;
  }
  if (ext) {
    for (unsigned i = 0;
         i < ext->param_count && i < e->as.call.args.len; ++i) {
      if (ext->arg_aggregate_sizes[i] > 0 &&
          NYIR_ARG_AGG_SIZE(ext->arg_aggregate_sizes[i]) <= 16 &&
          (NYIR_ARG_AGG_CLASS(ext->arg_aggregate_sizes[i], 0) ==
               NY_SYSV_AGG_UNSUPPORTED ||
           NYIR_ARG_AGG_CLASS(ext->arg_aggregate_sizes[i], 0) ==
               NY_SYSV_AGG_NONE)) {
        ny_native_nir_fail(
            b, "native NYIR lower: register aggregate argument is not represented for the selected ABI");
        return -1;
      }
    }
  }
  int aggregate_ret_ptr = -1;
  if (has_aggregate_return) {
    aggregate_ret_ptr = nyir_emit(
        &b->nyir,
        (nyir_inst_t){.op = NYIR_ALLOCA,
                        .dst = -1,
                        .a = -1,
                        .b = -1,
                        .c = -1,
                        .imm = ext->ret_aggregate_size});
    if (aggregate_ret_ptr < 0) {
      ny_native_nir_fail(b, "native NYIR lower: aggregate return allocation failed");
      return -1;
    }
  }

  uint32_t *arg_sizes = NULL;
  if (ext && ext->param_count > 0) {
    bool has_byval = false;
    for (unsigned i = 0; i < ext->param_count; ++i) {
      if (ext->arg_aggregate_sizes[i] > 0) has_byval = true;
    }
    if (has_byval) {
      size_t total_args = e->as.call.args.len + (has_sret ? 1 : 0);
      arg_sizes = (uint32_t *)calloc(total_args, sizeof(*arg_sizes));
      if (!arg_sizes) {
        ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
        return -1;
      }
      for (unsigned i = 0;
           i < e->as.call.args.len && i < ext->param_count; ++i) {
        arg_sizes[i + (has_sret ? 1 : 0)] = ext->arg_aggregate_sizes[i];
      }
    }
  }

  size_t original_argc = lowered_argc;
  size_t argc = original_argc + (has_sret ? 1 : 0);
  if (has_sret) {
    if (argc > NYIR_CALL_MAX_ARGS) {
      free(arg_sizes);
      ny_native_nir_fail(b, "native NYIR lower: call exceeds maximum args with sret");
      return -1;
    }
    for (int i = (int)original_argc - 1; i >= 0; --i) {
      args[i + 1] = args[i];
    }
    args[0] = aggregate_ret_ptr;
  }

  ny_builtin_alloc_kind_t builtin_kind = ny_builtin_alloc_kind(leaf);
  bool builtin_c_call = builtin_kind != NY_BUILTIN_ALLOC_NONE;
  const char *runtime_symbol = ny_native_runtime_symbol(name);
  if (!runtime_symbol)
    runtime_symbol = ny_native_runtime_symbol(leaf);
  bool runtime_c_call = runtime_symbol != NULL;
  bool declared_fn_call = callee_ext_decl != NULL ||
                          (callee_fn && callee_fn->as.fn.is_extern);
  bool ffi_c_call = !ext && !callee_fn && !callee_ext_decl &&
                    ny_native_ffi_symbol_name(name);
  const char *decl_symbol =
      callee_ext_decl ? callee_ext_decl->as.ext.link_name
                      : callee_fn ? callee_fn->as.fn.link_name : NULL;
  const char *symbol =
      ext ? ext->c_symbol
          : (declared_fn_call && decl_symbol
                 ? decl_symbol
                 : (builtin_c_call ? NULL
                                    : (runtime_symbol
                                           ? runtime_symbol
                                           : (callee_fn && callee_fn->as.fn.name
                                                  ? callee_fn->as.fn.name
                                                  : name))));
  if (!ext && builtin_c_call) {
    switch (builtin_kind) {
    case NY_BUILTIN_ALLOC_MALLOC: symbol = "malloc"; break;
    case NY_BUILTIN_ALLOC_CALLOC: symbol = "calloc"; break;
    case NY_BUILTIN_ALLOC_REALLOC: symbol = "realloc"; break;
    case NY_BUILTIN_ALLOC_FREE: symbol = "free"; break;
    case NY_BUILTIN_ALLOC_NONE: break;
    }
  }
  unsigned flags = (ext || declared_fn_call || builtin_c_call ||
                    runtime_c_call || ffi_c_call)
                       ? NYIR_INST_F_EXTERN
                       : 0;
  if (has_sret)
    flags |= NYIR_INST_F_SRET;
  const char *return_type =
      callee_fn ? callee_fn->as.fn.return_type
                : callee_ext_decl ? callee_ext_decl->as.ext.return_type : NULL;
  if ((symbol && strcmp(symbol, "rt_native_bigfloat_to_f64") == 0) ||
      ny_native_type_name_is_f64(return_type)) {
    flags |= NYIR_INST_F_RET_F64;
  } else if (ny_native_type_name_is_f32(return_type)) {
    flags |= NYIR_INST_F_RET_F32;
  } else if (ny_native_nir_expr_is_f64(b, e)) {
    flags |= NYIR_INST_F_RET_F64;
  }
  if (has_aggregate_return && !has_sret) {
    if (ext->ret_aggregate_classes[0] == NY_SYSV_AGG_SSE ||
        ext->ret_aggregate_classes[0] == NY_SYSV_AGG_HFA_F64)
      flags |= NYIR_INST_F_RET_F64;
    else if (ext->ret_aggregate_classes[0] == NY_SYSV_AGG_HFA_F32)
      flags |= NYIR_INST_F_RET_F32;
  }
  int *extra = NULL;
  if (argc > 6) {
    size_t extra_len = argc - 6;
    extra = (int *)malloc(extra_len * sizeof(*extra));
    if (!extra) {
      free(arg_sizes);
      ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
      return -1;
    }
    memcpy(extra, &args[6], extra_len * sizeof(*extra));
  }
  int v = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_CALL,
                                               .dst = -1,
                                               .a = argc > 0 ? args[0] : -1,
                                               .b = argc > 1 ? args[1] : -1,
                                               .c = argc > 2 ? args[2] : -1,
                                               .d = argc > 3 ? args[3] : -1,
                                               .e = argc > 4 ? args[4] : -1,
                                               .f = argc > 5 ? args[5] : -1,
                                               .imm = (int64_t)argc,
                                               .flags = flags,
                                               .symbol = symbol,
                                               .extra_args = extra,
                                               .extra_args_len = argc > 6 ? argc - 6 : 0,
                                               .arg_sizes = arg_sizes});
  if (v < 0) {
    free(extra);
    free(arg_sizes);
    ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
    return -1;
  }
  if (builtin_c_call &&
      !ny_native_nir_record_alloc_fact(
          b, v, ny_native_nir_literal_allocation_size(leaf, e))) {
    return -1;
  }
  if (has_aggregate_return && !has_sret &&
      ext->ret_aggregate_classes[0] == NY_SYSV_AGG_HVA_V128) {
    unsigned elem_count = ext->ret_aggregate_size / 16u;
    if (elem_count < 1 || elem_count > 4 ||
        elem_count * 16u != ext->ret_aggregate_size) {
      ny_native_nir_fail(b, "native NYIR lower: invalid AAPCS64 HVA return layout");
      return -1;
    }
    for (unsigned i = 0; i < elem_count; ++i) {
      int captured = nyir_emit(
          &b->nyir, (nyir_inst_t){.op = NYIR_CAPTURE_RET,
                                   .dst = -1, .a = -1, .b = -1, .c = -1,
                                   .imm = 10 + (int64_t)i});
      int off = i ? ny_native_nir_emit_const(b, (int64_t)i * 16) : -1;
      int addr = i ? ny_native_nir_emit_add_i64(b, aggregate_ret_ptr, off)
                   : aggregate_ret_ptr;
      if (captured < 0 || addr < 0 ||
          nyir_emit(&b->nyir,
                      (nyir_inst_t){.op = NYIR_VEC4_STORE_I64,
                                      .dst = -1, .a = addr, .b = captured,
                                      .c = -1}) < 0)
        return -1;
    }
    return aggregate_ret_ptr;
  }
  if (has_aggregate_return && !has_sret &&
      (ext->ret_aggregate_classes[0] == NY_SYSV_AGG_HFA_F32 ||
       ext->ret_aggregate_classes[0] == NY_SYSV_AGG_HFA_F64)) {
    bool f32_hfa = ext->ret_aggregate_classes[0] == NY_SYSV_AGG_HFA_F32;
    unsigned elem_size = f32_hfa ? 4u : 8u;
    unsigned elem_count = ext->ret_aggregate_size / elem_size;
    if (elem_count < 1 || elem_count > 4 ||
        elem_count * elem_size != ext->ret_aggregate_size) {
      ny_native_nir_fail(b, "native NYIR lower: invalid AAPCS64 HFA return layout");
      return -1;
    }
    static const int f64_selectors[4] = {2, 3, 8, 9};
    int captured[4] = {-1, -1, -1, -1};
    for (unsigned i = 0; i < elem_count; ++i) {
      captured[i] = nyir_emit(
          &b->nyir, (nyir_inst_t){.op = NYIR_CAPTURE_RET,
                                   .dst = -1, .a = -1, .b = -1, .c = -1,
                                   .imm = f32_hfa ? 4 + (int64_t)i
                                                  : f64_selectors[i]});
      if (captured[i] < 0)
        return -1;
    }
    if (!f32_hfa) {
      for (unsigned i = 0; i < elem_count; ++i) {
        int off = i ? ny_native_nir_emit_const(b, (int64_t)i * 8) : -1;
        int addr = i ? ny_native_nir_emit_add_i64(b, aggregate_ret_ptr, off)
                     : aggregate_ret_ptr;
        if (addr < 0 ||
            !ny_native_nir_emit_store_i64(b, addr, captured[i]))
          return -1;
      }
    } else {
      int shift32 = ny_native_nir_emit_const(b, 32);
      if (shift32 < 0)
        return -1;
      for (unsigned pair = 0; pair * 2 < elem_count; ++pair) {
        unsigned lo_idx = pair * 2;
        int packed = captured[lo_idx];
        if (lo_idx + 1 < elem_count) {
          int shifted = nyir_emit(
              &b->nyir, (nyir_inst_t){.op = NYIR_SHL_I64,
                                       .dst = -1,
                                       .a = captured[lo_idx + 1],
                                       .b = shift32});
          packed = shifted < 0 ? -1 : nyir_emit(
              &b->nyir, (nyir_inst_t){.op = NYIR_OR_I64,
                                       .dst = -1, .a = captured[lo_idx],
                                       .b = shifted});
        }
        int off = pair ? ny_native_nir_emit_const(b, (int64_t)pair * 8) : -1;
        int addr = pair ? ny_native_nir_emit_add_i64(b, aggregate_ret_ptr, off)
                        : aggregate_ret_ptr;
        if (packed < 0 || addr < 0 ||
            !ny_native_nir_emit_store_i64(b, addr, packed))
          return -1;
      }
    }
    return aggregate_ret_ptr;
  }
  int primary_ret = v;
  int second_ret = -1;
  bool capture_second_integer_first =
      has_aggregate_return && !has_sret &&
      ext->ret_aggregate_classes[0] == NY_SYSV_AGG_SSE &&
      ext->ret_aggregate_classes[1] == NY_SYSV_AGG_INTEGER;
  if (capture_second_integer_first) {
    second_ret = nyir_emit(
        &b->nyir, (nyir_inst_t){.op = NYIR_CAPTURE_RET,
                                 .dst = -1,
                                 .a = -1,
                                 .b = -1,
                                 .c = -1,
                                 .imm = 1});
    if (second_ret < 0) {
      ny_native_nir_fail(b, "native NYIR lower: secondary return register capture failed");
      return -1;
    }
  }
  if (has_aggregate_return && !has_sret &&
      ext->ret_aggregate_classes[0] == NY_SYSV_AGG_SSE) {
    primary_ret = nyir_emit(
        &b->nyir, (nyir_inst_t){.op = NYIR_CAPTURE_RET,
                                 .dst = -1,
                                 .a = -1,
                                 .b = -1,
                                 .c = -1,
                                 .imm = 2});
    if (primary_ret < 0) {
      ny_native_nir_fail(b, "native NYIR lower: primary return register capture failed");
      return -1;
    }
  }
  if (has_aggregate_return && !has_sret &&
      ext->ret_aggregate_classes[1] != NY_SYSV_AGG_NONE &&
      !capture_second_integer_first) {
    int selector = -1;
    if (ext->ret_aggregate_classes[1] == NY_SYSV_AGG_INTEGER)
      selector = ext->ret_aggregate_classes[0] == NY_SYSV_AGG_INTEGER ? 0 : 1;
    else if (ext->ret_aggregate_classes[1] == NY_SYSV_AGG_SSE)
      selector = ext->ret_aggregate_classes[0] == NY_SYSV_AGG_SSE ? 3 : 2;
    if (selector < 0) {
      ny_native_nir_fail(
          b, "native NYIR lower: secondary aggregate return class is not represented for the selected ABI");
      return -1;
    }
    second_ret = nyir_emit(
        &b->nyir, (nyir_inst_t){.op = NYIR_CAPTURE_RET,
                                 .dst = -1,
                                 .a = -1,
                                 .b = -1,
                                 .c = -1,
                                 .imm = selector});
    if (second_ret < 0) {
      ny_native_nir_fail(b, "native NYIR lower: return register capture failed");
      return -1;
    }
  }
  if (has_aggregate_return && !has_sret) {
    if (!ny_native_nir_emit_store_i64(b, aggregate_ret_ptr, primary_ret))
      return -1;
    if (second_ret >= 0) {
      int off = ny_native_nir_emit_const(b, 8);
      int addr = off >= 0
                     ? ny_native_nir_emit_add_i64(b, aggregate_ret_ptr, off)
                     : -1;
      if (addr < 0 ||
          !ny_native_nir_emit_store_i64(b, addr, second_ret))
        return -1;
    }
  }
  if (return_type && strcmp(return_type, "bigint") == 0) {
    int bigint_tag = ny_native_nir_emit_const(b, TAG_BIGINT);
    if (bigint_tag < 0 ||
        !ny_native_nir_record_dyn_fact(b, v, NY_NATIVE_NIR_FACT_DYN_TAG,
                                       bigint_tag))
      return -1;
  }
  if (callee_fn && ny_native_type_name_is_list(callee_fn->as.fn.return_type)) {
    int length = ny_native_nir_emit_runtime_call(
        b, "rt_native_tbuf_len", v, -1, -1, 1, 0);
    if (length < 0 || !ny_native_nir_record_list_len_fact(b, v, length))
      return -1;
  } else if (leaf && strcmp(leaf, "_ensure_windows") == 0) {
    int meta_off = ny_native_nir_emit_const(b, -16);
    int meta = meta_off < 0 ? -1 : ny_native_nir_emit_add_i64(b, v, meta_off);
    int length = meta < 0 ? -1 : ny_native_nir_emit_load_i64(b, meta);
    if (length < 0 || !ny_native_nir_record_list_len_fact(b, v, length))
      return -1;
  }
  return has_aggregate_return ? aggregate_ret_ptr : v;
}
static bool ny_native_nir_qualified_expr(const expr_t *e, char *out,
                                          size_t out_len) {
  if (!e || !out || out_len == 0)
    return false;
  if (e->kind == NY_E_IDENT && e->as.ident.name) {
    int n = snprintf(out, out_len, "%s", e->as.ident.name);
    return n >= 0 && (size_t)n < out_len;
  }
  if (e->kind == NY_E_MEMBER && e->as.member.target &&
      e->as.member.name) {
    char prefix[512];
    if (!ny_native_nir_qualified_expr(e->as.member.target, prefix,
                                      sizeof(prefix)))
      return false;
    int n = snprintf(out, out_len, "%s.%s", prefix, e->as.member.name);
    return n >= 0 && (size_t)n < out_len;
  }
  return false;
}
