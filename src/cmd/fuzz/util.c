#include "core.h"

#include <stdarg.h>

bool sb_reserve(str_buf_t *b, size_t need) {
  if (need <= b->cap) return true;
  size_t cap = b->cap ? b->cap : 4096;
  while (cap < need) cap *= 2;
  char *next = (char *)realloc(b->data, cap);
  if (!next) return false;
  b->data = next;
  b->cap = cap;
  return true;
}

bool sb_append_n(str_buf_t *b, const char *data, size_t len) {
  if (!sb_reserve(b, b->len + len + 1)) return false;
  memcpy(b->data + b->len, data, len);
  b->len += len;
  b->data[b->len] = '\0';
  return true;
}

bool sb_append(str_buf_t *b, const char *text) {
  return sb_append_n(b, text, strlen(text));
}

bool sb_append_c(str_buf_t *b, char c) {
  return sb_append_n(b, &c, 1);
}

char *sb_take(str_buf_t *b) {
  if (!b->data) {
    b->data = (char *)calloc(1, 1);
    b->cap = 1;
  }
  char *out = b->data;
  b->data = NULL;
  b->len = 0;
  b->cap = 0;
  return out;
}

bool sb_appendf(str_buf_t *b, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  va_list copy;
  va_copy(copy, ap);
  int n = vsnprintf(NULL, 0, fmt, copy);
  va_end(copy);
  if (n < 0) {
    va_end(ap);
    return false;
  }
  size_t old_len = b->len;
  if (!sb_reserve(b, old_len + (size_t)n + 1u)) {
    va_end(ap);
    return false;
  }
  vsnprintf(b->data + old_len, (size_t)n + 1u, fmt, ap);
  va_end(ap);
  b->len = old_len + (size_t)n;
  return true;
}

bool sb_append_json_str(str_buf_t *b, const char *s) {
  if (!sb_append_c(b, '"')) return false;
  for (const unsigned char *p = (const unsigned char *)s; p && *p; ++p) {
    switch (*p) {
    case '\\':
      if (!sb_append(b, "\\\\")) return false;
      break;
    case '"':
      if (!sb_append(b, "\\\"")) return false;
      break;
    case '\n':
      if (!sb_append(b, "\\n")) return false;
      break;
    case '\r':
      if (!sb_append(b, "\\r")) return false;
      break;
    case '\t':
      if (!sb_append(b, "\\t")) return false;
      break;
    default:
      if (*p < 32) {
        if (!sb_appendf(b, "\\u%04x", (unsigned)*p)) return false;
      } else if (!sb_append_c(b, (char)*p)) {
        return false;
      }
    }
  }
  return sb_append_c(b, '"');
}

void json_str(FILE *out, const char *s) {
  fputc('"', out);
  for (const unsigned char *p = (const unsigned char *)s; p && *p; ++p) {
    switch (*p) {
    case '\\': fputs("\\\\", out); break;
    case '"': fputs("\\\"", out); break;
    case '\n': fputs("\\n", out); break;
    case '\r': fputs("\\r", out); break;
    case '\t': fputs("\\t", out); break;
    default:
      if (*p < 32) fprintf(out, "\\u%04x", (unsigned)*p);
      else fputc(*p, out);
    }
  }
  fputc('"', out);
}

bool read_file(const char *path, file_buf_t *out) {
  FILE *f = fopen(path, "rb");
  if (!f) return false;
  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
  long n = ftell(f);
  if (n < 0) { fclose(f); return false; }
  if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return false; }
  char *data = (char *)malloc((size_t)n + 1u);
  if (!data) { fclose(f); return false; }
  size_t got = fread(data, 1, (size_t)n, f);
  fclose(f);
  if (got != (size_t)n) { free(data); return false; }
  data[got] = '\0';
  out->data = data;
  out->len = got;
  return true;
}

