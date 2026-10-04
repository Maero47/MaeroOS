#pragma once
#include <stdint.h>

struct proc;

/*
 * Minimal shared-memory objects (SerenityOS/Wayland-style window buffers).
 *
 * An object is a fixed list of physical frames.  Frame lifetime is governed
 * by pmm refcounts: the object itself holds one reference per frame from
 * creation until destruction, and every PTE that maps a frame holds one more
 * (so munmap, a MAP_FIXED overlay, fork and address-space teardown all keep
 * the counts right on their own).  Detaching releases only the pages whose PTE
 * still maps the object's frame, never a reference some other unmap already
 * dropped.
 *
 * Attachments are recorded per ADDRESS SPACE (page directory), not per
 * thread: the threads of a process share one set of attachments and one mmap
 * layout, exactly as Linux keeps shm attachments as VMAs of the mm.  An
 * attachment is also a VMA (shared, flagged shm) so /proc/pid/maps shows it
 * and the free-range search never places anything on top of it.
 *
 * The creating address space holds a reservation that its first map turns
 * into its attachment; an object is destroyed once no attachment and no
 * reservation is left — also when the creator dies before ever mapping it.
 *
 * Access control (Linux ipcperms): the object belongs to the creator's
 * euid/egid with mode 0600.  Mapping (read-write) needs rw permission for the
 * caller's class; root bypasses.  The owner may widen the mode.
 *
 * Syscalls (custom numbers 500-502 and 506, outside the Linux i386 table):
 *   shm_create(npages)   → id  (-ENOSPC past SHM_UID_MAX_PAGES, non-root)
 *   shm_map(id)          → vaddr (mapped PAGE_SHARED|WRITABLE|USER)
 *   shm_unmap(id)        → 0   (also drops an unused creator reservation)
 *   shm_chmod(id, mode)  → 0   (506; owner or root; mode & 0666)
 */

#define SHM_MAX_OBJECTS 32
#define SHM_MAX_PAGES   2048  /* 8 MiB — one 1920x1080x32 surface (a maximized window) */
#define SHM_MAX_ATTACH  256   /* attachments system-wide */
/* Pages one non-root user may hold in shm objects at once: a desktop
 * session's window surfaces (four maximized 1920x1080 windows, or a dozen
 * ordinary ones), so no user can pin SHM_MAX_OBJECTS x 8 MiB.  Plus room
 * for one transient surface: a resizing window creates its new surface
 * while the old one is still held (until the desktop unmaps it), so a
 * session whose end state fits must not be refused in between. */
#define SHM_UID_SESSION_PAGES 8192   /* 32 MiB */
#define SHM_UID_MAX_PAGES (SHM_UID_SESSION_PAGES + SHM_MAX_PAGES)   /* 40 MiB */

int shm_sys_create(uint32_t npages);
int shm_sys_map(int id);
int shm_sys_unmap(int id);
int shm_sys_chmod(int id, uint32_t mode);

/* fork: the child's new address space inherited every PTE (shared ones are
 * increfed by the clone loop); copy the parent's attachment records to it.  A
 * CLONE_VM child shares the parent's records and needs nothing. */
void shm_proc_fork(struct proc *parent, struct proc *child);

/* exit: p is going away.  If nothing else still runs in its address space,
 * drop the address space's attachments and reservations now (the PTEs, and
 * with them the frames, go when the page directory is freed at reap). */
void shm_proc_exit(struct proc *p);

/* The page directory pgdir_phys is being freed: forget every attachment and
 * reservation recorded for it.  Called by pgdir_free_user. */
void shm_mm_release(uint32_t pgdir_phys);

/* Address-space side of an attachment (proc/syscall.c, next to the VMA code;
 * both work on the CURRENT address space).  mm_shm_attach maps the frames at a
 * free range and records a shm VMA; it returns the base or 0 (-ENOMEM).
 * mm_shm_detach unmaps the pages of [base, base + npages pages) that still map
 * frames[i], releases their references after the TLB shootdown, and drops the
 * VMA coverage of exactly those pages.  mm_shm_mapped counts such pages. */
uint32_t mm_shm_attach(const uint32_t *frames, uint32_t npages);
/* The same at a fixed page-aligned `addr` (0: anywhere), read-only when
 * `rdonly`: for SysV shmat (proc/sysvipc.c).  0 when the range is taken. */
uint32_t mm_shm_attach_at(const uint32_t *frames, uint32_t npages, uint32_t addr,
                          int rdonly);
/* Largest object either kind of attachment can detach (SysV segments are
 * bigger than window surfaces). */
#define MM_SHM_MAX_PAGES 8192
void     mm_shm_detach(uint32_t base, const uint32_t *frames, uint32_t npages);
uint32_t mm_shm_mapped(uint32_t base, const uint32_t *frames, uint32_t npages);
