/* inotify(7) — see inotify.h. */
#include "inotify.h"
#include "vfs.h"
#include "../proc/process.h"
#include "../proc/scheduler.h"
#include "../proc/signal.h"
#include "../proc/syscall.h"
#include "../mm/heap.h"
#include "../lib/string.h"
#include "../lib/printf.h"
#include <stddef.h>

typedef struct ino_ev {
    struct ino_ev *next;
    int32_t  wd;
    uint32_t mask, cookie, len;    /* len: name bytes incl. NUL padding */
    char     name[];
} ino_ev_t;

struct inotify;

typedef struct ino_watch {
    struct ino_watch *next;        /* every watch of every instance */
    struct inotify   *in;
    int               wd;
    vfs_node_t       *node;        /* referenced while watched */
    uint32_t          mask;
} ino_watch_t;

struct inotify;
static void ino_put(struct inotify *in);

typedef struct inotify {
    vfs_node_t      v;             /* first: the node is the instance */
    int             refs;
    ino_ev_t       *head, *tail;
    uint32_t        nq;            /* events queued */
    uint32_t        bytes;         /* their size as read() returns them */
    int             next_wd;
    uint32_t        uid;
} inotify_t;

uint32_t inotify_nwatches;
static ino_watch_t *watches;
static uint32_t cookie_next = 1;
static uint32_t ninstances;
static struct inotify *instances[INOTIFY_MAX_INSTANCES * 8];

#define EV_HDR 16u

uint32_t inotify_next_cookie(void) {
    uint32_t c = cookie_next++;
    if (!cookie_next) cookie_next = 1;
    return c;
}

/* Linux pads the name to a multiple of the event header size. */
static uint32_t name_len(const char *name) {
    if (!name || !name[0]) return 0;
    uint32_t l = (uint32_t)strlen(name) + 1;
    return (l + EV_HDR - 1) & ~(EV_HDR - 1);
}

static void queue_event(inotify_t *in, int wd, uint32_t mask, uint32_t cookie,
                        const char *name) {
    uint32_t nl = name_len(name);
    /* Merge with an identical event at the tail (Linux fsnotify does). */
    ino_ev_t *t = in->tail;
    if (t && t->wd == wd && t->mask == mask && t->cookie == cookie && t->len == nl &&
        (!nl || !strcmp(t->name, name)) && !(mask & IN_Q_OVERFLOW))
        return;
    if (in->nq >= INOTIFY_MAX_QUEUED) {
        if (t && (t->mask & IN_Q_OVERFLOW)) return;
        wd = -1; mask = IN_Q_OVERFLOW; cookie = 0; nl = 0; name = NULL;
    }
    ino_ev_t *e = (ino_ev_t *)kmalloc(sizeof(*e) + nl);
    if (!e) return;
    e->next = NULL;
    e->wd = wd;
    e->mask = mask;
    e->cookie = cookie;
    e->len = nl;
    if (nl) {
        memset(e->name, 0, nl);
        strcpy(e->name, name);
    }
    if (in->tail) in->tail->next = e; else in->head = e;
    in->tail = e;
    in->nq++;
    in->bytes += EV_HDR + nl;
    wake_up(in);
    io_wake();                               /* poll/select/epoll sleepers */
}

static void watch_free(ino_watch_t *w) {
    ino_watch_t **pp = &watches;
    while (*pp && *pp != w) pp = &(*pp)->next;
    if (*pp) *pp = w->next;
    vfs_close(w->node);
    kfree(w);
    if (inotify_nwatches) inotify_nwatches--;
    /* Unrecorded from here on, so the record must not go stale. */
    if (!inotify_nwatches) vfs_forget_last_lookup();
}

/* Deliver `mask` (with IN_ISDIR etc. already in it) to the watches on node. */
static void deliver(vfs_node_t *node, uint32_t mask, uint32_t cookie, const char *name) {
    uint32_t ev = mask & (IN_ALL_EVENTS | IN_UNMOUNT);
    ino_watch_t *w = watches;
    while (w) {
        ino_watch_t *next = w->next;
        if (w->node == node && (w->mask & ev)) {
            queue_event(w->in, w->wd, mask, cookie, name);
            if (w->mask & IN_ONESHOT) {
                queue_event(w->in, w->wd, IN_IGNORED, 0, NULL);
                watch_free(w);
            }
        }
        w = next;
    }
}

