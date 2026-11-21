<!-- nytrix-doc: {"audience":"user","featured":false,"group":"learn","order":450,"summary":"Create a package manifest, add dependencies, and reproduce installs from the lockfile."} -->
# Packages

Nytrix packages are source dependencies resolved to an import root. The package
manager records direct dependencies in `ny.pkg.json` and writes resolved state
to `ny.pkg.json.lock`.

Prerequisites: `ny` on `PATH`; dependency resolution may require an explicitly
configured repository or reachable source.

## Repository trust

Nytrix ships no default package repository or public-registry fallback. Add
repositories you trust, or supply a dependency's source explicitly. Package
search is a convenience for discovering source packages in those configured
repositories and registry entries, not a centrally curated package catalog.

The package manager is not a dependency-injection framework. It discovers,
installs, and records source dependencies; choosing a repository does not make
its packages safe. Review the source and resolved revisions you install.
Content hashes identify bytes, not the publisher's identity or trustworthiness.

Repository lookup reads project and user configuration, including paths selected
by environment variables such as `NYTRIX_PKG_REGISTRY`. Inspect the active sources
before resolving a name:

```bash
ny pkg registry
ny pkg repo list
```

## Start a project

Use `ny new <dir>` for an application scaffold. In an existing project:

```bash
ny pkg init myapp
ny pkg add helper ./deps/helper
ny pkg list
ny pkg path
```

`ny pkg add` accepts a package name and source. `ny get <name> [source]` is an
alias; omitting the source requires a registry or repository entry. See
`ny pkg --help` for install roots and options.
When a lockfile exists, adding a new dependency extends it while retaining the
revisions already recorded. `ny pkg update` explicitly resolves the complete
dependency graph again.

## Manifest

A manifest contains package metadata and direct dependencies:

```json
{
  "schema": "ny.pkg.v1",
  "name": "myapp",
  "version": "0.1.0",
  "dependencies": {
    "helper": {"source": "./deps/helper"}
  }
}
```

The package name is the import name:

> **Note:** This import line is shown with a dependency named `helper`; create
> or install that package before compiling it.

```ny
use helper
```

Dependency sources can be local paths, Git URLs, or supported tar/zip
archives. Use `--ref` to select a Git ref. The package manager reports the
accepted source forms and install modes in `ny pkg --help`.

## Reproduce an install

Commit the lockfile for an application when repeatable dependency resolution
matters. `ny pkg sync` installs manifest dependencies; `ny pkg sync --offline`
uses the lockfile and local package cache without contacting registries. A
normal `ny pkg sync` also uses recorded commits and archive hashes when a
lockfile exists. Run `ny pkg update` to resolve new revisions. Archived
packages are stored as original archive bytes and checked against the lockfile
SHA-256 before extraction, including offline installs. A missing cached package
is an error in offline mode. Local path dependencies must remain available on
disk.

## Repositories and search

To search a local repository you own and trust:

```bash
ny pkg repo add local ./packages
ny pkg repo list
ny pkg repo sync
ny pkg search helper
ny get helper
```

`./packages` must contain the packages to discover. An unconfigured package
name is not looked up in an implicit public repository. Use
`ny pkg search --interactive query` for the built-in picker, and
`ny pkg repo rm local` to remove this discovery source.

## Diagnose an import

- Check the package name in the `use` statement against the manifest key.
- Run `ny pkg path` to see the active install root.
- Inspect the lockfile for the resolved source and ref.
- For offline installs, verify the exact package content is cached.

See [Programs](programs.md) for module structure and
[Troubleshooting](troubleshooting.md) for import errors.

Next:

- [Programs](programs.md) — local module imports.
- [Tooling](tooling.md#find-api-documentation) — inspect module APIs.
- [Troubleshooting](troubleshooting.md) — diagnose import errors.
