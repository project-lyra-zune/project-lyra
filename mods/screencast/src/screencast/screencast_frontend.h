/* The two client frontends over the shared engine. Each owns its own listen
 * socket and serves until stop_event; neither touches WSAStartup (the process
 * entry owns the Winsock instance). on_client(1/0) brackets a connected client
 * so the daemon can drive the Live status. */
#ifndef SCREENCAST_FRONTEND_H
#define SCREENCAST_FRONTEND_H

#include <winsock2.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*sc_client_cb)(int active);

/* Browser frontend: GET / (viewer), GET /stream (MJPEG at ~1000/frame_ms fps,
 * JPEG quality jpeg_q 0..100), POST /tap (inject). Thread-per-connection. */
int sc_http_run(int port, unsigned int frame_ms, unsigned int jpeg_q,
                sc_client_cb on_client, HANDLE stop_event);

/* Desktop frontend: the binary delta protocol zune-screencast.py speaks
 * (RGB565 delta frames out, [action][x][y] touches in, full-duplex). One client
 * at a time. frame_ms caps the frame rate. */
int sc_delta_run(int port, unsigned int frame_ms,
                 sc_client_cb on_client, HANDLE stop_event);

/* Run both frontends (HTTP on SC_HTTP_PORT, delta on SC_DELTA_PORT) on their own
 * threads until stop_event is signalled, then join. cb is forwarded to both
 * (NULL to drive no status). Blocks for the serving lifetime. */
void sc_serve(HANDLE stop_event, sc_client_cb cb);

#ifdef __cplusplus
}
#endif

#endif /* SCREENCAST_FRONTEND_H */