void inotify_dir_event(vfs_node_t *dir, uint32_t mask, uint32_t cookie, const char *name) {
    if (!inotify_nwatches || !dir) return;
    deliver(vfs_resolve_mount(dir), mask, cookie, name);
}

void inotify_parent_event(vfs_node_t *dir, uint32_t mask, const char *name) {
    if (!inotify_nwatches || !dir) return;
    deliver(dir, mask, 0, name);
}

void inotify_self_event(vfs_node_t *node, uint32_t mask) {
    if (!inotify_nwatches || !node) return;
    deliver(node, mask, 0, NULL);
}

void inotify_child_event(vfs_node_t *node, const char *path, uint32_t mask) {
    if (!inotify_nwatches || !node) return;
    if (node->flags == VFS_FLAG_DIR) mask |= IN_ISDIR;
    deliver(node, mask, 0, NULL);
    if (path && path[0] == '/') {
        int err;
        vfs_lookup(path, 1, &err);           /* records the parent */
    }
    vfs_node_t *parent;
    char name[256];
    if (vfs_last_parent(node, &parent, name)) deliver(parent, mask, 0, name);
}

void inotify_node_gone(vfs_node_t *node) {
    if (!inotify_nwatches || !node) return;
    ino_watch_t *w = watches;
    while (w) {
        ino_watch_t *next = w->next;
        if (w->node == node) {
            if (w->mask & IN_DELETE_SELF)
                queue_event(w->in, w->wd, IN_DELETE_SELF, 0, NULL);
            queue_event(w->in, w->wd, IN_IGNORED, 0, NULL);
            watch_free(w);
        }
        w = next;
    }
}

/* ── the instance node ── */

static int ino_ready(vfs_node_t *n) { return ((inotify_t *)n)->nq != 0; }

static uint32_t ino_read(vfs_node_t *n, uint32_t off, uint32_t len, uint8_t *buf) {
    inotify_t *in = (inotify_t *)n;
    (void)off;
    in->refs++;                              /* a close meanwhile must not free it */
    while (!in->head) {
        if (signal_interrupt_pending(current_proc)) { ino_put(in); return (uint32_t)-4; }
        sleep_on(in);
    }
    uint32_t done = 0;
    while (in->head) {
        ino_ev_t *e = in->head;
        uint32_t sz = EV_HDR + e->len;
        if (done + sz > len) break;
        uint32_t hdr[4] = { (uint32_t)e->wd, e->mask, e->cookie, e->len };
        memcpy(buf + done, hdr, EV_HDR);
        if (e->len) memcpy(buf + done + EV_HDR, e->name, e->len);
        done += sz;
        in->head = e->next;
        if (!in->head) in->tail = NULL;
        in->nq--;
        in->bytes -= sz;
        kfree(e);
    }
    ino_put(in);
    return done ? done : (uint32_t)-22;      /* the first event does not fit */
}

static int ino_ioctl(vfs_node_t *n, uint32_t req, void *arg) {
    if (req == 0x541B) {                     /* FIONREAD: bytes read() would give */
        *(int *)arg = (int)((inotify_t *)n)->bytes;
        return 0;
    }
    return -25;                              /* -ENOTTY */
}

static void ino_retain(vfs_node_t *n) { ((inotify_t *)n)->refs++; }

/* Drop a reference; the last one frees the instance and its watches. */
static void ino_put(struct inotify *in) {
    if (--in->refs > 0) return;
    ino_watch_t *w = watches;
    while (w) {
        ino_watch_t *next = w->next;
        if (w->in == in) watch_free(w);
        w = next;
    }
    while (in->head) {
        ino_ev_t *e = in->head;
        in->head = e->next;
        kfree(e);
    }
    if (ninstances) ninstances--;
    for (int i = 0; i < INOTIFY_MAX_INSTANCES * 8; i++)
        if (instances[i] == in) instances[i] = NULL;
    kfree(in);
}

static void ino_close(vfs_node_t *n) { ino_put((inotify_t *)n); }

