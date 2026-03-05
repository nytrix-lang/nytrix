#ifndef NY_CURL_UTILS_H
#define NY_CURL_UTILS_H

#include <stdbool.h>

typedef void *(*curl_easy_init_t)(void);
typedef int (*curl_easy_setopt_t)(void *, int, ...);
typedef int (*curl_easy_perform_t)(void *);
typedef void (*curl_easy_cleanup_t)(void *);
typedef const char *(*curl_easy_strerror_t)(int);
typedef int (*curl_global_init_t)(long);

#define NY_CURL_GLOBAL_ALL 3L
#define NY_CURLOPT_URL 10002
#define NY_CURLOPT_WRITEDATA 10001
#define NY_CURLOPT_WRITEFUNCTION 20011
#define NY_CURLOPT_FOLLOWLOCATION 52
#define NY_CURLOPT_USERAGENT 10018
#define NY_CURLOPT_FAILONERROR 10194
#define NY_CURLOPT_TIMEOUT 10013

typedef struct {
  void *handle;
  curl_easy_init_t init;
  curl_easy_setopt_t setopt;
  curl_easy_perform_t perform;
  curl_easy_cleanup_t cleanup;
  curl_easy_strerror_t strerror;
  curl_global_init_t global_init;
  bool initialized;
  bool failed;
} ny_curl_state_t;

bool ny_curl_init(ny_curl_state_t *state);
void ny_curl_cleanup(ny_curl_state_t *state);
void *ny_curl_easy_init(ny_curl_state_t *state);
int ny_curl_easy_perform(ny_curl_state_t *state, void *curl);

#endif
