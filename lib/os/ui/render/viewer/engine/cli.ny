;; Keywords: engine cli args viewer options os ui render scene
;; Command-line option parsing for engine viewer startup and model loading.
;; References:
;; - std.os.args
;; - std.os.ui.assets.viewer
module std.os.ui.render.viewer.engine.cli(argv_list, parse_options, startup_plan, option_takes_value)
use std.core
use std.core.str as str

fn argv_list() list {
   "Returns process argv as a Nytrix list."
   mut out = []
   mut i = 0
   def n = argc()
   while(i < n){
      out = out.append(argv(i))
      i += 1
   }
   out
}

fn _value(args, i) str {
   (is_list(args) && i + 1 < args.len) ? str.strip(to_str(args.get(i + 1, ""))) : ""
}

fn _truthy_token(token) str {
   str.lower(str.strip(to_str(token)))
}

fn _name_in_list(name, items) bool {
   def needle = _truthy_token(name)
   for item in items {
      if(_truthy_token(item) == needle){ return true }
   }
   false
}

fn _startup_cmd_arity(cmd, one_arg_cmds) int {
   if(_name_in_list(cmd, one_arg_cmds)){ return 1 }
   (_truthy_token(cmd) == "bg") ? 3 : 0
}

fn option_takes_value(token_l) bool {
   "Returns true for CLI options that consume the next token."
   case _truthy_token(token_l){
      "--timeout", "--dump-path", "--dump-settle-frames", "--profile-dir", "--dump-dir",
      "--dump-models", "--dump-png-level", "--gui-shot", "--gui-layout",
      "--expect-frame-hash", "--expect-fb-hash", "--check-frame-hash", "--set",
      "--script", "-o", "--output" -> true
      _ -> false
   }
}

fn _default_options() dict {
   {
      "headless": false, "nosurface": false, "bench": false, "sim": false,
      "surfaced_headless": false, "nosurface_profile": false,
      "dump_requested": false, "frame_hash_requested": false,
      "dump_all": false, "dump_missing": false, "dump_path": "",
      "dump_dir": "", "batch_models_raw": "", "png_encode_level": -1,
      "gui_shot": "", "gui_probe": false, "gui_layout": "",
      "dump_delay_frames": -1, "timeout_sec": -1.0,
      "profile": false, "profile_frame_trace": false, "render_trace": false,
      "frame_trace": false, "frame_print_every": false, "profile_dir": "",
      "render_backend": "", "verbose": false, "help": false
   }
}

fn _set_headless(out, bool bench=false, bool sim=false) {
   out["headless"] = true
   out["nosurface"] = true
   out["bench"] = bench
   out["sim"] = sim
}

fn _parse_mode_option(out, str key) bool {
   if(key == "--headless"){
      _set_headless(out)
   } elif(key == "--compare-headless" || key == "--surfaced-headless"){
      out["surfaced_headless"] = true
   } elif(key == "--headless-sim" || key == "--real-headless"){
      _set_headless(out, true, true)
   } elif(key == "--nosurface-profile"){
      _set_headless(out, true, true)
      out["nosurface_profile"] = true
   } else {
      return false
   }
   true
}

fn _parse_switch_option(out, str key) bool {
   if(key == "-h" || key == "--help" || key == "help"){
      out["help"] = true
   } elif(key == "--dump"){
      out["dump_requested"] = true
   } elif(key == "--frame-hash" || key == "--fbhash"){
      out["dump_requested"] = true
      out["frame_hash_requested"] = true
   } elif(key == "--dump-all"){
      out["dump_all"] = true
   } elif(key == "--dump-missing"){
      out["dump_missing"] = true
   } elif(key == "--ms-profile" || key == "--msprofile" || key == "--profile-ms"){
      out["profile"] = true
   } elif(key == "--render-trace"){
      out["profile"] = true
      out["profile_frame_trace"] = true
      out["render_trace"] = true
      out["frame_print_every"] = true
   } elif(key == "--frame-trace"){
      out["frame_trace"] = true
   } elif(key == "-vk" || key == "--vk" || key == "-vulkan" || key == "--vulkan"){
      out["render_backend"] = "vk"
   } elif(key == "-gl" || key == "--gl" || key == "-opengl" || key == "--opengl"){
      out["render_backend"] = "gl"
   } elif(key == "-webgl" || key == "--webgl"){
      out["render_backend"] = "webgl"
   } elif(key == "-mock" || key == "--mock" || key == "-cpu" || key == "--cpu" || key == "-software" || key == "--software"){
      out["render_backend"] = "mock"
   } elif(key == "-v" || key == "--verbose" || key == "-vv" || key == "-vvv" || key == "--debug" || key == "--debug-deep" || key == "-trace" || key == "--trace" || key == "-trace-ui" || key == "--trace-ui" || key == "--trace-spam"){
      out["verbose"] = true
   } else {
      return false
   }
   true
}

fn _parse_value_option(args, int i, str key, out) int {
   if(i + 1 >= args.len){ return i }
   def raw = _value(args, i)
   if(key == "--timeout"){
      if(raw.len > 0){ out["timeout_sec"] = float(str.atof(raw)) }
   } elif(key == "--dump-path"){
      if(raw.len > 0){
         out["dump_path"] = raw
         out["dump_requested"] = true
      }
   } elif(key == "--dump-dir"){
      if(raw.len > 0){ out["dump_dir"] = raw }
   } elif(key == "--dump-models"){
      if(raw.len > 0){
         out["batch_models_raw"] = raw
         out["dump_all"] = true
      }
   } elif(key == "--dump-png-level"){
      if(raw.len > 0){ out["png_encode_level"] = int(str.atof(raw)) }
   } elif(key == "--gui-shot"){
      if(raw.len > 0){
         out["gui_shot"] = raw
         out["gui_probe"] = true
      }
   } elif(key == "--gui-layout"){
      if(raw.len > 0){ out["gui_layout"] = raw }
   } elif(key == "--dump-settle-frames"){
      if(raw.len > 0){ out["dump_delay_frames"] = int(str.atof(raw)) }
   } elif(key == "--profile-dir"){
      if(raw.len > 0){ out["profile_dir"] = raw }
   } else {
      return i
   }
   i + 1
}

