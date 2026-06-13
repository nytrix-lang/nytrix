#!/usr/bin/env ny

;; Keywords: ui engine example
;; A collection of rendering paths, benchmarks, and utilities for testing, profiling, diagnostics, and tooling.
use std.core
use std.core.common as common
use std.core.str as str
use std.math
use std.math.float (is_nan, is_inf)
use std.os (exit)
use std.os.subprocess (run_capture)
use std.os.fs as osfs
use std.os.path as ospath
use std.os.ui.render.viewer.app as ui_app
use std.os.ui.assets.catalog as asset_catalog
use std.os.ui.assets.viewer as ui_assets
use std.os.ui.render.camera as camera
use std.os.ui.window.consts
use std.os.ui.render.diag as ui_diag
use std.os.ui.render.dump as ui_dump
use std.os.ui.render.viewer.gui as gui
use std.os.ui.render.viewer.idle as ui_idle
use std.os.ui.render.dump as ui_profile
use std.os.ui.render as gfx
use std.os.ui.render.matrix as rmat
use std.os.ui.render.viewer.term as terminal
use std.os.ui.render.viewer.runtime as ui_runtime
use std.os.ui.render.scene as scene_engine
use std.os.ui.render.viewer.engine.selection as ui_selection
use std.os.ui.assets.batch as asset_batch
use std.os.ui.render.viewer.bootstrap as ui_bootstrap
use std.os.ui.render.viewer.engine.browser as viewer_browser
use std.os.ui.render.viewer.engine.catalog as viewer_catalog
use std.os.ui.render.viewer.engine.cli as viewer_cli
use std.os.ui.render.viewer.dock as ui_editor
use std.os.ui.render.viewer.engine.editor.chrome as demo_editor
use std.os.ui.render.viewer.engine.hierarchy as viewer_hierarchy
use std.os.ui.render.viewer.icons as viewer_icons
use std.os.ui.render.viewer.engine.inspector as viewer_inspector
use std.os.ui.render.viewer.keyboard as viewer_keyboard
use std.os.ui.render.viewer.engine.loading as viewer_loading
use std.os.ui.render.viewer.loop as ui_loop
use std.os.ui.render.viewer.engine.overlay as viewer_overlay
use std.os.ui.render.viewer.engine.panels as viewer_panels
use std.os.ui.render.viewer.engine.shell as ui_shell
use std.os.ui.render.viewer.engine.tools as viewer_tools
use std.os.ui.window
use std.os.ui.window.input as uin
use std.os.ui.window.native as win_native
use std.parse.3d.gltf as gltf
use std.os.ui.render.viewer.engine.env
use std.os.ui.render.viewer.engine.gizmo
use std.os.ui.render.viewer.engine.state

fn _render_set_next_frame_load_color(any enabled) bool { gfx.set_next_frame_load_color(enabled) }
fn _render_set_frame_time_sec(any seconds) bool { gfx.set_frame_time_sec(seconds) }
fn _render_set_skybox_view(any yaw, any pitch, any fov) bool { gfx.set_skybox_view(yaw, pitch, fov) }
fn _render_begin_frame() bool { gfx.begin_frame() }
fn _render_end_frame() bool { gfx.end_frame() }
fn _render_draw_skybox(any tex_id) bool { gfx.draw_skybox(int(tex_id)) }
fn _render_vertex_offset() int { gfx.renderer_vertex_offset() }
fn _render_reset_overlay_state() bool { gfx.reset_overlay_state() }

def STARTUP_ONE_ARG_CMDS = ["load", "timeout", "skybox", "anim", "gizmo"]
def HUD_BG_U32 = 0xE6080A12
def WHITE_U32 = 0xFFFFFFFF
mut str: _gui_pending_model_load_name = ""
mut bool: _gui_pending_unload_scene = false
mut int: _gui_last_asset_load_press_seq = 0
mut bool: _gui_asset_load_wait_mouse_up = false
mut bool: _cli_scripted_scene_load = false
mut bool: _scene_load_finalizing = false
mut int: _gui_material_selected_idx = -1
mut int: _gui_material_selected_part_idx = -1
mut str: _gui_asset_state_cache_sig = "\x00"
mut dict: _gui_asset_state_cache = dict(0)

fn create(any config) dict {
   "Create a shared demo UI app context."
   {"config": config}
}

fn _ui_font() {
   res_font_ui ? res_font_ui : res_font
}

fn _ui_title_font() {
   res_font_title ? res_font_title : _ui_font()
}

fn _ui_small_font() {
   res_font_small ? res_font_small : _ui_font()
}

fn _ui_auto_dpi_scale() f64 {
   ui_app.app_dpi_scale_from_env_or_metrics(1.0, _win_h)
}

fn _dbg_ui(any msg) bool {
   if(_ui_debug_enabled == 1){
      ui_runtime.dbg("ui", to_str(msg))
   }
   true
}

fn _gui_layout_preset_env() str {
   def cli = str.strip(to_str(_cli_gui_layout))
   if(cli.len > 0){ return cli }
   mut raw = ui_profile.env_trim_cached("NY_UI_GUI_LAYOUT")
   if(raw.len == 0){ raw = ui_profile.env_trim_cached("NY_UI_GUI_LAYOUT_PRESET") }
   if(raw.len == 0){ raw = ui_profile.env_trim_cached("NY_GUI_LAYOUT") }
   if(raw.len > 0){ return raw }
   (_gui_layout_preset_name.len > 0) ? _gui_layout_preset_name : "default"
}

fn _gui_shot_env() str {
   def cli = str.strip(to_str(_cli_gui_shot))
   if(cli.len > 0){ return cli }
   mut raw = ui_profile.env_trim_cached("NY_UI_GUI_SHOT")
   if(raw.len == 0){ raw = ui_profile.env_trim_cached("NY_GUI_SHOT") }
   if(raw.len > 0){ return raw }
   str.strip(to_str(_gui_shot_name))
}

fn _batch_dump_enabled() bool {
   _batch_dump_active_mode && _batch_dump_models.len > 0
}

fn _proof_dump_active() bool {
   ui_profile.env_truthy_cached("NY_UI_PROOF_DUMP") ||
   ui_profile.env_truthy_cached("NY_UI_PROOF_SKYBOX")
}

fn _chrome_visible() bool {
   !ui_profile.nosurface_enabled() || ui_profile.headless_gui_enabled()
}

fn _gui_enabled_now() bool {
   _gui_visible || _gui_show_editor || _gui_show_gallery || _gui_show_probe ||
   _gui_show_browser || _gui_show_profiler || _gui_show_workspace ||
   _gui_show_graph || _gui_show_inspector
}

fn _ui_scene_visible() bool {
   show_scene && is_dict(active_scene)
}

fn _gui_probe_mode_enabled() bool {
   if(_gui_probe_mode == 1){ return true }
   def on =
   _gui_dump_suite_active ||
   _cli_gui_shot.len > 0 ||
   _gui_shot_name.len > 0 ||
   ui_profile.env_truthy_cached("NY_UI_GUI_PROBE") ||
   ui_profile.env_truthy_cached("NY_GUI_PROBE") ||
   ui_profile.env_truthy_cached("NY_UI_GUI_DUMP_PROBE")
   if(on){
      _gui_probe_mode = 1
      return true
   }
   if(_gui_probe_mode < 0){ _gui_probe_mode = 0 }
   false
}

fn _gui_shell_show(
   bool editor=false, bool gallery=false, bool probe=false, bool browser=false,
   bool inspector=false, bool profiler=false, bool workspace=false, bool graph=false
) dict {
   _gui_show_editor = editor
   _gui_show_gallery = gallery
   _gui_show_probe = probe
   _gui_show_browser = browser
   _gui_show_inspector = inspector
   _gui_show_profiler = profiler
   _gui_show_workspace = workspace
   _gui_show_graph = graph
   {
      "show_editor": _gui_show_editor, "show_gallery": _gui_show_gallery,
      "show_probe": _gui_show_probe, "show_browser": _gui_show_browser,
      "show_inspector": _gui_show_inspector, "show_profiler": _gui_show_profiler,
      "show_workspace": _gui_show_workspace, "show_graph": _gui_show_graph
   }
}

fn _gui_shell_tabs(any editor_tab=0, any workspace_mode=0, any center_tab=0, any side_tab=0) dict {
   _gui_editor_tab = int(editor_tab)
   _gui_workspace_mode = int(workspace_mode)
   _gui_center_tab = int(center_tab)
   _gui_side_tab = int(side_tab)
   {
      "editor_tab": _gui_editor_tab, "workspace_mode": _gui_workspace_mode,
      "center_tab": _gui_center_tab, "side_tab": _gui_side_tab
   }
}

fn _gui_sanitize_workspace_state() bool {
   def st = ui_shell.sanitize_workspace_state(
      _gui_editor_tab, _gui_workspace_mode, _gui_center_tab, _gui_side_tab,
   _gui_editor_tab_items.len)
   _gui_editor_tab = int(st.get("editor_tab", _gui_editor_tab))
   _gui_workspace_mode = int(st.get("workspace_mode", _gui_workspace_mode))
   _gui_center_tab = int(st.get("center_tab", _gui_center_tab))
   _gui_side_tab = int(st.get("side_tab", _gui_side_tab))
   true
}

fn _gui_sync_workspace_visibility() bool {
   def st = ui_shell.workspace_visibility(_gui_workspace_mode, _gui_center_tab, _gui_side_tab)
   _gui_show_workspace = bool(st.get("workspace", _gui_show_workspace))
   _gui_show_graph = bool(st.get("graph", _gui_show_graph))
   _gui_show_inspector = bool(st.get("inspector", _gui_show_inspector))
   _gui_show_profiler = bool(st.get("profiler", _gui_show_profiler))
   _gui_show_probe = bool(st.get("probe", _gui_show_probe))
   _gui_show_gallery = bool(st.get("gallery", _gui_show_gallery))
   true
}

fn _gui_capture_layout_state() dict {
   ui_shell.capture_layout_state(
      _gui_layout_preset_env(), _gui_workspace_mode, _gui_center_tab, _gui_side_tab,
      _gui_editor_tab, _gui_gallery_tab, _gui_browser_tab, _gui_inspector_tab,
      _gui_show_gallery, _gui_show_probe, _gui_show_browser, _gui_show_profiler,
      _gui_show_workspace, _gui_show_graph, _gui_show_inspector,
      _gui_scale, _gui_layout_gap
   )
}

fn _gui_apply_layout_state(any st) bool {
   if(!is_dict(st)){ return false }
   _gui_layout_preset_name = to_str(st.get("preset", _gui_layout_preset_name))
   _gui_workspace_mode = int(st.get("mode", _gui_workspace_mode))
   _gui_center_tab = int(st.get("center_tab", _gui_center_tab))
   _gui_side_tab = int(st.get("side_tab", _gui_side_tab))
   _gui_editor_tab = int(st.get("editor_tab", _gui_editor_tab))
   _gui_gallery_tab = int(st.get("gallery_tab", _gui_gallery_tab))
   _gui_browser_tab = int(st.get("browser_tab", _gui_browser_tab))
   _gui_inspector_tab = int(st.get("inspector_tab", _gui_inspector_tab))
   _gui_show_gallery = bool(st.get("show_gallery", _gui_show_gallery))
   _gui_show_probe = bool(st.get("show_probe", _gui_show_probe))
   _gui_show_browser = bool(st.get("show_browser", _gui_show_browser))
   _gui_show_profiler = bool(st.get("show_profiler", _gui_show_profiler))
   _gui_show_workspace = bool(st.get("show_workspace", _gui_show_workspace))
   _gui_show_graph = bool(st.get("show_graph", _gui_show_graph))
   _gui_show_inspector = bool(st.get("show_inspector", _gui_show_inspector))
   _gui_scale = float(st.get("scale", _gui_scale))
   _gui_layout_gap = float(st.get("gap", _gui_layout_gap))
   _gui_visible = _gui_enabled_now()
   _gui_layout_dirty = true
   _gui_layout_warm_frames = 4
   true
}

fn _gui_invalidate_model_filter_cache() bool {
   _gui_filtered_model_names = []
   _gui_filter_cache_key = "\x00"
   _gui_filter_cache_source_len = -1
   _gui_selected_pick_cache_name = "\x00"
   _gui_selected_pick_cache_filter = "\x00"
   _gui_selected_pick_cache_len = -1
   _gui_selected_pick_cache_idx = -1
   _gui_asset_state_cache_sig = "\x00"
   _gui_asset_state_cache = dict(0)
   true
}

fn _gui_invalidate_model_catalog_caches() bool {
   _model_names = []
   _gui_invalidate_model_filter_cache()
   viewer_catalog.invalidate_caches()
   true
}

fn _refresh_model_catalog() list {
   _model_names = viewer_catalog.refresh_names(_model_names)
   _model_names
}

fn _gui_sync_model_selection() bool {
   _gui_model_selected_name = viewer_catalog.sync_selected(
      _gui_model_selected_name,
      _loaded_scene_name,
   _refresh_model_catalog())
   true
}

fn _gui_filtered_model_catalog(any names) list {
   def res = viewer_catalog.filter_cached(
      names,
      _gui_model_filter,
      _gui_filter_cache_key,
      _gui_filter_cache_source_len,
   _gui_filtered_model_names)
   _gui_filtered_model_names = res.get("items", [])
   _gui_filter_cache_key = to_str(res.get("key", "\x00"))
   _gui_filter_cache_source_len = int(res.get("source_len", -1))
   _gui_filtered_model_names
}

fn _gui_selected_pick_index(any items, any name) int {
   mut dict: cache = dict(4)
   cache["name"] = _gui_selected_pick_cache_name
   cache["filter"] = _gui_selected_pick_cache_filter
   cache["len"] = _gui_selected_pick_cache_len
   cache["idx"] = _gui_selected_pick_cache_idx
   def res = viewer_catalog.pick_index_cached(
      cache,
      items,
      name,
   _gui_model_filter)
   _gui_selected_pick_cache_name = to_str(res.get("name", "\x00"))
   _gui_selected_pick_cache_filter = to_str(res.get("filter", "\x00"))
   _gui_selected_pick_cache_len = int(res.get("len", -1))
   _gui_selected_pick_cache_idx = int(res.get("idx", -1))
   _gui_selected_pick_cache_idx
}

fn _gui_asset_state_sig(list names) str {
   asset_catalog.catalog_filter_key(_gui_model_filter) + "|" +
   to_str(names.len) + "|" + _loaded_scene_name + "|" + _gui_model_selected_name + "|" +
   to_str(_gui_model_show_paths ? 1 : 0) + "|" + to_str(_gui_browser_tab) + "|" +
   to_str(is_dict(active_scene) ? 1 : 0)
}

fn _gui_asset_derived_state() dict {
   def names = _refresh_model_catalog()
   _gui_model_selected_name = viewer_catalog.sync_selected(_gui_model_selected_name, _loaded_scene_name, names)
   def sig = _gui_asset_state_sig(names)
   if(sig == _gui_asset_state_cache_sig && is_dict(_gui_asset_state_cache) && _gui_asset_state_cache.contains("names")){
      return _gui_asset_state_cache
   }
   def filtered_names = _gui_filtered_model_catalog(names)
   def filter_key = asset_catalog.catalog_filter_key(_gui_model_filter)
   def pick_names = (filter_key.len > 0) ? filtered_names : names
   def selected_idx = max(0, _gui_selected_pick_index(pick_names, _gui_model_selected_name))
   mut dict: state_cache = dict(6)
   state_cache["names"] = names
   state_cache["filtered_names"] = filtered_names
   state_cache["filter_key"] = filter_key
   state_cache["selected_idx"] = selected_idx
   state_cache["catalog"] = ui_assets.gltf_asset_catalog()
   _gui_asset_state_cache_sig = sig
   _gui_asset_state_cache = state_cache
   _gui_asset_state_cache
}

fn _gui_init_editor_graph() bool {
   if(_gui_graph_nodes.len == 0){
      _gui_graph_nodes = demo_editor.graph_nodes()
   }
   if(_gui_graph_links.len == 0){
      _gui_graph_links = demo_editor.graph_links()
   }
   true
}

fn _scene_selection_bounds_cache_clear() bool {
   _scene_selection_cached_bounds = []
   _scene_selection_cached_name = ""
   true
}

fn _scene_selection_bounds_cache_key() str {
   if(!is_dict(active_scene)){ return _loaded_scene_name + "|none" }
   _loaded_scene_name + "|" +
   to_str(active_scene.get("fit_scale", 1.0)) + "|" +
   to_str(active_scene.get("fit_tx", 0.0)) + "|" +
   to_str(active_scene.get("fit_ty", 0.0)) + "|" +
   to_str(active_scene.get("fit_tz", 0.0)) + "|" +
   to_str(active_scene.get("edit_tx", 0.0)) + "|" +
   to_str(active_scene.get("edit_ty", 0.0)) + "|" +
   to_str(active_scene.get("edit_tz", 0.0)) + "|" +
   to_str(active_scene.get("edit_rx", 0.0)) + "|" +
   to_str(active_scene.get("edit_ry", 0.0)) + "|" +
   to_str(active_scene.get("edit_rz", 0.0)) + "|" +
   to_str(active_scene.get("edit_scale", 1.0)) + "|" +
   to_str(active_scene.get("edit_sx", active_scene.get("edit_scale", 1.0))) + "|" +
   to_str(active_scene.get("edit_sy", active_scene.get("edit_scale", 1.0))) + "|" +
   to_str(active_scene.get("edit_sz", active_scene.get("edit_scale", 1.0)))
}

fn _scene_selection_bounds() list {
   def cache_key = _scene_selection_bounds_cache_key()
   if(_scene_selection_cached_name == cache_key && is_list(_scene_selection_cached_bounds)){
      return _scene_selection_cached_bounds
   }
   if(!is_dict(active_scene)){
      _scene_selection_cached_bounds = []
      _scene_selection_cached_name = cache_key
      return []
   }
   _scene_selection_cached_bounds = ui_selection.selection_bounds_from_scene(active_scene)
   _scene_selection_cached_name = cache_key
   _scene_selection_cached_bounds
}

fn _scene_selection_bounds_cache_update() bool {
   def bounds = _scene_selection_bounds()
   is_list(bounds) && bounds.len >= 6
}

fn _selection_overlay_clear_rects() bool {
   def rects = ui_selection.selection_overlay_rects()
   _selection_overlay_panel_rect = rects.get(0, [0.0, 0.0, 0.0, 0.0])
   _selection_overlay_toolbar_rect = rects.get(1, [0.0, 0.0, 0.0, 0.0])
   _selection_overlay_move_rect = rects.get(2, [0.0, 0.0, 0.0, 0.0])
   _selection_overlay_rotate_rect = rects.get(3, [0.0, 0.0, 0.0, 0.0])
   _selection_overlay_scale_rect = rects.get(4, [0.0, 0.0, 0.0, 0.0])
   _selection_overlay_hover_mode = -1
   true
}

fn _selection_overlay_hit_test(any x, any y) bool {
   if(!_scene_editor_tools_enabled()){ return false }
   _selection_overlay_hover_mode = ui_selection.selection_overlay_hit_test(
      x, y,
      _selection_overlay_panel_rect,
      _selection_overlay_toolbar_rect,
      _selection_overlay_move_rect,
      _selection_overlay_rotate_rect,
   _selection_overlay_scale_rect)
   _selection_overlay_hover_mode >= 0
}

fn _selection_overlay_handle_click(any x, any y) bool {
   if(!_scene_editor_tools_enabled()){ return false }
   def hit = ui_selection.selection_overlay_hit_test(
      x, y,
      _selection_overlay_panel_rect,
      _selection_overlay_toolbar_rect,
      _selection_overlay_move_rect,
      _selection_overlay_rotate_rect,
   _selection_overlay_scale_rect)
   _selection_overlay_hover_mode = hit
   if(hit <= 0){ return hit == 0 }
   _gizmo_mode = hit - 1
   true
}

fn _apply_selection_overlay_probe_env() bool {
   mut mode_name = ui_profile.env_trim_cached("NY_UI_GIZMO")
   if(mode_name.len == 0){ mode_name = ui_profile.env_trim_cached("NY_UI_SELECTION_GIZMO") }
   if(mode_name.len == 0){ mode_name = ui_profile.env_trim_cached("NY_UI_GIZMO_MODE") }
   def requested =
   mode_name.len > 0 ||
   ui_profile.env_truthy_cached("NY_UI_SELECTION_OVERLAY_PROBE") ||
   ui_profile.env_truthy_cached("NY_UI_SELECTION_OVERLAY_DUMP") ||
   ui_profile.env_truthy_cached("NY_UI_GIZMO_PROBE")
   if(!requested){ return false }
   if(mode_name.len > 0){
      _gizmo_mode = ui_selection.selection_gizmo_mode(mode_name, _gizmo_mode)
   }
   if(is_dict(active_scene)){ show_scene = true }
   _scene_selected = is_dict(active_scene) && show_scene
   _scene_selection_rect = true
   _selection_overlay_dump_mode = 1
   _gui_probe_mode = 1
   _gui_visible = false
   _gui_shot_name = "gizmo_" + ui_selection.selection_gizmo_label(_gizmo_mode)
   _scene_selection_bounds_cache_update()
   _selection_overlay_clear_rects()
   _gizmo_trace("env mode=" + ui_selection.selection_gizmo_label(_gizmo_mode)
      + " active=" + to_str(is_dict(active_scene))
   + " selected=" + to_str(_scene_selected))
   true
}

fn _gizmo_trace(any msg) bool {
   if(ui_profile.env_truthy_cached("NY_UI_GIZMO_TRACE") || ui_profile.trace_enabled()){
      ui_profile.print_text("[ui:gizmo] " + to_str(msg))
   }
   true
}

fn _gui_apply_shell_shot(any shot_name) bool {
   mut shot = str.strip(to_str(shot_name))
   if(shot.len == 0){ shot = "full_editor" }
   def plan = ui_shell.shot_plan(shot)
   _gui_shell_show(
      bool(plan.get("show_editor", false)),
      bool(plan.get("show_gallery", false)),
      bool(plan.get("show_probe", false)),
      bool(plan.get("show_browser", false)),
      bool(plan.get("show_inspector", false)),
      bool(plan.get("show_profiler", false)),
      bool(plan.get("show_workspace", false)),
   bool(plan.get("show_graph", false)))
   _gui_shell_tabs(
      int(plan.get("editor_tab", _gui_editor_tab)),
      int(plan.get("workspace_mode", _gui_workspace_mode)),
      int(plan.get("center_tab", _gui_center_tab)),
   int(plan.get("side_tab", _gui_side_tab)))
   _gui_browser_tab = int(plan.get("browser_tab", _gui_browser_tab))
   _gui_gallery_tab = int(plan.get("gallery_tab", _gui_gallery_tab))
   _gui_inspector_tab = int(plan.get("inspector_tab", _gui_inspector_tab))
   def filter = to_str(plan.get("model_filter", _gui_model_filter))
   if(filter != _gui_model_filter){
      _gui_model_filter = filter
      _gui_invalidate_model_filter_cache()
   }
   _gui_probe_overlay = bool(plan.get("probe_overlay", _gui_probe_overlay))
   _gui_shot_name = shot
   _gui_visible = _gui_enabled_now()
   _gui_probe_mode = 1
   _gui_layout_dirty = true
   _gui_layout_warm_frames = 4
   ui_shell.reset_tool_scrolls()
   true
}

fn _gui_apply_probe_preset() bool {
   def shot = _gui_shot_env()
   if(shot.len > 0){
      _gui_visible = true
      return _gui_apply_shell_shot(shot)
   }
   if(!_gui_probe_mode_enabled()){
      return false
   }
   _gui_visible = true
   _gui_show_editor = true
   _gui_probe_bootstrapped = true
   _gui_layout_dirty = true
   _gui_layout_warm_frames = 4
   true
}

fn _gui_close_editor_shell() bool {
   _gui_visible = false
   _gui_shell_show(false, false, false, false, false, false, false, false)
   ui_shell.hide_tools()
   _gui_layout_dirty = true
   _gui_layout_warm_frames = 2
   true
}

fn _gui_editor_shell_open() bool {
   _gui_visible && (
      _gui_show_editor || _gui_show_inspector || _gui_show_browser ||
      _gui_show_gallery || _gui_show_probe || _gui_show_profiler ||
      _gui_show_workspace || _gui_show_graph
   )
}

fn _gui_close_tool_window(any id, bool break_dock=true) bool {
   if(!ui_editor.tool_closed(id)){ return false }
   if(break_dock && _gui_workspace_mode == 1){ _gui_workspace_mode = 0 }
   _gui_layout_dirty = true
   _gui_layout_warm_frames = 4
   true
}

fn _gui_has_workspace_peer() bool {
   _gui_show_inspector || _gui_show_profiler ||
   _gui_show_workspace || _gui_show_graph || _gui_show_probe || _gui_show_gallery
}

fn _gui_has_tiled_peer() bool {
   _gui_show_editor || _gui_workspace_mode == 1 || _gui_has_workspace_peer()
}

fn _gui_pinned_tool_opts(bool scrollable=true) dict {
   mut opts = dict(8)
   opts["scrollable"] = bool(scrollable)
   opts["closable"] = false
   opts["movable"] = false
   opts["resizable"] = false
   opts["collapsible"] = false
   opts["titlebar"] = false
   opts
}

fn _gui_fit_standalone_editor() bool {
   if(!_gui_show_editor || _gui_workspace_mode == 1){ return false }
   ui_editor.apply_tool_rect_if_visible("editor_main", true, [0.0, 0.0, max(1.0, _win_w), max(1.0, _win_h)])
   true
}

fn _gui_fit_standalone_asset_browser() bool {
   if(!_gui_show_browser || _gui_show_editor){ return false }
   ui_editor.apply_focus_layout(["asset_browser"], _win_w, _win_h, _gui_layout_gap)
   true
}

fn _gui_apply_tiled_layout(bool force=false) bool {
   if(!force && !_gui_layout_dirty && _gui_layout_warm_frames <= 0){ return false }
   _gui_sanitize_workspace_state()
   def plan = ui_shell.tiled_layout_plan(
      _gui_layout_preset_env(), _win_w, _win_h, _gui_layout_gap, _gui_workspace_mode,
      _gui_show_editor, _gui_show_gallery, _gui_show_probe, _gui_show_browser,
      _gui_show_inspector, _gui_show_profiler, _gui_show_workspace, _gui_show_graph
   )
   if(bool(plan.get("focus_only", false))){
      def ids = ui_shell.focus_window_ids(
         _gui_show_profiler, _gui_show_workspace, _gui_show_graph, _gui_show_inspector,
      _gui_show_browser, _gui_show_editor, _gui_show_probe, _gui_show_gallery)
      ui_editor.apply_focus_layout(ids, _win_w, _win_h, _gui_layout_gap)
   } else {
      ui_shell.apply_tool_plan(plan)
   }
   if(_gui_layout_warm_frames > 0){ _gui_layout_warm_frames -= 1 }
   else { _gui_layout_dirty = false }
   true
}

fn _gui_refresh_auto_scale() bool {
   if(_gui_scale_manual || ui_profile.gui_scale_env_present()){ return false }
   def next = ui_app.app_gui_scale_for_window(_win_w, _win_h, _gui_dpi_scale)
   if(ui_app.app_absf(next - _gui_scale) <= 0.001){ return false }
   _gui_scale = next
   _gui_layout_dirty = true
   _gui_layout_warm_frames = 3
   true
}


fn _ui_cli_color(str code, str text) str {
   if(common.env_truthy("NO_COLOR")){ return text }
   def esc = chr(27)
   esc + "[" + code + "m" + text + esc + "[0m"
}

fn _ui_has_help_arg() bool {
   mut i = 1
   while(i < argc()){
      def a = str.lower(str.strip(to_str(argv(i))))
      if(a == "-h" || a == "--help" || a == "help"){ return true }
      i += 1
   }
   false
}

fn _ui_help_line(str left, str right) any {
   print("  " + _ui_cli_color("1;36", left) + "  " + right)
   0
}

fn _ui_print_help() any {
   print(_ui_cli_color("1;37", "Nytrix UI Engine"))
   print(_ui_cli_color("90", "Renderer, model viewer, GUI/editor shell, diagnostics, and benchmark harness"))
   print("")
   print(_ui_cli_color("1;33", "Usage"))
   print("  ./make ny etc/projects/ui/engine.ny " + _ui_cli_color("36", "[options]") + " " + _ui_cli_color("90", "[load MODEL | commands]") )
   print("")
   print(_ui_cli_color("1;33", "Renderer"))
   _ui_help_line("-gl, --gl", "use OpenGL")
   _ui_help_line("-vk, --vk", "use Vulkan")
   _ui_help_line("-auto", "auto-select Vulkan/OpenGL/mock")
   _ui_help_line("-mock, -cpu", "software/headless mock renderer")
   print("")
   print(_ui_cli_color("1;33", "Scenes / commands"))
   _ui_help_line("load NAME", "load asset/model by catalog name")
   _ui_help_line("-gltf PATH", "load a glTF file")
   _ui_help_line("-ex CMD", "run viewer command after startup")
   _ui_help_line("--gui-shot NAME", "open a GUI tool/panel for dumps")
   print("")
   print(_ui_cli_color("1;33", "Headless / test"))
   _ui_help_line("--headless", "headless mock/no-surface run")
   _ui_help_line("--headless-sim", "headless simulation/benchmark path")
   _ui_help_line("--dump", "capture output frame")
   _ui_help_line("--dump-path PATH", "capture to PATH")
   _ui_help_line("--timeout SEC", "auto-close after idle timeout")
   print("")
   print(_ui_cli_color("1;33", "Debug"))
   _ui_help_line("-v, --verbose", "bounded startup/input/render diagnostics")
   _ui_help_line("-vv", "compact deep diagnostics/profiler summaries")
   _ui_help_line("--trace-spam", "last-resort per-stage/per-glyph/per-frame tracing")
   _ui_help_line("--render-trace", "print render frame trace")
   print("")
   print(_ui_cli_color("1;33", "Examples"))
   print("  ./make ny etc/projects/ui/engine.ny -v -vk load BoxAnimated")
   print("  ./make ny etc/projects/ui/engine.ny --headless --dump load Avocado")
   0
}

fn _ui_apply_cli_options() bool {
   ui_profile.apply_verbose_argv()
   def opts = viewer_cli.parse_options(viewer_cli.argv_list())
   if(bool(opts.get("verbose", false))){ ui_profile.apply_verbose_argv() }
   if(bool(opts.get("surfaced_headless", false))){
      ui_profile.force_surfaced_headless()
   } elif(bool(opts.get("headless", false))){
      ui_profile.force_headless(bool(opts.get("nosurface", true)), bool(opts.get("bench", false)), bool(opts.get("sim", false)))
   }
   if(bool(opts.get("frame_hash_requested", false))){
      ui_profile.force_frame_hash_lock()
   }
   if(bool(opts.get("profile", false))){
      ui_profile.profile_dump_force(true)
   }
   if(bool(opts.get("profile_frame_trace", false)) || bool(opts.get("frame_trace", false))){
      ui_profile.set_bool("NY_UI_PROFILE_TRACE", true)
   }
   if(bool(opts.get("render_trace", false))){
      ui_profile.set_bool("NY_GFX_FRAME_TRACE", true)
   }
   if(bool(opts.get("frame_print_every", false))){
      ui_profile.force_frame_print_every(1)
   }
   def profile_dir = to_str(opts.get("profile_dir", ""))
   if(profile_dir.len > 0){
      ui_profile.profile_dump_set_path(ospath.join(profile_dir, "nytrix_ui_profile.oasset.jsonl"))
   }
   _cli_dump_requested = bool(opts.get("dump_requested", false))
   _cli_dump_all_requested = bool(opts.get("dump_all", false))
   _cli_dump_missing_requested = bool(opts.get("dump_missing", false))
   _cli_dump_dir = to_str(opts.get("dump_dir", ""))
   _cli_batch_models_raw = to_str(opts.get("batch_models_raw", ""))
   _cli_dump_delay_frames = int(opts.get("dump_delay_frames", -1))
   _cli_timeout_sec = float(opts.get("timeout_sec", -1.0))
   _cli_post_load_cmds = opts.get("post_load_cmds", [])
   _cli_gui_layout = to_str(opts.get("gui_layout", ""))
   _cli_gui_shot = to_str(opts.get("gui_shot", ""))
   _cli_render_backend = to_str(opts.get("render_backend", ""))
   if(_cli_gui_layout.len > 0){ _gui_layout_preset_name = _cli_gui_layout }
   if(_cli_gui_shot.len > 0){
      _gui_probe_mode = 1
      _gui_shot_name = _cli_gui_shot
      _gui_visible = true
   }
   def dump_path = to_str(opts.get("dump_path", ""))
   if(dump_path.len > 0){ _auto_dump_path = dump_path }
   true
}

;; ---- core ----
fn from_env() dict {
   "Build the demo UI config from current CLI and environment state."
   mut dict: cfg = dict(8)
   cfg["headless"] = ui_profile.headless_enabled()
   cfg["headless_gui"] = ui_profile.headless_gui_enabled()
   cfg["headless_sim"] = ui_profile.headless_sim_enabled()
   cfg["bench"] = ui_profile.ui_bench_enabled()
   cfg["nosurface"] = ui_profile.nosurface_enabled()
   cfg["gui_probe"] = _gui_probe_mode_enabled()
   cfg["dump_path"] = _auto_dump_path
   cfg
}

fn _gui_refresh_frame_metrics() {
   mut int: interval = ui_profile.env_int_cached("NY_UI_GUI_STATS_INTERVAL", 18, 1, 240)
   if(_gui_probe_mode_enabled() || _gui_dump_suite_active_now()){
      interval = 1
   }
   elif(_gui_show_profiler){
      interval = 4
   }
   elif(_gui_show_inspector){ interval = min(interval, 12) }
   if(ui_profile.trace_enabled()){
      interval = int(min(interval, 4))
   }
   if(ui_profile.parity_lock_stats_enabled()){
      fps = 0
      _gui_frame_stats_refresh_frame = total_frames
      _gui_frame_stats_cache = {
         "draws": 0, "dynamic_draws": 0, "static_draws": 0, "indexed_draws": 0,
         "flushes": 0, "pipeline_binds": 0, "descriptor_binds": 0, "submitted_vertices": 0
      }
      _gui_scene_parts_cache = 0
      _gui_renderer_hotspot_cache = "steady"
      return
   }
   if(_gui_frame_stats_refresh_frame >= 0 && (total_frames - _gui_frame_stats_refresh_frame) < interval){
      return
   }
   _gui_frame_stats_refresh_frame = total_frames
   _gui_frame_stats_cache = renderer_frame_stats()
   _gui_scene_parts_cache = asset_catalog.scene_part_count(active_scene)
   _gui_renderer_hotspot_cache = str.strip(to_str(ui_app.app_renderer_hotspot_label(_gui_frame_stats_cache)))
   if(_gui_renderer_hotspot_cache.len == 0 || _gui_renderer_hotspot_cache == "<nil>"){ _gui_renderer_hotspot_cache = "steady" }
}

fn _batch_dump_terminal_log(msg) {
   if(asset_batch.terminal_log_enabled(_batch_dump_enabled(), ui_profile.batch_fast_env_enabled())){
      terminal.log(msg)
   }
}

fn _prime_dump_anim_pose() {
   if(!ui_profile.dump_pose_enabled(_auto_dump_enabled, _cli_dump_requested, _batch_dump_enabled(), _gui_dump_suite_active, _gui_probe_mode_enabled()) || !is_dict(active_scene) || !_anim_enabled || _anim_duration <= 0.0001){
      return false
   }
   def pose_frac = asset_batch.dump_anim_pose_fraction(0.35)
   mut pose_time = 0.0
   if(pose_frac > 0.0){
      pose_time = _anim_duration * pose_frac
   }
   if(is_nan(pose_time) || is_inf(pose_time) || pose_time < 0.0 || pose_time > _anim_duration){
      pose_time = _anim_duration * pose_frac
   }
   _anim_time = float(pose_time)
   _anim_speed = 0.0
   active_scene["anim_idx"] = _anim_idx
   active_scene["anim_time"] = _anim_time
   active_scene["anim_duration"] = _anim_duration
   active_scene["anim_time_override"] = true
   _dbg_ui("[ui] dump anim pose: t=" + f"{_anim_time:.3f}" + "/" + f"{_anim_duration:.3f}")
   true
}

fn _freeze_dump_deform_pose() {
   if(!ui_profile.dump_pose_enabled(_auto_dump_enabled, _cli_dump_requested, _batch_dump_enabled(), _gui_dump_suite_active, _gui_probe_mode_enabled()) || !is_dict(active_scene)){
      return false
   }
   def skin_count = int(active_scene.get("skin_count", 0))
   def morph_count = int(active_scene.get("morph_target_count", 0))
   if(skin_count <= 0 && morph_count <= 0){
      return false
   }
   _anim_enabled = false
   _anim_speed = 0.0
   _anim_count = 0
   active_scene["anim_count"] = 0
   active_scene["skin_count"] = 0
   active_scene["morph_target_count"] = 0
   active_scene["anim_time"] = _anim_time
   active_scene["anim_time_override"] = true
   active_scene["gpu_parts"] = []
   active_scene["gpu_parts_slab"] = 0
   active_scene["gpu_parts_count"] = 0
   active_scene["gpu_draw_state"] = [
      0, 0, 0, 0, 1, 0,
      active_scene.get("scene_lights_slab", 0),
      int(active_scene.get("scene_lights_count", 0)),
      (active_scene.get("has_optical", false) ? 1 : 0)
   ]
   active_scene["gpu_model_baked"] = false
   active_scene["parts_model_baked"] = false
   active_scene["fit_applied"] = false
   true
}

fn _stop_model_prefetch() {
   if(_model_prefetch_thread){
      thread_join(_model_prefetch_thread)
      _model_prefetch_thread = 0
   }
   _model_prefetch_name = ""
}

fn _schedule_model_prefetch(idx) {
   def names = _refresh_model_catalog()
   if(!is_list(names) || idx < 0 || idx >= names.len){
      return false
   }
   def name = to_str(names.get(idx, ""))
   if(name.len == 0){
      return false
   }
   if(_model_prefetch_thread && _model_prefetch_name == name){
      return true
   }
   _stop_model_prefetch()
   _model_prefetch_name = name
   _model_prefetch_thread = int(thread_spawn(fn() {
            ui_assets.prefetch_gltf_asset(name)
   }))
   _model_prefetch_thread != 0
}

