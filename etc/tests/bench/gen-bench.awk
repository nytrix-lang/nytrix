#!/usr/bin/awk -f
# gen-bench.awk — generate benchmark nshape files with repetitive patterns
#
# Usage:
#   awk -f gen-bench.awk -v bench=straight </dev/null
#   awk -f gen-bench.awk -v bench=dce </dev/null

function shape_header(name, family, generator, features) {
  printf "shape %s {\n", name
  printf "  family \"%s\"\n", family
  printf "  generator \"%s\"\n", generator
  printf "  features %s\n", features
  print  "  template ny-test-case"
  print  "  expect compile_and_run"
}

function ny_open() {
  print "  source ny <<'NY'"
}

function ny_preamble() {
  print "use std.core"
  print "use std.os.time"
}

function ny_epilogue() {
  print "def t1 = ticks()"
  print "print(\"checksum=\", x)"
  print "print(\"elapsed_ns=\", t1 - t0)"
}

function ny_close() {
  print "NY"
  print "}"
}

BEGIN {
  if (bench == "straight") {
    shape_header("bench_straight",
      "perf-real", "stress",
      "[\"perf\", \"straight-line\", \"native\", \"checksum\", \"time\"]")
    ny_open()
    ny_preamble()
    print "def t0 = ticks()"
    print "mut x = 0"
    for (i = 1; i <= 100; i++)
      print "x = x + " i
    ny_epilogue()
    ny_close()
  } else if (bench == "dce") {
    shape_header("bench_dce",
      "perf-real", "stress",
      "[\"perf\", \"dce\", \"branch\", \"native\", \"checksum\", \"time\"]")
    ny_open()
    ny_preamble()
    print "def t0 = ticks()"
    print "mut x = 0"
    for (i = 0; i < 500; i++)
      print "if x == " i " { x = x + 1 } else { x = x + 2 }"
    ny_epilogue()
    ny_close()
  } else {
    print "Unknown bench: " bench > "/dev/stderr"
    exit 1
  }
}
