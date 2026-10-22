<!-- nytrix-doc: {"audience":"user","featured":false,"group":"learn","order":160,"summary":"Use the REPL for interactive experiments and move reproducible checks into files."} -->
# REPL

The REPL evaluates source interactively and retains imports and definitions
between prompts. Use a source file when you need a reproducible bug report, test,
or benchmark.

## Start

```bash
ny
ny -i
ny --plain-repl
ny -ic 'value = 1337'
ny --repl < file.ny
```

`--repl` reads stdin as source once; it is not an interactive prompt. Run `ny
--help` for the installed options.

## Work interactively

Type expressions and declarations at the prompt. Use `ny doc search` to find a
symbol or concept. Session state can make a snippet behave differently from a
fresh file, so move surprising cases into a file and run them directly.

| Task | Command or action |
| --- | --- |
| Check exported names | `ny doc search --symbols name` |
| Find docs for a topic | `ny doc search --docs topic` |
| Reproduce a source failure | Save the snippet, then run `ny file.ny` |
| Validate formatting | `ny fmt --check file.ny` |

Interactive prompt commands are not source syntax. See the prompt's `:help`
for supported commands.

## Snapshots

Use the REPL snapshot command only for saving a session that must be restored
later. For application source or a regression, prefer a `.ny` file and an
executable fixture; files keep imports, setup, and assertions visible.

See [Start](start.md), [Programs](programs.md), and [Testing](testing.md).
