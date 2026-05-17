// ============================================================
//  fs/qfs.c — QuantaFS-Weave namespace seed (Phase 6)
//
//  This is not a FAT/ext clone. The first tranche models the future
//  filesystem as a graph of capsules, tags, timelines, and generated views,
//  while using the existing RamFS as the temporary storage substrate.
// ============================================================
#include "qfs.h"
#include "vfs.h"
#include "../drivers/virtio/virtio.h"
#include "../lib/string.h"
#include "../lib/kprintf.h"
#include "../version.h"
#include <stddef.h>
#include <stdint.h>

#define QFS_JOURNAL_SECTOR 4096u
#define QFS_CAP_PATH_MAX   96u

static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (i * 8));
}

static void put64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (i * 8));
}

static void qfs_write_file(const char *path, const void *data, size_t len) {
    int fd = vfs_open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        kprintf("[QFS] Could not create %s\n", path);
        return;
    }
    vfs_write(fd, data, len);
    vfs_close(fd);
}

static void qfs_write_text(const char *path, const char *text) {
    qfs_write_file(path, text, strlen(text));
}

static void qfs_append_text(const char *path, const char *text) {
    int fd = vfs_open(path, O_WRONLY | O_CREAT | O_APPEND);
    if (fd < 0) {
        kprintf("[QFS] Could not append %s\n", path);
        return;
    }
    vfs_write(fd, text, strlen(text));
    vfs_close(fd);
}

static void cat2(char *dst, size_t dstsz, const char *a, const char *b) {
    if (!dstsz) return;
    dst[0] = '\0';
    strncpy(dst, a, dstsz - 1);
    dst[dstsz - 1] = '\0';
    size_t used = strlen(dst);
    if (used < dstsz - 1)
        strncpy(dst + used, b, dstsz - used - 1);
    dst[dstsz - 1] = '\0';
}

static void append_str(char *buf, size_t bufsz, const char *s) {
    size_t used = strlen(buf);
    if (used >= bufsz - 1) return;
    strncpy(buf + used, s, bufsz - used - 1);
    buf[bufsz - 1] = '\0';
}

static void append_u64(char *buf, size_t bufsz, uint64_t v, int base) {
    char tmp[32];
    kuitoa(v, tmp, base);
    append_str(buf, bufsz, tmp);
}

static uint64_t qfs_hash_file(const char *path, uint64_t *size_out) {
    uint64_t hash = 1469598103934665603ULL; // FNV-1a 64-bit
    uint64_t size = 0;
    uint8_t chunk[128];

    int fd = vfs_open(path, O_RDONLY);
    if (fd < 0) {
        if (size_out) *size_out = 0;
        return 0;
    }

    for (;;) {
        ssize_t n = vfs_read(fd, chunk, sizeof(chunk));
        if (n <= 0)
            break;
        for (ssize_t i = 0; i < n; i++) {
            hash ^= chunk[i];
            hash *= 1099511628211ULL;
        }
        size += (uint64_t)n;
    }
    vfs_close(fd);

    if (size_out) *size_out = size;
    return hash;
}

static void qfs_register_capsule(const char *path, const char *kind,
                                 const char *realm, const char *tag) {
    uint64_t size = 0;
    uint64_t hash = qfs_hash_file(path, &size);
    if (!hash && !size)
        return;

    char hash_hex[32];
    kuitoa(hash, hash_hex, 16);

    char cap_path[QFS_CAP_PATH_MAX];
    cat2(cap_path, sizeof(cap_path), "/qfs/capsules/", hash_hex);
    append_str(cap_path, sizeof(cap_path), ".cap");

    char manifest[512];
    manifest[0] = '\0';
    append_str(manifest, sizeof(manifest), "capsule=");
    append_str(manifest, sizeof(manifest), hash_hex);
    append_str(manifest, sizeof(manifest), "\npath=");
    append_str(manifest, sizeof(manifest), path);
    append_str(manifest, sizeof(manifest), "\nkind=");
    append_str(manifest, sizeof(manifest), kind);
    append_str(manifest, sizeof(manifest), "\nrealm=");
    append_str(manifest, sizeof(manifest), realm);
    append_str(manifest, sizeof(manifest), "\nbytes=");
    append_u64(manifest, sizeof(manifest), size, 10);
    append_str(manifest, sizeof(manifest), "\nfnv64=");
    append_str(manifest, sizeof(manifest), hash_hex);
    append_str(manifest, sizeof(manifest), "\nstate=sealed\n");
    qfs_write_text(cap_path, manifest);
    qfs_append_text("/qfs/catalog", hash_hex);
    qfs_append_text("/qfs/catalog", " ");
    qfs_append_text("/qfs/catalog", path);
    qfs_append_text("/qfs/catalog", "\n");

    char view_path[QFS_CAP_PATH_MAX];
    cat2(view_path, sizeof(view_path), "/qfs/views/by-kind/", kind);
    qfs_append_text(view_path, path);
    qfs_append_text(view_path, "\n");

    cat2(view_path, sizeof(view_path), "/qfs/views/by-realm/", realm);
    qfs_append_text(view_path, path);
    qfs_append_text(view_path, "\n");

    if (tag && *tag) {
        cat2(view_path, sizeof(view_path), "/qfs/tags/", tag);
        qfs_append_text(view_path, path);
        qfs_append_text(view_path, "\n");
    }
}

