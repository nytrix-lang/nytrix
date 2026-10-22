/*
 * TTY progress bar: renders a compact, rate-limited progress display
 * with elapsed time and throughput estimates for long compilation phases.
 */
#include "base/progress.h"
#include "base/common.h"
#include "base/compat.h"
#include "base/util.h"
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#ifndef _WIN32
#include <pthread.h>
#include <unistd.h>
#endif

#define NY_PROGRESS_MAX_NODES 16
#define NY_PROGRESS_BAR_CAP 30

#define RATE_EMA_ALPHA 0.25

typedef struct {
  char name[64];
  long completed;
  long total;
  int active;
  ny_tick_t started_at;
} ny_progress_slot_t;

static ny_progress_slot_t g_progress[NY_PROGRESS_MAX_NODES];
static char g_progress_root[64];
static int g_progress_enabled = 0;
static int g_progress_running = 0;
static int g_progress_forced = 0;
#ifndef _WIN32
/*
 * ANSI is a presentation protocol for an interactive terminal, never for a
 * redirected log.  Keeping this separate from `g_progress_enabled` lets an
 * explicit NYTRIX_PROGRESS request retain a useful final summary without
 * leaking control bytes into pipes and captured test output.
 */
static int g_progress_ansi = 0;
#endif
static ny_tick_t g_progress_start_tick = 0;
/*
 * The worker must not repaint an unchanged phase forever.  Apart from being
 * noisy, repeated carriage-return frames become literal lines when a parent
 * launcher forwards stderr through a pipe or a terminal without VT support.
 */
static int g_progress_dirty = 0;

static double g_smoothed_rate = 0.0;
static int g_rate_initialized = 0;
static ny_tick_t g_last_tick_time = 0;
static double g_last_progress = 0.0;

#ifndef _WIN32
static pthread_mutex_t g_progress_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_progress_thread;
static int g_progress_thread_started = 0;
#endif

#ifndef _WIN32
static const char *bar_fill[] = {"░", "▏", "▎", "▍", "▌", "▋", "▊", "▉", "█"};
static const char *bar_fill_ascii[] = {" ", "#", "#", "#", "#",
                                       "#", "#", "#", "#"};
#endif

bool ny_progress_enabled_from_env(void) {
  return ny_env_enabled("NYTRIX_PROGRESS");
}

void ny_progress_force(void) { g_progress_forced = 1; }

static void fmt_duration(char *buf, size_t size, double sec, int ms) {
  if (sec < 0)
    sec = 0;
  if (ms && sec < 60.0) {
    int t = (int)(sec * 1000);
    snprintf(buf, size, "%d.%03ds", t / 1000, t % 1000);
  } else {
    int h = (int)(sec / 3600);
    int m = (int)(sec / 60) % 60;
    int s = (int)sec % 60;
    if (h > 0)
      snprintf(buf, size, "%d:%02d:%02d", h, m, s);
    else
      snprintf(buf, size, "%02d:%02d", m, s);
  }
}

#ifndef _WIN32
static void fmt_rate(char *buf, size_t size, double rate) {
  if (rate < 0.001)
    snprintf(buf, size, "?/s");
  else if (rate < 1.0)
    snprintf(buf, size, "%.2f/s", rate);
  else if (rate < 100.0)
    snprintf(buf, size, "%.1f/s", rate);
  else
    snprintf(buf, size, "%.0f/s", rate);
}

static void progress_clip(char *out, size_t out_size, const char *text,
                          int cells) {
  if (!out || out_size == 0)
    return;
  if (!text)
    text = "";
  if (cells < 0)
    cells = 0;
  size_t n = strlen(text);
  size_t limit = (size_t)cells;
  if (n <= limit || limit < 2) {
    if (limit < 2 && n > limit)
      n = 0;
    if (n >= out_size)
      n = out_size - 1;
    memcpy(out, text, n);
    out[n] = '\0';
    return;
  }
  if (limit >= out_size)
    limit = out_size - 1;
  size_t keep = limit - 1;
  if (keep >= out_size)
    keep = out_size - 1;
  memcpy(out, text, keep);
  out[keep] = '~';
  out[keep + 1] = '\0';
}
#endif

