#pragma once

#ifdef _WIN32
#include <BaseTsd.h>
#include <direct.h>
#include <io.h>
#include <process.h>
#include <sys/stat.h>

typedef SSIZE_T ssize_t;

#ifndef STDIN_FILENO
#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
#endif

#ifndef strcasecmp
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#endif

#ifndef strdup
#define strdup _strdup
#endif

#ifndef setlinebuf
#define setlinebuf(stream) setvbuf((stream), NULL, _IONBF, 0)
#endif

#define isatty _isatty
#define fileno _fileno
#define access _access
#define unlink _unlink
#define chdir _chdir
#define getcwd _getcwd
#define read _read
#define write _write
#define fsync _commit
#define getpid _getpid

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#ifndef S_ISDIR
#define S_ISDIR(mode) (((mode) & _S_IFMT) == _S_IFDIR)
#endif
#ifndef S_ISREG
#define S_ISREG(mode) (((mode) & _S_IFMT) == _S_IFREG)
#endif

#ifndef R_OK
#define R_OK 4
#endif
#ifndef W_OK
#define W_OK 2
#endif
#ifndef X_OK
#define X_OK 1
#endif
#ifndef F_OK
#define F_OK 0
#endif
#endif
