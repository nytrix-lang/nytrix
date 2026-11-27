#!/usr/bin/env bash
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
NYC=$(realpath "${1:?nyc path}")
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cd "$work"
for n in 17 31 32 33 47 63 64 65 95 96 127 128 129 191; do
  printf 'void dot(double *a,double *b,double *c) { for(long i=0;i<%s;i++)for(long j=0;j<%s;j++)for(long k=0;k<%s;k++)c[i*%s+j]+=a[i*%s+k]*b[j*%s+k]; }\n' "$n" "$n" "$n" "$n" "$n" "$n" > dot-$n.c
  for g in auto 1 2; do
    if [ "$g" = auto ]; then unset NYTRIX_POLY_HORIZONTAL; else export NYTRIX_POLY_HORIZONTAL=$g; fi
    NYTRIX_FAST_MATH=1 NYTRIX_POLY_EXPERIMENTAL_ALIAS_VERSIONING=1 NYTRIX_POLY_DUMP=1 "$NYC" -O3 -c dot-$n.c -o dot-$n-$g.o 2>dot-$n-$g.log
    grep -q '"simd_axis":"reduction"' dot-$n-$g.log
    clang -O2 -DN=$n "$here/horizontal-native-driver.c" dot-$n-$g.o -o dot-$n-$g.exe
    ./dot-$n-$g.exe
  done
done
# These requests must not enter the new reduction path. In particular the
# four-vector candidate exposed a downstream native-code failure at N=127.
for mode in strict disabled unsupported; do
  flags=()
  case "$mode" in
    strict) export NYTRIX_FAST_MATH=0 NYTRIX_POLY_HORIZONTAL=2 ;;
    disabled) export NYTRIX_FAST_MATH=1 NYTRIX_POLY_HORIZONTAL=2; flags=(-mno-avx) ;;
    unsupported) export NYTRIX_FAST_MATH=1 NYTRIX_POLY_HORIZONTAL=4 ;;
  esac
  if ! NYTRIX_POLY_EXPERIMENTAL_ALIAS_VERSIONING=1 NYTRIX_POLY_DUMP=1 "$NYC" -O3 "${flags[@]}" -c dot-127.c -o "reject-$mode.o" 2>"reject-$mode.log"; then
    # The current C frontend rejects all FP on its no-SSE target path.
    if [ "$mode" = disabled ] && grep -q 'floating-point or SIMD code is incompatible' "reject-$mode.log"; then
      echo "no-AVX FP compilation rejected by frontend (no native test)"
      continue
    fi
    cat "reject-$mode.log" >&2
    exit 1
  fi
  if grep -q '"simd_axis":"reduction"' "reject-$mode.log"; then
    echo "unexpected horizontal reduction: $mode" >&2
    exit 1
  fi
  clang -O2 -DN=127 "$here/horizontal-native-driver.c" "reject-$mode.o" -o "reject-$mode.exe"
  ./"reject-$mode.exe"
  echo "horizontal reduction rejected: $mode"
done
