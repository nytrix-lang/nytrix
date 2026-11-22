/*
 * POSIX terminal UI replay runner used by command-line integration tests.
 */
#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L
#include "ui.h"

#ifdef _WIN32
#include <stdio.h>
int ny_test_ui_replay_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  fprintf(stderr, "ui-replay is currently supported on POSIX hosts\n");
  return 2;
}
#else
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static const char *apps[] = {"animation", "editor", "engine", "input",
                             "monitor",   "pong",   "synth",  "term"};
static const char *routes[] = {"native", "jit", "vm"};
static char root[PATH_MAX];
static int failures;

static int has_prefix(const char *s, const char *p) {
  return strncmp(s, p, strlen(p)) == 0;
}

static int mkdirs(const char *path) {
  char copy[PATH_MAX];
  size_t n = strlen(path);
  if (!n || n >= sizeof(copy))
    return -1;
  memcpy(copy, path, n + 1);
  for (char *p = copy + 1; *p; ++p) {
    if (*p == '/') {
      *p = 0;
      if (mkdir(copy, 0777) && errno != EEXIST)
        return -1;
      *p = '/';
    }
  }
  return mkdir(copy, 0777) && errno != EEXIST ? -1 : 0;
}

static int selected(const char *csv, const char *value) {
  size_t n = strlen(value);
  const char *p = csv;
  while (*p) {
    while (*p == ',' || *p == ' ')
      ++p;
    const char *end = p;
    while (*end && *end != ',')
      ++end;
    if ((size_t)(end - p) == n && !memcmp(p, value, n))
      return 1;
    p = end;
  }
  return 0;
}

static void setenv_num(const char *name, int value) {
  char text[32];
  snprintf(text, sizeof(text), "%d", value);
  setenv(name, text, 1);
}

