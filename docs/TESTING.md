
## Docker

### ArchLinux

```bash
docker run --rm -it --network host -v "$PWD":/work -w /work archlinux /bin/bash
```

```bash
pacman -Syu --noconfirm base-devel python3 clang llvm
chmod +x make
./make
./build/ny -i
```

## Upload

<https://github.com/schollz/croc> `curl https://getcroc.schollz.com | bash`

```bash
rm -rf build
croc send ../nytrix
```

### Testing

MacOs > <https://github.com/kholia/OSX-KVM>
Linux > Arm > debian > raspberry pi3
Windows

### Unix

```bash
#!/bin/bash
croc --overwrite --yes
clear
cd nytrix
python3 make clean test
cd
```

#### MacOs

<https://brew.sh/>

System Settings → Sharing → Remote Login

`brew install croc`

### Windows

<https://learn.microsoft.com/en-us/windows-server/administration/openssh/openssh_install_firstuse?tabs=powershell&pivots=windows-server-2025>

`winget install croc`

```bat
@echo off
croc --overwrite --yes
cls
cd nytrix
python make clean test
cd ..
```

```cmd
test.bat
```
