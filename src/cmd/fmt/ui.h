#pragma once

#include "color.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  char **items;
  size_t len;
  size_t cap;
} StrVec;

static inline void sv_push(StrVec *v, const char *s) {
  if (!v)
    return;
  if (v->len == v->cap) {
    size_t nc = v->cap ? v->cap * 2 : 32;
    char **p = (char **)realloc(v->items, nc * sizeof(char *));
    if (!p)
      return;
    v->items = p;
    v->cap = nc;
  }
  v->items[v->len] = strdup(s ? s : "");
  if (v->items[v->len])
    v->len++;
}

static inline void sv_free(StrVec *v) {
  if (!v)
    return;
  for (size_t i = 0; i < v->len; i++)
    free(v->items[i]);
  free(v->items);
  v->items = NULL;
  v->len = 0;
  v->cap = 0;
}

static inline const char *nyt_temp_dir(void) {
  static const char *envs[] = {"TMPDIR", "TEMP", "TMP"};
  for (size_t i = 0; i < sizeof(envs) / sizeof(envs[0]); i++) {
    const char *v = getenv(envs[i]);
    if (v && *v)
      return v;
  }
#ifdef _WIN32
  return "C:\\Windows\\Temp";
#else
  return "/tmp";
#endif
}

static inline void nyt_rule(FILE *out) {
  FILE *stream = out ? out : stdout;
  int width = ny_ui_width();
  const char *rule = ny_ui_glyph("─", "-");
  fprintf(stream, "%s", nyt_clr(NYT_GRAY));
  for (int i = 0; i < width; ++i)
    fputs(rule, stream);
  fprintf(stream, "%s\n", nyt_clr(NYT_RESET));
}

static inline void nyt_heading(const char *name) {
  const char *title = name;
  if (title && strncmp(title, "Nytrix ", 7) == 0)
    title += 7;
  printf("%s%s%s %sNytrix%s",
         nyt_clr(NYT_MAGENTA), ny_ui_mark("brand"), nyt_clr(NYT_RESET),
         nyt_clr(NYT_BOLD), nyt_clr(NYT_RESET));
  if (title && *title)
    printf("  %s·%s  %s%s%s", nyt_clr(NYT_GRAY), nyt_clr(NYT_RESET),
           nyt_clr(NYT_CYAN), title, nyt_clr(NYT_RESET));
  fputc('\n', stdout);
  nyt_rule(stdout);
}

static inline void nyt_subheading(const char *name) {
  const char *title = name ? name : "";
  const char *rule = ny_ui_glyph("─", "-");
  const char *icon_name = ny_ui_section_icon(title);
  const char *icon = icon_name ? ny_ui_icon(icon_name) : "";
  int prefix = 1 + 1 + (int)strlen(icon) + (int)strlen(title) + 1;
  printf("%s%s %s%s %s%s%s ", nyt_clr(NYT_MAGENTA), ny_ui_mark("section"),
         icon_name ? nyt_clr(NYT_CYAN) : "", icon,
         nyt_clr(NYT_RESET), nyt_clr(NYT_BOLD), title);
  for (int i = prefix; i < ny_ui_width(); i++)
    fputs(rule, stdout);
  fputc('\n', stdout);
}

static inline int nyt_span_width(const char *text, size_t length) {
  int width = 0;
  for (size_t i = 0; i < length;) {
    if ((unsigned char)text[i] == 0x1b && i + 1 < length && text[i + 1] == '[') {
      i += 2;
      while (i < length) {
        unsigned char ch = (unsigned char)text[i++];
        if (ch >= '@' && ch <= '~') break;
      }
      continue;
    }
    if (((unsigned char)text[i] & 0xc0) != 0x80)
      width++;
    i++;
  }
  return width;
}

static inline void nyt_wrap_value_stream(FILE *out, const char *text, int indent,
                                         int width) {
  FILE *stream = out ? out : stdout;
  const char *p = text ? text : "";
  int column = 0;
  int emitted = 0;
  if (width < 1) width = 1;
  while (*p) {
    while (*p && isspace((unsigned char)*p)) ++p;
    if (!*p) break;
    const char *word = p;
    while (*p && !isspace((unsigned char)*p)) ++p;
    int length = (int)(p - word);
    int word_width = nyt_span_width(word, (size_t)length);
    if (word_width > width) {
      if (emitted) {
        fputc('\n', stream);
        fprintf(stream, "%*s", indent, "");
      }
      /* Never split an opaque token by bytes: paths and UTF-8 text must stay
       * valid even when one word is wider than the terminal. */
      fwrite(word, 1, (size_t)length, stream);
      column = word_width;
      emitted = 1;
      continue;
    }
    if (!emitted) {
      fwrite(word, 1, (size_t)length, stream);
      column = word_width;
      emitted = 1;
    } else if (column + 1 + word_width <= width) {
      fputc(' ', stream);
      fwrite(word, 1, (size_t)length, stream);
      column += 1 + word_width;
    } else {
      fputc('\n', stream);
      fprintf(stream, "%*s", indent, "");
      fwrite(word, 1, (size_t)length, stream);
      column = word_width;
    }
  }
  if (!emitted)
    fputc(' ', stream);
  fputc('\n', stream);
}

