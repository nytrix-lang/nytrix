#ifndef NY_INCREMENTAL_H
#define NY_INCREMENTAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/*
 * Incremental compilation support for Nytrix.
 *
 * Provides file-level dependency tracking, stable file hashing,
 * incremental recompilation driver, and cache invalidation protocol.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* File fingerprint: stable hash of file content + metadata */
typedef struct {
    uint64_t content_hash;      /* FNV-1a hash of file content */
    uint64_t mtime_nsec;        /* Modification time (nanoseconds) */
    uint64_t size;              /* File size in bytes */
    uint64_t ino;               /* Inode number (for detecting replacement) */
    uint64_t dev;               /* Device number */
} ny_file_fingerprint_t;

/* Dependency edge: file A depends on file B */
typedef struct ny_dep_edge_t {
    struct ny_dep_edge_t *next;
    char *from_file;            /* Dependent file (relative path) */
    char *to_file;              /* Dependency file (relative path) */
    bool is_system;             /* System/include dependency */
} ny_dep_edge_t;

/* Dependency graph node */
typedef struct ny_dep_node_t {
    struct ny_dep_node_t *next;
    char *file_path;            /* File path (relative to project root) */
    ny_file_fingerprint_t fingerprint;
    ny_dep_edge_t *deps;        /* Files this node depends on */
    ny_dep_edge_t *rdeps;       /* Files that depend on this node (reverse deps) */
    bool needs_recompile;       /* Whether this file needs recompilation */
    bool visited;               /* For DFS traversal */
} ny_dep_node_t;

/* Dependency graph */
typedef struct {
    ny_dep_node_t *nodes;
    size_t node_count;
    char *project_root;         /* Absolute path to project root */
    uint64_t graph_hash;        /* Hash of entire graph structure */
} ny_dep_graph_t;

/* Incremental compilation result */
typedef struct {
    bool success;
    char **recompile_files;     /* Files that need recompilation */
    size_t recompile_count;
    char **invalidated_cache;   /* Cache entries invalidated */
    size_t invalidated_count;
    double elapsed_ms;
    char *error_message;        /* Error message if failed */
} ny_incremental_result_t;

/* Configuration for incremental compilation */
typedef struct {
    const char *project_root;   /* Project root directory */
    const char *cache_dir;      /* Cache directory for dependency graph */
    bool enable_incremental;    /* Enable incremental compilation */
    bool force_full_rebuild;    /* Force full rebuild */
    bool track_system_deps;     /* Track system header dependencies */
    int max_depth;              /* Max dependency depth to track */
} ny_incremental_config_t;

/* Initialize incremental compilation subsystem */
bool ny_incremental_init(const ny_incremental_config_t *config);

/* Shutdown incremental compilation subsystem */
void ny_incremental_shutdown(void);

/* Compute stable fingerprint of a file */
bool ny_incremental_file_fingerprint(const char *file_path,
                                     ny_file_fingerprint_t *out_fingerprint);

/* Parse a source file and extract its dependencies (imports, includes) */
bool ny_incremental_parse_dependencies(const char *file_path,
                                       const char *source,
                                       size_t source_len,
                                       char ***out_deps,
                                       size_t *out_dep_count);

/* Build dependency graph from source files */
bool ny_incremental_build_graph(const char **source_files,
                                size_t file_count,
                                ny_dep_graph_t *out_graph);

/* Load dependency graph from cache */
bool ny_incremental_load_graph(const char *cache_path,
                               ny_dep_graph_t *out_graph);

/* Save dependency graph to cache */
bool ny_incremental_save_graph(const ny_dep_graph_t *graph,
                               const char *cache_path);

/* Check which files need recompilation based on fingerprint changes */
bool ny_incremental_check_changes(ny_dep_graph_t *graph,
                                  char ***out_changed_files,
                                  size_t *out_changed_count);

/* Compute transitive closure of files that need recompilation */
bool ny_incremental_compute_recompile_set(ny_dep_graph_t *graph,
                                          const char **changed_files,
                                          size_t changed_count,
                                          char ***out_recompile_files,
                                          size_t *out_recompile_count);

/* Invalidate cache entries for files and their dependents */
bool ny_incremental_invalidate_cache(const ny_dep_graph_t *graph,
                                     const char **recompile_files,
                                     size_t recompile_count);

/* Run incremental compilation */
ny_incremental_result_t ny_incremental_compile(const char **source_files,
                                                size_t file_count,
                                                const ny_incremental_config_t *config);

/* Free dependency graph */
void ny_incremental_free_graph(ny_dep_graph_t *graph);

/* Free incremental result */
void ny_incremental_free_result(ny_incremental_result_t *result);

/* Debug: print dependency graph */
void ny_incremental_dump_graph(const ny_dep_graph_t *graph, FILE *out);

#ifdef __cplusplus
}
#endif

#endif /* NY_INCREMENTAL_H */