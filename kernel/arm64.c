/* arm64 Linux Image protocol: FDT -> EfiEmuBootInfo, an identity map, then
 * the loader with no firmware (kernel_boot). Runs with the MMU off until arm64_enable_mmu, so this file
 * is built with -mstrict-align (Device memory faults on unaligned access). */
#include "efi_emulation.h"
#include "serial.h"
#include "platform.h"

static EfiEmuBootInfo boot_info;
static KernelFdt fdt_facts;

extern CONST CHAR8 embedded_initrd_start[], embedded_initrd_end[];
extern UINT8 __kernel_start, __kernel_end;

/* 4K granule, 39-bit VA: 1 GiB blocks, split into 2 MiB blocks where RAM starts.
 * RAM is Normal write-back, everything else Device-nGnRE and execute-never. */
#define PT_BLOCK 1ULL
#define PT_TABLE 3ULL
#define PT_AF (1ULL << 10)
#define PT_SH_INNER (3ULL << 8)
#define PT_ATTR(n) ((UINT64)(n) << 2)
#define PT_XN (3ULL << 53)
#define MAIR_VALUE 0x00000000000004ff00ULL /* 0 Device-nGnRnE, 1 Normal WB, 2 Device-nGnRE */

static UINT64 l1[512] __attribute__((aligned(4096)));
static UINT64 l2[8][512] __attribute__((aligned(4096)));

static BOOLEAN is_ram(UINT64 b, UINT64 e) {
  CONST KernelRange *ram = fdt_facts.ram;
  for (UINT32 r = 0; r < fdt_facts.nram; ++r)
    if (b >= ram[r].base && e <= ram[r].base + ram[r].size)
      return TRUE;
  return FALSE;
}

static BOOLEAN touches_ram(UINT64 b, UINT64 e) {
  CONST KernelRange *ram = fdt_facts.ram;
  for (UINT32 r = 0; r < fdt_facts.nram; ++r)
    if (b < ram[r].base + ram[r].size && e > ram[r].base)
      return TRUE;
  return FALSE;
}

static UINT64 block(UINT64 pa, BOOLEAN normal) {
  return pa | PT_BLOCK | PT_AF |
         (normal ? PT_ATTR(1) | PT_SH_INNER : PT_ATTR(2) | PT_XN);
}

static void arm64_enable_mmu(void) {
  UINT32 used = 0;
  for (UINT64 g = 0; g < 512; ++g) {
    UINT64 gb = g << 30;
    if (touches_ram(gb, gb + (1ULL << 30)) && !is_ram(gb, gb + (1ULL << 30)) && used < 8) {
      UINT64 *t = l2[used++];
      for (UINT64 i = 0; i < 512; ++i) {
        UINT64 pa = gb + (i << 21);
        t[i] = block(pa, is_ram(pa, pa + (1ULL << 21)));
      }
      l1[g] = (UINT64)(UINTN)t | PT_TABLE;
    } else {
      l1[g] = block(gb, touches_ram(gb, gb + (1ULL << 30)));
    }
  }
  UINT64 mmfr0, ips;
  __asm__ volatile("mrs %0, id_aa64mmfr0_el1" : "=r"(mmfr0));
  ips = mmfr0 & 7;
  if (ips > 5)
    ips = 5;
  /* T0SZ=25, IRGN0/ORGN0 write-back, inner shareable, 4K, TTBR1 walks off */
  UINT64 tcr = 25 | (1ULL << 8) | (1ULL << 10) | (3ULL << 12) | (1ULL << 23) | (ips << 32);
  /* Written with caches off: drop any stale lines firmware left for our image */
  UINT64 ctr, line;
  __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
  line = 4ULL << ((ctr >> 16) & 0xf);
  for (UINT64 a = (UINT64)(UINTN)&__kernel_start & ~(line - 1);
       a < (UINT64)(UINTN)&__kernel_end; a += line)
    __asm__ volatile("dc civac, %0" : : "r"(a) : "memory");
  __asm__ volatile("dsb sy; ic iallu; dsb ish; isb" : : : "memory");
  __asm__ volatile(
      "msr mair_el1, %0\n"
      "msr tcr_el1, %1\n"
      "msr ttbr0_el1, %2\n"
      "isb\n"
      "tlbi vmalle1\n"
      "dsb ish\n"
      "isb\n"
      "mrs x9, sctlr_el1\n"
      "orr x9, x9, #(1 << 0)\n"
      "orr x9, x9, #(1 << 2)\n"
      "orr x9, x9, #(1 << 12)\n"
      "bic x9, x9, #(1 << 1)\n"
      "msr sctlr_el1, x9\n"
      "isb\n"
      :
      : "r"(MAIR_VALUE), "r"(tcr), "r"((UINT64)(UINTN)l1)
      : "x9", "memory");
}

void kernel_arm64_main(UINT64 fdt) {
  // the console comes from the tree, so read the board before the first message
  fdt_board_parse((CONST VOID *)(UINTN)fdt);
  serial_init();
  serial_puts8((CONST CHAR8 *)"\nxnu-loader kernel: C entry\n");
  efiemu_exceptions_install();
  if (!kernel_parse_fdt(&boot_info, fdt, &fdt_facts)) {
    serial_puts8((CONST CHAR8 *)"xnu-loader kernel: no device tree in x0\n");
    for (;;)
      __asm__ volatile("wfi");
  }
  serial_puts8((CONST CHAR8 *)"xnu-loader kernel: device tree parsed, enabling the MMU\n");
  arm64_enable_mmu();
  serial_reinit();
  efiemu_debug_string("xnu-loader kernel: arm64 Image entry\n");
  kernel_fdt_memory_map(&boot_info);

  UINT64 initrd = fdt_facts.initrd_start;
  UINT64 initrd_size = fdt_facts.initrd_end > fdt_facts.initrd_start ? fdt_facts.initrd_end - fdt_facts.initrd_start : 0;
  if (embedded_initrd_end != embedded_initrd_start) {
    initrd = (UINT64)(UINTN)embedded_initrd_start;
    initrd_size = (UINT64)(embedded_initrd_end - embedded_initrd_start);
    efiemu_debug_string("xnu-loader kernel: using the built-in initrd\n");
  }
  if (initrd && initrd_size)
    kernel_parse_cpio(&boot_info, initrd, initrd_size);
  else
    efiemu_debug_string("xnu-loader kernel: no initrd\n");
  if (boot_info.memory_count == 0) {
    efiemu_debug_string("xnu-loader kernel: device tree has no memory\n");
    for (;;)
      __asm__ volatile("wfi");
  }
  kernel_boot(&boot_info);
}