vfs_node_t *inotify_new(void) {
    /* fs.inotify.max_user_instances: per user, so no one user can take
     * every instance (root is not limited). */
    uint32_t uid = current_proc ? current_proc->euid : 0, mine = 0;
    int slot = -1;
    for (int i = 0; i < INOTIFY_MAX_INSTANCES * 8; i++) {
        if (!instances[i]) { if (slot < 0) slot = i; continue; }
        if (instances[i]->uid == uid) mine++;
    }
    if (slot < 0 || (uid != 0 && mine >= INOTIFY_MAX_INSTANCES)) return NULL;
    inotify_t *in = (inotify_t *)kmalloc(sizeof(*in));
    if (!in) return NULL;
    memset(in, 0, sizeof(*in));
    strcpy(in->v.name, "inotify");
    /* Not a regular file (a read may block, and must not be split into
     * chunks); stat shows a 0600 node owned by the creator. */
    in->v.flags = VFS_FLAG_CHARDEV;
    in->v.mask = 0600;
    in->v.uid = current_proc ? current_proc->euid : 0;
    in->v.gid = current_proc ? current_proc->egid : 0;
    in->v.inode = 0x7E000000U | (uint32_t)((uintptr_t)in >> 4 & 0xFFFFFF);
    in->v.read_fn = ino_read;
    in->v.read_ready_fn = ino_ready;
    in->v.ioctl_fn = ino_ioctl;
    in->v.retain_fn = ino_retain;
    in->v.close_fn = ino_close;
    in->v.private = in;
    in->next_wd = 1;
    in->uid = in->v.uid;
    instances[slot] = in;
    ninstances++;
    return &in->v;
}

int inotify_is(vfs_node_t *n) {
    return n && n->read_fn == ino_read;
}

int inotify_add(vfs_node_t *inst, vfs_node_t *target, uint32_t mask) {
    inotify_t *in = (inotify_t *)inst;
    if (!(mask & (IN_ALL_EVENTS | IN_UNMOUNT))) return -22;
    if ((mask & IN_MASK_ADD) && (mask & IN_MASK_CREATE)) return -22;
    target = vfs_resolve_mount(target);
    for (ino_watch_t *w = watches; w; w = w->next) {
        if (w->in != in || w->node != target) continue;
        if (mask & IN_MASK_CREATE) return -17;               /* -EEXIST */
        uint32_t m = mask & ~(IN_MASK_ADD | IN_MASK_CREATE | IN_DONT_FOLLOW | IN_ONLYDIR);
        w->mask = (mask & IN_MASK_ADD) ? (w->mask | m) : m;
        return w->wd;
    }
    uint32_t mine = 0;
    for (ino_watch_t *w = watches; w; w = w->next)
        if (w->in->uid == in->uid) mine++;
    if (mine >= INOTIFY_MAX_WATCHES) return -28;             /* -ENOSPC */
    ino_watch_t *w = (ino_watch_t *)kmalloc(sizeof(*w));
    if (!w) return -12;
    w->in = in;
    w->wd = in->next_wd++;
    w->node = target;
    w->mask = mask & ~(IN_MASK_ADD | IN_MASK_CREATE | IN_DONT_FOLLOW | IN_ONLYDIR);
    vfs_retain(target);
    w->next = watches;
    watches = w;
    inotify_nwatches++;
    return w->wd;
}

int inotify_rm(vfs_node_t *inst, int wd) {
    inotify_t *in = (inotify_t *)inst;
    for (ino_watch_t *w = watches; w; w = w->next) {
        if (w->in != in || w->wd != wd) continue;
        queue_event(in, wd, IN_IGNORED, 0, NULL);
        watch_free(w);
        return 0;
    }
    return -22;
}

void inotify_show_fdinfo(vfs_node_t *n, char *buf, uint32_t *pos, uint32_t cap) {
    if (!inotify_is(n)) return;
    inotify_t *in = (inotify_t *)n;
    for (ino_watch_t *w = watches; w; w = w->next) {
        if (w->in != in || *pos + 1 >= cap) continue;
        int k = snprintf(buf + *pos, cap - *pos,
                         "inotify wd:%d ino:%x sdev:0 mask:%x ignored_mask:0\n",
                         w->wd, w->node->inode, w->mask);
        if (k > 0) *pos += (uint32_t)k;
        if (*pos >= cap) *pos = cap - 1;
    }
}