static void qfs_make_kernel_elf(uint8_t out[120], uint64_t phys_base,
                                uint64_t virt_base, uint64_t entry) {
    memset(out, 0, 120);

    out[0] = 0x7f;
    out[1] = 'E';
    out[2] = 'L';
    out[3] = 'F';
    out[4] = 2; // ELFCLASS64
    out[5] = 1; // ELFDATA2LSB
    out[6] = 1; // EV_CURRENT

    put16(out + 16, 2);       // ET_EXEC
    put16(out + 18, 0x3e);    // EM_X86_64
    put32(out + 20, 1);       // EV_CURRENT
    put64(out + 24, entry);   // e_entry
    put64(out + 32, 64);      // e_phoff
    put64(out + 40, 0);       // e_shoff
    put32(out + 48, 0);       // e_flags
    put16(out + 52, 64);      // e_ehsize
    put16(out + 54, 56);      // e_phentsize
    put16(out + 56, 1);       // e_phnum

    uint8_t *ph = out + 64;
    put32(ph + 0, 1);         // PT_LOAD
    put32(ph + 4, 5);         // PF_R | PF_X
    put64(ph + 8, 0);         // p_offset
    put64(ph + 16, virt_base);
    put64(ph + 24, phys_base);
    put64(ph + 32, 0);        // descriptor only for now
    put64(ph + 40, 0);
    put64(ph + 48, 0x1000);
}

