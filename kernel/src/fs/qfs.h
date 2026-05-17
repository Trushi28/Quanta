#pragma once
#include <stdint.h>

// Seed the Quanta-native filesystem namespace on top of the current RamFS
// substrate. This is the Phase 6 prototype layer for QuantaFS-Weave.
void qfs_seed_namespace(uint64_t kernel_phys_base, uint64_t kernel_virt_base,
                        uint64_t kernel_entry);

// Called after VirtIO/KV storage probing. Records the boot epoch into the
// QFS journal area when a block device is available, and mirrors the result
// back into the /qfs timeline view.
void qfs_storage_checkpoint(void);
