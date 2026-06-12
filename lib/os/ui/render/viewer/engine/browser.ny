;; Keywords: engine browser assets model gltf os ui render viewer scene
;; Asset browser panel helpers for choosing and loading GLTF scenes.
;; References:
;; - std.os.ui.assets.catalog
;; - std.os.ui.assets.viewer
module std.os.ui.render.viewer.engine.browser(draw_body, prepare_state)
use std.core
use std.core.str as str
use std.math (clamp, max, min)
use std.os.ui.assets.catalog as asset_catalog
use std.os.ui.window.consts as uin
use std.os.ui.render.viewer.gui as gui
use std.os.ui.render.dump as ui_profile
use std.os.ui.render.viewer.engine.catalog as viewer_catalog
use std.os.ui.render.viewer.icons as icons

fn _result(str filter, bool filter_changed, int tab, bool show_paths, str action="", str model="") dict {
   {"filter": filter, "filter_changed": filter_changed, "tab": tab, "show_paths": show_paths, "action": action, "model": model}
}

fn _ctx_result(dict ctx, str action="", str model="") dict {
   def out = _result(to_str(ctx.get("filter", "")), bool(ctx.get("filter_changed", false)),
   int(ctx.get("tab", 0)), bool(ctx.get("show_paths", false)), action, model)
   if(action == "load"){
      out["press_seq"] = gui.mouse_press_seq()
   }
   out
}

fn _pick_names(str filter_key, list names, list filtered_names) list {
   (filter_key.len > 0) ? filtered_names : names
}

fn _asset_focus_active(str idp) bool {
   def fid = gui.focused_id()
   if(fid.len == 0){ return false }
   str.find(fid, to_str(idp) + "_") >= 0 ||
   str.find(fid, "asset_title_filter") >= 0 ||
   str.find(fid, "asset_browser::") >= 0 ||
   str.find(fid, "editor_main::editor_asset") >= 0
}

fn _keyboard_pick(dict ctx, list pick_names, int selected_idx, str selected_name) dict {
   def idp = to_str(ctx.get("idp", "asset"))
   if(!_asset_focus_active(idp) || pick_names.len <= 0){ return _ctx_result(ctx) }
   mut idx = int(clamp(float(selected_idx), 0.0, float(pick_names.len - 1)))
   def start_idx = idx
   if(gui.key_pressed(uin.KEY_UP)){ idx = max(0, idx - 1) }
   elif(gui.key_pressed(uin.KEY_DOWN)){ idx = min(pick_names.len - 1, idx + 1) }
   elif(gui.key_pressed(uin.KEY_PAGE_UP)){ idx = max(0, idx - 8) }
   elif(gui.key_pressed(uin.KEY_PAGE_DOWN)){ idx = min(pick_names.len - 1, idx + 8) }
   elif(gui.key_pressed(uin.KEY_HOME)){ idx = 0 }
   elif(gui.key_pressed(uin.KEY_END)){ idx = pick_names.len - 1 }
   def model = to_str(pick_names.get(idx, selected_name))
   if(idx != start_idx){ return _ctx_result(ctx, "select", model) }
   if(gui.key_pressed(uin.KEY_ENTER) && model.len > 0){ return _ctx_result(ctx, "load", model) }
   _ctx_result(ctx)
}

fn _state_for_model(dict state, str model) dict {
   if(model.len <= 0){ return state }
   mut out = state
   out["selected_name"] = model
   out
}

fn prepare_state(dict state, any names, any filtered_names, any filter_key, any selected_idx) dict {
   "Builds the derived browser state consumed by draw_body."
   def loaded_name = to_str(state.get("loaded_name", ""))
   mut selected_name = to_str(state.get("selected_name", ""))
   if(selected_name.len == 0){ selected_name = loaded_name }
   state["names"] = is_list(names) ? names : []
   state["filtered_names"] = is_list(filtered_names) ? filtered_names : []
   state["shown_total"] = state["filtered_names"].len
   state["filter_key"] = to_str(filter_key)
   state["selected_name"] = selected_name
   state["loaded_label"] = loaded_name.len > 0 ? loaded_name : "<none>"
   state["selected_label"] = selected_name.len > 0 ? selected_name : "<none>"
   def shown = to_str(filter_key).len > 0 ? state["filtered_names"] : state["names"]
   def resolved_idx = selected_name.len > 0 ? viewer_catalog.model_index(shown, selected_name) : int(selected_idx)
   state["selected_idx"] = max(0, resolved_idx)
   state
}

