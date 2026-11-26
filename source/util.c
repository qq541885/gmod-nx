/* util.c -- misc utility functions
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>

#include "util.h"
#include "config.h"

#if DEBUG_LOG

static int s_nxlinkSock = -1;

static void initNxLink(void) {
  if (R_FAILED(socketInitializeDefault()))
    return;
  s_nxlinkSock = nxlinkStdio();
  if (s_nxlinkSock < 0)
    socketExit();
}

static void deinitNxLink(void) {
  if (s_nxlinkSock >= 0) {
    close(s_nxlinkSock);
    socketExit();
    s_nxlinkSock = -1;
  }
}

void userAppInit(void) {
  initNxLink();
}

void userAppExit(void) {
  deinitNxLink();
}

#endif

/* Kept open for the whole run. fopen/fclose per line cost a full filesystem
 * round-trip each; with ~4000 constructor lines that alone dominated boot
 * time on the SD card. fflush still forces the bytes out so a crash cannot
 * swallow the tail of the trace. */
static FILE *s_log = NULL;

int debugPrintf(const char *text, ...) {
#if DEBUG_LOG
  va_list list;

  if (s_log == NULL)
    s_log = fopen(LOG_NAME, "a");

  if (s_log != NULL) {
    va_start(list, text);
    vfprintf(s_log, text, list);
    va_end(list);
    fflush(s_log);
  }

  /* Only mirror to the host when nxlink is actually connected; an
   * unconnected socket makes vprintf block on every call. */
  if (s_nxlinkSock >= 0) {
    va_start(list, text);
    vprintf(text, list);
    va_end(list);
  }
#endif
  return 0;
}

// boost the CPU to 1785MHz while loading
void cpu_boost(int on) {
  appletSetCpuBoostMode(on ? ApmCpuBoostMode_FastLoad : ApmCpuBoostMode_Normal);
}

int ret0(void) { return 0; }

int retm1(void) { return -1; }
