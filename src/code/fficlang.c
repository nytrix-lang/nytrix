#include "fficlang.h"
#include "base/util.h"
#include "priv.h"
#include "code/c/c.h"
#include <clang-c/Index.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <dlfcn.h>
#include <unistd.h>
#endif

#ifndef alloca
#ifdef _WIN32
#include <malloc.h>
#else
#include <alloca.h>
#endif
#endif

#ifdef _WIN32
#define NY_FFI_NULL_DEVICE "NUL"
#else
#define NY_FFI_NULL_DEVICE "/dev/null"
#endif

static void ffi_buf_append(char **buf, size_t *len, size_t *cap,
                           const char *s) {
  if (!buf || !len || !cap || !s)
    return;
  size_t add = strlen(s);
  size_t need = *len + add + 1;
  if (need > *cap) {
    size_t new_cap = *cap ? *cap : 256;
    while (new_cap < need)
      new_cap *= 2;
    char *new_buf = (char *)realloc(*buf, new_cap);
    if (!new_buf)
      return;
    *buf = new_buf;
    *cap = new_cap;
  }
  memcpy(*buf + *len, s, add);
  *len += add;
  (*buf)[*len] = '\0';
}

static void ny_push_unique_owned_arg(char ***args, size_t *len, size_t *cap,
                                     const char *tok) {
  if (!args || !len || !cap || !tok || !*tok)
    return;
  for (size_t i = 0; i < *len; i++) {
    if ((*args)[i] && strcmp((*args)[i], tok) == 0)
      return;
  }
  if (*len >= *cap) {
    size_t new_cap = *cap ? (*cap * 2) : 16;
    char **new_args = (char **)realloc(*args, new_cap * sizeof(char *));
    if (!new_args)
      return;
    *args = new_args;
    *cap = new_cap;
  }
  (*args)[*len] = ny_strdup(tok);
  if (!(*args)[*len])
    return;
  (*len)++;
}

static void ny_pkgconfig_append_cflags(const char *pkg, char ***args,
                                       size_t *len, size_t *cap) {
  if (!pkg || !*pkg || !args || !len || !cap)
    return;

  typedef struct {
    const char *pkg;
    char *out;
  } ny_pkgcfg_cflags_entry_t;
  static ny_pkgcfg_cflags_entry_t *g_cflags_cache = NULL;
  static size_t g_cflags_cache_len = 0, g_cflags_cache_cap = 0;
  for (size_t i = 0; i < g_cflags_cache_len; i++) {
    if (g_cflags_cache[i].pkg && strcmp(g_cflags_cache[i].pkg, pkg) == 0) {
      const char *buf = g_cflags_cache[i].out;
      if (!buf || !*buf)
        return;

      const char *p = buf;
      while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
          p++;
        if (!*p)
          break;
        const char *start = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n')
          p++;
        size_t tok_len = (size_t)(p - start);
        if (tok_len == 0)
          continue;
        char *tmp = (char *)alloca(tok_len + 1);
        memcpy(tmp, start, tok_len);
        tmp[tok_len] = '\0';
        ny_push_unique_owned_arg(args, len, cap, tmp);
      }
      return;
    }
  }
  char cmd[256];
  snprintf(cmd, sizeof(cmd), "pkg-config --cflags %s 2>%s", pkg,
           NY_FFI_NULL_DEVICE);
  FILE *f = popen(cmd, "r");
  if (!f)
    return;
  char *buf = NULL;
  size_t buf_len = 0, buf_cap = 0;
  char chunk[256];
  while (fgets(chunk, sizeof(chunk), f))
    ffi_buf_append(&buf, &buf_len, &buf_cap, chunk);
  pclose(f);
  if (!buf || !*buf) {
    free(buf);

    if (g_cflags_cache_len == g_cflags_cache_cap) {
      size_t nc = g_cflags_cache_cap ? g_cflags_cache_cap * 2 : 32;
      ny_pkgcfg_cflags_entry_t *nn =
          (ny_pkgcfg_cflags_entry_t *)realloc(g_cflags_cache, nc * sizeof(*nn));
      if (nn) {
        g_cflags_cache = nn;
        g_cflags_cache_cap = nc;
      }
    }
    if (g_cflags_cache && g_cflags_cache_len < g_cflags_cache_cap) {
      g_cflags_cache[g_cflags_cache_len++] =
          (ny_pkgcfg_cflags_entry_t){.pkg = ny_strdup(pkg), .out = NULL};
    }
    return;
  }

  if (g_cflags_cache_len == g_cflags_cache_cap) {
    size_t nc = g_cflags_cache_cap ? g_cflags_cache_cap * 2 : 32;
    ny_pkgcfg_cflags_entry_t *nn =
        (ny_pkgcfg_cflags_entry_t *)realloc(g_cflags_cache, nc * sizeof(*nn));
    if (nn) {
      g_cflags_cache = nn;
      g_cflags_cache_cap = nc;
    }
  }
  if (g_cflags_cache && g_cflags_cache_len < g_cflags_cache_cap) {
    g_cflags_cache[g_cflags_cache_len++] =
        (ny_pkgcfg_cflags_entry_t){.pkg = ny_strdup(pkg), .out = buf};
  }
  char *p = buf;
  while (*p) {
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
      p++;
    if (!*p)
      break;
    char *start = p;
    while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n')
      p++;
    char save = *p;
    *p = '\0';
    ny_push_unique_owned_arg(args, len, cap, start);
    if (!save)
      break;
    *p++ = save;
  }

  if (!g_cflags_cache)
    free(buf);
}

typedef struct {
  const char *header_prefix;
  const char *pkg_config;
  const char *fallback_lib;
  const char *prefix;
} ny_autolink_entry_t;

static const ny_autolink_entry_t ny_autolink_table[] = {

    {"alsa/", "alsa", "libasound.so.2", "snd_"},
    {"sound/", "alsa", "libasound.so.2", "snd_"},
    {"jack/", "jack", "libjack.so.0", "jack_"},
    {"pulse/simple", "libpulse-simple", "libpulse-simple.so.0", "pa_"},
    {"pulse/", "libpulse", "libpulse.so.0", "pa_"},
    {"sndfile", "sndfile", "libsndfile.so.1", "sf_"},

    {"vulkan/", "vulkan", "libvulkan.so.1", "vk"},
    {"vk_", "vulkan", "libvulkan.so.1", "vk"},
    {"X11/Xlib-xcb", "x11-xcb", "libX11-xcb.so.1", "X"},
    {"X11/Xcursor", "xcursor", "libXcursor.so.1", "Xcursor"},
    {"X11/extensions/Xfixes", "xfixes", "libXfixes.so.3", "XFixes"},
    {"X11/extensions/XInput2", "xi", "libXi.so.6", "XI"},
    {"X11/extensions/Xrandr", "xrandr", "libXrandr.so.2", "XRR"},
    {"X11/", "x11", "libX11.so.6", "X"},
    {"xcb/", "xcb", "libxcb.so.1", "xcb_"},
    {"xkbcommon/", "xkbcommon", "libxkbcommon.so.0", "xkb_"},
    {"xkbcommon", "xkbcommon", "libxkbcommon.so.0", "xkb_"},
    {"xrandr", "xrandr", "libXrandr.so.2", "XRR"},
    {"Xi.", "xi", "libXi.so.6", "XI"},
    {"Xext", "xext", "libXext.so.6", "X"},
    {"GL/gl", "gl", "libGL.so.1", "gl"},
    {"GL/glx", "glx", "libGL.so.1", "glX"},
    {"GLES2/", "glesv2", "libGLESv2.so.2", "gl"},
    {"GL/osmesa", "osmesa", "libOSMesa.so.8", "OSMesa"},
    {"EGL/", "egl", "libEGL.so.1", "egl"},
    {"wayland-client", "wayland-client", "libwayland-client.so.0", "wl_"},
    {"wayland-cursor", "wayland-cursor", "libwayland-cursor.so.0", "wl_"},
    {"wayland-server", "wayland-server", "libwayland-server.so.0", "wl_"},

    {"SDL2/", "sdl2", "libSDL2-2.0.so.0", "SDL_"},
    {"SDL3/", "sdl3", "libSDL3.so.0", "SDL_"},
    {"raylib", "raylib", "libraylib.so", NULL},
    {"rlgl", "raylib", "libraylib.so", "rl"},

    {"curl/", "libcurl", "libcurl.so.4", "curl_"},

    {"zlib", "zlib", "libz.so.1", NULL},
    {"lz4", "liblz4", "liblz4.so.1", NULL},
    {"zstd", "libzstd", "libzstd.so.1", NULL},

    {"fftw3", "fftw3", "libfftw3.so.3", NULL},
    {"z3", "z3", "libz3.so", "Z3_"},

    {"freetype2/", "freetype2", "libfreetype.so.6", "FT_"},
    {"fontconfig/", "fontconfig", "libfontconfig.so.1", "Fc"},
    {"librsvg-2.0/", "librsvg-2.0", "librsvg-2.so.2", "rsvg_"},
    {"cairo/", "cairo", "libcairo.so.2", "cairo_"},

    {"png.h", "libpng", "libpng16.so.16", "png_"},
    {"turbojpeg.h", "libturbojpeg", "libturbojpeg.so.0", "tj"},
    {"webp/", "libwebp", "libwebp.so.7", "WebP"},

    {"libinput", "libinput", "libinput.so.10", "libinput_"},
    {"evdev/", NULL, "libevdev.so.2", "libevdev_"},

    {"openssl/", "openssl", "libcrypto.so", NULL},
    {"crypt.h", NULL, "libcrypt.so.1", "crypt"},

    {NULL, NULL, NULL, NULL},
};

static const ny_autolink_entry_t *ny_ffi_header_entry(const char *header_path) {
  if (!header_path)
    return NULL;
  const char *base = strrchr(header_path, '/');
  base = base ? base + 1 : header_path;
  for (const ny_autolink_entry_t *e = ny_autolink_table; e->header_prefix;
       e++) {
    size_t n = strlen(e->header_prefix);
    if (strncmp(header_path, e->header_prefix, n) == 0 ||
        strncmp(base, e->header_prefix, n) == 0)
      return e;
  }
  return NULL;
}

static bool ny_ffi_internal_c_parse_header(codegen_t *cg,
                                           const char *header_path,
                                           bool is_std,
                                           bool strict_internal,
                                           size_t *parsed_out,
                                           ny_c_header_summary_t *summary_out) {
  if (parsed_out)
    *parsed_out = 0;
  if (summary_out)
    memset(summary_out, 0, sizeof(*summary_out));
  if (is_std) {
    if (strict_internal) {
      NY_LOG_ERR("[ffi:c] internal frontend does not support system include <%s> yet\n",
                 header_path ? header_path : "");
      if (cg)
        cg->had_error = 1;
      return false;
    }
    return true;
  }

  size_t n = 0;
  char *src = ny_read_file_raw(header_path, &n);
  if (!src) {
    if (strict_internal) {
      NY_LOG_ERR("[ffi:c] internal frontend could not read header \"%s\"\n",
                 header_path ? header_path : "");
      if (cg)
        cg->had_error = 1;
      return false;
    }
    return true;
  }

  ny_c_header_summary_t summary = {0};
  char parse_err[256] = {0};
  bool ok = ny_parse_header_summary(src, n, &summary, parse_err, sizeof(parse_err)) != 0;
  free(src);
  if (!ok && strict_internal) {
    NY_LOG_ERR("[ffi:c] internal frontend rejected \"%s\": %s\n",
               header_path ? header_path : "",
               parse_err[0] ? parse_err : "unsupported C declaration");
    if (cg)
      cg->had_error = 1;
    return false;
  }
  if (parsed_out)
    *parsed_out = summary.declarations;
  if (summary_out)
    *summary_out = summary;
  if (summary.declarations || summary.preprocessor_lines) {
    NY_LOG_V2("[ffi:c] internal frontend summary %s: decls=%zu funcs=%zu vars=%zu typedefs=%zu tags=%zu aggregates=%zu fields=%zu aggregate_bytes=%zu fnptrs=%zu preproc=%zu includes=%zu defines=%zu object_defines=%zu function_defines=%zu unsupported_defines=%zu undefs=%zu conditionals=%zu active=%zu inactive=%zu unsupported=%zu\n",
              header_path ? header_path : "<memory>", summary.declarations,
              summary.functions, summary.variables, summary.typedefs,
              summary.tag_decls, summary.aggregate_layouts,
              summary.aggregate_fields, summary.aggregate_bytes,
              summary.function_pointers, summary.preprocessor_lines,
              summary.include_lines, summary.define_lines,
              summary.object_like_define_lines, summary.function_like_define_lines,
              summary.unsupported_define_lines, summary.undef_lines,
              summary.conditional_lines, summary.conditional_active_lines,
              summary.conditional_inactive_lines, summary.unsupported);
  }
  return true;
}