fn _filter_input(dict ctx, str suffix, f64 win_w, f64 min_w) dict {
   def idp = to_str(ctx.get("idp", "asset"))
   def filter = to_str(ctx.get("filter", ""))
   def next_filter = gui.input_text(idp + "_model_filter" + suffix, "Filter", filter, "Type model name...", max(min_w, win_w - 118.0))
   def changed = next_filter != filter
   gui.same_line()
   if(gui.small_button(idp + "_model_filter_clear" + suffix, "Clear", 62.0)){
      ctx["filter"] = ""
      ctx["filter_changed"] = true
      return _ctx_result(ctx)
   }
   ctx["filter"] = next_filter
   ctx["filter_changed"] = bool(ctx.get("filter_changed", false)) || changed
   _ctx_result(ctx)
}

fn _quick_pick(dict ctx, list pick_names, int selected_idx, int max_visible, str selected_name) dict {
   def idp = to_str(ctx.get("idp", "asset"))
   def next_idx = gui.combo_box(idp + "_model_quick_pick", "Quick Pick", pick_names, selected_idx, 0.0, max_visible)
   mut action = ""
   mut model = ""
   if(next_idx >= 0 && next_idx < pick_names.len && next_idx != selected_idx){
      ;; Quick-pick changes selection only; the explicit Load button performs heavy scene IO.
      action = "select"
      model = to_str(pick_names.get(next_idx, selected_name))
   }
   def out = _ctx_result(ctx, action, model)
   out["stop"] = gui.remaining_h(44.0) < 160.0
   out
}

fn _summary_bar(list names, int shown_total, str filter_key, str loaded_label, str selected_label) int {
   def visible = (filter_key.len > 0) ? (to_str(shown_total) + " filtered") : (to_str(shown_total) + " visible")
   def loaded = (loaded_label == selected_label) ? loaded_label : (loaded_label + " / " + selected_label)
   gui.text_colored("Catalog " + to_str(names.len) + "   " + visible + "   Model " + loaded, [0.70, 0.70, 0.70, 0.94])
   0
}

fn _toolbar(dict ctx, dict state) dict {
   def idp = to_str(ctx.get("idp", "asset"))
   def show_paths = bool(ctx.get("show_paths", false))
   def selected = to_str(state.get("selected_name", ""))
   def loaded = to_str(state.get("loaded_name", ""))
   if(selected.len > 0 && selected != loaded){
      if(gui.button(idp + "_model_load_selected", "Load", 72.0)){
         return _ctx_result(ctx, "load", selected)
      }
      gui.same_line()
   }
   def next_show_paths = gui.checkbox(idp + "_model_paths", "Resolved paths", show_paths)
   ctx["show_paths"] = next_show_paths
   if(gui.icon_button(idp + "_model_unload", icons.icon_sprite("asset_unload"), "", 34.0, 30.0, false)){
      return _ctx_result(ctx, "unload", "")
   }
   gui.same_line()
   if(gui.icon_button(idp + "_model_fit", icons.icon_sprite("asset_fit"), "", 34.0, 30.0, false)){
      return _ctx_result(ctx, "fit", "")
   }
   gui.same_line()
   if(gui.icon_button(idp + "_model_refresh", icons.icon_sprite("asset_refresh"), "", 34.0, 30.0, false)){
      return _ctx_result(ctx, "refresh", "")
   }
   _ctx_result(ctx)
}

fn _grid(dict ctx, str suffix, list names, f64 win_w, f64 list_h, bool compact, dict state, bool hide_detail=false, bool file_list=false) dict {
   def idp = to_str(ctx.get("idp", "asset"))
   def show_paths = bool(ctx.get("show_paths", false))
   def res = viewer_catalog.draw_grid(idp, suffix, names, win_w, list_h, compact, {
         "show_paths": show_paths,
         "catalog": state.get("catalog", 0),
         "scene_loaded": bool(state.get("scene_loaded", false)),
         "loaded_name": state.get("loaded_name", ""),
         "selected_name": state.get("selected_name", ""),
         "selected_idx": int(state.get("selected_idx", -1)),
         "ensure_selected": bool(state.get("ensure_selected", false)),
         "parity_lock": bool(state.get("parity_lock", false)),
         "hide_detail": hide_detail,
         "file_list": file_list
   })
   def clicked = to_str(res.get("clicked", ""))
   clicked.len > 0 ? _ctx_result(ctx, "load", clicked) : _ctx_result(ctx)
}

