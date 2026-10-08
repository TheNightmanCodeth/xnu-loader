#include "app.h"
#include "console.h"
#include "serial.h"
#include "platform.h"
#include "fdt.h"

// the tree the firmware or boot protocol handed over, checked once here for every reader
static CONST VOID *env_fdt(BootEnv *env) {
  static EFI_GUID dtb_guid = { 0xb1b621d5, 0xf19c, 0x41a5, { 0x83, 0x0b, 0xd9, 0x15, 0x2c, 0x69, 0xaa, 0xe0 } };

  for (UINTN i = 0; i < env->config_table_count; i++) {
    EFI_CONFIGURATION_TABLE *e = &env->config_tables[i];
    if (!CompareMem(&e->VendorGuid, &dtb_guid, sizeof(EFI_GUID)))
      return fdt_check(e->VendorTable) ? e->VendorTable : NULL;
  }
  return NULL;
}

EFI_STATUS app_init(AppContext *ctx, BootEnv *env) {
  if (!ctx || !env)
    return EFI_INVALID_PARAMETER;

  // ensure there's no leftover data. this caused a bug on some machines
  // to falsely report having a RAMDisk when it did not.
  SetMem(ctx, sizeof(*ctx), 0);

  ctx->env = env;
  console_attach(env);
  ctx->fdt = env_fdt(env);
#if defined(__riscv)
  ctx->boot_hartid = env->boot_hartid;
#endif

#if defined(__aarch64__) && defined(XNU_LOADER_PLATFORM_GENERIC)
  // the console and the rest of the board come from the firmware's device tree, else the build's defaults
  fdt_board_parse(ctx->fdt);
#endif

  /* Bring up the serial console before any logging so serial captures the full
   * boot on real hardware (where there is no EFI console to read). */
  serial_init();

  return EFI_SUCCESS;
}

EFI_STATUS app_alloc_pool(AppContext *ctx, UINTN size, VOID **ptr) {
  return ctx->env->allocate_pool(EfiLoaderData, size, ptr);
}

VOID app_free_pool(AppContext *ctx, VOID *ptr) {
  if (ptr)
    ctx->env->free_pool(ptr);
}

UINT64 app_detect_physical_memory_size(AppContext *ctx) {
  UINTN map_size = 0, key = 0, desc_size = 0;
  UINT32 desc_ver = 0;
  EFI_MEMORY_DESCRIPTOR *mm = NULL;
  UINT64 total = 0;

  ctx->env->memory_map(&map_size, mm, &key, &desc_size, &desc_ver);
  map_size += desc_size * 4;

  EFI_STATUS s = ctx->env->allocate_pool(EfiLoaderData, map_size, (VOID **)&mm);
  if (EFI_ERROR(s) || !mm)
    return 0;

  s = ctx->env->memory_map(&map_size, mm, &key, &desc_size, &desc_ver);
  if (EFI_ERROR(s)) {
    ctx->env->free_pool(mm);
    return 0;
  }

  UINTN n = map_size / desc_size;
  for (UINTN i = 0; i < n; i++) {
    EFI_MEMORY_DESCRIPTOR *d =
        (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)mm + i * desc_size);
    if (d->Type == EfiLoaderCode         ||
        d->Type == EfiLoaderData         ||
        d->Type == EfiBootServicesCode   ||
        d->Type == EfiBootServicesData   ||
        d->Type == EfiRuntimeServicesCode ||
        d->Type == EfiRuntimeServicesData ||
        d->Type == EfiConventionalMemory ||
        d->Type == EfiACPIReclaimMemory  ||
        d->Type == EfiACPIMemoryNVS      ||
        d->Type == EfiPalCode) {
      total += d->NumberOfPages << EFI_PAGE_SHIFT;
    }
  }

  ctx->env->free_pool(mm);
  return total;
}
