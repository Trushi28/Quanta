// ============================================================
//  fs/vfs.c — Virtual File System + RamFS + DevFS  (Phase 3)
//
//  Key changes:
//    • node_alloc() assigns an inode number, timestamps, and
//      default permissions on every node creation.
//    • ramfs read/write update atime/mtime on the node.
//    • New public API: vfs_stat2, vfs_mkdir, vfs_unlink,
//      vfs_rmdir, vfs_rename, vfs_chmod, vfs_touch.
//    • ramfs_ops updated with unlink/rmdir handlers.
//    • Path utility split_path() avoids code duplication.
// ============================================================
#include "vfs.h"
#include "../sched/sched.h"   // sched_uptime_ms() for timestamps
#include "../mm/heap.h"
#include "../lib/string.h"
#include "../lib/kprintf.h"
#include "../lib/spinlock.h"
#include <stddef.h>

// ── Global state ──────────────────────────────────────────────────────────
static vfs_node_t  *vfs_root = NULL;
static vfs_fd_t     fd_table[VFS_MAX_OPEN_FDS];
static spinlock_t   vfs_lock = SPINLOCK_INIT;
static uint32_t     g_next_inode = 1;   // monotonic inode counter

// ── Path helpers ──────────────────────────────────────────────────────────

// Split "/a/b/c" → parent="/a/b"  name="c"
// Returns 0 on success, -1 if path has no '/' or is too long.
static int split_path(const char *path,
                      char parent[VFS_PATH_MAX],
                      char name[VFS_NAME_MAX])
{
    size_t len = strlen(path);
    if (len == 0) return -1;

    // Find last '/'
    const char *last = path + len - 1;
    while (last > path && *last != '/') last--;

    size_t parent_len = (size_t)(last - path);
    if (parent_len == 0) {
        parent[0] = '/'; parent[1] = '\0';
    } else {
        if (parent_len >= VFS_PATH_MAX) parent_len = VFS_PATH_MAX - 1;
        memcpy(parent, path, parent_len);
        parent[parent_len] = '\0';
    }

    const char *base = (*last == '/') ? last + 1 : last;
    size_t name_len  = strlen(base);
    if (name_len == 0 || name_len >= VFS_NAME_MAX) return -1;
    memcpy(name, base, name_len + 1);
    return 0;
}

// ── Node allocation ───────────────────────────────────────────────────────
static vfs_node_t *node_alloc(const char *name, vfs_node_type_t type,
                               const vfs_ops_t *ops)
{
    vfs_node_t *n = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
    if (!n) return NULL;
    memset(n, 0, sizeof(*n));

    strncpy(n->name, name, VFS_NAME_MAX - 1);
    n->name[VFS_NAME_MAX - 1] = '\0';

    n->type      = type;
    n->ops       = ops;
    n->inode_nr  = g_next_inode++;
    n->mode      = (type == VFS_DIR) ? VFS_MODE_DIR : VFS_MODE_FILE;
    n->uid       = 0;
    n->gid       = 0;
    n->nlinks    = 1;

    uint64_t now = sched_uptime_ms();
    n->ctime = n->mtime = n->atime = now;

    list_init(&n->children);
    list_init(&n->sibling);
    return n;
}

static void dir_add_child(vfs_node_t *dir, vfs_node_t *child) {
    if (!dir || !child) return;
    child->parent = dir;
    list_append(&dir->children, &child->sibling);
}

// ── Path resolution ───────────────────────────────────────────────────────
vfs_node_t *vfs_lookup(const char *path) {
    if (!path || path[0] != '/') return NULL;
    vfs_node_t *cur = vfs_root;
    if (!cur) return NULL;

    char buf[VFS_PATH_MAX];
    strncpy(buf, path, VFS_PATH_MAX - 1);
    buf[VFS_PATH_MAX - 1] = '\0';

    char *p = buf + 1;
    while (*p) {
        char *sep = strchr(p, '/');
        if (sep) *sep = '\0';

        if (!*p) { p = sep ? sep + 1 : p + strlen(p); continue; }

        vfs_node_t *found = NULL;
        if (cur->ops && cur->ops->lookup)
            found = cur->ops->lookup(cur, p);
        else {
            list_foreach(&cur->children, node) {
                vfs_node_t *ch = container_of(node, vfs_node_t, sibling);
                if (strcmp(ch->name, p) == 0) { found = ch; break; }
            }
        }
        if (!found) return NULL;
        cur = found;
        p   = sep ? sep + 1 : p + strlen(p);
    }
    return cur;
}

