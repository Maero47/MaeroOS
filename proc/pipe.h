#pragma once
#include <stdint.h>
#include <kernel/klock.h>

#define PIPE_BUF_SIZE 4096

typedef struct pipe_buf {
    char     data[PIPE_BUF_SIZE];
    uint32_t head;      /* index of next byte to read */
    uint32_t count;     /* bytes currently in buffer */
    int      nreaders;  /* open read-end FD count (across all procs) */
    int      nwriters;  /* open write-end FD count */
    int      fifo;      /* owned by a named FIFO's node: see pipe_fifo_alloc */
    int      pins;      /* pipe_read/pipe_write calls in progress: the buffer
                         * is not freed under a sleeper whose descriptor a
                         * sibling thread (CLONE_FILES) closed meanwhile */
    /* Guards head/count/nreaders/nwriters/pins and is the lock of the
     * pipe's wait queue (the pipe itself is the channel): readers and
     * writers test for data or room under it and sleep with sleep_locked,
     * and every change that can unblock someone wakes under it
     * (docs/smp-plan.md stage 2a).  The user copies run outside it (they can
     * fault and sleep); until stage 8 the BKL still keeps two readers or two
     * writers of one pipe apart across their copies. */
    kspinlock_t lock;
} pipe_buf_t;

/* Allocate and initialise a new pipe. Returns NULL on OOM. */
pipe_buf_t *pipe_alloc(void);

/*
 * Allocate the buffer of a named FIFO.  It belongs to the FIFO's vfs node, not
 * to the descriptors: the open path stores it in node->private once and every
 * later open reuses it, so the last close must not free it (the node would be
 * left pointing at freed memory).  The filesystem that owns the node frees it
 * with pipe_fifo_free() when the node itself goes away.  Starts with no
 * readers or writers.
 */
pipe_buf_t *pipe_fifo_alloc(void);
void pipe_fifo_free(pipe_buf_t *p);

/*
 * Read up to len bytes from pipe into buf (POSIX semantics: returns as soon
 * as any data is available, possibly fewer than len bytes).
 * Blocks while empty and nwriters > 0, unless nonblock (then -EAGAIN).
 * A pending unblocked signal interrupts the wait with -EINTR.
 * Returns 0 at EOF (nwriters == 0 and buffer empty).
 */
int pipe_read(pipe_buf_t *p, char *buf, int len, int nonblock);

/*
 * Write len bytes from buf into pipe.
 * Returns -EPIPE and queues SIGPIPE if nreaders == 0.
 * Blocks while full, unless nonblock (then -EAGAIN if nothing written).
 * A pending unblocked signal interrupts the wait with -EINTR (or returns the
 * partial count if some bytes were already written).
 */
int pipe_write(pipe_buf_t *p, const char *buf, int len, int nonblock);

/* A new descriptor for an end (dup, fork, FIFO open). */
void pipe_add_reader(pipe_buf_t *p);
void pipe_add_writer(pipe_buf_t *p);

/* Called when a read-end FD is closed; frees pipe if both ends gone (a FIFO's
 * buffer is only emptied, see pipe_fifo_alloc). */
void pipe_close_read(pipe_buf_t *p);

/* Called when a write-end FD is closed; frees pipe if both ends gone (a FIFO's
 * buffer is only emptied). */
void pipe_close_write(pipe_buf_t *p);
