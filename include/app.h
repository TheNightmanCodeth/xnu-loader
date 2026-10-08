#ifndef APP_H
#define APP_H

#include "common.h"
#include "uefi/smbios.h"

EFI_STATUS app_init(AppContext *ctx, BootEnv *env);
EFI_STATUS app_alloc_pool(AppContext *ctx, UINTN size, VOID **ptr);
VOID app_free_pool(AppContext *ctx, VOID *ptr);
uint64_t app_detect_physical_memory_size(AppContext *ctx);

#endif