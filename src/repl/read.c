#include "repl/read.h"
#include "base/common.h"
#include "base/util.h"
#include "repl/priv.h"
#include "repl/types.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <conio.h>
#include <windows.h>
#else
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

#ifndef _WIN32
#include <poll.h>
#include <signal.h>
#include <strings.h>
#endif

#ifndef _WIN32
typedef int sig_atomic_t;
#endif

extern int g_repl_sigint;
static char **ny_hist = NULL;
static int ny_hist_len = 0;
static int ny_hist_cap = 0;
static int ny_hist_max = 1000;
static const char *NY_HIST_MAGIC = "NYHIST1";

static int starts_with_ci(const char *s, const char *prefix) {
  if (!s || !prefix)
    return 0;
  while (*prefix) {
    if (!*s)
      return 0;
    if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix))
      return 0;
    s++;
    prefix++;
  }
  return 1;
}

static int repl_next_cap(int current, int needed_len, int extra) {
  if (needed_len < 0)
    return 0;
  size_t need = (size_t)needed_len + 1u + (extra > 0 ? (size_t)extra : 0u);
  size_t cap = current > 0 ? (size_t)current : 256u;
  while (cap < need) {
    if (cap > (size_t)INT_MAX / 2u) {
      cap = need;
      break;
    }
    cap *= 2u;
  }
  return cap <= (size_t)INT_MAX ? (int)cap : 0;
}

static int repl_reserve_buf(char **pbuf, int *cap, int needed_len, int extra) {
  if (!pbuf || !*pbuf || !cap)
    return 0;
  if (*cap > needed_len)
    return 1;
  int new_cap = repl_next_cap(*cap, needed_len, extra);
  if (new_cap <= *cap)
    return 0;
  char *grown = realloc(*pbuf, (size_t)new_cap);
  if (!grown)
    return 0;
  *pbuf = grown;
  *cap = new_cap;
  return 1;
}

static int repl_insert_bytes(char **pbuf, int *cap, int *len, int *pos,
                             const char *data, int data_len, int extra) {
  if (!pbuf || !*pbuf || !cap || !len || !pos || !data || data_len <= 0)
    return 0;
  if (!repl_reserve_buf(pbuf, cap, *len + data_len, extra))
    return 0;
  char *buf = *pbuf;
  memmove(buf + *pos + data_len, buf + *pos, (size_t)(*len - *pos) + 1u);
  memcpy(buf + *pos, data, (size_t)data_len);
  *len += data_len;
  *pos += data_len;
  return 1;
}

static int repl_insert_newline_indent(char **pbuf, int *cap, int *len,
                                      int *pos, int indent, int extra) {
  if (!pbuf || !*pbuf || !cap || !len || !pos)
    return 0;
  if (indent < 0)
    indent = 0;
  int data_len = 1 + indent;
  if (!repl_reserve_buf(pbuf, cap, *len + data_len, extra + indent))
    return 0;
  char *buf = *pbuf;
  memmove(buf + *pos + data_len, buf + *pos, (size_t)(*len - *pos) + 1u);
  buf[*pos] = '\n';
  if (indent > 0)
    memset(buf + *pos + 1, ' ', (size_t)indent);
  *pos += data_len;
  *len += data_len;
  return 1;
}

static int repl_set_buffer_text(char **pbuf, int *cap, int *len, int *pos,
                                const char *src, int extra) {
  if (!pbuf || !cap || !len || !pos)
    return 0;
  char *copy = ny_strdup(src ? src : "");
  if (!copy)
    return 0;
  int new_len = (int)strlen(copy);
  int new_cap = repl_next_cap(0, new_len, extra);
  if (new_cap <= 0) {
    free(copy);
    return 0;
  }
  char *grown = realloc(copy, (size_t)new_cap);
  if (!grown)
    new_cap = new_len + 1;
  else
    copy = grown;
  free(*pbuf);
  *pbuf = copy;
  *cap = new_cap;
  *len = new_len;
  *pos = new_len;
  return 1;
}

void ny_readline_init(void) {}

void ny_readline_add_history(const char *line) {
  if (!line || !*line)
    return;
  if (ny_hist_len > 0 && strcmp(ny_hist[ny_hist_len - 1], line) == 0)
    return;
  if (ny_hist_len >= ny_hist_max && ny_hist_len > 0) {
    free(ny_hist[0]);
    memmove(ny_hist, ny_hist + 1, sizeof(char *) * (ny_hist_len - 1));
    ny_hist_len--;
  }
  if (ny_hist_len >= ny_hist_cap) {
    int new_cap = ny_hist_cap ? ny_hist_cap * 2 : 64;
    char **grown = realloc(ny_hist, sizeof(char *) * (size_t)new_cap);
    if (!grown)
      return;
    ny_hist = grown;
    ny_hist_cap = new_cap;
  }
  char *copy = ny_strdup(line);
  if (!copy)
    return;
  ny_hist[ny_hist_len++] = copy;
}

void ny_readline_stifle_history(int max) { ny_hist_max = max > 0 ? max : 1000; }

int ny_readline_read_history(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return 0;
  char magic[16] = {0};
  if (fgets(magic, sizeof(magic), f)) {
    size_t mlen = strlen(magic);
    while (mlen > 0 && (magic[mlen - 1] == '\n' || magic[mlen - 1] == '\r'))
      magic[--mlen] = '\0';
    if (strcmp(magic, NY_HIST_MAGIC) == 0) {
      char hdr[128];
      while (fgets(hdr, sizeof(hdr), f)) {
        unsigned long long n = 0;
        if (sscanf(hdr, "@@ %llu", &n) != 1)
          break;
        char *entry = malloc((size_t)n + 1);
        if (!entry)
          break;
        size_t got = fread(entry, 1, (size_t)n, f);
        entry[got] = '\0';
        if (got == (size_t)n)
          ny_readline_add_history(entry);
        free(entry);
        int ch = fgetc(f);
        if (ch == '\r') {
          int ch2 = fgetc(f);
          if (ch2 != '\n' && ch2 != EOF)
            ungetc(ch2, f);
        } else if (ch != '\n' && ch != EOF) {
          ungetc(ch, f);
        }
      }
      fclose(f);
      return 0;
    }
    fseek(f, 0, SEEK_SET);
  } else {
    fclose(f);
    return 0;
  }
  char buf[4096];
  while (fgets(buf, sizeof(buf), f)) {
    size_t n = strlen(buf);
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
      buf[--n] = '\0';
    ny_readline_add_history(buf);
  }
  fclose(f);
  return 0;
}

int ny_readline_write_history(const char *path) {
  if (!path)
    return 0;
  FILE *f = fopen(path, "wb");
  if (!f)
    return -1;
  fprintf(f, "%s\n", NY_HIST_MAGIC);
  for (int i = 0; i < ny_hist_len; i++) {
    if (ny_hist[i]) {
      size_t len = strlen(ny_hist[i]);
      fprintf(f, "@@ %zu\n", len);
      fwrite(ny_hist[i], 1, len, f);
      fputc('\n', f);
    }
  }
  fclose(f);
  return 0;
}

enum {
  K_UP = 1000,
  K_DOWN,
  K_LEFT,
  K_RIGHT,
  K_HOME,
  K_END,
  K_DEL,
  K_BACKSPACE,
  K_ENTER,
  K_TAB,
  K_CTRL_A,
  K_CTRL_B,
  K_CTRL_C,
  K_CTRL_D,
  K_CTRL_E,
  K_CTRL_F,
  K_CTRL_K,
  K_CTRL_L,
  K_CTRL_N,
  K_CTRL_P,
  K_CTRL_R,
  K_CTRL_U,
  K_CTRL_W,
  K_CTRL_Y,
  K_CTRL_Z,
  K_CTRL_G,
  K_PASTE_START,
  K_PASTE_END,
  K_PASTE_CHAR,
  K_EOF,
  K_SHIFT_UP,
  K_SHIFT_DOWN,
  K_SHIFT_LEFT,
  K_SHIFT_RIGHT,
  K_SHIFT_HOME,
  K_SHIFT_END,
  K_CTRL_SHIFT_UP,
  K_CTRL_SHIFT_DOWN,
  K_CTRL_SHIFT_LEFT,
  K_CTRL_SHIFT_RIGHT,
  K_ALT_SHIFT_UP,
  K_ALT_SHIFT_DOWN,
  K_ALT_SHIFT_LEFT,
  K_ALT_SHIFT_RIGHT,
  K_ALT_SHIFT_HOME,
  K_ALT_SHIFT_END,
  K_ALT_UP,
  K_ALT_DOWN,
  K_CTRL_LEFT,
  K_CTRL_RIGHT,
  K_ALT_LEFT,
  K_ALT_RIGHT,
  K_CTRL_BACKSPACE,
  K_CTRL_DEL,
  K_SHIFT_TAB,
  K_ALT_ENTER,
  K_PAGE_UP,
  K_PAGE_DOWN,
  K_MOUSE_WHEEL_UP,
  K_MOUSE_WHEEL_DOWN,
  K_UNKNOWN
};

static volatile int g_got_winch = 0;

static int repl_control_key(int c) {
  switch (c) {
  case 1:
    return K_CTRL_A;
  case 2:
    return K_CTRL_B;
  case 3:
    return K_CTRL_C;
  case 5:
    return K_CTRL_E;
  case 6:
    return K_CTRL_F;
  case 11:
    return K_CTRL_K;
  case 12:
    return K_CTRL_L;
  case 14:
    return K_CTRL_N;
  case 16:
    return K_CTRL_P;
  case 18:
    return K_CTRL_R;
  case 21:
    return K_CTRL_U;
  case 23:
    return K_CTRL_W;
  case 25:
    return K_CTRL_Y;
  case 26:
  case 31:
    return K_CTRL_Z;
  default:
    return 0;
  }
}

#ifndef _WIN32
static void handle_winch(int sig) {
  (void)sig;
  g_got_winch = 1;
}
#endif

extern int g_repl_sigint;

static int csi_is_final(int c) { return c >= 0x40 && c <= 0x7e; }

#ifndef _WIN32
static int get_char_timeout(int ms) {
  struct pollfd pfd = {STDIN_FILENO, POLLIN, 0};
  while (1) {
    int r = poll(&pfd, 1, ms);
    if (r > 0) {
      unsigned char c = 0;
      if (read(STDIN_FILENO, &c, 1) == 1)
        return c;
    } else if (r < 0) {
      if (errno == EINTR) {
        if (g_repl_sigint)
          return 3;
        if (g_got_winch)
          return -2; // Special value for winch
        continue;
      }
    }
    break;
  }
  return -1;
}

static void csi_consume_tail(int first, int ms) {
  if (first < 0 || csi_is_final(first))
    return;
  for (int i = 0; i < 64; ++i) {
    int c = get_char_timeout(ms);
    if (c == -1 || c == -2 || csi_is_final(c))
      return;
  }
}

static int csi_read_int_until(int *out, int *term, int ms) {
  int value = 0;
  int seen = 0;
  while (1) {
    int c = get_char_timeout(ms);
    if (c == -1 || c == -2) {
      if (term)
        *term = c;
      return seen;
    }
    if (c >= '0' && c <= '9') {
      seen = 1;
      if (value < 100000)
        value = value * 10 + (c - '0');
      continue;
    }
    if (term)
      *term = c;
    if (out)
      *out = value;
    return seen;
  }
}

static int csi_parse_sgr_mouse(int ms) {
  int button = 0;
  int term = 0;
  if (!csi_read_int_until(&button, &term, ms)) {
    csi_consume_tail(term, ms);
    return K_UNKNOWN;
  }
  while (term != -1 && term != -2 && !csi_is_final(term)) {
    int ignored = 0;
    if (!csi_read_int_until(&ignored, &term, ms))
      break;
  }
  if (term == 'M' || term == 'm') {
    int wheel = button & 0x43;
    if (wheel == 0x40)
      return K_MOUSE_WHEEL_UP;
    if (wheel == 0x41)
      return K_MOUSE_WHEEL_DOWN;
  }
  csi_consume_tail(term, ms);
  return K_UNKNOWN;
}

static int csi_parse_x10_mouse(int ms) {
  int b = get_char_timeout(ms);
  int x = get_char_timeout(ms);
  int y = get_char_timeout(ms);
  (void)x;
  (void)y;
  if (b == -1 || b == -2 || x == -1 || x == -2 || y == -1 || y == -2)
    return K_UNKNOWN;
  int button = b - 32;
  int wheel = button & 0x43;
  if (wheel == 0x40)
    return K_MOUSE_WHEEL_UP;
  if (wheel == 0x41)
    return K_MOUSE_WHEEL_DOWN;
  return K_UNKNOWN;
}

static int read_key_posix(char *out_chr) {
  static int unget_char = -1;
  int c;
  if (unget_char != -1) {
    c = unget_char;
    unget_char = -1;
  } else {
    c = get_char_timeout(-1);
  }
  if (c == -2)
    return -2; // WINCH
  if (c == -1 || c == 4)
    return K_EOF;
  if (c == '\r') {
    int next = get_char_timeout(repl_is_input_pending() ? 1 : 50);
    if (next != '\n' && next != -1) {
      unget_char = next;
    }
    return K_ENTER;
  }
  if (c == '\n')
    return K_ENTER;
  if (c == '\t')
    return K_TAB;
  if (c == 127 || c == 8)
    return K_BACKSPACE;
  int ctrl_code = repl_control_key(c);
  if (ctrl_code)
    return ctrl_code;
  if (c == '\x1b') {
    int seq1 = get_char_timeout(250);
    if (seq1 == -1)
      return '\x1b';
    if (seq1 == '\r' || seq1 == '\n')
      return K_ALT_ENTER;
    if (seq1 == '[') {
      int seq2 = get_char_timeout(1000);
      if (seq2 == -1)
        return K_UNKNOWN;
      if (seq2 == 'Z')
        return K_SHIFT_TAB;
      if (seq2 == '<')
        return csi_parse_sgr_mouse(100);
      if (seq2 == 'M')
        return csi_parse_x10_mouse(100);
      if (seq2 == 'A')
        return K_UP;
      if (seq2 == 'B')
        return K_DOWN;
      if (seq2 == 'C')
        return K_RIGHT;
      if (seq2 == 'D')
        return K_LEFT;
      if (seq2 == 'H')
        return K_HOME;
      if (seq2 == 'F')
        return K_END;
      if (seq2 == '1') {
        int seq3 = get_char_timeout(1000);
        if (seq3 == '3') {
          int seq4 = get_char_timeout(1000);
          if (seq4 == '~')
            return K_ALT_ENTER;
          csi_consume_tail(seq4, 1000);
          return K_UNKNOWN;
        }
        if (seq3 == ';') {
          int seq4 = get_char_timeout(1000);
          int seq5 = get_char_timeout(1000);
          if (seq4 == '2') {
            if (seq5 == 'A')
              return K_SHIFT_UP;
            if (seq5 == 'B')
              return K_SHIFT_DOWN;
            if (seq5 == 'C')
              return K_SHIFT_RIGHT;
            if (seq5 == 'D')
              return K_SHIFT_LEFT;
            if (seq5 == 'H')
              return K_SHIFT_HOME;
            if (seq5 == 'F')
              return K_SHIFT_END;
          }
          if (seq4 == '5') {
            if (seq5 == 'C')
              return K_CTRL_RIGHT;
            if (seq5 == 'D')
              return K_CTRL_LEFT;
          } else if (seq4 == '6') {
            if (seq5 == 'A')
              return K_CTRL_SHIFT_UP;
            if (seq5 == 'B')
              return K_CTRL_SHIFT_DOWN;
            if (seq5 == 'C')
              return K_CTRL_SHIFT_RIGHT;
            if (seq5 == 'D')
              return K_CTRL_SHIFT_LEFT;
          } else if (seq4 == '3') {
            if (seq5 == 'C')
              return K_ALT_RIGHT;
            if (seq5 == 'D')
              return K_ALT_LEFT;
          } else if (seq4 == '4') {
            if (seq5 == 'A')
              return K_ALT_SHIFT_UP;
            if (seq5 == 'B')
              return K_ALT_SHIFT_DOWN;
            if (seq5 == 'C')
              return K_ALT_SHIFT_RIGHT;
            if (seq5 == 'D')
              return K_ALT_SHIFT_LEFT;
            if (seq5 == 'H')
              return K_ALT_SHIFT_HOME;
            if (seq5 == 'F')
              return K_ALT_SHIFT_END;
          }
        }
      }
      if (seq2 == '2') {
        int seq3 = get_char_timeout(1000);
        if (seq3 == '0') {
          int seq4 = get_char_timeout(1000);
          int seq5 = get_char_timeout(1000);
          if (seq4 == '0' && seq5 == '~')
            return K_PASTE_START;
          if (seq4 == '1' && seq5 == '~')
            return K_PASTE_END;
        }
      }
      if (seq2 >= '0' && seq2 <= '9') {
        int seq3 = get_char_timeout(1000);
        if (seq3 == '~') {
          if (seq2 == '1' || seq2 == '7')
            return K_HOME;
          if (seq2 == '4' || seq2 == '8')
            return K_END;
          if (seq2 == '3')
            return K_DEL;
          if (seq2 == '5')
            return K_PAGE_UP;
          if (seq2 == '6')
            return K_PAGE_DOWN;
        } else if (seq3 == ';') {
          int seq4 = get_char_timeout(1000);
          int seq5 = get_char_timeout(1000);
          if (seq2 == '3' && seq4 == '5' && seq5 == '~')
            return K_CTRL_DEL;
          if ((seq2 == '1' || seq2 == '7') && seq4 == '2' && seq5 == '~')
            return K_SHIFT_HOME;
          if ((seq2 == '4' || seq2 == '8') && seq4 == '2' && seq5 == '~')
            return K_SHIFT_END;
          if ((seq2 == '1' || seq2 == '7') && seq4 == '4' && seq5 == '~')
            return K_ALT_SHIFT_HOME;
          if ((seq2 == '4' || seq2 == '8') && seq4 == '4' && seq5 == '~')
            return K_ALT_SHIFT_END;
        } else {
          csi_consume_tail(seq3, 1000);
        }
      }
      csi_consume_tail(seq2, 1000);
    } else if (seq1 == 'f')
      return K_ALT_RIGHT;
    else if (seq1 == 'b')
      return K_ALT_LEFT;
    else if (seq1 == 127 || seq1 == 8)
      return K_CTRL_BACKSPACE;
    else if (seq1 == 'O') {
      int seq2 = get_char_timeout(1000);
      if (seq2 == 'A')
        return K_UP;
      if (seq2 == 'B')
        return K_DOWN;
      if (seq2 == 'C')
        return K_RIGHT;
      if (seq2 == 'D')
        return K_LEFT;
      if (seq2 == 'H')
        return K_HOME;
      if (seq2 == 'F')
        return K_END;
    }
    return K_UNKNOWN;
  }
  *out_chr = (char)c;
  return c;
}
#else

