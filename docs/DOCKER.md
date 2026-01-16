
Only linux is supported for now

And is tested on arch.

```bash
docker run --rm -it --network host -v "$PWD":/work -w /work archlinux /bin/bash
```

```bash
pacman -Syu --noconfirm base-devel python3 clang llvm
make
```
