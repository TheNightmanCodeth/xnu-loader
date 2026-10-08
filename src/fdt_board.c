// Board description from the flattened device tree the firmware hands over: RAM base,
// the console UART and the GIC, so one arm64 build runs on any board with a sane DTB.
// Runs before the MMU is on in the Linux Image path, so every read is bytewise.
#include "fdt_board.h"
#include "fdt.h"
#include "platform.h"

#if defined(__aarch64__) && defined(XNU_LOADER_PLATFORM_GENERIC)

#define MAX_DEPTH   16
#define PATH_LEN    256
#define MAX_ALIASES 16

FdtBoard g_board;

// no library calls: this runs before the MMU and caches are on
static VOID mem_zero(VOID *p, UINTN n) {
  volatile UINT8 *b = (volatile UINT8 *)p;
  for (UINTN i = 0; i < n; ++i)
    b[i] = 0;
}

// a == b up to n bytes of a, with b ending there
static BOOLEAN str_eq_n(CONST CHAR8 *a, UINTN n, CONST CHAR8 *b) {
  for (UINTN i = 0; i < n; ++i) {
    if (a[i] != b[i])
      return FALSE;
  }
  return b[n] == 0;
}

VOID fdt_board_defaults(VOID) {
  mem_zero(&g_board, sizeof(g_board));
  g_board.ram_base = 0x40000000ULL;
  g_board.uart_baud = 115200;
#if defined(XNU_LOADER_PLATFORM_QEMUVIRT)
  g_board.is_qemu_virt = TRUE;
  g_board.has_pci_ecam = TRUE;
  g_board.soc_base = 0x08000000ULL;
  g_board.soc_size = 0x08000000ULL;
  g_board.uart_kind = FDT_UART_PL011;
  g_board.uart_base = 0x09000000ULL;
  g_board.uart_size = 0x1000;
  g_board.uart_width = 4;
  g_board.gic_version = 3;
  g_board.gicd_base = 0x08000000ULL;
  g_board.gicd_size = 0x10000;
  g_board.gic2_base = 0x080a0000ULL;
  g_board.gic2_size = 0xf60000;
#elif defined(XNU_LOADER_PLATFORM_SUN50I)
  // allwinner h616/h618, orange pi zero 3
  g_board.soc_base = 0x01000000ULL;
  g_board.soc_size = 0x07000000ULL;
  g_board.uart_kind = FDT_UART_16550;
  g_board.uart_base = 0x05000000ULL;
  g_board.uart_size = 0x400;
  g_board.uart_shift = 2;
  g_board.uart_width = 4;
  g_board.uart_clock = 24000000;
  g_board.gic_version = 2;
  g_board.gicd_base = 0x03021000ULL;
  g_board.gicd_size = 0x1000;
  g_board.gic2_base = 0x03022000ULL;
  g_board.gic2_size = 0x2000;
#elif defined(XNU_LOADER_PLATFORM_SG2002)
  // sophgo sg2002 (licheerv nano) on its cortex-a53
  g_board.ram_base = 0x80000000ULL;
  g_board.soc_base = 0x01000000ULL;
  g_board.soc_size = 0x07000000ULL;
  g_board.uart_kind = FDT_UART_16550;
  g_board.uart_base = 0x04140000ULL;
  g_board.uart_size = 0x100;
  g_board.uart_shift = 2;
  g_board.uart_width = 4;
  g_board.uart_clock = 25000000;
  g_board.gic_version = 2;
  g_board.gicd_base = 0x01F01000ULL;
  g_board.gicd_size = 0x1000;
  g_board.gic2_base = 0x01F02000ULL;
  g_board.gic2_size = 0x2000;
#elif defined(XNU_LOADER_PLATFORM_SC8280XP)
  g_board.ram_base = 0x80000000ULL;
  g_board.soc_base = 0x17000000ULL;
  g_board.soc_size = 0x01000000ULL;
  g_board.psci_method = 2;
  g_board.gic_version = 3;
  g_board.gicd_base = 0x17a00000ULL;
  g_board.gicd_size = 0x10000;
  g_board.gic2_base = 0x17a60000ULL;
  g_board.gic2_size = 0x100000;
#endif
}

