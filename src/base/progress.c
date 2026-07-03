#include "base/progress.h"
#include "base/compat.h"
#include "base/util.h"
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <math.h>
#ifndef _WIN32
#include <pthread.h>
#include <unistd.h>
#endif

#define NY_PROGRESS_MAX_NODES 16
#define NY_PROGRESS_BAR_WIDTH 30

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
static ny_tick_t g_progress_start_tick = 0;

static double g_smoothed_rate = 0.0;
static int g_rate_initialized = 0;
static ny_tick_t g_last_tick_time = 0;
static double g_last_progress = 0.0;

#ifndef _WIN32
static pthread_mutex_t g_progress_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_progress_thread;
static int g_progress_thread_started = 0;
#endif

static const char *bar_fill[] = {
    "░", "▏", "▎", "▍", "▌", "▋", "▊", "▉", "█"
};

bool ny_progress_enabled_from_env(void) {
  return ny_env_enabled("NYTRIX_PROGRESS");
}

void ny_progress_force(void) {
  g_progress_forced = 1;
}

static void fmt_duration(char *buf, size_t size, double sec, int ms) {
  if (sec < 0) sec = 0;
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

static void ny_progress_draw_locked(void) {
  if (!g_progress_enabled || !g_progress_running)
    return;

  long total = g_progress[0].total > 0 ? g_progress[0].total : 1;
  long done = g_progress[0].completed;
  if (done > total) done = total;

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
        g_smoothed_rate = RATE_EMA_ALPHA * inst +
                          (1.0 - RATE_EMA_ALPHA) * g_smoothed_rate;
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

  /* Build two-color bar: filled (green) then empty (dim). */
  char filled[NY_PROGRESS_BAR_WIDTH * 3 + 1];
  char empty[NY_PROGRESS_BAR_WIDTH * 3 + 1];
  int flen = 0, elen = 0;
  int eighth = (int)(progress * NY_PROGRESS_BAR_WIDTH * 8.0 / (double)total);
  for (int i = 0; i < NY_PROGRESS_BAR_WIDTH; ++i) {
    int rem = eighth - i * 8;
    int lvl = rem >= 8 ? 8 : (rem > 0 ? rem : 0);
    const char *ch = bar_fill[lvl];
    if (lvl > 0) {
      while (*ch) filled[flen++] = *ch++;
    } else {
      while (*ch) empty[elen++] = *ch++;
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

  /* Synchronised output + colours. */
  fprintf(stderr,
    "\x1b[?2026h"                          /* begin sync */
    "\r\033[J"                             /* clear line */
    "\033[1;37m%s\033[m: "                 /* root name, bold white */
    "\033[33m%3d%%\033[m "                 /* percentage, yellow */
    "|"
    "\033[32m%s\033[m"                     /* filled bar, green */
    "\033[2m%s\033[m"                      /* empty bar, dim */
    "| "
    "\033[37m%ld/%ld\033[m "               /* count, white */
    "\033[36m[%s<%s\033[m, "               /* elapsed/eta, cyan */
    "\033[35m%s\033[m]"                    /* rate, magenta */
    " \033[1;34m%s\033[m"                  /* phase, bold blue */
    "\x1b[?2026l",                         /* end sync */
    g_progress_root[0] ? g_progress_root : "compile",
    pct,
    filled, empty,
    done, total,
    elapsed_str, eta_str,
    rate_str,
    phase);
  fflush(stderr);
}

#ifndef _WIN32
static void *ny_progress_thread_main(void *arg) {
  (void)arg;
  usleep(200000);
  while (1) {
    pthread_mutex_lock(&g_progress_lock);
    int running = g_progress_running;
    if (running)
      ny_progress_draw_locked();
    pthread_mutex_unlock(&g_progress_lock);
    if (!running) break;
    usleep(80000);
  }
  return NULL;
}
#endif

void ny_progress_start(const char *name, long total) {
#ifndef _WIN32
  if (!isatty(STDERR_FILENO) && !ny_progress_enabled_from_env() && !g_progress_forced)
    return;
  pthread_mutex_lock(&g_progress_lock);
#else
  if (!ny_progress_enabled_from_env() && !g_progress_forced) return;
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
  g_progress_start_tick = ny_ticks_now();
  g_last_tick_time = 0;
  g_last_progress = 0.0;
  g_smoothed_rate = 0.0;
  g_rate_initialized = 0;
#ifndef _WIN32
  pthread_mutex_unlock(&g_progress_lock);
  g_progress_thread_started =
      pthread_create(&g_progress_thread, NULL, ny_progress_thread_main, NULL) == 0;
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
    if (g_progress[i].active) continue;
    g_progress[i].active = 1;
    g_progress[i].completed = 0;
    g_progress[i].total = total > 0 ? total : 1;
    g_progress[i].started_at = ny_ticks_now();
    snprintf(g_progress[i].name, sizeof(g_progress[i].name), "%s",
             name && name[0] ? name : "task");
    node.id = i;
    break;
  }
done:
#ifndef _WIN32
  pthread_mutex_unlock(&g_progress_lock);
#endif
  return node;
}

void ny_progress_task_update(ny_progress_node_t node, long completed) {
  if (node.id < 0 || node.id >= NY_PROGRESS_MAX_NODES) return;
#ifndef _WIN32
  pthread_mutex_lock(&g_progress_lock);
#endif
  if (g_progress[node.id].active)
    g_progress[node.id].completed = completed;
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
        g_smoothed_rate = RATE_EMA_ALPHA * inst +
                          (1.0 - RATE_EMA_ALPHA) * g_smoothed_rate;
      }
    }
    memset(&g_progress[node.id], 0, sizeof(g_progress[node.id]));
  }
  if (g_progress[0].active)
    g_progress[0].completed++;
#ifndef _WIN32
  pthread_mutex_unlock(&g_progress_lock);
#endif
}

void ny_progress_finish(void) {
#ifndef _WIN32
  pthread_mutex_lock(&g_progress_lock);
#endif
  if (!g_progress_enabled) goto done;
  g_progress_running = 0;

  /* Synchronised clear + final summary. */
  fprintf(stderr,
    "\x1b[?2026h"
    "\r\033[J"
    "\x1b[?2026l");
  fflush(stderr);

  long total = g_progress[0].total > 0 ? g_progress[0].total : 1;
  long done = g_progress[0].completed;
  if (done > total) done = total;
  double elapsed = g_progress_start_tick > 0
                      ? ny_ticks_elapsed_sec(g_progress_start_tick)
                      : 0.0;
  char elapsed_str[32];
  fmt_duration(elapsed_str, sizeof(elapsed_str), elapsed, 1);

  fprintf(stderr,
    "\033[32m\xE2\x9C\x93\033[m "
    "\033[1m%s\033[m completed "
    "\033[2m·\033[m %ld/%ld phases "
    "\033[2m·\033[m %s\n",
    g_progress_root[0] ? g_progress_root : "compile",
    done, total, elapsed_str);
  fflush(stderr);

done:
#ifndef _WIN32
  pthread_mutex_unlock(&g_progress_lock);
  if (g_progress_thread_started)
    pthread_join(g_progress_thread, NULL);
  g_progress_thread_started = 0;
#endif
  g_progress_enabled = 0;
}

void ny_progress_stderr_lock(void) {
#ifndef _WIN32
  pthread_mutex_lock(&g_progress_lock);
#endif
  fprintf(stderr, "\r\033[J");
  fflush(stderr);
}

void ny_progress_stderr_unlock(void) {
#ifndef _WIN32
  pthread_mutex_unlock(&g_progress_lock);
#endif
}
