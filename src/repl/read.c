#include "repl/read.h"
#include "base/common.h"
#include "base/util.h"
#include "repl/priv.h"
#include "repl/types.h"
#include <ctype.h>
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
#endif

#ifndef _WIN32
typedef int sig_atomic_t;
#endif

extern int g_repl_sigint;
static char **ny_hist = NULL;
static int ny_hist_len = 0;
static int ny_hist_cap = 0;
static int ny_hist_max = 1000;

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
    ny_hist_cap = ny_hist_cap ? ny_hist_cap * 2 : 64;
    ny_hist = realloc(ny_hist, sizeof(char *) * ny_hist_cap);
  }
  ny_hist[ny_hist_len++] = ny_strdup(line);
}

void ny_readline_stifle_history(int max) { ny_hist_max = max > 0 ? max : 1000; }

int ny_readline_read_history(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return 0;
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
  for (int i = 0; i < ny_hist_len; i++) {
    if (ny_hist[i])
      fprintf(f, "%s\n", ny_hist[i]);
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
  K_CTRL_G,
  K_PASTE_START,
  K_PASTE_END,
  K_PASTE_CHAR,
  K_EOF,
  K_SHIFT_UP,
  K_SHIFT_DOWN,
  K_SHIFT_LEFT,
  K_SHIFT_RIGHT,
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
  K_UNKNOWN
};

static volatile int g_got_winch = 0;

#ifndef _WIN32
static void handle_winch(int sig) {
  (void)sig;
  g_got_winch = 1;
}
#endif

extern int g_repl_sigint;

#ifndef _WIN32
static int get_char_timeout(int ms) {
  struct pollfd pfd = {STDIN_FILENO, POLLIN, 0};
  if (poll(&pfd, 1, ms) > 0) {
    unsigned char c = 0;
    if (read(STDIN_FILENO, &c, 1) == 1)
      return c;
  }
  return -1;
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
  if (c == 1)
    return K_CTRL_A;
  if (c == 2)
    return K_CTRL_B;
  if (c == 3)
    return K_CTRL_C;
  if (c == 5)
    return K_CTRL_E;
  if (c == 6)
    return K_CTRL_F;
  if (c == 11)
    return K_CTRL_K;
  if (c == 12)
    return K_CTRL_L;
  if (c == 14)
    return K_CTRL_N;
  if (c == 16)
    return K_CTRL_P;
  if (c == 18)
    return K_CTRL_R;
  if (c == 21)
    return K_CTRL_U;
  if (c == 23)
    return K_CTRL_W;

  if (c == '\x1b') {
    int seq1 = get_char_timeout(250);
    if (seq1 == -1)
      return '\x1b';
    if (seq1 == '\r' || seq1 == '\n')
      return K_ALT_ENTER;
    if (seq1 == '[') {
      int seq2 = get_char_timeout(100);
      if (seq2 == -1)
        return K_UNKNOWN;
      if (seq2 == 'Z')
        return K_SHIFT_TAB;
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
        int seq3 = get_char_timeout(100);
        if (seq3 == ';') {
          int seq4 = get_char_timeout(100);
          int seq5 = get_char_timeout(100);
          if (seq4 == '5') { // Ctrl
            if (seq5 == 'C')
              return K_CTRL_RIGHT;
            if (seq5 == 'D')
              return K_CTRL_LEFT;
          } else if (seq4 == '3') { // Alt
            if (seq5 == 'C')
              return K_ALT_RIGHT;
            if (seq5 == 'D')
              return K_ALT_LEFT;
          }
        }
      }
      if (seq2 == '2') {
        int seq3 = get_char_timeout(100);
        if (seq3 == '0') {
          int seq4 = get_char_timeout(100);
          int seq5 = get_char_timeout(100);
          if (seq4 == '0' && seq5 == '~')
            return K_PASTE_START;
          if (seq4 == '1' && seq5 == '~')
            return K_PASTE_END;
        }
      }
      if (seq2 >= '0' && seq2 <= '9') {
        int seq3 = get_char_timeout(100);
        if (seq3 == '~') {
          if (seq2 == '1' || seq2 == '7')
            return K_HOME;
          if (seq2 == '4' || seq2 == '8')
            return K_END;
          if (seq2 == '3')
            return K_DEL;
          if (seq2 == '5')
            return K_UP;
        } else if (seq3 == ';') {
          int seq4 = get_char_timeout(100);
          int seq5 = get_char_timeout(100);
          if (seq2 == '3' && seq4 == '5' && seq5 == '~')
            return K_CTRL_DEL;
        }
      }
    } else if (seq1 == 'f')
      return K_ALT_RIGHT;
    else if (seq1 == 'b')
      return K_ALT_LEFT;
    else if (seq1 == 127 || seq1 == 8)
      return K_CTRL_BACKSPACE;
    else if (seq1 == 'O') {
      int seq2 = get_char_timeout(100);
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
/*
 * Windows console key reader.
 *
 * Design notes:
 *  - Uses ReadConsoleInputW (wide) so Unicode chars work correctly.
 *  - VK codes handle navigation keys; uChar.UnicodeChar handles printable.
 *  - ESC sequences (bracketed paste \x1b[200~ / \x1b[201~, VT nav) are
 *    parsed with a short timeout so pasted text works correctly.
 *  - Non-ASCII Unicode is UTF-8–encoded; continuation bytes are queued in
 *    a ring buffer so multi-byte chars arrive one byte at a time.
 *  - ENABLE_PROCESSED_INPUT must be OFF so Ctrl+C comes as ch==3, not
 *    SIGINT.
 */

/* UTF-8 continuation byte queue */
static unsigned char g_win_pending[8];
static int g_win_pending_head = 0;
static int g_win_pending_tail = 0;

static void win_queue_byte(unsigned char b) {
  g_win_pending[g_win_pending_tail & 7] = b;
  g_win_pending_tail++;
}
static int win_dequeue_byte(void) {
  if (g_win_pending_head == g_win_pending_tail)
    return -1;
  return (int)g_win_pending[g_win_pending_head++ & 7];
}

/*
 * Read the Unicode char of the next key-down event, with a timeout in ms.
 * Pass ms < 0 for infinite wait.  Returns -1 on timeout / no char.
 *
 * Used only for parsing VT escape sequences that arrive as individual
 * character events (e.g. the bracketed-paste markers \x1b[200~ / \x1b[201~
 * sent by Windows Terminal when ENABLE_VIRTUAL_TERMINAL_INPUT is set).
 */
static int win_read_char_ms(HANDLE hIn, int ms) {
  DWORD deadline = (ms >= 0) ? (GetTickCount() + (DWORD)ms) : 0xFFFFFFFFu;
  while (1) {
    DWORD now  = GetTickCount();
    DWORD left = (ms < 0) ? INFINITE
                           : (deadline > now ? deadline - now : 0);
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
    if (rec.EventType != KEY_EVENT)    continue; /* drain mouse/focus/etc */
    if (!rec.Event.KeyEvent.bKeyDown)  continue; /* ignore key-up */
    WCHAR wc = rec.Event.KeyEvent.uChar.UnicodeChar;
    return wc ? (int)(unsigned int)wc : -1;
  }
}

static int win_parse_esc(HANDLE hIn, int ms) {
  int s1 = win_read_char_ms(hIn, ms);
  if (s1 < 0)
    return K_UNKNOWN; /* bare ESC */

  if (s1 == '[') {
    int s2 = win_read_char_ms(hIn, ms);
    if (s2 < 0) return K_UNKNOWN;

    /* Bracketed paste: \x1b[200~ and \x1b[201~ */
    if (s2 == '2') {
      int s3 = win_read_char_ms(hIn, ms);
      if (s3 == '0') {
        int s4 = win_read_char_ms(hIn, ms);
        if (s4 == '0') {
          int s5 = win_read_char_ms(hIn, ms);
          if (s5 == '~') return K_PASTE_START;
        } else if (s4 == '1') {
          int s5 = win_read_char_ms(hIn, ms);
          if (s5 == '~') return K_PASTE_END;
        }
      }
      return K_UNKNOWN;
    }

    /* Single-letter CSI sequence */
    if (s2 == 'A') return K_UP;
    if (s2 == 'B') return K_DOWN;
    if (s2 == 'C') return K_RIGHT;
    if (s2 == 'D') return K_LEFT;
    if (s2 == 'H') return K_HOME;
    if (s2 == 'F') return K_END;
    if (s2 == 'Z') return K_SHIFT_TAB;

    /* Numeric CSI sequences: \x1b[N~ */
    if (s2 >= '1' && s2 <= '9') {
      int s3 = win_read_char_ms(hIn, ms);
      if (s3 == '~') {
        if (s2 == '1' || s2 == '7') return K_HOME;
        if (s2 == '4' || s2 == '8') return K_END;
        if (s2 == '3')              return K_DEL;
        if (s2 == '5')              return K_UP;
        if (s2 == '6')              return K_DOWN;
      }
    }
    return K_UNKNOWN;
  }

  if (s1 == 'O') {
    int s2 = win_read_char_ms(hIn, ms);
    if (s2 == 'A') return K_UP;
    if (s2 == 'B') return K_DOWN;
    if (s2 == 'C') return K_RIGHT;
    if (s2 == 'D') return K_LEFT;
    if (s2 == 'H') return K_HOME;
    if (s2 == 'F') return K_END;
    return K_UNKNOWN;
  }

  /* Alt+letter (ESC letter) or other two-char sequences – ignore */
  return K_UNKNOWN;
}

static int read_key_win(char *out_chr) {
  /* Drain any queued UTF-8 continuation bytes first */
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
      continue; /* drain mouse / focus / resize */

    KEY_EVENT_RECORD k = rec.Event.KeyEvent;
    if (!k.bKeyDown)
      continue;

    WORD  vk  = k.wVirtualKeyCode;
    WCHAR wch = k.uChar.UnicodeChar;
    char  ch  = (wch >= 1 && wch < 128) ? (char)(unsigned char)wch : 0;

    int ctrl = (k.dwControlKeyState &
                (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
    int alt  = (k.dwControlKeyState &
                (LEFT_ALT_PRESSED  | RIGHT_ALT_PRESSED))  != 0;

    /* ESC / VT escape sequences
     * Check BEFORE the VK dispatch so VK_ESCAPE goes through here too.
     * Bracketed paste (\x1b[200~ / \x1b[201~) arrives as individual char
     * events when ENABLE_VIRTUAL_TERMINAL_INPUT is active. */
    if (wch == 0x1B || vk == VK_ESCAPE) {
      int r = win_parse_esc(hIn, 60); /* 60 ms per-char timeout */
      if (r == K_UNKNOWN) continue;   /* bare ESC or unrecognised seq */
      return r;
    }

    /* Navigation / special keys (by VK) */
    if (vk == VK_RETURN)  return K_ENTER;
    if (vk == VK_UP)      return alt ? K_ALT_UP    : K_UP;
    if (vk == VK_DOWN)    return alt ? K_ALT_DOWN  : K_DOWN;
    if (vk == VK_LEFT)    return ctrl ? K_CTRL_LEFT  : alt ? K_ALT_LEFT  : K_LEFT;
    if (vk == VK_RIGHT)   return ctrl ? K_CTRL_RIGHT : alt ? K_ALT_RIGHT : K_RIGHT;
    if (vk == VK_HOME)    return K_HOME;
    if (vk == VK_END)     return K_END;
    if (vk == VK_BACK)    return ctrl ? K_CTRL_BACKSPACE : K_BACKSPACE;
    if (vk == VK_DELETE)  return ctrl ? K_CTRL_DEL : K_DEL;
    if (vk == VK_TAB)
      return (k.dwControlKeyState & SHIFT_PRESSED) ? K_SHIFT_TAB : K_TAB;
    if (vk == VK_PRIOR)   return K_UP;   /* Page Up  → history up  */
    if (vk == VK_NEXT)    return K_DOWN; /* Page Dn  → history down */
    if (vk == VK_INSERT)  continue;      /* ignore Insert */

    /* Ctrl+letter (ch == 1..31) */
    if (ch == '\r' || ch == '\n') return K_ENTER;
    if (ch == 1)  return K_CTRL_A;
    if (ch == 2)  return K_CTRL_B;
    if (ch == 3)  return K_CTRL_C;   /* Ctrl+C — needs PROCESSED_INPUT off */
    if (ch == 4)  return K_EOF;       /* Ctrl+D */
    if (ch == 5)  return K_CTRL_E;
    if (ch == 6)  return K_CTRL_F;
    if (ch == 11) return K_CTRL_K;
    if (ch == 12) return K_CTRL_L;
    if (ch == 14) return K_CTRL_N;
    if (ch == 16) return K_CTRL_P;
    if (ch == 18) return K_CTRL_R;
    if (ch == 21) return K_CTRL_U;
    if (ch == 23) return K_CTRL_W;
    if (ch == 25) return K_CTRL_Y;
    if (ch == 26) return K_EOF;       /* Ctrl+Z — Windows EOF */

    /* Printable ASCII */
    if (ch >= 32 && ch != 127) {
      *out_chr = ch;
      return (unsigned char)ch;
    }

    /* Non-ASCII BMP Unicode -> UTF-8 */
    if (wch >= 0x80) {
      if (wch >= 0xD800 && wch <= 0xDFFF)
        continue; /* lone surrogate — skip */
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

    /* Unrecognised VK (F-keys, IME, dead keys, etc.) — skip */
    (void)ctrl; (void)alt;
  }
}
#endif

/* 1 when VT/ANSI escape output works (always on POSIX; depends on
 * ENABLE_VIRTUAL_TERMINAL_PROCESSING on Windows). */
static int g_vt_output_ok = 1;

static int is_break_char(char c) {
  return isspace((unsigned char)c) || strchr("()[]{}\"'.@$><=;|&", c) != NULL;
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
        while (*s && ((*s >= '0' && *s <= '9') || *s == ';' || *s == '?' ||
                      *s == ' '))
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

static void calc_cursor(const char *buf, int pos, int term_cols,
                        int prompt_cols, int *r, int *c) {
  int row = 0;
  int col = prompt_cols;
  while (col >= term_cols) {
    row++;
    col -= term_cols;
  }
  for (int i = 0; i < pos; i++) {
    if (buf[i] == '\n') {
      row++;
      col = 3;
    } else if (buf[i] == '\t') {
      if (col >= term_cols) {
        row++;
        col = 0;
      }
      col += 4;
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

static int visual_to_pos(const char *buf, int len, int target_r, int target_c,
                         int term_cols, int prompt_cols) {
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
      col = 3;
    } else if (buf[i] == '\t') {
      if (col >= term_cols) {
        row++;
        col = 0;
      }
      col += 4;
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

static void buf_row_slice(const char *buf, int term_cols, int prompt_cols,
                          int start_r, int max_r, int *out_start,
                          int *out_len) {
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
      col = 3;
    } else if (buf[i] == '\t') {
      if (col >= term_cols) {
        row++;
        col = 0;
      }
      col += 4;
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

static void draw_line(const char *prompt, const char *buf, int len, int pos,
                      int prompt_cols) {
  /* On non-VT terminals (old CMD.EXE), avoid sending raw escape codes */
  if (!g_vt_output_ok) {
    (void)len; (void)pos; (void)prompt_cols;
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

  if (term_cols != prev_cols) {
    if (prev_cols != 0) {
      printf("\r\n");
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
    buf_row_slice(buf, term_cols, prompt_cols, viewport_start, max_code_rows,
                  &s_off, &s_len);
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
    int sub_prompt_cols = (viewport_start == 0) ? prompt_cols : 3;
    calc_cursor(sub, adj_pos, term_cols, sub_prompt_cols, &sub_cur_r,
                &sub_cur_c);
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
    repl_highlight_line_ex(sub, adj_pos, "\x1b[90m..|\x1b[0m");
    if (has_bottom) {
      printf("\r\n\x1b[90m%d rows below (%d%%)\x1b[K", tot_r - cur_end_row,
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
    repl_highlight_line_ex(buf, pos, "\x1b[90m..|\x1b[0m");
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

char *ny_readline(const char *prompt) {
  int prompt_cols = visible_len(prompt);

  int cap = 256;
  char *buf = malloc(cap);
  buf[0] = '\0';
  int len = 0;
  int pos = 0;

  int hist_idx = ny_hist_len;
  char *saved_buf = NULL;

  prev_lines = 0;
  prev_total_rows = 0;
  viewport_start = 0;
  int target_col = -1;

#ifdef _WIN32
#ifndef ENABLE_VIRTUAL_TERMINAL_INPUT
#define ENABLE_VIRTUAL_TERMINAL_INPUT  0x0200
#endif
#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif
#ifndef ENABLE_PROCESSED_INPUT
#define ENABLE_PROCESSED_INPUT 0x0001
#endif
  /* Reset Windows UTF-8 continuation byte queue on each readline call */
  g_win_pending_head = g_win_pending_tail = 0;

  HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
  DWORD oldMode = 0;
  GetConsoleMode(hIn, &oldMode);
  /* Raw mode:
   *  - Clear ENABLE_LINE_INPUT    so we get chars one at a time
   *  - Clear ENABLE_ECHO_INPUT    so we control echo ourselves
   *  - Clear ENABLE_PROCESSED_INPUT so Ctrl+C arrives as key ch==3
   *    instead of raising SIGINT, which we cannot cleanly intercept
   */
  SetConsoleMode(hIn,
                 (oldMode & ~(ENABLE_LINE_INPUT |
                              ENABLE_ECHO_INPUT |
                              ENABLE_PROCESSED_INPUT)) |
                 ENABLE_VIRTUAL_TERMINAL_INPUT);

  HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
  DWORD oldOutMode = 0;
  int vt_out_ok = 0;
  if (GetConsoleMode(hOut, &oldOutMode)) {
    vt_out_ok = (SetConsoleMode(
        hOut, oldOutMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0);
  }
  /* UTF-8 code pages so non-ASCII input/output works (Windows 10 1903+) */
  UINT oldInCP  = GetConsoleCP();
  UINT oldOutCP = GetConsoleOutputCP();
  SetConsoleCP(CP_UTF8);
  SetConsoleOutputCP(CP_UTF8);
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

    signal(SIGWINCH, handle_winch);
  }
#endif
  g_vt_output_ok = vt_out_ok;
  if (vt_out_ok) {
    printf("\x1b[?2004h"); // Enable bracketed paste
    fflush(stdout);
  }

  if (vt_out_ok) {
    draw_line(prompt, buf, len, pos, prompt_cols);
  } else {
    if (prompt && *prompt)
      fputs(prompt, stdout);
    fflush(stdout);
  }

  int pasting = 0;
  int paste_start = -1;

  int last_was_tab = 0;
  char **comp_matches = NULL;
  size_t comp_count = 0;
  int comp_idx = -1;

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
        if (saved_buf)
          free(saved_buf);
        if (comp_matches)
          nytrix_free_completions(comp_matches, comp_count);
        return NULL;
      } else {
        len = 0;
        pos = 0;
        buf[0] = '\0';
        draw_line(prompt, buf, len, pos, prompt_cols);
        continue;
      }
    } else if (k == K_CTRL_C) {
      len = 0;
      pos = 0;
      buf[0] = '\0';
      printf("\n");
      prev_lines = 0;
      prev_total_rows = 0;
      draw_line(prompt, buf, len, pos, prompt_cols);
      g_repl_sigint = 0;
      continue;
    }

    if (g_got_winch) {
      g_got_winch = 0;
      draw_line(prompt, buf, len, pos, prompt_cols);
    }

    if (k == K_CTRL_C || k == K_CTRL_G) {
      printf("^C\n");
      buf[0] = '\0';
      len = 0;
      pos = 0;
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

    if (k == K_CTRL_R || k == K_CTRL_F) {
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
          printf(" \x1b[90m» %s\x1b[0m", display_h);
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
        free(buf);
        buf = ny_strdup(ny_hist[found_idx]);
        len = strlen(buf);
        cap = len + 128;
        buf = realloc(buf, cap);
        pos = len;
      }
      prev_lines = 0;
      draw_line(prompt, buf, len, pos, prompt_cols);
      continue;
    }

    if (k == K_PASTE_START) {
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
        while (r < old_pos) {
          if (is_line_start) {
            int r0 = r;
            while (r < old_pos && (buf[r] == ' ' || buf[r] == '\t'))
              r++;
            if (r + 4 <= old_pos && !memcmp(&buf[r], "ny> ", 4))
              r += 4;
            else if (r + 4 <= old_pos && !memcmp(&buf[r], "ny! ", 4))
              r += 4;
            else if (r + 5 <= old_pos && !memcmp(&buf[r], "..|  ", 5))
              r += 5;
            else if (r + 3 <= old_pos && !memcmp(&buf[r], "..|", 3))
              r += 3;
            else
              r = r0;
            is_line_start = 0;
            if (r >= old_pos)
              break;
          }
          if (r < old_pos) {
            if (buf[r] == '\n')
              is_line_start = 1;
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
      if (pasting) {
        if (len + 1 >= cap) {
          cap += 256;
          buf = realloc(buf, cap);
        }
        memmove(buf + pos + 1, buf + pos, len - pos + 1);
        buf[pos++] = '\n';
        len++;
        buf[len] = '\0';
      } else {
        int complete = is_input_complete(buf);
        int input_pending = (pasting || repl_is_input_pending());
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
        if (at_end && (complete || line_empty || force_submit) &&
            !input_pending) {
          while (len > 0 && isspace((unsigned char)buf[len - 1]))
            len--;
          buf[len] = '\0';
          pos = len;
          if (!g_vt_output_ok) {
            /* Non-VT fallback: just move to a new line */
            printf("\n");
          } else if (viewport_start > 0) {
            printf("\r");
            if (prev_lines > 0)
              printf("\x1b[%dA", prev_lines);
            printf("\x1b[J");
            if (prompt)
              fputs(prompt, stdout);
            repl_highlight_line_ex(buf, -1, "\x1b[90m..|\x1b[0m");
            printf("\n");
          } else {
            draw_line(prompt, buf, len, pos, prompt_cols);
            int last_r, last_c, t_cols, t_rows;
            get_term_size(&t_cols, &t_rows);
            calc_cursor(buf, len, t_cols, prompt_cols, &last_r, &last_c);
            if (last_r > prev_lines)
              printf("\x1b[%dB", last_r - prev_lines);
            printf("\n");
          }
          fflush(stdout);
          break;
        } else {
          int indent = input_pending ? 0 : repl_calc_indent(buf);
          if (len + 2 + indent >= cap) {
            cap += 256 + indent;
            buf = realloc(buf, cap);
          }
          memmove(buf + pos + 1 + indent, buf + pos, len - pos + 1);
          buf[pos] = '\n';
          for (int i = 0; i < indent; i++)
            buf[pos + 1 + i] = ' ';
          pos += 1 + indent;
          len += 1 + indent;
          buf[len] = '\0';
        }
      }
      if (!pasting && !repl_is_input_pending())
        draw_line(prompt, buf, len, pos, prompt_cols);
      continue;
    }

    if (k == K_TAB) {
      if (last_was_tab) {
        if (comp_matches && comp_count > 1) {
          comp_idx = (comp_idx + 1) % (int)comp_count;
          int word_start = pos;
          while (word_start > 0 && !is_break_char(buf[word_start - 1]))
            word_start--;
          int word_end = pos;
          while (word_end < len && !is_break_char(buf[word_end]))
            word_end++;
          int old_len = word_end - word_start;
          const char *new_match = comp_matches[comp_idx];
          int new_len = (int)strlen(new_match);
          if (len - old_len + new_len >= cap) {
            cap = len + new_len + 128;
            buf = realloc(buf, cap);
          }
          memmove(buf + word_start + new_len, buf + word_end,
                  len - word_end + 1);
          memcpy(buf + word_start, new_match, new_len);
          len = len - old_len + new_len;
          pos = word_start + new_len;
          if (comp_idx == 0 && last_was_tab > 1) {
            repl_display_match_list(comp_matches, (int)comp_count, 0);
            prev_lines = 0;
          }
          last_was_tab++;
        }
      } else {
        size_t count = 0;
        char **matches = nytrix_get_completions_for_line(buf, pos, &count);
        if (comp_matches)
          nytrix_free_completions(comp_matches, comp_count);
        comp_matches = matches;
        comp_count = count;
        if (count == 0) {
          int spaces = 2;
          if (len + spaces >= cap) {
            cap += 128;
            buf = realloc(buf, cap);
          }
          memmove(buf + pos + spaces, buf + pos, len - pos + 1);
          for (int i = 0; i < spaces; i++)
            buf[pos++] = ' ';
          len += spaces;
        } else if (count == 1) {
          int word_start = pos;
          while (word_start > 0 && !is_break_char(buf[word_start - 1]))
            word_start--;
          int old_word_len = pos - word_start;
          const char *match = comp_matches[0];
          int match_len = (int)strlen(match);
          if (len - old_word_len + match_len >= cap) {
            cap = len + match_len + 128;
            buf = realloc(buf, cap);
          }
          memmove(buf + word_start + match_len, buf + pos, len - pos + 1);
          memcpy(buf + word_start, match, match_len);
          len = len - old_word_len + match_len;
          pos = word_start + match_len;
        } else if (count > 1) {
          last_was_tab = 1;
          comp_idx = -1;
          size_t cp_len = strlen(comp_matches[0]);
          for (size_t i = 1; i < count; i++) {
            size_t j = 0;
            while (j < cp_len && comp_matches[i][j] &&
                   comp_matches[0][j] == comp_matches[i][j])
              j++;
            cp_len = j;
          }
          if (cp_len > 0) {
            int word_start = pos;
            while (word_start > 0 && !is_break_char(buf[word_start - 1]))
              word_start--;
            int old_word_len = pos - word_start;
            if (cp_len > (size_t)old_word_len) {
              if (len + cp_len >= (size_t)cap) {
                cap += (int)cp_len + 128;
                buf = realloc(buf, cap);
              }
              memmove(buf + word_start + cp_len, buf + pos, len - pos + 1);
              memcpy(buf + word_start, comp_matches[0], cp_len);
              len = len - old_word_len + (int)cp_len;
              pos = word_start + (int)cp_len;
            }
          }
        }
      }
      draw_line(prompt, buf, len, pos, prompt_cols);
      if (last_was_tab)
        continue;
      else {
        continue;
      }
    }

    if (k == K_SHIFT_TAB) {
      if (last_was_tab && comp_matches && comp_count > 1) {
        if (comp_idx == -1)
          comp_idx = (int)comp_count - 1;
        else
          comp_idx = (comp_idx - 1 + (int)comp_count) % (int)comp_count;

        int word_start = pos;
        while (word_start > 0 && !is_break_char(buf[word_start - 1]))
          word_start--;
        int word_end = pos;
        while (word_end < len && !is_break_char(buf[word_end]))
          word_end++;
        int old_len = word_end - word_start;
        const char *new_match = comp_matches[comp_idx];
        int new_len = (int)strlen(new_match);
        if (len - old_len + new_len >= cap) {
          cap = len + new_len + 128;
          buf = realloc(buf, cap);
        }
        memmove(buf + word_start + new_len, buf + word_end, len - word_end + 1);
        memcpy(buf + word_start, new_match, new_len);
        len = len - old_len + new_len;
        pos = word_start + new_len;
        last_was_tab = 2; // Keep cycling
        draw_line(prompt, buf, len, pos, prompt_cols);
        continue;
      }
    }
    if (k != K_UP && k != K_DOWN && k != K_CTRL_P && k != K_CTRL_N) {
      target_col = -1;
    }
    last_was_tab = 0;

    if (k == K_BACKSPACE) {
      if (pos > 0) {
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
        if (all_spaces && (pos - line_start) >= 2 &&
            (pos - line_start) % 2 == 0)
          skip = 2;
        else {
          while (pos - skip > 0 && (buf[pos - skip] & 0xc0) == 0x80)
            skip++;
        }
        memmove(buf + pos - skip, buf + pos, len - pos + 1);
        pos -= skip;
        len -= skip;
      }
    } else if (k == K_DEL) {
      if (pos < len) {
        int skip = 1;
        while (pos + skip < len && (buf[pos + skip] & 0xc0) == 0x80)
          skip++;
        memmove(buf + pos, buf + pos + skip, len - pos - skip + 1);
        len -= skip;
      }
    } else if (k == K_LEFT || k == K_CTRL_B) {
      if (pos > 0) {
        pos--;
        while (pos > 0 && (buf[pos] & 0xc0) == 0x80)
          pos--;
      }
    } else if (k == K_RIGHT || k == K_CTRL_F) {
      if (pos < len) {
        pos++;
        while (pos < len && (buf[pos] & 0xc0) == 0x80)
          pos++;
      }
    } else if (k == K_CTRL_LEFT || k == K_ALT_LEFT) {
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
      }
    } else if (k == K_CTRL_RIGHT || k == K_ALT_RIGHT) {
      if (pos < len) {
        while (pos < len && !is_break_char(buf[pos]))
          pos++;
        while (pos < len && isspace((unsigned char)buf[pos]))
          pos++;
      }
    } else if (k == K_CTRL_BACKSPACE) {
      if (pos > 0) {
        int start = pos;
        pos--;
        while (pos > 0 && isspace((unsigned char)buf[pos - 1]))
          pos--;
        while (pos > 0 && !is_break_char(buf[pos - 1]))
          pos--;
        int klen = start - pos;
        if (kill_ring)
          free(kill_ring);
        kill_ring = malloc(klen + 1);
        memcpy(kill_ring, buf + pos, klen);
        kill_ring[klen] = '\0';
        memmove(buf + pos, buf + start, len - start + 1);
        len -= klen;
      }
    } else if (k == K_CTRL_DEL) {
      if (pos < len) {
        int end = pos;
        while (end < len && !is_break_char(buf[end]))
          end++;
        while (end < len && isspace((unsigned char)buf[end]))
          end++;
        int klen = end - pos;
        if (kill_ring)
          free(kill_ring);
        kill_ring = malloc(klen + 1);
        memcpy(kill_ring, buf + pos, klen);
        kill_ring[klen] = '\0';
        memmove(buf + pos, buf + end, len - end + 1);
        len -= klen;
      }
    } else if (k == K_UP || k == K_CTRL_P) {
      int cur_r, cur_c;
      calc_cursor(buf, pos, term_cols, prompt_cols, &cur_r, &cur_c);
      if (target_col == -1)
        target_col = cur_c;
      if (cur_r > 0) {
        pos = visual_to_pos(buf, len, cur_r - 1, target_col, term_cols,
                            prompt_cols);
      } else {
        if (ny_hist_len > 0 && hist_idx > 0) {
          if (hist_idx == ny_hist_len) {
            free(saved_buf);
            saved_buf = ny_strdup(buf);
          }
          hist_idx--;
          free(buf);
          buf = ny_strdup(ny_hist[hist_idx]);
          len = (int)strlen(buf);
          cap = len + 256;
          buf = realloc(buf, cap);
          pos = len;
          prev_lines = 0;
          prev_total_rows = 0;
        }
      }
    } else if (k == K_DOWN || k == K_CTRL_N) {
      int cur_r, cur_c, tot_r, tot_c;
      calc_cursor(buf, pos, term_cols, prompt_cols, &cur_r, &cur_c);
      calc_cursor(buf, len, term_cols, prompt_cols, &tot_r, &tot_c);
      if (target_col == -1)
        target_col = cur_c;
      if (cur_r < tot_r) {
        pos = visual_to_pos(buf, len, cur_r + 1, target_col, term_cols,
                            prompt_cols);
      } else {
        if (hist_idx < ny_hist_len) {
          hist_idx++;
          free(buf);
          if (hist_idx == ny_hist_len) {
            buf = ny_strdup(saved_buf ? saved_buf : "");
          } else {
            buf = ny_strdup(ny_hist[hist_idx]);
          }
          len = (int)strlen(buf);
          cap = len + 256;
          buf = realloc(buf, cap);
          pos = len;
          if (saved_buf && hist_idx == ny_hist_len) {
            free(saved_buf);
            saved_buf = NULL;
          }
          prev_lines = 0;
          prev_total_rows = 0;
        } else if (len > 0) {
          len = 0;
          pos = 0;
          buf[0] = '\0';
          prev_lines = 0;
          prev_total_rows = 0;
        }
      }
    }

    else if (k == K_HOME || k == K_CTRL_A) {
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
    } else if (k == K_END || k == K_CTRL_E) {
      while (pos < len && buf[pos] != '\n')
        pos++;
    } else if (k == K_CTRL_K) {
      if (kill_ring)
        free(kill_ring);
      int line_end = pos;
      while (line_end < len && buf[line_end] != '\n')
        line_end++;
      if (line_end == pos && line_end < len && buf[line_end] == '\n')
        line_end++;
      int klen = line_end - pos;
      kill_ring = malloc(klen + 1);
      memcpy(kill_ring, buf + pos, klen);
      kill_ring[klen] = '\0';
      memmove(buf + pos, buf + line_end, len - line_end + 1);
      len -= klen;
    } else if (k == K_CTRL_Y) {
      if (kill_ring) {
        int klen = (int)strlen(kill_ring);
        if (len + klen >= cap) {
          cap = len + klen + 128;
          buf = realloc(buf, cap);
        }
        memmove(buf + pos + klen, buf + pos, len - pos + 1);
        memcpy(buf + pos, kill_ring, klen);
        len += klen;
        pos += klen;
      }
    } else if (k == K_CTRL_U) {
      int line_start = pos;
      while (line_start > 0 && buf[line_start - 1] != '\n')
        line_start--;
      int klen = pos - line_start;
      if (kill_ring)
        free(kill_ring);
      kill_ring = malloc(klen + 1);
      memcpy(kill_ring, buf + line_start, klen);
      kill_ring[klen] = '\0';
      memmove(buf + line_start, buf + pos, len - pos + 1);
      len -= klen;
      pos = line_start;
    } else if (k == K_CTRL_W) {
      int start = pos;
      while (start > 0 && isspace((unsigned char)buf[start - 1]))
        start--;
      while (start > 0 && !is_break_char(buf[start - 1]))
        start--;
      int klen = pos - start;
      if (klen > 0) {
        if (kill_ring)
          free(kill_ring);
        kill_ring = malloc(klen + 1);
        memcpy(kill_ring, buf + start, klen);
        kill_ring[klen] = '\0';
        memmove(buf + start, buf + pos, len - pos + 1);
        len -= klen;
        pos = start;
      }
    } else if (k < 1000 && k > 0) {
      if (len + 1 >= cap) {
        cap += 256;
        buf = realloc(buf, cap);
      }
      memmove(buf + pos + 1, buf + pos, len - pos + 1);
      buf[pos] = (char)k;
      len++;
      pos++;
      buf[len] = '\0';
    }

    if (!pasting && !repl_is_input_pending())
      draw_line(prompt, buf, len, pos, prompt_cols);
  }

  if (vt_out_ok) {
    printf("\x1b[?2004l"); // Disable bracketed paste
    fflush(stdout);
  }
#ifdef _WIN32
  SetConsoleMode(hIn, oldMode);
  SetConsoleMode(hOut, oldOutMode);
  SetConsoleCP(oldInCP);
  SetConsoleOutputCP(oldOutCP);
#else
  if (is_tty)
    tcsetattr(STDIN_FILENO, TCSANOW, &old_t);
#endif

  if (saved_buf)
    free(saved_buf);
  if (comp_matches)
    nytrix_free_completions(comp_matches, comp_count);
  prev_lines = 0;
  prev_total_rows = 0;
  return buf;
}