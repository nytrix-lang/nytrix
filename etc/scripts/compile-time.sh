#!/usr/bin/env bash
#
# Measure Nytrix compile time without depending on /usr/bin/time, which is not
# installed everywhere (it is absent on the CI containers this repository is
# developed on, where the shell builtin is the only clock available).
#
# Each run compiles with the native and AOT caches disabled, so the number
# reflects real work rather than a cache hit. Reports the median, which is far
# steadier than a mean on a shared machine.
#
#   etc/scripts/compile-time.sh fixtures/bench.ny
#   etc/scripts/compile-time.sh -n 9 -l 'gate' fixtures/bench.ny
#   etc/scripts/compile-time.sh -- -Dfoo fixtures/bench.ny
#
# Extra arguments after the source file are passed to the compiler unchanged.

set -euo pipefail

runs=7
label=""
output=""

usage() {
  sed -n '3,16p' "$0" | sed 's/^# \{0,1\}//'
  cat <<'USAGE'

Options:
  -n RUNS     number of compiles (default 7)
  -l LABEL    label printed before the result
  -o PATH     output path (default: a temporary file)
  -h          show this help
USAGE
}

while getopts ":n:l:o:h" opt; do
  case "$opt" in
    n) runs=$OPTARG ;;
    l) label=$OPTARG ;;
    o) output=$OPTARG ;;
    h) usage; exit 0 ;;
    *) usage >&2; exit 2 ;;
  esac
done
shift $((OPTIND - 1))

src=${1:-}
if [ -z "$src" ]; then
  usage >&2
  exit 2
fi
shift

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
ny=${NY:-$root/build/release/ny}
if [ ! -x "$ny" ]; then
  echo "compile-time: no compiler at $ny (set NY=/path/to/ny)" >&2
  exit 1
fi
if [ ! -f "$src" ]; then
  echo "compile-time: no such source: $src" >&2
  exit 1
fi

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT
if [ -z "$output" ]; then
  output=$tmpdir/out
fi

times=()
for i in $(seq 1 "$runs"); do
  start=$(date +%s%N)
  NYTRIX_NATIVE_CACHE=0 NYTRIX_AOT_CACHE=0 \
    "$ny" -O3 --native-backend x86_64 --run=none --output="$output" "$src" "$@" \
    >"$tmpdir/log" 2>&1 || {
      echo "compile-time: compilation failed on run $i:" >&2
      cat "$tmpdir/log" >&2
      exit 1
    }
  end=$(date +%s%N)
  times+=($(( (end - start) / 1000000 )))
done

mapfile -t sorted < <(printf '%s\n' "${times[@]}" | sort -n)
median=${sorted[$((runs / 2))]}
[ -n "$label" ] && printf '%-14s ' "$label"
printf 'median %d ms  min %d ms  max %d ms  (n=%d, cold caches)\n' \
  "$median" "${sorted[0]}" "${sorted[$((runs - 1))]}" "$runs"
