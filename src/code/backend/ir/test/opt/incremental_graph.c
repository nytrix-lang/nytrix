/* Dependency inventories must never classify new or removed inputs as clean. */
#include "code/backend/wire/incremental/incremental.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>

static bool write_source(const char *path, const char *source) {
  FILE *file = fopen(path, "wb");
  if (!file)
    return false;
  size_t size = strlen(source);
  bool ok = fwrite(source, 1, size, file) == size;
  return fclose(file) == 0 && ok;
}

static bool has_path(char *const *paths, size_t count, const char *path) {
  for (size_t i = 0; i < count; ++i)
    if (strcmp(paths[i], path) == 0)
      return true;
  return false;
}

static bool expect_dirty(const char **files, size_t count,
                         const ny_incremental_config_t *config,
                         const char **expected, size_t expected_count) {
  ny_incremental_result_t result = ny_incremental_compile(files, count, config);
  bool ok = result.success && result.recompile_count == expected_count;
  for (size_t i = 0; ok && i < expected_count; ++i)
    ok = has_path(result.recompile_files, result.recompile_count, expected[i]);
  if (!ok)
    fprintf(stderr,
            "incremental inventory: expected %zu dirty files, got %zu "
            "(success=%d)\n",
            expected_count, result.recompile_count, result.success);
  ny_incremental_free_result(&result);
  return ok;
}
#endif

int main(void) {
#ifdef _WIN32
  return 0;
#else
  char root[] = "/tmp/nytrix-incremental-XXXXXX";
  if (!mkdtemp(root))
    return 1;
  char provider[512], consumer[512], independent[512], added[512], cache[512];
  char second_cache[512], duplicate_dir[512], duplicate[512];
  snprintf(provider, sizeof(provider), "%s/provider.ny", root);
  snprintf(consumer, sizeof(consumer), "%s/consumer.ny", root);
  snprintf(independent, sizeof(independent), "%s/independent.ny", root);
  snprintf(added, sizeof(added), "%s/added.ny", root);
  snprintf(cache, sizeof(cache), "%s/cache", root);
  snprintf(second_cache, sizeof(second_cache), "%s/other-cache", root);
  snprintf(duplicate_dir, sizeof(duplicate_dir), "%s/other", root);
  snprintf(duplicate, sizeof(duplicate), "%s/other/provider.ny", root);
  const char *files[] = {provider, consumer, independent};
  const char *changed[] = {provider, consumer};
  ny_incremental_config_t config = {
      .project_root = root, .cache_dir = cache, .enable_incremental = true};
  bool ok = write_source(provider, "fn value() int { 41 }\n") &&
            write_source(consumer, "use provider\n") &&
            write_source(independent, "def value = 7\n") &&
            expect_dirty(files, 3, &config, files, 3) &&
            expect_dirty(files, 3, &config, NULL, 0) &&
            write_source(provider, "fn value() int { 4200 }\n") &&
            expect_dirty(files, 3, &config, changed, 2);
  config.force_full_rebuild = true;
  ok = ok && expect_dirty(files, 3, &config, files, 3);
  config.force_full_rebuild = false;
  ok = ok && expect_dirty(files, 3, &config, NULL, 0);

  /* A deleted provider must still dirty its former consumers. */
  const char *survivors[] = {consumer, independent};
  const char *deleted_dependents[] = {consumer};
  ok = ok && unlink(provider) == 0 &&
       expect_dirty(survivors, 2, &config, deleted_dependents, 1) &&
       expect_dirty(survivors, 2, &config, NULL, 0);

  /* Added unrelated files do not dirty the surviving independent module. */
  const char *with_added[] = {consumer, independent, added};
  const char *only_added[] = {added};
  ok = ok && write_source(added, "def added = 13\n") &&
       expect_dirty(with_added, 3, &config, only_added, 1);

  /* Reconfiguration must not silently keep using the previous cache root. */
  config.cache_dir = second_cache;
  ok = ok && expect_dirty(with_added, 3, &config, with_added, 3) &&
       expect_dirty(with_added, 3, &config, NULL, 0);

  /* Conservatively retain every provider for an ambiguous basename. */
  ny_dep_graph_t graph = {0};
  ok = ok && mkdir(duplicate_dir, 0700) == 0 &&
       write_source(provider, "def value = 17\n") &&
       write_source(duplicate, "def value = 29\n");
  const char *ambiguous[] = {duplicate, provider, consumer};
  ok = ok && ny_incremental_build_graph(ambiguous, 3, &graph);
  char **dirty = NULL;
  size_t dirty_count = 0;
  const char *provider_changed[] = {provider};
  ok = ok &&
       ny_incremental_compute_recompile_set(&graph, provider_changed, 1, &dirty,
                                            &dirty_count) &&
       dirty_count == 2 && has_path(dirty, dirty_count, consumer) &&
       has_path(dirty, dirty_count, provider);
  for (size_t i = 0; i < dirty_count; ++i)
    free(dirty[i]);
  free(dirty);

  /* A source disappearing after graph construction is a change, not a hit. */
  char **missing = NULL;
  size_t missing_count = 0;
  ok = ok && unlink(provider) == 0 &&
       ny_incremental_check_changes(&graph, &missing, &missing_count) &&
       missing_count == 1 && has_path(missing, missing_count, provider);
  for (size_t i = 0; i < missing_count; ++i)
    free(missing[i]);
  free(missing);
  ny_incremental_free_graph(&graph);
  ny_incremental_shutdown();

  /* The graph filenames are implementation details; enumerate owned caches. */
  char command[2048];
  snprintf(command, sizeof(command), "rm -rf -- '%s'", root);
  if (system(command) != 0)
    ok = false;
  return ok ? 0 : 1;
#endif
}