#ifndef _WIN32
static void ny_progress_draw_locked(void) {
  if (!g_progress_enabled || !g_progress_running || !g_progress_dirty)
    return;
#ifndef _WIN32
  if (!g_progress_ansi)
    return;
#endif

  long total = g_progress[0].total > 0 ? g_progress[0].total : 1;
  long done = g_progress[0].completed;
  if (done > total)
    done = total;

  int active_idx = -1;
  const char *phase = "";
  for (int i = 1; i < NY_PROGRESS_MAX_NODES; ++i) {
    if (g_progress[i].active) {
      active_idx = i;
      phase = g_progress[i].name;
      break;
    }
  }

  double phase_frac = 0.0;
  if (active_idx > 0 && g_progress[active_idx].total > 0) {
    long sub_done = g_progress[active_idx].completed;
    long sub_total = g_progress[active_idx].total;
    if (sub_done < 0)
      sub_done = 0;
    if (sub_done > sub_total)
      sub_done = sub_total;
    phase_frac = (double)sub_done / (double)sub_total;
  }
  double progress = (double)done + phase_frac;
  if (progress > (double)total)
    progress = (double)total;
  int pct = (int)((progress * 100.0 / (double)total) + 0.5);

  double elapsed = g_progress_start_tick > 0
                       ? ny_ticks_elapsed_sec(g_progress_start_tick)
                       : 0.0;

  double rate = 0.0;
  ny_tick_t now_tick = ny_ticks_now();
  if (g_last_tick_time > 0 && progress > g_last_progress) {
    double dt = ny_ticks_elapsed_sec(g_last_tick_time);
    if (dt > 0.001) {
      double inst = (progress - g_last_progress) / dt;
      if (!g_rate_initialized) {
        g_smoothed_rate = inst;
        g_rate_initialized = 1;
      } else {
        g_smoothed_rate =
            RATE_EMA_ALPHA * inst + (1.0 - RATE_EMA_ALPHA) * g_smoothed_rate;
      }
    }
  }
  if (g_smoothed_rate > 0.0)
    rate = g_smoothed_rate;
  else if (elapsed > 0.001 && progress > 0.0)
    rate = progress / elapsed;

  g_last_tick_time = now_tick;
  g_last_progress = progress;

  double eta = 0.0;
  if (rate > 0.001 && progress < (double)total) {
    double rem = (double)total - progress;
    double ema_eta = rem / rate;
    double avg_eta = progress > 0.001 ? elapsed * rem / progress : ema_eta;
    eta = g_rate_initialized ? (ema_eta * 0.70 + avg_eta * 0.30) : avg_eta;
  }

  /*
   * Build two-color bar: filled (green) then empty (dim).
   */
  int terminal_width = ny_ui_width();
  int compact = terminal_width < 72;
  int bar_width = compact ? terminal_width - 31 : terminal_width - 58;
  if (bar_width < 8)
    bar_width = 8;
  if (bar_width > NY_PROGRESS_BAR_CAP)
    bar_width = NY_PROGRESS_BAR_CAP;
  char filled[NY_PROGRESS_BAR_CAP * 3 + 1];
  char empty[NY_PROGRESS_BAR_CAP * 3 + 1];
  int flen = 0, elen = 0;
  int eighth = (int)(progress * bar_width * 8.0 / (double)total);
  for (int i = 0; i < bar_width; ++i) {
    int rem = eighth - i * 8;
    int lvl = rem >= 8 ? 8 : (rem > 0 ? rem : 0);
    const char *ch = (ny_ui_ascii() ? bar_fill_ascii : bar_fill)[lvl];
    if (lvl > 0) {
      while (*ch)
        filled[flen++] = *ch++;
    } else {
      while (*ch)
        empty[elen++] = *ch++;
    }
  }
  filled[flen] = '\0';
  empty[elen] = '\0';

  char elapsed_str[32], eta_str[32], rate_str[32];
  fmt_duration(elapsed_str, sizeof(elapsed_str), elapsed, 0);
  if (eta > 0.0 && eta < 86400.0)
    fmt_duration(eta_str, sizeof(eta_str), eta, 0);
  else
    snprintf(eta_str, sizeof(eta_str), "?");
  fmt_rate(rate_str, sizeof(rate_str), rate);

  /*
   * Synchronised output + colours.
   */
  char root[32];
  progress_clip(root, sizeof(root),
                g_progress_root[0] ? g_progress_root : "compile",
                compact ? 12 : 20);
  if (compact) {
    fprintf(stderr,
            "\r\033[2K\033[1;37m%s\033[m: \033[33m%3d%%\033[m |"
            "\033[32m%s\033[m\033[2m%s\033[m| \033[37m%ld/%ld\033[m",
            root, pct, filled, empty, done, total);
  } else {
    int phase_cells = terminal_width - bar_width - (int)strlen(root) - 45;
    char phase_text[64];
    progress_clip(phase_text, sizeof(phase_text), phase, phase_cells);
    fprintf(stderr,
            "\r\033[2K\033[1;37m%s\033[m: \033[33m%3d%%\033[m |"
            "\033[32m%s\033[m\033[2m%s\033[m| \033[37m%ld/%ld\033[m "
            "\033[36m[%s<%s\033[m, \033[35m%s\033[m] \033[1;34m%s\033[m",
            root, pct, filled, empty, done, total, elapsed_str, eta_str,
            rate_str, phase_text);
  }
  fflush(stderr);
  g_progress_dirty = 0;
}

