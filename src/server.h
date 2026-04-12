// SPDX-License-Identifier: CC-PDM-1.0
/*
 * VitaDrop - server.h
 * HTTP Server Header for PS Vita Wi-Fi File Transfer
 * by Ibrahim Dogan
 *
 * Declares the HTTP server interface: init, run loop, shutdown,
 * and shared transfer state for the UI to display progress.
 */

#ifndef SERVER_H
#define SERVER_H

#include <stdint.h>

/* ============================================================
 * Transfer state - shared between server thread and UI thread
 * ============================================================ */
typedef enum {
    TRANSFER_IDLE,
    TRANSFER_RECEIVING,
    TRANSFER_COMPLETE,
    TRANSFER_ERROR
} TransferState;

typedef struct {
    volatile TransferState state;
    volatile uint64_t bytes_received;
    volatile uint64_t bytes_total;     /* 0 if unknown (chunked) */
    char filename[256];
    char error_msg[256];
    volatile int files_completed;      /* total files transferred this session */
    
    char history[5][256];              /* Last 5 files */
    volatile int history_count;
} TransferStatus;

/* Global transfer status (defined in server.c) */
extern TransferStatus g_transfer;

/* ============================================================
 * Server control
 * ============================================================ */

/* Initialize the HTTP server on the given port.
 * Returns 0 on success, <0 on error. */
int server_init(int port);

/* Run the server accept loop. This is meant to be called
 * from a dedicated thread — it blocks until server_stop(). */
void server_run(void);

/* Signal the server to stop, breaking the accept loop. */
void server_stop(void);

/* Clean up all server resources (sockets, etc.). */
void server_cleanup(void);

/* Get the local IP address string (after network init).
 * Returns pointer to static buffer, or NULL on failure. */
const char *server_get_ip(void);

#endif /* SERVER_H */