static char *ny_pkgconfig_lib(const char *pkg) {
  if (!pkg || !*pkg)
    return NULL;

  typedef struct {
    const char *pkg;
    char *lib;
  } ny_pkgcfg_lib_entry_t;
  static ny_pkgcfg_lib_entry_t *g_lib_cache = NULL;
  static size_t g_lib_cache_len = 0, g_lib_cache_cap = 0;
  for (size_t i = 0; i < g_lib_cache_len; i++) {
    if (g_lib_cache[i].pkg && strcmp(g_lib_cache[i].pkg, pkg) == 0) {

      return g_lib_cache[i].lib ? strdup(g_lib_cache[i].lib) : NULL;
    }
  }

  char cmd[256];
  snprintf(cmd, sizeof(cmd), "pkg-config --libs-only-l %s 2>%s", pkg,
           NY_FFI_NULL_DEVICE);
  FILE *f = popen(cmd, "r");
  if (!f)
    return NULL;
  char buf[256];
  buf[0] = '\0';
  if (!fgets(buf, sizeof(buf), f)) {
    pclose(f);
    return NULL;
  }
  pclose(f);

  size_t n = strlen(buf);
  while (n > 0 &&
         (buf[n - 1] == '\n' || buf[n - 1] == ' ' || buf[n - 1] == '\r'))
    buf[--n] = '\0';
  if (!buf[0])
    return NULL;

  const char *p = buf;
  while (*p == ' ')
    p++;
  if (p[0] == '-' && p[1] == 'l') {
    p += 2;

    const char *end = p;
    while (*end && *end != ' ')
      end++;
    size_t len = (size_t)(end - p);

    char *result = malloc(len + 8);
    if (!result)
      return NULL;
    memcpy(result, "lib", 3);
    memcpy(result + 3, p, len);
    memcpy(result + 3 + len, ".so", 4);

    if (g_lib_cache_len == g_lib_cache_cap) {
      size_t nc = g_lib_cache_cap ? g_lib_cache_cap * 2 : 32;
      ny_pkgcfg_lib_entry_t *nn =
          (ny_pkgcfg_lib_entry_t *)realloc(g_lib_cache, nc * sizeof(*nn));
      if (nn) {
        g_lib_cache = nn;
        g_lib_cache_cap = nc;
      }
    }
    if (g_lib_cache && g_lib_cache_len < g_lib_cache_cap) {
      g_lib_cache[g_lib_cache_len++] =
          (ny_pkgcfg_lib_entry_t){.pkg = ny_strdup(pkg), .lib = strdup(result)};
    }
    return result;
  }

  if (g_lib_cache_len == g_lib_cache_cap) {
    size_t nc = g_lib_cache_cap ? g_lib_cache_cap * 2 : 32;
    ny_pkgcfg_lib_entry_t *nn =
        (ny_pkgcfg_lib_entry_t *)realloc(g_lib_cache, nc * sizeof(*nn));
    if (nn) {
      g_lib_cache = nn;
      g_lib_cache_cap = nc;
    }
  }
  if (g_lib_cache && g_lib_cache_len < g_lib_cache_cap) {
    g_lib_cache[g_lib_cache_len++] =
        (ny_pkgcfg_lib_entry_t){.pkg = ny_strdup(pkg), .lib = NULL};
  }
  return NULL;
}

static char *ny_autolink_resolve(const char *header_path) {
  const ny_autolink_entry_t *e = ny_ffi_header_entry(header_path);
  if (!e)
    return NULL;
#ifdef __APPLE__
  if (e->fallback_lib && strcmp(e->fallback_lib, "libvulkan.so.1") == 0)
    return strdup("libMoltenVK.dylib");
#endif
  char *lib = ny_pkgconfig_lib(e->pkg_config);
  if (lib)
    return lib;
  if (e->fallback_lib)
    return strdup(e->fallback_lib);
  return NULL;
}

static void ny_pkgconfig_append_header_cflags(const char *header_path,
                                              char ***args, size_t *len,
                                              size_t *cap) {
  if (!header_path || !*header_path || !args || !len || !cap)
    return;
  const ny_autolink_entry_t *match = ny_ffi_header_entry(header_path);
  if (match && match->pkg_config)
    ny_pkgconfig_append_cflags(match->pkg_config, args, len, cap);
}

static void ny_ffi_append_default_clang_args(char ***args, size_t *len,
                                             size_t *cap) {
  static const char *const base_args[] = {
      "-I.",
      "-Isrc",
      "-Isrc/rt",
      "-I/usr/include",
      "-I/usr/local/include",
      "-I/opt/homebrew/include",
      "-I/opt/homebrew/opt/llvm@20/include",
      "-I/opt/homebrew/opt/llvm/include",
      "-I/usr/include/x86_64-linux-gnu",
      "-I/usr/include/aarch64-linux-gnu",
      "-x",
      "c",
      "-std=gnu11",
      "-D__NYTRIX_FFI_IMPORT__",
      NULL,
  };
  for (int i = 0; base_args[i]; i++)
    ny_push_unique_owned_arg(args, len, cap, base_args[i]);

#ifdef __APPLE__
  const char *sdk = getenv("SDKROOT");
  if (!ny_path_readable(sdk))
    sdk = "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk";
  if (!ny_path_readable(sdk))
    sdk = "/Applications/Xcode.app/Contents/Developer/Platforms/"
          "MacOSX.platform/Developer/SDKs/"
          "MacOSX.sdk";
  if (ny_path_readable(sdk)) {
    char inc[1024];
    ny_push_unique_owned_arg(args, len, cap, "-isysroot");
    ny_push_unique_owned_arg(args, len, cap, sdk);
    snprintf(inc, sizeof(inc), "-I%s/usr/include", sdk);
    ny_push_unique_owned_arg(args, len, cap, inc);
    snprintf(inc, sizeof(inc), "-I%s/System/Library/Frameworks", sdk);
    ny_push_unique_owned_arg(args, len, cap, inc);
  }
  ny_push_unique_owned_arg(args, len, cap, "-D_DARWIN_C_SOURCE");
#endif
}

static void ny_ffi_free_clang_args(char **args, size_t len) {
  for (size_t i = 0; i < len; i++)
    free(args[i]);
  free(args);
}

static bool ny_ffi_type_spelling_is_bool(CXType type) {
  CXString spelling_cx = clang_getTypeSpelling(type);
  const char *spelling = clang_getCString(spelling_cx);
  bool ok = spelling &&
            (strcmp(spelling, "bool") == 0 || strcmp(spelling, "_Bool") == 0 ||
             strcmp(spelling, "enum bool") == 0);
  clang_disposeString(spelling_cx);
  return ok;
}

static bool ny_ffi_function_return_spelling_is_bool(CXCursor cursor) {
  CXType fn_type = clang_getCursorType(cursor);
  CXString spelling_cx = clang_getTypeSpelling(fn_type);
  const char *spelling = clang_getCString(spelling_cx);
  bool ok = false;
  if (spelling) {
    while (isspace((unsigned char)*spelling))
      spelling++;
    ok = strncmp(spelling, "bool", 4) == 0 &&
         (isspace((unsigned char)spelling[4]) || spelling[4] == '(');
    ok = ok || (strncmp(spelling, "_Bool", 5) == 0 &&
                (isspace((unsigned char)spelling[5]) || spelling[5] == '('));
    ok = ok || (strncmp(spelling, "enum bool", 9) == 0 &&
                (isspace((unsigned char)spelling[9]) || spelling[9] == '('));
  }
  clang_disposeString(spelling_cx);
  return ok;
}

static bool ny_ffi_token_is_bool(const char *tok) {
  return tok && (strcmp(tok, "bool") == 0 || strcmp(tok, "_Bool") == 0 ||
                 strcmp(tok, "enum bool") == 0);
}

static bool ny_ffi_text_has_bool_token(const char *text, size_t limit) {
  if (!text)
    return false;
  for (size_t i = 0; i < limit && text[i]; i++) {
    if (strncmp(text + i, "_Bool", 5) == 0) {
      bool left = i == 0 ||
                  !(isalnum((unsigned char)text[i - 1]) || text[i - 1] == '_');
      bool right = !(isalnum((unsigned char)text[i + 5]) || text[i + 5] == '_');
      if (left && right)
        return true;
    }
    if (strncmp(text + i, "bool", 4) == 0) {
      bool left = i == 0 ||
                  !(isalnum((unsigned char)text[i - 1]) || text[i - 1] == '_');
      bool right = !(isalnum((unsigned char)text[i + 4]) || text[i + 4] == '_');
      if (left && right)
        return true;
    }
  }
  return false;
}

static bool ny_ffi_function_return_tokens_bool(CXCursor cursor,
                                               const char *c_name) {
  if (!c_name || !*c_name)
    return false;
  CXTranslationUnit tu = clang_Cursor_getTranslationUnit(cursor);
  CXSourceRange range = clang_getCursorExtent(cursor);
  CXToken *tokens = NULL;
  unsigned count = 0;
  clang_tokenize(tu, range, &tokens, &count);
  if (!tokens || count == 0) {
    clang_disposeTokens(tu, tokens, count);
    return false;
  }
  bool seen_bool = false;
  for (unsigned i = 0; i < count; i++) {
    CXString tok_cx = clang_getTokenSpelling(tu, tokens[i]);
    const char *tok = clang_getCString(tok_cx);
    if (tok && strcmp(tok, c_name) == 0) {
      clang_disposeString(tok_cx);
      break;
    }
    if (ny_ffi_token_is_bool(tok))
      seen_bool = true;
    clang_disposeString(tok_cx);
  }
  clang_disposeTokens(tu, tokens, count);
  return seen_bool;
}

static bool ny_ffi_function_return_line_bool(CXCursor cursor,
                                             const char *c_name) {
  if (!c_name || !*c_name)
    return false;
  CXSourceLocation loc = clang_getCursorLocation(cursor);
  CXFile cx_file = NULL;
  unsigned line = 0;
  clang_getSpellingLocation(loc, &cx_file, &line, NULL, NULL);
  if (!cx_file || line == 0)
    return false;
  CXString file_cx = clang_getFileName(cx_file);
  const char *file = clang_getCString(file_cx);
  if (!file || !*file) {
    clang_disposeString(file_cx);
    return false;
  }
  FILE *f = fopen(file, "r");
  if (!f) {
    clang_disposeString(file_cx);
    return false;
  }
  char buf[4096];
  bool ok = false;
  for (unsigned cur = 1; fgets(buf, sizeof(buf), f); cur++) {
    if (cur != line)
      continue;
    char *name = strstr(buf, c_name);
    if (name)
      ok = ny_ffi_text_has_bool_token(buf, (size_t)(name - buf));
    break;
  }
  fclose(f);
  clang_disposeString(file_cx);
  return ok;
}

