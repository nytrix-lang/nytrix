#include "base/time.h"

uint64_t ny_stat_mtime_nsec(const struct stat *st) {
  if (!st)
    return 0;
#if defined(__APPLE__)
  return (uint64_t)st->st_mtimespec.tv_nsec;
#elif !defined(_WIN32)
  return (uint64_t)st->st_mtim.tv_nsec;
#else
  return 0;
#endif
}
