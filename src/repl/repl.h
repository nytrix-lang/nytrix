#ifndef NY_REPL_H
#define NY_REPL_H

#include <stddef.h>

void ny_repl_run(int opt_level, const char *opt_pipeline, const char *init_code,
                 int batch_mode);
void ny_repl_set_plain(int plain);

#endif
