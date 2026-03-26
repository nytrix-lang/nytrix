# Packages

Nytrix packages are source packages. The package manager records direct
dependencies, installs source into an import root, and writes a lockfile with
installed state.

The resolver imports package names the same way it imports modules. The package
manager only decides where source lives and which version or local path was
installed.

## Commands

| Command | Behavior |
| --- | --- |
| `ny new myapp` | Create a project scaffold. |
| `ny pkg init name` | Create only `ny.pkg.json`. |
| `ny pkg info` | Print package metadata and installed paths. |
| `ny pkg add name source` | Add and install a direct dependency. |
| `ny get name` | Resolve from registry/repository and install. |
| `ny pkg search query` | Fuzzy-search registry entries and registered package repositories. |
| `ny pkg search --interactive query` | Open the built-in fuzzy package picker. |
| `ny pkg repo add name source` | Register a package repository for name lookup. |
| `ny pkg uninstall name` | Remove a local dependency. |
| `ny pkg path` | Print the selected install root. |

## Project layout

```text
myapp/
  ny.pkg.json
  src/main.ny
  .gitignore
```

After dependency installation:

```text
myapp/
  ny.pkg.json
  ny.pkg.json.lock
  ny_modules/
```

Imports use package names:

```text
use package_name
```

## Manifest

`ny.pkg.json` stores metadata and direct dependencies.

```json
{
  "schema": "ny.pkg.v1",
  "name": "myapp",
  "version": "0.1",
  "description": "small tool",
  "author": "Name <mail@example>",
  "license": "MIT",
  "repository": "https://example.com/myapp.git",
  "dependencies": {
    "foo": {"source": "./deps/foo"},
    "bar": {"source": "git+https://example.com/bar.git", "ref": "main"}
  }
}
```

## Sources

| Source | Form |
| --- | --- |
| Local folder | `./deps/foo` |
| Local file | `./one.ny` |
| Git HTTPS | `git+https://host/bar.git#main` |
| Git SSH | `git@host:baz.git --ref v1.2.0` |
| Git URL | any supported `.git` URL |
| Archive | `./arcfoo.tgz`, `./arcfoo.zip` |
| Explicit archive | `archive+./arcfoo.zip` |
| Repository package | `repo+repo_name/package_name` |

`#ref` or `--ref` pins a git ref.

## Registry

Registry entries map names to sources:

```text
foo = ./deps/foo
bar = git+https://example.com/bar.git#main
```

Registry lookup checks local registry files, configured registry environment,
and the user registry.

## Package repositories

A package repository root contains package directories:

```text
packages/
  bigint/mod.ny
  crypto_extra/mod.ny
```

Repository commands:

```bash
ny pkg repo add local ./packages
ny pkg repo list
ny pkg repo sync
ny pkg repo path local
ny pkg repo remove local
ny get bigint
ny pkg search [--interactive] query
```

`ny get <name>` checks direct registry entries first, then registered
repositories for `<name>/mod.ny` or `<name>.ny`.

`ny pkg search <query>` scans the same registry and repository sources used by
`ny get`. Results include package name, source, repository, version, and
description when a package manifest is present.

Use `--interactive` for the built-in fuzzy picker. The picker is implemented
inside `ny`; it does not shell out to external `fzf`.

## Install roots

| Mode | Root |
| --- | --- |
| default | `./ny_modules` |
| `--vendor` | `./vendor/ny_modules` |
| `--venv` | `./.nytrix/venv/lib` |
| `--global` | user package home |
| `--system` | system package home |
| `--root path` | custom root |

Resolver order includes local modules, vendored modules, venv modules,
configured package path, user package home, and system package home.

## Lockfile

`ny.pkg.json.lock` records installed paths, git refs, source metadata, and
package state. It records installed direct state; it is not a full dependency
solver.

Commit the lockfile for applications when reproducibility matters. Libraries
can keep the manifest broad and document the tested dependency refs in release
notes or examples.

## Common package failures

| Symptom | Check |
| --- | --- |
| Import not found | Run `ny pkg path` and confirm the package exists under the selected root. |
| Wrong source version | Inspect `ny.pkg.json.lock` for the resolved git ref or local path. |
| Package name mismatch | Confirm the dependency key matches the import name. |
| Repo package not found | Run `ny pkg repo list` and `ny pkg repo sync <repo>`. |

See [tooling.md](tooling.md) for package command families and
[troubleshooting.md](troubleshooting.md) for import failures.
