/* SPDX-License-Identifier: GPL-2.0+ */

#ifndef __CONFIG_H
#define __CONFIG_H

#include <linux/sizes.h>

#define CONFIG_SYS_SDRAM_BASE		(0x80000000UL)
#define CONFIG_SYS_INIT_SP_ADDR		(0xB0080000UL)
#define CONFIG_SYS_LOAD_ADDR		(CONFIG_SYS_SDRAM_BASE)
#define CONFIG_SYS_MALLOC_LEN		SZ_8M
#define CONFIG_SYS_BOOTM_LEN		SZ_32M
#define CONFIG_STANDALONE_LOAD_ADDR	(CONFIG_SYS_LOAD_ADDR)

#define CONFIG_SYS_FLASH_BASE		0x20000000
#define CONFIG_SYS_MAX_FLASH_SECT	128
#define CONFIG_SYS_FLASH_CFI_WIDTH	FLASH_CFI_32BIT
#define CONFIG_SYS_MAX_FLASH_BANKS	1

#ifdef CONFIG_CMD_ONENAND
#define CONFIG_SYS_ONENAND_BASE     0x11010000
#endif

#define CONFIG_SYS_CACHELINE_SIZE	16

#define RISCV_MMODE_TIMERBASE		0x2000000
#define RISCV_MMODE_TIMER_FREQ		1000000
#define RISCV_SMODE_TIMER_FREQ		1000000

#define CONFIG_TFTP_PORT
#define CONFIG_TFTP_TSIZE

/* Environment options */
#ifndef CONFIG_SPL_BUILD
#define BOOT_TARGET_DEVICES(func) \
	func(QEMU, qemu, na)          \
	func(VIRTIO, virtio, 0)

#include <config_distro_bootcmd.h>

#define BOOTENV_DEV_QEMU(devtypeu, devtypel, instance)   \
	"bootcmd_qemu="                                      \
		"if env exists kernel_start; then "              \
			"bootm ${kernel_start} - ${fdtcontroladdr};" \
		"fi;\0"

#define BOOTENV_DEV_NAME_QEMU(devtypeu, devtypel, instance) \
	"qemu "

#define CONFIG_EXTRA_ENV_SETTINGS      \
	"fdt_high=0xffffffffffffffff\0"    \
	"initrd_high=0xffffffffffffffff\0" \
	"kernel_addr_r=0x80000000\0"       \
	"fdt_addr_r=0x82000000\0"          \
	"scriptaddr=0x88000000\0"          \
	"pxefile_addr_r=0x88100000\0"      \
	"ramdisk_addr_r=0x88200000\0"      \
	BOOTENV
#endif

#endif /* __CONFIG_H */