// ── vfs_init ──────────────────────────────────────────────────────────────
void vfs_init(void) {
    memset(fd_table, 0, sizeof(fd_table));
    vfs_root = ramfs_create_root();
    if (!vfs_root) kpanic("[VFS] Failed to create root\n");
    devfs_init();
    kprintf("[VFS] Mounted ramfs at /  devfs at /dev\n");
}

int vfs_mount_root(vfs_node_t *root) { vfs_root = root; return 0; }

int vfs_register(const char *path, vfs_node_t *node) {
    char parent_path[VFS_PATH_MAX], name[VFS_NAME_MAX];
    if (split_path(path, parent_path, name) < 0) return -1;
    vfs_node_t *parent = vfs_lookup(parent_path);
    if (!parent) return -1;
    dir_add_child(parent, node);
    return 0;
}

// ── FD helpers ────────────────────────────────────────────────────────────
static int alloc_fd(void) {
    // Reserve 0/1/2 for process stdio. Ring 3 syscalls treat fd 0 as keyboard
    // and fd 1/2 as console output, so VFS-backed descriptors start at 3.
    for (int i = 3; i < VFS_MAX_OPEN_FDS; i++)
        if (!fd_table[i].valid) return i;
    return -1;
}

// ── Standard file operations ──────────────────────────────────────────────
int vfs_open(const char *path, int flags) {
    vfs_node_t *node = vfs_lookup(path);

    if (!node && (flags & O_CREAT)) {
        char pp[VFS_PATH_MAX], nm[VFS_NAME_MAX];
        if (split_path(path, pp, nm) < 0) return -1;
        vfs_node_t *parent = vfs_lookup(pp);
        if (!parent || !parent->ops || !parent->ops->create) return -1;
        parent->ops->create(parent, nm);
        node = vfs_lookup(path);
    }
    if (!node) return -1;

    uint64_t rflags = spinlock_irq_acquire(&vfs_lock);
    int fd = alloc_fd();
    if (fd < 0) { spinlock_irq_release(&vfs_lock, rflags); return -1; }
    fd_table[fd] = (vfs_fd_t){ .node = node, .offset = 0,
                                .flags = flags, .valid = true };
    node->ref_count++;
    spinlock_irq_release(&vfs_lock, rflags);

    // Truncate if O_TRUNC
    if ((flags & O_TRUNC) && node->ops && node->ops->write) {
        node->size = 0;
        node->mtime = sched_uptime_ms();
    }

    if (node->ops && node->ops->open) node->ops->open(node, flags);
    node->atime = sched_uptime_ms();
    return fd;
}

int vfs_close(int fd) {
    if (fd < 0 || fd >= VFS_MAX_OPEN_FDS || !fd_table[fd].valid) return -1;
    uint64_t rflags = spinlock_irq_acquire(&vfs_lock);
    vfs_node_t *node = fd_table[fd].node;
    fd_table[fd].valid = false;
    if (node->ref_count) node->ref_count--;
    spinlock_irq_release(&vfs_lock, rflags);
    if (node->ops && node->ops->close) node->ops->close(node);
    return 0;
}

ssize_t vfs_read(int fd, void *buf, size_t len) {
    if (fd < 0 || fd >= VFS_MAX_OPEN_FDS || !fd_table[fd].valid) return -1;
    vfs_fd_t   *f = &fd_table[fd];
    if (!f->node->ops || !f->node->ops->read) return -1;
    ssize_t n = f->node->ops->read(f->node, buf, len, f->offset);
    if (n > 0) {
        f->offset += (uint64_t)n;
        f->node->atime = sched_uptime_ms();
    }
    return n;
}

ssize_t vfs_write(int fd, const void *buf, size_t len) {
    if (fd < 0 || fd >= VFS_MAX_OPEN_FDS || !fd_table[fd].valid) return -1;
    vfs_fd_t   *f = &fd_table[fd];
    if (!f->node->ops || !f->node->ops->write) return -1;
    uint64_t off = (f->flags & O_APPEND) ? f->node->size : f->offset;
    ssize_t n = f->node->ops->write(f->node, buf, len, off);
    if (n > 0) {
        f->offset = off + (uint64_t)n;
        f->node->mtime = sched_uptime_ms();
        f->node->atime = f->node->mtime;
    }
    return n;
}