static void *ny_progress_thread_main(void *arg) {
  (void)arg;
  usleep(200000);
  while (1) {
    pthread_mutex_lock(&g_progress_lock);
    int running = g_progress_running;
    if (running)
      ny_progress_draw_locked();
    pthread_mutex_unlock(&g_progress_lock);
    if (!running)
      break;
    usleep(80000);
  }
  return NULL;
}
#endif

void ny_progress_start(const char *name, long total) {
#ifndef _WIN32
  int tty = isatty(STDERR_FILENO);
  const char *term = getenv("TERM");
  g_progress_ansi =
      tty && (!term || strcmp(term, "dumb") != 0) && color_enabled();
  if (!tty && !ny_progress_enabled_from_env() && !g_progress_forced)
    return;
  pthread_mutex_lock(&g_progress_lock);
#else
  if (!ny_progress_enabled_from_env() && !g_progress_forced)
    return;
#endif
  memset(g_progress, 0, sizeof(g_progress));
  memset(g_progress_root, 0, sizeof(g_progress_root));
  if (name && name[0])
    snprintf(g_progress_root, sizeof(g_progress_root), "%s", name);
  g_progress[0].active = 1;
  snprintf(g_progress[0].name, sizeof(g_progress[0].name), "pipeline");
  g_progress[0].total = total > 0 ? total : 1;
  g_progress[0].completed = 0;
  g_progress_enabled = 1;
  g_progress_running = 1;
  g_progress_dirty = 1;
  g_progress_start_tick = ny_ticks_now();
  g_last_tick_time = 0;
  g_last_progress = 0.0;
  g_smoothed_rate = 0.0;
  g_rate_initialized = 0;
#ifndef _WIN32
  pthread_mutex_unlock(&g_progress_lock);
  g_progress_thread_started =
      pthread_create(&g_progress_thread, NULL, ny_progress_thread_main, NULL) ==
      0;
#endif
}

ny_progress_node_t ny_progress_task_begin(const char *name, long total) {
  ny_progress_node_t node = {.id = -1};
#ifndef _WIN32
  pthread_mutex_lock(&g_progress_lock);
#endif
  if (!g_progress_enabled || !g_progress_running)
    goto done;
  for (int i = NY_PROGRESS_MAX_NODES - 1; i >= 1; --i) {
    if (g_progress[i].active)
      continue;
    g_progress[i].active = 1;
    g_progress[i].completed = 0;
    g_progress[i].total = total > 0 ? total : 1;
    g_progress[i].started_at = ny_ticks_now();
    snprintf(g_progress[i].name, sizeof(g_progress[i].name), "%s",
             name && name[0] ? name : "task");
    node.id = i;
    g_progress_dirty = 1;
    break;
  }
done:
#ifndef _WIN32
  pthread_mutex_unlock(&g_progress_lock);
#endif
  return node;
}

