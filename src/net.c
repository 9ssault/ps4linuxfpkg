#include "net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <orbis/Http.h>
#include <orbis/Net.h>
#include <orbis/Ssl.h>
#include <orbis/Sysmodule.h>

#define HTTP_USER_AGENT "Mozilla/5.0 (PLAYSTATION 4; 1.00)"
#define NET_POOLSIZE    (4 * 1024)
#define MAX_REDIRECTS   6
/* GitHub's signed release-asset URLs run to ~2 KB, so a 1 KB buffer truncated
 * them into an invalid host. */
#define URL_MAX         4096
/* libSceHttp rejects GitHub's release redirects (5 KB of headers on their own)
 * with TOO_LARGE_RESPONSE_HEADER unless the limit is raised. */
#define HEADER_MAX      (64 * 1024)

static int g_net_id;
static int g_http_id;
static int g_ssl_id;
static int g_ready;

/* The console has no user-managed trust store we can use here, and every
 * payload is a public release artifact, so verification is accepted
 * unconditionally. */
static int ssl_accepted(int ssl_id, unsigned int verify_err, void *const cert[], int cert_num, void *arg)
{
    (void)ssl_id;
    (void)verify_err;
    (void)cert;
    (void)cert_num;
    (void)arg;

    return 1;
}

int net_init(void)
{
    int rc;

    if (g_ready)
        return 1;

    if (sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_NET) < 0)
        return 0;
    if (sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_HTTP) < 0)
        return 0;
    if (sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_SSL) < 0)
        return 0;

    sceNetInit();

    rc = sceNetPoolCreate("ps4li_net", NET_POOLSIZE, 0);
    if (rc < 0) {
        printf("sceNetPoolCreate: 0x%08X\n", rc);
        return 0;
    }
    g_net_id = rc;

    rc = sceSslInit(SSL_POOLSIZE);
    if (rc < 0) {
        printf("sceSslInit: 0x%08X\n", rc);
        return 0;
    }
    g_ssl_id = rc;

    rc = sceHttpInit(g_net_id, g_ssl_id, LIBHTTP_POOLSIZE);
    if (rc < 0) {
        printf("sceHttpInit: 0x%08X\n", rc);
        return 0;
    }
    g_http_id = rc;

    g_ready = 1;
    return 1;
}

void net_shutdown(void)
{
    if (g_http_id)
        sceHttpTerm(g_http_id);
    if (g_ssl_id)
        sceSslTerm(g_ssl_id);
    if (g_net_id)
        sceNetPoolDestroy(g_net_id);

    g_http_id = 0;
    g_ssl_id = 0;
    g_net_id = 0;
    g_ready = 0;
}

static void set_err(char *err, size_t err_len, const char *msg, int code)
{
    if (!err || err_len == 0)
        return;

    if (code)
        snprintf(err, err_len, "%s (0x%08X)", msg, code);
    else
        snprintf(err, err_len, "%s", msg);
}

/* libSceHttp's auto-redirect entry point is an unimplemented stub in the
 * OpenOrbis stubs, so Location headers are followed by hand. */
static int extract_location(int req, char *out, size_t out_len)
{
    char *headers = NULL;
    size_t headers_size = 0;
    char text[8192];

    if (sceHttpGetAllResponseHeaders(req, &headers, &headers_size) < 0 || !headers)
        return 0;

    if (headers_size >= sizeof(text))
        headers_size = sizeof(text) - 1;

    memcpy(text, headers, headers_size);
    text[headers_size] = '\0';

    for (char *line = text; line && *line; ) {
        char *end = strchr(line, '\n');

        if (end)
            *end = '\0';

        if (strncasecmp(line, "location:", 9) == 0) {
            char *value = line + 9;

            while (*value == ' ' || *value == '\t')
                value++;

            snprintf(out, out_len, "%s", value);
            return out[0] != '\0';
        }

        line = end ? end + 1 : NULL;
    }

    return 0;
}

