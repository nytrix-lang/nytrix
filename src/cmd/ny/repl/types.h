/* Shared data structures for persistent Ny REPL bindings and values. */
#ifndef REPL_TYPES_H
#define REPL_TYPES_H

#include <stddef.h>

typedef struct {
  char *name;
  char *doc;
  char *def;
  char *src;
  int kind;
} ny_doc_entry;

typedef struct {
  ny_doc_entry *data;
  size_t len;
  size_t cap;
  size_t *name_slots;
  size_t name_slot_cap;
} doc_list_t;

#endif