int vfs_readdir(int fd, uint32_t idx, char name[VFS_NAME_MAX]) {
    if (fd < 0 || fd >= VFS_MAX_OPEN_FDS || !fd_table[fd].valid) return -1;
    vfs_node_t *node = fd_table[fd].node;
    if (!node->ops || !node->ops->readdir) return -1;
    return node->ops->readdir(node, idx, name);
}

int vfs_seek(int fd, int64_t offset, int whence) {
    if (fd < 0 || fd >= VFS_MAX_OPEN_FDS || !fd_table[fd].valid) return -1;
    vfs_fd_t *f = &fd_table[fd];
    switch (whence) {
        case 0: f->offset = (uint64_t)offset; break;
        case 1: f->offset = (uint64_t)((int64_t)f->offset + offset); break;
        case 2: f->offset = f->node->size; break;
    }
    return 0;
}

int vfs_stat(const char *path, uint64_t *size, vfs_node_type_t *type) {
    vfs_node_t *n = vfs_lookup(path);
    if (!n) return -1;
    if (size) *size = n->size;
    if (type) *type = n->type;
    return 0;
}

// ── Phase 3: rich stat ────────────────────────────────────────────────────
int vfs_stat2(const char *path, vfs_stat_t *st) {
    vfs_node_t *n = vfs_lookup(path);
    if (!n || !st) return -1;
    st->inode_nr = n->inode_nr;
    st->type     = n->type;
    st->mode     = n->mode;
    st->uid      = n->uid;
    st->gid      = n->gid;
    st->nlinks   = n->nlinks;
    st->size     = n->size;
    st->ctime    = n->ctime;
    st->mtime    = n->mtime;
    st->atime    = n->atime;
    return 0;
}

// ── Phase 3: mkdir ────────────────────────────────────────────────────────
int vfs_mkdir(const char *path, uint16_t mode) {
    char pp[VFS_PATH_MAX], nm[VFS_NAME_MAX];
    if (split_path(path, pp, nm) < 0) return -1;

    vfs_node_t *parent = vfs_lookup(pp);
    if (!parent || parent->type != VFS_DIR) return -1;

    // Already exists?
    if (parent->ops && parent->ops->lookup &&
        parent->ops->lookup(parent, nm)) return -1;

    if (!parent->ops || !parent->ops->mkdir) return -1;
    int r = parent->ops->mkdir(parent, nm);
    if (r == 0) {
        // Apply requested mode
        vfs_node_t *newdir = parent->ops->lookup(parent, nm);
        if (newdir) {
            newdir->mode = mode & 0777u;
            parent->mtime = sched_uptime_ms();
        }
    }
    return r;
}

// ── Phase 3: unlink (remove file) ────────────────────────────────────────
int vfs_unlink(const char *path) {
    char pp[VFS_PATH_MAX], nm[VFS_NAME_MAX];
    if (split_path(path, pp, nm) < 0) return -1;

    vfs_node_t *parent = vfs_lookup(pp);
    if (!parent) return -1;

    vfs_node_t *target = NULL;
    if (parent->ops && parent->ops->lookup)
        target = parent->ops->lookup(parent, nm);
    if (!target) return -1;
    if (target->type == VFS_DIR) return -1; // use rmdir

    if (!parent->ops || !parent->ops->unlink) return -1;
    int r = parent->ops->unlink(parent, nm);
    if (r == 0) parent->mtime = sched_uptime_ms();
    return r;
}

// ── Phase 3: rmdir ────────────────────────────────────────────────────────
int vfs_rmdir(const char *path) {
    char pp[VFS_PATH_MAX], nm[VFS_NAME_MAX];
    if (split_path(path, pp, nm) < 0) return -1;

    vfs_node_t *parent = vfs_lookup(pp);
    if (!parent) return -1;
    if (!parent->ops || !parent->ops->rmdir) return -1;
    int r = parent->ops->rmdir(parent, nm);
    if (r == 0) parent->mtime = sched_uptime_ms();
    return r;
}

