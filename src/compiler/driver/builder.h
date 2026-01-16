#ifndef NT_BUILDER_H
#define NT_BUILDER_H

#include <stdbool.h>
#include <unistd.h>

const char *nt_builder_choose_cc(void);
int nt_exec_spawn(const char *const argv[]);

bool nt_builder_compile_runtime(const char *cc, const char *out_runtime, const char *out_ast);
bool nt_builder_link(const char *cc, const char *obj_path, const char *runtime_obj,
					 const char *runtime_ast_obj, const char *const extra_objs[],
					 size_t extra_count, const char *output_path, bool link_strip);
bool nt_builder_strip(const char *path);

#endif
