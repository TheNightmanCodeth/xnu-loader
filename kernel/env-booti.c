/* The loader started as a Linux Image (booti, QEMU -kernel, OpenSBI) with no
 * firmware underneath: memory is the FDT's RAM less its reservations, files are
 * the initrd's, and the console is the UART. Shared by the arm64 and riscv64
 * entries, which fill EfiEmuBootInfo from the FDT and call kernel_boot. */
#include "efi_emulation.h"
#include "common.h"
#include "serial.h"

extern EFI_STATUS loader_main(BootEnv *env);
extern UINT8 __kernel_start;
extern UINT8 __kernel_end;

#define MAX_RANGES 192
#define PAGE 4096ULL

static BootEnv env;
static EfiEmuBootInfo *boot;

/* The memory map: sorted, with touching ranges of one type merged, since every
 * pool allocation is a page run and the loader's device tree alone makes
 * hundreds of them */
static EFI_MEMORY_DESCRIPTOR ranges[MAX_RANGES];
static UINTN nranges;
static UINTN map_key = 1;

static void sort_ranges(void) {
  for (UINTN i = 1; i < nranges; ++i) {
    EFI_MEMORY_DESCRIPTOR v = ranges[i];
    UINTN j = i;
    while (j && ranges[j - 1].PhysicalStart > v.PhysicalStart) {
      ranges[j] = ranges[j - 1];
      --j;
    }
    ranges[j] = v;
  }
  UINTN out = 0;
  for (UINTN i = 0; i < nranges; ++i) {
    if (out) {
      EFI_MEMORY_DESCRIPTOR *p = &ranges[out - 1];
      if (p->Type == ranges[i].Type && p->Attribute == ranges[i].Attribute &&
          p->PhysicalStart + p->NumberOfPages * PAGE == ranges[i].PhysicalStart) {
        p->NumberOfPages += ranges[i].NumberOfPages;
        continue;
      }
    }
    ranges[out++] = ranges[i];
  }
  nranges = out;
}

/* Carve [base, base + pages) out of free memory as type */
static EFI_STATUS reserve_range(EFI_PHYSICAL_ADDRESS base, UINTN pages, EFI_MEMORY_TYPE type) {
  UINT64 end = base + pages * PAGE;
  for (UINTN i = 0; i < nranges; ++i) {
    UINT64 rb = ranges[i].PhysicalStart;
    UINT64 re = rb + ranges[i].NumberOfPages * PAGE;
    if (ranges[i].Type != EfiConventionalMemory || base < rb || end > re)
      continue;
    if (nranges + 2 >= MAX_RANGES)
      return EFI_OUT_OF_RESOURCES;
    EFI_MEMORY_DESCRIPTOR old = ranges[i];
    ranges[i].PhysicalStart = base;
    ranges[i].NumberOfPages = pages;
    ranges[i].Type = type;
    ranges[i].Attribute = 0;
    if (base > rb) {
      ranges[nranges] = old;
      ranges[nranges].NumberOfPages = (base - rb) / PAGE;
      ++nranges;
    }
    if (end < re) {
      ranges[nranges] = old;
      ranges[nranges].PhysicalStart = end;
      ranges[nranges].NumberOfPages = (re - end) / PAGE;
      ++nranges;
    }
    sort_ranges();
    ++map_key;
    return EFI_SUCCESS;
  }
  return EFI_NOT_FOUND;
}

/* Like UEFI's: AllocateAddress takes exactly the pages asked for, anything else
 * the highest free run below the limit (4GB, or a lower AllocateMaxAddress) */
static EFI_STATUS booti_allocate_pages(EFI_ALLOCATE_TYPE type, EFI_MEMORY_TYPE kind, UINTN pages,
                                       EFI_PHYSICAL_ADDRESS *addr) {
  if (!addr || !pages)
    return EFI_INVALID_PARAMETER;
  if (type == AllocateAddress)
    return reserve_range(*addr, pages, kind);
  UINT64 limit = 0xffffffffULL;
  if (type == AllocateMaxAddress && *addr < limit)
    limit = *addr;
  for (UINTN n = nranges; n-- > 0;) {
    if (ranges[n].Type != EfiConventionalMemory)
      continue;
    UINT64 rb = ranges[n].PhysicalStart;
    UINT64 re = rb + ranges[n].NumberOfPages * PAGE;
    if (re > limit + 1)
      re = (limit + 1) & ~(PAGE - 1);
    if (re < rb + pages * PAGE)
      continue;
    UINT64 base = re - pages * PAGE;
    EFI_STATUS s = reserve_range(base, pages, kind);
    if (!EFI_ERROR(s))
      *addr = base;
    return s;
  }
  return EFI_OUT_OF_RESOURCES;
}

