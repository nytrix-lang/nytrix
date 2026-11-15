/*
 * Selftest runner for sinking safe additions across machine CFG edges.
 */
#include "code/backend/lower/machine.h"
#include <stdio.h>

int main(void) {
  if (!nyr_mach_sink_edge_adds_selftest()) {
    fputs("[machine-edge-add-sink] dependency/barrier checks failed\n", stderr);
    return 1;
  }
  puts("[machine-edge-add-sink] edge addition sinks; input writes, extra uses, "
       "effects and ordinary copies retained");
  return 0;
}