void ny_progress_task_update(ny_progress_node_t node, long completed) {
  if (node.id < 0 || node.id >= NY_PROGRESS_MAX_NODES)
    return;
#ifndef _WIN32
  pthread_mutex_lock(&g_progress_lock);
#endif
  if (g_progress[node.id].active &&
      g_progress[node.id].completed != completed) {
    g_progress[node.id].completed = completed;
    g_progress_dirty = 1;
  }
#ifndef _WIN32
  pthread_mutex_unlock(&g_progress_lock);
#endif
}

void ny_progress_task_end(ny_progress_node_t node) {
#ifndef _WIN32
  pthread_mutex_lock(&g_progress_lock);
#endif
  if (node.id > 0 && node.id < NY_PROGRESS_MAX_NODES) {
    double dt = ny_ticks_elapsed_sec(g_progress[node.id].started_at);
    if (dt > 0.001 && g_progress[0].total > 0) {
      double inst = 1.0 / dt;
      if (!g_rate_initialized) {
        g_smoothed_rate = inst;
        g_rate_initialized = 1;
      } else {
        g_smoothed_rate =
            RATE_EMA_ALPHA * inst + (1.0 - RATE_EMA_ALPHA) * g_smoothed_rate;
      }
    }
    memset(&g_progress[node.id], 0, sizeof(g_progress[node.id]));
  }
  if (g_progress[0].active)
    g_progress[0].completed++;
  g_progress_dirty = 1;
#ifndef _WIN32
  pthread_mutex_unlock(&g_progress_lock);
#endif
}

void ny_progress_finish(void) {
#ifndef _WIN32
  pthread_mutex_lock(&g_progress_lock);
#endif
  if (!g_progress_enabled)
    goto done;
  g_progress_running = 0;

#ifndef _WIN32
  if (g_progress_ansi) {
    fprintf(stderr, "\r\033[2K");
    fflush(stderr);
  }
#endif

  long total = g_progress[0].total > 0 ? g_progress[0].total : 1;
  long done = g_progress[0].completed;
  if (done > total)
    done = total;
  double elapsed = g_progress_start_tick > 0
                       ? ny_ticks_elapsed_sec(g_progress_start_tick)
                       : 0.0;
  char elapsed_str[32];
  fmt_duration(elapsed_str, sizeof(elapsed_str), elapsed, 1);

#ifndef _WIN32
  if (g_progress_ansi) {
    const char *ok = ny_ui_mark("success");
    const char *dot = ny_ui_glyph("·", ".");
    fprintf(stderr,
            "\033[32m%s\033[m "
            "\033[1m%s\033[m completed "
            "\033[2m%s\033[m %ld/%ld phases "
            "\033[2m%s\033[m %s\n",
            ok, dot, g_progress_root[0] ? g_progress_root : "compile", done,
            total, dot, elapsed_str);
  } else {
    fprintf(stderr, "%s completed: %ld/%ld phases %s %s\n",
            g_progress_root[0] ? g_progress_root : "compile", done, total,
            ny_ui_glyph("·", "."), elapsed_str);
  }
#else
  fprintf(stderr, "%s completed: %ld/%ld phases %s %s\n",
          g_progress_root[0] ? g_progress_root : "compile", done, total,
          ny_ui_glyph("·", "."), elapsed_str);
#endif
  fflush(stderr);

done:
#ifndef _WIN32
  pthread_mutex_unlock(&g_progress_lock);
  if (g_progress_thread_started)
    pthread_join(g_progress_thread, NULL);
  g_progress_thread_started = 0;
#endif
  g_progress_enabled = 0;
#ifndef _WIN32
  g_progress_ansi = 0;
#endif
}

void ny_progress_stderr_lock(void) {
#ifndef _WIN32
  pthread_mutex_lock(&g_progress_lock);
#endif
#ifndef _WIN32
  if (g_progress_ansi) {
    fprintf(stderr, "\r\033[2K");
    fflush(stderr);
  }
#endif
}

void ny_progress_stderr_unlock(void) {
#ifndef _WIN32
  pthread_mutex_unlock(&g_progress_lock);
#endif
}
