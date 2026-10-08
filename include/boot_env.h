#ifndef XNU_LOADER_BOOT_ENV_H
#define XNU_LOADER_BOOT_ENV_H

/*
 * What the loader needs from whatever started it. UEFI firmware fills this in
 * from its boot services (src/env-uefi.c); a Linux Image entry fills it in from
 * the FDT and the initrd with no firmware at all (kernel/env-booti.c).
 *
 * Memory keeps UEFI's allocation types, memory types and descriptor layout,
 * since that map is what XNU is handed. Everything else is plain C: no handles,
 * no protocols, no system table unless a firmware really is there.
 */

#include <efi.h>

typedef struct FileBuffer {
  VOID *data;
  UINTN size;
} FileBuffer;

// read_file flags
#define BOOT_ENV_FILE_OWN_VOLUME 1   // only the volume the loader itself came from
#define BOOT_ENV_FILE_NETWORK    2   // over TFTP instead, when the firmware netbooted us

// the linear 32-bit framebuffer the kernel inherits
typedef struct {
  UINT64 base;
  UINT32 width, height;
  UINT32 pixels_per_scanline;
} BootFramebuffer;

typedef struct BootEnv {
  EFI_STATUS (*allocate_pages)(EFI_ALLOCATE_TYPE type, EFI_MEMORY_TYPE kind, UINTN pages,
                               EFI_PHYSICAL_ADDRESS *addr);
  EFI_STATUS (*free_pages)(EFI_PHYSICAL_ADDRESS addr, UINTN pages);
  EFI_STATUS (*allocate_pool)(EFI_MEMORY_TYPE kind, UINTN size, VOID **out);
  EFI_STATUS (*free_pool)(VOID *p);
  // GetMemoryMap's contract: EFI_BUFFER_TOO_SMALL with *size set when map is short
  EFI_STATUS (*memory_map)(UINTN *size, EFI_MEMORY_DESCRIPTOR *map, UINTN *key,
                           UINTN *desc_size, UINT32 *desc_version);
  // after this only memory the loader owns is touched; key is the last memory map's
  EFI_STATUS (*exit)(UINTN key);

  // a whole boot file into pool memory, path as L"\\EFI\\BOOT\\kernel"; *volume (when
  // asked for) names where it came from, NULL without firmware volumes
  EFI_STATUS (*read_file)(CONST CHAR16 *path, UINT32 flags, FileBuffer *out, VOID **volume);
  // FALSE when there is no linear framebuffer, which is not an error
  BOOLEAN (*framebuffer)(CONST CHAR8 *cmdline, BootFramebuffer *out);
  VOID (*console)(CONST CHAR16 *text);
  VOID (*stall)(UINTN usec);

  // tables the firmware or the boot protocol handed over (ACPI, SMBIOS, the DTB),
  // spelled as UEFI configuration-table entries
  EFI_CONFIGURATION_TABLE *config_tables;
  UINTN config_table_count;
  CONST CHAR16 *firmware_vendor;
  UINT32 firmware_revision;
  // where the loader itself sits, for the log
  UINT64 loader_base, loader_size;
#if defined(__riscv)
  // the hart that booted, ~0 when nothing says
  UINT64 boot_hartid;
#endif

  // only with UEFI firmware underneath, NULL otherwise: its system table (x86 XNU
  // is handed a copy), its runtime services, and its boot services for what only
  // firmware has (disks, an RNG, device paths)
  EFI_SYSTEM_TABLE *system_table;
  EFI_RUNTIME_SERVICES *runtime;
  EFI_BOOT_SERVICES *firmware;
  EFI_HANDLE image;
} BootEnv;

#endif