fn parse_options(args) dict {
   "Parses viewer CLI flags into runtime option fields."
   mut post_cmds = []
   mut out = _default_options()
   if(!is_list(args)){ out["post_load_cmds"] = post_cmds return out }
   mut i = 1
   while(i < args.len){
      def key = _truthy_token(args.get(i, ""))
      def next_i = _parse_value_option(args, i, key, out)
      if(next_i != i){
         i = next_i
      } elif(_parse_mode_option(out, key) || _parse_switch_option(out, key)){
         0
      } elif(key == "--autofit"){
         post_cmds = post_cmds.append("autofit")
      } elif(key == "--lookat"){
         post_cmds = post_cmds.append("lookat")
      }
      i += 1
   }
   out["post_load_cmds"] = post_cmds
   out
}

fn startup_plan(args, one_arg_cmds=[]) dict {
   "Parses startup scene loads and command actions from argv."
   mut actions = []
   mut direct_scene = ""
   if(!is_list(args)){ return {"actions": actions, "direct_scene": direct_scene} }
   mut i = 1
   while(i < args.len){
      mut token = str.strip(to_str(args.get(i, "")))
      if(token.len == 0 || token == ";"){ i += 1 continue }
      def token_l = _truthy_token(token)
      def token_parts = str.split_words(token)
      if(token_parts.len > 1 && _truthy_token(token_parts.get(0, "")) == "load"){
         direct_scene = str.strip(str.join_words(token_parts, " ", 1))
         actions = actions.append({"kind": "scene", "value": direct_scene})
         i += 1
         continue
      }
      if(token_l == "load" && i + 1 < args.len){
         direct_scene = _value(args, i)
         actions = actions.append({"kind": "scene", "value": direct_scene})
         i += 2
         continue
      }
      if(token_l == "-ex" || token_l == "--ex"){
         if(i + 1 < args.len){ actions = actions.append({"kind": "cmd", "value": to_str(args.get(i + 1, ""))}) i += 2 continue }
         i += 1 continue
      }
      if(token_l == "-gltf" || token_l == "--gltf"){
         def spec = _value(args, i)
         if(spec.len > 0){ actions = actions.append({"kind": "cmd", "value": "load " + spec}) }
         i += (i + 1 < args.len) ? 2 : 1
         continue
      }
      if(token_l == "-vulkan" || token_l == "--vulkan" ||
         token_l == "-vk" || token_l == "--vk" ||
         token_l == "-gl" || token_l == "--gl" ||
         token_l == "-opengl" || token_l == "--opengl" ||
         token_l == "-webgl" || token_l == "--webgl" ||
         token_l == "-mock" || token_l == "--mock" ||
         token_l == "-cpu" || token_l == "--cpu" ||
         token_l == "-software" || token_l == "--software" ||
         token_l == "-v" || token_l == "--verbose" || token_l == "-vv" || token_l == "-vvv" || token_l == "--debug" ||
         token_l == "--debug-deep" || token_l == "-trace" || token_l == "--trace" || token_l == "-trace-ui" || token_l == "--trace-ui" ||
         token_l == "--trace-spam" || token_l == "-h" || token_l == "--help" || token_l == "help" ||
         token_l == "-gltf-debug" || token_l == "--gltf-debug"){
         i += 1
         continue
      }
      if(option_takes_value(token_l)){ i += 2 continue }
      if(str.find(token, "-") == 0){ i += 1 continue }
      if(str.find(token, ";") >= 0){
         actions = actions.append({"kind": "cmds", "value": token})
         i += 1
         continue
      }
      def want = _startup_cmd_arity(token_l, one_arg_cmds)
      mut line = token
      mut used = 0
      while(used < want && i + 1 + used < args.len){
         line = line + " " + to_str(args.get(i + 1 + used, ""))
         used += 1
      }
      actions = actions.append({"kind": "cmd", "value": line})
      i += 1 + used
   }
   {"actions": actions, "direct_scene": direct_scene}
}

#main {
   def args = ["ny", "--headless-sim", "--gl", "--timeout", "2.5", "--gui-shot", "probe", "load", "Avocado"]
   def opt = parse_options(args)
   def plan = startup_plan(args, ["load"])
   assert(bool(opt.get("headless", false)) && bool(opt.get("sim", false)) && float(opt.get("timeout_sec", 0.0)) == 2.5, "viewer cli options")
   def actions = plan.get("actions", [])
   assert(to_str(opt.get("render_backend", "")) == "gl", "viewer cli render backend")
   assert(to_str(opt.get("gui_shot", "")) == "probe" && bool(opt.get("gui_probe", false)) && to_str(plan.get("direct_scene", "")) == "Avocado", "viewer cli startup")
   assert(is_list(actions) && actions.len == 1 && actions.get(0, {}).get("kind", "") == "scene", "viewer cli action order")
   print("✓ std.os.ui.render.viewer.engine.cli self-test passed")
}
