#ifndef PLAYNEXT_QUEUE_H
#define PLAYNEXT_QUEUE_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Read/permute servicesd's music-kind now-playing queue (two std::vector<u32>:
   id storage + play-order permutation). See playnext_queue.c and
   notes/re-2026-07-14-queue-insertitem-stub/. */

/* Current now-playing queue length (play-order count), or -1 on failure. */
int queue_count(void);

/* Move the last `n` (just-appended) queue entries to after play-order position
   `cur`. Pure permutation of the play order; the id storage is untouched.
   Returns 0 on success, negative on failure. */
int queue_move_tail_next(int n, int cur);

#ifdef __cplusplus
}
#endif

#endif /* PLAYNEXT_QUEUE_H */