static void replay_env(const char *app, int width, int height) {
  extern char **environ;
  for (;;) {
    char **p = environ;
    int removed = 0;
    for (; p && *p; ++p) {
      char *eq = strchr(*p, '=');
      if (!eq)
        continue;
      size_t n = (size_t)(eq - *p);
      if ((n >= 6 && !memcmp(*p, "NY_UI_", 6)) ||
          (n >= 10 && !memcmp(*p, "NY_EDITOR_", 10)) ||
          (n >= 8 && !memcmp(*p, "NY_TERM_", 8)) ||
          (n >= 10 && !memcmp(*p, "NYTRIX_UI_", 10)) ||
          (n >= 15 && !memcmp(*p, "NYTRIX_FIXTURE_", 15)) ||
          (n >= 8 && !memcmp(*p, "NY_TRACE", 8))) {
        if (n < 256) {
          char name[256];
          memcpy(name, *p, n);
          name[n] = 0;
          unsetenv(name);
          removed = 1;
          break;
        }
      }
    }
    if (!removed)
      break;
  }
  setenv("NYTRIX_UI_REPLAY", "1", 1);
  setenv("NY_UI_RENDER_BACKEND", "mock", 1);
  setenv("NY_UI_MOCK_TEXT_FAST", strcmp(app, "editor") ? "bars" : "off", 1);
  setenv("NYTRIX_FIXTURE_TIMEOUT_MS", "0", 1);
  setenv("NYTRIX_FIXTURE_FRAME_MS", "100", 1);
  /*
   * Replay compares route markers, not texture fidelity. The default 256-wide
   * generated environment is filled per pixel in the VM route and dominates
   * the engine replay.
   */
  setenv("NY_UI_GENERATED_ENV_W", "64", 1);
  setenv_num("NYTRIX_FIXTURE_FRAMES", 5);
  setenv_num("NYTRIX_FIXTURE_WIDTH", width);
  setenv_num("NYTRIX_FIXTURE_HEIGHT", height);
  char events[160];
  snprintf(events, sizeof(events),
           "2,resize,%d,%d;3,key_down,340;4,key_up,340;5,close", width + 40,
           height + 40);
  setenv("NYTRIX_FIXTURE_EVENTS", events, 1);
  setenv("NY_UI_FRAME_HASH_LOCK", "1", 1);
  setenv("NY_TERM_CURSOR_BLINK", "0", 1);
  setenv("NY_TERM_STAY_OPEN_ON_EXIT", "1", 1);
  setenv("NY_AUDIO_BACKEND", "none", 1);
  if (!strcmp(app, "engine")) {
    char path[PATH_MAX];
    snprintf(
        path, sizeof(path),
        "%s/etc/tests/code/probes/ui/fixtures/ui-triangle/ui-triangle.gltf",
        root);
    setenv("NY_UI_BOOT_SCENE", path, 1);
    setenv("NY_UI_POST_LOAD_CMD", "select; translate 0.25 0.0 0.0", 1);
    setenv("NY_UI_DISABLE_ENV", "1", 1);
    setenv("NY_UI_SKYBOX_SOURCE", "generated", 1);
    setenv("NY_UI_SELECTION_OVERLAY_PROBE", "1", 1);
  } else if (!strcmp(app, "editor")) {
    static const char seed[] =
        "Nytrix editor replay fixture\nA fixed document for deterministic UI "
        "output.\nThe editor keeps this text visible during replay.\n";
    mkdirs("/tmp/nytrix-ui-editor-replay");
    FILE *f = fopen("/tmp/nytrix-ui-editor-replay/README.md", "rb");
    int rewrite = 1;
    if (f) {
      char existing[sizeof(seed)];
      size_t n = fread(existing, 1, sizeof(existing), f);
      rewrite =
          n != sizeof(seed) - 1 || memcmp(existing, seed, sizeof(seed) - 1);
      fclose(f);
    }
    if (rewrite &&
        (f = fopen("/tmp/nytrix-ui-editor-replay/README.md", "wb")) != NULL) {
      fwrite(seed, 1, sizeof(seed) - 1, f);
      fclose(f);
    }
    setenv("NY_EDITOR_LSP", "off", 1);
    setenv("NY_EDITOR_PROJECT_ROOT", "/tmp/nytrix-ui-editor-replay", 1);
    setenv("NY_EDITOR_FRAME_HASH_FRAME", "1000000", 1);
  } else if (!strcmp(app, "term")) {
    setenv("NY_UI_HEADLESS", "1", 1);
    setenv("NY_UI_BACKEND", "none", 1);
  }
}

static pid_t child_start(const char *compiler, const char *app,
                         const char *route, const char *log_path) {
  char project[PATH_MAX];
  snprintf(project, sizeof(project), "%s/etc/labs/%s.ny", root, app);
  int width = (!strcmp(app, "engine") || !strcmp(app, "synth")) ? 32
              : !strcmp(app, "input")                           ? 64
              : !strcmp(app, "term")                            ? 128
              : (!strcmp(app, "editor") || !strcmp(app, "monitor")) ? 160
                                                                : 320;
  int height = (!strcmp(app, "engine") || !strcmp(app, "synth")) ? 24
               : !strcmp(app, "input")                           ? 48
               : !strcmp(app, "term")                            ? 96
               : (!strcmp(app, "editor") || !strcmp(app, "monitor")) ? 120
                                                                 : 240;
  replay_env(app, width, height);
  char *args[16];
  int n = 0;
  args[n++] = (char *)compiler;
  if (!strcmp(route, "native")) {
    args[n++] = "--native-only";
    args[n++] = "--native-tier=baseline";
    args[n++] = "-O0";
  } else if (!strcmp(route, "jit")) {
    args[n++] = "--native-backend=llvm";
    args[n++] = "--jit";
    args[n++] = "--jit-engine=mcjit";
    args[n++] = "-O0";
  } else {
    args[n++] = "--nyr-run";
    args[n++] = "--nyr-run-max-steps=50000000";
    args[n++] = "-O0";
  }
  /*
   * Nested replay compilers must use the same frozen core as their suite.
   */
  const char *core_bc = getenv("NYTRIX_TEST_CORE_BC");
  if (core_bc && *core_bc) {
    args[n++] = "--core-bc";
    args[n++] = (char *)core_bc;
  }
  args[n++] = "--parallel=off";
  args[n++] = "--no-progress";
  args[n++] = project;
  args[n] = NULL;
#ifdef __linux__
  const pid_t parent = getpid();
#endif
  pid_t pid = fork();
  if (pid < 0)
    return -1;
  if (!pid) {
#ifdef __linux__
    /*
     * A replay route has its own session, so it must follow the helper's
     * lifetime even when the outer fixture is killed.
     */
    if (prctl(PR_SET_PDEATHSIG, SIGKILL) < 0 || getppid() != parent)
      _exit(126);
#endif
    setsid();
    int fd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
      _exit(126);
    dup2(fd, STDOUT_FILENO);
    dup2(fd, STDERR_FILENO);
    close(fd);
    chdir(root);
    execv(compiler, args);
    _exit(127);
  }
  return pid;
}

