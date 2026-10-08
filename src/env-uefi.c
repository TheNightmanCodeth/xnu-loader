/* The loader under UEFI firmware: efi_main fills a BootEnv from the boot
 * services and hands it to loader_main. On x86 the multiboot and BIOS entries
 * reach here too, through efi-emulation's stand-in firmware. */
#include "common.h"
#include "app.h"
#include "console.h"
#include <efi/efipxebc.h>
#if defined(__riscv)
#include "riscv_efi_boot.h"
#endif

extern EFI_STATUS loader_main(BootEnv *env);

static EFI_SYSTEM_TABLE *g_st;
static EFI_HANDLE g_image;
static BootEnv g_env;

static EFI_STATUS uefi_allocate_pages(EFI_ALLOCATE_TYPE type, EFI_MEMORY_TYPE kind, UINTN pages,
                                      EFI_PHYSICAL_ADDRESS *addr) {
  return uefi_call_wrapper(g_st->BootServices->AllocatePages, 4, type, kind, pages, addr);
}

static EFI_STATUS uefi_free_pages(EFI_PHYSICAL_ADDRESS addr, UINTN pages) {
  return uefi_call_wrapper(g_st->BootServices->FreePages, 2, addr, pages);
}

static EFI_STATUS uefi_allocate_pool(EFI_MEMORY_TYPE kind, UINTN size, VOID **out) {
  return uefi_call_wrapper(g_st->BootServices->AllocatePool, 3, kind, size, out);
}

static EFI_STATUS uefi_free_pool(VOID *p) {
  return uefi_call_wrapper(g_st->BootServices->FreePool, 1, p);
}

static EFI_STATUS uefi_memory_map(UINTN *size, EFI_MEMORY_DESCRIPTOR *map, UINTN *key,
                                  UINTN *desc_size, UINT32 *desc_version) {
  return uefi_call_wrapper(g_st->BootServices->GetMemoryMap, 5, size, map, key, desc_size,
                           desc_version);
}

static EFI_STATUS uefi_exit(UINTN key) {
  return uefi_call_wrapper(g_st->BootServices->ExitBootServices, 2, g_image, key);
}

static VOID uefi_console(CONST CHAR16 *text) {
  if (g_st->ConOut)
    uefi_call_wrapper(g_st->ConOut->OutputString, 2, g_st->ConOut, (CHAR16 *)text);
}

static VOID uefi_stall(UINTN usec) {
  uefi_call_wrapper(g_st->BootServices->Stall, 1, usec);
}

static EFI_STATUS file_get_size(EFI_FILE_PROTOCOL *file, UINTN *out_size) {
  EFI_STATUS status;
  UINTN info_size = SIZE_OF_EFI_FILE_INFO + 256;
  EFI_FILE_INFO *info = NULL;

  status = uefi_allocate_pool(EfiLoaderData, info_size, (VOID **)&info);
  if (EFI_ERROR(status))
    return status;

  status = uefi_call_wrapper(file->GetInfo, 4, file, &gEfiFileInfoGuid, &info_size, info);
  if (!EFI_ERROR(status))
    *out_size = (UINTN)info->FileSize;

  uefi_free_pool(info);
  return status;
}

static EFI_STATUS read_from_root(EFI_FILE_PROTOCOL *root, CONST CHAR16 *path, FileBuffer *out) {
  EFI_STATUS status;
  EFI_FILE_PROTOCOL *file = NULL;
  UINTN size = 0;
  VOID *buffer = NULL;
  UINTN read_size;

  out->data = NULL;
  out->size = 0;

  status = uefi_call_wrapper(root->Open, 5, root, &file, (CHAR16 *)path, EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR(status))
    return status;

  status = file_get_size(file, &size);
  if (EFI_ERROR(status))
    goto done;

  status = uefi_allocate_pool(EfiLoaderData, size, &buffer);
  if (EFI_ERROR(status))
    goto done;

  read_size = size;
  status = uefi_call_wrapper(file->Read, 3, file, &read_size, buffer);
  if (EFI_ERROR(status))
    goto done;

  out->data = buffer;
  out->size = read_size;
  buffer = NULL;

done:
  if (buffer)
    uefi_free_pool(buffer);
  if (file)
    uefi_call_wrapper(file->Close, 1, file);
  return status;
}

