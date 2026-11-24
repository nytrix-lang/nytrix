#!/usr/bin/env python3
"""Seeded differential fuzzer: random well-defined integer C programs, GCC vs NYC.

Every program uses only defined behaviour (unsigned arithmetic for wrapping
operations, masked shift counts, guarded division) and prints a checksum, so
any output difference between GCC and NYC at -O0/-O2 is a miscompile. Failing
seeds are kept as reproducers in the output directory.

usage: fuzz-expr.py NYC OUTDIR [--seeds A:B] [--jobs N]
"""
import argparse, os, random, subprocess, sys
from concurrent.futures import ThreadPoolExecutor

TYPES = [("uint8_t", 8, False), ("int8_t", 8, True), ("uint16_t", 16, False),
         ("int16_t", 16, True), ("uint32_t", 32, False), ("int32_t", 32, True),
         ("uint64_t", 64, False), ("int64_t", 64, True)]


class Gen:
    def __init__(self, seed):
        self.r = random.Random(seed)
        self.vars = []

    def atom(self):
        if self.vars and self.r.random() < 0.7:
            return self.r.choice(self.vars)
        if getattr(self, "loop", False) and self.r.random() < 0.3:
            return "(uint64_t)i"
        return "%du" % self.r.randrange(0, 1 << self.r.choice([4, 8, 16, 31]))

    def emit(self, text):
        """Bind one operation to a fresh temporary (three-address form) so a
        line-based reducer can minimise failures statement by statement."""
        self.tmp = getattr(self, "tmp", 0) + 1
        name = "e%d" % self.tmp
        self.stmts.append("  uint64_t %s = %s;" % (name, text))
        return name

    def expr(self, depth):
        if depth == 0:
            return self.atom()
        r = self.r
        k = r.randrange(11)
        a, b = self.expr(depth - 1), self.expr(depth - 1)
        if k == 0: return self.emit("(uint64_t)(%s) + (uint64_t)(%s)" % (a, b))
        if k == 1: return self.emit("(uint64_t)(%s) - (uint64_t)(%s)" % (a, b))
        if k == 2: return self.emit("(uint64_t)(%s) * (uint64_t)(%s)" % (a, b))
        if k == 3: return self.emit("(uint64_t)(%s) & (uint64_t)(%s)" % (a, b))
        if k == 4: return self.emit("(uint64_t)(%s) | (uint64_t)(%s)" % (a, b))
        if k == 5: return self.emit("(uint64_t)(%s) ^ (uint64_t)(%s)" % (a, b))
        if k == 6: return self.emit("(uint64_t)(%s) << (%s & 63u)" % (a, b))
        if k == 7: return self.emit("(uint64_t)(%s) >> (%s & 63u)" % (a, b))
        if k == 8:
            t = r.choice(TYPES)[0]
            return self.emit("(uint64_t)(%s)(%s)" % (t, a))
        if k == 9:
            c = r.choice(["<", "<=", "==", "!=", ">", ">="])
            t = r.choice(TYPES)[0]
            x, y = self.expr(depth - 1), self.expr(depth - 1)
            return self.emit("(%s)(%s) %s (%s)(%s) ? (uint64_t)(%s) : (uint64_t)(%s)" % (t, a, c, t, b, x, y))
        d = "((uint64_t)(%s) | 1u)" % b
        op = "%" if r.random() < 0.5 else "/"
        return self.emit("(uint64_t)(%s) %s %s" % (a, op, d))

    def program(self):
        r = self.r
        out = ["#include <stdint.h>", "#include <stdio.h>",
               "static uint64_t sink = 1469598103934665603ull;",
               "static void mix(uint64_t v) { sink = (sink ^ v) * 1099511628211ull; }"]
        nvars = r.randrange(3, 8)
        decls = []
        for i in range(nvars):
            t = r.choice(TYPES)[0]
            decls.append((t, "g%d" % i, r.randrange(0, 1 << 31)))
        for t, n, v in decls:
            out.append("static %s %s = %d;" % (t, n, v))
        small = bool(os.environ.get("FUZZ_SMALL"))
        nfun = 1 if small or os.environ.get("FUZZ_ONE_FUN") else r.randrange(2, 5)
        for f in range(nfun):
            self.vars = [n for _, n, _ in decls]
            params = ["a%d" % i for i in range(r.randrange(1, 4))]
            ptypes = [r.choice(TYPES)[0] for _ in params]
            self.vars += params
            body = []
            self.stmts = body
            for k in range(1 if small else r.randrange(2, 6)):
                v = "t%d" % k
                body.append("  %s %s = (%s)(%s);" % (r.choice(TYPES)[0], v, "uint64_t", self.expr(r.randrange(1, 3) if small else r.randrange(1, 4))))
                self.vars.append(v)
            if not small and not os.environ.get("FUZZ_NO_LOOP") and r.random() < 0.6:
                n = r.randrange(2, 7)
                acc = self.vars[-1]
                self.loop = True
                inner = []
                self.stmts = inner
                last = self.expr(2)
                self.stmts = body
                self.loop = False
                body.append("  for (int i = 0; i < %d; ++i) {\n  %s\n    %s = (uint64_t)(%s);\n  }" %
                            (n, "\n  ".join(inner), acc, last))
            if not small and not os.environ.get("FUZZ_NO_GWRITE"):
                body.append("  g%d = (%s)(%s);" % (r.randrange(nvars), decls[0][0], self.expr(2)))
            body.append("  return (uint64_t)(%s);" % self.expr(1 if small else 3))
            out.append("static __attribute__((noinline)) uint64_t f%d(%s) {\n%s\n}" % (
                f, ", ".join("%s %s" % (t, p) for t, p in zip(ptypes, params)), "\n".join(body)))
            self.nparams = getattr(self, "nparams", [])
            self.nparams.append(ptypes)
        out.append("int main(void) {")
        for it in range(2 if small else r.randrange(4, 12)):
            f = r.randrange(nfun)
            args = ", ".join("(%s)%du" % (t, r.randrange(0, 1 << r.choice([6, 20, 31]))) for t in self.nparams[f])
            out.append("  mix(f%d(%s));" % (f, args))
        for _, n, _ in decls:
            out.append("  mix((uint64_t)%s);" % n)
        out.append('  printf("%llu\\n", (unsigned long long)sink);\n  return 0;\n}')
        return "\n".join(out)