fn _scene_load_async_mutex() {
   if(!_scene_load_async_mu){
      _scene_load_async_mu = mutex_new()
   }
   _scene_load_async_mu
}

fn _scene_load_async_active() {
   _scene_load_async_thread != 0 && is_dict(_scene_load_async_job)
}

fn _scene_load_async_status() {
   if(!_scene_load_async_active()){
      return [false, false]
   }
   def mu = _scene_load_async_mutex()
   if(mu){
      mutex_lock(mu)
   }
   def done = bool(_scene_load_async_job.get("done", false))
   def ok = bool(_scene_load_async_job.get("ok", false))
   if(mu){
      mutex_unlock(mu)
   }
   [done, ok]
}

fn _scene_load_async_clear() {
   _scene_load_async_job = 0
   _scene_load_async_spec = ""
   _scene_load_async_path = ""
   _scene_load_async_display = ""
   _scene_load_async_auto_frame = true
   _sync_cursor_state("scene load clear")
}

fn _scene_select_active_loaded(str reason="scene loaded") bool {
   _scene_selection_bounds_cache_clear()
   _selection_overlay_clear_rects()
   if(!is_dict(active_scene) || !show_scene){
      _scene_selected = false
      _scene_selection_rect = false
      _sync_cursor_state(reason)
      return false
   }
   if(!_scene_editor_tools_enabled()){
      _scene_selected = false
      _scene_selection_rect = false
      _sync_cursor_state(reason)
      return false
   }
   _scene_selection_rect = true
   _scene_selected = true
   _scene_selection_bounds_cache_update()
   _scene_edit_redraw(3)
   _sync_cursor_state(reason)
   true
}

fn _scene_runtime_finish_loaded(new_scene, old_scene, old_name, auto_frame=true) {
   if(!is_dict(new_scene)){
      return false
   }
   _scene_selection_bounds_cache_clear()
   if(is_dict(old_scene)){
      scene_engine.unload_scene(old_scene, old_name)
   }
   active_scene = new_scene
   _loaded_scene_name = to_str(active_scene.get("scene_name", _loaded_scene_name))
   _gui_material_selected_idx = -1
   _gui_material_selected_part_idx = -1
   if(_loaded_scene_name.len == 0){
      _loaded_scene_name = "Scene"
   }
   show_scene = true
   _invalidate_chrome_frame(4)
   _sync_cam_state_from_camthreed()
   _sync_anim_state_from_scene(active_scene)
   _apply_batch_scene_clear_color(_loaded_scene_name)
   _prime_dump_anim_pose()
   _sync_model_index_from_loaded()
   _release_scene_loading_input("scene loaded")
   gui.suppress_mouse_clicks(10, true)
   if(auto_frame){
      _cmd_autofit(false)
      _cmd_lookat(false)
   }
   _scene_select_active_loaded("scene loaded")
   terminal.log(_loaded_scene_name + " loaded")
   if(ui_profile.env_truthy_cached("NY_UI_ADJACENT_PREFETCH")){
      _schedule_adjacent_prefetch_from_current(1)
   }
   true
}

fn _resolve_gltf_path_or_log(want) {
   def gltf_path = ui_assets.resolve_gltf_asset_path(want)
   if(gltf_path.len == 0){
      terminal.log("ERROR: glTF asset not found: " + want)
   }
   gltf_path
}

fn _finish_scene_load_path(gltf_path, want, auto_frame=true) {
   mut any: old_scene = active_scene
   mut old_name = _loaded_scene_name
   if(is_dict(old_scene) && old_name == want){
      scene_engine.unload_scene(old_scene, old_name)
      active_scene = 0
      show_scene = false
      _scene_selected = false
      _loaded_scene_name = ""
      _clear_anim_state()
      _scene_edit_redraw(2)
      old_scene = 0
      old_name = ""
   }
   def new_scene = scene_engine.load_scene_path(gltf_path, "", camthreed, M_SP, M_PT, M_PS)
   if(!is_dict(new_scene)){
      terminal.log("Load failed: " + want)
      return false
   }
   _scene_runtime_finish_loaded(new_scene, old_scene, old_name, auto_frame)
}

fn _load_scene_runtime_sync(spec, auto_frame=true, show_loading=true) {
   def want = str.strip(to_str(spec))
   if(want.len == 0){
      return false
   }
   if(to_str(str.lower(want)) == "list"){
      def names = ui_assets.list_gltf_asset_names()
      if(names.len == 0){
         terminal.log("No glTF folders found in " + asset_catalog.format_name_list(ui_assets.gltf_asset_roots()))
      }
      else { terminal.log("glTF folders: " + scene_engine.format_name_list(names)) }
      return true
   }
   def gltf_path = _resolve_gltf_path_or_log(want)
   if(gltf_path.len == 0){
      return false
   }
   if(show_loading && !is_dict(active_scene)){
      _release_scene_loading_input("scene load sync")
      _present_loading_frame("Loading " + want)
   }
   _finish_scene_load_path(gltf_path, want, auto_frame)
}

fn _load_scene_runtime_async(spec, auto_frame=true) {
   def want = str.strip(to_str(spec))
   if(want.len == 0){
      return false
   }
   if(to_str(str.lower(want)) == "list"){
      return _load_scene_runtime_sync(want, auto_frame, false)
   }
   if(_scene_load_async_active()){
      terminal.log("Load already in progress: " + _scene_load_async_spec)
      return false
   }
   def gltf_path = _resolve_gltf_path_or_log(want)
   if(gltf_path.len == 0){
      return false
   }
   _app_release_pointer_ownership("scene load start")
   mut job = dict(4)
   job["done"] = false
   job["ok"] = false
   _scene_load_async_job = job
   _scene_load_async_spec = want
   _scene_load_async_path = gltf_path
   _scene_load_async_display = want
   _scene_load_async_auto_frame = auto_frame
   _scene_load_async_thread = int(thread_spawn(fn() {
            def ok = ui_assets.prefetch_gltf_asset(gltf_path)
            def mu = _scene_load_async_mutex()
            if(mu){
               mutex_lock(mu)
            }
            job["ok"] = ok ? true : false
            job["done"] = true
            if(mu){
               mutex_unlock(mu)
            }
   }))
   if(!_scene_load_async_thread){
      _scene_load_async_clear()
      return _load_scene_runtime_sync(want, auto_frame, false)
   }
   if(!is_dict(active_scene)){
      show_scene = false
   }
   _sync_cursor_state("scene load async")
   terminal.log("Loading in background: " + want)
   false
}

fn _poll_scene_load_async() {
   if(!_scene_load_async_active()){
      return false
   }
   _release_scene_loading_input("scene load pending")
   def st = _scene_load_async_status()
   if(!bool(st.get(0, false))){
      return false
   }
   thread_join(_scene_load_async_thread)
   _scene_load_async_thread = 0
   def ok = bool(st.get(1, false))
   def want = _scene_load_async_spec
   def gltf_path = _scene_load_async_path
   def auto_frame = _scene_load_async_auto_frame
   _scene_load_finalizing = true
   _scene_load_async_clear()
   if(!ok){
      terminal.log("Background load failed: " + want)
      if(_startup_post_load_pending){
         _startup_post_load_pending = false
      }
      _scene_load_finalizing = false
      _sync_cursor_state("scene load failed")
      return false
   }
   terminal.log("Finalizing: " + want)
   _present_loading_frame("Finalizing " + want)
   def loaded_ok = _finish_scene_load_path(gltf_path, want, auto_frame)
   if(_startup_post_load_pending){
      _startup_post_load_pending = false
      if(loaded_ok){
         _startup_exec_post_load_cmds()
      }
   }
   if(loaded_ok){
      _present_loading_frame("Loaded " + want)
   }
   _scene_load_finalizing = false
   _sync_cursor_state("scene load finished")
   loaded_ok
}

fn _schedule_adjacent_prefetch_from_current(dir=1) {
   def names = _refresh_model_catalog()
   if(!is_list(names) || names.len == 0){
      return false
   }
   mut idx = _sync_model_index_from_loaded()
   if(idx < 0){
      return false
   }
   mut next_idx = idx + dir
   while(next_idx < 0){
      next_idx += names.len
   }
   while(next_idx >= names.len){
      next_idx -= names.len
   }
   _schedule_model_prefetch(next_idx)
}

fn _schedule_batch_prefetch_next() {
   if(!ui_profile.batch_prefetch_enabled()){
      return false
   }
   if(!_batch_dump_enabled()){
      return false
   }
   def next_idx = _batch_dump_index + 1
   if(next_idx < 0 || next_idx >= _batch_dump_models.len){
      return false
   }
   def name = to_str(_batch_dump_models.get(next_idx, ""))
   def spec = to_str(_batch_dump_model_specs.get(next_idx, name))
   if(spec.len == 0){
      return false
   }
   if(_model_prefetch_thread && _model_prefetch_name == spec){
      return true
   }
   _stop_model_prefetch()
   _model_prefetch_name = spec
   _model_prefetch_thread = int(thread_spawn(fn() {
            ui_assets.prefetch_gltf_asset(spec)
   }))
   _model_prefetch_thread != 0
}

fn _sync_model_index_from_loaded() {
   _model_index = viewer_catalog.model_index(_refresh_model_catalog(), _loaded_scene_name)
   _model_index
}

fn _load_scene_runtime(spec, auto_frame=true) {
   def want = str.strip(to_str(spec))
   if(want.len == 0){
      return false
   }
   if(!_batch_dump_enabled() && !ui_profile.headless_enabled()){
      return _load_scene_runtime_async(want, auto_frame)
   }
   return _load_scene_runtime_sync(want, auto_frame, true)
}

fn _cycle_loaded_model(dir) {
   def names = _refresh_model_catalog()
   if(!is_list(names) || names.len == 0){
      terminal.log("No glTF models found")
      return false
   }
   def count = names.len
   mut idx = _sync_model_index_from_loaded()
   if(idx < 0){
      idx = 0
   }
   mut next_idx = idx + dir
   while(next_idx < 0){
      next_idx += count
   }
   while(next_idx >= count){
      next_idx -= count
   }
   def name = to_str(names.get(next_idx, ""))
   if(name.len == 0){
      return false
   }
   if(_load_scene_runtime(name, true)){
      _model_index = next_idx
      terminal.log("Model: " + _loaded_scene_name + " (" + to_str(next_idx + 1) + "/" + to_str(count) + ")")
      return true
   }
   false
}

fn _gui_open_default_editor() {
   _gui_visible = true
   def _discard_warm = _warm_gui_editor_resources()
   _gui_shell_show(true, false, false, false, true, false, false, false)
   _gui_workspace_mode = 1
   _gui_center_tab = 0
   _gui_side_tab = 0
   _gui_sanitize_workspace_state()
   if(is_dict(active_scene) && show_scene){
      _set_active_scene_model_matrix()
      _scene_select_active_loaded("editor open")
   }
   ui_shell.hide_tools(["profiler", "node_graph", "workspace_grid", "asset_browser", "widget_probe", "widget_gallery"])
   ui_editor.show_tool("editor_main", true)
   ui_editor.show_tool("inspector", true)
   gui.focus_window("editor_main")
   _gui_layout_dirty = true
   _gui_layout_warm_frames = 4
   _invalidate_chrome_frame(4)
}

fn _gui_dump_path() {
   def shot = _gui_shot_env()
   if(shot.len > 0){
      return ui_dump.path_named("ui_gui_" + shot + ".png", _cli_dump_dir)
   }
   ui_dump.path_named("ui_gui_" + ui_dump.safe_name(_gui_layout_preset_env()) + "_" + to_str(int(ticks())) + ".png", _cli_dump_dir)
}

fn _snapshot_ok(path) {
   def out_path = to_str(path)
   def dir = ospath.dirname(out_path)
   if(dir.len > 0 && dir != "." && !osfs.is_dir(dir)){
      def res = run_capture(["mkdir", "-p", dir], [], nil, false)
      if(!bool(res.get("ok", false)) && !osfs.is_dir(dir)){ return false }
   }
   if(gfx.snapshot(out_path)){ return true }
   osfs.is_file(out_path)
}

fn _gui_take_snapshot() {
   def path = _gui_dump_path()
   _gui_last_dump_path = path
   if(_snapshot_ok(path)){
      terminal.log("GUI dump: " + path)
      return true
   }
   terminal.log("ERROR: GUI dump failed: " + path)
   false
}

fn _hotkey_frame_dump() {
   def path = ui_dump.path_named("frame_dump_" + to_str(int(ticks())) + ".png", _cli_dump_dir)
   _gui_last_dump_path = path
   if(_snapshot_ok(path)){
      ui_profile.print_text("[ui:hotkey] F5 frame_dump path=" + path)
      terminal.log("Frame dump: " + path)
      return true
   }
   ui_profile.print_text("[ui:hotkey] F5 frame_dump failed path=" + path)
   terminal.log("ERROR: frame dump failed: " + path)
   false
}

fn _static_world_redraw(any frames=2) bool {
   _static_world_redraw_frames = max(_static_world_redraw_frames, int(frames))
   true
}

fn _static_world_color_reuse_reset() bool {
   _static_world_color_reuse_frames = 0
   _static_world_color_reuse_w = 0
   _static_world_color_reuse_h = 0
   true
}

fn _scene_edit_redraw(any frames=3) bool {
   _render_set_next_frame_load_color(false)
   _static_world_color_reuse_reset()
   if(is_dict(active_scene)){
      scene_engine.scene_fast_reset(active_scene)
   }
   _static_world_redraw(frames)
   true
}

fn _scene_transform_redraw(any frames=2) bool {
   ;; Transform-only changes should not rebuild/reupload static scene GPU data.
   ;; Dragging the gizmo changes the model matrix, not the mesh/material buffers.
   ;; The old path called scene_fast_reset() on every mouse move, which made F1
   ;; gizmo drags hitch/jitter badly on VK/GL.
   _render_set_next_frame_load_color(false)
   _static_world_color_reuse_reset()
   _static_world_redraw(frames)
   true
}

fn _ui_view_input_active() bool {
   _move_w || _move_a || _move_s || _move_d || _move_space || _move_shift ||
   _move_ctrl || _rmb_look_active || _scene_drag_active || skip_mouse_frames > 0 ||
   ui_app.app_absf(_mouse_dx_acc) > 0.001 || ui_app.app_absf(_mouse_dy_acc) > 0.001 ||
   ui_app.app_absf(_spdx) > 0.001 || ui_app.app_absf(_spdy) > 0.001 || ui_app.app_absf(_spd_z) > 0.001
}

fn _ui_static_world_fast_enabled() bool {
   if(ui_profile.env_present_cached("NY_UI_STATIC_WORLD_FAST")){
      return ui_profile.env_toggle_cached("NY_UI_STATIC_WORLD_FAST", false)
   }
   true
}

fn _scene_static_pose_gpu_ready() bool {
   if(!is_dict(active_scene)){ return false }
   bool(active_scene.get("static_pose_gpu_ready", false)) &&
   !bool(active_scene.get("anim_playing", _anim_enabled)) &&
   to_int(active_scene.get("gpu_parts_slab", 0)) != 0 &&
   int(active_scene.get("gpu_parts_count", 0)) > 0
}

fn _scene_deform_idle_ready() bool {
   if(!is_dict(active_scene)){ return false }
   def has_deform =
   int(active_scene.get("anim_count", 0)) > 0 ||
   int(active_scene.get("skin_count", 0)) > 0 ||
   int(active_scene.get("morph_target_count", 0)) > 0
   has_deform && !_anim_enabled && !bool(active_scene.get("anim_playing", false))
}

fn _scene_deform_blocks_static() bool {
   if(!is_dict(active_scene)){ return false }
   def has_deform =
   int(active_scene.get("anim_count", 0)) > 0 ||
   int(active_scene.get("skin_count", 0)) > 0 ||
   int(active_scene.get("morph_target_count", 0)) > 0
   if(!has_deform){ return false }
   if(_anim_enabled || bool(active_scene.get("anim_playing", false)) || bool(active_scene.get("anim_time_override", false))){
      return true
   }
   !_scene_static_pose_gpu_ready() && !_scene_deform_idle_ready()
}

fn _ui_static_update_clean() bool {
   if(_proj_dirty || _ui_view_input_active()){ return false }
   if(_term_open || _gui_enabled_now() || _scene_load_async_active()){ return false }
   if(_batch_dump_enabled() || _gui_dump_suite_active || _pending_auto_dump){ return false }
   if(_anim_enabled){ return false }
   if(_anim_count > 0 && !_scene_static_pose_gpu_ready() && !_scene_deform_idle_ready()){ return false }
   if(_scene_deform_blocks_static()){ return false }
   true
}

fn _anim_frame_trace_enabled() bool {
   ui_profile.env_truthy_cached("NY_UI_ANIM_FRAME_TRACE") ||
   ui_profile.env_truthy_cached("NY_GLTF_ANIM_TRACE")
}

fn _static_world_present_reuse_allowed(any load_color=false) bool {
   bool(load_color) &&
   _static_world_color_reuse_frames > 0 &&
   _static_world_color_reuse_w == int(_win_w) &&
   _static_world_color_reuse_h == int(_win_h)
}

fn _static_world_color_reuse_enabled() bool {
   if(ui_profile.env_present_cached("NY_UI_STATIC_COLOR_REUSE")){
      return ui_profile.env_toggle_cached("NY_UI_STATIC_COLOR_REUSE", false)
   }
   _batch_dump_enabled() || _gui_dump_suite_active || _proof_dump_active() ||
   _auto_dump_enabled == 1 || ui_profile.ui_bench_enabled()
}

fn _static_world_color_reuse_allowed() bool {
   if(_static_world_redraw_frames > 0){ return false }
   if(!_static_world_color_reuse_enabled()){ return false }
   if(!gfx.backend_capabilities().get("load_color_resume", false)){ return false }
   if(!_ui_static_world_fast_enabled()){ return false }
   if(!_ui_static_update_clean()){ return false }
   if(_static_world_color_reuse_frames <= 0){ return false }
   _static_world_color_reuse_w == int(_win_w) && _static_world_color_reuse_h == int(_win_h)
}

fn _static_world_color_reuse_note(any load_color=false) bool {
   if(_static_world_redraw_frames > 0){ _static_world_redraw_frames -= 1 }
   if(bool(load_color)){
      _static_world_color_reuse_frames += 1
   } else {
      _static_world_color_reuse_frames = 1
      _static_world_color_reuse_w = int(_win_w)
      _static_world_color_reuse_h = int(_win_h)
   }
   true
}

fn _clear_mouse_look_state() bool {
   _mouse_dx_acc = 0.0
   _mouse_dy_acc = 0.0
   _rmb_dx_smooth = 0.0
   _rmb_dy_smooth = 0.0
   _mouse_look_last_event_ns = 0
   _mouse_look_last_frame = -1
   _mouse_look_last_source = ""
   true
}

fn _clear_camera_input_state() bool {
   _move_w = false
   _move_a = false
   _move_s = false
   _move_d = false
   _move_space = false
   _move_shift = false
   _move_ctrl = false
   _spdx = 0.0
   _spdy = 0.0
   _spd_z = 0.0
   _vx = 0.0
   _vy = 0.0
   _vz = 0.0
   _camera_sim_dt_smooth = 0.0
   _clear_mouse_look_state()
   true
}

fn _ui_move_input_active() bool {
   _move_w || _move_a || _move_s || _move_d || _move_space || _move_shift ||
   _move_ctrl || ui_app.app_absf(_vx) > 0.001 || ui_app.app_absf(_vy) > 0.001 ||
   ui_app.app_absf(_vz) > 0.001 || ui_app.app_absf(_spdx) > 0.001 ||
   ui_app.app_absf(_spdy) > 0.001 || ui_app.app_absf(_spd_z) > 0.001
}

fn _scene_loading_input_guard_active() bool {
   _scene_load_async_active() || _scene_load_finalizing || _startup_post_load_pending
}

fn _release_scene_loading_input(str reason="scene loading") bool {
   if(!_scene_loading_input_guard_active()){
      return false
   }
   def _discard_drag = _scene_drag_end()
   _clear_camera_input_state()
   _rmb_look_active = false
   _middle_mouse_active = false
   _middle_mouse_suppress_scroll_until_ns = ticks() + _MIDDLE_SCROLL_SUPPRESS_NS
   skip_mouse_frames = max(skip_mouse_frames, 3)
   _suppress_mouse_deltas(_CURSOR_TRANSITION_SUPPRESS_NS)
   if(win && !_term_open && !ui_profile.headless_enabled()){
      _intended_cursor_mode = CURSOR_NORMAL
      set_cursor_mode(win, CURSOR_NORMAL)
   }
   true
}

fn _invalidate_chrome_frame(frames=4) {
   _render_set_next_frame_load_color(false)
   _static_world_color_reuse_reset()
   _static_world_redraw(frames)
   true
}

fn _hotkey_toggle_editor() {
   def now = ticks()
   if(_hotkey_f1_last_ns > 0 && now - _hotkey_f1_last_ns < 160000000){
      return false
   }
   _hotkey_f1_last_ns = now
   def opening = !_gui_editor_shell_open()
   _gui_layout_dirty = true
   _gui_layout_warm_frames = 2
   _invalidate_chrome_frame(4)
   skip_mouse_frames = 2
   _rmb_look_active = false
   def _discard_drag = _scene_drag_end()
   _clear_camera_input_state()
   if(opening){
      _gui_open_default_editor()
      _gui_visible = true
      focus(win)
      gui.suppress_mouse_clicks(5, true)
      if(!ui_profile.headless_enabled()){
         show_centered_cursor(win)
      }
   } else {
      _gui_close_editor_shell()
      gui.clear_focus()
      _scene_clear_selection("editor close")
   }
   if(ui_profile.trace_enabled() || ui_profile.env_truthy_cached("NY_UI_HOTKEY_TRACE")){
      ui_profile.print_text("[ui:hotkey] F1 editor=" + (opening ? "open" : "closed"))
   }
   _sync_cursor_state("F1 editor toggle")
   true
}

fn _gui_focus_asset_search() {
   _gui_visible = true
   _gui_browser_tab = 0
   _gui_layout_dirty = true
   _invalidate_chrome_frame(4)
   _rmb_look_active = false
   _clear_camera_input_state()
   if(_gui_show_browser && !_gui_show_editor){
      gui.focus_window("asset_browser")
      gui.request_focus("asset_browser::asset_title_filter")
      _sync_cursor_state("gui focus asset search")
      terminal.log("GUI focus: asset browser search")
      return
   }
   _gui_show_editor = true
   _gui_show_browser = false
   _gui_editor_tab = 0
   gui.focus_window("editor_main")
   gui.request_focus("editor_main::editor_asset_model_filter")
   _sync_cursor_state("gui focus asset search")
   terminal.log("GUI focus: editor asset filter")
}

fn _gui_unload_scene() {
   if(is_dict(active_scene)){
      scene_engine.unload_scene(active_scene, _loaded_scene_name)
      active_scene = 0
   }
   _scene_selection_bounds_cache_clear()
   show_scene = false
   _scene_selected = false
   _loaded_scene_name = ""
   _gui_material_selected_idx = -1
   _gui_material_selected_part_idx = -1
   _clear_anim_state()
   _sync_cursor_state("gui unload scene")
}

fn _gui_mark_model_loaded() {
   _gui_model_selected_name = _loaded_scene_name
   _scene_select_active_loaded("gui model loaded")
   terminal.log("Loaded: " + _loaded_scene_name)
   true
}

fn _gui_activate_model_name(name) {
   def n = str.strip(to_str(name))
   if(n.len == 0){
      return false
   }
   _gui_model_selected_name = n
   if(_load_scene_runtime(n, true)){
      return _gui_mark_model_loaded()
   }
   false
}

fn _queue_gui_model_load(name) bool {
   def n = str.strip(to_str(name))
   if(n.len == 0){ return false }
   _gui_pending_model_load_name = n
   _gui_pending_unload_scene = false
   true
}

fn _queue_gui_unload_scene() bool {
   _gui_pending_model_load_name = ""
   _gui_pending_unload_scene = true
   true
}

fn _process_gui_scene_requests() bool {
   if(_scene_load_async_active()){
      if(_gui_pending_model_load_name.len > 0){
         _release_scene_loading_input("scene load request queued")
      }
      return false
   }
   if(_gui_pending_unload_scene){
      _gui_pending_unload_scene = false
      _gui_unload_scene()
      return true
   }
   def n = str.strip(_gui_pending_model_load_name)
   if(n.len <= 0){ return false }
   _gui_pending_model_load_name = ""
   if(_gui_activate_model_name(n)){
      _gui_asset_load_wait_mouse_up = true
      return true
   }
   false
}

fn _batch_dump_parse_env() {
   _batch_dump_index, _batch_dump_wait_frames = 0, 0
   _batch_dump_model_started_ns, _batch_dump_run_started_ns, _batch_dump_load_started_ns = 0, 0, 0
   _batch_dump_completed_count, _batch_dump_active_mode = 0, false
   def requested = _cli_batch_models_raw.len > 0 ||
   _cli_dump_all_requested ||
   ui_profile.env_truthy_cached("NY_UI_BATCH_DUMP_ALL") ||
   ui_profile.env_trim_cached("NY_UI_BATCH_DUMP_FILE").len > 0 ||
   ui_profile.env_trim_cached("NY_UI_BATCH_DUMP_LIST").len > 0
   if(!requested){ return }
   def all_names = (_cli_dump_all_requested || ui_profile.env_truthy_cached("NY_UI_BATCH_DUMP_ALL")) ? _refresh_model_catalog() : []
   def cfg = asset_batch.parse_env(_cli_batch_models_raw,
      _cli_dump_all_requested,
      all_names,
      _cli_dump_dir,
      _cli_dump_delay_frames,
      _cli_dump_missing_requested,
   ui_assets.gltf_asset_catalog())
   _batch_dump_models = cfg.get("models", [])
   _batch_dump_model_specs = cfg.get("specs", [])
   _batch_dump_skip_models = cfg.get("skip_models", [])
   _batch_dump_dir = to_str(cfg.get("dir", ""))
   _batch_dump_settle_frames = int(cfg.get("settle_frames", 4))
   _batch_dump_model_timeout_sec = float(cfg.get("timeout_sec", 45.0))
   if(bool(cfg.get("no_missing", false))){
      ui_profile.print_text("[ui:batch:complete] count=0 reason=no_missing dir=" + _batch_dump_dir)
      __exit(0)
   }
   _batch_dump_active_mode = bool(cfg.get("active", false))
   if(_batch_dump_active_mode){
      _batch_dump_run_started_ns = ticks()
      print(
         "[ui:batch:init] count=" + to_str(_batch_dump_models.len)
         + " settle_frames=" + to_str(_batch_dump_settle_frames)
         + " timeout_sec=" + to_str(_batch_dump_model_timeout_sec)
         + " prefetch=" + to_str(ui_profile.batch_prefetch_enabled())
         + " skip=" + to_str(_batch_dump_skip_models.len)
         + " dir=" + _batch_dump_dir
      )
   }
}

fn _gui_dump_suite_active_now() {
   _gui_dump_suite_active && _gui_dump_suite_index >= 0 && _gui_dump_suite_index < _gui_dump_suite_specs.len
}

fn _gui_dump_suite_parse_env() {
   _gui_dump_suite_specs, _gui_dump_suite_dir, _gui_dump_suite_active = [], "", false
   _gui_dump_suite_index, _gui_dump_suite_wait_frames, _gui_dump_suite_attempts, _gui_dump_suite_settle_frames = 0, 0, 0, 4
   def cfg = ui_dump.suite_parse_env(_cli_dump_dir)
   _gui_dump_suite_specs = cfg.get("specs", [])
   _gui_dump_suite_dir = to_str(cfg.get("dir", ""))
   _gui_dump_suite_settle_frames = int(cfg.get("settle_frames", 4))
   _gui_dump_suite_active = _gui_dump_suite_specs.len > 0
   if(_gui_dump_suite_active){
      _gui_probe_mode = 1
      print(
         "[ui:gui-suite:init] count=" + to_str(_gui_dump_suite_specs.len)
         + " settle_frames=" + to_str(_gui_dump_suite_settle_frames)
         + " dir=" + _gui_dump_suite_dir
      )
   }
   _gui_dump_suite_active
}

fn _gui_dump_suite_apply_current() {
   if(!_gui_dump_suite_active_now()){
      return false
   }
   def spec = _gui_dump_suite_specs.get(_gui_dump_suite_index, [])
   def filename = ui_dump.suite_field(spec, 0, "gui_dump.png")
   def shot = ui_dump.suite_field(spec, 1, "")
   def layout_name = ui_dump.suite_field(spec, 2, "")
   def combo = ui_dump.suite_field(spec, 3, "")
   def kind = ui_dump.suite_field(spec, 4, "gui")
   def gizmo = ui_dump.suite_field(spec, 5, "")
   def model = ui_dump.suite_field(spec, 6, "")
   if(layout_name.len > 0){ _gui_layout_preset_name = layout_name }
   gui.debug_force_combo_open("")
   _selection_overlay_dump_mode, _scene_selection_rect, _scene_selected = 0, false, false
   if(kind == "gizmo"){
      if(model.len > 0 && (!is_dict(active_scene) || _loaded_scene_name != model)){
         if(_load_scene_runtime_sync(model, true, false)){
            _cmd_autofit(false)
            _cmd_lookat(false)
         }
      }
      show_scene = is_dict(active_scene)
      _scene_selection_bounds_cache_update()
      _gui_visible = false
      _gui_shell_show(false, false, false, false, false, false, false, false)
      _gui_shell_tabs(0, 0, 0, 0)
      _gizmo_mode = ui_dump.gizmo_mode(gizmo, _gizmo_mode)
      _selection_overlay_dump_mode = 1
      _scene_selection_rect = true
      _scene_selected = is_dict(active_scene) && show_scene
      _selection_overlay_clear_rects()
      _gui_shot_name = "gizmo_" + ui_selection.selection_gizmo_label(_gizmo_mode)
      _gizmo_trace("suite mode=" + ui_selection.selection_gizmo_label(_gizmo_mode)
         + " scene=" + _loaded_scene_name
         + " active=" + to_str(is_dict(active_scene))
         + " show=" + to_str(show_scene)
      + " selected=" + to_str(_scene_selected))
   } else {
      _gui_visible = true
      _gui_apply_shell_shot(shot)
      if(combo.len > 0){ gui.debug_force_combo_open(combo) }
   }
   _gui_layout_dirty = true
   _gui_layout_warm_frames = max(_gui_layout_warm_frames, _gui_dump_suite_settle_frames)
   _gui_dump_suite_wait_frames, _gui_dump_suite_attempts = _gui_dump_suite_settle_frames, 0
   print(
      "[ui:gui-suite] start idx=" + to_str(_gui_dump_suite_index + 1)
      + "/" + to_str(_gui_dump_suite_specs.len)
      + " file=" + filename
      + " kind=" + kind
      + " shot=" + shot
   )
   true
}

fn _gui_dump_suite_begin() {
   if(!_gui_dump_suite_active){ return false }
   _gui_probe_bootstrapped = true
   _gui_dump_suite_apply_current()
}

fn _gui_dump_suite_finish() {
   ui_profile.print_line("ui:gui-suite:complete", "count=" + to_str(_gui_dump_suite_specs.len))
   _gui_dump_suite_active = false
   gui.debug_force_combo_open("")
   _selection_overlay_dump_mode = 0
   if(ui_dump.suite_exit_enabled()){ set_should_close(win, true) }
}

fn _gui_dump_suite_advance() {
   _gui_dump_suite_index += 1
   if(!_gui_dump_suite_active_now()){
      _gui_dump_suite_finish()
      return false
   }
   _gui_dump_suite_apply_current()
}

fn _gui_dump_suite_after_frame() {
   if(!_gui_dump_suite_active_now()){ return false }
   if(_gui_dump_suite_wait_frames > 0){
      _gui_dump_suite_wait_frames -= 1
      return false
   }
   def path = ui_dump.suite_snapshot_path(_gui_dump_suite_specs, _gui_dump_suite_index, _gui_dump_suite_dir, _cli_dump_dir)
   if(path.len == 0){ _gui_dump_suite_advance() return false }
   _gui_dump_suite_attempts += 1
   if(_gui_dump_suite_attempts <= 2){
      def _discard_7 = print(
         "[ui:gui-suite] snapshot idx=" + to_str(_gui_dump_suite_index + 1)
         + "/" + to_str(_gui_dump_suite_specs.len)
         + " attempt=" + to_str(_gui_dump_suite_attempts)
         + " path=" + path
      )
   }
   if(_snapshot_ok(path)){
      _gui_last_dump_path = path
      ui_profile.print_line("ui:gui-suite", "path=" + path)
      _gui_dump_suite_advance()
      return true
   }
   if(_gui_dump_suite_attempts >= 6){
      ui_profile.print_line("ui:gui-suite:fail", "path=" + path + " attempts=" + to_str(_gui_dump_suite_attempts))
      _gui_dump_suite_advance()
   }
   false
}

fn _batch_dump_advance_or_exit() {
   _batch_dump_index += 1
   def models_n = _batch_dump_models.len
   while(_batch_dump_index < models_n){
      if(_batch_dump_begin_current()){
         return
      }
      _batch_dump_index += 1
   }
   ui_profile.print_line(
      "ui:batch:complete",
      "count=" + to_str(_batch_dump_models.len)
      + " elapsed_s=" + str.to_fixed(asset_batch.elapsed_s(_batch_dump_run_started_ns), 2)
   )
   __exit(0)
}

fn _batch_dump_begin_current() {
   if(!_batch_dump_enabled()){ return false }
   if(_batch_dump_index < 0 || _batch_dump_index >= _batch_dump_models.len){ return false }
   def spec = to_str(_batch_dump_model_specs.get(_batch_dump_index, ""))
   def display_name = to_str(_batch_dump_models.get(_batch_dump_index, ""))
   if(spec.len == 0){ return false }
   _stop_model_prefetch()
   if(is_dict(active_scene)){
      scene_engine.unload_scene(active_scene, _loaded_scene_name)
      active_scene = 0
   }
   show_scene = false
   _loaded_scene_name = ""
   _clear_anim_state()
   _batch_dump_load_started_ns = ticks()
   _batch_dump_model_started_ns = _batch_dump_load_started_ns
   print(
      "[ui:batch] loading idx=" + to_str(_batch_dump_index + 1)
      + "/" + to_str(_batch_dump_models.len)
      + " model=" + display_name
      + " elapsed_s=" + str.to_fixed(asset_batch.elapsed_s(_batch_dump_run_started_ns), 2)
   )
   if(ui_dump.model_skipped(display_name, _batch_dump_skip_models) || ui_dump.model_skipped(spec, _batch_dump_skip_models)){
      _batch_dump_completed_count += 1
      print(
         "[ui:batch] skip idx=" + to_str(_batch_dump_index + 1)
         + "/" + to_str(_batch_dump_models.len)
         + " model=" + display_name
         + " reason=skip_list"
         + " elapsed_s=" + str.to_fixed(asset_batch.elapsed_s(_batch_dump_run_started_ns), 2)
         + " eta_s=" + str.to_fixed(asset_batch.eta_s(_batch_dump_run_started_ns, _batch_dump_completed_count, _batch_dump_models.len), 1)
      )
      return false
   }
   def load_scene_t0 = ticks()
   if(ospath.has_sep(spec)){
      active_scene = scene_engine.load_scene_path(spec, display_name, camthreed, M_SP, M_PT, M_PS)
   } else {
      active_scene = ui_assets.load_named_scene(spec, camthreed, M_SP, M_PT, M_PS)
   }
   ui_profile.stage_log("ui:batch:prof", display_name, "scene_load", load_scene_t0)
   if(!is_dict(active_scene)){
      _batch_dump_terminal_log("batch load failed: " + spec)
      ui_profile.print_line("ui:batch:fail", "load failed model=" + spec)
      return false
   }
   _loaded_scene_name = display_name
   if(_loaded_scene_name.len == 0){
      _loaded_scene_name = spec
   }
   _invalidate_chrome_frame(max(4, _batch_dump_settle_frames + 2))
   mut step_t0 = ticks()
   _apply_batch_scene_clear_color(_loaded_scene_name)
   ui_profile.stage_log("ui:batch:prof", _loaded_scene_name, "clear_color", step_t0)
   show_scene = true
   step_t0 = ticks()
   _sync_cam_state_from_camthreed()
   ui_profile.stage_log("ui:batch:prof", _loaded_scene_name, "sync_camera", step_t0)
   step_t0 = ticks()
   _sync_anim_state_from_scene(active_scene)
   ui_profile.stage_log("ui:batch:prof", _loaded_scene_name, "sync_anim", step_t0)
   step_t0 = ticks()
   _prime_dump_anim_pose()
   ui_profile.stage_log("ui:batch:prof", _loaded_scene_name, "prime_pose", step_t0)
   step_t0 = ticks()
   _cmd_autofit(false)
   ui_profile.stage_log("ui:batch:prof", _loaded_scene_name, "autofit", step_t0)
   step_t0 = ticks()
   _freeze_dump_deform_pose()
   ui_profile.stage_log("ui:batch:prof", _loaded_scene_name, "freeze_pose", step_t0)
   step_t0 = ticks()
   _cmd_lookat(false)
   ui_profile.stage_log("ui:batch:prof", _loaded_scene_name, "lookat", step_t0)
   _batch_dump_wait_frames = _batch_dump_settle_frames
   _batch_dump_wait_frames = asset_batch.fast_static_settle_frames(_batch_dump_wait_frames)
   _batch_dump_model_started_ns = ticks()
   _schedule_batch_prefetch_next()
   _batch_dump_terminal_log("batch load: " + _loaded_scene_name)
   print(
      "[ui:batch] load idx=" + to_str(_batch_dump_index + 1)
      + "/" + to_str(_batch_dump_models.len)
      + " model=" + _loaded_scene_name
      + " settle=" + to_str(_batch_dump_wait_frames)
      + " load_ms=" + str.to_fixed(ui_profile.stage_ms_since(_batch_dump_load_started_ns), 2)
      + " path=" + ui_dump.snapshot_path(_loaded_scene_name, _batch_dump_dir, _cli_dump_dir)
   )
   true
}

