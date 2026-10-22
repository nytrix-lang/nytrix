<!-- nytrix-doc: {"audience":"user","featured":false,"group":"learn","order":130,"summary":"Create a package manifest, add dependencies, and reproduce installs from the lockfile."} -->
# Packages

Nytrix packages are source dependencies resolved to an import root. The package
manager records direct dependencies in `ny.pkg.json` and writes resolved state
to `ny.pkg.json.lock`.

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

```text
use helper
```

Dependency sources can be local paths, Git URLs, or supported tar/zip
archives. Use `--ref` to select a Git ref. The package manager reports the
accepted source forms and install modes in `ny pkg --help`.

## Reproduce an install

Commit the lockfile for an application when repeatable dependency resolution
matters. `ny pkg sync` installs manifest dependencies; `ny pkg sync --offline`
uses the lockfile and local package cache without contacting registries. A
missing cached package is an error in offline mode. Local path dependencies
must remain available on disk.

Content hashes identify the bytes that were installed; they do not prove who
published them. Treat package sources and registry configuration as trusted
inputs.

## Repositories and search

```bash
ny pkg search query
ny pkg repo add local ./packages
ny pkg repo list
ny pkg repo sync
ny get helper
```

Use `ny pkg search --interactive query` for the built-in picker. `ny pkg --help`
lists repository commands and registry configuration.

## Diagnose an import

- Check the package name in the `use` statement against the manifest key.
- Run `ny pkg path` to see the active install root.
- Inspect the lockfile for the resolved source and ref.
- For offline installs, verify the exact package content is cached.

See [Programs](programs.md) for module structure and
[Troubleshooting](troubleshooting.md) for import errors.
