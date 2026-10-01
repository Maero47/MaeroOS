#include "shm.h"
#include "process.h"
#include "../arch/i686/mm/paging.h"
#include "../mm/pmm.h"
#include "../include/kernel/config.h"
#include "../lib/string.h"
#include "../kernel/printk.h"
#include <stddef.h>

typedef struct {
    int      used;
    uint32_t npages;
    int      maps;                 /* attachments + the creator reservation */
    uint32_t creator_mm;           /* pgdir holding the reservation, 0 = none */
    uint32_t uid, gid, mode;       /* owner (creator's euid/egid), 0600 */
    uint32_t frames[SHM_MAX_PAGES];
} shm_object_t;

/* One object attached in one address space. */
typedef struct {
    uint32_t mm;                   /* page directory; 0 = slot free */
    int      id;
    uint32_t addr;
    uint32_t npages;
} shm_attach_t;

static shm_object_t objects[SHM_MAX_OBJECTS];
static shm_attach_t attaches[SHM_MAX_ATTACH];

static shm_object_t *shm_get(int id) {
    if (id < 0 || id >= SHM_MAX_OBJECTS || !objects[id].used)
        return NULL;
    return &objects[id];
}

static uint32_t cur_mm(void) {
    return current_proc ? current_proc->pgdir_phys : 0;
}

static shm_attach_t *attach_find(uint32_t mm, int id) {
    for (int i = 0; i < SHM_MAX_ATTACH; i++)
        if (attaches[i].mm == mm && attaches[i].id == id) return &attaches[i];
    return NULL;
}

static shm_attach_t *attach_alloc(void) {
    for (int i = 0; i < SHM_MAX_ATTACH; i++)
        if (!attaches[i].mm) return &attaches[i];
    return NULL;
}

/* Release the object's own frame references and free the slot.  Frames still
 * mapped somewhere live on through their PTE references. */
static void shm_destroy(shm_object_t *obj) {
    for (uint32_t i = 0; i < obj->npages; i++)
        pmm_frame_decref(obj->frames[i]);
    obj->used = 0;
    obj->npages = 0;
    obj->maps = 0;
    obj->creator_mm = 0;
}

/* Drop one attachment or reservation of obj; destroy it on the last. */
static void shm_put(shm_object_t *obj) {
    if (--obj->maps <= 0)
        shm_destroy(obj);
}

int shm_sys_create(uint32_t npages) {
    shm_object_t *obj = NULL;
    int id = -1;

    if (npages < 1 || npages > SHM_MAX_PAGES)
        return -22;  /* -EINVAL */
    if (!current_proc) return -22;
    if (current_proc->euid != 0) {
        uint32_t held = 0;
        for (int i = 0; i < SHM_MAX_OBJECTS; i++)
            if (objects[i].used && objects[i].uid == current_proc->euid)
                held += objects[i].npages;
        if (held + npages > SHM_UID_MAX_PAGES)
            return -28;  /* -ENOSPC, as shmget past SHMALL */
    }
    for (int i = 0; i < SHM_MAX_OBJECTS; i++) {
        if (!objects[i].used) {
            obj = &objects[i];
            id = i;
            break;
        }
    }
    if (!obj) return -28;  /* -ENOSPC, as shmget at SHMMNI */

    obj->npages = npages;
    for (uint32_t i = 0; i < npages; i++) {
        uint32_t phys = pmm_alloc_frame();
        if (!phys) {
            obj->npages = i;
            shm_destroy(obj);
            return -12;  /* -ENOMEM */
        }
        pmm_frame_incref(phys);   /* the object's own reference */
        /* Zero the frame: shared buffers must not leak prior contents.
         * Temp mappings require IF=0. */
        __asm__ volatile("cli");
        memset(paging_temp_map(phys), 0, PAGE_SIZE);
        paging_temp_unmap();
        __asm__ volatile("sti");
        obj->frames[i] = phys;
    }
    obj->used = 1;
    obj->maps = 1;                          /* the creator's reservation */
    obj->creator_mm = cur_mm();
    obj->uid  = current_proc->euid;
    obj->gid  = current_proc->egid;
    obj->mode = 0600;
    return id;
}

static int in_group(struct proc *p, uint32_t g) {
    if (g == p->egid) return 1;
    for (uint32_t i = 0; i < p->ngroups; i++)
        if (p->groups[i] == g) return 1;
    return 0;
}

/* Linux ipcperms() for a read-write attach: the class the caller falls in
 * must grant both read and write. */
