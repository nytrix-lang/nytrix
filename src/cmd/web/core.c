#include "../tools/tool.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/limits.h>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#else
#include <windows.h>
#include <process.h>
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static int ny_mkdir_compat(const char *path) {
#ifdef _WIN32
  return mkdir(path);
#else
  return mkdir(path, 0755);
#endif
}

static int mkdir_p(const char *path) {
  char tmp[PATH_MAX];
  snprintf(tmp, sizeof(tmp), "%s", path);
  for (char *p = tmp + 1; *p; p++) {
    if (*p != '/') continue;
    *p = '\0';
    ny_mkdir_compat(tmp);
    *p = '/';
  }
  return ny_mkdir_compat(tmp) == 0 || ny_access(tmp, F_OK) == 0;
}

static int write_file(const char *path, const char *txt) {
  FILE *f = fopen(path, "wb");
  if (!f)
    return 0;
  size_t n = strlen(txt);
  int ok = fwrite(txt, 1, n, f) == n;
  fclose(f);
  return ok;
}

static int copy_file_bytes(const char *src, const char *dst) {
  FILE *in = fopen(src, "rb");
  if (!in)
    return 0;
  FILE *out = fopen(dst, "wb");
  if (!out) {
    fclose(in);
    return 0;
  }
  char buf[8192];
  int ok = 1;
  for (;;) {
    size_t n = fread(buf, 1, sizeof(buf), in);
    if (n > 0 && fwrite(buf, 1, n, out) != n) {
      ok = 0;
      break;
    }
    if (n < sizeof(buf)) {
      if (ferror(in))
        ok = 0;
      break;
    }
  }
  if (fclose(out) != 0)
    ok = 0;
  fclose(in);
  return ok;
}

static int join_path(char *out, size_t out_n, const char *a, const char *b);

static int copy_regular_files_in_dir(const char *src_dir, const char *dst_dir,
                                     const char *skip_name) {
  DIR *d = opendir(src_dir);
  if (!d)
    return 0;
  if (!mkdir_p(dst_dir)) {
    closedir(d);
    return 0;
  }
  int ok = 1;
  struct dirent *ent;
  while ((ent = readdir(d))) {
    if (ent->d_name[0] == '.')
      continue;
    if (skip_name && strcmp(ent->d_name, skip_name) == 0)
      continue;
    char src[PATH_MAX], dst[PATH_MAX];
    if (!join_path(src, sizeof(src), src_dir, ent->d_name) ||
        !join_path(dst, sizeof(dst), dst_dir, ent->d_name)) {
      ok = 0;
      continue;
    }
    struct stat st;
    if (stat(src, &st) != 0)
      continue;
    if (S_ISREG(st.st_mode) && !copy_file_bytes(src, dst))
      ok = 0;
  }
  closedir(d);
  return ok;
}

static int join_path(char *out, size_t out_n, const char *a, const char *b) {
  size_t al = strlen(a), bl = strlen(b);
  int need_slash = al > 0 && a[al - 1] != '/';
  if (al + (size_t)need_slash + bl + 1 > out_n)
    return 0;
  memcpy(out, a, al);
  size_t p = al;
  if (need_slash)
    out[p++] = '/';
  memcpy(out + p, b, bl);
  out[p + bl] = '\0';
  return 1;
}

static int path_is_dir(const char *path) {
  struct stat st;
  return path && *path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int path_has_suffix(const char *path, const char *suffix) {
  return ny_has_suffix(path, suffix) ? 1 : 0;
}

static void abs_path_for_display(char *out, size_t out_n, const char *path) {
  if (!out || out_n == 0)
    return;
  out[0] = '\0';
  if (!path || !*path)
    return;
  if (path[0] == '/') {
    snprintf(out, out_n, "%s", path);
    return;
  }
  char cwd[PATH_MAX];
  if (getcwd(cwd, sizeof(cwd)) && join_path(out, out_n, cwd, path))
    return;
  snprintf(out, out_n, "%s", path);
}

static int has_web_assets(const char *root) {
  char probe[PATH_MAX];
  return join_path(probe, sizeof(probe), root, "etc/assets/website/web.html") &&
         ny_path_readable(probe);
}

static const char *find_cwd_share_root(void) {
  static char root[PATH_MAX];
  char cur[PATH_MAX];
  if (!getcwd(cur, sizeof(cur)))
    return NULL;
  for (;;) {
    if (has_web_assets(cur)) {
      snprintf(root, sizeof(root), "%s", cur);
      return root;
    }
    char *slash = strrchr(cur, '/');
    if (!slash || slash == cur)
      break;
    *slash = '\0';
  }
  return NULL;
}

static const char *nyt_web_executable_path(void) {
  static char path[PATH_MAX];
  if (path[0])
    return path;
#ifdef _WIN32
  DWORD len = GetModuleFileNameA(NULL, path, sizeof(path));
  if (len > 0 && len < sizeof(path)) {
    path[len] = '\0';
    return path;
  }
#elif defined(__APPLE__)
  uint32_t size = (uint32_t)sizeof(path);
  if (_NSGetExecutablePath(path, &size) == 0)
    return path;
#else
  ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
  if (len >= 0) {
    path[len] = '\0';
    return path;
  }
#endif
  return NULL;
}

static const char *find_nytrix_share_root(void) {
  static char root[PATH_MAX];
  if (root[0])
    return root;
  const char *envs[] = {getenv("NYTRIX_SHARE_ROOT"), getenv("NYTRIX_ROOT")};
  for (size_t i = 0; i < sizeof(envs) / sizeof(envs[0]); i++) {
    const char *env = envs[i];
    if (env && *env && has_web_assets(env)) {
      snprintf(root, sizeof(root), "%s", env);
      return root;
    }
  }
  const char *cwd_root = find_cwd_share_root();
  if (cwd_root && *cwd_root) {
    snprintf(root, sizeof(root), "%s", cwd_root);
    return root;
  }
  {
    const char *resolved_exe = nyt_web_executable_path();
    if (resolved_exe && *resolved_exe) {
      char exe_path[PATH_MAX];
      snprintf(exe_path, sizeof(exe_path), "%s", resolved_exe);
      char *slash = strrchr(exe_path, '/');
#ifdef _WIN32
      char *backslash = strrchr(exe_path, '\\');
      if (!slash || (backslash && backslash > slash))
        slash = backslash;
#endif
      if (slash) {
        *slash = '\0';
        const char *suffixes[] = {"../share/nytrix", "../../share/nytrix",
                                  "../.."};
        for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
          char cand[PATH_MAX];
          if (join_path(cand, sizeof(cand), exe_path, suffixes[i]) &&
              has_web_assets(cand)) {
            snprintf(root, sizeof(root), "%s", cand);
            return root;
          }
        }
      }
    }
  }
  const char *system_roots[] = {
      "/usr/share/nytrix",          "/usr/local/share/nytrix",
      "/opt/nytrix/share/nytrix",   "/opt/nytrix/share",
      "/opt/homebrew/share/nytrix",
  };
  for (size_t i = 0; i < sizeof(system_roots) / sizeof(system_roots[0]); i++) {
    if (!has_web_assets(system_roots[i])) continue;
    snprintf(root, sizeof(root), "%s", system_roots[i]);
    return root;
  }
  return NULL;
}

typedef struct {
  char *data;
  size_t len;
  size_t cap;
} sb_t;

static int sb_reserve(sb_t *sb, size_t need) {
  if (need <= sb->cap)
    return 1;
  size_t cap = sb->cap ? sb->cap : 4096;
  while (cap < need)
    cap *= 2;
  char *p = (char *)realloc(sb->data, cap);
  if (!p)
    return 0;
  sb->data = p;
  sb->cap = cap;
  return 1;
}

static int sb_addn(sb_t *sb, const char *s, size_t n) {
  if (!sb_reserve(sb, sb->len + n + 1))
    return 0;
  memcpy(sb->data + sb->len, s, n);
  sb->len += n;
  sb->data[sb->len] = '\0';
  return 1;
}

static int sb_add(sb_t *sb, const char *s) { return sb_addn(sb, s, strlen(s)); }
static void sb_add_xml_escaped(sb_t *sb, const char *s);

#define NYT_OG_W 1200
#define NYT_OG_H 630

static char *file_data_uri_from_file(const char *path, const char *mime) {
  size_t n = 0;
  char *raw = ny_read_file_raw(path, &n);
  if (!raw)
    return NULL;
  static const char table[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  sb_t out = {0};
  sb_add(&out, "data:");
  sb_add(&out, mime && *mime ? mime : "application/octet-stream");
  sb_add(&out, ";base64,");
  for (size_t i = 0; i < n; i += 3) {
    unsigned int a = (unsigned char)raw[i];
    unsigned int b = i + 1 < n ? (unsigned char)raw[i + 1] : 0;
    unsigned int c = i + 2 < n ? (unsigned char)raw[i + 2] : 0;
    char enc[4];
    enc[0] = table[(a >> 2) & 63];
    enc[1] = table[((a & 3) << 4) | ((b >> 4) & 15)];
    enc[2] = i + 1 < n ? table[((b & 15) << 2) | ((c >> 6) & 3)] : '=';
    enc[3] = i + 2 < n ? table[c & 63] : '=';
    sb_addn(&out, enc, sizeof(enc));
  }
  free(raw);
  return out.data;
}

static char *svg_data_uri_from_file(const char *path) {
  return file_data_uri_from_file(path, "image/svg+xml");
}

static int run_doc_tool_with_fontconfig(char *const argv[],
                                        const char *fontconfig_file) {
#ifdef _WIN32
  if (fontconfig_file && *fontconfig_file)
    ny_setenv("FONTCONFIG_FILE", fontconfig_file, 1);
  intptr_t rc = _spawnvp(_P_WAIT, argv[0], (const char *const *)argv);
  return rc == 0;
#else
  pid_t pid = fork();
  if (pid < 0)
    return 0;
  if (pid == 0) {
    if (fontconfig_file && *fontconfig_file)
      setenv("FONTCONFIG_FILE", fontconfig_file, 1);
    execvp(argv[0], argv);
    _exit(127);
  }
  int status = 0;
  while (waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR)
      return 0;
  }
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
}

static int run_doc_tool(char *const argv[]) {
  return run_doc_tool_with_fontconfig(argv, NULL);
}

static int dirname_into(char *out, size_t out_n, const char *path) {
  if (!out || !out_n || !path || !*path)
    return 0;
  snprintf(out, out_n, "%s", path);
  char *slash = strrchr(out, '/');
  char *backslash = strrchr(out, '\\');
  if (!slash || (backslash && backslash > slash))
    slash = backslash;
  if (!slash) {
    snprintf(out, out_n, ".");
    return 1;
  }
  if (slash == out)
    slash[1] = '\0';
  else
    *slash = '\0';
  return 1;
}

static int write_og_fontconfig(const char *path, const char *font_path) {
  if (!font_path || !ny_path_readable(font_path))
    return 0;
  char font_dir[PATH_MAX];
  if (!dirname_into(font_dir, sizeof(font_dir), font_path))
    return 0;
  sb_t xml = {0};
  sb_add(&xml, "<?xml version=\"1.0\"?>\n"
               "<!DOCTYPE fontconfig SYSTEM \"urn:fontconfig:fonts.dtd\">\n"
               "<fontconfig>\n"
               "  <include ignore_missing=\"yes\">/etc/fonts/fonts.conf</include>\n"
               "  <dir>");
  sb_add_xml_escaped(&xml, font_dir);
  sb_add(&xml, "</dir>\n</fontconfig>\n");
  int ok = xml.data && write_file(path, xml.data);
  free(xml.data);
  return ok;
}

static int generate_website_og_png(const char *svg_path, const char *png_path,
                                   const char *font_path) {
  unlink(png_path);
  char fontconfig_path[PATH_MAX];
  int has_fontconfig = 0;
  if (font_path && *font_path &&
      snprintf(fontconfig_path, sizeof(fontconfig_path), "%s.fonts.conf",
               png_path) < (int)sizeof(fontconfig_path)) {
    has_fontconfig = write_og_fontconfig(fontconfig_path, font_path);
  }
  char *rsvg[] = {"rsvg-convert", "-w", "1200", "-h", "630", "-f", "png",
                  "-o",           (char *)png_path, (char *)svg_path, NULL};
  if (run_doc_tool_with_fontconfig(rsvg,
                                   has_fontconfig ? fontconfig_path : NULL) &&
      ny_path_readable(png_path))
    return 1;

  char *magick[] = {"magick", (char *)svg_path, "-resize", "1200x630!",
                    (char *)png_path, NULL};
  if (run_doc_tool_with_fontconfig(
          magick, has_fontconfig ? fontconfig_path : NULL) &&
      ny_path_readable(png_path))
    return 1;

  char *convert[] = {"convert", (char *)svg_path, "-resize", "1200x630!",
                     (char *)png_path, NULL};
  if (run_doc_tool_with_fontconfig(
          convert, has_fontconfig ? fontconfig_path : NULL) &&
      ny_path_readable(png_path))
    return 1;
  return 0;
}

static int generate_website_og_svg(const char *path, const char *logo_path,
                                   const char *favicon_path,
                                   const char *mono_font_path,
                                   const char *display_font_path, int modules,
                                   int symbols, int files) {
  (void)modules;
  (void)symbols;
  (void)files;
  char *logo_uri = svg_data_uri_from_file(logo_path);
  char *favicon_uri = svg_data_uri_from_file(favicon_path);
  char *mono_font_uri = mono_font_path && ny_path_readable(mono_font_path)
                            ? file_data_uri_from_file(mono_font_path, "font/ttf")
                            : NULL;
  char *display_font_uri =
      display_font_path && ny_path_readable(display_font_path)
          ? file_data_uri_from_file(display_font_path, "font/ttf")
          : NULL;
  if (!logo_uri || !favicon_uri) {
    free(logo_uri);
    free(favicon_uri);
    free(mono_font_uri);
    free(display_font_uri);
    return 0;
  }
  sb_t svg = {0};
  sb_add(&svg, "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"1200\" "
               "height=\"630\" viewBox=\"0 0 1200 630\" role=\"img\" "
               "aria-label=\"Nytrix\">\n"
               "  <defs>\n"
               "    <style><![CDATA[\n");
  if (mono_font_uri) {
    sb_add(&svg, "      @font-face{font-family:'NyOGMono';src:url(");
    sb_add(&svg, mono_font_uri);
    sb_add(&svg, ") format('truetype');font-weight:400 900;}\n");
  }
  if (display_font_uri) {
    sb_add(&svg, "      @font-face{font-family:'NyOGDisplay';src:url(");
    sb_add(&svg, display_font_uri);
    sb_add(&svg, ") format('truetype');font-weight:400;}\n");
  }
  sb_add(&svg,
               "    ]]></style>\n"
               "    <linearGradient id=\"rule\" x1=\"0\" y1=\"0\" x2=\"1\" y2=\"0\">\n"
               "      <stop offset=\"0\" stop-color=\"#8ccfff\"/>\n"
               "      <stop offset=\"0.48\" stop-color=\"#b8b0ff\"/>\n"
               "      <stop offset=\"1\" stop-color=\"#8bdc9a\"/>\n"
               "    </linearGradient>\n"
               "    <pattern id=\"checker\" width=\"48\" height=\"48\" patternUnits=\"userSpaceOnUse\">\n"
               "      <rect width=\"48\" height=\"48\" fill=\"#000\"/>\n"
               "      <path d=\"M0 0h24v24H0zM24 24h24v24H24z\" fill=\"#040405\" opacity=\"0.62\"/>\n"
               "      <path d=\"M24 0h24v24H24zM0 24h24v24H0z\" fill=\"#010102\" opacity=\"0.72\"/>\n"
               "      <path d=\"M47.5 0v48M0 47.5h48\" stroke=\"#b6a0ff\" stroke-width=\"1\" opacity=\"0.035\"/>\n"
               "    </pattern>\n"
               "  </defs>\n"
               "  <rect width=\"1200\" height=\"630\" fill=\"#000\"/>\n"
               "  <rect width=\"1200\" height=\"630\" fill=\"url(#checker)\" opacity=\"0.72\"/>\n"
               "  <path d=\"M0 0h1200v630H0z\" fill=\"#000\" opacity=\"0.36\"/>\n"
               "  <path d=\"M84 548h1040\" stroke=\"#202433\" stroke-width=\"1.5\"/>\n"
               "  <image x=\"70\" y=\"113\" width=\"90\" height=\"90\" preserveAspectRatio=\"xMidYMid meet\" href=\"");
  sb_add(&svg, favicon_uri);
  sb_add(&svg, "\"/>\n"
               "  <image x=\"168\" y=\"113\" width=\"106\" height=\"106\" preserveAspectRatio=\"xMidYMid meet\" href=\"");
  sb_add(&svg, logo_uri);
  sb_add(&svg, "\"/>\n"
               "  <g font-family=\"NyOGDisplay, Quantico, Inter, DejaVu Sans, Arial, sans-serif\">\n"
               "    <text x=\"257\" y=\"197\" fill=\"#f6f7fb\" font-family=\"NyOGDisplay, Quantico, Inter, DejaVu Sans, Arial, sans-serif\" font-size=\"76\" font-weight=\"400\">ytrix</text>\n"
               "    <text x=\"84\" y=\"258\" fill=\"#f0f1f5\" font-size=\"36\" font-weight=\"400\">Think freely.</text>\n"
               "    <text x=\"84\" y=\"298\" fill=\"#b9bdc8\" font-size=\"25\" font-weight=\"400\">Native. Explicit. Comptime. Cross-platform.</text>\n"
               "  </g>\n"
               "  <g font-family=\"NyOGMono, JetBrains Mono, DejaVu Sans Mono, Consolas, monospace\">\n"
               "    <rect x=\"72\" y=\"330\" width=\"1052\" height=\"150\" fill=\"#010102\" stroke=\"#202433\"/>\n"
               "    <path d=\"M72 361h1052\" stroke=\"#151823\" stroke-width=\"1\"/>\n"
               "    <text x=\"80\" y=\"350\" fill=\"#77717f\" font-size=\"13\" font-weight=\"700\" letter-spacing=\"2\">NYTRIX</text>\n"
               "    <text x=\"80\" y=\"382\" xml:space=\"preserve\" font-size=\"19\" font-weight=\"720\"><tspan fill=\"#b6a0ff\" font-weight=\"760\">use</tspan><tspan fill=\"#aebdff\"> std.core</tspan></text>\n"
               "    <text x=\"80\" y=\"410\" xml:space=\"preserve\" font-size=\"19\" font-weight=\"720\"><tspan fill=\"#b6a0ff\" font-weight=\"760\">fn</tspan><tspan fill=\"#d1c2ff\"> area</tspan><tspan fill=\"#85808d\" font-weight=\"700\">(</tspan><tspan fill=\"#bcaaff\">int</tspan><tspan fill=\"#eeeeef\"> w</tspan><tspan fill=\"#85808d\" font-weight=\"700\">, </tspan><tspan fill=\"#bcaaff\">int</tspan><tspan fill=\"#eeeeef\"> h</tspan><tspan fill=\"#85808d\" font-weight=\"700\">) </tspan><tspan fill=\"#bcaaff\">int</tspan><tspan fill=\"#85808d\" font-weight=\"700\"> {</tspan><tspan fill=\"#b6a0ff\" font-weight=\"760\"> return</tspan><tspan fill=\"#eeeeef\"> w</tspan><tspan fill=\"#c4bed2\"> *</tspan><tspan fill=\"#eeeeef\"> h</tspan><tspan fill=\"#85808d\" font-weight=\"700\"> }</tspan></text>\n"
               "    <text x=\"80\" y=\"438\" xml:space=\"preserve\" font-size=\"19\" font-weight=\"720\"><tspan fill=\"#b6a0ff\" font-weight=\"760\">def</tspan><tspan fill=\"#eeeeef\"> pixels</tspan><tspan fill=\"#c4bed2\"> =</tspan><tspan fill=\"#d1c2ff\"> area</tspan><tspan fill=\"#85808d\" font-weight=\"700\">(</tspan><tspan fill=\"#c9b6ff\">12</tspan><tspan fill=\"#85808d\" font-weight=\"700\">, </tspan><tspan fill=\"#c9b6ff\">8</tspan><tspan fill=\"#85808d\" font-weight=\"700\">)</tspan></text>\n"
               "    <text x=\"80\" y=\"466\" xml:space=\"preserve\" font-size=\"19\" font-weight=\"720\"><tspan fill=\"#e4ddff\">print</tspan><tspan fill=\"#85808d\" font-weight=\"700\">(</tspan><tspan fill=\"#eeeeef\">pixels</tspan><tspan fill=\"#85808d\" font-weight=\"700\">)</tspan></text>\n"
               "  </g>\n"
               "  <g font-family=\"NyOGDisplay, Quantico, Inter, DejaVu Sans, Arial, sans-serif\" font-size=\"21\" font-weight=\"400\">\n"
               "    <text x=\"84\" y=\"584\" fill=\"#858a96\">nytrix.x3ric.com</text>\n"
               "    <text x=\"798\" y=\"584\" fill=\"#858a96\">learn / spec / changelog / API</text>\n"
               "  </g>\n"
               "</svg>\n");
  free(logo_uri);
  free(favicon_uri);
  free(mono_font_uri);
  free(display_font_uri);
  if (!svg.data)
    return 0;
  int ok = write_file(path, svg.data);
  free(svg.data);
  return ok;
}

static int generate_website_og_assets(const char *svg_path, const char *png_path,
                                      const char *logo_path,
                                      const char *favicon_path,
                                      const char *mono_font_path,
                                      const char *display_font_path, int modules,
                                      int symbols, int files) {
  return generate_website_og_svg(svg_path, logo_path, favicon_path,
                                 mono_font_path, display_font_path, modules,
                                 symbols, files) &&
         generate_website_og_png(svg_path, png_path, display_font_path);
}

static int sb_add_json_strn(sb_t *sb, const char *s, size_t n) {
  if (!sb_add(sb, "\""))
    return 0;
  if (!s)
    n = 0;
  for (size_t i = 0; i < n; i++) {
    const unsigned char ch = (const unsigned char)s[i];
    char tmp[8];
    switch (ch) {
    case '\\':
      if (!sb_add(sb, "\\\\"))
        return 0;
      break;
    case '"':
      if (!sb_add(sb, "\\\""))
        return 0;
      break;
    case '\n':
      if (!sb_add(sb, "\\n"))
        return 0;
      break;
    case '\r':
      if (!sb_add(sb, "\\r"))
        return 0;
      break;
    case '\t':
      if (!sb_add(sb, "\\t"))
        return 0;
      break;
    default:
      if (ch < 0x20) {
        snprintf(tmp, sizeof(tmp), "\\u%04x", ch);
        if (!sb_add(sb, tmp))
          return 0;
      } else if (!sb_addn(sb, (const char *)&s[i], 1))
        return 0;
      break;
    }
  }
  return sb_add(sb, "\"");
}

static int sb_add_json_str(sb_t *sb, const char *s) {
  return sb_add_json_strn(sb, s ? s : "", s ? strlen(s) : 0);
}

static char *strndup0(const char *s, size_t n) {
  char *p = (char *)malloc(n + 1);
  if (!p)
    return NULL;
  memcpy(p, s, n);
  p[n] = '\0';
  return p;
}

static int starts_with_at(const char *s, size_t pos, const char *prefix) {
  return strncmp(s + pos, prefix, strlen(prefix)) == 0;
}

static int ny_brace_depth_at(const char *s, size_t pos);

static int is_ident_ch(int c) {
  return isalnum((unsigned char)c) || c == '_' || c == '.' || c == ':';
}

static int is_line_decl_at(const char *s, size_t pos, const char *prefix) {
  if (!starts_with_at(s, pos, prefix))
    return 0;
  size_t line_start = pos;
  while (line_start && s[line_start - 1] != '\n')
    line_start--;
  for (size_t i = line_start; i < pos; i++)
    if (s[i] != ' ' && s[i] != '\t')
      return 0;
  return ny_brace_depth_at(s, pos) == 0;
}

static char *trim_copy(const char *s, size_t n) {
  while (n && isspace((unsigned char)*s)) {
    s++;
    n--;
  }
  while (n && isspace((unsigned char)s[n - 1]))
    n--;
  return strndup0(s, n);
}

static char *sanitize_id(const char *name) {
  char *out = strdup(name ? name : "");
  if (!out)
    return NULL;
  for (char *p = out; *p; p++)
    if (!isalnum((unsigned char)*p) && *p != '_')
      *p = '_';
  return out;
}

static int seen_cstr(char **items, int n, const char *needle) {
  for (int i = 0; i < n; i++)
    if (strcmp(items[i], needle) == 0)
      return 1;
  return 0;
}

static char *unique_symbol_id(const char *id_src, char **seen_ids,
                              int *seen_id_n, int seen_id_cap) {
  char *base = sanitize_id(id_src);
  if (!base)
    return strdup("");
  char *id = strdup(base);
  int suffix = 2;
  while (id && seen_cstr(seen_ids, *seen_id_n, id)) {
    free(id);
    sb_t b = {0};
    char num[32];
    snprintf(num, sizeof(num), "_%d", suffix++);
    sb_add(&b, base);
    sb_add(&b, num);
    id = b.data ? b.data : strdup(base);
  }
  if (id && *seen_id_n < seen_id_cap)
    seen_ids[(*seen_id_n)++] = strdup(id);
  free(base);
  return id ? id : strdup("");
}

