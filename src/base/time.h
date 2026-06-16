#ifndef NY_BASE_TIME_H
#define NY_BASE_TIME_H

#include <stdint.h>
#include <sys/stat.h>

#if defined(__GNUC__) || defined(__clang__)
#include_next <time.h>
#else
#include <time.h>
#endif

uint64_t ny_stat_mtime_nsec(const struct stat *st);

#endif
