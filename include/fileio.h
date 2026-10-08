#ifndef FILEIO_H
#define FILEIO_H

#include "common.h"
#include "app.h"

/* A whole boot file through the boot environment: flags are BOOT_ENV_FILE_*,
 * *volume (optional) names where it came from. */
EFI_STATUS file_read(
    AppContext *ctx,
    CONST CHAR16 *path,
    UINT32 flags,
    FileBuffer *out_buf,
    VOID **volume);

VOID file_free(
    AppContext *ctx,
    FileBuffer *buf);

#endif