fn maybe_run(any app=0) bool {
   "Return true when batch dump orchestration should own the UI run."
   if(_batch_dump_enabled()){
      if(_batch_dump_models.len == 0){ _batch_dump_parse_env() }
      return _batch_dump_enabled()
   }
   false
}

fn _startup_trace_enabled() bool {
   ui_profile.env_truthy_cached("NY_UI_STARTUP_TRACE")
}

fn _startup_trace(stage, t0) {
   if(!_startup_trace_enabled()){
      return 0
   }
   def ms = ui_profile.elapsed_ms(t0)
   ui_profile.print_text("[startup] " + stage + "=" + to_str(ms) + "ms")
   ms
}

fn _log_projection() {
   terminal.log(is_ortho ? "Projection: ORTHOGRAPHIC" : "Projection: PERSPECTIVE")
}

fn _set_projection_mode(next_ortho) {
   is_ortho = next_ortho
   _proj_dirty = true
   _log_projection()
}

fn _focused_scene_look_active() bool {
   if(ui_profile.headless_enabled()){
      return false
   }
   if(_scene_loading_input_guard_active()){
      return false
   }
   if(_auto_dump_enabled == 1 || _batch_dump_active_mode || _gui_dump_suite_active || _proof_dump_active()){
      return false
   }
   if(_term_open || _gui_visible || _gui_enabled_now()){
      return false
   }
   if(APP_WIRE || !_ui_scene_visible()){
      return false
   }
   if(_scene_drag_active){
      return false
   }
   if(ui_profile.env_truthy_cached("NY_UI_DISABLE_FOCUS_LOOK")){
      return false
   }
   def live_win = get_win(win)
   if(is_dict(live_win) && live_win.contains("focused") && !bool(live_win.get("focused", true))){
      return false
   }
   true
}

fn _camera_keyboard_nav_allowed(bool gui_active=false, bool gui_keys_blocking=false) bool {
   if(_scene_loading_input_guard_active()){
      return false
   }
   if(_term_open || gui_keys_blocking){
      return false
   }
   if(_rmb_look_active && _ui_scene_visible()){
      return true
   }
   if(_cursor_lock_enabled && !gui_active && !_gui_enabled_now() && _ui_scene_visible()){
      return true
   }
   if(_focused_scene_look_active()){
      return true
   }
   !gui_active && !_gui_enabled_now()
}

fn _desired_cursor_mode() {
   if(ui_profile.headless_enabled()){
      return CURSOR_NORMAL
   }
   if(_scene_loading_input_guard_active()){
      return CURSOR_NORMAL
   }
   if(_auto_dump_enabled == 1 || _batch_dump_active_mode || _gui_dump_suite_active){
      return CURSOR_NORMAL
   }
   if(_term_open){
      return CURSOR_NORMAL
   }
   ;; RMB/look ownership must win over visible GUI panels.  The previous order
   ;; returned CURSOR_NORMAL whenever the editor UI was visible, so camera look
   ;; used unstable window-position deltas instead of captured/raw deltas.
   if(_scene_drag_active){
      return CURSOR_NORMAL
   }
   if(_rmb_look_active){
      return CURSOR_DISABLED
   }
   if(_gui_enabled_now() || _gui_visible){
      return CURSOR_NORMAL
   }
   if(_cursor_lock_enabled){
      return CURSOR_DISABLED
   }
   if(_scene_selected){
      return CURSOR_NORMAL
   }
   if(_focused_scene_look_active()){
      return CURSOR_DISABLED
   }
   CURSOR_NORMAL
}

fn _sync_cursor_state(reason) {
   if(!win){
      return
   }
   def mode = _desired_cursor_mode()
   def prev_mode = _intended_cursor_mode
   _intended_cursor_mode = mode
   if(ui_profile.headless_enabled()){
      return
   }
   elif(mode == CURSOR_NORMAL && prev_mode == CURSOR_DISABLED){
      set_cursor_mode(win, CURSOR_NORMAL)
   } else {
      set_cursor_mode(win, mode)
   }
   if(_startup_trace_enabled()){
      def pos = cursor_pos(win)
      ui_runtime.dbg("ui",
         reason + " - _term_open=" + to_str(_term_open) +
         " mode=" + to_str(mode) +
         " pos=(" + to_str(int(pos.get(0, 0.0))) +
         "," + to_str(int(pos.get(1, 0.0))) +
      ")")
   }
}

fn _set_timeout() {
   mut env_t = ui_profile.env_trim_cached("NY_UI_TIMEOUT")
   if(env_t.len == 0){
      env_t = ui_profile.env_trim_cached("NY_TIMEOUT")
   }
   if(_cli_timeout_sec > 0.0){
      _timeout_ns = int(_cli_timeout_sec * 1e9)
   }
   elif(env_t.len > 0){
      _timeout_ns = int(str.atof(env_t) * 1e9)
   }
   elif(ui_profile.env_truthy_cached("CI") || ui_profile.env_truthy_cached("NYTRIX_TEST_MODE")){
      _timeout_ns = int(5.0 * 1e9)
   }
   else { _timeout_ns = 0 }
}

fn _init_fonts() {
   def t0 = (_ui_debug_enabled == 1) ? ticks() : 0
   def editor_paths = ui_assets.EDITOR_FONT_CANDIDATES
   _gui_dpi_scale = _ui_auto_dpi_scale()
   if(!ui_profile.gui_scale_env_present()){
      _gui_scale = ui_app.app_gui_scale_for_window(_win_w, _win_h, _gui_dpi_scale)
   }
   def base_font_sz = ui_runtime.default_font_size("ui", 13.0 * _gui_dpi_scale)
   def ui_font_sz = ui_runtime.default_font_size("ui", 13.0 * _gui_dpi_scale, "UI_FONT_SIZE")
   def title_font_sz = ui_runtime.default_font_size("ui", 15.0 * _gui_dpi_scale, "TITLE_FONT_SIZE")
   def small_font_sz = ui_runtime.default_font_size("ui", 13.0 * _gui_dpi_scale, "SMALL_FONT_SIZE")
   def font_filter = ui_runtime.default_font_filter("ui", FONT_FILTER_LINEAR)
   def ui_font_filter = ui_runtime.default_font_filter("ui", FONT_FILTER_LINEAR, "UI_FONT_FILTER")
   def title_font_filter = ui_runtime.default_font_filter("ui", FONT_FILTER_LINEAR, "TITLE_FONT_FILTER")
   if(_ui_debug_enabled == 1){
      _dbg_ui("[ui] fonts: begin base_sz=" + to_str(base_font_sz) + " ui_sz=" + to_str(ui_font_sz) +
         " title_sz=" + to_str(title_font_sz) + " small_sz=" + to_str(small_font_sz) +
         " base_filter=" + to_str(font_filter) +
         " ui_filter=" + to_str(ui_font_filter) +
      " title_filter=" + to_str(title_font_filter))
   }
   res_font = ui_assets.editor_font(base_font_sz, editor_paths, font_filter)
   res_font_ui = ui_assets.editor_font(ui_font_sz, editor_paths, ui_font_filter)
   res_font_title = ui_assets.editor_font(title_font_sz, editor_paths, title_font_filter)
   res_font_small = ui_assets.editor_font(small_font_sz, editor_paths, ui_font_filter)
   if(!res_font){
      ui_runtime.dbg("ui", "font fallback active")
   }
   if(!res_font_ui){
      res_font_ui = ui_assets.editor_font(ui_font_sz, editor_paths, ui_font_filter)
   }
   if(!res_font_title){
      res_font_title = _ui_font()
   }
   if(!res_font_small){
      res_font_small = _ui_font()
   }
   if(_ui_debug_enabled == 1){
      def ms = ui_profile.elapsed_ms(t0)
      _dbg_ui("[ui] fonts: done base_id=" + to_str(res_font) + " ui_id=" + to_str(res_font_ui) +
      " title_id=" + to_str(res_font_title) + " small_id=" + to_str(res_font_small) + " ms=" + to_str(ms))
   }
}

fn _warm_mesh_helpers() {
   def buf = malloc(VERTEX_STRIDE)
   if(!buf){
      return 0
   }
   push_vertex(buf, 0.0, 0.0, 0.0, 0.0, 0.0, WHITE)
   free(buf)
   gltf.gltf_warm_runtime()
   0
}

fn _startup_exec_line(any line, str source="cmd") {
   def cmd_line = str.strip(to_str(line))
   if(cmd_line.len == 0){
      return 0
   }
   _dbg_ui("[ui] startup " + source + ": " + cmd_line)
   if(source == "post"){
      def post_cmd = to_str(str.lower(cmd_line))
      if(post_cmd == "autofit" || post_cmd == "fit"){
         _cmd_autofit(false) return 1
      }
      if(post_cmd == "lookat" || post_cmd == "focus" || post_cmd == "frame"){
         _cmd_lookat(false) return 1
      }
   }
   def cmd_parts = str.split_words(cmd_line)
   if(cmd_parts.len > 1 && eq(str.lower(to_str(cmd_parts.get(0, ""))), "load")){
      _loaded_scene_name = str.strip(str.join_words(cmd_parts, " ", 1))
      show_scene = _loaded_scene_name.len > 0
      if(source == "cmd"){
         _cli_scene_requested = true
         _cli_scripted_scene_load = true
      }
      _dbg_ui("[ui] startup state: scene=" + to_str(is_dict(active_scene)) + " show_scene=" + to_str(show_scene))
      return 1
   }
   exec_cmd(cmd_line)
   _dbg_ui("[ui] startup state: scene=" + to_str(is_dict(active_scene)) + " show_scene=" + to_str(show_scene))
   1
}

fn _startup_exec_semicolon_list(line, source="cmd") {
   def line0 = str.strip(to_str(line))
   if(line0.len == 0){
      return 0
   }
   if(str.find(line0, ";") < 0){
      return _startup_exec_line(line0, source)
   }
   def parts = str.split(line0, ";")
   def parts_len = parts.len
   mut j = 0
   while(j < parts_len){
      _startup_exec_line(parts.get(j, ""), source)
      j += 1
   }
   0
}

fn _ui_print_frame_hash(dump_path="") {
   def line = ui_dump.framebuffer_hash_line()
   if(line.len > 0){
      print(line)
      if(dump_path.len > 0){
         ui_profile.print_line("ui:hash", "path=" + dump_path + " " + line)
      }
      return true
   }
   ui_profile.print_line("ui:hash:fail", "framebuffer hash unavailable" + ((dump_path.len > 0) ? (" path=" + dump_path) : ""))
   false
}

fn _startup_exec_cli_cmds() {
   def plan = viewer_cli.startup_plan(viewer_cli.argv_list(), STARTUP_ONE_ARG_CMDS)
   def actions = plan.get("actions", [])
   mut i = 0
   while(is_list(actions) && i < actions.len){
      def a = actions.get(i, {})
      def kind = to_str(a.get("kind", ""))
      def value = to_str(a.get("value", ""))
      if(kind == "scene"){
         _loaded_scene_name = str.strip(value)
         show_scene = _loaded_scene_name.len > 0
         if(show_scene){ _cli_scripted_scene_load = true }
         _dbg_ui("[ui] startup state: scene=" + to_str(is_dict(active_scene)) + " show_scene=" + to_str(show_scene))
      } elif(kind == "cmds"){
         _startup_exec_semicolon_list(value, "cmd")
      } elif(kind == "cmd"){
         _startup_exec_line(value, "cmd")
      }
      i += 1
   }
   0
}

fn _startup_exec_env_var(name, origin) {
   def line0 = ui_profile.env_trim_cached(to_str(name))
   if(line0.len == 0){
      return 0
   }
   _startup_exec_semicolon_list(line0, origin)
   0
}

fn _startup_exec_env_cmds() {
   _startup_exec_env_var("NY_UI_CMD", "env")
}

fn _apply_startup_render_env() bool {
   gfx.apply_backend_env()
   if(_cli_render_backend.len > 0){ gfx.apply_backend_name(_cli_render_backend) }
   def cfg = ui_app.app_startup_render_config(APP_MSAA, APP_VSYNC, APP_FILTER_LINEAR)
   APP_MSAA = int(cfg.get("msaa", APP_MSAA))
   APP_VSYNC = bool(cfg.get("vsync", APP_VSYNC))
   APP_FILTER_LINEAR = bool(cfg.get("filter_linear", APP_FILTER_LINEAR))
   true
}

fn _reset_skybox_resources() bool {
   load_skybox(false, "off")
   true
}

fn _load_skybox(any visible=true, any source="") bool {
   load_skybox(bool(visible), to_str(source))
}

fn _load_fast_generated_skybox(any visible=true) bool {
   load_skybox(bool(visible), "generated")
}

fn _ensure_visible_skybox_ready() bool {
   if(skybox_tex_id >= 0){ return true }
   _load_fast_generated_skybox(true)
}

fn _setup_window() {
   _dbg_ui("[ui] startup: opening window")
   _apply_startup_render_env()
   ui_bootstrap.apply_identity_hints()
   if(ui_profile.headless_sim_enabled() && ui_bootstrap.headless_sim_refused()){
      ui_profile.print_line("ui:headless-sim:fail", "refused surfaced backend; expected no-surface backend")
      exit(2)
   }
   def want_fullscreen = ui_profile.env_truthy_cached("NY_UI_FULLSCREEN")
   def opened = ui_bootstrap.open_viewer_window("Nytrix", APP_MSAA, APP_VSYNC, APP_FILTER_LINEAR, want_fullscreen, ui_profile.headless_enabled())
   win = opened.get("win", 0)
   _win_w, _win_h = float(opened.get("w", _win_w)), float(opened.get("h", _win_h))
   if(!win){
      def fail_msg = ui_bootstrap.failure_summary(get_active_backend_name(), ui_profile.headless_enabled())
      ui_profile.print_line("ui:window:fail", fail_msg)
      eprint("[ui] failed to create GPU window: " + fail_msg)
      exit(1)
   }
   _dbg_ui("[ui] startup: window opened")
   active_backend_name = get_active_backend_name()
   set_clear_color(APP_BG)
   if(!gfx.backend_capabilities().get("double_buffered", false)){
      def fail_msg = ui_bootstrap.failure_summary(active_backend_name, ui_profile.headless_enabled())
      eprint("[ui] GPU renderer is required: " + fail_msg)
      exit(1)
   }
   def live_size = ui_bootstrap.finish_window(win, want_fullscreen, ui_profile.headless_enabled())
   _win_w, _win_h = float(live_size.get(0, _win_w)), float(live_size.get(1, _win_h))
   set_win_size(int(_win_w), int(_win_h))
   set_clear_color(APP_BG)
}

fn _setup_camera() {
   cam = camera_init([0.0, 0.0, 0.0], 0.0, 0.0)
   def start_pos = [22.0, 14.0, 26.0]
   def start_target = [0.0, 4.0, 0.0]
   def f64: fx = float(start_target.get(0, 0.0)) - float(start_pos.get(0, 0.0))
   def f64: fy = float(start_target.get(1, 0.0)) - float(start_pos.get(1, 0.0))
   def f64: fz = float(start_target.get(2, 0.0)) - float(start_pos.get(2, 0.0))
   def f64: fh = sqrt(fx * fx + fz * fz)
   def f64: start_yaw = atan2(fx, -fz) * 180.0 / PI
   def f64: start_pitch = atan2(fy, max(0.000001, fh)) * 180.0 / PI
   camthreed = camera.init(start_pos, start_yaw, start_pitch)
   mut start_fov = 120.0
   def dump_fov_env = ui_profile.env_trim_cached("NY_UI_DUMP_FOV")
   if(dump_fov_env.len > 0){
      def raw = str.atof(dump_fov_env)
      if(raw >= 15.0 && raw <= 120.0){
         start_fov = raw
      }
   }
   camera.set_fov(camthreed, start_fov)
   camera.set_smoothing(camthreed, 25.0, 10.0)
   _cam_px, _cam_py, _cam_pz = camthreed.get(0), camthreed.get(1), camthreed.get(2)
   _vx, _vy, _vz = camthreed.get(3), camthreed.get(4), camthreed.get(5)
   _h_yaw, _h_pch = camthreed.get(6), camthreed.get(7)
   _target_yaw, _target_pch = camthreed.get(8), camthreed.get(9)
   _spdx, _spdy, _spd_z = camthreed.get(17), camthreed.get(18), camthreed.get(19)
   _cam_fov, _sens = float(camthreed.get(16)), float(camthreed.get(10)) * 3.5
   _spd, _drag = float(camthreed.get(11)), float(camthreed.get(12))
   _damp, _spdmul = float(camthreed.get(14)), float(camthreed.get(15))
   _p_min, _p_max = float(camthreed.get(22, -89.9)), float(camthreed.get(23, 89.9))
   _cam_px_cache, _cam_py_cache, _cam_pz_cache = _cam_px, _cam_py, _cam_pz
}

fn _sync_cam_state_from_camthreed() {
   _cam_px, _cam_py, _cam_pz = camthreed.get(0, _cam_px), camthreed.get(1, _cam_py), camthreed.get(2, _cam_pz)
   _vx, _vy, _vz = camthreed.get(3, _vx), camthreed.get(4, _vy), camthreed.get(5, _vz)
   _h_yaw, _h_pch = camthreed.get(6, _h_yaw), camthreed.get(7, _h_pch)
   _target_yaw, _target_pch = camthreed.get(8, _target_yaw), camthreed.get(9, _target_pch)
   _spdx, _spdy, _spd_z = camthreed.get(17, _spdx), camthreed.get(18, _spdy), camthreed.get(19, _spd_z)
   _cam_fov = camthreed.get(16, _cam_fov)
   _cam_px_cache, _cam_py_cache, _cam_pz_cache = _cam_px, _cam_py, _cam_pz
   _proj_dirty = true
}

fn _active_scene_valid() {
   show_scene && is_dict(active_scene)
}

fn _scene_editor_tools_enabled() bool {
   _gui_editor_shell_open() || _selection_overlay_dump_mode == 1 || _gui_probe_mode_enabled()
}

fn _event_mouse_xy_view(any data) list {
   "Returns mouse event coordinates in the active renderer framebuffer/view space."
   def scaled = uin.scale_event_xy(win, data, _win_w, _win_h)
   def out = uin.event_mouse_xy(win, scaled)
   def _discard_evxy_trace = _app_input_trace(
      "event_mouse_xy_view raw=(" + to_str(is_dict(data) ? data.get("x", 0.0) : 0.0) + "," + to_str(is_dict(data) ? data.get("y", 0.0) : 0.0) + ")" +
      " scaled=(" + to_str(scaled.get("x", 0.0)) + "," + to_str(scaled.get("y", 0.0)) + ")" +
      " out=(" + to_str(out.get(0, 0.0)) + "," + to_str(out.get(1, 0.0)) + ")" +
      " win=(" + to_str(_win_w) + "," + to_str(_win_h) + ")"
   )
   out
}

fn _cursor_xy_view() list {
   "Returns live cursor coordinates in the active renderer framebuffer/view space."
   uin.mouse_view_pos(win, _win_w, _win_h)
}

fn _scene_clear_selection(str reason="scene selection clear") bool {
   if(_scene_drag_active){
      _scene_drag_state["active"] = false
      _scene_drag_active = false
      _scene_drag_mode = _gizmo_mode
   }
   _scene_selected = false
   _scene_selection_rect = false
   _selection_overlay_clear_rects()
   _scene_selection_bounds_cache_clear()
   _sync_cursor_state(reason)
   true
}

fn _cam_slot_num(int idx, f64 fallback=0.0) f64 {
   camthreed ? float(camthreed.get(idx, fallback)) : float(fallback)
}

fn _active_scene_model_matrix() {
   if(!is_dict(active_scene) || !is_dict(active_scene)){
      return M_SP
   }
   scene_engine.scene_active_model_matrix_into(active_scene, M_SP, M_ID, M_W, M_PT, M_PR, M_PS, M_PW, M_Ptmp)
}

fn _set_active_scene_model_matrix() {
   gfx.set_model_matrix(_active_scene_model_matrix())
}

fn _scene_drag_begin(x, y, any pick=0) bool {
   if(!_scene_editor_tools_enabled() || !is_dict(active_scene) || !show_scene || !is_dict(active_scene)){
      return false
   }
   if(is_dict(pick)){
      _gizmo_mode = int(clamp(float(pick.get("mode", _gizmo_mode)), 0.0, 2.0))
      _gizmo_axis = int(clamp(float(pick.get("axis", _gizmo_axis)), 0.0, 3.0))
   }
   _scene_selected = true
   _scene_selection_rect = true
   def drag_bounds = _scene_selection_bounds()
   _scene_drag_state = scene_engine.scene_drag_begin_state(active_scene, x, y, _gizmo_mode, {
         "axis": _gizmo_axis,
         "precise": _gizmo_precise || _move_shift || key_down(win, uin.KEY_LEFT_SHIFT) || key_down(win, uin.KEY_RIGHT_SHIFT) || key_down(win, uin.KEY_SHIFT),
         "snap": _gizmo_snap || key_down(win, uin.KEY_LEFT_CONTROL) || key_down(win, uin.KEY_RIGHT_CONTROL) || key_down(win, uin.KEY_CTRL),
         "screen_axis_x": is_dict(pick) ? float(pick.get("screen_axis_x", 0.0)) : 0.0,
         "screen_axis_y": is_dict(pick) ? float(pick.get("screen_axis_y", 0.0)) : 0.0,
         "axis_world_per_pixel": is_dict(pick) ? float(pick.get("screen_world_per_pixel", 0.0)) : 0.0,
         "axis_ray_ok": false,
         "axis_coord_start": 0.0,
         "axis_world_delta_ok": false,
         "axis_world_delta": 0.0
   })
   _scene_drag_state["bounds"] = drag_bounds
   _scene_drag_active = bool(_scene_drag_state.get("active", false))
   _scene_drag_mode = int(_scene_drag_state.get("mode", _gizmo_mode))
   def _discard_drag_begin_trace = _app_input_trace(
      "drag begin x=" + to_str(x) + " y=" + to_str(y) +
      " mode=" + to_str(_scene_drag_mode) +
      " axis=" + to_str(_gizmo_axis) +
      " pick_hit=" + to_str(is_dict(pick) ? bool(pick.get("hit", false)) : false) +
      " screen_axis=(" + to_str(_scene_drag_state.get("screen_axis_x", 0.0)) + "," + to_str(_scene_drag_state.get("screen_axis_y", 0.0)) + ")" +
      " axis_wpp=" + to_str(_scene_drag_state.get("axis_world_per_pixel", 0.0)) +
      " axis_ray_ok=" + to_str(_scene_drag_state.get("axis_ray_ok", false)) +
      " axis_coord_start=" + to_str(_scene_drag_state.get("axis_coord_start", 0.0)) +
      " active=" + to_str(_scene_drag_active)
   )
   _selection_overlay_clear_rects()
   _rmb_look_active = false
   _clear_mouse_look_state()
   _scene_drag_active
}

fn _scene_drag_update(x, y) bool {
   if(!_scene_drag_active || !_scene_editor_tools_enabled() || !is_dict(active_scene) || !show_scene){
      if(_scene_drag_active){ _scene_drag_end() }
      return false
   }
   ;; Preserve the axis that was picked at mouse-down.  Re-reading the global
   ;; gizmo axis every motion event can turn a locked Y-axis drag back into a
   ;; free drag if hover/UI state changes mid-frame.
   if(int(_scene_drag_state.get("axis", 0)) <= 0 && _gizmo_axis > 0){ _scene_drag_state["axis"] = _gizmo_axis }
   def drag_axis = int(_scene_drag_state.get("axis", 0))
   ;; Keep translate drags continuous in screen space. The ray/axis closest-point
   ;; solve can become ill-conditioned for shallow projected axes and then jumps
   ;; between coarse world sections even though mouse coordinates are floats.
   _scene_drag_state["axis_world_delta_ok"] = false
   _scene_drag_state["precise"] = _gizmo_precise || _move_shift || key_down(win, uin.KEY_LEFT_SHIFT) || key_down(win, uin.KEY_RIGHT_SHIFT) || key_down(win, uin.KEY_SHIFT)
   _scene_drag_state["snap"] = _gizmo_snap || key_down(win, uin.KEY_LEFT_CONTROL) || key_down(win, uin.KEY_RIGHT_CONTROL) || key_down(win, uin.KEY_CTRL)
   _scene_drag_state = scene_engine.scene_drag_apply(active_scene, _scene_drag_state, x, y, _h_yaw, _scene_drag_state.get("bounds", _scene_selection_bounds()), _cam_px, _cam_py, _cam_pz, _cam_fov, _h_pch)
   def _discard_drag_update_trace = _app_input_trace(
      "drag update x=" + to_str(x) + " y=" + to_str(y) +
      " axis=" + to_str(drag_axis) +
      " mode=" + to_str(_scene_drag_state.get("mode", 0)) +
      " ray_ok=" + to_str(_scene_drag_state.get("axis_ray_ok", false)) +
      " world_ok=" + to_str(_scene_drag_state.get("axis_world_delta_ok", false)) +
      " world_delta=" + to_str(_scene_drag_state.get("axis_world_delta", 0.0)) +
      " ok=" + to_str(_scene_drag_state.get("ok", false)) +
      " changed=" + to_str(_scene_drag_state.get("changed", false)) +
      " edit_t=(" + to_str(active_scene.get("edit_tx", 0.0)) + "," + to_str(active_scene.get("edit_ty", 0.0)) + "," + to_str(active_scene.get("edit_tz", 0.0)) + ")"
   )
   if(!bool(_scene_drag_state.get("ok", false))){ return false }
   if(bool(_scene_drag_state.get("changed", false))){
      _scene_selection_bounds_cache_clear()
      _scene_transform_redraw(2)
   }
   true
}

fn _scene_drag_end() bool {
   if(!_scene_drag_active){
      return false
   }
   _scene_drag_state["active"] = false
   _scene_drag_active = false
   _scene_drag_mode = _gizmo_mode
   _clear_mouse_look_state()
   true
}

fn _apply_scene_fit_transform(mesh) {
   scene_engine.scene_fit_transform_into(mesh, M_SP, M_PT, M_PS)
}

fn _trace_fit_camera_apply(any mesh, f64 fit_cam_x, f64 fit_cam_y, f64 fit_cam_z, f64 sane_cam_x, f64 sane_cam_y, f64 sane_cam_z,
   f64 sane_target_x, f64 sane_target_y, f64 sane_target_z, f64 min_x, f64 min_y, f64 min_z, f64 max_x, f64 max_y, f64 max_z,
   bool force_dump_pose, str fit_branch, f64 fit_cam_yaw, f64 fit_cam_pitch, f64 applied_fov) bool {
   if(ui_profile.env_truthy_cached("NY_GLTF_MODEL_DEBUG") || ui_profile.env_truthy_cached("NY_UI_FITCAM_TRACE")){
      def _discard_trace_log = terminal.log("[ui] fitcam apply: name=" + _loaded_scene_name +
         " " + ui_app.app_vec3_text("source_cam", fit_cam_x, fit_cam_y, fit_cam_z) +
         " " + ui_app.app_vec3_text("applied_cam", sane_cam_x, sane_cam_y, sane_cam_z) +
         " " + ui_app.app_vec3_text("target", sane_target_x, sane_target_y, sane_target_z) +
         " " + ui_app.app_vec3_text("bounds_min", min_x, min_y, min_z) +
         " " + ui_app.app_vec3_text("bounds_max", max_x, max_y, max_z) +
         " fit_applied=" + to_str(scene_engine.scene_mesh_bool(mesh, "fit_applied", false)) +
         " gpu_baked=" + to_str(scene_engine.scene_mesh_bool(mesh, "gpu_model_baked", false)) +
         " dump_pose=" + to_str(ui_profile.dump_pose_enabled(_auto_dump_enabled, _cli_dump_requested, _batch_dump_enabled(), _gui_dump_suite_active, _gui_probe_mode_enabled())) +
         " auto_dump=" + to_str(_auto_dump_enabled) +
         " auto_env=" + to_str(ui_profile.env_truthy_cached("NYTRIX_AUTO_DUMP")) +
         " gui_probe=" + to_str(_gui_probe_mode_enabled()) +
         " force_dump=" + to_str(force_dump_pose) +
         " branch=" + fit_branch +
      " yaw=" + to_str(fit_cam_yaw) + " pitch=" + to_str(fit_cam_pitch) + " fov=" + to_str(applied_fov))
   }
   true
}

fn _commit_fit_camera_state(sane_cam_x, sane_cam_y, sane_cam_z, fit_cam_yaw, fit_cam_pitch, applied_fov) {
   camthreed[0] = sane_cam_x
   camthreed[1] = sane_cam_y
   camthreed[2] = sane_cam_z
   camthreed[3] = 0.0
   camthreed[4] = 0.0
   camthreed[5] = 0.0
   camthreed[6] = fit_cam_yaw
   camthreed[7] = fit_cam_pitch
   camthreed[8] = fit_cam_yaw
   camthreed[9] = fit_cam_pitch
   if(applied_fov >= 15.0 && applied_fov <= 120.0){
      camthreed[16] = applied_fov
      camera.set_fov(camthreed, applied_fov)
   }
   camera.reset_motion(camthreed)
   _sync_cam_state_from_camthreed()
   _proj_dirty = true
   true
}

fn _apply_scene_fit_camera(mesh) {
   if(!is_dict(mesh) || !camthreed){
      return false
   }
   def dump_pose = ui_profile.dump_pose_enabled(_auto_dump_enabled, _cli_dump_requested, _batch_dump_enabled(), _gui_dump_suite_active, _gui_probe_mode_enabled())
   def any: fit_opts = {
      "dump_pose": dump_pose,
      "wide_mode": _batch_dump_enabled() || _proof_dump_active(),
      "win_w": _win_w, "win_h": _win_h,
      "cam_x": _cam_slot_num(0, 0.0), "cam_y": _cam_slot_num(1, 0.0), "cam_z": _cam_slot_num(2, 0.0),
      "cam_yaw": _cam_slot_num(6, 0.0), "cam_pitch": _cam_slot_num(7, 0.0), "cam_fov": _cam_slot_num(16, _cam_fov)
   }
   def st = camera.fit_scene_state(mesh, fit_opts)
   if(!is_dict(st) || !st.contains("cam_x")){ return false }
   _trace_fit_camera_apply(mesh, float(st.get("fit_cam_x", 0.0)), float(st.get("fit_cam_y", 0.0)), float(st.get("fit_cam_z", 0.0)),
      float(st.get("cam_x", 0.0)), float(st.get("cam_y", 0.0)), float(st.get("cam_z", 0.0)),
      float(st.get("target_x", 0.0)), float(st.get("target_y", 0.0)), float(st.get("target_z", 0.0)),
      float(st.get("min_x", 0.0)), float(st.get("min_y", 0.0)), float(st.get("min_z", 0.0)),
      float(st.get("max_x", 0.0)), float(st.get("max_y", 0.0)), float(st.get("max_z", 0.0)),
      bool(st.get("force_dump_pose", false)), to_str(st.get("branch", "default")),
   float(st.get("yaw", 0.0)), float(st.get("pitch", 0.0)), float(st.get("fov", _cam_fov)))
   _commit_fit_camera_state(float(st.get("cam_x", 0.0)), float(st.get("cam_y", 0.0)), float(st.get("cam_z", 0.0)),
   float(st.get("yaw", 0.0)), float(st.get("pitch", 0.0)), float(st.get("fov", _cam_fov)))
   true
}

fn _cmd_autofit(log_result=true) {
   if(!_active_scene_valid()){
      if(log_result && (_loaded_scene_name.len > 0 || show_scene || _cli_scene_requested)){
         terminal.log("ERROR: no active scene")
      }
      return false
   }
   def fitted_scene = scene_engine.scene_apply_fit(active_scene)
   if(camera.fit_camera_sane(fitted_scene)){
      active_scene = fitted_scene
   } elif(ui_profile.env_truthy_cached("NY_GLTF_MODEL_DEBUG")){
      terminal.log("[ui] autofit rejected invalid scene fit; preserving loader camera")
   }
   if(ui_profile.env_truthy_cached("NY_GLTF_MODEL_DEBUG")){
      terminal.log("[ui] autofit mesh: baked=" + to_str(bool(active_scene.get("gpu_model_baked", false))) +
         " fit_applied=" + to_str(bool(active_scene.get("fit_applied", false))) +
         " fit_scale=" + to_str(float(active_scene.get("fit_scale", 1.0))) +
         " cam=(" + to_str(float(active_scene.get("fit_cam_x", 0.0))) + "," +
         to_str(float(active_scene.get("fit_cam_y", 0.0))) + "," +
      to_str(float(active_scene.get("fit_cam_z", 0.0))) + ")")
   }
   _apply_scene_fit_transform(active_scene)
   _apply_scene_fit_camera(active_scene)
   _scene_selection_bounds_cache_update()
   if(log_result){
      terminal.log("autofit: " + _loaded_scene_name)
   }
   true
}

fn _cmd_lookat(log_result=true) {
   if(!_active_scene_valid()){
      if(log_result && (_loaded_scene_name.len > 0 || show_scene || _cli_scene_requested)){
         terminal.log("ERROR: no active scene")
      }
      return false
   }
   _apply_scene_fit_camera(active_scene)
   _scene_selection_bounds_cache_update()
   if(log_result){
      terminal.log("lookat: " + _loaded_scene_name)
   }
   true
}

fn _startup_exec_post_load_cmds() {
   _startup_exec_env_var("NY_UI_POST_LOAD_CMD", "post")
   mut i = 0
   while(i < _cli_post_load_cmds.len){
      _startup_exec_line(_cli_post_load_cmds.get(i, ""), "post")
      i += 1
   }
}

fn _sync_window_size_from_live() {
   if(!win){
      return false
   }
   def fsz = win_native.get_framebuffer_size(id(win))
   mut live_w, live_h = float(fsz.get(0, _win_w)), float(fsz.get(1, _win_h))
   if(live_w <= 0.0 || live_h <= 0.0){
      def wsz = size(win)
      live_w, live_h = float(wsz.get(0, _win_w)), float(wsz.get(1, _win_h))
   }
   if(live_w <= 0.0 || live_h <= 0.0){
      return false
   }
   if(live_w == _win_w && live_h == _win_h){
      return false
   }
   _win_w, _win_h = live_w, live_h
   _gui_layout_dirty = true
   _gui_layout_warm_frames = 3
   _proj_dirty = true
   _cross_cx_last = -9e9
   _cross_cy_last = -9e9
   set_win_size(int(_win_w), int(_win_h))
   if(!ui_profile.headless_enabled() && !_term_open && !_scene_loading_input_guard_active() && _intended_cursor_mode == CURSOR_DISABLED){
      center_cursor(win)
   }
   true
}

fn _sync_anim_state_from_scene(mesh) {
   if(!is_dict(mesh)){
      _anim_gltf_data = 0
      _anim_count = 0
      _anim_duration = 0.0
      _skin_count = 0
      _morph_target_count = 0
   } else {
      _anim_gltf_data = mesh.get("gltf_data", 0)
      _anim_count = scene_engine.scene_mesh_int(mesh, "anim_count", 0)
      _anim_duration = scene_engine.scene_mesh_num(mesh, "anim_duration", 0.0)
      _skin_count = scene_engine.scene_mesh_int(mesh, "skin_count", 0)
      _morph_target_count = scene_engine.scene_mesh_int(mesh, "morph_target_count", 0)
   }
   if(_anim_count > 1){
      _anim_idx = -1
   } else {
      _anim_idx = 0
   }
   _anim_time = 0.0
   _anim_speed = 1.0
   mut autoplay = false
   if(ui_profile.env_present_cached("NY_GLTF_AUTOPLAY")){
      autoplay = ui_profile.env_toggle_cached("NY_GLTF_AUTOPLAY", false)
   } else {
      autoplay = !_batch_dump_enabled() && !_gui_dump_suite_active
   }
   _anim_enabled = (_anim_count > 0 && is_dict(_anim_gltf_data) && autoplay)
   if(_anim_enabled && _anim_duration > 0.0001){
      def raw_pose_frac = ui_profile.env_trim_cached("NY_GLTF_AUTOPLAY_POSE_FRACTION")
      ;; Interactive autoplay starts from the clip start.  A mid-clip default is
      ;; useful for deterministic screenshots but misleading for live playback.
      ;; Use NY_GLTF_AUTOPLAY_POSE_FRACTION only for an explicit custom offset.
      mut pose_frac = raw_pose_frac.len > 0 ? float(str.atof(raw_pose_frac)) : 0.0
      if(is_nan(pose_frac) || is_inf(pose_frac)){ pose_frac = 0.0 }
      pose_frac = clamp(pose_frac, 0.0, 1.0)
      _anim_time = _anim_duration * pose_frac
   }
   if(is_dict(mesh)){
      mesh["anim_playing"] = _anim_enabled
      mesh["anim_idx"] = _anim_idx
      mesh["anim_time"] = _anim_time
      mesh["anim_duration"] = _anim_duration
      ;; False means normal autoplay may use the renderer clock until the frame
      ;; update driver writes an explicit sampled time. Setting this true at load
      ;; freezes draw-side sampling at t=0 when the simple update path is used.
      mesh["anim_time_override"] = false
      if(_anim_enabled){
         mesh["static_pose_gpu_ready"] = false
         mesh["parts_model_baked"] = false
         mesh["gpu_model_baked"] = false
         ;; Prime the first visible frame with the same sampled hierarchy that
         ;; the frame loop will use. Otherwise the load command can show a
         ;; partially posed/cached scene until the next update tick.
         def posed_mesh = scene_engine.apply_gltf_animation(mesh, _anim_idx, _anim_time)
         if(to_int(mesh) == to_int(active_scene)){ active_scene = posed_mesh }
      }
   }
}

fn _clear_anim_state() {
   _anim_enabled = false
   _anim_gltf_data = 0
   _anim_count = 0
   _anim_duration = 0.0
   _anim_time = 0.0
   _anim_idx = 0
   _skin_count = 0
   _morph_target_count = 0
   if(is_dict(active_scene)){ active_scene["anim_playing"] = false }
}

