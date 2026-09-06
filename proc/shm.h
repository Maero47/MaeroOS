#pragma once
#include <stdint.h>

struct proc;

/*
 * Minimal shared-memory objects (SerenityOS/Wayland-style window buffers).
 *
 * An object is a fixed list of physical frames.  Frame lifetime is governed
 * by pmm refcounts: the object itself holds one reference per frame from
 * creation until destruction, and every mapping holds one more.  The object
 * is destroyed when its mapping count returns to zero (after at least one
 * map), releasing the object references; the frames are then freed once the
 * last mapper's page directory is torn down.
 *
 * Syscalls (custom numbers 500-502, outside the Linux i386 table):
 *   shm_create(npages) → id
 *   shm_map(id)        → vaddr (mapped PAGE_SHARED|WRITABLE|USER)
 *   shm_unmap(id)      → 0
 */

#define SHM_MAX_OBJECTS 32
#define SHM_MAX_PAGES   768   /* 3 MiB — one 1024x768x32 surface */

int shm_sys_create(uint32_t npages);
int shm_sys_map(int id);
int shm_sys_unmap(int id);

/* fork: child inherited all PTEs (incl. shared, increfed by the clone loop);
 * copy the parent's mapping records and bump object mapping counts. */
void shm_proc_fork(struct proc *parent, struct proc *child);

/* exit/exec: drop all of p's mapping records (PTE teardown is handled by
 * pgdir_free_user; this only adjusts object bookkeeping). */
void shm_proc_cleanup(struct proc *p);
