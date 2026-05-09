// ============================================================
//  fs/vfs.c — Virtual File System layer + RamFS + DevFS  v2.1
//
//  Foundation fixes:
//    • All strncpy calls now explicitly null-terminated
//    • vfs_lookup buf[] always null-terminated
//    • vfs_open parent_path always null-terminated
//    • ramfs_create forward-declaration fixed
//    • dir_add_child checks for NULL
// ============================================================
#include "vfs.h"
#include "../mm/heap.h"
#include "../lib/string.h"
#include "../lib/kprintf.h"
#include "../lib/spinlock.h"
#include <stddef.h>

// ── Global state ──────────────────────────────────────────────────────────
static vfs_node_t   *vfs_root  = NULL;
static vfs_fd_t      fd_table[VFS_MAX_OPEN_FDS];
static spinlock_t    vfs_lock  = SPINLOCK_INIT;

// ── Node helpers ──────────────────────────────────────────────────────────
static vfs_node_t *node_alloc(const char *name, vfs_node_type_t type,
                               const vfs_ops_t *ops) {
    vfs_node_t *n = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
    if (!n) return NULL;
    memset(n, 0, sizeof(*n));
    strncpy(n->name, name, VFS_NAME_MAX - 1);
    n->name[VFS_NAME_MAX - 1] = '\0';
    n->type = type;
    n->ops  = ops;
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

    char *p = buf + 1;   // skip leading '/'
    while (*p) {
        char *sep = strchr(p, '/');
        if (sep) *sep = '\0';

        if (!*p) { p = sep ? sep+1 : p + strlen(p); continue; }

        vfs_node_t *found = NULL;
        if (cur->ops && cur->ops->lookup) {
            found = cur->ops->lookup(cur, p);
        } else {
            list_foreach(&cur->children, node) {
                vfs_node_t *child = container_of(node, vfs_node_t, sibling);
                if (strcmp(child->name, p) == 0) { found = child; break; }
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

int vfs_mount_root(vfs_node_t *root) {
    vfs_root = root;
    return 0;
}

int vfs_register(const char *path, vfs_node_t *node) {
    char buf[VFS_PATH_MAX];
    strncpy(buf, path, VFS_PATH_MAX - 1);
    buf[VFS_PATH_MAX - 1] = '\0';

    char *last_slash = NULL;
    for (char *p = buf; *p; p++) if (*p == '/') last_slash = p;
    if (!last_slash) return -1;

    char parent_path[VFS_PATH_MAX];
    size_t plen = (size_t)(last_slash - buf);
    if (!plen) {
        parent_path[0] = '/'; parent_path[1] = '\0';
    } else {
        if (plen >= VFS_PATH_MAX) plen = VFS_PATH_MAX - 1;
        memcpy(parent_path, buf, plen);
        parent_path[plen] = '\0';
    }

    vfs_node_t *parent = vfs_lookup(parent_path);
    if (!parent) return -1;
    dir_add_child(parent, node);
    return 0;
}

// ── File descriptor management ────────────────────────────────────────────
static int alloc_fd(void) {
    for (int i = 0; i < VFS_MAX_OPEN_FDS; i++)
        if (!fd_table[i].valid) return i;
    return -1;
}

int vfs_open(const char *path, int flags) {
    vfs_node_t *node = vfs_lookup(path);
    if (!node && (flags & O_CREAT)) {
        char parent_path[VFS_PATH_MAX];
        const char *slash = path + strlen(path);
        while (slash > path && *slash != '/') slash--;
        size_t plen = (size_t)(slash - path);
        if (!plen) { parent_path[0]='/'; parent_path[1]='\0'; }
        else {
            if (plen >= VFS_PATH_MAX) plen = VFS_PATH_MAX - 1;
            memcpy(parent_path, path, plen);
            parent_path[plen] = '\0';
        }

        vfs_node_t *parent = vfs_lookup(parent_path);
        if (!parent) return -1;
        if (parent->ops && parent->ops->create) {
            parent->ops->create(parent, slash + 1);
            node = vfs_lookup(path);
        }
    }
    if (!node) return -1;

    uint64_t rflags = spinlock_irq_acquire(&vfs_lock);
    int fd = alloc_fd();
    if (fd < 0) { spinlock_irq_release(&vfs_lock, rflags); return -1; }

    fd_table[fd].node   = node;
    fd_table[fd].offset = 0;
    fd_table[fd].flags  = flags;
    fd_table[fd].valid  = true;
    node->ref_count++;
    spinlock_irq_release(&vfs_lock, rflags);

    if (node->ops && node->ops->open)
        node->ops->open(node, flags);
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
    vfs_fd_t   *f    = &fd_table[fd];
    vfs_node_t *node = f->node;
    if (!node->ops || !node->ops->read) return -1;
    ssize_t n = node->ops->read(node, buf, len, f->offset);
    if (n > 0) f->offset += (uint64_t)n;
    return n;
}

ssize_t vfs_write(int fd, const void *buf, size_t len) {
    if (fd < 0 || fd >= VFS_MAX_OPEN_FDS || !fd_table[fd].valid) return -1;
    vfs_fd_t   *f    = &fd_table[fd];
    vfs_node_t *node = f->node;
    if (!node->ops || !node->ops->write) return -1;
    ssize_t n = node->ops->write(node, buf, len, f->offset);
    if (n > 0) f->offset += (uint64_t)n;
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
    if (whence == 0) f->offset = (uint64_t)offset;
    else if (whence == 1) f->offset = (uint64_t)((int64_t)f->offset + offset);
    else if (whence == 2) f->offset = f->node->size;
    return 0;
}

int vfs_stat(const char *path, uint64_t *size, vfs_node_type_t *type) {
    vfs_node_t *node = vfs_lookup(path);
    if (!node) return -1;
    if (size) *size = node->size;
    if (type) *type = node->type;
    return 0;
}

// ============================================================
//  RamFS — In-memory filesystem backed by kmalloc
// ============================================================

typedef struct {
    uint8_t *data;
    size_t   capacity;
} ramfs_file_t;

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
        size_t new_cap = (size_t)(end * 2);
        if (new_cap < 64) new_cap = 64;
        uint8_t *new_data = (uint8_t *)krealloc(f->data, new_cap);
        if (!new_data) return -1;
        f->data     = new_data;
        f->capacity = new_cap;
    }
    memcpy(f->data + off, buf, len);
    if (end > n->size) n->size = end;
    return (ssize_t)len;
}

static int ramfs_readdir(vfs_node_t *dir, uint32_t idx, char name[VFS_NAME_MAX]) {
    uint32_t i = 0;
    list_foreach(&dir->children, node) {
        vfs_node_t *child = container_of(node, vfs_node_t, sibling);
        if (i == idx) {
            strncpy(name, child->name, VFS_NAME_MAX - 1);
            name[VFS_NAME_MAX - 1] = '\0';
            return 0;
        }
        i++;
    }
    return -1;
}

static vfs_node_t *ramfs_lookup(vfs_node_t *dir, const char *name) {
    list_foreach(&dir->children, node) {
        vfs_node_t *child = container_of(node, vfs_node_t, sibling);
        if (strcmp(child->name, name) == 0) return child;
    }
    return NULL;
}

static int ramfs_create(vfs_node_t *parent, const char *name);
static int ramfs_mkdir(vfs_node_t *parent, const char *name);

static const vfs_ops_t ramfs_file_ops = {
    .read    = ramfs_read,
    .write   = ramfs_write,
};

static const vfs_ops_t ramfs_dir_ops = {
    .readdir = ramfs_readdir,
    .lookup  = ramfs_lookup,
    .create  = ramfs_create,
    .mkdir   = ramfs_mkdir,
};

static int ramfs_create(vfs_node_t *parent, const char *name) {
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
    return 0;
}

static int ramfs_mkdir(vfs_node_t *parent, const char *name) {
    vfs_node_t *n = node_alloc(name, VFS_DIR, &ramfs_dir_ops);
    if (!n) return -1;
    dir_add_child(parent, n);
    return 0;
}

vfs_node_t *ramfs_create_root(void) {
    vfs_node_t *root = node_alloc("/", VFS_DIR, &ramfs_dir_ops);
    if (!root) return NULL;
    ramfs_mkdir(root, "dev");
    ramfs_mkdir(root, "tmp");
    ramfs_mkdir(root, "sys");
    return root;
}

// ============================================================
//  DevFS — /dev character devices
// ============================================================

static vfs_node_t *devfs_dir = NULL;

static ssize_t null_read (vfs_node_t *n, void *buf, size_t len, uint64_t off)
    { (void)n;(void)buf;(void)len;(void)off; return 0; }
static ssize_t null_write(vfs_node_t *n, const void *buf, size_t len, uint64_t off)
    { (void)n;(void)buf;(void)off; return (ssize_t)len; }
static const vfs_ops_t null_ops = { .read=null_read, .write=null_write };

static ssize_t zero_read(vfs_node_t *n, void *buf, size_t len, uint64_t off) {
    (void)n;(void)off; memset(buf, 0, len); return (ssize_t)len; }
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
    dir_add_child(devfs_dir, n);
    return 0;
}
