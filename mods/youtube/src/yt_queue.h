#ifndef YT_QUEUE_H
#define YT_QUEUE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Play-order count of the runtime now-playing queue, or -1 on failure. */
int yt_queue_count(void);

/* Append a kind-tagged id to the id-storage vector only; returns its storage index, or -1. */
int yt_queue_push_id(unsigned int id);

/* Overwrite the play-order vector with `n` storage indices and set the current position. */
int yt_queue_set_order(const unsigned int* order, int n, int cur);

#ifdef __cplusplus
}
#endif

#endif /* YT_QUEUE_H */