static unsigned char g_win_pending[8];
static int g_win_pending_head = 0;
static int g_win_pending_tail = 0;
static int g_win_skip_lf_after_cr = 0;

static void win_queue_byte(unsigned char b) {
  g_win_pending[g_win_pending_tail & 7] = b;
  g_win_pending_tail++;
}
static int win_dequeue_byte(void) {
  if (g_win_pending_head == g_win_pending_tail)
    return -1;
  return (int)g_win_pending[g_win_pending_head++ & 7];
}

static int win_read_char_ms(HANDLE hIn, int ms) {
  DWORD deadline = (ms >= 0) ? (GetTickCount() + (DWORD)ms) : 0xFFFFFFFFu;
  while (1) {
    DWORD now = GetTickCount();
    DWORD left = (ms < 0) ? INFINITE : (deadline > now ? deadline - now : 0);
    if (left == 0 && ms >= 0)
      return -1;
    if (WaitForSingleObject(hIn, left) != WAIT_OBJECT_0)
      return -1;
    DWORD n = 0;
    if (!GetNumberOfConsoleInputEvents(hIn, &n) || n == 0)
      return -1;
    INPUT_RECORD rec;
    DWORD nread = 0;
    if (!ReadConsoleInputW(hIn, &rec, 1, &nread) || nread == 0)
      return -1;
    if (rec.EventType != KEY_EVENT)
      continue;
    if (!rec.Event.KeyEvent.bKeyDown)
      continue;
    WCHAR wc = rec.Event.KeyEvent.uChar.UnicodeChar;
    return wc ? (int)(unsigned int)wc : -1;
  }
}

static int win_key_from_csi_final(int final, int mod) {
  int shifted = mod == 2 || mod == 4 || mod == 6 || mod == 8;
  int alt = mod == 3 || mod == 4 || mod == 7 || mod == 8;
  int ctrl = mod == 5 || mod == 6 || mod == 7 || mod == 8;
  switch (final) {
  case 'A':
    return ctrl && shifted ? K_CTRL_SHIFT_UP
           : alt && shifted ? K_ALT_SHIFT_UP
           : shifted        ? K_SHIFT_UP
           : alt            ? K_ALT_UP
                            : K_UP;
  case 'B':
    return ctrl && shifted ? K_CTRL_SHIFT_DOWN
           : alt && shifted ? K_ALT_SHIFT_DOWN
           : shifted        ? K_SHIFT_DOWN
           : alt            ? K_ALT_DOWN
                            : K_DOWN;
  case 'C':
    return ctrl && shifted ? K_CTRL_SHIFT_RIGHT
           : alt && shifted ? K_ALT_SHIFT_RIGHT
           : ctrl           ? K_CTRL_RIGHT
           : shifted        ? K_SHIFT_RIGHT
           : alt            ? K_ALT_RIGHT
                            : K_RIGHT;
  case 'D':
    return ctrl && shifted ? K_CTRL_SHIFT_LEFT
           : alt && shifted ? K_ALT_SHIFT_LEFT
           : ctrl           ? K_CTRL_LEFT
           : shifted        ? K_SHIFT_LEFT
           : alt            ? K_ALT_LEFT
                            : K_LEFT;
  case 'H':
    return alt && shifted ? K_ALT_SHIFT_HOME
           : shifted      ? K_SHIFT_HOME
                          : K_HOME;
  case 'F':
    return alt && shifted ? K_ALT_SHIFT_END
           : shifted      ? K_SHIFT_END
                          : K_END;
  default:
    return K_UNKNOWN;
  }
}

static int win_key_from_csi_tilde(int code, int mod) {
  switch (code) {
  case 1:
  case 7:
    if (mod == 2)
      return K_SHIFT_HOME;
    if (mod == 4)
      return K_ALT_SHIFT_HOME;
    return K_HOME;
  case 2:
    return K_UNKNOWN;
  case 3:
    return mod == 5 ? K_CTRL_DEL : K_DEL;
  case 4:
  case 8:
    if (mod == 2)
      return K_SHIFT_END;
    if (mod == 4)
      return K_ALT_SHIFT_END;
    return K_END;
  case 5:
    return K_PAGE_UP;
  case 6:
    return K_PAGE_DOWN;
  case 13:
    return K_ALT_ENTER;
  case 200:
    return K_PASTE_START;
  case 201:
    return K_PASTE_END;
  default:
    return K_UNKNOWN;
  }
}

static int win_parse_csi_params(const char *seq, int len, int *params,
                                int max_params) {
  int count = 0;
  int value = 0;
  int seen = 0;
  for (int i = 0; i < len; ++i) {
    unsigned char c = (unsigned char)seq[i];
    if (c >= '0' && c <= '9') {
      seen = 1;
      if (value < 100000)
        value = value * 10 + (c - '0');
      continue;
    }
    if (c == ';') {
      if (count < max_params)
        params[count++] = seen ? value : 0;
      value = 0;
      seen = 0;
      continue;
    }
    if (c == '?' || c == '<')
      continue;
    break;
  }
  if (seen || count > 0) {
    if (count < max_params)
      params[count++] = seen ? value : 0;
  }
  return count;
}

static int win_parse_sgr_mouse_seq(const char *seq, int len) {
  if (!seq || len < 2 || seq[0] != '<')
    return K_UNKNOWN;
  int button = 0;
  int seen = 0;
  for (int i = 1; i < len; ++i) {
    unsigned char c = (unsigned char)seq[i];
    if (c >= '0' && c <= '9') {
      seen = 1;
      if (button < 100000)
        button = button * 10 + (c - '0');
      continue;
    }
    break;
  }
  if (!seen)
    return K_UNKNOWN;
  int wheel = button & 0x43;
  if (wheel == 0x40)
    return K_MOUSE_WHEEL_UP;
  if (wheel == 0x41)
    return K_MOUSE_WHEEL_DOWN;
  return K_UNKNOWN;
}

static int win_parse_csi_seq(const char *seq, int len) {
  if (!seq || len <= 0)
    return K_UNKNOWN;
  int final = (unsigned char)seq[len - 1];
  int body_len = len - 1;
  if (final == 'Z')
    return K_SHIFT_TAB;
  if (seq[0] == '<' && (final == 'M' || final == 'm'))
    return win_parse_sgr_mouse_seq(seq, len);
  if (body_len == 0 && (final == 'A' || final == 'B' || final == 'C' ||
                        final == 'D' || final == 'H' || final == 'F'))
    return win_key_from_csi_final(final, 0);
  int params[4] = {0, 0, 0, 0};
  int count = win_parse_csi_params(seq, body_len, params, 4);
  int mod = count >= 2 ? params[1] : 0;
  if (final == 'A' || final == 'B' || final == 'C' || final == 'D' ||
      final == 'H' || final == 'F')
    return win_key_from_csi_final(final, mod);
  if (final == '~' && count >= 1)
    return win_key_from_csi_tilde(params[0], mod);
  return K_UNKNOWN;
}

static int win_parse_esc(HANDLE hIn, int ms) {
  int s1 = win_read_char_ms(hIn, ms);
  if (s1 < 0)
    return '\x1b';
  if (s1 == 127 || s1 == 8)
    return K_CTRL_BACKSPACE;
  if (s1 == 'f')
    return K_ALT_RIGHT;
  if (s1 == 'b')
    return K_ALT_LEFT;
  if (s1 == '\r' || s1 == '\n')
    return K_ALT_ENTER;
  if (s1 == '[') {
    char seq[32];
    int len = 0;
    while (len < (int)sizeof(seq)) {
      int c = win_read_char_ms(hIn, ms);
      if (c < 0)
        return K_UNKNOWN;
      seq[len++] = (char)c;
      if (csi_is_final(c))
        return win_parse_csi_seq(seq, len);
    }
    return K_UNKNOWN;
  }
  if (s1 == 'O') {
    char seq[16];
    int len = 0;
    while (len < (int)sizeof(seq)) {
      int c = win_read_char_ms(hIn, ms);
      if (c < 0)
        return K_UNKNOWN;
      seq[len++] = (char)c;
      if (csi_is_final(c))
        return win_parse_csi_seq(seq, len);
    }
    return K_UNKNOWN;
  }
  return K_UNKNOWN;
}

