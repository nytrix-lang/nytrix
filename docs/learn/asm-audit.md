<!-- nytrix-doc: {"audience":"contributor","featured":false,"group":"learn","order":190,"summary":"Per-fixture native machine-code sizes from --bench-show-asm for every benchmark source."} -->
# Native assembly and code-size audit

Collected 2026-11-15 on the release build, one `--bench-show-asm` run per
fixture:

```
ny-test --bin build/release/ny-full --bench <fixture> --bench-show-asm \
  --bench-run 1 --bench-warmup 0
```

for all 77 `etc/tests/bench/**/*.nshape` sources. `code bytes` is the
internal x86-64 machine code size the runner reports, so it measures
emitted code rather than the assembly printer.

Timings below come from a single sample per fixture. The C column is the
runner's C route for that fixture and can use a different repetition count
from the fixture's own timing loop (`knight` reports 16.5x here while the
harness row for it is 1.61x). Use these numbers only to rank code size;
[the full benchmark report](measurements.md) is the timing evidence.

- fixtures: 77 (76 ok, 1 fail)
- emitted machine code: 91,714,429 bytes total; median 26,381 bytes (bce-matrix)
- records without a byte count: 0

## Largest fixtures

| fixture | path | code bytes |
|---|---|---|
| pbkdf2 | `etc/tests/bench/algorithms/pbkdf2.nshape` | 5,627,626 |
| sha256 | `etc/tests/bench/algorithms/sha256.nshape` | 5,592,189 |
| inline | `etc/tests/bench/startup/inline.nshape` | 5,392,368 |
| call-chain | `etc/tests/bench/concurrency/call-chain.nshape` | 5,391,983 |
| expression-tree | `etc/tests/bench/algorithms/expression-tree.nshape` | 5,307,968 |
| page-rank | `etc/tests/bench/algorithms/page-rank.nshape` | 4,522,018 |
| bigint-oracle | `etc/tests/bench/numeric/bigint-oracle.nshape` | 4,295,923 |
| json-parser | `etc/tests/bench/string/json-parser.nshape` | 3,950,079 |

## All fixtures

