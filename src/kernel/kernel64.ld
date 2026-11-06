ENTRY(_start)
SECTIONS {
  . = KERNEL_BASE;
  .entry : { *(.entry) }
  . = KERNEL_BASE + 0x100;
  .dos_entry : { *(.dos_entry) }
  . = KERNEL_BASE + 0x140;
  .bios_entry : { *(.bios_entry) }
  . = KERNEL_BASE + 0x200;
  .text : { *(.text*) }
  .rodata : { *(.rodata*) }
  .data : { *(.data*) }
  .bss : { __bss_start = .; *(COMMON) *(.bss*) __bss_end = .; }
  /DISCARD/ : { *(.eh_frame*) *(.comment*) }
}
