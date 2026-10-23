#pragma once

#include <stddef.h>

typedef int (*NetWriteFn)(void *ctx, const void *data, size_t len);
typedef int (*NetProgressFn)(void *ctx, unsigned long long done, unsigned long long total);

int  net_init(void);
void net_shutdown(void);

/* Streams url into write_fn. Returns 1 on success and fills total_out with the
 * announced size (0 when the server does not send one). */
int net_download(const char *url,
                 NetWriteFn write_fn, void *write_ctx,
                 unsigned long long *total_out,
                 NetProgressFn progress_fn, void *progress_ctx,
                 char *err, size_t err_len);

int net_tcp_probe(const char *host, int port);
int net_send_file(const char *host, int port, const char *path);
