;; Keywords: assets viewer gltf fonts skybox catalog os ui
;; Viewer asset lookup, font discovery, GLTF catalog loading, and skybox resolution.
;; References:
;; - std.os.ui.assets.catalog
;; - std.os.ui.render.scene
module std.os.ui.assets.viewer(
   MONO_FONT_CANDIDATES, EDITOR_FONT_CANDIDATES, UI_FONT_CANDIDATES, TERM_FONT_DEFAULT, TERM_FONT_CANDIDATES,
   mono_font, editor_font, ui_font,
   gltf_asset_root_envs, gltf_asset_roots, gltf_asset_catalog, resolve_gltf_asset_path,
   list_gltf_asset_names, prefetch_gltf_asset, load_named_scene,
   icon_dirs, icon_path,
   skybox_env_size, skybox_source_arg, skybox_source_is_off, skybox_source_is_generated,
   skybox_source_is_default_asset, skybox_resolve_source_path,
   load_env_skybox
)

use std.core
use std.core.common as common
use std.core.str as str
use std.os.fs as osfs
use std.os.path as ospath
use std.os.ui.assets.catalog as asset_catalog
use std.os.ui.render.viewer.runtime as ui_runtime
use std.os.ui.render.scene as scene_engine

def list MONO_FONT_CANDIDATES = [
   "etc/assets/fonts/monocraft.ttf",
   "etc/assets/fonts/jetbrains.ttf",
   "/usr/share/fonts/TTF/JetBrainsMonoNerdFontMono-Regular.ttf",
   "/usr/share/fonts/TTF/JetBrainsMonoNerdFont-Regular.ttf",
   "/usr/share/fonts/TTF/FiraCode-Regular.ttf",
   "/usr/share/fonts/TTF/DejaVuSansMono.ttf"
]

def list EDITOR_FONT_CANDIDATES = [
   "etc/assets/fonts/maplemono.ttf",
   "etc/assets/fonts/JetBrainsMono-Regular.ttf",
   "etc/assets/fonts/jetbrains.ttf",
   "etc/assets/fonts/MapleMono-NF-Regular.ttf",
   "/usr/share/fonts/TTF/JetBrainsMonoNerdFontMono-Regular.ttf",
   "/usr/share/fonts/TTF/JetBrainsMonoNerdFont-Regular.ttf",
   "/usr/share/fonts/TTF/FiraCode-Regular.ttf",
   "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
   "etc/assets/fonts/monocraft.ttf"
]

def str TERM_FONT_DEFAULT = "/usr/share/fonts/TTF/DejaVuSansMono.ttf"
def list TERM_FONT_CANDIDATES = [
   "/usr/share/fonts/TTF/JetBrainsMonoNerdFontMono-Regular.ttf",
   "/usr/share/fonts/TTF/JetBrainsMonoNLNerdFontMono-Regular.ttf",
   "/usr/share/fonts/TTF/MesloLGSNerdFontMono-Regular.ttf",
   "/usr/share/fonts/OTF/FiraMonoNerdFontMono-Regular.otf",
   "etc/assets/fonts/jetbrains.ttf",
   "etc/assets/fonts/monocraft.ttf",
   "/usr/share/fonts/TTF/DejaVuSansMono.ttf"
]

def list UI_FONT_CANDIDATES = [
   "etc/assets/fonts/jetbrains.ttf",
   "etc/assets/fonts/maplemono.ttf",
   "etc/assets/fonts/monocraft.ttf",
   "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
   "/usr/share/fonts/TTF/DejaVuSans.ttf",
   "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
   "/usr/share/fonts/noto/NotoSans-Regular.ttf",
   "/usr/share/fonts/TTF/Arial.ttf",
   "/usr/share/fonts/TTF/DejaVuSansMono.ttf"
]

fn mono_font(int size, candidates=0, int font_filter=-1) int {
   "Runs the mono font operation."
   ui_runtime.mono_font(size, common.value_or(candidates, MONO_FONT_CANDIDATES), font_filter)
}