/* Like UEFI's, any page run inside one allocation can be freed, splitting it */
static EFI_STATUS booti_free_pages(EFI_PHYSICAL_ADDRESS addr, UINTN pages) {
  UINT64 end = addr + pages * PAGE;
  for (UINTN i = 0; i < nranges; ++i) {
    UINT64 rb = ranges[i].PhysicalStart;
    UINT64 re = rb + ranges[i].NumberOfPages * PAGE;
    if (ranges[i].Type == EfiConventionalMemory || addr < rb || end > re)
      continue;
    if (nranges + 2 >= MAX_RANGES)
      return EFI_OUT_OF_RESOURCES;
    EFI_MEMORY_DESCRIPTOR old = ranges[i];
    ranges[i].PhysicalStart = addr;
    ranges[i].NumberOfPages = pages;
    ranges[i].Type = EfiConventionalMemory;
    ranges[i].Attribute = 0;
    if (addr > rb) {
      ranges[nranges] = old;
      ranges[nranges].NumberOfPages = (addr - rb) / PAGE;
      ++nranges;
    }
    if (end < re) {
      ranges[nranges] = old;
      ranges[nranges].PhysicalStart = end;
      ranges[nranges].NumberOfPages = (re - end) / PAGE;
      ++nranges;
    }
    sort_ranges();
    ++map_key;
    return EFI_SUCCESS;
  }
  return EFI_NOT_FOUND;
}

static EFI_STATUS booti_memory_map(UINTN *size, EFI_MEMORY_DESCRIPTOR *map, UINTN *key,
                                   UINTN *desc_size, UINT32 *desc_version) {
  if (!size || !key || !desc_size || !desc_version)
    return EFI_INVALID_PARAMETER;
  UINTN needed = nranges * sizeof(EFI_MEMORY_DESCRIPTOR);
  *desc_size = sizeof(EFI_MEMORY_DESCRIPTOR);
  *desc_version = 1;
  if (!map || *size < needed) {
    *size = needed;
    return EFI_BUFFER_TOO_SMALL;
  }
  for (UINTN i = 0; i < nranges; ++i)
    map[i] = ranges[i];
  *size = needed;
  *key = map_key;
  return EFI_SUCCESS;
}

typedef struct {
  UINTN pages;
  UINT64 magic;
} PoolHeader;
#define POOL_MAGIC 0x504f4f4c

static EFI_STATUS booti_allocate_pool(EFI_MEMORY_TYPE kind, UINTN size, VOID **out) {
  if (!out)
    return EFI_INVALID_PARAMETER;
  UINTN pages = (size + sizeof(PoolHeader) + PAGE - 1) / PAGE;
  EFI_PHYSICAL_ADDRESS addr = ~0ULL;
  EFI_STATUS s = booti_allocate_pages(AllocateMaxAddress, kind, pages, &addr);
  if (EFI_ERROR(s))
    return s;
  PoolHeader *h = (PoolHeader *)(UINTN)addr;
  h->pages = pages;
  h->magic = POOL_MAGIC;
  *out = h + 1;
  return EFI_SUCCESS;
}

static EFI_STATUS booti_free_pool(VOID *p) {
  if (!p)
    return EFI_INVALID_PARAMETER;
  PoolHeader *h = (PoolHeader *)p - 1;
  if (h->magic != POOL_MAGIC)
    return EFI_INVALID_PARAMETER;
  return booti_free_pages((EFI_PHYSICAL_ADDRESS)(UINTN)h, h->pages);
}

/* Nothing to leave: the map the loader read last is still the map */
static EFI_STATUS booti_exit(UINTN key) {
  return key == map_key ? EFI_SUCCESS : EFI_INVALID_PARAMETER;
}

/* True when [base, end) already lies inside one range of this type */
static BOOLEAN range_has_type(UINT64 base, UINT64 end, EFI_MEMORY_TYPE type) {
  for (UINTN i = 0; i < nranges; ++i) {
    UINT64 rb = ranges[i].PhysicalStart;
    UINT64 re = rb + ranges[i].NumberOfPages * PAGE;
    if (ranges[i].Type == type && base >= rb && end <= re)
      return TRUE;
  }
  return FALSE;
}

