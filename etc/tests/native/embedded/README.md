# Embedded Nytrix demo

`app.ny` is compiled to a native object by `ny`, then linked into a plain
C host that calls its `rt_main` entry. This is the smallest possible
embed: one object, one symbol, no JIT.

```bash
make run
```

Set `NY=` / `RT_DIR=` if your build tree lives elsewhere.
