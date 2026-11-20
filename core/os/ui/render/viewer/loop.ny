;; Keywords: viewer loop frame input render os ui
;; Frame loop orchestration for viewer windows, input polling, and render dispatch.
;; References:
;; - core.os.ui.render.viewer.runtime
;; - core.os.ui.window
module core.os.ui.render.viewer.loop(sample_fps, print_fps_summary, print_bench_summary, profile_dump_file, record_frame_profile)
use core
use core.math.stat as stat
use core.os.ui.render.viewer.app as ui_app
use core.os.ui.render.dump as render_profile

fn sample_fps(list samples, int frames, int last_tick, int now, int cap=256) dict {
   "Samples FPS once per second and appends it to history."
   ;; Typed ints are boxed before they enter the result: stored raw they read
   ;; back as nil, which surfaced as a "nil FPS" window title.
   mut dict out = dict(8)
   if now - last_tick < 1000000000 {
      out["samples"] = samples
      out["frames"] = __box_i64(frames)
      out["last"] = __box_i64(last_tick)
      out["fps"] = __box_i64(0)
      out["sampled"] = false
      return out
   }
   if frames > 0 { out["samples"] = ui_app.app_push_hist_sample(samples, float(frames), cap) }
   else { out["samples"] = samples }
   out["frames"] = __box_i64(0)
   out["last"] = __box_i64(now)
   out["fps"] = __box_i64(frames)
   out["sampled"] = true
   out
}

fn print_fps_summary(int log_enabled, int started_at, int total, list samples, bool show_median=false) bool {
   "Prints average and optional median FPS summary."
   if log_enabled != 1 || total <= 0 { return false }
   def elapsed_s = render_profile.elapsed_s(started_at)
   def avg = int(float(total) / elapsed_s)
   render_profile.print_text(f"[FPS] avg={avg}  frames={total}  time={elapsed_s:.2f}s")
   if show_median && samples.len > 0 {
      render_profile.print_text("[ui] fps median=" + to_str(int(stat.median(samples))) + " samples=" + to_str(samples.len))
   }
   true
}

fn profile_dump_file() str {
   "Returns the active profile dump path, if frame tracing is enabled."
   render_profile.profile_dump_enabled(render_profile.trace_enabled()) ? render_profile.profile_dump_file() : ""
}

fn _hot_stage(f64 up, f64 dr, f64 wo, f64 ui, f64 ev, f64 gp, f64 sm) str {
   mut name = "update"
   mut value = up
   if dr > value { name = "draw" value = dr }
   if wo > value { name = "world" value = wo }
   if ui > value { name = "ui" value = ui }
   if ev > value { name = "events" value = ev }
   if gp > value { name = "prep" value = gp }
   if sm > value { name = "sim" value = sm }
   name + "=" + to_str(value) + "ms"
}

fn _budget_hint(f64 frame_ms, f64 draw_ms, f64 ui_ms, f64 event_ms) str {
   def budget = float(render_profile.env_int_cached("NY_UI_FRAME_BUDGET_MS", 16, 1, 1000))
   if frame_ms <= budget { return "ok" }
   if event_ms >= draw_ms && event_ms >= ui_ms { return "events: coalesce mouse/paste or lower per-frame event budget" }
   if ui_ms >= draw_ms { return "ui: reduce visible widgets/text or enable idle reuse" }
   "draw: inspect renderer batches/textures or switch backend/vsync"
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
   if !trace { return false }
   render_profile.frame_record(
      last_update_ms,
      last_draw_ms,
      last_world_ms,
      last_ui_ms,
      last_frame_ms,
      last_evt_ms,
      last_gui_prep_ms,
      last_sim_ms)
   def nprint = render_profile.frame_print_every()
   if render_profile.frame_samples() < nprint { return false }
   def n = render_profile.frame_samples()
   def avg_up = render_profile.frame_avg("update_ms")
   def avg_dr = render_profile.frame_avg("draw_ms")
   def avg_wo = render_profile.frame_avg("world_ms")
   def avg_ui = render_profile.frame_avg("ui_ms")
   def avg_fr = render_profile.frame_avg("frame_ms")
   def avg_ev = render_profile.frame_avg("event_ms")
   def avg_gp = render_profile.frame_avg("gui_prep_ms")
   def avg_sm = render_profile.frame_avg("sim_ms")
   def fps_est = render_profile.frame_fps("frame_ms")
   mut deep_profile_msg = ""
   if render_profile.deep_enabled() {
      deep_profile_msg = " evt=" + to_str(avg_ev) +
      "ms prep=" + to_str(avg_gp) +
      "ms sim=" + to_str(avg_sm) + "ms"
   }
   def hot = _hot_stage(avg_up, avg_dr, avg_wo, avg_ui, avg_ev, avg_gp, avg_sm)
   def hint = _budget_hint(avg_fr, avg_dr, avg_ui, avg_ev)
   render_profile.print_text("[frame] fps~" + to_str(__flt_to_int(fps_est + 0.5)) +
      " frame=" + to_str(avg_fr) + "ms" +
      " upd=" + to_str(avg_up) + "ms" +
      " draw=" + to_str(avg_dr) + "ms" +
      " world=" + to_str(avg_wo) + "ms" +
      " ui=" + to_str(avg_ui) + "ms" +
      " hot=" + hot +
      " hint=" + hint +
      deep_profile_msg)
   render_profile.profile_dump_row(total_frames, n, fps_est, avg_fr, avg_up, avg_dr, avg_wo, avg_ui, avg_ev, avg_gp, avg_sm)
   render_profile.frame_reset()
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
   if !print_fps_summary(log_enabled, started_at, total, samples, show_median) { return false }
   render_profile.print_text(prefix + ui_app.app_renderer_stats_line(renderer_stats, with_verts, with_desc))
   true
}

#main {
   def s0 = sample_fps([], 12, 0, 1000000000)
   assert(bool(s0.get("sampled", false)) && int(s0.get("fps", 0)) == 12, "fps sample")
   assert(!print_fps_summary(0, 0, 0, []), "fps summary disabled")
   print("✓ core.os.ui.render.viewer.loop self-test passed")
}
