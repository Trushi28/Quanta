// ============================================================
//  drivers/kvstore.c — Simple persistent key-value store
//
//  Reads and writes a single 512-byte sector on the virtio-blk
//  device.  All operations keep an in-memory mirror and flush it
//  to disk on every write so data is always durable.
// ============================================================
#include "kvstore.h"
#include "virtio/virtio.h"
#include "../lib/string.h"
#include "../lib/kprintf.h"
#include "../lib/spinlock.h"
#include <stddef.h>
#include <stdint.h>

// ── Disk layout ───────────────────────────────────────────────────────────
#define KVSTORE_SECTOR   2048u      // 1 MiB offset — safely past any boot data
#define KVSTORE_MAGIC    0x4B564E49u // "KVNI"
#define KVSTORE_VERSION  1u

typedef struct __attribute__((packed)) {
    char key[KVSTORE_KEY_MAX];
    char val[KVSTORE_VAL_MAX];
} kv_entry_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t count;
    uint16_t version;
    kv_entry_t entries[KVSTORE_MAX_ENTRIES];
    // padding to 512 bytes
    uint8_t  _pad[512 - 8 - KVSTORE_MAX_ENTRIES * (KVSTORE_KEY_MAX + KVSTORE_VAL_MAX)];
} kv_sector_t;

// Make sure we fit in exactly one sector
_Static_assert(sizeof(kv_sector_t) == 512, "kv_sector_t must be 512 bytes");

// ── In-memory state ───────────────────────────────────────────────────────
static kv_sector_t  g_store;
static int          g_ready_flag = 0;
static spinlock_t   g_lock = SPINLOCK_INIT;

// ── Disk I/O ──────────────────────────────────────────────────────────────
static int flush_to_disk(void) {
    return virtio_blk_write(KVSTORE_SECTOR, 1, &g_store);
}

static int load_from_disk(void) {
    return virtio_blk_read(KVSTORE_SECTOR, 1, &g_store);
}

// ── kv_init ───────────────────────────────────────────────────────────────
int kv_init(void) {
    // Check that virtio-blk is available
    virtio_blk_info_t info;
    virtio_blk_info(&info);
    if (!info.capacity) {
        kprintf("[KV] No virtio disk — persistence disabled\n");
        return -1;
    }

    if (load_from_disk() < 0) {
        kprintf("[KV] Disk read failed — starting fresh\n");
        goto fresh;
    }

    if (g_store.magic != KVSTORE_MAGIC || g_store.version != KVSTORE_VERSION) {
        kprintf("[KV] No existing store (magic=%08x) — creating\n",
                (unsigned)g_store.magic);
        goto fresh;
    }

    if (g_store.count > KVSTORE_MAX_ENTRIES)
        g_store.count = 0; // corrupted count

    kprintf("[KV] Loaded %u persistent key(s) from sector %u\n",
            (unsigned)g_store.count, (unsigned)KVSTORE_SECTOR);
    g_ready_flag = 1;
    return 0;

fresh:
    memset(&g_store, 0, sizeof(g_store));
    g_store.magic   = KVSTORE_MAGIC;
    g_store.version = KVSTORE_VERSION;
    g_store.count   = 0;
    flush_to_disk();
    g_ready_flag = 1;
    return 0;
}

int kv_ready(void) { return g_ready_flag; }

// ── kv_set ────────────────────────────────────────────────────────────────
int kv_set(const char *key, const char *val) {
    if (!g_ready_flag || !key || !val) return -1;
    if (strlen(key) >= KVSTORE_KEY_MAX) return -1;
    if (strlen(val) >= KVSTORE_VAL_MAX) return -1;

    uint64_t rflags = spinlock_irq_acquire(&g_lock);

    // Update existing entry
    for (int i = 0; i < (int)g_store.count; i++) {
        if (strcmp(g_store.entries[i].key, key) == 0) {
            strncpy(g_store.entries[i].val, val, KVSTORE_VAL_MAX - 1);
            g_store.entries[i].val[KVSTORE_VAL_MAX - 1] = '\0';
            int r = flush_to_disk();
            spinlock_irq_release(&g_lock, rflags);
            return r;
        }
    }

    // Add new entry
    if (g_store.count >= KVSTORE_MAX_ENTRIES) {
        spinlock_irq_release(&g_lock, rflags);
        return -1; // store full
    }

    int idx = (int)g_store.count++;
    strncpy(g_store.entries[idx].key, key, KVSTORE_KEY_MAX - 1);
    g_store.entries[idx].key[KVSTORE_KEY_MAX - 1] = '\0';
    strncpy(g_store.entries[idx].val, val, KVSTORE_VAL_MAX - 1);
    g_store.entries[idx].val[KVSTORE_VAL_MAX - 1] = '\0';

    int r = flush_to_disk();
    spinlock_irq_release(&g_lock, rflags);
    return r;
}

// ── kv_get ────────────────────────────────────────────────────────────────
int kv_get(const char *key, char *val, size_t valsz) {
    if (!g_ready_flag || !key || !val) return -1;

    uint64_t rflags = spinlock_irq_acquire(&g_lock);
    for (int i = 0; i < (int)g_store.count; i++) {
        if (strcmp(g_store.entries[i].key, key) == 0) {
            strncpy(val, g_store.entries[i].val, valsz - 1);
            val[valsz - 1] = '\0';
            spinlock_irq_release(&g_lock, rflags);
            return 0;
        }
    }
    spinlock_irq_release(&g_lock, rflags);
    return -1;
}

// ── kv_del ────────────────────────────────────────────────────────────────
int kv_del(const char *key) {
    if (!g_ready_flag || !key) return -1;

    uint64_t rflags = spinlock_irq_acquire(&g_lock);
    for (int i = 0; i < (int)g_store.count; i++) {
        if (strcmp(g_store.entries[i].key, key) == 0) {
            // Shift remaining entries down
            int last = (int)g_store.count - 1;
            for (int j = i; j < last; j++)
                g_store.entries[j] = g_store.entries[j + 1];
            memset(&g_store.entries[last], 0, sizeof(kv_entry_t));
            g_store.count--;
            int r = flush_to_disk();
            spinlock_irq_release(&g_lock, rflags);
            return r;
        }
    }
    spinlock_irq_release(&g_lock, rflags);
    return -1;
}

// ── kv_list ───────────────────────────────────────────────────────────────
void kv_list(kv_iter_fn fn, void *ud) {
    if (!g_ready_flag || !fn) return;

    uint64_t rflags = spinlock_irq_acquire(&g_lock);
    for (int i = 0; i < (int)g_store.count; i++) {
        spinlock_irq_release(&g_lock, rflags);
        int cont = fn(g_store.entries[i].key, g_store.entries[i].val, ud);
        rflags = spinlock_irq_acquire(&g_lock);
        if (!cont) break;
    }
    spinlock_irq_release(&g_lock, rflags);
}
