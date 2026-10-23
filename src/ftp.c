#include "ftp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

struct FtpSink {
    int ctrl;
    int data;
};

static int probe_state = -1;
static char ftp_host[64] = "127.0.0.1";

int ftp_set_host(const char *host)
{
    if (!host || !*host)
        return 0;

    snprintf(ftp_host, sizeof(ftp_host), "%s", host);
    probe_state = -1;
    return 1;
}

static int open_socket(const char *host, int port)
{
    struct sockaddr_in addr;
    struct timeval tv;
    int fd;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    tv.tv_sec = 10;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    addr.sin_addr.s_addr = inet_addr(host);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }

    return fd;
}

/* Reads one complete FTP reply (skipping "123-" continuation lines) and
 * optionally hands back the final line for PASV parsing. */
static int read_reply(int fd, int *code, char *line_out, size_t line_len)
{
    char line[512];

    if (line_out && line_len)
        line_out[0] = '\0';

    for (;;) {
        int got = 0;
        int done = 0;

        while (got < (int)sizeof(line) - 1) {
            char ch = 0;
            ssize_t n = recv(fd, &ch, 1, 0);

            if (n <= 0)
                return 0;

            if (ch == '\n') {
                done = 1;
                break;
            }

            if (ch != '\r')
                line[got++] = ch;
        }

        if (!done)
            return 0;

        line[got] = '\0';

        if (got >= 4 && line[3] == '-')
            continue;

        if (got >= 3 && code) {
            char digits[4] = { line[0], line[1], line[2], '\0' };
            *code = atoi(digits);
        }

        if (line_out && line_len)
            snprintf(line_out, line_len, "%s", line);

        return 1;
    }
}

static int send_cmd(int fd, const char *cmd, int *code, char *line_out, size_t line_len)
{
    char buffer[768];
    int len;

    len = snprintf(buffer, sizeof(buffer), "%s\r\n", cmd);
    if (send(fd, buffer, (size_t)len, 0) != len)
        return 0;

    return read_reply(fd, code, line_out, line_len);
}

static int login(int fd)
{
    int code = 0;

    if (!read_reply(fd, &code, NULL, 0))
        return 0;

    if (code == 220) {
        if (!send_cmd(fd, "USER anonymous", &code, NULL, 0))
            return 0;

        if (code == 331) {
            if (!send_cmd(fd, "PASS ps4linux@", &code, NULL, 0))
                return 0;
        }

        if (code != 230)
            return 0;
    } else if (code != 230) {
        return 0;
    }

    if (!send_cmd(fd, "TYPE I", &code, NULL, 0))
        return 0;

    return code == 200;
}

static int quit(int fd)
{
    int code = 0;

    send_cmd(fd, "QUIT", &code, NULL, 0);
    close(fd);

    return 1;
}

int ftp_probe_host(const char *host)
{
    int fd;
    int ok;

    if (!host || !*host)
        return 0;

    fd = open_socket(host, FTP_PORT);
    if (fd < 0)
        return 0;

    ok = login(fd);
    quit(fd);

    return ok;
}

int ftp_probe(void)
{
    if (probe_state < 0)
        probe_state = ftp_probe_host(ftp_host);

    return probe_state;
}

int ftp_mkdir(const char *path)
{
    int fd;
    int code = 0;
    char cmd[640];
    int ok;

    if (!path || !*path || !ftp_probe())
        return 0;

    fd = open_socket(ftp_host, FTP_PORT);
    if (fd < 0)
        return 0;

    if (!login(fd)) {
        close(fd);
        return 0;
    }

    snprintf(cmd, sizeof(cmd), "MKD %s", path);
    ok = send_cmd(fd, cmd, &code, NULL, 0) && (code == 257 || code == 550 || code == 521);

    quit(fd);
    return ok;
}

static int parse_pasv(const char *line, char *host, size_t host_len, int *port)
{
    const char *open;
    int h1, h2, h3, h4, p1, p2;

    if (!line)
        return 0;

    open = strchr(line, '(');
    if (!open)
        return 0;

    if (sscanf(open, "(%d,%d,%d,%d,%d,%d)", &h1, &h2, &h3, &h4, &p1, &p2) != 6)
        return 0;

    snprintf(host, host_len, "%d.%d.%d.%d", h1, h2, h3, h4);
    *port = p1 * 256 + p2;

    return 1;
}

FtpSink *ftp_begin(const char *path)
{
    FtpSink *sink;
    char reply[512];
    char data_host[64];
    int data_port = 0;
    int code = 0;

    if (!path || !*path || !ftp_probe())
        return NULL;

    sink = (FtpSink *)calloc(1, sizeof(FtpSink));
    if (!sink)
        return NULL;

    sink->ctrl = -1;
    sink->data = -1;

    sink->ctrl = open_socket(ftp_host, FTP_PORT);
    if (sink->ctrl < 0)
        goto fail;

    if (!login(sink->ctrl))
        goto fail;

    if (!send_cmd(sink->ctrl, "PASV", &code, reply, sizeof(reply)) || code != 227)
        goto fail;

    if (!parse_pasv(reply, data_host, sizeof(data_host), &data_port))
        goto fail;

    sink->data = open_socket(data_host, data_port);
    if (sink->data < 0)
        goto fail;

    {
        char cmd[640];
        snprintf(cmd, sizeof(cmd), "STOR %s", path);
        if (!send_cmd(sink->ctrl, cmd, &code, NULL, 0))
            goto fail;
    }

    if (code != 150 && code != 125)
        goto fail;

    return sink;

fail:
    if (sink->data >= 0)
        close(sink->data);
    if (sink->ctrl >= 0)
        close(sink->ctrl);

    free(sink);
    return NULL;
}

int ftp_write(FtpSink *sink, const void *data, size_t len)
{
    const char *p = (const char *)data;
    size_t sent = 0;

    if (!sink || sink->data < 0)
        return 0;

    while (sent < len) {
        ssize_t n = send(sink->data, p + sent, len - sent, 0);

        if (n <= 0)
            return 0;

        sent += (size_t)n;
    }

    return 1;
}

int ftp_end(FtpSink *sink, int success)
{
    int code = 0;

    if (!sink)
        return 0;

    if (sink->data >= 0) {
        close(sink->data);
        sink->data = -1;
    }

    if (sink->ctrl >= 0) {
        if (success)
            read_reply(sink->ctrl, &code, NULL, 0);

        send_cmd(sink->ctrl, "QUIT", &code, NULL, 0);
        close(sink->ctrl);
    }

    free(sink);
    return success;
}
