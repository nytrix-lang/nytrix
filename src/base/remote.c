/*
 * Remote module downloads and atomic cache publication.
 */
#include "base/remote.h"
#include "base/common.h"
#include "base/util.h"
#include "base/debug.h"
#include "base/curl.h"
#include "base/sha256.h"
#ifdef _WIN32
#include "base/compat.h"
#include <io.h>
#include <fcntl.h>
#include <stdlib.h>
#else
#include <unistd.h>
#endif
#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

char *ny_resolve_remote_module(const char *url) {
  static ny_curl_state_t curl_state = {0};

  if (!url)
    return NULL;
  char download_url[4096];
  char expected[72] = {0};
  const char *fragment = strstr(url, "#sha256=");
  size_t url_len = fragment ? (size_t)(fragment - url) : strlen(url);
  if (url_len == 0 || url_len >= sizeof(download_url))
    return NULL;
  memcpy(download_url, url, url_len);
  download_url[url_len] = '\0';
  if (fragment) {
    const char *hex = fragment + 8;
    if (strlen(hex) != 64)
      return NULL;
    memcpy(expected, "sha256:", 7);
    for (size_t i = 0; i < 64; ++i) {
      if (!isxdigit((unsigned char)hex[i]))
        return NULL;
      expected[i + 7] = (char)tolower((unsigned char)hex[i]);
    }
    expected[71] = '\0';
  }

  if (!ny_curl_init(&curl_state))
    return NULL;

  uint64_t h = ny_hash64_cstr(url);
  char cache_dir[1024];
  const char *root = ny_default_cache_root_dir();
  int n = snprintf(cache_dir, sizeof(cache_dir), "%s/remote",
                   root && *root ? root : ny_get_temp_dir());
  if (n <= 0 || (size_t)n >= sizeof(cache_dir))
    return NULL;
  ny_ensure_dir_recursive(cache_dir);

  char cache_path[1024];
  n = snprintf(cache_path, sizeof(cache_path), "%s/%016llx.ny", cache_dir,
               (unsigned long long)h);
  if (n <= 0 || (size_t)n >= sizeof(cache_path))
    return NULL;

  struct stat st;
  bool reload = ny_env_enabled("NYTRIX_REMOTE_RELOAD");
  if (!reload && stat(cache_path, &st) == 0 && st.st_size > 0) {
    char actual[72];
    if (!expected[0] ||
        (ny_sha256_file(cache_path, actual, sizeof(actual)) &&
         strcmp(actual, expected) == 0))
      return ny_strdup(cache_path);
  }

  NY_LOG_INFO("Downloading remote module: %s\n", url);
  void *curl = ny_curl_easy_init(&curl_state);
  if (!curl)
    return NULL;

  char temporary[1100];
  n = snprintf(temporary, sizeof(temporary), "%s.XXXXXX", cache_path);
  if (n <= 0 || (size_t)n >= sizeof(temporary)) {
    curl_state.cleanup(curl);
    return NULL;
  }
#ifdef _WIN32
  if (_mktemp_s(temporary, sizeof(temporary)) != 0) {
    curl_state.cleanup(curl);
    return NULL;
  }
  int fd = _open(temporary, _O_CREAT | _O_EXCL | _O_BINARY | _O_WRONLY,
                 _S_IREAD | _S_IWRITE);
  FILE *fp = fd >= 0 ? _fdopen(fd, "wb") : NULL;
#else
  int fd = mkstemp(temporary);
  FILE *fp = fd >= 0 ? fdopen(fd, "wb") : NULL;
#endif
  if (!fp) {
    if (fd >= 0) {
#ifdef _WIN32
      _close(fd);
#else
      close(fd);
#endif
      unlink(temporary);
    }
    curl_state.cleanup(curl);
    return NULL;
  }

  bool options_ok =
      curl_state.setopt(curl, NY_CURLOPT_URL, download_url) == 0 &&
      curl_state.setopt(curl, NY_CURLOPT_WRITEDATA, fp) == 0 &&
      curl_state.setopt(curl, NY_CURLOPT_FOLLOWLOCATION, 1L) == 0 &&
      curl_state.setopt(curl, NY_CURLOPT_USERAGENT,
                        "nytrix-compiler/1.0") == 0 &&
      curl_state.setopt(curl, NY_CURLOPT_FAILONERROR, 1L) == 0 &&
      curl_state.setopt(curl, NY_CURLOPT_TIMEOUT, 60L) == 0;
  if (!options_ok) {
    fclose(fp);
    curl_state.cleanup(curl);
    unlink(temporary);
    return NULL;
  }

  int res_code = ny_curl_easy_perform(&curl_state, curl);
  bool ok = fclose(fp) == 0;
  curl_state.cleanup(curl);

  if (res_code != 0 || !ok || stat(temporary, &st) != 0 || st.st_size == 0) {
    fprintf(stderr, "Error: Failed to download remote module: %s (%s)\n", url,
            res_code != 0 && curl_state.strerror ? curl_state.strerror(res_code)
                                        : "empty or incomplete response");
    unlink(temporary);
    return NULL;
  }
  if (expected[0]) {
    char actual[72];
    if (!ny_sha256_file(temporary, actual, sizeof(actual)) ||
        strcmp(actual, expected) != 0) {
      fprintf(stderr, "Error: Remote module SHA-256 mismatch: %s\n", url);
      unlink(temporary);
      return NULL;
    }
  }

#ifdef _WIN32
  if (!MoveFileExA(temporary, cache_path, MOVEFILE_REPLACE_EXISTING)) {
#else
  if (rename(temporary, cache_path) != 0) {
#endif
    unlink(temporary);
    return NULL;
  }

  return ny_strdup(cache_path);
}