fn _sync_scene_anim_playing_flag() bool {
   if(is_dict(active_scene)){
      active_scene["anim_playing"] = _anim_enabled
      if(_anim_enabled){
         active_scene["static_pose_gpu_ready"] = false
         scene_engine.scene_fast_reset(active_scene)
      }
   }
   true
}

fn _anim_select(idx, play=false, label="switched to") {
   if(_anim_count <= 0 || idx < 0 || idx >= _anim_count){
      return false
   }
   _anim_idx = idx
   _anim_time = 0.0
   def ainfo = gltf.gltf_animation_info(_anim_gltf_data, _anim_idx)
   _anim_duration = float(ainfo.get("duration", 0.0))
   if(play){
      _anim_enabled = true
   }
   terminal.log("[anim] " + label + " #" + to_str(_anim_idx) + " dur=" + to_str(_anim_duration) + "s")
   true
}

fn init(any app=0) {
   "Initialize model catalogs and scene runtime caches."
   _refresh_model_catalog()
}

fn update(any app=0, dt=0.0) {
   "Advance scene runtime state for one frame."
   _process_gui_scene_requests()
   _batch_dump_update_anim_fast(dt)
}

fn load_model(any app=0, name="") {
   "Load a model by catalog name through the shared UI runtime."
   _gui_activate_model_name(name)
}

fn _draw_infinite_world_grid(cam_px, cam_pz, include_axes=false) {
   def fine_spacing = 1.0
   def fine_slices = 72
   def coarse_spacing = 10.0
   def coarse_slices = 24
   def fine_x = floor(cam_px / fine_spacing) * fine_spacing
   def fine_z = floor(cam_pz / fine_spacing) * fine_spacing
   rmat.mat4_translate_into(fine_x, 0.0, fine_z, M_PR)
   gfx.set_model_matrix(M_PR)
   def coarse_x = floor(cam_px / coarse_spacing) * coarse_spacing
   def coarse_z = floor(cam_pz / coarse_spacing) * coarse_spacing
   if(include_axes && draw_grid_pair_axes(coarse_x - fine_x, coarse_z - fine_z, 0.0 - fine_x, 0.0 - fine_z)){
      return true
   }
   draw_grid_pair(
      fine_slices,
      fine_spacing,
      [0.11, 0.12, 0.14, 0.28],
      [0.18, 0.20, 0.24, 0.58],
      0.008,
      coarse_slices,
      coarse_spacing,
      [0.20, 0.24, 0.30, 0.18],
      [0.36, 0.42, 0.52, 0.72],
      0.014,
      coarse_x - fine_x,
      coarse_z - fine_z
   )
   false
}

fn _scene_pref_cache_update(name) {
   def key = str.strip(to_str(name))
   if(_scene_pref_cache_name == key){
      return
   }
   _scene_pref_cache_name = key
   _scene_pref_studio = gfx.scene_prefers_studio_env(key)
   _scene_pref_neutral = gfx.scene_prefers_neutral_env(key)
   _scene_pref_reflect = gfx.scene_prefers_compare_reflect_env(key)
   _scene_pref_visible = gfx.scene_prefers_compare_visible_env(key)
   _scene_pref_optical = gfx.scene_prefers_optical_spec_env(key)
   _scene_pref_black_visible = gfx.scene_prefers_black_visible_env(key)
}

fn _apply_batch_scene_clear_color(name) {
   if(!ui_profile.dump_pose_enabled(_auto_dump_enabled, _cli_dump_requested, _batch_dump_enabled(), _gui_dump_suite_active, _gui_probe_mode_enabled())){
      return
   }
   APP_BG = [0.045, 0.052, 0.055, 1.0]
   set_clear_color(APP_BG)
   return
}

fn _auto_dump_ready_now() {
   if(_scene_load_async_active() || _startup_post_load_pending){
      return false
   }
   if(_loaded_scene_name.len > 0 && (!is_dict(active_scene) || !show_scene)){
      return false
   }
   true
}

fn _exec_cursor_cmd(line_l) bool {
   if(eq(line_l, "cursor lock")){
      _cursor_lock_enabled = true
      _sync_cursor_state("cmd cursor lock")
      terminal.log("Cursor lock: ON")
      return true
   }
   if(eq(line_l, "cursor unlock")){
      _cursor_lock_enabled = false
      _sync_cursor_state("cmd cursor unlock")
      terminal.log("Cursor lock: OFF")
      return true
   }
   if(eq(line_l, "cursor toggle") || eq(line_l, "cursor")){
      _cursor_lock_enabled = !_cursor_lock_enabled
      _sync_cursor_state("cmd cursor toggle")
      terminal.log("Cursor lock: " + (_cursor_lock_enabled ? "ON" : "OFF"))
      return true
   }
   false
}

fn _exec_hash_cmd(cmd) bool {
   if(!eq(cmd, "hash")){
      return false
   }
   def cp, cr = camera.get_pos(camthreed), camera.get_rot(camthreed)
   def cf = camera.get_fov(camthreed)
   def c_pos = "pos " + to_str(cp.get(0)) + " " + to_str(cp.get(1)) + " " + to_str(cp.get(2))
   def c_rot = "rot " + to_str(cr.get(0)) + " " + to_str(cr.get(1))
   def c_fov = "fov " + to_str(cf)
   terminal.log(c_pos)
   terminal.log(c_rot)
   terminal.log(c_fov)
   if(print_to_stdout){
      def _discard_hash_pos = print(c_pos)
      def _discard_hash_rot = print(c_rot)
      def _discard_hash_fov = print(c_fov)
   }
   def fb_hash_line = ui_dump.framebuffer_hash_line()
   if(fb_hash_line.len > 0){
      terminal.log(fb_hash_line)
      if(print_to_stdout){
         def _discard_fb_hash = print(fb_hash_line)
      }
   } else {
      terminal.log("ERROR: framebuffer hash unavailable")
   }
   true
}

fn _exec_snapshot_cmd(cmd, line) bool {
   if(!eq(cmd, "snapshot")){
      return false
   }
   mut path = ui_dump.path_named("snapshot_" + to_str(int(ticks())) + ".png", _cli_dump_dir)
   def space_idx = str.find(line, " ")
   if(space_idx >= 0){
      path = str.strip(str.str_slice(line, space_idx + 1, line.len))
   }
   if(path.len == 0){
      path = ui_dump.path_named("snapshot_" + to_str(int(ticks())) + ".png", _cli_dump_dir)
   }
   terminal.log(_snapshot_ok(path) ? "Snapshot saved to " + path : "ERROR: Failed to save snapshot to " + path)
   true
}

fn _exec_projection_camera_cmd(cmd, parts) bool {
   if(eq(cmd, "ortho")){
      _set_projection_mode(common.parse_toggle_arg(parts, is_ortho, !is_ortho))
      return true
   }
   if(eq(cmd, "persp") || eq(cmd, "perspective")){
      _set_projection_mode(!common.parse_toggle_arg(parts, !is_ortho, true))
      return true
   }
   if(eq(cmd, "fov")){
      if(parts.len > 1){
         _cam_fov = clamp(str.atof(parts.get(1)), 15.0, 120.0)
         camera.set_fov(camthreed, _cam_fov)
         _proj_dirty = true
      }
      return true
   }
   if(eq(cmd, "speed")){ if(parts.len > 1){ _spd = str.atof(parts.get(1)) camera.set_speed(camthreed, _spd) } return true }
   if(eq(cmd, "sens")){ if(parts.len > 1){ _sens = str.atof(parts.get(1)) camera.set_sens(camthreed, _sens) } return true }
   if(eq(cmd, "pos")){
      def p = camera.get_pos(camthreed)
      terminal.log(f"POS: {p.get(0):.2f}, {p.get(1):.2f}, {p.get(2):.2f}")
      return true
   }
   if(eq(cmd, "rot")){
      def r = camera.get_rot(camthreed)
      terminal.log(f"ROT: Yaw={r.get(0):.1f} Pitch={r.get(1):.1f}")
      return true
   }
   false
}

fn _exec_selection_cmd(cmd, parts) bool {
   if(eq(cmd, "autofit") || eq(cmd, "fit")){ def _discard_16 = _cmd_autofit(true) return true }
   if(eq(cmd, "lookat") || eq(cmd, "focus") || eq(cmd, "frame")){ def _discard_17 = _cmd_lookat(true) return true }
   if(eq(cmd, "select")){
      if(!_scene_editor_tools_enabled()){
         _scene_clear_selection("cmd select ignored")
         terminal.log("Selection: editor closed")
         return true
      }
      _scene_selected = is_dict(active_scene) && show_scene
      _sync_cursor_state("cmd select")
      terminal.log(_scene_selected ? "Selection: scene" : "Selection: none")
      return true
   }
   if(eq(cmd, "gizmo")){
      if(!_scene_editor_tools_enabled()){
         terminal.log("Gizmo: editor closed")
         return true
      }
      def mode = str.lower(parts.get(1, ""))
      if(eq(mode, "rotate") || eq(mode, "rot") || eq(mode, "r") || eq(mode, "1")){
         _gizmo_mode = 1
      } elif(eq(mode, "scale") || eq(mode, "s") || eq(mode, "2")){
         _gizmo_mode = 2
      } else {
         _gizmo_mode = 0
      }
      terminal.log("Gizmo: " + ui_selection.selection_gizmo_label(_gizmo_mode))
      return true
   }
   if(eq(cmd, "deselect") || eq(cmd, "unselect")){
      _scene_selected = false
      _selection_overlay_clear_rects()
      _sync_cursor_state("cmd deselect")
      terminal.log("Selection: none")
      return true
   }
   false
}

fn _exec_render_cmd(cmd, parts) bool {
   if(eq(cmd, "bg")){ if(parts.len >= 4){ APP_BG = [str.atof(parts.get(1)), str.atof(parts.get(2)), str.atof(parts.get(3)), 1.0] } return true }
   if(eq(cmd, "wireframe")){
      APP_WIRE = common.parse_toggle_arg(parts, APP_WIRE, !APP_WIRE)
      set_wireframe(APP_WIRE)
      return true
   }
   if(eq(cmd, "stats")){ APP_STATS = common.parse_toggle_arg(parts, APP_STATS, !APP_STATS) return true }
   if(eq(cmd, "skybox")){
      def source = parts.len > 1 ? str.strip(str.join_words(parts, " ", 1)) : ""
      if(ui_assets.skybox_source_is_off(source)){
         _reset_skybox_resources()
         skybox_enabled = false
         ui_profile.set_bool("NY_UI_PROOF_SKYBOX", false)
         terminal.log("Skybox: off")
         return true
      }
      if(ui_assets.skybox_source_is_generated(source)){
         _reset_skybox_resources()
         ui_profile.set_bool("NY_UI_PROOF_SKYBOX", true)
         if(_load_fast_generated_skybox(true)){
            terminal.log("Skybox: generated tex=" + to_str(skybox_tex_id))
         } else {
            terminal.log("Skybox: generated failed")
         }
         return true
      }
      skybox_enabled = true
      ui_profile.set_bool("NY_UI_PROOF_SKYBOX", true)
      def ok = _load_skybox(true, source)
      if(ok){
         terminal.log("Skybox: tex=" + to_str(skybox_tex_id))
      } else {
         terminal.log("Skybox: failed")
      }
      return true
   }
   false
}

fn _exec_runtime_cmd(cmd, parts) bool {
   if(eq(cmd, "clear")){ terminal.clear() return true }
   if(eq(cmd, "exit")){ set_should_close(win, true) return true }
   if(eq(cmd, "timeout")){
      if(parts.len > 1){
         def f64: secs = float(max(0.0, str.atof(parts.get(1))))
         _timeout_ns = int(secs * 1e9)
         terminal.log("Timeout: " + to_str(secs) + "s")
      } else {
         _timeout_ns = 0
         terminal.log("Timeout: OFF")
      }
      return true
   }
   if(eq(cmd, "help")){
      terminal.log("CMD: skybox [path|generated|off], load, unload, autofit, lookat, anim, timeout, ortho, persp, clear, hash")
      terminal.log("CMD: snapshot, exit, fov, speed, sens, bg, pos, rot, wireframe, stats")
      return true
   }
   false
}

fn _exec_scene_cmd(cmd, parts) bool {
   if(eq(cmd, "load")){
      def _discard_18 = _load_scene_runtime(str.join_words(parts, " ", 1), false)
      return true
   }
   if(eq(cmd, "unload")){
      if(is_dict(active_scene)){
         scene_engine.unload_scene(active_scene, _loaded_scene_name)
      }
      active_scene = 0
      show_scene = false
      _scene_selected = false
      _scene_selection_bounds_cache_clear()
      _clear_anim_state()
      _loaded_scene_name = ""
      return true
   }
   false
}

fn _exec_anim_cmd(cmd, parts) bool {
   if(!eq(cmd, "anim")){
      return false
   }
   def sub = str.lower(parts.get(1, ""))
   if(eq(sub, "play") || eq(sub, "on")){
      _anim_enabled = true
      def _discard_anim_sync = _sync_scene_anim_playing_flag()
      terminal.log("[anim] playing")
   }
   elif(eq(sub, "pause") || eq(sub, "off")){
      _anim_enabled = false
      def _discard_anim_sync = _sync_scene_anim_playing_flag()
      terminal.log("[anim] paused")
   }
   elif(eq(sub, "stop")){
      _anim_enabled = false
      _anim_time = 0.0
      def _discard_anim_sync = _sync_scene_anim_playing_flag()
      terminal.log("[anim] stopped")
   }
   elif(eq(sub, "next")){
      if(_anim_count > 0){
         def _discard_19 = _anim_select((_anim_idx + 1) % _anim_count)
      }
   }
   elif(eq(sub, "prev")){
      if(_anim_count > 0){
         def _discard_20 = _anim_select((_anim_idx + _anim_count - 1) % _anim_count)
      }
   }
   elif(eq(sub, "speed")){
      if(parts.len > 2){
         _anim_speed = str.atof(parts.get(2))
         terminal.log("[anim] speed=" + to_str(_anim_speed))
      }
   }
   elif(sub.len > 0){
      def n = int(str.atof(sub))
      if(!_anim_select(n, true, "playing")){
         terminal.log("[anim] index out of range(0.." + to_str(_anim_count - 1) + ")")
      }
   } else {
      terminal.log("[anim] count=" + to_str(_anim_count) + " idx=" + to_str(_anim_idx) +
         " t=" + to_str(_anim_time) + "/" + to_str(_anim_duration) +
         " speed=" + to_str(_anim_speed) +
         " skins=" + to_str(_skin_count) +
         " morphTargets=" + to_str(_morph_target_count) +
      " " + (_anim_enabled ? "(playing)" : "(paused)"))
   }
   def _discard_anim_sync_final = _sync_scene_anim_playing_flag()
   true
}

fn exec_cmd(line) {
   "Execute one command line from the UI terminal."
   def line_l = str.lower(str.strip(line))
   if(_exec_cursor_cmd(line_l)){ return }
   def parts = str.split_words(line)
   if(parts.len == 0){
      return
   }
   def cmd = str.lower(parts.get(0, ""))
   if(_exec_runtime_cmd(cmd, parts)){ return }
   if(_exec_hash_cmd(cmd)){ return }
   if(_exec_snapshot_cmd(cmd, line)){ return }
   if(_exec_projection_camera_cmd(cmd, parts)){ return }
   if(_exec_selection_cmd(cmd, parts)){ return }
   if(_exec_render_cmd(cmd, parts)){ return }
   if(_exec_scene_cmd(cmd, parts)){ return }
   if(_exec_anim_cmd(cmd, parts)){ return }
}

;; ---- panel ----
fn _browser_apply_result(res) int {
   if(!is_dict(res)){ return 0 }
   def next_filter = to_str(res.get("filter", _gui_model_filter))
   if(next_filter != _gui_model_filter || bool(res.get("filter_changed", false))){
      _gui_model_filter = next_filter
      _gui_invalidate_model_filter_cache()
   }
   _gui_browser_tab = int(res.get("tab", _gui_browser_tab))
   _gui_model_show_paths = bool(res.get("show_paths", _gui_model_show_paths))
   def action = to_str(res.get("action", ""))
   def model = str.strip(to_str(res.get("model", "")))
   if(_cli_scripted_scene_load && _timeout_ns > 0 && (action == "load" || action == "unload" || action == "fit")){
      if(action == "load" && model.len > 0){ _gui_model_selected_name = model }
      return 0
   }
   if(_gui_asset_load_wait_mouse_up && !gui.mouse_down()){
      _gui_asset_load_wait_mouse_up = false
   }
   if(action == "select" && model.len > 0){
      _gui_model_selected_name = model
   } elif(action == "load" && model.len > 0){
      def press_seq = int(res.get("press_seq", 0))
      if(!_gui_asset_load_wait_mouse_up && press_seq > _gui_last_asset_load_press_seq){
         _gui_last_asset_load_press_seq = press_seq
         def _discard_load = _queue_gui_model_load(model)
      } else {
         _gui_model_selected_name = model
      }
   } elif(action == "unload"){
      def _discard_unload = _queue_gui_unload_scene()
   } elif(action == "fit"){
      if(show_scene && is_dict(active_scene)){
         def _discard_fit, _discard_look = _cmd_autofit(false), _cmd_lookat(false)
      }
   } elif(action == "prefetch" && model.len > 0){
      ui_assets.prefetch_gltf_asset(model)
      terminal.log("Prefetched: " + model)
   } elif(action == "refresh"){
      _model_names = []
      _gui_invalidate_model_catalog_caches()
      _refresh_model_catalog()
   }
   0
}

fn _icon_mode_button(str id, str icon_name, bool selected, f64 w=36.0, f64 h=30.0) bool {
   gui.icon_button(id, viewer_icons.icon_sprite(icon_name), "", w, h, selected)
}

fn _draw_asset_mode_bar(str idp, bool compact=false) int {
   mut tab = (int(_gui_browser_tab) == 1) ? 1 : 0
   gui.text_colored("Assets", [0.82, 0.82, 0.82, 1.0])
   if(_icon_mode_button(idp + "_mode_catalog", "asset_grid", tab == 0)){ tab = 0 }
   gui.same_line()
   if(_icon_mode_button(idp + "_mode_hierarchy", "hierarchy", tab == 1)){ tab = 1 }
   gui.same_line()
   gui.text_colored(tab == 1 ? "Hierarchy" : "Catalog", compact ? [0.70, 0.70, 0.70, 0.92] : [0.76, 0.76, 0.76, 0.94])
   _gui_browser_tab = tab
   tab
}

fn _browser_take_tab_shortcut(str idp, f64 win_w, f64 win_h, bool compact, bool standalone) bool {
   if(standalone){
      _draw_asset_mode_bar(idp, compact)
      gui.separator()
      if(_gui_browser_tab == 1){
         viewer_hierarchy.draw_body(idp, active_scene, win_w, win_h, false, ui_profile.parity_lock_stats_enabled())
         return true
      }
      return false
   }
   false
}

fn _draw_gui_asset_browser_body(str host_id, bool standalone=false) {
   def prof = ui_profile.enabled()
   mut t_prof = prof ? ui_profile.now() : 0
   def host = to_str(host_id)
   def idp = standalone ? "asset" : "editor_asset"
   def win_w = ui_app.app_window_w(host, standalone ? 390.0 : 520.0)
   def win_h = ui_app.app_window_h(host, standalone ? 450.0 : 700.0)
   def compact = win_w < 470.0
   t_prof = ui_profile.mark_next(prof, "asset_body_prep", t_prof)
   if(_browser_take_tab_shortcut(idp, win_w, win_h, compact, standalone)){
      ui_profile.mark_done(prof, "asset_body_shortcut", t_prof)
      return
   }
   def asset_state = _gui_asset_derived_state()
   t_prof = ui_profile.mark_next(prof, "asset_derived_state", t_prof)
   def names = asset_state.get("names", [])
   def filtered_names = asset_state.get("filtered_names", [])
   def filter_key = to_str(asset_state.get("filter_key", ""))
   def selected_idx = int(asset_state.get("selected_idx", 0))
   def any: browser_state = {
      "idp": idp,
      "standalone": standalone,
      "win_w": win_w,
      "win_h": win_h,
      "compact": compact,
      "tab": _gui_browser_tab,
      "filter": _gui_model_filter,
      "show_paths": _gui_model_show_paths,
      "loaded_name": _loaded_scene_name,
      "selected_name": _gui_model_selected_name,
      "scene_loaded": is_dict(active_scene),
      "catalog": asset_state.get("catalog", 0),
      "parity_lock": ui_profile.parity_lock_stats_enabled()
   }
   def res = viewer_browser.draw_body(viewer_browser.prepare_state(browser_state, names, filtered_names, filter_key, selected_idx))
   _browser_apply_result(res)
   def mark = standalone ? "asset_draw_standalone" : (compact ? "asset_draw_compact" : "asset_draw_full")
   def _discard_asset_mark = ui_profile.mark_done(prof, mark, t_prof)
   return
}

fn _draw_gui_asset_browser() {
   if(!_gui_show_browser || _gui_show_editor){
      ui_editor.show_tool("asset_browser", false)
      return
   }
   mut browser_opts = dict(2)
   browser_opts["scrollable"] = false
   _gui_fit_standalone_asset_browser()
   def tool = ui_editor.begin_tool("asset_browser",
      _gui_show_browser,
      "Asset Browser",
      860.0,
      400.0,
      390.0,
      450.0,
   browser_opts)
   def body = bool(tool.get(0, false))
   if(body){
      def title_filter_prev = _gui_model_filter
      _gui_model_filter = gui.title_input_text("asset_title_filter", _gui_model_filter, "Search assets...", 0.0)
      if(_gui_model_filter != title_filter_prev){
         _gui_invalidate_model_filter_cache()
         _gui_layout_dirty = true
         _gui_layout_warm_frames = 2
      }
   }
   if(ui_profile.gui_trace_enabled()){ ui_profile.print_text("[ui:gui-browser] rect=" + ui_app.app_rect_text("asset_browser") + " body=" + to_str(body)) }
   if(_gui_close_tool_window("asset_browser") || bool(tool.get(1, false))){
      _gui_show_browser = false
      return
   }
   if(body){
      _draw_gui_asset_browser_body("asset_browser", true)
   }
   ui_editor.end_tool()
}

fn _draw_gui_console_panel() {
   def res = viewer_panels.console_body(_gui_console_input, terminal.get_history())
   _gui_console_input = to_str(res.get("input", _gui_console_input))
   def action = to_str(res.get("action", ""))
   if(action == "command"){
      def cmd_line = str.strip(to_str(res.get("command", "")))
      if(cmd_line.len > 0){
         terminal.log("> " + cmd_line)
         exec_cmd(cmd_line)
         _gui_console_input = ""
      }
   } elif(action == "clear"){ terminal.clear() }
   elif(action == "fit"){ def _discard_fit_console = _cmd_autofit(true) }
   elif(action == "lookat"){ def _discard_look_console = _cmd_lookat(true) }
}

fn _editor_draw_header(rs, layout_now, active_shot, editor_w, editor_h, card_w) {
   mut header_stats = rs
   header_stats["frame_ms"] = _last_frame_ms
   def display_fps = ui_profile.parity_lock_stats_enabled() ? 0 : fps
   demo_editor.draw_header(
      _loaded_scene_name, display_fps, layout_now, active_shot, _gui_editor_tab, editor_w, editor_h, card_w,
      viewer_icons.icon_sprite("asset_grid"), viewer_icons.icon_sprite("hierarchy"),
      viewer_icons.icon_sprite("preferences"), viewer_icons.icon_sprite("console"), header_stats, _gui_renderer_hotspot_cache
   )
}

fn _editor_catalog_tab(bool dense) {
   if(ui_profile.gui_trace_enabled()){ ui_profile.print_text("[ui:gui-editor] tab_catalog") }
   def prof = ui_profile.enabled()
   mut t_prof = prof ? ui_profile.now() : 0
   if(ui_profile.gui_trace_enabled()){ ui_profile.print_text("[ui:gui-editor] catalog_sanitize") }
   _gui_sanitize_workspace_state()
   t_prof = ui_profile.mark_next(prof, "catalog_sanitize", t_prof)
   _gui_show_browser = false
   if(ui_profile.gui_trace_enabled()){ ui_profile.print_text("[ui:gui-editor] catalog_assets") }
   _draw_gui_asset_browser_body("editor_main", false)
   if(prof){ ui_profile.mark("catalog_asset_browser", t_prof) }
}

fn _editor_hierarchy_tab(bool dense) {
   if(ui_profile.gui_trace_enabled()){ ui_profile.print_text("[ui:gui-editor] tab_hierarchy") }
   viewer_hierarchy.draw_body("editor_hierarchy", active_scene,
      ui_app.app_window_w("editor_main", 520.0),
      max(220.0, gui.remaining_h(0.0)),
      dense,
   ui_profile.parity_lock_stats_enabled())
}

fn _editor_legacy_scene_tab(bool dense) {
   _editor_catalog_tab(dense)
   if(!dense && gui.collapsing_header("scene_runtime", "Runtime", false)){
      APP_STATS = gui.checkbox("stats", "Stats Overlay", APP_STATS)
      def next_wire = gui.checkbox("wire", "Wireframe", APP_WIRE)
      if(next_wire != APP_WIRE){
         APP_WIRE = next_wire
         set_wireframe(APP_WIRE)
      }
      _anim_enabled = gui.checkbox("anim_enabled", "Animation Enabled", _anim_enabled)
      gui.text("Selected model: " + ((_gui_model_selected_name.len > 0) ? _gui_model_selected_name : "<none>"))
      _scene_selected = gui.checkbox("scene_selected", "Scene Selection", _scene_selected)
      _scene_selection_rect = gui.checkbox("scene_selection_rect", "Selection Rectangle", _scene_selection_rect)
   }
}

fn _editor_style_tab() {
   def any: style_state = {"scale": _gui_scale, "gap": _gui_layout_gap, "bg": APP_BG, "accent": _gui_accent}
   def st = demo_editor.draw_style_tab(style_state)
   def scale0 = _gui_scale
   _gui_scale = float(st.get("scale", _gui_scale))
   if(ui_app.app_absf(scale0 - _gui_scale) > 0.001){ _gui_scale_manual = true }
   gui.set_scale(_gui_scale)
   _gui_layout_gap = float(st.get("gap", _gui_layout_gap))
   APP_BG = st.get("bg", APP_BG)
   if(bool(st.get("bg_changed", false))){ set_clear_color(APP_BG) }
   _gui_accent = st.get("accent", _gui_accent)
   if(bool(st.get("accent_changed", false))){ gui.set_accent(_gui_accent) }
}

fn _editor_layout_tab(bool dense, bool compact_header, bool compact, str layout_now, str active_shot) {
   def any: layout_state = {
      "dense": bool(dense),
      "compact_header": bool(compact_header),
      "compact": bool(compact),
      "layout_now": to_str(layout_now),
      "active_shot": to_str(active_shot),
      "layout_items": _gui_layout_preset_items,
      "shot_items": _gui_shot_preset_items,
      "slot_items": _gui_layout_slot_items,
      "probe_overlay": _gui_probe_overlay,
      "layout_name": _gui_layout_preset_name,
      "shot_name": _gui_shot_name,
      "slot_idx": _gui_layout_slot_idx,
   }
   def res = demo_editor.draw_layout_tab(layout_state)
   _gui_probe_overlay = bool(res.get(0, _gui_probe_overlay))
   _gui_layout_preset_name = to_str(res.get(1, _gui_layout_preset_name))
   _gui_shot_name = to_str(res.get(2, _gui_shot_name))
   _gui_layout_slot_idx = int(res.get(3, _gui_layout_slot_idx))
   if(bool(res.get(4, false))){ _gui_layout_dirty = true }
   case to_str(res.get(5, "")){
      "apply_shot" -> { _gui_apply_shell_shot(_gui_shot_name) }
      "retile" -> { _gui_layout_dirty = true }
      "save_slot" -> {
         _gui_layout_slots[_gui_layout_slot_idx] = _gui_capture_layout_state()
         terminal.log("Saved layout " + to_str(_gui_layout_slot_idx + 1))
      }
      "load_slot" -> {
         if(_gui_apply_layout_state(_gui_layout_slots.get(_gui_layout_slot_idx, 0))){
            terminal.log("Loaded layout " + to_str(_gui_layout_slot_idx + 1))
         }
      }
      "dump_shot", "frame_dump" -> { def _discard_27 = _gui_take_snapshot() }
      "reset_graph" -> {
         _gui_graph_nodes = []
         _gui_graph_links = []
         _gui_init_editor_graph()
      }
      "probe" -> {
         _gui_last_probe_text = ui_diag.probe_text()
         terminal.log(_gui_last_probe_text)
      }
      "print_probe" -> {
         ui_diag.print_probe()
         _gui_last_probe_text = ui_diag.probe_text()
      }
      _ -> {}
   }
}

fn _editor_console_info_tab(bool dense, bool compact_header, bool compact, str layout_now, str active_shot) {
   _draw_gui_console_panel()
   if(gui.remaining_h(0.0) >= 190.0){
      gui.separator()
      _editor_layout_tab(dense, compact_header, compact, layout_now, active_shot)
   }
}

fn _editor_tab_body(bool dense, bool compact_header, bool compact, str layout_now, str active_shot) int {
   case _gui_editor_tab {
      0 -> { _editor_catalog_tab(dense) }
      1 -> { _editor_hierarchy_tab(dense) }
      2 -> { _editor_style_tab() }
      _ -> { _editor_console_info_tab(dense, compact_header, compact, layout_now, active_shot) }
   }
   0
}

fn _draw_gui_editor() {
   if(ui_profile.gui_trace_enabled()){ ui_profile.print_text("[ui:gui-editor] begin_window") }
   def prof = ui_profile.enabled()
   mut t_prof = prof ? ui_profile.now() : 0
   mut editor_opts = _gui_has_tiled_peer() ? _gui_pinned_tool_opts(false) : dict(2)
   editor_opts["scrollable"] = false
   _gui_fit_standalone_editor()
   def tool = ui_editor.begin_tool("editor_main", _gui_show_editor, "Editor", 20.0, 20.0, 520.0, 700.0, editor_opts)
   t_prof = ui_profile.mark_next(prof, "editor_begin_tool", t_prof)
   def body = bool(tool.get(0, false))
   if(ui_profile.gui_trace_enabled()){ ui_profile.print_text("[ui:gui-editor] body=" + to_str(body)) }
   if(_gui_close_tool_window("editor_main", false) || bool(tool.get(1, false))){
      _gui_close_editor_shell()
      return
   }
   if(!_gui_show_editor){
      return
   }
   if(body){
      def rs = _gui_frame_stats_cache
      def editor_w, editor_h = ui_app.app_window_w("editor_main", 520.0), ui_app.app_window_h("editor_main", 700.0)
      def editor_metrics = demo_editor.chrome_metrics(editor_w, editor_h)
      def compact, dense = bool(editor_metrics.get("compact", false)), bool(editor_metrics.get("dense", false))
      def compact_header, summary_cols = bool(editor_metrics.get("compact_header", false)), int(editor_metrics.get("summary_cols", 1))
      def card_w = ui_app.app_card_w("editor_main", summary_cols, 10.0, compact ? 0.0 : 82.0)
      def last_dump_path, last_probe_text = to_str(_gui_last_dump_path), to_str(_gui_last_probe_text)
      def layout_now, active_shot = _gui_layout_preset_env(), (_gui_shot_name.len > 0) ? _gui_shot_name : "full_editor"
      if(ui_profile.gui_trace_enabled()){ ui_profile.print_text("[ui:gui-editor] intro") }
      _gui_sanitize_workspace_state()
      def prev_visible = demo_editor.visibility_snapshot(_gui_show_gallery, _gui_show_probe, _gui_show_browser, _gui_show_profiler, _gui_show_workspace, _gui_show_graph, _gui_show_inspector)
      t_prof = ui_profile.mark_next(prof, "editor_prep", t_prof)
      _gui_editor_tab = _editor_draw_header(rs, layout_now, active_shot, editor_w, editor_h, card_w)
      t_prof = ui_profile.mark_next(prof, "editor_header", t_prof)
      gui.separator()
      if(ui_profile.gui_trace_enabled()){ ui_profile.print_text("[ui:gui-editor] body") }
      t_prof = ui_profile.mark_next(prof, "editor_tabs", t_prof)
      _editor_tab_body(dense, compact_header, compact, layout_now, active_shot)
      t_prof = ui_profile.mark_next(prof, "editor_tab_body", t_prof)
      _gui_sync_workspace_visibility()
      if(demo_editor.visibility_changed(prev_visible, _gui_show_gallery, _gui_show_probe, _gui_show_browser, _gui_show_profiler, _gui_show_workspace, _gui_show_graph, _gui_show_inspector)){
         _gui_layout_dirty = true
         _gui_layout_warm_frames = 4
      }
      demo_editor.draw_footer(last_dump_path, last_probe_text, ui_profile.gui_trace_enabled(), editor_w)
      if(prof){ ui_profile.mark("editor_footer", t_prof) }
   }
   if(ui_profile.gui_trace_enabled()){ ui_profile.print_text("[ui:gui-editor] end_window") }
   ui_editor.end_tool()
}

fn _apply_tool_close_state(st, break_dock=true) bool {
   if(!bool(st.get("closed", false))){ return false }
   if(break_dock && _gui_workspace_mode == 1){ _gui_workspace_mode = 0 }
   _gui_layout_dirty = true
   _gui_layout_warm_frames = 4
   true
}

fn _draw_gui_gallery(phase) {
   def any: gallery_state = {
      "show": _gui_show_gallery, "tab": _gui_gallery_tab, "context_items": _gui_demo_context_items,
      "combo": _gui_demo_combo, "radio": _gui_demo_radio, "toggle_a": _gui_demo_toggle_a,
      "toggle_b": _gui_demo_toggle_b, "progress": _gui_demo_progress, "float": _gui_demo_float,
      "int": _gui_demo_int, "accent": _gui_accent, "phase": phase, "frame_stats": _gui_frame_stats_cache,
      "renderer_hotspot": _gui_renderer_hotspot_cache, "last_frame_ms": _last_frame_ms,
      "last_draw_ms": _last_draw_ms, "last_ui_ms": _last_ui_ms, "fps": fps, "model_count": _model_names.len
   }
   def st = viewer_tools.draw_gallery(gallery_state)
   _gui_show_gallery, _gui_demo_toggle_a, _gui_demo_toggle_b =
   bool(st.get("show", _gui_show_gallery)), bool(st.get("toggle_a", _gui_demo_toggle_a)), bool(st.get("toggle_b", _gui_demo_toggle_b))
   _gui_gallery_tab, _gui_demo_combo, _gui_demo_radio, _gui_demo_int =
   int(st.get("tab", _gui_gallery_tab)), int(st.get("combo", _gui_demo_combo)), int(st.get("radio", _gui_demo_radio)), int(st.get("int", _gui_demo_int))
   _gui_demo_progress, _gui_demo_float, _gui_accent =
   float(st.get("progress", _gui_demo_progress)), float(st.get("float", _gui_demo_float)), st.get("accent", _gui_accent)
   def action = to_str(st.get("action", ""))
   if(action == "primary"){ terminal.log("Primary button clicked") }
   elif(action == "small"){ terminal.log("Small button clicked") }
   _apply_tool_close_state(st)
}

fn _draw_gui_graph() {
   _gui_init_editor_graph()
   def any: graph_state = {"show": _gui_show_graph, "nodes": _gui_graph_nodes, "links": _gui_graph_links, "workspace_grid": _gui_workspace_grid}
   def st = viewer_tools.draw_graph(graph_state)
   _gui_show_graph, _gui_graph_nodes, _gui_graph_links =
   bool(st.get("show", _gui_show_graph)), st.get("nodes", _gui_graph_nodes), st.get("links", _gui_graph_links)
   if(to_str(st.get("action", "")) == "reset_graph"){ _gui_init_editor_graph() }
   _apply_tool_close_state(st)
}