static char *parse_value_decl_name(const char *body, size_t body_n, size_t p,
                                   size_t *name_end) {
  while (p < body_n && isspace((unsigned char)body[p]) && body[p] != '\n')
    p++;
  size_t first = p;
  while (p < body_n && ny_symbol_path_char((unsigned char)body[p]))
    p++;
  size_t first_end = p;
  while (p < body_n && isspace((unsigned char)body[p]) && body[p] != '\n')
    p++;
  if (first_end > first && p < body_n && body[p] == ':') {
    p++;
    while (p < body_n && isspace((unsigned char)body[p]) && body[p] != '\n')
      p++;
    size_t ns = p;
    while (p < body_n && (isalnum((unsigned char)body[p]) || body[p] == '_'))
      p++;
    if (p > ns) {
      if (name_end)
        *name_end = p;
      return strndup0(body + ns, p - ns);
    }
  }
  p = first;
  while (p < body_n && (isalnum((unsigned char)body[p]) || body[p] == '_'))
    p++;
  if (name_end)
    *name_end = p;
  return p > first ? strndup0(body + first, p - first) : NULL;
}

static char *dedent_clean_code(const char *code, size_t n) {
  if (!code)
    return strdup("");
  size_t start = 0, end = n;
  while (start < end) {
    size_t p = start;
    while (p < end && code[p] != '\n')
      p++;
    int blank = 1;
    for (size_t i = start; i < p; i++) {
      if (code[i] == ' ' || code[i] == '\t' || code[i] == '\r') continue;
      blank = 0;
      break;
    }
    if (!blank)
      break;
    start = p < end ? p + 1 : p;
  }
  while (end > start && isspace((unsigned char)code[end - 1]))
    end--;
  char *tmp = strndup0(code + start, end - start);
  if (!tmp)
    return NULL;
  int min_indent = -1;
  for (char *line = tmp; *line;) {
    char *next = strchr(line, '\n');
    size_t ln = next ? (size_t)(next - line) : strlen(line);
    size_t i = 0;
    while (i < ln && (line[i] == ' ' || line[i] == '\t'))
      i++;
    if (i < ln) {
      if (min_indent < 0 || (int)i < min_indent)
        min_indent = (int)i;
    }
    if (!next)
      break;
    line = next + 1;
  }
  if (min_indent <= 0)
    return tmp;
  sb_t sb = {0};
  for (char *line = tmp; *line;) {
    char *next = strchr(line, '\n');
    size_t ln = next ? (size_t)(next - line) : strlen(line);
    size_t cut = ln >= (size_t)min_indent ? (size_t)min_indent : 0;
    sb_addn(&sb, line + cut, ln - cut);
    if (next)
      sb_add(&sb, "\n");
    else
      break;
    line = next + 1;
  }
  free(tmp);
  return sb.data ? sb.data : strdup("");
}

static int find_matching_brace(const char *s, size_t open_pos, size_t limit,
                               size_t *close_pos) {
  int depth = 0, in_string = 0, in_comment = 0, esc_next = 0;
  char quote = 0;
  for (size_t i = open_pos; i < limit; i++) {
    char c = s[i];
    if (esc_next) {
      esc_next = 0;
      continue;
    }
    if (in_string && c == '\\') {
      esc_next = 1;
      continue;
    }
    if (!in_string && c == ';') {
      in_comment = 1;
      continue;
    }
    if (in_comment) {
      if (c == '\n')
        in_comment = 0;
      continue;
    }
    if (!in_string && (c == '"' || c == '\'')) {
      in_string = 1;
      quote = c;
      continue;
    }
    if (in_string) {
      if (c == quote)
        in_string = 0;
      continue;
    }
    if (c == '{')
      depth++;
    else if (c == '}') {
      depth--;
      if (depth == 0) {
        *close_pos = i;
        return 1;
      }
    }
  }
  return 0;
}

typedef struct {
  size_t ret_start;
  size_t ret_end;
  size_t body_open;
  size_t body_close;
  char *name;
  char *args;
} ny_web_fn_decl_t;

static void ny_web_fn_decl_free(ny_web_fn_decl_t *decl) {
  if (!decl)
    return;
  free(decl->name);
  free(decl->args);
  memset(decl, 0, sizeof(*decl));
}

static size_t ny_web_skip_ws(const char *s, size_t n, size_t p) {
  while (p < n && isspace((unsigned char)s[p]))
    p++;
  return p;
}

static size_t ny_web_trim_end_ws(const char *s, size_t start, size_t end) {
  while (end > start && isspace((unsigned char)s[end - 1]))
    end--;
  return end;
}

static int find_matching_paren(const char *s, size_t open_pos, size_t limit,
                               size_t *close_pos) {
  int depth = 0, in_string = 0, in_comment = 0, esc_next = 0;
  char quote = 0;
  for (size_t i = open_pos; i < limit; i++) {
    char c = s[i];
    if (esc_next) {
      esc_next = 0;
      continue;
    }
    if (in_string && c == '\\') {
      esc_next = 1;
      continue;
    }
    if (!in_string && c == ';') {
      in_comment = 1;
      continue;
    }
    if (in_comment) {
      if (c == '\n')
        in_comment = 0;
      continue;
    }
    if (!in_string && (c == '"' || c == '\'')) {
      in_string = 1;
      quote = c;
      continue;
    }
    if (in_string) {
      if (c == quote)
        in_string = 0;
      continue;
    }
    if (c == '(')
      depth++;
    else if (c == ')') {
      depth--;
      if (depth == 0) {
        *close_pos = i;
        return 1;
      }
    }
  }
  return 0;
}

static int parse_fn_decl_at(const char *body, size_t body_n, size_t pos,
                            ny_web_fn_decl_t *out) {
  ny_web_fn_decl_t decl = {0};
  if (!out || pos + 3 >= body_n || !is_line_decl_at(body, pos, "fn "))
    return 0;

  size_t p = pos + 3;
  while (p < body_n && isspace((unsigned char)body[p]) && body[p] != '\n')
    p++;
  size_t name_start = p;
  while (p < body_n && is_ident_ch((unsigned char)body[p]))
    p++;
  if (p == name_start)
    return 0;
  size_t name_end = p;

  p = ny_web_skip_ws(body, body_n, p);
  if (p >= body_n || body[p] != '(')
    return 0;
  size_t args_open = p, args_close = 0;
  if (!find_matching_paren(body, args_open, body_n, &args_close))
    return 0;

  p = ny_web_skip_ws(body, body_n, args_close + 1);
  size_t ret_start = p, ret_end = p;
  int has_ret = 0;
  if (p < body_n && body[p] == ':') {
    p = ny_web_skip_ws(body, body_n, p + 1);
    ret_start = p;
    has_ret = 1;
  } else if (p + 1 < body_n && body[p] == '-' && body[p + 1] == '>') {
    p = ny_web_skip_ws(body, body_n, p + 2);
    ret_start = p;
    has_ret = 1;
  } else if (p < body_n && body[p] != '{') {
    ret_start = p;
    has_ret = 1;
  }

  if (has_ret) {
    while (p < body_n && body[p] != '{' && body[p] != '\n' &&
           body[p] != '\r' && body[p] != ';')
      p++;
    ret_end = ny_web_trim_end_ws(body, ret_start, p);
    p = ny_web_skip_ws(body, body_n, p);
  }

  if (p >= body_n || body[p] != '{')
    return 0;
  size_t closep = 0;
  if (!find_matching_brace(body, p, body_n, &closep))
    return 0;

  decl.ret_start = ret_start;
  decl.ret_end = ret_end;
  decl.body_open = p;
  decl.body_close = closep;
  decl.name = strndup0(body + name_start, name_end - name_start);
  decl.args = trim_copy(body + args_open + 1, args_close - args_open - 1);
  if (!decl.name || !decl.args) {
    ny_web_fn_decl_free(&decl);
    return 0;
  }
  *out = decl;
  return 1;
}

static void append_fn_signature(sb_t *sig, const char *display_name,
                                const ny_web_fn_decl_t *decl,
                                const char *body) {
  sb_add(sig, "fn ");
  sb_add(sig, display_name ? display_name : (decl ? decl->name : ""));
  sb_add(sig, "(");
  sb_add(sig, (decl && decl->args) ? decl->args : "");
  sb_add(sig, ")");
  if (decl && decl->ret_end > decl->ret_start) {
    sb_add(sig, " ");
    sb_addn(sig, body + decl->ret_start, decl->ret_end - decl->ret_start);
  }
}

static int ny_brace_depth_at(const char *s, size_t pos) {
  int depth = 0, in_string = 0, in_comment = 0, esc_next = 0;
  char quote = 0;
  for (size_t i = 0; i < pos; i++) {
    char c = s[i];
    if (esc_next) {
      esc_next = 0;
      continue;
    }
    if (in_string && c == '\\') {
      esc_next = 1;
      continue;
    }
    if (!in_string && c == ';') {
      in_comment = 1;
      continue;
    }
    if (in_comment) {
      if (c == '\n')
        in_comment = 0;
      continue;
    }
    if (!in_string && (c == '"' || c == '\'')) {
      in_string = 1;
      quote = c;
      continue;
    }
    if (in_string) {
      if (c == quote)
        in_string = 0;
      continue;
    }
    if (c == '{')
      depth++;
    else if (c == '}' && depth > 0)
      depth--;
  }
  return depth;
}

static char *leading_comments(const char *s, size_t start) {
  if (start == 0)
    return strdup("");
  size_t line_end = start;
  while (line_end && s[line_end - 1] != '\n')
    line_end--;
  sb_t rev = {0};
  int any = 0;
  while (line_end > 0) {
    size_t prev_end = line_end - 1;
    size_t line_start = prev_end;
    while (line_start && s[line_start - 1] != '\n')
      line_start--;
    const char *line = s + line_start;
    size_t ln = prev_end - line_start;
    while (ln && isspace((unsigned char)*line) && *line != '\n') {
      line++;
      ln--;
    }
    if (ln >= 2 && line[0] == ';' && line[1] == ';') {
      char *part = trim_copy(line + 2, ln - 2);
      if (!part)
        break;
      sb_t next = {0};
      sb_add(&next, part);
      if (any)
        sb_add(&next, "\n");
      if (rev.data)
        sb_add(&next, rev.data);
      free(part);
      free(rev.data);
      rev = next;
      any = 1;
    } else if (ln == 0) {

    } else {
      break;
    }
    line_end = line_start;
  }
  return rev.data ? rev.data : strdup("");
}

static char *leading_file_comments(const char *s, size_t n) {
  sb_t out = {0};
  int any = 0;
  for (size_t p = 0; p < n;) {
    const char *line = s + p;
    size_t e = p;
    while (e < n && s[e] != '\n')
      e++;
    size_t ln = e - p;
    while (ln && isspace((unsigned char)*line) && *line != '\n') {
      line++;
      ln--;
    }
    if (ln >= 2 && line[0] == ';' && line[1] == ';') {
      char *part = trim_copy(line + 2, ln - 2);
      if (!part)
        break;
      if (any)
        sb_add(&out, "\n");
      sb_add(&out, part);
      free(part);
      any = 1;
    } else if (ln == 0) {

    } else {
      break;
    }
    p = e < n ? e + 1 : e;
  }
  return out.data ? out.data : strdup("");
}

static char *extract_docstring(const char *body, size_t n, size_t *code_start) {
  size_t i = 0;
  while (i < n && isspace((unsigned char)body[i]))
    i++;
  if (i >= n || (body[i] != '"' && body[i] != '\'')) {
    *code_start = 0;
    return strdup("");
  }
  char quote = body[i++];
  sb_t doc = {0};
  int esc_next = 0;
  for (; i < n; i++) {
    char c = body[i];
    if (esc_next) {
      if (c == 'n')
        sb_add(&doc, "\n");
      else if (c == 't')
        sb_add(&doc, "\t");
      else
        sb_addn(&doc, &c, 1);
      esc_next = 0;
      continue;
    }
    if (c == '\\') {
      esc_next = 1;
      continue;
    }
    if (c == quote) {
      *code_start = i + 1;
      return doc.data ? doc.data : strdup("");
    }
    sb_addn(&doc, &c, 1);
  }
  free(doc.data);
  *code_start = 0;
  return strdup("");
}

static char *extract_body_html(const char *html) {
  const char *lo = html;
  const char *body = NULL;
  for (const char *p = lo; *p; p++) {
    if (strncasecmp(p, "<body", 5) != 0) continue;
    body = strchr(p, '>');
    if (body)
      body++;
    break;
  }
  if (!body)
    return strdup(html);
  const char *end = NULL;
  for (const char *p = body; *p; p++) {
    if (strncasecmp(p, "</body>", 7) != 0) continue;
    end = p;
    break;
  }
  return strndup0(body, end ? (size_t)(end - body) : strlen(body));
}

static void replace_all(sb_t *out, const char *src, const char *needle,
                        const char *repl) {
  size_t nl = strlen(needle);
  const char *p = src;
  const char *hit;
  while ((hit = strstr(p, needle))) {
    sb_addn(out, p, (size_t)(hit - p));
    sb_add(out, repl);
    p = hit + nl;
  }
  sb_add(out, p);
}

static const char *find_case(const char *haystack, const char *needle) {
  size_t nl = strlen(needle);
  if (!nl)
    return haystack;
  for (const char *p = haystack; *p; p++) {
    size_t i = 0;
    while (i < nl && p[i] &&
           tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i]))
      i++;
    if (i == nl)
      return p;
  }
  return NULL;
}

static void remove_html_block(sb_t *out, const char *src,
                              const char *open_needle,
                              const char *close_needle) {
  const char *p = src;
  size_t open_n = strlen(open_needle), close_n = strlen(close_needle);
  for (;;) {
    const char *open = find_case(p, open_needle);
    if (!open) {
      sb_add(out, p);
      return;
    }
    sb_addn(out, p, (size_t)(open - p));
    const char *close = find_case(open + open_n, close_needle);
    if (!close)
      return;
    p = close + close_n;
  }
}

static char *refine_info_html(char *html) {
  sb_t out = {0};

  replace_all(&out, html,
              "<div class=\"contents\" style=\"margin-left: 20px;\">",
              "<div class=\"contents\">");
  free(html);
  html = out.data ? out.data : strdup("");
  out = (sb_t){0};

  replace_all(&out, html,
              "<ul class=\"mini-toc\" style=\"margin-left: 20px;\">",
              "<ul class=\"mini-toc\">");
  free(html);
  html = out.data ? out.data : strdup("");
  out = (sb_t){0};

  remove_html_block(&out, html, "<div class=\"nav-panel\">", "</div>");
  free(html);
  html = out.data ? out.data : strdup("");
  out = (sb_t){0};

  replace_all(&out, html, "[Contents] [Index]", "");
  free(html);
  html = out.data ? out.data : strdup("");
  out = (sb_t){0};

  replace_all(&out, html, "[Contents]&nbsp;[Index]", "");
  free(html);
  html = out.data ? out.data : strdup("");
  out = (sb_t){0};

  remove_html_block(&out, html, "<a class=\"copiable-link\"", "</a>");
  free(html);
  return out.data ? out.data : strdup("");
}

static const char *path_basename(const char *p) {
  const char *slash = strrchr(p, '/');
  return slash ? slash + 1 : p;
}

static char *stem_name(const char *p) {
  const char *base = path_basename(p);
  const char *dot = strrchr(base, '.');
  if (!dot)
    return strdup(base);
  return strndup0(base, (size_t)(dot - base));
}

static char *doc_route_name(const char *base_dir, const char *path) {
  const char *rel = path;
  if (base_dir && *base_dir && path && *path) {
    size_t n = strlen(base_dir);
    if (strncmp(path, base_dir, n) == 0 &&
        (path[n] == '/' || path[n] == '\\' || path[n] == '\0')) {
      rel = path + n;
      while (*rel == '/' || *rel == '\\')
        rel++;
    }
  }
  sb_t out = {0};
  const char *p = rel ? rel : path;
  const char *last_dot = strrchr(p, '.');
  size_t n = last_dot ? (size_t)(last_dot - p) : strlen(p);
  for (size_t i = 0; i < n; i++)
    sb_addn(&out, (p[i] == '\\') ? "/" : p + i, 1);
  return out.data ? out.data : stem_name(path);
}

static char *doc_title_from_route(const char *route) {
  sb_t out = {0};
  int word_start = 1;
  for (const char *p = route ? route : ""; *p; p++) {
    if (*p == '/' || *p == '\\') {
      sb_add(&out, " / ");
      word_start = 1;
    } else if (*p == '-' || *p == '_') {
      sb_add(&out, " ");
      word_start = 1;
    } else {
      char c = word_start ? (char)toupper((unsigned char)*p) : *p;
      sb_addn(&out, &c, 1);
      word_start = 0;
    }
  }
  return out.data ? out.data : strdup(route ? route : "");
}

static int cmp_cstr_ptr(const void *a, const void *b) {
  const char *sa = *(const char *const *)a;
  const char *sb = *(const char *const *)b;
  return strcmp(sa, sb);
}

static int path_is_under(const char *root, const char *path) {
  if (!root || !*root || !path || !*path)
    return 0;
  size_t n = strlen(root);
  return strncmp(path, root, n) == 0 &&
         (path[n] == '/' || path[n] == '\\' || path[n] == '\0');
}

static int paths_overlap(const char *a, const char *b) {
  return path_is_under(a, b) || path_is_under(b, a);
}

static int web_skip_source_dir(const char *name) {
  return !name || strcmp(name, ".") == 0 || strcmp(name, "..") == 0 ||
         strcmp(name, ".git") == 0 || strcmp(name, ".cache") == 0 ||
         strcmp(name, "build") == 0 || strcmp(name, "tmp") == 0 ||
         strcmp(name, "node_modules") == 0 || strcmp(name, "__pycache__") == 0;
}

static int append_source_file_bundle(sb_t *bundle, const char *path,
                                     int *source_count) {
  char *txt = ny_read_file_raw(path, NULL);
  if (!txt)
    return 0;
  sb_add(bundle, "\n;; Module from ");
  sb_add(bundle, path);
  sb_add(bundle, "\n");
  sb_add(bundle, txt);
  if (bundle->len == 0 || bundle->data[bundle->len - 1] != '\n')
    sb_add(bundle, "\n");
  free(txt);
  if (source_count)
    (*source_count)++;
  return 1;
}

static int append_sources_from_dir(sb_t *bundle, const char *dir,
                                   int *source_count) {
  DIR *d = opendir(dir);
  if (!d)
    return 0;
  char **names = NULL;
  size_t len = 0, cap = 0;
  struct dirent *ent;
  while ((ent = readdir(d))) {
    if (web_skip_source_dir(ent->d_name))
      continue;
    if (len == cap) {
      size_t next = cap ? cap * 2 : 64;
      char **tmp = (char **)realloc(names, next * sizeof(*names));
      if (!tmp)
        break;
      names = tmp;
      cap = next;
    }
    names[len++] = strdup(ent->d_name);
  }
  closedir(d);
  qsort(names, len, sizeof(*names), cmp_cstr_ptr);
  for (size_t i = 0; i < len; i++) {
    char path[PATH_MAX];
    if (join_path(path, sizeof(path), dir, names[i])) {
      if (path_is_dir(path))
        append_sources_from_dir(bundle, path, source_count);
      else if (path_has_suffix(path, ".ny"))
        append_source_file_bundle(bundle, path, source_count);
    }
    free(names[i]);
  }
  free(names);
  return 1;
}

static int bundle_has_source_markers(const char *text) {
  return text && (strstr(text, "\n;; Module from ") ||
                  strncmp(text, ";; Module from ", 15) == 0 ||
                  strstr(text, "\n#line ") || strncmp(text, "#line ", 6) == 0);
}

static int append_system_source_roots(sb_t *bundle, const char *asset_root,
                                      const char *input, int *source_count) {
  int before = source_count ? *source_count : 0;
  char lib_path[PATH_MAX];
  char input_abs[PATH_MAX];
  const char *input_root = input;
  if (input && *input && input[0] != '/') {
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd))) {
      if (strcmp(input, ".") == 0) {
        snprintf(input_abs, sizeof(input_abs), "%s", cwd);
        input_root = input_abs;
      } else if (join_path(input_abs, sizeof(input_abs), cwd, input))
        input_root = input_abs;
    }
  }
  if (asset_root && join_path(lib_path, sizeof(lib_path), asset_root, "lib") &&
      path_is_dir(lib_path) &&
      (!input_root || !paths_overlap(input_root, lib_path)))
    append_sources_from_dir(bundle, lib_path, source_count);

  const char *env_lib = getenv("NYTRIX_LIB_PATH");
  if (env_lib && *env_lib) {
    const char *p = env_lib;
    while (*p) {
      const char *colon = strchr(p, ':');
      size_t n = colon ? (size_t)(colon - p) : strlen(p);
      if (n > 0 && n < sizeof(lib_path)) {
        memcpy(lib_path, p, n);
        lib_path[n] = '\0';
        if (path_is_dir(lib_path) &&
            (!input_root || !paths_overlap(input_root, lib_path)))
          append_sources_from_dir(bundle, lib_path, source_count);
      }
      if (!colon)
        break;
      p = colon + 1;
    }
  }

  return source_count ? (*source_count > before) : 0;
}

static char *read_web_input_bundle(const char *input, const char *asset_root,
                                   int *source_count) {
  if (source_count)
    *source_count = 0;
  if (path_is_dir(input)) {
    sb_t bundle = {0};
    append_sources_from_dir(&bundle, input, source_count);
    append_system_source_roots(&bundle, asset_root, input, source_count);
    return bundle.data ? bundle.data : strdup("");
  }
  char *txt = ny_read_file_raw(input, NULL);
  if (!txt)
    return NULL;
  if (bundle_has_source_markers(txt)) {
    if (source_count)
      *source_count = 1;
    return txt;
  }
  sb_t bundle = {0};
  sb_add(&bundle, ";; Module from ");
  sb_add(&bundle, input ? input : "unknown");
  sb_add(&bundle, "\n");
  sb_add(&bundle, txt);
  if (bundle.len == 0 || bundle.data[bundle.len - 1] != '\n')
    sb_add(&bundle, "\n");
  free(txt);
  if (source_count)
    *source_count = 1;
  return bundle.data ? bundle.data : strdup("");
}

static int docs_origin_is_lib(const char *root, const char *orig) {
  if (!orig || !*orig || strcmp(orig, "unknown") == 0)
    return 0;
  char norm[PATH_MAX];
  snprintf(norm, sizeof(norm), "%s", orig);
  for (char *p = norm; *p; p++)
    if (*p == '\\')
      *p = '/';
  const char *rel = norm;
  if (root && *root && path_is_under(root, norm)) {
    rel = norm + strlen(root);
    while (*rel == '/')
      rel++;
  }
  while (rel[0] == '.' && rel[1] == '/')
    rel += 2;
  if (strncmp(rel, "etc/projects/", 13) == 0 ||
      strncmp(rel, "etc/tests/", 10) == 0 ||
      strstr(rel, "/etc/projects/") || strstr(rel, "/etc/tests/"))
    return 0;
  char lib_dir[PATH_MAX];
  if (join_path(lib_dir, sizeof(lib_dir), root, "lib")) {
    size_t n = strlen(lib_dir);
    if (strncmp(norm, lib_dir, n) == 0 &&
        (norm[n] == '/' || norm[n] == '\0'))
      return 1;
  }
  if (strcmp(rel, "lib") == 0 || strncmp(rel, "lib/", 4) == 0 ||
      strstr(rel, "/lib/"))
    return 1;
  if (root && *root && path_is_under(root, norm))
    return 0;
  return path_has_suffix(orig, ".ny");
}

static char *docs_origin_relative(const char *root, const char *orig) {
  if (!orig || !*orig)
    return strdup("unknown");
  const char *rel = NULL;
  char lib_dir[PATH_MAX];
  if (join_path(lib_dir, sizeof(lib_dir), root, "lib")) {
    size_t n = strlen(lib_dir);
    if (strncmp(orig, lib_dir, n) == 0 &&
        (orig[n] == '/' || orig[n] == '\\' || orig[n] == '\0')) {
      rel = orig + n;
      while (*rel == '/' || *rel == '\\')
        rel++;
    }
  }
  if (!rel) {
    const char *slash = strstr(orig, "/lib/");
    const char *backslash = strstr(orig, "\\lib\\");
    if (slash)
      rel = slash + 5;
    else if (backslash)
      rel = backslash + 5;
    else if (strncmp(orig, "lib/", 4) == 0 || strncmp(orig, "lib\\", 4) == 0)
      rel = orig + 4;
  }
  sb_t out = {0};
  const char *prefix = "lib/";
  char cwd[PATH_MAX];
  if (!rel && getcwd(cwd, sizeof(cwd))) {
    size_t n = strlen(cwd);
    if (strncmp(orig, cwd, n) == 0 &&
        (orig[n] == '/' || orig[n] == '\\' || orig[n] == '\0')) {
      rel = orig + n;
      while (*rel == '/' || *rel == '\\')
        rel++;
      prefix = "./";
    }
  }
  if (!rel)
    prefix = "";
  sb_add(&out, prefix);
  const char *p = rel ? rel : orig;
  while (*p) {
    sb_addn(&out, (*p == '\\') ? "/" : p, 1);
    p++;
  }
  return out.data ? out.data : strdup(orig);
}

static char *docs_origin_module_name(const char *root, const char *orig) {
  char *rel = docs_origin_relative(root, orig);
  const char *p = rel ? rel : (orig ? orig : "source");
  if (strncmp(p, "lib/", 4) == 0 || strncmp(p, "lib\\", 4) == 0)
    p += 4;
  else if (strncmp(p, "./", 2) == 0 || strncmp(p, ".\\", 2) == 0)
    p += 2;

  size_t n = strlen(p);
  if (n >= 3 && strcmp(p + n - 3, ".ny") == 0)
    n -= 3;

  sb_t out = {0};
  int last_dot = 1;
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)p[i];
    if (c == '/' || c == '\\' || c == '.') {
      if (!last_dot) {
        sb_add(&out, ".");
        last_dot = 1;
      }
    } else if (isalnum(c) || c == '_') {
      char ch[2] = {(char)c, 0};
      sb_add(&out, ch);
      last_dot = 0;
    } else if (c == '-' || isspace(c)) {
      sb_add(&out, "_");
      last_dot = 0;
    }
  }
  while (out.len && out.data[out.len - 1] == '.')
    out.data[--out.len] = '\0';
  free(rel);
  return out.data && *out.data ? out.data : strdup("source");
}

