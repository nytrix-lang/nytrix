#ifndef REPL_TYPES_H
#define REPL_TYPES_H

#include <stddef.h>

typedef struct {
	char *name;
	char *doc;
	char *def;
	char *src;
	int kind; // 0=unknown, 1=pkg, 2=mod, 3=fn, 4=var
} nt_doc_entry;

typedef struct {
	nt_doc_entry *data;
	size_t len;
	size_t cap;
} nt_doc_list;

#endif
