#!/usr/bin/env bash
set -euo pipefail

repo_root="$(git rev-parse --show-toplevel)"
cd "$repo_root"

compiler="${NYTRIX_NY_FULL_BIN:-$repo_root/build/release/ny-full}"
mkdir -p "$repo_root/build/cache/scratch"
work="$(mktemp -d "$repo_root/build/cache/scratch/bpf-kernel-smoke.XXXXXX")"
trap 'rm -rf "$work"' EXIT

program='fn rotate_right32_8(int value) int {
  def word = value & 0xffffffff
  ((word >> 8) | (word << 24)) & 0xffffffff
}
(5 >> 1) + (7 << 1) +
(rotate_right32_8(0x0123456789abcd2e) == 0x2e89abcd ? 0 : 1)'

"$compiler" --native-backend=bpf --native-only -O3 --nyir-verify \
  --emit-asm="$work/program.s" -no-std -emit-only -e "$program"
clang --target=bpf -c "$work/program.s" -o "$work/program.o"
llvm-objcopy -O binary --only-section=socket \
  "$work/program.o" "$work/socket.bin"
cc -std=c11 -O2 -Wall -Wextra -Werror \
  etc/tests/code/targets/bpf/kernel-load-test-run.c \
  -o "$work/kernel-load-test-run"

if [[ "${NYTRIX_BPF_TEST_DOCKER:-0}" == "1" ]]; then
  image="${NYTRIX_BPF_TEST_IMAGE:-ubuntu:24.04}"
  docker run --rm --privileged -v "$work:/work:ro" "$image" \
    /work/kernel-load-test-run /work/socket.bin
else
  "$work/kernel-load-test-run" "$work/socket.bin"
fi
