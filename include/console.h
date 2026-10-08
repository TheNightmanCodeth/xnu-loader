#ifndef CONSOLE_H
#define CONSOLE_H

#include "common.h"

// log lines go to env's console from here on
VOID console_attach(BootEnv *env);
VOID log_info(CONST CHAR16 *fmt, ...);
VOID log_error(CONST CHAR16 *fmt, ...);

#endif