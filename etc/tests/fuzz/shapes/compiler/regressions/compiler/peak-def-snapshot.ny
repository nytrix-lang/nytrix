;; NY-008: Nytrix peak profile miscompiles an immutable def snapshot of a
;; mutable int when the snapshot is reused after mutating the original.
;;
;; Expected output from O0, O3, and non-peak O3 profiles:
;; -1
;;
;; Repro:
;;   ny --compiler-asserts -o /tmp/ny008_o0 etc/tests/fuzz/errors/compiler/peak-def-snapshot.ny && /tmp/ny008_o0
;;   ny --compiler-asserts -O3 -o /tmp/ny008_o3 etc/tests/fuzz/errors/compiler/peak-def-snapshot.ny && /tmp/ny008_o3
;;   ny --compiler-asserts -O3 --profile=peak -o /tmp/ny008_peak etc/tests/fuzz/errors/compiler/peak-def-snapshot.ny && /tmp/ny008_peak
;;
;; Current local bad output from peak on 2026-06-01:
;; 0

mut int: acc = 1
def int: row = acc
acc -= row
acc -= row
print(acc)
