;; Keywords: fixture budget frames timeout keys smoke harness interactive os ui server
;; Bounded-fixture protocol shared by terminal, window, and daemon examples.
;; References:
;; - core.os
;; - core.term
;; Documentation:
;; ## Protocol
;; Interactive programs run under one explicit, harness-controllable budget
;; instead of per-app environment escape hatches or hidden exits in runtime
;; loops.  An app joins the protocol by driving its frame loop through this
;; module:
;;
;; - **Startup:** `start` and `start_ui` resolve the whole budget once. An
;;   explicit positive CLI positional wins over `NYTRIX_FIXTURE_FRAMES`, which
;;   wins over the app default. A non-interactive terminal run or mock-renderer
;;   run defaults to one frame. A zero frame limit is unbounded.
;; - **Frame budget:** `tick` and `tick_ui` admit one frame per truthy return.
;; - **Timeout:** `NYTRIX_FIXTURE_TIMEOUT_MS` bounds the wall-clock run
;;   (0 = unbounded) even when the frame budget is unbounded.
;; - **Frame delay:** `delay` sleeps the per-frame pause
;;   (`NYTRIX_FIXTURE_FRAME_MS` over the app default) in interactive runs
;;   only; piped runs never sleep.
;; - **Input injection:** `NYTRIX_FIXTURE_KEYS` (comma-separated key codes,
;;   e.g. "113,27") supplies one key per frame. Terminal `tick` consumes
;;   injected keys before polling the TTY; GUI apps use `next_ui_key`.
;; - **Readiness/status:** call `ready` only after the UI window or server bind
;;   succeeds. A bounded frame/request completion exits successfully;
;;   `exit_status` returns 124 on a wall timeout. Apps close resources before
;;   returning that status.
module core.os.fixture(start, start_ui, tick, tick_ui, next_ui_key, delay, is_interactive, interactive, quit_key, ready, exit_status, canvas_loop)
use core
use core.common as common
use core.str
use core.term as cli_term
use core.os.args as cli
use core.os.time as time

fn interactive() bool {
   "Returns whether stdin and stdout are both attached to a terminal."
   __tty_isatty(0) != 0 && __tty_isatty(1) != 0
}

fn quit_key(any key) bool {
   "Returns true for the shared TUI quit keys(ESC, Ctrl+C, q, Q)."
   key == 27 || key == 3 || key == 113 || key == 81
}

fn _injected_keys() list {
   "Parses NYTRIX_FIXTURE_KEYS as comma-separated key codes."
   mut keys = list(8)
   def raw = common.env_trim("NYTRIX_FIXTURE_KEYS")
   if raw.len == 0 { return keys }
   def parts = str.split(raw, ",")
   mut i = 0
   while i < parts.len {
      keys = keys.append(common.parse_nonneg_int(parts.get(i)))
      i += 1
   }
   keys
}

fn _state(int max_frames, int timeout_ms, int frame_ms, bool live) list {
   mut fx = list(9)
   fx = fx.append(max_frames) ;; 0: frame budget, 0 = unbounded
   fx = fx.append(0) ;; 1: frames admitted so far
   fx = fx.append(timeout_ms) ;; 2: timeout budget (ms)
   fx = fx.append(__tag(time.ticks())) ;; 3: tag raw ticks for the any-list slot
   fx = fx.append(frame_ms) ;; 4: frame delay (ms)
   fx = fx.append(live ? 1 : 0) ;; 5: interactive
   fx = fx.append(_injected_keys()) ;; 6: scripted keys
   fx = fx.append(0) ;; 7: next scripted-key index
   fx = fx.append(0) ;; 8: stop reason (0 running, 1 budget, 2 timeout, 3 input)
   fx
}

fn start(int def_frames=0, int def_frame_ms=0, int def_timeout_ms=0) list {
   "Resolves the bounded-fixture budget for one interactive run and returns
   the fixture state consumed by tick, delay, and is_interactive.  An
   explicit positive CLI argument is the frame count ; otherwise
   NYTRIX_FIXTURE_FRAMES applies ; otherwise the app default, and a
   non-interactive run renders a single frame."
   def live = interactive()
   def want = cli.first_positive_int()
   mut max_frames = def_frames
   if want > 0 {
      max_frames = want
   } elif common.env_present("NYTRIX_FIXTURE_FRAMES") {
      max_frames = common.env_int_clamped("NYTRIX_FIXTURE_FRAMES", def_frames, 0, 100000000)
   } elif !live {
      max_frames = 1
   }
   def timeout = common.env_int_clamped("NYTRIX_FIXTURE_TIMEOUT_MS", def_timeout_ms, 0, 3600000)
   def frame_ms = common.env_int_clamped("NYTRIX_FIXTURE_FRAME_MS", def_frame_ms, 0, 3600000)
   _state(max_frames, timeout, frame_ms, live)
}

