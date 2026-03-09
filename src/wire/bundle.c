#include "wire/bundle.h"
#include "base/loader.h"
#include "base/util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int ny_bundle_save(const ny_options *opt) {
  if (!opt->bundle_std_path && !opt->bundle_symbols_path)
    return 0;

  std_mode_t mode = opt->std_mode;
  if (mode == STD_MODE_DEFAULT || mode == STD_MODE_MINIMAL)
    mode = STD_MODE_FULL;

  if (opt->bundle_std_path) {
    char *bundle = ny_build_std_source_ex(NULL, 0, mode, opt->verbose, NULL, false);
    if (!bundle) {
      NY_LOG_ERR("Failed to generate std.ny\n");
      return 1;
    }
    if (ny_write_if_changed(opt->bundle_std_path, bundle, strlen(bundle))) {
      if (opt->verbose)
        NY_LOG_SUCCESS("Generated std.ny -> %s\n", opt->bundle_std_path);
    }
    free(bundle);
  }

  if (opt->bundle_symbols_path) {
    char *header = ny_std_generate_c_symbols_header(mode);
    if (!header) {
      NY_LOG_ERR("Failed to generate c symbols header\n");
      return 1;
    }
    if (ny_write_if_changed(opt->bundle_symbols_path, header, strlen(header))) {
      if (opt->verbose)
        NY_LOG_SUCCESS("Generated symbols -> %s\n", opt->bundle_symbols_path);
    }
    free(header);
  }

  return 0;
}