static bool ny_ffi_clean_record_name(const char *raw, char *buf, size_t cap) {
  if (!raw || !*raw || !buf || cap == 0)
    return false;
  while (strncmp(raw, "const ", 6) == 0)
    raw += 6;
  while (strncmp(raw, "struct ", 7) == 0 || strncmp(raw, "union ", 6) == 0 ||
         strncmp(raw, "enum ", 5) == 0) {
    if (strncmp(raw, "struct ", 7) == 0)
      raw += 7;
    else if (strncmp(raw, "union ", 6) == 0)
      raw += 6;
    else
      raw += 5;
  }
  size_t n = strlen(raw);
  while (n > 0 && isspace((unsigned char)raw[n - 1]))
    n--;
  if (n == 0 || n + 1 > cap)
    return false;
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)raw[i];
    if (!(isalnum(c) || c == '_'))
      return false;
  }
  memcpy(buf, raw, n);
  buf[n] = '\0';
  return buf[0] && buf[0] != '_';
}

static bool ny_ffi_record_type_name(CXType type, char *buf, size_t cap) {
  if (!buf || cap == 0)
    return false;
  buf[0] = '\0';

  if (type.kind == CXType_Typedef) {
    CXType canon = clang_getCanonicalType(type);
    if (canon.kind == CXType_Record) {
      CXString name_cx = clang_getTypeSpelling(type);
      bool ok = ny_ffi_clean_record_name(clang_getCString(name_cx), buf, cap);
      clang_disposeString(name_cx);
      if (ok)
        return true;
    }
  }

  CXType canon = clang_getCanonicalType(type);
  if (type.kind != CXType_Record && canon.kind != CXType_Record)
    return false;
  CXType record_ty = type.kind == CXType_Record ? type : canon;
  CXString name_cx = clang_getTypeSpelling(record_ty);
  bool ok = ny_ffi_clean_record_name(clang_getCString(name_cx), buf, cap);
  clang_disposeString(name_cx);
  if (ok)
    return true;

  CXCursor decl = clang_getTypeDeclaration(record_ty);
  name_cx = clang_getCursorSpelling(decl);
  ok = ny_ffi_clean_record_name(clang_getCString(name_cx), buf, cap);
  clang_disposeString(name_cx);
  return ok;
}

static const char *map_clang_type(CXType type);

static bool ny_ffi_type_is_char_pointer(CXType type) {
  CXType probe = type;
  if (probe.kind != CXType_Pointer)
    probe = clang_getCanonicalType(type);
  if (probe.kind != CXType_Pointer)
    return false;
  CXType pointee = clang_getPointeeType(probe);
  CXType canon = clang_getCanonicalType(pointee);
  switch (canon.kind) {
  case CXType_Char_S:
  case CXType_Char_U:
  case CXType_SChar:
  case CXType_UChar:
    return true;
  default:
    return false;
  }
}

static const char *map_clang_type_named(CXType type, char *buf, size_t cap) {
  if (ny_ffi_type_spelling_is_bool(type))
    return "u8";
  if (type.kind == CXType_Pointer)
    return map_clang_type(type);
  if (ny_ffi_record_type_name(type, buf, cap))
    return buf;
  return map_clang_type(type);
}

static const char *map_clang_type(CXType type) {
  if (ny_ffi_type_spelling_is_bool(type))
    return "u8";
  switch (type.kind) {
  case CXType_Void:
    return "void";
  case CXType_Bool:
    return "u8";
  case CXType_Char_S:
  case CXType_SChar:
    return "i8";
  case CXType_Char_U:
  case CXType_UChar:
    return "u8";
  case CXType_Short:
    return "i16";
  case CXType_UShort:
    return "u16";
  case CXType_Int:
    return "i32";
  case CXType_UInt:
    return "u32";
  case CXType_Long:
    return "i64";
  case CXType_ULong:
    return "u64";
  case CXType_LongLong:
    return "i64";
  case CXType_ULongLong:
    return "u64";
  case CXType_Float:
    return "f32";
  case CXType_Double:
    return "f64";
  case CXType_Pointer: {
    CXType pointee = clang_getPointeeType(type);
    if (pointee.kind == CXType_FunctionProto ||
        pointee.kind == CXType_FunctionNoProto)
      return "fnptr";

    if (pointee.kind == CXType_Typedef) {
      CXString pointee_name = clang_getTypeSpelling(pointee);
      const char *pn = clang_getCString(pointee_name);
      bool handle_pointee =
          pn && (strstr(pn, "Vk") || strstr(pn, "wl_") || strstr(pn, "xcb_"));
      clang_disposeString(pointee_name);
      if (handle_pointee)
        return "ptr";
    }

    CXString name_cx = clang_getTypeSpelling(type);
    const char *name = clang_getCString(name_cx);
    bool is_handle = name && (strstr(name, "Vk") || strstr(name, "wl_") ||
                              strstr(name, "xcb_"));
    clang_disposeString(name_cx);

    if (is_handle)
      return "u64";
    return "ptr";
  }
  case CXType_FunctionProto:
  case CXType_FunctionNoProto:
    return "fnptr";
  case CXType_Enum:

    return "i32";
  case CXType_Typedef: {
    char name_buf[128];
    if (ny_ffi_record_type_name(type, name_buf, sizeof(name_buf)))
      return "ptr";

    CXType canon = clang_getCanonicalType(type);
    if (canon.kind != CXType_Typedef)
      return map_clang_type(canon);
    return "u64";
  }
  case CXType_Record:
  case CXType_ConstantArray:
  case CXType_IncompleteArray:

    return "ptr";
  default:
    return "u64";
  }
}

typedef struct {
  codegen_t *cg;
  const char *prefix;
  const char *header_path;
  size_t prefix_len;
  bool explicit_prefix;
  bool namespace_alias;
  bool is_std_header;
  int pass;
} ffi_context;

static bool ny_ffi_prefix_is_namespace_alias(const char *prefix) {
  if (!prefix || !*prefix)
    return false;
  size_t len = strlen(prefix);
  if (prefix[len - 1] == '.')
    return len > 1;
  return len == 1 && islower((unsigned char)prefix[0]);
}

static const char *ny_ffi_public_symbol_name(ffi_context *ctx,
                                             const char *c_name, char *buf,
                                             size_t buf_len) {
  if (!ctx || !ctx->namespace_alias || !ctx->prefix || !*ctx->prefix ||
      !c_name || !*c_name)
    return c_name;
  size_t prefix_len = ctx->prefix_len;
  if (prefix_len > 0 && ctx->prefix[prefix_len - 1] == '.')
    prefix_len--;
  if (prefix_len == 0 || !buf || buf_len == 0)
    return c_name;
  int n = snprintf(buf, buf_len, "%.*s.%s", (int)prefix_len, ctx->prefix,
                   c_name);
  if (n <= 0 || (size_t)n >= buf_len)
    return c_name;
  return buf;
}

static bool ny_ffi_import_allowed(ffi_context *ctx, const char *name,
                                  const char *kind);
static void ny_ffi_register_int_constant(ffi_context *ctx, const char *name,
                                         int64_t value);
static bool ny_ffi_layout_uses_sret(layout_def_t *layout);
static bool ny_ffi_layout_uses_byval(layout_def_t *layout);
static void ny_ffi_add_type_attr(ffi_context *ctx, LLVMValueRef fn,
                                 LLVMAttributeIndex idx, const char *name,
                                 LLVMTypeRef type);
static void ny_ffi_add_align_attr(ffi_context *ctx, LLVMValueRef fn,
                                  LLVMAttributeIndex idx, size_t align);

static char *ny_ffi_ctok_strdup(ny_ctok_t tok) {
  if (!tok.start || tok.len == 0)
    return NULL;
  char *s = (char *)malloc(tok.len + 1);
  if (!s)
    return NULL;
  memcpy(s, tok.start, tok.len);
  s[tok.len] = '\0';
  return s;
}

static const char *ny_ffi_map_internal_c_type(const ny_ctype_t *ty, char *buf,
                                              size_t cap) {
  if (!ty)
    return NULL;
  if (ty->flags & NY_CTYPEF_FUNCTION_PTR)
    return "fnptr";
  if (ty->ptr_depth > 0)
    return "ptr";
  switch (ty->kind) {
  case NY_CTYPE_VOID:
    return "void";
  case NY_CTYPE_BOOL:
    return "u8";
  case NY_CTYPE_CHAR:
    return (ty->flags & NY_CTYPEF_UNSIGNED) ? "u8" : "i8";
  case NY_CTYPE_SHORT:
    return (ty->flags & NY_CTYPEF_UNSIGNED) ? "u16" : "i16";
  case NY_CTYPE_INT:
  case NY_CTYPE_ENUM:
    return (ty->flags & NY_CTYPEF_UNSIGNED) ? "u32" : "i32";
  case NY_CTYPE_LONG:
    return (ty->flags & NY_CTYPEF_UNSIGNED) ? "u64" : "i64";
  case NY_CTYPE_FLOAT:
    return "f32";
  case NY_CTYPE_DOUBLE:
    return "f64";
  case NY_CTYPE_NAMED:
  case NY_CTYPE_STRUCT:
  case NY_CTYPE_UNION:
    if (buf && cap > 0 && ty->name.start && ty->name.len > 0 &&
        ty->name.len < cap) {
      memcpy(buf, ty->name.start, ty->name.len);
      buf[ty->name.len] = '\0';
      return buf;
    }
    return NULL;
  case NY_CTYPE_INVALID:
  default:
    return NULL;
  }
}

static const char *ny_ffi_map_internal_c_field_type(const ny_c_field_t *field,
                                                    char *buf, size_t cap) {
  if (!field)
    return NULL;
  ny_ctype_t ty = {0};
  ty.kind = field->kind;
  ty.flags = field->flags;
  ty.ptr_depth = field->ptr_depth;
  ty.name = field->type_name;
  return ny_ffi_map_internal_c_type(&ty, buf, cap);
}

static bool ny_ffi_internal_c_type_is_builtin(const char *ty) {
  if (!ty || !*ty)
    return false;
  static const char *const builtins[] = {
      "void", "u8", "i8", "u16", "i16", "u32", "i32",
      "u64",  "i64", "f32", "f64", "ptr", "fnptr", "str", "cstr",
  };
  for (size_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++) {
    if (strcmp(ty, builtins[i]) == 0)
      return true;
  }
  return false;
}

static bool ny_ffi_internal_c_type_requires_layout(const char *ty) {
  return ty && *ty && !ny_ffi_internal_c_type_is_builtin(ty);
}

static bool ny_ffi_internal_c_type_supported(codegen_t *cg, const char *ty) {
  if (!ny_ffi_internal_c_type_requires_layout(ty))
    return true;
  return cg && lookup_layout(cg, ty) != NULL;
}

