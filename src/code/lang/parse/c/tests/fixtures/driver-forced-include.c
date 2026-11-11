/*
 * Fixture: expose source paths and a macro supplied by a forced include.
 */
#include "driver-forced-include.h"
const char *nyc_driver_source_path = __FILE__;
const char *nyc_driver_base_path = __BASE_FILE__;
int nyc_driver_forced_value(void) { return NYC_DRIVER_FORCED_VALUE; }