char *nynth_shape_source_block(const char *shape_path, const char *name) {
  file_buf_t f = {0};
  if (!shape_path || !name || !*name || !read_file(shape_path, &f)) return NULL;
  char needle[128];
  snprintf(needle, sizeof(needle), "source %s <<'", name);
  char *p = strstr(f.data, needle);
  if (!p) {
    free(f.data);
    return NULL;
  }
  char *marker = p + strlen(needle);
  char *marker_end = strchr(marker, '\'');
  char *body = marker_end ? strchr(marker_end, '\n') : NULL;
  if (!marker_end || !body || marker_end == marker) {
    free(f.data);
    return NULL;
  }
  ++body;
  size_t marker_len = (size_t)(marker_end - marker);
  for (char *line = body; line && *line;) {
    char *next = strchr(line, '\n');
    size_t line_len = next ? (size_t)(next - line) : strlen(line);
    if (line_len && line[line_len - 1] == '\r') --line_len;
    if (line_len == marker_len && memcmp(line, marker, marker_len) == 0) {
      size_t n = (size_t)(line - body);
      char *out = (char *)malloc(n + 1u);
      if (out) {
        memcpy(out, body, n);
        out[n] = '\0';
      }
      free(f.data);
      return out;
    }
    line = next ? next + 1 : NULL;
  }
  free(f.data);
  return NULL;
}

uint64_t fnv1a64(const char *data, size_t len) {
  uint64_t h = UINT64_C(14695981039346656037);
  for (size_t i = 0; i < len; ++i) {
    h ^= (unsigned char)data[i];
    h *= UINT64_C(1099511628211);
  }
  return h;
}

double now_ms(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0.0;
  return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

int count_sub(const char *data, size_t len, const char *needle) {
  int count = 0;
  size_t n = strlen(needle);
  if (!n || n > len) return 0;
  for (size_t i = 0; i + n <= len; ++i) {
    if (memcmp(data + i, needle, n) == 0) {
      ++count;
      i += n - 1;
    }
  }
  return count;
}

bool ident_char(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
         (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '$';
}

int count_word_call(const char *data, size_t len, const char *word) {
  int count = 0;
  size_t n = strlen(word);
  for (size_t i = 0; i + n <= len; ++i) {
    if (memcmp(data + i, word, n) != 0) continue;
    char prev = i ? data[i - 1] : '\0';
    char next = i + n < len ? data[i + n] : '\0';
    if (!ident_char(prev) && !ident_char(next)) ++count;
  }
  return count;
}

int count_regexish_assign_list(const char *data, size_t len, const char *kw) {
  int count = 0;
  size_t kw_len = strlen(kw);
  size_t start = 0;
  for (size_t i = 0; i <= len; ++i) {
    if (i != len && data[i] != '\n') continue;
    const char *line = data + start;
    size_t line_len = i - start;
    if (memmem(line, line_len, kw, kw_len) &&
        memmem(line, line_len, "= [", 3)) {
      const char *eq = memmem(line, line_len, "= [", 3);
      if (eq && (eq + 3 < line + line_len) &&
          ((*((eq + 3)) >= '0' && *((eq + 3)) <= '9') || *((eq + 3)) == '-'))
        ++count;
    }
    start = i + 1;
  }
  return count;
}

int count_lines(const char *data, size_t len) {
  int lines = len ? 1 : 0;
  for (size_t i = 0; i < len; ++i) if (data[i] == '\n') ++lines;
  return lines;
}

bool has_suffix(const char *path, const char *suffix) {
  return ny_has_suffix(path, suffix);
}

bool mkdir_p(const char *path) {
  ny_ensure_dir_recursive(path);
  return true;
}

const char *arg_value(int argc, char **argv, const char *name, const char *fallback) {
  size_t name_len = strlen(name);
  for (int i = 2; i < argc; ++i) {
    if (strncmp(argv[i], name, name_len) == 0 && argv[i][name_len] == '=')
      return argv[i] + name_len + 1;
    if (i + 1 < argc && strcmp(argv[i], name) == 0) return argv[i + 1];
  }
  return fallback;
}

bool arg_flag(int argc, char **argv, const char *name) {
  for (int i = 2; i < argc; ++i) {
    if (strcmp(argv[i], name) == 0) return true;
  }
  return false;
}

const char *base_name(const char *path) {
  return ny_base_name(path);
}

void stem_name(const char *path, char *out, size_t out_sz) {
  const char *base = base_name(path);
  snprintf(out, out_sz, "%s", base);
  char *dot = strrchr(out, '.');
  if (dot) *dot = '\0';
}
