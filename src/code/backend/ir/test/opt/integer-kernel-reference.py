#!/usr/bin/env python3
"""Independent integer-kernel oracle. Expected output: 5168580067."""
acc = sum((i % 97) * (i % 89) for i in range(1, 250001))
acc += sum(i for i in range(1, 250001) if i % 11 == 0)
state = 7
mixed = 0
for i in range(1, 50001):
    state = (state * 1103515 + 12345) % 2147483647
    if state % 13 == 0:
        mixed += i
acc += mixed + state
for i in range(1, 100001):
    x = i * 1664525 + 1013904223
    acc += x % 65537 if x % 2 == 0 else -(x % 32749)
print(acc)