static void reserve_bytes(UINT64 base, UINT64 size, EFI_MEMORY_TYPE type) {
  if (!size)
    return;
  UINT64 b = base & ~(PAGE - 1);
  UINT64 e = (base + size + PAGE - 1) & ~(PAGE - 1);
  /* Files packed in one initrd share edge pages that are already reserved */
  if (e - b > PAGE && range_has_type(b, b + PAGE, type))
    b += PAGE;
  if (e - b > PAGE && range_has_type(e - PAGE, e, type))
    e -= PAGE;
  if (range_has_type(b, e, type))
    return;
  if (EFI_ERROR(reserve_range(b, (e - b) / PAGE, type))) {
    efiemu_debug_string("booti: could not reserve ");
    efiemu_debug_hex(b);
    efiemu_debug_string("\n");
  }
}

static void initialize_ranges(void) {
  nranges = 0;
  for (UINT32 i = 0; i < boot->memory_count && nranges < MAX_RANGES; ++i) {
    EfiEmuMemoryRange *m = &boot->memory[i];
    UINT64 b = (m->base + PAGE - 1) & ~(PAGE - 1);
    UINT64 e = (m->base + m->length) & ~(PAGE - 1);
    if (e <= b)
      continue;
    EFI_MEMORY_DESCRIPTOR *d = &ranges[nranges++];
    SetMem(d, sizeof(*d), 0);
    d->PhysicalStart = b;
    d->NumberOfPages = (e - b) / PAGE;
    d->Type = m->type == EfiEmuMemoryUsable ? EfiConventionalMemory : EfiReservedMemoryType;
  }
  sort_ranges();

  /* Page zero stays unallocatable so a null pointer never looks valid */
  reserve_range(0, 1, EfiReservedMemoryType);

  /* The loader itself, with the stack and page tables it brought */
  UINT64 loader_begin = (UINT64)(UINTN)&__kernel_start;
  UINT64 loader_end = ((UINT64)(UINTN)&__kernel_end + PAGE - 1) & ~(PAGE - 1);
  reserve_range(loader_begin, (loader_end - loader_begin) / PAGE, EfiLoaderCode);

  for (UINT32 i = 0; i < boot->module_count; ++i)
    reserve_bytes(boot->modules[i].start, boot->modules[i].size, EfiLoaderData);
  if (boot->fdt)
    reserve_bytes(boot->fdt, boot->fdt_size, EfiACPIReclaimMemory);
}

/* Files are the initrd's: paths match ignoring case, slash direction and
 * leading separators, so the module EFI/BOOT/kernel opens as \EFI\BOOT\kernel */
static CHAR16 fold(CHAR16 c) {
  if (c == '/')
    return '\\';
  if (c >= 'A' && c <= 'Z')
    return c + 32;
  return c;
}

static BOOLEAN path_equal(CONST CHAR16 *wide, CONST CHAR8 *narrow) {
  while (*wide == '\\' || *wide == '/')
    ++wide;
  while (*narrow == '\\' || *narrow == '/')
    ++narrow;
  for (;; ++wide, ++narrow) {
    CHAR16 a = fold(*wide), b = fold((UINT8)*narrow);
    if (a != b)
      return FALSE;
    if (a == 0)
      return TRUE;
  }
}

static CHAR8 cmdline_copy[1024];

static EFI_STATUS booti_read_file(CONST CHAR16 *path, UINT32 flags, FileBuffer *out,
                                  VOID **volume) {
  if (!path || !out)
    return EFI_INVALID_PARAMETER;
  out->data = NULL;
  out->size = 0;
  if (volume)
    *volume = NULL;
  if (flags & BOOT_ENV_FILE_NETWORK)
    return EFI_NOT_FOUND;

  for (UINT32 i = 0; i < boot->module_count; ++i) {
    EfiEmuModule *m = &boot->modules[i];
    if (!m->name || !path_equal(path, m->name))
      continue;
    VOID *data = NULL;
    EFI_STATUS s = booti_allocate_pool(EfiLoaderData, m->size, &data);
    if (EFI_ERROR(s))
      return s;
    CopyMem(data, (VOID *)(UINTN)m->start, m->size);
    out->data = data;
    out->size = m->size;
    return EFI_SUCCESS;
  }
  return EFI_NOT_FOUND;
}

static BOOLEAN booti_framebuffer(CONST CHAR8 *cmdline, BootFramebuffer *out) {
  EfiEmuFramebuffer *fb = &boot->framebuffer;
  (VOID)cmdline;
  if (!fb->valid || fb->bits_per_pixel != 32)
    return FALSE;
  out->base = fb->base;
  out->width = fb->width;
  out->height = fb->height;
  out->pixels_per_scanline = fb->pixels_per_scanline;
  return TRUE;
}

