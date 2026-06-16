#include "base/curl.h"
#include "base/common.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

bool ny_curl_init(ny_curl_state_t *state) {
  if (!state)
    return false;

  if (state->failed)
    return false;

  if (state->handle)
    return true;

#ifdef _WIN32
  state->handle = LoadLibraryA("libcurl.dll");
  if (!state->handle)
    state->handle = LoadLibraryA("curl.dll");
#else
  state->handle = dlopen("libcurl.so.4", RTLD_LAZY);
  if (!state->handle)
    state->handle = dlopen("libcurl.so", RTLD_LAZY);
#endif

  if (!state->handle) {
    NY_LOG_INFO("libcurl not found, remote operations disabled\n");
    state->failed = true;
    return false;
  }

#ifdef _WIN32
  state->init = (curl_easy_init_t)GetProcAddress(state->handle, "curl_easy_init");
  state->setopt = (curl_easy_setopt_t)GetProcAddress(state->handle, "curl_easy_setopt");
  state->perform = (curl_easy_perform_t)GetProcAddress(state->handle, "curl_easy_perform");
  state->cleanup = (curl_easy_cleanup_t)GetProcAddress(state->handle, "curl_easy_cleanup");
  state->strerror = (curl_easy_strerror_t)GetProcAddress(state->handle, "curl_easy_strerror");
  state->global_init = (curl_global_init_t)GetProcAddress(state->handle, "curl_global_init");
#else
  state->init = (curl_easy_init_t)dlsym(state->handle, "curl_easy_init");
  state->setopt = (curl_easy_setopt_t)dlsym(state->handle, "curl_easy_setopt");
  state->perform = (curl_easy_perform_t)dlsym(state->handle, "curl_easy_perform");
  state->cleanup = (curl_easy_cleanup_t)dlsym(state->handle, "curl_easy_cleanup");
  state->strerror = (curl_easy_strerror_t)dlsym(state->handle, "curl_easy_strerror");
  state->global_init = (curl_global_init_t)dlsym(state->handle, "curl_global_init");
#endif

  if (!state->init || !state->setopt || !state->perform || !state->cleanup) {
    NY_LOG_INFO("libcurl symbols not found, remote operations disabled\n");
    state->failed = true;
    return false;
  }

  if (state->global_init && !state->initialized) {
    state->global_init(NY_CURL_GLOBAL_ALL);
    state->initialized = true;
  }

  return true;
}

void ny_curl_cleanup(ny_curl_state_t *state) {
  if (!state)
    return;

  if (state->handle) {
#ifdef _WIN32
    FreeLibrary((HMODULE)state->handle);
#else
    dlclose(state->handle);
#endif
    state->handle = NULL;
  }
  state->initialized = false;
  state->failed = false;
}

void *ny_curl_easy_init(ny_curl_state_t *state) {
  if (!state || !state->init)
    return NULL;
  return state->init();
}

int ny_curl_easy_perform(ny_curl_state_t *state, void *curl) {
  if (!state || !state->perform || !curl)
    return -1;
  return state->perform(curl);
}
