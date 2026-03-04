#include "fficlang.h"
#include "base/util.h"
#include "priv.h"
#include <clang-c/Index.h>
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

/*
   Auto-link heuristic table
   Maps a header path prefix/name to one or more SO names to load.
   This covers everything used in lib/ plus common system libraries.
   ---------------------------------------------------------------------------
 */
typedef struct {
  const char *header_prefix; /* matched against the start of the header path */
  const char *pkg_config;    /* pkg-config package to query first (or NULL)   */
  const char *fallback_lib; /* dlopen name if pkg-config is unavailable       */
} ny_autolink_entry_t;

static const ny_autolink_entry_t ny_autolink_table[] = {
    /* Audio */
    {"alsa/", "alsa", "libasound.so.2"},
    {"sound/", "alsa", "libasound.so.2"},
    {"jack/", "jack", "libjack.so.0"},
    {"pulse/simple", "libpulse-simple", "libpulse-simple.so.0"},
    {"pulse/", "libpulse", "libpulse.so.0"},
    {"sndfile", "sndfile", "libsndfile.so.1"},
    /* Graphics / Windowing */
    {"vulkan/", "vulkan", "libvulkan.so.1"},
    {"vk_", "vulkan", "libvulkan.so.1"},
    {"X11/", "x11", "libX11.so.6"},
    {"xcb/", "xcb", "libxcb.so.1"},
    {"xrandr", "xrandr", "libXrandr.so.2"},
    {"Xi.", "xi", "libXi.so.6"},
    {"Xext", "xext", "libXext.so.6"},
    {"GL/gl", "gl", "libGL.so.1"},
    {"GL/glx", "glx", "libGL.so.1"},
    {"GLES2/", "glesv2", "libGLESv2.so.2"},
    {"GL/osmesa", "osmesa", "libOSMesa.so.8"},
    {"EGL/", "egl", "libEGL.so.1"},
    {"wayland-client", "wayland-client", "libwayland-client.so.0"},
    {"wayland-server", "wayland-server", "libwayland-server.so.0"},
    /* SDL / Raylib */
    {"SDL2/", "sdl2", "libSDL2-2.0.so.0"},
    {"SDL3/", "sdl3", "libSDL3.so.0"},
    {"raylib", "raylib", "libraylib.so"},
    /* Curl / Network */
    {"curl/", "libcurl", "libcurl.so.4"},
    /* Compression */
    {"zlib", "zlib", "libz.so.1"},
    {"lz4", "liblz4", "liblz4.so.1"},
    {"zstd", "libzstd", "libzstd.so.1"},
    /* Math */
    {"fftw3", "fftw3", "libfftw3.so.3"},
    /* Fonts */
    {"freetype2/", "freetype2", "libfreetype.so.6"},
    {"fontconfig/", "fontconfig", "libfontconfig.so.1"},
    /* Input */
    {"libinput", "libinput", "libinput.so.10"},
    {"evdev/", NULL, "libevdev.so.2"},
    /* End */
    {NULL, NULL, NULL},
};

/* Try pkg-config to get the .so name, returns a malloc'd string or NULL */
static char *ny_pkgconfig_lib(const char *pkg) {
  if (!pkg || !*pkg)
    return NULL;
  /* "pkg-config --libs-only-l <pkg>" returns e.g. "-lasound\n" */
  char cmd[256];
  snprintf(cmd, sizeof(cmd), "pkg-config --libs-only-l %s 2>/dev/null", pkg);
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
  /* strip trailing whitespace */
  size_t n = strlen(buf);
  while (n > 0 &&
         (buf[n - 1] == '\n' || buf[n - 1] == ' ' || buf[n - 1] == '\r'))
    buf[--n] = '\0';
  if (!buf[0])
    return NULL;
  /* convert "-lfoo -lbar" → first entry "libfoo.so" */
  const char *p = buf;
  while (*p == ' ')
    p++;
  if (p[0] == '-' && p[1] == 'l') {
    p += 2;
    /* find end of token */
    const char *end = p;
    while (*end && *end != ' ')
      end++;
    size_t len = (size_t)(end - p);
    /* build "libNAME.so" */
    char *result = malloc(len + 8);
    if (!result)
      return NULL;
    memcpy(result, "lib", 3);
    memcpy(result + 3, p, len);
    memcpy(result + 3 + len, ".so", 4); /* includes NUL */
    return result;
  }
  return NULL;
}