static bool ny_ffi_register_internal_c_layout(codegen_t *cg,
                                              const ny_cdecl_t *decl) {
  if (!cg || !decl || decl->kind != NY_CDECL_TYPEDEF ||
      !decl->type.aggregate_has_layout || decl->type.field_count == 0)
    return true;
  if (decl->type.field_count != decl->type.aggregate_fields)
    return true;
  char *name = ny_ffi_ctok_strdup(decl->name);
  if (!name || !*name) {
    free(name);
    return false;
  }
  if (lookup_layout(cg, name)) {
    free(name);
    return true;
  }

  LLVMTypeRef elems[NY_C_MAX_FIELDS];
  const char *field_types[NY_C_MAX_FIELDS];
  char field_type_bufs[NY_C_MAX_FIELDS][128];
  type_layout_t field_layouts[NY_C_MAX_FIELDS];
  unsigned elem_count = 0;
  token_t empty_tok = {0};
  for (unsigned i = 0; i < decl->type.field_count && i < NY_C_MAX_FIELDS; i++) {
    const ny_c_field_t *field = &decl->type.fields[i];
    const char *ny_type =
        ny_ffi_map_internal_c_field_type(field, field_type_bufs[i],
                                         sizeof(field_type_bufs[i]));
    if (!ny_type) {
      NY_LOG_V1("[ffi:c] internal frontend cannot lower field %u for layout %s\n",
                i + 1, name);
      free(name);
      return true;
    }
    if (ny_ffi_internal_c_type_requires_layout(ny_type) &&
        !lookup_layout(cg, ny_type)) {
      NY_LOG_V1("[ffi:c] internal frontend cannot lower nested field %u for layout %s yet\n",
                i + 1, name);
      free(name);
      return true;
    }
    type_layout_t tl = resolve_raw_layout(cg, ny_type, empty_tok);
    LLVMTypeRef llvm_ty = (tl.is_valid && tl.llvm_type)
                              ? tl.llvm_type
                              : resolve_abi_type_name(cg, ny_type, empty_tok);
    if (!llvm_ty || !tl.is_valid || tl.size != field->size) {
      free(name);
      return true;
    }
    field_types[i] = ny_type;
    field_layouts[i] = tl;
    elems[elem_count++] = llvm_ty;
  }

  LLVMTypeRef st = LLVMStructCreateNamed(cg->ctx, name);
  layout_def_t *def = (layout_def_t *)calloc(1, sizeof(*def));
  if (!def) {
    free(name);
    return false;
  }
  def->name = ny_strdup(name);
  def->llvm_type = st;
  def->is_layout = true;
  def->heap_allocated = true;
  def->size = decl->type.aggregate_size;
  def->align = decl->type.aggregate_align ? decl->type.aggregate_align : 1;

  for (unsigned i = 0; i < decl->type.field_count && i < NY_C_MAX_FIELDS; i++) {
    const ny_c_field_t *field = &decl->type.fields[i];
    char *field_name = ny_ffi_ctok_strdup(field->name);
    if (!field_name) {
      for (size_t j = 0; j < def->fields.len; j++) {
        free((char *)def->fields.data[j].name);
        free((char *)def->fields.data[j].type_name);
      }
      free(def->fields.data);
      free((char *)def->name);
      free(def);
      free(name);
      return false;
    }
    layout_field_info_t info = {
        .name = field_name,
        .type_name = ny_strdup(field_types[i]),
        .offset = field->offset,
        .size = field_layouts[i].size,
        .align = field_layouts[i].align ? field_layouts[i].align : 1,
    };
    vec_push(&def->fields, info);
  }
  LLVMStructSetBody(st, elems, elem_count, false);
  vec_push(&cg->layouts, def);
  free(name);
  return true;
}

static void ny_ffi_add_link_once(codegen_t *cg, const char *lib) {
  if (!cg || !lib || !*lib)
    return;
#ifndef _WIN32
  (void)dlopen(lib, RTLD_LAZY | RTLD_GLOBAL);
#endif
  for (size_t i = 0; i < cg->links.len; i++) {
    if (strcmp(cg->links.data[i], lib) == 0)
      return;
  }
  vec_push(&cg->links, ny_strdup(lib));
}

static bool ny_ffi_register_internal_c_function(codegen_t *cg,
                                                const ny_cdecl_t *decl,
                                                const char *prefix,
                                                const char *lib,
                                                size_t *lowered_out) {
  if (!cg || !decl || decl->kind != NY_CDECL_FUNC)
    return true;
  char *c_name_owned = ny_ffi_ctok_strdup(decl->name);
  if (!c_name_owned || !*c_name_owned) {
    free(c_name_owned);
    NY_LOG_ERR("[ffi:c] internal frontend saw a function declaration without a name\n");
    return false;
  }
  if (c_name_owned[0] == '_') {
    free(c_name_owned);
    return true;
  }

  ffi_context ctx = {
      .cg = cg,
      .prefix = prefix,
      .prefix_len = prefix ? strlen(prefix) : 0,
      .explicit_prefix = prefix && *prefix,
      .namespace_alias = ny_ffi_prefix_is_namespace_alias(prefix),
  };
  if (ctx.prefix && ctx.prefix_len > 0 && !ctx.namespace_alias &&
      strncmp(c_name_owned, ctx.prefix, ctx.prefix_len) != 0) {
    free(c_name_owned);
    return true;
  }

  char public_name_buf[512];
  const char *ny_name =
      ny_ffi_public_symbol_name(&ctx, c_name_owned, public_name_buf,
                                sizeof(public_name_buf));
  fun_sig *existing_sig = lookup_fun_exact(cg, ny_name);
  if (existing_sig && existing_sig->is_native_abi) {
    free(c_name_owned);
    return true;
  }
  if (!ny_ffi_import_allowed(&ctx, ny_name, "function")) {
    free(c_name_owned);
    return true;
  }

  char ret_buf[128];
  const char *ny_ret =
      ny_ffi_map_internal_c_type(&decl->type, ret_buf, sizeof(ret_buf));
  if (!ny_ret) {
    NY_LOG_V1("[ffi:c] internal frontend cannot lower return type for %s\n",
               c_name_owned);
    free(c_name_owned);
    return false;
  }
  if (!ny_ffi_internal_c_type_supported(cg, ny_ret)) {
    NY_LOG_V1("[ffi:c] internal frontend cannot lower return layout %s for %s yet\n",
              ny_ret, c_name_owned);
    free(c_name_owned);
    return false;
  }
  layout_def_t *ret_layout = lookup_layout(cg, ny_ret);
  bool native_sret_return = ny_ffi_layout_uses_sret(ret_layout);

  token_t empty_tok = {0};
  LLVMTypeRef *param_llvm_types = NULL;
  const char **param_ny_types = NULL;
  char (*param_bufs)[128] = NULL;
  layout_def_t **param_byval_layouts = NULL;
  unsigned actual_params = decl->param_count + (native_sret_return ? 1u : 0u);
  if (actual_params > 0) {
    param_llvm_types =
        (LLVMTypeRef *)alloca(sizeof(LLVMTypeRef) * actual_params);
    if (native_sret_return)
      param_llvm_types[0] = cg->type_i8ptr;
  }
  if (decl->param_count > 0) {
    param_ny_types =
        (const char **)alloca(sizeof(const char *) * decl->param_count);
    param_byval_layouts =
        (layout_def_t **)alloca(sizeof(layout_def_t *) * decl->param_count);
    param_bufs = (char (*)[128])alloca(sizeof(char[128]) * decl->param_count);
    for (unsigned i = 0; i < decl->param_count; i++) {
      param_byval_layouts[i] = NULL;
      const char *ny_arg_type = ny_ffi_map_internal_c_type(
          &decl->params[i], param_bufs[i], sizeof(param_bufs[i]));
      if (!ny_arg_type || strcmp(ny_arg_type, "void") == 0) {
        NY_LOG_V1("[ffi:c] internal frontend cannot lower parameter %u for %s\n",
                   i + 1, c_name_owned);
        free(c_name_owned);
        return false;
      }
      if (!ny_ffi_internal_c_type_supported(cg, ny_arg_type)) {
        NY_LOG_V1("[ffi:c] internal frontend cannot lower parameter layout %s for %s yet\n",
                  ny_arg_type, c_name_owned);
        free(c_name_owned);
        return false;
      }
      layout_def_t *ly = lookup_layout(cg, ny_arg_type);
      if (ly && ny_ffi_layout_uses_byval(ly)) {
        param_ny_types[i] = "ptr";
        param_byval_layouts[i] = ly;
      } else {
        param_ny_types[i] = ny_arg_type;
      }
      param_llvm_types[i + (native_sret_return ? 1u : 0u)] =
          resolve_abi_type_name(cg, ny_arg_type, empty_tok);
    }
  }

  LLVMTypeRef llvm_ret = native_sret_return
                             ? LLVMVoidTypeInContext(cg->ctx)
                             : resolve_abi_type_name(cg, ny_ret, empty_tok);
  LLVMTypeRef ft = LLVMFunctionType(llvm_ret, param_llvm_types, actual_params,
                                    decl->is_variadic ? true : false);
  LLVMValueRef f = LLVMGetNamedFunction(cg->module, c_name_owned);
  if (!f) {
    f = LLVMAddFunction(cg->module, c_name_owned, ft);
    LLVMSetLinkage(f, LLVMExternalLinkage);
  }
  ffi_context attr_ctx = {.cg = cg};
  if (native_sret_return && ret_layout) {
    ny_ffi_add_type_attr(&attr_ctx, f, 1, "sret", ret_layout->llvm_type);
    ny_ffi_add_align_attr(&attr_ctx, f, 1, ret_layout->align);
  }
  for (unsigned i = 0; i < decl->param_count; i++) {
    layout_def_t *arg_layout = param_byval_layouts ? param_byval_layouts[i] : NULL;
    if (!arg_layout)
      continue;
    LLVMAttributeIndex idx =
        (LLVMAttributeIndex)(i + 1 + (native_sret_return ? 1u : 0u));
    ny_ffi_add_type_attr(&attr_ctx, f, idx, "byval", arg_layout->llvm_type);
    ny_ffi_add_align_attr(&attr_ctx, f, idx, arg_layout->align);
  }

  bool already_known = false;
  bool existing_is_ny_fn = false;
  for (int si = 0; si < (int)cg->fun_sigs.len; si++) {
    fun_sig *existing = &((fun_sig *)cg->fun_sigs.data)[si];
    if (existing->name && strcmp(existing->name, ny_name) == 0) {
      already_known = true;
      existing_is_ny_fn = existing->stmt_t != NULL;
      break;
    }
  }
  if (!already_known || existing_is_ny_fn) {
    fun_sig sig;
    ny_fun_sig_init(&sig, ny_name, ft, f, NULL, (int)decl->param_count,
                    decl->is_variadic ? true : false, true);
    sig.link_name = ny_strdup(c_name_owned);
    sig.is_native_abi = true;
    sig.native_sret_return = native_sret_return;
    sig.return_type = ny_strdup(ny_ret);
    for (unsigned i = 0; i < decl->param_count; i++) {
      vec_push(&sig.param_types, ny_strdup(param_ny_types[i]));
      vec_push(&sig.native_byval_param_layouts,
               param_byval_layouts[i] && ny_ffi_layout_uses_byval(param_byval_layouts[i])
                   ? ny_strdup(param_byval_layouts[i]->name)
                   : NULL);
    }
    vec_push(&cg->fun_sigs, sig);
  }
  ny_ffi_add_link_once(cg, lib);
  if (lowered_out)
    (*lowered_out)++;
  free(c_name_owned);
  return true;
}

static void ny_ffi_register_internal_c_defines(codegen_t *cg,
                                               const ny_parser_t *p,
                                               const char *prefix,
                                               size_t *lowered_out) {
  if (lowered_out)
    *lowered_out = 0;
  if (!cg || !p || p->define_count == 0)
    return;
  ffi_context ctx = {
      .cg = cg,
      .prefix = prefix,
      .prefix_len = prefix ? strlen(prefix) : 0,
      .explicit_prefix = prefix && *prefix,
      .namespace_alias = ny_ffi_prefix_is_namespace_alias(prefix),
  };
  size_t lowered = 0;
  for (unsigned i = 0; i < p->define_count; i++) {
    char *name = ny_ffi_ctok_strdup(p->define_names[i]);
    if (!name)
      continue;
    size_t before = cg->global_vars.len;
    ny_ffi_register_int_constant(&ctx, name, (int64_t)p->define_values[i]);
    if (cg->global_vars.len > before)
      lowered++;
    free(name);
  }
  if (lowered_out)
    *lowered_out = lowered;
}

static bool ny_ffi_internal_c_lower_header(codegen_t *cg,
                                           const char *header_path,
                                           const char *prefix,
                                           const char *lib,
                                           size_t *lowered_out,
                                           size_t *constants_out) {
  if (lowered_out)
    *lowered_out = 0;
  if (constants_out)
    *constants_out = 0;
  if (!cg || !header_path || !*header_path)
    return false;
  size_t n = 0;
  char *src = ny_read_file_raw(header_path, &n);
  if (!src) {
    NY_LOG_ERR("[ffi:c] internal frontend could not read header \"%s\"\n",
               header_path);
    cg->had_error = 1;
    return false;
  }
  ny_parser_t p;
  ny_parse_init(&p, src, n);
  bool ok = true;
  size_t lowered = 0;
  while (p.tok.kind != NY_CTOK_EOF) {
    ny_cdecl_t decl;
    int rc = ny_parse_decl(&p, &decl);
    if (rc > 0) {
      if (!ny_ffi_register_internal_c_layout(cg, &decl)) {
        ok = false;
        break;
      }
      if (!ny_ffi_register_internal_c_function(cg, &decl, prefix, lib,
                                               &lowered)) {
        ok = false;
        break;
      }
      continue;
    }
    if (rc < 0) {
      NY_LOG_ERR("[ffi:c] internal frontend rejected \"%s\": %s\n",
                 header_path, ny_parse_error(&p));
      cg->had_error = 1;
      ok = false;
    }
    break;
  }
  size_t constants = 0;
  ny_ffi_register_internal_c_defines(cg, &p, prefix, &constants);
  free(src);
  if (lowered_out)
    *lowered_out = lowered;
  if (constants_out)
    *constants_out = constants;
  if (ok)
    NY_LOG_V1("[ffi:c] internal frontend lowered %zu function declaration(s) and %zu integer constant(s) without libclang\n",
              lowered, constants);
  return ok;
}

