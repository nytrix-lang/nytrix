<!-- nytrix-doc: {"audience":"user","featured":true,"group":"learn","order":10,"summary":"Compile and run a first Nytrix file, then find the next guide for your task."} -->
# Start

Use a file to check the compiler before setting up a larger project. If you
already have Nytrix installed, confirm which executable is on your path:

```bash
ny --version
```

## Run a program

Create `hello.ny`:

```ny
use core

fn greet(str name) str {
   "hello, " + name
}

assert_eq(greet("Nytrix"), "hello, Nytrix", "greeting")
print(greet("world"))
```

Run it:

```bash
ny hello.ny
```

The assertion is silent when it passes; the final line prints `hello, world`.

## Continue

- Use [`ny new`](programs.md) or `ny new app` to start a project.
- Read the [tour](tour.md) for the basic language forms.
- Search standard-library APIs with `ny doc search --symbols name`; see
  [Library](library.md).
- Use [Programs](programs.md) for imports and module entry points, and
  [Build](build.md) for repository builds.
- Check [Implementation status](status.md) before relying on a target-specific
  feature.
