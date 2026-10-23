#pragma once

#include <stddef.h>

typedef struct FtpSink FtpSink;

/* GoldHEN runs an FTP server inside the console, so even when the app's own
 * sandbox forbids writing to /data or /user, files can still be put there. */
#define FTP_PORT 2121

/* Sets the host used by the cached probe, mkdir and uploads. */
int ftp_set_host(const char *host);

/* One-shot probe of an arbitrary host, no caching. */
int ftp_probe_host(const char *host);

/* Cached probe of the configured host. */
int ftp_probe(void);

int      ftp_mkdir(const char *path);

FtpSink *ftp_begin(const char *path);
int      ftp_write(FtpSink *sink, const void *data, size_t len);
int      ftp_end(FtpSink *sink, int success);