fn _inspector_state(any rs, int part_count, int mat_mask) any {
   _scene_pref_cache_update(_loaded_scene_name)
   mut display_rs = rs
   mut display_part_count = part_count
   if(ui_profile.parity_lock_stats_enabled()){
      display_rs = {
         "draws": 0, "dynamic_draws": 0, "static_draws": 0, "indexed_draws": 0,
         "flushes": 0, "pipeline_binds": 0, "descriptor_binds": 0, "submitted_vertices": 0
      }
      display_part_count = 0
   }
   def selected_path = (_gui_model_selected_name.len > 0) ? ui_assets.resolve_gltf_asset_path(_gui_model_selected_name) : "<no selected model>"
   def selected_part_idx = viewer_hierarchy.selected_part()
   def selected_part_mat_idx = viewer_hierarchy.selected_material()
   mut selected_material_idx = _gui_material_selected_idx
   if(selected_part_idx >= 0 && selected_part_idx != _gui_material_selected_part_idx && selected_part_mat_idx >= 0){
      selected_material_idx = selected_part_mat_idx
      _gui_material_selected_idx = selected_material_idx
      _gui_material_selected_part_idx = selected_part_idx
   }
   def edit_scale = is_dict(active_scene) ? float(active_scene.get("edit_scale", 1.0)) : 1.0
   def any: st = {
      "tab": _gui_inspector_tab, "tab_items": viewer_inspector.TAB_ITEMS,
      "renderer": display_rs, "renderer_hotspot": _gui_renderer_hotspot_cache,
      "scene": is_dict(active_scene) ? active_scene : dict(0), "has_scene": is_dict(active_scene),
      "show_scene": show_scene, "scene_name": _loaded_scene_name, "selected_path": selected_path,
      "part_count": display_part_count, "mat_mask": mat_mask,
      "selected_part": selected_part_idx, "selected_material": selected_material_idx,
      "anim_count": _anim_count, "anim_time": _anim_time, "anim_duration": _anim_duration,
      "anim_enabled": _anim_enabled, "anim_speed": _anim_speed,
      "gizmo_mode": _gizmo_mode, "gizmo_axis": _gizmo_axis,
      "gizmo_snap": _gizmo_snap, "gizmo_precise": _gizmo_precise, "gizmo_ruler": _gizmo_ruler,
      "scene_selected": _scene_selected, "selection_rect": _scene_selection_rect,
      "edit_tx": is_dict(active_scene) ? float(active_scene.get("edit_tx", 0.0)) : 0.0,
      "edit_ty": is_dict(active_scene) ? float(active_scene.get("edit_ty", 0.0)) : 0.0,
      "edit_tz": is_dict(active_scene) ? float(active_scene.get("edit_tz", 0.0)) : 0.0,
      "edit_rx": is_dict(active_scene) ? float(active_scene.get("edit_rx", 0.0)) : 0.0,
      "edit_ry": is_dict(active_scene) ? float(active_scene.get("edit_ry", 0.0)) : 0.0,
      "edit_rz": is_dict(active_scene) ? float(active_scene.get("edit_rz", 0.0)) : 0.0,
      "edit_sx": is_dict(active_scene) ? float(active_scene.get("edit_sx", edit_scale)) : 1.0,
      "edit_sy": is_dict(active_scene) ? float(active_scene.get("edit_sy", edit_scale)) : 1.0,
      "edit_sz": is_dict(active_scene) ? float(active_scene.get("edit_sz", edit_scale)) : 1.0,
      "cam_x": _cam_px, "cam_y": _cam_py, "cam_z": _cam_pz,
      "yaw": _h_yaw, "pitch": _h_pch, "fov": _cam_fov,
      "speed": _spd, "sens": _sens, "rmb_sens": _rmb_look_sens_mul, "drag": _drag, "damp": _damp,
      "is_ortho": is_ortho, "ortho_zoom": _ortho_zoom, "cursor_lock": _cursor_lock_enabled,
      "stats": APP_STATS, "wire": APP_WIRE, "bg": APP_BG,
      "gui_scale": _gui_scale, "layout_gap": _gui_layout_gap,
      "workspace_grid": _gui_workspace_grid, "workspace_major": _gui_workspace_major,
      "env_mode": _gui_env_mode, "env_items": _gui_env_mode_items,
      "skybox_enabled": skybox_enabled, "skybox_tex": skybox_tex_id, "skybox_spec_tex": skybox_spec_tex_id,
      "compare_env_tex": compare_env_tex_id, "neutral_env_tex": neutral_env_tex_id,
      "scene_env_sensitive": is_dict(active_scene) && bool(active_scene.get("scene_env_sensitive_materials", false)),
      "pref_studio": _scene_pref_studio, "pref_neutral": _scene_pref_neutral, "pref_reflect": _scene_pref_reflect,
      "pref_visible": _scene_pref_visible, "pref_optical": _scene_pref_optical,
      "msaa": APP_MSAA, "msaa_items": _gui_msaa_items, "vsync": APP_VSYNC, "filter_linear": APP_FILTER_LINEAR,
      "backend": active_backend_name, "dpi_scale": _gui_dpi_scale,
      "last_frame_ms": _last_frame_ms, "last_update_ms": _last_update_ms,
      "last_world_ms": _last_world_ms, "last_draw_ms": _last_draw_ms, "last_ui_ms": _last_ui_ms,
      "frame_ms_samples": _frame_ms_samples, "win_w": _win_w, "win_h": _win_h, "term_open": _term_open,
      "inspector_w": ui_app.app_window_w("inspector", 390.0),
      "last_dump": _gui_last_dump_path, "window_backend": backend()
   }
   st
}

fn _apply_inspector_result(st) any {
   if(!is_dict(st)){ return 0 }
   def old_msaa, old_vsync, old_filter = APP_MSAA, APP_VSYNC, APP_FILTER_LINEAR
   _gui_inspector_tab = int(st.get("tab", _gui_inspector_tab))
   def old_anim_enabled = _anim_enabled
   _anim_enabled = bool(st.get("anim_enabled", _anim_enabled))
   if(old_anim_enabled != _anim_enabled){
      def _discard_anim_sync = _sync_scene_anim_playing_flag()
   }
   _anim_speed = float(st.get("anim_speed", _anim_speed))
   def old_gizmo_mode, old_gizmo_axis = _gizmo_mode, _gizmo_axis
   def old_gizmo_snap, old_gizmo_precise, old_gizmo_ruler = _gizmo_snap, _gizmo_precise, _gizmo_ruler
   _gizmo_mode = int(clamp(float(st.get("gizmo_mode", _gizmo_mode)), 0.0, 2.0))
   _gizmo_axis = int(clamp(float(st.get("gizmo_axis", _gizmo_axis)), 0.0, 3.0))
   _gizmo_snap = bool(st.get("gizmo_snap", _gizmo_snap))
   _gizmo_precise = bool(st.get("gizmo_precise", _gizmo_precise))
   _gizmo_ruler = bool(st.get("gizmo_ruler", _gizmo_ruler))
   if(old_gizmo_mode != _gizmo_mode || old_gizmo_axis != _gizmo_axis ||
      old_gizmo_snap != _gizmo_snap || old_gizmo_precise != _gizmo_precise ||
   old_gizmo_ruler != _gizmo_ruler){ _scene_edit_redraw(2) }
   if(bool(st.get("transform_changed", false)) && is_dict(active_scene)){
      active_scene["edit_tx"] = float(st.get("edit_tx", active_scene.get("edit_tx", 0.0)))
      active_scene["edit_ty"] = float(st.get("edit_ty", active_scene.get("edit_ty", 0.0)))
      active_scene["edit_tz"] = float(st.get("edit_tz", active_scene.get("edit_tz", 0.0)))
      active_scene["edit_rx"] = float(st.get("edit_rx", active_scene.get("edit_rx", 0.0)))
      active_scene["edit_ry"] = float(st.get("edit_ry", active_scene.get("edit_ry", 0.0)))
      active_scene["edit_rz"] = float(st.get("edit_rz", active_scene.get("edit_rz", 0.0)))
      active_scene["edit_sx"] = clamp(float(st.get("edit_sx", active_scene.get("edit_sx", 1.0))), 0.02, 50.0)
      active_scene["edit_sy"] = clamp(float(st.get("edit_sy", active_scene.get("edit_sy", 1.0))), 0.02, 50.0)
      active_scene["edit_sz"] = clamp(float(st.get("edit_sz", active_scene.get("edit_sz", 1.0))), 0.02, 50.0)
      active_scene["edit_scale"] = (float(active_scene.get("edit_sx", 1.0)) + float(active_scene.get("edit_sy", 1.0)) + float(active_scene.get("edit_sz", 1.0))) / 3.0
      _scene_selection_bounds_cache_clear()
      _scene_edit_redraw(3)
   }
   if(bool(st.get("selection_changed", false))){
      _scene_selected = bool(st.get("scene_selected", _scene_selected)) && is_dict(active_scene) && show_scene && _scene_editor_tools_enabled()
      _scene_selection_rect = bool(st.get("selection_rect", _scene_selection_rect))
      if(_scene_selected){
         _scene_selection_bounds_cache_update()
         _scene_edit_redraw(2)
      } else {
         _selection_overlay_clear_rects()
         _scene_selection_bounds_cache_clear()
      }
      _sync_cursor_state("inspect selection")
   }
   _cam_px, _cam_py, _cam_pz = float(st.get("cam_x", _cam_px)), float(st.get("cam_y", _cam_py)), float(st.get("cam_z", _cam_pz))
   _h_yaw, _h_pch, _cam_fov = float(st.get("yaw", _h_yaw)), float(st.get("pitch", _h_pch)), float(st.get("fov", _cam_fov))
   _spd, _sens = float(st.get("speed", _spd)), float(st.get("sens", _sens))
   _rmb_look_sens_mul = float(st.get("rmb_sens", _rmb_look_sens_mul))
   _drag, _damp = float(st.get("drag", _drag)), float(st.get("damp", _damp))
   _ortho_zoom = float(st.get("ortho_zoom", _ortho_zoom))
   if(bool(st.get("projection_changed", false))){ _set_projection_mode(bool(st.get("is_ortho", is_ortho))) }
   if(bool(st.get("camera_changed", false))){ _gui_apply_camera_state() }
   APP_STATS = bool(st.get("stats", APP_STATS))
   if(bool(st.get("wire_changed", false))){ APP_WIRE = bool(st.get("wire", APP_WIRE)) set_wireframe(APP_WIRE) }
   show_scene = bool(st.get("show_scene", show_scene))
   if(bool(st.get("cursor_changed", false))){ _cursor_lock_enabled = bool(st.get("cursor_lock", _cursor_lock_enabled)) _sync_cursor_state("inspect cursor lock") }
   _gui_material_selected_idx = int(st.get("selected_material", _gui_material_selected_idx))
   _gui_material_selected_part_idx = int(st.get("selected_part", _gui_material_selected_part_idx))
   if(bool(st.get("material_changed", false)) && is_dict(active_scene)){
      def tweak = st.get("material_tweak", dict(0))
      if(is_dict(tweak)){
         active_scene = scene_engine.scene_apply_material_tweak(active_scene,
            int(tweak.get("mat_idx", -1)),
            tweak,
            float(tweak.get("metallic", 1.0)),
         float(tweak.get("roughness", 1.0)))
      }
   }
   APP_BG = st.get("bg", APP_BG)
   if(bool(st.get("bg_changed", false))){ set_clear_color(APP_BG) }
   def scale0 = _gui_scale
   _gui_scale = float(st.get("gui_scale", _gui_scale))
   if(bool(st.get("scale_changed", false)) || ui_app.app_absf(scale0 - _gui_scale) > 0.001){ _gui_scale_manual = true }
   gui.set_scale(_gui_scale)
   def gap0 = _gui_layout_gap
   _gui_layout_gap = float(st.get("layout_gap", _gui_layout_gap))
   _gui_workspace_grid = float(st.get("workspace_grid", _gui_workspace_grid))
   _gui_workspace_major = int(st.get("workspace_major", _gui_workspace_major))
   if(bool(st.get("layout_changed", false)) || ui_app.app_absf(gap0 - _gui_layout_gap) > 0.001){ _gui_layout_dirty = true _gui_layout_warm_frames = 4 }
   _gui_env_mode = int(st.get("env_mode", _gui_env_mode))
   skybox_enabled = bool(st.get("skybox_enabled", skybox_enabled))
   APP_MSAA = int(st.get("msaa", APP_MSAA))
   APP_VSYNC = bool(st.get("vsync", APP_VSYNC))
   APP_FILTER_LINEAR = bool(st.get("filter_linear", APP_FILTER_LINEAR))
   if(APP_MSAA != old_msaa){ terminal.log("MSAA queued for next renderer init: " + to_str(APP_MSAA)) }
   if(APP_VSYNC != old_vsync){ terminal.log("VSync queued for next renderer init: " + (APP_VSYNC ? "on" : "off")) }
   if(APP_FILTER_LINEAR != old_filter){ terminal.log("Texture filter queued for next renderer init: " + (APP_FILTER_LINEAR ? "on" : "off")) }
   case to_str(st.get("action", "")){
      "autofit" -> { def _discard_48 = _cmd_autofit(true) }
      "lookat" -> { def _discard_49 = _cmd_lookat(true) }
      "unload" -> { def _discard_unload = _queue_gui_unload_scene() }
      "reset_transform" -> {
         if(is_dict(active_scene)){
            active_scene["edit_tx"] = 0.0
            active_scene["edit_ty"] = 0.0
            active_scene["edit_tz"] = 0.0
            active_scene["edit_rx"] = 0.0
            active_scene["edit_ry"] = 0.0
            active_scene["edit_rz"] = 0.0
            active_scene["edit_sx"] = 1.0
            active_scene["edit_sy"] = 1.0
            active_scene["edit_sz"] = 1.0
            active_scene["edit_scale"] = 1.0
            _scene_selection_bounds_cache_clear()
            _scene_edit_redraw(3)
         }
      }
      "ensure_skybox" -> { def _discard_env = _ensure_visible_skybox_ready() }
      "load_skybox" -> { _load_skybox() }
      "lighting_off" -> { _gui_env_mode = 4 }
      "probe" -> { _gui_last_probe_text = ui_diag.probe_text() terminal.log(_gui_last_probe_text) }
      "retile" -> { _gui_layout_dirty = true _gui_layout_warm_frames = 4 }
      _ -> {}
   }
}

fn _draw_gui_inspector() {
   def inspector_opts = _gui_show_editor ? _gui_pinned_tool_opts(true) : dict(0)
   def tool = ui_editor.begin_tool("inspector", _gui_show_inspector, "Inspector", 860.0, 20.0, 390.0, 430.0, inspector_opts)
   def body = bool(tool.get(0, false))
   if(ui_profile.gui_trace_enabled()){ ui_profile.print_text("[ui:gui-inspector] rect=" + ui_app.app_rect_text("inspector") + " body=" + to_str(body)) }
   if(_gui_close_tool_window("inspector") || bool(tool.get(1, false))){
      if(_gui_show_editor && _gui_workspace_mode == 1){
         _gui_close_editor_shell()
      } else {
         _gui_show_inspector = false
      }
      return
   }
   if(!_gui_show_inspector){
      return
   }
   if(body){
      def rs = _gui_frame_stats_cache
      def part_count = _gui_scene_parts_cache
      def mat_mask = (is_dict(active_scene)) ? int(active_scene.get("material_feature_mask", 0)) : 0
      _apply_inspector_result(viewer_inspector.draw_body(_inspector_state(rs, part_count, mat_mask)))
   }
   ui_editor.end_tool()
}

fn _draw_gui_probe() {
   def any: probe_state = {
      "show": _gui_show_probe, "win": win, "layout": _gui_layout_preset_env(),
      "shot": (_gui_shot_name.len > 0) ? _gui_shot_name : "<live>", "scene": _loaded_scene_name,
      "fps": fps, "win_w": _win_w, "win_h": _win_h, "last_frame_ms": _last_frame_ms, "last_probe_text": _gui_last_probe_text
   }
   def st = viewer_tools.draw_probe(probe_state)
   _gui_show_probe = bool(st.get("show", _gui_show_probe))
   def action = to_str(st.get("action", ""))
   if(action == "refresh"){
      _gui_last_probe_text = ui_diag.probe_text()
      terminal.log(_gui_last_probe_text)
   }
   _apply_tool_close_state(st)
}

fn _draw_gui_profiler() {
   def any: profiler_state = {
      "show": _gui_show_profiler, "renderer": _gui_frame_stats_cache,
      "profile": viewer_tools.profiler_snapshot(fps, _last_frame_ms, _last_update_ms, _last_world_ms, _last_draw_ms, _last_ui_ms, _fps_samples, _frame_ms_samples, _draw_ms_samples, _ui_ms_samples, ui_profile.parity_lock_stats_enabled()),
      "renderer_hotspot": _gui_renderer_hotspot_cache
   }
   def st = viewer_tools.draw_profiler(profiler_state)
   _gui_show_profiler = bool(st.get("show", _gui_show_profiler))
   _apply_tool_close_state(st)
}

fn _draw_gui_workspace() {
   def any: workspace_state = {
      "show": _gui_show_workspace, "grid": _gui_workspace_grid, "major": _gui_workspace_major,
      "cam_x": _cam_px, "cam_y": _cam_py, "cam_z": _cam_pz, "font": _ui_font()
   }
   def st = viewer_tools.draw_workspace(workspace_state)
   _gui_show_workspace, _gui_workspace_grid, _gui_workspace_major =
   bool(st.get("show", _gui_show_workspace)), float(st.get("grid", _gui_workspace_grid)), int(st.get("major", _gui_workspace_major))
   _apply_tool_close_state(st)
}

;; ---- overlay ----
fn _prepare_gui_overlay_pass() {
   viewer_overlay.prepare_pass(_win_w, _win_h, M_UI_OVERLAY)
   set_font(_ui_font())
}

fn _draw_editor_backdrop(phase, ww, wh) {
   if(!_gui_enabled_now()){
      return
   }
   if(is_dict(active_scene) && show_scene){
      return
   }
   if(!_gui_probe_mode_enabled()){
      return
   }
   viewer_overlay.draw_backdrop(float(ww), float(wh), 0.86)
   return
}

fn _draw_loading_overlay(phase, ww, wh) {
   if(!_scene_load_async_active() || _proof_dump_active()){
      return
   }
   _prepare_gui_overlay_pass()
   viewer_loading.draw_card(float(phase), float(ww), float(wh), _ui_title_font(), _ui_font(), _scene_load_async_spec)
   return
}

fn _draw_runtime_stats_overlay_fast(reuse_color=false) {
   if(!APP_STATS || _proof_dump_active() || _gui_enabled_now() || _batch_dump_enabled() || _auto_dump_enabled == 1 || _pending_auto_dump){
      return
   }
   viewer_overlay.draw_fps(_ui_font(), int(fps), bool(reuse_color))
   return
}

fn _draw_runtime_overlay_after_world(reuse_color=false) {
   if(!APP_STATS){
      return
   }
   if(viewer_overlay.fps_skip_reuse(bool(reuse_color), int(fps))){
      return
   }
   _prepare_gui_overlay_pass()
   _draw_runtime_stats_overlay_fast(reuse_color)
}

fn _draw_ui(phase, term_open) {
   def ww = _win_w
   def wh = _win_h
   def chrome_on = _chrome_visible()
   if(!chrome_on){
      if(term_open){
         terminal.draw(ww, wh, phase)
      }
      return
   }
   def gui_now = _gui_enabled_now()
   def loading_now = _scene_load_async_active() && !_proof_dump_active()
   def crosshair_now = ui_profile.crosshair_enabled() && !term_open && !_proof_dump_active() && !gui_now
   if(!gui_now && !term_open && !loading_now && !APP_STATS && !crosshair_now){
      gui.set_enabled(false)
      return
   }
   _prepare_gui_overlay_pass()
   if(gui_now || term_open || loading_now){
      _draw_editor_backdrop(phase, ww, wh)
      _draw_editor_gui(phase)
      if(term_open){
         terminal.draw(ww, wh, phase)
      }
      if(loading_now){
         _draw_loading_overlay(phase, ww, wh)
      }
   }
   _draw_runtime_stats_overlay_fast()
   if(crosshair_now){
      def center = viewer_overlay.draw_crosshair(float(ww), float(wh), _crosshair_mesh, M_cross_t, _cross_cx_last, _cross_cy_last)
      _cross_cx_last = float(center.get(0, _cross_cx_last))
      _cross_cy_last = float(center.get(1, _cross_cy_last))
   }
   return
}

fn _prepare_static_world_visual_fast() bool {
   if(_scene_deform_blocks_static()){ return false }
   _scene_pref_cache_update(_loaded_scene_name)
   if(_scene_pref_studio || _scene_pref_neutral || _scene_pref_reflect ||
      _scene_pref_visible || _scene_pref_optical || _scene_pref_black_visible){
      return false
   }
   def proof_sky = _proof_dump_active() && ui_profile.env_truthy_cached("NY_UI_PROOF_SKYBOX")
   if((skybox_enabled || proof_sky) && skybox_tex_id < 0 && !_ensure_visible_skybox_ready()){
      return false
   }
   _static_world_draw_sky = (skybox_enabled || proof_sky) && skybox_tex_id >= 0
   _ui_update_projection_fast()
   gfx.set_view_proj(M_VP)
   gfx.set_cam_pos(_cam_px_cache, _cam_py_cache, _cam_pz_cache)
   if(ui_profile.env_truthy_cached("NY_UI_DISABLE_ENV")){
      gfx.set_env_tex(-1)
      gfx.set_env_spec_tex(-1)
   } elif(ui_profile.env_truthy_cached("NY_UI_DISABLE_ENV_SPEC")){
      gfx.set_env_tex(skybox_tex_id)
      gfx.set_env_spec_tex(-1)
   } else {
      gfx.set_env_tex(skybox_tex_id)
      gfx.set_env_spec_tex((skybox_spec_tex_id >= 0) ? skybox_spec_tex_id : skybox_tex_id)
   }
   true
}

fn _draw_static_world_visual_fast(load_color=false) {
   if(_static_world_draw_sky && !load_color){
      gfx.set_view_proj(M_VP_SKY)
      gfx.set_model_matrix(M_ID)
      _render_draw_skybox(skybox_tex_id)
      gfx.set_view_proj(M_VP)
   }
   _draw_active_scene_fast_or_fallback()
}

fn _static_world_skip_draw_now(load_color=false) bool {
   if(!load_color){ return false }
   if(!_static_world_present_reuse_allowed(load_color) && _ui_view_input_active()){ return false }
   _last_world_ms = 0.0
   _last_ui_ms = 0.0
   true
}

fn _draw_active_scene_fast_or_fallback() bool {
   if(!is_dict(active_scene) || !show_scene){
      return false
   }
   def draw_trace = ui_profile.env_truthy_cached("NY_UI_GROUP_TRACE")
   if(draw_trace){
      def scene_gpu_slab_ready = to_int(active_scene.get("gpu_parts_slab", 0)) != 0
      mut scene_state_count = 0
      mut scene_state_slab = false
      def scene_gpu_state = active_scene.get("gpu_draw_state", 0)
      if(is_list(scene_gpu_state) && scene_gpu_state.len >= 2){
         scene_state_slab = to_int(scene_gpu_state.get(0, 0)) != 0
         scene_state_count = int(scene_gpu_state.get(1, 0))
      }
      ui_profile.print_text("[ui:scene-draw] gpu_parts=" + to_str(int(active_scene.get("gpu_parts_count", 0))) +
         " parts=" + to_str(int(active_scene.get("parts_count", 0))) +
         " slab=" + to_str(scene_gpu_slab_ready) +
         " state_slab=" + to_str(scene_state_slab) +
      " state_count=" + to_str(scene_state_count))
   }
   if(!_scene_deform_blocks_static() && scene_engine.scene_fast_draw(active_scene, _active_scene_model_matrix(), draw_trace)){
      return true
   }
   if(draw_trace){ ui_profile.print_text("[ui:scene-draw] fast path missed; falling back") }
   _set_active_scene_model_matrix()
   gfx.reset_overlay_state()
   gfx.set_unlit(false)
   draw_mesh_group(active_scene)
   true
}

fn draw(phase, term_open) {
   "Draw only the overlay pass. Frame/world composition lives in this file's frame loop."
   _draw_ui(phase, term_open)
}

fn _gui_frame_trace(bool on, msg) bool {
   if(!on){ return false }
   ui_profile.print_text("[ui:gui-frame] " + to_str(msg))
}

fn _draw_editor_gui(phase) {
   def gui_enabled = _gui_enabled_now()
   gui.set_enabled(gui_enabled)
   if(!gui_enabled){
      return
   }
   def gui_trace = ui_profile.gui_trace_enabled()
   def parity_editor_compact =
   ui_profile.editor_parity_trace_enabled() &&
   _gui_probe_mode_enabled() &&
   _gui_shot_name == "editor_scene_compact"
   def rs_before = parity_editor_compact ? renderer_frame_stats() : dict(0)
   _gui_frame_trace(gui_trace, "begin_frame")
   def gui_vo0 = gui_trace ? _render_vertex_offset() : 0
   _gui_refresh_frame_metrics()
   def _discard_auto_scale = _gui_refresh_auto_scale()
   gui.set_scale(_gui_scale)
   gui.set_accent(_gui_accent)
   gui.set_debug_overlay(_gui_probe_overlay)
   gui.set_fonts(_ui_font(), _ui_title_font(), _ui_small_font())
   gui.begin_frame(0, _ui_font(), _win_w, _win_h)
   _gui_apply_tiled_layout(false)
   if(_gui_show_editor){
      _gui_frame_trace(gui_trace, "editor")
      _draw_gui_editor()
   }
   if(_gui_show_profiler){
      _gui_frame_trace(gui_trace, "profiler")
      _draw_gui_profiler()
   }
   if(_gui_show_workspace){
      _gui_frame_trace(gui_trace, "workspace")
      _draw_gui_workspace()
   }
   if(_gui_show_graph){
      _gui_frame_trace(gui_trace, "graph")
      _draw_gui_graph()
   }
   if(_gui_show_inspector){
      _gui_frame_trace(gui_trace, "inspector")
      _draw_gui_inspector()
   }
   if(_gui_show_gallery){
      _gui_frame_trace(gui_trace, "gallery")
      _draw_gui_gallery(phase)
   }
   if(_gui_show_probe){
      _gui_frame_trace(gui_trace, "probe")
      _draw_gui_probe()
   }
   if(_gui_show_browser && !_gui_show_editor){
      _gui_frame_trace(gui_trace, "browser")
      _draw_gui_asset_browser()
   }
   _gui_frame_trace(gui_trace, "end_frame")
   gui.end_frame()
   if(parity_editor_compact){
      def rs_after = renderer_frame_stats()
      def sig =
      ui_app.app_renderer_stats_line(rs_before, true, true) + "|" +
      ui_app.app_renderer_stats_line(rs_after, true, true)
      if(sig != _last_editor_parity_sig){
         _last_editor_parity_sig = sig
         def _discard_66 = ui_profile.print_text("[parity:editor] shot=editor_scene_compact begin " +
         ui_app.app_renderer_stats_line(rs_before, true, true))
         def _discard_67 = ui_profile.print_text("[parity:editor] shot=editor_scene_compact end   " +
         ui_app.app_renderer_stats_line(rs_after, true, true))
      }
   }
   if(gui_trace){
      def gui_vo1 = _render_vertex_offset()
      _gui_frame_trace(gui_trace, "verts_added=" + to_str(gui_vo1 - gui_vo0))
   }
   return
}

;; ---- idle ----
fn _idle_opts(gui_now_frame, want_auto_capture=false) dict {
   def static_pose_ready = _scene_static_pose_gpu_ready() || _scene_deform_idle_ready()
   def any: opts = {
      "enabled": true,
      "warmup": ui_profile.env_int_cached("NY_UI_GUI_IDLE_REUSE_WARMUP", 2, 2, 128),
      "redraw_interval": ui_profile.env_int_cached("NY_UI_GUI_IDLE_REUSE_REDRAW_INTERVAL", 120, 0, 1000000),
      "trace": ui_profile.env_truthy_cached("NY_UI_GUI_IDLE_REUSE_TRACE"),
      "gui_frame": bool(gui_now_frame),
      "gui_visible": _gui_visible,
      "terminal_open": _term_open,
      "first_frame_done": _did_first_frame,
      "scene_active": _ui_scene_visible(),
      "bench_active": ui_profile.ui_bench_enabled(),
      "proof_dump": _proof_dump_active(),
      "capture_request": gfx.renderer_capture_requested(),
      "pending_capture": _pending_auto_dump || bool(want_auto_capture),
      "auto_capture": _auto_dump_enabled == 1,
      "dump_suite": _gui_dump_suite_active_now(),
      "batch_dump": _batch_dump_enabled(),
      "layout_dirty": _gui_layout_dirty,
      "layout_warm_frames": _gui_layout_warm_frames,
      "projection_dirty": _proj_dirty,
      "async_load": _scene_load_async_active(),
      "startup_load": _startup_post_load_pending,
      "dynamic_active": _gui_show_gallery || _gui_show_probe || _gui_show_profiler,
      "animated": _anim_enabled,
      "static_pose_ready": static_pose_ready,
      "animation_count": static_pose_ready ? 0 : _anim_count,
      "skin_count": static_pose_ready ? 0 : _skin_count,
      "morph_target_count": static_pose_ready ? 0 : _morph_target_count,
      "input_active": _ui_move_input_active() || skip_mouse_frames > 0 || gui.active_id() != "",
      "scroll_dx": _spdx,
      "scroll_dy": _spdy,
      "scroll_z": _spd_z,
      "mouse_dx": _mouse_dx_acc,
      "mouse_dy": _mouse_dy_acc,
      "win_w": int(_win_w),
      "win_h": int(_win_h)
   }
   opts
}

fn note_full_draw(gui_now_frame, want_auto_capture=false) {
   ui_idle.note_full_draw(_idle_opts(gui_now_frame, want_auto_capture))
}

fn try_present(gui_now_frame, want_auto_capture=false) bool {
   if(!ui_idle.try_present(_idle_opts(gui_now_frame, want_auto_capture))){ return false }
   _last_world_ms = 0.0
   _last_ui_ms = 0.0
   true
}

;; ---- loop ----
mut int: _first_frame_begin_fail_count = 0
mut bool: _first_frame_begin_fail_reported = false
mut bool: _app_prep_gui = false
mut bool: _app_gui_nav_blocking = false
mut str: _main_loop_gui_dump_path = ""
mut bool: _main_loop_want_auto_capture = false
mut bool: _main_loop_force_full_render = false
mut bool: _main_loop_rendered = false
mut int: _main_loop_draw_t0 = 0
mut int: _main_loop_draw_t1 = 0
mut bool: _middle_mouse_active = false
mut int: _middle_mouse_suppress_scroll_until_ns = 0
mut int: _mouse_delta_suppress_until_ns = 0
mut int: _mouse_look_trace_count = 0
mut int: _mouse_look_raw_until_ns = 0
mut int: _mouse_look_last_event_ns = 0
mut int: _mouse_look_last_frame = -1
mut str: _mouse_look_last_source = ""
mut f64: _camera_sim_dt_smooth = 0.0
def int: _MOUSE_LEFT = 0
def int: _MOUSE_RIGHT = 1
def int: _MOUSE_MIDDLE = 2
def int: _MIDDLE_SCROLL_SUPPRESS_NS = 180000000
def int: _CURSOR_TRANSITION_SUPPRESS_NS = 16000000

fn _camera_sim_dt(any dt) f64 {
   mut raw = float(dt)
   if(raw <= 0.0){ raw = 0.0001 }
   def max_ms = ui_profile.env_int_cached("NY_UI_CAMERA_DT_MAX_MS", 33, 8, 100)
   def min_ms = ui_profile.env_int_cached("NY_UI_CAMERA_DT_MIN_MS", 1, 0, 16)
   mut lo = float(min_ms) / 1000.0
   mut hi = float(max_ms) / 1000.0
   if(lo < 0.0001){ lo = 0.0001 }
   if(hi < lo){ hi = lo }
   raw = clamp(raw, lo, hi)
   def smooth_pct = ui_profile.env_int_cached("NY_UI_CAMERA_DT_SMOOTH_PCT", 35, 0, 100)
   if(smooth_pct <= 0){
      _camera_sim_dt_smooth = raw
      return raw
   }
   if(_camera_sim_dt_smooth <= 0.0){
      _camera_sim_dt_smooth = raw
      return raw
   }
   def alpha = clamp(float(smooth_pct) / 100.0, 0.0, 1.0)
   _camera_sim_dt_smooth = _camera_sim_dt_smooth + (raw - _camera_sim_dt_smooth) * alpha
   _camera_sim_dt_smooth
}

fn _app_input_trace(str msg) bool {
   if(!ui_profile.env_truthy_cached("NY_UI_INPUT_TRACE")){ return false }
   def limit = ui_profile.env_int_cached("NY_UI_APP_INPUT_TRACE_LIMIT", 80, 0, 1000000)
   if(_mouse_look_trace_count >= limit){ return false }
   _mouse_look_trace_count += 1
   ui_profile.eprint_text("[ui:input] " + msg)
}

fn _ui_begin_hotkey_text_suppress() {
   _hotkey_text_suppress_until_ns = ticks() + 120000000
}

fn _ui_consume_hotkey_text_spill(typ) {
   if(typ != EVENT_KEY_CHAR){ return false }
   if(_hotkey_text_suppress_until_ns <= 0){ return false }
   def now = ticks()
   if(now <= _hotkey_text_suppress_until_ns){
      _hotkey_text_suppress_until_ns = 0
      _dbg_ui("[ui:key] consumed hotkey text spill")
      return true
   }
   _hotkey_text_suppress_until_ns = 0
   false
}

fn _app_poll_f1_hotkey() bool {
   def f1_down = key_down(win, uin.KEY_F1)
   if(_hotkey_f1_down){
      if(!f1_down){ _hotkey_f1_down = false }
      return f1_down
   }
   if(f1_down && !_hotkey_f1_down){
      _hotkey_f1_down = true
      def _discard_f1 = _hotkey_toggle_editor()
      _suppress_mouse_deltas(_CURSOR_TRANSITION_SUPPRESS_NS)
      return true
   }
   false
}

fn _ui_handle_menu_hotkey(typ, data, fast=false) bool {
   _ui_set_update_stage("hotkey.menu.start")
   if(typ != EVENT_KEY_PRESSED && typ != EVENT_KEY_RELEASED){ return false }
   _ui_set_update_stage("hotkey.menu.data")
   if(!is_dict(data)){ return false }
   _ui_set_update_stage("hotkey.menu.key")
   def fn_key = uin.event_function_key(data)
   def k = fn_key > 0 ? fn_key : uin.event_key(data)
   if(k == 0){ return false }
   def is_f1 = k == uin.KEY_F1
   _ui_set_update_stage("hotkey.menu.range")
   if(!uin.is_function_key(k)){ return false }
   _ui_set_update_stage("hotkey.menu.release")
   if(typ == EVENT_KEY_RELEASED){
      if(is_f1){ _hotkey_f1_down = false }
      return viewer_keyboard.is_viewer_menu_hotkey(k)
   }
   _ui_set_update_stage("hotkey.menu.suppress")
   _ui_begin_hotkey_text_suppress()
   _ui_set_update_stage("hotkey.menu.action")
   def action = int(data.get("action", 1))
   if(is_f1){
      if(action != 2 && !_hotkey_f1_down){
         _hotkey_f1_down = true
         _ui_set_update_stage("hotkey.menu.f1")
         _hotkey_toggle_editor()
         _suppress_mouse_deltas(_CURSOR_TRANSITION_SUPPRESS_NS)
      }
      return true
   }
   if(action == 2){ return false }
   case k {
      uin.KEY_F3 -> {
         _gui_visible = !_gui_visible
         _invalidate_chrome_frame(4)
         if(_gui_visible){
            _clear_camera_input_state()
            gui.suppress_mouse_clicks(4, true)
         }
         _sync_cursor_state("F3 gui toggle")
         terminal.log("GUI: " + (_gui_visible ? "ON" : "OFF"))
         true
      }
      uin.KEY_F4 -> {
         if(fast){ return false }
         _gui_probe_overlay = !_gui_probe_overlay
         terminal.log("GUI debug overlay: " + (_gui_probe_overlay ? "ON" : "OFF"))
         true
      }
      uin.KEY_F5 -> {
         _hotkey_frame_dump()
         true
      }
      uin.KEY_F6 -> {
         if(fast){ return false }
         _gui_last_probe_text = ui_diag.probe_text()
         ui_diag.print_probe()
         terminal.log(_gui_last_probe_text)
         true
      }
      uin.KEY_F7 -> {
         if(fast){ return false }
         _gui_layout_dirty = true
         if(ui_profile.env_truthy_cached("NY_UI_HOTKEY_TRACE")){
            terminal.log("GUI layout retile requested")
         }
         true
      }
      _ -> false
   }
}

fn _ui_set_update_stage(stage) {
   _ui_update_stage = to_str(stage)
}

fn _first_frame_trace_allowed() bool {
   total_frames == 0 && _first_frame_begin_fail_count < 4
}

fn _note_first_frame_render_result(bool rendered) {
   if(rendered || total_frames > 0){
      _first_frame_begin_fail_count = 0
      return
   }
   _first_frame_begin_fail_count += 1
   def limit = ui_profile.env_int_cached("NY_UI_BEGIN_FRAME_FAIL_LIMIT", 24, 1, 1000000)
   if(_first_frame_begin_fail_count >= limit){
      if(!_first_frame_begin_fail_reported){
         _first_frame_begin_fail_reported = true
         ui_profile.print_text("[ui:render] begin_frame failed before first frame count=" +
            to_str(_first_frame_begin_fail_count) +
         "; closing")
      }
      set_should_close(win, true)
   }
   return
}

fn _startup_config_env() {
   _ui_apply_cli_options()
   _ui_debug_enabled = (ui_profile.env_truthy_cached("NY_UI_DEBUG") || _startup_trace_enabled()) ? 1 : 0
   APP_STATS = ui_profile.env_toggle_cached("NY_UI_STATS", APP_STATS)
   if(!ui_profile.env_present_cached("NY_UI_STATS") && ui_profile.env_truthy_cached("NY_UI_STATS_AUTO") &&
      !ui_profile.frame_hash_lock_enabled() && !ui_profile.env_truthy_cached("NYTRIX_AUTO_DUMP") &&
      !ui_profile.headless_enabled() && !ui_profile.ui_bench_enabled()){
      APP_STATS = true
   }
   if(ui_profile.ui_bench_enabled()){
      APP_STATS = false
      _gui_visible = false
      _gui_show_editor = false
      _gui_show_gallery = false
      _gui_show_probe = false
      _gui_show_browser = false
      _gui_show_profiler = false
      _gui_show_workspace = false
      _gui_show_graph = false
      _gui_show_inspector = false
      skybox_enabled = false
   }
   _batch_dump_parse_env()
   _gui_dump_suite_parse_env()
   _fps_log_enabled = ui_profile.env_truthy_cached("NY_UI_FPS_LOG") ? 1 : 0
   if(ui_profile.headless_sim_enabled() && ui_profile.ui_bench_enabled()){
      _fps_log_enabled = 0
   }
   _auto_dump_enabled = ui_profile.env_truthy_cached("NYTRIX_AUTO_DUMP") ? 1 : 0
   _auto_dump_exit_mode = ui_profile.env_truthy_cached("NYTRIX_AUTO_DUMP_EXIT") ? 1 : 0
   _auto_dump_immediate_mode = ui_profile.env_truthy_cached("NYTRIX_AUTO_DUMP_IMMEDIATE") ? 1 : 0
   if(_cli_dump_requested){
      _auto_dump_enabled = 1
      if(!ui_profile.frame_hash_lock_enabled()){
         _auto_dump_immediate_mode = 1
      }
      _auto_dump_exit_mode = 1
   }
}