def run(cmd, timeout=30):
    try:
        p = subprocess.run(cmd, capture_output=True, timeout=timeout, text=True)
        return p.returncode, p.stdout, p.stderr
    except subprocess.TimeoutExpired:
        return 124, "", "timeout"


def one(seed, nyc, outdir):
    d = os.path.join(outdir, "s%d" % seed)
    os.makedirs(d, exist_ok=True)
    src = os.path.join(d, "p.c")
    with open(src, "w") as f:
        f.write(Gen(seed).program() + "\n")
    rc, want, err = run(["gcc", "-w", "-O0", "-o", d + "/ref", src])
    if rc:
        return seed, "invalid", err[:200]
    rc, want, _ = run([d + "/ref"])
    if rc:
        return seed, "invalid", "reference exits %d" % rc
    for opt in ("-O0", "-O2"):
        obj = d + "/n%s.o" % opt
        rc, _, err = run([nyc, "-c", opt, src, "-o", obj])
        if rc:
            return seed, "compile-fail " + opt, err.strip()[:160]
        rc, _, err = run(["gcc", "-o", d + "/n" + opt, obj])
        if rc:
            return seed, "link-fail " + opt, err[:160]
        rc, got, _ = run([d + "/n" + opt])
        if rc or got != want:
            return seed, "MISCOMPILE " + opt, "want %s got %s rc=%d" % (want.strip(), got.strip(), rc)
    subprocess.run(["rm", "-rf", d])
    return seed, "ok", ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("nyc"); ap.add_argument("outdir")
    ap.add_argument("--seeds", default="0:200"); ap.add_argument("--jobs", type=int, default=16)
    a = ap.parse_args()
    lo, hi = map(int, a.seeds.split(":"))
    os.makedirs(a.outdir, exist_ok=True)
    bad = 0
    with ThreadPoolExecutor(a.jobs) as ex:
        for seed, status, detail in ex.map(lambda s: one(s, a.nyc, a.outdir), range(lo, hi)):
            if status != "ok":
                bad += 1
                print("seed %d: %s %s" % (seed, status, detail))
    print("%d seeds, %d failing" % (hi - lo, bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
