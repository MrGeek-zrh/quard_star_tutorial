SHELL_FOLDER=$(cd "$(dirname "$0")";pwd)
QEMU_BIN=$SHELL_FOLDER/qemu-6.0.0/build/qemu-system-riscv64
# QEMU_BIN=$SHELL_FOLDER/output/qemu/bin/qemu-system-riscv64

$QEMU_BIN \
-M quard-star \
-m 1G \
-smp 8 \
-drive if=pflash,bus=0,unit=0,format=raw,file=$SHELL_FOLDER/output/lowlevelboot/fw.bin \
-nographic --parallel none