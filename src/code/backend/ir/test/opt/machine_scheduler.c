#include "code/backend/ir/machine.h"

#include <stdio.h>

int main(void) {
  if (!nyr_mach_schedule_post_ra_selftest()) {
    fputs("[machine-scheduler] safe-segment scheduling regression failed\n",
          stderr);
    return 1;
  }
  puts("[machine-scheduler] pure segments reorder; barriers remain fixed");
  return 0;
}