// console drivers by compatible, with the register layout a DTB without reg-shift implies
static CONST struct {
  CONST CHAR8 *compat;
  FdtUartKind kind;
  UINT32 shift, width;
} uart_table[] = {
  { "arm,pl011",                FDT_UART_PL011, 0, 4 },
  { "snps,dw-apb-uart",         FDT_UART_16550, 2, 4 },
  { "allwinner,uart-v100",      FDT_UART_16550, 2, 4 },
  { "allwinner,sun6i-a31-uart", FDT_UART_16550, 2, 4 },
  { "ns16550a",                 FDT_UART_16550, 0, 1 },
  { "ns16550",                  FDT_UART_16550, 0, 1 },
};

static CONST CHAR8 *gic3_compat[] = { "arm,gic-v3" };
static CONST CHAR8 *gic2_compat[] = { "arm,gic-400", "arm,cortex-a15-gic", "arm,cortex-a9-gic", "arm,gic-v2" };

typedef struct {
  UINT32 addr_cells, size_cells;   // for this node's children
  CONST UINT8 *ranges;
  UINT32 ranges_len;
  BOOLEAN has_ranges;
  UINTN path_len;
  CONST UINT8 *reg;
  UINT32 reg_len;
  CONST CHAR8 *compat;
  UINT32 compat_len;
  BOOLEAN disabled;
  BOOLEAN has_shift, has_width;
  UINT32 shift, width, clock, speed;
} Level;

// the console the firmware means: stdout-path, then console= on the command line, then serial0
typedef struct {
  CONST CHAR8 *stdout_path;
  UINT32 stdout_len;
  CONST CHAR8 *bootargs;
  UINT32 bootargs_len;
  CONST CHAR8 *alias_name[MAX_ALIASES];
  CONST CHAR8 *alias_path[MAX_ALIASES];
  UINT32 naliases;
  CONST UINT8 *mem_reg;
  UINT32 mem_len;
  UINT32 root_addr, root_size;
  UINT32 psci;
} Prescan;

static VOID prescan(CONST VOID *blob, Prescan *p) {
  FdtWalk w;
  enum { N_OTHER, N_CHOSEN, N_ALIASES, N_MEMORY, N_PSCI } kind = N_OTHER;
  int ev;

  p->root_addr = 2;
  p->root_size = 1;
  fdt_walk_init(&w, blob);
  while ((ev = fdt_walk_next(&w)) != FDT_EV_DONE) {
    if (ev == FDT_EV_BEGIN && w.depth == 2) {
      kind = fdt_str_eq(w.name, "chosen") ? N_CHOSEN
             : fdt_str_eq(w.name, "aliases") ? N_ALIASES
             : fdt_name_is(w.name, "memory") ? N_MEMORY
             : fdt_str_eq(w.name, "psci") ? N_PSCI
             : N_OTHER;
    } else if (ev == FDT_EV_END && w.depth == 2) {
      kind = N_OTHER;
    } else if (ev == FDT_EV_PROP) {
      CONST CHAR8 *pname = w.prop;
      CONST UINT8 *v = w.value;
      UINT32 len = w.len;
      if (w.depth == 1 && fdt_str_eq(pname, "#address-cells"))
        p->root_addr = fdt_be32(v);
      else if (w.depth == 1 && fdt_str_eq(pname, "#size-cells"))
        p->root_size = fdt_be32(v);
      else if (w.depth == 1 && fdt_str_eq(pname, "compatible"))
        g_board.is_qemu_virt = fdt_list_has(v, len, "linux,dummy-virt");
      else if (w.depth == 2 && kind == N_CHOSEN && fdt_str_eq(pname, "stdout-path")) {
        p->stdout_path = (CONST CHAR8 *)v;
        p->stdout_len = len;
      } else if (w.depth == 2 && kind == N_CHOSEN && fdt_str_eq(pname, "bootargs")) {
        p->bootargs = (CONST CHAR8 *)v;
        p->bootargs_len = len;
      } else if (w.depth == 2 && kind == N_ALIASES && p->naliases < MAX_ALIASES) {
        p->alias_name[p->naliases] = pname;
        p->alias_path[p->naliases++] = (CONST CHAR8 *)v;
      } else if (w.depth == 2 && kind == N_PSCI && fdt_str_eq(pname, "method")) {
        p->psci = fdt_str_eq((CONST CHAR8 *)v, "hvc") ? 1 : fdt_str_eq((CONST CHAR8 *)v, "smc") ? 2 : 0;
      } else if (w.depth == 2 && kind == N_MEMORY && fdt_str_eq(pname, "reg") && !p->mem_reg) {
        p->mem_reg = v;
        p->mem_len = len;
      }
    }
  }
}