static EFI_STATUS read_from_volume(EFI_HANDLE volume, CONST CHAR16 *path, FileBuffer *out) {
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
  EFI_FILE_PROTOCOL *root = NULL;
  EFI_STATUS status;

  status = uefi_call_wrapper(g_st->BootServices->HandleProtocol, 3, volume,
                             &gEfiSimpleFileSystemProtocolGuid, (VOID **)&fs);
  if (EFI_ERROR(status))
    return status;
  status = uefi_call_wrapper(fs->OpenVolume, 2, fs, &root);
  if (EFI_ERROR(status))
    return status;
  status = read_from_root(root, path, out);
  uefi_call_wrapper(root->Close, 1, root);
  return status;
}

/* Netboot: fetch path (relative to the TFTP server root BOOTX64.EFI itself was
 * served from) through the PXE Base Code protocol the firmware installs when it
 * netbooted us; EFI_NOT_FOUND when it did not. */
static EFI_STATUS read_via_tftp(CONST CHAR16 *path, FileBuffer *out) {
  EFI_STATUS status;
  EFI_HANDLE *handles = NULL;
  UINTN handle_count = 0;
  EFI_PXE_BASE_CODE_PROTOCOL *pxe = NULL;
  EFI_IP_ADDRESS server_ip;
  UINT64 file_size = 0;
  VOID *buffer = NULL;
  CHAR8 filename[256];
  UINTN n = 0;

  // \EFI\BOOT\kernel is EFI/BOOT/kernel on the server
  while (*path == '\\')
    ++path;
  for (; path[n] && n < sizeof(filename) - 1; ++n)
    filename[n] = path[n] == '\\' ? '/' : (CHAR8)path[n];
  filename[n] = 0;

  status = uefi_call_wrapper(g_st->BootServices->LocateHandleBuffer, 5, ByProtocol,
                             &gEfiPxeBaseCodeProtocolGuid, NULL, &handle_count, &handles);
  if (EFI_ERROR(status))
    return EFI_NOT_FOUND;

  for (UINTN i = 0; i < handle_count && !pxe; i++) {
    EFI_PXE_BASE_CODE_PROTOCOL *candidate = NULL;
    if (EFI_ERROR(uefi_call_wrapper(g_st->BootServices->HandleProtocol, 3, handles[i],
                                    &gEfiPxeBaseCodeProtocolGuid, (VOID **)&candidate)))
      continue;
    pxe = candidate;
  }
  uefi_free_pool(handles);

  if (!pxe)
    return EFI_NOT_FOUND;

  if (!pxe->Mode->Started) {
    status = uefi_call_wrapper(pxe->Start, 2, pxe, FALSE);
    if (EFI_ERROR(status))
      return status;
  }

  if (!pxe->Mode->DhcpAckReceived) {
    /* Firmware netbooted us, so it already ran DHCP itself; if for some
     * reason no ack was captured there is no server IP to target. */
    return EFI_NOT_FOUND;
  }

  SetMem(&server_ip, sizeof(server_ip), 0);
  CopyMem(&server_ip.v4, pxe->Mode->DhcpAck.Dhcpv4.BootpSiAddr, 4);

  status = uefi_call_wrapper(pxe->Mtftp, 10, pxe, EFI_PXE_BASE_CODE_TFTP_GET_FILE_SIZE, NULL,
                             FALSE, &file_size, NULL, &server_ip, (UINT8 *)filename, NULL, FALSE);
  if (EFI_ERROR(status) || file_size == 0)
    return EFI_ERROR(status) ? status : EFI_NOT_FOUND;

  status = uefi_allocate_pool(EfiLoaderData, (UINTN)file_size, &buffer);
  if (EFI_ERROR(status))
    return status;

  status = uefi_call_wrapper(pxe->Mtftp, 10, pxe, EFI_PXE_BASE_CODE_TFTP_READ_FILE, buffer, FALSE,
                             &file_size, NULL, &server_ip, (UINT8 *)filename, NULL, FALSE);
  if (EFI_ERROR(status)) {
    uefi_free_pool(buffer);
    return status;
  }

  out->data = buffer;
  out->size = (UINTN)file_size;
  return EFI_SUCCESS;
}