fn _standalone(dict ctx, f64 win_w, f64 win_h, list names, list filtered_names, int shown_total, str filter_key, str loaded_label, str selected_label, dict state) dict {
   def list_h_limit = asset_catalog.asset_grid_view_h(max(120.0, win_h - 58.0), false, true)
   mut list_h = viewer_catalog.grid_h(max(120.0, win_h - 42.0), false, true)
   if(filter_key.len > 0 && shown_total <= 12){
      list_h = viewer_catalog.grid_h(asset_catalog.asset_grid_fit_h(shown_total, win_w, list_h_limit, false, bool(ctx.get("show_paths", false))), false, true)
   }
   _grid(ctx, "standalone", filtered_names, win_w, list_h, false, state)
}

fn _compact_catalog(dict ctx, f64 win_w, f64 win_h, list names, list filtered_names, int shown_total, str filter_key, dict state) dict {
   def pick_names = _pick_names(filter_key, names, filtered_names)
   def selected_idx = max(0, int(state.get("selected_idx", 0)))
   def filter_res = _filter_input(ctx, "_compact", win_w, 150.0)
   def nav = _keyboard_pick(ctx, pick_names, selected_idx, to_str(state.get("selected_name", "")))
   def nav_action = to_str(nav.get("action", ""))
   mut draw_state = (nav_action.len > 0) ? _state_for_model(state, to_str(nav.get("model", ""))) : state
   if(nav_action.len > 0){ draw_state["ensure_selected"] = true }
   def active_idx = max(0, viewer_catalog.model_index(pick_names, to_str(draw_state.get("selected_name", ""))))
   def quick = _quick_pick(ctx, pick_names, active_idx, 5, to_str(draw_state.get("selected_name", "")))
   def quick_action = to_str(quick.get("action", ""))
   if(quick_action.len > 0){
      draw_state = _state_for_model(draw_state, to_str(quick.get("model", "")))
      draw_state["ensure_selected"] = true
   }
   if(bool(quick.get("stop", false))){
      return quick
   }
   gui.text_colored("Catalog " + to_str(shown_total) + " / " + to_str(names.len), [0.68, 0.68, 0.68, 1.0])
   def compact_h = max(96.0, gui.remaining_h(4.0))
   def grid_res = _grid(ctx, "compact", filtered_names, win_w, viewer_catalog.grid_h(compact_h, true, false, 4.0),
   true, draw_state)
   if(nav_action.len > 0){ return nav }
   if(to_str(grid_res.get("action", "")).len > 0){ return grid_res }
   filter_res
}

fn _compact(dict ctx, f64 win_w, f64 win_h, list names, list filtered_names, int shown_total, str filter_key, str loaded_label, str selected_label, dict state) dict {
   _compact_catalog(ctx, win_w, win_h, names, filtered_names, shown_total, filter_key, state)
}

