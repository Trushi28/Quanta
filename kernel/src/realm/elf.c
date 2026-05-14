// ============================================================
//  realm/elf.c — ELF64 segment loader (Phase 4)
//
//  Validates ELF64 headers and maps each PT_LOAD segment into
//  a Realm's address space with the correct page permissions.
//  BSS is zero-filled.  Returns the entry point on success.
// ============================================================
#include "elf.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../lib/kprintf.h"
#include "../lib/string.h"
#include <stddef.h>

// ── Permission mapping ────────────────────────────────────────────────────
static uint64_t elf_flags_to_vmm(uint32_t p_flags) {
    if (p_flags & PF_X) {
        // Executable — must NOT have NX bit
        if (p_flags & PF_W) {
            // RWX — unusual but supported (no W^X enforcement yet)
            return VMM_FLAG_PRESENT | VMM_FLAG_WRITE | VMM_FLAG_USER;
        }
        return VMM_USER_RX;   // Read + Execute
    }
    if (p_flags & PF_W) {
        return VMM_USER_RW;   // Read + Write (NX set)
    }
    return VMM_USER_RO;       // Read-only (NX set)
}

// ── elf_load ──────────────────────────────────────────────────────────────
uint64_t elf_load(realm_t *realm, const void *binary, size_t size) {
    if (!realm || !binary || size < sizeof(Elf64_Ehdr)) {
        kprintf("[ELF] Invalid arguments\n");
        return 0;
    }

    const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)binary;

    // Validate ELF magic
    if (ehdr->e_ident[0] != ELFMAG0 || ehdr->e_ident[1] != ELFMAG1 ||
        ehdr->e_ident[2] != ELFMAG2 || ehdr->e_ident[3] != ELFMAG3) {
        kprintf("[ELF] Bad magic: 0x%02x%02x%02x%02x\n",
                ehdr->e_ident[0], ehdr->e_ident[1],
                ehdr->e_ident[2], ehdr->e_ident[3]);
        return 0;
    }

    // Validate class (must be 64-bit)
    if (ehdr->e_ident[EI_CLASS] != 2) {
        kprintf("[ELF] Not 64-bit (class=%u)\n", ehdr->e_ident[EI_CLASS]);
        return 0;
    }

    // Validate endianness (must be little-endian)
    if (ehdr->e_ident[EI_DATA] != 1) {
        kprintf("[ELF] Not little-endian\n");
        return 0;
    }

    // Validate type (EXEC or DYN)
    if (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN) {
        kprintf("[ELF] Not executable (type=%u)\n", ehdr->e_type);
        return 0;
    }

    // Validate machine (must be x86-64)
    if (ehdr->e_machine != EM_X86_64) {
        kprintf("[ELF] Not x86-64 (machine=%u)\n", ehdr->e_machine);
        return 0;
    }

    // Validate program header table
    if (ehdr->e_phoff == 0 || ehdr->e_phnum == 0) {
        kprintf("[ELF] No program headers\n");
        return 0;
    }

    if (ehdr->e_phoff + (uint64_t)ehdr->e_phnum * ehdr->e_phentsize > size) {
        kprintf("[ELF] Program headers extend past end of binary\n");
        return 0;
    }

    kprintf("[ELF] Loading ELF64: entry=0x%llx  %u phdrs\n",
            (unsigned long long)ehdr->e_entry, ehdr->e_phnum);

    // Walk program headers and map PT_LOAD segments
    const uint8_t *raw = (const uint8_t *)binary;
    uint32_t segments_loaded = 0;

    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        const Elf64_Phdr *phdr = (const Elf64_Phdr *)(raw + ehdr->e_phoff +
                                                       (uint64_t)i * ehdr->e_phentsize);
        if (phdr->p_type != PT_LOAD) continue;
        if (phdr->p_memsz == 0) continue;

        // Validate segment is within binary bounds
        if (phdr->p_offset + phdr->p_filesz > size) {
            kprintf("[ELF] Segment %u extends past binary (off=%llu, fsz=%llu, total=%zu)\n",
                    i, (unsigned long long)phdr->p_offset,
                    (unsigned long long)phdr->p_filesz, size);
            return 0;
        }

        uint64_t vaddr_start = PAGE_ALIGN_DOWN(phdr->p_vaddr);
        uint64_t vaddr_end   = PAGE_ALIGN_UP(phdr->p_vaddr + phdr->p_memsz);
        size_t   n_pages     = (size_t)((vaddr_end - vaddr_start) / PAGE_SIZE);

        uint64_t vmm_flags = elf_flags_to_vmm(phdr->p_flags);

        kprintf("[ELF]   PT_LOAD #%u: vaddr=0x%llx  memsz=0x%llx  filesz=0x%llx  "
                "flags=%c%c%c  pages=%zu\n",
                segments_loaded,
                (unsigned long long)phdr->p_vaddr,
                (unsigned long long)phdr->p_memsz,
                (unsigned long long)phdr->p_filesz,
                (phdr->p_flags & PF_R) ? 'R' : '-',
                (phdr->p_flags & PF_W) ? 'W' : '-',
                (phdr->p_flags & PF_X) ? 'X' : '-',
                n_pages);

        // Allocate and map pages
        for (size_t pg = 0; pg < n_pages; pg++) {
            uint64_t phys = pmm_alloc();
            if (!phys) {
                kprintf("[ELF] OOM loading segment %u page %zu\n", i, pg);
                return 0;
            }

            // Zero the page first (handles BSS and partial pages)
            memset(phys_to_virt(phys), 0, PAGE_SIZE);

            int rc = vmm_map_page(realm->page_table,
                                  vaddr_start + pg * PAGE_SIZE,
                                  phys, vmm_flags);
            if (rc != 0) {
                kprintf("[ELF] Failed to map page at 0x%llx\n",
                        (unsigned long long)(vaddr_start + pg * PAGE_SIZE));
                pmm_free(phys);
                return 0;
            }

            realm->page_count++;
        }

        // Copy file data into mapped pages via HHDM
        if (phdr->p_filesz > 0) {
            const uint8_t *src = raw + phdr->p_offset;
            uint64_t dst_vaddr = phdr->p_vaddr;
            size_t remaining = (size_t)phdr->p_filesz;

            while (remaining > 0) {
                uint64_t page_off = dst_vaddr & (PAGE_SIZE - 1);
                size_t chunk = PAGE_SIZE - (size_t)page_off;
                if (chunk > remaining) chunk = remaining;

                uint64_t phys = vmm_virt_to_phys(realm->page_table, dst_vaddr);
                if (!phys) {
                    kprintf("[ELF] Cannot resolve vaddr 0x%llx for copy\n",
                            (unsigned long long)dst_vaddr);
                    return 0;
                }

                memcpy(phys_to_virt(phys), src, chunk);

                src       += chunk;
                dst_vaddr += chunk;
                remaining -= chunk;
            }
        }

        segments_loaded++;
    }

    if (segments_loaded == 0) {
        kprintf("[ELF] No PT_LOAD segments found\n");
        return 0;
    }

    kprintf("[ELF] Loaded %u segments  entry=0x%llx  total_pages=%llu\n",
            segments_loaded, (unsigned long long)ehdr->e_entry,
            (unsigned long long)realm->page_count);

    return ehdr->e_entry;
}
