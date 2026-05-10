#pragma once
#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
//  drivers/kvstore.h — Simple persistent key-value store
//
//  Stores up to KVSTORE_MAX_ENTRIES key-value pairs in a single 512-byte
//  sector on the virtio-blk device.  Data survives reboots as long as the
//  disk image is preserved (the normal case with QEMU raw disk images).
//
//  Sector layout (KVSTORE_SECTOR, 512 bytes):
//    [0..3]   uint32_t magic  = 0x4B564E49 ("KVNI")
//    [4..5]   uint16_t count
//    [6..7]   uint16_t version = 1
//    [8..511] entries: { key[28], val[60] }  × KVSTORE_MAX_ENTRIES (= 5)
//
//  Usage:
//    kv_init();                  // call after virtio_init()
//    kv_set("hostname", "quanta");
//    char buf[60]; kv_get("hostname", buf, sizeof(buf));
//    kv_del("hostname");
// ---------------------------------------------------------------------------

#define KVSTORE_MAX_ENTRIES  5
#define KVSTORE_KEY_MAX     28
#define KVSTORE_VAL_MAX     60

// Initialise: load existing store from disk or create a blank one.
// Returns 0 on success, -1 if no virtio disk is available.
int  kv_init(void);

// Write or update a key.  Returns 0 on success, -1 on error or full.
int  kv_set(const char *key, const char *val);

// Read a value.  Returns 0 on success, -1 if key not found.
int  kv_get(const char *key, char *val, size_t valsz);

// Delete a key.  Returns 0 on success, -1 if not found.
int  kv_del(const char *key);

// Iterate all entries.  Callback receives key, value, and user data.
// Returning 0 from the callback stops iteration.
typedef int (*kv_iter_fn)(const char *key, const char *val, void *ud);
void kv_list(kv_iter_fn fn, void *ud);

// Return 1 if the store was successfully loaded/initialised.
int  kv_ready(void);
