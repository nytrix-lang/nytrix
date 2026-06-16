#include "core.h"

static int usage(void) {
  nynth_print_worker_usage(stdout);
  return 2;
}

int main(int argc, char **argv) {
  if (argc == 3 && strcmp(argv[1], "shape-count") == 0) return cmd_shape_count(argv[2]);
  if (argc == 3 && strcmp(argv[1], "hash-case") == 0) return cmd_hash_case(argv[2]);
  if (argc == 3 && strcmp(argv[1], "scan-ir-markers") == 0) return cmd_scan_ir_markers(argv[2]);
  if (argc == 3 && strcmp(argv[1], "analyze-ir") == 0) return cmd_analyze_ir(argv[2]);
  if (argc == 3 && strcmp(argv[1], "validate-shapes") == 0) return cmd_validate_shapes(argv[2]);
  if (argc == 4 && strcmp(argv[1], "source-shape-counts") == 0) return cmd_source_shape_counts(argv[2], argv[3]);
  if (argc >= 3 && strcmp(argv[1], "generate-batch") == 0) return cmd_generate_batch(argc, argv);
  if (argc >= 3 && strcmp(argv[1], "convert-cbridge") == 0) return cmd_convert_cbridge(argc, argv);
  if (argc >= 3 && strcmp(argv[1], "compare-case") == 0) return cmd_compare_case(argc, argv);
  if (argc >= 3 && strcmp(argv[1], "replay-corpus-entry") == 0) return cmd_replay_corpus_entry(argc, argv);
  return usage();
}