void qfs_seed_namespace(uint64_t kernel_phys_base, uint64_t kernel_virt_base,
                        uint64_t kernel_entry) {
    vfs_mkdir("/system", VFS_MODE_DIR);
    vfs_mkdir("/apps", VFS_MODE_DIR);
    vfs_mkdir("/qfs", VFS_MODE_DIR);
    vfs_mkdir("/qfs/capsules", VFS_MODE_DIR);
    vfs_mkdir("/qfs/tags", VFS_MODE_DIR);
    vfs_mkdir("/qfs/timeline", VFS_MODE_DIR);
    vfs_mkdir("/qfs/views", VFS_MODE_DIR);
    vfs_mkdir("/qfs/views/by-kind", VFS_MODE_DIR);
    vfs_mkdir("/qfs/views/by-realm", VFS_MODE_DIR);
    qfs_write_text("/qfs/catalog", "");

    static const char qfs_plan[] =
        "QuantaFS-Weave Phase 6 plan\n"
        "\n"
        "Core model:\n"
        "  capsule  = immutable byte extent + typed metadata\n"
        "  weave    = ordered relations between capsules\n"
        "  tag      = searchable semantic label, not a directory copy\n"
        "  epoch    = append-only namespace checkpoint\n"
        "  view     = generated path projection over the object graph\n"
        "\n"
        "Why it is not FAT/ext/Btrfs:\n"
        "  no block-group directory tree as the source of truth\n"
        "  no inode table as the primary API\n"
        "  no copy-on-write B-tree clone\n"
        "  paths are views; capsules and relations are canonical\n"
        "\n"
        "Current tranche:\n"
        "  RamFS-backed seed namespace\n"
        "  /system/kernel.elf Ring 0 ELF descriptor visible to Ring 3\n"
        "  /apps/hello.wasm sample binary for Realm routing\n"
        "  /qfs/capsules/* sealed object manifests with FNV-1a IDs\n"
        "  /qfs/timeline/* boot and storage epochs\n"
        "  /qfs/views/* generated namespace projections\n";
    qfs_write_text("/system/qfs.plan", qfs_plan);

    static const char qfs_super[] =
        "QFSW\001\000\000\000"
        "features=capsules,weaves,tags,epochs,views\n"
        "substrate=ramfs-prototype\n";
    qfs_write_file("/qfs/super.qfs", qfs_super, sizeof(qfs_super) - 1);

    static const char epoch0[] =
        "epoch=0\n"
        "root=/\n"
        "state=boot-seed\n"
        "policy=append-intent-before-persist\n";
    qfs_write_text("/qfs/timeline/00000000.epoch", epoch0);

    static const char kernel_manifest[] =
        "capsule=kernel-ring0\n"
        "kind=elf64-descriptor\n"
        "realm=kernel\n"
        "path=/system/kernel.elf\n"
        "version=" QUANTA_VERSION "\n";
    qfs_write_text("/system/kernel.manifest", kernel_manifest);
    qfs_write_text("/qfs/tags/kernel", "/system/kernel.elf\n/system/kernel.manifest\n");
    qfs_write_text("/qfs/views/by-kind/elf", "");
    qfs_write_text("/qfs/views/by-kind/wasm", "");
    qfs_write_text("/qfs/views/by-kind/text", "");
    qfs_write_text("/qfs/views/by-realm/kernel", "");
    qfs_write_text("/qfs/views/by-realm/native-init", "");

    uint8_t kernel_elf[120];
    qfs_make_kernel_elf(kernel_elf, kernel_phys_base, kernel_virt_base,
                        kernel_entry);
    qfs_write_file("/system/kernel.elf", kernel_elf, sizeof(kernel_elf));

    static const uint8_t hello_wasm[] = {
        0x00, 'a', 's', 'm', 0x01, 0x00, 0x00, 0x00,
        0x00, 0x17, 0x06, 'q', 'u', 'a', 'n', 't', 'a',
        'h', 'e', 'l', 'l', 'o', ' ', 'f', 'r', 'o', 'm',
        ' ', 'w', 'a', 's', 'm', '\n'
    };
    qfs_write_file("/apps/hello.wasm", hello_wasm, sizeof(hello_wasm));

    qfs_write_text("/qfs/tags/kernel", "");
    qfs_write_text("/qfs/tags/wasm", "");
    qfs_write_text("/qfs/tags/docs", "");
    qfs_register_capsule("/system/kernel.elf", "elf", "kernel", "kernel");
    qfs_register_capsule("/system/kernel.manifest", "text", "kernel", "kernel");
    qfs_register_capsule("/system/qfs.plan", "text", "native-init", "docs");
    qfs_register_capsule("/apps/hello.wasm", "wasm", "native-init", "wasm");

    kprintf("[QFS] QuantaFS-Weave seed namespace online  (/system /qfs /apps)\n");
}

void qfs_storage_checkpoint(void) {
    virtio_blk_info_t info;
    memset(&info, 0, sizeof(info));
    virtio_blk_info(&info);

    uint8_t sector[512];
    memset(sector, 0, sizeof(sector));
    memcpy(sector, "QFSWJRN", 7);
    sector[7] = 1;
    put64(sector + 8, QFS_JOURNAL_SECTOR);
    put64(sector + 16, info.capacity);
    put64(sector + 24, 1);
    memcpy(sector + 64, "epoch=1\nstate=storage-checkpoint\n", 33);

    char status[256];
    status[0] = '\0';
    append_str(status, sizeof(status), "epoch=1\nstate=");

    if (!info.capacity) {
        append_str(status, sizeof(status), "storage-offline\n");
        append_str(status, sizeof(status),
                   "detail=virtio-blk unavailable; RamFS substrate active\n");
        qfs_write_text("/qfs/timeline/00000001-storage.epoch", status);
        return;
    }

    int rc = virtio_blk_write(QFS_JOURNAL_SECTOR, 1, sector);
    append_str(status, sizeof(status),
               rc == 0 ? "journal-written\n" : "journal-write-failed\n");
    append_str(status, sizeof(status), "sector=");
    append_u64(status, sizeof(status), QFS_JOURNAL_SECTOR, 10);
    append_str(status, sizeof(status), "\ncapacity_sectors=");
    append_u64(status, sizeof(status), info.capacity, 10);
    append_str(status, sizeof(status), "\n");
    qfs_write_text("/qfs/timeline/00000001-storage.epoch", status);

    if (rc == 0)
        kprintf("[QFS] Journal checkpoint written at sector %u\n",
                (unsigned)QFS_JOURNAL_SECTOR);
    else
        kprintf("[QFS] Journal checkpoint failed at sector %u\n",
                (unsigned)QFS_JOURNAL_SECTOR);
}
