#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "../lib/list.h"

// Freestanding ssize_t
typedef long ssize_t;

// ---------------------------------------------------------------------------
//  fs/vfs.h — Quanta Virtual File System (VFS)
//
//  Provides an abstraction layer over concrete filesystems.
//  All I/O goes through vfs_open / vfs_read / vfs_write / vfs_close.
//  Currently supports: ramfs (in-memory), devfs (/dev/)
// ---------------------------------------------------------------------------

#define VFS_PATH_MAX     256
#define VFS_NAME_MAX     64
#define VFS_MAX_OPEN_FDS 64

// ── Node types ────────────────────────────────────────────────────────────
typedef enum {
    VFS_FILE      = 0,
    VFS_DIR       = 1,
    VFS_SYMLINK   = 2,
    VFS_CHARDEV   = 3,
    VFS_BLOCKDEV  = 4,
} vfs_node_type_t;

// ── File-system operations vtable ─────────────────────────────────────────
struct vfs_node;

typedef struct vfs_ops {
    int    (*open)  (struct vfs_node *node, int flags);
    void   (*close) (struct vfs_node *node);
    ssize_t(*read)  (struct vfs_node *node, void *buf, size_t len, uint64_t off);
    ssize_t(*write) (struct vfs_node *node, const void *buf, size_t len, uint64_t off);
    int    (*readdir)(struct vfs_node *dir, uint32_t idx, char name[VFS_NAME_MAX]);
    struct vfs_node *(*lookup)(struct vfs_node *dir, const char *name);
    int    (*mkdir) (struct vfs_node *parent, const char *name);
    int    (*create)(struct vfs_node *parent, const char *name);
    int    (*unlink)(struct vfs_node *parent, const char *name);
    int    (*stat)  (struct vfs_node *node, uint64_t *size, vfs_node_type_t *type);
} vfs_ops_t;

// ── VFS node ──────────────────────────────────────────────────────────────
typedef struct vfs_node {
    char              name[VFS_NAME_MAX];
    vfs_node_type_t   type;
    uint64_t          size;       // file size in bytes
    uint32_t          ref_count;  // open file handles pointing here
    const vfs_ops_t  *ops;
    void             *fs_data;    // filesystem-private data
    struct vfs_node  *parent;
    list_t            children;   // child nodes (for directories)
    list_node_t       sibling;    // link in parent->children
} vfs_node_t;

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

// ── Public API ────────────────────────────────────────────────────────────
void  vfs_init(void);

// Mount a root filesystem node
int   vfs_mount_root(vfs_node_t *root);

// Register a filesystem node at an absolute path
int   vfs_register(const char *path, vfs_node_t *node);

// Standard file operations (return fd index ≥ 0, or negative on error)
int     vfs_open  (const char *path, int flags);
int     vfs_close (int fd);
ssize_t vfs_read  (int fd, void *buf, size_t len);
ssize_t vfs_write (int fd, const void *buf, size_t len);
int     vfs_readdir(int fd, uint32_t idx, char name[VFS_NAME_MAX]);
int     vfs_seek  (int fd, int64_t offset, int whence);
int     vfs_stat  (const char *path, uint64_t *size, vfs_node_type_t *type);

// Walk a path to find a node (NULL = not found)
vfs_node_t *vfs_lookup(const char *path);

// ── RamFS (in-memory filesystem) ─────────────────────────────────────────
vfs_node_t *ramfs_create_root(void);

// ── DevFS (/dev devices) ──────────────────────────────────────────────────
void devfs_init(void);
int  devfs_register(const char *name, const vfs_ops_t *ops, void *priv);
