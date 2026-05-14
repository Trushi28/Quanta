#pragma once
#include <stdint.h>
#include <stddef.h>
#include "realm.h"

// ---------------------------------------------------------------------------
//  realm/elf.h — ELF64 binary loader (Phase 4)
//
//  Parses ELF64 headers and maps PT_LOAD segments into a Realm's
//  address space with correct permissions (RX, RW, RO).
// ---------------------------------------------------------------------------

// ── ELF64 structures (from ELF specification) ─────────────────────────────

#define EI_NIDENT 16

typedef struct {
    uint8_t  e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} Elf64_Phdr;

// ELF magic
#define ELFMAG0  0x7F
#define ELFMAG1  'E'
#define ELFMAG2  'L'
#define ELFMAG3  'F'

// e_ident indices
#define EI_CLASS   4    // 2 = 64-bit
#define EI_DATA    5    // 1 = little endian
#define EI_OSABI   7

// e_type
#define ET_EXEC  2
#define ET_DYN   3

// e_machine
#define EM_X86_64  62

// p_type
#define PT_NULL    0
#define PT_LOAD    1

// p_flags
#define PF_X  0x1
#define PF_W  0x2
#define PF_R  0x4

// ── Loader API ────────────────────────────────────────────────────────────

// Load an ELF64 binary into a Realm's address space.
// Returns the entry point virtual address, or 0 on error.
uint64_t elf_load(realm_t *realm, const void *binary, size_t size);
