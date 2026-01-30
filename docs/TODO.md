
# TODO Pending Fixes and Improvements

* [ ] External bindings and FFI polish with expanded stdlib test coverage and automatic C wrapper generation.
* [ ] Allow -l -L linker like flags and export to not need to ffi.
* [ ] Add proper type management with minimal quirks. and more generics functions.
* [ ] Allow  variables and functions starting with numbers,kebabcase and `->` and `!?`.
* [ ] Remove rt_ functions and reimplement in std.
* [ ] Implement int/float unbounded. for default operations when the module is imported.
* [ ] Start Forcing use and removing prelude by default.
* [ ] Implement auto resize in runtime based in term size for examples.
* [ ] When calling a function not imported should suggest the library from where it should be imported.
* [ ] Expose internal __ functions through docstring std wrappers to remove desc from defs.c .
* [ ] Use well mut/def.
* [ ] wrap all __ to non __ functions with docstring and in all code use the wrapped ones.
