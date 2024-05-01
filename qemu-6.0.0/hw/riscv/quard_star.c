/*
 * QEMU RISC-V Quard Star Board
 *
 * Copyright (c) 2021 qiao qiming
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/boards.h"
#include "hw/loader.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "hw/char/serial.h"
#include "target/riscv/cpu.h"
#include "hw/riscv/riscv_hart.h"
#include "hw/riscv/quard_star.h"
#include "hw/riscv/boot.h"
#include "hw/riscv/numa.h"
#include "hw/intc/sifive_clint.h"
#include "hw/intc/sifive_plic.h"
#include "chardev/char.h"
#include "sysemu/arch_init.h"
#include "sysemu/device_tree.h"
#include "sysemu/sysemu.h"

static const MemMapEntry virt_memmap[] = {
    [QUARD_STAR_MROM]  = {        0x0,        0x8000 },
    [QUARD_STAR_SRAM]  = {     0x8000,        0x8000 },
    [QUARD_STAR_CLINT] = {  0x2000000,       0x10000 },
    [QUARD_STAR_PLIC]  = {  0xc000000, QUARD_STAR_PLIC_SIZE(QUARD_STAR_CPUS_MAX * 2) },
    [QUARD_STAR_UART0] = { 0x10000000,         0x100 },
    [QUARD_STAR_UART1] = { 0x10001000,         0x100 },
    [QUARD_STAR_UART2] = { 0x10002000,         0x100 },
    [QUARD_STAR_FLASH] = { 0x20000000,     0x2000000 },
    [QUARD_STAR_DRAM]  = { 0x80000000,           0x0 },
};

#define QUARD_STAR_FLASH_SECTOR_SIZE (256 * KiB)

static PFlashCFI01 *quard_star_flash_create(RISCVVirtState *s,
                                       const char *name,
                                       const char *alias_prop_name)
{
    DeviceState *dev = qdev_new(TYPE_PFLASH_CFI01);

    qdev_prop_set_uint64(dev, "sector-length", QUARD_STAR_FLASH_SECTOR_SIZE);
    qdev_prop_set_uint8(dev, "width", 4);
    qdev_prop_set_uint8(dev, "device-width", 2);
    qdev_prop_set_bit(dev, "big-endian", false);
    qdev_prop_set_uint16(dev, "id0", 0x89);
    qdev_prop_set_uint16(dev, "id1", 0x18);
    qdev_prop_set_uint16(dev, "id2", 0x00);
    qdev_prop_set_uint16(dev, "id3", 0x00);
    qdev_prop_set_string(dev, "name", name);

    object_property_add_child(OBJECT(s), name, OBJECT(dev));
    object_property_add_alias(OBJECT(s), alias_prop_name,
                              OBJECT(dev), "drive");

    return PFLASH_CFI01(dev);
}

static void quard_star_flash_map(PFlashCFI01 *flash,
                            hwaddr base, hwaddr size,
                            MemoryRegion *sysmem)
{
    DeviceState *dev = DEVICE(flash);

    assert(QEMU_IS_ALIGNED(size, QUARD_STAR_FLASH_SECTOR_SIZE));
    assert(size / QUARD_STAR_FLASH_SECTOR_SIZE <= UINT32_MAX);
    qdev_prop_set_uint32(dev, "num-blocks", size / QUARD_STAR_FLASH_SECTOR_SIZE);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);

    memory_region_add_subregion(sysmem, base,
                                sysbus_mmio_get_region(SYS_BUS_DEVICE(dev),
                                                       0));
}

static void quard_star_setup_rom_reset_vec(MachineState *machine, RISCVHartArrayState *harts,
                               hwaddr start_addr,
                               hwaddr rom_base, hwaddr rom_size,
                               uint64_t kernel_entry,
                               uint32_t fdt_load_addr)
{
    int i;
    uint32_t start_addr_hi32 = 0x00000000;

    if (!riscv_is_32bit(harts)) {
        start_addr_hi32 = start_addr >> 32;
    }
    /* reset vector */
    uint32_t reset_vec[10] = {
        0x00000297,                  /* 1:  auipc  t0, %pcrel_hi(fw_dyn) */
        0x02828613,                  /*     addi   a2, t0, %pcrel_lo(1b) */
        0xf1402573,                  /*     csrr   a0, mhartid  */
        0,
        0,
        0x00028067,                  /*     jr     t0 */
        start_addr,                  /* start: .dword */
        start_addr_hi32,
        fdt_load_addr,               /* fdt_laddr: .dword */
        0x00000000,
                                     /* fw_dyn: */
    };
    if (riscv_is_32bit(harts)) {
        reset_vec[3] = 0x0202a583;   /*     lw     a1, 32(t0) */
        reset_vec[4] = 0x0182a283;   /*     lw     t0, 24(t0) */
    } else {
        reset_vec[3] = 0x0202b583;   /*     ld     a1, 32(t0) */
        reset_vec[4] = 0x0182b283;   /*     ld     t0, 24(t0) */
    }

    /* copy in the reset vector in little_endian byte order */
    for (i = 0; i < ARRAY_SIZE(reset_vec); i++) {
        reset_vec[i] = cpu_to_le32(reset_vec[i]);
    }

    rom_add_blob_fixed_as("mrom.reset", reset_vec, sizeof(reset_vec),
                          rom_base, &address_space_memory);
}

