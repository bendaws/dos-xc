/* \SYSTEM\ELFAPI.DLL: fixed-address module, struct runtime_header first. */
ENTRY(runtime_init)
SECTIONS {
  . = RUNTIME_ADDRESS;
  .header : {
    LONG(0x46454442) LONG(1) LONG(RUNTIME_BITS) LONG(RUNTIME_ADDRESS)
    LONG(runtime_init) LONG(runtime_run) LONG(__runtime_bss_start) LONG(__runtime_bss_end)
  }
  .text : { *(.text*) }
  .rodata : { *(.rodata*) }
  .data : { *(.data*) }
  .bss (NOLOAD) : { __runtime_bss_start = .; *(COMMON) *(.bss*) __runtime_bss_end = .; }
  /DISCARD/ : { *(.eh_frame*) *(.comment*) *(.note*) }
}
