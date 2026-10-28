/* The TLS module (tls_module.h): one image at address 0, code, constants,
 * data, then zeroed data; the loader moves it wherever its memory is and
 * fixes up the absolute addresses pack_module.py lists. Nothing may need
 * running before the entry (no constructors), and nothing unwinds. */
ENTRY(tls_module_entry)
SECTIONS
{
    . = 0;
    .text : {
        KEEP(*(.text.tls_module_entry))
        *(.text .text.* .glue_7 .glue_7t .vfp11_veneer .v4_bx)
    }
    .rodata : ALIGN(8) { *(.rodata .rodata.*) }
    .ARM.exidx : { *(.ARM.exidx*) }
    .data : ALIGN(8) { *(.data .data.*) }
    .init_array : { *(.init_array*) *(.ctors*) }
    .bss : ALIGN(8) { *(.bss .bss.* COMMON) }
    /DISCARD/ : { *(.comment) *(.note*) *(.ARM.attributes) *(.fini_array*) *(.dtors*) }
}