static const char *ny_ffi_basename(const char *path) {
  if (!path || !*path)
    return path;
  const char *slash = strrchr(path, '/');
#ifdef _WIN32
  const char *backslash = strrchr(path, '\\');
  if (!slash || (backslash && backslash > slash))
    slash = backslash;
#endif
  return slash ? slash + 1 : path;
}

static bool ny_ffi_cursor_in_requested_header(ffi_context *ctx,
                                              CXCursor cursor) {
  if (!ctx || !ctx->header_path || !*ctx->header_path)
    return false;
  CXSourceLocation loc = clang_getCursorLocation(cursor);
  CXFile cx_file = NULL;
  clang_getSpellingLocation(loc, &cx_file, NULL, NULL, NULL);
  if (!cx_file)
    return false;
  CXString file_cx = clang_getFileName(cx_file);
  const char *file = clang_getCString(file_cx);
  const char *want = ny_ffi_basename(ctx->header_path);
  const char *got = ny_ffi_basename(file);
  bool ok = want && got && strcmp(want, got) == 0;
  clang_disposeString(file_cx);
  return ok;
}

static bool ny_ffi_const_name_ok(const char *name) {
  if (!name || !*name || name[0] == '_')
    return false;
  bool saw_upper = false;
  for (const unsigned char *p = (const unsigned char *)name; *p; ++p) {
    if (!(isalnum(*p) || *p == '_'))
      return false;
    if (isupper(*p))
      saw_upper = true;
  }
  return saw_upper;
}

static bool ny_ffi_name_collides(codegen_t *cg, const char *name) {
  return cg && name && *name &&
         (lookup_global_exact(cg, name) || lookup_fun_exact(cg, name) ||
          lookup_global(cg, name) || lookup_fun(cg, name, 0));
}

static bool ny_ffi_import_allowed(ffi_context *ctx, const char *name,
                                  const char *kind) {
  if (!ctx || !ctx->cg || !name || !*name)
    return false;
  if (ctx->prefix_len > 0)
    return true;
  if (!ny_ffi_name_collides(ctx->cg, name))
    return true;
  if (kind && strcmp(kind, "function") == 0) {
    if (verbose_enabled >= 1) {
      ny_diag_warning((token_t){0},
                      "skipping FFI function import '%s' from '%s' because a "
                      "Nytrix symbol already uses that name",
                      name, ctx->header_path ? ctx->header_path : "<ffi>");
      ny_diag_hint("import the header with a namespace alias, for example: "
                   "#include <...> as \"c\"");
    }
    return false;
  }
  ny_diag_error((token_t){0},
                "FFI %s import '%s' from '%s' conflicts with an existing Nytrix symbol",
                kind ? kind : "symbol", name,
                ctx->header_path ? ctx->header_path : "<ffi>");
  ny_diag_hint(
      "import the header with a namespace alias, for example: #include <...> "
      "as \"c\"");
  ctx->cg->had_error = 1;
  return false;
}

static void ny_ffi_register_int_constant(ffi_context *ctx, const char *name,
                                         int64_t value) {
  if (!ctx || !ctx->cg || !ny_ffi_const_name_ok(name))
    return;
  if (!ny_small_int_fits_i64(value))
    return;
  char public_name_buf[512];
  const char *ny_name =
      ny_ffi_public_symbol_name(ctx, name, public_name_buf,
                                sizeof(public_name_buf));
  if (lookup_global_exact(ctx->cg, ny_name))
    return;
  if (ny_ffi_name_collides(ctx->cg, ny_name))
    return;
  if (!ny_ffi_import_allowed(ctx, ny_name, "constant"))
    return;

  LLVMValueRef raw = LLVMConstInt(ctx->cg->type_i64, (uint64_t)value, true);
  LLVMValueRef tagged =
      LLVMConstInt(ctx->cg->type_i64, (((uint64_t)value) << 1) | 1u, true);
  binding b = {0};
  b.name = ny_strdup(ny_name);
  b.value = tagged;
  b.raw_int_value = raw;
  b.is_stable = true;
  b.owned = true;
  b.is_int_direct = true;
  b.is_int_raw_direct = true;
  b.type_name = "int";
  b.decl_type_name = "int";
  b.has_int_range = true;
  b.int_min_raw = value;
  b.int_max_raw = value;
  vec_push(&ctx->cg->global_vars, b);
}

static bool ny_ffi_parse_integer_literal(const char *text, int64_t *out) {
  if (!text || !*text || !out)
    return false;
  char buf[128];
  size_t n = 0;
  for (const char *p = text; *p && n + 1 < sizeof(buf); ++p) {
    if (*p == '\'' || *p == '_')
      continue;
    if (isalnum((unsigned char)*p) || *p == 'x' || *p == 'X' || *p == '+' ||
        *p == '-')
      buf[n++] = *p;
    else
      break;
  }
  buf[n] = '\0';
  while (n > 0) {
    char c = buf[n - 1];
    if (c == 'u' || c == 'U' || c == 'l' || c == 'L')
      buf[--n] = '\0';
    else
      break;
  }
  if (n == 0)
    return false;
  char *end = NULL;
  long long v = strtoll(buf, &end, 0);
  if (!end || *end != '\0')
    return false;
  *out = (int64_t)v;
  return true;
}

typedef struct {
  const char *p;
  ffi_context *ctx;
} ny_ffi_int_expr_t;

static void ny_ffi_expr_ws(ny_ffi_int_expr_t *e) {
  while (e && e->p && isspace((unsigned char)*e->p))
    e->p++;
}

static bool ny_ffi_expr_accept(ny_ffi_int_expr_t *e, const char *op) {
  ny_ffi_expr_ws(e);
  size_t n = strlen(op);
  if (!e || !e->p || strncmp(e->p, op, n) != 0)
    return false;
  e->p += n;
  return true;
}

static bool ny_ffi_expr_number(ny_ffi_int_expr_t *e, int64_t *out);

static bool ny_ffi_expr_primary(ny_ffi_int_expr_t *e, int64_t *out) {
  ny_ffi_expr_ws(e);
  if (ny_ffi_expr_accept(e, "(")) {
    int64_t v = 0;
    if (!ny_ffi_expr_number(e, &v) || !ny_ffi_expr_accept(e, ")"))
      return false;
    *out = v;
    return true;
  }

  const char *start = e ? e->p : NULL;
  if (!start)
    return false;
  bool signed_number =
      (start[0] == '+' || start[0] == '-') && isdigit((unsigned char)start[1]);
  if (isdigit((unsigned char)*start) || signed_number) {
    if (*start == '+' || *start == '-')
      e->p++;
    while (isalnum((unsigned char)*e->p) || *e->p == 'x' || *e->p == 'X' ||
           *e->p == '\'' || *e->p == '_')
      e->p++;
    char buf[128];
    size_t n = (size_t)(e->p - start);
    if (n == 0 || n >= sizeof(buf))
      return false;
    memcpy(buf, start, n);
    buf[n] = '\0';
    return ny_ffi_parse_integer_literal(buf, out);
  }

  if (*start == '+' || *start == '-')
    e->p++;

  if (isalpha((unsigned char)*e->p) || *e->p == '_') {
    start = e->p;
    while (isalnum((unsigned char)*e->p) || *e->p == '_')
      e->p++;
    char name[128];
    size_t n = (size_t)(e->p - start);
    if (n == 0 || n >= sizeof(name) || !e->ctx || !e->ctx->cg)
      return false;
    memcpy(name, start, n);
    name[n] = '\0';
    binding *b = lookup_global_exact(e->ctx->cg, name);
    if (!b || !b->raw_int_value || !(b->is_int_slot || b->is_int_direct) ||
        !LLVMIsAConstantInt(b->raw_int_value))
      return false;
    *out = LLVMConstIntGetSExtValue(b->raw_int_value);
    return true;
  }
  return false;
}

static bool ny_ffi_expr_unary(ny_ffi_int_expr_t *e, int64_t *out) {
  if (ny_ffi_expr_accept(e, "+"))
    return ny_ffi_expr_unary(e, out);
  if (ny_ffi_expr_accept(e, "-")) {
    int64_t v = 0;
    if (!ny_ffi_expr_unary(e, &v))
      return false;
    *out = -v;
    return true;
  }
  if (ny_ffi_expr_accept(e, "~")) {
    int64_t v = 0;
    if (!ny_ffi_expr_unary(e, &v))
      return false;
    *out = ~v;
    return true;
  }
  return ny_ffi_expr_primary(e, out);
}

static bool ny_ffi_expr_mul(ny_ffi_int_expr_t *e, int64_t *out) {
  int64_t v = 0;
  if (!ny_ffi_expr_unary(e, &v))
    return false;
  for (;;) {
    if (ny_ffi_expr_accept(e, "*")) {
      int64_t r = 0;
      if (!ny_ffi_expr_unary(e, &r))
        return false;
      v *= r;
    } else if (ny_ffi_expr_accept(e, "/")) {
      int64_t r = 0;
      if (!ny_ffi_expr_unary(e, &r) || r == 0)
        return false;
      v /= r;
    } else if (ny_ffi_expr_accept(e, "%")) {
      int64_t r = 0;
      if (!ny_ffi_expr_unary(e, &r) || r == 0)
        return false;
      v %= r;
    } else {
      break;
    }
  }
  *out = v;
  return true;
}

static bool ny_ffi_expr_add(ny_ffi_int_expr_t *e, int64_t *out) {
  int64_t v = 0;
  if (!ny_ffi_expr_mul(e, &v))
    return false;
  for (;;) {
    if (ny_ffi_expr_accept(e, "+")) {
      int64_t r = 0;
      if (!ny_ffi_expr_mul(e, &r))
        return false;
      v += r;
    } else if (ny_ffi_expr_accept(e, "-")) {
      int64_t r = 0;
      if (!ny_ffi_expr_mul(e, &r))
        return false;
      v -= r;
    } else {
      break;
    }
  }
  *out = v;
  return true;
}

static bool ny_ffi_expr_shift(ny_ffi_int_expr_t *e, int64_t *out) {
  int64_t v = 0;
  if (!ny_ffi_expr_add(e, &v))
    return false;
  for (;;) {
    if (ny_ffi_expr_accept(e, "<<")) {
      int64_t r = 0;
      if (!ny_ffi_expr_add(e, &r) || r < 0 || r >= 63)
        return false;
      v <<= r;
    } else if (ny_ffi_expr_accept(e, ">>")) {
      int64_t r = 0;
      if (!ny_ffi_expr_add(e, &r) || r < 0 || r >= 63)
        return false;
      v >>= r;
    } else {
      break;
    }
  }
  *out = v;
  return true;
}

static bool ny_ffi_expr_and(ny_ffi_int_expr_t *e, int64_t *out) {
  int64_t v = 0;
  if (!ny_ffi_expr_shift(e, &v))
    return false;
  while (ny_ffi_expr_accept(e, "&")) {
    int64_t r = 0;
    if (!ny_ffi_expr_shift(e, &r))
      return false;
    v &= r;
  }
  *out = v;
  return true;
}

