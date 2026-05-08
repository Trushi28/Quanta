#pragma once

// ============================================================
//  Limine Boot Protocol v2 — Quanta OS Header
//  Compatible with: Limine 8.x
//
//  Used features:
//    - Framebuffer          (pixel terminal)
//    - Memory Map           (physical memory setup)
//    - HHDM                 (higher-half direct map)
//    - Kernel Address       (phys/virt kernel base)
//    - Bootloader Info      (display on boot)
//    - SMP (MP)             (multi-processor bringup + x2APIC)
//    - RSDP                 (ACPI root pointer)
//    - Boot Time            (Unix timestamp at boot)
// ============================================================

#include <stdint.h>

// ---------------------------------------------------------------------------
// Protocol magic shared by every request
// ---------------------------------------------------------------------------
#define LIMINE_COMMON_MAGIC  0xc7b1dd30df4c8b88ULL, 0x0a82e883a194f07bULL

// Base revision tag (revision 2 is the last non-deprecated revision for v8)
#define LIMINE_BASE_REVISION(rev)                                      \
    volatile uint64_t LIMINE_BASE_REVISION_VALID[3] = {               \
        0xf9562b2d5c95a6c8ULL, 0x6a7b384944536bdcULL, (rev)           \
    }

// ---------------------------------------------------------------------------
// Bootloader Info
// ---------------------------------------------------------------------------
#define LIMINE_BOOTLOADER_INFO_REQUEST \
    { LIMINE_COMMON_MAGIC, 0xf55038d8e2a1202fULL, 0x279426fcf5f59740ULL }

struct limine_bootloader_info_response {
    uint64_t revision;
    char    *name;
    char    *version;
};
struct limine_bootloader_info_request {
    uint64_t id[4];
    uint64_t revision;
    struct limine_bootloader_info_response *response;
};

// ---------------------------------------------------------------------------
// Framebuffer
// ---------------------------------------------------------------------------
#define LIMINE_FRAMEBUFFER_REQUEST \
    { LIMINE_COMMON_MAGIC, 0x9d5827dcd881dd75ULL, 0xa3148604f6fab11bULL }

#define LIMINE_FRAMEBUFFER_RGB 1

struct limine_video_mode {
    uint64_t pitch;
    uint64_t width;
    uint64_t height;
    uint16_t bpp;
    uint8_t  memory_model;
    uint8_t  red_mask_size,   red_mask_shift;
    uint8_t  green_mask_size, green_mask_shift;
    uint8_t  blue_mask_size,  blue_mask_shift;
};
struct limine_framebuffer {
    void    *address;
    uint64_t width;
    uint64_t height;
    uint64_t pitch;       // bytes per scanline
    uint16_t bpp;
    uint8_t  memory_model;
    uint8_t  red_mask_size,   red_mask_shift;
    uint8_t  green_mask_size, green_mask_shift;
    uint8_t  blue_mask_size,  blue_mask_shift;
    uint8_t  unused[7];
    uint64_t edid_size;
    void    *edid;
    // revision >= 1
    uint64_t  mode_count;
    struct limine_video_mode **modes;
};
struct limine_framebuffer_response {
    uint64_t revision;
    uint64_t framebuffer_count;
    struct limine_framebuffer **framebuffers;
};
struct limine_framebuffer_request {
    uint64_t id[4];
    uint64_t revision;
    struct limine_framebuffer_response *response;
};

// ---------------------------------------------------------------------------
// Higher Half Direct Map
// ---------------------------------------------------------------------------
#define LIMINE_HHDM_REQUEST \
    { LIMINE_COMMON_MAGIC, 0x48dcf1cb8ad2b852ULL, 0x63984e959a98244bULL }

struct limine_hhdm_response {
    uint64_t revision;
    uint64_t offset;
};
struct limine_hhdm_request {
    uint64_t id[4];
    uint64_t revision;
    struct limine_hhdm_response *response;
};

// ---------------------------------------------------------------------------
// Memory Map
// ---------------------------------------------------------------------------
#define LIMINE_MEMMAP_REQUEST \
    { LIMINE_COMMON_MAGIC, 0x67cf3d9d378a806fULL, 0xe304acdfc50c3c62ULL }

