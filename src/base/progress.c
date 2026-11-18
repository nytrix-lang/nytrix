/*
 * Hierarchical terminal progress renderer for Nytrix tools.
 *
 * Zig-inspired: one bounded-rate renderer, in-place redraw, initial delay,
 * tree view of active work, real counters only, clean stderr synchronization.
 */

#include "base/progress.h"

#include "base/common.h"
#include "base/compat.h"
#include "base/time.h"
#include "base/util.h"

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <pthread.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

#define NY_PROGRESS_NAME_MAX 96
#define NY_PROGRESS_STATUS_MAX 64
#define NY_PROGRESS_DRAW_INTERVAL_MS 80
#define NY_PROGRESS_INITIAL_DELAY_MS 120
#define NY_PROGRESS_SHORT_RUN_MS 250

typedef enum {
  NY_PSTATE_ACTIVE = 0,
  NY_PSTATE_DONE,
  NY_PSTATE_FAILED,
} ny_progress_state_t;

struct ny_progress_node {
  char name[NY_PROGRESS_NAME_MAX];
  char status[NY_PROGRESS_STATUS_MAX];
  ny_progress_node_t *parent;
  ny_progress_node_t *first_child;
  ny_progress_node_t *last_child;
  ny_progress_node_t *next_sibling;
  ny_progress_node_t *next_active;
  size_t completed;
  size_t total;
  bool has_total;
  ny_progress_state_t state;
  ny_tick_t start_tick;
  ny_tick_t end_tick;
  uint64_t seq;
};

typedef struct ny_progress_ctx {
  ny_progress_node_t *root;
  ny_progress_node_t *active_head;
  ny_progress_node_t **active_tail;
  size_t active_count;
  bool enabled;
  bool forced;
  bool running;
  bool ansi;
  bool tty;
  bool interactive;
  bool render_allowed;
  bool stop_requested;
  bool dirty;
  bool rendered;
  bool finished;
  bool failed;
  bool cursor_hidden;
  int stderr_lock_depth;
  int last_lines;
  uint64_t change_seq;
  uint64_t drawn_seq;
  ny_tick_t start_tick;
  ny_tick_t first_change_tick;
  char prev_frame[16384];
#ifndef _WIN32
  pthread_mutex_t lock;
  pthread_cond_t cond;
  pthread_t thread;
  bool thread_started;
#endif
} ny_progress_ctx_t;

static ny_progress_ctx_t g_progress = {
#ifndef _WIN32
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER,
#endif
};

static void ny_progress_lock(void) {
#ifndef _WIN32
  pthread_mutex_lock(&g_progress.lock);
#endif
}

static void ny_progress_unlock(void) {
#ifndef _WIN32
  pthread_mutex_unlock(&g_progress.lock);
#endif
}

static void ny_progress_wake_locked(void) {
#ifndef _WIN32
  pthread_cond_broadcast(&g_progress.cond);
#endif
}

static void ny_progress_copy_text(char *dst, size_t dst_len, const char *src) {
  if (!dst || dst_len == 0)
    return;
  if (!src)
    src = "";
  snprintf(dst, dst_len, "%s", src);
}

static bool ny_progress_env_ci(void) {
  const char *ci = getenv("CI");
  if (ci && *ci && strcmp(ci, "0") != 0 && strcasecmp(ci, "false") != 0)
    return true;
  return getenv("GITHUB_ACTIONS") || getenv("BUILDKITE") ||
         getenv("TEAMCITY_VERSION") || getenv("TF_BUILD") ||
         getenv("JENKINS_URL") || getenv("GITLAB_CI");
}

static bool ny_progress_stderr_is_tty(void) {
#ifndef _WIN32
  return isatty(STDERR_FILENO) != 0;
#else
  return _isatty(_fileno(stderr)) != 0;
#endif
}

static bool ny_progress_term_dumb(void) {
  const char *term = getenv("TERM");
  return term && strcmp(term, "dumb") == 0;
}