static bool ny_ffi_expr_xor(ny_ffi_int_expr_t *e, int64_t *out) {
  int64_t v = 0;
  if (!ny_ffi_expr_and(e, &v))
    return false;
  while (ny_ffi_expr_accept(e, "^")) {
    int64_t r = 0;
    if (!ny_ffi_expr_and(e, &r))
      return false;
    v ^= r;
  }
  *out = v;
  return true;
}

static bool ny_ffi_expr_number(ny_ffi_int_expr_t *e, int64_t *out) {
  int64_t v = 0;
  if (!ny_ffi_expr_xor(e, &v))
    return false;
  while (ny_ffi_expr_accept(e, "|")) {
    int64_t r = 0;
    if (!ny_ffi_expr_xor(e, &r))
      return false;
    v |= r;
  }
  *out = v;
  return true;
}

static bool ny_ffi_parse_integer_expr(ffi_context *ctx, const char *text,
                                      int64_t *out) {
  if (!text || !out)
    return false;
  ny_ffi_int_expr_t e = {.p = text, .ctx = ctx};
  int64_t v = 0;
  if (!ny_ffi_expr_number(&e, &v))
    return false;
  ny_ffi_expr_ws(&e);
  if (*e.p)
    return false;
  *out = v;
  return true;
}

static bool ny_ffi_macro_int_value(ffi_context *ctx, CXCursor cursor,
                                   int64_t *out) {
  if (!out)
    return false;
  if (clang_Cursor_isMacroFunctionLike(cursor))
    return false;

  CXEvalResult eval = clang_Cursor_Evaluate(cursor);
  if (eval) {
    CXEvalResultKind kind = clang_EvalResult_getKind(eval);
    if (kind == CXEval_Int) {
      *out = (int64_t)clang_EvalResult_getAsLongLong(eval);
      clang_EvalResult_dispose(eval);
      return true;
    }
    clang_EvalResult_dispose(eval);
  }

  CXTranslationUnit tu = clang_Cursor_getTranslationUnit(cursor);
  CXSourceRange range = clang_getCursorExtent(cursor);
  CXToken *tokens = NULL;
  unsigned count = 0;
  clang_tokenize(tu, range, &tokens, &count);
  if (!tokens || count < 2) {
    clang_disposeTokens(tu, tokens, count);
    return false;
  }

  char expr[128];
  size_t len = 0;
  for (unsigned i = 1; i < count && len + 2 < sizeof(expr); ++i) {
    CXString tok_cx = clang_getTokenSpelling(tu, tokens[i]);
    const char *tok = clang_getCString(tok_cx);
    if (!tok) {
      clang_disposeString(tok_cx);
      continue;
    }
    if (i == 1 && strcmp(tok, "(") == 0) {
      clang_disposeString(tok_cx);
      continue;
    }
    if (i + 1 == count && strcmp(tok, ")") == 0) {
      clang_disposeString(tok_cx);
      continue;
    }
    size_t add = strlen(tok);
    if (len + add + 1 >= sizeof(expr)) {
      clang_disposeString(tok_cx);
      break;
    }
    memcpy(expr + len, tok, add);
    len += add;
    clang_disposeString(tok_cx);
  }
  expr[len] = '\0';
  bool ok = ny_ffi_parse_integer_expr(ctx, expr, out);
  clang_disposeTokens(tu, tokens, count);
  return ok;
}

typedef struct {
  ffi_context *ctx;
  layout_def_t *def;
  CXType record_type;
  LLVMTypeRef *element_types;
  size_t len;
  size_t cap;
} ny_ffi_layout_builder_t;

static bool ny_ffi_register_layout_type(ffi_context *ctx, const char *name,
                                        CXType type);

static void ny_ffi_layout_push_element(ny_ffi_layout_builder_t *b,
                                       LLVMTypeRef ty) {
  if (!b || !ty)
    return;
  if (b->len >= b->cap) {
    size_t nc = b->cap ? b->cap * 2 : 8;
    LLVMTypeRef *nn =
        (LLVMTypeRef *)realloc(b->element_types, nc * sizeof(*nn));
    if (!nn)
      return;
    b->element_types = nn;
    b->cap = nc;
  }
  b->element_types[b->len++] = ty;
}

static enum CXChildVisitResult
ny_ffi_layout_field_visitor(CXCursor cursor, CXCursor parent,
                            CXClientData client_data) {
  (void)parent;
  ny_ffi_layout_builder_t *b = (ny_ffi_layout_builder_t *)client_data;
  if (!b || !b->ctx || clang_getCursorKind(cursor) != CXCursor_FieldDecl)
    return CXChildVisit_Continue;

  CXString field_name_cx = clang_getCursorSpelling(cursor);
  const char *field_name = clang_getCString(field_name_cx);
  if (!field_name || !*field_name) {
    clang_disposeString(field_name_cx);
    return CXChildVisit_Continue;
  }

  CXType field_type = clang_getCursorType(cursor);
  char type_buf[128];
  const char *ny_type =
      map_clang_type_named(field_type, type_buf, sizeof(type_buf));
  if (ny_type == type_buf)
    (void)ny_ffi_register_layout_type(b->ctx, ny_type, field_type);

  token_t empty_tok = {0};
  type_layout_t tl = resolve_raw_layout(b->ctx->cg, ny_type, empty_tok);
  LLVMTypeRef llvm_ty =
      (tl.is_valid && tl.llvm_type) ? tl.llvm_type : b->ctx->cg->type_i64;
  ny_ffi_layout_push_element(b, llvm_ty);

  long long bit_off = clang_Type_getOffsetOf(b->record_type, field_name);
  long long size = clang_Type_getSizeOf(field_type);
  long long align = clang_Type_getAlignOf(field_type);
  layout_field_info_t info = {
      .name = ny_strdup(field_name),
      .type_name = ny_strdup(ny_type),
      .offset = bit_off >= 0 ? (size_t)(bit_off / 8) : 0,
      .size = size >= 0 ? (size_t)size : (tl.is_valid ? tl.size : 8),
      .align = align > 0 ? (size_t)align : (tl.is_valid ? tl.align : 1),
  };
  vec_push(&b->def->fields, info);
  clang_disposeString(field_name_cx);
  return CXChildVisit_Continue;
}

static bool ny_ffi_register_layout_type(ffi_context *ctx, const char *name,
                                        CXType type) {
  if (!ctx || !ctx->cg || !name || !*name || lookup_layout(ctx->cg, name))
    return false;

  CXType canon = clang_getCanonicalType(type);
  if (canon.kind != CXType_Record)
    return false;
  long long c_size = clang_Type_getSizeOf(canon);
  long long c_align = clang_Type_getAlignOf(canon);
  if (c_size < 0 || c_align <= 0)
    return false;

  CXCursor decl = clang_getTypeDeclaration(canon);
  if (clang_Cursor_isNull(decl))
    return false;

  LLVMTypeRef st = LLVMStructCreateNamed(ctx->cg->ctx, name);
  layout_def_t *def = (layout_def_t *)calloc(1, sizeof(*def));
  if (!def)
    return false;
  def->name = ny_strdup(name);
  def->llvm_type = st;
  def->is_layout = true;
  def->heap_allocated = true;
  def->size = (size_t)c_size;
  def->align = (size_t)c_align;
  vec_push(&ctx->cg->layouts, def);

  ny_ffi_layout_builder_t builder = {
      .ctx = ctx,
      .def = def,
      .record_type = canon,
  };
  clang_visitChildren(decl, ny_ffi_layout_field_visitor, &builder);
  LLVMStructSetBody(st, builder.element_types, (unsigned)builder.len, false);
  free(builder.element_types);
  return true;
}

static void ny_ffi_register_layout_cursor(ffi_context *ctx, CXCursor cursor) {
  if (!ctx || !ny_ffi_cursor_in_requested_header(ctx, cursor))
    return;
  CXType type = clang_getCursorType(cursor);
  if (clang_getCursorKind(cursor) == CXCursor_TypedefDecl)
    type = clang_getTypedefDeclUnderlyingType(cursor);
  char name_buf[128];
  CXString cursor_name_cx = clang_getCursorSpelling(cursor);
  const char *cursor_name = clang_getCString(cursor_name_cx);
  bool have_name =
      ny_ffi_clean_record_name(cursor_name, name_buf, sizeof(name_buf));
  clang_disposeString(cursor_name_cx);
  if (!have_name && !ny_ffi_record_type_name(type, name_buf, sizeof(name_buf)))
    return;
  (void)ny_ffi_register_layout_type(ctx, name_buf, type);
}

static bool ny_ffi_layout_uses_sret(layout_def_t *layout) {
  if (!layout)
    return false;
#ifdef _WIN32
  return layout->size > 8;
#else
  return layout->size > 16;
#endif
}

static bool ny_ffi_layout_uses_byval(layout_def_t *layout) {
  return ny_ffi_layout_uses_sret(layout);
}

static void ny_ffi_add_type_attr(ffi_context *ctx, LLVMValueRef fn,
                                 LLVMAttributeIndex idx, const char *name,
                                 LLVMTypeRef type) {
  if (!ctx || !ctx->cg || !fn || !name || !*name || !type)
    return;
  unsigned kind = LLVMGetEnumAttributeKindForName(name, strlen(name));
  if (!kind)
    return;
  LLVMAttributeRef attr = LLVMCreateTypeAttribute(ctx->cg->ctx, kind, type);
  LLVMAddAttributeAtIndex(fn, idx, attr);
}

static void ny_ffi_add_align_attr(ffi_context *ctx, LLVMValueRef fn,
                                  LLVMAttributeIndex idx, size_t align) {
  if (!ctx || !ctx->cg || !fn || align == 0)
    return;
  unsigned kind = LLVMGetEnumAttributeKindForName("align", 5);
  if (!kind)
    return;
  LLVMAttributeRef attr =
      LLVMCreateEnumAttribute(ctx->cg->ctx, kind, (uint64_t)align);
  LLVMAddAttributeAtIndex(fn, idx, attr);
}

