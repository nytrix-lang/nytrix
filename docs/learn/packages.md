<!-- nytrix-doc: {"audience":"user","featured":false,"group":"learn","order":130,"summary":"Organize reusable code into packages and keep project boundaries clear."} -->
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

```ny
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
repo core = git+https://github.com/owner/ny-packages.git
```

Registry lookup checks local registry files, configured registry environment,
and the user registry.

The shared Nytrix config files can also hold package repository entries:

```text
~/.config/nytrix/config
./.nytrix/config
```

Use the same line form:

```text
repo core = git+https://github.com/owner/ny-packages.git
NYTRIX_PKG_HOME=~/.local/share/nytrix/pkg
NYTRIX_PKG_PATH=./ny_modules:./vendor/ny_modules
```

Environment variables override config defaults. Repository lines are read by
`ny pkg repo list`, `ny pkg search`, and `ny get`.

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
ny pkg repo add core git+https://github.com/owner/ny-packages.git
ny pkg repo list
ny pkg repo sync
ny pkg repo path local
ny pkg repo remove local
ny get bigint
ny pkg search [--interactive] query
```

`repo list` prints the configured repository name, source URL/path, and local
cache path. Repository sources can be local paths, `git+https://...`, SSH Git
URLs, or archives. Config-file repository entries are listed with registry
entries.

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

`ny.pkg.json.lock` uses schema `ny.pkg.lock.v2`. It records every package
reachable from the root manifest, parent→child dependency edges, installed
paths, and immutable source identity. Git dependencies retain the resolved
commit. Archive dependencies retain a SHA-256 digest computed from the archive
bytes before extraction. Relative local dependencies inside a package are
resolved relative to that package's own manifest, not the root project.

Resolution is intentionally deterministic rather than version-solving: one
package name identifies one source/ref pair for the complete graph. If two
parents request the same package name from different sources or refs,
installation fails with a conflict instead of silently choosing traversal
order. Identical requests are deduplicated while every parent→child edge is
retained. A package is reserved in the graph before descending into its
manifest, so dependency cycles terminate; a depth bound remains as defense in
depth for malformed graphs. Nested install failures propagate to the root and
do not produce a successful lockfile.

Commit the lockfile for applications when reproducibility matters. The lock is
a record of the complete resolved graph. Successful online installs snapshot
immutable git/archive package contents into the package content cache. Run
`ny pkg sync --offline` (or another package install command with `--offline`)
to replay strictly from that lock plus local package content: no registry or
network lookup is attempted, source/ref mismatches fail, and a missing cached
package is an error instead of silently falling back online. Local path
dependencies remain path-backed and therefore must still be present locally.

### Integrity and trust

Content identity and publisher authenticity are separate. Git commits and
archive SHA-256 digests detect content changes after an identity is trusted,
but a digest learned from an untrusted registry is not authentication. Local
paths are trusted as local filesystem input, and Git authenticity follows the
configured transport/repository trust. A security-sensitive public registry
would need authenticated or signed metadata that binds package name, immutable
content identity, and release metadata.

## Common package failures

| Symptom | Check |
| --- | --- |
| Import not found | Run `ny pkg path` and confirm the package exists under the selected root. |
| Wrong source version | Inspect `ny.pkg.json.lock` for the resolved git ref or local path. |
| Package name mismatch | Confirm the dependency key matches the import name. |
| Repo package not found | Run `ny pkg repo list` and `ny pkg repo sync <repo>`. |

See [Tooling](tooling.md) for package command families and
[Troubleshooting](troubleshooting.md) for import failures.