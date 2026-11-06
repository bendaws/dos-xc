ENTRY(program_main)
SECTIONS {
 . = PROGRAM_ADDRESS;
 .header : { LONG(0x4d435042) LONG(1) LONG(PROGRAM_BITS) LONG(PROGRAM_ADDRESS) LONG(program_main) LONG(__program_bss_start) LONG(__program_bss_end) LONG(PROGRAM_RESUME) }
 .text : { *(.text*) }
 .rodata : { *(.rodata*) }
 .data : { *(.data*) }
 .bss : { __program_bss_start = .; *(COMMON) *(.bss*) __program_bss_end = .; }
 /DISCARD/ : { *(.eh_frame*) *(.comment*) }
}