// ── Phase 3: rename ───────────────────────────────────────────────────────
int vfs_rename(const char *old_path, const char *new_path) {
    char op[VFS_PATH_MAX], on[VFS_NAME_MAX];
    char np[VFS_PATH_MAX], nn[VFS_NAME_MAX];
    if (split_path(old_path, op, on) < 0) return -1;
    if (split_path(new_path, np, nn) < 0) return -1;

    vfs_node_t *old_parent = vfs_lookup(op);
    vfs_node_t *new_parent = vfs_lookup(np);
    if (!old_parent || !new_parent) return -1;

    vfs_node_t *node = NULL;
    if (old_parent->ops && old_parent->ops->lookup)
        node = old_parent->ops->lookup(old_parent, on);
    if (!node) return -1;

    // Detach from old parent
    list_remove(&node->sibling);
    old_parent->mtime = sched_uptime_ms();

    // Update name
    strncpy(node->name, nn, VFS_NAME_MAX - 1);
    node->name[VFS_NAME_MAX - 1] = '\0';
    node->mtime = sched_uptime_ms();

    // Attach to new parent
    dir_add_child(new_parent, node);
    new_parent->mtime = sched_uptime_ms();
    return 0;
}

// ── Phase 3: chmod ────────────────────────────────────────────────────────
int vfs_chmod(const char *path, uint16_t mode) {
    vfs_node_t *n = vfs_lookup(path);
    if (!n) return -1;
    n->mode = mode & 0777u;
    n->mtime = sched_uptime_ms();
    return 0;
}

// ── Phase 3: touch ────────────────────────────────────────────────────────
int vfs_touch(const char *path) {
    vfs_node_t *n = vfs_lookup(path);
    if (n) {
        // Update timestamps
        uint64_t now = sched_uptime_ms();
        n->mtime = n->atime = now;
        return 0;
    }
    // Create the file if it doesn't exist
    int fd = vfs_open(path, O_WRONLY | O_CREAT);
    if (fd < 0) return -1;
    vfs_close(fd);
    return 0;
}

// ============================================================
//  RamFS — in-memory filesystem with full metadata
// ============================================================

typedef struct {
    uint8_t *data;
    size_t   capacity;
} ramfs_file_t;

// Forward declarations
static int ramfs_create(vfs_node_t *parent, const char *name);
static int ramfs_mkdir_op(vfs_node_t *parent, const char *name);

// ── ramfs ops ─────────────────────────────────────────────────────────────
static ssize_t ramfs_read(vfs_node_t *n, void *buf, size_t len, uint64_t off) {
    ramfs_file_t *f = (ramfs_file_t *)n->fs_data;
    if (!f || off >= n->size) return 0;
    size_t avail = (size_t)(n->size - off);
    if (len > avail) len = avail;
    memcpy(buf, f->data + off, len);
    return (ssize_t)len;
}

static ssize_t ramfs_write(vfs_node_t *n, const void *buf, size_t len, uint64_t off) {
    ramfs_file_t *f = (ramfs_file_t *)n->fs_data;
    if (!f) return -1;
    uint64_t end = off + len;
    if (end > f->capacity) {
        size_t nc = (size_t)(end * 2);
        if (nc < 64) nc = 64;
        uint8_t *nd = (uint8_t *)krealloc(f->data, nc);
        if (!nd) return -1;
        f->data     = nd;
        f->capacity = nc;
    }
    memcpy(f->data + off, buf, len);
    if (end > n->size) n->size = end;
    return (ssize_t)len;
}

static int ramfs_readdir(vfs_node_t *dir, uint32_t idx, char name[VFS_NAME_MAX]) {
    uint32_t i = 0;
    list_foreach(&dir->children, node) {
        vfs_node_t *ch = container_of(node, vfs_node_t, sibling);
        if (i == idx) {
            strncpy(name, ch->name, VFS_NAME_MAX - 1);
            name[VFS_NAME_MAX - 1] = '\0';
            return 0;
        }
        i++;
    }
    return -1;
}

static vfs_node_t *ramfs_lookup(vfs_node_t *dir, const char *name) {
    list_foreach(&dir->children, node) {
        vfs_node_t *ch = container_of(node, vfs_node_t, sibling);
        if (strcmp(ch->name, name) == 0) return ch;
    }
    return NULL;
}

