/* Linux Image protocol, shared by the arm64 and riscv64 entries: the boot loader's FDT
 * becomes EfiEmuBootInfo (ram less its reservations, the command line, a framebuffer).
 * The arm64 entry calls this with the MMU off, so it is built with -mstrict-align there. */
#include "efi_emulation.h"
#include "fdt.h"

#define MAX_RAM 16
#define MAX_RESERVED 32

static CHAR8 cmdline[2048];
static KernelRange ram[MAX_RAM], reserved[MAX_RESERVED];
static UINT32 nram, nreserved;

/* A simple-framebuffer node from u-boot, committed when the node ends */
static struct {
  BOOLEAN compat, disabled, argb;
  UINT64 base;
  UINT32 width, height, stride;
} sfb;

static void commit_simplefb(EfiEmuBootInfo *info) {
  if (sfb.compat && !sfb.disabled && sfb.argb && sfb.base && sfb.width &&
      sfb.height && sfb.stride >= sfb.width * 4) {
    EfiEmuFramebuffer *fb = &info->framebuffer;
    fb->base = sfb.base;
    fb->width = sfb.width;
    fb->height = sfb.height;
    fb->pixels_per_scanline = sfb.stride / 4;
    fb->bits_per_pixel = 32;
    fb->red_position = 16;
    fb->blue_position = 0;
    fb->valid = 1;
  }
  UINT8 *z = (UINT8 *)&sfb;
  for (UINTN i = 0; i < sizeof(sfb); ++i)
    z[i] = 0;
}

static void add_reserved(UINT64 base, UINT64 size) {
  if (size && nreserved < MAX_RESERVED) {
    reserved[nreserved].base = base;
    reserved[nreserved++].size = size;
  }
}

static void log_ranges(CONST CHAR8 *what, CONST KernelRange *r, UINT32 n) {
  for (UINT32 i = 0; i < n; ++i) {
    efiemu_debug_string("xnu-loader kernel: ");
    efiemu_debug_string(what);
    efiemu_debug_string(" ");
    efiemu_debug_hex(r[i].base);
    efiemu_debug_string(" size ");
    efiemu_debug_hex(r[i].size);
    efiemu_debug_string("\n");
  }
}

