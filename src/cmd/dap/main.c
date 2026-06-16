#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <unistd.h>

static int g_seq = 1;
static char g_program[4096];
static int g_last_exit = 0;

static char *read_message(void) {
  char line[256];
  int content_len = 0;
  for (;;) {
    size_t n = 0;
    int c = 0;
    while (n + 1 < sizeof(line) && (c = getchar()) != EOF) {
      if (c == '\r')
        continue;
      if (c == '\n')
        break;
      line[n++] = (char)c;
    }
    if (c == EOF)
      return NULL;
    line[n] = '\0';
    if (n == 0)
      break;
    if (strncasecmp(line, "Content-Length:", 15) == 0)
      content_len = atoi(line + 15);
  }
  if (content_len <= 0 || content_len > 16 * 1024 * 1024)
    return NULL;
  char *body = (char *)calloc((size_t)content_len + 1, 1);
  if (!body)
    return NULL;
  if (fread(body, 1, (size_t)content_len, stdin) != (size_t)content_len) {
    free(body);
    return NULL;
  }
  return body;
}

static void send_json(const char *json) {
  int n = (int)strlen(json);
  printf("Content-Length: %d\r\n\r\n%s", n, json);
  fflush(stdout);
}

static char *json_string(const char *json, const char *key) {
  char pat[128];
  snprintf(pat, sizeof(pat), "\"%s\"", key);
  const char *p = strstr(json, pat);
  if (!p)
    return NULL;
  p = strchr(p + strlen(pat), ':');
  if (!p)
    return NULL;
  p = strchr(p, '"');
  if (!p)
    return NULL;
  p++;
  const char *e = p;
  while (*e && (*e != '"' || e[-1] == '\\'))
    e++;
  char *out = (char *)malloc((size_t)(e - p) + 1);
  if (!out)
    return NULL;
  memcpy(out, p, (size_t)(e - p));
  out[e - p] = '\0';
  return out;
}

static int json_int(const char *json, const char *key, int fallback) {
  char pat[128];
  snprintf(pat, sizeof(pat), "\"%s\"", key);
  const char *p = strstr(json, pat);
  if (!p)
    return fallback;
  p = strchr(p + strlen(pat), ':');
  if (!p)
    return fallback;
  return atoi(p + 1);
}

static void respond(int request_seq, const char *command, int success, const char *body) {
  char msg[8192];
  snprintf(msg, sizeof(msg),
           "{\"seq\":%d,\"type\":\"response\",\"request_seq\":%d,\"success\":%s,"
           "\"command\":\"%s\",\"body\":%s}",
           g_seq++, request_seq, success ? "true" : "false", command ? command : "",
           body ? body : "{}");
  send_json(msg);
}

static void event(const char *name, const char *body) {
  char msg[8192];
  snprintf(msg, sizeof(msg), "{\"seq\":%d,\"type\":\"event\",\"event\":\"%s\",\"body\":%s}",
           g_seq++, name ? name : "", body ? body : "{}");
  send_json(msg);
}

static int run_program(const char *program) {
  if (!program || !*program)
    return 1;
  pid_t pid = fork();
  if (pid == 0) {
    execlp("ny", "ny", program, (char *)NULL);
    execlp("./build/release/ny", "./build/release/ny", program, (char *)NULL);
    _exit(127);
  }
  if (pid < 0)
    return 1;
  int st = 0;
  while (waitpid(pid, &st, 0) < 0) {
  }
  if (WIFEXITED(st))
    return WEXITSTATUS(st);
  if (WIFSIGNALED(st))
    return 128 + WTERMSIG(st);
  return 1;
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  for (;;) {
    char *msg = read_message();
    if (!msg)
      break;
    char *command = json_string(msg, "command");
    int seq = json_int(msg, "seq", 0);
    if (!command) {
      free(msg);
      continue;
    }
    if (strcmp(command, "initialize") == 0) {
      respond(seq, command, 1,
              "{\"supportsConfigurationDoneRequest\":true,\"supportsTerminateRequest\":true,"
              "\"supportsSetVariable\":false,\"supportsEvaluateForHovers\":true}");
      event("initialized", "{}");
    } else if (strcmp(command, "launch") == 0) {
      char *program = json_string(msg, "program");
      if (program && *program) {
        snprintf(g_program, sizeof(g_program), "%s", program);
        respond(seq, command, 1, "{}");
        event("process", "{\"name\":\"ny\",\"systemProcessId\":0,\"isLocalProcess\":true,\"startMethod\":\"launch\"}");
      } else {
        respond(seq, command, 0, "{\"error\":{\"format\":\"launch requires program\"}}");
      }
      free(program);
    } else if (strcmp(command, "configurationDone") == 0) {
      respond(seq, command, 1, "{}");
      if (g_program[0]) {
        g_last_exit = run_program(g_program);
        char body[128];
        snprintf(body, sizeof(body), "{\"exitCode\":%d}", g_last_exit);
        event("exited", body);
        event("terminated", "{}");
      }
    } else if (strcmp(command, "threads") == 0) {
      respond(seq, command, 1, "{\"threads\":[{\"id\":1,\"name\":\"main\"}]}");
    } else if (strcmp(command, "stackTrace") == 0) {
      respond(seq, command, 1,
              "{\"stackFrames\":[{\"id\":1,\"name\":\"main\",\"line\":1,\"column\":1}],\"totalFrames\":1}");
    } else if (strcmp(command, "scopes") == 0) {
      respond(seq, command, 1,
              "{\"scopes\":[{\"name\":\"Nytrix\",\"variablesReference\":1,\"expensive\":false}]}");
    } else if (strcmp(command, "variables") == 0) {
      char body[256];
      snprintf(body, sizeof(body),
               "{\"variables\":[{\"name\":\"program\",\"value\":\"%s\",\"variablesReference\":0},"
               "{\"name\":\"lastExitCode\",\"value\":\"%d\",\"variablesReference\":0}]}",
               g_program, g_last_exit);
      respond(seq, command, 1, body);
    } else if (strcmp(command, "continue") == 0) {
      respond(seq, command, 1, "{\"allThreadsContinued\":true}");
    } else if (strcmp(command, "disconnect") == 0 || strcmp(command, "terminate") == 0) {
      respond(seq, command, 1, "{}");
      free(command);
      free(msg);
      break;
    } else {
      respond(seq, command, 1, "{}");
    }
    free(command);
    free(msg);
  }
  return 0;
}