static int read_key_win(char *out_chr) {
  int queued = win_dequeue_byte();
  if (queued != -1) {
    *out_chr = (char)(unsigned char)queued;
    return queued;
  }
  HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
  INPUT_RECORD rec;
  DWORD nread = 0;
  while (1) {
    if (!ReadConsoleInputW(hIn, &rec, 1, &nread) || nread == 0)
      return K_EOF;
    if (rec.EventType != KEY_EVENT)
      continue;
    KEY_EVENT_RECORD k = rec.Event.KeyEvent;
    if (!k.bKeyDown)
      continue;
    WORD vk = k.wVirtualKeyCode;
    WCHAR wch = k.uChar.UnicodeChar;
    char ch = (wch >= 1 && wch < 128) ? (char)(unsigned char)wch : 0;
    int ctrl = (k.dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
    int alt = (k.dwControlKeyState & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) != 0;
    if (g_win_skip_lf_after_cr) {
      if (wch == L'\r')
        continue;
      if (wch == L'\n') {
        g_win_skip_lf_after_cr = 0;
        continue;
      }
      if (wch != 0)
        g_win_skip_lf_after_cr = 0;
    }
    if (wch == 0x1B || vk == VK_ESCAPE) {
      int r = win_parse_esc(hIn, 60);
      return r;
    }
    if (vk == VK_RETURN) {
      if (wch == L'\r')
        g_win_skip_lf_after_cr = 1;
      return K_ENTER;
    }
    if (ch == 8 || ch == 127)
      return ctrl ? K_CTRL_BACKSPACE : K_BACKSPACE;
    if (ch == '\t')
      return K_TAB;
    if (vk == VK_UP)
      return (ctrl && (k.dwControlKeyState & SHIFT_PRESSED))  ? K_CTRL_SHIFT_UP
             : (alt && (k.dwControlKeyState & SHIFT_PRESSED)) ? K_ALT_SHIFT_UP
             : (k.dwControlKeyState & SHIFT_PRESSED)          ? K_SHIFT_UP
             : alt                                            ? K_ALT_UP
                                                              : K_UP;
    if (vk == VK_DOWN)
      return (ctrl && (k.dwControlKeyState & SHIFT_PRESSED))  ? K_CTRL_SHIFT_DOWN
             : (alt && (k.dwControlKeyState & SHIFT_PRESSED)) ? K_ALT_SHIFT_DOWN
             : (k.dwControlKeyState & SHIFT_PRESSED)          ? K_SHIFT_DOWN
             : alt                                            ? K_ALT_DOWN
                                                              : K_DOWN;
    if (vk == VK_LEFT)
      return (ctrl && (k.dwControlKeyState & SHIFT_PRESSED))  ? K_CTRL_SHIFT_LEFT
             : (alt && (k.dwControlKeyState & SHIFT_PRESSED)) ? K_ALT_SHIFT_LEFT
             : ctrl                                           ? K_CTRL_LEFT
             : (k.dwControlKeyState & SHIFT_PRESSED)          ? K_SHIFT_LEFT
             : alt                                            ? K_ALT_LEFT
                                                              : K_LEFT;
    if (vk == VK_RIGHT)
      return (ctrl && (k.dwControlKeyState & SHIFT_PRESSED))  ? K_CTRL_SHIFT_RIGHT
             : (alt && (k.dwControlKeyState & SHIFT_PRESSED)) ? K_ALT_SHIFT_RIGHT
             : ctrl                                           ? K_CTRL_RIGHT
             : (k.dwControlKeyState & SHIFT_PRESSED)          ? K_SHIFT_RIGHT
             : alt                                            ? K_ALT_RIGHT
                                                              : K_RIGHT;
    if (vk == VK_HOME)
      return (alt && (k.dwControlKeyState & SHIFT_PRESSED)) ? K_ALT_SHIFT_HOME
             : (k.dwControlKeyState & SHIFT_PRESSED)        ? K_SHIFT_HOME
                                                            : K_HOME;
    if (vk == VK_END)
      return (alt && (k.dwControlKeyState & SHIFT_PRESSED)) ? K_ALT_SHIFT_END
             : (k.dwControlKeyState & SHIFT_PRESSED)        ? K_SHIFT_END
                                                            : K_END;
    if (vk == VK_BACK)
      return ctrl ? K_CTRL_BACKSPACE : K_BACKSPACE;
    if (vk == VK_DELETE)
      return ctrl ? K_CTRL_DEL : K_DEL;
    if (vk == VK_TAB)
      return (k.dwControlKeyState & SHIFT_PRESSED) ? K_SHIFT_TAB : K_TAB;
    if (vk == VK_PRIOR)
      return K_PAGE_UP;
    if (vk == VK_NEXT)
      return K_PAGE_DOWN;
    if (vk == VK_INSERT)
      continue;
    if (ch == '\r') {
      g_win_skip_lf_after_cr = 1;
      return K_ENTER;
    }
    if (ch == '\n')
      return K_ENTER;
    if (ch == 4)
      return K_EOF;
    if (ch == 26)
      return K_EOF;
    int ctrl_code = repl_control_key(ch);
    if (ctrl_code)
      return ctrl_code;
    if (ch >= 32 && ch != 127) {
      *out_chr = ch;
      return (unsigned char)ch;
    }
    if (wch >= 0x80) {
      if (wch >= 0xD800 && wch <= 0xDFFF)
        continue;
      unsigned char utf8[3];
      int ulen = 0;
      if (wch < 0x800) {
        utf8[ulen++] = (unsigned char)(0xC0 | (wch >> 6));
        utf8[ulen++] = (unsigned char)(0x80 | (wch & 0x3F));
      } else {
        utf8[ulen++] = (unsigned char)(0xE0 | (wch >> 12));
        utf8[ulen++] = (unsigned char)(0x80 | ((wch >> 6) & 0x3F));
        utf8[ulen++] = (unsigned char)(0x80 | (wch & 0x3F));
      }
      for (int i = 1; i < ulen; i++)
        win_queue_byte(utf8[i]);
      *out_chr = (char)utf8[0];
      return (int)utf8[0];
    }
    (void)ctrl;
    (void)alt;
  }
}

static int win_text_event_utf8(const KEY_EVENT_RECORD *k, char out[4]) {
  if (!k || !k->bKeyDown)
    return 0;
  int ctrl = (k->dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
  int alt = (k->dwControlKeyState & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) != 0;
  if (ctrl || alt)
    return 0;
  WCHAR wch = k->uChar.UnicodeChar;
  if (wch == L'\r' || wch == L'\n') {
    out[0] = '\n';
    return 1;
  }
  if (wch == L'\t') {
    out[0] = '\t';
    return 1;
  }
  if (wch < 32 || wch == 127 || (wch >= 0xD800 && wch <= 0xDFFF))
    return 0;
  if (wch < 0x80) {
    out[0] = (char)wch;
    return 1;
  }
  if (wch < 0x800) {
    out[0] = (char)(0xC0 | (wch >> 6));
    out[1] = (char)(0x80 | (wch & 0x3F));
    return 2;
  }
  out[0] = (char)(0xE0 | (wch >> 12));
  out[1] = (char)(0x80 | ((wch >> 6) & 0x3F));
  out[2] = (char)(0x80 | (wch & 0x3F));
  return 3;
}

static int win_append_bytes(char **buf, int *len, int *cap,
                            const char *data, int data_len) {
  if (!buf || !len || !cap || !data || data_len <= 0)
    return 0;
  if (*len + data_len + 1 > *cap) {
    int new_cap = *cap > 0 ? *cap : 4096;
    while (new_cap < *len + data_len + 1) {
      if (new_cap > INT_MAX / 2) {
        new_cap = *len + data_len + 1;
        break;
      }
      new_cap *= 2;
    }
    char *grown = realloc(*buf, (size_t)new_cap);
    if (!grown)
      return 0;
    *buf = grown;
    *cap = new_cap;
  }
  memcpy(*buf + *len, data, (size_t)data_len);
  *len += data_len;
  (*buf)[*len] = '\0';
  return 1;
}

static int win_drain_queued_text(char **pbuf, int *cap, int *len, int *pos,
                                 int prev_was_cr) {
  HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
  char *text = NULL;
  int text_cap = 0;
  int text_len = 0;
  int drained = 0;
  DWORD idle_start = 0;

  while (drained < 1048576) {
    DWORD count = 0;
    if (!GetNumberOfConsoleInputEvents(hIn, &count) || count == 0) {
      if (drained > 0) {
        DWORD wait_ms = drained > 4096 ? 2 : 1;
        DWORD max_idle_ms = drained > 4096 ? 32 : 16;
        DWORD now = GetTickCount();
        if (idle_start == 0)
          idle_start = now;
        if ((DWORD)(now - idle_start) < max_idle_ms) {
          if (WaitForSingleObject(hIn, wait_ms) == WAIT_OBJECT_0)
            continue;
        }
      }
      break;
    }
    idle_start = 0;

    INPUT_RECORD rec;
    DWORD got = 0;
    if (!PeekConsoleInputW(hIn, &rec, 1, &got) || got == 0)
      break;
    if (rec.EventType != KEY_EVENT || !rec.Event.KeyEvent.bKeyDown) {
      ReadConsoleInputW(hIn, &rec, 1, &got);
      continue;
    }

    char bytes[4] = {0};
    WCHAR wch = rec.Event.KeyEvent.uChar.UnicodeChar;
    if (wch == L'\r' && prev_was_cr) {
      if (!ReadConsoleInputW(hIn, &rec, 1, &got) || got == 0)
        break;
      drained++;
      idle_start = 0;
      continue;
    }
    if (wch == L'\n' && prev_was_cr) {
      if (!ReadConsoleInputW(hIn, &rec, 1, &got) || got == 0)
        break;
      prev_was_cr = 0;
      drained++;
      idle_start = 0;
      continue;
    }
    int n = win_text_event_utf8(&rec.Event.KeyEvent, bytes);
    if (n <= 0)
      break;
    if (!ReadConsoleInputW(hIn, &rec, 1, &got) || got == 0)
      break;
    if (!win_append_bytes(&text, &text_len, &text_cap, bytes, n))
      break;
    prev_was_cr = (wch == L'\r');
    drained++;
    idle_start = 0;
  }

  int ok = 1;
  if (text_len > 0)
    ok = repl_insert_bytes(pbuf, cap, len, pos, text, text_len, text_len + 256);
  free(text);
  return ok ? drained : 0;
}

static int win_has_queued_text_event(void) {
  HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
  DWORD count = 0;
  if (!GetNumberOfConsoleInputEvents(hIn, &count) || count == 0)
    return 0;
  if (count > 256)
    count = 256;
  INPUT_RECORD recs[256];
  DWORD got = 0;
  if (!PeekConsoleInputW(hIn, recs, count, &got) || got == 0)
    return 0;
  for (DWORD i = 0; i < got; ++i) {
    if (recs[i].EventType != KEY_EVENT || !recs[i].Event.KeyEvent.bKeyDown)
      continue;
    char bytes[4] = {0};
    if (win_text_event_utf8(&recs[i].Event.KeyEvent, bytes) > 0)
      return 1;
  }
  return 0;
}

static int win_pasted_buffer_should_submit(const char *buf, int len, int pos) {
  if (!buf || len <= 0 || pos != len)
    return 0;
  int i = len - 1;
  while (i >= 0 && (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\r'))
    i--;
  if (i < 0 || buf[i] != '\n')
    return 0;
  int newline_count = 0;
  for (int n = 0; n <= i; ++n)
    if (buf[n] == '\n')
      newline_count++;
  if (newline_count > 1) {
    int j = i - 1;
    while (j >= 0 && (buf[j] == ' ' || buf[j] == '\t' || buf[j] == '\r'))
      j--;
    if (j < 0 || buf[j] != '\n')
      return 0;
  }
  return is_input_complete(buf);
}
#endif

#ifdef _WIN32
static int win_append_utf16_as_utf8(char **buf, int *len, int *cap,
                                    const WCHAR *text, int text_len) {
  if (!buf || !len || !cap || !text || text_len <= 0)
    return 1;
  int bytes = WideCharToMultiByte(CP_UTF8, 0, text, text_len, NULL, 0, NULL,
                                  NULL);
  if (bytes <= 0)
    return 0;
  if (*len + bytes + 1 > *cap) {
    int new_cap = *cap > 0 ? *cap : 256;
    while (new_cap < *len + bytes + 1) {
      if (new_cap > INT_MAX / 2) {
        new_cap = *len + bytes + 1;
        break;
      }
      new_cap *= 2;
    }
    char *grown = realloc(*buf, (size_t)new_cap);
    if (!grown)
      return 0;
    *buf = grown;
    *cap = new_cap;
  }
  int written =
      WideCharToMultiByte(CP_UTF8, 0, text, text_len, *buf + *len,
                          *cap - *len, NULL, NULL);
  if (written != bytes)
    return 0;
  *len += written;
  (*buf)[*len] = '\0';
  return 1;
}

static char *win_readline_stdio(const char *prompt) {
  if (prompt && *prompt)
    fputs(prompt, stdout);
  fflush(stdout);
  int cap = 256;
  int len = 0;
  char *buf = malloc((size_t)cap);
  if (!buf)
    return NULL;
  int ch = 0;
  while ((ch = fgetc(stdin)) != EOF) {
    if (ch == '\n')
      break;
    if (len + 2 > cap) {
      int new_cap = cap * 2;
      char *grown = realloc(buf, (size_t)new_cap);
      if (!grown) {
        free(buf);
        return NULL;
      }
      buf = grown;
      cap = new_cap;
    }
    buf[len++] = (char)ch;
  }
  if (len == 0 && ch == EOF) {
    free(buf);
    return NULL;
  }
  while (len > 0 && (buf[len - 1] == '\r' || buf[len - 1] == '\n'))
    len--;
  buf[len] = '\0';
  return buf;
}

static char *win_readline_cooked(const char *prompt) {
  ny_readline_prepare_console();
  HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
  DWORD old_mode = 0;
  if (!GetConsoleMode(hIn, &old_mode))
    return win_readline_stdio(prompt);

  if (prompt && *prompt)
    fputs(prompt, stdout);
  fflush(stdout);

  DWORD cooked = old_mode | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT |
                 ENABLE_PROCESSED_INPUT;
#ifdef ENABLE_VIRTUAL_TERMINAL_INPUT
  cooked &= ~ENABLE_VIRTUAL_TERMINAL_INPUT;
#endif
  SetConsoleMode(hIn, cooked);

  char *buf = NULL;
  int len = 0;
  int cap = 0;
  int saw_input = 0;
  WCHAR wbuf[1024];
  for (;;) {
    DWORD got = 0;
    if (!ReadConsoleW(hIn, wbuf, (DWORD)(sizeof(wbuf) / sizeof(wbuf[0])), &got,
                      NULL) ||
        got == 0) {
      break;
    }
    saw_input = 1;
    if (!win_append_utf16_as_utf8(&buf, &len, &cap, wbuf, (int)got)) {
      free(buf);
      buf = NULL;
      break;
    }
    int done = 0;
    for (DWORD i = 0; i < got; i++) {
      if (wbuf[i] == L'\n' || wbuf[i] == L'\r') {
        done = 1;
        break;
      }
    }
    if (done)
      break;
  }
  SetConsoleMode(hIn, old_mode);

  if (!saw_input) {
    free(buf);
    return NULL;
  }
  if (!buf) {
    buf = ny_strdup("");
    if (!buf)
      return NULL;
    len = 0;
  }
  while (len > 0 && (buf[len - 1] == '\r' || buf[len - 1] == '\n'))
    buf[--len] = '\0';
  if (len == 1 && buf[0] == 26) {
    free(buf);
    return NULL;
  }
  return buf;
}
#endif

#ifdef _WIN32
#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif
#ifndef DISABLE_NEWLINE_AUTO_RETURN
#define DISABLE_NEWLINE_AUTO_RETURN 0x0008
#endif
#endif

static int g_vt_output_ok = 1;

int ny_readline_vt_output_ok(void) { return g_vt_output_ok; }

void ny_readline_prepare_console(void) {
#ifdef _WIN32
  static int prepared = 0;
  if (prepared)
    return;
  prepared = 1;

  HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
  HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
  HANDLE hErr = GetStdHandle(STD_ERROR_HANDLE);
  DWORD mode = 0;
  int have_console = 0;
  int vt_ok = 0;

  if (GetConsoleMode(hIn, &mode)) {
    have_console = 1;
    SetConsoleCP(CP_UTF8);
  }
  if (GetConsoleMode(hOut, &mode)) {
    have_console = 1;
    if (SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING |
                                 DISABLE_NEWLINE_AUTO_RETURN)) {
      vt_ok = 1;
    } else if (SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
      vt_ok = 1;
    }
    SetConsoleOutputCP(CP_UTF8);
  }
  if (GetConsoleMode(hErr, &mode)) {
    have_console = 1;
    SetConsoleMode(hErr, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    SetConsoleOutputCP(CP_UTF8);
  }
  if (have_console) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    g_vt_output_ok = vt_ok;
  }
#else
  (void)g_vt_output_ok;
#endif
}

static int is_break_char(char c) {
  return isspace((unsigned char)c) || strchr("()[]{}\"'.@$><=;|&", c) != NULL;
}

static int repl_has_completion_prefix(const char *buf, int pos) {
  if (!buf || pos <= 0)
    return 0;
  int i = pos - 1;
  while (i >= 0 && (buf[i] & 0xc0) == 0x80)
    i--;
  if (i < 0)
    return 0;
  return !is_break_char(buf[i]);
}

static void get_term_size(int *cols, int *rows) {
  *cols = 80;
  *rows = 24;
#ifdef _WIN32
  CONSOLE_SCREEN_BUFFER_INFO info;
  if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info)) {
    *cols = info.srWindow.Right - info.srWindow.Left + 1;
    *rows = info.srWindow.Bottom - info.srWindow.Top + 1;
  }
#else
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
    *cols = ws.ws_col;
    *rows = ws.ws_row;
  }
#endif
  if (*cols <= 0)
    *cols = 80;
  if (*rows <= 0)
    *rows = 24;
}

static int visible_len(const char *s) {
  int len = 0;
  while (*s) {
    if (*s == '\x1b') {
      s++;
      if (*s == '[') {
        s++;
        while (*s && ((*s >= '0' && *s <= '9') || *s == ';' || *s == '?' || *s == ' '))
          s++;
        if (*s)
          s++;
      }
    } else if (*s == '\001' || *s == '\002') {
      s++;
    } else {
      if ((*s & 0xc0) != 0x80) {
        len++;
      }
      s++;
    }
  }
  return len;
}

static int repl_skip_ansi_seq(const char *s, int i, int end) {
  if (!s || i >= end || s[i] != '\x1b')
    return i;
  int p = i + 1;
  if (p < end && s[p] == '[') {
    p++;
    while (p < end && !csi_is_final((unsigned char)s[p]))
      p++;
    if (p < end)
      p++;
    return p;
  }
  return i;
}

static int repl_skip_ansi_runs(const char *s, int i, int end) {
  while (i < end && s[i] == '\x1b') {
    int next = repl_skip_ansi_seq(s, i, end);
    if (next == i)
      break;
    i = next;
  }
  return i;
}

static int repl_match_pasted_prompt(const char *s, int i, int end) {
  int p = repl_skip_ansi_runs(s, i, end);
  if (p + 2 <= end && s[p] == 'n' && s[p + 1] == 'y') {
    p += 2;
    p = repl_skip_ansi_runs(s, p, end);
    if (p < end && s[p] == '!')
      p++;
    p = repl_skip_ansi_runs(s, p, end);
    if (p < end && s[p] == '>')
      return p + 1;
  }
  p = repl_skip_ansi_runs(s, i, end);
  if (p + 3 <= end && s[p] == '.' && s[p + 1] == '.' && s[p + 2] == '|') {
    p += 3;
    p = repl_skip_ansi_runs(s, p, end);
    for (int n = 0; n < 2 && p < end && (s[p] == ' ' || s[p] == '\t'); ++n)
      p++;
    return p;
  }
  return i;
}

static void repl_paste_string_state_update(char ch, int *in_str, char *quote, int *esc) {
  if (!in_str || !quote || !esc)
    return;
  if (*in_str) {
    if (*esc) {
      *esc = 0;
    } else if (ch == '\\') {
      *esc = 1;
    } else if (ch == *quote) {
      *in_str = 0;
      *quote = '\0';
    }
  } else if (ch == '"' || ch == '\'') {
    *in_str = 1;
    *quote = ch;
    *esc = 0;
  }
}

enum { REPL_TAB_VISUAL_WIDTH = 2, REPL_CONT_PROMPT_COLS = 3 };

static void calc_cursor(const char *buf, int pos, int term_cols, int prompt_cols, int *r, int *c) {
  int row = 0;
  int col = prompt_cols;
  while (col >= term_cols) {
    row++;
    col -= term_cols;
  }
  for (int i = 0; i < pos; i++) {
    if (buf[i] == '\n') {
      row++;
      col = REPL_CONT_PROMPT_COLS;
    } else if (buf[i] == '\t') {
      if (col >= term_cols) {
        row++;
        col = 0;
      }
      col += REPL_TAB_VISUAL_WIDTH;
      if (col > term_cols) {
        row++;
        col -= term_cols;
      }
    } else if ((buf[i] & 0xc0) == 0x80) {
      continue;
    } else {
      if (col >= term_cols) {
        row++;
        col = 0;
      }
      col++;
    }
  }
  *r = row;
  *c = col;
}