fn _full(dict ctx, f64 win_w, f64 win_h, list names, list filtered_names, int shown_total, str filter_key, str loaded_label, str selected_label, dict state) dict {
   def idp = to_str(ctx.get("idp", "asset"))
   def prof = ui_profile.enabled()
   mut t_prof = prof ? ui_profile.now() : 0
   def embedded = idp == "editor_asset"
   _summary_bar(names, shown_total, filter_key, loaded_label, selected_label)
   t_prof = ui_profile.mark_next(prof, "asset_full_summary", t_prof)
   _filter_input(ctx, "", win_w, 180.0)
   t_prof = ui_profile.mark_next(prof, "asset_full_filter", t_prof)
   def pick_names = _pick_names(filter_key, names, filtered_names)
   def selected_idx = max(0, int(state.get("selected_idx", 0)))
   def nav = _keyboard_pick(ctx, pick_names, selected_idx, to_str(state.get("selected_name", "")))
   def nav_action = to_str(nav.get("action", ""))
   mut draw_state = (nav_action.len > 0) ? _state_for_model(state, to_str(nav.get("model", ""))) : state
   if(nav_action.len > 0){ draw_state["ensure_selected"] = true }
   t_prof = ui_profile.mark_next(prof, "asset_full_pick_index", t_prof)
   if(embedded && int(ctx.get("tab", 0)) == 1){
      def list_h = max(96.0, gui.remaining_h(4.0))
      def out = _grid(ctx, "files", filtered_names, win_w, list_h, true, draw_state, true, true)
      ui_profile.mark_done(prof, "asset_full_files", t_prof)
      if(nav_action.len > 0){ return nav }
      return out
   }
   def active_idx = max(0, viewer_catalog.model_index(pick_names, to_str(draw_state.get("selected_name", ""))))
   def quick = _quick_pick(ctx, pick_names, active_idx, 8, to_str(draw_state.get("selected_name", "")))
   def quick_action = to_str(quick.get("action", ""))
   if(quick_action.len > 0){
      draw_state = _state_for_model(draw_state, to_str(quick.get("model", "")))
      draw_state["ensure_selected"] = true
   }
   if(bool(quick.get("stop", false))){ return quick }
   t_prof = ui_profile.mark_next(prof, "asset_full_quick_pick", t_prof)
   def tools = _toolbar(ctx, draw_state)
   if(to_str(tools.get("action", "")).len > 0){ return tools }
   t_prof = ui_profile.mark_next(prof, "asset_full_toolbar", t_prof)
   def grid_compact = embedded ? true : false
   ;; Size against the actual remaining body height, not the full window height.
   ;; Embedded sidebars should fill the whole vertical slot.  gui.remaining_h()
   ;; can be conservative inside nested panels, leaving a dead black tail below
   ;; the asset list.  Use the actual panel height budget instead.
   def requested_h = embedded ? max(160.0, win_h - 150.0) : max(140.0, win_h - 166.0)
   def out = _grid(ctx, "main", filtered_names, win_w, viewer_catalog.grid_h(requested_h, grid_compact, embedded, 4.0),
   grid_compact, draw_state, embedded && !bool(ctx.get("show_paths", false)))
   ui_profile.mark_done(prof, "asset_full_grid", t_prof)
   if(nav_action.len > 0){ return nav }
   out
}

fn draw_body(dict state) dict {
   "Draws draw body."
   def idp = to_str(state.get("idp", "asset"))
   def raw_w = float(state.get("win_w", 520.0))
   def raw_h = float(state.get("win_h", 700.0))
   ;; Width comes from the docking/layout parent.  Do not clamp by default or
   ;; the asset panel will fight automatic tiling.  Developers can still force
   ;; a cap with NY_ASSET_BROWSER_MAX_W when comparing layouts.
   def max_w_override = float(ui_profile.env_int_cached("NY_ASSET_BROWSER_MAX_W", 0, 0, 4096))
   def win_w = max(180.0, (max_w_override > 0.0) ? min(raw_w, max_w_override) : raw_w)
   def win_h = max(180.0, raw_h)
   def standalone = bool(state.get("standalone", false))
   def compact = bool(state.get("compact", false))
   def names = state.get("names", [])
   def filtered_names = state.get("filtered_names", [])
   def shown_total = int(state.get("shown_total", 0))
   def filter_key = to_str(state.get("filter_key", ""))
   def loaded_label = to_str(state.get("loaded_label", "<none>"))
   def selected_label = to_str(state.get("selected_label", "<none>"))
   def filter = to_str(state.get("filter", ""))
   def show_paths = bool(state.get("show_paths", false))
   def tab_raw = int(state.get("tab", 0))
   def tab = (tab_raw == 1) ? 1 : 0
   def ctx = {"idp": idp, "filter": filter, "filter_changed": false, "tab": tab, "show_paths": show_paths}
   if(standalone){
      return _standalone(ctx, win_w, win_h, names, filtered_names, shown_total, filter_key, loaded_label, selected_label, state)
   }
   if(compact){
      return _compact(ctx, win_w, win_h, names, filtered_names, shown_total, filter_key, loaded_label, selected_label, state)
   }
   _full(ctx, win_w, win_h, names, filtered_names, shown_total, filter_key, loaded_label, selected_label, state)
}

#main {
   def r = _result("x", true, 2, true, "load", "Model")
   assert(r.get("filter", "") == "x" && r.get("action", "") == "load", "browser result")
   assert(_pick_names("", ["a"], ["b"]).get(0, "") == "a" && _pick_names("b", ["a"], ["b"]).get(0, "") == "b", "browser pick names")
   print("✓ std.os.ui.render.viewer.engine.browser self-test passed")
}