static int ny_progress_term_width(void) {
#ifndef _WIN32
  struct winsize ws;
  if (ioctl(STDERR_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
    return (int)ws.ws_col;
#endif
  return ny_ui_width();
}

static bool ny_progress_color_enabled(void) {
  if (!g_progress.ansi)
    return false;
  if (getenv("NO_COLOR"))
    return false;
  const char *force = getenv("CLICOLOR_FORCE");
  if (force && *force && strcmp(force, "0") != 0)
    return true;
  force = getenv("FORCE_COLOR");
  if (force && *force && strcmp(force, "0") != 0)
    return true;
  const char *clicolor = getenv("CLICOLOR");
  if (clicolor && strcmp(clicolor, "0") == 0)
    return false;
  return color_enabled();
}

static void ny_progress_append(char *out, size_t out_len, size_t *pos,
                               const char *fmt, ...) {
  if (!out || !pos || *pos >= out_len)
    return;
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(out + *pos, out_len - *pos, fmt, ap);
  va_end(ap);
  if (n < 0)
    return;
  size_t avail = out_len - *pos - 1;
  if ((size_t)n > avail)
    *pos = out_len - 1;
  else
    *pos += (size_t)n;
}

static void ny_progress_append_str(char *out, size_t out_len, size_t *pos,
                                   const char *s) {
  if (!s)
    s = "";
  ny_progress_append(out, out_len, pos, "%s", s);
}

/*
 * Truncate a line to a visible width while preserving ANSI CSI sequences.
 * Avoids cutting inside escapes or UTF-8 sequences.
 */
static void ny_progress_trim_visible(char *line, int max_visible) {
  if (!line || max_visible <= 0) {
    if (line)
      line[0] = '\0';
    return;
  }
  const unsigned char *s = (const unsigned char *)line;
  unsigned char *out = (unsigned char *)line;
  int visible = 0;
  bool truncated = false;
  while (*s) {
    if (*s == '\033' && s[1] == '[') {
      const unsigned char *seq = s;
      s += 2;
      while (*s && (*s < 0x40 || *s > 0x7e))
        s++;
      if (*s)
        s++;
      while (seq < s)
        *out++ = *seq++;
      continue;
    }
    if (*s == '\033') {
      *out++ = *s++;
      continue;
    }

    unsigned char c = *s;
    size_t clen = 1;
    if ((c & 0x80) == 0x00)
      clen = 1;
    else if ((c & 0xe0) == 0xc0)
      clen = 2;
    else if ((c & 0xf0) == 0xe0)
      clen = 3;
    else if ((c & 0xf8) == 0xf0)
      clen = 4;
    size_t avail = 0;
    for (size_t i = 0; i < clen && s[i]; ++i)
      avail++;
    if (avail < clen)
      clen = avail ? avail : 1;

    int char_cells = 1;
    if (visible + char_cells > max_visible) {
      truncated = true;
      break;
    }
    for (size_t i = 0; i < clen; ++i)
      *out++ = *s++;
    visible += char_cells;
  }

  if (truncated) {
    if (visible < max_visible)
      *out++ = '~';
    else if (out != (unsigned char *)line)
      out[-1] = '~';
    *out = '\0';
    strcat((char *)out, "\033[0m");
    return;
  }
  *out = '\0';
}

static void ny_progress_active_append_locked(ny_progress_node_t *node) {
  node->next_active = NULL;
  if (!g_progress.active_tail)
    g_progress.active_tail = &g_progress.active_head;
  *g_progress.active_tail = node;
  g_progress.active_tail = &node->next_active;
  g_progress.active_count++;
}

static void ny_progress_active_remove_locked(ny_progress_node_t *node) {
  ny_progress_node_t **pp = &g_progress.active_head;
  while (*pp) {
    if (*pp == node) {
      *pp = node->next_active;
      if (g_progress.active_tail == &node->next_active)
        g_progress.active_tail = pp;
      node->next_active = NULL;
      if (g_progress.active_count > 0)
        g_progress.active_count--;
      return;
    }
    pp = &(*pp)->next_active;
  }
}

static void ny_progress_mark_dirty_locked(void) {
  g_progress.change_seq++;
  g_progress.dirty = true;
  if (g_progress.first_change_tick == 0)
    g_progress.first_change_tick = ny_ticks_now();
  ny_progress_wake_locked();
}

static ny_progress_node_t *ny_progress_alloc_node_locked(ny_progress_node_t *parent,
                                                         const char *name,
                                                         size_t estimated_total) {
  ny_progress_node_t *node = (ny_progress_node_t *)calloc(1, sizeof(*node));
  if (!node)
    return NULL;
  ny_progress_copy_text(node->name, sizeof(node->name), name ? name : "work");
  node->parent = parent;
  node->completed = 0;
  node->total = estimated_total;
  node->has_total = estimated_total > 0;
  node->state = NY_PSTATE_ACTIVE;
  node->start_tick = ny_ticks_now();
  node->seq = ++g_progress.change_seq;
  if (parent) {
    if (parent->last_child)
      parent->last_child->next_sibling = node;
    else
      parent->first_child = node;
    parent->last_child = node;
  }
  ny_progress_active_append_locked(node);
  return node;
}

static bool ny_progress_has_active_desc(const ny_progress_node_t *node) {
  for (const ny_progress_node_t *c = node ? node->first_child : NULL; c;
       c = c->next_sibling) {
    if (c->state == NY_PSTATE_ACTIVE || ny_progress_has_active_desc(c))
      return true;
  }
  return false;
}

static bool ny_progress_visible(const ny_progress_node_t *node) {
  if (!node)
    return false;
  if (node->state == NY_PSTATE_ACTIVE)
    return true;
  return ny_progress_has_active_desc(node);
}

static void ny_progress_format_duration(char *buf, size_t size, double sec) {
  if (sec < 0)
    sec = 0;
  if (sec < 10.0) {
    snprintf(buf, size, "%.1fs", sec);
    return;
  }
  int total = (int)(sec + 0.5);
  int h = total / 3600;
  int m = (total / 60) % 60;
  int s = total % 60;
  if (h > 0)
    snprintf(buf, size, "%d:%02d:%02d", h, m, s);
  else
    snprintf(buf, size, "%d:%02d", m, s);
}

static double ny_progress_elapsed_sec(const ny_progress_node_t *node,
                                      ny_tick_t now) {
  if (!node || node->start_tick == 0)
    return 0.0;
  ny_tick_t end = node->end_tick ? node->end_tick : now;
  if (end < node->start_tick)
    return 0.0;
  return ny_ticks_delta_sec(node->start_tick, end);
}

static const char *ny_progress_spinner(ny_tick_t now) {
  static const char *frames_u[] = {"⠋", "⠙", "⠹", "⠸", "⠼",
                                   "⠴", "⠦", "⠧", "⠇", "⠏"};
  static const char *frames_a[] = {"|", "/", "-", "\\"};
  unsigned idx = (unsigned)((now / 100000000ull) % (ny_ui_ascii() ? 4u : 10u));
  return ny_ui_ascii() ? frames_a[idx] : frames_u[idx];
}

static void ny_progress_cursor_to_block_top(char *out, size_t out_len,
                                            size_t *pos, int lines) {
  if (lines <= 0)
    return;
  ny_progress_append_str(out, out_len, pos, "\r");
  if (lines > 1)
    ny_progress_append(out, out_len, pos, "\033[%dA", lines - 1);
}

static void ny_progress_clear_lines_down(char *out, size_t out_len, size_t *pos,
                                         int lines) {
  for (int i = 0; i < lines; ++i) {
    ny_progress_append_str(out, out_len, pos, "\r\033[2K");
    if (i + 1 < lines)
      ny_progress_append_str(out, out_len, pos, "\n");
  }
}

static void ny_progress_clear_previous_block(char *out, size_t out_len,
                                             size_t *pos, int lines) {
  if (lines <= 0)
    return;
  ny_progress_cursor_to_block_top(out, out_len, pos, lines);
  ny_progress_clear_lines_down(out, out_len, pos, lines);
  ny_progress_cursor_to_block_top(out, out_len, pos, lines);
}

typedef struct {
  char lines[32][512];
  int count;
  int width;
  bool color;
  bool ascii;
  ny_tick_t now;
} ny_progress_render_t;

static void ny_progress_render_node(ny_progress_render_t *r,
                                    const ny_progress_node_t *node,
                                    const char *prefix, bool is_root,
                                    bool is_last);

static void ny_progress_render_children(ny_progress_render_t *r,
                                        const ny_progress_node_t *parent,
                                        const char *prefix) {
  const ny_progress_node_t *last_vis = NULL;
  for (const ny_progress_node_t *c = parent->first_child; c; c = c->next_sibling) {
    if (ny_progress_visible(c))
      last_vis = c;
  }
  for (const ny_progress_node_t *c = parent->first_child; c; c = c->next_sibling) {
    if (!ny_progress_visible(c))
      continue;
    ny_progress_render_node(r, c, prefix, false, c == last_vis);
  }
}

static void ny_progress_render_node(ny_progress_render_t *r,
                                    const ny_progress_node_t *node,
                                    const char *prefix, bool is_root,
                                    bool is_last) {
  if (!r || !node || r->count >= 30)
    return;

  const char *c_dim = r->color ? "\033[2m" : "";
  const char *c_bold = r->color ? "\033[1m" : "";
  const char *c_cyan = r->color ? "\033[36m" : "";
  const char *c_green = r->color ? "\033[32m" : "";
  const char *c_yellow = r->color ? "\033[33m" : "";
  const char *c_red = r->color ? "\033[31m" : "";
  const char *c_rst = r->color ? "\033[0m" : "";
  const char *spin = ny_progress_spinner(r->now + node->seq * 13000000ull);

  char line[512];
  line[0] = '\0';
  const char *name = node->name[0] ? node->name : "work";
  char dur[32];
  ny_progress_format_duration(dur, sizeof(dur), ny_progress_elapsed_sec(node, r->now));

  if (is_root) {
    char counts[64] = "";
    if (node->has_total && node->total > 0)
      snprintf(counts, sizeof(counts), "[%zu/%zu]", node->completed, node->total);

    char bar[96] = "";
    if (node->has_total && node->total > 0) {
      int barw = r->width < 80 ? 10 : 16;
      size_t done = node->completed > node->total ? node->total : node->completed;
      int filled = (int)((done * (size_t)barw) / node->total);
      if (filled > barw)
        filled = barw;
      char bf[48] = {0}, be[48] = {0};
      int p = 0, q = 0;
      for (int i = 0; i < barw; ++i) {
        const char *ch = i < filled ? (r->ascii ? "#" : "━")
                                    : (r->ascii ? "." : "─");
        if (i < filled) {
          while (*ch && p < (int)sizeof(bf) - 1) bf[p++] = *ch++;
        } else {
          while (*ch && q < (int)sizeof(be) - 1) be[q++] = *ch++;
        }
      }
      bf[p] = '\0'; be[q] = '\0';
      snprintf(bar, sizeof(bar), " %s%s%s%s%s", c_green, bf, c_dim, be, c_rst);
    }

    if (counts[0])
      snprintf(line, sizeof(line), "%s%s%s %s%s%s%s %s%s%s %s%s%s",
               c_cyan, spin, c_rst, c_bold, name, c_rst, bar,
               c_dim, counts, c_rst, c_dim, dur, c_rst);
    else
      snprintf(line, sizeof(line), "%s%s%s %s%s%s %s%s%s",
               c_cyan, spin, c_rst, c_bold, name, c_rst, c_dim, dur, c_rst);

    ny_progress_trim_visible(line, r->width);
    snprintf(r->lines[r->count], sizeof(r->lines[r->count]), "%s", line);
    r->count++;
    ny_progress_render_children(r, node, "");
    return;
  }

  const char *branch = is_last ? (r->ascii ? "`- " : "└─ ")
                               : (r->ascii ? "|- " : "├─ ");

  bool show_fraction = node->has_total && node->total > 1;
  bool show_count = show_fraction ||
                    (!node->has_total && node->completed > 0) ||
                    (node->has_total && node->total == 1 && node->completed > 0);

  char countbuf[48] = "";
  if (show_fraction)
    snprintf(countbuf, sizeof(countbuf), "[%zu/%zu] ", node->completed, node->total);
  else if (show_count)
    snprintf(countbuf, sizeof(countbuf), "[%zu] ", node->completed);

  const char *name_color = node->state == NY_PSTATE_ACTIVE ? c_cyan : c_green;
  if (countbuf[0]) {
    snprintf(line, sizeof(line), "%s%s%s%s %s%s%s %s%s%s %s%s%s",
             c_dim, prefix ? prefix : "", branch, c_rst,
             c_dim, countbuf, c_rst,
             name_color, name, c_rst,
             c_dim, dur, c_rst);
  } else {
    snprintf(line, sizeof(line), "%s%s%s%s %s%s%s %s %s%s%s",
             c_dim, prefix ? prefix : "", branch, c_rst,
             c_cyan, spin, c_rst,
             name,
             c_dim, dur, c_rst);
  }

  if (node->status[0]) {
    size_t used = strlen(line);
    snprintf(line + used, sizeof(line) - used, " %s%s%s",
             node->state == NY_PSTATE_FAILED ? c_red : c_yellow, node->status,
             c_rst);
  }

  ny_progress_trim_visible(line, r->width);
  snprintf(r->lines[r->count], sizeof(r->lines[r->count]), "%s", line);
  r->count++;

  char next_prefix[256];
  const char *guide = is_last ? "   " : (r->ascii ? "|  " : "│  ");
  snprintf(next_prefix, sizeof(next_prefix), "%s%s", prefix ? prefix : "", guide);
  ny_progress_render_children(r, node, next_prefix);
}

static void ny_progress_render_frame_locked(char *frame, size_t frame_len,
                                            int *out_lines) {
  size_t pos = 0;
  frame[0] = '\0';
  *out_lines = 0;
  if (!g_progress.enabled || !g_progress.render_allowed ||
      g_progress.stderr_lock_depth > 0)
    return;

  ny_tick_t now = ny_ticks_now();
  if (g_progress.first_change_tick == 0)
    return;
  double since_first =
      ny_ticks_delta_sec(g_progress.first_change_tick, now) * 1000.0;
  if (since_first < (double)NY_PROGRESS_INITIAL_DELAY_MS)
    return;

  int width = ny_progress_term_width();
  if (width < 20)
    width = 20;
  if (width > 240)
    width = 240;

  ny_progress_render_t r = {0};
  r.width = width;
  r.color = ny_progress_color_enabled();
  r.ascii = ny_ui_ascii();
  r.now = now;

  if (g_progress.root)
    ny_progress_render_node(&r, g_progress.root, "", true, true);

  if (r.count == 0)
    return;

  if (g_progress.rendered && g_progress.last_lines > 0)
    ny_progress_clear_previous_block(frame, frame_len, &pos, g_progress.last_lines);
  else
    ny_progress_append_str(frame, frame_len, &pos, "\r");

  for (int i = 0; i < r.count; ++i) {
    ny_progress_append_str(frame, frame_len, &pos, r.lines[i]);
    ny_progress_append_str(frame, frame_len, &pos, "\033[0m\033[K");
    if (i + 1 < r.count)
      ny_progress_append_str(frame, frame_len, &pos, "\n");
  }

  *out_lines = r.count;
}

static void ny_progress_flush_locked(bool force) {
  if (!g_progress.enabled || !g_progress.render_allowed || !g_progress.running)
    return;
  if (g_progress.stderr_lock_depth > 0)
    return;

  ny_tick_t now = ny_ticks_now();
  bool past_delay = false;
  if (g_progress.first_change_tick != 0) {
    double since_first =
        ny_ticks_delta_sec(g_progress.first_change_tick, now) * 1000.0;
    past_delay = since_first >= (double)NY_PROGRESS_INITIAL_DELAY_MS;
  }
  bool animating = past_delay && (g_progress.active_count > 0 ||
                                  (g_progress.root &&
                                   g_progress.root->state == NY_PSTATE_ACTIVE));
  if (!force && !g_progress.dirty && !animating)
    return;

  char frame[16384];
  int lines = 0;
  ny_progress_render_frame_locked(frame, sizeof(frame), &lines);
  if (lines <= 0)
    return;
  if (!force && strcmp(frame, g_progress.prev_frame) == 0) {
    g_progress.dirty = false;
    return;
  }

  bool need_hide = g_progress.ansi && !g_progress.cursor_hidden;
  if (need_hide)
    fputs("\033[?25l", stderr);
  fwrite(frame, 1, strlen(frame), stderr);
  fflush(stderr);
  if (need_hide)
    g_progress.cursor_hidden = true;
  snprintf(g_progress.prev_frame, sizeof(g_progress.prev_frame), "%s", frame);
  g_progress.last_lines = lines;
  g_progress.rendered = true;
  g_progress.dirty = false;
  g_progress.drawn_seq = g_progress.change_seq;
}

static void ny_progress_show_cursor_locked(void) {
  if (g_progress.cursor_hidden && g_progress.ansi) {
    fputs("\033[?25h", stderr);
    fflush(stderr);
  }
  g_progress.cursor_hidden = false;
}

static void ny_progress_clear_display_locked(void) {
  if (!g_progress.rendered || g_progress.last_lines <= 0) {
    ny_progress_show_cursor_locked();
    g_progress.prev_frame[0] = '\0';
    return;
  }
  char buf[1024];
  size_t pos = 0;
  buf[0] = '\0';
  if (g_progress.ansi)
    ny_progress_clear_previous_block(buf, sizeof(buf), &pos, g_progress.last_lines);
  else
    ny_progress_append_str(buf, sizeof(buf), &pos, "\r");
  fwrite(buf, 1, pos, stderr);
  ny_progress_show_cursor_locked();
  fflush(stderr);
  g_progress.rendered = false;
  g_progress.last_lines = 0;
  g_progress.prev_frame[0] = '\0';
}

#ifndef _WIN32
static void ny_progress_sleep_locked(int ms) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_sec += ms / 1000;
  ts.tv_nsec += (long)(ms % 1000) * 1000000L;
  if (ts.tv_nsec >= 1000000000L) {
    ts.tv_sec += 1;
    ts.tv_nsec -= 1000000000L;
  }
  pthread_cond_timedwait(&g_progress.cond, &g_progress.lock, &ts);
}

static void *ny_progress_thread_main(void *arg) {
  (void)arg;
  ny_progress_lock();
  while (!g_progress.stop_requested) {
    if (g_progress.running && g_progress.render_allowed &&
        g_progress.stderr_lock_depth == 0)
      ny_progress_flush_locked(false);
    ny_progress_sleep_locked(NY_PROGRESS_DRAW_INTERVAL_MS);
  }
  ny_progress_unlock();
  return NULL;
}
#endif

static void ny_progress_reset_locked(bool keep_env_flags) {
  bool forced = keep_env_flags ? g_progress.forced : false;
  ny_progress_node_t *stack[256];
  size_t sp = 0;
  if (g_progress.root)
    stack[sp++] = g_progress.root;
  while (sp > 0) {
    ny_progress_node_t *n = stack[--sp];
    ny_progress_node_t *c = n->first_child;
    while (c) {
      ny_progress_node_t *next = c->next_sibling;
      if (sp < 256)
        stack[sp++] = c;
      c = next;
    }
    free(n);
  }

#ifndef _WIN32
  pthread_mutex_t lock = g_progress.lock;
  pthread_cond_t cond = g_progress.cond;
  memset(&g_progress, 0, sizeof(g_progress));
  g_progress.lock = lock;
  g_progress.cond = cond;
#else
  memset(&g_progress, 0, sizeof(g_progress));
#endif
  g_progress.forced = forced;
}

static void ny_progress_ensure_thread_locked(void) {
#ifndef _WIN32
  if (g_progress.thread_started || !g_progress.render_allowed)
    return;
  if (pthread_create(&g_progress.thread, NULL, ny_progress_thread_main, NULL) ==
      0)
    g_progress.thread_started = true;
#endif
}

bool ny_progress_enabled_from_env(void) {
  if (ny_env_enabled("NYTRIX_NO_PROGRESS"))
    return false;
  return ny_env_enabled("NYTRIX_PROGRESS");
}

void ny_progress_force(void) {
  ny_progress_lock();
  g_progress.forced = true;
  ny_progress_unlock();
}

ny_progress_node_t *ny_progress_start(const char *name, size_t estimated_total) {
  ny_progress_lock();
  if (g_progress.root && g_progress.running) {
    ny_progress_node_t *root = g_progress.root;
    ny_progress_unlock();
    return root;
  }
  if (g_progress.root || g_progress.enabled)
    ny_progress_reset_locked(true);

  bool tty = ny_progress_stderr_is_tty();
  bool ci = ny_progress_env_ci();
  bool term_dumb = ny_progress_term_dumb();
  bool explicit_on = ny_progress_enabled_from_env() || g_progress.forced;
  bool explicit_off = ny_env_enabled("NYTRIX_NO_PROGRESS");
  bool interactive = tty && !ci && !term_dumb;

  bool enable = false;
  if (!explicit_off)
    enable = explicit_on || interactive;
  if (!enable) {
    ny_progress_unlock();
    return NULL;
  }

  g_progress.enabled = true;
  g_progress.running = true;
  g_progress.tty = tty;
  g_progress.interactive = interactive;
  g_progress.ansi = interactive;
  g_progress.render_allowed = interactive;
  g_progress.start_tick = ny_ticks_now();
  g_progress.first_change_tick = 0;
  g_progress.active_tail = &g_progress.active_head;
  g_progress.root = ny_progress_alloc_node_locked(NULL, name, estimated_total);
  g_progress.dirty = true;
  g_progress.change_seq = 1;
  ny_progress_ensure_thread_locked();
  ny_progress_node_t *root = g_progress.root;
  ny_progress_unlock();
  return root;
}

ny_progress_node_t *ny_progress_begin(ny_progress_node_t *parent,
                                      const char *name,
                                      size_t estimated_total) {
  ny_progress_lock();
  if (!g_progress.enabled || !g_progress.running) {
    ny_progress_unlock();
    return NULL;
  }
  if (!parent)
    parent = g_progress.root;
  ny_progress_node_t *node =
      ny_progress_alloc_node_locked(parent, name, estimated_total);
  ny_progress_mark_dirty_locked();
  ny_progress_unlock();
  return node;
}

void ny_progress_set_name(ny_progress_node_t *node, const char *name) {
  if (!node)
    return;
  ny_progress_lock();
  if (node->state == NY_PSTATE_ACTIVE) {
    ny_progress_copy_text(node->name, sizeof(node->name), name);
    ny_progress_mark_dirty_locked();
  }
  ny_progress_unlock();
}

void ny_progress_set_total(ny_progress_node_t *node, size_t estimated_total) {
  if (!node)
    return;
  ny_progress_lock();
  if (node->state == NY_PSTATE_ACTIVE) {
    node->total = estimated_total;
    node->has_total = estimated_total > 0;
    ny_progress_mark_dirty_locked();
  }
  ny_progress_unlock();
}

void ny_progress_set_completed(ny_progress_node_t *node, size_t completed) {
  if (!node)
    return;
  ny_progress_lock();
  if (node->state == NY_PSTATE_ACTIVE) {
    if (node->has_total && node->total > 0 && completed > node->total)
      completed = node->total;
    if (node->completed != completed) {
      node->completed = completed;
      ny_progress_mark_dirty_locked();
    }
  }
  ny_progress_unlock();
}

void ny_progress_add(ny_progress_node_t *node, size_t amount) {
  if (!node || amount == 0)
    return;
  ny_progress_lock();
  if (node->state == NY_PSTATE_ACTIVE) {
    size_t next = node->completed + amount;
    if (node->has_total && node->total > 0 && next > node->total)
      next = node->total;
    if (next != node->completed) {
      node->completed = next;
      ny_progress_mark_dirty_locked();
    }
  }
  ny_progress_unlock();
}

static void ny_progress_end_locked(ny_progress_node_t *node,
                                   ny_progress_state_t state,
                                   const char *status) {
  if (!node || node->state != NY_PSTATE_ACTIVE)
    return;
  node->state = state;
  node->end_tick = ny_ticks_now();
  if (node->has_total && state == NY_PSTATE_DONE && node->total > 0)
    node->completed = node->total;
  if (status && *status)
    ny_progress_copy_text(node->status, sizeof(node->status), status);
  ny_progress_active_remove_locked(node);
  if (state == NY_PSTATE_FAILED)
    g_progress.failed = true;

  if (state == NY_PSTATE_DONE && node->parent &&
      node->parent->state == NY_PSTATE_ACTIVE && node->parent->has_total &&
      node->parent->total > 0) {
    if (node->parent->completed < node->parent->total)
      node->parent->completed++;
  }

  ny_progress_mark_dirty_locked();
}

void ny_progress_end(ny_progress_node_t *node) {
  if (!node)
    return;
  ny_progress_lock();
  ny_progress_end_locked(node, NY_PSTATE_DONE, NULL);
  ny_progress_unlock();
}

void ny_progress_fail(ny_progress_node_t *node, const char *status) {
  ny_progress_lock();
  if (node)
    ny_progress_end_locked(node, NY_PSTATE_FAILED, status);
  else if (g_progress.root)
    ny_progress_end_locked(g_progress.root, NY_PSTATE_FAILED, status);
  ny_progress_unlock();
}

void ny_progress_stderr_lock(void) {
  ny_progress_lock();
  if (g_progress.enabled && g_progress.render_allowed &&
      g_progress.stderr_lock_depth == 0) {
    ny_progress_clear_display_locked();
  }
  g_progress.stderr_lock_depth++;
  ny_progress_unlock();
}

void ny_progress_stderr_unlock(void) {
  ny_progress_lock();
  if (g_progress.stderr_lock_depth > 0)
    g_progress.stderr_lock_depth--;
  if (g_progress.stderr_lock_depth == 0 && g_progress.running) {
    g_progress.dirty = true;
    ny_progress_wake_locked();
  }
  ny_progress_unlock();
}

void ny_progress_finish(void) {
  ny_progress_lock();
  if (!g_progress.enabled || g_progress.finished) {
    ny_progress_unlock();
    return;
  }
  g_progress.finished = true;
  g_progress.running = false;
  g_progress.stop_requested = true;
  ny_progress_wake_locked();
  bool thread_started = false;
#ifndef _WIN32
  thread_started = g_progress.thread_started;
#endif
  ny_progress_unlock();

#ifndef _WIN32
  if (thread_started)
    pthread_join(g_progress.thread, NULL);
#endif

  ny_progress_lock();
  if (g_progress.root && g_progress.root->state == NY_PSTATE_ACTIVE) {
    g_progress.root->end_tick = ny_ticks_now();
    g_progress.root->state = g_progress.failed ? NY_PSTATE_FAILED : NY_PSTATE_DONE;
    if (g_progress.root->has_total && !g_progress.failed && g_progress.root->total > 0)
      g_progress.root->completed = g_progress.root->total;
  }

  ny_progress_clear_display_locked();

  bool emit_summary = g_progress.enabled && g_progress.root;
  if (emit_summary) {
    double total_sec = ny_progress_elapsed_sec(g_progress.root, ny_ticks_now());
    bool short_run = (total_sec * 1000.0) < (double)NY_PROGRESS_SHORT_RUN_MS;
    if (short_run && g_progress.render_allowed && !g_progress.forced &&
        !ny_progress_enabled_from_env())
      emit_summary = false;
  }
  if (emit_summary) {
    char dur[32];
    ny_progress_format_duration(
        dur, sizeof(dur), ny_progress_elapsed_sec(g_progress.root, ny_ticks_now()));
    if (g_progress.ansi) {
      const char *color = g_progress.failed ? "\033[31m" : "\033[32m";
      const char *mark = g_progress.failed ? ny_ui_mark("error")
                                           : ny_ui_mark("success");
      fprintf(stderr, "%s%s\033[0m %s %s%s%s\n", color, mark,
              g_progress.root->name[0] ? g_progress.root->name : "progress",
              "\033[2m", dur, "\033[0m");
    } else {
      fprintf(stderr, "%s: %s in %s\n",
              g_progress.root->name[0] ? g_progress.root->name : "progress",
              g_progress.failed ? "failed" : "done", dur);
    }
    fflush(stderr);
  }

  ny_progress_reset_locked(true);
  ny_progress_unlock();
}
