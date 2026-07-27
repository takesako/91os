#!/bin/bash
set -xue
xxd -i gengo.js > gengo.h
xxd -i shell.gengo > shell.h
QEMU=qemu-system-riscv64
CC=/opt/homebrew/opt/llvm/bin/clang
CFLAGS='-std=c11 -O2 -g3 -Wall -Wextra --target=riscv64-unknown-elf -march=rv64imafd -mabi=lp64d -mcmodel=medany -fuse-ld=lld -fno-stack-protector -ffreestanding -nostdlib'
$CC $CFLAGS -Wl,-Tboot.ld -Wl,-Map=os.map -o os.elf kernel.c libc.c miniregex.c elk.c main.c
$QEMU -machine virt -bios default -nographic -serial mon:stdio --no-reboot -kernel os.elf