typedef struct {
  pid_t pid;
  const char *route;
  char log[PATH_MAX];
  struct timespec started;
  int timeout;
  int code;
} replay_child_t;

static double elapsed_since(struct timespec started) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (double)(now.tv_sec - started.tv_sec) +
         (double)(now.tv_nsec - started.tv_nsec) / 1e9;
}

static void child_stop(pid_t pid) {
  if (pid <= 0)
    return;
  kill(-pid, SIGKILL);
  /*
   * Also cover a forked child which has not reached setsid yet.
   */
  kill(pid, SIGKILL);
  while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {
  }
}

static int children_wait(const char *app, replay_child_t *children,
                         size_t count) {
  size_t pending = count;
  int ok = 1;
  while (pending) {
    for (size_t i = 0; i < count; ++i) {
      replay_child_t *child = &children[i];
      if (child->pid <= 0)
        continue;
      int status = 0;
      pid_t got = waitpid(child->pid, &status, WNOHANG);
      double elapsed = elapsed_since(child->started);
      if (got == child->pid) {
        child->code = WIFEXITED(status)     ? WEXITSTATUS(status)
                      : WIFSIGNALED(status) ? 128 + WTERMSIG(status)
                                            : 1;
      } else if (got < 0 && errno != EINTR) {
        child_stop(child->pid);
        child->code = 1;
      } else if (elapsed >= child->timeout) {
        child_stop(child->pid);
        child->code = 124;
      } else
        continue;
      child->pid = 0;
      --pending;
      printf("%s/%s: completed in %.3fs (exit %d)\n", app, child->route,
             elapsed, child->code);
      fflush(stdout);
      if (child->code != 0) {
        fprintf(stderr, "ui-replay: %s/%s exited %d (log %s)\n", app,
                child->route, child->code, child->log);
        ok = 0;
      }
    }
    if (!ok) {
      for (size_t i = 0; i < count; ++i)
        child_stop(children[i].pid);
      return 0;
    }
    if (pending) {
      struct timespec pause = {0, 50000000};
      nanosleep(&pause, NULL);
    }
  }
  return 1;
}

static char *read_file(const char *path, size_t *length) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;
  if (fseek(f, 0, SEEK_END)) {
    fclose(f);
    return NULL;
  }
  long n = ftell(f);
  if (n < 0 || fseek(f, 0, SEEK_SET)) {
    fclose(f);
    return NULL;
  }
  char *s = malloc((size_t)n + 1);
  if (!s) {
    fclose(f);
    return NULL;
  }
  size_t got = fread(s, 1, (size_t)n, f);
  fclose(f);
  s[got] = 0;
  if (length)
    *length = got;
  return s;
}