#define LIMINE_MEMMAP_USABLE                 0
#define LIMINE_MEMMAP_RESERVED               1
#define LIMINE_MEMMAP_ACPI_RECLAIMABLE       2
#define LIMINE_MEMMAP_ACPI_NVS               3
#define LIMINE_MEMMAP_BAD_MEMORY             4
#define LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE 5
#define LIMINE_MEMMAP_KERNEL_AND_MODULES     6
#define LIMINE_MEMMAP_FRAMEBUFFER            7

struct limine_memmap_entry {
    uint64_t base;
    uint64_t length;
    uint64_t type;
};
struct limine_memmap_response {
    uint64_t revision;
    uint64_t entry_count;
    struct limine_memmap_entry **entries;
};
struct limine_memmap_request {
    uint64_t id[4];
    uint64_t revision;
    struct limine_memmap_response *response;
};

// ---------------------------------------------------------------------------
// Kernel Address
// ---------------------------------------------------------------------------
#define LIMINE_KERNEL_ADDRESS_REQUEST \
    { LIMINE_COMMON_MAGIC, 0x71ba76863cc55f63ULL, 0xb2644a48c516a487ULL }

struct limine_kernel_address_response {
    uint64_t revision;
    uint64_t physical_base;
    uint64_t virtual_base;
};
struct limine_kernel_address_request {
    uint64_t id[4];
    uint64_t revision;
    struct limine_kernel_address_response *response;
};

// ---------------------------------------------------------------------------
// SMP / Multiprocessor  (called "MP" in v8 protocol docs)
//   flags bit 0: request x2APIC enablement
//   response flags bit 0: x2APIC was enabled
// ---------------------------------------------------------------------------
#define LIMINE_SMP_REQUEST \
    { LIMINE_COMMON_MAGIC, 0x95a67b819a1b857eULL, 0xa0b61b723b6a73e0ULL }

#define LIMINE_SMP_X2APIC (1u << 0)

struct limine_smp_info;
typedef void (*limine_goto_address)(struct limine_smp_info *);

struct limine_smp_info {
    uint32_t processor_id;    // ACPI UID from MADT
    uint32_t lapic_id;        // Local APIC ID from MADT
    uint64_t reserved;
    limine_goto_address goto_address;  // atomic write to wake this AP
    uint64_t extra_argument;           // passed in RDI on entry
};

struct limine_smp_response {
    uint64_t revision;
    uint32_t flags;           // bit 0: x2APIC was enabled
    uint32_t bsp_lapic_id;    // BSP's LAPIC ID
    uint64_t cpu_count;
    struct limine_smp_info **cpus;
};

struct limine_smp_request {
    uint64_t id[4];
    uint64_t revision;
    struct limine_smp_response *response;
    uint64_t flags;           // bit 0: request x2APIC
};

// ---------------------------------------------------------------------------
// RSDP  (ACPI Root System Description Pointer)
// ---------------------------------------------------------------------------
#define LIMINE_RSDP_REQUEST \
    { LIMINE_COMMON_MAGIC, 0xc5e77b6b397e7b43ULL, 0x27637845accdcf3cULL }

struct limine_rsdp_response {
    uint64_t revision;
    void    *address;    // virtual address (HHDM offset already applied)
};
struct limine_rsdp_request {
    uint64_t id[4];
    uint64_t revision;
    struct limine_rsdp_response *response;
};

// ---------------------------------------------------------------------------
// Boot Time  (Unix timestamp at the moment of boot)
// ---------------------------------------------------------------------------
#define LIMINE_BOOT_TIME_REQUEST \
    { LIMINE_COMMON_MAGIC, 0x502746e184c088aaULL, 0xfbc5ec83e6327893ULL }

struct limine_boot_time_response {
    uint64_t revision;
    int64_t  boot_time;   // seconds since Unix epoch
};
struct limine_boot_time_request {
    uint64_t id[4];
    uint64_t revision;
    struct limine_boot_time_response *response;
};
