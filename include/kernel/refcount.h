#pragma once
/*
 * Atomic reference counts (docs/smp-plan.md stage 2d): shared objects (fd
 * tables, signal handler tables, eventfds, epoll instances) are retained and
 * released from several threads, and from stage 3 on from several CPUs at
 * once.  A plain ++/-- would lose an update there; these cannot.  The last
 * put, the one that sees 0, owns the object and frees it.
 */
static inline void ref_get(int *r) { __atomic_add_fetch(r, 1, __ATOMIC_ACQ_REL); }

/* 1 when this was the last reference. */
static inline int ref_put(int *r) { return __atomic_sub_fetch(r, 1, __ATOMIC_ACQ_REL) <= 0; }

static inline int ref_read(const int *r) { return __atomic_load_n(r, __ATOMIC_ACQUIRE); }