static int validate_log(const char *app, const char *route, const char *path,
                        char **markers, size_t *marker_len) {
  size_t length = 0;
  char *text = read_file(path, &length);
  if (!text)
    return 0;
  char *copy = strdup(text), *save = NULL, *line = strtok_r(copy, "\n", &save);
  int frames = 0, states = 0, events = 0, engine_states = 0, engine_actions = 0;
  int animation_states = 0;
  int width = (!strcmp(app, "engine") || !strcmp(app, "synth")) ? 32
              : !strcmp(app, "input")                           ? 64
              : !strcmp(app, "term")                            ? 128
              : (!strcmp(app, "editor") || !strcmp(app, "monitor")) ? 160
                                                                : 320;
  int height = (!strcmp(app, "engine") || !strcmp(app, "synth")) ? 24
               : !strcmp(app, "input")                           ? 48
               : !strcmp(app, "term")                            ? 96
               : (!strcmp(app, "editor") || !strcmp(app, "monitor")) ? 120
                                                                 : 240;
  size_t used = 0, cap = 0;
  while (line) {
    if (has_prefix(line, "NYTRIX_UI_") ||
        has_prefix(line, "NYTRIX_ENGINE_STATE") ||
        has_prefix(line, "NYTRIX_ENGINE_ACTION")) {
      size_t n = strlen(line);
      if (used + n + 2 > cap) {
        cap = (used + n + 2) * 2;
        *markers = realloc(*markers, cap);
      }
      memcpy(*markers + used, line, n);
      used += n;
      (*markers)[used++] = '\n';
      (*markers)[used] = 0;
      if (has_prefix(line, "NYTRIX_UI_FRAMEHASH frame=")) {
        char expect[48];
        snprintf(expect, sizeof(expect),
                 "NYTRIX_UI_FRAMEHASH frame=%d FB_HASH: ", frames + 1);
        if (!has_prefix(line, expect))
          failures++;
        frames++;
      } else if (has_prefix(line, "NYTRIX_UI_STATE frame=")) {
        char expect[96];
        int frame = states + 1;
        snprintf(expect, sizeof(expect), "NYTRIX_UI_STATE frame=%d size=%dx%d ",
                 frame, frame == 1 ? width : width + 40,
                 frame == 1 ? height : height + 40);
        if (!has_prefix(line, expect))
          failures++;
        states++;
      } else if (has_prefix(line, "NYTRIX_UI_EVENT ")) {
        static const char *expected[] = {
            "NYTRIX_UI_EVENT frame=2 type=resize",
            "NYTRIX_UI_EVENT frame=3 type=key_down",
            "NYTRIX_UI_EVENT frame=4 type=key_up",
            "NYTRIX_UI_EVENT frame=5 type=close"};
        if (events >= 4 || strcmp(line, expected[events]))
          failures++;
        events++;
      } else if (has_prefix(line, "NYTRIX_ENGINE_STATE "))
        engine_states++;
      else if (has_prefix(line, "NYTRIX_ENGINE_ACTION "))
        engine_actions++;
      if (!strcmp(app, "animation") &&
          has_prefix(line, "NYTRIX_UI_APP_STATE demo=animation ")) {
        char expected[32];
        snprintf(expected, sizeof(expected), "timeline=%d",
                 (animation_states + 1) * 100);
        if (!strstr(line, expected))
          failures++;
        animation_states++;
      }
    }
    line = strtok_r(NULL, "\n", &save);
  }
  free(copy);
  free(text);
  *marker_len = used;
  if (frames != 5 || states != 5 || events != 4) {
    fprintf(stderr,
            "ui-replay: %s/%s marker counts frames=%d states=%d events=%d (log "
            "%s)\n",
            app, route, frames, states, events, path);
    return 0;
  }
  if (!strcmp(app, "engine") && (engine_states != 4 || engine_actions != 3)) {
    fprintf(stderr,
            "ui-replay: %s/%s engine markers states=%d actions=%d (log %s)\n",
            app, route, engine_states, engine_actions, path);
    return 0;
  }
  if (failures)
    fprintf(stderr,
            "ui-replay: %s/%s event, frame-size, or animation-clock mismatch "
            "(log %s)\n",
            app, route, path);
  return 1;
}