static int visual_to_pos(const char *buf, int len, int target_r, int target_c, int term_cols,
                         int prompt_cols) {
  int row = 0;
  int col = prompt_cols;
  while (col >= term_cols) {
    row++;
    col -= term_cols;
  }
  if (target_r < 0)
    return 0;
  int best_pos = 0;
  int best_dist = 1000000;
  for (int i = 0; i <= len; i++) {
    if (row == target_r) {
      int d = abs(col - target_c);
      if (d < best_dist) {
        best_dist = d;
        best_pos = i;
      }
      if (col >= target_c)
        return i;
    } else if (row > target_r) {
      return best_pos;
    }
    if (i == len)
      break;
    if (buf[i] == '\n') {
      if (row == target_r)
        return i;
      row++;
      col = REPL_CONT_PROMPT_COLS;
    } else if (buf[i] == '\t') {
      if (col >= term_cols) {
        row++;
        col = 0;
      }
      col += REPL_TAB_VISUAL_WIDTH;
      if (col > term_cols) {
        row++;
        col -= term_cols;
      }
    } else if ((buf[i] & 0xc0) == 0x80) {
      continue;
    } else {
      if (col >= term_cols) {
        row++;
        col = 0;
      }
      col++;
    }
  }
  return len;
}

static int prev_total_rows = 0;
static int prev_lines = 0;
static int prev_cols = 0;
static int viewport_start = 0;
static char *kill_ring = NULL;

static void repl_set_kill_ring(const char *src, int len) {
  if (len < 0)
    len = 0;
  free(kill_ring);
  kill_ring = malloc((size_t)len + 1);
  if (!kill_ring)
    return;
  if (len > 0 && src)
    memcpy(kill_ring, src, (size_t)len);
  kill_ring[len] = '\0';
}

static int repl_sel_active(int sel_anchor, int pos);
static int repl_sel_start(int sel_anchor, int pos);
static int repl_sel_end(int sel_anchor, int pos);
static void repl_sel_clear(int *sel_anchor);
static void repl_sel_begin(int *sel_anchor, int pos);
static int repl_delete_selection(char *buf, int *len, int *pos, int *sel_anchor);

static void buf_row_slice(const char *buf, int term_cols, int prompt_cols, int start_r, int max_r,
                          int *out_start, int *out_len) {
  int row = 0;
  int col = prompt_cols;
  while (col >= term_cols) {
    row++;
    col -= term_cols;
  }
  int bytes = strlen(buf);
  int s_idx = -1;
  if (start_r <= 0)
    s_idx = 0;
  for (int i = 0; i < bytes; i++) {
    if (s_idx == -1 && row == start_r)
      s_idx = i;
    if (s_idx != -1 && row >= start_r + max_r) {
      *out_start = s_idx;
      *out_len = i - s_idx;
      return;
    }
    if (buf[i] == '\n') {
      row++;
      col = REPL_CONT_PROMPT_COLS;
    } else if (buf[i] == '\t') {
      if (col >= term_cols) {
        row++;
        col = 0;
      }
      col += REPL_TAB_VISUAL_WIDTH;
      if (col > term_cols) {
        row++;
        col -= term_cols;
      }
    } else if ((buf[i] & 0xc0) == 0x80) {
      continue;
    } else {
      if (col >= term_cols) {
        row++;
        col = 0;
      }
      col++;
    }
  }
  if (s_idx == -1) {
    *out_start = bytes;
    *out_len = 0;
    return;
  }
  *out_start = s_idx;
  *out_len = bytes - s_idx;
}

static int g_draw_sel_anchor = -1;
static int g_draw_sel_mode = REPL_SEL_NONE;

static void draw_line(const char *prompt, const char *buf, int len, int pos, int prompt_cols) {
  if (!g_vt_output_ok) {
    (void)len;
    (void)pos;
    (void)prompt_cols;
    printf("\r");
    if (prompt && *prompt)
      fputs(prompt, stdout);
    if (buf && *buf)
      fputs(buf, stdout);
    fflush(stdout);
    return;
  }
  int term_cols, term_rows;
  get_term_size(&term_cols, &term_rows);
  if (term_rows < 2)
    term_rows = 24;
  if (term_cols != prev_cols || term_rows < prev_lines) {
    if (prev_cols != 0) {
      // Clear previous lines if we know how many there were
      if (prev_lines > 0) {
        printf("\r\x1b[%dA", prev_lines);
      }
      printf("\x1b[J"); // Clear from cursor to end of screen
      prev_lines = 0;
      prev_total_rows = 0;
    }
    prev_cols = term_cols;
  }
  int cur_r, cur_c, tot_r, tot_c;
  calc_cursor(buf, pos, term_cols, prompt_cols, &cur_r, &cur_c);
  calc_cursor(buf, len, term_cols, prompt_cols, &tot_r, &tot_c);
  int rows_below = tot_r - cur_r;
  if (tot_r >= term_rows) {
    if (cur_r < viewport_start + 1)
      viewport_start = cur_r - 2;
    if (cur_r >= viewport_start + (term_rows - 2))
      viewport_start = cur_r - (term_rows - 4);
    int max_vp = tot_r - (term_rows - 2);
    if (max_vp < 0)
      max_vp = 0;
    if (viewport_start > max_vp)
      viewport_start = max_vp;
    if (viewport_start < 0)
      viewport_start = 0;
    int up = prev_lines < term_rows ? prev_lines : term_rows - 1;
    printf("\r");
    if (up > 0)
      printf("\x1b[%dA", up);
    printf("\x1b[J");
    int hud_top = 0;
    int max_code_rows = term_rows;
    if (viewport_start > 0) {
      hud_top = 1;
      max_code_rows--;
    }
    int cur_end_row = viewport_start + max_code_rows - 1;
    int has_bottom = 0;
    if (tot_r > cur_end_row) {
      has_bottom = 1;
      max_code_rows--;
      cur_end_row = viewport_start + max_code_rows - 1;
    }
    int s_off, s_len;
    buf_row_slice(buf, term_cols, prompt_cols, viewport_start, max_code_rows, &s_off, &s_len);
    if (s_len > 0 && buf[s_off + s_len - 1] == '\n')
      s_len--;
    char *sub = malloc(s_len + 1);
    memcpy(sub, buf + s_off, s_len);
    sub[s_len] = '\0';
    int adj_pos = pos - s_off;
    if (adj_pos < 0)
      adj_pos = 0;
    if (adj_pos > s_len)
      adj_pos = s_len;
    int sub_cur_r, sub_cur_c, sub_tot_r, sub_tot_c;
    int sub_prompt_cols = (viewport_start == 0) ? prompt_cols : REPL_CONT_PROMPT_COLS;
    calc_cursor(sub, adj_pos, term_cols, sub_prompt_cols, &sub_cur_r, &sub_cur_c);
    calc_cursor(sub, s_len, term_cols, sub_prompt_cols, &sub_tot_r, &sub_tot_c);
    if (hud_top) {
      printf("\r\x1b[90m%d rows above\x1b[K\n", viewport_start);
    }
    if (viewport_start == 0) {
      if (prompt && *prompt)
        fputs(prompt, stdout);
    } else {
      fputs("\x1b[90m..|\x1b[0m", stdout);
    }
    int sub_sel_start = -1;
    int sub_sel_end = -1;
    if (repl_sel_active(g_draw_sel_anchor, pos)) {
      sub_sel_start = repl_sel_start(g_draw_sel_anchor, pos) - s_off;
      sub_sel_end = repl_sel_end(g_draw_sel_anchor, pos) - s_off;
      if (sub_sel_start < 0)
        sub_sel_start = 0;
      if (sub_sel_end > s_len)
        sub_sel_end = s_len;
      if (sub_sel_start >= sub_sel_end) {
        sub_sel_start = -1;
        sub_sel_end = -1;
      }
    }
    repl_highlight_line_ex(sub, adj_pos, "\x1b[90m..|\x1b[0m", sub_sel_start, sub_sel_end,
                           g_draw_sel_mode);
    if (has_bottom) {
      printf("\n\x1b[90m%d rows below (%d%%)\x1b[K", tot_r - cur_end_row,
             (cur_end_row * 100) / tot_r);
      sub_tot_r++;
    }
    free(sub);
    int max_lines_rendered = hud_top + (sub_tot_r + 1) + has_bottom;
    int active_r = sub_cur_r + hud_top;
    if (active_r >= max_lines_rendered)
      active_r = max_lines_rendered - 1;
    int up_dist = (max_lines_rendered - 1) - active_r;
    if (up_dist > 0)
      printf("\x1b[%dA", up_dist);
    printf("\r");
    if (sub_cur_c > 0)
      printf("\x1b[%dC", sub_cur_c);
    prev_lines = active_r;
  } else {
    viewport_start = 0;
    int up = prev_lines;
    if (up >= term_rows)
      up = term_rows - 1;
    printf("\r");
    if (up > 0)
      printf("\x1b[%dA", up);
    printf("\x1b[J");
    int growth = tot_r - prev_total_rows;
    if (prev_total_rows == 0 && tot_r > 0)
      growth = tot_r;
    if (growth > 0) {
      if (growth > term_rows - 2)
        growth = term_rows - 2;
      for (int i = 0; i < growth; i++)
        printf("\n");
      printf("\x1b[%dA", growth);
    }
    if (prompt && *prompt)
      fputs(prompt, stdout);
    int sel_start =
        repl_sel_active(g_draw_sel_anchor, pos) ? repl_sel_start(g_draw_sel_anchor, pos) : -1;
    int sel_end =
        repl_sel_active(g_draw_sel_anchor, pos) ? repl_sel_end(g_draw_sel_anchor, pos) : -1;
    repl_highlight_line_ex(buf, pos, "\x1b[90m..|\x1b[0m", sel_start, sel_end, g_draw_sel_mode);
    int up_dist = rows_below;
    if (up_dist >= term_rows)
      up_dist = term_rows - 1;
    if (up_dist > 0)
      printf("\x1b[%dA", up_dist);
    printf("\r");
    if (cur_c > 0)
      printf("\x1b[%dC", cur_c);
    prev_lines = cur_r;
  }
  prev_total_rows = tot_r;
  fflush(stdout);
}

static void repl_finish_submit_display(const char *prompt, const char *buf,
                                       int len, int pos, int prompt_cols,
                                       int allow_viewport_summary) {
  if (!g_vt_output_ok) {
    printf("\n");
  } else if (allow_viewport_summary && viewport_start > 0) {
    printf("\r");
    if (prev_lines > 0)
      printf("\x1b[%dA", prev_lines);
    printf("\x1b[J");
    if (prompt)
      fputs(prompt, stdout);
    repl_highlight_line_ex(buf, -1, "\x1b[90m..|\x1b[0m", -1, -1,
                           REPL_SEL_NONE);
    printf("\n");
  } else {
    draw_line(prompt, buf, len, pos, prompt_cols);
    int last_r, last_c, term_cols, term_rows;
    get_term_size(&term_cols, &term_rows);
    calc_cursor(buf, len, term_cols, prompt_cols, &last_r, &last_c);
    if (last_r > prev_lines)
      printf("\x1b[%dB", last_r - prev_lines);
    printf("\n");
  }
  fflush(stdout);
}

static int repl_sel_active(int sel_anchor, int pos) { return sel_anchor >= 0 && sel_anchor != pos; }

static int repl_sel_start(int sel_anchor, int pos) { return (sel_anchor < pos) ? sel_anchor : pos; }

static int repl_sel_end(int sel_anchor, int pos) { return (sel_anchor < pos) ? pos : sel_anchor; }

static void repl_sel_clear(int *sel_anchor) { *sel_anchor = -1; }

static void repl_sel_begin(int *sel_anchor, int pos) {
  if (*sel_anchor < 0)
    *sel_anchor = pos;
}

static int repl_delete_selection(char *buf, int *len, int *pos, int *sel_anchor) {
  if (!repl_sel_active(*sel_anchor, *pos))
    return 0;
  int start = repl_sel_start(*sel_anchor, *pos);
  int end = repl_sel_end(*sel_anchor, *pos);
  memmove(buf + start, buf + end, (size_t)(*len - end + 1));
  *len -= (end - start);
  *pos = start;
  *sel_anchor = -1;
  return 1;
}

static int repl_delete_block_selection(char *buf, int *len, int *pos, int *sel_anchor) {
  if (!repl_sel_active(*sel_anchor, *pos))
    return 0;
  int row0, col0, row1, col1;
  int row = 0, col = 0;
  for (int i = 0; i < *sel_anchor; i++) {
    if (buf[i] == '\n') {
      row++;
      col = 0;
    } else {
      col++;
    }
  }
  row0 = row;
  col0 = col;
  row = 0;
  col = 0;
  for (int i = 0; i < *pos; i++) {
    if (buf[i] == '\n') {
      row++;
      col = 0;
    } else {
      col++;
    }
  }
  row1 = row;
  col1 = col;
  int rmin = row0 < row1 ? row0 : row1;
  int rmax = row0 > row1 ? row0 : row1;
  int cmin = col0 < col1 ? col0 : col1;
  int cmax = col0 > col1 ? col0 : col1;
  int src = 0;
  int dst = 0;
  row = 0;
  col = 0;
  while (src < *len) {
    int remove = 0;
    if (buf[src] != '\n' && row >= rmin && row <= rmax && col >= cmin && col < cmax)
      remove = 1;
    if (!remove)
      buf[dst++] = buf[src];
    if (buf[src] == '\n') {
      row++;
      col = 0;
    } else {
      col++;
    }
    src++;
  }
  buf[dst] = '\0';
  *len = dst;
  *pos = *sel_anchor;
  *sel_anchor = -1;
  return 1;
}

static int repl_delete_active_selection(char *buf, int *len, int *pos, int *sel_anchor,
                                        int *sel_mode) {
  int deleted = 0;
  if (*sel_mode == REPL_SEL_BLOCK)
    deleted = repl_delete_block_selection(buf, len, pos, sel_anchor);
  else
    deleted = repl_delete_selection(buf, len, pos, sel_anchor);
  if (deleted)
    *sel_mode = REPL_SEL_NONE;
  return deleted;
}

static void repl_reset_selection_state(int *sel_anchor, int *sel_mode) {
  repl_sel_clear(sel_anchor);
  *sel_mode = REPL_SEL_NONE;
}

static int repl_completion_word_start(const char *buf, int pos) {
  int start = pos;
  while (start > 0 && !is_break_char(buf[start - 1]))
    start--;
  return start;
}

static int repl_completion_word_end(const char *buf, int len, int pos) {
  int end = pos;
  while (end < len && !is_break_char(buf[end]))
    end++;
  return end;
}

static void repl_completion_query_from_span(const char *buf, int start, int pos,
                                            char *out, size_t out_cap) {
  if (!out || out_cap == 0)
    return;
  out[0] = '\0';
  if (!buf || pos <= start)
    return;
  size_t n = (size_t)(pos - start);
  if (n >= out_cap)
    n = out_cap - 1;
  memcpy(out, buf + start, n);
  out[n] = '\0';
}

static char *repl_completion_probe_line(const char *buf, int len, int start, int pos,
                                        int *out_cursor) {
  (void)start;
  if (!buf || len < 0)
    return NULL;
  char *probe = malloc((size_t)len + 1);
  if (!probe)
    return NULL;
  memcpy(probe, buf, (size_t)len);
  probe[len] = '\0';
  if (out_cursor)
    *out_cursor = pos;
  return probe;
}

typedef struct {
  int idx;
  int score;
} repl_comp_item_t;

typedef struct {
  int color;
  int nerd;
  const char *reset;
  const char *dim;
  const char *accent;
  const char *match;
  const char *item;
  const char *selected;
  const char *selected_match;
  const char *search_icon;
  const char *selected_icon;
  const char *more_icon;
} repl_comp_theme_t;

typedef struct {
  const char *name;
  const char *def;
  const char *doc;
  int kind;
} repl_comp_meta_t;

typedef struct {
  const char *name;
  const char *def;
  const char *doc;
  int kind;
} repl_static_doc_t;

