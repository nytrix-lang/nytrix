/*
 * Ny-side frontend desugar pass (--ny-desugar=ny): runs the
 * core.compiler.desugar library over the user source through a child
 * compiler process and swaps in the rewritten text before the pipeline
 * parses it.  The C reference pass is the in-parser compound-assignment
 * desugar; --ny-desugar=c keeps that path unchanged, and the differential
 * harness proves both produce identical programs.  Textually included by
 * pipeline/init.c.
 */

#define NY_DESUGAR_SOURCE_MAX_BYTES (1u << 20)
#define NY_DESUGAR_OUTPUT_MAX_BYTES (1u << 20)
#define NY_DESUGAR_CPU_SECONDS 30
#define NY_DESUGAR_WALL_SECONDS 30
#define NY_DESUGAR_MAX_MEMORY_BYTES (1024ULL * 1024ULL * 1024ULL)
#define NY_DESUGAR_MAX_OUTPUT_BYTES (1ULL << 20)

static _Thread_local bool ny_desugar_session_active;

static bool ny_desugar_temp_paths(char *in_path, size_t in_cap, char *out_path,
                                  size_t out_cap) {
  const char *dir = ny_get_temp_dir();
  if (!dir || !*dir)
    return false;
  for (unsigned attempt = 0; attempt < 64; ++attempt) {
    uint64_t nonce = ny_ticks_now() + attempt;
    int n = snprintf(in_path, in_cap, "%s%cnyd-%llu-%u.ny", dir,
#ifdef _WIN32
                     '\\',
#else
                     '/',
#endif
                     (unsigned long long)nonce, attempt);
    if (n <= 0 || (size_t)n >= in_cap)
      return false;
    n = snprintf(out_path, out_cap, "%s.out", in_path);
    if (n <= 0 || (size_t)n >= out_cap)
      return false;
#ifdef _WIN32
    int fd = _open(in_path, _O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY,
                   _S_IREAD | _S_IWRITE);
#else
    int fd = open(in_path, O_CREAT | O_EXCL | O_WRONLY, 0600);
#endif
    if (fd >= 0) {
#ifdef _WIN32
      _close(fd);
#else
      close(fd);
#endif
      return true;
    }
    if (errno != EEXIST)
      return false;
  }
  return false;
}

static bool ny_desugar_escape_path(const char *path, char *out,
                                   size_t out_cap) {
  if (!path || !out || out_cap < 3)
    return false;
  size_t len = 0;
  for (const char *p = path; *p; ++p) {
    if (*p == '"' || *p == '\\') {
      if (len + 3 > out_cap)
        return false;
      out[len++] = '\\';
    } else if (len + 2 > out_cap) {
      return false;
    }
    out[len++] = *p;
  }
  if (len + 1 > out_cap)
    return false;
  out[len] = '\0';
  return true;
}

/*
 * Runs one desugar round trip; returns a malloc'd rewritten source or NULL
 * with a diagnostic on stderr.
 */