static void quard_star_machine_init(MachineState *machine)
{
    const MemMapEntry *memmap = virt_memmap;  // 访问预定义的内存映射表，包括各个硬件设备的地址和大小。
    RISCVVirtState *s = RISCV_VIRT_MACHINE(machine);  // 将传入的 MachineState 强制转换为 RISCVVirtState 结构体。
    MemoryRegion *system_memory = get_system_memory();  // 获取系统的内存对象。
    
    // 初始化各种内存区域
    MemoryRegion *main_mem = g_new(MemoryRegion, 1);  // 为主内存分配内存区域结构体。
    MemoryRegion *sram_mem = g_new(MemoryRegion, 1);  // 为 SRAM 分配内存区域结构体。
    MemoryRegion *mask_rom = g_new(MemoryRegion, 1);  // 为 ROM 分配内存区域结构体。

    int i, j, base_hartid, hart_count;  // 控制循环的变量以及hartid和数量。
    char *plic_hart_config, *soc_name;  // 字符串，用于配置PLIC和存储SoC名称。
    size_t plic_hart_config_len;  // PLIC配置字符串的长度。
    DeviceState *mmio_plic = NULL;  // 指向PLIC设备的指针，用于中断控制。

    // 检查系统支持的NUMA节点数量是否超过了预设的最大值。
    if (QUARD_STAR_SOCKETS_MAX < riscv_socket_count(machine)) {
        error_report("number of sockets/nodes should be less than %d",
            QUARD_STAR_SOCKETS_MAX);
        exit(1);
    }

    // 循环初始化每个NUMA节点。
    for (i = 0; i < riscv_socket_count(machine); i++) {
        // 确保NUMA节点中的hartid是连续的。
        if (!riscv_socket_check_hartids(machine, i)) {
            error_report("discontinuous hartids in socket%d", i);
            exit(1);
        }

        // 获取此NUMA节点的基础hartid。
        base_hartid = riscv_socket_first_hartid(machine, i);
        if (base_hartid < 0) {
            error_report("can't find hartid base for socket%d", i);
            exit(1);
        }

        // 获取此NUMA节点中的hart数量。
        hart_count = riscv_socket_hart_count(machine, i);
        if (hart_count < 0) {
            error_report("can't find hart count for socket%d", i);
            exit(1);
        }

        // 为每个NUMA节点创建对应的SoC实例，并初始化相关属性。
        soc_name = g_strdup_printf("soc%d", i);
        object_initialize_child(OBJECT(machine), soc_name, &s->soc[i],
                                TYPE_RISCV_HART_ARRAY);
        g_free(soc_name);
        object_property_set_str(OBJECT(&s->soc[i]), "cpu-type",
                                machine->cpu_type, &error_abort);
        object_property_set_int(OBJECT(&s->soc[i]), "hartid-base",
                                base_hartid, &error_abort);
        object_property_set_int(OBJECT(&s->soc[i]), "num-harts",
                                hart_count, &error_abort);
        sysbus_realize(SYS_BUS_DEVICE(&s->soc[i]), &error_abort);

        // 为每个NUMA节点创建一个 CLINT 设备。
        sifive_clint_create(
            memmap[QUARD_STAR_CLINT].base + i * memmap[QUARD_STAR_CLINT].size,
            memmap[QUARD_STAR_CLINT].size, base_hartid, hart_count,
            SIFIVE_SIP_BASE, SIFIVE_TIMECMP_BASE, SIFIVE_TIME_BASE,
            SIFIVE_CLINT_TIMEBASE_FREQ, true);

        // 这个配置字符串用于定义哪些处理器（Hart）可以接收到哪些中断。
        // 代码首先计算这个字符串需要的长度，并动态分配足够的内存。接着，它通过循环为每个处理器构造一个配置字符串，每个处理器配置以逗号分隔（除了第一个）。
        plic_hart_config_len =
            (strlen(QUARD_STAR_PLIC_HART_CONFIG) + 1) * hart_count;
        // 这个最终的效果应该是：MS,MS,.....
        plic_hart_config = g_malloc0(plic_hart_config_len);
        for (j = 0; j < hart_count; j++) {
            if (j != 0) {
                strncat(plic_hart_config, ",", plic_hart_config_len);
            }
            strncat(plic_hart_config, QUARD_STAR_PLIC_HART_CONFIG,
                plic_hart_config_len);
            plic_hart_config_len -= (strlen(QUARD_STAR_PLIC_HART_CONFIG) + 1);
        }

        // 为每个NUMA节点创建一个 PLIC 设备。
        s->plic[i] = sifive_plic_create(
            memmap[QUARD_STAR_PLIC].base + i * memmap[QUARD_STAR_PLIC].size,
            plic_hart_config, base_hartid,
            QUARD_STAR_PLIC_NUM_SOURCES,
            QUARD_STAR_PLIC_NUM_PRIORITIES,
            QUARD_STAR_PLIC_PRIORITY_BASE,
            QUARD_STAR_PLIC_PENDING_BASE,
            QUARD_STAR_PLIC_ENABLE_BASE,
            QUARD_STAR_PLIC_ENABLE_STRIDE,
            QUARD_STAR_PLIC_CONTEXT_BASE,
            QUARD_STAR_PLIC_CONTEXT_STRIDE,
            memmap[QUARD_STAR_PLIC].size);
        g_free(plic_hart_config);

        // 特殊处理第一个NUMA节点的PLIC，可能用于特殊的中断或控制路径。
        if (i == 0) {
            mmio_plic = s->plic[i];
        }
    }

    // 初始化和映射主内存、SRAM和ROM。
    memory_region_init_ram(main_mem, NULL, "riscv_quard_star_board.dram",
                           machine->ram_size, &error_fatal);
    memory_region_add_subregion(system_memory, memmap[QUARD_STAR_DRAM].base,
        main_mem);
    memory_region_init_ram(sram_mem, NULL, "riscv_quard_star_board.sram",
                           memmap[QUARD_STAR_SRAM].size, &error_fatal);
    memory_region_add_subregion(system_memory, memmap[QUARD_STAR_SRAM].base,
        sram_mem);
    memory_region_init_rom(mask_rom, NULL, "riscv_quard_star_board.mrom",
                           memmap[QUARD_STAR_MROM].size, &error_fatal);
    memory_region_add_subregion(system_memory, memmap[QUARD_STAR_MROM].base,
                                mask_rom);

    // 设置开机时的复位向量和内存配置。
    quard_star_setup_rom_reset_vec(machine, &s->soc[0], virt_memmap[QUARD_STAR_FLASH].base,
                              virt_memmap[QUARD_STAR_MROM].base,
                              virt_memmap[QUARD_STAR_MROM].size,
                              0x0, 0x0);

    // 配置UART设备，包括其中断连接。
    // GPIO通常用于设备之间进行简单的信号通信，如触发中断、控制信号或其他形式的简单数据传输.在 QEMU 的上下文中，GPIO的概念被用来模拟类似的行为，尤其是在设备模拟中实现中断信号的传递
    // qdev_get_gpio_in 函数获取了 QUARD_STAR_UART0_IRQ 号对应的 GPIO
    // 输入,这是获取指向中断控制器（PLIC）中的某个 GPIO
    // 输入的指针，以便将串口设备的中断连接到中断控制器。
    /**
        0 (regshift): regshift 参数用于定义访问串口寄存器时的地址偏移量。在许多硬件设计中，寄存器可能不是连续排列的，而是每隔几个字节放置一个。regshift 定义了每个寄存器之间的字节偏移。 在这个调用中，regshift 的值是 0，意味着寄存器是连续的，没有额外的字节偏移。这是最简单的寄存器布局，每个寄存器紧挨着前一个寄存器。
    */ 
    serial_mm_init(system_memory, memmap[QUARD_STAR_UART0].base,
        0, qdev_get_gpio_in(DEVICE(mmio_plic), QUARD_STAR_UART0_IRQ), 399193,
        serial_hd(0), DEVICE_LITTLE_ENDIAN);
    serial_mm_init(system_memory, memmap[QUARD_STAR_UART1].base,
        0, qdev_get_gpio_in(DEVICE(mmio_plic), QUARD_STAR_UART1_IRQ), 399193,
        serial_hd(1), DEVICE_LITTLE_ENDIAN);
    serial_mm_init(system_memory, memmap[QUARD_STAR_UART2].base,
        0, qdev_get_gpio_in(DEVICE(mmio_plic), QUARD_STAR_UART2_IRQ), 399193,
        serial_hd(2), DEVICE_LITTLE_ENDIAN);

    // 创建并配置Flash存储设备。
    s->flash = quard_star_flash_create(s, "quard-star.flash0", "pflash0");
    // 这个是将这个flash设备与qemu实现的驱动绑定起来，一般情况下，设备的驱动是需要自己写的
    pflash_cfi01_legacy_drive(s->flash, drive_get(IF_PFLASH, 0, 0));
    quard_star_flash_map(s->flash, virt_memmap[QUARD_STAR_FLASH].base,
                         virt_memmap[QUARD_STAR_FLASH].size, system_memory);
}

static void quard_star_machine_instance_init(Object *obj)
{
}

static void quard_star_machine_class_init(ObjectClass *oc, void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "RISC-V Quard Star board";
    mc->init = quard_star_machine_init;
    mc->max_cpus = QUARD_STAR_CPUS_MAX;
    mc->default_cpu_type = TYPE_RISCV_CPU_BASE;
    mc->pci_allow_0_address = true;
    mc->possible_cpu_arch_ids = riscv_numa_possible_cpu_arch_ids;
    mc->cpu_index_to_instance_props = riscv_numa_cpu_index_to_props;
    mc->get_default_cpu_node_id = riscv_numa_get_default_cpu_node_id;
    mc->numa_mem_supported = true;
}

static const TypeInfo quard_star_machine_typeinfo = {
    .name       = MACHINE_TYPE_NAME("quard-star"),
    .parent     = TYPE_MACHINE,
    .class_init = quard_star_machine_class_init,
    .instance_init = quard_star_machine_instance_init,
    .instance_size = sizeof(RISCVVirtState),
};

static void quard_star_machine_init_register_types(void)
{
    type_register_static(&quard_star_machine_typeinfo);
}

type_init(quard_star_machine_init_register_types)
