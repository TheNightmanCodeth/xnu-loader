// risc-v linux image protocol: the fdt becomes EfiEmuBootInfo, then the efi emulation runs
// the loader runs bare with satp = 0, so there is no page table to build here
#include "efi_emulation.h"
#include "serial.h"

static EfiEmuBootInfo boot_info;
static KernelFdt fdt_facts;

UINT64 efiemu_riscv_hartid;
UINT64 efiemu_riscv_timebase;

extern CONST CHAR8 embedded_initrd_start[], embedded_initrd_end[];

static void __attribute__((noreturn)) halt(void) {
  for (;;)
    __asm__ volatile("wfi");
}

void kernel_riscv64_main(UINT64 hartid, UINT64 fdt) {
  serial_init();
  efiemu_exceptions_install();
  efiemu_riscv_hartid = hartid;
  efiemu_debug_string("\nxnu-loader kernel: riscv64 Image entry on hart ");
  efiemu_debug_hex(hartid);
  efiemu_debug_string(", fdt ");
  efiemu_debug_hex(fdt);
  efiemu_debug_string("\n");
  if (!fdt || !kernel_parse_fdt(&boot_info, fdt, &fdt_facts)) {
    efiemu_debug_string("xnu-loader kernel: no device tree in a1\n");
    halt();
  }
  efiemu_riscv_timebase = fdt_facts.timebase;
  efiemu_debug_string("xnu-loader kernel: device tree parsed, timebase ");
  efiemu_debug_hex(efiemu_riscv_timebase);
  efiemu_debug_string(" Hz\n");
  kernel_fdt_memory_map(&boot_info);

  UINT64 initrd = fdt_facts.initrd_start;
  UINT64 initrd_size = fdt_facts.initrd_end > fdt_facts.initrd_start ? fdt_facts.initrd_end - fdt_facts.initrd_start : 0;
  // clang folds a compare of two distinct arrays, so compare the addresses opaquely
  UINTN embedded_start = (UINTN)embedded_initrd_start, embedded_end = (UINTN)embedded_initrd_end;
  __asm__("" : "+r"(embedded_start), "+r"(embedded_end));
  if (embedded_end != embedded_start) {
    initrd = embedded_start;
    initrd_size = embedded_end - embedded_start;
    efiemu_debug_string("xnu-loader kernel: using the built-in initrd\n");
  }
  if (initrd && initrd_size) {
    efiemu_debug_string("xnu-loader kernel: initrd ");
    efiemu_debug_hex(initrd);
    efiemu_debug_string(" size ");
    efiemu_debug_hex(initrd_size);
    efiemu_debug_string("\n");
    kernel_parse_cpio(&boot_info, initrd, initrd_size);
  } else {
    efiemu_debug_string("xnu-loader kernel: no initrd\n");
  }
  if (boot_info.memory_count == 0) {
    efiemu_debug_string("xnu-loader kernel: device tree has no memory\n");
    halt();
  }
  efiemu_main(&boot_info);
}