static int markdown_include_path_ok(const char *path) {
  return path && *path && path[0] != '/' && !strchr(path, '\\') &&
         !strstr(path, "..");
}

static char *github_source_href_for_markdown(const char *href, size_t href_n) {
  if (!href || href_n == 0)
    return NULL;
  const char *scheme = memchr(href, ':', href_n);
  const char *slash = memchr(href, '/', href_n);
  if ((href_n >= 2 && href[0] == '/' && href[1] == '/') ||
      (scheme && (!slash || scheme < slash)))
    return NULL;
  size_t path_n = href_n;
  const char *suffix = NULL;
  for (size_t i = 0; i < href_n; i++) {
    if (href[i] == '?' || href[i] == '#') {
      path_n = i;
      suffix = href + i;
      break;
    }
  }
  char path[PATH_MAX];
  if (path_n == 0 || path_n >= sizeof(path))
    return NULL;
  memcpy(path, href, path_n);
  path[path_n] = '\0';
  for (char *p = path; *p; p++)
    if (*p == '\\')
      *p = '/';
  const char *rel = strstr(path, "etc/projects/");
  if (!rel || strstr(rel, ".."))
    return NULL;
  sb_t out = {0};
  sb_add(&out, "https://github.com/nytrix-lang/nytrix/blob/main/");
  sb_add(&out, rel);
  if (suffix)
    sb_addn(&out, suffix, href_n - (size_t)(suffix - href));
  return out.data ? out.data : NULL;
}

static void rewrite_markdown_source_links_line(sb_t *out, const char *line,
                                               size_t len) {
  size_t last = 0;
  for (size_t i = 0; i < len; i++) {
    if (line[i] != '[')
      continue;
    const char *label_end = memchr(line + i + 1, ']', len - i - 1);
    if (!label_end)
      break;
    size_t after_label = (size_t)(label_end - line + 1);
    if (after_label >= len || line[after_label] != '(')
      continue;
    const char *href_start = line + after_label + 1;
    const char *href_end =
        memchr(href_start, ')', line + len - href_start);
    if (!href_end)
      break;
    char *github =
        github_source_href_for_markdown(href_start,
                                        (size_t)(href_end - href_start));
    if (github) {
      sb_addn(out, line + last, (size_t)(href_start - (line + last)));
      sb_add(out, github);
      free(github);
      last = (size_t)(href_end - line);
      i = last;
    }
  }
  sb_addn(out, line + last, len - last);
}

static char *rewrite_markdown_site_source_links(const char *md) {
  if (!md)
    return strdup("");
  sb_t out = {0};
  int in_code = 0;
  for (const char *line = md; *line;) {
    const char *next = strchr(line, '\n');
    size_t len = next ? (size_t)(next - line) : strlen(line);
    if (len >= 3 && strncmp(line, "```", 3) == 0) {
      in_code = !in_code;
      sb_addn(&out, line, len);
    } else if (in_code) {
      sb_addn(&out, line, len);
    } else {
      rewrite_markdown_source_links_line(&out, line, len);
    }
    if (next)
      sb_add(&out, "\n");
    else
      break;
    line = next + 1;
  }
  return out.data ? out.data : strdup(md);
}

static char *expand_markdown_code_includes(const char *root, const char *md);

static char *prepare_markdown_doc_body(const char *root, const char *txt) {
  char *expanded = expand_markdown_code_includes(root, txt);
  char *linked = rewrite_markdown_site_source_links(expanded);
  free(expanded);
  return linked;
}

static int parse_code_include_directive(const char *line, size_t len,
                                        char **lang_out, char **path_out) {
  if (lang_out)
    *lang_out = NULL;
  if (path_out)
    *path_out = NULL;
  while (len && isspace((unsigned char)*line)) {
    line++;
    len--;
  }
  while (len && isspace((unsigned char)line[len - 1]))
    len--;
  const char *open = "<!--";
  const char *key = "ny-doc-include-code:";
  const char *close = "-->";
  size_t open_n = strlen(open), key_n = strlen(key), close_n = strlen(close);
  if (len < open_n + key_n + close_n ||
      strncmp(line, open, open_n) != 0)
    return 0;
  line += open_n;
  len -= open_n;
  while (len && isspace((unsigned char)*line)) {
    line++;
    len--;
  }
  if (len < key_n || strncmp(line, key, key_n) != 0)
    return 0;
  line += key_n;
  len -= key_n;
  while (len && isspace((unsigned char)*line)) {
    line++;
    len--;
  }
  const char *lang = line;
  size_t lang_n = 0;
  while (lang_n < len && !isspace((unsigned char)line[lang_n]))
    lang_n++;
  if (lang_n == 0)
    return 0;
  line += lang_n;
  len -= lang_n;
  while (len && isspace((unsigned char)*line)) {
    line++;
    len--;
  }
  const char *path = line;
  size_t path_n = 0;
  while (path_n < len && !isspace((unsigned char)line[path_n]))
    path_n++;
  if (path_n == 0)
    return 0;
  line += path_n;
  len -= path_n;
  while (len && isspace((unsigned char)*line)) {
    line++;
    len--;
  }
  if (len != close_n || strncmp(line, close, close_n) != 0)
    return 0;
  char *lang_copy = strndup0(lang, lang_n);
  char *path_copy = strndup0(path, path_n);
  if (!lang_copy || !path_copy || !markdown_include_path_ok(path_copy)) {
    free(lang_copy);
    free(path_copy);
    return 0;
  }
  if (lang_out)
    *lang_out = lang_copy;
  else
    free(lang_copy);
  if (path_out)
    *path_out = path_copy;
  else
    free(path_copy);
  return 1;
}

static char *expand_markdown_code_includes(const char *root, const char *md) {
  if (!md)
    return strdup("");
  sb_t out = {0};
  for (const char *line = md; *line;) {
    const char *next = strchr(line, '\n');
    size_t len = next ? (size_t)(next - line) : strlen(line);
    char *lang = NULL, *rel = NULL;
    if (parse_code_include_directive(line, len, &lang, &rel)) {
      char full[PATH_MAX];
      char *code = NULL;
      if (root && *root && join_path(full, sizeof(full), root, rel))
        code = ny_read_file_raw(full, NULL);
      if (code) {
        sb_add(&out, "```");
        sb_add(&out, lang);
        sb_add(&out, "\n");
        sb_add(&out, code);
        if (!out.len || out.data[out.len - 1] != '\n')
          sb_add(&out, "\n");
        sb_add(&out, "```\n");
        free(code);
      } else {
        sb_addn(&out, line, len);
        if (next)
          sb_add(&out, "\n");
      }
      free(lang);
      free(rel);
    } else {
      sb_addn(&out, line, len);
      if (next)
        sb_add(&out, "\n");
    }
    if (!next)
      break;
    line = next + 1;
  }
  return out.data ? out.data : strdup(md);
}

static int append_markdown_doc_json_item(sb_t *json, const char *base_dir,
                                         const char *root, const char *path,
                                         int *first,
                                         char **seen, int *seen_n,
                                         int seen_cap) {
  const char *dot = strrchr(path, '.');
  if (!dot || (strcmp(dot, ".md") != 0 && strcmp(dot, ".html") != 0))
    return 1;
  char *name = doc_route_name(base_dir, path);
  if (!name)
    return 0;
  for (int i = 0; i < *seen_n; i++) {
    if (strcmp(seen[i], name) == 0) {
      free(name);
      return 1;
    }
  }
  char *txt = ny_read_file_raw(path, NULL);
  if (!txt) {
    free(name);
    return 1;
  }
  char *body = NULL;
  const char *fmt = "md";
  if (strcmp(dot, ".html") == 0) {
    body = refine_info_html(extract_body_html(txt));
    fmt = "html";
    free(txt);
  } else {
    body = prepare_markdown_doc_body(root, txt);
    free(txt);
  }
  char *title = doc_title_from_route(name);
  if (!*first)
    sb_add(json, ",");
  *first = 0;
  sb_add(json, "{\"name\":");
  sb_add_json_str(json, name);
  sb_add(json, ",\"title\":");
  sb_add_json_str(json, title ? title : name);
  sb_add(json, ",\"format\":");
  sb_add_json_str(json, fmt);
  sb_add(json, ",\"html\":");
  sb_add_json_str(json, body ? body : "");
  sb_add(json, "}");
  if (*seen_n < seen_cap)
    seen[(*seen_n)++] = strdup(name);
  free(name);
  free(title);
  free(body);
  return 1;
}

static void append_markdown_docs_from_dir(sb_t *json, const char *base_dir,
                                          const char *root, const char *dir,
                                          int *first,
                                          char **seen, int *seen_n,
                                          int seen_cap, int recursive) {
  DIR *d = opendir(dir);
  if (!d)
    return;
  char **names = NULL;
  int names_n = 0, names_cap = 0;
  struct dirent *ent;
  while ((ent = readdir(d))) {
    if (web_skip_source_dir(ent->d_name))
      continue;
    if (names_n == names_cap) {
      int next_cap = names_cap ? names_cap * 2 : 32;
      char **next = (char **)realloc(names, (size_t)next_cap * sizeof(char *));
      if (!next)
        break;
      names = next;
      names_cap = next_cap;
    }
    names[names_n++] = strdup(ent->d_name);
  }
  closedir(d);
  qsort(names, (size_t)names_n, sizeof(char *), cmp_cstr_ptr);
  for (int i = 0; i < names_n; i++) {
    char path[PATH_MAX];
    if (join_path(path, sizeof(path), dir, names[i])) {
      if (recursive && path_is_dir(path))
        append_markdown_docs_from_dir(json, base_dir, root, path, first, seen,
                                      seen_n, seen_cap, recursive);
      else
        append_markdown_doc_json_item(json, base_dir, root, path, first, seen,
                                      seen_n, seen_cap);
    }
    free(names[i]);
  }
  free(names);
}

static void append_markdown_docs_json(sb_t *json, const char *root) {
  char docs_dir[PATH_MAX], env_dir[PATH_MAX];
  if (!join_path(docs_dir, sizeof(docs_dir), root, "docs"))
    docs_dir[0] = '\0';
  const char *info_dir = getenv("NYTRIX_DOC_INFO_DIR");
  if (!info_dir || !*info_dir)
    info_dir = getenv("NYTRIX_WEBDOC_INFO_DIR");
  if (info_dir && *info_dir)
    snprintf(env_dir, sizeof(env_dir), "%s", info_dir);
  else if (!join_path(env_dir, sizeof(env_dir), nyt_temp_dir(), "nytrix-info"))
    env_dir[0] = '\0';
  int first = 1;
  char *seen[1024];
  int seen_n = 0;
  sb_add(json, "[");
  if (docs_dir[0])
    append_markdown_docs_from_dir(json, docs_dir, root, docs_dir, &first, seen,
                                  &seen_n,
                                  (int)(sizeof(seen) / sizeof(seen[0])), 1);
  if (env_dir[0])
    append_markdown_docs_from_dir(json, env_dir, root, env_dir, &first, seen,
                                  &seen_n,
                                  (int)(sizeof(seen) / sizeof(seen[0])), 0);
  for (int i = 0; i < seen_n; i++)
    free(seen[i]);
  sb_add(json, "]");
}

static void append_one_import_json(sb_t *json, const char *full,
                                   const char *module, const char *symbol,
                                   const char *alias, int *first) {
  if (!*first)
    sb_add(json, ",");
  *first = 0;
  sb_add(json, "{\"full_path\":");
  sb_add_json_str(json, full);
  sb_add(json, ",\"module_target\":");
  sb_add_json_str(json, module);
  sb_add(json, ",\"symbol_target\":");
  sb_add_json_str(json, symbol);
  sb_add(json, ",\"alias\":");
  if (alias)
    sb_add_json_str(json, alias);
  else
    sb_add(json, "null");
  sb_add(json, "}");
}

static char *parse_import_target(const char *body, size_t n, size_t *pos) {
  size_t p = *pos;
  if (p < n && (body[p] == '"' || body[p] == '\'')) {
    char q = body[p++];
    size_t start = p;
    while (p < n && body[p] != q) {
      if (body[p] == '\\' && p + 1 < n)
        p += 2;
      else
        p++;
    }
    char *target = strndup0(body + start, p - start);
    if (p < n)
      p++;
    *pos = p;
    return target;
  }
  size_t start = p;
  while (p < n && (isalnum((unsigned char)body[p]) || body[p] == '_' ||
                   body[p] == '.'))
    p++;
  if (p == start)
    return NULL;
  *pos = p;
  return strndup0(body + start, p - start);
}

static int parse_import_symbol(const char *body, size_t n, size_t *pos,
                               char **sym_out, char **alias_out) {
  size_t p = *pos;
  while (p < n && (isspace((unsigned char)body[p]) || body[p] == ','))
    p++;
  size_t ss = p;
  while (p < n && (isalnum((unsigned char)body[p]) || body[p] == '_' ||
                   body[p] == '*'))
    p++;
  if (p == ss)
    return 0;
  *sym_out = strndup0(body + ss, p - ss);
  *alias_out = NULL;
  while (p < n && isspace((unsigned char)body[p]))
    p++;
  if (p + 3 <= n && strncmp(body + p, "as ", 3) == 0) {
    p += 3;
    while (p < n && isspace((unsigned char)body[p]))
      p++;
    size_t as = p;
    while (p < n && (isalnum((unsigned char)body[p]) || body[p] == '_'))
      p++;
    *alias_out = strndup0(body + as, p - as);
  }
  *pos = p;
  return 1;
}

static void append_imports_json(sb_t *json, const char *body, size_t n) {
  sb_add(json, "[");
  int first = 1;
  for (size_t i = 0; i + 4 < n; i++) {
    if (!is_line_decl_at(body, i, "use "))
      continue;
    size_t p = i + 4;
    while (p < n && isspace((unsigned char)body[p]))
      p++;
    char *target = parse_import_target(body, n, &p);
    if (!target)
      continue;
    while (p < n && isspace((unsigned char)body[p]))
      p++;
    if (p < n && body[p] == '(') {
      p++;
      while (p < n && body[p] != ')') {
        char *sym = NULL, *alias = NULL;
        if (!parse_import_symbol(body, n, &p, &sym, &alias))
          break;
        sb_t full = {0};
        sb_add(&full, target);
        if (strcmp(sym, "*") != 0) {
          sb_add(&full, ".");
          sb_add(&full, sym);
        }
        append_one_import_json(json, full.data ? full.data : target, target,
                               sym, alias, &first);
        free(full.data);
        free(sym);
        free(alias);
      }
    } else {
      char *alias = NULL;
      if (p + 3 <= n && strncmp(body + p, "as ", 3) == 0) {
        p += 3;
        while (p < n && isspace((unsigned char)body[p]))
          p++;
        size_t as = p;
        while (p < n && (isalnum((unsigned char)body[p]) || body[p] == '_'))
          p++;
        alias = strndup0(body + as, p - as);
      }
      char *lastdot = strrchr(target, '.');
      char *module =
          lastdot ? strndup0(target, (size_t)(lastdot - target)) : strdup("");
      char *sym = strdup(lastdot ? lastdot + 1 : target);
      append_one_import_json(json, target, module, sym, alias, &first);
      free(alias);
      free(module);
      free(sym);
    }
    free(target);
  }
  sb_add(json, "]");
}

static void append_symbol_json(sb_t *json, const char *id_src, const char *name,
                               const char *kind, const char *doc,
                               const char *code, const char *imports_body,
                               size_t imports_n, char **seen_ids,
                               int *seen_id_n, int seen_id_cap,
                               int *first_sym) {
  char *id = unique_symbol_id(id_src, seen_ids, seen_id_n, seen_id_cap);
  if (!*first_sym)
    sb_add(json, ",");
  *first_sym = 0;
  sb_add(json, "{\"id\":");
  sb_add_json_str(json, id ? id : "");
  sb_add(json, ",\"name\":");
  sb_add_json_str(json, name ? name : "");
  sb_add(json, ",\"kind\":");
  sb_add_json_str(json, kind ? kind : "");
  sb_add(json, ",\"doc\":");
  sb_add_json_str(json, doc ? doc : "");
  sb_add(json, ",\"code\":");
  sb_add_json_str(json, code ? code : "");
  if (imports_body) {
    sb_add(json, ",\"imports\":");
    append_imports_json(json, imports_body, imports_n);
  }
  sb_add(json, "}");
  free(id);
}

typedef struct {
  int total;
  char examples[10][80];
  int example_n;
  char tags[64][40];
  int tag_counts[64];
  int tag_n;
} module_export_summary_t;

static int module_doc_has_summary_text(const char *doc) {
  const char *p = doc ? doc : "";
  while (*p) {
    const char *line = p;
    size_t n = 0;
    while (p[n] && p[n] != '\n')
      n++;
    char *trimmed = trim_copy(line, n);
    if (trimmed && *trimmed && strncasecmp(trimmed, "keywords:", 9) != 0) {
      free(trimmed);
      return 1;
    }
    free(trimmed);
    p += n;
    if (*p == '\n')
      p++;
  }
  return 0;
}

static int module_export_stop_word(const char *s) {
  const char *words[] = {"get",  "set",   "read",    "write", "load",
                         "save", "parse", "make",    "new",   "init",
                         "free", "to",    "from",    "is",    "has",
                         "as",   "try",   "default", "with"};
  for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++)
    if (strcmp(s, words[i]) == 0)
      return 1;
  return 0;
}

static void module_summary_add_tag(module_export_summary_t *st,
                                   const char *tag) {
  if (!st || !tag || !*tag || strlen(tag) < 2)
    return;
  for (int i = 0; i < st->tag_n; i++) {
    if (strcmp(st->tags[i], tag) == 0) {
      st->tag_counts[i]++;
      return;
    }
  }
  if (st->tag_n >= (int)(sizeof(st->tags) / sizeof(st->tags[0])))
    return;
  snprintf(st->tags[st->tag_n], sizeof(st->tags[st->tag_n]), "%s", tag);
  st->tag_counts[st->tag_n] = 1;
  st->tag_n++;
}

static void module_summary_add_export(module_export_summary_t *st,
                                      const char *name) {
  if (!st || !name || !*name)
    return;
  st->total++;
  if (st->example_n < (int)(sizeof(st->examples) / sizeof(st->examples[0])) &&
      name[0] != '_') {
    snprintf(st->examples[st->example_n], sizeof(st->examples[st->example_n]),
             "%s", name);
    st->example_n++;
  }

  char first[40] = {0}, second[40] = {0};
  int part = 0, pos = 0;
  for (const char *p = name; *p; p++) {
    char c = (char)tolower((unsigned char)*p);
    if (c == '_' || c == '-' || c == '.') {
      if (part == 0) {
        first[pos] = '\0';
        part = 1;
        pos = 0;
      } else if (part == 1) {
        second[pos] = '\0';
        part = 2;
      }
      continue;
    }
    if (!isalnum((unsigned char)c))
      continue;
    if (part == 0 && pos < (int)sizeof(first) - 1)
      first[pos++] = c;
    else if (part == 1 && pos < (int)sizeof(second) - 1)
      second[pos++] = c;
  }
  if (part == 0)
    first[pos] = '\0';
  else if (part == 1)
    second[pos] = '\0';
  const char *tag = (first[0] && second[0] && module_export_stop_word(first))
                        ? second
                        : first;
  if (tag && tag[0] && strcmp(tag, "std") != 0)
    module_summary_add_tag(st, tag);
}

static void module_summary_collect_exports(const char *decl_tail, size_t n,
                                           module_export_summary_t *st) {
  if (!decl_tail || !st)
    return;
  size_t p = 0;
  while (p < n && isspace((unsigned char)decl_tail[p]))
    p++;
  if (p >= n || decl_tail[p] != '(')
    return;
  p++;
  int depth = 1;
  while (p < n && depth > 0) {
    if (decl_tail[p] == ';') {
      while (p < n && decl_tail[p] != '\n')
        p++;
      continue;
    }
    if (decl_tail[p] == '(') {
      depth++;
      p++;
      continue;
    }
    if (decl_tail[p] == ')') {
      depth--;
      p++;
      continue;
    }
    if (depth == 1 &&
        (isalpha((unsigned char)decl_tail[p]) || decl_tail[p] == '_')) {
      size_t s = p;
      p++;
      while (p < n &&
             (isalnum((unsigned char)decl_tail[p]) || decl_tail[p] == '_'))
        p++;
      char *name = strndup0(decl_tail + s, p - s);
      module_summary_add_export(st, name);
      free(name);
      continue;
    }
    p++;
  }
}

static void module_summary_append_topic(sb_t *out, const char *mod_name) {
  const char *p = mod_name ? mod_name : "";
  if (strncmp(p, "std.", 4) == 0)
    p += 4;
  int any = 0;
  for (; *p; p++) {
    if (*p == '.') {
      sb_add(out, any ? " / " : "");
      any = 0;
    } else if (*p == '_' || *p == '-') {
      sb_add(out, " ");
      any = 1;
    } else {
      char c[2] = {*p, 0};
      sb_add(out, c);
      any = 1;
    }
  }
  if (!any)
    sb_add(out, "runtime");
}

static void module_summary_append_tags(sb_t *out, module_export_summary_t *st) {
  int used[64] = {0};
  int written = 0;
  for (;;) {
    int best = -1;
    for (int i = 0; i < st->tag_n; i++) {
      if (!used[i] && (best < 0 || st->tag_counts[i] > st->tag_counts[best]))
        best = i;
    }
    if (best < 0 || written >= 8)
      break;
    used[best] = 1;
    if (written)
      sb_add(out, ", ");
    sb_add(out, st->tags[best]);
    written++;
  }
}

static char *synthesize_module_doc(const char *mod_name, const char *decl_tail,
                                   size_t decl_tail_n) {
  (void)mod_name;
  (void)decl_tail;
  (void)decl_tail_n;
  return strdup("");
}

static char *module_doc_with_generated_summary(const char *mod_name,
                                               const char *mod_doc,
                                               const char *decl_tail,
                                               size_t decl_tail_n) {
  (void)mod_name;
  (void)decl_tail;
  (void)decl_tail_n;
  return strdup(mod_doc ? mod_doc : "");
}

