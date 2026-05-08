// ============================================================
//  drivers/virtio/virtio.c — VirtIO 1.1 PCI driver
//
//  Scans the PCIe bus via ACPI MCFG ECAM, finds virtio devices,
//  and initialises the block device (virtio-blk).
// ============================================================
#include "virtio.h"
#include "../../acpi/acpi.h"
#include "../../mm/pmm.h"
#include "../../mm/heap.h"
#include "../../lib/kprintf.h"
#include "../../lib/string.h"
#include <stddef.h>

// ── PCIe ECAM access helpers ──────────────────────────────────────────────
// ECAM: base + (bus<<20 | dev<<15 | fn<<12) + register_offset

static volatile uint32_t *pcie_cfg_addr(uintptr_t ecam, uint8_t bus,
                                         uint8_t dev, uint8_t fn,
                                         uint16_t off) {
    uintptr_t addr = ecam + ((uint32_t)bus << 20) + ((uint32_t)dev << 15)
                          + ((uint32_t)fn  << 12) + off;
    return (volatile uint32_t *)phys_to_virt(addr);
}

static uint32_t pci_read32(uintptr_t ecam, uint8_t b, uint8_t d,
                            uint8_t f, uint16_t off) {
    return *pcie_cfg_addr(ecam, b, d, f, off);
}

static void pci_write32(uintptr_t ecam, uint8_t b, uint8_t d,
                         uint8_t f, uint16_t off, uint32_t val) {
    *pcie_cfg_addr(ecam, b, d, f, off) = val;
}

// ── VirtIO Common Config (PCI Capability type 1) offsets ─────────────────
// Defined in VirtIO spec 4.1.4.3
#define VCFG_DEVICE_FEATURE_SEL  0x00
#define VCFG_DEVICE_FEATURE      0x04
#define VCFG_DRIVER_FEATURE_SEL  0x08
#define VCFG_DRIVER_FEATURE      0x0C
#define VCFG_MSIX_CONFIG         0x10
#define VCFG_NUM_QUEUES          0x12
#define VCFG_DEVICE_STATUS       0x14
#define VCFG_CONFIG_GEN          0x15
#define VCFG_QUEUE_SEL           0x16
#define VCFG_QUEUE_SIZE          0x18
#define VCFG_QUEUE_MSIX_VECTOR   0x1A
#define VCFG_QUEUE_ENABLE        0x1C
#define VCFG_QUEUE_NOTIFY_OFF    0x1E
#define VCFG_QUEUE_DESC_LO       0x20
#define VCFG_QUEUE_DESC_HI       0x24
#define VCFG_QUEUE_DRIVER_LO     0x28
#define VCFG_QUEUE_DRIVER_HI     0x2C
#define VCFG_QUEUE_DEVICE_LO     0x30
#define VCFG_QUEUE_DEVICE_HI     0x34

#define VCFG8(base,off)   (*(volatile uint8_t  *)((uintptr_t)(base)+(off)))
#define VCFG16(base,off)  (*(volatile uint16_t *)((uintptr_t)(base)+(off)))
#define VCFG32(base,off)  (*(volatile uint32_t *)((uintptr_t)(base)+(off)))

// ── VirtIO block device config ────────────────────────────────────────────
typedef struct __attribute__((packed)) {
    uint64_t capacity;
    uint32_t size_max;
    uint32_t seg_max;
    struct { uint16_t cylinders; uint8_t heads; uint8_t sectors; } geometry;
    uint32_t blk_size;
    struct { uint8_t physical_block_exp, alignment_offset; uint16_t min_io_size; uint32_t opt_io_size; } topology;
    uint8_t  writeback;
    uint8_t  unused0[3];
    uint32_t max_discard_sectors;
    uint32_t max_discard_seg;
    uint32_t discard_sector_alignment;
    uint32_t max_write_zeroes_sectors;
    uint32_t max_write_zeroes_seg;
    uint8_t  write_zeroes_may_unmap;
    uint8_t  unused1[3];
} virtio_blk_config_t;