BOOLEAN kernel_parse_fdt(EfiEmuBootInfo *info, UINT64 fdt, KernelFdt *out) {
  CONST VOID *h = (CONST VOID *)(UINTN)fdt;
  UINT32 addr_cells = 2, size_cells = 1, rsv_addr = 2, rsv_size = 1;
  /* What the node at depth 2 is */
  enum { N_OTHER, N_MEMORY, N_CHOSEN, N_CPUS, N_PSCI, N_RESMEM } kind = N_OTHER;
  FdtWalk w;
  int ev;

  if (!fdt_walk_init(&w, h))
    return FALSE;
  info->fdt = fdt;
  info->fdt_size = fdt_check(h);
  for (UINT32 i = 0;; ++i) {
    UINT64 b, s;
    if (!fdt_rsv(h, i, &b, &s))
      break;
    add_reserved(b, s);
  }

  while ((ev = fdt_walk_next(&w)) != FDT_EV_DONE) {
    if (ev == FDT_EV_BEGIN) {
      if (w.depth == 2)
        kind = fdt_name_is(w.name, "memory")            ? N_MEMORY
               : fdt_name_is(w.name, "chosen")          ? N_CHOSEN
               : fdt_name_is(w.name, "cpus")            ? N_CPUS
               : fdt_name_is(w.name, "psci")            ? N_PSCI
               : fdt_name_is(w.name, "reserved-memory") ? N_RESMEM
                                                        : N_OTHER;
      continue;
    }
    if (ev == FDT_EV_END) {
      if (w.depth == 2) {
        commit_simplefb(info);
        kind = N_OTHER;
      }
      continue;
    }

    CONST CHAR8 *pname = w.prop;
    CONST UINT8 *v = w.value;
    UINT32 len = w.len;
    if (w.depth == 1) {
      if (fdt_str_eq(pname, "#address-cells"))
        addr_cells = rsv_addr = fdt_be32(v);
      else if (fdt_str_eq(pname, "#size-cells"))
        size_cells = rsv_size = fdt_be32(v);
    } else if (w.depth == 2 && kind == N_MEMORY && fdt_str_eq(pname, "reg")) {
      UINT32 stride = 4 * (addr_cells + size_cells);
      for (UINT32 o = 0; o + stride <= len && nram < MAX_RAM; o += stride) {
        ram[nram].base = fdt_cells(v + o, addr_cells);
        ram[nram++].size = fdt_cells(v + o + 4 * addr_cells, size_cells);
      }
    } else if (w.depth == 2 && kind == N_CHOSEN) {
      if (fdt_str_eq(pname, "bootargs")) {
        UINT32 n = len < sizeof(cmdline) ? len : sizeof(cmdline) - 1;
        for (UINT32 i = 0; i < n; ++i)
          cmdline[i] = (CHAR8)v[i];
        cmdline[n] = 0;
      } else if (fdt_str_eq(pname, "linux,initrd-start")) {
        out->initrd_start = fdt_cells(v, len / 4);
      } else if (fdt_str_eq(pname, "linux,initrd-end")) {
        out->initrd_end = fdt_cells(v, len / 4);
      }
    } else if (w.depth == 2 && kind == N_CPUS && fdt_str_eq(pname, "timebase-frequency")) {
      out->timebase = fdt_cells(v, len / 4);
    } else if (w.depth == 2 && kind == N_PSCI && fdt_str_eq(pname, "method")) {
      out->psci = fdt_str_eq((CONST CHAR8 *)v, "hvc") ? 1 : fdt_str_eq((CONST CHAR8 *)v, "smc") ? 2 : 0;
    } else if (w.depth == 2 && kind == N_OTHER) {
      if (fdt_str_eq(pname, "compatible"))
        sfb.compat = fdt_list_has(v, len, "simple-framebuffer");
      else if (fdt_str_eq(pname, "status"))
        sfb.disabled = !fdt_okay(v);
      else if (fdt_str_eq(pname, "format"))
        sfb.argb = fdt_str_eq((CONST CHAR8 *)v, "a8r8g8b8") || fdt_str_eq((CONST CHAR8 *)v, "x8r8g8b8");
      else if (fdt_str_eq(pname, "reg") && len >= 4 * addr_cells)
        sfb.base = fdt_cells(v, addr_cells);
      else if (fdt_str_eq(pname, "width"))
        sfb.width = fdt_be32(v);
      else if (fdt_str_eq(pname, "height"))
        sfb.height = fdt_be32(v);
      else if (fdt_str_eq(pname, "stride"))
        sfb.stride = fdt_be32(v);
    } else if (w.depth == 2 && kind == N_RESMEM) {
      if (fdt_str_eq(pname, "#address-cells"))
        rsv_addr = fdt_be32(v);
      else if (fdt_str_eq(pname, "#size-cells"))
        rsv_size = fdt_be32(v);
    } else if (w.depth == 3 && kind == N_RESMEM && fdt_str_eq(pname, "reg")) {
      UINT32 stride = 4 * (rsv_addr + rsv_size);
      for (UINT32 o = 0; o + stride <= len; o += stride)
        add_reserved(fdt_cells(v + o, rsv_addr), fdt_cells(v + o + 4 * rsv_addr, rsv_size));
    }
  }
  if (cmdline[0])
    info->cmdline = cmdline;
  out->ram = ram;
  out->nram = nram;
  return TRUE;
}

/* RAM minus reserved ranges, as the usable/reserved list efi-emulation expects */
void kernel_fdt_memory_map(EfiEmuBootInfo *info) {
  log_ranges("ram", ram, nram);
  log_ranges("reserved", reserved, nreserved);
  for (UINT32 r = 0; r < nram; ++r) {
    UINT64 b = ram[r].base, e = ram[r].base + ram[r].size;
    while (b < e && info->memory_count < EFIEMU_MAX_MEMORY_RANGES - 1) {
      UINT64 cut = e, skip = e;
      for (UINT32 i = 0; i < nreserved; ++i) {
        UINT64 rb = reserved[i].base, re = rb + reserved[i].size;
        if (re > b && rb < cut) {
          cut = rb > b ? rb : b;
          skip = re;
        }
      }
      if (cut > b) {
        EfiEmuMemoryRange *m = &info->memory[info->memory_count++];
        m->base = b;
        m->length = cut - b;
        m->type = EfiEmuMemoryUsable;
      }
      if (skip > cut && info->memory_count < EFIEMU_MAX_MEMORY_RANGES) {
        EfiEmuMemoryRange *m = &info->memory[info->memory_count++];
        m->base = cut;
        m->length = (skip < e ? skip : e) - cut;
        m->type = EfiEmuMemoryReserved;
      }
      b = skip;
    }
  }
}
