#include "base/version.h"

#include <stdio.h>
#include <string.h>

#if defined(__has_include)
#if __has_include("nytrix_version.h")
#include "nytrix_version.h"
#endif
#if __has_include("nytrix_std_id.h")
#include "nytrix_std_id.h"
#endif
#endif

#ifndef NYTRIX_VERSION
#define NYTRIX_VERSION "0.10.0+source"
#endif
#ifndef NYTRIX_VERSION_COMMIT
#define NYTRIX_VERSION_COMMIT "source"
#endif
#ifndef NYTRIX_VERSION_HEAD
#define NYTRIX_VERSION_HEAD "source"
#endif
#ifndef NYTRIX_VERSION_HEAD_DIRTY
#define NYTRIX_VERSION_HEAD_DIRTY 0
#endif
#ifndef NYTRIX_VERSION_BUILD
#define NYTRIX_VERSION_BUILD 0
#endif
#ifndef NYTRIX_STD_SHA256
#define NYTRIX_STD_SHA256 "unknown"
#endif
#ifndef NYTRIX_STD_FILES
#define NYTRIX_STD_FILES 0
#endif

const char *ny_version_id(void) {
  static char id[1024];
  char std_short[13];
  size_t i = 0;
  const char *sha = NYTRIX_STD_SHA256;
  while (i < 12 && sha[i])
    i++;
  memcpy(std_short, sha, i);
  std_short[i] = '\0';
  snprintf(id, sizeof(id), "%s head=%s dirty=%d build=%d std=%s:%d",
           NYTRIX_VERSION, NYTRIX_VERSION_HEAD, NYTRIX_VERSION_HEAD_DIRTY,
           NYTRIX_VERSION_BUILD, std_short, NYTRIX_STD_FILES);
  return id;
}

int ny_print_version_id(void) {
  printf("%s\n", ny_version_id());
  return 0;
}