static const repl_static_doc_t k_repl_completion_docs[] = {
    {":help", ":help [name]", "Show command, module, or symbol documentation.", 8},
    {":doc", ":doc [name]", "Alias for :help.", 8},
    {":h", ":h [name]", "Short alias for :help.", 8},
    {":exit", ":exit", "Leave the REPL.", 8},
    {":quit", ":quit", "Leave the REPL.", 8},
    {":q", ":q", "Leave the REPL.", 8},
    {":clear", ":clear", "Clear the terminal screen.", 8},
    {":reset", ":reset", "Reset the current REPL session state.", 8},
    {":time", ":time", "Toggle timing output for evaluated snippets.", 8},
    {":trace", ":trace [mode] [filter]", "Toggle runtime/compiler tracing.", 8},
    {":expand", ":expand [mode] [code|last]", "Inspect parsed and lowered code without running it.", 8},
    {":vars", ":vars", "Show persistent REPL definitions and variables.", 8},
    {":history", ":history", "Show command history.", 8},
    {":pwd", ":pwd", "Print the current working directory.", 8},
    {":ls", ":ls [path]", "List files.", 8},
    {":cd", ":cd [path]", "Change the current working directory.", 8},
    {":load", ":load <file>", "Load a Nytrix source or REPL snapshot file.", 8},
    {":run", ":run <file>", "Compile and run a Nytrix file.", 8},
    {":save", ":save [file]", "Save the current session source.", 8},
    {":std", ":std", "Show standard-library loading information.", 8},
    {":complete", ":complete [prefix]", "Print completion candidates for a prefix.", 8},
    {"fn", "fn name(type arg) ret { ... }", "Define a function.", 6},
    {"def", "def name = value", "Bind an immutable value.", 6},
    {"mut", "mut name = value", "Bind a mutable value.", 6},
    {"if", "if(cond){ ... }", "Branch when a condition is true.", 6},
    {"elif", "elif(cond){ ... }", "Add another condition to an if chain.", 6},
    {"else", "else { ... }", "Fallback branch for an if chain.", 6},
    {"while", "while(cond){ ... }", "Loop while a condition is true.", 6},
    {"for", "for(x in xs){ ... }", "Iterate over a sequence.", 6},
    {"in", "for(x in xs)", "Membership/iteration keyword.", 6},
    {"return", "return value", "Return from the current function.", 6},
    {"break", "break", "Exit the nearest loop.", 6},
    {"continue", "continue", "Skip to the next loop iteration.", 6},
    {"use", "use std.module", "Import a standard-library module.", 6},
    {"module", "module name { ... }", "Group declarations under a namespace.", 6},
    {"as", "use module as alias", "Bind an import or include to an alias.", 6},
    {"lambda", "lambda(args){ expr }", "Create an anonymous function.", 6},
    {"defer", "defer { ... }", "Run cleanup code when leaving scope.", 6},
    {"try", "try { ... } catch(e){ ... }", "Handle thrown errors.", 6},
    {"catch", "catch(e){ ... }", "Handle a try block error.", 6},
    {"throw", "throw value", "Raise an error value.", 6},
    {"finally", "finally { ... }", "Run code after try/catch.", 6},
    {"case", "case value { pattern -> expr }", "Match a value against patterns.", 6},
    {"match", "match value { pattern -> expr }", "Match a value against patterns.", 6},
    {"enum", "enum Name { A, B }", "Declare an enum type.", 6},
    {"struct", "struct Name { type: field }", "Declare a structured value type.", 6},
    {"layout", "layout Name { type: field }", "Declare an ABI/layout-shaped type.", 6},
    {"extern", "extern \"lib\" { fn name(...) }", "Declare foreign functions.", 6},
    {"embed", "embed \"path\"", "Embed file contents at compile time.", 6},
    {"comptime", "comptime { ... }", "Run code during compilation.", 6},
    {"impl", "impl type { fn method(...) { ... } }", "Attach methods/operators to a type.", 6},
    {"operator", "operator + Type: Type = fn_name", "Bind an operator implementation.", 6},
    {"self", "self", "Receiver type marker inside impl methods.", 6},
    {"true", "true", "Boolean true literal.", 6},
    {"false", "false", "Boolean false literal.", 6},
    {"nil", "nil", "Nil/empty value.", 6},
    {"none", "none", "Alias-style empty value.", 6},
    {"del", "del name", "Delete a binding or container entry.", 6},
    {"export", "export fn name(...)", "Expose a declaration from a module.", 6},
    {"type", "type", "Type declaration or type-introspection keyword.", 6},
    {"any", "any", "Dynamic value type.", 7},
    {"bool", "bool", "Boolean type.", 7},
    {"int", "int", "Machine integer type.", 7},
    {"bigint", "bigint", "Arbitrary-precision integer type.", 7},
    {"number", "number", "Numeric type family.", 7},
    {"f32", "f32", "32-bit floating-point type.", 7},
    {"f64", "f64", "64-bit floating-point type.", 7},
    {"str", "str", "String type.", 7},
    {"bytes", "bytes", "Byte-buffer type.", 7},
    {"list", "list<T>", "List type.", 7},
    {"tuple", "tuple", "Tuple type.", 7},
    {"dict", "dict<K,V>", "Dictionary type.", 7},
    {"set", "set<T>", "Set type.", 7},
    {"range", "range", "Range/iterator type.", 7},
    {"ptr", "ptr<T>", "Raw pointer type.", 7},
    {"fnptr", "fnptr", "Function pointer type.", 7},
    {NULL, NULL, NULL, 0},
};

static char **g_repl_comp_sort_matches = NULL;

static int repl_env_bool(const char *name, int *out) {
  const char *v = getenv(name);
  if (!v || !*v)
    return 0;
  bool parsed = false;
  if (ny_color_mode_value(v, &parsed)) {
    *out = parsed ? 1 : 0;
    return 1;
  }
  if (strcasecmp(v, "auto") == 0)
    return 0;
  *out = ny_env_truthy(v) ? 1 : 0;
  return 1;
}

static int repl_nerdfont_enabled(void) {
  int out = 0;
  if (repl_env_bool("NYTRIX_REPL_NERDFONT", &out) ||
      repl_env_bool("NYTRIX_NERDFONT", &out) ||
      repl_env_bool("NERD_FONT", &out) ||
      repl_env_bool("NF_ICONS", &out))
    return out;
  const char *icons = getenv("NYTRIX_REPL_ICONS");
  return icons && (strcasecmp(icons, "nerd") == 0 || strcasecmp(icons, "nf") == 0);
}

static repl_comp_theme_t repl_completion_theme(void) {
  repl_comp_theme_t t;
  memset(&t, 0, sizeof(t));
  t.color = color_enabled() ? 1 : 0;
  t.nerd = repl_nerdfont_enabled();
  t.reset = t.color ? "\x1b[0m" : "";
  t.search_icon = t.nerd ? "\xef\x80\x82" : "/";
  t.selected_icon = t.nerd ? "\xef\x81\x94" : ">";
  t.more_icon = t.nerd ? "\xef\x85\x81" : "...";
  if (!t.color) {
    t.dim = t.accent = t.match = t.item = t.selected = t.selected_match = "";
    return t;
  }
  t.dim = "\x1b[90m";
  t.accent = "\x1b[36m";
  t.match = "\x1b[1;33m";
  t.item = "\x1b[37m";
  t.selected = "\x1b[100m\x1b[97m";
  t.selected_match = "\x1b[100m\x1b[1;33m";
  return t;
}

static const char *repl_completion_last_segment(const char *s) {
  const char *dot = s ? strrchr(s, '.') : NULL;
  return dot ? dot + 1 : (s ? s : "");
}

static int repl_completion_name_ends_with(const char *name, const char *suffix) {
  if (!name || !suffix || !*suffix)
    return 0;
  if (strcmp(name, suffix) == 0)
    return 1;
  size_t nl = strlen(name);
  size_t sl = strlen(suffix);
  return nl > sl && name[nl - sl - 1] == '.' && strcmp(name + nl - sl, suffix) == 0;
}

static int repl_completion_method_owner_is(const char *name, const char *owner) {
  if (!name || !owner || !*owner)
    return 0;
  const char *end = strrchr(name, '.');
  if (!end)
    return 0;
  const char *start = end;
  while (start > name && start[-1] != '.')
    start--;
  size_t got = (size_t)(end - start);
  return got == strlen(owner) && strncmp(start, owner, got) == 0;
}

static int repl_completion_number_owner_is(const char *name) {
  return repl_completion_method_owner_is(name, "number") ||
         repl_completion_method_owner_is(name, "numeric") ||
         repl_completion_method_owner_is(name, "int") ||
         repl_completion_method_owner_is(name, "f32") ||
         repl_completion_method_owner_is(name, "f64") ||
         repl_completion_method_owner_is(name, "float") ||
         repl_completion_method_owner_is(name, "bigint");
}

static int repl_completion_doc_score(const ny_doc_entry *e, const char *candidate,
                                     int member_context, const char *owner_hint) {
  if (!e || !e->name || !candidate || !*candidate)
    return 0;
  int score = 0;
  size_t name_len = strlen(e->name);
  if (strcmp(e->name, candidate) == 0) {
    score = 10000;
  } else if (repl_completion_name_ends_with(e->name, candidate)) {
    score = 8000;
  } else if (repl_completion_name_ends_with(candidate, e->name)) {
    score = 7000;
  } else if (strcmp(repl_completion_last_segment(e->name), candidate) == 0) {
    score = 6000;
  }
  if (!score)
    return 0;
  if (member_context && e->kind == 5)
    score += 2500;
  if (!member_context && e->kind == 5)
    score -= 2500;
  if (member_context && owner_hint && *owner_hint && e->kind == 5) {
    if (strcmp(owner_hint, "number") == 0) {
      score += repl_completion_number_owner_is(e->name) ? 3500 : -1000;
    } else {
      score += repl_completion_method_owner_is(e->name, owner_hint) ? 4000 : -1000;
    }
  }
  if (e->def && *e->def)
    score += 200;
  if (e->doc && *e->doc)
    score += 100;
  if (name_len < 200)
    score += (int)(200 - name_len);
  return score;
}

static const repl_static_doc_t *repl_completion_static_doc(const char *candidate) {
  if (!candidate || !*candidate)
    return NULL;
  for (int i = 0; k_repl_completion_docs[i].name; ++i) {
    if (strcmp(k_repl_completion_docs[i].name, candidate) == 0)
      return &k_repl_completion_docs[i];
  }
  return NULL;
}

static void repl_completion_ensure_docs(const char *candidate, int member_context) {
  if (!g_repl_docs || !candidate || !*candidate)
    return;
  doc_list_t *docs = (doc_list_t *)g_repl_docs;
  repl_ensure_docs_for_query(docs, candidate);
  if (member_context || !strchr(candidate, '.')) {
    static const char *const common_modules[] = {
        "std.core",     "std.core.iter", "std.core.str", "std.core.collections",
        "std.math",     "std.math.bin",  "std.math.nt",  "std.math.big",
        NULL,
    };
    for (int i = 0; common_modules[i]; ++i)
      repl_load_module_docs(docs, common_modules[i]);
  }
}

static repl_comp_meta_t repl_completion_meta_for(const char *candidate, int member_context,
                                                const char *owner_hint) {
  repl_comp_meta_t meta = {0};
  repl_completion_ensure_docs(candidate, member_context);
  if (g_repl_docs && candidate && *candidate) {
    const doc_list_t *docs = g_repl_docs;
    const ny_doc_entry *best = NULL;
    int best_score = 0;
    for (size_t i = 0; i < docs->len; ++i) {
      const ny_doc_entry *e = &docs->data[i];
      int score = repl_completion_doc_score(e, candidate, member_context, owner_hint);
      if (score > best_score) {
        best = e;
        best_score = score;
      }
    }
    if (best) {
      meta.name = best->name;
      meta.def = best->def;
      meta.doc = best->doc;
      meta.kind = best->kind;
      return meta;
    }
  }
  const repl_static_doc_t *sd = repl_completion_static_doc(candidate);
  if (sd) {
    meta.name = sd->name;
    meta.def = sd->def;
    meta.doc = sd->doc;
    meta.kind = sd->kind;
  }
  return meta;
}

static int repl_completion_is_member_probe(const char *line, int cursor) {
  if (!line || cursor <= 0)
    return 0;
  int i = cursor - 1;
  while (i >= 0 && isspace((unsigned char)line[i]))
    i--;
  return i >= 0 && line[i] == '.';
}

static void repl_completion_receiver_owner(const char *line, int cursor, char *out,
                                           size_t out_cap) {
  if (!out || out_cap == 0)
    return;
  out[0] = '\0';
  if (!line || cursor <= 0 || !repl_completion_is_member_probe(line, cursor))
    return;
  int dot = cursor - 1;
  while (dot >= 0 && isspace((unsigned char)line[dot]))
    dot--;
  if (dot < 0 || line[dot] != '.')
    return;
  if (dot > 0 && line[dot - 1] == ']') {
    snprintf(out, out_cap, "list");
    return;
  }
  if (dot > 0 && (line[dot - 1] == '"' || line[dot - 1] == '\'')) {
    snprintf(out, out_cap, "str");
    return;
  }
  if (dot > 0 && line[dot - 1] == '}') {
    int brace = dot - 1;
    int depth = 0;
    for (; brace >= 0; --brace) {
      if (line[brace] == '}')
        depth++;
      else if (line[brace] == '{' && --depth == 0)
        break;
    }
    int has_colon = 0;
    for (int i = brace + 1; brace >= 0 && i < dot; ++i) {
      if (line[i] == ':') {
        has_colon = 1;
        break;
      }
    }
    snprintf(out, out_cap, "%s", has_colon ? "dict" : "set");
    return;
  }
  int start = dot;
  while (start > 0 && !is_break_char(line[start - 1]))
    start--;
  while (start < dot && isspace((unsigned char)line[start]))
    start++;
  if (start >= dot)
    return;
  char head[128];
  int len = dot - start;
  if (len >= (int)sizeof(head))
    len = (int)sizeof(head) - 1;
  memcpy(head, line + start, (size_t)len);
  head[len] = '\0';
  char *h = head;
  while (isspace((unsigned char)*h))
    h++;
  if (*h == '[')
    snprintf(out, out_cap, "list");
  else if (*h == '"' || *h == '\'')
    snprintf(out, out_cap, "str");
  else if ((*h == 'b' || *h == 'B') && (h[1] == '"' || h[1] == '\''))
    snprintf(out, out_cap, "bytes");
  else if (*h == '{')
    snprintf(out, out_cap, "%s", strchr(h, ':') ? "dict" : "set");
  else if (strncmp(h, "range(", 6) == 0 || strstr(h, ".."))
    snprintf(out, out_cap, "range");
  else if (strcmp(h, "true") == 0 || strcmp(h, "false") == 0)
    snprintf(out, out_cap, "bool");
  else if (repl_head_is_number(h))
    snprintf(out, out_cap, "number");
}

static void repl_print_trimmed(const char *s, int width) {
  if (!s || width <= 0)
    return;
  printf("%.*s", width, s);
}

static int repl_print_flat_trimmed(const char *s, int width) {
  if (!s || width <= 0)
    return 0;
  int used = 0;
  int pending_space = 0;
  while (*s && isspace((unsigned char)*s))
    s++;
  for (; *s && used < width; ++s) {
    unsigned char c = (unsigned char)*s;
    if (isspace(c)) {
      pending_space = used > 0;
      continue;
    }
    if (pending_space && used < width) {
      putchar(' ');
      used++;
      pending_space = 0;
      if (used >= width)
        break;
    }
    putchar((int)c);
    if ((c & 0xc0) != 0x80)
      used++;
  }
  return used;
}

static int repl_completion_param_has_default(const char *start, const char *end) {
  int depth = 0;
  for (const char *p = start; p && p < end && *p; ++p) {
    if (*p == '(' || *p == '[' || *p == '{')
      depth++;
    else if ((*p == ')' || *p == ']' || *p == '}') && depth > 0)
      depth--;
    else if (*p == '=' && depth == 0)
      return 1;
  }
  return 0;
}

static int repl_completion_slice_contains(const char *start, const char *end,
                                          const char *needle) {
  if (!start || !end || end < start || !needle || !*needle)
    return 0;
  size_t needle_len = strlen(needle);
  size_t span_len = (size_t)(end - start);
  if (needle_len > span_len)
    return 0;
  const char *last = end - needle_len;
  for (const char *p = start; p <= last; ++p) {
    if (memcmp(p, needle, needle_len) == 0)
      return 1;
  }
  return 0;
}

