;; Keywords: viewer loop frame input render os ui
;; Frame loop orchestration for viewer windows, input polling, and render dispatch.
;; References:
;; - std.os.ui.render.viewer.runtime
;; - std.os.ui.window
module std.os.ui.render.viewer.loop(sample_fps, print_fps_summary, print_bench_summary, profile_dump_file, record_frame_profile)
use std.core
use std.math.crypto.stat as stat
use std.os.ui.render.viewer.app as ui_app
use std.os.ui.render.dump as ui_profile

fn sample_fps(list samples, int frames, int last_tick, int now, int cap=256) dict {
   "Samples FPS once per second and appends it to history."
   if(now - last_tick < 1000000000){
      return {"samples": samples, "frames": frames, "last": last_tick, "fps": 0, "sampled": false}
   }
   def fps = frames
   {
      "samples": fps > 0 ? ui_app.app_push_hist_sample(samples, float(fps), cap) : samples,
      "frames": 0,
      "last": now,
      "fps": fps,
      "sampled": true
   }
}

fn print_fps_summary(int log_enabled, int started_at, int total, list samples, bool show_median=false) bool {
   "Prints average and optional median FPS summary."
   if(log_enabled != 1 || total <= 0){ return false }
   def elapsed_s = ui_profile.elapsed_s(started_at)
   def avg = int(float(total) / elapsed_s)
   ui_profile.print_text(f"[FPS] avg={avg}  frames={total}  time={elapsed_s:.2f}s")
   if(show_median && samples.len > 0){
      ui_profile.print_text("[ui] fps median=" + to_str(int(stat.median(samples))) + " samples=" + to_str(samples.len))
   }
   true
}

fn profile_dump_file() str {
   "Returns the active profile dump path, if frame tracing is enabled."
   ui_profile.profile_dump_enabled(ui_profile.trace_enabled()) ? ui_profile.profile_dump_file() : ""
}

fn record_frame_profile(
   bool trace,
   int total_frames,
   f64 last_update_ms,
   f64 last_draw_ms,
   f64 last_world_ms,
   f64 last_ui_ms,
   f64 last_frame_ms,
   f64 last_evt_ms,
   f64 last_gui_prep_ms,
   f64 last_sim_ms
) bool {
   "Records frame timings and prints the configured aggregate profile line."
   if(!trace){ return false }
   ui_profile.frame_record(
      last_update_ms,
      last_draw_ms,
      last_world_ms,
      last_ui_ms,
      last_frame_ms,
      last_evt_ms,
      last_gui_prep_ms,
   last_sim_ms)
   def nprint = ui_profile.frame_print_every()
   if(ui_profile.frame_samples() < nprint){ return false }
   def n = ui_profile.frame_samples()
   def avg_up = ui_profile.frame_avg("update_ms")
   def avg_dr = ui_profile.frame_avg("draw_ms")
   def avg_wo = ui_profile.frame_avg("world_ms")
   def avg_ui = ui_profile.frame_avg("ui_ms")
   def avg_fr = ui_profile.frame_avg("frame_ms")
   def avg_ev = ui_profile.frame_avg("event_ms")
   def avg_gp = ui_profile.frame_avg("gui_prep_ms")
   def avg_sm = ui_profile.frame_avg("sim_ms")
   def fps_est = ui_profile.frame_fps("frame_ms")
   mut deep_profile_msg = ""
   if(ui_profile.deep_enabled()){
      deep_profile_msg = " evt=" + to_str(avg_ev) +
      "ms prep=" + to_str(avg_gp) +
      "ms sim=" + to_str(avg_sm) + "ms"
   }
   ui_profile.print_text("[frame] fps~" + to_str(__flt_to_int(fps_est + 0.5)) +
      " frame=" + to_str(avg_fr) + "ms" +
      " upd=" + to_str(avg_up) + "ms" +
      " draw=" + to_str(avg_dr) + "ms" +
      " world=" + to_str(avg_wo) + "ms" +
      " ui=" + to_str(avg_ui) + "ms" +
   deep_profile_msg)
   ui_profile.profile_dump_row(total_frames, n, fps_est, avg_fr, avg_up, avg_dr, avg_wo, avg_ui, avg_ev, avg_gp, avg_sm)
   ui_profile.frame_reset()
   true
}

fn print_bench_summary(
   str prefix,
   int log_enabled,
   int started_at,
   int total,
   list samples,
   dict renderer_stats,
   bool show_median=false,
   bool with_verts=true,
   bool with_desc=true
) bool {
   "Prints FPS summary followed by renderer benchmark stats."
   if(!print_fps_summary(log_enabled, started_at, total, samples, show_median)){ return false }
   ui_profile.print_text(prefix + ui_app.app_renderer_stats_line(renderer_stats, with_verts, with_desc))
   true
}

#main {
   def s0 = sample_fps([], 12, 0, 1000000000)
   assert(bool(s0.get("sampled", false)) && int(s0.get("fps", 0)) == 12, "fps sample")
   assert(!print_fps_summary(0, 0, 0, []), "fps summary disabled")
   print("✓ std.os.ui.render.viewer.loop self-test passed")
}