static EFI_STATUS uefi_read_file(CONST CHAR16 *path, UINT32 flags, FileBuffer *out,
                                 VOID **volume) {
  EFI_STATUS status;
  EFI_HANDLE *handles = NULL;
  UINTN handle_count = 0;

  if (!path || !out)
    return EFI_INVALID_PARAMETER;
  out->data = NULL;
  out->size = 0;
  if (volume)
    *volume = NULL;

  if (flags & BOOT_ENV_FILE_NETWORK)
    return read_via_tftp(path, out);

  if (flags & BOOT_ENV_FILE_OWN_VOLUME) {
    EFI_LOADED_IMAGE *li = NULL;
    status = uefi_call_wrapper(g_st->BootServices->HandleProtocol, 3, g_image,
                               &LoadedImageProtocol, (VOID **)&li);
    if (EFI_ERROR(status))
      return status;
    status = read_from_volume(li->DeviceHandle, path, out);
    if (!EFI_ERROR(status) && volume)
      *volume = li->DeviceHandle;
    return status;
  }

  status = uefi_call_wrapper(g_st->BootServices->LocateHandleBuffer, 5, ByProtocol,
                             &gEfiSimpleFileSystemProtocolGuid, NULL, &handle_count, &handles);
  if (EFI_ERROR(status))
    return status;

  for (UINTN i = 0; i < handle_count; i++) {
    if (EFI_ERROR(read_from_volume(handles[i], path, out)))
      continue;
    if (volume)
      *volume = handles[i];
    uefi_free_pool(handles);
    return EFI_SUCCESS;
  }

  uefi_free_pool(handles);
  return EFI_NOT_FOUND;
}

/*
 * The firmware's GOP, settled on a linear mode. Not finding one - no GOP at
 * all, or only Blt-only modes - is not an error: XNU boots fine on the serial
 * console with v_display zeroed.
 */