| fixture | path | status | NYR | C | code bytes |
|---|---|---|---|---|---|
| append-scaling | `etc/tests/bench/string/append-scaling.nshape` | ok | 8.47ms | 13.1ms | 19814 |
| bce-matrix | `etc/tests/bench/numeric/bce-matrix.nshape` | ok | 4.92ms | 2.20ms | 26381 |
| bigint-oracle | `etc/tests/bench/numeric/bigint-oracle.nshape` | ok | 36.0ms | 3.72ms | 4295923 |
| binary | `etc/tests/bench/algorithms/binary.nshape` | ok | 823µs | 559µs | 25630 |
| binary-trees | `etc/tests/bench/algorithms/binary-trees.nshape` | ok | 3.1µs | 16µs | 23797 |
| call-chain | `etc/tests/bench/concurrency/call-chain.nshape` | ok | 587µs | 586µs | 5391983 |
| calls | `etc/tests/bench/concurrency/calls.nshape` | ok | 104µs | 103µs | 20341 |
| cmov-sort | `etc/tests/bench/startup/cmov-sort.nshape` | ok | 882µs | 572µs | 20796 |
| collatz | `etc/tests/bench/algorithms/collatz.nshape` | ok | 78µs | 55µs | 6497 |
| currying | `etc/tests/bench/startup/currying.nshape` | FAIL | 2.21ms | 4.9µs | 27034 |
| dce | `etc/tests/bench/startup/dce.nshape` | ok | 15µs | 247µs | 3149464 |
| dgemm | `etc/tests/bench/numeric/dgemm.nshape` | ok | 40µs | 40µs | 65454 |
| dict | `etc/tests/bench/datastructures/dict.nshape` | ok | 12.4ms | 5.73ms | 405931 |
| dict-oracle | `etc/tests/bench/datastructures/dict-oracle.nshape` | ok | 68µs | 30µs | 406426 |
| dict-set | `etc/tests/bench/datastructures/dict-set.nshape` | ok | 56.3ms | 86.4ms | 3544919 |
| escape-loop | `etc/tests/bench/startup/escape-loop.nshape` | ok | 4.58ms | 4.97ms | 17792 |
| euler9 | `etc/tests/bench/algorithms/euler9.nshape` | ok | 55µs | 30µs | 11465 |
| expression-tree | `etc/tests/bench/algorithms/expression-tree.nshape` | ok | 8.37ms | 6.89ms | 5307968 |
| fannkuch | `etc/tests/bench/algorithms/fannkuch.nshape` | ok | 237ms | 214ms | 26087 |
| fasta | `etc/tests/bench/string/fasta.nshape` | ok | 1.75ms | 1.37ms | 23883 |
| fft | `etc/tests/bench/numeric/fft.nshape` | ok | 17µs | 39µs | 3615888 |
| fibonacci | `etc/tests/bench/numeric/fibonacci.nshape` | ok | 118µs | 112µs | 21064 |
| float | `etc/tests/bench/numeric/float.nshape` | ok | 9.45ms | 9.19ms | 21547 |
| float-cmp-oracle | `etc/tests/bench/numeric/float-cmp-oracle.nshape` | ok | 19µs | 10µs | 3600354 |
| gcbench | `etc/tests/bench/startup/gcbench.nshape` | ok | 39.7ms | 23.0ms | 24817 |
| graph-bfs | `etc/tests/bench/algorithms/graph-bfs.nshape` | ok | 23.2ms | 19.7ms | 41627 |
| hash | `etc/tests/bench/datastructures/hash.nshape` | ok | 4.46ms | 3.69ms | 18257 |
| havlak | `etc/tests/bench/algorithms/havlak.nshape` | ok | 3.52ms | 2.32ms | 22167 |
| heapsort | `etc/tests/bench/datastructures/heapsort.nshape` | ok | 4.78ms | 4.82ms | 25916 |
| inline | `etc/tests/bench/startup/inline.nshape` | ok | 60µs | 2.09ms | 5392368 |
| intops | `etc/tests/bench/numeric/intops.nshape` | ok | 636µs | 346µs | 19665 |
| iter | `etc/tests/bench/datastructures/iter.nshape` | ok | 385µs | 337µs | 25688 |
| json-parser | `etc/tests/bench/string/json-parser.nshape` | ok | 13.1ms | 12.4ms | 3950079 |
| knight | `etc/tests/bench/algorithms/knight.nshape` | ok | 2.35ms | 142µs | 34558 |
| knucleotide | `etc/tests/bench/algorithms/knucleotide.nshape` | ok | 435µs | 364µs | 18855 |
| knucleotide | `etc/tests/bench/string/knucleotide.nshape` | ok | 122µs | 120µs | 23514 |
| licm | `etc/tests/bench/startup/licm.nshape` | ok | 74µs | 610µs | 3707492 |
| licm-math | `etc/tests/bench/startup/licm-math.nshape` | ok | 4.58ms | 4.64ms | 18778 |
| linpack | `etc/tests/bench/numeric/linpack.nshape` | ok | 268µs | 96µs | 3453711 |
| list | `etc/tests/bench/datastructures/list.nshape` | ok | 3.98ms | 4.15ms | 3418251 |
| list-any-append | `etc/tests/bench/datastructures/list-any-append.nshape` | ok | 9.13ms | 8.54ms | 3414172 |
| list-dynamic | `etc/tests/bench/datastructures/list-dynamic.nshape` | ok | 5.92ms | 4.40ms | 3418251 |
| loop | `etc/tests/bench/startup/loop.nshape` | ok | 1.15ms | 1.17ms | 18275 |
| loop-unswitch | `etc/tests/bench/startup/loop-unswitch.nshape` | ok | 4.49ms | 4.48ms | 24176 |
| mandel | `etc/tests/bench/algorithms/mandel.nshape` | ok | 137µs | 80µs | 7905 |
| mandelbrot | `etc/tests/bench/algorithms/mandelbrot.nshape` | ok | 325µs | 408µs | 3597871 |
| matrix | `etc/tests/bench/numeric/matrix.nshape` | ok | 22µs | 7.6µs | 20954 |
| matrix-raw | `etc/tests/bench/numeric/matrix-raw.nshape` | ok | 43µs | 30µs | 3600142 |
| mixed-static-is~ | `etc/tests/bench/startup/mixed-static-island.nshape` | ok | 33µs | 30µs | 21829 |
| n-body | `etc/tests/bench/algorithms/n-body.nshape` | ok | 2.72ms | 1.82ms | 26466 |
| native-oracle | `etc/tests/bench/startup/native-oracle.nshape` | ok | 88µs | 33µs | 17361 |
| nqueens | `etc/tests/bench/algorithms/nqueens.nshape` | ok | 27.8ms | 27.4ms | 21116 |
| page-rank | `etc/tests/bench/algorithms/page-rank.nshape` | ok | 18µs | 4.9µs | 4522018 |
| pbkdf2 | `etc/tests/bench/algorithms/pbkdf2.nshape` | ok | 160µs | 1.75ms | 5627626 |
| pbkdf2 | `etc/tests/bench/numeric/pbkdf2.nshape` | ok | 960µs | 1.10ms | 103673 |
| pidigits | `etc/tests/bench/numeric/pidigits.nshape` | ok | 9.4µs | 7.1µs | 75209 |
| prime | `etc/tests/bench/algorithms/prime.nshape` | ok | 1.35ms | 1.23ms | 8906 |
| queen | `etc/tests/bench/algorithms/queen.nshape` | ok | 2.35ms | 1.78ms | 20434 |
| records | `etc/tests/bench/datastructures/records.nshape` | ok | 46.7ms | 44.1ms | 35014 |
| recursive-fibon~ | `etc/tests/bench/startup/recursive-fibonacci.nshape` | ok | 310ns | 191ns | 3710910 |
| repeat | `etc/tests/bench/string/repeat.nshape` | ok | 83.8ms | 97.9ms | 14927 |
| revcomp | `etc/tests/bench/string/revcomp.nshape` | ok | 5.0µs | 7.8µs | 30235 |
| sha256 | `etc/tests/bench/algorithms/sha256.nshape` | ok | 1.36ms | 896µs | 5592189 |
| sha256 | `etc/tests/bench/numeric/sha256.nshape` | ok | 756µs | 907µs | 37510 |
| sieve | `etc/tests/bench/algorithms/sieve.nshape` | ok | 6.04ms | 4.94ms | 24934 |
| sor | `etc/tests/bench/algorithms/sor.nshape` | ok | 1.74ms | 851µs | 28110 |
| spectral | `etc/tests/bench/algorithms/spectral.nshape` | ok | 80µs | 27µs | 3611523 |
| splay | `etc/tests/bench/datastructures/splay.nshape` | ok | 4.01ms | 4.07ms | 3611602 |
| startup | `etc/tests/bench/startup/startup.nshape` | ok | 27ns | 27ns | 17072 |
| straight | `etc/tests/bench/startup/straight.nshape` | ok | 20µs | 20µs | 17997 |
| string | `etc/tests/bench/string/string.nshape` | ok | 198µs | 284µs | 28818 |
| tce-recursion | `etc/tests/bench/startup/tce-recursion.nshape` | ok | 91µs | 91µs | 19481 |
| thread-ring | `etc/tests/bench/concurrency/thread-ring.nshape` | ok | 6.32ms | 4.54ms | 27509 |
| time-profile | `etc/tests/bench/startup/time-profile.nshape` | ok | 99µs | 21µs | 16444 |
| vector | `etc/tests/bench/datastructures/vector.nshape` | ok | 336µs | 267µs | 20227 |
| word-frequency | `etc/tests/bench/string/word-frequency.nshape` | ok | 4.63ms | 3.34ms | 23981 |
| word-frequency-~ | `etc/tests/bench/string/word-frequency-scan.nshape` | ok | 3.03ms | 2.49ms | 27381 |

## Findings

- The size cost is MACH-to-x64 expansion, not one missed opcode idiom.
  `ny_fn_moves` (`knight`) is 188 MACH instructions and 498 x64
  instructions, 199 of them `(%rbp)` home traffic: each MACH operand is
  materialised through a stack home unless a value stays resident across
  the block.
- Zero tests already use `test %reg,%reg`, and immediates already take the
  zero / 32-bit / sign-extended forms, so `cmp $0` and `mov $0` peepholes
  have no remaining targets (0 occurrences of `cmp $0` in `knight`).
- 37 adjacent `mov A,B; mov B,A` reversed pairs remain in `knight`. They
  come out of the inline ALU/copy paths, so removing them needs a
  MACH-level copy-forwarding pass (see `measurements.md`, 2026-11-15).
- The 32-bit path already emits compact forms: no `pushl`/`popl` pairs, no
  `cmpl $0`, no self moves in `knight`'s i386 assembly; its 119 `pushl`
  are call arguments under the stack ABI.