/* Resolve the shared library to dlopen for a given header path.
   Returns a malloc'd string (caller frees) or NULL if unknown. */
static char *ny_autolink_resolve(const char *header_path) {
  if (!header_path)
    return NULL;
  /* strip leading dir components to get the base/prefix */
  const char *base = strrchr(header_path, '/');
  base = base ? base + 1 : header_path;
  /* also grab the parent dir component, e.g. "SDL2" from "SDL2/SDL.h" */
  const char *slash = strchr(header_path, '/');
  char dir_prefix[128] = {0};
  if (slash) {
    size_t n = (size_t)(slash - header_path) + 1; /* include trailing '/' */
    if (n < sizeof(dir_prefix))
      memcpy(dir_prefix, header_path, n);
    dir_prefix[n] = '\0';
  }
  for (const ny_autolink_entry_t *e = ny_autolink_table; e->header_prefix;
       e++) {
    /* match against "dir/" or full base name */
    bool match =
        (strncmp(header_path, e->header_prefix, strlen(e->header_prefix)) ==
         0) ||
        (strncmp(base, e->header_prefix, strlen(e->header_prefix)) == 0);
    if (!match)
      continue;
    /* try pkg-config first */
    char *lib = ny_pkgconfig_lib(e->pkg_config);
    if (lib)
      return lib;
    /* fallback */
    if (e->fallback_lib)
      return strdup(e->fallback_lib);
    return NULL;
  }

  /* Dynamic fallback: emulate C compiler's default library heuristics
   * (-l<name>) */
  char file_stem[128] = {0};
  const char *dot = strrchr(base, '.');
  if (dot) {
    size_t n = (size_t)(dot - base);
    if (n < sizeof(file_stem)) {
      memcpy(file_stem, base, n);
      file_stem[n] = '\0';
    }
  } else {
    strncpy(file_stem, base, sizeof(file_stem) - 1);
  }

  char dir_stem[128] = {0};
  const char *first_slash = strchr(header_path, '/');
  if (first_slash) {
    size_t n = (size_t)(first_slash - header_path);
    if (n < sizeof(dir_stem)) {
      memcpy(dir_stem, header_path, n);
      dir_stem[n] = '\0';
    }
  }

  /* Helper to convert to lowercase */
  char dir_lower[128] = {0};
  char file_lower[128] = {0};
  size_t i = 0;
  for (; dir_stem[i] && i < 127; i++) {
    char c = dir_stem[i];
    dir_lower[i] = (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
  }
  dir_lower[i] = '\0';

  i = 0;
  for (; file_stem[i] && i < 127; i++) {
    char c = file_stem[i];
    file_lower[i] = (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
  }
  file_lower[i] = '\0';

  char *lib = NULL;

  /* 1. pkg-config queries */
  if (dir_stem[0]) {
    lib = ny_pkgconfig_lib(dir_stem);
    if (lib)
      return lib;
    if (strcmp(dir_stem, dir_lower) != 0) {
      lib = ny_pkgconfig_lib(dir_lower);
      if (lib)
        return lib;
    }
  }
  if (file_stem[0]) {
    lib = ny_pkgconfig_lib(file_stem);
    if (lib)
      return lib;
    if (strcmp(file_stem, file_lower) != 0) {
      lib = ny_pkgconfig_lib(file_lower);
      if (lib)
        return lib;
    }
  }

  /* 2. OS direct load (test if exists) */
#ifndef _WIN32
  char fallback[256];
  void *test = NULL;

  if (file_stem[0]) {
    snprintf(fallback, sizeof(fallback), "lib%s.so", file_stem);
    test = dlopen(fallback, RTLD_LAZY | RTLD_LOCAL);
    if (test) {
      dlclose(test);
      return strdup(fallback);
    }
  }

  if (file_lower[0]) {
    snprintf(fallback, sizeof(fallback), "lib%s.so", file_lower);
    test = dlopen(fallback, RTLD_LAZY | RTLD_LOCAL);
    if (test) {
      dlclose(test);
      return strdup(fallback);
    }
  }

  if (dir_stem[0]) {
    snprintf(fallback, sizeof(fallback), "lib%s.so", dir_stem);
    test = dlopen(fallback, RTLD_LAZY | RTLD_LOCAL);
    if (test) {
      dlclose(test);
      return strdup(fallback);
    }
  }

  if (dir_lower[0]) {
    snprintf(fallback, sizeof(fallback), "lib%s.so", dir_lower);
    test = dlopen(fallback, RTLD_LAZY | RTLD_LOCAL);
    if (test) {
      dlclose(test);
      return strdup(fallback);
    }
  }
#endif

  return NULL;
}

/*
   Type mapping
   ---------------------------------------------------------------------------
 */
static const char *map_clang_type(CXType type) {
  switch (type.kind) {
  case CXType_Void:
    return "void";
  case CXType_Bool:
    return "bool";
  case CXType_Char_S:
  case CXType_SChar:
  case CXType_Char_U:
  case CXType_UChar:
    return "i8";
  case CXType_Short:
  case CXType_UShort:
    return "i16";
  case CXType_Int:
  case CXType_UInt:
    return "i32";
  case CXType_Long:
  case CXType_ULong:
  case CXType_LongLong:
  case CXType_ULongLong:
    return "i64";
  case CXType_Float:
    return "f32";
  case CXType_Double:
    return "f64";
  case CXType_Pointer:
    return "ptr";
  case CXType_Enum:
    /* C enums always have underlying type 'int' (i32) on all platforms */
    return "i32";
  case CXType_Typedef: {
    /* Resolve typedef to its canonical type */
    CXType canon = clang_getCanonicalType(type);
    if (canon.kind != CXType_Typedef)
      return map_clang_type(canon);
    return "i64";
  }
  case CXType_Record:
  case CXType_ConstantArray:
  case CXType_IncompleteArray:
    /* Struct/array by value — pass as opaque pointer; caller must handle */
    return "ptr";
  default:
    return "i64";
  }
}

/*
   libclang AST visitor — registers each function decl as an extern fun_sig
   ---------------------------------------------------------------------------
 */
typedef struct {
  codegen_t *cg;
  const char *prefix;
  const char *header_path;
  size_t prefix_len;
} ffi_context;

static enum CXChildVisitResult ffi_visitor(CXCursor cursor, CXCursor parent,
                                           CXClientData client_data) {
  (void)parent;
  ffi_context *ctx = (ffi_context *)client_data;
  enum CXCursorKind kind = clang_getCursorKind(cursor);

  /* Accept declarations from the header and any file it transitively includes,
     as long as the source file lives inside a standard include directory.
     This handles umbrella headers like <alsa/asoundlib.h> that just pull in
     sub-headers where the real function decls live.
     We reject clang built-in virtual files (no real path).              */
  CXSourceLocation loc = clang_getCursorLocation(cursor);
  CXFile cx_file;
  clang_getSpellingLocation(loc, &cx_file, NULL, NULL, NULL);
  if (!cx_file)
    return CXChildVisit_Continue;
  CXString cx_fname = clang_getFileName(cx_file);
  const char *fname = clang_getCString(cx_fname);
  bool in_sys = fname && (strncmp(fname, "/usr/include", 12) == 0 ||
                          strncmp(fname, "/usr/local/include", 18) == 0);
  clang_disposeString(cx_fname);
  if (!in_sys)
    return CXChildVisit_Continue;

  if (kind == CXCursor_FunctionDecl) {
    CXString name_cx = clang_getCursorSpelling(cursor);
    const char *c_name = clang_getCString(name_cx);

    /* Skip C stdlib symbols that conflict with Nytrix runtime builtins */
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

    /* Only import functions that match the prefix (if specified).
       This prevents importing hundreds of unrelated system functions.
       We keep the full C function name, just filter by prefix.          */
    if (c_name && *c_name && *c_name != '_') {
      if (ctx->prefix && ctx->prefix_len > 0) {
        if (strncmp(c_name, ctx->prefix, ctx->prefix_len) != 0) {
          clang_disposeString(name_cx);
          return CXChildVisit_Continue;
        }
      }
      const char *ny_name = c_name; /* Keep full name, don't strip prefix */

      CXType ret_cxtype = clang_getCursorResultType(cursor);
      const char *ny_ret = map_clang_type(ret_cxtype);

      int num_params = clang_Cursor_getNumArguments(cursor);
      LLVMTypeRef *param_llvm_types = NULL;
      if (num_params > 0) {
        param_llvm_types =
            (LLVMTypeRef *)alloca(sizeof(LLVMTypeRef) * (size_t)num_params);
        for (int i = 0; i < num_params; i++) {
          CXCursor arg = clang_Cursor_getArgument(cursor, (unsigned)i);
          CXType arg_cxtype = clang_getCursorType(arg);
          token_t empty_tok = {0};
          param_llvm_types[i] = resolve_abi_type_name(
              ctx->cg, map_clang_type(arg_cxtype), empty_tok);
        }
      }

      token_t empty_tok = {0};
      LLVMTypeRef llvm_ret = resolve_abi_type_name(ctx->cg, ny_ret, empty_tok);
      CXType fn_cxtype = clang_getCursorType(cursor);
      int is_variadic = clang_isFunctionTypeVariadic(fn_cxtype);
      LLVMTypeRef ft = LLVMFunctionType(llvm_ret, param_llvm_types,
                                        (unsigned)num_params, is_variadic);

      LLVMValueRef f = LLVMGetNamedFunction(ctx->cg->module, c_name);
      if (!f) {
        f = LLVMAddFunction(ctx->cg->module, c_name, ft);
        LLVMSetLinkage(f, LLVMExternalLinkage);
      }

      /* Dedup: if already registered (either from a prior extern fn or a
         previous #include of a different header), skip to avoid conflicts.  */
      bool already_known = false;
      for (int si = 0; si < (int)ctx->cg->fun_sigs.len; si++) {
        fun_sig *existing = &((fun_sig *)ctx->cg->fun_sigs.data)[si];
        if (existing->name && strcmp(existing->name, ny_name) == 0) {
          already_known = true;
          break;
        }
      }
      if (!already_known) {
        fun_sig sig;
        ny_fun_sig_init(&sig, ny_name, ft, f, NULL, num_params,
                        (bool)is_variadic, true);
        sig.link_name = ny_strdup(c_name);
        sig.is_native_abi = true;
        sig.return_type = ny_strdup(ny_ret);
        vec_push(&ctx->cg->fun_sigs, sig);
      }
    }
    clang_disposeString(name_cx);
  }

  return CXChildVisit_Continue;
}

/*
   Public entry point
   ---------------------------------------------------------------------------
 */
void ny_ffi_clang_import(codegen_t *cg, const char *header_path,
                         const char *prefix, bool is_std, const char *lib) {
  (void)is_std;

  if (!cg || !header_path || !*header_path) {
    fprintf(stderr, "[ffi:clang] ERROR: null or empty header path\n");
    return;
  }

  /* Auto-derive prefix from header path if not explicitly provided.
     This prevents importing hundreds of unrelated system functions. */
  char auto_prefix_buf[256] = {0};
  if (!prefix || !*prefix) {
    const char *base = strrchr(header_path, '/');
    base = base ? base + 1 : header_path;

    /* Common header prefixes - for libs with many functions */
    struct {
      const char *hdr;
      const char *pfx;
    } prefixes[] = {{"alsa/", "snd_"},   {"pulse/", "pa_"}, {"jack/", "jack_"},
                    {"sndfile", "sf_"},  {"vulkan/", "vk"}, {"wayland-", "wl_"},
                    {"freetype", "FT_"}, {NULL, NULL}};

    for (int i = 0; prefixes[i].hdr; i++) {
      if (strncmp(header_path, prefixes[i].hdr, strlen(prefixes[i].hdr)) == 0 ||
          strncmp(base, prefixes[i].hdr, strlen(prefixes[i].hdr)) == 0) {
        strncpy(auto_prefix_buf, prefixes[i].pfx, sizeof(auto_prefix_buf) - 1);
        prefix = auto_prefix_buf;
        break;
      }
    }
  }

  /* Auto-resolve the library to link if the caller didn't specify one */
  char *auto_lib = NULL;
  const char *resolved_lib = lib;
  if (!resolved_lib || !*resolved_lib) {
    auto_lib = ny_autolink_resolve(header_path);
    resolved_lib = auto_lib;
  }

  if (verbose_enabled >= 1) {
    fprintf(stderr, "[ffi:clang] extern #include: path=%s prefix=%s lib=%s%s\n",
            header_path, prefix ? prefix : "(none)",
            resolved_lib ? resolved_lib : "(none)", auto_lib ? " [auto]" : "");
  }

  /* dlopen the library so JIT can resolve the symbols (mirrors 'link') */
  if (resolved_lib && *resolved_lib) {
#ifndef _WIN32
    void *handle = dlopen(resolved_lib, RTLD_LAZY | RTLD_GLOBAL);
    if (!handle && verbose_enabled >= 1)
      fprintf(stderr, "[ffi:clang] warning: dlopen('%s') failed: %s\n",
              resolved_lib, dlerror());
#endif
    /* Also register in cg->links so the native JIT cache .so gets the
       library pre-loaded via the .libs sidecar file on cache replay. */
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

  /* --- Resolve header to an absolute path ---
     libclang's parseTranslationUnit needs a real file path, not just
     the include-style short form (e.g. "alsa/asoundlib.h").          */
  static const char *const sys_include_dirs[] = {
      "/usr/include",
      "/usr/local/include",
      "/usr/include/x86_64-linux-gnu",
      "/usr/include/aarch64-linux-gnu",
      NULL,
  };

  char resolved_path_buf[1024];
  const char *clang_file = header_path;

  if (header_path[0] != '/') {
    /* Try each system include dir until the file is found */
    bool found = false;
    for (int d = 0; sys_include_dirs[d]; d++) {
      snprintf(resolved_path_buf, sizeof(resolved_path_buf), "%s/%s",
               sys_include_dirs[d], header_path);
      if (access(resolved_path_buf, R_OK) == 0) {
        clang_file = resolved_path_buf;
        found = true;
        break;
      }
    }
    if (!found) {
      fprintf(stderr, "[ffi:clang] ERROR: header not found: %s\n", header_path);
      return;
    }
  }

  /* Parse the header with libclang */
  CXIndex index = clang_createIndex(0, 0);
  const char *clang_args[] = {"-I.", "-I/usr/include", "-I/usr/local/include",
                              "-D__NYTRIX_FFI_IMPORT__"};
  int num_args = (int)(sizeof(clang_args) / sizeof(clang_args[0]));

  CXTranslationUnit tu =
      clang_parseTranslationUnit(index, clang_file, clang_args, num_args, NULL,
                                 0, CXTranslationUnit_SkipFunctionBodies);

  if (!tu) {
    fprintf(stderr, "[ffi:clang] ERROR: failed to parse header: %s\n",
            header_path);
    clang_disposeIndex(index);
    return;
  }

  ffi_context ctx = {
      .cg = cg,
      .prefix = prefix,
      .header_path = header_path,
      .prefix_len = prefix ? strlen(prefix) : 0,
  };

  CXCursor root = clang_getTranslationUnitCursor(tu);
  clang_visitChildren(root, ffi_visitor, &ctx);

  clang_disposeTranslationUnit(tu);
  clang_disposeIndex(index);
}
