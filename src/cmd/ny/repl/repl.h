#ifndef NY_REPL_H
#define NY_REPL_H

#include "base/common.h"
#include "base/options.h"
#include <stddef.h>

void ny_repl_run(int opt_level, const char *opt_pipeline, const char *init_code,
                 int batch_mode, const ny_options *options);
void ny_repl_set_std_mode(std_mode_t mode);
void ny_repl_set_plain(int plain);
void ny_repl_set_max_errors(int max_errors);

#endif