static CONST CHAR8 *alias(Prescan *p, CONST CHAR8 *name, UINTN n) {
  for (UINT32 i = 0; i < p->naliases; ++i) {
    if (str_eq_n(name, n, p->alias_name[i]))
      return p->alias_path[i];
  }
  return NULL;
}

static UINT32 parse_uint(CONST CHAR8 *s, CONST CHAR8 *end) {
  UINT32 v = 0;
  while (s < end && *s >= '0' && *s <= '9')
    v = v * 10 + (UINT32)(*s++ - '0');
  return v;
}

// the path of the console node, and the baud when the firmware says one
static CONST CHAR8 *console_path(Prescan *p, UINT32 *baud) {
  if (p->stdout_path && p->stdout_len > 1) {
    CONST CHAR8 *s = p->stdout_path;
    UINTN n = 0;
    while (s[n] && s[n] != ':')
      ++n;
    if (s[n] == ':' && parse_uint(s + n + 1, s + p->stdout_len))
      *baud = parse_uint(s + n + 1, s + p->stdout_len);
    if (s[0] == '/')
      return s;   // compared up to the ':'
    return alias(p, s, n);
  }

  // console=ttyS<n>[,baud] or ttyAMA<n>, the last one wins as in Linux
  CONST CHAR8 *found = NULL;
  if (p->bootargs) {
    CONST CHAR8 *s = p->bootargs, *end = p->bootargs + p->bootargs_len;
    while (s < end && *s) {
      CONST CHAR8 *tty = NULL;
      if (fdt_str_prefix(s, "console=ttyS"))
        tty = s + 12;
      else if (fdt_str_prefix(s, "console=ttyAMA"))
        tty = s + 14;
      if (tty && *tty >= '0' && *tty <= '9') {
        CHAR8 name[16] = { 's', 'e', 'r', 'i', 'a', 'l' };
        UINTN n = 6;
        while (*tty >= '0' && *tty <= '9' && n < sizeof(name) - 1)
          name[n++] = *tty++;
        if (alias(p, name, n)) {
          found = alias(p, name, n);
          if (*tty == ',' && parse_uint(tty + 1, end))
            *baud = parse_uint(tty + 1, end);
        }
      }
      while (s < end && *s && *s != ' ')
        ++s;
      while (s < end && *s == ' ')
        ++s;
    }
  }
  if (found)
    return found;
  return alias(p, "serial0", 7);
}

// a node address in its parent's space, carried up through every ranges on the way to root
static BOOLEAN translate(Level *lv, UINT32 depth, UINT64 *addr) {
  for (UINT32 d = depth - 1; d >= 1; --d) {
    Level *bus = &lv[d];
    Level *parent = &lv[d - 1];
    if (!bus->has_ranges)
      return TRUE;
    if (bus->ranges_len == 0)
      continue;
    UINT32 child = bus->addr_cells, up = parent->addr_cells, size = bus->size_cells;
    UINT32 stride = 4 * (child + up + size);
    BOOLEAN hit = FALSE;
    for (UINT32 o = 0; o + stride <= bus->ranges_len; o += stride) {
      UINT64 cb = fdt_cells(bus->ranges + o, child);
      UINT64 pb = fdt_cells(bus->ranges + o + 4 * child, up);
      UINT64 sz = fdt_cells(bus->ranges + o + 4 * (child + up), size);
      if (*addr >= cb && *addr - cb < sz) {
        *addr = *addr - cb + pb;
        hit = TRUE;
        break;
      }
    }
    if (!hit)
      return FALSE;
  }
  return TRUE;
}