fn editor_font(int size, candidates=0, int font_filter=-1) int {
   "Runs the editor font operation."
   mono_font(size, common.value_or(candidates, EDITOR_FONT_CANDIDATES), font_filter)
}

fn ui_font(int size, candidates=0, int font_filter=-1) int {
   "Runs the font operation."
   ui_runtime.ui_font_from_candidates(size, common.value_or(candidates, UI_FONT_CANDIDATES), EDITOR_FONT_CANDIDATES, font_filter)
}

mut _gltf_asset_catalog_cache = 0

fn gltf_asset_root_envs() list {
   "Runs the asset root envs operation."
   ["NY_UI_GLTF_ROOTS", "NY_UI_GLTF_ROOT", "NY_GLTF_ASSET_ROOT", "NY_GLTF_ROOT"]
}

fn gltf_asset_roots(any defaults=["tmp/assets/models"]) list {
   "Runs the asset roots operation."
   asset_catalog.asset_dirs_from_env(defaults, gltf_asset_root_envs())
}

fn gltf_asset_catalog() dict {
   "Runs the asset catalog operation."
   if is_dict(_gltf_asset_catalog_cache) { return _gltf_asset_catalog_cache }
   _gltf_asset_catalog_cache = asset_catalog.gltf_catalog_make(gltf_asset_roots())
   _gltf_asset_catalog_cache
}

fn resolve_gltf_asset_path(any spec) str {
   "Resolves a glTF asset spec(name, relative path, or absolute path) to a file path."
   def raw = str.strip(to_str(spec))
   if raw.len == 0 { return "" }
   asset_catalog.gltf_catalog_resolve(gltf_asset_catalog(), raw)
}

fn list_gltf_asset_names(int limit=24) list {
   "Lists glTF asset names from configured asset roots."
   asset_catalog.gltf_catalog_names(gltf_asset_catalog(), limit)
}

fn prefetch_gltf_asset(any spec) bool {
   "Preloads and parses a glTF/GLB asset in the background."
   def gltf_path = resolve_gltf_asset_path(spec)
   if gltf_path.len == 0 { return false }
   scene_engine.prefetch_gltf_asset_path(gltf_path)
   true
}

fn load_named_scene(any spec, any cam3d=0, any M_SP=0, any M_PT=0, any M_PS=0) any {
   "Resolves and loads a glTF asset spec."
   def gltf_path = resolve_gltf_asset_path(spec)
   if gltf_path.len == 0 { return 0 }
   scene_engine.load_scene_mesh(gltf_path, scene_engine.scene_asset_name_from_gltf_path(gltf_path), cam3d, M_SP, M_PT, M_PS, 0)
}

fn icon_dirs(any defaults=["etc/assets/images/icons"], any env_names=["NY_UI_ICON_DIRS", "NY_UI_ICON_DIR", "NY_UI_ICON_ROOT"]) list {
   "Returns existing icon directories from env values plus defaults."
   asset_catalog.asset_dirs_from_env(defaults, env_names)
}

fn icon_path(any name, any dirs=0, str ext=".svg", str fallback_name="tree") str {
   "Resolves an icon name against configured icon directories."
   def key = str.lower(str.strip(to_str(name)))
   if key.len == 0 { return "" }
   def compact = str.str_replace(key, "_", "")
   def roots = is_list(dirs) ? dirs : icon_dirs()
   mut i = 0
   while i < roots.len {
      def root = to_str(roots[i])
      def direct = ospath.join(root, key + ext)
      if osfs.is_file(direct) { return direct }
      if compact != key {
         def compact_path = ospath.join(root, compact + ext)
         if osfs.is_file(compact_path) { return compact_path }
      }
      if key == "node" && fallback_name.len > 0 {
         def fallback = ospath.join(root, fallback_name + ext)
         if osfs.is_file(fallback) { return fallback }
      }
      i += 1
   }
   ""
}

fn _skybox_env_source(str env_name, str default_rel_path) str {
   mut raw = common.env_trim(env_name)
   if raw.len == 0 && env_name != "NY_DEMO_SKYBOX" {
      raw = common.env_trim("NY_DEMO_SKYBOX")
   }
   if raw.len == 0 { return "" }
   def low = str.lower(raw)
   case low {
      "0", "false", "off", "no" -> { return "" }
      "1", "true", "on", "yes" -> { return default_rel_path }
      _ -> {}
   }
   raw
}

