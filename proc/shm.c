#include "shm.h"
#include "process.h"
#include "../arch/i686/mm/paging.h"
#include "../mm/pmm.h"
#include "../include/kernel/config.h"
#include "../lib/string.h"
#include "../kernel/panic.h"
#include <stddef.h>

typedef struct {
    int      used;
    int      ever_mapped;          /* destroy only after first map+unmap */
    uint32_t npages;
    int      maps;                 /* current mapping count */
    uint32_t frames[SHM_MAX_PAGES];
} shm_object_t;

static shm_object_t objects[SHM_MAX_OBJECTS];

static shm_object_t *shm_get(int id) {
    if (id < 0 || id >= SHM_MAX_OBJECTS || !objects[id].used)
        return NULL;
    return &objects[id];
}

/* Release the object's own frame references and free the slot. */
static void shm_destroy(shm_object_t *obj) {
    for (uint32_t i = 0; i < obj->npages; i++)
        pmm_frame_decref(obj->frames[i]);
    obj->used = 0;
    obj->npages = 0;
    obj->maps = 0;
    obj->ever_mapped = 0;
}

int shm_sys_create(uint32_t npages) {
    shm_object_t *obj = NULL;
    int id = -1;

    if (npages < 1 || npages > SHM_MAX_PAGES)
        return -22;  /* -EINVAL */
    for (int i = 0; i < SHM_MAX_OBJECTS; i++) {
        if (!objects[i].used) {
            obj = &objects[i];
            id = i;
            break;
        }
    }
    if (!obj) return -12;  /* -ENOMEM */

    obj->npages = npages;
    obj->maps = 0;
    obj->ever_mapped = 0;
    for (uint32_t i = 0; i < npages; i++) {
        uint32_t phys = pmm_alloc_frame();
        if (!phys) {
            obj->npages = i;
            shm_destroy(obj);
            return -12;
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
    return id;
}

int shm_sys_map(int id) {
    shm_object_t *obj = shm_get(id);
    struct proc *p = current_proc;
    shm_map_t *rec = NULL;

    if (!obj || !p) return -22;
    for (int i = 0; i < SHM_PROC_MAPS; i++) {
        if (p->shm_maps[i].id == id) return -17;  /* -EEXIST: already mapped */
        if (!rec && p->shm_maps[i].id < 0) rec = &p->shm_maps[i];
    }
    if (!rec) return -12;

    uint32_t base = (p->mmap_next + PAGE_SIZE - 1) & ~(uint32_t)(PAGE_SIZE - 1);
    if (base + obj->npages * PAGE_SIZE >= USER_STACK_BASE)
        return -12;

    /* Reserve the page tables before taking any reference, so the whole
     * attach either happens or leaves the process exactly as it was.  Linux
     * shmat() returns ENOMEM here too. */
    if (paging_reserve_range(base, base + obj->npages * PAGE_SIZE, 1) != 0)
        return -12;

    for (uint32_t i = 0; i < obj->npages; i++) {
        pmm_frame_incref(obj->frames[i]);   /* this mapping's reference */
        /* Cannot fail: the table was reserved above. */
        if (paging_map(base + i * PAGE_SIZE, obj->frames[i],
                       PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER |
                       PAGE_SHARED) != 0)
            panic("shmat: reserved page table vanished", NULL);
    }
    p->mmap_next = base + obj->npages * PAGE_SIZE;

    rec->id = id;
    rec->addr = base;
    rec->npages = obj->npages;
    obj->maps++;
    obj->ever_mapped = 1;
    return (int)base;
}

int shm_sys_unmap(int id) {
    shm_object_t *obj = shm_get(id);
    struct proc *p = current_proc;
    shm_map_t *rec = NULL;

    if (!obj || !p) return -22;
    for (int i = 0; i < SHM_PROC_MAPS; i++) {
        if (p->shm_maps[i].id == id) {
            rec = &p->shm_maps[i];
            break;
        }
    }
    if (!rec) return -22;

    for (uint32_t i = 0; i < rec->npages; i++) {
        paging_unmap(rec->addr + i * PAGE_SIZE);
        pmm_frame_decref(obj->frames[i]);
    }
    rec->id = -1;
    obj->maps--;
    if (obj->maps <= 0 && obj->ever_mapped)
        shm_destroy(obj);
    return 0;
}

void shm_proc_fork(struct proc *parent, struct proc *child) {
    for (int i = 0; i < SHM_PROC_MAPS; i++) {
        child->shm_maps[i] = parent->shm_maps[i];
        if (parent->shm_maps[i].id >= 0) {
            shm_object_t *obj = shm_get(parent->shm_maps[i].id);
            if (obj) obj->maps++;
        }
    }
}

void shm_proc_cleanup(struct proc *p) {
    for (int i = 0; i < SHM_PROC_MAPS; i++) {
        if (p->shm_maps[i].id < 0) continue;
        shm_object_t *obj = shm_get(p->shm_maps[i].id);
        p->shm_maps[i].id = -1;
        if (!obj) continue;
        obj->maps--;
        if (obj->maps <= 0 && obj->ever_mapped)
            shm_destroy(obj);
    }
}
