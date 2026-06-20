ENTRY(_start)

SECTIONS
{
    . = 0;

    .text : {
        *(.text.startup)
        *(.text .text.*)
    }

    .rodata : {
        *(.rodata .rodata.*)
    }

    .data : {
        *(.data .data.*)
    }

    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    }

    /DISCARD/ : {
        *(.comment)
        *(.eh_frame)
        *(.note*)
    }
}
