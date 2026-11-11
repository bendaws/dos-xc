/*
 * DOS-XC /os/runtime/runtime.ld
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

ENTRY(runtime_init)
SECTIONS {
  . = RUNTIME_ADDRESS;
  .header : {
    LONG(0x46454442) LONG(2) LONG(RUNTIME_BITS) LONG(RUNTIME_ADDRESS)
    LONG(runtime_init) LONG(runtime_run) LONG(__runtime_bss_start) LONG(__runtime_bss_end)
    LONG(runtime_resume)
  }
  .text : { *(.text*) }
  .rodata : { *(.rodata*) }
  .data : { *(.data*) }
  .bss (NOLOAD) : { __runtime_bss_start = .; *(COMMON) *(.bss*) __runtime_bss_end = .; }
  /DISCARD/ : { *(.eh_frame*) *(.comment*) *(.note*) }
}