static char *parse_docs_json(const char *bundle, const char *root,
                             int *module_count, int *symbol_count) {
  sb_t json = {0};
  sb_add(&json,
         "[{\"name\":\"Overview\",\"module_doc\":\"Nytrix library reference.\","
         "\"symbols\":[],\"path\":[\"Home\"],\"markdown_docs\":");
  append_markdown_docs_json(&json, root);
  sb_add(&json, "}");

  size_t len = strlen(bundle);
  *module_count = 0;
  *symbol_count = 0;
  for (size_t chunk_start = 0; chunk_start < len;) {
    size_t chunk_end = len;
    char orig[PATH_MAX] = "unknown";
    int had_marker = 0;
    for (size_t i = chunk_start; i < len; i++) {
      if ((i == 0 || bundle[i - 1] == '\n') &&
          (starts_with_at(bundle, i, "#line ") ||
           starts_with_at(bundle, i, ";; Module from "))) {
        had_marker = 1;
        size_t line_end = i;
        while (line_end < len && bundle[line_end] != '\n')
          line_end++;
        if (starts_with_at(bundle, i, "#line ")) {
          const char *q1 = memchr(bundle + i, '"', line_end - i);
          const char *q2 =
              q1 ? memchr(q1 + 1, '"', (size_t)(bundle + line_end - q1 - 1))
                 : NULL;
          if (q1 && q2) {
            size_t n = (size_t)(q2 - q1 - 1);
            if (n >= sizeof(orig))
              n = sizeof(orig) - 1;
            memcpy(orig, q1 + 1, n);
            orig[n] = '\0';
          }
        } else {
          const char *p = bundle + i + strlen(";; Module from ");
          size_t n = (size_t)(bundle + line_end - p);
          if (n >= sizeof(orig))
            n = sizeof(orig) - 1;
          memcpy(orig, p, n);
          orig[n] = '\0';
        }
        chunk_start = line_end < len ? line_end + 1 : line_end;
        break;
      }
    }
    if (!had_marker && chunk_start != 0)
      break;
    if (had_marker) {
      chunk_end = len;
      for (size_t i = chunk_start; i < len; i++) {
        if ((i == 0 || bundle[i - 1] == '\n') &&
            (starts_with_at(bundle, i, "#line ") ||
             starts_with_at(bundle, i, ";; Module from "))) {
          chunk_end = i;
          break;
        }
      }
    }

    const char *body = bundle + chunk_start;
    size_t body_n = chunk_end - chunk_start;
    if (!docs_origin_is_lib(root, orig)) {
      chunk_start = chunk_end;
      continue;
    }
    const char *mm = NULL;
    for (size_t i = 0; i + 7 < body_n; i++) {
      if (is_line_decl_at(body, i, "module ")) {
        mm = body + i;
        break;
      }
    }
    if (!mm) {
      char *mod_name = docs_origin_module_name(root, orig);
      char *mod_doc = leading_file_comments(body, body_n);
      sb_add(&json, ",{\"name\":");
      sb_add_json_str(&json, mod_name);
      sb_add(&json, ",\"module_doc\":");
      sb_add_json_str(&json, mod_doc ? mod_doc : "");
      sb_add(&json, ",\"symbols\":[");
      int first_sym = 1;
      char *seen_ids[20000];
      int seen_id_n = 0;

      for (size_t i = 0; i + 3 < body_n; i++) {
        ny_web_fn_decl_t fn = {0};
        if (!parse_fn_decl_at(body, body_n, i, &fn))
          continue;
        size_t p = fn.body_open;
        size_t closep = fn.body_close;
        char *doc = leading_comments(body, i);
        size_t code_start = 0;
        char *inner =
            extract_docstring(body + p + 1, closep - p - 1, &code_start);
        sb_t doc2 = {0};
        if (doc && *doc)
          sb_add(&doc2, doc);
        if (inner && *inner) {
          if (doc2.len)
            sb_add(&doc2, "\n\n");
          sb_add(&doc2, inner);
        }
        char *code = dedent_clean_code(body + p + 1 + code_start,
                                       closep - p - 1 - code_start);
        sb_t sig = {0};
        append_fn_signature(&sig, fn.name, &fn, body);
        append_symbol_json(
            &json, fn.name, sig.data, "function", doc2.data ? doc2.data : "",
            code, body + p + 1, closep - p - 1, seen_ids, &seen_id_n,
            (int)(sizeof(seen_ids) / sizeof(seen_ids[0])), &first_sym);
        (*symbol_count)++;
        ny_web_fn_decl_free(&fn);
        free(doc);
        free(inner);
        free(doc2.data);
        free(code);
        free(sig.data);
        i = closep;
      }

      sb_add(&json, "],\"path\":[");
      char *tmp = strdup(mod_name);
      int first_path = 1;
      for (char *part = strtok(tmp, "."); part; part = strtok(NULL, ".")) {
        if (!first_path)
          sb_add(&json, ",");
        first_path = 0;
        sb_add_json_str(&json, part);
      }
      free(tmp);
      sb_add(&json, "],\"orig_file\":");
      char *rel_orig = docs_origin_relative(root, orig);
      sb_add_json_str(&json, rel_orig ? rel_orig : orig);
      free(rel_orig);
      sb_add(&json, ",\"source\":");
      size_t source_n = 0;
      char *source_txt = ny_read_file_raw(orig, &source_n);
      if (source_txt) {
        sb_add_json_strn(&json, source_txt, source_n);
        free(source_txt);
      } else {
        sb_add_json_strn(&json, body, body_n);
      }
      sb_add(&json, "}");
      (*module_count)++;
      for (int sid = 0; sid < seen_id_n; sid++)
        free(seen_ids[sid]);
      free(mod_name);
      free(mod_doc);
      if (!had_marker)
        break;
      chunk_start = chunk_end;
      continue;
    }
    if (mm) {
      size_t mp = (size_t)(mm - body) + 7;
      while (mp < body_n && isspace((unsigned char)body[mp]))
        mp++;
      size_t name_start = mp;
      while (mp < body_n && ny_symbol_path_char((unsigned char)body[mp]))
        mp++;
      char *mod_name = strndup0(body + name_start, mp - name_start);
      char *mod_doc = leading_comments(body, (size_t)(mm - body));
      size_t mdp = mp;
      while (mdp < body_n && isspace((unsigned char)body[mdp]))
        mdp++;
      if (mdp < body_n && body[mdp] == '(') {
        int depth = 1;
        mdp++;
        while (mdp < body_n && depth) {
          if (body[mdp] == '(')
            depth++;
          else if (body[mdp] == ')')
            depth--;
          mdp++;
        }
      }
      while (mdp < body_n && isspace((unsigned char)body[mdp]))
        mdp++;
      if (mdp < body_n && body[mdp] == '{')
        mdp++;
      size_t unused_code_start = 0;
      char *body_doc =
          extract_docstring(body + mdp, body_n - mdp, &unused_code_start);
      if (body_doc && *body_doc) {
        sb_t merged = {0};
        if (mod_doc && *mod_doc) {
          sb_add(&merged, mod_doc);
          sb_add(&merged, "\n\n");
        }
        sb_add(&merged, body_doc);
        free(mod_doc);
        mod_doc = merged.data ? merged.data : strdup("");
      }
      free(body_doc);
      char *generated_mod_doc = module_doc_with_generated_summary(
          mod_name, mod_doc, body + mp, body_n - mp);
      free(mod_doc);
      mod_doc = generated_mod_doc;
      sb_add(&json, ",{\"name\":");
      sb_add_json_str(&json, mod_name);
      sb_add(&json, ",\"module_doc\":");
      sb_add_json_str(&json, mod_doc);
      sb_add(&json, ",\"symbols\":[");
      int first_sym = 1;
      char *seen_ids[20000];
      int seen_id_n = 0;
      size_t ranges[20000][2];
      int range_n = 0;

      for (size_t i = 0; i + 3 < body_n; i++) {
        ny_web_fn_decl_t fn = {0};
        if (!parse_fn_decl_at(body, body_n, i, &fn))
          continue;
        size_t p = fn.body_open;
        size_t closep = fn.body_close;
        if (range_n < (int)(sizeof(ranges) / sizeof(ranges[0]))) {
          ranges[range_n][0] = i;
          ranges[range_n][1] = closep;
          range_n++;
        }
        char *doc = leading_comments(body, i);
        size_t code_start = 0;
        char *inner =
            extract_docstring(body + p + 1, closep - p - 1, &code_start);
        sb_t doc2 = {0};
        if (doc && *doc)
          sb_add(&doc2, doc);
        if (inner && *inner) {
          if (doc2.len)
            sb_add(&doc2, "\n\n");
          sb_add(&doc2, inner);
        }
        char *code = dedent_clean_code(body + p + 1 + code_start,
                                       closep - p - 1 - code_start);
        sb_t sig = {0};
        append_fn_signature(&sig, fn.name, &fn, body);
        append_symbol_json(
            &json, fn.name, sig.data, "function", doc2.data ? doc2.data : "",
            code, body + p + 1, closep - p - 1, seen_ids, &seen_id_n,
            (int)(sizeof(seen_ids) / sizeof(seen_ids[0])), &first_sym);
        (*symbol_count)++;
        ny_web_fn_decl_free(&fn);
        free(doc);
        free(inner);
        free(doc2.data);
        free(code);
        free(sig.data);
        i = closep;
      }

      const char *kinds[] = {"struct", "layout", "enum"};
      for (int kk = 0; kk < 3; kk++) {
        const char *kw = kinds[kk];
        size_t kwl = strlen(kw);
        char decl_kw[16];
        snprintf(decl_kw, sizeof(decl_kw), "%s ", kw);
        for (size_t i = 0; i + kwl + 1 < body_n; i++) {
          if (!is_line_decl_at(body, i, decl_kw))
            continue;
          int inside = 0;
          for (int r = 0; r < range_n; r++)
            if (ranges[r][0] <= i && i <= ranges[r][1])
              inside = 1;
          if (inside)
            continue;
          size_t p = i + kwl;
          while (p < body_n && isspace((unsigned char)body[p]))
            p++;
          size_t ns = p;
          while (p < body_n &&
                 (isalnum((unsigned char)body[p]) || body[p] == '_'))
            p++;
          if (p == ns)
            continue;
          char *nm = strndup0(body + ns, p - ns);
          const char *open = memchr(body + p, '{', body_n - p);
          if (!open) {
            free(nm);
            continue;
          }
          size_t op = (size_t)(open - body), closep = 0;
          if (!find_matching_brace(body, op, body_n, &closep)) {
            free(nm);
            continue;
          }
          char *doc = leading_comments(body, i);
          if (!doc || !*doc) {
            free(doc);
            sb_t d = {0};
            sb_add(&d, kw);
            sb_add(&d, " definition.");
            doc = d.data;
          }
          sb_t code = {0}, name = {0};
          char *inner = dedent_clean_code(body + op + 1, closep - op - 1);
          sb_add(&code, kw);
          sb_add(&code, " ");
          sb_add(&code, nm);
          sb_add(&code, " {\n");
          sb_add(&code, inner ? inner : "");
          sb_add(&code, "\n}");
          sb_add(&name, kw);
          sb_add(&name, " ");
          sb_add(&name, nm);
          append_symbol_json(&json, nm, name.data, kw, doc, code.data, NULL, 0,
                             seen_ids, &seen_id_n,
                             (int)(sizeof(seen_ids) / sizeof(seen_ids[0])),
                             &first_sym);
          (*symbol_count)++;
          free(nm);
          free(doc);
          free(inner);
          free(code.data);
          free(name.data);
          i = closep;
        }
      }

      const char *topkinds[] = {"extern fn ", "def ", "mut ", "alias "};
      for (int tk = 0; tk < 4; tk++) {
        const char *kw = topkinds[tk];
        size_t kwl = strlen(kw);
        for (size_t i = 0; i + kwl < body_n; i++) {
          if (!is_line_decl_at(body, i, kw))
            continue;
          int inside = 0;
          for (int r = 0; r < range_n; r++)
            if (ranges[r][0] <= i && i <= ranges[r][1])
              inside = 1;
          if (inside)
            continue;
          size_t p = i + kwl, name_end = p;
          char *nm = (tk == 1 || tk == 2)
                         ? parse_value_decl_name(body, body_n, p, &name_end)
                         : NULL;
          if (!nm) {
            size_t ns = p;
            while (p < body_n &&
                   (isalnum((unsigned char)body[p]) || body[p] == '_'))
              p++;
            if (p > ns) {
              nm = strndup0(body + ns, p - ns);
              name_end = p;
            }
          }
          if (!nm)
            continue;
          size_t le = name_end;
          while (le < body_n && body[le] != '\n')
            le++;
          char *code = trim_copy(body + i, le - i);
          char *doc = leading_comments(body, i);
          const char *kind = tk == 0   ? "extern"
                             : tk == 1 ? "constant"
                             : tk == 2 ? "variable"
                                       : "alias";
          if (!doc || !*doc) {
            free(doc);
            sb_t d = {0};
            sb_add(&d, kind);
            sb_add(&d, " definition.");
            doc = d.data;
          }
          sb_t name = {0};
          if (tk == 0)
            sb_add(&name, code);
          else if (tk == 3) {
            sb_add(&name, "alias ");
            sb_add(&name, nm);
          } else
            sb_add(&name, nm);
          append_symbol_json(&json, nm, name.data, kind, doc, code, NULL, 0,
                             seen_ids, &seen_id_n,
                             (int)(sizeof(seen_ids) / sizeof(seen_ids[0])),
                             &first_sym);
          (*symbol_count)++;
          free(nm);
          free(code);
          free(doc);
          free(name.data);
          i = le;
        }
      }

      sb_add(&json, "],\"path\":[");
      char *tmp = strdup(mod_name);
      int first_path = 1;
      for (char *part = strtok(tmp, "."); part; part = strtok(NULL, ".")) {
        if (!first_path)
          sb_add(&json, ",");
        first_path = 0;
        sb_add_json_str(&json, part);
      }
      free(tmp);
      sb_add(&json, "],\"orig_file\":");
      char *rel_orig = docs_origin_relative(root, orig);
      sb_add_json_str(&json, rel_orig ? rel_orig : orig);
      free(rel_orig);
      sb_add(&json, ",\"source\":");
      size_t source_n = 0;
      char *source_txt = ny_read_file_raw(orig, &source_n);
      if (source_txt) {
        sb_add_json_strn(&json, source_txt, source_n);
        free(source_txt);
      } else {
        sb_add_json_strn(&json, body, body_n);
      }
      sb_add(&json, "}");
      (*module_count)++;
      for (int sid = 0; sid < seen_id_n; sid++)
        free(seen_ids[sid]);
      free(mod_name);
      free(mod_doc);
    }
    if (!had_marker)
      break;
    chunk_start = chunk_end;
  }
  sb_add(&json, "]");
  return json.data;
}

typedef struct {
  char *kind;
  char *name;
  char *full_name;
  char *path;
  char *signature;
  char *keywords;
  char *doc;
  char *code;
  int line;
  int score;
} doc_search_entry_t;

typedef struct {
  doc_search_entry_t *items;
  size_t len;
  size_t cap;
} doc_search_index_t;

static void doc_search_index_free(doc_search_index_t *idx) {
  if (!idx)
    return;
  for (size_t i = 0; i < idx->len; i++) {
    free(idx->items[i].kind);
    free(idx->items[i].name);
    free(idx->items[i].full_name);
    free(idx->items[i].path);
    free(idx->items[i].signature);
    free(idx->items[i].keywords);
    free(idx->items[i].doc);
    free(idx->items[i].code);
  }
  free(idx->items);
  idx->items = NULL;
  idx->len = 0;
  idx->cap = 0;
}

static int doc_keyword_line_value(const char *line, size_t n,
                                  const char **value, size_t *value_n) {
  size_t p = 0;
  while (p < n && isspace((unsigned char)line[p]))
    p++;
  while (p < n && (line[p] == ';' || line[p] == '#' || line[p] == '-')) {
    p++;
    while (p < n && isspace((unsigned char)line[p]))
      p++;
  }
  if (p + 8 > n || strncasecmp(line + p, "Keywords", 8) != 0)
    return 0;
  p += 8;
  while (p < n && isspace((unsigned char)line[p]))
    p++;
  if (p >= n || (line[p] != ':' && line[p] != '='))
    return 0;
  p++;
  while (p < n && isspace((unsigned char)line[p]))
    p++;
  size_t e = n;
  while (e > p && isspace((unsigned char)line[e - 1]))
    e--;
  if (value)
    *value = line + p;
  if (value_n)
    *value_n = e - p;
  return 1;
}

static char *doc_extract_keywords(const char *txt) {
  if (!txt || !*txt)
    return strdup("");
  sb_t out = {0};
  for (const char *line = txt; *line;) {
    const char *next = strchr(line, '\n');
    size_t n = next ? (size_t)(next - line) : strlen(line);
    const char *value = NULL;
    size_t value_n = 0;
    if (doc_keyword_line_value(line, n, &value, &value_n) && value_n > 0) {
      if (out.len)
        sb_add(&out, " ");
      sb_addn(&out, value, value_n);
    }
    if (!next)
      break;
    line = next + 1;
  }
  return out.data ? out.data : strdup("");
}

static char *doc_without_keyword_lines(const char *txt) {
  if (!txt || !*txt)
    return strdup("");
  sb_t out = {0};
  for (const char *line = txt; *line;) {
    const char *next = strchr(line, '\n');
    size_t n = next ? (size_t)(next - line) : strlen(line);
    if (!doc_keyword_line_value(line, n, NULL, NULL)) {
      if (out.len)
        sb_add(&out, "\n");
      sb_addn(&out, line, n);
    }
    if (!next)
      break;
    line = next + 1;
  }
  return out.data ? out.data : strdup("");
}

static char *doc_merge_keywords(const char *inherited, const char *local) {
  int has_inherited = inherited && *inherited;
  int has_local = local && *local;
  if (!has_inherited && !has_local)
    return strdup("");
  if (!has_inherited)
    return strdup(local);
  if (!has_local)
    return strdup(inherited);
  sb_t out = {0};
  sb_add(&out, inherited);
  sb_add(&out, " ");
  sb_add(&out, local);
  return out.data ? out.data : strdup("");
}

static int doc_search_add(doc_search_index_t *idx, const char *kind,
                          const char *name, const char *full_name,
                          const char *path, int line, const char *signature,
                          const char *keywords, const char *doc,
                          const char *code) {
  if (!idx || !name || !*name)
    return 0;
  const char *entry_kind = kind ? kind : "doc";
  const char *entry_full = (full_name && *full_name) ? full_name : name;
  const char *entry_path = path ? path : "";
  const char *entry_sig = signature ? signature : "";
  char *local_keywords = doc_extract_keywords(doc);
  char *entry_keywords_owned = doc_merge_keywords(keywords, local_keywords);
  char *entry_doc_owned = doc_without_keyword_lines(doc);
  const char *entry_keywords = entry_keywords_owned ? entry_keywords_owned : "";
  const char *entry_doc = entry_doc_owned ? entry_doc_owned : "";
  for (size_t i = 0; i < idx->len; i++) {
    doc_search_entry_t *old = &idx->items[i];
    if (old->line == line && strcmp(old->kind, entry_kind) == 0 &&
        strcmp(old->full_name, entry_full) == 0 &&
        strcmp(old->path, entry_path) == 0 &&
        strcmp(old->signature, entry_sig) == 0 &&
        strcmp(old->keywords ? old->keywords : "", entry_keywords) == 0) {
      free(local_keywords);
      free(entry_keywords_owned);
      free(entry_doc_owned);
      return 1;
    }
  }
  if (idx->len == idx->cap) {
    size_t next_cap = idx->cap ? idx->cap * 2 : 256;
    doc_search_entry_t *next = (doc_search_entry_t *)realloc(
        idx->items, next_cap * sizeof(doc_search_entry_t));
    if (!next) {
      free(local_keywords);
      free(entry_keywords_owned);
      free(entry_doc_owned);
      return 0;
    }
    idx->items = next;
    idx->cap = next_cap;
  }
  doc_search_entry_t *e = &idx->items[idx->len++];
  memset(e, 0, sizeof(*e));
  e->kind = strdup(entry_kind);
  e->name = strdup(name);
  e->full_name = strdup(entry_full);
  e->path = strdup(entry_path);
  e->signature = strdup(entry_sig);
  e->keywords = strdup(entry_keywords);
  e->doc = strdup(entry_doc);
  e->code = strdup(code ? code : "");
  e->line = line;
  free(local_keywords);
  free(entry_keywords_owned);
  free(entry_doc_owned);
  return e->kind && e->name && e->full_name && e->path && e->signature &&
         e->keywords && e->doc && e->code;
}

static char *display_path_for(const char *root, const char *path) {
  char cwd[PATH_MAX];
  if (path && *path && getcwd(cwd, sizeof(cwd))) {
    size_t n = strlen(cwd);
    if (strncmp(path, cwd, n) == 0 &&
        (path[n] == '/' || path[n] == '\\' || path[n] == '\0')) {
      const char *rel = path + n;
      while (*rel == '/' || *rel == '\\')
        rel++;
      sb_t out = {0};
      if (*rel)
        sb_add(&out, rel);
      else
        sb_add(&out, ".");
      return out.data ? out.data : strdup(path);
    }
  }
  if (root && path && *path) {
    size_t n = strlen(root);
    if (strncmp(path, root, n) == 0 &&
        (path[n] == '/' || path[n] == '\\' || path[n] == '\0')) {
      const char *rel = path + n;
      while (*rel == '/' || *rel == '\\')
        rel++;
      return strdup(*rel ? rel : ".");
    }
  }
  return strdup(path ? path : "");
}

static int line_number_at(const char *s, size_t pos) {
  int line = 1;
  for (size_t i = 0; i < pos; i++)
    if (s[i] == '\n')
      line++;
  return line;
}

static char *join_args_query(int start, int argc, char **argv) {
  sb_t q = {0};
  for (int i = start; i < argc; i++) {
    if (q.len)
      sb_add(&q, " ");
    sb_add(&q, argv[i]);
  }
  return q.data ? q.data : strdup("");
}

static int docs_file_supported(const char *name) {
  return path_has_suffix(name, ".md") || path_has_suffix(name, ".txt") ||
         path_has_suffix(name, ".texi");
}

static char *markdown_heading_title(const char *line, size_t n) {
  size_t p = 0;
  while (p < n && p < 3 && (line[p] == ' ' || line[p] == '\t'))
    p++;
  size_t hashes = 0;
  while (p + hashes < n && line[p + hashes] == '#')
    hashes++;
  if (hashes == 0 || hashes > 6 || p + hashes >= n ||
      !isspace((unsigned char)line[p + hashes]))
    return NULL;
  p += hashes;
  while (p < n && isspace((unsigned char)line[p]))
    p++;
  size_t e = n;
  while (e > p && isspace((unsigned char)line[e - 1]))
    e--;
  while (e > p && line[e - 1] == '#')
    e--;
  while (e > p && isspace((unsigned char)line[e - 1]))
    e--;
  return e > p ? strndup0(line + p, e - p) : NULL;
}

static char *first_nonblank_excerpt(const char *s, size_t n) {
  size_t p = 0;
  while (p < n) {
    while (p < n && isspace((unsigned char)s[p]))
      p++;
    size_t e = p;
    while (e < n && s[e] != '\n')
      e++;
    size_t a = p, b = e;
    while (a < b && isspace((unsigned char)s[a]))
      a++;
    while (b > a && isspace((unsigned char)s[b - 1]))
      b--;
    if (b > a) {
      size_t len = b - a;
      if (len > 800)
        len = 800;
      return strndup0(s + a, len);
    }
    p = e < n ? e + 1 : e;
  }
  return strdup("");
}

static void add_markdown_doc_entries(doc_search_index_t *idx, const char *root,
                                     const char *path, const char *txt) {
  char *display = display_path_for(root, path);
  char *stem = stem_name(path);
  if (!display || !stem) {
    free(display);
    free(stem);
    return;
  }

  int saw_heading = 0;
  size_t len = strlen(txt);
  size_t section_start = 0;
  int section_line = 1;
  char *section_title = NULL;
  int line = 1;

  for (size_t pos = 0; pos <= len;) {
    size_t line_start = pos;
    while (pos < len && txt[pos] != '\n')
      pos++;
    size_t line_len = pos - line_start;
    char *title = markdown_heading_title(txt + line_start, line_len);
    if (title) {
      saw_heading = 1;
      if (section_title) {
        char *section =
            trim_copy(txt + section_start, line_start - section_start);
        sb_t full = {0};
        sb_add(&full, display);
        sb_add(&full, "#");
        sb_add(&full, section_title);
        doc_search_add(idx, "doc", section_title,
                       full.data ? full.data : section_title, display,
                       section_line, "", NULL, section ? section : "", "");
        free(full.data);
        free(section);
      }
      free(section_title);
      section_title = title;
      section_start = line_start;
      section_line = line;
    }
    if (pos >= len)
      break;
    pos++;
    line++;
  }
  if (section_title) {
    char *section = trim_copy(txt + section_start, len - section_start);
    sb_t full = {0};
    sb_add(&full, display);
    sb_add(&full, "#");
    sb_add(&full, section_title);
    doc_search_add(idx, "doc", section_title,
                   full.data ? full.data : section_title, display, section_line,
                   "", NULL, section ? section : "", "");
    free(full.data);
    free(section);
    free(section_title);
  }
  if (!saw_heading) {
    char *body = trim_copy(txt, len);
    doc_search_add(idx, "doc", stem, display, display, 1, "", NULL,
                   body ? body : "", "");
    free(body);
  }
  free(display);
  free(stem);
}

static void add_plain_doc_entry(doc_search_index_t *idx, const char *root,
                                const char *path, const char *txt) {
  char *display = display_path_for(root, path);
  char *stem = stem_name(path);
  char *excerpt = trim_copy(txt, strlen(txt));
  if (display && stem)
    doc_search_add(idx, "doc", stem, display, display, 1, "", NULL,
                   excerpt ? excerpt : "", "");
  free(display);
  free(stem);
  free(excerpt);
}

static void collect_doc_files(doc_search_index_t *idx, const char *root,
                              const char *dir) {
  DIR *d = opendir(dir);
  if (!d)
    return;
  char **names = NULL;
  size_t len = 0, cap = 0;
  struct dirent *ent;
  while ((ent = readdir(d))) {
    if (web_skip_source_dir(ent->d_name))
      continue;
    if (len == cap) {
      size_t next_cap = cap ? cap * 2 : 64;
      char **next = (char **)realloc(names, next_cap * sizeof(char *));
      if (!next)
        break;
      names = next;
      cap = next_cap;
    }
    names[len++] = strdup(ent->d_name);
  }
  closedir(d);
  qsort(names, len, sizeof(*names), cmp_cstr_ptr);
  for (size_t i = 0; i < len; i++) {
    char path[PATH_MAX];
    if (join_path(path, sizeof(path), dir, names[i])) {
      if (path_is_dir(path)) {
        collect_doc_files(idx, root, path);
      } else if (docs_file_supported(path)) {
        char *txt = ny_read_file_raw(path, NULL);
        if (txt) {
          if (path_has_suffix(path, ".md"))
            add_markdown_doc_entries(idx, root, path, txt);
          else
            add_plain_doc_entry(idx, root, path, txt);
          free(txt);
        }
      }
    }
    free(names[i]);
  }
  free(names);
}

static void collect_named_doc_dirs(doc_search_index_t *idx, const char *root,
                                   const char *dir, const char *wanted_name) {
  const char *base = path_basename(dir);
  if (base && strcmp(base, wanted_name) == 0) {
    collect_doc_files(idx, root, dir);
    return;
  }

  DIR *d = opendir(dir);
  if (!d)
    return;
  char **names = NULL;
  size_t len = 0, cap = 0;
  struct dirent *ent;
  while ((ent = readdir(d))) {
    if (web_skip_source_dir(ent->d_name))
      continue;
    if (len == cap) {
      size_t next_cap = cap ? cap * 2 : 64;
      char **next = (char **)realloc(names, next_cap * sizeof(char *));
      if (!next)
        break;
      names = next;
      cap = next_cap;
    }
    names[len++] = strdup(ent->d_name);
  }
  closedir(d);
  qsort(names, len, sizeof(*names), cmp_cstr_ptr);
  for (size_t i = 0; i < len; i++) {
    char path[PATH_MAX];
    if (join_path(path, sizeof(path), dir, names[i]) && path_is_dir(path))
      collect_named_doc_dirs(idx, root, path, wanted_name);
    free(names[i]);
  }
  free(names);
}

static void collect_all_docs(doc_search_index_t *idx, const char *root) {
  collect_named_doc_dirs(idx, root, root, "docs");
  collect_named_doc_dirs(idx, root, root, "status");

  const char *info_dir = getenv("NYTRIX_DOC_INFO_DIR");
  if (!info_dir || !*info_dir)
    info_dir = getenv("NYTRIX_WEBDOC_INFO_DIR");
  if (info_dir && *info_dir)
    collect_doc_files(idx, root, info_dir);
}