// the index-th (address, size) pair of a node's reg, in CPU physical addresses
static BOOLEAN node_reg(Level *lv, UINT32 depth, UINT32 index, UINT64 *base, UINT64 *size) {
  Level *node = &lv[depth], *parent = &lv[depth - 1];
  UINT32 ac = parent->addr_cells, sc = parent->size_cells;
  UINT32 off = index * 4 * (ac + sc);

  if (!node->reg || off + 4 * (ac + sc) > node->reg_len)
    return FALSE;
  *base = fdt_cells(node->reg + off, ac);
  *size = sc ? fdt_cells(node->reg + off + 4 * ac, sc) : 0;
  return translate(lv, depth, base);
}

static BOOLEAN path_is(CONST CHAR8 *path, UINTN n, CONST CHAR8 *want) {
  UINTN w = 0;
  while (want[w] && want[w] != ':')
    ++w;
  if (w != n)
    return FALSE;
  for (UINTN i = 0; i < n; ++i) {
    if (path[i] != want[i])
      return FALSE;
  }
  return TRUE;
}

static VOID node_done(Level *lv, UINT32 depth, CONST CHAR8 *path, CONST CHAR8 *console,
                      BOOLEAN *have_uart, BOOLEAN *have_gic) {
  Level *node = &lv[depth];
  UINT64 base, size;

  if (node->disabled || !node->compat)
    return;

  if (fdt_list_has(node->compat, node->compat_len, "pci-host-ecam-generic"))
    g_board.has_pci_ecam = TRUE;

  if (!*have_uart && console && path_is(path, node->path_len, console)) {
    for (UINT32 i = 0; i < sizeof(uart_table) / sizeof(uart_table[0]); ++i) {
      if (!fdt_list_has(node->compat, node->compat_len, uart_table[i].compat))
        continue;
      if (!node_reg(lv, depth, 0, &base, &size))
        break;
      g_board.uart_kind = uart_table[i].kind;
      g_board.uart_base = base;
      g_board.uart_size = size ? size : 0x1000;
      g_board.uart_shift = node->has_shift ? node->shift : uart_table[i].shift;
      g_board.uart_width = node->has_width ? node->width : uart_table[i].width;
      g_board.uart_clock = node->clock;
      if (node->speed)
        g_board.uart_baud = node->speed;
      *have_uart = TRUE;
      break;
    }
  }

  if (!*have_gic) {
    UINT32 version = 0;
    for (UINT32 i = 0; i < sizeof(gic3_compat) / sizeof(gic3_compat[0]); ++i)
      version = fdt_list_has(node->compat, node->compat_len, gic3_compat[i]) ? 3 : version;
    for (UINT32 i = 0; i < sizeof(gic2_compat) / sizeof(gic2_compat[0]); ++i)
      version = fdt_list_has(node->compat, node->compat_len, gic2_compat[i]) ? 2 : version;
    if (version && node_reg(lv, depth, 0, &g_board.gicd_base, &g_board.gicd_size) &&
        node_reg(lv, depth, 1, &g_board.gic2_base, &g_board.gic2_size)) {
      g_board.gic_version = version;
      *have_gic = TRUE;
    }
  }
}