#define VIRTIO_BLK_T_IN   0
#define VIRTIO_BLK_T_OUT  1
#define VIRTIO_BLK_T_FLUSH 4

typedef struct __attribute__((packed)) {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
} virtio_blk_req_hdr_t;

#define VIRTIO_BLK_S_OK     0
#define VIRTIO_BLK_S_IOERR  1
#define VIRTIO_BLK_S_UNSUPP 2

// ── Global state ──────────────────────────────────────────────────────────
static virtio_dev_t *g_blk_dev = NULL;

// ── BAR MMIO helpers ──────────────────────────────────────────────────────
static uintptr_t bar_base(uintptr_t ecam, uint8_t b, uint8_t d,
                           uint8_t f, uint8_t bar_idx) {
    uint16_t off = 0x10 + bar_idx * 4;
    uint32_t lo  = pci_read32(ecam, b, d, f, off);
    if (lo & 4) {  // 64-bit BAR
        uint32_t hi = pci_read32(ecam, b, d, f, off + 4);
        return ((uint64_t)hi << 32) | (lo & ~0xFUL);
    }
    return lo & ~0xFUL;
}

// ── Virtqueue setup ────────────────────────────────────────────────────────
static int virtq_init(virtio_dev_t *dev, uint16_t qidx) {
    volatile uint8_t *base = (volatile uint8_t *)dev->cfg.common_cfg;

    VCFG16(base, VCFG_QUEUE_SEL) = qidx;
    uint16_t qsz = VCFG16(base, VCFG_QUEUE_SIZE);
    if (!qsz) return -1;
    if (qsz > VIRTQ_SIZE) qsz = VIRTQ_SIZE;

    // Allocate descriptor table + available ring + used ring
    size_t desc_sz  = sizeof(virtq_desc_t) * qsz;
    size_t avail_sz = sizeof(virtq_avail_t) + sizeof(uint16_t) * qsz + 2;
    size_t used_sz  = sizeof(virtq_used_t)  + sizeof(virtq_used_elem_t) * qsz + 2;

    // Align to page boundary
    size_t total_pages = (desc_sz + avail_sz + used_sz + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t phys = pmm_alloc_n(total_pages + 1);  // +1 for alignment headroom
    if (!phys) return -1;

    virtq_t *vq     = &dev->queues[qidx];
    vq->desc        = (virtq_desc_t *)phys_to_virt(phys);
    vq->avail       = (virtq_avail_t *)((uint8_t *)vq->desc + desc_sz);
    vq->used        = (virtq_used_t *)((uint8_t *)vq->avail + avail_sz);
    vq->num         = qsz;
    vq->free_head   = 0;
    vq->last_used   = 0;
    vq->avail_idx   = 0;
    vq->queue_index = qidx;
    vq->dev         = dev;

    // Initialise free-list chain in descriptor table
    for (int i = 0; i < qsz - 1; i++) vq->desc[i].next = (uint16_t)(i + 1);
    vq->desc[qsz - 1].next = 0;

    // Tell the device about the queue
    uint64_t desc_phys  = virt_to_phys(vq->desc);
    uint64_t avail_phys = virt_to_phys(vq->avail);
    uint64_t used_phys  = virt_to_phys(vq->used);

    VCFG16(base, VCFG_QUEUE_SIZE)     = qsz;
    VCFG32(base, VCFG_QUEUE_DESC_LO)  = (uint32_t)desc_phys;
    VCFG32(base, VCFG_QUEUE_DESC_HI)  = (uint32_t)(desc_phys >> 32);
    VCFG32(base, VCFG_QUEUE_DRIVER_LO)= (uint32_t)avail_phys;
    VCFG32(base, VCFG_QUEUE_DRIVER_HI)= (uint32_t)(avail_phys >> 32);
    VCFG32(base, VCFG_QUEUE_DEVICE_LO)= (uint32_t)used_phys;
    VCFG32(base, VCFG_QUEUE_DEVICE_HI)= (uint32_t)(used_phys >> 32);
    VCFG16(base, VCFG_QUEUE_ENABLE)   = 1;

    return 0;
}

// ── virtq_add_buf / kick / poll ────────────────────────────────────────────
int virtq_add_buf(virtq_t *vq, virtq_desc_t *descs, uint16_t count) {
    if (vq->free_head + count > vq->num) return -1;

    uint16_t head = vq->free_head;
    for (uint16_t i = 0; i < count; i++) {
        uint16_t idx   = (uint16_t)(vq->free_head + i);
        vq->desc[idx]  = descs[i];
        if (i < count - 1) {
            vq->desc[idx].flags |= VIRTQ_DESC_F_NEXT;
            vq->desc[idx].next   = (uint16_t)(idx + 1);
        }
    }
    vq->free_head = (uint16_t)(vq->free_head + count);

    // Add to available ring
    uint16_t ai = vq->avail_idx & (vq->num - 1);
    vq->avail->ring[ai] = head;
    __asm__ volatile ("" ::: "memory");  // compiler fence
    vq->avail->idx = (uint16_t)(vq->avail_idx + 1);
    vq->avail_idx++;
    return 0;
}

void virtq_kick(virtq_t *vq) {
    virtio_dev_t *dev = (virtio_dev_t *)vq->dev;
    volatile uint8_t *notify = dev->cfg.notify_base +
        vq->queue_index * dev->cfg.notify_off_multiplier;
    __asm__ volatile ("" ::: "memory");
    *(volatile uint16_t *)notify = vq->queue_index;
}

int virtq_poll(virtq_t *vq) {
    int n = 0;
    while (vq->last_used != vq->used->idx) {
        vq->last_used++;
        n++;
    }
    return n;
}

// ── virtio_blk_init ───────────────────────────────────────────────────────
int virtio_blk_init(virtio_dev_t *dev) {
    volatile uint8_t *base = (volatile uint8_t *)dev->cfg.common_cfg;

    // Reset device
    VCFG8(base, VCFG_DEVICE_STATUS) = 0;
    // Acknowledge + Driver
    VCFG8(base, VCFG_DEVICE_STATUS) = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER;

    // Negotiate features: require VERSION_1
    VCFG32(base, VCFG_DEVICE_FEATURE_SEL) = 0;
    uint64_t dev_feat = VCFG32(base, VCFG_DEVICE_FEATURE);
    VCFG32(base, VCFG_DEVICE_FEATURE_SEL) = 1;
    dev_feat |= (uint64_t)VCFG32(base, VCFG_DEVICE_FEATURE) << 32;

    uint64_t drv_feat = VIRTIO_F_VERSION_1;
    VCFG32(base, VCFG_DRIVER_FEATURE_SEL) = 0;
    VCFG32(base, VCFG_DRIVER_FEATURE)     = (uint32_t)drv_feat;
    VCFG32(base, VCFG_DRIVER_FEATURE_SEL) = 1;
    VCFG32(base, VCFG_DRIVER_FEATURE)     = (uint32_t)(drv_feat >> 32);

    VCFG8(base, VCFG_DEVICE_STATUS) |= VIRTIO_STATUS_FEATURES_OK;
    if (!(VCFG8(base, VCFG_DEVICE_STATUS) & VIRTIO_STATUS_FEATURES_OK)) {
        kprintf("[VirtIO-blk] Feature negotiation failed\n");
        return -1;
    }

    dev->features = drv_feat;

    // Set up request virtqueue (queue 0)
    if (virtq_init(dev, 0) < 0) {
        kprintf("[VirtIO-blk] Queue init failed\n");
        return -1;
    }

    // Driver OK
    VCFG8(base, VCFG_DEVICE_STATUS) |= VIRTIO_STATUS_DRIVER_OK;

    // Read block device config
    virtio_blk_config_t *blk_cfg =
        (virtio_blk_config_t *)dev->cfg.device_cfg;
    uint64_t cap = blk_cfg->capacity;
    g_blk_dev = dev;

    kprintf("[VirtIO-blk] Ready  capacity=%llu sectors (%llu MiB)\n",
            (unsigned long long)cap,
            (unsigned long long)(cap * 512 / (1024*1024)));
    return 0;
}

// ── virtio_blk I/O ────────────────────────────────────────────────────────
static int blk_io(uint32_t type, uint64_t sector, uint32_t count, void *buf) {
    if (!g_blk_dev) return -1;
    virtq_t *vq = &g_blk_dev->queues[0];

    // Allocate request header and status byte in a small DMA buffer
    struct {
        virtio_blk_req_hdr_t hdr;
        uint8_t status;
    } *req = (void *)kmalloc(sizeof(*req));
    if (!req) return -1;
    memset(req, 0, sizeof(*req));
    req->hdr.type   = type;
    req->hdr.sector = sector;
    req->status     = 0xFF;   // sentinel

    uint64_t hdr_phys  = virt_to_phys(&req->hdr);
    uint64_t buf_phys  = virt_to_phys(buf);
    uint64_t stat_phys = virt_to_phys(&req->status);
    uint32_t buf_len   = count * 512;

    virtq_desc_t descs[3];
    descs[0] = (virtq_desc_t){ hdr_phys,  sizeof(virtio_blk_req_hdr_t), 0, 0 };
    descs[1] = (virtq_desc_t){ buf_phys,  buf_len,
                                (type == VIRTIO_BLK_T_IN) ? VIRTQ_DESC_F_WRITE : 0, 0 };
    descs[2] = (virtq_desc_t){ stat_phys, 1, VIRTQ_DESC_F_WRITE, 0 };

    virtq_add_buf(vq, descs, 3);
    virtq_kick(vq);

    // Busy-wait for completion (will be replaced by IRQ + semaphore later)
    uint64_t timeout = 10000000ULL;
    while (req->status == 0xFF && --timeout) __asm__ volatile ("pause");

    int rc = (req->status == VIRTIO_BLK_S_OK) ? 0 : -1;
    vq->free_head = 0;  // simple reset for now (no concurrent I/O yet)
    kfree(req);
    return rc;
}

int virtio_blk_read(uint64_t sector, uint32_t count, void *buf) {
    return blk_io(VIRTIO_BLK_T_IN, sector, count, buf);
}
int virtio_blk_write(uint64_t sector, uint32_t count, const void *buf) {
    return blk_io(VIRTIO_BLK_T_OUT, sector, count, (void *)buf);
}

void virtio_blk_info(virtio_blk_info_t *out) {
    if (!g_blk_dev || !out) return;
    virtio_blk_config_t *c = (virtio_blk_config_t *)g_blk_dev->cfg.device_cfg;
    out->capacity  = c->capacity;
    out->size_max  = c->size_max;
    out->seg_max   = c->seg_max;
    out->read_only = !!(g_blk_dev->features & VIRTIO_BLK_F_RO);
}

// ── PCI scan ─────────────────────────────────────────────────────────────
#define PCI_VENDOR_VIRTIO  0x1AF4
#define PCI_CLASS_STORAGE  0x01

static void probe_device(uintptr_t ecam, uint8_t b, uint8_t d, uint8_t f) {
    uint32_t id = pci_read32(ecam, b, d, f, 0);
    uint16_t vid = (uint16_t)(id & 0xFFFF);
    uint16_t did = (uint16_t)(id >> 16);
    if (vid != PCI_VENDOR_VIRTIO) return;
    if (did < 0x1040 || did > 0x107F) return;  // VirtIO 1.0+ device IDs

    uint16_t dev_id = did - 0x1040;

    // Enable bus mastering + memory space
    uint32_t cmd = pci_read32(ecam, b, d, f, 0x04);
    pci_write32(ecam, b, d, f, 0x04, cmd | 0x06);

    // Allocate device handle
    virtio_dev_t *dev = (virtio_dev_t *)kmalloc(sizeof(virtio_dev_t));
    if (!dev) return;
    memset(dev, 0, sizeof(*dev));
    dev->device_id = dev_id;
    dev->pci_bus   = b;
    dev->pci_dev   = d;
    dev->pci_fn    = f;
    dev->ecam_base = ecam;

    // Locate VirtIO capabilities by walking PCI cap list
    uint8_t cap_ptr = (uint8_t)(pci_read32(ecam, b, d, f, 0x34) & 0xFF);
    while (cap_ptr) {
        uint32_t cap_hdr = pci_read32(ecam, b, d, f, cap_ptr);
        uint8_t  cap_id  = (uint8_t)(cap_hdr & 0xFF);
        uint8_t  cfg_type;
        uint8_t  bar_idx;
        uint32_t offset;

        if (cap_id == 0x09) {  // VirtIO vendor capability
            volatile uint8_t *cap_addr =
                (volatile uint8_t *)pcie_cfg_addr(ecam, b, d, f, cap_ptr);
            cfg_type = cap_addr[3];
            bar_idx  = cap_addr[4];
            offset   = *(uint32_t *)(cap_addr + 8);

            uintptr_t bar = bar_base(ecam, b, d, f, bar_idx);
            uintptr_t vaddr = (uintptr_t)phys_to_virt(bar + offset);

            switch (cfg_type) {
                case 1: dev->cfg.common_cfg  = (volatile uint32_t *)vaddr; break;
                case 2: dev->cfg.notify_base  = (volatile uint8_t *)vaddr;
                        dev->cfg.notify_off_multiplier =
                            *(uint32_t *)(cap_addr + 16); break;
                case 3: dev->cfg.isr_cfg      = (volatile uint8_t *)vaddr; break;
                case 4: dev->cfg.device_cfg   = (volatile uint8_t *)vaddr; break;
            }
        }
        cap_ptr = (uint8_t)((cap_hdr >> 8) & 0xFF);
    }

    if (!dev->cfg.common_cfg) {
        kprintf("[VirtIO] Device %u.%u.%u: no common config cap\n", b, d, f);
        kfree(dev);
        return;
    }

    kprintf("[VirtIO] Found device ID=%u at %02x:%02x.%x\n",
            dev_id, b, d, f);

    switch (dev_id) {
        case VIRTIO_ID_BLOCK:
            virtio_blk_init(dev);
            break;
        default:
            kprintf("[VirtIO] Device ID %u — no driver\n", dev_id);
            kfree(dev);
            break;
    }
}

void virtio_init(void) {
    acpi_mcfg_t *mcfg = acpi_mcfg();
    if (!mcfg) {
        kprintf("[VirtIO] No MCFG — PCIe enumeration skipped\n");
        return;
    }

    uint64_t entry_count = (mcfg->header.length - sizeof(acpi_mcfg_t))
                           / sizeof(mcfg_entry_t);

    for (uint64_t e = 0; e < entry_count; e++) {
        uintptr_t ecam = (uintptr_t)mcfg->entries[e].base_address;
        for (uint16_t b = mcfg->entries[e].start_bus;
             b <= mcfg->entries[e].end_bus; b++) {
            for (uint8_t d = 0; d < 32; d++) {
                uint32_t id = pci_read32(ecam, (uint8_t)b, d, 0, 0);
                if ((id & 0xFFFF) == 0xFFFF) continue;

                uint8_t hdr_type = (uint8_t)(pci_read32(ecam,(uint8_t)b,d,0,0x0C)>>16);
                uint8_t fn_count = (hdr_type & 0x80) ? 8 : 1;

                for (uint8_t f = 0; f < fn_count; f++) {
                    uint32_t fid = pci_read32(ecam, (uint8_t)b, d, f, 0);
                    if ((fid & 0xFFFF) == 0xFFFF) continue;
                    probe_device(ecam, (uint8_t)b, d, f);
                }
            }
        }
    }
}
