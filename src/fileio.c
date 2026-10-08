#include "fileio.h"
#include "app.h"

EFI_STATUS file_read(
    AppContext *ctx,
    CONST CHAR16 *path,
    UINT32 flags,
    FileBuffer *out_buf,
    VOID **volume) {
  if (!ctx || !path || !out_buf)
    return EFI_INVALID_PARAMETER;
  return ctx->env->read_file(path, flags, out_buf, volume);
}

VOID file_free(AppContext *ctx, FileBuffer *buf) {
  if (!buf)
    return;
  if (buf->data)
    app_free_pool(ctx, buf->data);
  buf->data = NULL;
  buf->size = 0;
}