BOOLEAN fdt_board_parse(CONST VOID *blob) {
  FdtWalk w;
  Prescan p;
  Level lv[MAX_DEPTH];
  CHAR8 path[PATH_LEN];
  UINT32 baud = 0;
  BOOLEAN have_uart = FALSE, have_gic = FALSE;
  int ev;

  fdt_board_defaults();
  if (!fdt_walk_init(&w, blob))
    return FALSE;

  mem_zero(&p, sizeof(p));
  prescan(blob, &p);

  // a board with a tree is described by it alone, not by the build's defaults
  BOOLEAN virt = g_board.is_qemu_virt;
  UINT64 soc_base = g_board.soc_base, soc_size = g_board.soc_size;
  mem_zero(&g_board, sizeof(g_board));
  g_board.from_fdt = TRUE;
  g_board.is_qemu_virt = virt;
  g_board.soc_base = soc_base;
  g_board.soc_size = soc_size;
  g_board.uart_baud = 115200;
  g_board.ram_base = 0x40000000ULL;
  g_board.psci_method = p.psci;

  if (p.mem_reg) {
    UINT32 stride = 4 * (p.root_addr + p.root_size);
    UINT64 lowest = ~0ULL;
    for (UINT32 o = 0; o + stride <= p.mem_len; o += stride) {
      UINT64 b = fdt_cells(p.mem_reg + o, p.root_addr);
      if (b < lowest)
        lowest = b;
    }
    if (lowest != ~0ULL)
      g_board.ram_base = lowest;
  }

  CONST CHAR8 *console = console_path(&p, &baud);
  if (baud)
    g_board.uart_baud = baud;

  // lv[] is indexed from the root at 0
  while ((ev = fdt_walk_next(&w)) != FDT_EV_DONE) {
    UINT32 depth = w.depth - 1;
    if (depth >= MAX_DEPTH)
      break;
    if (ev == FDT_EV_BEGIN) {
      Level *l = &lv[depth];
      UINTN n = fdt_str_len(w.name);
      mem_zero(l, sizeof(*l));
      l->addr_cells = 2;
      l->size_cells = 1;
      if (depth == 0) {
        path[0] = '/';
        l->path_len = 1;
      } else {
        UINTN at = lv[depth - 1].path_len;
        if (at > 1 && at < PATH_LEN - 1)
          path[at++] = '/';
        for (UINTN i = 0; i < n && at < PATH_LEN - 1; ++i)
          path[at++] = w.name[i];
        l->path_len = at;
      }
    } else if (ev == FDT_EV_END) {
      if (depth >= 1)
        node_done(lv, depth, path, console, &have_uart, &have_gic);
    } else if (ev == FDT_EV_PROP) {
      CONST CHAR8 *pname = w.prop;
      CONST UINT8 *v = w.value;
      UINT32 len = w.len;
      Level *l = &lv[depth];
      if (fdt_str_eq(pname, "#address-cells"))
        l->addr_cells = fdt_be32(v);
      else if (fdt_str_eq(pname, "#size-cells"))
        l->size_cells = fdt_be32(v);
      else if (fdt_str_eq(pname, "ranges")) {
        l->has_ranges = TRUE;
        l->ranges = v;
        l->ranges_len = len;
      } else if (fdt_str_eq(pname, "reg")) {
        l->reg = v;
        l->reg_len = len;
      } else if (fdt_str_eq(pname, "compatible")) {
        l->compat = (CONST CHAR8 *)v;
        l->compat_len = len;
      } else if (fdt_str_eq(pname, "status")) {
        l->disabled = !fdt_okay(v);
      } else if (fdt_str_eq(pname, "reg-shift") && len == 4) {
        l->has_shift = TRUE;
        l->shift = fdt_be32(v);
      } else if (fdt_str_eq(pname, "reg-io-width") && len == 4) {
        l->has_width = TRUE;
        l->width = fdt_be32(v);
      } else if (fdt_str_eq(pname, "clock-frequency") && len == 4) {
        l->clock = fdt_be32(v);
      } else if (fdt_str_eq(pname, "current-speed") && len == 4) {
        l->speed = fdt_be32(v);
      }
    }
  }

  // the arm-io window XNU offsets devices from: the span of what the kernel touches
  if (!g_board.soc_size) {
    UINT64 lo = ~0ULL, hi = 0;
    UINT64 b[3] = { g_board.uart_base, g_board.gicd_base, g_board.gic2_base };
    UINT64 s[3] = { g_board.uart_size, g_board.gicd_size, g_board.gic2_size };
    for (UINT32 i = 0; i < 3; ++i) {
      if (!b[i])
        continue;
      if (b[i] < lo)
        lo = b[i];
      if (b[i] + s[i] > hi)
        hi = b[i] + s[i];
    }
    if (hi) {
      g_board.soc_base = lo & ~0xFFFFFFULL;
      g_board.soc_size = (hi - g_board.soc_base + 0xFFFFFFULL) & ~0xFFFFFFULL;
    }
  }
  return TRUE;
}

#endif