static void append_symbol_entries_from_bundle(doc_search_index_t *idx,
                                              const char *bundle,
                                              const char *root,
                                              int *module_count,
                                              int *symbol_count) {
  size_t len = strlen(bundle);
  if (module_count)
    *module_count = 0;
  if (symbol_count)
    *symbol_count = 0;
  for (size_t chunk_start = 0; chunk_start < len;) {
    size_t chunk_end = len;
    char orig[PATH_MAX] = "unknown";
    int had_marker = 0;
    for (size_t i = chunk_start; i < len; i++) {
      if ((i == 0 || bundle[i - 1] == '\n') &&
          (starts_with_at(bundle, i, "#line ") ||
           starts_with_at(bundle, i, ";; Module from "))) {
        had_marker = 1;
        size_t line_end = i;
        while (line_end < len && bundle[line_end] != '\n')
          line_end++;
        if (starts_with_at(bundle, i, "#line ")) {
          const char *q1 = memchr(bundle + i, '"', line_end - i);
          const char *q2 =
              q1 ? memchr(q1 + 1, '"', (size_t)(bundle + line_end - q1 - 1))
                 : NULL;
          if (q1 && q2) {
            size_t n = (size_t)(q2 - q1 - 1);
            if (n >= sizeof(orig))
              n = sizeof(orig) - 1;
            memcpy(orig, q1 + 1, n);
            orig[n] = '\0';
          }
        } else {
          const char *p = bundle + i + strlen(";; Module from ");
          size_t n = (size_t)(bundle + line_end - p);
          if (n >= sizeof(orig))
            n = sizeof(orig) - 1;
          memcpy(orig, p, n);
          orig[n] = '\0';
        }
        chunk_start = line_end < len ? line_end + 1 : line_end;
        break;
      }
    }
    if (!had_marker && chunk_start != 0)
      break;
    if (had_marker) {
      chunk_end = len;
      for (size_t i = chunk_start; i < len; i++) {
        if ((i == 0 || bundle[i - 1] == '\n') &&
            (starts_with_at(bundle, i, "#line ") ||
             starts_with_at(bundle, i, ";; Module from "))) {
          chunk_end = i;
          break;
        }
      }
    }

    const char *body = bundle + chunk_start;
    size_t body_n = chunk_end - chunk_start;
    if (!docs_origin_is_lib(root, orig)) {
      chunk_start = chunk_end;
      continue;
    }

    const char *mm = NULL;
    for (size_t i = 0; i + 7 < body_n; i++) {
      if (is_line_decl_at(body, i, "module ")) {
        mm = body + i;
        break;
      }
    }
    if (!mm) {
      char *doc = leading_file_comments(body, body_n);
      char *rel_orig = docs_origin_relative(root, orig);
      char *stem = stem_name(rel_orig ? rel_orig : orig);
      char *mod_name = docs_origin_module_name(root, orig);
      char *mod_keywords = doc_extract_keywords(doc);
      if (doc && *doc) {
        char *code = dedent_clean_code(body, body_n);
        if (stem && *stem) {
          doc_search_add(idx, "script", stem, rel_orig ? rel_orig : orig,
                         rel_orig ? rel_orig : orig, 1, "", NULL, doc,
                         code ? code : "");
          if (symbol_count)
            (*symbol_count)++;
        }
        free(code);
      }
      for (size_t i = 0; i + 3 < body_n; i++) {
        ny_web_fn_decl_t fn = {0};
        if (!parse_fn_decl_at(body, body_n, i, &fn))
          continue;
        size_t p = fn.body_open;
        size_t closep = fn.body_close;
        char *fn_doc = leading_comments(body, i);
        size_t code_start = 0;
        char *inner =
            extract_docstring(body + p + 1, closep - p - 1, &code_start);
        sb_t doc2 = {0};
        if (fn_doc && *fn_doc)
          sb_add(&doc2, fn_doc);
        if (inner && *inner) {
          if (doc2.len)
            sb_add(&doc2, "\n\n");
          sb_add(&doc2, inner);
        }
        sb_t display = {0}, sig = {0}, full = {0};
        if (mod_name && *mod_name && !strchr(fn.name, '.')) {
          sb_add(&display, mod_name);
          sb_add(&display, ".");
          sb_add(&full, mod_name);
          sb_add(&full, ".");
        }
        sb_add(&display, fn.name);
        sb_add(&full, fn.name);
        append_fn_signature(&sig, display.data ? display.data : fn.name, &fn,
                            body);
        char *code = dedent_clean_code(body + i, closep - i + 1);
        doc_search_add(idx, "function", fn.name, full.data ? full.data : fn.name,
                       rel_orig ? rel_orig : orig, line_number_at(body, i),
                       sig.data ? sig.data : fn.name, mod_keywords,
                       doc2.data ? doc2.data : "", code ? code : "");
        if (symbol_count)
          (*symbol_count)++;
        ny_web_fn_decl_free(&fn);
        free(fn_doc);
        free(inner);
        free(doc2.data);
        free(display.data);
        free(sig.data);
        free(full.data);
        free(code);
        i = closep;
      }
      free(rel_orig);
      free(stem);
      free(mod_name);
      free(mod_keywords);
      free(doc);
      if (!had_marker)
        break;
      chunk_start = chunk_end;
      continue;
    }

    size_t mp = (size_t)(mm - body) + 7;
    while (mp < body_n && isspace((unsigned char)body[mp]))
      mp++;
    size_t name_start = mp;
    while (mp < body_n && ny_symbol_path_char((unsigned char)body[mp]))
      mp++;
    char *mod_name = strndup0(body + name_start, mp - name_start);
    char *mod_doc = leading_comments(body, (size_t)(mm - body));
    char *generated_mod_doc = module_doc_with_generated_summary(
        mod_name, mod_doc, body + mp, body_n - mp);
    free(mod_doc);
    mod_doc = generated_mod_doc;
    char *mod_keywords = doc_extract_keywords(mod_doc);
    char *rel_orig = docs_origin_relative(root, orig);
    int mod_line = line_number_at(body, (size_t)(mm - body));
    if (mod_name && *mod_name) {
      doc_search_add(idx, "module", mod_name, mod_name,
                     rel_orig ? rel_orig : orig, mod_line, "", NULL,
                     mod_doc ? mod_doc : "", "");
      if (module_count)
        (*module_count)++;
    }

    size_t ranges[20000][2];
    int range_n = 0;
    for (size_t i = 0; i + 3 < body_n; i++) {
      ny_web_fn_decl_t fn = {0};
      if (!parse_fn_decl_at(body, body_n, i, &fn))
        continue;
      size_t p = fn.body_open;
      size_t closep = fn.body_close;
      if (range_n < (int)(sizeof(ranges) / sizeof(ranges[0]))) {
        ranges[range_n][0] = i;
        ranges[range_n][1] = closep;
        range_n++;
      }
      char *doc = leading_comments(body, i);
      size_t code_start = 0;
      char *inner =
          extract_docstring(body + p + 1, closep - p - 1, &code_start);
      sb_t doc2 = {0};
      if (doc && *doc)
        sb_add(&doc2, doc);
      if (inner && *inner) {
        if (doc2.len)
          sb_add(&doc2, "\n\n");
        sb_add(&doc2, inner);
      }
      sb_t sig = {0};
      sb_t display = {0};
      if (mod_name && *mod_name && !strchr(fn.name, '.')) {
        sb_add(&display, mod_name);
        sb_add(&display, ".");
      }
      sb_add(&display, fn.name);
      append_fn_signature(&sig, display.data ? display.data : fn.name, &fn,
                          body);
      sb_t full = {0};
      if (mod_name && *mod_name && !strchr(fn.name, '.')) {
        sb_add(&full, mod_name);
        sb_add(&full, ".");
      }
      sb_add(&full, fn.name);
      char *code = dedent_clean_code(body + i, closep - i + 1);
      doc_search_add(idx, "function", fn.name, full.data ? full.data : fn.name,
                     rel_orig ? rel_orig : orig, line_number_at(body, i),
                     sig.data ? sig.data : fn.name, mod_keywords,
                     doc2.data ? doc2.data : "", code ? code : "");
      if (symbol_count)
        (*symbol_count)++;
      ny_web_fn_decl_free(&fn);
      free(doc);
      free(inner);
      free(doc2.data);
      free(sig.data);
      free(display.data);
      free(full.data);
      free(code);
      i = closep;
    }

    const char *kinds[] = {"struct", "layout", "enum"};
    for (int kk = 0; kk < 3; kk++) {
      const char *kw = kinds[kk];
      size_t kwl = strlen(kw);
      char decl_kw[16];
      snprintf(decl_kw, sizeof(decl_kw), "%s ", kw);
      for (size_t i = 0; i + kwl + 1 < body_n; i++) {
        if (!is_line_decl_at(body, i, decl_kw))
          continue;
        int inside = 0;
        for (int r = 0; r < range_n; r++)
          if (ranges[r][0] <= i && i <= ranges[r][1])
            inside = 1;
        if (inside)
          continue;
        size_t p = i + kwl;
        while (p < body_n && isspace((unsigned char)body[p]))
          p++;
        size_t ns = p;
        while (p < body_n &&
               (isalnum((unsigned char)body[p]) || body[p] == '_'))
          p++;
        if (p == ns)
          continue;
        char *nm = strndup0(body + ns, p - ns);
        const char *open = memchr(body + p, '{', body_n - p);
        if (!open) {
          free(nm);
          continue;
        }
        size_t op = (size_t)(open - body), closep = 0;
        if (!find_matching_brace(body, op, body_n, &closep)) {
          free(nm);
          continue;
        }
        char *doc = leading_comments(body, i);
        char *code = dedent_clean_code(body + i, closep - i + 1);
        sb_t full = {0}, sig = {0};
        if (mod_name && *mod_name) {
          sb_add(&full, mod_name);
          sb_add(&full, ".");
        }
        sb_add(&full, nm);
        sb_add(&sig, kw);
        sb_add(&sig, " ");
        sb_add(&sig, full.data ? full.data : nm);
        doc_search_add(idx, kw, nm, full.data ? full.data : nm,
                       rel_orig ? rel_orig : orig, line_number_at(body, i),
                       sig.data ? sig.data : nm, mod_keywords, doc ? doc : "",
                       code ? code : "");
        if (symbol_count)
          (*symbol_count)++;
        free(nm);
        free(doc);
        free(code);
        free(full.data);
        free(sig.data);
        i = closep;
      }
    }

    free(rel_orig);
    free(mod_name);
    free(mod_doc);
    free(mod_keywords);
    if (!had_marker)
      break;
    chunk_start = chunk_end;
  }
}

static int fuzzy_score_one(const char *cand, const char *query) {
  if (!cand || !*cand || !query || !*query)
    return 0;
  const char *hit = find_case(cand, query);
  if (hit) {
    int score = 10000;
    if (hit == cand)
      score += 2000;
    if (hit == cand || hit[-1] == '.' || hit[-1] == '/' || hit[-1] == '-' ||
        hit[-1] == '_' || isspace((unsigned char)hit[-1]))
      score += 700;
    size_t cn = strlen(cand), qn = strlen(query);
    if (cn > qn && cn - qn < 200)
      score += (int)(200 - (cn - qn));
    return score;
  }

  const unsigned char *c = (const unsigned char *)cand;
  const unsigned char *q = (const unsigned char *)query;
  int score = 0;
  int streak = 0;
  int last_match = -2;
  for (int qi = 0; q[qi]; qi++) {
    int qc = tolower(q[qi]);
    int found = 0;
    for (int ci = last_match + 1; c[ci]; ci++) {
      if (tolower(c[ci]) == qc) {
        found = 1;
        score += 20;
        if (ci == 0 || c[ci - 1] == '.' || c[ci - 1] == '/' ||
            c[ci - 1] == '-' || c[ci - 1] == '_' || isspace(c[ci - 1]))
          score += 20;
        if (ci == last_match + 1) {
          streak++;
          score += 15 + streak * 2;
        } else {
          streak = 0;
        }
        last_match = ci;
        break;
      }
    }
    if (!found)
      return 0;
  }
  return score;
}

static int field_match_score(const char *field, const char *term, int weight) {
  int s = fuzzy_score_one(field, term);
  return s > 0 ? s + weight : 0;
}

static int doc_search_query_has_alt(const char *query) {
  if (!query)
    return 0;
  for (const char *p = query; *p; p++) {
    if (*p == '|')
      return 1;
    if (*p == '\\' && p[1] == '|')
      return 1;
  }
  return 0;
}

static char *doc_search_query_terms_copy(const char *query) {
  const char *src = query ? query : "";
  size_t n = strlen(src);
  char *out = (char *)malloc(n + 1);
  if (!out)
    return NULL;
  size_t w = 0;
  for (size_t i = 0; i < n; i++) {
    if (src[i] == '\\' && i + 1 < n && src[i + 1] == '|') {
      out[w++] = ' ';
      i++;
    } else if (src[i] == '|') {
      out[w++] = ' ';
    } else {
      out[w++] = src[i];
    }
  }
  out[w] = '\0';
  return out;
}

static char *doc_search_trimmed_slice(const char *start, size_t len) {
  while (len && isspace((unsigned char)*start)) {
    start++;
    len--;
  }
  while (len && isspace((unsigned char)start[len - 1]))
    len--;
  if (!len)
    return NULL;
  return strndup0(start, len);
}

static int doc_entry_score_terms(const doc_search_entry_t *e,
                                 const char *query) {
  if (query && *query) {
    if (e->full_name && strcasecmp(e->full_name, query) == 0)
      return 1000000;
    if (e->name && strcasecmp(e->name, query) == 0)
      return 950000;
    const char *leaf = e->full_name ? strrchr(e->full_name, '.') : NULL;
    leaf = leaf ? leaf + 1 : NULL;
    if (leaf && strcasecmp(leaf, query) == 0)
      return 925000;
  }
  char *copy = strdup(query ? query : "");
  if (!copy)
    return 0;
  int total = 0;
  int terms = 0;
  int matched_terms = 0;
  int strong_terms = 0;
  for (char *term = strtok(copy, " \t\r\n"); term;
       term = strtok(NULL, " \t\r\n")) {
    int full_s = field_match_score(e->full_name, term, 600);
    int name_s = field_match_score(e->name, term, 550);
    int sig_s = field_match_score(e->signature, term, 450);
    int keyword_s = field_match_score(e->keywords, term, 700);
    int path_s = field_match_score(e->path, term, 300);
    int doc_s = field_match_score(e->doc, term, 80);
    int code_s = field_match_score(e->code, term, 30);
    int best = full_s;
    if (name_s > best)
      best = name_s;
    if (sig_s > best)
      best = sig_s;
    if (keyword_s > best)
      best = keyword_s;
    if (path_s > best)
      best = path_s;
    if (doc_s > best)
      best = doc_s;
    if (code_s > best)
      best = code_s;
    if (best <= 0) {
      terms++;
      continue;
    }
    matched_terms++;
    if (best >= 10000)
      strong_terms++;
    int support = 0;
    if (full_s > 0)
      support += full_s > 500 ? 500 : full_s;
    if (name_s > 0)
      support += name_s > 650 ? 650 : name_s;
    if (sig_s > 0)
      support += sig_s > 450 ? 450 : sig_s;
    if (keyword_s > 0)
      support += keyword_s > 700 ? 700 : keyword_s;
    if (doc_s > 0)
      support += doc_s > 550 ? 550 : doc_s;
    if (path_s > 0)
      support += path_s > 180 ? 180 : path_s;
    if (code_s > 0)
      support += code_s > 80 ? 80 : code_s;
    total += best + support;
    terms++;
  }
  free(copy);
  if (!terms || !matched_terms)
    return 0;
  if ((terms >= 4 && matched_terms < terms - 1) ||
      (terms == 3 && matched_terms < 2))
    return 0;
  if (terms > 1 && matched_terms < terms) {
    if (terms <= 2)
      return 0;
    if (matched_terms * 2 < terms && strong_terms == 0)
      return 0;
    total -= (terms - matched_terms) * 2400;
  }
  total += matched_terms * 1200;
  if (matched_terms == terms)
    total += 3000;
  if (e->kind && strcmp(e->kind, "function") == 0) {
    const char *leaf = strrchr(e->full_name ? e->full_name : e->name, '.');
    leaf = leaf ? leaf + 1 : (e->name ? e->name : "");
    int explicit_private = query && strchr(query, '_');
    if (leaf[0] == '_' && !explicit_private)
      total -= 2500;
    else if (leaf[0] != '_')
      total += 300;
  }
  return total > 0 ? total : 1;
}

static int doc_entry_score(const doc_search_entry_t *e, const char *query) {
  if (!doc_search_query_has_alt(query))
    return doc_entry_score_terms(e, query);

  int best = 0;
  const char *start = query;
  const char *p = query;
  while (p && *p) {
    if (*p != '|' && !(*p == '\\' && p[1] == '|')) {
      p++;
      continue;
    }
    char *part = doc_search_trimmed_slice(start, (size_t)(p - start));
    if (part) {
      int score = doc_entry_score_terms(e, part);
      if (score > best)
        best = score;
      free(part);
    }
    p += (*p == '\\' && p[1] == '|') ? 2 : 1;
    start = p;
  }
  char *part = doc_search_trimmed_slice(start, strlen(start));
  if (part) {
    int score = doc_entry_score_terms(e, part);
    if (score > best)
      best = score;
    free(part);
  }
  return best;
}

static int cmp_doc_match_ptr(const void *a, const void *b) {
  const doc_search_entry_t *ea = *(const doc_search_entry_t *const *)a;
  const doc_search_entry_t *eb = *(const doc_search_entry_t *const *)b;
  if (ea->score != eb->score)
    return eb->score - ea->score;
  int k = strcmp(ea->kind, eb->kind);
  if (k)
    return k;
  return strcmp(ea->full_name, eb->full_name);
}

static void print_limited_text(const char *txt, int max_lines, int max_chars,
                               const char *prefix) {
  if (!txt || !*txt)
    return;
  int lines = 0, chars = 0, at_line_start = 1;
  for (const char *p = txt; *p; p++) {
    if (chars >= max_chars || lines >= max_lines) {
      printf("\n%s...%s\n", nyt_clr(NYT_GRAY), nyt_clr(NYT_RESET));
      return;
    }
    if (at_line_start && prefix)
      fputs(prefix, stdout);
    fputc(*p, stdout);
    chars++;
    at_line_start = 0;
    if (*p == '\n') {
      lines++;
      at_line_start = 1;
    }
  }
  if (!at_line_start)
    fputc('\n', stdout);
}

static int low_value_snippet_line(const char *line, size_t n) {
  while (n && isspace((unsigned char)*line)) {
    line++;
    n--;
  }
  if (!n)
    return 1;
  if (line[0] == '#')
    return 1;
  if (n >= 3 && line[0] == '`' && line[1] == '`' && line[2] == '`')
    return 1;
  return doc_keyword_line_value(line, n, NULL, NULL);
}

static char *snippet_line_copy(const char *line, size_t n) {
  while (n && isspace((unsigned char)*line)) {
    line++;
    n--;
  }
  while (n && isspace((unsigned char)line[n - 1]))
    n--;
  if (n > 160)
    n = 160;
  return strndup0(line, n);
}

static char *snippet_around_term(const char *txt, const char *term) {
  if (!txt || !*txt || !term || !*term)
    return NULL;
  const char *scan = txt;
  while (*scan) {
    const char *hit = find_case(scan, term);
    if (!hit)
      return NULL;
    const char *line = hit;
    while (line > txt && line[-1] != '\n')
      line--;
    const char *end = hit;
    while (*end && *end != '\n')
      end++;
    size_t n = (size_t)(end - line);
    if (!low_value_snippet_line(line, n))
      return snippet_line_copy(line, n);
    scan = *end ? end + 1 : end;
  }
  return NULL;
}

static char *one_line_snippet_for_query(const char *txt, const char *query) {
  if (!txt)
    return strdup("");
  char *copy = doc_search_query_terms_copy(query);
  if (copy) {
    for (char *term = strtok(copy, " \t\r\n"); term;
         term = strtok(NULL, " \t\r\n")) {
      char *hit = snippet_around_term(txt, term);
      if (hit && *hit) {
        free(copy);
        return hit;
      }
      free(hit);
    }
    free(copy);
  }
  for (const char *line = txt; *line;) {
    const char *end = strchr(line, '\n');
    size_t n = end ? (size_t)(end - line) : strlen(line);
    if (!low_value_snippet_line(line, n))
      return snippet_line_copy(line, n);
    if (!end)
      break;
    line = end + 1;
  }
  return strdup("");
}

static char *keyword_snippet_for_query(const char *keywords,
                                       const char *query) {
  if (!keywords || !*keywords || !query || !*query)
    return NULL;
  char *copy = doc_search_query_terms_copy(query);
  if (!copy)
    return NULL;
  for (char *term = strtok(copy, " \t\r\n"); term;
       term = strtok(NULL, " \t\r\n")) {
    if (find_case(keywords, term)) {
      free(copy);
      return snippet_line_copy(keywords, strlen(keywords));
    }
  }
  free(copy);
  return NULL;
}

static void print_search_result(const doc_search_entry_t *e, int rank,
                                const char *query) {
  const char *title =
      e->signature && *e->signature ? e->signature : e->full_name;
  printf("%s%2d%s  %s%-9s%s %s%s%s", nyt_clr(NYT_GRAY), rank,
         nyt_clr(NYT_RESET), nyt_clr(NYT_CYAN), e->kind, nyt_clr(NYT_RESET),
         nyt_clr(NYT_BOLD), title, nyt_clr(NYT_RESET));
  if (e->path && *e->path) {
    printf(" %s%s", nyt_clr(NYT_GRAY), e->path);
    if (e->line > 0)
      printf(":%d", e->line);
    printf("%s", nyt_clr(NYT_RESET));
  }
  fputc('\n', stdout);
  char *keyword_snippet = keyword_snippet_for_query(e->keywords, query);
  char *snippet = keyword_snippet
                      ? NULL
                      : one_line_snippet_for_query(
                            e->doc && *e->doc ? e->doc : e->code, query);
  if (keyword_snippet && *keyword_snippet)
    printf("    keywords: %s\n", keyword_snippet);
  else if (snippet && *snippet)
    printf("    %s\n", snippet);
  else if (e->keywords && *e->keywords)
    printf("    keywords: %s\n", e->keywords);
  free(keyword_snippet);
  free(snippet);
}

static void print_doc_entry_detail(const doc_search_entry_t *e) {
  const char *title =
      e->signature && *e->signature ? e->signature : e->full_name;
  printf("%s%s%s\n", nyt_clr(NYT_BOLD), title, nyt_clr(NYT_RESET));
  nyt_rule(stdout);
  nyt_kv("kind", "%s", e->kind);
  if (e->path && *e->path) {
    if (e->line > 0)
      nyt_kv("path", "%s:%d", e->path, e->line);
    else
      nyt_kv("path", "%s", e->path);
  }
  if (e->keywords && *e->keywords)
    nyt_kv("keywords", "%s", e->keywords);
  if (e->doc && *e->doc) {
    nyt_subheading("Docs");
    print_limited_text(e->doc, 80, 12000, NULL);
  }
  if (e->code && *e->code) {
    nyt_subheading("Source");
    print_limited_text(e->code, 160, 24000, NULL);
  }
}

static int build_search_index(doc_search_index_t *idx, const char *root,
                              const char *input, int include_docs,
                              int include_symbols, int *source_files,
                              int *module_count, int *symbol_count) {
  if (source_files)
    *source_files = 0;
  if (module_count)
    *module_count = 0;
  if (symbol_count)
    *symbol_count = 0;
  if (include_docs)
    collect_all_docs(idx, root);
  if (include_symbols) {
    int src_n = 0;
    char *bundle = read_web_input_bundle(input, root, &src_n);
    if (!bundle)
      return 0;
    append_symbol_entries_from_bundle(idx, bundle, root, module_count,
                                      symbol_count);
    free(bundle);
    if (source_files)
      *source_files = src_n;
  }
  return 1;
}

typedef struct {
  const char *input;
  char *query;
  int include_docs;
  int include_symbols;
  int limit;
} doc_search_opts_t;

static void doc_search_print_usage(int get_one) {
  nyt_heading(get_one ? "Nytrix Docs Get" : "Nytrix Docs Search");
  printf("%susage:%s %sny doc %s%s %s[--input PATH] [--docs|--symbols] [-n "
         "N] QUERY%s\n\n",
         nyt_clr(NYT_BOLD), nyt_clr(NYT_RESET), nyt_clr(NYT_CYAN),
         get_one ? "get" : "search", nyt_clr(NYT_RESET), nyt_clr(NYT_GREEN),
         nyt_clr(NYT_RESET));
  printf("Fuzzy-searches Nytrix docs, modules, symbols, and documented "
         "scripts.\n");
}