static void repl_completion_arg_summary(const repl_comp_meta_t *meta, char *out, size_t out_cap) {
  if (!out || out_cap == 0)
    return;
  out[0] = '\0';
  if (!meta || !meta->def)
    return;
  const char *open = strchr(meta->def, '(');
  const char *close = open ? strchr(open + 1, ')') : NULL;
  if (!open || !close || close <= open)
    return;
  int total = 0;
  int required = 0;
  int variadic = 0;
  const char *seg = open + 1;
  const char *p = seg;
  int depth = 0;
  int skip_first = (meta->kind == 5);
  while (p <= close) {
    int at_end = (p == close);
    if (!at_end) {
      if (*p == '(' || *p == '[' || *p == '{')
        depth++;
      else if ((*p == ')' || *p == ']' || *p == '}') && depth > 0)
        depth--;
    }
    if (at_end || (*p == ',' && depth == 0)) {
      const char *a = seg;
      const char *b = p;
      while (a < b && isspace((unsigned char)*a))
        a++;
      while (b > a && isspace((unsigned char)b[-1]))
        b--;
      if (a < b) {
        if (skip_first) {
          skip_first = 0;
        } else {
          total++;
          if (repl_completion_slice_contains(a, b, "..."))
            variadic = 1;
          if (!repl_completion_param_has_default(a, b) && !variadic)
            required++;
        }
      }
      seg = p + 1;
    }
    if (at_end)
      break;
    p++;
  }
  if (variadic) {
    snprintf(out, out_cap, "%d+ arg%s", required, required == 1 ? "" : "s");
  } else if (required != total) {
    snprintf(out, out_cap, "%d-%d args", required, total);
  } else {
    snprintf(out, out_cap, "%d arg%s", total, total == 1 ? "" : "s");
  }
}

static void repl_completion_signature(const repl_comp_meta_t *meta, const char *candidate,
                                      char *out, size_t out_cap) {
  if (!out || out_cap == 0)
    return;
  out[0] = '\0';
  if (!meta || !meta->def || !*meta->def)
    return;
  const char *def = meta->def;
  const char *name = def;
  if (strncmp(def, "extern fn ", 10) == 0)
    name = def + 10;
  else if (strncmp(def, "fn ", 3) == 0)
    name = def + 3;
  const char *open = strchr(name, '(');
  if (open) {
    const char *dot = NULL;
    for (const char *p = name; p < open; ++p) {
      if (*p == '.')
        dot = p;
    }
    if (dot && candidate && !strchr(candidate, '.'))
      name = dot + 1;
    if (meta->kind == 5) {
      const char *close = strchr(open + 1, ')');
      const char *comma = NULL;
      int depth = 0;
      if (close) {
        for (const char *p = open + 1; p < close; ++p) {
          if (*p == '(' || *p == '[' || *p == '{')
            depth++;
          else if ((*p == ')' || *p == ']' || *p == '}') && depth > 0)
            depth--;
          else if (*p == ',' && depth == 0) {
            comma = p;
            break;
          }
        }
        const char *args = comma ? comma + 1 : close;
        while (args < close && isspace((unsigned char)*args))
          args++;
        const char *args_end = close;
        while (args_end > args && isspace((unsigned char)args_end[-1]))
          args_end--;
        int n = snprintf(out, out_cap, "%.*s(", (int)(open - name), name);
        if (n < 0)
          return;
        if (args < args_end && (size_t)n < out_cap) {
          int w = snprintf(out + n, out_cap - (size_t)n, "%.*s", (int)(args_end - args), args);
          if (w > 0)
            n += w;
        }
        if ((size_t)n < out_cap)
          snprintf(out + n, out_cap - (size_t)n, ")%s", close + 1);
        return;
      }
    }
  }
  snprintf(out, out_cap, "%s", name);
}

static void repl_completion_detail(const repl_comp_meta_t *meta, const char *candidate,
                                   char *out, size_t out_cap) {
  if (!out || out_cap == 0)
    return;
  out[0] = '\0';
  if (!meta)
    return;
  char sig[384];
  char argc[48];
  repl_completion_signature(meta, candidate, sig, sizeof(sig));
  repl_completion_arg_summary(meta, argc, sizeof(argc));
  if (sig[0] && argc[0])
    snprintf(out, out_cap, "%s  %s", sig, argc);
  else if (sig[0])
    snprintf(out, out_cap, "%s", sig);
  else if (meta->doc)
    snprintf(out, out_cap, "%s", meta->doc);
}

static void repl_print_fuzzy_candidate(const char *candidate, const char *query,
                                       const repl_comp_theme_t *theme, int width,
                                       int selected) {
  if (!candidate || width <= 0)
    return;
  if (!theme || !theme->color) {
    repl_print_trimmed(candidate, width);
    return;
  }
  const char *base = selected ? theme->selected : theme->item;
  const char *hit = selected ? theme->selected_match : theme->match;
  int qi = 0;
  int in_hit = 0;
  printf("%s", base);
  for (int i = 0; candidate[i] && i < width; ++i) {
    unsigned char c = (unsigned char)candidate[i];
    int is_hit = query && query[qi] &&
                 tolower(c) == tolower((unsigned char)query[qi]);
    if (is_hit != in_hit) {
      printf("%s", is_hit ? hit : base);
      in_hit = is_hit;
    }
    putchar(candidate[i]);
    if (is_hit)
      qi++;
  }
  printf("%s", theme->reset);
}

static int repl_completion_item_cmp(const void *a, const void *b) {
  const repl_comp_item_t *ia = (const repl_comp_item_t *)a;
  const repl_comp_item_t *ib = (const repl_comp_item_t *)b;
  if (ia->score != ib->score)
    return ib->score - ia->score;
  const char *sa = g_repl_comp_sort_matches ? g_repl_comp_sort_matches[ia->idx] : "";
  const char *sb = g_repl_comp_sort_matches ? g_repl_comp_sort_matches[ib->idx] : "";
  return strcasecmp(sa, sb);
}

static int repl_completion_boundary_bonus(char prev) {
  if (prev == '\0')
    return 35;
  if (prev == '.' || prev == '_' || prev == '-' || prev == '/' || prev == '\\')
    return 28;
  if (isspace((unsigned char)prev) || prev == ':' || prev == '(' || prev == '[')
    return 18;
  return 0;
}

static int repl_completion_fuzzy_score(const char *candidate, const char *query) {
  if (!candidate)
    return 0;
  if (!query || !*query)
    return 1;
  if (strcmp(candidate, query) == 0)
    return 2000;
  if (strcasecmp(candidate, query) == 0)
    return 1900;
  size_t qlen = strlen(query);
  if (strncasecmp(candidate, query, qlen) == 0)
    return 1600 - (int)(strlen(candidate) > 120 ? 120 : strlen(candidate));
  int score = 0;
  int last = -1;
  int first = -1;
  int ci = 0;
  for (int qi = 0; query[qi]; ++qi) {
    unsigned char q = (unsigned char)tolower((unsigned char)query[qi]);
    int found = -1;
    while (candidate[ci]) {
      unsigned char c = (unsigned char)tolower((unsigned char)candidate[ci]);
      if (c == q) {
        found = ci++;
        break;
      }
      ci++;
    }
    if (found < 0)
      return 0;
    if (first < 0)
      first = found;
    score += 35;
    if (last >= 0 && found == last + 1)
      score += 55;
    if (found == qi)
      score += 25;
    score += repl_completion_boundary_bonus(found > 0 ? candidate[found - 1] : '\0');
    score -= found > 120 ? 120 : found;
    last = found;
  }
  if (starts_with_ci(candidate, query))
    score += 500;
  if (first == 0)
    score += 120;
  size_t clen = strlen(candidate);
  score -= clen > 160 ? 160 : (int)(clen / 2);
  return score > 0 ? score : 1;
}

static int repl_completion_visible_rows(int term_rows) {
  int max_rows = term_rows - 7;
  if (max_rows < 4)
    max_rows = 4;
  if (max_rows > 10)
    max_rows = 10;
  return max_rows;
}

static int repl_completion_page_rows(void) {
  int term_cols, term_rows;
  get_term_size(&term_cols, &term_rows);
  (void)term_cols;
  return repl_completion_visible_rows(term_rows);
}

static repl_comp_item_t *repl_completion_filter(char **matches, size_t count,
                                                const char *query, int *out_len) {
  if (out_len)
    *out_len = 0;
  if (!matches || count == 0)
    return NULL;
  if (count > (size_t)INT_MAX)
    count = (size_t)INT_MAX;
  repl_comp_item_t *items = malloc(sizeof(*items) * count);
  if (!items)
    return NULL;
  int n = 0;
  for (size_t i = 0; i < count; ++i) {
    int score = repl_completion_fuzzy_score(matches[i], query);
    if (score > 0) {
      items[n].idx = (int)i;
      items[n].score = score;
      n++;
    }
  }
  g_repl_comp_sort_matches = matches;
  qsort(items, (size_t)n, sizeof(*items), repl_completion_item_cmp);
  g_repl_comp_sort_matches = NULL;
  if (out_len)
    *out_len = n;
  return items;
}

static int repl_completion_find_item(char **matches, repl_comp_item_t *items, int item_count,
                                     const char *candidate) {
  if (!matches || !items || item_count <= 0 || !candidate)
    return 0;
  for (int i = 0; i < item_count; ++i) {
    const char *m = matches[items[i].idx];
    if (m && strcmp(m, candidate) == 0)
      return i;
  }
  return 0;
}

static void repl_replace_range(char **pbuf, int *cap, int *len, int *pos,
                               int start, int end, const char *replacement) {
  if (!pbuf || !*pbuf || !cap || !len || !pos || !replacement)
    return;
  if (start < 0)
    start = 0;
  if (end < start)
    end = start;
  if (end > *len)
    end = *len;
  int old_len = end - start;
  int new_len = (int)strlen(replacement);
  if (!repl_reserve_buf(pbuf, cap, *len - old_len + new_len, 128))
    return;
  char *buf = *pbuf;
  memmove(buf + start + new_len, buf + end, (size_t)(*len - end) + 1);
  memcpy(buf + start, replacement, (size_t)new_len);
  *len = *len - old_len + new_len;
  *pos = start + new_len;
}

static void repl_return_to_editor_cursor(int up, int cur_c) {
  if (up > 0)
    printf("\x1b[%dA", up);
  printf("\r");
  if (cur_c > 0)
    printf("\x1b[%dC", cur_c);
}

static void repl_reserve_completion_popup(const char *prompt, const char *buf, int len,
                                          int pos, int prompt_cols) {
  if (!g_vt_output_ok)
    return;
  draw_line(prompt, buf, len, pos, prompt_cols);
  int term_cols, term_rows, cur_r, cur_c, tot_r, tot_c;
  get_term_size(&term_cols, &term_rows);
  calc_cursor(buf, pos, term_cols, prompt_cols, &cur_r, &cur_c);
  calc_cursor(buf, len, term_cols, prompt_cols, &tot_r, &tot_c);
  int down = tot_r - cur_r;
  if (down > 0)
    printf("\x1b[%dB", down);
  int reserve = repl_completion_visible_rows(term_rows) + 5;
  for (int i = 0; i < reserve; ++i)
    printf("\n");
  repl_return_to_editor_cursor(down + reserve, cur_c);
  fflush(stdout);
}

static void repl_clear_completion_popup(const char *prompt, const char *buf, int len,
                                        int pos, int prompt_cols) {
  if (!g_vt_output_ok)
    return;
  draw_line(prompt, buf, len, pos, prompt_cols);
  int term_cols, term_rows, cur_r, cur_c, tot_r, tot_c;
  get_term_size(&term_cols, &term_rows);
  calc_cursor(buf, pos, term_cols, prompt_cols, &cur_r, &cur_c);
  calc_cursor(buf, len, term_cols, prompt_cols, &tot_r, &tot_c);
  int down = tot_r - cur_r;
  if (down > 0)
    printf("\x1b[%dB", down);
  printf("\n\x1b[J");
  repl_return_to_editor_cursor(down + 1, cur_c);
  fflush(stdout);
}

static void repl_print_completion_row(const char *candidate, const char *query,
                                      const repl_comp_theme_t *theme, int item_width,
                                      int selected, int member_context,
                                      const char *owner_hint) {
  if (!candidate)
    candidate = "";
  repl_comp_meta_t meta = repl_completion_meta_for(candidate, member_context, owner_hint);
  char detail[512];
  repl_completion_detail(&meta, candidate, detail, sizeof(detail));
  int name_width = visible_len(candidate);
  int cand_width = item_width;
  if (detail[0] && name_width + 3 < item_width)
    cand_width = name_width;
  if (cand_width < 0)
    cand_width = 0;
  if (theme && theme->color) {
    if (selected)
      printf("%s%s ", theme->selected, theme->selected_icon);
    else
      printf("%s  ", theme->dim);
    repl_print_fuzzy_candidate(candidate, query, theme, cand_width, selected);
    int used = name_width < cand_width ? name_width : cand_width;
    const char *fill = selected ? theme->selected : theme->dim;
    if (used < item_width) {
      printf("%s", fill);
      if (detail[0] && item_width - used > 3) {
        printf("  ");
        used += 2;
        used += repl_print_flat_trimmed(detail, item_width - used);
      }
      while (used < item_width) {
        putchar(' ');
        used++;
      }
    }
    printf("%s\n", theme->reset);
  } else {
    printf("%s ", selected && theme ? theme->selected_icon : " ");
    repl_print_trimmed(candidate, cand_width);
    int used = name_width < cand_width ? name_width : cand_width;
    if (detail[0] && item_width - used > 3) {
      printf("  ");
      used += 2;
      repl_print_flat_trimmed(detail, item_width - used);
    }
    printf("\n");
  }
}

static int repl_print_completion_preview(const char *candidate, const repl_comp_theme_t *theme,
                                         int item_width, int member_context,
                                         const char *owner_hint) {
  repl_comp_meta_t meta = repl_completion_meta_for(candidate, member_context, owner_hint);
  if (!meta.def && !meta.doc)
    return 0;
  int lines = 0;
  char detail[512];
  repl_completion_detail(&meta, candidate, detail, sizeof(detail));
  if (detail[0]) {
    if (theme && theme->color)
      printf("%s  sig %s", theme->dim, theme->item);
    else
      printf("  sig ");
    repl_print_flat_trimmed(detail, item_width > 6 ? item_width - 6 : item_width);
    if (theme && theme->color)
      printf("%s", theme->reset);
    printf("\n");
    lines++;
  }
  if (meta.doc && *meta.doc) {
    if (theme && theme->color)
      printf("%s  doc %s", theme->dim, theme->item);
    else
      printf("  doc ");
    repl_print_flat_trimmed(meta.doc, item_width > 6 ? item_width - 6 : item_width);
    if (theme && theme->color)
      printf("%s", theme->reset);
    printf("\n");
    lines++;
  }
  return lines;
}

static void repl_render_completion_popup(const char *prompt, const char *buf, int len,
                                         int pos, int prompt_cols, const char *query,
                                         char **matches, repl_comp_item_t *items,
                                         int item_count, int selected, int member_context,
                                         const char *owner_hint) {
  repl_comp_theme_t theme = repl_completion_theme();
  if (!g_vt_output_ok) {
    if (items && item_count > 0) {
      printf("\n");
      int max = item_count < 20 ? item_count : 20;
      for (int i = 0; i < max; ++i)
        printf("%s\n", matches[items[i].idx]);
    }
    return;
  }
  draw_line(prompt, buf, len, pos, prompt_cols);
  int term_cols, term_rows, cur_r, cur_c, tot_r, tot_c;
  get_term_size(&term_cols, &term_rows);
  calc_cursor(buf, pos, term_cols, prompt_cols, &cur_r, &cur_c);
  calc_cursor(buf, len, term_cols, prompt_cols, &tot_r, &tot_c);
  int item_width = term_cols - 5;
  if (item_width < 12)
    item_width = term_cols > 4 ? term_cols - 4 : term_cols;
  int max_rows = repl_completion_visible_rows(term_rows);
  int start = 0;
  if (selected >= max_rows)
    start = selected - max_rows + 1;
  if (start < 0)
    start = 0;
  int down = tot_r - cur_r;
  if (down > 0)
    printf("\x1b[%dB", down);
  printf("\n\x1b[J");
  const char *q = (query && *query) ? query : "all";
  int popup_lines = 0;
  if (theme.color)
    printf("%s%s %scomplete%s %s%s%s %s(%d match%s)%s\n",
           theme.accent, theme.search_icon, theme.dim, theme.reset,
           (query && *query) ? theme.match : theme.dim, q, theme.reset,
           theme.dim, item_count, item_count == 1 ? "" : "es", theme.reset);
  else
    printf("%s complete %s (%d match%s)\n", theme.search_icon, q, item_count,
           item_count == 1 ? "" : "es");
  popup_lines++;
  if (!items || item_count == 0) {
    if (theme.color)
      printf("%s  no matches%s\n", theme.dim, theme.reset);
    else
      printf("  no matches\n");
    popup_lines++;
  } else {
    int end = start + max_rows;
    if (end > item_count)
      end = item_count;
    for (int i = start; i < end; ++i) {
      const char *m = matches[items[i].idx];
      repl_print_completion_row(m, query, &theme, item_width, i == selected, member_context,
                                owner_hint);
      popup_lines++;
    }
    if (end < item_count) {
      if (theme.color)
        printf("%s  %s %d more%s\n", theme.dim, theme.more_icon, item_count - end,
               theme.reset);
      else
        printf("  %s %d more\n", theme.more_icon, item_count - end);
      popup_lines++;
    }
    if (selected >= 0 && selected < item_count)
      popup_lines += repl_print_completion_preview(matches[items[selected].idx], &theme, item_width,
                                                   member_context, owner_hint);
  }
  repl_return_to_editor_cursor(down + 1 + popup_lines, cur_c);
  fflush(stdout);
}