int net_download(const char *url,
                 NetWriteFn write_fn, void *write_ctx,
                 unsigned long long *total_out,
                 NetProgressFn progress_fn, void *progress_ctx,
                 char *err, size_t err_len)
{
    char current[URL_MAX];
    int ok = 0;

    if (!url || !*url) {
        set_err(err, err_len, "no URL", 0);
        return 0;
    }

    if (!net_init()) {
        set_err(err, err_len, "network stack unavailable", 0);
        return 0;
    }

    if (total_out)
        *total_out = 0;

    snprintf(current, sizeof(current), "%s", url);

    for (int hop = 0; hop <= MAX_REDIRECTS; hop++) {
        int tpl = -1;
        int conn = -1;
        int req = -1;
        int rc;
        int status = 0;
        int len_type = 0;
        size_t content_length = 0;
        unsigned long long done = 0;
        unsigned char *buffer = NULL;
        int streaming = 0;

        tpl = sceHttpCreateTemplate(g_http_id, HTTP_USER_AGENT, ORBIS_HTTP_VERSION_1_1, 0);
        if (tpl < 0) {
            set_err(err, err_len, "could not create HTTP template", tpl);
            goto next;
        }

        sceHttpsSetSslCallback(tpl, ssl_accepted, NULL);
        sceHttpSetResponseHeaderMaxSize(tpl, HEADER_MAX);

        conn = sceHttpCreateConnectionWithURL(tpl, current, 0);
        if (conn < 0) {
            set_err(err, err_len, "could not connect", conn);
            goto next;
        }

        req = sceHttpCreateRequestWithURL(conn, ORBIS_METHOD_GET, current, 0);
        if (req < 0) {
            set_err(err, err_len, "could not create request", req);
            goto next;
        }

        rc = sceHttpSendRequest(req, NULL, 0);
        if (rc < 0) {
            set_err(err, err_len, "request failed", rc);
            goto next;
        }

        rc = sceHttpGetStatusCode(req, &status);
        if (rc < 0) {
            set_err(err, err_len, "could not read status", rc);
            goto next;
        }

        if (status >= 300 && status < 400) {
            char location[URL_MAX];

            if (!extract_location(req, location, sizeof(location))) {
                snprintf(err, err_len, "HTTP %d without a Location header", status);
                goto next;
            }

            if (location[0] != 'h') {
                snprintf(err, err_len, "unsupported relative redirect");
                goto next;
            }

            snprintf(current, sizeof(current), "%s", location);
            streaming = 1; /* handled below as "try again" */
        } else if (status != 200) {
            snprintf(err, err_len, "server answered HTTP %d", status);
            goto next;
        } else {
            if (sceHttpGetResponseContentLength(req, &len_type, &content_length) < 0 ||
                len_type != ORBIS_HTTP_CONTENTLEN_EXIST)
                content_length = 0;

            if (total_out)
                *total_out = (unsigned long long)content_length;

            if (progress_fn)
                progress_fn(progress_ctx, 0, (unsigned long long)content_length);

            buffer = (unsigned char *)malloc(256 * 1024);
            if (!buffer) {
                set_err(err, err_len, "out of memory", 0);
                goto next;
            }

            for (;;) {
                int got = sceHttpReadData(req, buffer, 256 * 1024);

                if (got < 0) {
                    set_err(err, err_len, "transfer interrupted", got);
                    goto next;
                }

                if (got == 0)
                    break;

                if (!write_fn(write_ctx, buffer, (size_t)got)) {
                    set_err(err, err_len, "could not write to destination", 0);
                    goto next;
                }

                done += (unsigned long long)got;

                if (progress_fn)
                    progress_fn(progress_ctx, done, (unsigned long long)content_length);
            }

            ok = 1;
        }

next:
        if (buffer)
            free(buffer);
        if (req >= 0)
            sceHttpDeleteRequest(req);
        if (conn >= 0)
            sceHttpDeleteConnection(conn);
        if (tpl >= 0)
            sceHttpDeleteTemplate(tpl);

        if (ok)
            break;

        /* A redirect sets streaming=1 and loops; anything else is fatal. */
        if (!streaming)
            break;
    }

    if (!ok && (!err || err[0] == '\0'))
        set_err(err, err_len, "download failed (too many redirects or no data)", 0);

    return ok;
}

static int tcp_connect(const char *host, int port, int timeout_ms)
{
    struct sockaddr_in addr;
    struct timeval tv;
    fd_set wfds;
    int fd;
    int flags;
    int rc;
    int err = 0;
    socklen_t err_len = sizeof(err);

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    addr.sin_addr.s_addr = inet_addr(host);

    flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (rc < 0 && errno != EINPROGRESS) {
        close(fd);
        return -1;
    }

    FD_ZERO(&wfds);
    FD_SET(fd, &wfds);

    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    rc = select(fd + 1, NULL, &wfds, NULL, &tv);
    if (rc <= 0) {
        close(fd);
        return -1;
    }

    getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &err_len);
    if (err != 0) {
        close(fd);
        return -1;
    }

    return fd;
}

int net_tcp_probe(const char *host, int port)
{
    int fd = tcp_connect(host, port, 1500);

    if (fd < 0)
        return 0;

    close(fd);
    return 1;
}

int net_send_file(const char *host, int port, const char *path)
{
    FILE *fp;
    char *buffer;
    size_t got;
    int fd;
    int ok = 1;

    fd = tcp_connect(host, port, 3000);
    if (fd < 0)
        return 0;

    fp = fopen(path, "rb");
    if (!fp) {
        close(fd);
        return 0;
    }

    buffer = (char *)malloc(64 * 1024);
    if (!buffer) {
        fclose(fp);
        close(fd);
        return 0;
    }

    while ((got = fread(buffer, 1, 64 * 1024, fp)) > 0) {
        size_t sent = 0;

        while (sent < got) {
            ssize_t n = send(fd, buffer + sent, got - sent, 0);

            if (n <= 0) {
                ok = 0;
                break;
            }
            sent += (size_t)n;
        }

        if (!ok)
            break;
    }

    free(buffer);
    fclose(fp);

    /* GoldHEN executes the payload once the stream is closed. */
    close(fd);

    return ok;
}
