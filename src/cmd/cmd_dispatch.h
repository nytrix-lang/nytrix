/*
 * Shared command dispatch and CLI flag parsing helper framework for Nytrix tools.
 */
#ifndef NYTRIX_CMD_DISPATCH_H
#define NYTRIX_CMD_DISPATCH_H

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct ny_cmd_subcommand_t {
  const char *name;
  const char *summary;
  int (*handler)(int argc, char **argv);
} ny_cmd_subcommand_t;

typedef struct ny_cmd_app_t {
  const char *name;
  const char *version;
  const char *usage;
  const ny_cmd_subcommand_t *subcommands;
  size_t subcommand_count;
} ny_cmd_app_t;

static inline void ny_cmd_print_usage(const ny_cmd_app_t *app) {
  if (!app)
    return;
  printf("Usage: %s %s\n\n", app->name ? app->name : "tool",
         app->usage ? app->usage : "[options] [command]");
  if (app->subcommands && app->subcommand_count > 0) {
    printf("Commands:\n");
    for (size_t i = 0; i < app->subcommand_count; i++) {
      printf("  %-16s %s\n", app->subcommands[i].name,
             app->subcommands[i].summary ? app->subcommands[i].summary : "");
    }
    printf("\n");
  }
}

static inline int ny_cmd_dispatch(const ny_cmd_app_t *app, int argc,
                                  char **argv) {
  if (!app)
    return 1;
  if (argc <= 1) {
    ny_cmd_print_usage(app);
    return 0;
  }
  const char *arg1 = argv[1];
  if (strcmp(arg1, "-h") == 0 || strcmp(arg1, "--help") == 0 ||
      strcmp(arg1, "help") == 0) {
    ny_cmd_print_usage(app);
    return 0;
  }
  if (strcmp(arg1, "-v") == 0 || strcmp(arg1, "--version") == 0 ||
      strcmp(arg1, "version") == 0) {
    printf("%s %s\n", app->name ? app->name : "tool",
           app->version ? app->version : "0.1.0");
    return 0;
  }
  for (size_t i = 0; i < app->subcommand_count; i++) {
    if (app->subcommands[i].name &&
        strcmp(arg1, app->subcommands[i].name) == 0) {
      return app->subcommands[i].handler(argc - 1, argv + 1);
    }
  }
  fprintf(stderr, "Unknown command '%s'. Run '%s --help' for available commands.\n",
          arg1, app->name ? app->name : "tool");
  return 1;
}

#endif /* NYTRIX_CMD_DISPATCH_H */
