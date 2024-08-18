#include "uvdb.h"
#include <psp2/kernel/modulemgr.h>

void _start() __attribute__ ((weak, alias ("module_start")));
int module_start(SceSize argc, const void *args)
{
  uvdb_enter();
  return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void *args)
{
  return SCE_KERNEL_STOP_SUCCESS;
}
