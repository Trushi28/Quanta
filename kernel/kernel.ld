/* ============================================================
 *  kernel.ld — Quanta OS higher-half linker script
 *
 *  Kernel is loaded at 0xFFFFFFFF80000000 (virtual).
 *  Physical base is negotiated with Limine (non-PIE ELF).
 *  HHDM maps all physical RAM at 0xFFFF800000000000+.
 * ============================================================ */

ENTRY(kmain)

SECTIONS {
    /* ── Virtual placement ── */
    . = 0xFFFFFFFF80000000;

    /* Limine request markers MUST appear in this order, contiguous */
    .requests_start_marker : {
        KEEP(*(.requests_start_marker))
    }
    .requests : {
        KEEP(*(.requests))
    }
    .requests_end_marker : {
        KEEP(*(.requests_end_marker))
    }

    /* ── Executable code ── */
    .text : {
        *(.text .text.*)
    }

    . = ALIGN(4096);

    /* ── Read-only data ── */
    .rodata : {
        *(.rodata .rodata.*)
    }

    . = ALIGN(4096);

    /* ── Writable data ── */
    .data : {
        *(.data .data.*)
    }

    . = ALIGN(4096);

    /* ── Zero-initialised data ── */
    .bss : {
        *(COMMON)
        *(.bss .bss.*)
    }

    . = ALIGN(4096);

    /* ── Discard unneeded sections ── */
    /DISCARD/ : {
        *(.eh_frame .eh_frame_hdr)
        *(.note .note.*)
        *(.comment)
    }
}