fn start_ui(int def_frames=0, int def_timeout_ms=0, bool headless=false) list {
   "Resolves frame and wall-time budgets for a GUI example. `headless` selects a one-frame default."
   def want = cli.first_positive_int()
   mut max_frames = def_frames
   if want > 0 {
      max_frames = want
   } elif common.env_present("NYTRIX_FIXTURE_FRAMES") {
      max_frames = common.env_int_clamped("NYTRIX_FIXTURE_FRAMES", def_frames, 0, 100000000)
   } elif headless && max_frames == 0 {
      max_frames = 1
   }
   def timeout = common.env_int_clamped("NYTRIX_FIXTURE_TIMEOUT_MS", def_timeout_ms, 0, 3600000)
   _state(max_frames, timeout, 0, interactive())
}

fn _next_scripted_key(list fx) int {
   "Returns the next injected key, or -1 if the script has no key for this frame."
   def keys = fx.get(6)
   def pos = fx.get(7)
   if pos < keys.len {
      fx.set(7, pos + 1)
      return keys.get(pos)
   }
   -1
}

fn next_ui_key(list fx) int {
   "Consumes one scripted UI key without reading or stealing a real window event."
   _next_scripted_key(fx)
}

fn _next_terminal_key(list fx) int {
   def scripted = _next_scripted_key(fx)
   if scripted >= 0 { return scripted }
   __poll_key()
}

fn _admit_frame(list fx) int {
   if fx.get(0) > 0 && fx.get(1) >= fx.get(0) {
      fx.set(8, 1)
      return 0
   }
   if fx.get(2) > 0 && time.ticks() - fx.get(3) >= fx.get(2) * 1000000 {
      fx.set(8, 2)
      return 0
   }
   fx.set(1, fx.get(1) + 1)
   1
}

;; Keep frame admission and key polling out of callback-bearing callers.
@noinline
fn tick(list fx) int {
   "Admits one terminal frame; injected or physical ESC, Ctrl-C, q, or Q ends the run."
   if !_admit_frame(fx) { return 0 }
   if quit_key(_next_terminal_key(fx)) {
      fx.set(8, 3)
      fx.set(1, max(0, fx.get(1) - 1))
      return 0
   }
   1
}

fn tick_ui(list fx) int {
   "Admits one GUI frame, without polling terminal input or consuming window events."
   _admit_frame(fx)
}

fn delay(list fx) int {
   "Sleeps the per-frame pause for interactive runs; piped runs never sleep."
   if fx.get(5) != 0 && fx.get(4) > 0 { time.msleep(fx.get(4)) }
   0
}

fn is_interactive(list fx) int {
   "Returns whether the fixture detected an interactive(TTY) run."
   fx.get(5)
}

fn ready(str name="app") int {
   "Emits the machine-readable readiness marker when the harness asks for it."
   if common.env_truthy("NYTRIX_FIXTURE_READY") {
      print("NYTRIX_FIXTURE_READY " + name)
      __print_flush()
   }
   0
}

fn exit_status(list fx) int {
   "Returns 124 after the configured wall timeout, otherwise 0."
   fx.get(8) == 2 ? 124 : 0
}

fn canvas_loop(fnptr draw, int def_frames=0, int def_frame_ms=0, int def_timeout_ms=0) int {
   "Runs a resize-aware TUI canvas frame loop under the bounded-fixture
   protocol and returns the fixture exit status(124 after a wall timeout).
   core.term owns the TTY and canvas policy(alternate screen, raw
   mode, resize polling, cleanup) ; the fixture owns admission, scripted
   and physical quit keys, per-frame delay, and the whole-run deadline.
   Calls draw(canvas, zbuf, width, height) once per admitted frame.
   The loop owns its fixture state; callers supply only the draw callback and
   frame policy."
   def fx = start(def_frames, def_frame_ms, def_timeout_ms)
   def interactive = __tty_isatty(0) != 0 && __tty_isatty(1) != 0
   if interactive { cli_term.tui_begin() }
   defer { if interactive { cli_term.tui_end() } }
   mut s = cli_term.get_terminal_size()
   mut w, h = s[0], s[1] - 1
   if w < 1 { w = 80 }
   if h < 1 { h = 23 }
   if w > 300 { w = 300 }
   if h > 120 { h = 120 }
   mut c, z = cli_term.canvas(w, h), [0.0] * w * h
   mut frames = 0
   while tick(fx) {
      ;; Terminal-size probes are comparatively expensive and some pseudo-TTYs
      ;; report alternating row values.  Check periodically so a flaky size
      ;; query cannot turn every frame into a full-screen clear storm.
      if frames % 30 == 0 {
         s = cli_term.get_terminal_size()
         if w != s[0] || h != s[1] - 1 {
            w, h = s[0], s[1] - 1
            if w < 1 { w = 80 }
            if h < 1 { h = 23 }
            if w > 300 { w = 300 }
            if h > 120 { h = 120 }
            cli_term.canvas_free(c)
            c, z = cli_term.canvas(w, h), [0.0] * w * h
            cli_term.clear_screen()
         }
      }
      draw(c, z, w, h)
      ;; Pace only the interactive canvas; headless fixture runs stay fast.
      if interactive && fx.get(4) > 0 { time.msleep(fx.get(4)) }
      frames += 1
   }
   ;; Resize replaces the handle, so release the current canvas rather than a
   ;; handle captured by a defer before the first resize.
   cli_term.canvas_free(c)
   exit_status(fx)
}