static enum CXChildVisitResult ffi_visitor(CXCursor cursor, CXCursor parent,
                                           CXClientData client_data) {
  (void)parent;
  ffi_context *ctx = (ffi_context *)client_data;
  enum CXCursorKind kind = clang_getCursorKind(cursor);

  CXSourceLocation loc = clang_getCursorLocation(cursor);
  CXFile cx_file;
  clang_getSpellingLocation(loc, &cx_file, NULL, NULL, NULL);
  if (!cx_file)
    return CXChildVisit_Continue;

  if (ctx->pass == 0) {
    if (kind == CXCursor_TypedefDecl || kind == CXCursor_StructDecl) {
      ny_ffi_register_layout_cursor(ctx, cursor);
    } else if (kind == CXCursor_EnumConstantDecl) {
      if (ny_ffi_cursor_in_requested_header(ctx, cursor)) {
        CXString name_cx = clang_getCursorSpelling(cursor);
        const char *name = clang_getCString(name_cx);
        ny_ffi_register_int_constant(
            ctx, name, (int64_t)clang_getEnumConstantDeclValue(cursor));
        clang_disposeString(name_cx);
      }
    } else if (kind == CXCursor_MacroDefinition) {
      if (ny_ffi_cursor_in_requested_header(ctx, cursor)) {
        CXString name_cx = clang_getCursorSpelling(cursor);
        const char *name = clang_getCString(name_cx);
        int64_t value = 0;
        if (ny_ffi_macro_int_value(ctx, cursor, &value))
          ny_ffi_register_int_constant(ctx, name, value);
        clang_disposeString(name_cx);
      }
    }
    return kind == CXCursor_EnumDecl ? CXChildVisit_Recurse
                                     : CXChildVisit_Continue;
  }

  if (kind == CXCursor_FunctionDecl) {
    if ((!ctx->prefix || ctx->prefix_len == 0) &&
        !ny_ffi_cursor_in_requested_header(ctx, cursor))
      return CXChildVisit_Continue;

    CXString name_cx = clang_getCursorSpelling(cursor);
    const char *c_name = clang_getCString(name_cx);
    if (verbose_enabled >= 3 && c_name)
      fprintf(stderr, "[ffi:import] symbol=%s\n", c_name);

    static const char *const ffi_blacklist[] = {
        "malloc",   "calloc",    "realloc",  "free",    "aligned_alloc",
        "memcpy",   "memmove",   "memset",   "memcmp",  "memchr",
        "strlen",   "strcpy",    "strncpy",  "strcmp",  "strncmp",
        "strcat",   "strncat",   "strchr",   "strrchr", "printf",
        "fprintf",  "sprintf",   "snprintf", "vprintf", "vfprintf",
        "vsprintf", "vsnprintf", "dlopen",   "dlsym",   "dlclose",
        "dlerror",  "dlvsym",    "abort",    "exit",    "atexit",
        NULL};
    if (c_name) {
      for (int bi = 0; ffi_blacklist[bi]; bi++) {
        if (strcmp(c_name, ffi_blacklist[bi]) == 0) {
          clang_disposeString(name_cx);
          return CXChildVisit_Continue;
        }
      }
    }

    if (c_name && *c_name && *c_name != '_') {
      if (ctx->prefix && ctx->prefix_len > 0 && !ctx->namespace_alias) {
        if (strncmp(c_name, ctx->prefix, ctx->prefix_len) != 0) {
          clang_disposeString(name_cx);
          return CXChildVisit_Continue;
        }
      }
      char public_name_buf[512];
      const char *ny_name =
          ny_ffi_public_symbol_name(ctx, c_name, public_name_buf,
                                    sizeof(public_name_buf));
      fun_sig *existing_sig = lookup_fun_exact(ctx->cg, ny_name);
      if (existing_sig && existing_sig->is_native_abi) {
        clang_disposeString(name_cx);
        return CXChildVisit_Continue;
      }
      if (!ny_ffi_import_allowed(ctx, ny_name, "function")) {
        clang_disposeString(name_cx);
        return CXChildVisit_Continue;
      }

      CXType ret_cxtype = clang_getCursorResultType(cursor);
      char ret_buf[128];
      bool ret_is_cstr = ny_ffi_type_is_char_pointer(ret_cxtype);
      const char *ny_ret =
          ret_is_cstr ? "str" : map_clang_type_named(ret_cxtype, ret_buf, sizeof(ret_buf));
      if (strcmp(ny_ret, "i32") == 0 &&
          (ny_ffi_function_return_spelling_is_bool(cursor) ||
           ny_ffi_function_return_tokens_bool(cursor, c_name) ||
           ny_ffi_function_return_line_bool(cursor, c_name)))
        ny_ret = "u8";
      const char *abi_ret = ret_is_cstr ? "cstr" : ny_ret;

      int num_params = clang_Cursor_getNumArguments(cursor);
      const char **param_ny_types = NULL;
      char (*param_bufs)[128] = NULL;
      LLVMTypeRef *param_llvm_types = NULL;
      layout_def_t **param_byval_layouts = NULL;
      layout_def_t *ret_layout = lookup_layout(ctx->cg, abi_ret);
      bool native_sret_return = ny_ffi_layout_uses_sret(ret_layout);
      int actual_params = num_params + (native_sret_return ? 1 : 0);
      if (actual_params > 0) {
        param_llvm_types =
            (LLVMTypeRef *)alloca(sizeof(LLVMTypeRef) * (size_t)actual_params);
        if (native_sret_return)
          param_llvm_types[0] = ctx->cg->type_i8ptr;
      }
      if (num_params > 0) {
        param_ny_types =
            (const char **)alloca(sizeof(const char *) * (size_t)num_params);
        param_byval_layouts = (layout_def_t **)alloca(sizeof(layout_def_t *) *
                                                      (size_t)num_params);
        param_bufs =
            (char (*)[128])alloca(sizeof(char[128]) * (size_t)num_params);
        for (int i = 0; i < num_params; i++) {
          param_byval_layouts[i] = NULL;
          CXCursor arg = clang_Cursor_getArgument(cursor, (unsigned)i);
          CXType arg_cxtype = clang_getCursorType(arg);
          token_t empty_tok = {0};
          const char *ny_arg_type = map_clang_type_named(
              arg_cxtype, param_bufs[i], sizeof(param_bufs[i]));
          layout_def_t *arg_layout = lookup_layout(ctx->cg, ny_arg_type);
          if (ny_ffi_layout_uses_byval(arg_layout)) {
            param_ny_types[i] = "ptr";
            param_byval_layouts[i] = arg_layout;
          } else {
            param_ny_types[i] = ny_arg_type;
          }
          param_llvm_types[i + (native_sret_return ? 1 : 0)] =
              resolve_abi_type_name(ctx->cg, ny_arg_type, empty_tok);
        }
      }

      token_t empty_tok = {0};
      LLVMTypeRef llvm_ret =
          native_sret_return
              ? LLVMVoidTypeInContext(ctx->cg->ctx)
              : resolve_abi_type_name(ctx->cg, abi_ret, empty_tok);
      CXType fn_cxtype = clang_getCursorType(cursor);
      int is_variadic = clang_isFunctionTypeVariadic(fn_cxtype);
      LLVMTypeRef ft = LLVMFunctionType(llvm_ret, param_llvm_types,
                                        (unsigned)actual_params, is_variadic);

      LLVMValueRef f = LLVMGetNamedFunction(ctx->cg->module, c_name);
      if (!f) {
        f = LLVMAddFunction(ctx->cg->module, c_name, ft);
        LLVMSetLinkage(f, LLVMExternalLinkage);
      }
      if (native_sret_return && ret_layout) {
        ny_ffi_add_type_attr(ctx, f, 1, "sret", ret_layout->llvm_type);
        ny_ffi_add_align_attr(ctx, f, 1, ret_layout->align);
      }
      for (int i = 0; i < num_params; i++) {
        layout_def_t *arg_layout =
            param_byval_layouts ? param_byval_layouts[i] : NULL;
        if (!arg_layout)
          continue;
        LLVMAttributeIndex idx =
            (LLVMAttributeIndex)(i + 1 + (native_sret_return ? 1 : 0));
        ny_ffi_add_type_attr(ctx, f, idx, "byval", arg_layout->llvm_type);
        ny_ffi_add_align_attr(ctx, f, idx, arg_layout->align);
      }

      bool already_known = false;
      bool existing_is_ny_fn = false;
      for (int si = 0; si < (int)ctx->cg->fun_sigs.len; si++) {
        fun_sig *existing = &((fun_sig *)ctx->cg->fun_sigs.data)[si];
        if (existing->name && strcmp(existing->name, ny_name) == 0) {
          already_known = true;
          existing_is_ny_fn = existing->stmt_t != NULL;
          break;
        }
      }
      if (!already_known || existing_is_ny_fn) {
        fun_sig sig;
        ny_fun_sig_init(&sig, ny_name, ft, f, NULL, num_params,
                        (bool)is_variadic, true);
        sig.link_name = ny_strdup(c_name);
        sig.is_native_abi = true;
        sig.native_sret_return = native_sret_return;
        sig.return_type = ny_strdup(ny_ret);
        if (strcmp(abi_ret, ny_ret) != 0)
          sig.abi_return_type = ny_strdup(abi_ret);
        for (int i = 0; i < num_params; i++) {
          vec_push(&sig.param_types, ny_strdup(param_ny_types[i]));
          vec_push(&sig.native_byval_param_layouts,
                   param_byval_layouts[i] && ny_ffi_layout_uses_byval(param_byval_layouts[i])
                       ? ny_strdup(param_byval_layouts[i]->name)
                       : NULL);
        }
        vec_push(&ctx->cg->fun_sigs, sig);
      }
    }
    clang_disposeString(name_cx);
  } else if (kind == CXCursor_EnumConstantDecl) {
    if (ny_ffi_cursor_in_requested_header(ctx, cursor)) {
      CXString name_cx = clang_getCursorSpelling(cursor);
      const char *name = clang_getCString(name_cx);
      ny_ffi_register_int_constant(
          ctx, name, (int64_t)clang_getEnumConstantDeclValue(cursor));
      clang_disposeString(name_cx);
    }
  } else if (kind == CXCursor_MacroDefinition) {
    if (ny_ffi_cursor_in_requested_header(ctx, cursor)) {
      CXString name_cx = clang_getCursorSpelling(cursor);
      const char *name = clang_getCString(name_cx);
      int64_t value = 0;
      if (ny_ffi_macro_int_value(ctx, cursor, &value))
        ny_ffi_register_int_constant(ctx, name, value);
      clang_disposeString(name_cx);
    }
  }

  return kind == CXCursor_EnumDecl ? CXChildVisit_Recurse
                                   : CXChildVisit_Continue;
}