fn _startup_config_auto_dump() {
   def _dump_delay_env = ui_profile.env_trim_cached("NYTRIX_AUTO_DUMP_DELAY_FRAMES")
   def _dump_elapsed_env = ui_profile.env_trim_cached("NYTRIX_AUTO_DUMP_MIN_ELAPSED_SEC")
   def _dump_path_env = ui_profile.env_trim_cached("NYTRIX_AUTO_DUMP_PATH")
   if(_dump_path_env.len > 0){
      _auto_dump_path = _dump_path_env
   }
   if(_auto_dump_immediate_mode == 1){
      _auto_dump_delay_frames = 4
   }
   else { _auto_dump_delay_frames = 0 }
   if(_dump_delay_env.len > 0){
      def _dump_delay_n = int(str.atof(_dump_delay_env))
      if(_dump_delay_n >= 0){
         _auto_dump_delay_frames = _dump_delay_n
      }
   }
   if(_cli_dump_delay_frames >= 0){
      _auto_dump_delay_frames = _cli_dump_delay_frames
   }
   _auto_dump_min_elapsed_sec = 0.0
   if(_dump_elapsed_env.len > 0){
      def _dump_elapsed_v = float(str.atof(_dump_elapsed_env))
      if(_dump_elapsed_v > 0.0){
         _auto_dump_min_elapsed_sec = _dump_elapsed_v
      }
   }
   _auto_dump_frame_counter = 0
   if(_auto_dump_enabled == 1){
      ui_profile.print_line(
         "ui:dump:init",
         "delay_frames=" + to_str(_auto_dump_delay_frames) +
         " min_elapsed=" + to_str(_auto_dump_min_elapsed_sec) +
         " immediate=" + to_str(_auto_dump_immediate_mode) +
         " path=" + _auto_dump_path
      )
   }
}

fn _startup_editor_default_enabled() bool {
   if(ui_profile.env_present_cached("NY_UI_START_EDITOR")){
      return ui_profile.env_toggle_cached("NY_UI_START_EDITOR", false)
   }
   if(!_chrome_visible() || _batch_dump_enabled() || ui_profile.ui_bench_enabled()){
      return false
   }
   true
}

fn _startup_open_editor_default() bool {
   if(!_startup_editor_default_enabled() || (_gui_visible && _gui_show_editor)){
      return false
   }
   if(show_scene || is_dict(active_scene) || _loaded_scene_name.len > 0){
      return false
   }
   _gui_editor_tab = 3
   _gui_open_default_editor()
   _invalidate_chrome_frame(2)
   if(!ui_profile.headless_enabled()){
      show_centered_cursor(win)
   }
   true
}

fn _startup_init_window_runtime() {
   _dbg_ui("[ui] startup: begin")
   mut stage_t0 = ticks()
   _setup_window()
   _startup_trace("window", stage_t0)
   if(_chrome_visible()){
      _dbg_ui("[ui] startup: init fonts")
      stage_t0 = ticks()
      _init_fonts()
      _startup_trace("fonts", stage_t0)
      stage_t0 = ticks()
      terminal.init(res_font, HUD_BG_U32, WHITE_U32)
      _startup_trace("terminal_init", stage_t0)
   } else {
      res_font = 0
      res_font_ui = 0
      res_font_title = 0
      res_font_small = 0
   }
   _term_open = terminal.is_open()
   _dbg_ui("[ui] startup: warm mesh helpers")
   stage_t0 = ticks()
   _warm_mesh_helpers()
   _startup_trace("warm_mesh_helpers", stage_t0)
   _dbg_ui("[ui] startup: setup camera")
   stage_t0 = ticks()
   _setup_camera()
   _startup_trace("camera", stage_t0)
   stage_t0 = ticks()
   _warm_gui_text_renderer()
   _startup_trace("text_renderer", stage_t0)
   stage_t0 = ticks()
   def _discard_icon_warm = _warm_gui_editor_resources()
   _startup_trace("editor_resources", stage_t0)
}

fn _warm_gui_text_renderer() bool {
   if(!_chrome_visible() || !win || !ui_profile.env_toggle_cached("NY_UI_WARM_TEXT_RENDERER", true)){
      return false
   }
   if(!gfx.begin_frame()){
      return false
   }
   gfx.set_ortho_2d(0, _win_w, _win_h, 0)
   _render_reset_overlay_state()
   gfx.set_unlit(true)
   gfx.set_model_matrix(M_UI_OVERLAY)
   gui.warm_text_pipeline(res_font_ui, res_font_title, res_font_small)
   def ok = gfx.end_frame()
   poll_events()
   ok
}

fn _warm_gui_editor_resources() bool {
   if(!_chrome_visible()){
      return false
   }
   def icon_names = [
      "scene_data", "preferences", "console", "info",
      "asset_loaded", "asset_model", "asset_texture"
   ]
   viewer_icons.warm_sprites(icon_names) >= 0
}

fn _startup_init_timers_and_skybox() {
   start_t = ticks()
   last_upd_t = start_t
   last_fps_t = start_t
   _set_timeout()
   if(_startup_trace_enabled()){
      terminal.log("[startup] terminal ready")
   }
   skybox_enabled = ui_profile.visible_skybox_default()
   skybox_tex_id = -1
   if(skybox_enabled && ui_profile.startup_skybox_enabled() && !_batch_dump_enabled()){
      def sky_t0 = ticks()
      if(ui_assets.skybox_source_arg("").len > 0){
         _load_skybox(true)
      } else {
         _load_fast_generated_skybox(true)
      }
      _startup_skybox_pending = false
      if(_startup_trace_enabled()){
         terminal.log("[startup] skybox=" + to_str(ui_profile.elapsed_ms(sky_t0)) + "ms")
      }
   } else {
      _startup_skybox_pending = false
   }
   if(ui_profile.trace_enabled()){
      print_to_stdout = true
      def _discard_10 = ui_profile.print_text("[profile] NY_UI_PROFILE_TRACE=1 every=" + to_str(ui_profile.frame_print_every()) + " frames")
      if(ui_profile.profile_dump_enabled(ui_profile.trace_enabled())){
         def _discard_11 = ui_profile.print_text("[profile] ui_jsonl=" + ui_profile.profile_dump_file())
      }
   }
}

fn _startup_exec_commands_and_scene() {
   _dbg_ui("[ui] startup: exec startup cmds")
   mut stage_t0 = ticks()
   _startup_exec_env_cmds()
   _startup_trace("env_cmds", stage_t0)
   stage_t0 = ticks()
   _startup_exec_cli_cmds()
   _startup_trace("cli_cmds", stage_t0)
   if(_cli_scene_requested && _cli_post_load_cmds.len == 0 && !ui_profile.env_present_cached("NY_UI_POST_LOAD_CMD")){
      _cli_post_load_cmds = _cli_post_load_cmds.append("autofit")
      _cli_post_load_cmds = _cli_post_load_cmds.append("lookat")
   }
   mut boot_scene_env = ui_profile.env_trim_cached("NY_UI_BOOT_SCENE")
   if(boot_scene_env.len == 0){
      boot_scene_env = ui_profile.env_trim_cached("NYTRIX_DEFAULT_SCENE")
   }
   if(!show_scene && boot_scene_env.len > 0){
      _loaded_scene_name = boot_scene_env
      show_scene = _loaded_scene_name.len > 0
      _clear_anim_state()
      if(show_scene){
         def _discard_12 = _dbg_ui("[ui] startup env load: " + _loaded_scene_name)
      }
   }
   if(!show_scene && argc() > 2 && eq(str.lower(to_str(argv(1))), "load")){
      _loaded_scene_name = str.strip(to_str(argv(2)))
      show_scene = _loaded_scene_name.len > 0
      _clear_anim_state()
      _cli_scene_requested = true
      _cli_scripted_scene_load = true
      def _discard_13 = _dbg_ui("[ui] startup argv load: " + _loaded_scene_name)
   }
   if(!ui_profile.headless_enabled() && ui_profile.env_truthy_cached("NY_UI_STARTUP_SLEEP")){
      msleep(20)
   }
   if(_batch_dump_enabled()){
      stage_t0 = ticks()
      if(!_batch_dump_begin_current()){
         def _discard_14 = _batch_dump_advance_or_exit()
      }
      _startup_trace("batch_scene_load", stage_t0)
   }
   elif(show_scene && !is_dict(active_scene)){
      stage_t0 = ticks()
      def startup_scene_name = _loaded_scene_name
      _startup_post_load_pending = true
      def show_startup_loading = !ui_profile.fast_nosurface_bench_enabled(_timeout_ns, _auto_dump_enabled, _batch_dump_enabled()) && !ui_profile.sim_nosurface_bench_enabled(_timeout_ns, _auto_dump_enabled, _batch_dump_enabled())
      if(_load_scene_runtime_sync(startup_scene_name, true, show_startup_loading)){
         _startup_post_load_pending = false
         _startup_exec_post_load_cmds()
         def scene_stage = show_startup_loading ? "scene_load_sync_startup" : "scene_load_sync_bench"
         def scene_ms = _startup_trace(scene_stage, stage_t0)
         if(_startup_trace_enabled()){
            terminal.log(_loaded_scene_name + " loaded in " + to_str(scene_ms) + "ms")
         }
      }
      else {
         _startup_post_load_pending = false
         if(_startup_trace_enabled()){
            terminal.log("Scene load failed(non-fatal)")
         }
      }
   }
}

fn _startup_finalize_gui_assets(startup_t0) {
   if(!_gui_dump_suite_begin()){
      _gui_apply_probe_preset()
      _apply_selection_overlay_probe_env()
      _startup_open_editor_default()
   }
   mut stage_t0 = 0
   if(_chrome_visible()){
      _dbg_ui("[ui] startup: build axes")
      stage_t0 = ticks()
      def axes_res = mesh_build_axes(18.0, 0.08)
      def axes_ptr = axes_res.get("ptr", 0)
      def int: axes_cnt = int(axes_res.get("cnt", 0))
      mut axes_opts = dict(4)
      axes_opts["no_cull"] = true
      axes_opts["unlit"] = true
      axes_opts["vc_mode"] = 1
      axes_opts["storage"] = "cpu"
      if(axes_ptr && axes_cnt > 0){
         _axes_mesh = mesh_create_cpu(axes_ptr, axes_cnt, false, axes_opts)
      }
      _startup_trace("axes", stage_t0)
   }
   _dbg_ui("[ui] startup: finalize")
   stage_t0 = ticks()
   viewer_overlay.init()
   if(_chrome_visible() && ui_profile.crosshair_enabled()){
      def _cross_pair = ui_runtime.build_crosshair_mesh()
      _crosshair_buf, _crosshair_mesh = _cross_pair.get(0, 0), _cross_pair.get(1, 0)
   }
   _intended_cursor_mode = _desired_cursor_mode()
   _proj_dirty = true
   set_clear_color(APP_BG)
   _sync_cursor_state("startup")
   _startup_trace("finalize", stage_t0)
   _startup_trace("total", startup_t0)
}

fn startup(any app=0) {
   "Prepare the demo UI runtime before the frame loop starts."
   def startup_t0 = ticks()
   _startup_config_env()
   _startup_config_auto_dump()
   _startup_init_window_runtime()
   _startup_init_timers_and_skybox()
   _startup_exec_commands_and_scene()
   _startup_finalize_gui_assets(startup_t0)
}

fn _batch_dump_update_anim_fast(dt) {
   if(_anim_enabled && is_dict(active_scene) && _anim_count > 0 && is_dict(_anim_gltf_data)){
      mut frame_dt = float(dt)
      if(is_nan(frame_dt) || is_inf(frame_dt) || frame_dt <= 0.0){ frame_dt = 0.0166667 }
      if(frame_dt > 0.1){ frame_dt = 0.0166667 }
      if(_anim_duration <= 0.0001){
         _anim_duration = float(active_scene.get("anim_duration", 0.0))
      }
      _anim_time += frame_dt * _anim_speed
      if(_anim_duration > 0.0001){
         while(_anim_time >= _anim_duration){ _anim_time -= _anim_duration }
         while(_anim_time < 0.0){ _anim_time += _anim_duration }
      }
      active_scene["anim_time"] = _anim_time
      active_scene["anim_idx"] = _anim_idx
      active_scene["anim_duration"] = _anim_duration
      active_scene["anim_time_override"] = true
      active_scene["anim_playing"] = true
      active_scene["static_pose_gpu_ready"] = false
      active_scene["parts_model_baked"] = false
      active_scene["gpu_model_baked"] = false
      active_scene = scene_engine.apply_gltf_animation(active_scene, _anim_idx, _anim_time)
      scene_engine.scene_fast_reset(active_scene)
      _static_world_color_reuse_reset()
      _static_world_redraw(1)
   } elif(!is_dict(active_scene) || _anim_count <= 0 || !is_dict(_anim_gltf_data)){
      _anim_enabled = false
      if(is_dict(active_scene)){ active_scene["anim_playing"] = false }
   } elif(is_dict(active_scene)){
      active_scene["anim_playing"] = false
   }
}

fn _handle_window_resize_event(data, layout_dirty=false, reset_cross=false, recenter_locked=false) {
   def extent = uin.resize_event_extent(id(win), data, _win_w, _win_h)
   _win_w = float(extent.get(0, _win_w))
   _win_h = float(extent.get(1, _win_h))
   _proj_dirty = true
   _static_world_redraw(3)
   if(layout_dirty){
      _gui_layout_dirty = true
      _gui_layout_warm_frames = 3
   }
   if(reset_cross){
      _cross_cx_last, _cross_cy_last = -9e9, -9e9
   }
   set_win_size(int(_win_w), int(_win_h))
   if(recenter_locked && !ui_profile.headless_enabled() && !_term_open && !_scene_loading_input_guard_active() && _intended_cursor_mode == CURSOR_DISABLED){
      center_cursor(win)
   }
}

fn _app_next_event() {
   "Reads the next input event, allowing the window layer to poll native fallback paths."
   check_event(win)
}

fn _batch_dump_update_fast(dt) {
   mut e = _app_next_event()
   while(e != 0){
      def typ = event_type(e)
      def data = event_data(e)
      if(typ == EVENT_WINDOW_RESIZED){
         _handle_window_resize_event(data)
      }
      if(quit(e)){
         set_should_close(win, true)
      }
      e = _app_next_event()
   }
   gui.set_enabled(false)
   _mouse_dx_acc, _mouse_dy_acc = 0.0, 0.0
   _last_evt_ms = 0.0
   _last_gui_prep_ms = 0.0
   _ui_update_projection_fast()
   _batch_dump_update_anim_fast(dt)
   _last_sim_ms = 0.0
}

fn _app_simulate_camera_frame(dt, bool prep_gui, bool gui_nav_blocking) {
   def _discard_f1_poll = _app_poll_f1_hotkey()
   if(_scene_loading_input_guard_active()){
      _release_scene_loading_input("scene load camera block")
      return
   }
   def live_win = get_win(win)
   mut live_keys = dict(8)
   mut live_mods = 0
   if(is_dict(live_win)){
      def raw_keys = live_win.get("key_states", 0)
      if(is_dict(raw_keys)){ live_keys = raw_keys }
      live_mods = int(live_win.get("modifiers", 0))
   }
   def skip_look = skip_mouse_frames > 0
   if(skip_look){ skip_mouse_frames -= 1 }
   mut dx, dy = _mouse_dx_acc, _mouse_dy_acc
   _mouse_dx_acc, _mouse_dy_acc = 0.0, 0.0
   def dragging_scene = _scene_drag_active
   if(dragging_scene){
      dx = 0.0
      dy = 0.0
   }
   mut overlay_mouse = false
   if(prep_gui && _scene_selected && _scene_selection_rect){
      def cur = _cursor_xy_view()
      overlay_mouse = _selection_overlay_hit_test(float(cur.get(0, 0.0)), float(cur.get(1, 0.0)))
   }
   def editor_nav = _rmb_look_active && !_term_open
   def focus_mouse_look = _focused_scene_look_active()
   def nav_keys = !dragging_scene && _camera_keyboard_nav_allowed(prep_gui, gui_nav_blocking)
   if(prep_gui && !nav_keys && _ui_move_input_active()){
      _clear_camera_input_state()
   }
   def gui_mouse = dragging_scene || (!editor_nav && (prep_gui || overlay_mouse))
   def key_w = nav_keys && (_move_w || live_keys.get(uin.KEY_W, false))
   def key_s = nav_keys && (_move_s || live_keys.get(uin.KEY_S, false))
   def key_a = nav_keys && (_move_a || live_keys.get(uin.KEY_A, false))
   def key_d = nav_keys && (_move_d || live_keys.get(uin.KEY_D, false))
   def key_space = nav_keys && (_move_space || live_keys.get(uin.KEY_SPACE, false))
   def key_ctrl = nav_keys && (_move_ctrl || ((live_mods & MOD_CONTROL) != 0) ||
   live_keys.get(uin.KEY_CTRL, false) || live_keys.get(uin.KEY_LEFT_CONTROL, false) || live_keys.get(uin.KEY_RIGHT_CONTROL, false))
   def key_shift = nav_keys && (_move_shift || ((live_mods & MOD_SHIFT) != 0) ||
   live_keys.get(uin.KEY_SHIFT, false) || live_keys.get(uin.KEY_LEFT_SHIFT, false) || live_keys.get(uin.KEY_RIGHT_SHIFT, false))
   def sim_dt = _camera_sim_dt(dt)
   def any: sim_state = {
      "dt": sim_dt, "dx": dx, "dy": dy, "skip_look": skip_look, "prep_gui": prep_gui, "gui_mouse": gui_mouse,
      "rmb_look_active": _rmb_look_active, "cursor_lock": _cursor_lock_enabled, "focus_mouse_look": focus_mouse_look,
      "yaw": _h_yaw, "pitch": _h_pch, "target_yaw": _target_yaw, "target_pitch": _target_pch,
      "sens": _sens, "rmb_sens_mul": _rmb_look_sens_mul, "pitch_min": _p_min, "pitch_max": _p_max,
      "rmb_dx_smooth": _rmb_dx_smooth, "rmb_dy_smooth": _rmb_dy_smooth,
      "look_smooth_alpha": float(ui_profile.env_int_cached("NY_UI_MOUSE_LOOK_SMOOTH_PCT", 100, 0, 100)) / 100.0,
      "key_w": key_w, "key_s": key_s, "key_a": key_a, "key_d": key_d,
      "key_space": key_space, "key_ctrl": key_ctrl, "key_shift": key_shift,
      "spdx": _spdx, "spdy": _spdy, "spdz": _spd_z, "vx": _vx, "vy": _vy, "vz": _vz,
      "speed": _spd, "speed_mul": _spdmul, "damp": _damp, "drag": _drag,
      "cam_x": _cam_px, "cam_y": _cam_py, "cam_z": _cam_pz
   }
   def sim = camera.simulate_frame_state(sim_state)
   _h_yaw, _h_pch = float(sim.get("yaw", _h_yaw)), float(sim.get("pitch", _h_pch))
   _target_yaw, _target_pch = float(sim.get("target_yaw", _target_yaw)), float(sim.get("target_pitch", _target_pch))
   _rmb_dx_smooth, _rmb_dy_smooth = float(sim.get("rmb_dx_smooth", _rmb_dx_smooth)), float(sim.get("rmb_dy_smooth", _rmb_dy_smooth))
   _spdx, _spdy, _spd_z = float(sim.get("spdx", _spdx)), float(sim.get("spdy", _spdy)), float(sim.get("spdz", _spd_z))
   _vx, _vy, _vz = float(sim.get("vx", _vx)), float(sim.get("vy", _vy)), float(sim.get("vz", _vz))
   _cam_px, _cam_py, _cam_pz = float(sim.get("cam_x", _cam_px)), float(sim.get("cam_y", _cam_py)), float(sim.get("cam_z", _cam_pz))
   if(bool(sim.get("look_applied", false))){
      def _discard_input_trace = _app_input_trace(
         "look apply dx=" + to_str(sim.get("look_dx", 0.0)) +
         " dy=" + to_str(sim.get("look_dy", 0.0)) +
         " yaw=" + to_str(_h_yaw) +
         " pitch=" + to_str(_h_pch) +
         " rmb=" + to_str(_rmb_look_active) +
         " lock=" + to_str(_cursor_lock_enabled) +
         " focus=" + to_str(focus_mouse_look) +
      " gui=" + to_str(prep_gui))
   }
}

fn _app_update_fast_path(dt) bool {
   if(_batch_dump_enabled() && ui_profile.batch_fast_env_enabled()){
      _cam_fov = camthreed.get(16)
      _batch_dump_update_fast(dt)
      return true
   }
   false
}

fn _app_run_bench_sim(dt, trace, deep_trace) {
   gui.set_enabled(false)
   _last_evt_ms = 0.0
   _last_gui_prep_ms = 0.0
   def t_sim0 = (trace && deep_trace) ? ticks() : 0
   _ui_update_projection_fast()
   _batch_dump_update_anim_fast(dt)
   _last_sim_ms = (trace && deep_trace) ? ui_profile.elapsed_ms(t_sim0) : 0.0
}

fn _app_pump_bench_events() {
   mut e = _app_next_event()
   _cam_fov = camthreed.get(16)
   while(e != 0){
      def typ = event_type(e)
      def data = event_data(e)
      if(typ == EVENT_WINDOW_RESIZED){
         _handle_window_resize_event(data, false, true)
      }
      if(quit(e)){
         set_should_close(win, true)
      }
      e = _app_next_event()
   }
}

fn _app_update_bench_frame(dt, trace, deep_trace, t_evt0) bool {
   if(!ui_profile.ui_bench_enabled()){
      return false
   }
   if(ui_profile.sim_nosurface_bench_enabled(_timeout_ns, _auto_dump_enabled, _batch_dump_enabled())){
      _app_run_bench_sim(dt, trace, deep_trace)
      return true
   }
   _app_pump_bench_events()
   gui.set_enabled(false)
   _last_evt_ms = (trace && deep_trace) ? ui_profile.elapsed_ms(t_evt0) : 0.0
   _last_gui_prep_ms = 0.0
   def t_sim0 = (trace && deep_trace) ? ticks() : 0
   _ui_update_projection_fast()
   _batch_dump_update_anim_fast(dt)
   _last_sim_ms = (trace && deep_trace) ? ui_profile.elapsed_ms(t_sim0) : 0.0
   true
}

fn _app_set_move_key(k, sc, pressed) bool {
   if(k == uin.KEY_W){ _move_w = pressed return true }
   if(k == uin.KEY_A){ _move_a = pressed return true }
   if(k == uin.KEY_S){ _move_s = pressed return true }
   if(k == uin.KEY_D){ _move_d = pressed return true }
   if(k == uin.KEY_SPACE){ _move_space = pressed return true }
   if(ui_app.app_key_is_shift(k, sc)){ _move_shift = pressed return true }
   if(ui_app.app_key_is_ctrl(k, sc)){ _move_ctrl = pressed return true }
   false
}

fn _app_submit_console_key(k, gui_block_keys) bool {
   if(_term_open || !gui_block_keys || gui.focused_id() != "editor_main::console_cmd"){
      return false
   }
   if(k != uin.KEY_ENTER && k != 257){
      return false
   }
   def cmd_line = str.strip(_gui_console_input)
   if(cmd_line.len > 0){
      terminal.log("> " + cmd_line)
      exec_cmd(cmd_line)
      _gui_console_input = ""
   }
   true
}

fn _app_handle_escape_key(k) bool {
   if(k != uin.KEY_ESCAPE && k != 256){
      return false
   }
   def _discard_drag = _scene_drag_end()
   if(_gui_editor_shell_open() || _scene_selected){
      if(_gui_editor_shell_open()){
         _gui_close_editor_shell()
         gui.clear_focus()
      }
      _scene_selected = false
      _invalidate_chrome_frame(3)
      _sync_cursor_state("escape clear ui")
   } else {
      set_should_close(win, true)
   }
   true
}

fn _app_handle_selected_gizmo_key(k, mods, gui_blocks_world) bool {
   if(_term_open || gui_blocks_world || !_scene_editor_tools_enabled() || !_scene_selected || (mods & MOD_CONTROL) != 0){
      return false
   }
   if(k == uin.KEY_1){
      _gizmo_mode = 0
      _gizmo_axis = 0
      terminal.log("Gizmo: Move")
      _scene_edit_redraw(2)
      return true
   }
   if(k == uin.KEY_2){
      _gizmo_mode = 1
      _gizmo_axis = 0
      terminal.log("Gizmo: Rotate")
      _scene_edit_redraw(2)
      return true
   }
   if(k == uin.KEY_3){
      _gizmo_mode = 2
      _gizmo_axis = 0
      terminal.log("Gizmo: Scale")
      _scene_edit_redraw(2)
      return true
   }
   if(k == uin.KEY_X || k == uin.KEY_Y || k == uin.KEY_Z){
      def next_axis = (k == uin.KEY_X) ? 1 : ((k == uin.KEY_Y) ? 2 : 3)
      _gizmo_axis = (_gizmo_axis == next_axis) ? 0 : next_axis
      terminal.log("Axis: " + ((_gizmo_axis == 1) ? "X" : ((_gizmo_axis == 2) ? "Y" : ((_gizmo_axis == 3) ? "Z" : "Free"))))
      _scene_edit_redraw(2)
      return true
   }
   false
}

fn _app_handle_focus_model_key(k, mods, gui_blocks_world) bool {
   if(_term_open || gui_blocks_world || (mods & MOD_CONTROL) != 0){
      return false
   }
   if(k != uin.KEY_F){
      return false
   }
   _cmd_lookat(true)
}

fn _app_focus_editor_tab(idx) {
   _gui_editor_tab = idx
   gui.focus_window("editor_main")
}

fn _app_handle_editor_shortcut(k, mods, gui_enabled_frame) bool {
   if(_term_open || !gui_enabled_frame){
      return false
   }
   def ctrl = (mods & MOD_CONTROL) != 0
   if(ctrl && (k == uin.KEY_P || k == uin.KEY_K || k == uin.KEY_F)){
      _gui_focus_asset_search()
      return true
   }
   if(ctrl && k >= uin.KEY_1 && k <= uin.KEY_4){
      _app_focus_editor_tab(k - uin.KEY_1)
      return true
   }
   if(!ctrl && k == uin.KEY_SLASH){
      _gui_focus_asset_search()
      return true
   }
   false
}

fn _app_block_scripted_model_cycle(k) bool {
   _cli_scripted_scene_load && _timeout_ns > 0 && (k == uin.KEY_PAGE_UP || k == uin.KEY_PAGE_DOWN)
}

fn _app_handle_model_cycle_key(k, gui_blocks_world) bool {
   if(_term_open || gui_blocks_world){
      return false
   }
   if(_app_block_scripted_model_cycle(k)){
      return true
   }
   if(k == uin.KEY_PAGE_UP){
      _cycle_loaded_model(-1)
      return true
   }
   if(k == uin.KEY_PAGE_DOWN){
      _cycle_loaded_model(1)
      return true
   }
   false
}

fn _app_handle_key_pressed(data, gui_blocks_world, gui_block_keys, gui_enabled_frame, gui_event_consumed=false) bool {
   def k = uin.event_key(data)
   def sc = data.get("scancode", data.get("raw_key", 0))
   def mods = ui_app.app_effective_mods(data.get("mod", 0), k, sc, _move_shift, _move_ctrl)
   if(_scene_loading_input_guard_active()){
      _release_scene_loading_input("scene load key block")
      if(gui_event_consumed){ return true }
      if(_app_handle_escape_key(k)){ return true }
      return false
   }
   if(_camera_keyboard_nav_allowed(gui_enabled_frame, gui_blocks_world)){
      def _discard_move = _app_set_move_key(k, sc, true)
   } elif(gui_enabled_frame){
      def _discard_blocked_move = _app_set_move_key(k, sc, false)
   }
   if(_app_submit_console_key(k, gui_block_keys)){ return true }
   if(gui_event_consumed){ return true }
   if(_app_handle_escape_key(k)){ return true }
   if(_app_handle_focus_model_key(k, mods, gui_blocks_world)){ return true }
   if(_app_handle_selected_gizmo_key(k, mods, gui_blocks_world)){ return true }
   if(_app_handle_editor_shortcut(k, mods, gui_enabled_frame)){ return true }
   _app_handle_model_cycle_key(k, gui_blocks_world)
}

fn _app_handle_key_released(data) {
   def k = uin.event_key(data)
   def sc = data.get("scancode", data.get("raw_key", 0))
   if(uin.event_is_key(data, uin.KEY_F1)){
      _hotkey_f1_down = false
   }
   if(_scene_loading_input_guard_active()){
      _release_scene_loading_input("scene load key release")
      return
   }
   def _discard_move = _app_set_move_key(k, sc, false)
}

fn _app_handle_mouse_pos(data, gui_on_event) {
   if(_scene_loading_input_guard_active()){
      _release_scene_loading_input("scene load mouse move")
      return
   }
   if(_scene_drag_active){
      ;; Gizmo dragging is screen-space/absolute.  Do not feed raw/relative
      ;; mouse events into it: on X11/Wayland those events can carry virtual
      ;; cursor coordinates from captured look, which makes the gizmo jump or
      ;; jitter when the editor (F1) is open.
      if(is_dict(data) && bool(data.get("relative", false))){
         def _discard_drag_rel_trace = _app_input_trace(
            "drag dropped relative-event x=" + to_str(data.get("x", 0.0)) +
            " y=" + to_str(data.get("y", 0.0)) +
            " dx=" + to_str(data.get("dx", 0.0)) +
            " dy=" + to_str(data.get("dy", 0.0)) +
            " raw=" + to_str(data.get("raw", false))
         )
         _clear_mouse_look_state()
         return
      }
      def pos = _event_mouse_xy_view(data)
      def _discard_drag = _scene_drag_update(float(pos.get(0, 0.0)), float(pos.get(1, 0.0)))
      _clear_mouse_look_state()
      return
   }
   if(skip_mouse_frames > 0 || ticks() < _mouse_delta_suppress_until_ns){
      _clear_mouse_look_state()
      return
   }
   def focus_mouse_look = _focused_scene_look_active()
   ;; During captured/RMB look, consume only relative motion.  Some backends
   ;; still emit absolute pointer-position events while the cursor is locked;
   ;; mixing those with raw relative events makes the camera alternate between
   ;; real deltas and warp/GUI-position deltas, which feels like Vulkan jitter.
   mut gui_blocks_look = false
   if(!_rmb_look_active){
      def mp_for_hit = _event_mouse_xy_view(data)
      gui_blocks_look = gui_on_event && gui.hit_test(float(mp_for_hit.get(0, 0.0)), float(mp_for_hit.get(1, 0.0)))
   }
   def viewport_free_look = (_cursor_lock_enabled || focus_mouse_look) && !gui_blocks_look
   def captured_look = _rmb_look_active || viewport_free_look
   if(!_term_open && captured_look){
      if(!is_dict(data) || !_mouse_look_accept_event(data, captured_look)){ return }
      def relative_ev = bool(data.get("relative", true))
      mut mdx, mdy = float(data.get("dx", 0.0)), float(data.get("dy", 0.0))
      def dead = float(ui_profile.env_int_cached("NY_UI_MOUSE_LOOK_DEADZONE_MILLI", 0, 0, 1000)) / 1000.0
      if(abs(mdx) <= dead){ mdx = 0.0 }
      if(abs(mdy) <= dead){ mdy = 0.0 }
      def max_delta = float(ui_profile.env_int_cached("NY_UI_MOUSE_LOOK_MAX_DELTA", 32, 4, 512))
      mdx, mdy = clamp(mdx, 0.0 - max_delta, max_delta), clamp(mdy, 0.0 - max_delta, max_delta)
      if(mdx == 0.0 && mdy == 0.0){ return }
      _mouse_dx_acc += mdx
      _mouse_dy_acc += mdy
      def _discard_input_trace = _app_input_trace(
         "look event dx=" + to_str(mdx) +
         " dy=" + to_str(mdy) +
         " acc=(" + to_str(_mouse_dx_acc) + "," + to_str(_mouse_dy_acc) + ")" +
         " rel=" + to_str(relative_ev) +
         " raw=" + to_str(bool(data.get("raw", false))) +
         " rmb=" + to_str(_rmb_look_active) +
         " lock=" + to_str(_cursor_lock_enabled) +
         " focus=" + to_str(focus_mouse_look) +
         " free=" + to_str(viewport_free_look) +
      " gui_event=" + to_str(gui_on_event))
   } elif(gui_on_event){
      def _discard_clear_mouse = _clear_mouse_look_state()
   }
   return
}

fn _middle_mouse_scroll_suppressed() bool {
   _middle_mouse_active || mouse_down(win, _MOUSE_MIDDLE) || ticks() < _middle_mouse_suppress_scroll_until_ns
}

fn _mouse_look_accept_event(dict data, bool captured_look) bool {
   if(!captured_look){ return false }
   def has_dx = data.contains("dx") || data.contains("dy")
   if(!has_dx){ return false }
   def now = ticks()
   def is_raw = bool(data.get("raw", false))
   def is_relative = bool(data.get("relative", is_raw))
   if(!is_relative){ return false }

   ;; Pick one mouse source per capture session.  X11/Wayland can emit both raw
   ;; relative events and cursor-warp fallback events around the same frame.  Even
   ;; if each event is valid alone, alternating sources makes yaw/pitch jitter.
   ;; Once raw motion is observed, ignore fallback motion briefly.  If raw never
   ;; arrives, fallback remains usable.
   if(is_raw){
      if(_mouse_look_last_source == "fallback" && total_frames == _mouse_look_last_frame){
         _mouse_dx_acc = 0.0
         _mouse_dy_acc = 0.0
      }
      _mouse_look_raw_until_ns = now + 250000000
      _mouse_look_last_source = "raw"
   } elif(now < _mouse_look_raw_until_ns){
      return false
   } else {
      _mouse_look_last_source = "fallback"
   }

   ;; Drop duplicate zero/near-zero transition noise from cursor mode changes.
   if(total_frames == _mouse_look_last_frame && now - _mouse_look_last_event_ns < 1000000){
      def dx = abs(float(data.get("dx", 0.0)))
      def dy = abs(float(data.get("dy", 0.0)))
      if(dx < 0.001 && dy < 0.001){ return false }
   }
   _mouse_look_last_frame = total_frames
   _mouse_look_last_event_ns = now
   true
}

fn _suppress_mouse_deltas(int ns) any {
   _mouse_delta_suppress_until_ns = ticks() + ns
   _clear_mouse_look_state()
}

fn _middle_mouse_start() bool {
   _middle_mouse_active = true
   _middle_mouse_suppress_scroll_until_ns = ticks() + _MIDDLE_SCROLL_SUPPRESS_NS
   _suppress_mouse_deltas(_CURSOR_TRANSITION_SUPPRESS_NS)
   if(_scene_drag_active){
      def _discard_drag = _scene_drag_end()
   }
   _rmb_look_active = false
   skip_mouse_frames = 2
   _clear_mouse_look_state()
   _sync_cursor_state("middle mouse guard start")
   true
}

fn _middle_mouse_stop() bool {
   if(!_middle_mouse_active){ return false }
   _middle_mouse_active = false
   _middle_mouse_suppress_scroll_until_ns = ticks() + _MIDDLE_SCROLL_SUPPRESS_NS
   _suppress_mouse_deltas(_CURSOR_TRANSITION_SUPPRESS_NS)
   skip_mouse_frames = 2
   _clear_mouse_look_state()
   _sync_cursor_state("middle mouse guard stop")
   true
}

fn _app_handle_mouse_button_pressed(data, gui_enabled_frame, gui_event_consumed) bool {
   if(!ui_profile.headless_enabled()){ focus(win) }
   def b = int(data.get("button", -1))
   if(_scene_loading_input_guard_active()){
      _release_scene_loading_input("scene load mouse press")
      return true
   }
   def pos = _event_mouse_xy_view(data)
   def mx = float(pos.get(0, 0.0))
   def my = float(pos.get(1, 0.0))
   mut gui_under_mouse = false
   if(gui_enabled_frame){
      gui_under_mouse = gui.hit_test(mx, my)
   }
   def gui_mouse_now = gui_enabled_frame &&
   (gui.wants_mouse() || gui_event_consumed || gui_under_mouse)
   def editor_scene_tools = _scene_editor_tools_enabled() && is_dict(active_scene) && show_scene
   ;; World gizmo pick must win before the selection overlay/GUI consumes the
   ;; click.  The Y handle often projects over the overlay region; checking the
   ;; overlay first makes the click change mode or free-drag instead of starting
   ;; a locked Y-axis drag.
   if(!_term_open && editor_scene_tools && b == _MOUSE_LEFT && _scene_selected && _scene_selection_rect){
      _ui_update_projection_fast()
      def pick = _scene_world_gizmo_pick(mx, my)
      if(bool(pick.get("hit", false))){
         def _discard_begin = _scene_drag_begin(mx, my, pick)
         skip_mouse_frames = _scene_drag_active ? 0 : 2
         _sync_cursor_state("scene gizmo drag")
         return true
      }
   }
   mut overlay_mouse = false
   if(!_term_open && b == _MOUSE_LEFT && _scene_selected && _scene_selection_rect){
      overlay_mouse = _selection_overlay_handle_click(mx, my)
   }
   if(overlay_mouse){
      _sync_cursor_state("selection overlay")
      return true
   }
   def gui_mouse = overlay_mouse || gui_mouse_now
   def gui_blocks_viewport_rmb = overlay_mouse || gui_under_mouse
   if(!_term_open && b == _MOUSE_MIDDLE){
      _middle_mouse_suppress_scroll_until_ns = ticks() + _MIDDLE_SCROLL_SUPPRESS_NS
      if(!gui_mouse && is_dict(active_scene) && show_scene){ return _middle_mouse_start() }
      return gui_mouse
   }
   if(!_term_open && !gui_blocks_viewport_rmb && b == _MOUSE_RIGHT && is_dict(active_scene) && show_scene){
      _rmb_look_active = true
      gui.clear_focus()
      skip_mouse_frames = 0
      _clear_camera_input_state()
      _mouse_look_raw_until_ns = 0
      _rmb_dx_smooth, _rmb_dy_smooth = 0.0, 0.0
      _sync_cursor_state("rmb look start")
      return true
   }
   if(!_term_open && !gui_mouse && editor_scene_tools && b == _MOUSE_LEFT && is_dict(active_scene) && show_scene){
      _scene_selected = true
      _scene_selection_rect = true
      _scene_selection_bounds_cache_update()
      _scene_edit_redraw(2)
      skip_mouse_frames = 2
      _sync_cursor_state("scene select")
      return true
   }
   false
}

