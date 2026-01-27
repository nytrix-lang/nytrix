#ifndef NY_BUILDER_H
#define NY_BUILDER_H

#include <stdbool.h>
#include <unistd.h>

const char *ny_builder_choose_cc(void);
int ny_exec_spawn(const char *const argv[]);

bool ny_builder_compile_runtime(const char *cc, const char *out_runtime,
                                const char *out_ast, bool debug);
bool ny_builder_link(const char *cc, const char *obj_path,
                     const char *runtime_obj, const char *runtime_ast_obj,
                     const char *const extra_objs[], size_t extra_count,
                     const char *const link_dirs[], size_t link_dir_count,
                     const char *const link_libs[], size_t link_lib_count,
                     const char *output_path, bool link_strip, bool debug);
bool ny_builder_strip(const char *path);

#endif
