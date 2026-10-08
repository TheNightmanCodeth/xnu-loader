/* UnicodeVSPrint for the loader started without UEFI firmware (the Linux Image,
 * multiboot and BIOS entries): links ahead of gnu-efi's, so log lines are
 * formatted the same whatever started the loader. */
#include <efi.h>
#include <efilib.h>

static void format_char(CHAR16 **cursor, CHAR16 *end, CHAR16 value) {
  if (*cursor < end)
    *(*cursor)++ = value;
}

static void format_unsigned(CHAR16 **cursor, CHAR16 *end, UINT64 value,
                            UINTN base, BOOLEAN uppercase, UINTN width,
                            CHAR16 padding) {
  CHAR16 digits[24];
  UINTN length = 0;

  do {
    UINTN digit = value % base;
    digits[length++] = digit < 10
                           ? (CHAR16)('0' + digit)
                           : (CHAR16)((uppercase ? 'A' : 'a') + digit - 10);
    value /= base;
  } while (value && length < sizeof(digits) / sizeof(digits[0]));

  while (width > length) {
    format_char(cursor, end, padding);
    --width;
  }
  while (length)
    format_char(cursor, end, digits[--length]);
}

UINTN UnicodeVSPrint(CHAR16 *buffer, UINTN buffer_size, CONST CHAR16 *format,
                     va_list args) {
  if (!buffer || buffer_size < sizeof(CHAR16) || !format)
    return 0;

  CHAR16 *cursor = buffer;
  CHAR16 *end = buffer + buffer_size / sizeof(CHAR16) - 1;

  while (*format && cursor < end) {
    if (*format != '%') {
      format_char(&cursor, end, *format++);
      continue;
    }

    ++format;
    if (*format == '%') {
      format_char(&cursor, end, *format++);
      continue;
    }

    BOOLEAN left = FALSE;
    CHAR16 padding = ' ';
    while (*format == '-' || *format == '0') {
      if (*format == '-')
        left = TRUE;
      else
        padding = '0';
      ++format;
    }

    UINTN width = 0;
    while (*format >= '0' && *format <= '9')
      width = width * 10 + (*format++ - '0');

    UINTN precision = ~(UINTN)0;
    if (*format == '.') {
      precision = 0;
      ++format;
      while (*format >= '0' && *format <= '9')
        precision = precision * 10 + (*format++ - '0');
    }

    BOOLEAN wide = FALSE;
    if (*format == 'l') {
      wide = TRUE;
      ++format;
    }

    CHAR16 specifier = *format++;
    if (specifier == 'a' || specifier == 's') {
      CONST CHAR8 *ascii = NULL;
      CONST CHAR16 *unicode = NULL;
      UINTN length = 0;

      if (specifier == 'a') {
        ascii = va_arg(args, CONST CHAR8 *);
        while (ascii && ascii[length] && length < precision)
          ++length;
      } else {
        unicode = va_arg(args, CONST CHAR16 *);
        while (unicode && unicode[length] && length < precision)
          ++length;
      }

      if (!left)
        while (width > length) {
          format_char(&cursor, end, ' ');
          --width;
        }
      for (UINTN i = 0; i < length; ++i)
        format_char(&cursor, end,
                    specifier == 'a' ? (UINT8)ascii[i] : unicode[i]);
      if (left)
        while (width > length) {
          format_char(&cursor, end, ' ');
          --width;
        }
      continue;
    }

    if (specifier == 'c') {
      format_char(&cursor, end, (CHAR16)va_arg(args, UINTN));
      continue;
    }

    if (specifier == 'r') {
      format_char(&cursor, end, '0');
      format_char(&cursor, end, 'x');
      format_unsigned(&cursor, end, va_arg(args, EFI_STATUS), 16, FALSE, width,
                      padding);
      continue;
    }

    if (specifier == 'p') {
      UINT64 value = (UINT64)(UINTN)va_arg(args, VOID *);
      format_char(&cursor, end, '0');
      format_char(&cursor, end, 'x');
      format_unsigned(&cursor, end, value, 16, FALSE,
                      width ? width : sizeof(VOID *) * 2, '0');
      continue;
    }

    if (specifier == 'x' || specifier == 'X' || specifier == 'u' ||
        specifier == 'd') {
      UINT64 value;
      BOOLEAN negative = FALSE;

      if (specifier == 'd') {
        INT64 signed_value = wide ? va_arg(args, INT64) : va_arg(args, INT32);
        if (signed_value < 0) {
          negative = TRUE;
          signed_value = -signed_value;
        }
        value = (UINT64)signed_value;
      } else {
        value = wide ? va_arg(args, UINT64) : va_arg(args, UINT32);
      }

      if (negative) {
        format_char(&cursor, end, '-');
        if (width)
          --width;
      }
      format_unsigned(&cursor, end, value,
                      specifier == 'u' || specifier == 'd' ? 10 : 16,
                      specifier == 'X', width, padding);
      continue;
    }

    format_char(&cursor, end, '?');
  }

  *cursor = 0;
  return (UINTN)(cursor - buffer);
}