fn _app_handle_mouse_button_released(data) {
   def b = int(data.get("button", -1))
   if(_scene_loading_input_guard_active()){
      _release_scene_loading_input("scene load mouse release")
      return
   }
   if(b == _MOUSE_MIDDLE){
      def _discard_middle = _middle_mouse_stop()
      return
   }
   if(b == _MOUSE_LEFT && _scene_drag_active){
      def _discard_drag = _scene_drag_end()
      skip_mouse_frames = 2
      _sync_cursor_state("scene drag stop")
      return
   }
   if(b == _MOUSE_RIGHT && _rmb_look_active){
      _rmb_look_active = false
      skip_mouse_frames = 0
      _clear_camera_input_state()
      _mouse_look_raw_until_ns = 0
      _rmb_dx_smooth, _rmb_dy_smooth = 0.0, 0.0
      _sync_cursor_state("rmb look stop")
   }
}

fn _app_release_pointer_ownership(reason="pointer release") {
   def _discard_drag = _scene_drag_end()
   _clear_camera_input_state()
   _rmb_look_active = false
   _middle_mouse_active = false
   _middle_mouse_suppress_scroll_until_ns = 0
   _mouse_delta_suppress_until_ns = ticks() + _CURSOR_TRANSITION_SUPPRESS_NS
   _hotkey_f1_down = false
   if(!_term_open && !ui_profile.headless_enabled()){
      _intended_cursor_mode = CURSOR_NORMAL
      set_cursor_mode(win, CURSOR_NORMAL)
   }
}

fn _app_handle_focus_out() {
   _app_release_pointer_ownership("focus out")
}

fn _app_handle_focus_in() {
   skip_mouse_frames = 3
   _suppress_mouse_deltas(_CURSOR_TRANSITION_SUPPRESS_NS)
   camera.reset_motion(camthreed)
   _sync_cursor_state("focus in")
}

fn _app_handle_mouse_leave() {
   _app_release_pointer_ownership("mouse leave")
}

fn _app_handle_mouse_enter() {
   skip_mouse_frames = 3
   _suppress_mouse_deltas(_CURSOR_TRANSITION_SUPPRESS_NS)
   if(!ui_profile.headless_enabled()){ focus(win) }
   _sync_cursor_state("mouse enter")
}

fn _app_handle_scroll(data, gui_enabled_frame, gui_event_consumed=false) {
   if(_scene_loading_input_guard_active()){
      _release_scene_loading_input("scene load scroll")
      return
   }
   if(_middle_mouse_scroll_suppressed()){
      return
   }
   if(_scene_drag_active){
      ;; A gizmo axis/translate drag is in progress.  Touchpads emit scroll
      ;; events alongside two-finger drags, and letting those change camera
      ;; FOV/zoom mid-drag shifts the view/projection used by the axis ray
      ;; solve, which looks like the Y-axis drag also "scrolling"/zooming.
      return
   }
   def dy = float(data.get("dy", 0.0))
   mut scroll_over_gui = gui_event_consumed
   if(gui_enabled_frame){
      def pos = _event_mouse_xy_view(data)
      scroll_over_gui = scroll_over_gui || gui.hit_test(float(pos.get(0, 0.0)), float(pos.get(1, 0.0)))
   }
   def camera_scroll = !_term_open && (_rmb_look_active || !gui_enabled_frame || !scroll_over_gui)
   if(camera_scroll && is_ortho){
      _ortho_zoom = clamp(_ortho_zoom - dy * 2.0, 5.0, 200.0)
      _proj_dirty = true
   }
   elif(camera_scroll){
      _cam_fov = clamp(_cam_fov - dy * 5.0, 15.0, 120.0)
      _proj_dirty = true
   }
}

fn _app_handle_terminal_event(typ, data) {
   if(!_term_open){
      return
   }
   _ui_set_update_stage("terminal.event")
   def int: res = int(terminal.handle_event(typ, data))
   if(res == 2){
      terminal.exec(exec_cmd)
   }
}

fn _app_gui_blocks_keys(gui_on_event, gui_event_consumed) bool {
   def editor_nav_event = _rmb_look_active && !_term_open
   !editor_nav_event &&
   gui_on_event &&
   (gui.wants_keyboard() || gui_event_consumed || gui.focused_id() != "")
}

fn _ui_trace_event(typ, data, tag="event") {
   ui_profile.event_trace(typ, data, tag, should_close(win))
}

fn _app_process_events() {
   _ui_set_update_stage("event.poll")
   mut e = _app_next_event()
   ui_idle.note_events_seen(e != 0)
   _cam_fov = camthreed.get(16)
   _ui_set_update_stage("gui.enable")
   mut gui_enabled_frame = _gui_enabled_now()
   gui.set_enabled(gui_enabled_frame)
   if(gui_enabled_frame){
      _ui_set_update_stage("gui.prepare.pre")
      gui.prepare_input(win, _win_w, _win_h)
   }
   while(e != 0){
      ui_idle.mark_event_seen()
      _ui_set_update_stage("event.type")
      def typ = event_type(e)
      _ui_set_update_stage("event.data")
      def data = event_data(e)
      _ui_trace_event(typ, data)
      _ui_set_update_stage("hotkey.text")
      if(_ui_consume_hotkey_text_spill(typ)){
         e = _app_next_event()
         continue
      }
      _ui_set_update_stage("hotkey.menu")
      if(_ui_handle_menu_hotkey(typ, data, false)){
         gui_enabled_frame = _gui_enabled_now()
         gui.set_enabled(gui_enabled_frame)
         e = _app_next_event()
         continue
      }
      mut gui_on_event = gui_enabled_frame
      ;; While a scene gizmo is being dragged, the GUI must not also consume
      ;; mouse motion/release events. The gizmo is absolute screen-space; letting
      ;; the editor panels process the same stream causes hover/focus/layout churn
      ;; and visible jitter.
      if(_scene_drag_active && (typ == EVENT_MOUSE_POS_CHANGED || typ == EVENT_MOUSE_BUTTON_RELEASED || typ == EVENT_MOUSE_SCROLL)){
         gui_on_event = false
      }
      _ui_set_update_stage("gui.feed")
      def gui_event_consumed = gui_on_event ? gui.feed_event(typ, data) : false
      def gui_block_keys = _app_gui_blocks_keys(gui_on_event, gui_event_consumed)
      def gui_blocks_world = gui_block_keys
      def consumed = case typ {
         EVENT_KEY_PRESSED -> _app_handle_key_pressed(data, gui_blocks_world, gui_block_keys, gui_enabled_frame, gui_event_consumed)
         EVENT_KEY_RELEASED -> { _app_handle_key_released(data) false }
         EVENT_MOUSE_POS_CHANGED -> { _app_handle_mouse_pos(data, gui_on_event) true }
         EVENT_MOUSE_BUTTON_PRESSED -> _app_handle_mouse_button_pressed(data, gui_enabled_frame, gui_event_consumed)
         EVENT_MOUSE_BUTTON_RELEASED -> { _app_handle_mouse_button_released(data) true }
         EVENT_MOUSE_LEAVE -> { _app_handle_mouse_leave() false }
         EVENT_MOUSE_ENTER -> { _app_handle_mouse_enter() false }
         EVENT_FOCUS_OUT -> { _app_handle_focus_out() false }
         EVENT_FOCUS_IN -> { _app_handle_focus_in() false }
         EVENT_WINDOW_RESIZED -> { _handle_window_resize_event(data, true, true, true) false }
         EVENT_MOUSE_SCROLL -> { _app_handle_scroll(data, gui_enabled_frame, gui_event_consumed) true }
         _ -> false
      }
      if(consumed){
         gui_enabled_frame = _gui_enabled_now()
         gui.set_enabled(gui_enabled_frame)
         e = _app_next_event()
         continue
      }
      _app_handle_terminal_event(typ, data)
      if(quit(e)){
         _ui_trace_event(typ, data, "event.quit")
         set_should_close(win, true)
      }
      _ui_set_update_stage("event.next")
      e = _app_next_event()
   }
   gui_enabled_frame
}

fn _app_prepare_gui_after_events(bench, gui_enabled_frame, trace, deep_trace) {
   def t_prep0 = (trace && deep_trace) ? ticks() : 0
   def prep_gui = !bench && gui_enabled_frame
   gui.set_enabled(prep_gui)
   mut gui_nav_blocking = false
   if(prep_gui){
      _ui_set_update_stage("gui.prepare.post")
      gui.prepare_input(win, _win_w, _win_h)
      gui_nav_blocking = !_rmb_look_active && (gui.wants_keyboard() || gui.focused_id() != "")
      if(gui_nav_blocking && !_app_gui_nav_blocking){
         _clear_camera_input_state()
      }
   }
   _last_gui_prep_ms = (trace && deep_trace) ? ui_profile.elapsed_ms(t_prep0) : 0.0
   if(_term_open){
      _mouse_dx_acc, _mouse_dy_acc = 0.0, 0.0
   }
   _app_prep_gui = prep_gui
   _app_gui_nav_blocking = gui_nav_blocking
}

fn _app_idle_after_events(bench, prep_gui) bool {
   if(!bench && !_term_open && !prep_gui && !_rmb_look_active && !_scene_drag_active && !_focused_scene_look_active() &&
      _intended_cursor_mode != CURSOR_DISABLED && skip_mouse_frames <= 0 &&
      _mouse_dx_acc == 0.0 && _mouse_dy_acc == 0.0 &&
      !_move_w && !_move_a && !_move_s && !_move_d && !_move_space && !_move_shift && !_move_ctrl &&
      _vx == 0.0 && _vy == 0.0 && _vz == 0.0 && _spdx == 0.0 && _spdy == 0.0 && _spd_z == 0.0 &&
      !_proj_dirty && _anim_count <= 0){
      _last_sim_ms = 0.0
      _ui_set_update_stage("idle")
      return true
   }
   false
}

fn _camera_state_snapshot() {
   [
      _cam_px, _cam_py, _cam_pz, _vx, _vy, _vz, _h_yaw, _h_pch,
      _target_yaw, _target_pch, _spdx, _spdy, _spd_z, _cam_fov,
   ]
}

fn _camera_state_changed(prev) bool {
   _cam_px != float(prev.get(0, _cam_px)) ||
   _cam_py != float(prev.get(1, _cam_py)) ||
   _cam_pz != float(prev.get(2, _cam_pz)) ||
   _vx != float(prev.get(3, _vx)) ||
   _vy != float(prev.get(4, _vy)) ||
   _vz != float(prev.get(5, _vz)) ||
   _h_yaw != float(prev.get(6, _h_yaw)) ||
   _h_pch != float(prev.get(7, _h_pch)) ||
   _target_yaw != float(prev.get(8, _target_yaw)) ||
   _target_pch != float(prev.get(9, _target_pch)) ||
   _spdx != float(prev.get(10, _spdx)) ||
   _spdy != float(prev.get(11, _spdy)) ||
   _spd_z != float(prev.get(12, _spd_z)) ||
   _cam_fov != float(prev.get(13, _cam_fov))
}

fn _sync_camera_state_to_arrays() {
   camthreed[0] = _cam_px
   camthreed[1] = _cam_py
   camthreed[2] = _cam_pz
   camthreed[3] = _vx
   camthreed[4] = _vy
   camthreed[5] = _vz
   camthreed[6] = _h_yaw
   camthreed[7] = _h_pch
   camthreed[8] = _target_yaw
   camthreed[9] = _target_pch
   camthreed[16] = _cam_fov
   camthreed[17] = _spdx
   camthreed[18] = _spdy
   camthreed[19] = _spd_z
   cam[4] = _h_yaw
   cam[5] = _h_pch
   _cam_px_cache, _cam_py_cache = _cam_px, _cam_py
   _cam_pz_cache = _cam_pz
}

fn _gui_apply_camera_state() bool {
   if(!camthreed){ return false }
   if(_cam_fov < 15.0){ _cam_fov = 15.0 }
   if(_cam_fov > 120.0){ _cam_fov = 120.0 }
   if(_h_pch < _p_min){ _h_pch = _p_min }
   if(_h_pch > _p_max){ _h_pch = _p_max }
   _target_yaw = _h_yaw
   _target_pch = _h_pch
   _vx, _vy, _vz = 0.0, 0.0, 0.0
   _spdx, _spdy, _spd_z = 0.0, 0.0, 0.0
   _sync_camera_state_to_arrays()
   camera.set_fov(camthreed, _cam_fov)
   camera.set_speed(camthreed, _spd)
   camera.set_sens(camthreed, _sens)
   camera.set_smoothing(camthreed, _damp, _drag)
   camera.set_pitch_limits(camthreed, _p_min, _p_max)
   camera.reset_motion(camthreed)
   _sync_projection_from_camera()
   _proj_dirty = true
   _static_world_redraw(2)
   true
}

fn _ui_projection_aspect() f64 {
   if(_win_h > 0.0 && _win_w > 0.0){ return _win_w / _win_h }
   16.0 / 9.0
}

fn _ui_update_projection(f64 px, f64 py, f64 pz, f64 tx, f64 ty, f64 tz) bool {
   def aspect = _ui_projection_aspect()
   rmat.mat4_look_at_into_xyz(px, py, pz, tx, ty, tz, 0.0, 1.0, 0.0, M_V)
   rmat.mat4_look_at_into_xyz(px, py, pz, tx, ty, tz, 0.0, 1.0, 0.0, M_V_SKY)
   M_V_SKY[14] = 0.0
   M_V_SKY[15] = 0.0
   M_V_SKY[16] = 0.0
   if(is_ortho){
      mut z = _ortho_zoom
      if(z < 0.01){ z = 0.01 }
      rmat.mat4_ortho_into(-z * aspect, z * aspect, -z, z, -1000.0, 1000.0, M_P)
   } else {
      mut fov = _cam_fov
      if(fov < 15.0){ fov = 15.0 }
      if(fov > 120.0){ fov = 120.0 }
      rmat.mat4_perspective_into(fov * PI / 180.0, aspect, 0.1, 1000.0, M_P)
   }
   rmat.mat4_mul_into(M_P, M_V, M_VP)
   rmat.mat4_mul_into(M_P, M_V_SKY, M_VP_SKY)
   _cam_px_cache, _cam_py_cache = px, py
   _cam_pz_cache = pz
   _proj_dirty = false
   true
}

fn _ui_update_projection_fast() bool {
   if(!_proj_dirty){ return true }
   def tgt = camera.target_from_angles(_cam_px, _cam_py, _cam_pz, _h_yaw, _h_pch)
   _ui_update_projection(
      _cam_px, _cam_py, _cam_pz,
      float(tgt.get(0, _cam_px)),
      float(tgt.get(1, _cam_py)),
      float(tgt.get(2, _cam_pz - 1.0))
   )
}

fn _sync_projection_from_camera() {
   _ui_set_update_stage("projection")
   def tgt = camera.target_from_angles(_cam_px, _cam_py, _cam_pz, _h_yaw, _h_pch)
   def tx = float(tgt.get(0, _cam_px))
   def ty = float(tgt.get(1, _cam_py))
   def tz = float(tgt.get(2, _cam_pz - 1.0))
   mut g_pos = cam.get(0)
   g_pos[0] = _cam_px
   g_pos[1] = _cam_py
   g_pos[2] = _cam_pz
   mut g_tgt = cam.get(1)
   g_tgt[0] = tx
   g_tgt[1] = ty
   g_tgt[2] = tz
   _ui_update_projection(_cam_px, _cam_py, _cam_pz, tx, ty, tz)
}

fn _app_update_simulation(dt, bench, prep_gui, gui_nav_blocking, trace, deep_trace) {
   def prev_cam = _camera_state_snapshot()
   def t_sim0 = (trace && deep_trace) ? ticks() : 0
   if(!_term_open && !bench){
      _ui_set_update_stage("camera.sim")
      _app_simulate_camera_frame(dt, prep_gui, gui_nav_blocking)
   }
   def cam_dirty = _camera_state_changed(prev_cam)
   if(cam_dirty){
      _sync_camera_state_to_arrays()
   }
   if(cam_dirty || _proj_dirty){
      _sync_projection_from_camera()
      _static_world_redraw(2)
   }
   _ui_set_update_stage("animation")
   _batch_dump_update_anim_fast(dt)
   _last_sim_ms = (trace && deep_trace) ? ui_profile.elapsed_ms(t_sim0) : 0.0
}

fn app_update(dt) {
   "Advance input, camera, batch, scene, and GUI state for one frame."
   _ui_set_update_stage("start")
   _process_gui_scene_requests()
   def trace = ui_profile.trace_enabled()
   def deep_trace = ui_profile.deep_enabled()
   def bench = ui_profile.ui_bench_enabled()
   def t_evt0 = (trace && deep_trace) ? ticks() : 0
   if(_app_update_fast_path(dt)){ return }
   if(_app_update_bench_frame(dt, trace, deep_trace, t_evt0)){ return }
   def gui_enabled_frame = _app_process_events()
   _ui_set_update_stage("post-events")
   _last_evt_ms = (trace && deep_trace) ? ui_profile.elapsed_ms(t_evt0) : 0.0
   _app_prepare_gui_after_events(bench, gui_enabled_frame, trace, deep_trace)
   def _discard_f1_poll = _app_poll_f1_hotkey()
   if(_app_idle_after_events(bench, _app_prep_gui)){ return }
   _app_update_simulation(dt, bench, _app_prep_gui, _app_gui_nav_blocking, trace, deep_trace)
   _ui_set_update_stage("done")
}

fn _draw_world_bench_fast(cam_px, cam_py, cam_pz) {
   if(!_bench_env_bound){
      gfx.set_env_tex(-1)
      gfx.set_env_spec_tex(-1)
      _bench_env_bound = true
   }
   _cam_px_cache, _cam_py_cache = cam_px, cam_py
   _cam_pz_cache = cam_pz
   if(!_bench_cam_bound || cam_px != _bench_cam_x || cam_py != _bench_cam_y || cam_pz != _bench_cam_z){
      gfx.set_cam_pos(cam_px, cam_py, cam_pz)
      _bench_cam_x, _bench_cam_y = cam_px, cam_py
      _bench_cam_z = cam_pz
      _bench_cam_bound = true
   }
   _draw_active_scene_fast_or_fallback()
}

fn _world_ensure_generated_envs(
   batch_on, proof_on, gui_probe_on, studio_env, neutral_env, compare_reflect_env,
   compare_visible_env, optical_spec_env, feature_fallback_env
){
   def plan = scene_engine.generated_plan(
      _world_env_textures(), bool(batch_on), bool(proof_on), bool(gui_probe_on), _gui_env_mode,
      bool(studio_env), bool(neutral_env), bool(compare_reflect_env),
      bool(compare_visible_env), bool(optical_spec_env), bool(feature_fallback_env)
   )
   if(bool(plan.get(0, false))){
      build_generated_textures(
         plan.get(1, false),
         plan.get(2, false),
         plan.get(3, false),
         plan.get(4, false)
      )
      if(bool(plan.get(5, false))){
         def _discard_21 = _load_fast_generated_skybox(ui_profile.visible_skybox_default())
      }
   }
}

fn _world_env_textures() dict {
   {
      "compare_env": compare_env_tex_id,
      "compare_env_spec": compare_env_spec_tex_id,
      "compare_visible_env": compare_visible_env_tex_id,
      "compare_reflect_spec": compare_reflect_spec_tex_id,
      "neutral_env": neutral_env_tex_id,
      "neutral_env_spec": neutral_env_spec_tex_id,
      "skybox": skybox_tex_id,
      "skybox_spec": skybox_spec_tex_id,
   }
}

fn _world_scene_env_allowed() bool {
   if(ui_profile.env_truthy_cached("NY_UI_DISABLE_ENV")){ return false }
   if(ui_profile.env_present_cached("NY_UI_VISIBLE_SKYBOX") && !ui_profile.env_enabled_cached("NY_UI_VISIBLE_SKYBOX")){ return false }
   if(ui_profile.env_present_cached("NY_UI_SHOW_SKYBOX") && !ui_profile.env_enabled_cached("NY_UI_SHOW_SKYBOX")){ return false }
   true
}

fn _world_ensure_scene_env_ready() bool {
   if(!show_scene || !is_dict(active_scene) || !_world_scene_env_allowed()){ return false }
   if(skybox_tex_id >= 0 || compare_env_tex_id >= 0 || neutral_env_tex_id >= 0 || compare_visible_env_tex_id >= 0){
      return true
   }
   if(_load_skybox(true, "")){ return true }
   _load_fast_generated_skybox(true)
}

fn _world_scene_has_lights() bool {
   if(!is_dict(active_scene)){
      return false
   }
   mut has_scene_lights = int(active_scene.get("scene_lights_count", 0)) > 0
   if(!has_scene_lights){
      def scene_lights_fallback = active_scene.get("scene_lights", [])
      has_scene_lights = is_list(scene_lights_fallback) && scene_lights_fallback.len > 0
   }
   has_scene_lights
}

fn _world_proof_batch_env_fallbacks(
   env_tex, env_spec_tex, proof_on, batch_on, scene_env_sensitive_materials,
   scene_needs_reflect_spec, scene_needs_optical_spec, compare_visible_env
){
   def can_fallback = (proof_on || batch_on) && is_dict(active_scene) &&
   !_scene_pref_black_visible && _gui_env_mode != 4
   def has_lights = _world_scene_has_lights()
   def tex0 = _world_env_textures()
   if(scene_engine.proof_needs_generated(tex0, int(env_tex), int(env_spec_tex), can_fallback, has_lights, bool(scene_env_sensitive_materials), bool(scene_needs_reflect_spec))){
      def _discard_22 = build_generated_textures(true, true, true, scene_needs_reflect_spec)
   }
   def pair = scene_engine.proof_fallbacks(
      _world_env_textures(), int(env_tex), int(env_spec_tex), can_fallback, has_lights,
      bool(scene_env_sensitive_materials), bool(scene_needs_reflect_spec), bool(scene_needs_optical_spec), bool(compare_visible_env)
   )
   mut out_env_tex = int(pair.get(0, env_tex))
   mut out_env_spec_tex = int(pair.get(1, env_spec_tex))
   if(can_fallback && out_env_tex < 0 && skybox_tex_id < 0){
      def _discard_23 = _load_fast_generated_skybox(false)
   }
   if(can_fallback && out_env_tex < 0){
      def sky_pair = scene_engine.skybox_fallback(_world_env_textures(), out_env_tex, out_env_spec_tex, true)
      out_env_tex = int(sky_pair.get(0, out_env_tex))
      out_env_spec_tex = int(sky_pair.get(1, out_env_spec_tex))
   }
   [out_env_tex, out_env_spec_tex]
}

fn _world_env_background_state(env_tex, env_spec_tex, batch_on, proof_on, gui_probe_on, gui_probe_has_scene, compare_visible_env) {
   mut out_env_tex = env_tex
   mut out_env_spec_tex = env_spec_tex
   def draw_env_background = scene_engine.background_requested(
      skybox_enabled, bool(compare_visible_env), bool(batch_on), _gui_env_mode,
      bool(gui_probe_on), bool(gui_probe_has_scene), out_env_tex >= 0,
      _gui_enabled_now(), _scene_pref_black_visible,
      ui_profile.env_truthy_cached("NY_UI_GUI_DRAW_ENV_BG"), bool(proof_on),
      ui_profile.env_truthy_cached("NY_UI_PROOF_SKYBOX")
   )
   if(draw_env_background && skybox_enabled && _ensure_visible_skybox_ready() && skybox_tex_id >= 0){
      out_env_tex = skybox_tex_id
      if(out_env_spec_tex < 0){
         out_env_spec_tex = (skybox_spec_tex_id >= 0) ? skybox_spec_tex_id : skybox_tex_id
      }
   }
   elif(draw_env_background && out_env_tex < 0 && _ensure_visible_skybox_ready()){
      out_env_tex = skybox_tex_id
      if(out_env_spec_tex < 0){
         out_env_spec_tex = (skybox_spec_tex_id >= 0) ? skybox_spec_tex_id : skybox_tex_id
      }
   }
   [out_env_tex, out_env_spec_tex, draw_env_background]
}

fn _world_trace_parity(env_tex, env_spec_tex, draw_env_background, scene_mat_mask, batch_on, proof_on, gui_probe_on) {
   if(!ui_profile.parity_trace_enabled()){ return }
   mut lights_count = 0
   if(is_dict(active_scene)){ lights_count = int(active_scene.get("scene_lights_count", 0)) }
   def sig = to_str(env_tex) + "|" + to_str(env_spec_tex) + "|" +
   to_str(draw_env_background ? 1 : 0) + "|" + to_str(scene_mat_mask) + "|" +
   to_str(lights_count) + "|" + to_str(_gui_env_mode) + "|" +
   to_str(batch_on ? 1 : 0) + "|" + to_str(proof_on ? 1 : 0) + "|" +
   to_str(gui_probe_on ? 1 : 0) + "|" + to_str(show_scene ? 1 : 0) + "|" + _loaded_scene_name
   if(sig != _last_world_parity_sig){
      _last_world_parity_sig = sig
      def _discard_world_parity_trace = ui_profile.print_text("[parity:world] scene=" + _loaded_scene_name +
         " env=" + to_str(env_tex) +
         " spec=" + to_str(env_spec_tex) +
         " bg=" + to_str(draw_env_background ? 1 : 0) +
         " lights=" + to_str(lights_count) +
         " mask=" + to_str(scene_mat_mask) +
         " gui_env=" + to_str(_gui_env_mode) +
         " batch=" + to_str(batch_on ? 1 : 0) +
         " proof=" + to_str(proof_on ? 1 : 0) +
      " probe=" + to_str(gui_probe_on ? 1 : 0))
   }
}

fn _draw_world_skybox(draw_env_background, env_tex) {
   if(draw_env_background && env_tex >= 0){
      _render_set_skybox_view(_h_yaw, _h_pch, _cam_fov)
      gfx.set_view_proj(M_VP_SKY)
      gfx.set_model_matrix(M_ID)
      _render_draw_skybox(env_tex)
      gfx.set_view_proj(M_VP)
   }
}

fn _draw_world_grid_and_scene(phase, cam_px, cam_pz, chrome_on, gui_probe_on, gui_probe_has_scene) {
   def clean_proof = _proof_dump_active()
   def have_scene = gui_probe_has_scene
   def have_axes = is_dict(_axes_mesh) && !clean_proof
   if(ui_profile.env_truthy_cached("NY_UI_GROUP_TRACE")){
      mut scene_gpu_count = 0
      mut scene_cpu_count = 0
      mut scene_state_count = 0
      mut scene_state_slab = false
      if(is_dict(active_scene)){
         scene_gpu_count = int(active_scene.get("gpu_parts_count", 0))
         scene_cpu_count = int(active_scene.get("parts_count", 0))
         def scene_gpu_state = active_scene.get("gpu_draw_state", 0)
         if(is_list(scene_gpu_state) && scene_gpu_state.len >= 2){
            scene_state_slab = to_int(scene_gpu_state.get(0, 0)) != 0
            scene_state_count = int(scene_gpu_state.get(1, 0))
         }
      }
      ui_profile.print_text("[ui:world] show=" + to_str(show_scene) +
         " dict=" + to_str(is_dict(active_scene)) +
         " have=" + to_str(have_scene) +
         " chrome=" + to_str(chrome_on) +
         " gpu=" + to_str(scene_gpu_count) +
         " cpu=" + to_str(scene_cpu_count) +
         " state_slab=" + to_str(scene_state_slab) +
      " state_count=" + to_str(scene_state_count))
   }
   if(chrome_on && (ui_profile.world_grid_enabled(_gui_visible, _scene_selected, _gui_probe_mode_enabled()) || (_gizmo_ruler && _gui_editor_shell_open())) && !clean_proof && !(gui_probe_on && !have_scene)){
      gfx.set_unlit(true)
      gfx.reset_overlay_state()
      def axes_in_grid = _draw_infinite_world_grid(cam_px, cam_pz, have_axes)
      if(have_axes && !axes_in_grid){
         gfx.set_model_matrix(M_ID)
         draw_mesh(_axes_mesh)
      }
      gfx.set_unlit(false)
   }
   if(have_scene){
      gfx.clear_depth()
      _set_active_scene_model_matrix()
      gfx.reset_overlay_state()
      gfx.set_unlit(false)
      _draw_active_scene_fast_or_fallback()
   }
}

fn _draw_world(phase, cam_px, cam_py, cam_pz) {
   if(ui_profile.ui_bench_enabled()){
      _draw_world_bench_fast(cam_px, cam_py, cam_pz)
      return
   }
   def chrome_on = _chrome_visible()
   def batch_on = _batch_dump_enabled()
   def proof_on = _proof_dump_active()
   _scene_pref_cache_update(_loaded_scene_name)
   def _discard_scene_env_ready = _world_ensure_scene_env_ready()
   mut env_tex_map = _world_env_textures()
   def modes = scene_engine.mode_flags(
      _scene_pref_studio, _scene_pref_neutral, _scene_pref_reflect,
      _scene_pref_visible, _scene_pref_optical, batch_on, _gui_env_mode
   )
   def studio_env = modes.get(0, false)
   def neutral_env = modes.get(1, false)
   def compare_reflect_env = modes.get(2, false)
   def compare_visible_env = modes.get(3, false)
   def optical_spec_env = modes.get(4, false)
   def scene_info = scene_engine.scene_material_info(active_scene, studio_env, neutral_env)
   def scene_mat_mask = int(scene_info.get(0, 0))
   def scene_env_sensitive_materials = scene_info.get(1, false)
   def scene_needs_reflect_spec = scene_info.get(2, false)
   def scene_needs_optical_spec = scene_info.get(3, false)
   def feature_fallback_env = scene_info.get(4, false)
   def gui_probe_on = _gui_probe_mode_enabled()
   def gui_probe_has_scene = _ui_scene_visible()
   _world_ensure_generated_envs(
      batch_on, proof_on, gui_probe_on, studio_env, neutral_env, compare_reflect_env,
      compare_visible_env, optical_spec_env, feature_fallback_env
   )
   env_tex_map = _world_env_textures()
   gfx.set_cam_pos(cam_px, cam_py, cam_pz)
   def override_pair = scene_engine.scene_override_pair(
      env_tex_map, studio_env, neutral_env, compare_reflect_env, compare_visible_env,
      optical_spec_env, feature_fallback_env, scene_env_sensitive_materials,
      scene_needs_reflect_spec, scene_needs_optical_spec, batch_on,
      _gui_env_mode, _scene_pref_black_visible,
      ui_profile.env_truthy_cached("NY_UI_DISABLE_ENV"),
      ui_profile.env_truthy_cached("NY_UI_DISABLE_ENV_SPEC")
   )
   def fallback_pair = _world_proof_batch_env_fallbacks(
      int(override_pair.get(0, -1)), int(override_pair.get(1, -1)), proof_on, batch_on,
      scene_env_sensitive_materials, scene_needs_reflect_spec, scene_needs_optical_spec, compare_visible_env
   )
   def bg_state = _world_env_background_state(
      int(fallback_pair.get(0, -1)), int(fallback_pair.get(1, -1)),
      batch_on, proof_on, gui_probe_on, gui_probe_has_scene, compare_visible_env
   )
   def env_pair = scene_engine.normalize_pair(
      env_tex_map, int(bg_state.get(0, -1)), int(bg_state.get(1, -1)),
      batch_on, proof_on, scene_needs_optical_spec
   )
   def env_tex = int(env_pair.get(0, -1))
   def env_spec_tex = int(env_pair.get(1, -1))
   def draw_env_background = bg_state.get(2, false)
   gfx.set_env_tex(env_tex)
   gfx.set_env_spec_tex(env_spec_tex)
   _world_trace_parity(env_tex, env_spec_tex, draw_env_background, scene_mat_mask, batch_on, proof_on, gui_probe_on)
   _cam_px_cache, _cam_py_cache = cam_px, cam_py
   _cam_pz_cache = cam_pz
   _draw_world_skybox(draw_env_background, env_tex)
   _draw_world_grid_and_scene(phase, cam_px, cam_pz, chrome_on, gui_probe_on, gui_probe_has_scene)
   _draw_scene_world_gizmo()
   return
}

fn _present_loading_frame(label) {
   if(ui_profile.headless_enabled() || !win){ return false }
   poll_events()
   if(should_close(win)){ return false }
   _sync_window_size_from_live()
   if(!gfx.begin_frame()){ return false }
   _prepare_gui_overlay_pass()
   viewer_loading.draw_startup_card(_win_w, _win_h, _ui_title_font(), _ui_font(), label)
   def ok = gfx.end_frame()
   poll_events()
   ok
}

fn render_thread_obj() {
   "Draw one full renderer frame for the current UI state."
   def phase = _render_phase
   if(phase < 0.0){ return false }
   def render_trace = ui_profile.env_truthy_cached("NY_UI_GROUP_TRACE")
   if(_ui_static_world_fast_enabled() && total_frames >= 2 && _ui_static_update_clean() && _prepare_static_world_visual_fast()){
      def load_color = _main_loop_force_full_render ? false : _static_world_color_reuse_allowed()
      if(render_trace){
         ui_profile.print_text("[ui:render] static force=" + to_str(_main_loop_force_full_render) +
            " load_color=" + to_str(load_color) +
            " color_frames=" + to_str(_static_world_color_reuse_frames) +
         " redraw=" + to_str(_static_world_redraw_frames))
      }
      if(load_color){ _render_set_next_frame_load_color(true) }
      _ui_set_update_stage("render.static.begin_frame")
      if(!gfx.begin_frame()){
         if(load_color){ _render_set_next_frame_load_color(false) }
         return false
      }
      _ui_set_update_stage("render.static.draw")
      if(!_static_world_skip_draw_now(load_color)){
         _draw_static_world_visual_fast(load_color)
      }
      _draw_runtime_overlay_after_world(load_color)
      _ui_set_update_stage("render.static.end_frame")
      def _discard_70 = gfx.end_frame()
      _static_world_color_reuse_note(load_color)
      return true
   }
   if(render_trace){
      ui_profile.print_text("[ui:render] full force=" + to_str(_main_loop_force_full_render) +
         " static=" + to_str(_ui_static_world_fast_enabled()) +
      " tf=" + to_str(total_frames))
   }
   _main_loop_trace_once("[ui:render] begin_frame.before")
   _ui_set_update_stage("render.begin_frame")
   if(!gfx.begin_frame()){
      if(ui_profile.gui_trace_enabled()){
         def _discard_72 = ui_profile.print_text("[ui:frame] skip reason=begin_frame_false")
      }
      _main_loop_trace_once("[ui:render] begin_frame.false")
      return false
   }
   _main_loop_trace_once("[ui:render] begin_frame.after")
   _main_loop_trace_once("[ui:render] draw.before")
   _ui_set_update_stage("render.draw")
   _draw_frame(phase, _term_open)
   _main_loop_trace_once("[ui:render] draw.after")
   _main_loop_trace_once("[ui:render] end_frame.before")
   _ui_set_update_stage("render.end_frame")
   def _discard_78 = gfx.end_frame()
   _main_loop_trace_once("[ui:render] end_frame.after")
   true
}

fn _draw_frame(phase, term_open) {
   def cam_px = _cam_px_cache
   def cam_py = _cam_py_cache
   def cam_pz = _cam_pz_cache
   def trace = ui_profile.trace_enabled()
   def t0 = trace ? ticks() : 0
   if(_ui_debug_enabled == 1){
      def cur_to = term_open ? 1 : 0
      if(_last_draw_world_state != 1 || cur_to != _last_term_open_state){
         _last_draw_world_state = 1
         _last_term_open_state = cur_to
         _dbg_ui("[ui] draw mode: term_open=" + to_str(term_open) + " world_pass=true")
      }
   }
   gfx.set_view_proj(M_VP)
   try {
      _draw_world(phase, cam_px, cam_py, cam_pz)
   } catch err {
      ui_profile.eprint_text("[ui:panic] stage=draw.world frame=" + to_str(total_frames))
      panic(err)
   }
   if(ui_profile.ui_bench_enabled()){
      return
   }
   if(trace){
      def t1 = ticks()
      _last_world_ms = ui_profile.ms_between(t1, t0)
      try {
         _draw_ui(phase, term_open)
      } catch err {
         ui_profile.eprint_text("[ui:panic] stage=draw.ui frame=" + to_str(total_frames) +
         " update_stage=" + _ui_update_stage)
         panic(err)
      }
      def t2 = ticks()
      _last_ui_ms = ui_profile.ms_between(t2, t1)
   } else {
      try {
         _draw_ui(phase, term_open)
      } catch err {
         ui_profile.eprint_text("[ui:panic] stage=draw.ui frame=" + to_str(total_frames) +
         " update_stage=" + _ui_update_stage)
         panic(err)
      }
   }
}

fn _render_thread_world_only_fast(draw_overlay=false) {
   def phase = _render_phase
   if(phase < 0.0){ return false }
   if(!gfx.begin_frame()){ return false }
   gfx.set_view_proj(M_VP)
   _draw_world(phase, _cam_px_cache, _cam_py_cache, _cam_pz_cache)
   if(draw_overlay){ def _discard_80 = _draw_runtime_overlay_after_world(false) }
   def _discard_81 = gfx.end_frame()
   true
}

fn _render_thread_static_world_visual_direct(load_color=false) {
   if(load_color){ _render_set_next_frame_load_color(true) }
   if(!gfx.begin_frame()){
      if(load_color){ _render_set_next_frame_load_color(false) }
      return false
   }
   if(!_static_world_skip_draw_now(load_color)){
      _draw_static_world_visual_fast(load_color)
   }
   def _discard_86 = gfx.end_frame()
   true
}

fn _bench_draw_world_only_fast() {
   _draw_active_scene_fast_or_fallback()
}

fn _render_bench_frame() bool {
   if(ui_profile.bench_enabled(ui_profile.profile_dump_enabled(ui_profile.trace_enabled()))){
      def t0 = ticks()
      if(!_render_begin_frame()){ return false }
      def t1 = ticks()
      _bench_draw_world_only_fast()
      def t2 = ticks()
      def _discard_87 = _render_end_frame()
      def t3 = ticks()
      ui_profile.bench_record(t1 - t0, t2 - t1, t3 - t2)
      return true
   }
   if(!_render_begin_frame()){ return false }
   _bench_draw_world_only_fast()
   def _discard_88 = _render_end_frame()
   true
}

