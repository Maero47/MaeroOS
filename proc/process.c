#include "process.h"
#include "scheduler.h"
#include "elf.h"
#include "../mm/heap.h"
#include "../mm/vmm.h"
#include "../mm/pmm.h"
#include "../kernel/printk.h"
#include "../arch/i686/cpu/tss.h"
#include "../arch/i686/cpu/percpu.h"
#include "../arch/i686/mm/paging.h"
#include "../arch/i686/cpu/fpu.h"
#include "../fs/vfs.h"
#include <kernel/config.h>
#include <stdint.h>
#include <stddef.h>

/* User virtual memory layout */
#define USER_CODE_BASE  0x08048000U

struct proc ptable[MAX_PROCS];
/* current_proc is now a per-CPU macro (see process.h) — no global definition. */

static int next_pid = 1;

extern void trapret(void);  /* defined in isr.asm */

void proc_init(void) {
    for (int i = 0; i < MAX_PROCS; i++)
        ptable[i].state = PROC_UNUSED;
    printk("[PROC] Process table initialized (%d slots).\n", MAX_PROCS);
}



struct proc *allocproc(void) {
    struct proc *p = NULL;
    int live = 0;
    for (int i = 0; i < MAX_PROCS; i++) {
        if (ptable[i].state == PROC_UNUSED) {
            if (!p) p = &ptable[i];
        } else {
            live++;
        }
    }
    if (!p) { printk("[proc] table FULL (%d/%d) — clone/fork fails\n",
                     live, MAX_PROCS); return NULL; }

    p->state          = PROC_EMBRYO;
    p->pid            = next_pid++;
    p->pgdir_phys     = 0;
    p->parent         = NULL;
    p->exit_status    = 0;
    p->heap_end       = 0;
    p->pending_sigs   = 0;
    p->blocked_sigs   = 0;
    p->sigframe_addr  = 0;
    p->saved_sigmask  = 0;
    p->restore_sigmask = 0;
    p->sleep_chan     = NULL;
    p->wake_tick      = 0;
    p->cwd[0]        = '/';
    p->cwd[1]        = '\0';
    p->mmap_next     = 0x40000000U;
    p->umask         = 022;
    p->uid = p->gid = p->euid = p->egid = 0;   /* root until setuid drops */
    p->pgrp          = p->pid;
    p->sid           = p->pid;
    p->ctty          = NULL;
    p->exe[0]        = '\0';
    p->utime_ticks   = 0;
    p->sched_count   = 0;
    p->no_preempt    = 0;
    p->tgid          = p->pid;
    p->tls_base      = 0;
    p->clear_child_tid = 0;
    p->set_child_tid = 0;
    p->vm_owner      = NULL;
    p->robust_list_head = 0;
    p->vfork_parent  = NULL;
    p->vfork_waiting = 0;
    p->group_exit    = 0;
    p->sleep_timed_out = 0;
    p->futex_wait    = 0;
    fpu_state_init(fpu_area(p));
    /* Fresh private handler table (fork copies the parent's into it; a
     * CLONE_SIGHAND thread drops it for the shared one — see sys_clone). */
    p->sighand = sighand_alloc();
    if (!p->sighand) { p->state = PROC_UNUSED; return NULL; }
    /* Fresh private fd table (fork/initial keep it; thread clone replaces it
     * with the shared group table — see sys_clone). */
    fdtable_attach(p, fdtable_alloc());
    if (!p->fdt) { sighand_put(p->sighand); p->sighand = NULL; p->state = PROC_UNUSED; return NULL; }
    for (int i = 0; i < SHM_PROC_MAPS; i++)
        p->shm_maps[i].id = -1;

    /* Allocate kernel stack */
    p->kstack = kmalloc(KSTACKSIZE);
    if (!p->kstack) {
        fdtable_put(p);
        sighand_put(p->sighand); p->sighand = NULL;
        p->state = PROC_UNUSED;
        return NULL;
    }

