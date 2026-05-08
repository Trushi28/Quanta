#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// ---------------------------------------------------------------------------
//  drivers/virtio/virtio.h — VirtIO MMIO/PCI transport
//
//  Implements the VirtIO 1.1 specification split-ring virtqueues.
//  Quanta uses the PCI transport (via PCIe ECAM from ACPI MCFG).
// ---------------------------------------------------------------------------

// ── VirtIO device IDs ─────────────────────────────────────────────────────
#define VIRTIO_ID_NETWORK   1
#define VIRTIO_ID_BLOCK     2
#define VIRTIO_ID_CONSOLE   3
#define VIRTIO_ID_RNG       4
#define VIRTIO_ID_BALLOON   5
#define VIRTIO_ID_SCSI      8
#define VIRTIO_ID_9P        9
#define VIRTIO_ID_GPU       16
#define VIRTIO_ID_INPUT     18

// ── Feature bits ──────────────────────────────────────────────────────────
#define VIRTIO_F_VERSION_1   (1ULL << 32)
#define VIRTIO_F_RING_EVENT_IDX (1ULL << 29)
#define VIRTIO_BLK_F_RO      (1ULL << 5)
#define VIRTIO_BLK_F_FLUSH   (1ULL << 9)
#define VIRTIO_BLK_F_SIZE_MAX (1ULL << 1)
#define VIRTIO_BLK_F_SEG_MAX  (1ULL << 2)

// ── Device status bits ────────────────────────────────────────────────────
#define VIRTIO_STATUS_ACKNOWLEDGE  1
#define VIRTIO_STATUS_DRIVER       2
#define VIRTIO_STATUS_DRIVER_OK    4
#define VIRTIO_STATUS_FEATURES_OK  8
#define VIRTIO_STATUS_FAILED      128

// ── Virtqueue descriptor flags ────────────────────────────────────────────
#define VIRTQ_DESC_F_NEXT      1   // chain to next descriptor
#define VIRTQ_DESC_F_WRITE     2   // device write-only (read from device)
#define VIRTQ_DESC_F_INDIRECT  4

// ── Virtqueue descriptor (16 bytes) ──────────────────────────────────────
typedef struct __attribute__((packed)) {
    uint64_t addr;   // physical address of buffer
    uint32_t len;
    uint16_t flags;
    uint16_t next;   // index of next descriptor (if VIRTQ_DESC_F_NEXT)
} virtq_desc_t;

// ── Available ring ────────────────────────────────────────────────────────
typedef struct __attribute__((packed)) {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];   // descriptor indices
    // uint16_t used_event; (after ring[queue_size])
} virtq_avail_t;

// ── Used ring element ─────────────────────────────────────────────────────
typedef struct __attribute__((packed)) {
    uint32_t id;   // descriptor chain head index
    uint32_t len;  // bytes written by device
} virtq_used_elem_t;

typedef struct __attribute__((packed)) {
    uint16_t flags;
    uint16_t idx;
    virtq_used_elem_t ring[];
    // uint16_t avail_event; (after ring[queue_size])
} virtq_used_t;

// ── Virtqueue handle ──────────────────────────────────────────────────────
#define VIRTQ_SIZE  256   // power-of-two, must match device negotiation

typedef struct {
    virtq_desc_t  *desc;        // descriptor table (physical, HHDM-mapped)
    virtq_avail_t *avail;       // driver → device
    virtq_used_t  *used;        // device → driver

    uint16_t       num;         // queue size
    uint16_t       free_head;   // next free descriptor index
    uint16_t       last_used;   // last used-ring index processed
    uint16_t       avail_idx;   // avail->idx shadow

    uint16_t       queue_index; // queue index within device
    void          *dev;         // back-pointer to virtio_dev_t
} virtq_t;

// ── PCI capability / BAR config ───────────────────────────────────────────
typedef struct {
    volatile uint32_t *common_cfg;  // virtio common config (BAR)
    volatile uint8_t  *notify_base; // notification area
    uint32_t           notify_off_multiplier;
    volatile uint8_t  *isr_cfg;
    volatile uint8_t  *device_cfg; // device-specific config
} virtio_pci_cfg_t;

// ── VirtIO device handle ──────────────────────────────────────────────────
#define VIRTIO_MAX_QUEUES  4

typedef struct {
    uint16_t         device_id;
    uint8_t          pci_bus, pci_dev, pci_fn;
    uintptr_t        ecam_base;    // PCIe ECAM config space base
    virtio_pci_cfg_t cfg;
    uint64_t         features;     // negotiated features
    virtq_t          queues[VIRTIO_MAX_QUEUES];
    void            *driver_data;  // driver-specific private data
} virtio_dev_t;

// ── Public API ────────────────────────────────────────────────────────────

// Scan PCIe bus for VirtIO devices and initialise them.
void virtio_init(void);

// Enqueue a descriptor chain into a virtqueue (returns 0 on success).
int virtq_add_buf(virtq_t *vq, virtq_desc_t *descs, uint16_t count);

// Notify the device that new buffers are available.
void virtq_kick(virtq_t *vq);

// Process completed buffers from the used ring.
// Returns number of completions processed.
int virtq_poll(virtq_t *vq);

// ── Block device (virtio-blk) API ─────────────────────────────────────────
typedef struct {
    uint64_t capacity;    // sectors (512 bytes each)
    uint32_t seg_max;
    uint32_t size_max;
    bool     read_only;
} virtio_blk_info_t;

int  virtio_blk_init(virtio_dev_t *dev);
int  virtio_blk_read (uint64_t sector, uint32_t count, void *buf);
int  virtio_blk_write(uint64_t sector, uint32_t count, const void *buf);
void virtio_blk_info (virtio_blk_info_t *out);