static int doc_search_parse_args(int argc, char **argv, int get_one,
                                 doc_search_opts_t *opts, char *err,
                                 size_t err_sz) {
  memset(opts, 0, sizeof(*opts));
  opts->include_docs = 1;
  opts->include_symbols = 1;
  opts->limit = get_one ? 1 : 20;
  sb_t query_sb = {0};

  for (int i = 2; i < argc; i++) {
    const char *a = argv[i];
    if (ny_arg_match(a, "--help", "-h")) {
      doc_search_print_usage(get_one);
      return 1;
    }
    int color_mode = -2;
    int color_idx = i;
    int color_rc = ny_arg_consume_color(&color_idx, argc, argv, &color_mode,
                                        err, err_sz);
    if (color_rc < 0) {
      nyt_err("ny-doc", "%s", err);
      free(query_sb.data);
      return 2;
    }
    if (color_rc > 0) {
      ny_arg_apply_color_mode(color_mode);
      i = color_idx;
      continue;
    }
    if (ny_arg_match(a, "--input", "-i")) {
      const char *v = NULL;
      if (!ny_arg_take_value(a, &i, argc, argv, &v, err, err_sz)) {
        nyt_err("ny-doc", "%s", err);
        free(query_sb.data);
        return 2;
      }
      opts->input = v;
    } else if (ny_arg_match(a, "--limit", "-n")) {
      if (!ny_arg_take_int(a, &i, argc, argv, 1, 200, &opts->limit, "limit",
                           err, err_sz)) {
        nyt_err("ny-doc", "%s", err);
        free(query_sb.data);
        return 2;
      }
    } else if (strcmp(a, "--docs") == 0) {
      opts->include_docs = 1;
      opts->include_symbols = 0;
    } else if (strcmp(a, "--symbols") == 0 || strcmp(a, "--functions") == 0) {
      opts->include_docs = 0;
      opts->include_symbols = 1;
    } else if (a[0] == '-') {
      nyt_err("ny-doc", "unknown option: %s", a);
      free(query_sb.data);
      return 2;
    } else {
      if (query_sb.len)
        sb_add(&query_sb, " ");
      sb_add(&query_sb, a);
    }
  }

  char *query = query_sb.data ? query_sb.data : strdup("");
  if (!query || !*query) {
    free(query);
    nyt_err("ny-doc", "missing search query");
    return 2;
  }
  opts->query = query;
  return 0;
}

static const char *doc_search_resolve_input(const char *root,
                                            const char *input,
                                            char *default_input) {
  if (input)
    return input;
  if (join_path(default_input, PATH_MAX, root, "lib") &&
      path_is_dir(default_input))
    return default_input;
  return root;
}

static void doc_search_collect_matches(doc_search_index_t *idx,
                                       const char *query,
                                       doc_search_entry_t ***matches_out,
                                       size_t *match_n_out) {
  doc_search_entry_t **matches = NULL;
  size_t match_n = 0, match_cap = 0;
  for (size_t i = 0; i < idx->len; i++) {
    int score = doc_entry_score(&idx->items[i], query);
    if (score <= 0)
      continue;
    idx->items[i].score = score;
    if (match_n == match_cap) {
      size_t next_cap = match_cap ? match_cap * 2 : 128;
      doc_search_entry_t **next = (doc_search_entry_t **)realloc(
          matches, next_cap * sizeof(doc_search_entry_t *));
      if (!next)
        break;
      matches = next;
      match_cap = next_cap;
    }
    matches[match_n++] = &idx->items[i];
  }
  qsort(matches, match_n, sizeof(*matches), cmp_doc_match_ptr);
  *matches_out = matches;
  *match_n_out = match_n;
}

static int ny_doc_search_main(int argc, char **argv, int get_one) {
  doc_search_opts_t opts = {0};
  char err[256];
  int rc = doc_search_parse_args(argc, argv, get_one, &opts, err, sizeof(err));
  if (rc != 0) {
    free(opts.query);
    return rc == 1 ? 0 : rc;
  }

  const char *root = find_nytrix_share_root();
  if (!root || !*root) {
    nyt_err("ny-doc", "failed to resolve nytrix share root");
    free(opts.query);
    return 1;
  }

  char default_input[PATH_MAX];
  const char *input = doc_search_resolve_input(root, opts.input, default_input);

  doc_search_index_t idx = {0};
  int source_files = 0, modules = 0, symbols = 0;
  if (!build_search_index(&idx, root, input, opts.include_docs,
                          opts.include_symbols, &source_files, &modules,
                          &symbols)) {
    doc_search_index_free(&idx);
    free(opts.query);
    nyt_err("ny-doc", "failed to build search index from %s", input);
    return 1;
  }

  doc_search_entry_t **matches = NULL;
  size_t match_n = 0;
  doc_search_collect_matches(&idx, opts.query, &matches, &match_n);

  if (get_one) {
    if (match_n == 0) {
      nyt_err("ny-doc", "no docs or symbols matched '%s'", opts.query);
      free(matches);
      doc_search_index_free(&idx);
      free(opts.query);
      return 1;
    }
    print_doc_entry_detail(matches[0]);
  } else {
    printf("%sny doc%s search '%s' %s(%zu matches, %zu indexed",
           nyt_clr(NYT_BOLD), nyt_clr(NYT_RESET), opts.query,
           nyt_clr(NYT_GRAY), match_n, idx.len);
    if (opts.include_symbols)
      printf(", %d modules, %d symbols, %d source files", modules, symbols,
             source_files);
    printf(")%s\n", nyt_clr(NYT_RESET));
    size_t shown =
        match_n < (size_t)opts.limit ? match_n : (size_t)opts.limit;
    for (size_t i = 0; i < shown; i++)
      print_search_result(matches[i], (int)i + 1, opts.query);
    if (shown < match_n)
      printf("%s... %zu more%s\n", nyt_clr(NYT_GRAY), match_n - shown,
             nyt_clr(NYT_RESET));
  }

  free(matches);
  doc_search_index_free(&idx);
  free(opts.query);
  return 0;
}

static char *replace_doc_template(const char *tpl, const char *data,
                                  const char *script, const char *css,
                                  const char *seo_head) {
  sb_t out = {0};
  const char *p = tpl;
  while (*p) {
    if (strncmp(p, "DATA_PLACEHOLDER", 16) == 0) {
      sb_add(&out, data);
      p += 16;
    } else if (strncmp(p, "SCRIPT_PLACEHOLDER", 18) == 0) {
      sb_add(&out, script);
      p += 18;
    } else if (strncmp(p, "CSS_PLACEHOLDER", 15) == 0) {
      sb_add(&out, css);
      p += 15;
    } else if (strncmp(p, "SEO_PLACEHOLDER", 15) == 0) {
      sb_add(&out, seo_head ? seo_head : "");
      p += 15;
    } else {
      sb_addn(&out, p, 1);
      p++;
    }
  }
  return out.data;
}

static void sb_add_xml_escaped(sb_t *sb, const char *s);
static void sb_add_html_escaped(sb_t *sb, const char *s);

static void append_site_url(sb_t *out, const char *site, const char *rel) {
  if (!site || !*site) {
    while (rel && *rel == '/')
      rel++;
    if (rel && *rel)
      sb_add(out, rel);
    return;
  }
  const char *base = site;
  sb_add(out, base);
  if (out->len && out->data[out->len - 1] != '/')
    sb_add(out, "/");
  while (rel && *rel == '/')
    rel++;
  if (rel && *rel)
    sb_add(out, rel);
}

typedef struct {
  char *route;
  char *title;
  char *path;
  time_t mtime;
} ny_doc_meta_entry_t;

typedef struct {
  ny_doc_meta_entry_t *data;
  size_t len;
  size_t cap;
} ny_doc_meta_list_t;

static void ny_doc_meta_list_push(ny_doc_meta_list_t *list, char *route,
                                  char *title, char *path, time_t mtime) {
  if (!list || !route || !title) {
    free(route);
    free(title);
    free(path);
    return;
  }
  if (list->len == list->cap) {
    size_t next_cap = list->cap ? list->cap * 2u : 32u;
    ny_doc_meta_entry_t *next = (ny_doc_meta_entry_t *)realloc(
        list->data, next_cap * sizeof(*list->data));
    if (!next) {
      free(route);
      free(title);
      free(path);
      return;
    }
    list->data = next;
    list->cap = next_cap;
  }
  list->data[list->len++] = (ny_doc_meta_entry_t){route, title, path, mtime};
}

static void ny_doc_meta_collect_dir(const char *base_dir, const char *dir,
                                    ny_doc_meta_list_t *list) {
  DIR *d = opendir(dir);
  if (!d)
    return;
  char **names = NULL;
  int names_n = 0, names_cap = 0;
  struct dirent *ent;
  while ((ent = readdir(d))) {
    if (web_skip_source_dir(ent->d_name))
      continue;
    if (names_n == names_cap) {
      int next_cap = names_cap ? names_cap * 2 : 32;
      char **next = (char **)realloc(names, (size_t)next_cap * sizeof(*names));
      if (!next)
        break;
      names = next;
      names_cap = next_cap;
    }
    names[names_n++] = strdup(ent->d_name);
  }
  closedir(d);
  qsort(names, (size_t)names_n, sizeof(char *), cmp_cstr_ptr);
  for (int i = 0; i < names_n; i++) {
    char path[PATH_MAX];
    if (join_path(path, sizeof(path), dir, names[i])) {
      if (path_is_dir(path)) {
        ny_doc_meta_collect_dir(base_dir, path, list);
      } else if (docs_file_supported(path)) {
        char *route = doc_route_name(base_dir, path);
        char *title = doc_title_from_route(route);
        char *doc_path = strdup(path);
        struct stat st;
        time_t mtime = stat(path, &st) == 0 ? st.st_mtime : 0;
        ny_doc_meta_list_push(list, route, title, doc_path, mtime);
      }
    }
    free(names[i]);
  }
  free(names);
}

static void ny_doc_meta_collect(const char *root, ny_doc_meta_list_t *list) {
  if (!root || !*root || !list)
    return;
  char docs_dir[PATH_MAX];
  if (join_path(docs_dir, sizeof(docs_dir), root, "docs") &&
      path_is_dir(docs_dir))
    ny_doc_meta_collect_dir(docs_dir, docs_dir, list);
}

static void append_doc_url(sb_t *out, const char *site, const char *route) {
  if (site && *site) {
    char rel[PATH_MAX];
    snprintf(rel, sizeof(rel), "%s/", route && *route ? route : "");
    append_site_url(out, site, rel);
    return;
  }
  sb_add(out, route && *route ? route : ".");
  if (route && *route)
    sb_add(out, "/");
}

static void append_doc_link_metadata(sb_t *out, const char *site,
                                     const ny_doc_meta_list_t *docs) {
  if (!out || !docs)
    return;
  for (size_t i = 0; i < docs->len; i++) {
    const char *route = docs->data[i].route;
    const char *title = docs->data[i].title;
    if (!route || !*route || !title || !*title)
      continue;
    const char *rel =
        strncmp(route, "learn/", 6) == 0 || strncmp(route, "spec/", 5) == 0
            ? "help"
            : "section";
    sb_add(out, "    <link rel=\"");
    sb_add(out, rel);
    sb_add(out, "\" href=\"");
    append_doc_url(out, site, route);
    sb_add(out, "\" title=\"");
    sb_add_html_escaped(out, title);
    sb_add(out, "\" />\n");
  }
}

static void ny_doc_meta_list_free(ny_doc_meta_list_t *list) {
  if (!list)
    return;
  for (size_t i = 0; i < list->len; i++) {
    free(list->data[i].route);
    free(list->data[i].title);
    free(list->data[i].path);
  }
  free(list->data);
  memset(list, 0, sizeof(*list));
}

static void append_site_open_graph(sb_t *out, const char *site,
                                   const sb_t *root_url, const sb_t *logo_url,
                                   const sb_t *feed_url) {
  if (!site || !*site) {
    sb_add(out, "    <link rel=\"alternate\" type=\"application/rss+xml\" "
                "title=\"Nytrix RSS feed\" href=\"feed.xml\" />\n");
    return;
  }
  sb_add(out, "    <link rel=\"canonical\" href=\"");
  sb_add_html_escaped(out, root_url->data ? root_url->data : "");
  sb_add(out, "\" />\n    <meta property=\"og:url\" content=\"");
  sb_add_html_escaped(out, root_url->data ? root_url->data : "");
  sb_add(out, "\" />\n    <meta property=\"og:image\" content=\"");
  sb_add_html_escaped(out, logo_url->data ? logo_url->data : "");
  sb_add(out,
         "\" />\n    <meta property=\"og:image:secure_url\" content=\"");
  sb_add_html_escaped(out, logo_url->data ? logo_url->data : "");
  sb_add(out,
         "\" />\n    <meta property=\"og:image:width\" content=\"1200\" />\n"
         "    <meta property=\"og:image:height\" content=\"630\" />\n"
         "    <meta name=\"twitter:image\" content=\"");
  sb_add_html_escaped(out, logo_url->data ? logo_url->data : "");
  sb_add(out, "\" />\n    <link rel=\"sitemap\" type=\"application/xml\" "
              "href=\"sitemap.xml\" />\n");
  sb_add(out, "    <link rel=\"alternate\" type=\"application/rss+xml\" "
              "title=\"Nytrix RSS feed\" href=\"");
  sb_add_html_escaped(out, feed_url->data ? feed_url->data : "");
  sb_add(out, "\" />\n");
}

static void append_site_ld_json(sb_t *out, const char *site,
                                const char *doc_url,
                                const ny_doc_meta_list_t *docs) {
  sb_add(out, "    <script type=\"application/ld+json\">\n        {\n"
              "            \"@context\": \"https://schema.org\",\n"
              "            \"@graph\": [\n"
              "                {\n"
              "                    \"@type\": \"WebSite\",\n"
              "                    \"@id\": ");
  sb_add_json_str(out, doc_url);
  sb_add(out, ",\n                    \"name\": \"Nytrix Lang\",\n"
              "                    \"headline\": \"Nytrix native programming "
              "language manual\",\n"
              "                    \"description\": \"Nytrix language manual, "
              "quick start, examples, standard library API reference, and "
              "local documentation search.\",\n"
              "                    \"url\": ");
  sb_add_json_str(out, doc_url);
  sb_add(out, ",\n                    \"sameAs\": [");
  sb_add_json_str(out, "https://discord.gg/XQDR6DZWb");
  sb_add(out, ", ");
  sb_add_json_str(out, "https://mastodon.social/@nytrix");
  sb_add(out, "]");
  sb_add(
      out,
      ",\n                    \"about\": [\"native programming language\", "
      "\"compiler\", \"compile-time execution\", \"native FFI\", \"C ABI\", "
      "\"standard library\", \"networking\", \"native FFI\", \"parsing\"],\n"
      "                    \"hasPart\": [");
  for (size_t i = 0; i < docs->len; i++) {
    if (i)
      sb_add(out, ",");
    sb_add(out, "\n                        {\"@type\":\"WebPage\",\"name\":");
    sb_add_json_str(out, docs->data[i].title ? docs->data[i].title : "");
    sb_add(out, ",\"url\":");
    sb_t page_url = {0};
    append_doc_url(&page_url, site, docs->data[i].route);
    sb_add_json_str(out, page_url.data ? page_url.data : "");
    free(page_url.data);
    sb_add(out, "}");
  }
  if (docs->len)
    sb_add(out, "\n                    ");
  sb_add(out, "],\n"
              "                    \"potentialAction\": {\n"
              "                        \"@type\": \"SearchAction\",\n"
              "                        \"target\": ");
  if (site && *site) {
    sb_t search_url = {0};
    append_site_url(&search_url, site, "?q={search_term_string}");
    sb_add_json_str(out, search_url.data ? search_url.data : "");
    free(search_url.data);
  } else {
    sb_add_json_str(out, "?q={search_term_string}");
  }
  sb_add(out, ",\n                        \"query-input\": \"required "
              "name=search_term_string\"\n"
              "                    }\n"
              "                },\n"
              "                {\n"
              "                    \"@type\": \"SoftwareSourceCode\",\n"
              "                    \"name\": \"Nytrix\",\n"
              "                    \"codeRepository\": ");
  sb_add_json_str(out, "https://github.com/nytrix-lang/nytrix");
  sb_add(
      out,
      ",\n                    \"programmingLanguage\": \"Nytrix\",\n"
      "                    \"license\": "
      "\"https://opensource.org/licenses/MIT\",\n"
      "                    \"runtimePlatform\": [\"Linux\", \"macOS\", "
      "\"Windows\"],\n"
      "                    \"description\": \"Nytrix is a compact native "
      "programming language with compile-time execution, native FFI, OS APIs, "
      "networking, parsing, UI, and source-linked documentation.\"\n"
      "                },\n"
      "                {\n"
      "                    \"@type\": \"TechArticle\",\n"
      "                    \"name\": \"Nytrix Documentation\",\n"
      "                    \"url\": ");
  sb_add_json_str(out, doc_url);
  sb_add(out, ",\n                    \"isPartOf\": {\"@id\": ");
  sb_add_json_str(out, doc_url);
  sb_add(out,
         "}\n                }\n            ]\n        }\n    </script>\n");
}

static char *render_site_head(const char *site, const char *root) {
  ny_doc_meta_list_t docs = {0};
  ny_doc_meta_collect(root, &docs);
  sb_t root_url = {0}, logo_url = {0}, feed_url = {0}, out = {0};
  if (site && *site) {
    append_site_url(&root_url, site, "");
    append_site_url(&logo_url, site, "og.png");
    append_site_url(&feed_url, site, "feed.xml");
  }
  append_site_open_graph(&out, site, &root_url, &logo_url, &feed_url);
  append_doc_link_metadata(&out, site, &docs);
  const char *doc_url = (root_url.data && *root_url.data)
                            ? root_url.data
                            : "https://github.com/nytrix-lang/nytrix";
  append_site_ld_json(&out, site, doc_url, &docs);
  ny_doc_meta_list_free(&docs);
  free(root_url.data);
  free(logo_url.data);
  free(feed_url.data);
  return out.data ? out.data : strdup("");
}

static void sb_add_xml_escaped(sb_t *sb, const char *s) {
  for (const char *p = s ? s : ""; *p; p++) {
    switch (*p) {
    case '&':
      sb_add(sb, "&amp;");
      break;
    case '<':
      sb_add(sb, "&lt;");
      break;
    case '>':
      sb_add(sb, "&gt;");
      break;
    case '"':
      sb_add(sb, "&quot;");
      break;
    default:
      sb_addn(sb, p, 1);
      break;
    }
  }
}

static void sb_add_html_escaped(sb_t *sb, const char *s) {
  sb_add_xml_escaped(sb, s);
}

static void sb_add_html_escapedn(sb_t *sb, const char *s, size_t n) {
  for (size_t i = 0; i < n; i++) {
    char tmp[2] = {s[i], 0};
    sb_add_html_escaped(sb, tmp);
  }
}

static void append_static_site_end_html(sb_t *html, const char *feed_href) {
  sb_add(html,
         "<footer class=\"site-end\" aria-label=\"Nytrix feeds and social "
         "links\"><div class=\"site-end-head\"><h2>Updates</h2><p>Reader-ready "
         "RSS for docs, release notes, and API changes.</p></div><nav "
         "class=\"site-links\" aria-label=\"Nytrix social links\"><a "
         "class=\"primary\" href=\"");
  sb_add_html_escaped(html, feed_href && *feed_href ? feed_href : "feed.xml");
  sb_add(html,
         "\" rel=\"noopener noreferrer\" title=\"Open RSS feed\">RSS</a>"
         "<a href=\"https://discord.gg/XQDR6DZWb\" target=\"_blank\" "
         "rel=\"me noopener noreferrer\" title=\"Open Discord\">Discord</a>"
         "<a href=\"https://mastodon.social/@nytrix\" target=\"_blank\" "
         "rel=\"me noopener noreferrer\" title=\"Open Mastodon\">Mastodon</a>"
         "</nav></footer>");
}

static int safe_route_path(char *out, size_t out_n, const char *route) {
  if (!out || out_n == 0)
    return 0;
  size_t j = 0;
  for (const char *p = route ? route : ""; *p; p++) {
    unsigned char c = (unsigned char)*p;
    char ch = 0;
    if (isalnum(c) || c == '_' || c == '-')
      ch = (char)c;
    else if (c == '/' || c == '\\')
      ch = '/';
    else if (c == '.')
      ch = '-';
    else
      ch = '-';
    if (j + 1 >= out_n)
      return 0;
    if (ch == '/' && (j == 0 || out[j - 1] == '/'))
      continue;
    out[j++] = ch;
  }
  while (j > 0 && out[j - 1] == '/')
    j--;
  out[j] = '\0';
  return j > 0 && strstr(out, "..") == NULL;
}

static char *markdown_summary(const char *md) {
  if (!md)
    return strdup("Nytrix documentation page.");
  int in_code = 0;
  for (const char *line = md; *line;) {
    const char *next = strchr(line, '\n');
    size_t n = next ? (size_t)(next - line) : strlen(line);
    const char *p = line;
    size_t len = n;
    while (len && isspace((unsigned char)*p)) {
      p++;
      len--;
    }
    while (len && isspace((unsigned char)p[len - 1]))
      len--;
    if (len >= 3 && strncmp(p, "```", 3) == 0) {
      in_code = !in_code;
    } else if (!in_code && len > 0 && p[0] != '#' && p[0] != '|' &&
               !(len >= 2 && p[0] == '-' && p[1] == ' ')) {
      sb_t out = {0};
      int bracket = 0, paren = 0;
      for (size_t i = 0; i < len && out.len < 190; i++) {
        char c = p[i];
        if (c == '[') {
          bracket = 1;
          continue;
        }
        if (c == ']') {
          bracket = 0;
          continue;
        }
        if (!bracket && c == '(') {
          paren = 1;
          continue;
        }
        if (paren) {
          if (c == ')')
            paren = 0;
          continue;
        }
        if (c == '`' || c == '*' || c == '_' || c == '~')
          continue;
        sb_addn(&out, &c, 1);
      }
      return out.data ? out.data : strdup("Nytrix documentation page.");
    }
    if (!next)
      break;
    line = next + 1;
  }
  return strdup("Nytrix documentation page.");
}

static int href_is_external(const char *href) {
  if (!href || !*href)
    return 0;
  if (href[0] == '/' && href[1] == '/')
    return 1;
  for (const char *p = href; *p; p++) {
    if (*p == ':')
      return 1;
    if (*p == '/' || *p == '#' || *p == '?')
      return 0;
  }
  return 0;
}

static char *doc_route_from_href_static(const char *base_route,
                                        const char *href, char **anchor_out) {
  if (anchor_out)
    *anchor_out = NULL;
  if (!href || !*href || href_is_external(href))
    return NULL;

  const char *hash = strchr(href, '#');
  const char *query = strchr(href, '?');
  size_t path_n = strlen(href);
  if (hash && (size_t)(hash - href) < path_n)
    path_n = (size_t)(hash - href);
  if (query && (size_t)(query - href) < path_n)
    path_n = (size_t)(query - href);
  if (hash && anchor_out)
    *anchor_out = strdup(hash + 1);
  if (path_n == 0)
    return base_route && *base_route ? strdup(base_route) : strdup("");

  char path[PATH_MAX];
  size_t copy_n = path_n < sizeof(path) - 1 ? path_n : sizeof(path) - 1;
  memcpy(path, href, copy_n);
  path[copy_n] = '\0';
  for (char *p = path; *p; p++)
    if (*p == '\\')
      *p = '/';
  while (strncmp(path, "./", 2) == 0)
    memmove(path, path + 2, strlen(path + 2) + 1);
  while (path[0] == '/')
    memmove(path, path + 1, strlen(path));
  if (strncmp(path, "docs/", 5) == 0)
    memmove(path, path + 5, strlen(path + 5) + 1);
  char *dot = strrchr(path, '.');
  if (dot &&
      (!strcmp(dot, ".md") || !strcmp(dot, ".html") || !strcmp(dot, ".htm")))
    *dot = '\0';
  size_t l = strlen(path);
  if (l >= 6 && strcmp(path + l - 6, "/index") == 0)
    path[l - 6] = '\0';

  char combined[PATH_MAX];
  combined[0] = '\0';
  int root_doc_path = !strncmp(path, "learn/", 6) || !strncmp(path, "spec/", 5);
  if (href[0] != '/' && strncmp(href, "docs/", 5) != 0 && !root_doc_path &&
      base_route && *base_route) {
    snprintf(combined, sizeof(combined), "%s", base_route);
    char *slash = strrchr(combined, '/');
    if (slash)
      *slash = '\0';
    else
      combined[0] = '\0';
  }
  if (combined[0] && path[0])
    strncat(combined, "/", sizeof(combined) - strlen(combined) - 1);
  strncat(combined, path, sizeof(combined) - strlen(combined) - 1);

  char buf[PATH_MAX];
  snprintf(buf, sizeof(buf), "%s", combined);
  char *parts[128];
  int n = 0;
  char *save = NULL;
  for (char *tok = strtok_r(buf, "/", &save); tok;
       tok = strtok_r(NULL, "/", &save)) {
    if (!tok[0] || !strcmp(tok, "."))
      continue;
    if (!strcmp(tok, "..")) {
      if (n > 0)
        n--;
      continue;
    }
    if (n < 128)
      parts[n++] = tok;
  }
  sb_t out = {0};
  for (int i = 0; i < n; i++) {
    if (i)
      sb_add(&out, "/");
    sb_add(&out, parts[i]);
  }
  return out.data ? out.data : strdup("");
}

static char *static_href_for_doc_route(const char *from_route,
                                       const char *to_route,
                                       const char *anchor) {
  if (!to_route)
    return NULL;
  if (from_route && to_route && strcmp(from_route, to_route) == 0) {
    sb_t same = {0};
    sb_add(&same, "#");
    if (anchor)
      sb_add(&same, anchor);
    return same.data ? same.data : strdup("#");
  }
  int depth = 0;
  if (from_route && *from_route) {
    depth = 1;
    for (const char *p = from_route; *p; p++)
      if (*p == '/')
        depth++;
  }
  sb_t out = {0};
  for (int i = 0; i < depth; i++)
    sb_add(&out, "../");
  sb_add(&out, to_route && *to_route ? to_route : ".");
  if (out.len && out.data[out.len - 1] != '/')
    sb_add(&out, "/");
  if (anchor && *anchor) {
    sb_add(&out, "#");
    sb_add(&out, anchor);
  }
  return out.data ? out.data : strdup("#");
}

