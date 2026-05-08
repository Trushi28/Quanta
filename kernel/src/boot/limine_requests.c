// ============================================================
//  boot/limine_requests.c — Quanta OS
//  All Limine request objects placed in .requests section.
// ============================================================
#include "limine_requests.h"
#include <limine.h>

// Protocol base revision tag
__attribute__((used, section(".requests")))
static volatile uint64_t limine_base_revision[3] = {
    0xf9562b2d5c95a6c8ULL,
    0x6a7b384944536bdcULL,
    KERNEL_LIMINE_REVISION
};

// Section-start / end markers (required by spec)
__attribute__((used, section(".requests_start_marker")))
static volatile uint64_t requests_start_marker[2] = {
    0xf6b8f4b39de7d1aeULL, 0xfab91a6940fcb9cfULL
};
__attribute__((used, section(".requests_end_marker")))
static volatile uint64_t requests_end_marker[2] = {
    0x925bf7ddbf0f2d85ULL, 0xf8a879f08a1272beULL
};

// ── Requests ────────────────────────────────────────────────────────────────

__attribute__((used, section(".requests")))
struct limine_bootloader_info_request bootloader_info_req = {
    .id = LIMINE_BOOTLOADER_INFO_REQUEST, .revision = 0, .response = 0 };

__attribute__((used, section(".requests")))
struct limine_framebuffer_request framebuffer_req = {
    .id = LIMINE_FRAMEBUFFER_REQUEST, .revision = 0, .response = 0 };

__attribute__((used, section(".requests")))
struct limine_hhdm_request hhdm_req = {
    .id = LIMINE_HHDM_REQUEST, .revision = 0, .response = 0 };

__attribute__((used, section(".requests")))
struct limine_memmap_request memmap_req = {
    .id = LIMINE_MEMMAP_REQUEST, .revision = 0, .response = 0 };

__attribute__((used, section(".requests")))
struct limine_kernel_address_request kernel_addr_req = {
    .id = LIMINE_KERNEL_ADDRESS_REQUEST, .revision = 0, .response = 0 };

// Request x2APIC if the CPU supports it
__attribute__((used, section(".requests")))
struct limine_smp_request smp_req = {
    .id = LIMINE_SMP_REQUEST, .revision = 0, .response = 0,
    .flags = LIMINE_SMP_X2APIC };

__attribute__((used, section(".requests")))
struct limine_rsdp_request rsdp_req = {
    .id = LIMINE_RSDP_REQUEST, .revision = 0, .response = 0 };

__attribute__((used, section(".requests")))
struct limine_boot_time_request boot_time_req = {
    .id = LIMINE_BOOT_TIME_REQUEST, .revision = 0, .response = 0 };

// ── Verification ─────────────────────────────────────────────────────────────
int limine_verify_requests(void) {
    if (!framebuffer_req.response || framebuffer_req.response->framebuffer_count < 1)
        return -1;
    if (!memmap_req.response)  return -1;
    if (!hhdm_req.response)    return -1;
    if (!rsdp_req.response)    return -1;
    return 0;
}