static BOOLEAN uefi_framebuffer(CONST CHAR8 *cmdline, BootFramebuffer *out) {
  EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
  (VOID)cmdline;

  EFI_STATUS status = uefi_call_wrapper(g_st->BootServices->LocateProtocol, 3,
                                        &gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&gop);

  /* No GOP (headless firmware, no display attached, UGA-only board, etc.):
   * leave args->Video/VideoV1 zeroed (v_display=0) and continue - XNU boots
   * fine on the serial console alone (serial=3 in the boot-args cmdline)
   * without a framebuffer. */
  if (EFI_ERROR(status) || !gop || !gop->Mode || !gop->Mode->Info) {
    log_info(L"boot_probe_video: no usable GOP (%r), continuing headless\r\n", status);
    return FALSE;
  }

  /* XNU needs a LINEAR framebuffer.  The firmware's current GOP mode is
   * normally the native panel resolution with a linear framebuffer, so keep it
   * when it is RGBX(0)/BGRX(1).  Only if the current mode is BltOnly/bitmask
   * (no CPU-addressable framebuffer) do we search for the highest-resolution
   * linear mode and switch to it. */
  {
    EFI_GRAPHICS_PIXEL_FORMAT curfmt = gop->Mode->Info->PixelFormat;
    BOOLEAN cur_linear =
        (curfmt == PixelRedGreenBlueReserved8BitPerColor ||
         curfmt == PixelBlueGreenRedReserved8BitPerColor);

    if (!cur_linear) {
      UINT32 best_mode = gop->Mode->MaxMode; /* sentinel = none found */
      UINT64 best_px   = 0;
      for (UINT32 m = 0; m < gop->Mode->MaxMode; m++) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mi = NULL;
        UINTN misz = 0;
        if (EFI_ERROR(uefi_call_wrapper(gop->QueryMode, 4, gop, m, &misz, &mi)) || !mi)
          continue;
        if (mi->PixelFormat != PixelRedGreenBlueReserved8BitPerColor &&
            mi->PixelFormat != PixelBlueGreenRedReserved8BitPerColor)
          continue;
        UINT64 px = (UINT64)mi->HorizontalResolution * mi->VerticalResolution;
        if (px > best_px) {
          best_px   = px;
          best_mode = m;
        }
      }
      if (best_mode == gop->Mode->MaxMode) {
        log_info(L"boot_probe_video: current mode not linear and no linear mode "
                 L"found, continuing headless\r\n");
        return FALSE;
      }
      log_info(L"boot_probe_video: current mode not linear, switching to mode %u\r\n",
               best_mode);
      status = uefi_call_wrapper(gop->SetMode, 2, gop, best_mode);
      if (EFI_ERROR(status)) {
        log_info(L"boot_probe_video: SetMode(%u) failed: %r, continuing headless\r\n",
                 best_mode, status);
        return FALSE;
      }
    }
  }

  out->base = gop->Mode->FrameBufferBase;
  out->width = gop->Mode->Info->HorizontalResolution;
  out->height = gop->Mode->Info->VerticalResolution;
  out->pixels_per_scanline = gop->Mode->Info->PixelsPerScanLine;
  return TRUE;
}

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st) {
  if (!st || st->Hdr.Signature != EFI_SYSTEM_TABLE_SIGNATURE)
    return EFI_INVALID_PARAMETER;

  InitializeLib(image, st);
  g_st = st;
  g_image = image;

  g_env.allocate_pages = uefi_allocate_pages;
  g_env.free_pages = uefi_free_pages;
  g_env.allocate_pool = uefi_allocate_pool;
  g_env.free_pool = uefi_free_pool;
  g_env.memory_map = uefi_memory_map;
  g_env.exit = uefi_exit;
  g_env.read_file = uefi_read_file;
  g_env.framebuffer = uefi_framebuffer;
  g_env.console = uefi_console;
  g_env.stall = uefi_stall;
  g_env.config_tables = st->ConfigurationTable;
  g_env.config_table_count = st->NumberOfTableEntries;
  g_env.firmware_vendor = st->FirmwareVendor;
  g_env.firmware_revision = st->FirmwareRevision;
  g_env.system_table = st;
  g_env.runtime = st->RuntimeServices;
  g_env.firmware = st->BootServices;
  g_env.image = image;

  {
    EFI_LOADED_IMAGE *li = NULL;
    if (!EFI_ERROR(uefi_call_wrapper(st->BootServices->HandleProtocol, 3, image,
                                     &LoadedImageProtocol, (VOID **)&li))) {
      g_env.loader_base = (UINT64)(UINTN)li->ImageBase;
      g_env.loader_size = li->ImageSize;
    }
  }

#if defined(__riscv)
  {
    static EFI_GUID guid = RISCV_EFI_BOOT_PROTOCOL_GUID;
    RISCV_EFI_BOOT_PROTOCOL *boot = NULL;
    UINTN hartid = 0;
    g_env.boot_hartid = ~0ULL;
    if (!EFI_ERROR(uefi_call_wrapper(st->BootServices->LocateProtocol, 3, &guid, NULL,
                                     (VOID **)&boot)) && boot &&
        !EFI_ERROR(uefi_call_wrapper(boot->GetBootHartId, 2, boot, &hartid)))
      g_env.boot_hartid = hartid;
  }
#endif

  return loader_main(&g_env);
}
