#pragma once
// ============================================================
//  boot/limine_requests.h — Quanta OS
//  Declarations and inline accessors for every Limine request.
// ============================================================
#include <limine.h>
#include <stdint.h>

#define KERNEL_LIMINE_REVISION 2

extern struct limine_bootloader_info_request bootloader_info_req;
extern struct limine_framebuffer_request     framebuffer_req;
extern struct limine_hhdm_request            hhdm_req;
extern struct limine_memmap_request          memmap_req;
extern struct limine_kernel_address_request  kernel_addr_req;
extern struct limine_smp_request             smp_req;
extern struct limine_rsdp_request            rsdp_req;
extern struct limine_boot_time_request       boot_time_req;

static inline struct limine_bootloader_info_response *limine_bootloader_info(void)
    { return bootloader_info_req.response; }
static inline struct limine_framebuffer_response *limine_framebuffers(void)
    { return framebuffer_req.response; }
static inline struct limine_hhdm_response *limine_hhdm(void)
    { return hhdm_req.response; }
static inline struct limine_memmap_response *limine_memmap(void)
    { return memmap_req.response; }
static inline struct limine_kernel_address_response *limine_kernel_addr(void)
    { return kernel_addr_req.response; }
static inline struct limine_smp_response *limine_smp(void)
    { return smp_req.response; }
static inline struct limine_rsdp_response *limine_rsdp(void)
    { return rsdp_req.response; }
static inline struct limine_boot_time_response *limine_boot_time(void)
    { return boot_time_req.response; }

// Returns 0 if all critical requests were fulfilled, -1 otherwise.
int limine_verify_requests(void);
