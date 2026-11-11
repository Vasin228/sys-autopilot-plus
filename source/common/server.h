#pragma once

#include "config.h"
#include <switch.h>

// Optional callback invoked while idle (roughly every 100ms) and between
// requests. Return false to shut the server down (used by the dev .nro app);
// pass NULL to run forever (sysmodule).
typedef bool (*ServerIdleCb)(void);

// Runs the HTTP server (blocking). Binds to cfg->port, enforcing basic auth
// when configured. Retries bind/listen failures internally.
void server_run(const Config *cfg, ServerIdleCb idle);

// Starts asynchronous MJPEG streaming on the given socket descriptor.
// Closes any previous stream connection cleanly.
void server_start_stream(int fd, int fps, ViLayerStack stack);

// Stops active streaming and closes the stream socket.
void server_stop_stream(void);