    /*
     * Build the initial kernel stack for a new process:
     *
     * [kstack + KSTACKSIZE]  ← top
     *   registers_t (trapframe)
     *   return address → trapret
     *   struct context { edi=0, esi=0, ebx=0, ebp=0, eip=forkret }
     * [sp ← p->context]
     */
    uint8_t *sp = p->kstack + KSTACKSIZE;

    /* Place trapframe */
    sp -= sizeof(registers_t);
    p->tf = (registers_t *)sp;

    /* Return address from forkret → trapret */
    sp -= sizeof(uint32_t);
    *(uint32_t *)sp = (uint32_t)trapret;

    /* Saved context */
    sp -= sizeof(struct context);
    p->context = (struct context *)sp;
    __builtin_memset(p->context, 0, sizeof(struct context));
    p->context->eip = (uint32_t)forkret;

    return p;
}

/*
 * forkret — called the first time a process is scheduled.
 * Returns to trapret (via the return address on the stack),
 * which executes iret into user mode using the trapframe.
 */
void forkret(void) {
    /* CLONE_CHILD_SETTID for a fork-style clone (Linux schedule_tail ->
     * put_user(task_pid_vnr(current), current->set_child_tid)): the tid word
     * must land in the CHILD's address space, which is only current now that
     * the child runs with its own page directory.  A COW fault here is taken
     * in kernel mode and resolved by the page-fault handler. */
    if (current_proc && current_proc->set_child_tid) {
        extern int copy_to_user(void *dst, const void *src, size_t len);
        uint32_t tid = (uint32_t)current_proc->pid;
        copy_to_user((void *)(uintptr_t)current_proc->set_child_tid, &tid, sizeof(tid));
        current_proc->set_child_tid = 0;
    }
    /* First dispatch of a USER process: the scheduler swtch'd here holding the
     * Big Kernel Lock; we are about to iret to user (via trapret), so release
     * it.  (Kernel threads bypass forkret — they keep the BKL while running.) */
    bkl_leave();
}

struct proc *proc_group_leader(struct proc *p) {
    if (!p || p->pid == p->tgid) return p;
    for (int i = 0; i < MAX_PROCS; i++)
        if (ptable[i].state != PROC_UNUSED && ptable[i].pid == p->tgid)
            return &ptable[i];
    return p;
}

int proc_group_empty(struct proc *leader) {
    for (int i = 0; i < MAX_PROCS; i++) {
        struct proc *q = &ptable[i];
        if (q == leader || q->state == PROC_UNUSED) continue;
        if (q->tgid != leader->tgid) continue;
        if (q->state == PROC_ZOMBIE && q->pid != q->tgid) continue;  /* released soon */
        return 0;
    }
    return 1;
}

void proc_release(struct proc *p) {
    if (!p || p->state != PROC_ZOMBIE) return;
    if (p->pgdir_phys && !pgdir_release(p->pgdir_phys))
        pgdir_free_user(p->pgdir_phys);
    if (p->kstack) kfree(p->kstack);
    p->kstack     = NULL;
    p->pgdir_phys = 0;
    p->parent     = NULL;
    p->state      = PROC_UNUSED;
}

/*
 * proc_create_kthread — create a kernel-mode thread that runs fn().
 */
struct proc *proc_create_kthread(void (*fn)(void), const char *name) {
    struct proc *p = NULL;
    for (int i = 0; i < MAX_PROCS; i++) {
        if (ptable[i].state == PROC_UNUSED) { p = &ptable[i]; break; }
    }
    if (!p) return NULL;

    p->state       = PROC_EMBRYO;
    p->pid         = next_pid++;
    p->pgdir_phys  = 0;   /* kthreads use the kernel pgdir */
    p->parent      = NULL;
    p->exit_status = 0;
    p->ctty        = NULL;
    p->pgrp        = p->pid;
    p->sid         = p->pid;

