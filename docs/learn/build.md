<!-- nytrix-doc: {"audience":"user","featured":true,"group":"learn","order":195,"summary":"Build, install, and diagnose Nytrix with the repository build driver."} -->
# Build Nytrix

Use the repository's `./make` driver for normal builds. It owns build
configuration, generated artifacts, and tool dependencies.

## Common commands

```bash
./make --help
./make all
./make bin
./make env
./make doctor
./make targets
./make install
./make docs
```

`all` builds the compiler, standard library, and tools; `bin` builds the Ny
executable. `env`, `doctor`, and `targets` report the active toolchain,
dependencies, and cross-target guidance. Use `./make help <command>` for the
options of a specific command.

## Build configuration

Keep separate build directories for different compilers, targets, or
instrumentation settings. Use the driver unless you need a CMake option that it
does not expose; in that case, record the exact configure arguments and build
directory so the result can be reproduced. Do not reuse a build tree after
changing toolchain or ABI settings without reconfiguring it.

Build output belongs under `build/`. Do not edit generated binaries, object
files, or cached compiler artifacts as source.

## Install and verify

Use the install command for a local installation and confirm the installed
compiler can find its core library:

```bash
./make install
ny --version
ny -c 'use core
print(1 + 1)'
```

For prefix selection, uninstall behavior, or cross-compilation options, check
`./make help install` and `./make help cross` rather than copying a command from
a different build configuration.

## Common failures

| Failure | Check |
| --- | --- |
| Missing compiler or linker | Run `./make doctor`, install the reported host development tools, and rebuild. |
| Wrong binary after a source change | Run `./make bin` from the repository root; use `./make env` to check `PATH`. |
| Target mismatch | Check `./make targets` and the native target matrix before changing ABI flags. |
| Unexpected cached result | Use the documented cache controls for the failing path; preserve unrelated build data. |

See [Tooling](tooling.md) for format/test commands and
[Troubleshooting](troubleshooting.md) for source and runtime diagnostics.