static int shm_may_map(shm_object_t *obj) {
    struct proc *p = current_proc;
    if (p->euid == 0) return 1;
    uint32_t bits;
    if (p->euid == obj->uid)      bits = obj->mode >> 6;
    else if (in_group(p, obj->gid)) bits = obj->mode >> 3;
    else                          bits = obj->mode;
    return (bits & 6) == 6;
}

/* An attachment none of whose pages still maps the object (munmap or a
 * MAP_FIXED overlay replaced them all) is only a record: forget it. */
static void attach_drop(shm_attach_t *a, shm_object_t *obj) {
    a->mm = 0;
    a->id = -1;
    shm_put(obj);
}

int shm_sys_map(int id) {
    shm_object_t *obj = shm_get(id);
    uint32_t mm = cur_mm();

    if (!obj || !mm) return -22;
    if (!shm_may_map(obj)) return -13;      /* -EACCES */

    shm_attach_t *a = attach_find(mm, id);
    if (a) {
        if (mm_shm_mapped(a->addr, obj->frames, a->npages))
            return -17;                     /* -EEXIST: already mapped */
        /* Every page was unmapped behind the record's back: the record is
         * stale.  Dropping it can release the last hold on the object. */
        attach_drop(a, obj);
        obj = shm_get(id);
        if (!obj) return -22;
    }
    a = attach_alloc();
    if (!a) return -12;

    uint32_t base = mm_shm_attach(obj->frames, obj->npages);
    if (!base) return -12;

    a->mm = mm;
    a->id = id;
    a->addr = base;
    a->npages = obj->npages;
    /* The creator's first map turns its reservation into this attachment. */
    if (obj->creator_mm == mm) obj->creator_mm = 0;
    else obj->maps++;
    return (int)base;
}

int shm_sys_unmap(int id) {
    shm_object_t *obj = shm_get(id);
    uint32_t mm = cur_mm();

    if (!obj || !mm) return -22;
    shm_attach_t *a = attach_find(mm, id);
    if (!a) {
        /* Never mapped here, but created here: give the object up. */
        if (obj->creator_mm == mm) {
            obj->creator_mm = 0;
            shm_put(obj);
            return 0;
        }
        return -22;
    }
    mm_shm_detach(a->addr, obj->frames, a->npages);
    attach_drop(a, obj);
    return 0;
}

int shm_sys_chmod(int id, uint32_t mode) {
    shm_object_t *obj = shm_get(id);
    if (!obj || !current_proc) return -22;
    if (current_proc->euid != 0 && current_proc->euid != obj->uid)
        return -1;                          /* -EPERM */
    obj->mode = mode & 0666;
    return 0;
}

void shm_proc_fork(struct proc *parent, struct proc *child) {
    uint32_t pm = parent->pgdir_phys, cm = child->pgdir_phys;
    if (!pm || !cm || pm == cm) return;     /* CLONE_VM: records are shared */
    for (int i = 0; i < SHM_MAX_ATTACH; i++) {
        if (attaches[i].mm != pm) continue;
        shm_object_t *obj = shm_get(attaches[i].id);
        if (!obj) continue;
        shm_attach_t *c = attach_alloc();
        if (!c) {
            /* The child still maps the frames (its PTEs hold references);
             * it only cannot shm_unmap them, and they go at its exit. */
            printk("[shm] attachment table full on fork (pid %d)\n", child->pid);
            return;
        }
        *c = attaches[i];
        c->mm = cm;
        obj->maps++;
    }
}

void shm_mm_release(uint32_t mm) {
    if (!mm) return;
    for (int i = 0; i < SHM_MAX_ATTACH; i++) {
        if (attaches[i].mm != mm) continue;
        shm_object_t *obj = shm_get(attaches[i].id);
        attaches[i].mm = 0;
        attaches[i].id = -1;
        if (obj) shm_put(obj);
    }
    for (int i = 0; i < SHM_MAX_OBJECTS; i++)
        if (objects[i].used && objects[i].creator_mm == mm) {
            objects[i].creator_mm = 0;
            shm_put(&objects[i]);
        }
}

void shm_proc_exit(struct proc *p) {
    uint32_t mm = p ? p->pgdir_phys : 0;
    if (!mm) return;
    for (int i = 0; i < MAX_PROCS; i++) {
        struct proc *q = &ptable[i];
        if (q == p || q->pgdir_phys != mm) continue;
        if (q->state == PROC_UNUSED || q->state == PROC_ZOMBIE) continue;
        return;                             /* someone still runs in it */
    }
    shm_mm_release(mm);
}
