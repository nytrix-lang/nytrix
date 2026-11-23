#!/bin/bash
# Build Linux with NYC (strict NYC -S + GNU as wrapper), boot the bzImage in QEMU with a
# freestanding initramfs /init, and require NYC_KERNEL_INIT_REACHED on the serial console.
# usage: kernel-boot-smoke.sh LINUX_SRC OUT_DIR NYC_BINARY   (run from the repository root)
set -e
SRC=$(realpath "$1"); OUT=$(realpath -m "$2"); NYC=$(realpath "$3")
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$OUT/build"
cp "$NYC" "$OUT/nyc"
cp "$HERE/nyc-kbuild-strict-cc.sh" "$OUT/cc.sh"; chmod +x "$OUT/cc.sh"
export NYC_BIN="$OUT/nyc" NYC_KERNEL_COMPAT=1 KCFLAGS=-O2
M=(make -C "$SRC" O="$OUT/build" ARCH=x86_64 CC="$OUT/cc.sh" HOSTCC=gcc AS=as LD=ld)
"${M[@]}" x86_64_defconfig >"$OUT/config.log" 2>&1
"${M[@]}" olddefconfig >>"$OUT/config.log" 2>&1
"${M[@]}" V=1 -j"${JOBS:-32}" -k bzImage modules >"$OUT/build.log" 2>&1
python3 "$HERE/kernel-provenance.py" "$OUT/build.log" "$OUT/build" "$OUT/nyc" "$OUT/provenance.json"
cat > "$OUT/init.c" <<'EOC'
static long sc(long n,long a,long b,long c){long r;__asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a),"S"(b),"d"(c):"rcx","r11","memory");return r;}
void _start(void){const char m[]="NYC_KERNEL_INIT_REACHED\n";sc(1,1,(long)m,sizeof m-1);sc(169,0xfee1dead,0x28121969,0x4321fedc);for(;;);}
EOC
gcc -static -nostdlib -O1 -fno-pie -no-pie -fno-stack-protector -o "$OUT/init" "$OUT/init.c"
python3 - "$OUT" <<'EOP'
import gzip,stat,sys
o=sys.argv[1]
def ent(name,mode,data=b'',rdev=(0,0),ino=[1]):
    ino[0]+=1; n=name.encode()+b'\0'
    h='070701'+''.join('%08X'%x for x in [ino[0],mode,0,0,1,0,len(data),0,0,rdev[0],rdev[1],len(n),0])
    b=h.encode()+n; b+=b'\0'*((4-len(b)%4)%4); b+=data+b'\0'*((4-len(data)%4)%4); return b
init=open(o+'/init','rb').read()
out=ent('dev',stat.S_IFDIR|0o755)+ent('dev/console',stat.S_IFCHR|0o600,rdev=(5,1))+ent('init',stat.S_IFREG|0o755,init)+ent('TRAILER!!!',0)
open(o+'/initramfs.gz','wb').write(gzip.compress(out))
EOP
cat > "$OUT/init_glibc.c" <<'EOC'
#include <stdio.h>
#include <unistd.h>
#include <sys/reboot.h>
int main(void){ puts("NYC_KERNEL_INIT_REACHED"); fflush(stdout); sync(); reboot(RB_POWER_OFF); for(;;) pause(); }
EOC
gcc -static -O2 -o "$OUT/init_glibc" "$OUT/init_glibc.c"
python3 - "$OUT" <<'EOP'
import gzip,stat,sys
o=sys.argv[1]
def ent(name,mode,data=b'',rdev=(0,0),ino=[1]):
    ino[0]+=1; n=name.encode()+b'\0'
    h='070701'+''.join('%08X'%x for x in [ino[0],mode,0,0,1,0,len(data),0,0,rdev[0],rdev[1],len(n),0])
    b=h.encode()+n; b+=b'\0'*((4-len(b)%4)%4); b+=data+b'\0'*((4-len(data)%4)%4); return b
init=open(o+'/init_glibc','rb').read()
out=ent('dev',stat.S_IFDIR|0o755)+ent('dev/console',stat.S_IFCHR|0o600,rdev=(5,1))+ent('init',stat.S_IFREG|0o755,init)+ent('TRAILER!!!',0)
open(o+'/initramfs_glibc.gz','wb').write(gzip.compress(out))
EOP
timeout "${BOOT_TIMEOUT:-90}" qemu-system-x86_64 -m 512 -smp 1 -display none -serial file:"$OUT/serial_glibc.log" -no-reboot -nic none \
  -kernel "$OUT/build/arch/x86/boot/bzImage" -initrd "$OUT/initramfs_glibc.gz" \
  -append "console=ttyS0 loglevel=7 panic=-1 rdinit=/init vga=normal nokaslr" -accel "${ACCEL:-kvm}" -cpu "${CPU:-host}" >/dev/null 2>&1 || true
grep -q NYC_KERNEL_INIT_REACHED "$OUT/serial_glibc.log" || { echo "FAIL: static glibc /init marker missing"; tail -n 20 "$OUT/serial_glibc.log"; exit 1; }
echo "PASS: static glibc /init"
timeout "${BOOT_TIMEOUT:-90}" qemu-system-x86_64 -m 512 -smp 1 -display none -serial file:"$OUT/serial.log" -no-reboot -nic none \
  -kernel "$OUT/build/arch/x86/boot/bzImage" -initrd "$OUT/initramfs.gz" \
  -append "console=ttyS0 loglevel=7 panic=-1 rdinit=/init vga=normal nokaslr" -accel "${ACCEL:-kvm}" -cpu "${CPU:-host}" >/dev/null 2>&1 || true
grep -q NYC_KERNEL_INIT_REACHED "$OUT/serial.log" && echo "PASS: kernel reached /init" || { echo "FAIL: /init marker missing"; tail -n 20 "$OUT/serial.log"; exit 1; }