static void markdown_render_inline(sb_t *out, const char *s, size_t n,
                                   const char *route) {
  int in_code = 0;
  for (size_t i = 0; i < n; i++) {
    char c = s[i];
    if (c == '`') {
      sb_add(out, in_code ? "</code>" : "<code>");
      in_code = !in_code;
      continue;
    }
    if (!in_code && c == '[') {
      const char *label_end = memchr(s + i + 1, ']', n - i - 1);
      if (label_end && (size_t)(label_end - s + 1) < n && label_end[1] == '(') {
        const char *href_start = label_end + 2;
        const char *href_end = memchr(href_start, ')', s + n - href_start);
        if (href_end) {
          char *href = strndup0(href_start, (size_t)(href_end - href_start));
          char *anchor = NULL;
          char *doc_route = doc_route_from_href_static(route, href, &anchor);
          char *final_href =
              doc_route ? static_href_for_doc_route(route, doc_route, anchor)
                        : NULL;
          sb_add(out, "<a href=\"");
          sb_add_html_escaped(out, final_href ? final_href : href);
          if (!doc_route && href_is_external(href))
            sb_add(out, "\" rel=\"noopener noreferrer\"");
          else
            sb_add(out, "\"");
          sb_add(out, ">");
          markdown_render_inline(out, s + i + 1,
                                 (size_t)(label_end - (s + i + 1)), route);
          sb_add(out, "</a>");
          free(href);
          free(anchor);
          free(doc_route);
          free(final_href);
          i = (size_t)(href_end - s);
          continue;
        }
      }
    }
    char tmp[2] = {c, 0};
    sb_add_html_escaped(out, tmp);
  }
  if (in_code)
    sb_add(out, "</code>");
}

static int markdown_table_separator(const char *p, size_t len) {
  for (size_t i = 0; i < len; i++) {
    char c = p[i];
    if (c == '|' || c == '-' || c == ':' || isspace((unsigned char)c))
      continue;
    return 0;
  }
  return 1;
}

static void markdown_render_table_row(sb_t *out, const char *p, size_t len,
                                      const char *route) {
  if (markdown_table_separator(p, len))
    return;
  sb_add(out, "<tr>");
  size_t start = 0;
  if (start < len && p[start] == '|')
    start++;
  while (start < len) {
    size_t end = start;
    while (end < len && p[end] != '|')
      end++;
    size_t next = end < len ? end + 1 : end;
    size_t cell_start = start;
    size_t cell_end = end;
    while (cell_start < cell_end && isspace((unsigned char)p[cell_start]))
      cell_start++;
    while (cell_end > cell_start && isspace((unsigned char)p[cell_end - 1]))
      cell_end--;
    sb_add(out, "<td>");
    markdown_render_inline(out, p + cell_start, cell_end - cell_start, route);
    sb_add(out, "</td>");
    start = next;
  }
  sb_add(out, "</tr>\n");
}

static void render_md_close_structures(sb_t *out, int *in_ul, int *in_table) {
  if (*in_ul) {
    sb_add(out, "</ul>\n");
    *in_ul = 0;
  }
  if (*in_table) {
    sb_add(out, "</tbody></table>\n");
    *in_table = 0;
  }
}

static void render_md_code_block(sb_t *out, const char *p, size_t len,
                                 int closing) {
  if (closing) {
    sb_add(out, "</code></pre>\n");
    return;
  }
  sb_add(out, "<pre><code");
  size_t off = 3;
  while (off < len && isspace((unsigned char)p[off]))
    off++;
  size_t lang_start = off;
  while (off < len && (isalnum((unsigned char)p[off]) || p[off] == '_' ||
                       p[off] == '-'))
    off++;
  if (off > lang_start) {
    sb_add(out, " class=\"language-");
    sb_add_html_escapedn(out, p + lang_start, off - lang_start);
    sb_add(out, "\"");
  } else {
    sb_add(out, " class=\"language-ny\"");
  }
  sb_add(out, ">");
}

static void render_md_heading(sb_t *out, const char *p, size_t len,
                              const char *route) {
  int level = 0;
  while ((size_t)level < len && p[level] == '#')
    level++;
  if (level < 1)
    level = 1;
  if (level > 4)
    level = 4;
  size_t off = (size_t)level;
  while (off < len && isspace((unsigned char)p[off]))
    off++;
  char tag[16];
  snprintf(tag, sizeof(tag), "h%d", level);
  sb_add(out, "<");
  sb_add(out, tag);
  sb_add(out, ">");
  markdown_render_inline(out, p + off, len - off, route);
  sb_add(out, "</");
  sb_add(out, tag);
  sb_add(out, ">\n");
}

static void render_md_list_item(sb_t *out, const char *p, size_t len,
                                const char *route) {
  size_t off = 2;
  while (off < len && isspace((unsigned char)p[off]))
    off++;
  sb_add(out, "<li>");
  markdown_render_inline(out, p + off, len - off, route);
  sb_add(out, "</li>\n");
}

static void render_md_paragraph(sb_t *out, const char *p, size_t len,
                                const char *route) {
  sb_add(out, "<p>");
  markdown_render_inline(out, p, len, route);
  sb_add(out, "</p>\n");
}

static void render_md_line(sb_t *out, const char *p, size_t len,
                           const char *route, int *in_ul, int *in_table) {
  if (len == 0) {
    render_md_close_structures(out, in_ul, in_table);
  } else if (p[0] == '#') {
    render_md_close_structures(out, in_ul, in_table);
    render_md_heading(out, p, len, route);
  } else if (len >= 2 && p[0] == '-' && isspace((unsigned char)p[1])) {
    if (*in_table) {
      sb_add(out, "</tbody></table>\n");
      *in_table = 0;
    }
    if (!*in_ul) {
      sb_add(out, "<ul>\n");
      *in_ul = 1;
    }
    render_md_list_item(out, p, len, route);
  } else if (p[0] == '|') {
    if (*in_ul) {
      sb_add(out, "</ul>\n");
      *in_ul = 0;
    }
    if (!*in_table) {
      sb_add(out, "<table><tbody>\n");
      *in_table = 1;
    }
    markdown_render_table_row(out, p, len, route);
  } else {
    render_md_close_structures(out, in_ul, in_table);
    render_md_paragraph(out, p, len, route);
  }
}

static char *render_markdown_static(const char *md, const char *route) {
  sb_t out = {0};
  int in_code = 0, in_ul = 0, in_table = 0;
  for (const char *line = md ? md : ""; *line;) {
    const char *next = strchr(line, '\n');
    size_t n = next ? (size_t)(next - line) : strlen(line);
    const char *p = line;
    size_t len = n;
    while (len && (p[len - 1] == '\r' || p[len - 1] == '\n'))
      len--;
    if (len >= 3 && strncmp(p, "```", 3) == 0) {
      render_md_close_structures(&out, &in_ul, &in_table);
      render_md_code_block(&out, p, len, in_code);
      in_code = !in_code;
    } else if (in_code) {
      sb_add_html_escapedn(&out, p, len);
      sb_add(&out, "\n");
    } else {
      while (len && isspace((unsigned char)*p)) {
        p++;
        len--;
      }
      while (len && isspace((unsigned char)p[len - 1]))
        len--;
      render_md_line(&out, p, len, route, &in_ul, &in_table);
    }
    if (!next)
      break;
    line = next + 1;
  }
  if (in_code)
    sb_add(&out, "</code></pre>\n");
  render_md_close_structures(&out, &in_ul, &in_table);
  return out.data ? out.data : strdup("");
}

static int write_static_doc_page(const char *out_dir, const char *site,
                                 const char *route, const char *title,
                                 const char *summary, const char *body_html) {
  char safe[PATH_MAX], dir[PATH_MAX], path[PATH_MAX];
  if (!safe_route_path(safe, sizeof(safe), route) ||
      !join_path(dir, sizeof(dir), out_dir, safe) || !mkdir_p(dir) ||
      !join_path(path, sizeof(path), dir, "index.html"))
    return 0;
  sb_t canonical = {0}, app = {0}, feed = {0}, root_rel = {0}, html = {0};
  int has_site = site && *site;
  if (has_site) {
    append_site_url(&canonical, site, safe);
    if (canonical.len && canonical.data[canonical.len - 1] != '/')
      sb_add(&canonical, "/");
    append_site_url(&app, site, "#");
    append_site_url(&feed, site, "feed.xml");
  } else {
    int depth = 1;
    for (const char *p = safe; *p; p++)
      if (*p == '/')
        depth++;
    for (int i = 0; i < depth; i++)
      sb_add(&root_rel, "../");
    sb_add(&app, root_rel.data ? root_rel.data : "");
    sb_add(&app, "index.html#");
    sb_add(&feed, root_rel.data ? root_rel.data : "");
    sb_add(&feed, "feed.xml");
  }
  sb_add(&app, route);

  sb_add(&html,
         "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\"><meta "
         "name=\"viewport\" "
         "content=\"width=device-width,initial-scale=1\"><meta name=\"robots\" "
         "content=\"index,follow\"><title>");
  sb_add(&html, "Nytrix - ");
  sb_add_html_escaped(&html, title);
  sb_add(&html, "</title><meta name=\"description\" content=\"");
  sb_add_html_escaped(&html, summary);
  sb_add(&html, "\">");
  if (has_site) {
    sb_add(&html, "<link rel=\"canonical\" href=\"");
    sb_add_html_escaped(&html, canonical.data ? canonical.data : "");
    sb_add(&html, "\"><link rel=\"alternate\" href=\"");
    sb_add_html_escaped(&html, app.data ? app.data : "");
    sb_add(&html, "\">");
  }
  sb_add(&html, "<link rel=\"alternate\" type=\"application/rss+xml\" "
                "title=\"Nytrix RSS feed\" href=\"");
  sb_add_html_escaped(&html, feed.data ? feed.data : "feed.xml");
  sb_add(&html, "\">");
  sb_add(
      &html,
      "<style>:root{color-scheme:dark}body{margin:0;background:#000;color:#"
      "f5f5f6;font:15px/1.62 "
      "system-ui,-apple-system,Segoe "
      "UI,sans-serif}body:before{content:\"\";position:fixed;inset:0;pointer-"
      "events:none;"
      "background:linear-gradient(90deg,rgba(182,160,255,.01) 1px,transparent "
      "1px),linear-gradient(180deg,rgba(182,160,255,.007) 1px,transparent 1px);"
      "background-size:48px 48px}main{max-width:920px;margin:0 "
      "auto;padding:34px 16px 68px;position:relative}a{color:#8ccfff;"
      "text-decoration:none;border-bottom:1px solid "
      "rgba(182,160,255,.24)}h1,h2,h3,h4{line-height:1.18;color:#fff}h1{font-"
      "size:31px;margin:0 0 17px}"
      "h2{margin-top:27px;border-left:2px solid "
      "#b6a0ff;padding-left:10px;font-size:21px}h3{font-size:17px}p,li{color:#"
      "c6c6ca}"
      "code,pre{font-family:JetBrains "
      "Mono,ui-monospace,monospace}code{background:#030304;border:1px solid "
      "#17171b;border-radius:2px;padding:1px 4px;color:#dedee3}"
      "pre{overflow:auto;background:#010102;border:1px solid "
      "#17171b;border-radius:2px;padding:11px}table{width:100%;border-collapse:"
      "collapse;margin:14px 0;"
      "background:#020203;border:1px solid #17171b}td{border-top:1px solid "
      "#17171b;padding:7px 8px;color:#c6c6ca;vertical-align:top}tr:first-child "
      "td{border-top:0;color:#f5f5f6}"
      ".open{display:inline-flex;margin:0 0 24px;padding:6px 8px;border:1px "
      "solid #292832;border-radius:2px;color:#d6d3e6;background:#020203;"
      "font:700 12px ui-monospace,monospace}.site-end{margin-top:26px;padding-"
      "top:12px;border-top:1px solid #17171b}.site-end-head{display:flex;"
      "align-items:end;justify-content:space-between;gap:10px;margin-bottom:"
      "8px}.site-end h2{margin:0;border-left:0;padding-left:0;font-size:16px}"
      ".site-end p{margin:0;color:#808087;font-size:12px}.site-links{display:"
      "flex;flex-wrap:wrap;gap:6px}.site-links a{display:inline-flex;align-"
      "items:center;min-height:24px;padding:3px 7px;border:1px solid #292832;"
      "background:#020203;font:700 11px ui-monospace,monospace}.site-links "
      "a.primary{color:#d6d3e6;border-color:rgba(182,160,255,.32)}</style></head>"
      "<body><main><a "
      "class=\"open\" "
      "href=\"");
  sb_add_html_escaped(&html, app.data ? app.data : "");
  sb_add(&html, "\">Open Manual</a><article>");
  sb_add(&html, body_html ? body_html : "");
  sb_add(&html, "</article>");
  append_static_site_end_html(&html, feed.data ? feed.data : "feed.xml");
  sb_add(&html, "</main></body></html>");

  int ok = write_file(path, html.data ? html.data : "");
  free(canonical.data);
  free(app.data);
  free(feed.data);
  free(root_rel.data);
  free(html.data);
  return ok;
}

static void append_sitemap_url(sb_t *sitemap, const char *site, const char *rel,
                               const char *priority) {
  if (!sitemap || !site || !*site)
    return;
  sb_t loc = {0};
  append_site_url(&loc, site, rel);
  if (loc.len && loc.data[loc.len - 1] != '/' &&
      (!rel || !strchr(path_basename(rel), '.')))
    sb_add(&loc, "/");
  sb_add(sitemap, "  <url><loc>");
  sb_add_xml_escaped(sitemap, loc.data ? loc.data : "");
  sb_add(sitemap, "</loc><changefreq>weekly</changefreq><priority>");
  sb_add(sitemap, priority ? priority : "0.7");
  sb_add(sitemap, "</priority></url>\n");
  free(loc.data);
}

static void format_rss_date(time_t when, char *out, size_t out_n) {
  if (!out || out_n == 0)
    return;
  if (when <= 0)
    when = time(NULL);
  struct tm tmv;
#ifdef _WIN32
  if (gmtime_s(&tmv, &when) != 0) {
    snprintf(out, out_n, "Thu, 01 Jan 1970 00:00:00 GMT");
    return;
  }
#else
  if (!gmtime_r(&when, &tmv)) {
    snprintf(out, out_n, "Thu, 01 Jan 1970 00:00:00 GMT");
    return;
  }
#endif
  if (!strftime(out, out_n, "%a, %d %b %Y %H:%M:%S GMT", &tmv))
    snprintf(out, out_n, "Thu, 01 Jan 1970 00:00:00 GMT");
}

static int cmp_doc_meta_recent(const void *a, const void *b) {
  const ny_doc_meta_entry_t *da = (const ny_doc_meta_entry_t *)a;
  const ny_doc_meta_entry_t *db = (const ny_doc_meta_entry_t *)b;
  if (da->mtime != db->mtime)
    return da->mtime > db->mtime ? -1 : 1;
  const char *ra = da->route ? da->route : "";
  const char *rb = db->route ? db->route : "";
  return strcmp(ra, rb);
}

static time_t doc_meta_latest_mtime(const ny_doc_meta_list_t *docs) {
  time_t latest = 0;
  if (!docs)
    return time(NULL);
  for (size_t i = 0; i < docs->len; i++) {
    if (docs->data[i].mtime > latest)
      latest = docs->data[i].mtime;
  }
  return latest > 0 ? latest : time(NULL);
}

static const char *doc_feed_category(const char *route) {
  if (!route)
    return "Documentation";
  if (strncmp(route, "learn/", 6) == 0)
    return "Learn";
  if (strncmp(route, "spec/", 5) == 0)
    return "Specification";
  return "Project";
}

static char *rss_doc_fallback_summary(const ny_doc_meta_entry_t *doc) {
  sb_t out = {0};
  sb_add(&out, "Nytrix documentation page");
  if (doc && doc->title && *doc->title) {
    sb_add(&out, ": ");
    sb_add(&out, doc->title);
  }
  sb_add(&out, ".");
  return out.data ? out.data : strdup("Nytrix documentation page.");
}

static char *rss_doc_summary(const char *root, const ny_doc_meta_entry_t *doc) {
  if (!doc || !doc->path)
    return rss_doc_fallback_summary(doc);
  if (path_has_suffix(doc->path, ".texi"))
    return rss_doc_fallback_summary(doc);
  char *txt = ny_read_file_raw(doc->path, NULL);
  if (!txt)
    return rss_doc_fallback_summary(doc);
  char *expanded = prepare_markdown_doc_body(root, txt);
  char *summary = markdown_summary(expanded ? expanded : txt);
  free(expanded);
  free(txt);
  return summary ? summary : rss_doc_fallback_summary(doc);
}

static void append_rss_item(sb_t *rss, const char *title, const char *link,
                            const char *description, const char *category,
                            time_t pub_date) {
  char date[64];
  format_rss_date(pub_date, date, sizeof(date));
  sb_add(rss, "    <item>\n      <title>");
  sb_add_xml_escaped(rss, title ? title : "Nytrix");
  sb_add(rss, "</title>\n      <link>");
  sb_add_xml_escaped(rss, link ? link : "");
  sb_add(rss, "</link>\n      <guid isPermaLink=\"true\">");
  sb_add_xml_escaped(rss, link ? link : "");
  sb_add(rss, "</guid>\n      <pubDate>");
  sb_add_xml_escaped(rss, date);
  sb_add(rss, "</pubDate>\n");
  if (category && *category) {
    sb_add(rss, "      <category>");
    sb_add_xml_escaped(rss, category);
    sb_add(rss, "</category>\n");
  }
  sb_add(rss, "      <description>");
  sb_add_xml_escaped(rss, description ? description : "");
  sb_add(rss, "</description>\n    </item>\n");
}

static void write_rss_feed(const char *out_dir, const char *site,
                           const char *root) {
  char path[PATH_MAX];
  if (!join_path(path, sizeof(path), out_dir, "feed.xml"))
    return;

  ny_doc_meta_list_t docs = {0};
  ny_doc_meta_collect(root, &docs);
  if (docs.len > 1)
    qsort(docs.data, docs.len, sizeof(*docs.data), cmp_doc_meta_recent);
  time_t latest = doc_meta_latest_mtime(&docs);

  sb_t rss = {0}, root_url = {0}, api_url = {0}, feed_url = {0};
  if (site && *site)
    append_site_url(&root_url, site, "");
  else
    sb_add(&root_url, ".");
  append_site_url(&api_url, site, "api/");
  append_site_url(&feed_url, site, "feed.xml");
  if (!feed_url.data || !*feed_url.data)
    sb_add(&feed_url, "feed.xml");

  char latest_date[64];
  format_rss_date(latest, latest_date, sizeof(latest_date));

  sb_add(&rss, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
               "<rss version=\"2.0\" "
               "xmlns:atom=\"http://www.w3.org/2005/Atom\">\n"
               "  <channel>\n"
               "    <title>Nytrix RSS feed</title>\n"
               "    <link>");
  sb_add_xml_escaped(&rss, root_url.data ? root_url.data : "");
  sb_add(&rss, "</link>\n"
               "    <description>Nytrix updates: documentation, release "
               "notes, and API index changes.</description>\n"
               "    <language>en</language>\n"
               "    <generator>ny tools web</generator>\n"
               "    <docs>https://www.rssboard.org/rss-specification</docs>\n"
               "    <ttl>60</ttl>\n"
               "    <lastBuildDate>");
  sb_add_xml_escaped(&rss, latest_date);
  sb_add(&rss, "</lastBuildDate>\n"
               "    <pubDate>");
  sb_add_xml_escaped(&rss, latest_date);
  sb_add(&rss, "</pubDate>\n"
               "    <atom:link href=\"");
  sb_add_xml_escaped(&rss, feed_url.data ? feed_url.data : "feed.xml");
  sb_add(&rss, "\" rel=\"self\" type=\"application/rss+xml\" />\n");
  append_rss_item(&rss, "Nytrix Manual", root_url.data ? root_url.data : "",
                  "Nytrix manual, specification, examples, and source-linked "
                  "API reference.",
                  "Documentation", latest);

  for (size_t i = 0; i < docs.len; i++) {
    sb_t page_url = {0};
    append_doc_url(&page_url, site, docs.data[i].route);
    char *summary = rss_doc_summary(root, &docs.data[i]);
    append_rss_item(&rss, docs.data[i].title ? docs.data[i].title : "",
                    page_url.data ? page_url.data : "",
                    summary ? summary : "Nytrix documentation page.",
                    doc_feed_category(docs.data[i].route),
                    docs.data[i].mtime > 0 ? docs.data[i].mtime : latest);
    free(page_url.data);
    free(summary);
  }
  append_rss_item(&rss, "Nytrix Standard Library API",
                  api_url.data ? api_url.data : "api/",
                  "Source-linked map for the bundled standard library.", "API",
                  latest);
  sb_add(&rss, "  </channel>\n</rss>\n");

  write_file(path, rss.data ? rss.data : "");
  ny_doc_meta_list_free(&docs);
  free(root_url.data);
  free(api_url.data);
  free(feed_url.data);
  free(rss.data);
}

static void generate_static_docs_from_dir(const char *out_dir,
                                          const char *root,
                                          const char *docs_dir, const char *dir,
                                          const char *site, sb_t *sitemap,
                                          sb_t *plain, sb_t *full,
                                          int recursive, int *count) {
  DIR *d = opendir(dir);
  if (!d)
    return;
  char **names = NULL;
  int names_n = 0, names_cap = 0;
  struct dirent *ent;
  while ((ent = readdir(d))) {
    if (web_skip_source_dir(ent->d_name))
      continue;
    if (names_n == names_cap) {
      int next_cap = names_cap ? names_cap * 2 : 32;
      char **next = (char **)realloc(names, (size_t)next_cap * sizeof(char *));
      if (!next)
        break;
      names = next;
      names_cap = next_cap;
    }
    names[names_n++] = strdup(ent->d_name);
  }
  closedir(d);
  qsort(names, (size_t)names_n, sizeof(char *), cmp_cstr_ptr);
  for (int i = 0; i < names_n; i++) {
    char path[PATH_MAX];
    if (join_path(path, sizeof(path), dir, names[i])) {
      if (recursive && path_is_dir(path)) {
        generate_static_docs_from_dir(out_dir, root, docs_dir, path, site,
                                      sitemap, plain, full, recursive, count);
      } else if (path_has_suffix(path, ".md")) {
        char *route = doc_route_name(docs_dir, path);
        char *title = doc_title_from_route(route);
        char *txt = ny_read_file_raw(path, NULL);
        char *expanded = prepare_markdown_doc_body(root, txt);
        char *summary = markdown_summary(expanded);
        char *body = render_markdown_static(expanded, route);
        if (route && title && body &&
            write_static_doc_page(out_dir, site, route, title, summary, body)) {
          char safe[PATH_MAX];
          if (site && *site && safe_route_path(safe, sizeof(safe), route))
            append_sitemap_url(sitemap, site, safe,
                               strstr(route, "learn/") || strstr(route, "spec/")
                                   ? "0.84"
                                   : "0.64");
        }
        if (route && title) {
          sb_add(plain, "- [");
          sb_add(plain, title);
          sb_add(plain, "](");
          sb_add(plain, route);
          sb_add(plain, "/): ");
          sb_add(plain, summary);
          sb_add(plain, "\n");
          sb_add(full, "\n\n# ");
          sb_add(full, title);
          sb_add(full, "\n\n");
          sb_add(full, txt ? txt : "");
          if (count)
            (*count)++;
        }
        free(route);
        free(title);
        free(txt);
        free(expanded);
        free(summary);
        free(body);
      }
    }
    free(names[i]);
  }
  free(names);
}

static char *json_read_string(const char **pp) {
  const char *p = *pp;
  if (*p != '"')
    return NULL;
  p++;
  sb_t out = {0};
  int esc = 0;
  for (; *p; p++) {
    char c = *p;
    if (esc) {
      if (c == 'n')
        sb_add(&out, "\n");
      else if (c == 't')
        sb_add(&out, "\t");
      else
        sb_addn(&out, &c, 1);
      esc = 0;
      continue;
    }
    if (c == '\\') {
      esc = 1;
      continue;
    }
    if (c == '"') {
      *pp = p + 1;
      return out.data ? out.data : strdup("");
    }
    sb_addn(&out, &c, 1);
  }
  free(out.data);
  return NULL;
}