    for (int i = 0; i < SHM_PROC_MAPS; i++)
        p->shm_maps[i].id = -1;
    p->utime_ticks = 0;
    p->sched_count = 0;
    p->no_preempt  = 0;
    p->tgid        = p->pid;
    p->tls_base    = 0;
    p->clear_child_tid = 0;
    p->set_child_tid = 0;
    p->vm_owner    = NULL;
    p->group_exit  = 0;
    p->pending_sigs = 0;
    p->blocked_sigs = 0;
    fpu_state_init(fpu_area(p));
    /* Kernel threads never take signals, but common paths (e.g. the network
     * stack's blocking waits) consult the handler table of current_proc. */
    p->sighand = sighand_alloc();
    if (!p->sighand) { p->state = PROC_UNUSED; return NULL; }

    p->kstack = kmalloc(KSTACKSIZE);
    if (!p->kstack) { sighand_put(p->sighand); p->sighand = NULL; p->state = PROC_UNUSED; return NULL; }

    uint8_t *sp = p->kstack + KSTACKSIZE;
    sp -= sizeof(uint32_t);
    *(uint32_t *)sp = 0;   /* crash if fn() returns */

    sp -= sizeof(struct context);
    p->context = (struct context *)sp;
    __builtin_memset(p->context, 0, sizeof(struct context));
    p->context->eip = (uint32_t)(uintptr_t)fn;

    int ni = 0;
    while (name[ni] && ni < 15) { p->name[ni] = name[ni]; ni++; }
    p->name[ni] = '\0';

    p->tf         = NULL;
    p->time_slice = 5;
    p->sleep_chan = NULL;
    p->wake_tick  = 0;
    p->state      = PROC_RUNNABLE;
    return p;
}

/*
 * proc_create_userproc — create a ring-3 process with its own page directory.
 *
 * Each user process gets a fresh page directory (pgdir_create) with the
 * kernel mappings copied.  The code and stack pages are mapped exclusively
 * in that process's pgdir so different processes can have independent user
 * address spaces.
 */
struct proc *proc_create_userproc(const uint8_t *code, uint32_t code_len,
                                   const char *name) {
    struct proc *p = allocproc();
    if (!p) return NULL;

    /* Create a private page directory for this process */
    p->pgdir_phys = pgdir_create();
    if (!p->pgdir_phys) {
        kfree(p->kstack);
        p->state = PROC_UNUSED;
        return NULL;
    }

    /* Allocate code page, map it in the process pgdir, copy bytecode */
    uint32_t code_phys = pmm_alloc_frame();
    if (!code_phys) {
        pgdir_free_user(p->pgdir_phys);
        kfree(p->kstack);
        p->state = PROC_UNUSED;
        return NULL;
    }
    pgdir_map(p->pgdir_phys, USER_CODE_BASE, code_phys,
              PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER);
    pmm_frame_incref(code_phys);

    /* Copy bytecode into the frame via temp mapping */
    uint8_t *cp = (uint8_t *)paging_temp_map(code_phys);
    for (uint32_t i = 0; i < code_len; i++)
        cp[i] = code[i];
    paging_temp_unmap();

    /* Allocate and map the user stack region. */
    for (uint32_t va = USER_STACK_BASE; va < USER_STACK_TOP; va += PAGE_SIZE) {
        uint32_t stack_phys = pmm_alloc_frame();
        if (!stack_phys) {
            pmm_frame_decref(code_phys);
            pgdir_free_user(p->pgdir_phys);
            kfree(p->kstack);
            p->state = PROC_UNUSED;
            return NULL;
        }
        pgdir_map(p->pgdir_phys, va, stack_phys,
                  PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER);
        pmm_frame_incref(stack_phys);
    }

    /* Fill the trapframe for ring-3 entry */
    registers_t *tf = p->tf;
    __builtin_memset(tf, 0, sizeof(*tf));

    tf->ds  = 0x23;
    tf->es  = 0x23;
    tf->fs  = 0x23;
    tf->gs  = 0x23;

    tf->eip     = USER_CODE_BASE;
    tf->cs      = 0x1B;
    tf->eflags  = 0x202;   /* IF=1 + reserved bit 1 */
    tf->useresp = USER_STACK_TOP;
    tf->ss      = 0x23;

    int ni = 0;
    while (name[ni] && ni < 15) { p->name[ni] = name[ni]; ni++; }
    p->name[ni] = '\0';

    p->state = PROC_RUNNABLE;
    printk("[PROC] Created user process '%s' pid=%d eip=0x%08x\n",
           p->name, p->pid, (unsigned)USER_CODE_BASE);
    return p;
}