static int repl_run_completion_menu(const char *prompt, char **pbuf, int *cap,
                                    int *len, int *pos, int prompt_cols) {
  if (!pbuf || !*pbuf || !cap || !len || !pos)
    return 0;
  int start = repl_completion_word_start(*pbuf, *pos);
  int end = repl_completion_word_end(*pbuf, *len, *pos);
  char query[256];
  repl_completion_query_from_span(*pbuf, start, *pos, query, sizeof(query));
  int probe_cursor = start;
  char *probe = repl_completion_probe_line(*pbuf, *len, start, *pos, &probe_cursor);
  if (!probe)
    return 0;
  size_t count = 0;
  int member_context = repl_completion_is_member_probe(probe, probe_cursor);
  char owner_hint[32];
  repl_completion_receiver_owner(probe, probe_cursor, owner_hint, sizeof(owner_hint));
  char **matches = nytrix_get_completions_for_line(probe, probe_cursor, &count);
  free(probe);
  if (!matches || count == 0) {
    if (matches)
      nytrix_free_completions(matches, count);
    return 0;
  }
  int item_count = 0;
  repl_comp_item_t *items = repl_completion_filter(matches, count, query, &item_count);
  int selected = 0;
  int accepted = 0;
  int canceled = 0;
  repl_reserve_completion_popup(prompt, *pbuf, *len, *pos, prompt_cols);
  repl_render_completion_popup(prompt, *pbuf, *len, *pos, prompt_cols, query,
                               matches, items, item_count, selected, member_context, owner_hint);
  while (!accepted && !canceled) {
    char ch_val = 0;
    int k;
#ifdef _WIN32
    k = read_key_win(&ch_val);
#else
    k = read_key_posix(&ch_val);
#endif
    if (k == K_EOF || k == K_CTRL_C || k == K_CTRL_G || k == '\x1b' || k == K_UNKNOWN) {
      canceled = 1;
      break;
    }
    if (k == K_ENTER || k == K_TAB) {
      if (items && item_count > 0) {
        const char *choice = matches[items[selected].idx];
        repl_replace_range(pbuf, cap, len, pos, start, end, choice);
        accepted = 1;
      } else {
        canceled = 1;
      }
      break;
    }
    if (k == K_UP || k == K_CTRL_P || k == K_SHIFT_TAB || k == K_MOUSE_WHEEL_UP) {
      if (item_count > 0)
        selected = (selected - 1 + item_count) % item_count;
    } else if (k == K_DOWN || k == K_CTRL_N || k == K_MOUSE_WHEEL_DOWN) {
      if (item_count > 0)
        selected = (selected + 1) % item_count;
    } else if (k == K_PAGE_UP) {
      if (item_count > 0) {
        int rows = repl_completion_page_rows();
        selected -= rows;
        if (selected < 0)
          selected = 0;
      }
    } else if (k == K_PAGE_DOWN) {
      if (item_count > 0) {
        int rows = repl_completion_page_rows();
        selected += rows;
        if (selected >= item_count)
          selected = item_count - 1;
      }
    } else if (k == K_BACKSPACE || k == K_CTRL_BACKSPACE) {
      size_t qlen = strlen(query);
      if (qlen > 0) {
        const char *old_choice = (items && selected >= 0 && selected < item_count)
                                     ? matches[items[selected].idx]
                                     : NULL;
        query[--qlen] = '\0';
        while (qlen > 0 && ((unsigned char)query[qlen] & 0xc0) == 0x80)
          query[--qlen] = '\0';
        free(items);
        items = repl_completion_filter(matches, count, query, &item_count);
        selected = repl_completion_find_item(matches, items, item_count, old_choice);
      } else {
        canceled = 1;
      }
    } else if (k < 1000 && k > 0 && isprint((unsigned char)k)) {
      size_t qlen = strlen(query);
      if (qlen + 2 < sizeof(query)) {
        const char *old_choice = (items && selected >= 0 && selected < item_count)
                                     ? matches[items[selected].idx]
                                     : NULL;
        query[qlen++] = (char)k;
        query[qlen] = '\0';
        free(items);
        items = repl_completion_filter(matches, count, query, &item_count);
        selected = repl_completion_find_item(matches, items, item_count, old_choice);
      }
    }
    if (selected >= item_count)
      selected = item_count > 0 ? item_count - 1 : 0;
    repl_render_completion_popup(prompt, *pbuf, *len, *pos, prompt_cols, query,
                                 matches, items, item_count, selected, member_context, owner_hint);
  }
  repl_clear_completion_popup(prompt, *pbuf, *len, *pos, prompt_cols);
  free(items);
  nytrix_free_completions(matches, count);
  return accepted ? 1 : -1;
}

char *ny_readline(const char *prompt) {
#ifdef _WIN32
  HANDLE initial_hin = GetStdHandle(STD_INPUT_HANDLE);
  DWORD initial_mode = 0;
  if (!GetConsoleMode(initial_hin, &initial_mode))
    return win_readline_stdio(prompt);
  const char *win_raw_env = getenv("NYTRIX_REPL_WIN_RAW");
  if (win_raw_env && !ny_env_truthy(win_raw_env))
    return win_readline_cooked(prompt);
#endif
  int prompt_cols = visible_len(prompt);
  int cap = 256;
  char *buf = malloc(cap);
  if (!buf)
    return NULL;
  buf[0] = '\0';
  int len = 0;
  int pos = 0;
  int hist_idx = ny_hist_len;
  char *saved_buf = NULL;
  char *undo_buf = NULL;
  int undo_pos = 0;
  prev_lines = 0;
  prev_total_rows = 0;
  viewport_start = 0;
  int target_col = -1;
#ifdef _WIN32
#ifndef ENABLE_VIRTUAL_TERMINAL_INPUT
#define ENABLE_VIRTUAL_TERMINAL_INPUT 0x0200
#endif
#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif
#ifndef ENABLE_PROCESSED_INPUT
#define ENABLE_PROCESSED_INPUT 0x0001
#endif
  g_win_pending_head = g_win_pending_tail = 0;
  g_win_skip_lf_after_cr = 0;
  ny_readline_prepare_console();
  HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
  DWORD oldMode = 0;
  GetConsoleMode(hIn, &oldMode);
  DWORD rawMode =
      oldMode & ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT);
#ifdef ENABLE_VIRTUAL_TERMINAL_INPUT
  if (!SetConsoleMode(hIn, rawMode | ENABLE_VIRTUAL_TERMINAL_INPUT))
    SetConsoleMode(hIn, rawMode);
#else
  SetConsoleMode(hIn, rawMode);
#endif
  HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
  DWORD outMode = 0;
  int vt_out_ok = ny_readline_vt_output_ok();
  if (GetConsoleMode(hOut, &outMode)) {
    vt_out_ok =
        SetConsoleMode(hOut, outMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
    SetConsoleOutputCP(CP_UTF8);
  }
#else
  struct termios old_t, new_t;
  int is_tty = isatty(STDIN_FILENO);
  int vt_out_ok = 1;
  if (is_tty) {
    tcgetattr(STDIN_FILENO, &old_t);
    new_t = old_t;
    new_t.c_lflag &= ~(ICANON | ECHO | IEXTEN | ISIG);
    new_t.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    new_t.c_cflag |= CS8;
    new_t.c_cc[VMIN] = 1;
    new_t.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &new_t);
    struct sigaction sa_winch;
    memset(&sa_winch, 0, sizeof(sa_winch));
    sa_winch.sa_handler = handle_winch;
    sigemptyset(&sa_winch.sa_mask);
    sa_winch.sa_flags = 0; // Ensure SA_RESTART is NOT set
    sigaction(SIGWINCH, &sa_winch, NULL);
  }