void ny_ffi_clang_import(codegen_t *cg, const char *header_path,
                         const char *prefix, bool is_std, const char *lib) {
  if (!cg || !header_path || !*header_path) {
    token_t fake = {0};
    ny_diag_error(fake, "null or empty header path for FFI import");
    ny_diag_hint("provide a valid path like \"<alsa/asoundlib.h>\" or "
                 "\"/usr/include/foo.h\"");
    return;
  }

  if (!prefix || !*prefix) {
    const ny_autolink_entry_t *entry = ny_ffi_header_entry(header_path);
    if (entry && entry->prefix)
      prefix = entry->prefix;
  }

  if (cg->c_frontend && (strcmp(cg->c_frontend, "nytrix") == 0 ||
                         strcmp(cg->c_frontend, "auto") == 0)) {
    bool strict_internal = strcmp(cg->c_frontend, "nytrix") == 0;
    size_t parsed = 0;
    ny_c_header_summary_t summary = {0};
    if (!ny_ffi_internal_c_parse_header(cg, header_path, is_std,
                                        strict_internal, &parsed, &summary))
      return;
    if (parsed || strict_internal) {
      NY_LOG_V1("[ffi:c] internal frontend parsed %zu declaration(s)%s\n",
                parsed, strict_internal
                            ? "; attempting internal import lowering first"
                            : "; libclang remains fallback/import backend");
    }
    if (strict_internal && !is_std) {
      char *internal_auto_lib = NULL;
      const char *internal_lib = lib;
      if (!internal_lib || !*internal_lib) {
        internal_auto_lib = ny_autolink_resolve(header_path);
        internal_lib = internal_auto_lib;
      }
      size_t lowered = 0;
      size_t constants = 0;
      bool lowered_ok = ny_ffi_internal_c_lower_header(
          cg, header_path, prefix, internal_lib, &lowered, &constants);
      free(internal_auto_lib);
      bool simple_function_only =
          summary.declarations > 0 && summary.functions == summary.declarations &&
          summary.variables == 0 && summary.typedefs == 0 &&
          summary.tag_decls == 0 && summary.aggregate_layouts == 0 &&
          summary.unsupported == 0;
      bool internally_complete =
          lowered_ok && summary.variables == 0 && summary.unsupported == 0 &&
          ((summary.functions > 0 && lowered == summary.functions) ||
           (summary.functions == 0 && summary.declarations == 0 &&
            summary.object_like_define_lines > 0 && constants > 0));
      if (internally_complete) {
        NY_LOG_V1("[ffi:c] internal frontend completed import lowering for %s; libclang skipped\n",
                  header_path);
        return;
      }
      if (simple_function_only) {
        NY_LOG_ERR("[ffi:c] internal frontend could not lower simple function-only header \"%s\"\n",
                   header_path);
        cg->had_error = 1;
        return;
      }
      NY_LOG_V1("[ffi:c] internal frontend import lowering incomplete for %s; libclang fallback remains active\n",
                header_path);
    }
  }

  char *auto_lib = NULL;
  const char *resolved_lib = lib;
  if (!resolved_lib || !*resolved_lib) {
    auto_lib = ny_autolink_resolve(header_path);
    resolved_lib = auto_lib;
  }

  if (verbose_enabled >= 3) {
    fprintf(stderr, "[ffi:clang] #include: path=%s prefix=%s lib=%s%s\n",
            header_path, prefix ? prefix : "(none)",
            resolved_lib ? resolved_lib : "(none)", auto_lib ? " [auto]" : "");
  }

  if (resolved_lib && *resolved_lib) {
#ifndef _WIN32
    void *handle = dlopen(resolved_lib, RTLD_LAZY | RTLD_GLOBAL);
    if (!handle && verbose_enabled >= 1) {
      token_t fake = {0};
      ny_diag_warning(fake, "dlopen('%s') failed: %s", resolved_lib, dlerror());
      ny_diag_hint("the library may still be unavailable at runtime");
    }
#endif

    bool found = false;
    for (size_t i = 0; i < cg->links.len; i++) {
      if (strcmp(cg->links.data[i], resolved_lib) == 0) {
        found = true;
        break;
      }
    }
    if (!found)
      vec_push(&cg->links, ny_strdup(resolved_lib));
  }

  free(auto_lib);

  static const char *const sys_include_dirs[] = {
      ".",
      "src",
      "src/rt",
      "/usr/include",
      "/usr/local/include",
      "/opt/homebrew/include",
      "/opt/homebrew/opt/llvm@20/include",
      "/opt/homebrew/opt/llvm/include",
#ifdef __APPLE__
      "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include",
      "/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/"
      "Developer/SDKs/"
      "MacOSX.sdk/usr/include",
#endif
      "/usr/include/x86_64-linux-gnu",
      "/usr/include/aarch64-linux-gnu",
      NULL,
  };

  char resolved_path_buf[1024];
  char virtual_include_src[2048];
  const char *clang_file = header_path;
  struct CXUnsavedFile unsaved;
  struct CXUnsavedFile *unsaved_ptr = NULL;
  unsigned unsaved_count = 0;

  if (header_path[0] != '/') {
    if (is_std) {
      snprintf(virtual_include_src, sizeof(virtual_include_src),
               "#include <%s>\n", header_path);
      unsaved.Filename = "ffi_import.c";
      unsaved.Contents = virtual_include_src;
      unsaved.Length = (unsigned long)strlen(virtual_include_src);
      unsaved_ptr = &unsaved;
      unsaved_count = 1;
      clang_file = "ffi_import.c";
    } else {
      bool found = false;
      for (int d = 0; sys_include_dirs[d]; d++) {
        snprintf(resolved_path_buf, sizeof(resolved_path_buf), "%s/%s",
                 sys_include_dirs[d], header_path);
        if (ny_access(resolved_path_buf, R_OK) == 0) {
          clang_file = resolved_path_buf;
          found = true;
          break;
        }
      }
      if (!found) {
        if (verbose_enabled >= 1) {
          token_t fake = {0};
          ny_diag_warning(fake, "FFI header not found (optional dep): %s",
                          header_path);
          ny_diag_hint("install the dev package or ignore if the library is not needed");
        }
        return;
      }
    }
  }

  CXIndex index = clang_createIndex(0, 0);

  char **clang_owned_args = NULL;
  size_t clang_args_len = 0, clang_args_cap = 0;
  ny_ffi_append_default_clang_args(&clang_owned_args, &clang_args_len,
                                   &clang_args_cap);
  ny_pkgconfig_append_header_cflags(header_path, &clang_owned_args,
                                    &clang_args_len, &clang_args_cap);
  for (size_t i = 0; i < cg->ffi.defines.len; i++) {
    char *buf = (char *)malloc(strlen(cg->ffi.defines.data[i]) + 3);
    if (!buf)
      continue;
    snprintf(buf, strlen(cg->ffi.defines.data[i]) + 3, "-D%s",
             cg->ffi.defines.data[i]);
    ny_push_unique_owned_arg(&clang_owned_args, &clang_args_len,
                             &clang_args_cap, buf);
    free(buf);
  }
  int num_args = (int)clang_args_len;

  CXTranslationUnit tu = clang_parseTranslationUnit(
      index, clang_file, (const char *const *)clang_owned_args, num_args,
      unsaved_ptr, unsaved_count,
      CXTranslationUnit_SkipFunctionBodies |
          CXTranslationUnit_DetailedPreprocessingRecord);

  ny_ffi_free_clang_args(clang_owned_args, clang_args_len);

  if (!tu) {
    token_t fake = {0};
    ny_diag_error(fake, "failed to parse FFI header: %s", header_path);
    ny_diag_hint("ensure the header is valid C and has no syntax errors");
    clang_disposeIndex(index);
    return;
  }

  ffi_context ctx = {
      .cg = cg,
      .prefix = prefix,
      .header_path = header_path,
      .prefix_len = prefix ? strlen(prefix) : 0,
      .explicit_prefix = prefix && *prefix,
      .namespace_alias = ny_ffi_prefix_is_namespace_alias(prefix),
      .is_std_header = is_std,
  };

  CXCursor root = clang_getTranslationUnitCursor(tu);
  ctx.pass = 0;
  clang_visitChildren(root, ffi_visitor, &ctx);
  ctx.pass = 1;
  clang_visitChildren(root, ffi_visitor, &ctx);

  clang_disposeTranslationUnit(tu);
  clang_disposeIndex(index);
}

void ny_ffi_clang_define(codegen_t *cg, const char *macro) {
  if (!cg || !macro || !*macro)
    return;
  vec_push(&cg->ffi.defines, ny_strdup(macro));
}

void ny_ffi_clang_include(codegen_t *cg, const char *header_path,
                          const char *prefix, bool is_std, const char *lib) {
  if (!cg || !header_path || !*header_path)
    return;

  if (cg->ffi.includes_len >= cg->ffi.includes_cap) {
    cg->ffi.includes_cap = cg->ffi.includes_cap ? cg->ffi.includes_cap * 2 : 16;
    cg->ffi.includes = realloc(
        cg->ffi.includes, cg->ffi.includes_cap * sizeof(cg->ffi.includes[0]));
  }

  cg->ffi.includes[cg->ffi.includes_len].path = ny_strdup(header_path);
  cg->ffi.includes[cg->ffi.includes_len].prefix =
      prefix ? ny_strdup(prefix) : NULL;
  cg->ffi.includes[cg->ffi.includes_len].lib = lib ? ny_strdup(lib) : NULL;
  cg->ffi.includes[cg->ffi.includes_len].is_std = is_std;
  cg->ffi.includes_len++;
}

static bool ny_ffi_internal_c_frontend_probe(codegen_t *cg) {
  if (!cg || !cg->c_frontend)
    return true;
  if (strcmp(cg->c_frontend, "nytrix") != 0 &&
      strcmp(cg->c_frontend, "auto") != 0)
    return true;
  bool strict_internal = strcmp(cg->c_frontend, "nytrix") == 0;
  size_t parsed = 0;
  size_t skipped = 0;
  for (size_t i = 0; i < cg->ffi.includes_len; ++i) {
    if (cg->ffi.includes[i].is_std) {
      size_t parsed_one = 0;
      if (!ny_ffi_internal_c_parse_header(cg, cg->ffi.includes[i].path,
                                          cg->ffi.includes[i].is_std,
                                          strict_internal, &parsed_one, NULL))
        return false;
      parsed += parsed_one;
      skipped++;
      continue;
    }
    size_t parsed_one = 0;
    if (!ny_ffi_internal_c_parse_header(cg, cg->ffi.includes[i].path,
                                        cg->ffi.includes[i].is_std,
                                        strict_internal, &parsed_one, NULL))
      return false;
    parsed += parsed_one;
  }
  if (parsed || strict_internal) {
    NY_LOG_V1("[ffi:c] internal frontend parsed %zu declaration(s)%s\n",
              parsed, strict_internal
                          ? "; attempting internal import lowering first"
                          : "; libclang remains fallback/import backend");
  }
  (void)skipped;
  return true;
}

void ny_ffi_clang_process(codegen_t *cg) {
  if (!cg || cg->ffi.includes_len == 0)
    return;

  if (!ny_ffi_internal_c_frontend_probe(cg))
    return;

  char *buf = NULL;
  size_t len = 0;
  size_t cap = 0;

  for (size_t i = 0; i < cg->ffi.defines.len; i++) {
    const char *d = cg->ffi.defines.data[i];

    char line[1024];
    snprintf(line, sizeof(line), "#define %s\n", d);
    ffi_buf_append(&buf, &len, &cap, line);
  }

  for (size_t i = 0; i < cg->ffi.includes_len; i++) {
    const char *p = cg->ffi.includes[i].path;
    bool is_std = cg->ffi.includes[i].is_std;
    char line[1024];
    if (is_std)
      snprintf(line, sizeof(line), "#include <%s>\n", p);
    else
      snprintf(line, sizeof(line), "#include \"%s\"\n", p);
    ffi_buf_append(&buf, &len, &cap, line);

    const char *lib = cg->ffi.includes[i].lib;
    char *auto_lib = NULL;
    if (!lib || !*lib) {
      auto_lib = ny_autolink_resolve(p);
      lib = auto_lib;
    }
    if (lib && *lib) {
#ifndef _WIN32
      (void)dlopen(lib, RTLD_LAZY | RTLD_GLOBAL);
#endif
      bool found = false;
      for (size_t k = 0; k < cg->links.len; k++) {
        if (strcmp(cg->links.data[k], lib) == 0) {
          found = true;
          break;
        }
      }
      if (!found)
        vec_push(&cg->links, ny_strdup(lib));
    }
    free(auto_lib);
  }

  CXIndex index = clang_createIndex(0, 0);
  char **clang_args = NULL;
  size_t clang_args_len = 0, clang_args_cap = 0;
  ny_ffi_append_default_clang_args(&clang_args, &clang_args_len,
                                   &clang_args_cap);
  for (size_t i = 0; i < cg->ffi.includes_len; i++)
    ny_pkgconfig_append_header_cflags(cg->ffi.includes[i].path, &clang_args,
                                      &clang_args_len, &clang_args_cap);
  int num_args = (int)clang_args_len;

  struct CXUnsavedFile unsaved = {
      .Filename = "ffi_session.c",
      .Contents = buf,
      .Length = (unsigned long)len,
  };

  CXTranslationUnit tu = clang_parseTranslationUnit(
      index, "ffi_session.c", (const char *const *)clang_args, num_args,
      &unsaved, 1,
      CXTranslationUnit_SkipFunctionBodies |
          CXTranslationUnit_DetailedPreprocessingRecord);
  ny_ffi_free_clang_args(clang_args, clang_args_len);

  if (!tu) {
    ny_diag_error((token_t){0}, "failed to parse FFI session block");
    free(buf);
    clang_disposeIndex(index);
    return;
  }

  for (size_t i = 0; i < cg->ffi.includes_len; i++) {
    const char *prefix = cg->ffi.includes[i].prefix;
    const char *path = cg->ffi.includes[i].path;

    if (!prefix || !*prefix) {
      const ny_autolink_entry_t *entry = ny_ffi_header_entry(path);
      if (entry && entry->prefix)
        prefix = entry->prefix;
    }

    ffi_context ctx = {
        .cg = cg,
        .prefix = prefix,
        .header_path = path,
        .prefix_len = prefix ? strlen(prefix) : 0,
        .explicit_prefix = prefix && *prefix,
        .namespace_alias = ny_ffi_prefix_is_namespace_alias(prefix),
        .is_std_header = cg->ffi.includes[i].is_std,
    };
    CXCursor root = clang_getTranslationUnitCursor(tu);
    ctx.pass = 0;
    clang_visitChildren(root, ffi_visitor, &ctx);
    ctx.pass = 1;
    clang_visitChildren(root, ffi_visitor, &ctx);
  }

  clang_disposeTranslationUnit(tu);
  clang_disposeIndex(index);
  free(buf);

  for (size_t i = 0; i < cg->ffi.defines.len; i++)
    free(cg->ffi.defines.data[i]);
  cg->ffi.defines.len = 0;
  for (size_t i = 0; i < cg->ffi.includes_len; i++) {
    free((void *)cg->ffi.includes[i].path);
    free((void *)cg->ffi.includes[i].prefix);
    free((void *)cg->ffi.includes[i].lib);
  }
  cg->ffi.includes_len = 0;
}