/*
 * proc_create_from_elf — create a user process by loading an ELF binary
 * directly from a VFS node.  Used by kernel_main to launch /init.
 */
struct proc *proc_create_from_elf(vfs_node_t *node, const char *name) {
    struct proc *p = allocproc();
    if (!p) return NULL;

    p->pgdir_phys = pgdir_create();
    if (!p->pgdir_phys) {
        kfree(p->kstack);
        p->state = PROC_UNUSED;
        return NULL;
    }

    uint32_t entry = 0, heap_end = 0;
    if (elf_load(node, p->pgdir_phys, &entry, &heap_end) < 0) {
        pgdir_free_user(p->pgdir_phys);
        kfree(p->kstack);
        p->state = PROC_UNUSED;
        return NULL;
    }
    p->heap_end = heap_end;

    /* Allocate and map the user stack region. */
    uint32_t stack_top_phys = 0;
    for (uint32_t va = USER_STACK_BASE; va < USER_STACK_TOP; va += PAGE_SIZE) {
        uint32_t stack_phys = pmm_alloc_frame();
        if (!stack_phys) {
            pgdir_free_user(p->pgdir_phys);
            kfree(p->kstack);
            p->state = PROC_UNUSED;
            return NULL;
        }
        pgdir_map(p->pgdir_phys, va, stack_phys,
                  PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER);
        pmm_frame_incref(stack_phys);
        if (va == USER_STACK_TOP - PAGE_SIZE)
            stack_top_phys = stack_phys;
    }

    /* Minimal Linux-layout entry frame: argc=0, argv NULL, envp NULL,
     * AT_NULL auxv — 5 words at the stack top. */
    uint32_t *frame = (uint32_t *)paging_temp_map(stack_top_phys);
    frame[(PAGE_SIZE - 32) / 4 + 0] = 0;  /* argc = 0 */
    frame[(PAGE_SIZE - 32) / 4 + 1] = 0;  /* argv NULL */
    frame[(PAGE_SIZE - 32) / 4 + 2] = 0;  /* envp NULL */
    frame[(PAGE_SIZE - 32) / 4 + 3] = 0;  /* AT_NULL type */
    frame[(PAGE_SIZE - 32) / 4 + 4] = 0;  /* AT_NULL value */
    paging_temp_unmap();

    /* Fill trapframe */
    registers_t *tf = p->tf;
    __builtin_memset(tf, 0, sizeof(*tf));
    tf->ds = tf->es = tf->fs = tf->gs = 0x23;
    tf->eip     = entry;
    tf->cs      = 0x1B;
    tf->eflags  = 0x202;
    tf->useresp = USER_STACK_TOP - 32;
    tf->ss      = 0x23;

    int ni = 0;
    while (name[ni] && ni < 15) { p->name[ni] = name[ni]; ni++; }
    p->name[ni] = '\0';

    /* Set up stdin/stdout/stderr as /dev/tty if available */
    {
        vfs_node_t *tty = vfs_open("/dev/tty");
        if (tty) {
            p->ofile[0].type   = FD_FILE;
            p->ofile[0].node   = tty;
            p->ofile[0].offset = 0;
            p->ofile[0].flags  = O_RDONLY;

            p->ofile[1].type   = FD_FILE;
            p->ofile[1].node   = tty;
            p->ofile[1].offset = 0;
            p->ofile[1].flags  = O_WRONLY;

            p->ofile[2].type   = FD_FILE;
            p->ofile[2].node   = tty;
            p->ofile[2].offset = 0;
            p->ofile[2].flags  = O_WRONLY;
        }
    }

    p->state = PROC_RUNNABLE;
    printk("[PROC] Created ELF process '%s' pid=%d entry=0x%08x\n",
           p->name, p->pid, (unsigned)entry);
    return p;
}
