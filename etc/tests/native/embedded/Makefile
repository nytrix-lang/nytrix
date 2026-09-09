# Embeddable Nytrix demo: compile app.ny to a native object and link it
# into a plain C host. Shape mirrors etc/tests/native/optcheck/driver.
NY      ?= $(abspath ../../../..)/build/release/ny
# libnytrixrt.so carries undefined libclang symbols (C-frontend bridge).
# Point CLANG_LIB at your libclang to resolve them eagerly; the default
# relies on lazy binding at runtime.
CLANG_LIB ?= -lclang
RT_DIR  ?= $(abspath ../../../..)/build/release
CC      ?= cc

all: host

app.o: app.ny
	$(NY) --native-backend x86_64 -emit-only -o $@ $<

host: host.c app.o
	$(CC) -O2 -o $@ host.c app.o -L$(RT_DIR) -lnytrixrt \
		-Wl,-rpath,$(RT_DIR) -Wl,--allow-shlib-undefined \
		$(CLANG_LIB)

run: host
	./host

clean:
	rm -f app.o host

.PHONY: all run clean