static char *ny_pipeline_desugar_via_ny(const char *source, size_t source_len,
                                        const char *display_name) {
  const char *configured = getenv("NYTRIX_NY_FULL_BIN");
  const char *exe =
      configured && *configured ? configured : ny_get_executable_path();
  if (!exe || !*exe) {
    fprintf(stderr,
            "error: --ny-desugar=ny could not locate the compiler binary\n");
    return NULL;
  }

  char in_path[PATH_MAX], out_path[PATH_MAX];
  if (!ny_desugar_temp_paths(in_path, sizeof(in_path), out_path,
                             sizeof(out_path))) {
    fprintf(stderr,
            "error: --ny-desugar=ny could not reserve temporary files\n");
    return NULL;
  }
  if (ny_write_file(in_path, source, source_len) != 0) {
    fprintf(stderr, "error: --ny-desugar=ny could not write %s\n", in_path);
    remove(in_path);
    return NULL;
  }

  char in_escaped[512], out_escaped[512];
  if (!ny_desugar_escape_path(in_path, in_escaped, sizeof(in_escaped)) ||
      !ny_desugar_escape_path(out_path, out_escaped, sizeof(out_escaped))) {
    fprintf(stderr, "error: --ny-desugar=ny temp path too long\n");
    remove(in_path);
    return NULL;
  }
  char program[2048];
  snprintf(program, sizeof(program),
           "use core.os (file_read, file_write)\n"
           "use core.compiler.desugar (desugar_file)\n"
           "match desugar_file(\"%s\", \"%s\") {\n"
           "   ok(_) -> {}\n"
           "   err(reason) -> { print(\"ny-desugar: \" + reason)\n"
           "                    exit(2) }\n"
           "}\n",
           in_escaped, out_escaped);
  const char *argv[] = {exe,    "--threads=1", "--parallel=off",
                        "-std", "minimal",     "--comptime-limit=1000000",
                        "-e",   program,       NULL};
  static const char *const std_env_names[] = {
      "NYTRIX_STD_PREBUILT",        "NYTRIX_BUILD_STD_PATH",
      "NYTRIX_CORE_PATH",           "NYTRIX_STD_PATH",
      "NYTRIX_STD_PREFER_PREBUILT", "NYTRIX_STD_BC_CACHE"};
  char *std_env_values[sizeof(std_env_names) / sizeof(std_env_names[0])] = {0};
  bool std_env_was_set[sizeof(std_env_names) / sizeof(std_env_names[0])] = {0};
  bool std_env_changed[sizeof(std_env_names) / sizeof(std_env_names[0])] = {0};
  bool std_env_ok = true;
  /*
   * Test and build launchers can point these variables at a full prebuilt
   * stdlib. The desugar worker explicitly requests minimal std; don't let the
   * inherited bundle override that request or exhaust its bounded memory.
   */
  for (size_t i = 0; i < sizeof(std_env_names) / sizeof(std_env_names[0]);
       ++i) {
    const char *value = getenv(std_env_names[i]);
    if (value) {
      std_env_values[i] = ny_strdup(value);
      if (!std_env_values[i]) {
        std_env_ok = false;
        break;
      }
      std_env_was_set[i] = true;
    }
    if (ny_setenv(std_env_names[i], "", 1) != 0) {
      std_env_ok = false;
      break;
    }
    std_env_changed[i] = true;
  }
  ny_safe_run_t limits = {0};
  limits.cpu_seconds = NY_DESUGAR_CPU_SECONDS;
  limits.wall_seconds = NY_DESUGAR_WALL_SECONDS;
  limits.max_rss_bytes = NY_DESUGAR_MAX_MEMORY_BYTES;
  limits.max_output_bytes = NY_DESUGAR_MAX_OUTPUT_BYTES;
  limits.contain_process_group = true;
  int rc = -1;
  if (std_env_ok) {
    rc = ny_safe_run_spawn(&limits, argv, "Ny compound-assignment desugar");
  } else {
    fprintf(stderr,
            "error: --ny-desugar=ny could not isolate the standard-library "
            "environment\n");
  }
  bool std_env_restore_failed = false;
  for (size_t i = sizeof(std_env_names) / sizeof(std_env_names[0]); i > 0;
       --i) {
    size_t index = i - 1;
    if (std_env_changed[index]) {
      int restore_rc =
          std_env_was_set[index]
              ? ny_setenv(std_env_names[index], std_env_values[index], 1)
              : ny_unsetenv(std_env_names[index]);
      std_env_restore_failed |= restore_rc != 0;
    }
    free(std_env_values[index]);
  }
  if (std_env_restore_failed) {
    fprintf(stderr,
            "error: --ny-desugar=ny could not restore the standard-library "
            "environment\n");
    if (rc == 0)
      rc = -1;
  }

  char *rewritten = NULL;
  if (rc != 0) {
    fprintf(stderr, "error: --ny-desugar=ny pass failed for %s (exit %d)\n",
            display_name ? display_name : "<source>", rc);
  } else {
    rewritten = ny_read_file(out_path);
    if (!rewritten || !*rewritten) {
      fprintf(stderr,
              "error: --ny-desugar=ny produced no rewritten source for %s\n",
              display_name ? display_name : "<source>");
      free(rewritten);
      rewritten = NULL;
    } else if (strlen(rewritten) > NY_DESUGAR_OUTPUT_MAX_BYTES) {
      fprintf(stderr,
              "error: --ny-desugar=ny output exceeds the 1 MiB rewrite "
              "budget for %s\n",
              display_name ? display_name : "<source>");
      free(rewritten);
      rewritten = NULL;
    }
  }

  remove(in_path);
  remove(out_path);
  return rewritten;
}

/*
 * Entry point called right after the user source is loaded.  Returns false
 * when the selected desugar mode failed; the caller must then abort the
 * pipeline.
 */
static bool ny_pipeline_apply_ny_desugar(const ny_options *opt,
                                         char **source_io,
                                         const char *display_name) {
  if (!opt || opt->ny_desugar != NY_DESUGAR_NY)
    return true;
  if (ny_desugar_session_active) {
    fprintf(stderr, "error: --ny-desugar=ny cannot run nested inside another "
                    "--ny-desugar=ny pass\n");
    return false;
  }
  if (!source_io || !*source_io) {
    fprintf(stderr, "error: --ny-desugar=ny has no source to rewrite\n");
    return false;
  }
  size_t source_len = strlen(*source_io);
  if (source_len > NY_DESUGAR_SOURCE_MAX_BYTES) {
    fprintf(stderr,
            "error: --ny-desugar=ny source exceeds the 1 MiB budget (%zu "
            "bytes)\n",
            source_len);
    return false;
  }
  ny_desugar_session_active = true;
  char *rewritten =
      ny_pipeline_desugar_via_ny(*source_io, source_len, display_name);
  ny_desugar_session_active = false;
  if (!rewritten)
    return false;
  free(*source_io);
  *source_io = rewritten;
  return true;
}