int ny_test_ui_replay_main(int argc, char **argv) {
  const char *compiler = NULL, *app_filter = NULL,
             *route_filter = "native,jit,vm";
  int timeout = 300;
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--compiler") && i + 1 < argc)
      compiler = argv[++i];
    else if (!strcmp(argv[i], "--apps") && i + 1 < argc)
      app_filter = argv[++i];
    else if (!strcmp(argv[i], "--routes") && i + 1 < argc)
      route_filter = argv[++i];
    else if (!strcmp(argv[i], "--timeout") && i + 1 < argc)
      timeout = atoi(argv[++i]);
    else {
      fprintf(stderr, "usage: ny-test ui-replay --compiler PATH [--apps CSV] "
                      "[--routes CSV] [--timeout SEC]\n");
      return 2;
    }
  }
  char resolved_compiler[PATH_MAX];
  if (!compiler || timeout < 1 || !getcwd(root, sizeof(root)) ||
      !realpath(compiler, resolved_compiler)) {
    fprintf(stderr,
            "ui-replay: missing compiler or invalid working directory\n");
    return 2;
  }
  compiler = resolved_compiler;
  char output[PATH_MAX];
  const char *output_dir = getenv("NYTRIX_TEST_UI_REPLAY_LOG_DIR");
  int output_len = output_dir && *output_dir
                       ? snprintf(output, sizeof(output), "%s", output_dir)
                       : snprintf(output, sizeof(output),
                                  "%s/build/diagnostics/ui-replay", root);
  if (output_len < 0 || (size_t)output_len >= sizeof(output))
    return 2;
  if (mkdirs(output)) {
    perror(output);
    return 2;
  }
  for (size_t ai = 0; ai < sizeof(apps) / sizeof(apps[0]); ++ai) {
    const char *app = apps[ai];
    if (app_filter && !selected(app_filter, app))
      continue;
    char *reference = NULL;
    size_t reference_len = 0;
    replay_child_t children[sizeof(routes) / sizeof(routes[0])] = {0};
    size_t child_count = 0;
    /*
     * Each route has its own process and log. Compile them concurrently so
     * the enclosing fixture pays the longest route, rather than their sum.
     */
    for (size_t ri = 0; ri < sizeof(routes) / sizeof(routes[0]); ++ri) {
      const char *route = routes[ri];
      if (!selected(route_filter, route))
        continue;
      replay_child_t *child = &children[child_count];
      child->route = route;
      snprintf(child->log, sizeof(child->log), "%s/%s-%s.log", output, app,
               route);
      child->timeout =
          (!strcmp(app, "engine") && !strcmp(route, "vm") && timeout < 400)
              ? 400
              : timeout;
      clock_gettime(CLOCK_MONOTONIC, &child->started);
      child->pid = child_start(compiler, app, route, child->log);
      if (child->pid < 0) {
        perror("ui-replay: fork");
        for (size_t i = 0; i < child_count; ++i)
          child_stop(children[i].pid);
        return 1;
      }
      ++child_count;
    }
    if (!children_wait(app, children, child_count))
      return 1;
    for (size_t ri = 0; ri < child_count; ++ri) {
      const char *route = children[ri].route;
      const char *log = children[ri].log;
      char *markers = NULL;
      size_t marker_len = 0;
      if (!validate_log(app, route, log, &markers, &marker_len)) {
        free(markers);
        free(reference);
        return 1;
      }
      if (!reference) {
        reference = markers;
        reference_len = marker_len;
      } else if (reference_len != marker_len ||
                 memcmp(reference, markers, marker_len)) {
        fprintf(stderr,
                "ui-replay: %s marker stream differs on %s route (log %s)\n",
                app, route, log);
        free(reference);
        free(markers);
        return 1;
      } else
        free(markers);
      printf("%s/%s: replay markers verified\n", app, route);
      fflush(stdout);
    }
    free(reference);
  }
  puts("UI-REPLAY-OK");
  return failures ? 1 : 0;
}
#endif
