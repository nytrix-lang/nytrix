#ifndef REPL_TYPES_H
#define REPL_TYPES_H

#include <stddef.h>
#include "std_loader.h"

typedef struct {
	char *name;
	char *doc;
	char *def;
} nt_doc_entry;

typedef struct {
	nt_doc_entry *data;
	size_t len;
	size_t cap;
} nt_doc_list;

void nt_repl_run(int opt_level, const char *opt_pipeline, const char *init_code);
void nt_repl_set_std_mode(nt_std_mode mode);
void nt_repl_set_plain(int plain);

#endif