static inline void nyt_wrap_value(const char *text, int indent, int width) {
  nyt_wrap_value_stream(stdout, text, indent, width);
}

static inline void nyt_kv(const char *key, const char *fmt, ...) {
  char value[4096];
  va_list ap;
  va_start(ap, fmt);
  if (fmt)
    vsnprintf(value, sizeof(value), fmt, ap);
  else
    value[0] = '\0';
  va_end(ap);
  int key_width = 18;
  int width = ny_ui_width();
  int indent = 2 + key_width + 2 + 1 + 2;
  if (width - indent < 18) {
    key_width = width - 2 - 2 - 1 - 2 - 18;
    if (key_width < 8) key_width = 8;
    indent = 2 + key_width + 2 + 1 + 2;
  }
  int available = width - indent;
  printf("  %s%-*s%s  %s·%s  ", nyt_clr(NYT_BOLD), key_width,
         key ? key : "", nyt_clr(NYT_RESET), nyt_clr(NYT_GRAY),
         nyt_clr(NYT_RESET));
  nyt_wrap_value(value, indent, available);
}

static inline void nyt_emit_message(FILE *out, const char *mark,
                                    const char *label, const char *color,
                                    const char *fmt, va_list ap) {
  FILE *stream = out ? out : stdout;
  char message[4096];
  vsnprintf(message, sizeof(message), fmt ? fmt : "", ap);
  const char *shown_label = label ? label : "";
  int prefix = nyt_span_width(mark, strlen(mark)) + 1 +
               nyt_span_width(shown_label, strlen(shown_label)) + 4;
  int available = ny_ui_width() - prefix;
  if (available < 8) {
    fprintf(stream, "%s%s%s %s%s%s %s·%s\n", nyt_clr(color), mark,
            nyt_clr(NYT_RESET), nyt_clr(NYT_BOLD), shown_label,
            nyt_clr(NYT_RESET), nyt_clr(NYT_GRAY), nyt_clr(NYT_RESET));
    nyt_wrap_value_stream(stream, message, 2, ny_ui_width() - 2);
  } else {
    fprintf(stream, "%s%s%s %s%s%s %s·%s ", nyt_clr(color), mark,
            nyt_clr(NYT_RESET), nyt_clr(NYT_BOLD), shown_label,
            nyt_clr(NYT_RESET), nyt_clr(NYT_GRAY), nyt_clr(NYT_RESET));
    nyt_wrap_value_stream(stream, message, prefix, available);
  }
  fflush(stream);
}

static inline void nyt_vmsg(FILE *out, const char *label, const char *color, const char *fmt, va_list ap) {
  const char *mark = ny_ui_mark("step");
  if (label && (strcmp(label, "OK") == 0 || strcmp(label, "SUCCESS") == 0))
    mark = ny_ui_mark("success");
  else if (label && (strcmp(label, "FAIL") == 0 || strcmp(label, "ERROR") == 0))
    mark = ny_ui_mark("error");
  else if (label && strcmp(label, "WARN") == 0)
    mark = ny_ui_mark("warn");
  nyt_emit_message(out, mark, label, color, fmt, ap);
}

static inline void nyt_msg(const char *label, const char *color, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  nyt_vmsg(stdout, label, color, fmt, ap);
  va_end(ap);
}

static inline void nyt_err(const char *tool, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  nyt_emit_message(stderr, ny_ui_mark("error"), tool ? tool : "ny-tool",
                   NYT_RED, fmt, ap);
  va_end(ap);
}

static inline void nyt_warn(const char *tool, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  nyt_emit_message(stderr, ny_ui_mark("warn"), tool ? tool : "ny-tool",
                   NYT_YELLOW, fmt, ap);
  va_end(ap);
}

static inline void nyt_section(const char *name) {
  nyt_heading(name);
}