#endif
  g_vt_output_ok = vt_out_ok;
  int sel_anchor = -1;
  int sel_mode = REPL_SEL_NONE;
  if (vt_out_ok) {
    printf("\x1b[?2004h");
    fflush(stdout);
  }
  if (vt_out_ok) {
    g_draw_sel_anchor = sel_anchor;
    g_draw_sel_mode = sel_mode;
    draw_line(prompt, buf, len, pos, prompt_cols);
  } else {
    if (prompt && *prompt)
      fputs(prompt, stdout);
    fflush(stdout);
  }
  int pasting = 0;
  int paste_start = -1;
  while (1) {
    int term_cols, term_rows;
    get_term_size(&term_cols, &term_rows);
    char ch_val = 0;
    int k;
#ifdef _WIN32
    k = read_key_win(&ch_val);
#else
    k = read_key_posix(&ch_val);
#endif
    if (k == K_EOF || k == K_CTRL_D) {
      if (len == 0) {
        free(buf);
        buf = NULL;
        if (saved_buf) {
          free(saved_buf);
          saved_buf = NULL;
        }
        if (undo_buf) {
          free(undo_buf);
          undo_buf = NULL;
        }
        if (vt_out_ok)
          printf("\n");
        break;
      } else {
        len = 0;
        pos = 0;
        buf[0] = '\0';
        repl_reset_selection_state(&sel_anchor, &sel_mode);
        draw_line(prompt, buf, len, pos, prompt_cols);
        continue;
      }
    } else if (k == K_CTRL_C) {
      len = 0;
      pos = 0;
      buf[0] = '\0';
      pasting = 0;
      paste_start = -1;
      repl_reset_selection_state(&sel_anchor, &sel_mode);
      if (vt_out_ok) {
        draw_line(prompt, buf, len, pos, prompt_cols);
        printf("^C\n");
      } else {
        printf("^C\n");
      }
      prev_lines = 0;
      prev_total_rows = 0;
      draw_line(prompt, buf, len, pos, prompt_cols);
      g_repl_sigint = 0;
      continue;
    }
    if (g_got_winch || k == -2) {
      g_got_winch = 0;
      draw_line(prompt, buf, len, pos, prompt_cols);
      if (k == -2)
        continue;
    }
    if (k == K_CTRL_G) {
      printf("^C\n");
      buf[0] = '\0';
      len = 0;
      pos = 0;
      repl_reset_selection_state(&sel_anchor, &sel_mode);
      hist_idx = ny_hist_len;
      prev_lines = 0;
      prev_total_rows = 0;
      draw_line(prompt, buf, len, pos, prompt_cols);
      continue;
    }
    if (k == K_CTRL_L) {
      printf("\x1b[2J\x1b[H");
      prev_lines = 0;
      prev_total_rows = 0;
      viewport_start = 0;
      draw_line(prompt, buf, len, pos, prompt_cols);
      continue;
    }
    if (k == K_CTRL_R) {
      char search_q[128] = {0};
      int sq_len = 0;
      int found_idx = -1;
      while (1) {
        printf("\r\x1b[K\x1b[90msearch: \x1b[33m%s\x1b[0m", search_q);
        if (found_idx != -1) {
          char display_h[80] = {0};
          const char *h = ny_hist[found_idx];
          int k_h = 0;
          while (h[k_h] && k_h < 60) {
            if (h[k_h] == '\n') {
              display_h[k_h++] = ';';
              display_h[k_h++] = ' ';
              break;
            }
            display_h[k_h] = h[k_h];
            k_h++;
          }
          display_h[k_h] = '\0';
          printf(" \x1b[90m> %s\x1b[0m", display_h);
        }
        fflush(stdout);
        char sq_ch = 0;
        int sq_k;
#ifdef _WIN32
        sq_k = read_key_win(&sq_ch);
#else
        sq_k = read_key_posix(&sq_ch);
#endif
        if (sq_k == K_ENTER)
          break;
        if (sq_k == 27 || sq_k == K_CTRL_G || sq_k == K_CTRL_C) {
          found_idx = -1;
          break;
        }
        if (sq_k == K_BACKSPACE && sq_len > 0) {
          search_q[--sq_len] = '\0';
        } else if (sq_k < 256 && sq_k > 0 && isprint(sq_ch) && sq_len < 127) {
          search_q[sq_len++] = sq_ch;
          search_q[sq_len] = '\0';
        }
        found_idx = -1;
        if (sq_len > 0) {
          for (int i = ny_hist_len - 1; i >= 0; i--) {
            if (strstr(ny_hist[i], search_q)) {
              found_idx = i;
              break;
            }
          }
        }
      }
      if (found_idx != -1) {
        if (repl_set_buffer_text(&buf, &cap, &len, &pos, ny_hist[found_idx], 128)) {
          if (search_q[0]) {
            char *hit = strstr(buf, search_q);
            if (hit)
              pos = (int)(hit - buf) + (int)strlen(search_q);
          }
          repl_reset_selection_state(&sel_anchor, &sel_mode);
        }
      }
      prev_lines = 0;
      draw_line(prompt, buf, len, pos, prompt_cols);
      continue;
    }
    if (k == K_PASTE_START) {
      repl_delete_active_selection(buf, &len, &pos, &sel_anchor, &sel_mode);
      pasting = 1;
      paste_start = pos;
      continue;
    }
    if (k == K_PASTE_END) {
      pasting = 0;
      if (paste_start >= 0 && paste_start < len) {
        int old_pos = pos;
        int r = paste_start;
        int w = paste_start;
        int is_line_start = (paste_start == 0 || buf[paste_start - 1] == '\n');
        int paste_in_str = 0;
        int paste_esc = 0;
        char paste_quote = '\0';
        while (r < old_pos) {
          if (is_line_start && !paste_in_str) {
            int r0 = r;
            while (r < old_pos && (buf[r] == ' ' || buf[r] == '\t'))
              r++;
            int pr = repl_match_pasted_prompt(buf, r, old_pos);
            if (pr != r)
              r = pr;
            else
              r = r0;
            is_line_start = 0;
            if (r >= old_pos)
              break;
          }
          if (r < old_pos) {
            if (buf[r] == '\n')
              is_line_start = 1;
            repl_paste_string_state_update(buf[r], &paste_in_str, &paste_quote, &paste_esc);
            buf[w++] = buf[r++];
          }
        }
        int removed = r - w;
        if (removed > 0) {
          memmove(&buf[w], &buf[r], len - r + 1);
          pos -= removed;
          len -= removed;
        }
      }
      paste_start = -1;
      if (!repl_is_input_pending())
        draw_line(prompt, buf, len, pos, prompt_cols);
      continue;
    }
    if (k == K_ENTER || k == K_ALT_ENTER) {
#ifdef _WIN32
      if (k == K_ENTER && !pasting && win_has_queued_text_event()) {
        if (!repl_insert_newline_indent(&buf, &cap, &len, &pos, 0, 256))
          continue;
        win_drain_queued_text(&buf, &cap, &len, &pos, 1);
        if (win_pasted_buffer_should_submit(buf, len, pos)) {
          while (len > 0 && isspace((unsigned char)buf[len - 1]))
            len--;
          buf[len] = '\0';
          pos = len;
          repl_finish_submit_display(prompt, buf, len, pos, prompt_cols, 0);
          break;
        }
        if (!repl_is_input_pending())
          draw_line(prompt, buf, len, pos, prompt_cols);
        continue;
      }
#endif
      if (pasting) {
        if (!repl_insert_newline_indent(&buf, &cap, &len, &pos, 0, 256))
          continue;
      } else {
        int complete = is_input_complete(buf);
        int at_end = (pos == len);
        int line_start = pos;
        while (line_start > 0 && buf[line_start - 1] != '\n')
          line_start--;
        int line_empty = 1;
        for (int i = line_start; i < pos; i++)
          if (!isspace((unsigned char)buf[i])) {
            line_empty = 0;
            break;
          }
        int force_submit = (k == K_ALT_ENTER);
        if (at_end && (complete || line_empty || force_submit) && !pasting) {
          while (len > 0 && isspace((unsigned char)buf[len - 1]))
            len--;
          buf[len] = '\0';
          pos = len;
          repl_finish_submit_display(prompt, buf, len, pos, prompt_cols, 1);
          break;
        } else {
          int indent = pasting ? 0 : repl_calc_indent(buf);
          if (!repl_insert_newline_indent(&buf, &cap, &len, &pos, indent, 256))
            continue;
        }
      }
      if (!pasting && !repl_is_input_pending())
        draw_line(prompt, buf, len, pos, prompt_cols);
      continue;
    }
    if (k == K_TAB) {
      if (repl_sel_active(sel_anchor, pos)) {
        repl_delete_active_selection(buf, &len, &pos, &sel_anchor, &sel_mode);
      }
      int handled = repl_run_completion_menu(prompt, &buf, &cap, &len, &pos, prompt_cols);
      if (handled == 0 && !repl_has_completion_prefix(buf, pos)) {
        int spaces = 3;
        if (!repl_reserve_buf(&buf, &cap, len + spaces, 128))
          continue;
        memmove(buf + pos + spaces, buf + pos, len - pos + 1);
        for (int i = 0; i < spaces; i++)
          buf[pos++] = ' ';
        len += spaces;
      }
      draw_line(prompt, buf, len, pos, prompt_cols);
      continue;
    }
    if (k == K_SHIFT_TAB) {
      if (repl_sel_active(sel_anchor, pos)) {
        repl_reset_selection_state(&sel_anchor, &sel_mode);
      }
      repl_run_completion_menu(prompt, &buf, &cap, &len, &pos, prompt_cols);
      draw_line(prompt, buf, len, pos, prompt_cols);
      continue;
    }
    if (k == K_CTRL_Z) {
      if (undo_buf) {
        char *redo = ny_strdup(buf);
        int redo_pos = pos;
        if (repl_set_buffer_text(&buf, &cap, &len, &pos, undo_buf, 256)) {
          pos = undo_pos <= len ? undo_pos : len;
          free(undo_buf);
          undo_buf = redo;
          undo_pos = redo_pos;
          repl_reset_selection_state(&sel_anchor, &sel_mode);
          prev_lines = 0;
          prev_total_rows = 0;
        } else {
          free(redo);
        }
      }
      draw_line(prompt, buf, len, pos, prompt_cols);
      continue;
    }
    if (k != K_UP && k != K_DOWN && k != K_CTRL_P && k != K_CTRL_N) {
      target_col = -1;
    }
    int mutating_key =
        k == K_BACKSPACE || k == K_CTRL_D || k == K_DEL || k == K_CTRL_BACKSPACE ||
        k == K_CTRL_DEL || k == K_CTRL_K || k == K_CTRL_Y || k == K_CTRL_U ||
        k == K_CTRL_W || (k < 1000 && k > 0 && isprint((unsigned char)k));
    if (mutating_key) {
      char *snapshot = ny_strdup(buf);
      if (snapshot) {
        free(undo_buf);
        undo_buf = snapshot;
        undo_pos = pos;
      }
    }
    int old_pos = pos;
    int moved_cursor = 0;
    int extend_linear =
        (k == K_SHIFT_LEFT || k == K_SHIFT_RIGHT || k == K_SHIFT_UP || k == K_SHIFT_DOWN ||
         k == K_SHIFT_HOME || k == K_SHIFT_END || k == K_CTRL_SHIFT_LEFT ||
         k == K_CTRL_SHIFT_RIGHT || k == K_CTRL_SHIFT_UP || k == K_CTRL_SHIFT_DOWN);
    int extend_block = (k == K_ALT_SHIFT_LEFT || k == K_ALT_SHIFT_RIGHT || k == K_ALT_SHIFT_UP ||
                        k == K_ALT_SHIFT_DOWN || k == K_ALT_SHIFT_HOME || k == K_ALT_SHIFT_END);
    int extend_sel = extend_linear || extend_block;
    if (k == K_BACKSPACE) {
      if (!repl_delete_active_selection(buf, &len, &pos, &sel_anchor, &sel_mode) && pos > 0) {
        int line_start = pos;
        while (line_start > 0 && buf[line_start - 1] != '\n')
          line_start--;
        int all_spaces = 1;
        for (int i = line_start; i < pos; i++)
          if (buf[i] != ' ') {
            all_spaces = 0;
            break;
          }
        int skip = 1;
        if (all_spaces && (pos - line_start) >= 3 && (pos - line_start) % 3 == 0)
          skip = 3;
        else {
          while (pos - skip > 0 && (buf[pos - skip] & 0xc0) == 0x80)
            skip++;
        }
        memmove(buf + pos - skip, buf + pos, len - pos + 1);
        pos -= skip;
        len -= skip;
      }
    } else if (k == K_CTRL_D) {
      if (len == 0) {
        free(buf);
        buf = NULL;
        break;
      } else if (!repl_delete_active_selection(buf, &len, &pos, &sel_anchor, &sel_mode) &&
                 pos < len) {
        int skip = 1;
        while (pos + skip < len && (buf[pos + skip] & 0xc0) == 0x80)
          skip++;
        memmove(buf + pos, buf + pos + skip, len - pos - skip + 1);
        len -= skip;
      }
    } else if (k == K_DEL) {
      if (!repl_delete_active_selection(buf, &len, &pos, &sel_anchor, &sel_mode) && pos < len) {
        int skip = 1;
        while (pos + skip < len && (buf[pos + skip] & 0xc0) == 0x80)
          skip++;
        memmove(buf + pos, buf + pos + skip, len - pos - skip + 1);
        len -= skip;
      }
    } else if (k == K_LEFT || k == K_CTRL_B || k == K_SHIFT_LEFT) {
      if (pos > 0) {
        pos--;
        while (pos > 0 && (buf[pos] & 0xc0) == 0x80)
          pos--;
        moved_cursor = 1;
      }
    } else if (k == K_RIGHT || k == K_CTRL_F || k == K_SHIFT_RIGHT) {
      if (pos < len) {
        pos++;
        while (pos < len && (buf[pos] & 0xc0) == 0x80)
          pos++;
        moved_cursor = 1;
      }
    } else if (k == K_CTRL_LEFT || k == K_ALT_LEFT || k == K_CTRL_SHIFT_LEFT ||
               k == K_ALT_SHIFT_LEFT) {
      if (pos > 0) {
        pos--;
        while (pos > 0 && (buf[pos] & 0xc0) == 0x80)
          pos--;
        while (pos > 0 && isspace((unsigned char)buf[pos - 1])) {
          pos--;
          while (pos > 0 && (buf[pos] & 0xc0) == 0x80)
            pos--;
        }
        while (pos > 0 && !is_break_char(buf[pos - 1])) {
          pos--;
          while (pos > 0 && (buf[pos] & 0xc0) == 0x80)
            pos--;
        }
        moved_cursor = 1;
      }
    } else if (k == K_CTRL_RIGHT || k == K_ALT_RIGHT || k == K_CTRL_SHIFT_RIGHT ||
               k == K_ALT_SHIFT_RIGHT) {
      if (pos < len) {
        while (pos < len && !is_break_char(buf[pos]))
          pos++;
        while (pos < len && isspace((unsigned char)buf[pos]))
          pos++;
        moved_cursor = 1;
      }
    } else if (k == K_CTRL_BACKSPACE) {
      if (!repl_delete_active_selection(buf, &len, &pos, &sel_anchor, &sel_mode) && pos > 0) {
        int start = pos;
        pos--;
        while (pos > 0 && isspace((unsigned char)buf[pos - 1]))
          pos--;
        while (pos > 0 && !is_break_char(buf[pos - 1]))
          pos--;
        int klen = start - pos;
        repl_set_kill_ring(buf + pos, klen);
        memmove(buf + pos, buf + start, len - start + 1);
        len -= klen;
      }
    } else if (k == K_CTRL_DEL) {
      if (!repl_delete_active_selection(buf, &len, &pos, &sel_anchor, &sel_mode) && pos < len) {
        int end = pos;
        while (end < len && !is_break_char(buf[end]))
          end++;
        while (end < len && isspace((unsigned char)buf[end]))
          end++;
        int klen = end - pos;
        repl_set_kill_ring(buf + pos, klen);
        memmove(buf + pos, buf + end, len - end + 1);
        len -= klen;
      }
    } else if (k == K_UP || k == K_CTRL_P || k == K_SHIFT_UP || k == K_CTRL_SHIFT_UP ||
               k == K_ALT_SHIFT_UP) {
      int cur_r, cur_c;
      calc_cursor(buf, pos, term_cols, prompt_cols, &cur_r, &cur_c);
      if (target_col == -1)
        target_col = cur_c;
      if (cur_r > 0) {
        pos = visual_to_pos(buf, len, cur_r - 1, target_col, term_cols, prompt_cols);
        moved_cursor = 1;
      } else {
        if (ny_hist_len > 0 && hist_idx > 0) {
          if (hist_idx == ny_hist_len) {
            free(saved_buf);
            saved_buf = ny_strdup(buf);
          }
          int next_hist_idx = hist_idx - 1;
          if (repl_set_buffer_text(&buf, &cap, &len, &pos, ny_hist[next_hist_idx], 256)) {
            hist_idx = next_hist_idx;
            repl_reset_selection_state(&sel_anchor, &sel_mode);
            prev_lines = 0;
            prev_total_rows = 0;
            moved_cursor = 1;
          }
        }
      }
    } else if (k == K_DOWN || k == K_CTRL_N || k == K_SHIFT_DOWN || k == K_CTRL_SHIFT_DOWN ||
               k == K_ALT_SHIFT_DOWN) {
      int cur_r, cur_c, tot_r, tot_c;
      calc_cursor(buf, pos, term_cols, prompt_cols, &cur_r, &cur_c);
      calc_cursor(buf, len, term_cols, prompt_cols, &tot_r, &tot_c);
      if (target_col == -1)
        target_col = cur_c;
      if (cur_r < tot_r) {
        pos = visual_to_pos(buf, len, cur_r + 1, target_col, term_cols, prompt_cols);
        moved_cursor = 1;
      } else {
        if (hist_idx < ny_hist_len) {
          int next_hist_idx = hist_idx + 1;
          const char *next =
              next_hist_idx == ny_hist_len ? (saved_buf ? saved_buf : "") : ny_hist[next_hist_idx];
          if (repl_set_buffer_text(&buf, &cap, &len, &pos, next, 256)) {
            hist_idx = next_hist_idx;
            repl_reset_selection_state(&sel_anchor, &sel_mode);
            if (saved_buf && hist_idx == ny_hist_len) {
              free(saved_buf);
              saved_buf = NULL;
            }
            prev_lines = 0;
            prev_total_rows = 0;
            moved_cursor = 1;
          }
        } else if (len > 0) {
          len = 0;
          pos = 0;
          buf[0] = '\0';
          repl_reset_selection_state(&sel_anchor, &sel_mode);
          prev_lines = 0;
          prev_total_rows = 0;
          moved_cursor = 1;
        }
      }
    } else if (k == K_HOME || k == K_CTRL_A || k == K_SHIFT_HOME || k == K_ALT_SHIFT_HOME) {
      int line_start = pos;
      while (line_start > 0 && buf[line_start - 1] != '\n')
        line_start--;
      int indent_end = line_start;
      while (indent_end < len && buf[indent_end] == ' ')
        indent_end++;
      if (pos == indent_end)
        pos = line_start;
      else
        pos = indent_end;
      moved_cursor = (pos != old_pos);
    } else if (k == K_END || k == K_CTRL_E || k == K_SHIFT_END || k == K_ALT_SHIFT_END) {
      while (pos < len && buf[pos] != '\n')
        pos++;
      moved_cursor = (pos != old_pos);
    } else if (k == K_CTRL_K) {
      if (!repl_delete_active_selection(buf, &len, &pos, &sel_anchor, &sel_mode)) {
        int line_end = pos;
        while (line_end < len && buf[line_end] != '\n')
          line_end++;
        if (line_end == pos && line_end < len && buf[line_end] == '\n')
          line_end++;
        int klen = line_end - pos;
        repl_set_kill_ring(buf + pos, klen);
        memmove(buf + pos, buf + line_end, len - line_end + 1);
        len -= klen;
      }
    } else if (k == K_CTRL_Y) {
      if (kill_ring) {
        int klen = (int)strlen(kill_ring);
        repl_delete_active_selection(buf, &len, &pos, &sel_anchor, &sel_mode);
        if (!repl_insert_bytes(&buf, &cap, &len, &pos, kill_ring, klen, 128))
          continue;
      }
    } else if (k == K_CTRL_U) {
      if (!repl_delete_active_selection(buf, &len, &pos, &sel_anchor, &sel_mode)) {
        int line_start = pos;
        while (line_start > 0 && buf[line_start - 1] != '\n')
          line_start--;
        int klen = pos - line_start;
        repl_set_kill_ring(buf + line_start, klen);
        memmove(buf + line_start, buf + pos, len - pos + 1);
        len -= klen;
        pos = line_start;
      }
    } else if (k == K_CTRL_W) {
      if (!repl_delete_active_selection(buf, &len, &pos, &sel_anchor, &sel_mode)) {
        int start = pos;
        while (start > 0 && isspace((unsigned char)buf[start - 1]))
          start--;
        while (start > 0 && !is_break_char(buf[start - 1]))
          start--;
        int klen = pos - start;
        if (klen > 0) {
          repl_set_kill_ring(buf + start, klen);
          memmove(buf + start, buf + pos, len - pos + 1);
          len -= klen;
          pos = start;
        }
      }
    } else if (k == K_CTRL_L) {
      if (g_vt_output_ok) {
        printf("\x1b[H\x1b[2J");
        prev_lines = 0;
        prev_total_rows = 0;
      }
    } else if (k < 1000 && k > 0) {
      repl_delete_active_selection(buf, &len, &pos, &sel_anchor, &sel_mode);
      char byte = (char)k;
      if (!repl_insert_bytes(&buf, &cap, &len, &pos, &byte, 1, 256))
        continue;
#ifdef _WIN32
      win_drain_queued_text(&buf, &cap, &len, &pos, 0);
#endif
    }
    if (moved_cursor) {
      if (extend_sel) {
        repl_sel_begin(&sel_anchor, old_pos);
        sel_mode = extend_block ? REPL_SEL_BLOCK : REPL_SEL_LINEAR;
      } else {
        repl_sel_clear(&sel_anchor);
        sel_mode = REPL_SEL_NONE;
      }
    } else if (!extend_sel &&
               (k == K_HOME || k == K_END || k == K_LEFT || k == K_RIGHT || k == K_CTRL_B ||
                k == K_CTRL_F || k == K_CTRL_LEFT || k == K_CTRL_RIGHT || k == K_ALT_LEFT ||
                k == K_ALT_RIGHT || k == K_UP || k == K_DOWN || k == K_CTRL_P || k == K_CTRL_N)) {
      repl_sel_clear(&sel_anchor);
      sel_mode = REPL_SEL_NONE;
    }
    if (!pasting && !repl_is_input_pending()) {
      g_draw_sel_anchor = sel_anchor;
      g_draw_sel_mode = sel_mode;
      draw_line(prompt, buf, len, pos, prompt_cols);
    }
  }
  if (vt_out_ok) {
    printf("\x1b[0m\x1b[?25h\x1b[?2004l");
    fflush(stdout);
  }
#ifdef _WIN32
  SetConsoleMode(hIn, oldMode);
#else
  if (is_tty)
    tcsetattr(STDIN_FILENO, TCSANOW, &old_t);
#endif
  if (saved_buf)
    free(saved_buf);
  if (undo_buf)
    free(undo_buf);
  prev_lines = 0;
  prev_total_rows = 0;
  return buf;
}
