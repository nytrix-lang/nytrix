/* Declarations and lookup helpers for supported C system headers. */
#ifndef NYC_HEADERS_H
#define NYC_HEADERS_H

#include <stdbool.h>
#include <stddef.h>

/* Bounded libc declarations for the standalone frontend. These are ordinary
 * preprocessor source files, not a second macro-expansion implementation. */
typedef struct {
  const char *name;
  const char *path;
  const char *text;
  size_t length;
  bool sysv_x86_64;
} nyc_header_t;

#define NYC_HEADER(NAME, SYSV, TEXT)                 \
  {NAME, "<" NAME ">", TEXT, sizeof(TEXT) - 1, SYSV}
static const nyc_header_t nyc_headers[] = {
    NYC_HEADER("assert.h", false,
               "#undef assert\n"
               "#ifdef NDEBUG\n#define assert(expr) ((void)0)\n#else\n"
               "#define assert(expr) __nyc_assert_int(!!(expr))\n"
               "#ifndef __NYC_ASSERT_SUPPORT\n#define __NYC_ASSERT_SUPPORT\n"
               "void abort(void);\n"
               "static void __nyc_assert_int(int condition) { if(!condition) "
               "abort(); }\n"
               "#endif\n#endif\n"),
    NYC_HEADER("endian.h", false,
               "#pragma once\n"
               "#define __LITTLE_ENDIAN 1234\n#define __BIG_ENDIAN 4321\n"
               "#define __PDP_ENDIAN 3412\n"
               "#define __BYTE_ORDER __LITTLE_ENDIAN\n"
               "#define LITTLE_ENDIAN __LITTLE_ENDIAN\n"
               "#define BIG_ENDIAN __BIG_ENDIAN\n"
               "#define PDP_ENDIAN __PDP_ENDIAN\n"
               "#define BYTE_ORDER __BYTE_ORDER\n"),
    NYC_HEADER(
        "sys/types.h", true,
        "#pragma once\n#include <endian.h>\n"
        "typedef int pid_t; typedef unsigned int uid_t; typedef unsigned int "
        "gid_t;\n"
        "typedef unsigned int mode_t; typedef unsigned long dev_t;\n"
        "typedef unsigned long ino_t; typedef unsigned long nlink_t;\n"
        "typedef long off_t; typedef long off64_t; typedef long ssize_t;\n"
        "typedef long blksize_t; typedef long blkcnt_t; typedef long "
        "time_t;\n"
        "typedef unsigned int __socklen_t; typedef long __off_t;\n"
        "typedef long __off64_t;\n"),
    NYC_HEADER(
        "time.h", true,
        "#pragma once\n#include <sys/types.h>\n"
        "struct timespec { long tv_sec; long tv_nsec; };\n"
        "struct tm { int tm_sec; int tm_min; int tm_hour; int tm_mday; int "
        "tm_mon;\n"
        "int tm_year; int tm_wday; int tm_yday; int tm_isdst; long tm_gmtoff;\n"
        "const char *tm_zone; };\n"
        "long time(long *);\n"
        "typedef int clockid_t;\n"
        "int clock_gettime(clockid_t, struct timespec *);\n"
        "#ifdef __linux__\n#define CLOCK_REALTIME 0\n"
        "#define CLOCK_MONOTONIC 1\n#define CLOCK_MONOTONIC_RAW 4\n#endif\n"
        "struct tm *localtime_r(const time_t *, struct tm *);\n"
        "struct tm *gmtime_r(const time_t *, struct tm *);\n"
        "int nanosleep(const struct timespec *, struct timespec *);\n"),
    NYC_HEADER("sys/time.h", true,
               "#pragma once\n#include <sys/types.h>\n"
               "struct timeval { long tv_sec; long tv_usec; };\n"
               "int gettimeofday(struct timeval *, void *);\n"
               "int utimes(const char *, const struct timeval *);\n"
               "struct itimerval { struct timeval it_interval;\n"
               "struct timeval it_value; };\n"
               "#define ITIMER_REAL 0\n#define ITIMER_VIRTUAL 1\n"
               "#define ITIMER_PROF 2\n"
               "int getitimer(int, struct itimerval *);\n"
               "int setitimer(int, const struct itimerval *, "
               "struct itimerval *);\n"),
    NYC_HEADER(
        "sys/resource.h", true,
        "#pragma once\n#include <sys/time.h>\n"
        "struct rusage { struct timeval ru_utime; struct timeval ru_stime;\n"
        "long ru_maxrss; long ru_ixrss; long ru_idrss; long ru_isrss;\n"
        "long ru_minflt; long ru_majflt; long ru_nswap; long ru_inblock;\n"
        "long ru_oublock; long ru_msgsnd; long ru_msgrcv; long ru_nsignals;\n"
        "long ru_nvcsw; long ru_nivcsw; };\n"
        "int getrusage(int, struct rusage *);\n"
        "#define RUSAGE_SELF 0\n#define RUSAGE_CHILDREN (-1)\n"
        "#define RUSAGE_THREAD 1\n"),
    NYC_HEADER("pwd.h", true,
               "#pragma once\n#include <sys/types.h>\n"
               "struct passwd { char *pw_name; char *pw_passwd; uid_t pw_uid; "
               "gid_t pw_gid;\n"
               "char *pw_gecos; char *pw_dir; char *pw_shell; };\n"
               "struct passwd *getpwuid(uid_t);\nstruct passwd *getpwnam(const "
               "char *);\n"),
    NYC_HEADER("dirent.h", true,
               "#pragma once\n#include <sys/types.h>\n"
               "typedef struct __dirstream DIR;\n"
               "struct dirent { unsigned long d_ino; long d_off; unsigned "
               "short d_reclen;\n"
               "unsigned char d_type; char d_name[256]; };\n"
               "DIR *opendir(const char *);\nstruct dirent *readdir(DIR "
               "*);\nint closedir(DIR *);\n"),
    NYC_HEADER("utime.h", true,
               "#pragma once\n#include <sys/types.h>\n"
               "struct utimbuf { time_t actime; time_t modtime; };\n"
               "int utime(const char *, const struct utimbuf *);\n"),
    NYC_HEADER("sys/ioctl.h", true,
               "#pragma once\nint ioctl(int, unsigned long, ...);\n"),
    NYC_HEADER(
        "pthread.h", true,
        "#pragma once\n"
        "typedef struct { long __align; char __storage[32]; } "
        "pthread_mutex_t;\n"
        "typedef unsigned long pthread_t; typedef unsigned int "
        "pthread_mutexattr_t;\n"
        "typedef unsigned int pthread_attr_t;\n"
        "int pthread_mutex_init(pthread_mutex_t *, const pthread_mutexattr_t "
        "*);\n"
        "int pthread_mutex_destroy(pthread_mutex_t *);\n"
        "int pthread_mutex_lock(pthread_mutex_t *);\n"
        "int pthread_mutex_unlock(pthread_mutex_t *);\n"
        "int pthread_mutex_trylock(pthread_mutex_t *);\n"
        "int pthread_mutexattr_init(pthread_mutexattr_t *);\n"
        "int pthread_mutexattr_settype(pthread_mutexattr_t *, int);\n"
        "int pthread_mutexattr_destroy(pthread_mutexattr_t *);\n"
        "pthread_t pthread_self(void);\nint pthread_equal(pthread_t, "
        "pthread_t);\n"
        "int pthread_create(pthread_t *, const pthread_attr_t *, void "
        "*(*)(void *), void *);\n"
        "int pthread_join(pthread_t, void **);\n"
        "#define PTHREAD_MUTEX_NORMAL 0\n#define PTHREAD_MUTEX_RECURSIVE 1\n"
        "#define PTHREAD_MUTEX_ERRORCHECK 2\n#define PTHREAD_MUTEX_INITIALIZER "
        "{0}\n"),
    NYC_HEADER(
        "fcntl.h", true,
        "#pragma once\n#include <sys/types.h>\n"
        "struct flock { short l_type; short l_whence; long l_start; long "
        "l_len; int l_pid; };\n"
        "int fcntl(int, int, ...);\nint open(const char *, int, ...);\n"
        "#define F_GETLK 5\n#define F_SETLK 6\n#define F_SETLKW 7\n"
        "#define F_GETFD 1\n#define F_SETFD 2\n#define FD_CLOEXEC 1\n"
        "#define F_RDLCK 0\n#define F_WRLCK 1\n#define F_UNLCK 2\n"
        "#define O_RDONLY 0\n#define O_WRONLY 1\n#define O_RDWR 2\n"
        "#define O_CREAT 64\n#define O_EXCL 128\n#define O_NOCTTY 256\n"
        "#define O_TRUNC 512\n#define O_APPEND 1024\n#define O_NONBLOCK 2048\n"
        "#define O_DSYNC 4096\n#define O_SYNC 1052672\n#define O_CLOEXEC "
        "524288\n"
        "#define O_NOFOLLOW 131072\n#define O_DIRECTORY 65536\n"
        "#define SEEK_SET 0\n#define SEEK_CUR 1\n#define SEEK_END 2\n"),
    NYC_HEADER(
        "unistd.h", true,
        "#pragma once\n#include <sys/types.h>\n#include <fcntl.h>\n"
        "int access(const char *, int);\nint close(int);\nint fdatasync(int);\n"
        "int chdir(const char *);\nint fchdir(int);\n"
        "int rmdir(const char *);\nint getpagesize(void);\n"
        "char *getcwd(char *, size_t);\n"
        "int fsync(int);\nint ftruncate(int, long);\nint getpid(void);\nint getppid(void);\n"
        "int fchown(int, unsigned int, unsigned int);\n"
        "unsigned int getuid(void);\nunsigned int geteuid(void);\n"
        "long lseek(int, long, int);\n"
        "ssize_t pread(int, void *, size_t, long);\n"
        "ssize_t pwrite(int, const void *, size_t, long);\n"
        "ssize_t pread64(int, void *, size_t, long);\n"
        "ssize_t pwrite64(int, const void *, size_t, long);\n"
        "ssize_t read(int, void *, size_t);\nlong sysconf(int);\n"
        "unsigned int sleep(unsigned int);\nint unlink(const char *);\n"
        "int symlink(const char *, const char *);\n"
        "long readlink(const char *, char *, size_t);\n"
        "int usleep(unsigned int);\nssize_t write(int, const void *, size_t);\n"
        "#define R_OK 4\n#define W_OK 2\n#define X_OK 1\n#define F_OK 0\n"
        "#define _SC_PAGESIZE 30\n"),
    NYC_HEADER("stdlib.h", false,
               "#pragma once\n#include <stddef.h>\n"
               "void *malloc(unsigned long);\n"
               "void *calloc(unsigned long, unsigned long);\n"
               "void *realloc(void *, unsigned long);\nvoid free(void *);\n"
               "void exit(int);\nint atexit(void (*)(void));\n"
               "char *getenv(const char *);\nint atoi(const char *);\ndouble atof(const char *);\nint posix_memalign(void **, unsigned long, unsigned long);\n"
               "long strtol(const char *, char **, int);\n"
               "unsigned long strtoul(const char *, char **, int);\n"
               "long long strtoll(const char *, char **, int);\n"
               "unsigned long long strtoull(const char *, char **, int);\n"
               "double strtod(const char *, char **);\n"
               "float strtof(const char *, char **);\n"
               "int system(const char *);\n"
               "void abort(void);\n"
               "void qsort(void *, unsigned long, unsigned long, int (*)(const "
               "void *, const void *));\n"),
    NYC_HEADER(
        "stdio.h", false,
        "#pragma once\n#include <stddef.h>\n"
        "typedef struct _IO_FILE FILE;\n"
        "extern FILE *stdin;\nextern FILE *stdout;\nextern FILE *stderr;\n"
        "FILE *fopen(const char *, const char *);\n"
        "FILE *tmpfile(void);\n"
        "FILE *freopen(const char *, const char *, FILE *);\n"
        "int fclose(FILE *);\nint fflush(FILE *);\n"
        "int printf(const char *, ...);\nint fprintf(FILE *, const char *, "
        "...);\n"
        "int scanf(const char *, ...);\nint fscanf(FILE *, const char *, "
        "...);\n"
        "int sscanf(const char *, const char *, ...);\n"
        "void perror(const char *);\n"
        "int sprintf(char *, const char *, ...);\n"
        "int snprintf(char *, size_t, const char *, ...);\n"
        "int vfprintf(FILE *, const char *, va_list);\n"
        "int vsnprintf(char *, size_t, const char *, va_list);\n"
        "size_t fread(void *, size_t, size_t, FILE *);\n"
        "size_t fwrite(const void *, size_t, size_t, FILE *);\n"
        "char *fgets(char *, int, FILE *);\nint fputs(const char *, FILE *);\n"
        "int puts(const char *);\n"
        "int fputc(int, FILE *);\nint fgetc(FILE *);\n"
        "int fseek(FILE *, long, int);\nlong ftell(FILE *);\n"
        "int feof(FILE *);\nint ferror(FILE *);\nvoid clearerr(FILE *);\n"
        "int fileno(FILE *);\nint remove(const char *);\n"
        "int rename(const char *, const char *);\nvoid rewind(FILE *);\n"
        "int setvbuf(FILE *, char *, int, size_t);\n"
        "#define FILENAME_MAX 4096\n#define EOF (-1)\n"
        "#define SEEK_SET 0\n#define SEEK_CUR 1\n#define SEEK_END 2\n"
        "#define _IOFBF 0\n#define _IOLBF 1\n#define _IONBF 2\n"),
    NYC_HEADER(
        "dlfcn.h", false,
        "#pragma once\nvoid *dlopen(const char *, int);\nint dlclose(void *);\n"
        "char *dlerror(void);\nvoid *dlsym(void *, const char *);\n"
        "#define RTLD_LAZY 1\n#define RTLD_NOW 2\n#define RTLD_LOCAL 0\n"
        "#define RTLD_GLOBAL 256\n"),
    NYC_HEADER(
        "errno.h", true,
        "#pragma once\nint *__errno_location(void);\n"
        "#define errno (*__errno_location())\n"
        "#define EPERM 1\n#define ENOENT 2\n#define ESRCH 3\n#define EINTR 4\n"
        "#define EIO 5\n#define ENXIO 6\n#define E2BIG 7\n#define ENOEXEC 8\n"
        "#define EBADF 9\n#define ECHILD 10\n#define EAGAIN 11\n#define ENOMEM "
        "12\n"
        "#define EACCES 13\n#define EFAULT 14\n#define EBUSY 16\n#define "
        "EEXIST 17\n"
        "#define EXDEV 18\n#define ENODEV 19\n#define ENOTDIR 20\n#define "
        "EISDIR 21\n"
        "#define EINVAL 22\n#define ENFILE 23\n#define EMFILE 24\n#define "
        "ENOTTY 25\n"
        "#define ETXTBSY 26\n#define EFBIG 27\n#define ENOSPC 28\n#define "
        "ESPIPE 29\n"
        "#define EROFS 30\n#define EMLINK 31\n#define EPIPE 32\n#define EDOM "
        "33\n"
        "#define ERANGE 34\n#define EDEADLK 35\n#define ENAMETOOLONG 36\n"
        "#define ENOLCK 37\n#define ENOSYS 38\n#define ENOTEMPTY 39\n#define "
        "ELOOP 40\n"
        "#define EOPNOTSUPP 95\n#define ENOTSUP 95\n#define ETIMEDOUT 110\n"),
    NYC_HEADER(
        "sys/stat.h", true,
        "#pragma once\n#include <time.h>\n"
        "struct stat { long st_dev; long st_ino; long st_nlink; unsigned int "
        "st_mode;\n"
        "unsigned int st_uid; unsigned int st_gid; int __pad0; long st_rdev;\n"
        "long st_size; long st_blksize; long st_blocks; struct timespec "
        "st_atim;\n"
        "struct timespec st_mtim; struct timespec st_ctim; long "
        "__glibc_reserved[3]; };\n"
        "#define st_atime st_atim.tv_sec\n#define st_mtime st_mtim.tv_sec\n"
        "#define st_ctime st_ctim.tv_sec\n"
        "int stat(const char *, struct stat *);\n"
        "int lstat(const char *, struct stat *);\n"
        "int fstat(int, struct stat *);\n"
        "int mkdir(const char *, unsigned int);\n"
        "int chmod(const char *, unsigned int);\n"
        "int fchmod(int, unsigned int);\n"
        "static int __nyc_S_ISREG(unsigned int mode) { return (mode & 0170000) "
        "== 0100000; }\n"
        "static int __nyc_S_ISFIFO(unsigned int mode) { return (mode & "
        "0170000) == 0010000; }\n"
        "static int __nyc_S_ISCHR(unsigned int mode) { return (mode & 0170000) "
        "== 0020000; }\n"
        "static int __nyc_S_ISDIR(unsigned int mode) { return (mode & 0170000) "
        "== 0040000; }\n"
        "static int __nyc_S_ISLNK(unsigned int mode) { return (mode & 0170000) "
        "== 0120000; }\n"
        "#define S_ISREG __nyc_S_ISREG\n#define S_ISDIR __nyc_S_ISDIR\n"
        "#define S_ISFIFO __nyc_S_ISFIFO\n#define S_ISCHR __nyc_S_ISCHR\n"
        "#define S_ISLNK __nyc_S_ISLNK\n#define S_IFMT 0170000\n"
        "#define S_IFREG 0100000\n#define S_IFDIR 0040000\n#define S_IFLNK "
        "0120000\n"
        "#define S_IRUSR 0400\n#define S_IWUSR 0200\n#define S_IXUSR 0100\n"
        "#define S_IRGRP 0040\n#define S_IWGRP 0020\n#define S_IROTH 0004\n"
        "#define S_IWOTH 0002\n"),
    NYC_HEADER(
        "sys/mman.h", true,
        "#pragma once\nvoid *mmap(void *, size_t, int, int, int, long);\n"
        "void *mremap(void *, size_t, size_t, int, ...);\n"
        "int munmap(void *, size_t);\n#define PROT_READ 1\n#define PROT_WRITE "
        "2\n"
        "#define PROT_EXEC 4\n#define PROT_NONE 0\n#define MAP_SHARED 1\n"
        "#define MAP_PRIVATE 2\n#define MAP_FAILED ((void *)-1)\n"
        "#define MREMAP_MAYMOVE 1\n"),
    /* These headers expose the common declarations needed by hosted C tools. */
    NYC_HEADER("ctype.h", false,
               "#pragma once\nint isalnum(int);\nint isalpha(int);\nint "
               "isblank(int);\n"
               "int iscntrl(int);\nint isdigit(int);\nint isgraph(int);\n"
               "int islower(int);\nint isprint(int);\nint ispunct(int);\n"
               "int isspace(int);\nint isupper(int);\nint isxdigit(int);\n"
               "int tolower(int);\nint toupper(int);\n"),
    NYC_HEADER("float.h", false, "#pragma once\n"),
    NYC_HEADER("inttypes.h", false, "#pragma once\n"),
    NYC_HEADER("limits.h", false,
               "#pragma once\n#define CHAR_BIT 8\n#define SCHAR_MIN (-128)\n"
               "#define SCHAR_MAX 127\n#define UCHAR_MAX 255\n"
               "#define SHRT_MIN (-32768)\n#define SHRT_MAX 32767\n"
               "#define USHRT_MAX 65535\n#define INT_MIN (-2147483647 - 1)\n"
               "#define INT_MAX 2147483647\n#define UINT_MAX 4294967295U\n"
               "#define LONG_MIN (-9223372036854775807L - 1L)\n"
               "#define LONG_MAX 9223372036854775807L\n"
               "#define ULONG_MAX 18446744073709551615UL\n"
               "#define LLONG_MIN (-9223372036854775807LL - 1LL)\n"
               "#define LLONG_MAX 9223372036854775807LL\n"
               "#define ULLONG_MAX 18446744073709551615ULL\n"),
    NYC_HEADER("math.h", false,
               "#pragma once\nint isnan(double);\nint isinf(double);\n"
               "double acos(double);\ndouble asin(double);\n"
               "double atan(double);\ndouble atan2(double, double);\n"
               "double acosh(double);\ndouble asinh(double);\n"
               "double atanh(double);\n"
               "double ceil(double);\ndouble cos(double);\n"
               "double cosh(double);\ndouble exp(double);\n"
               "double fabs(double);\ndouble floor(double);\n"
               "double fmod(double, double);\ndouble log(double);\n"
               "double log10(double);\ndouble log2(double);\n"
               "double pow(double, double);\ndouble sin(double);\n"
               "double sinh(double);\ndouble sqrt(double);\n"
               "double tan(double);\ndouble tanh(double);\n"
               "double trunc(double);\n"),
    NYC_HEADER(
        "signal.h", false,
        "#pragma once\n#define SIGHUP 1\n#define SIGINT 2\n#define SIGQUIT 3\n"
        "#define SIGILL 4\n#define SIGTRAP 5\n#define SIGABRT 6\n"
        "#define SIGFPE 8\n#define SIGKILL 9\n#define SIGUSR1 10\n"
        "#define SIGSEGV 11\n#define SIGUSR2 12\n#define SIGPIPE 13\n"
        "#define SIGALRM 14\n#define SIGTERM 15\n#define SIGCHLD 17\n"
        "typedef int sig_atomic_t;\ntypedef void (*sighandler_t)(int);\n"
        "#define SIG_DFL ((sighandler_t)0)\n#define SIG_IGN ((sighandler_t)1)\n"
        "#define SIG_ERR ((sighandler_t)-1)\n"
        "sighandler_t signal(int, sighandler_t);\nint raise(int);\n"
        "int kill(int, int);\n"),
    NYC_HEADER("stdarg.h", false, "#pragma once\n"),
    NYC_HEADER("stdbool.h", false,
               "#pragma once\n#define bool _Bool\n#define true 1\n"
               "#define false 0\n#define __bool_true_false_are_defined 1\n"),
    NYC_HEADER(
        "stddef.h", false,
        "#pragma once\n#ifndef NULL\n#define NULL ((void *)0)\n"
        "#endif\n"
        "#define offsetof(TYPE, MEMBER) __builtin_offsetof(TYPE, MEMBER)\n"),
    NYC_HEADER("stdint.h", false,
               "#pragma once\n#include <stddef.h>\n"
               "typedef signed char int8_t; typedef unsigned char uint8_t;\n"
               "typedef short int16_t; typedef unsigned short uint16_t;\n"
               "typedef int int32_t; typedef unsigned int uint32_t;\n"
               "typedef long long int64_t; typedef unsigned long long "
               "uint64_t;\n"
               "#define INT64_MAX 9223372036854775807LL\n"
               "#define INT64_MIN (-9223372036854775807LL - 1LL)\n"
               "#define UINT64_MAX 18446744073709551615ULL\n"
               "#define SIZE_MAX ((size_t)-1)\n"
               "#define INT64_C(value) value##LL\n"
               "#define UINT64_C(value) value##ULL\n"),
    NYC_HEADER(
        "string.h", false,
        "#pragma once\nvoid *memcpy(void *, const void *, size_t);\n"
        "void *memmove(void *, const void *, size_t);\n"
        "void *memset(void *, int, size_t);\n"
        "void *memchr(const void *, int, size_t);\n"
        "int memcmp(const void *, const void *, size_t);\n"
        "size_t strlen(const char *);\nchar *strstr(const char *, const char "
        "*);\n"
        "char *strchrnul(const char *, int);\n"
        "char *strcpy(char *, const char *);\n"
        "char *strncpy(char *, const char *, size_t);\n"
        "char *strcat(char *, const char *);\n"
        "char *strncat(char *, const char *, size_t);\n"
        "int strcmp(const char *, const char *);\n"
        "int strncmp(const char *, const char *, size_t);\n"
        "char *strchr(const char *, int);\n"
        "char *strrchr(const char *, int);\n"
        "size_t strspn(const char *, const char *);\n"
        "size_t strcspn(const char *, const char *);\n"
        "char *strpbrk(const char *, const char *);\n"
        "char *strtok(char *, const char *);\n"
        "char *strerror(int);\n"
        "char *strdup(const char *);\nchar *strndup(const char *, size_t);\n"),
    NYC_HEADER("strings.h", false, "#pragma once\n"),
    NYC_HEADER("memory.h", false, "#pragma once\n#include <string.h>\n"),
};
#undef NYC_HEADER

#endif
