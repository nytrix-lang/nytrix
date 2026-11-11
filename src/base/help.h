/* Shared terminal help formatting, ANSI-aware widths, and option tables. */
#pragma once

#include "base/common.h"
#include <ctype.h>

static inline bool ny_help_color_enabled(FILE *out) {
#ifdef _WIN32
  return ny_color_enabled_for_fd(_fileno(out));
#else
  return ny_color_enabled_for_fd(fileno(out));
#endif
}

static inline int ny_help_span_width(const char *text, size_t length) {
  int width = 0;
  for (size_t i = 0; i < length;) {
    if ((unsigned char)text[i] == 0x1b && i + 1 < length &&
        text[i + 1] == '[') {
      i += 2;
      while (i < length) {
        unsigned char ch = (unsigned char)text[i++];
        if (ch >= '@' && ch <= '~')
          break;
      }
      continue;
    }
    if (((unsigned char)text[i] & 0xc0) != 0x80)
      width++;
    i++;
  }
  return width;
}

static inline void ny_help_wrap(FILE *out, const char *text, int indent,
                                int width) {
  FILE *stream = out ? out : stdout;
  const char *p = text ? text : "";
  int column = 0;
  bool emitted = false;
  if (width < 1)
    width = 1;
  while (*p) {
    while (*p && isspace((unsigned char)*p))
      ++p;
    if (!*p)
      break;
    const char *word = p;
    while (*p && !isspace((unsigned char)*p))
      ++p;
    int length = (int)(p - word);
    int word_width = ny_help_span_width(word, (size_t)length);
    if (word_width > width) {
      if (emitted) {
        fputc('\n', stream);
        fprintf(stream, "%*s", indent, "");
      }
      /* Keep paths, option spellings, and UTF-8 tokens intact. */
      fwrite(word, 1, (size_t)length, stream);
      column = word_width;
      emitted = true;
      continue;
    }
    if (!emitted) {
      fwrite(word, 1, (size_t)length, stream);
      column = word_width;
      emitted = true;
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

static inline void ny_help_heading(FILE *out, const char *name) {
  bool color = ny_help_color_enabled(out);
  fprintf(out, "%sNytrix%s %s%s%s\n", color ? NY_CLR_MAGENTA : "",
          color ? NY_CLR_RESET : "", color ? NY_CLR_CYAN : "", name,
          color ? NY_CLR_RESET : "");
}

static inline void ny_help_section(FILE *out, const char *name) {
  bool color = ny_help_color_enabled(out);
  const char *reset = color ? NY_CLR_RESET : "";
  const char *mark = ny_ui_mark("section");
  const char *rule = ny_ui_glyph("─", "-");
  const char *icon_name = ny_ui_section_icon(name);
  const char *icon = icon_name ? ny_ui_icon(icon_name) : "";
  int prefix = ny_help_span_width(mark, strlen(mark)) + 1 +
               ny_help_span_width(name, strlen(name)) + 1;
  fprintf(out, "%s%s%s ", color ? NY_CLR_MAGENTA : "", mark, reset);
  if (*icon) {
    fprintf(out, "%s%s%s ", color ? NY_CLR_CYAN : "", icon, reset);
    prefix += ny_help_span_width(icon, strlen(icon)) + 1;
  }
  fprintf(out, "%s%s%s ", color ? NY_CLR_BOLD : "", name, reset);
  for (int i = prefix; i < ny_ui_width(); ++i)
    fputs(rule, out);
  fputc('\n', out);
}

static inline void ny_help_item(FILE *out, const char *color_code,
                                const char *flag, const char *desc) {
  const int gap = 2;
  int terminal = ny_ui_width();
  int flag_width = terminal - 2 - gap - 24;
  if (flag_width > 31)
    flag_width = 31;
  if (flag_width < 12)
    flag_width = 12;
  int desc_indent = 2 + flag_width + gap;
  int available = terminal - desc_indent;
  bool color = ny_help_color_enabled(out);
  const char *reset = color ? NY_CLR_RESET : "";
  int length = ny_help_span_width(flag, strlen(flag));
  if (length <= flag_width) {
    fprintf(out, "  %s%s%s%*s", color ? color_code : "", flag, reset,
            flag_width - length + gap, "");
  } else {
    fprintf(out, "  %s", color ? color_code : "");
    ny_help_wrap(out, flag, 2, terminal - 2);
    fprintf(out, "%s%*s", reset, desc_indent, "");
  }
  ny_help_wrap(out, desc, desc_indent, available);
}

typedef enum {
  NY_HELP_SECTION,
  NY_HELP_ITEM,
  NY_HELP_TEXT,
} ny_help_entry_kind_t;

typedef struct {
  ny_help_entry_kind_t kind;
  const char *label;
  const char *description;
} ny_help_entry_t;

static inline void ny_help_print(FILE *out, const ny_help_entry_t *entries,
                                 size_t count) {
  const char *color = NY_CLR_CYAN;
  for (size_t i = 0; i < count; ++i) {
    const ny_help_entry_t *entry = &entries[i];
    switch (entry->kind) {
    case NY_HELP_SECTION:
      fputc('\n', out);
      ny_help_section(out, entry->label);
      color = strcmp(entry->label, "COMPATIBILITY") == 0 ||
                      strcmp(entry->label, "ENVIRONMENT") == 0
                  ? NY_CLR_YELLOW
                  : NY_CLR_CYAN;
      break;
    case NY_HELP_ITEM:
      ny_help_item(out, color, entry->label, entry->description);
      break;
    case NY_HELP_TEXT:
      fputs("  ", out);
      ny_help_wrap(out, entry->label, 2, ny_ui_width() - 2);
      break;
    }
  }
}