fn _render_thread_obj_fast_bench() {
   def now = ticks()
   if(_last_frame_time == 0){ _last_frame_time = now }
   _current_frame_time = (now - _last_frame_time) / 1000000000.0
   _last_frame_time = now
   _frame_time_accum += _current_frame_time
   _render_set_frame_time_sec(_frame_time_accum)
   _render_bench_frame()
}

fn _render_thread_obj_sim_bench_fast() { _render_bench_frame() }

fn _fast_view_sync_live_keys() bool {
   if(key_down(win, uin.KEY_ESCAPE) || key_down(win, 0xFF1B)){
      set_should_close(win, true)
      return false
   }
   if(_app_poll_f1_hotkey()){
      return false
   }
   _move_w = key_down(win, uin.KEY_W)
   _move_a = key_down(win, uin.KEY_A)
   _move_s = key_down(win, uin.KEY_S)
   _move_d = key_down(win, uin.KEY_D)
   _move_space = key_down(win, uin.KEY_SPACE)
   _move_shift = key_down(win, uin.KEY_LEFT_SHIFT) || key_down(win, uin.KEY_RIGHT_SHIFT) || key_down(win, uin.KEY_SHIFT)
   _move_ctrl = key_down(win, uin.KEY_LEFT_CONTROL) || key_down(win, uin.KEY_RIGHT_CONTROL) || key_down(win, uin.KEY_CTRL)
   true
}

fn _fast_view_handle_key_pressed(any data) bool {
   def k = uin.event_key(data)
   def sc = data.get("scancode", data.get("raw_key", 0))
   def mods = ui_app.app_effective_mods(data.get("mod", 0), k, sc, _move_shift, _move_ctrl)
   if(k == uin.KEY_F1){
      def _discard_f1 = _hotkey_toggle_editor()
      return false
   }
   def _discard_move_key = _app_set_move_key(k, sc, true)
   if(k == uin.KEY_ESCAPE || k == 256){
      set_should_close(win, true)
   } elif(k == uin.KEY_PAGE_UP){
      if(_app_block_scripted_model_cycle(k)){ return true }
      _cycle_loaded_model(-1)
   } elif(k == uin.KEY_PAGE_DOWN){
      if(_app_block_scripted_model_cycle(k)){ return true }
      _cycle_loaded_model(1)
   } elif((mods & MOD_CONTROL) == 0 && k == uin.KEY_F){
      _cmd_lookat(true)
   } elif((mods & MOD_CONTROL) != 0 && (k == uin.KEY_P || k == uin.KEY_K || k == uin.KEY_F)){
      _gui_visible = true
      _gui_focus_asset_search()
   }
   true
}

fn _fast_view_handle_key_released(any data) {
   def k = uin.event_key(data)
   def sc = data.get("scancode", data.get("raw_key", 0))
   _app_set_move_key(k, sc, false)
}

fn _fast_view_handle_mouse_pressed(any data) {
   if(_scene_loading_input_guard_active()){
      _release_scene_loading_input("scene load fast mouse press")
      return
   }
   def b = int(data.get("button", -1))
   if(!_term_open && b == _MOUSE_MIDDLE){
      def _discard_middle = _middle_mouse_start()
   } elif(!_term_open && b == _MOUSE_RIGHT && is_dict(active_scene) && show_scene){
      _rmb_look_active = true
      gui.clear_focus()
      skip_mouse_frames = 0
      _clear_camera_input_state()
      _mouse_look_raw_until_ns = 0
      def _discard_sync = _sync_cursor_state("rmb look start")
   }
}

fn _fast_view_handle_mouse_released(any data) {
   def b = int(data.get("button", -1))
   if(b == _MOUSE_MIDDLE){
      def _discard_middle = _middle_mouse_stop()
   } elif(b == _MOUSE_RIGHT && _rmb_look_active){
      _rmb_look_active = false
      skip_mouse_frames = 0
      _clear_camera_input_state()
      _mouse_look_raw_until_ns = 0
      def _discard_sync = _sync_cursor_state("rmb look stop")
   }
}

fn _fast_view_pump_events() bool {
   "Polls the minimal event set needed while the static fast viewer is active."
   poll_events()
   mut interactive = false
   mut e = _app_next_event()
   while(e != 0){
      def typ = event_type(e)
      def data = event_data(e)
      if(quit(e)){
         set_should_close(win, true)
      }
      if(_ui_consume_hotkey_text_spill(typ)){
         e = _app_next_event()
         continue
      }
      if(_ui_handle_menu_hotkey(typ, data, true)){
         e = _app_next_event()
         continue
      }
      if(typ == EVENT_WINDOW_RESIZED){
         _handle_window_resize_event(data, true, true, true)
      }
      elif(typ == EVENT_KEY_PRESSED){
         interactive = true
         if(!_fast_view_handle_key_pressed(data)){ return false }
      }
      elif(typ == EVENT_KEY_RELEASED){
         interactive = true
         def _discard_key_release = _fast_view_handle_key_released(data)
      }
      elif(typ == EVENT_MOUSE_BUTTON_PRESSED){
         interactive = true
         def _discard_mouse_press = _fast_view_handle_mouse_pressed(data)
      }
      elif(typ == EVENT_MOUSE_BUTTON_RELEASED){
         interactive = true
         def _discard_mouse_release = _fast_view_handle_mouse_released(data)
      }
      elif(typ == EVENT_MOUSE_POS_CHANGED){
         interactive = true
         _app_handle_mouse_pos(data, false)
      }
      elif(typ == EVENT_MOUSE_SCROLL){
         interactive = true
         if(_middle_mouse_scroll_suppressed()){
            e = _app_next_event()
            continue
         }
         _app_handle_scroll(data, false, false)
      }
      elif(typ == EVENT_FOCUS_OUT){
         interactive = true
         _app_handle_focus_out()
      }
      elif(typ == EVENT_MOUSE_LEAVE){
         interactive = true
         _app_handle_mouse_leave()
      }
      elif(typ == EVENT_MOUSE_ENTER){
         interactive = true
         _app_handle_mouse_enter()
      }
      e = _app_next_event()
   }
   if(interactive){
      return false
   }
   if(!_fast_view_sync_live_keys()){
      return false
   }
   !(_ui_move_input_active() || _middle_mouse_active)
}

fn _ui_fast_view_enabled() bool {
   if(!win || ui_profile.headless_enabled() || ui_profile.nosurface_enabled()){ return false }
   if(!ui_profile.ui_bench_enabled() || _timeout_ns <= 0){ return false }
   if(_auto_dump_enabled == 1 || _batch_dump_enabled() || _gui_dump_suite_active){ return false }
   if(_term_open || _gui_enabled_now() || _gui_probe_mode_enabled()){ return false }
   if(!is_dict(active_scene) || !show_scene || _scene_load_async_active()){ return false }
   if(_anim_enabled){ return false }
   if((_anim_count > 0 || is_dict(_anim_gltf_data)) && !_scene_static_pose_gpu_ready() && !_scene_deform_idle_ready()){ return false }
   if(!_ui_static_world_fast_enabled()){ return false }
   !_ui_view_input_active()
}

fn _bench_reset_runtime(int t0) {
   _startup_pumped = true
   start_t = t0
   last_upd_t = t0
   last_fps_t = t0
   _last_frame_time = t0
   _current_frame_time = 0.0
   _frame_time_accum = 0.0
   frame_num = 0
   total_frames = 0
   fps = 0
   _fps_samples = []
   ui_profile.bench_reset()
   _render_phase = -1.0
   skip_mouse_frames = 0
   _mouse_delta_suppress_until_ns = 0
   _rmb_look_active = false
   _scene_drag_active = false
   _clear_camera_input_state()
}

fn _print_fps_summary(started_at, total, show_median=false) {
   ui_loop.print_fps_summary(_fps_log_enabled, int(started_at), int(total), _fps_samples, bool(show_median))
}

fn _bench_print_summary(
   str prefix,
   int started_at,
   int total,
   dict rs,
   bool show_median=false,
   bool with_verts=true,
   bool with_desc=true
){
   ui_loop.print_bench_summary(prefix, _fps_log_enabled, started_at, total, _fps_samples, rs, show_median, with_verts, with_desc)
}

fn _bench_log_start(mode_name) {
   if(_fps_log_enabled != 1){ return }
   def _printed = ui_profile.print_text("[bench] mode=" + mode_name +
      " headless=" + to_str(ui_profile.headless_enabled()) +
      " backend=" + to_str(get_active_backend_name()) +
      " _timeout_ns=" + to_str(_timeout_ns) +
      " show_scene=" + to_str(show_scene) +
      " active_scene=" + to_str(is_dict(active_scene)) +
   " loaded_scene=" + to_str(_loaded_scene_name))
   return
}

fn _bench_bind_env_and_camera(bool bind_env=true) {
   if(bind_env && !_bench_env_bound){
      gfx.set_env_tex(-1)
      gfx.set_env_spec_tex(-1)
      _bench_env_bound = true
   }
   if(!_bench_cam_bound){
      gfx.set_cam_pos(_cam_px_cache, _cam_py_cache, _cam_pz_cache)
      _bench_cam_x, _bench_cam_y, _bench_cam_z = _cam_px_cache, _cam_py_cache, _cam_pz_cache
      _bench_cam_bound = true
   }
}

fn _sample_fps_tick(int now, bool update_public_fps=false) bool {
   def sampled = ui_loop.sample_fps(_fps_samples, frame_num, last_fps_t, now)
   if(!bool(sampled.get("sampled", false))){ return false }
   _fps_samples = sampled.get("samples", _fps_samples)
   frame_num = int(sampled.get("frames", 0))
   last_fps_t = int(sampled.get("last", now))
   if(update_public_fps){
      fps = int(sampled.get("fps", fps))
      if(win && !ui_profile.headless_enabled()){
         set_title(win, "Nytrix UI - " + to_str(fps) + " FPS")
      }
   }
   true
}

fn _run_fast_nosurface_bench_loop() {
   "Runs the real renderer in no-surface benchmark mode without GUI/input/window polling overhead."
   def visual_bench = ui_profile.headless_visual_bench_enabled()
   _bench_log_start(visual_bench ? "fast_nosurface_visual" : "fast_nosurface_model")
   _bench_reset_runtime(ticks())
   mut now = start_t
   mut frame_iter = 0
   mut phase = 0.0
   def dt = 0.0001
   mut static_visual_fast = visual_bench && _prepare_static_world_visual_fast()
   if(_fps_log_enabled == 1 && visual_bench){
      ui_profile.print_text("[bench] visual_static=" + to_str(static_visual_fast) +
      " skybox=" + to_str(_static_world_draw_sky))
   }
   if(!static_visual_fast){
      gfx.set_view_proj(M_VP)
   }
   _bench_bind_env_and_camera(!static_visual_fast)
   mut visual_reuse_latched = false
   mut visual_profile = ui_profile.bench_enabled(ui_profile.profile_dump_enabled(ui_profile.trace_enabled()))
   while(true){
      if((frame_iter & 63) == 0){
         now = ticks()
         if(_timeout_ns > 0 && (now - start_t) >= _timeout_ns){
            break
         }
      }
      mut rendered = false
      if(static_visual_fast){
         mut load_color = visual_reuse_latched
         if(!visual_reuse_latched){
            load_color = _static_world_color_reuse_allowed()
            if(load_color){
               visual_reuse_latched = true
            }
         }
         if(visual_profile){
            def t0 = ticks()
            if(load_color){
               _render_set_next_frame_load_color(true)
            }
            if(gfx.begin_frame()){
               def t1 = ticks()
               if(!_static_world_skip_draw_now(load_color)){
                  _draw_static_world_visual_fast(load_color)
               }
               def t2 = ticks()
               def _discard_93 = gfx.end_frame()
               def t3 = ticks()
               ui_profile.bench_record(t1 - t0, t2 - t1, t3 - t2)
               rendered = true
            } elif(load_color){
               _render_set_next_frame_load_color(false)
            }
         } else {
            rendered = _render_thread_static_world_visual_direct(load_color)
         }
         if(rendered){
            _static_world_color_reuse_note(load_color)
         }
      } else {
         phase += dt
         _render_dt = dt
         _render_phase = phase
         rendered = _render_thread_obj_fast_bench()
      }
      if(rendered){
         _did_first_frame = true
         total_frames += 1
      }
      frame_iter += 1
   }
   def rs = renderer_frame_stats()
   ui_profile.bench_flush(ticks() - start_t, rs, ui_profile.profile_dump_enabled(ui_profile.trace_enabled()) ? ui_profile.profile_dump_file() : "")
   _bench_print_summary("[bench] ", start_t, total_frames, rs, false, true, true)
   __exit(0)
}

fn _run_simulated_nosurface_bench_loop() {
   "Runs benchmark-only hidden/headless scenes through a tight render loop."
   _bench_log_start(ui_profile.nosurface_enabled() ? "sim_nosurface" : "fast_surface")
   _bench_reset_runtime(ticks())
   mut now = start_t
   mut frame_iter = 0
   mut phase = 0.0
   def trace = ui_profile.trace_enabled()
   _ui_update_projection_fast()
   gfx.set_view_proj(M_VP)
   mut static_update_clean = _ui_static_update_clean()
   def dt = 0.0001
   if(static_update_clean){
      _current_frame_time = dt
      _render_dt = dt
      _render_phase = phase
      _render_set_frame_time_sec(_frame_time_accum)
   }
   _bench_bind_env_and_camera(true)
   while(true){
      if((frame_iter & 63) == 0){
         now = ticks()
         if(_timeout_ns > 0 && (now - start_t) >= _timeout_ns){
            break
         }
      }
      if(!(static_update_clean && !_proj_dirty)){
         phase += dt
         _current_frame_time = dt
         _frame_time_accum += dt
         _render_set_frame_time_sec(_frame_time_accum)
         _render_dt = dt
         _render_phase = phase
      }
      def frame_t0 = trace ? ticks() : 0
      mut update_t1 = frame_t0
      def anim_trace = _anim_frame_trace_enabled() && _anim_frame_trace_hits < 16
      def anim_loop_t0 = anim_trace ? ticks() : 0
      if(anim_trace){
         ui_profile.print_text("[anim:loop] iter=" + to_str(frame_iter) + " update.before")
      }
      if(static_update_clean && !_proj_dirty){
         _last_update_ms = 0.0
      } else {
         app_update(dt)
         update_t1 = trace ? ticks() : frame_t0
         _last_update_ms = trace ? ui_profile.ms_between(update_t1, frame_t0) : 0.0
         static_update_clean = _ui_static_update_clean()
      }
      if(anim_trace){
         ui_profile.print_text("[anim:loop] update_ms=" + to_str(ui_profile.elapsed_ms(anim_loop_t0)) + " render.before")
      }
      def anim_render_t0 = anim_trace ? ticks() : 0
      def rendered = _render_thread_obj_sim_bench_fast()
      if(anim_trace){
         ui_profile.print_text("[anim:loop] render_ms=" +
            to_str(ui_profile.elapsed_ms(anim_render_t0)) +
         " rendered=" + to_str(rendered))
         _anim_frame_trace_hits += 1
      }
      def draw_t1 = trace ? ticks() : update_t1
      if(rendered){
         _did_first_frame = true
         total_frames += 1
         frame_num += 1
         if(trace){
            _last_draw_ms = ui_profile.ms_between(draw_t1, update_t1)
            _last_frame_ms = ui_profile.ms_between(draw_t1, frame_t0)
         }
      }
      if((frame_iter & 1023) == 0){
         def _discard_sample = _sample_fps_tick(ticks())
      }
      frame_iter += 1
   }
   if(ui_profile.bench_enabled(ui_profile.profile_dump_enabled(ui_profile.trace_enabled()))){
      def rs = renderer_frame_stats()
      ui_profile.bench_flush(ticks() - start_t, rs, ui_profile.profile_dump_enabled(ui_profile.trace_enabled()) ? ui_profile.profile_dump_file() : "")
   }
   _print_fps_summary(start_t, total_frames, true)
   __exit(0)
}

fn _run_fast_view_loop() bool {
   "Runs static no-chrome model viewing through a tight visible renderer loop."
   if(_fps_log_enabled == 1){
      def _discard_94 = ui_profile.print_text("[fast-view] mode=static_view backend=" + to_str(get_active_backend_name()) +
         " _timeout_ns=" + to_str(_timeout_ns) +
      " loaded_scene=" + to_str(_loaded_scene_name))
   }
   _bench_reset_runtime(ticks())
   def view_start_t = start_t
   mut static_visual_fast = _prepare_static_world_visual_fast()
   if(_fps_log_enabled == 1){
      def _discard_95 = ui_profile.print_text("[fast-view] static_visual=" + to_str(static_visual_fast))
   }
   if(!static_visual_fast){
      def _discard_96 = _ui_update_projection_fast()
      gfx.set_view_proj(M_VP)
      set_clear_color(APP_BG)
      gfx.set_cam_pos(_cam_px_cache, _cam_py_cache, _cam_pz_cache)
   }
   mut frame_iter = 0
   mut phase = 0.0
   def dt = 0.0001
   _current_frame_time = dt
   _render_dt = dt
   _render_phase = phase
   _render_set_frame_time_sec(_frame_time_accum)
   mut static_reuse_latched = false
   while(true){
      if((frame_iter & 255) == 0){
         def now = ticks()
         def deadline = (_timeout_ns > 500000000) ? (_timeout_ns - 300000000) : _timeout_ns
         if(_timeout_ns > 0 && (now - start_t) >= deadline){
            break
         }
      }
      def fast_ok = _fast_view_pump_events()
      if(should_close(win)){
         break
      }
      if(!fast_ok || !_ui_fast_view_enabled()){
         return false
      }
      def proj_was_dirty = _proj_dirty
      if(proj_was_dirty){
         def _discard_97 = _ui_update_projection_fast()
      }
      if(proj_was_dirty || frame_iter == 0){
         gfx.set_view_proj(M_VP)
      }
      if(proj_was_dirty && static_visual_fast){
         static_reuse_latched = false
         _static_world_color_reuse_reset()
         static_visual_fast = _prepare_static_world_visual_fast()
      }
      mut rendered = false
      if(static_visual_fast){
         mut load_color = static_reuse_latched
         if(!static_reuse_latched){
            load_color = _static_world_color_reuse_allowed()
            if(load_color){
               static_reuse_latched = true
            }
         }
         if(load_color){
            _render_set_next_frame_load_color(true)
         }
         if(gfx.begin_frame()){
            if(!_static_world_skip_draw_now(load_color)){
               _draw_static_world_visual_fast(load_color)
            }
            if(APP_STATS){
               def _discard_98 = _draw_runtime_overlay_after_world(load_color)
            }
            def _discard_99 = gfx.end_frame()
            _static_world_color_reuse_note(load_color)
            rendered = true
         } elif(load_color){
            _render_set_next_frame_load_color(false)
         }
      } else {
         rendered = _render_thread_world_only_fast(APP_STATS)
      }
      if(rendered){
         _did_first_frame = true
         total_frames += 1
         frame_num += 1
      }
      if((APP_STATS || _fps_log_enabled == 1) && (frame_iter & 1023) == 0){
         def _discard_sample = _sample_fps_tick(ticks(), true)
      }
      frame_iter += 1
   }
   def rs = renderer_frame_stats()
   ui_profile.bench_flush(ticks() - view_start_t, rs, ui_profile.profile_dump_enabled(ui_profile.trace_enabled()) ? ui_profile.profile_dump_file() : "")
   _bench_print_summary("[fast-view] ", view_start_t, total_frames, rs, true, false, false)
   if(win && !should_close(win)){
      set_cursor_mode(win, CURSOR_NORMAL)
   }
   gfx.shutdown()
   if(win){ window.close(win) }
   true
}

fn _run_main_loop_fast_path() bool {
   if(_ui_fast_view_enabled() && _run_fast_view_loop()){
      return true
   }
   if(ui_profile.fast_surface_bench_enabled(_timeout_ns, _auto_dump_enabled, _batch_dump_enabled(), _gui_dump_suite_active)){
      _run_simulated_nosurface_bench_loop()
      return true
   }
   if(ui_profile.sim_nosurface_bench_enabled(_timeout_ns, _auto_dump_enabled, _batch_dump_enabled())){
      _run_simulated_nosurface_bench_loop()
      return true
   }
   if(ui_profile.fast_nosurface_bench_enabled(_timeout_ns, _auto_dump_enabled, _batch_dump_enabled())){
      _run_fast_nosurface_bench_loop()
      return true
   }
   false
}

fn _main_loop_startup_pump() {
   if(!_startup_pumped){
      poll_events()
      _startup_pumped = true
      if((!ui_profile.ui_bench_enabled()
         || !ui_profile.headless_enabled())
         && !(_batch_dump_enabled()
         && ui_profile.batch_fast_env_enabled())){
         def _discard_100 = _sync_window_size_from_live()
      }
      if(should_close(win)){
         if(_startup_trace_enabled()){
            ui_profile.print_text("[ui] startup post-pump: window requested close")
         }
      }
   }
}

fn _main_loop_reset_timers() {
   start_t = ticks()
   last_upd_t = start_t
   last_fps_t = start_t
   _last_frame_time = start_t
   _current_frame_time = 0.0
   _frame_time_accum = 0.0
   _render_phase = -1.0
}

fn _main_loop_trace_pre_enter() {
   if(should_close(win)){
      if(_startup_trace_enabled()){
         ui_profile.print_text("[ui:loop] pre-enter close flag is set")
      }
   }
   if(_startup_trace_enabled()){
      def _pre_win = get_win(win)
      mut local_should_close = "not-window"
      mut real_should_close = "not-window"
      if(is_dict(win)){
         local_should_close = win.get("should_close", "missing")
      }
      if(is_dict(_pre_win)){
         real_should_close = _pre_win.get("should_close", "missing")
      }
      ui_profile.print_text("[ui:loop] pre-enter closing=" + to_str(should_close(win)) +
         " local=" + to_str(local_should_close) +
         " real=" + to_str(real_should_close) +
         " id=0x" + to_hex(id(_pre_win)) +
      " headless=" + to_str(ui_profile.headless_enabled()))
   }
}

fn _main_loop_trace_once(msg) {
   if(_first_frame_trace_allowed() && _startup_trace_enabled()){
      ui_profile.print_text(msg)
   }
}

fn _main_loop_timeout_close(elapsed) bool {
   if(_timeout_ns <= 0 || _batch_dump_enabled() || elapsed < _timeout_ns ||
      (_auto_dump_enabled && !_did_first_frame)){
      return false
   }
   if(_auto_dump_enabled == 1){
      if(_auto_dump_ready_now()){
         _pending_auto_dump = true
         request_frame_capture()
         return true
      }
      return false
   }
   true
}

fn _batch_dump_check_model_timeout(now) {
   if(!_batch_dump_enabled() || _batch_dump_model_started_ns <= 0 ||
      _batch_dump_model_timeout_sec <= 0.0 ||
      _batch_dump_index < 0 || _batch_dump_index >= _batch_dump_models.len){
      return
   }
   def elapsed_batch_s = float(now - _batch_dump_model_started_ns) / 1e9
   if(elapsed_batch_s < _batch_dump_model_timeout_sec){
      return
   }
   def model_name = to_str(_batch_dump_models.get(_batch_dump_index, ""))
   def batch_path = ui_dump.snapshot_path(model_name, _batch_dump_dir, _cli_dump_dir)
   print(
      "[ui:batch] timeout snapshot idx=" + to_str(_batch_dump_index + 1)
      + "/" + to_str(_batch_dump_models.len)
      + " model=" + model_name
      + " elapsed=" + to_str(elapsed_batch_s)
      + " path=" + batch_path
   )
   gfx.snapshot(batch_path)
   _batch_dump_advance_or_exit()
}

fn _main_loop_call_update(dt) {
   _main_loop_trace_once("[ui:loop] update.before")
   try {
      app_update(dt)
   } catch err {
      ui_profile.eprint_text("[ui:panic] stage=update frame=" + to_str(total_frames) +
      " update_stage=" + _ui_update_stage)
      panic(err)
   }
   _main_loop_trace_once("[ui:loop] update.after")
}

fn _main_loop_gui_capture_requests(gui_now_frame) {
   _main_loop_gui_dump_path = ""
   _main_loop_want_auto_capture = false
   _main_loop_force_full_render = false
   mut want_gui_capture = false
   if(gui_now_frame && !_gui_auto_dump_done){
      _main_loop_gui_dump_path = ui_dump.gui_auto_dump_path(_cli_gui_auto_dump)
      want_gui_capture = _main_loop_gui_dump_path.len > 0 && total_frames >= ui_dump.gui_auto_dump_delay()
   }
   if(ui_profile.dump_trace_enabled() && total_frames < 18){
      def _discard_gui_dump_trace = ui_profile.print_text("[ui:gui-dump-state] frame=" + to_str(total_frames)
         + " gui_now=" + to_str(gui_now_frame)
         + " done=" + to_str(_gui_auto_dump_done)
         + " path_len=" + to_str(_main_loop_gui_dump_path.len)
         + " delay=" + to_str(ui_dump.gui_auto_dump_delay())
         + " want=" + to_str(want_gui_capture)
         + " suite=" + to_str(_gui_dump_suite_active_now())
         + " auto_enabled=" + to_str(_auto_dump_enabled)
      + " pending_auto=" + to_str(_pending_auto_dump))
   }
   mut want_gui_suite_capture = false
   if(_gui_dump_suite_active_now()){
      want_gui_suite_capture = _gui_dump_suite_wait_frames <= 0
   }
   mut want_batch_capture = false
   if(_batch_dump_enabled() && _batch_dump_wait_frames <= 0){
      want_batch_capture = _batch_dump_index >= 0 && _batch_dump_index < _batch_dump_models.len
   }
   if(_pending_auto_dump && !_auto_dump_done){
      _main_loop_want_auto_capture = _auto_dump_ready_now()
   }
   if(want_gui_capture || want_gui_suite_capture || want_batch_capture || _main_loop_want_auto_capture){
      request_frame_capture()
      _main_loop_force_full_render = true
   }
}

fn _main_loop_try_idle_present(update_t1, trace, gui_now_frame) bool {
   if(_main_loop_force_full_render){ return false }
   _main_loop_draw_t0 = update_t1
   _main_loop_trace_once("[ui:loop] idle-reuse.before")
   _main_loop_rendered = false
   try {
      _main_loop_rendered = try_present(gui_now_frame, _main_loop_want_auto_capture)
   } catch err {
      ui_profile.eprint_text("[ui:panic] stage=idle-reuse frame=" + to_str(total_frames))
      panic(err)
   }
   if(!_main_loop_rendered){
      return false
   }
   _note_first_frame_render_result(_main_loop_rendered)
   _main_loop_draw_t1 = trace ? ticks() : update_t1
   true
}

fn _main_loop_render_frame(frame_t0, update_t1, trace) {
   _main_loop_draw_t0 = update_t1
   _main_loop_trace_once("[ui:loop] render.before")
   _main_loop_rendered = false
   try {
      _main_loop_rendered = render_thread_obj()
   } catch err {
      ui_profile.eprint_text("[ui:panic] stage=" + _ui_update_stage +
      " frame=" + to_str(total_frames))
      panic(err)
   }
   _main_loop_trace_once("[ui:loop] render.after=" + to_str(_main_loop_rendered))
   _note_first_frame_render_result(_main_loop_rendered)
   _main_loop_draw_t1 = trace ? ticks() : update_t1
}

fn _main_loop_handle_deferred_skybox() {
   if(!_startup_skybox_pending){
      return
   }
   _startup_skybox_pending = false
   def sky_t0 = ticks()
   _load_fast_generated_skybox(ui_profile.visible_skybox_default())
   if(_startup_trace_enabled()){
      terminal.log("[startup] deferred_fast_skybox=" + to_str(ui_profile.elapsed_ms(sky_t0)) + "ms")
   }
}

fn _main_loop_maybe_queue_immediate_dump() {
   if(_auto_dump_enabled == 1 && !_auto_dump_done && !_pending_auto_dump &&
      _auto_dump_frame_counter >= _auto_dump_delay_frames &&
      _render_phase >= _auto_dump_min_elapsed_sec && _auto_dump_ready_now()){
      _pending_auto_dump = true
      request_frame_capture()
   }
}

fn _main_loop_record_timing(rendered, trace, frame_t0, draw_t0, draw_t1) {
   if(!rendered){
      _last_draw_ms = 0.0
      _last_frame_ms = 0.0
      return
   }
   _last_draw_ms = trace ? ui_profile.ms_between(draw_t1, draw_t0) : 0.0
   _last_frame_ms = trace ? ui_profile.ms_between(draw_t1, frame_t0) : 0.0
   if(trace){
      _frame_ms_samples = ui_app.app_push_hist_sample(_frame_ms_samples, _last_frame_ms)
      _draw_ms_samples = ui_app.app_push_hist_sample(_draw_ms_samples, _last_draw_ms)
      _world_ms_samples = ui_app.app_push_hist_sample(_world_ms_samples, _last_world_ms)
      _ui_ms_samples = ui_app.app_push_hist_sample(_ui_ms_samples, _last_ui_ms)
      _update_ms_samples = ui_app.app_push_hist_sample(_update_ms_samples, _last_update_ms)
   }
}

fn _main_loop_process_gui_dump(gui_now_frame, gui_dump_path) {
   if(_gui_dump_suite_active_now()){
      def _discard_102 = _gui_dump_suite_after_frame()
      return
   }
   if(!gui_now_frame || _gui_auto_dump_done || gui_dump_path.len <= 0 || total_frames < ui_dump.gui_auto_dump_delay()){
      return
   }
   if(_gui_auto_dump_attempts == 0){
      ui_profile.print_line("ui:gui-dump", "request path=" + gui_dump_path + " delay=" + to_str(ui_dump.gui_auto_dump_delay()))
   }
   _gui_auto_dump_attempts += 1
   if(_gui_auto_dump_attempts <= 2){
      ui_profile.print_line("ui:gui-dump", "attempt=" + to_str(_gui_auto_dump_attempts) + " path=" + gui_dump_path)
   }
   def ok = _snapshot_ok(gui_dump_path)
   if(ok){
      _gui_last_dump_path = gui_dump_path
      _gui_auto_dump_done = true
      ui_profile.print_line("ui:gui-dump", "path=" + gui_dump_path)
   } elif(_gui_auto_dump_attempts >= 8){
      _gui_auto_dump_done = true
      ui_profile.print_line("ui:gui-dump:fail", "path=" + gui_dump_path + " attempts=" + to_str(_gui_auto_dump_attempts))
   }
   if(_gui_auto_dump_done && ui_dump.gui_auto_dump_exit_enabled()){
      set_should_close(win, true)
   }
   return
}

fn _main_loop_process_batch_dump() {
   if(!_batch_dump_enabled()){
      return
   }
   if(_batch_dump_wait_frames > 0){
      _batch_dump_wait_frames -= 1
      return
   }
   if(_batch_dump_index < 0 || _batch_dump_index >= _batch_dump_models.len){
      return
   }
   def model_name = to_str(_batch_dump_models.get(_batch_dump_index, ""))
   def batch_path = ui_dump.snapshot_path(model_name, _batch_dump_dir, _cli_dump_dir)
   def snap_t0 = ticks()
   print(
      "[ui:batch] snapshot idx=" + to_str(_batch_dump_index + 1)
      + "/" + to_str(_batch_dump_models.len)
      + " model=" + model_name
      + " path=" + batch_path
   )
   def snapshot_ok = _snapshot_ok(batch_path)
   if(snapshot_ok){
      _batch_dump_completed_count += 1
      print(
         "[ui:batch] done idx=" + to_str(_batch_dump_index + 1)
         + "/" + to_str(_batch_dump_models.len)
         + " model=" + model_name
         + " snap_ms=" + str.to_fixed(ui_profile.elapsed_ms(snap_t0), 2)
         + " model_ms=" + str.to_fixed(ui_profile.stage_ms_since(_batch_dump_load_started_ns), 2)
         + " elapsed_s=" + str.to_fixed(asset_batch.elapsed_s(_batch_dump_run_started_ns), 2)
         + " eta_s=" + str.to_fixed(asset_batch.eta_s(_batch_dump_run_started_ns, _batch_dump_completed_count, _batch_dump_models.len), 1)
      )
      _batch_dump_advance_or_exit()
   } else {
      ui_profile.print_line("ui:batch:fail", "snapshot failed path=" + batch_path)
      print(
         "[ui:batch] failed idx=" + to_str(_batch_dump_index + 1)
         + "/" + to_str(_batch_dump_models.len)
         + " model=" + model_name
         + " snap_ms=" + str.to_fixed(ui_profile.elapsed_ms(snap_t0), 2)
      )
      _batch_dump_advance_or_exit()
   }
}

fn _main_loop_after_render_success(gui_now_frame, gui_dump_path) {
   _did_first_frame = true
   _auto_dump_frame_counter += 1
   if(_scene_load_async_thread != 0){
      def _discard_101 = _poll_scene_load_async()
   }
   _main_loop_handle_deferred_skybox()
   _main_loop_maybe_queue_immediate_dump()
   _main_loop_process_gui_dump(gui_now_frame, gui_dump_path)
   _main_loop_process_batch_dump()
}

fn _main_loop_profile_flush(trace) {
   ui_loop.record_frame_profile(
      bool(trace),
      int(total_frames),
      _last_update_ms,
      _last_draw_ms,
      _last_world_ms,
      _last_ui_ms,
      _last_frame_ms,
      _last_evt_ms,
      _last_gui_prep_ms,
   _last_sim_ms)
}

fn _main_loop_finish_auto_dump(rendered, want_auto_capture) {
   if(!rendered || !want_auto_capture || !_pending_auto_dump || _auto_dump_done){
      return
   }
   ui_profile.print_line(
      "ui:dump",
      "frame_counter=" + to_str(_auto_dump_frame_counter) +
      " total_frames=" + to_str(total_frames) +
      " path=" + _auto_dump_path
   )
   if(_snapshot_ok(_auto_dump_path)){
      if(ui_profile.env_truthy_cached("NY_UI_PRINT_FRAME_HASH") ||
         ui_profile.env_truthy_cached("NY_UI_EXPECT_FRAME_HASH") ||
         ui_profile.frame_hash_lock_enabled() ||
         str.find(_auto_dump_path, "fb_hash") >= 0){
         def _discard_103 = _ui_print_frame_hash(_auto_dump_path)
      }
      _auto_dump_done = true
      _pending_auto_dump = false
      if(_auto_dump_exit_mode == 1 || ui_profile.frame_hash_lock_enabled() || str.find(_auto_dump_path, "fb_hash") >= 0){
         __exit(0)
      }
      set_should_close(win, true)
   }
   if(ui_profile.frame_hash_lock_enabled() || str.find(_auto_dump_path, "fb_hash") >= 0){
      __exit(0)
   }
   return
}

fn _main_loop_finish_timeout(timeout_close) {
   if(!timeout_close){
      return
   }
   set_should_close(win, true)
   if(_auto_dump_enabled != 1 && _fps_log_enabled != 1){
      __exit(0)
   }
}

fn _main_loop_count_frame(rendered, now) {
   if(!rendered){
      return
   }
   frame_num += 1
   total_frames += 1
   def _discard_sample = _sample_fps_tick(now, true)
}

fn _main_loop_restore_cursor() {
   if(win && !should_close(win)){
      set_cursor_mode(win, CURSOR_NORMAL)
   }
}

fn _run_main_loop() {
   if(_run_main_loop_fast_path()){
      return
   }
   _main_loop_startup_pump()
   _main_loop_reset_timers()
   _main_loop_trace_pre_enter()
   while(!should_close(win)){
      if(_first_frame_trace_allowed() && _startup_trace_enabled()){
         ui_profile.print_text("[ui:loop] first-iter")
      }
      _main_loop_trace_once("[ui:loop] pump.before")
      poll_events()
      _main_loop_trace_once("[ui:loop] pump.after")
      def frame_t0 = ticks()
      def now = frame_t0
      mut dt = float(now - last_upd_t) / 1e9
      if(dt > 0.1){ dt = 0.016 }
      if(dt < 0.0001){ dt = 0.0001 }
      last_upd_t = now
      def elapsed = now - start_t
      def trace = ui_profile.trace_enabled()
      def timeout_close = _main_loop_timeout_close(elapsed)
      _batch_dump_check_model_timeout(now)
      _render_dt = dt
      _render_phase = float(elapsed) / 1e9
      mut gui_now_frame = _gui_enabled_now()
      _main_loop_call_update(dt)
      if(should_close(win)){
         _main_loop_trace_once("[ui:loop] close-after-update")
         break
      }
      def update_t1 = trace ? ticks() : frame_t0
      _last_update_ms = trace ? ui_profile.ms_between(update_t1, frame_t0) : 0.0
      gui_now_frame = _gui_enabled_now()
      _main_loop_maybe_queue_immediate_dump()
      _main_loop_gui_capture_requests(gui_now_frame)
      if(!_main_loop_try_idle_present(update_t1, trace, gui_now_frame)){
         _main_loop_render_frame(frame_t0, update_t1, trace)
         if(_main_loop_rendered){
            note_full_draw(gui_now_frame, _main_loop_want_auto_capture)
         }
      }
      if(_main_loop_rendered){
         _main_loop_after_render_success(gui_now_frame, _main_loop_gui_dump_path)
      }
      _main_loop_record_timing(_main_loop_rendered, trace, frame_t0, _main_loop_draw_t0, _main_loop_draw_t1)
      _main_loop_profile_flush(trace)
      _main_loop_finish_auto_dump(_main_loop_rendered, _main_loop_want_auto_capture)
      _main_loop_finish_timeout(timeout_close)
      _main_loop_count_frame(_main_loop_rendered, now)
   }
   _print_fps_summary(start_t, total_frames, true)
   _main_loop_restore_cursor()
   gfx.shutdown()
   if(win){ def _discard_close_window = window.close(win) }
}

fn run(any app=0) {
   "Run the demo UI frame loop."
   _run_main_loop()
}

fn draw_frame(any app=0) {
   "Draw one frame using the current shared UI state."
   draw(_render_phase, _term_open)
}

#main {
   if(_ui_has_help_arg()){ _ui_print_help() exit(0) }
   def app = create(from_env())
   startup(app)
   maybe_run(app)
   run(app)
}
