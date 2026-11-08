/* DOS-XC fixed-address module ABI 1. Each architecture is linked separately. */
SECTIONS {
  . = MODULE_BASE;
  .module : {
    LONG(0x4c4c4442) LONG(1) LONG(MODULE_BITS) LONG(MODULE_BASE)
    LONG(__module_bss_start) LONG(__module_bss_end) LONG(0) LONG(0)
  }
  .text : { *(.text*) }
  .rodata : { *(.rodata*) }
  .data : { *(.data*) }
  .bss : { __module_bss_start = .; *(COMMON) *(.bss*) __module_bss_end = .; }
  /DISCARD/ : { *(.eh_frame*) *(.comment*) }
}