fn skybox_env_size(any fast_batch_env=false) list {
   "Runs the skybox env size operation."
   def env_w = bool(fast_batch_env) ? 128 : 1024
   [env_w, max(1, env_w / 2)]
}

fn skybox_source_arg(any source="", str source_env="NY_UI_SKYBOX_SOURCE", str path_env="NY_UI_SKYBOX_PATH") str {
   "Runs the skybox source arg operation."
   mut raw = str.strip(to_str(source))
   if raw.len == 0 { raw = common.env_trim(source_env) }
   if raw.len == 0 { raw = common.env_trim(path_env) }
   raw
}

fn skybox_source_is_off(any source) bool {
   "Runs the skybox source is off operation."
   def raw = str.lower(str.strip(to_str(source)))
   raw == "0" || raw == "false" || raw == "off" || raw == "none" || raw == "disable" || raw == "disabled"
}

fn skybox_source_is_generated(any source) bool {
   "Runs the skybox source is generated operation."
   def raw = str.lower(str.strip(to_str(source)))
   raw == "generated" || raw == "fast" || raw == "fallback" || raw == "procedural"
}

fn skybox_source_is_default_asset(any source) bool {
   "Runs the skybox source is default asset operation."
   def raw = str.lower(str.strip(to_str(source)))
   raw.len == 0 || raw == "default" || raw == "asset" || raw == "real" || raw == "exr" || raw == "day" || raw == "daysky"
}

fn skybox_resolve_source_path(any source="", str default_rel_path="etc/assets/images/DaySkyHDRI041B_4K_HDR.exr", str default_path="") str {
   "Runs the skybox resolve source path operation."
   def raw = skybox_source_arg(source)
   if skybox_source_is_default_asset(raw) {
      def resolved_default = str.strip(to_str(default_path))
      if resolved_default.len > 0 { return resolved_default }
      def path = ospath.resolve_repo_asset(default_rel_path)
      if osfs.is_file(path) { return path }
      return path
   }
   if osfs.is_file(raw) { return raw }
   def repo_path = ospath.resolve_repo_asset(raw)
   if repo_path.len > 0 && osfs.is_file(repo_path) { return repo_path }
   raw
}

fn load_env_skybox(default_rel_path="etc/assets/images/DaySkyHDRI041B_4K_HDR.exr", env_name="NY_UI_SKYBOX") int {
   "Loads an optional equirectangular skybox from `env_name`; truthy values use `default_rel_path`, path values load that path."
   def source = _skybox_env_source(to_str(env_name), to_str(default_rel_path))
   if source.len == 0 { return -1 }
   def path = ospath.resolve_repo_asset(source)
   if !osfs.is_file(path) {
      ui_runtime.dbg("skybox", "missing equirect skybox: " + path)
      return -1
   }
   def tex = ui_runtime.load_equirect_skybox(path)
   if tex >= 0 { ui_runtime.dbg("skybox", "loaded equirect skybox tex=" + to_str(tex)) }
   else { ui_runtime.dbg("skybox", "failed to load equirect skybox: " + path) }
   tex
}

#main {
   assert(MONO_FONT_CANDIDATES.len > 0 && EDITOR_FONT_CANDIDATES.len > 0 && UI_FONT_CANDIDATES.len > 0, "ui asset font candidates")
   assert(icon_path("", []) == "" && icon_dirs(["/definitely/missing"], ["NY_UI_ASSET_TEST_MISSING"]).len == 0, "ui asset icon helpers")
   assert(_skybox_env_source("NY_UI_ASSET_TEST_MISSING", "fallback.exr") == "", "ui asset skybox disabled by missing env")
   assert(skybox_env_size(true).get(0) == 128 && skybox_source_is_off("off") && skybox_source_is_generated("fast"), "ui asset skybox helpers")
   print("✓ std.os.ui.assets.viewer self-test passed")
}