static int ramfs_unlink_op(vfs_node_t *parent, const char *name) {
    vfs_node_t *n = ramfs_lookup(parent, name);
    if (!n) return -1;
    if (n->type == VFS_DIR) return -1;
    list_remove(&n->sibling);
    n->nlinks--;
    if (n->nlinks == 0) {
        ramfs_file_t *f = (ramfs_file_t *)n->fs_data;
        if (f) { kfree(f->data); kfree(f); }
        kfree(n);
    }
    return 0;
}

static int ramfs_rmdir_op(vfs_node_t *parent, const char *name) {
    vfs_node_t *n = ramfs_lookup(parent, name);
    if (!n || n->type != VFS_DIR) return -1;
    if (!list_empty(&n->children)) return -1; // not empty
    list_remove(&n->sibling);
    kfree(n);
    return 0;
}

static const vfs_ops_t ramfs_file_ops = {
    .read    = ramfs_read,
    .write   = ramfs_write,
};

static const vfs_ops_t ramfs_dir_ops = {
    .readdir = ramfs_readdir,
    .lookup  = ramfs_lookup,
    .create  = ramfs_create,
    .mkdir   = ramfs_mkdir_op,
    .unlink  = ramfs_unlink_op,
    .rmdir   = ramfs_rmdir_op,
};

static int ramfs_create(vfs_node_t *parent, const char *name) {
    if (ramfs_lookup(parent, name)) return -1; // already exists

    ramfs_file_t *f = (ramfs_file_t *)kmalloc(sizeof(ramfs_file_t));
    if (!f) return -1;
    f->data     = (uint8_t *)kmalloc(64);
    f->capacity = 64;
    if (!f->data) { kfree(f); return -1; }
    memset(f->data, 0, 64);

    vfs_node_t *n = node_alloc(name, VFS_FILE, &ramfs_file_ops);
    if (!n) { kfree(f->data); kfree(f); return -1; }
    n->fs_data = f;
    dir_add_child(parent, n);
    parent->mtime = sched_uptime_ms();
    return 0;
}

static int ramfs_mkdir_op(vfs_node_t *parent, const char *name) {
    if (ramfs_lookup(parent, name)) return -1;
    vfs_node_t *n = node_alloc(name, VFS_DIR, &ramfs_dir_ops);
    if (!n) return -1;
    dir_add_child(parent, n);
    parent->mtime = sched_uptime_ms();
    return 0;
}

vfs_node_t *ramfs_create_root(void) {
    vfs_node_t *root = node_alloc("/", VFS_DIR, &ramfs_dir_ops);
    if (!root) return NULL;
    // Create standard top-level directories
    ramfs_mkdir_op(root, "dev");
    ramfs_mkdir_op(root, "tmp");
    ramfs_mkdir_op(root, "sys");
    ramfs_mkdir_op(root, "home");
    ramfs_mkdir_op(root, "proc");
    return root;
}

// ============================================================
//  DevFS — /dev character devices
// ============================================================

static vfs_node_t *devfs_dir = NULL;

static ssize_t null_read (vfs_node_t *n, void *b, size_t l, uint64_t o)
    { (void)n;(void)b;(void)l;(void)o; return 0; }
static ssize_t null_write(vfs_node_t *n, const void *b, size_t l, uint64_t o)
    { (void)n;(void)b;(void)o; return (ssize_t)l; }
static const vfs_ops_t null_ops = { .read=null_read, .write=null_write };

static ssize_t zero_read(vfs_node_t *n, void *b, size_t l, uint64_t o)
    { (void)n;(void)o; memset(b,0,l); return (ssize_t)l; }
static const vfs_ops_t zero_ops = { .read=zero_read, .write=null_write };

void devfs_init(void) {
    devfs_dir = vfs_lookup("/dev");
    if (!devfs_dir) { kprintf("[DevFS] /dev not found\n"); return; }
    devfs_register("null", &null_ops, NULL);
    devfs_register("zero", &zero_ops, NULL);
    kprintf("[DevFS] /dev/null  /dev/zero  registered\n");
}

int devfs_register(const char *name, const vfs_ops_t *ops, void *priv) {
    if (!devfs_dir) return -1;
    vfs_node_t *n = node_alloc(name, VFS_CHARDEV, ops);
    if (!n) return -1;
    n->fs_data = priv;
    n->mode    = VFS_MODE_DEV;
    dir_add_child(devfs_dir, n);
    return 0;
}