static void write_api_seo_index(const char *out_dir, const char *site,
                                const char *docs_json, sb_t *sitemap,
                                sb_t *plain, int module_count,
                                int symbol_count) {
  char dir[PATH_MAX], path[PATH_MAX], txt_path[PATH_MAX];
  if (!join_path(dir, sizeof(dir), out_dir, "api") || !mkdir_p(dir) ||
      !join_path(path, sizeof(path), dir, "index.html") ||
      !join_path(txt_path, sizeof(txt_path), dir, "modules.txt"))
    return;
  sb_t html = {0}, txt = {0}, feed = {0};
  sb_add(&html, "<!doctype html><html lang=\"en\"><head><meta "
                "charset=\"utf-8\"><meta name=\"viewport\" "
                "content=\"width=device-width,initial-scale=1\"><meta "
                "name=\"robots\" content=\"index,follow\">"
                "<title>Nytrix - Standard Library API</title><meta "
                "name=\"description\" content=\"Source-linked Nytrix "
                "standard library reference for core APIs, OS integration, "
                "native FFI, networking, parsers, math, media, and UI.\">");
  sb_t api_url = {0};
  if (site && *site) {
    append_site_url(&api_url, site, "api/");
    append_site_url(&feed, site, "feed.xml");
    sb_add(&html, "<link rel=\"canonical\" href=\"");
    sb_add_html_escaped(&html, api_url.data ? api_url.data : "");
    sb_add(&html, "\">");
  } else {
    sb_add(&feed, "../feed.xml");
  }
  sb_add(&html, "<link rel=\"alternate\" type=\"application/rss+xml\" "
                "title=\"Nytrix RSS feed\" href=\"");
  sb_add_html_escaped(&html, feed.data ? feed.data : "../feed.xml");
  sb_add(&html, "\">");
  sb_add(
      &html,
      "<style>:root{color-scheme:dark}body{margin:0;background:#000;color:#"
      "f5f5f6;font:15px/1.58 "
      "system-ui,-apple-system,Segoe "
      "UI,sans-serif}body:before{content:\"\";position:fixed;inset:0;pointer-"
      "events:none;"
      "background:linear-gradient(90deg,rgba(182,160,255,.01) 1px,transparent "
      "1px),linear-gradient(180deg,rgba(182,160,255,.007) 1px,transparent 1px);"
      "background-size:48px 48px}main{max-width:980px;margin:0 "
      "auto;padding:34px 16px 68px;position:relative}"
      "a{color:#8ccfff;text-decoration:none;border-bottom:1px solid "
      "rgba(182,160,255,.22)}h1{font-size:31px;line-height:1.15;margin:0 0 "
      "12px}"
      "p{color:#808087}ul{columns:3;column-gap:28px;padding-left:18px}li{break-"
      "inside:avoid;margin:3px 0;color:#c6c6ca}"
      ".site-end{margin-top:26px;padding-top:12px;border-top:1px solid "
      "#17171b}.site-end-head{display:flex;align-items:end;justify-content:"
      "space-between;gap:10px;margin-bottom:8px}.site-end h2{margin:0;font-"
      "size:16px}.site-end p{margin:0;color:#808087;font-size:12px}.site-"
      "links{display:flex;flex-wrap:wrap;gap:6px}.site-links a{display:inline-"
      "flex;align-items:center;min-height:24px;padding:3px 7px;border:1px "
      "solid #292832;background:#020203;font:700 11px ui-monospace,monospace}"
      ".site-links a.primary{color:#d6d3e6;border-color:rgba(182,160,255,.32)}"
      "@media(max-width:800px){ul{columns:1}}</style></"
      "head><body><main><h1>Nytrix Standard Library "
      "API</h1><p>");
  char count_buf[128];
  (void)module_count;
  (void)symbol_count;
  snprintf(count_buf, sizeof(count_buf),
           "Source-indexed API pages for local lookup.");
  sb_add_html_escaped(&html, count_buf);
  sb_add(&html, "</p><ul>");
  sb_add(&txt, "Nytrix Standard Library API\n\n");
  char **names = NULL;
  int names_n = 0, names_cap = 0;
  const char *p = docs_json;
  while ((p = strstr(p, "\"name\":\"std."))) {
    p += strlen("\"name\":");
    char *name = json_read_string(&p);
    if (!name)
      break;
    if (names_n == names_cap) {
      int next_cap = names_cap ? names_cap * 2 : 128;
      char **next = (char **)realloc(names, (size_t)next_cap * sizeof(char *));
      if (!next) {
        free(name);
        break;
      }
      names = next;
      names_cap = next_cap;
    }
    names[names_n++] = name;
  }
  qsort(names, (size_t)names_n, sizeof(char *), cmp_cstr_ptr);
  for (int i = 0; i < names_n; i++) {
    char *name = names[i];
    sb_add(&html, "<li><a href=\"../index.html#");
    sb_add_html_escaped(&html, name);
    sb_add(&html, "\">");
    sb_add_html_escaped(&html, name);
    sb_add(&html, "</a></li>");
    sb_add(&txt, name);
    sb_add(&txt, "\n");
    free(name);
  }
  free(names);
  sb_add(&html, "</ul>");
  append_static_site_end_html(&html, feed.data ? feed.data : "../feed.xml");
  sb_add(&html, "</main></body></html>");
  write_file(path, html.data ? html.data : "");
  write_file(txt_path, txt.data ? txt.data : "");
  append_sitemap_url(sitemap, site, "api/", "0.8");
  sb_add(plain, "- [Standard Library API](api/): source-linked map for the "
                "bundled standard library.\n");
  free(api_url.data);
  free(feed.data);
  free(html.data);
  free(txt.data);
}

static void write_seo_artifacts(const char *out_dir, const char *root,
                                const char *docs_json, int module_count,
                                int symbol_count, const char *site) {
  int has_site = site && *site;
  sb_t sitemap = {0}, plain = {0}, full = {0}, robots = {0};
  if (has_site) {
    sb_add(&sitemap,
           "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
           "<urlset xmlns=\"http://www.sitemaps.org/schemas/sitemap/0.9\">\n");
    append_sitemap_url(&sitemap, site, "", "1.0");
  }
  sb_add(&plain, "# Nytrix\n\n");
  sb_add(&plain, "> Nytrix is a compact native programming language with "
                 "compile-time execution, native FFI, OS APIs, networking, "
                 "parsing, UI, and source-linked local documentation.\n\n");
  sb_add(&plain, "## Public Manual\n\n");
  sb_add(&full, "# Nytrix Full Documentation\n");

  char docs_dir[PATH_MAX];
  int doc_count = 0;
  if (join_path(docs_dir, sizeof(docs_dir), root, "docs"))
    generate_static_docs_from_dir(out_dir, root, docs_dir, docs_dir, site,
                                  has_site ? &sitemap : NULL, &plain, &full, 1,
                                  &doc_count);

  sb_add(&plain, "\n## API\n\n");
  write_api_seo_index(out_dir, site, docs_json, has_site ? &sitemap : NULL,
                      &plain, module_count, symbol_count);
  sb_add(&plain,
            "\n## Feed and social\n\n"
            "- [RSS](feed.xml): reader-ready feed for documentation, "
            "release notes, and API index changes.\n"
         "- [Discord](https://discord.gg/XQDR6DZWb)\n"
         "- [Mastodon](https://mastodon.social/@nytrix)\n");
  if (has_site)
    append_sitemap_url(&sitemap, site, "feed.xml", "0.5");
  if (has_site)
    sb_add(&sitemap, "</urlset>\n");

  sb_t sitemap_url = {0};
  if (has_site) {
    append_site_url(&sitemap_url, site, "sitemap.xml");
    sb_add(&robots, "User-agent: *\nAllow: /\nSitemap: ");
    sb_add(&robots, sitemap_url.data ? sitemap_url.data : "");
    sb_add(&robots, "\n");
  }

  char path[PATH_MAX];
  if (has_site && join_path(path, sizeof(path), out_dir, "sitemap.xml"))
    write_file(path, sitemap.data ? sitemap.data : "");
  else if (!has_site && join_path(path, sizeof(path), out_dir, "sitemap.xml"))
    unlink(path);
  if (has_site && join_path(path, sizeof(path), out_dir, "robots.txt"))
    write_file(path, robots.data ? robots.data : "");
  else if (!has_site && join_path(path, sizeof(path), out_dir, "robots.txt"))
    unlink(path);
  write_rss_feed(out_dir, site, root);
  if (join_path(path, sizeof(path), out_dir, "manual.txt"))
    write_file(path, plain.data ? plain.data : "");
  if (join_path(path, sizeof(path), out_dir, "manual-full.txt"))
    write_file(path, full.data ? full.data : "");
  if (join_path(path, sizeof(path), out_dir, "site.txt")) {
    sb_t site_txt = {0};
    sb_add(&site_txt, "Nytrix Lang native programming language documentation\n");
    sb_add(&site_txt, "URL: ");
    sb_add(&site_txt, has_site ? site : "local");
    sb_add(&site_txt, "\nDocs pages: ");
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "%d\nAPI index: source-linked\n", doc_count);
    sb_add(&site_txt, tmp);
    sb_add(&site_txt, "RSS feed: feed.xml\n");
    sb_add(&site_txt, "Discord: https://discord.gg/XQDR6DZWb\n");
    sb_add(&site_txt, "Mastodon: https://mastodon.social/@nytrix\n");
    write_file(path, site_txt.data ? site_txt.data : "");
    free(site_txt.data);
  }
  free(sitemap_url.data);
  free(sitemap.data);
  free(plain.data);
  free(full.data);
  free(robots.data);
}

#ifndef _WIN32
static const char *guess_mime(const char *path) {
  const char *dot = strrchr(path, '.');
  if (!dot)
    return "application/octet-stream";
  if (strcmp(dot, ".html") == 0 || strcmp(dot, ".htm") == 0)
    return "text/html; charset=utf-8";
  if (strcmp(dot, ".css") == 0)
    return "text/css; charset=utf-8";
  if (strcmp(dot, ".js") == 0)
    return "application/javascript; charset=utf-8";
  if (strcmp(dot, ".json") == 0)
    return "application/json; charset=utf-8";
  if (strcmp(dot, ".xml") == 0)
    return "application/xml; charset=utf-8";
  if (strcmp(dot, ".svg") == 0)
    return "image/svg+xml";
  if (strcmp(dot, ".png") == 0)
    return "image/png";
  if (strcmp(dot, ".jpg") == 0 || strcmp(dot, ".jpeg") == 0)
    return "image/jpeg";
  if (strcmp(dot, ".gif") == 0)
    return "image/gif";
  if (strcmp(dot, ".txt") == 0)
    return "text/plain; charset=utf-8";
  return "application/octet-stream";
}

static int read_file_bytes(const char *path, char **out, size_t *out_n) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return 0;
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return 0;
  }
  long n = ftell(f);
  if (n < 0) {
    fclose(f);
    return 0;
  }
  rewind(f);
  char *buf = (char *)malloc((size_t)n + 1);
  if (!buf) {
    fclose(f);
    return 0;
  }
  size_t got = fread(buf, 1, (size_t)n, f);
  fclose(f);
  buf[got] = '\0';
  *out = buf;
  *out_n = got;
  return 1;
}

static int send_all(int fd, const char *buf, size_t n) {
  size_t off = 0;
  while (off < n) {
    ssize_t wr = send(fd, buf + off, n - off, 0);
    if (wr <= 0)
      return 0;
    off += (size_t)wr;
  }
  return 1;
}

static void serve_client(int cfd, const char *root) {
  char req[4096];
  ssize_t rn = recv(cfd, req, sizeof(req) - 1, 0);
  if (rn <= 0)
    return;
  req[rn] = '\0';

  char method[16] = {0}, path[1024] = {0};
  if (sscanf(req, "%15s %1023s", method, path) < 2) {
    const char *resp = "HTTP/1.1 400 Bad Request\r\nConnection: "
                       "close\r\nContent-Length: 0\r\n\r\n";
    (void)send_all(cfd, resp, strlen(resp));
    return;
  }
  if (strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0) {
    const char *resp = "HTTP/1.1 405 Method Not Allowed\r\nConnection: "
                       "close\r\nContent-Length: 0\r\n\r\n";
    (void)send_all(cfd, resp, strlen(resp));
    return;
  }

  char local[PATH_MAX];
  const char *rel = path;
  if (rel[0] == '/')
    rel++;
  if (*rel == '\0')
    rel = "index.html";
  if (strstr(rel, "..")) {
    const char *resp = "HTTP/1.1 403 Forbidden\r\nConnection: "
                       "close\r\nContent-Length: 0\r\n\r\n";
    (void)send_all(cfd, resp, strlen(resp));
    return;
  }
  snprintf(local, sizeof(local), "%s/%s", root, rel);

  char *body = NULL;
  size_t body_n = 0;
  int ok = read_file_bytes(local, &body, &body_n);
  if (!ok) {
    const char *msg = "404 Not Found\n";
    char hdr[256];
    snprintf(hdr, sizeof(hdr),
             "HTTP/1.1 404 Not Found\r\nConnection: close\r\nContent-Type: "
             "text/plain; charset=utf-8\r\nContent-Length: "
             "%zu\r\n\r\n",
             strlen(msg));
    (void)send_all(cfd, hdr, strlen(hdr));
    if (strcmp(method, "HEAD") != 0)
      (void)send_all(cfd, msg, strlen(msg));
    return;
  }

  const char *mime = guess_mime(local);
  char hdr[512];
  snprintf(hdr, sizeof(hdr),
           "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: "
           "%zu\r\nCache-Control: no-cache\r\n\r\n",
           mime, body_n);
  (void)send_all(cfd, hdr, strlen(hdr));
  if (strcmp(method, "HEAD") != 0)
    (void)send_all(cfd, body, body_n);
  free(body);
}

static int serve_http_forever(const char *root, int port) {
  int sfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sfd < 0) {
    nyt_err("ny-doc", "socket failed: %s", strerror(errno));
    return 1;
  }
  int one = 1;
  setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons((uint16_t)port);

  if (bind(sfd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    nyt_err("ny-doc", "bind failed on port %d: %s", port, strerror(errno));
    close(sfd);
    return 1;
  }
  if (listen(sfd, 64) != 0) {
    nyt_err("ny-doc", "listen failed: %s", strerror(errno));
    close(sfd);
    return 1;
  }

  nyt_msg("SERVE", NYT_CYAN, "docs from %s at http://127.0.0.1:%d/", root,
          port);
  for (;;) {
    int cfd = accept(sfd, NULL, NULL);
    if (cfd < 0)
      continue;
    serve_client(cfd, root);
    close(cfd);
  }
}
#endif

typedef struct {
  const char *input;
  const char *out_dir;
  const char *site_url;
  int serve;
  int port;
} web_opts_t;

typedef struct {
  char index_path[PATH_MAX];
  char cache_web_path[PATH_MAX];
  char cache_og_svg_path[PATH_MAX];
  char cache_og_png_path[PATH_MAX];
  char cache_website_dir[PATH_MAX];
  char website_assets_dir[PATH_MAX];
  char out_assets_dir[PATH_MAX];
  char out_website_assets_dir[PATH_MAX];
  char template_path[PATH_MAX];
  char js_path[PATH_MAX];
  char css_path[PATH_MAX];
  char favicon_path[PATH_MAX];
  char mono_font_path[PATH_MAX];
  char display_font_path[PATH_MAX];
  char out_favicon_path[PATH_MAX];
  char logo_path[PATH_MAX];
  char logo_png_path[PATH_MAX];
  char out_logo_path[PATH_MAX];
  char out_logo_png_path[PATH_MAX];
  char out_og_svg_path[PATH_MAX];
  char out_og_png_path[PATH_MAX];
  char out_assets_og_svg_path[PATH_MAX];
  char out_assets_og_png_path[PATH_MAX];
} web_paths_t;

static void web_print_usage(void) {
  nyt_heading("Nytrix Documentation");
  printf("%susage:%s %sny doc%s %s[-o OUTPUT] [--site URL] [-s] [-p PORT] "
         "[--color MODE] [input]%s\n",
         nyt_clr(NYT_BOLD), nyt_clr(NYT_RESET), nyt_clr(NYT_CYAN),
         nyt_clr(NYT_RESET), nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
  printf("       %sny doc search%s %s[--input PATH] [--docs|--symbols] [-n "
         "N] QUERY%s\n",
         nyt_clr(NYT_CYAN), nyt_clr(NYT_RESET), nyt_clr(NYT_GREEN),
         nyt_clr(NYT_RESET));
  printf("       %sny doc get%s %s[--input PATH] [--docs|--symbols] "
         "QUERY%s\n\n",
         nyt_clr(NYT_CYAN), nyt_clr(NYT_RESET), nyt_clr(NYT_GREEN),
         nyt_clr(NYT_RESET));
  printf("Generates the docs portal; %ssearch%s fuzzy-searches docs and "
         "%sget%s prints the best match.\n",
         nyt_clr(NYT_CYAN), nyt_clr(NYT_RESET), nyt_clr(NYT_CYAN),
         nyt_clr(NYT_RESET));
}

static int web_parse_args(int argc, char **argv, web_opts_t *opts, char *err,
                          size_t err_sz) {
  memset(opts, 0, sizeof(*opts));
  opts->out_dir = "build/docs";
  opts->port = 8000;

  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (ny_arg_match(a, "--help", "-h")) {
      web_print_usage();
      return 1;
    }
    int color_mode = -2;
    int color_idx = i;
    int color_rc = ny_arg_consume_color(&color_idx, argc, argv, &color_mode,
                                        err, err_sz);
    if (color_rc < 0) {
      nyt_err("ny-doc", "%s", err);
      return 2;
    }
    if (color_rc > 0) {
      ny_arg_apply_color_mode(color_mode);
      i = color_idx;
      continue;
    }
    if (ny_arg_match(a, "--output", "-o")) {
      const char *v = NULL;
      if (!ny_arg_take_value(a, &i, argc, argv, &v, err, err_sz)) {
        nyt_err("ny-doc", "%s", err);
        return 2;
      }
      opts->out_dir = v;
    } else if (ny_arg_match_with_value(a, "--site") ||
               ny_arg_match(a, "--site", NULL) ||
               ny_arg_match_with_value(a, "--site-url") ||
               ny_arg_match(a, "--site-url", NULL)) {
      const char *v = NULL;
      if (!ny_arg_take_value(a, &i, argc, argv, &v, err, err_sz)) {
        nyt_err("ny-doc", "%s", err);
        return 2;
      }
      opts->site_url = v;
    } else if (ny_arg_match(a, "--port", "-p")) {
      if (!ny_arg_take_int(a, &i, argc, argv, 1, 65535, &opts->port, "port",
                           err, err_sz)) {
        nyt_err("ny-doc", "%s", err);
        return 2;
      }
    } else if (ny_arg_match(a, "--serve", "-s"))
      opts->serve = 1;
    else if (a[0] == '-') {
      nyt_err("ny-doc", "unknown option: %s", a);
      return 2;
    } else
      opts->input = a;
  }
  return 0;
}

static const char *web_resolve_input(const char *root, const char *input,
                                     char *default_input) {
  if (input)
    return input;
  if (join_path(default_input, PATH_MAX, root, "lib") &&
      path_is_dir(default_input))
    return default_input;
  return root;
}

static int web_build_asset_paths(const char *root, const char *out_dir,
                                 const char *cache_docs_dir,
                                 const char *cache_website_dir,
                                 web_paths_t *paths) {
  snprintf(paths->cache_website_dir, sizeof(paths->cache_website_dir), "%s",
           cache_website_dir);
  return join_path(paths->index_path, sizeof(paths->index_path), out_dir,
                   "index.html") &&
         join_path(paths->cache_web_path, sizeof(paths->cache_web_path),
                   cache_docs_dir, "web.html") &&
         join_path(paths->cache_og_svg_path, sizeof(paths->cache_og_svg_path),
                   cache_website_dir, "og.svg") &&
         join_path(paths->cache_og_png_path, sizeof(paths->cache_og_png_path),
                   cache_website_dir, "og.png") &&
         join_path(paths->website_assets_dir, sizeof(paths->website_assets_dir),
                   root, "etc/assets/website") &&
         join_path(paths->template_path, sizeof(paths->template_path),
                   paths->website_assets_dir, "web.html") &&
         join_path(paths->js_path, sizeof(paths->js_path),
                   paths->website_assets_dir, "web.js") &&
         join_path(paths->css_path, sizeof(paths->css_path),
                   paths->website_assets_dir, "web.css") &&
         join_path(paths->favicon_path, sizeof(paths->favicon_path),
                   paths->website_assets_dir, "favicon.svg") &&
         join_path(paths->mono_font_path, sizeof(paths->mono_font_path), root,
                   "etc/assets/fonts/jetbrains.ttf") &&
         join_path(paths->display_font_path, sizeof(paths->display_font_path),
                   root, "etc/assets/fonts/quantico.ttf") &&
         join_path(paths->logo_path, sizeof(paths->logo_path),
                   paths->website_assets_dir, "logo.svg") &&
         join_path(paths->logo_png_path, sizeof(paths->logo_png_path),
                   paths->website_assets_dir, "logo.png") &&
         join_path(paths->out_favicon_path, sizeof(paths->out_favicon_path),
                   out_dir, "favicon.svg") &&
         join_path(paths->out_logo_path, sizeof(paths->out_logo_path), out_dir,
                   "logo.svg") &&
         join_path(paths->out_logo_png_path, sizeof(paths->out_logo_png_path),
                   out_dir, "logo.png") &&
         join_path(paths->out_og_svg_path, sizeof(paths->out_og_svg_path),
                   out_dir, "og.svg") &&
         join_path(paths->out_og_png_path, sizeof(paths->out_og_png_path),
                   out_dir, "og.png") &&
         join_path(paths->out_assets_dir, sizeof(paths->out_assets_dir),
                   out_dir, "assets") &&
         join_path(paths->out_website_assets_dir,
                   sizeof(paths->out_website_assets_dir), paths->out_assets_dir,
                   "website") &&
         join_path(paths->out_assets_og_svg_path,
                   sizeof(paths->out_assets_og_svg_path),
                   paths->out_website_assets_dir, "og.svg") &&
         join_path(paths->out_assets_og_png_path,
                   sizeof(paths->out_assets_og_png_path),
                   paths->out_website_assets_dir, "og.png");
}

static int web_render_docs_page(const char *root, const char *site_url,
                                const char *bundle, int source_files,
                                const char *out_dir,
                                const web_paths_t *paths,
                                int *parsed_modules, int *parsed_symbols) {
  char *html_tpl = ny_read_file_raw(paths->template_path, NULL);
  char *js_tpl = ny_read_file_raw(paths->js_path, NULL);
  char *css_tpl = ny_read_file_raw(paths->css_path, NULL);
  if (!html_tpl) {
    free(js_tpl);
    free(css_tpl);
    nyt_err("ny-doc", "failed to read template: %s", paths->template_path);
    return 0;
  }
  if (!js_tpl)
    js_tpl = strdup("");
  if (!css_tpl)
    css_tpl = strdup("");

  char *docs_json =
      parse_docs_json(bundle, root, parsed_modules, parsed_symbols);
  if (!docs_json) {
    free(html_tpl);
    free(js_tpl);
    free(css_tpl);
    nyt_err("ny-doc", "failed to build docs data");
    return 0;
  }
  if (!generate_website_og_assets(paths->cache_og_svg_path,
                                  paths->cache_og_png_path, paths->logo_path,
                                  paths->favicon_path, paths->mono_font_path,
                                  paths->display_font_path, *parsed_modules,
                                  *parsed_symbols, source_files)) {
    free(html_tpl);
    free(js_tpl);
    free(css_tpl);
    free(docs_json);
    nyt_err("ny-doc", "failed generating website OG assets in %s",
            paths->cache_website_dir);
    return 0;
  }
  char *seo_head = render_site_head(site_url, root);
  char *html =
      replace_doc_template(html_tpl, docs_json, js_tpl, css_tpl, seo_head);
  free(html_tpl);
  free(js_tpl);
  free(css_tpl);
  free(seo_head);
  if (!html) {
    free(docs_json);
    nyt_err("ny-doc", "failed to render docs HTML");
    return 0;
  }

  if (!write_file(paths->cache_web_path, html)) {
    free(html);
    free(docs_json);
    nyt_err("ny-doc", "failed writing %s", paths->cache_web_path);
    return 0;
  }
  if (!write_file(paths->index_path, html)) {
    free(html);
    free(docs_json);
    nyt_err("ny-doc", "failed writing %s", paths->index_path);
    return 0;
  }
  free(html);
  write_seo_artifacts(out_dir, root, docs_json, *parsed_modules,
                      *parsed_symbols, site_url);
  free(docs_json);
  return 1;
}

static int web_copy_output_assets(const web_paths_t *paths) {
  if (ny_path_readable(paths->favicon_path) &&
      !copy_file_bytes(paths->favicon_path, paths->out_favicon_path)) {
    nyt_err("ny-doc", "failed writing %s", paths->out_favicon_path);
    return 0;
  }
  if (!ny_path_readable(paths->favicon_path))
    unlink(paths->out_favicon_path);
  if (ny_path_readable(paths->logo_path) &&
      !copy_file_bytes(paths->logo_path, paths->out_logo_path)) {
    nyt_err("ny-doc", "failed writing %s", paths->out_logo_path);
    return 0;
  }
  if (ny_path_readable(paths->logo_png_path) &&
      !copy_file_bytes(paths->logo_png_path, paths->out_logo_png_path)) {
    nyt_err("ny-doc", "failed writing %s", paths->out_logo_png_path);
    return 0;
  }
  if (!ny_path_readable(paths->logo_png_path))
    unlink(paths->out_logo_png_path);
  if (!copy_file_bytes(paths->cache_og_svg_path, paths->out_og_svg_path)) {
    nyt_err("ny-doc", "failed writing %s", paths->out_og_svg_path);
    return 0;
  }
  if (!copy_file_bytes(paths->cache_og_png_path, paths->out_og_png_path)) {
    nyt_err("ny-doc", "failed writing %s", paths->out_og_png_path);
    return 0;
  }
  if (!copy_regular_files_in_dir(paths->website_assets_dir,
                                 paths->out_website_assets_dir, "web.html")) {
    nyt_err("ny-doc", "failed writing website assets: %s",
            paths->out_website_assets_dir);
    return 0;
  }
  if (!copy_file_bytes(paths->cache_og_svg_path,
                       paths->out_assets_og_svg_path) ||
      !copy_file_bytes(paths->cache_og_png_path,
                       paths->out_assets_og_png_path)) {
    nyt_err("ny-doc", "failed writing generated website assets: %s",
            paths->out_website_assets_dir);
    return 0;
  }
  if (!ny_path_readable(paths->logo_png_path)) {
    char stale_png[PATH_MAX];
    if (join_path(stale_png, sizeof(stale_png), paths->out_website_assets_dir,
                  "logo.png"))
      unlink(stale_png);
  }
  return 1;
}

