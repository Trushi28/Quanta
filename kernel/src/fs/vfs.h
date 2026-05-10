#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "../lib/list.h"

typedef long ssize_t;

// ---------------------------------------------------------------------------
//  fs/vfs.h — Quanta Virtual File System  (Phase 3 revision)
//
//  Phase 3 additions:
//    • vfs_node_t gains full inode metadata:
//        inode_nr  — unique inode number
//        mode      — Unix-style rwxrwxrwx permissions
//        uid/gid   — owner (kernel always 0:0 for now)
//        nlinks    — hard-link reference count
//        ctime / mtime / atime — ms-since-boot timestamps
//    • vfs_stat_t — rich stat structure for `ls -l` / `stat`
//    • vfs_stat2()  — fill a vfs_stat_t
//    • vfs_mkdir()  — create a directory
//    • vfs_unlink() — remove a file
//    • vfs_rmdir()  — remove an empty directory
//    • vfs_rename() — rename / move a node
//    • vfs_chmod()  — change permissions
//    • vfs_touch()  — create file or update mtime
// ---------------------------------------------------------------------------

#define VFS_PATH_MAX     256
#define VFS_NAME_MAX     64
#define VFS_MAX_OPEN_FDS 64

// ── Unix permission bits (stored in vfs_node_t.mode) ─────────────────────
#define VFS_MODE_IRWXU  0700u   // owner rwx
#define VFS_MODE_IRUSR  0400u
#define VFS_MODE_IWUSR  0200u
#define VFS_MODE_IXUSR  0100u
#define VFS_MODE_IRWXG  0070u   // group rwx
#define VFS_MODE_IRGRP  0040u
#define VFS_MODE_IWGRP  0020u
#define VFS_MODE_IXGRP  0010u
#define VFS_MODE_IRWXO  0007u   // other rwx
#define VFS_MODE_IROTH  0004u
#define VFS_MODE_IWOTH  0002u
#define VFS_MODE_IXOTH  0001u

// Convenience defaults
#define VFS_MODE_FILE   0644u   // -rw-r--r--
#define VFS_MODE_DIR    0755u   // drwxr-xr-x
#define VFS_MODE_DEV    0644u   // crw-r--r--

// ── Node types ────────────────────────────────────────────────────────────
typedef enum {
    VFS_FILE     = 0,
    VFS_DIR      = 1,
    VFS_SYMLINK  = 2,
    VFS_CHARDEV  = 3,
    VFS_BLOCKDEV = 4,
} vfs_node_type_t;

// ── Operations vtable ─────────────────────────────────────────────────────
struct vfs_node;

typedef struct vfs_ops {
    int      (*open)   (struct vfs_node *n, int flags);
    void     (*close)  (struct vfs_node *n);
    ssize_t  (*read)   (struct vfs_node *n, void *buf, size_t len, uint64_t off);
    ssize_t  (*write)  (struct vfs_node *n, const void *buf, size_t len, uint64_t off);
    int      (*readdir)(struct vfs_node *dir, uint32_t idx, char name[VFS_NAME_MAX]);
    struct vfs_node *(*lookup)(struct vfs_node *dir, const char *name);
    int      (*mkdir)  (struct vfs_node *parent, const char *name);
    int      (*create) (struct vfs_node *parent, const char *name);
    int      (*unlink) (struct vfs_node *parent, const char *name);
    int      (*rmdir)  (struct vfs_node *parent, const char *name);
    int      (*stat)   (struct vfs_node *n, uint64_t *size, vfs_node_type_t *type);
} vfs_ops_t;

// ── VFS node ──────────────────────────────────────────────────────────────
typedef struct vfs_node {
    // Identity
    char              name[VFS_NAME_MAX];
    vfs_node_type_t   type;
    uint32_t          inode_nr;      // unique per filesystem

    // Metadata (Phase 3)
    uint16_t          mode;          // Unix rwxrwxrwx
    uint32_t          uid, gid;      // owner (0:0 = kernel/root)
    uint32_t          nlinks;        // hard link count
    uint64_t          ctime;         // creation time  (ms since boot)
    uint64_t          mtime;         // last modification time
    uint64_t          atime;         // last access time

    // Data
    uint64_t          size;
    uint32_t          ref_count;     // open FD references
    const vfs_ops_t  *ops;
    void             *fs_data;       // filesystem-private blob

    // Tree
    struct vfs_node  *parent;
    list_t            children;
    list_node_t       sibling;
} vfs_node_t;

// ── Rich stat structure (Phase 3) ─────────────────────────────────────────
typedef struct {
    uint32_t        inode_nr;
    vfs_node_type_t type;
    uint16_t        mode;
    uint32_t        uid, gid;
    uint32_t        nlinks;
    uint64_t        size;
    uint64_t        ctime;   // ms since boot
    uint64_t        mtime;
    uint64_t        atime;
} vfs_stat_t;

// ── File descriptor ────────────────────────────────────────────────────────
#define O_RDONLY  0
#define O_WRONLY  1
#define O_RDWR    2
#define O_CREAT   4
#define O_TRUNC   8
#define O_APPEND  16

typedef struct {
    vfs_node_t *node;
    uint64_t    offset;
    int         flags;
    bool        valid;
} vfs_fd_t;

// ── Core API ──────────────────────────────────────────────────────────────
void  vfs_init(void);
int   vfs_mount_root(vfs_node_t *root);
int   vfs_register(const char *path, vfs_node_t *node);

int     vfs_open   (const char *path, int flags);
int     vfs_close  (int fd);
ssize_t vfs_read   (int fd, void *buf, size_t len);
ssize_t vfs_write  (int fd, const void *buf, size_t len);
int     vfs_readdir(int fd, uint32_t idx, char name[VFS_NAME_MAX]);
int     vfs_seek   (int fd, int64_t offset, int whence);

// Legacy single-call stat (kept for compatibility)
int  vfs_stat(const char *path, uint64_t *size, vfs_node_type_t *type);

// ── Phase 3 rich API ──────────────────────────────────────────────────────
int  vfs_stat2  (const char *path, vfs_stat_t *st);
int  vfs_mkdir  (const char *path, uint16_t mode);
int  vfs_unlink (const char *path);
int  vfs_rmdir  (const char *path);
int  vfs_rename (const char *old_path, const char *new_path);
int  vfs_chmod  (const char *path, uint16_t mode);
int  vfs_touch  (const char *path);   // create if missing, else update mtime

// ── Path resolution ───────────────────────────────────────────────────────
vfs_node_t *vfs_lookup(const char *path);

// ── RamFS ─────────────────────────────────────────────────────────────────
vfs_node_t *ramfs_create_root(void);

// ── DevFS ─────────────────────────────────────────────────────────────────
void devfs_init(void);
int  devfs_register(const char *name, const vfs_ops_t *ops, void *priv);