static VOID booti_console(CONST CHAR16 *text) {
  CHAR8 line[128];
  UINTN n = 0;
  for (; *text; ++text) {
    line[n++] = *text > 0x7f ? '?' : (CHAR8)*text;
    if (n == sizeof(line) - 1) {
      line[n] = 0;
      serial_puts8(line);
      n = 0;
    }
  }
  line[n] = 0;
  if (n)
    serial_puts8(line);
}

static VOID booti_stall(UINTN usec) {
  UINT64 freq, start, now;
#if defined(__aarch64__)
  __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
  __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(start));
#else
  // qemu virt ticks at 10 MHz, used when the fdt has no timebase
  freq = efiemu_riscv_timebase ? efiemu_riscv_timebase : 10000000ULL;
  __asm__ volatile("rdtime %0" : "=r"(start));
#endif
  UINT64 ticks = freq / 1000000ULL * usec + (freq % 1000000ULL) * usec / 1000000ULL;
  do {
#if defined(__aarch64__)
    __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(now));
#else
    __asm__ volatile("rdtime %0" : "=r"(now));
#endif
  } while (now - start < ticks);
}

static EFI_CONFIGURATION_TABLE config_tables[1];

void kernel_boot(EfiEmuBootInfo *info) {
  static EFI_GUID dtb_guid = { 0xb1b621d5, 0xf19c, 0x41a5,
                               { 0x83, 0x0b, 0xd9, 0x15, 0x2c, 0x69, 0xaa, 0xe0 } };
  BOOLEAN have_args = FALSE;

  boot = info;
  initialize_ranges();

  /* No boot-args.txt in the initrd: the kernel command line is the boot-args */
  for (UINT32 i = 0; i < info->module_count; ++i) {
    if (info->modules[i].name && path_equal(L"\\EFI\\BOOT\\boot-args.txt", info->modules[i].name))
      have_args = TRUE;
  }
  if (!have_args && info->cmdline && info->cmdline[0] && info->module_count < EFIEMU_MAX_MODULES) {
    UINTN n = 0;
    while (info->cmdline[n] && n < sizeof(cmdline_copy) - 1) {
      cmdline_copy[n] = info->cmdline[n];
      ++n;
    }
    cmdline_copy[n] = 0;
    EfiEmuModule *m = &info->modules[info->module_count++];
    m->start = (UINT64)(UINTN)cmdline_copy;
    m->size = n;
    m->name = (CONST CHAR8 *)"/EFI/BOOT/boot-args.txt";
  }
  for (UINT32 i = 0; i < info->module_count; ++i) {
    efiemu_debug_string("booti: file ");
    efiemu_debug_string((const char *)info->modules[i].name);
    efiemu_debug_string(" at ");
    efiemu_debug_hex(info->modules[i].start);
    efiemu_debug_string(" size ");
    efiemu_debug_hex(info->modules[i].size);
    efiemu_debug_string("\n");
  }

  env.allocate_pages = booti_allocate_pages;
  env.free_pages = booti_free_pages;
  env.allocate_pool = booti_allocate_pool;
  env.free_pool = booti_free_pool;
  env.memory_map = booti_memory_map;
  env.exit = booti_exit;
  env.read_file = booti_read_file;
  env.framebuffer = booti_framebuffer;
  env.console = booti_console;
  env.stall = booti_stall;
  if (info->fdt) {
    config_tables[0].VendorGuid = dtb_guid;
    config_tables[0].VendorTable = (VOID *)(UINTN)info->fdt;
    env.config_tables = config_tables;
    env.config_table_count = 1;
  }
  env.firmware_vendor = L"xnu-loader";
  env.loader_base = (UINT64)(UINTN)&__kernel_start;
  env.loader_size = (UINT64)(UINTN)(&__kernel_end - &__kernel_start);
#if defined(__riscv)
  env.boot_hartid = efiemu_riscv_hartid;
#endif

  efiemu_debug_string(info->framebuffer.valid ? "booti: framebuffer from the device tree\n"
                                              : "booti: no framebuffer, serial console only\n");
  efiemu_debug_string("booti: entering the loader, no firmware underneath\n");
  loader_main(&env);
  efiemu_debug_string("booti: loader returned\n");
  for (;;)
#if defined(__aarch64__)
    __asm__ volatile("msr daifset, #0xf; wfi");
#else
    __asm__ volatile("csrw sie, zero; wfi");
#endif
}
