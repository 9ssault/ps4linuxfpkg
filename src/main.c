#include "catalog.h"
#include "ftp.h"
#include "net.h"
#include "storage.h"
#include "ui.h"

#include <orbis/libkernel.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FRAME_W 1920
#define FRAME_H 1080

#define PAYLOAD_DIR       "/data/payloads"
#define PAYLOAD_ELF       "ps4-linux-loader.elf"
#define PAYLOAD_BIN       "ps4-linux-loader.bin"
#define PAYLOAD_LOCAL_ELF "/app0/" PAYLOAD_ELF
#define PAYLOAD_LOCAL_BIN "/app0/" PAYLOAD_BIN

/* Patched initramfs: identical to upstream until a ps4li.conf job is present. */
#define INITRAMFS_LOCAL   "/app0/assets/initramfs.cpio.gz"

#define INTERNAL_BOOTDIR "/data/linux/boot"
#define INTERNAL_DISTRO  "/user/system/boot/distro.tar.xz"
#define INTERNAL_SYSDIR  "/user/system/boot"

#define INSTALL_VRAM_MB 1024
#define MAX_TARGETS 9

#define BINLOADER_PORT 9090

/* The PS4 answering on the LAN, plus loopback for consoles whose IP is not
 * this one. Loopback always works when the console owns the address. */
static const char *HOST_CANDIDATES[] = { "192.168.18.16", "127.0.0.1" };
#define HOST_COUNT ((int)(sizeof(HOST_CANDIDATES) / sizeof(HOST_CANDIDATES[0])))

/* Matches the loader's built-in command line; bootargs.txt replaces it. */
#define DEFAULT_CMDLINE \
    "panic=0 clocksource=tsc consoleblank=0 net.ifnames=0 radeon.dpm=0 " \
    "amdgpu.dpm=0 drm.debug=0 console=ttyS0,115200n8 console=tty0 " \
    "video=HDMI-A-1:1920x1080@60"

/* ---------------------------------------------------------------- theme --- */

static const SDL_Color C_BG_TOP   = { 5, 14, 30, 255 };
static const SDL_Color C_BG_BOT   = { 8, 27, 54, 255 };
static const SDL_Color C_PANEL    = { 13, 42, 74, 255 };
static const SDL_Color C_PANEL_HI = { 17, 58, 101, 255 };
static const SDL_Color C_BLUE     = { 0, 112, 209, 255 };
static const SDL_Color C_BLUE_HI  = { 45, 140, 235, 255 };
static const SDL_Color C_TEXT     = { 240, 246, 255, 255 };
static const SDL_Color C_DIM      = { 143, 179, 217, 255 };
static const SDL_Color C_GOOD     = { 61, 220, 132, 255 };
static const SDL_Color C_WARN     = { 255, 176, 32, 255 };
static const SDL_Color C_ERROR    = { 255, 92, 92, 255 };

typedef enum {
    SCR_HOME,
    SCR_TARGET,
    SCR_KERNEL,
    SCR_DISTRO,
    SCR_CONFIRM,
    SCR_OPTIONS,
    SCR_WORK,
    SCR_DONE,
    SCR_ERROR
} Screen;

typedef struct {
    int auto_install;
    int boot_vram;
    int btrfs;
    int size_gb;
    int performance;
} Options;

typedef struct {
    char path[64];
    char label[72];
    char detail[112];
    int  internal;
    int  writable;
} Target;

static Screen g_screen = SCR_HOME;
static int g_menu_sel;
static int g_boot_mode;

static Target g_targets[MAX_TARGETS];
static int    g_target_count;
static int    g_target_sel;
static int    g_kernel_sel;
static int    g_distro_sel;

static Options g_opts = { 1, 2048, 0, 0, 0 };
static int g_option_sel;

static int g_have_data_write;
static int g_have_user_write;
static int g_have_ftp;
static int g_have_binloader;
static char g_ftp_host[64];
static char g_bin_host[64];

static char g_error[320];
static char g_done_text[1200];

static unsigned long long g_work_done;
static unsigned long long g_work_total;
static char g_work_file[160];
static char g_work_note[160];
static unsigned long long g_work_last_render;

static void render(void);
static void render_work_now(void);
static int  progress_cb(void *ctx, unsigned long long done, unsigned long long total);
static void paragraph_draw(int x, int y, int px, SDL_Color color, const char *text, int max_width);

/* --------------------------------------------------------------- helpers --- */

static void fmt_size(char *out, size_t out_len, unsigned long long bytes)
{
    if (bytes >= 1024ULL * 1024ULL * 1024ULL)
        snprintf(out, out_len, "%.2f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
    else if (bytes >= 1024ULL * 1024ULL)
        snprintf(out, out_len, "%.0f MB", (double)bytes / (1024.0 * 1024.0));
    else
        snprintf(out, out_len, "%llu KB", bytes / 1024ULL);
}

static void log_line(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    vsnprintf(g_work_note, sizeof(g_work_note), fmt, args);
    va_end(args);
}

static unsigned int make_job_id(void)
{
    static unsigned int counter;
    unsigned int value = (unsigned int)SDL_GetTicks() ^ (++counter * 2654435761u);

    return value ? value : 1u;
}

/* ---------------------------------------------------------------- sinks --- */

typedef struct {
    FILE    *file;
    FtpSink *ftp;
    char     final_path[512];
    char     temp_path[512];
} Sink;

static int sink_open(Sink *sink, const char *path)
{
    memset(sink, 0, sizeof(*sink));

    snprintf(sink->final_path, sizeof(sink->final_path), "%s", path);
    snprintf(sink->temp_path, sizeof(sink->temp_path), "%s.part", path);

    sink->file = fopen(sink->temp_path, "wb");
    if (sink->file)
        return 1;

    /* Sandbox refused the write; GoldHEN's FTP server runs outside it. */
    sink->ftp = ftp_begin(path);

    return sink->ftp != NULL;
}

static int sink_write(void *ctx, const void *data, size_t len)
{
    Sink *sink = (Sink *)ctx;

    if (sink->file)
        return fwrite(data, 1, len, sink->file) == len;

    return ftp_write(sink->ftp, data, len);
}

static int sink_close(Sink *sink, int ok)
{
    if (sink->file) {
        fclose(sink->file);
        sink->file = NULL;

        if (ok && rename(sink->temp_path, sink->final_path) != 0)
            ok = 0;

        if (!ok)
            remove(sink->temp_path);

        return ok;
    }

    if (sink->ftp) {
        ftp_end(sink->ftp, ok);
        sink->ftp = NULL;
    }

    return ok;
}

static int put_text(const char *path, const char *text)
{
    Sink sink;
    int ok;

    if (!sink_open(&sink, path))
        return 0;

    ok = sink_write(&sink, text, strlen(text));
    return sink_close(&sink, ok);
}

static int put_file(const char *src, const char *dst, const char *label)
{
    FILE *in;
    Sink sink;
    char *buffer;
    size_t got;
    int ok = 1;

    in = fopen(src, "rb");
    if (!in)
        return 0;

    if (!sink_open(&sink, dst)) {
        fclose(in);
        return 0;
    }

    if (label) {
        snprintf(g_work_file, sizeof(g_work_file), "%s", label);
        g_work_done = 0;
        g_work_total = (unsigned long long)fs_size(src);
        render_work_now();
    }

    buffer = (char *)malloc(256 * 1024);
    if (!buffer) {
        fclose(in);
        sink_close(&sink, 0);
        return 0;
    }

    while ((got = fread(buffer, 1, 256 * 1024, in)) > 0) {
        if (!sink_write(&sink, buffer, got)) {
            ok = 0;
            break;
        }

        if (label) {
            g_work_done += got;
            progress_cb(NULL, g_work_done, g_work_total);
        }
    }

    free(buffer);
    fclose(in);

    return sink_close(&sink, ok);
}

/* Creating a directory can fail inside the app sandbox; GoldHEN's FTP server
 * can create it instead, one component at a time. */
static void ensure_dir(const char *path)
{
    char tmp[512];
    size_t len;
    size_t i;

    if (fs_mkdir_p(path))
        return;

    if (!ftp_probe())
        return;

    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);

    for (i = 1; i < len; i++) {
        if (tmp[i] != '/')
            continue;

        tmp[i] = '\0';
        ftp_mkdir(tmp);
        tmp[i] = '/';
    }

    ftp_mkdir(path);
}

/* -------------------------------------------------------------- branding --- */

static void background(void)
{
    const int bands = 16;
    int height = FRAME_H / bands;

    for (int i = 0; i < bands; i++) {
        SDL_Color c;
        double t = (double)i / (bands - 1);

        c.r = (Uint8)(C_BG_TOP.r + (C_BG_BOT.r - C_BG_TOP.r) * t);
        c.g = (Uint8)(C_BG_TOP.g + (C_BG_BOT.g - C_BG_TOP.g) * t);
        c.b = (Uint8)(C_BG_TOP.b + (C_BG_BOT.b - C_BG_TOP.b) * t);
        c.a = 255;

        ui_rect(0, i * height, FRAME_W, (i == bands - 1) ? FRAME_H - i * height : height, c);
    }

    /* Soft blue glow behind the header. */
    ui_rect(0, 0, FRAME_W, 6, C_BLUE_HI);
    ui_rect(0, 6, FRAME_W, 2, C_BLUE);
}

static void wordmark(void)
{
    ui_rect(120, 66, 14, 14, C_BLUE_HI);
    ui_text_b(146, 58, 24, C_TEXT, "PS4 LINUX");
    ui_text(146, 88, 18, C_DIM, "installer");
}

static void step_badge(int step, int total)
{
    char text[32];
    int width;

    snprintf(text, sizeof(text), "%d / %d", step, total);
    width = ui_text_width(26, text) + 48;

    ui_rect(FRAME_W - 120 - width, 58, width, 44, C_PANEL);
    ui_text(FRAME_W - 120 - width + 24, 66, 26, C_DIM, "%s", text);
}

static void header(const char *title, const char *subtitle, int step, int total)
{
    background();
    wordmark();

    if (step > 0)
        step_badge(step, total);

    ui_heading(120, 168, 60, C_TEXT, C_BLUE_HI, "%s", title);

    if (subtitle && *subtitle)
        ui_text(120, 264, 26, C_DIM, "%s", subtitle);
}

static void footer(const char *hint)
{
    ui_rect(120, FRAME_H - 96, FRAME_W - 240, 1, C_PANEL_HI);
    ui_text(120, FRAME_H - 70, 22, C_DIM, "%s", hint);
}

static void pill(int x, int y, const char *label, int state)
{
    SDL_Color dot = state > 0 ? C_GOOD : (state == 0 ? C_WARN : C_ERROR);
    int width = ui_text_width(20, label) + 46;

    ui_rect(x, y, width, 38, C_PANEL);
    ui_rect(x + 16, y + 15, 8, 8, dot);
    ui_text(x + 32, y + 8, 20, C_DIM, "%s", label);
}

/* -------------------------------------------------------------- rendering --- */

static void draw_list(int sel, const char *const *labels, const char *const *details,
                      int count, int y0, int row_h, int label_px, int detail_px)
{
    for (int i = 0; i < count; i++) {
        int y = y0 + i * (row_h + 12);
        int selected = (i == sel);

        ui_rect(120, y, FRAME_W - 240, row_h, selected ? C_PANEL_HI : C_PANEL);

        if (selected)
            ui_rect(120, y, 6, row_h, C_BLUE_HI);

        ui_text_b(160, y + 16, label_px, selected ? C_TEXT : C_DIM, "%s", labels[i]);

        if (details && details[i])
            ui_text(160, y + 16 + label_px + 10, detail_px, C_DIM, "%s", details[i]);

        if (selected)
            ui_text(FRAME_W - 176, y + row_h / 2 - label_px / 2, label_px, C_BLUE_HI, ">");
    }
}

static void render_home(void)
{
    static const char *labels[4] = {
        "Install Linux",
        "Boot Linux",
        "Options",
        "Quit"
    };
    static const char *details[4] = {
        "Fetch the kernel, initramfs and distro, then install and boot automatically",
        "Start an installation that already exists, using your saved VRAM setting",
        "Automatic install, VRAM, filesystem, size, boot arguments",
        "Close the installer"
    };
    char status[128];
    int y = 340;

    header("Set up Linux on your PS4",
           "Everything the tutorial asks you to copy around, done in one pass.",
           0, 0);

    draw_list(g_menu_sel, labels, details, 4, y, 96, 34, 22);

    /* Environment strip */
    y = FRAME_H - 214;
    snprintf(status, sizeof(status), "%s",
             (g_have_data_write && g_have_user_write) ? "storage: direct" : "storage: FTP fallback");
    pill(120, y, status, (g_have_data_write && g_have_user_write) ? 1 : 0);

    snprintf(status, sizeof(status), "FTP %s:2121", g_ftp_host[0] ? g_ftp_host : "down");
    pill(430, y, status, g_have_ftp);

    snprintf(status, sizeof(status), "BinLoader %s:9090", g_bin_host[0] ? g_bin_host : "down");
    pill(840, y, status, g_have_binloader);

    ui_text(120, FRAME_H - 156, 21, C_DIM,
            "Needs GoldHEN v2.4b18.10+ with FTP and BinLoader enabled in its server settings.");
    ui_text(120, FRAME_H - 126, 21, C_DIM,
            "Zero-typing mode is %s. Console address %s, installer ports 2121 and 9090.",
            g_opts.auto_install ? "on" : "off",
            HOST_CANDIDATES[0]);

    footer("X Select     D-Pad Move     Options Quit");
}

static void render_target(void)
{
    static const char *labels[MAX_TARGETS];
    static const char *details[MAX_TARGETS];

    for (int i = 0; i < g_target_count; i++) {
        labels[i] = g_targets[i].label;
        details[i] = g_targets[i].detail;
    }

    header(g_boot_mode ? "Where is the installation?" : "Where should the files go?",
           g_boot_mode
               ? "The loader checks the USB drives before the internal HDD."
               : "External is faster and recommended. Internal only for Aeolia/Belize.",
           2, 4);

    if (g_target_count == 0) {
        ui_text(120, 360, 30, C_ERROR, "No USB drive found. Plug one in and reopen this screen.");
    } else {
        draw_list(g_target_sel, labels, details, g_target_count, 360, 96, 32, 21);
    }

    footer("X Select     O Back     D-Pad Move");
}

static void render_kernel(void)
{
    static const char *labels[KERNEL_COUNT];
    static const char *details[KERNEL_COUNT];

    for (int i = 0; i < KERNEL_COUNT; i++) {
        labels[i] = KERNELS[i].name;
        details[i] = KERNELS[i].detail;
    }

    header("Which southbridge?",
           "Settings > System Information shows it. Baikal needs a different kernel build.",
           3, 4);

    draw_list(g_kernel_sel, labels, details, KERNEL_COUNT, 380, 104, 34, 21);
    footer("X Select     O Back     D-Pad Move");
}

static void render_distro(void)
{
    static const char *labels[DISTRO_COUNT];
    static const char *details[DISTRO_COUNT];
    char detail[DISTRO_COUNT][112];

    for (int i = 0; i < DISTRO_COUNT; i++) {
        char size[32];

        if (DISTROS[i].size) {
            fmt_size(size, sizeof(size), DISTROS[i].size);
            snprintf(detail[i], sizeof(detail[i]), "%s  -  %s", DISTROS[i].detail, size);
        } else {
            snprintf(detail[i], sizeof(detail[i]), "%s", DISTROS[i].detail);
        }

        labels[i] = DISTROS[i].name;
        details[i] = detail[i];
    }

    header("Choose a distro",
           "Arch-based images update cleanly on PS4 hardware. CachyOS ships via Mega, so import it.",
           4, 4);

    draw_list(g_distro_sel, labels, details, DISTRO_COUNT, 340, 96, 32, 21);
    footer("X Select     O Back     D-Pad Move");
}

static long long target_free(const Target *t)
{
    if (!t->internal)
        return fs_free(t->path);

    if (fs_free(INTERNAL_SYSDIR) > 0)
        return fs_free(INTERNAL_SYSDIR);

    if (fs_free("/user/system") > 0)
        return fs_free("/user/system");

    return fs_free("/user");
}

static void render_confirm(void)
{
    const Target *t = &g_targets[g_target_sel];
    const DistroEntry *d = &DISTROS[g_distro_sel];
    char free_s[32];
    char need_s[32];
    long long free_bytes;
    unsigned long long need;
    int y = 360;

    header("Ready when you are", "Nothing is written to the console until you press X.", 4, 4);

    ui_text_b(120, y, 30, C_DIM, "Storage");
    ui_text(480, y, 30, C_TEXT, "%s", t->label);
    y += 54;

    ui_text_b(120, y, 30, C_DIM, "Kernel");
    ui_text(480, y, 30, C_TEXT, "%s", KERNELS[g_kernel_sel].name);
    y += 54;

    ui_text_b(120, y, 30, C_DIM, "Distro");
    ui_text(480, y, 30, C_TEXT, "%s", d->name);
    y += 54;

    ui_text_b(120, y, 30, C_DIM, "Install");
    ui_text(480, y, 30, C_TEXT, "%s%s",
            g_opts.auto_install ? "automatic (no typing in the rescue shell)" : "manual rescue shell",
            g_opts.performance ? ", performance boot args" : "");
    y += 54;

    need = d->size ? d->size + (64ULL * 1024ULL * 1024ULL) : 0;
    free_bytes = target_free(t);
    fmt_size(free_s, sizeof(free_s), free_bytes > 0 ? (unsigned long long)free_bytes : 0);

    ui_text_b(120, y, 30, C_DIM, "Space");
    if (need == 0) {
        ui_text(480, y, 30, C_TEXT, "archive comes from the USB drive (%s free)", free_s);
    } else {
        fmt_size(need_s, sizeof(need_s), need);
        ui_text(480, y, 30, (free_bytes < 0 || (unsigned long long)free_bytes < need) ? C_ERROR : C_TEXT,
                "%s needed, %s free", need_s, free_s);
    }

    y += 84;

    if (!d->url)
        ui_text(120, y, 24, C_WARN,
                "CachyOS is not downloadable here: put its .tar.xz in the USB root first.");

    if (!t->internal)
        ui_text(120, y + 36, 24, C_WARN,
                "External install needs a second, empty USB drive. Remove any disc from the console.");
    else
        ui_text(120, y + 36, 24, C_WARN,
                "Internal install is slow on the stock HDD and unsupported on Baikal.");

    footer("X Start     O Back");
}

static void render_options(void)
{
    char labels[6][48];
    char values[6][48];
    static const char *label_ptrs[6];
    static const char *value_ptrs[6];
    int y = 340;
    int row_h = 88;

    snprintf(labels[0], sizeof(labels[0]), "Automatic install");
    snprintf(values[0], sizeof(values[0]), "%s", g_opts.auto_install ? "On" : "Off");

    snprintf(labels[1], sizeof(labels[1]), "Boot VRAM");
    snprintf(values[1], sizeof(values[1]), "%d MB%s", g_opts.boot_vram,
             g_opts.boot_vram == 1024 ? "  (first boot after install)" : "");

    snprintf(labels[2], sizeof(labels[2]), "Internal filesystem");
    snprintf(values[2], sizeof(values[2]), "%s", g_opts.btrfs ? "btrfs" : "ext4");

    snprintf(labels[3], sizeof(labels[3]), "Internal size");
    if (g_opts.size_gb > 0)
        snprintf(values[3], sizeof(values[3]), "%d GB", g_opts.size_gb);
    else
        snprintf(values[3], sizeof(values[3]), "Auto");

    snprintf(labels[4], sizeof(labels[4]), "Boot arguments");
    snprintf(values[4], sizeof(values[4]), "%s", g_opts.performance ? "Performance (mitigations off)" : "Default");

    snprintf(labels[5], sizeof(labels[5]), "Back");

    for (int i = 0; i < 6; i++) {
        label_ptrs[i] = labels[i];
        value_ptrs[i] = values[i];
    }

    header("Options", "Left and right change a value. These are saved for this session.",
           0, 0);

    for (int i = 0; i < 6; i++) {
        int row = y + i * (row_h + 10);
        int selected = (i == g_option_sel);

        ui_rect(120, row, FRAME_W - 240, row_h, selected ? C_PANEL_HI : C_PANEL);
        if (selected)
            ui_rect(120, row, 6, row_h, C_BLUE_HI);

        ui_text_b(160, row + 30, 30, selected ? C_TEXT : C_DIM, "%s", labels[i]);

        if (i < 5)
            ui_text(FRAME_W - 560, row + 32, 26, selected ? C_BLUE_HI : C_DIM, "%s", values[i]);
    }

    footer("Up/Down Move     Left/Right Change     X or O Back");
}

static void render_work(void)
{
    char buf[160];
    int bar_w = FRAME_W - 240;
    int pct = 0;

    header(g_boot_mode ? "Starting Linux" : "Installing", "Do not close the app while this runs.", 0, 0);

    if (g_work_total > 0) {
        pct = (int)((g_work_done * 100ULL) / g_work_total);
        if (pct > 100)
            pct = 100;
    }

    ui_text_b(120, 360, 34, C_TEXT, "%s", g_work_file);

    ui_rect(120, 430, bar_w, 30, C_PANEL);
    if (g_work_total > 0)
        ui_rect(120, 430, (int)((long long)bar_w * g_work_done / (long long)g_work_total), 30, C_BLUE_HI);
    else
        ui_rect(120, 430, bar_w, 30, C_PANEL_HI);

    if (g_work_total > 0) {
        char done_s[32];
        char total_s[32];

        fmt_size(done_s, sizeof(done_s), g_work_done);
        fmt_size(total_s, sizeof(total_s), g_work_total);
        snprintf(buf, sizeof(buf), "%d%%     %s / %s", pct, done_s, total_s);
    } else {
        snprintf(buf, sizeof(buf), "%llu KB", g_work_done / 1024ULL);
    }

    ui_text(120, 476, 26, C_DIM, "%s", buf);

    if (g_work_note[0])
        ui_text(120, 540, 24, C_DIM, "%s", g_work_note);

    footer("Working...");
}

static void render_done(void)
{
    header(g_boot_mode ? "Linux is starting" : "Installation queued", "", 0, 0);
    paragraph_draw(120, 340, 28, C_TEXT, g_done_text, FRAME_W - 240);
    footer("X Back to menu");
}

static void render_error(void)
{
    header("That did not work", "", 0, 0);
    paragraph_draw(120, 340, 26, C_ERROR, g_error, FRAME_W - 240);
    footer("X or O Back to menu");
}

static void render(void)
{
    switch (g_screen) {
    case SCR_HOME:    render_home();    break;
    case SCR_TARGET:  render_target();  break;
    case SCR_KERNEL:  render_kernel();  break;
    case SCR_DISTRO:  render_distro();  break;
    case SCR_CONFIRM: render_confirm(); break;
    case SCR_OPTIONS: render_options(); break;
    case SCR_WORK:    render_work();    break;
    case SCR_DONE:    render_done();    break;
    case SCR_ERROR:   render_error();   break;
    }

    ui_present();
}

static void render_work_now(void)
{
    render();
    g_work_last_render = SDL_GetTicks();
}

/* Word-wrapped text so long status and error messages stay on screen. */
static void paragraph_draw(int x, int y, int px, SDL_Color color, const char *text, int max_width)
{
    char line[256] = "";
    char word[128];
    const char *p = text;

    while (*p) {
        int w = 0;

        if (*p == '\n') {
            ui_text(x, y, px, color, "%s", line);
            y += px + px / 2;
            line[0] = '\0';
            p++;
            continue;
        }

        while (*p == ' ')
            p++;

        while (p[w] && p[w] != ' ' && p[w] != '\n' && w < (int)sizeof(word) - 1) {
            word[w] = p[w];
            w++;
        }

        word[w] = '\0';
        p += w;

        {
            char candidate[384];

            if (line[0])
                snprintf(candidate, sizeof(candidate), "%s %s", line, word);
            else
                snprintf(candidate, sizeof(candidate), "%s", word);

            if (ui_text_width(px, candidate) > max_width && line[0]) {
                ui_text(x, y, px, color, "%s", line);
                y += px + px / 2;
                snprintf(line, sizeof(line), "%s", word);
            } else {
                snprintf(line, sizeof(line), "%s", candidate);
            }
        }
    }

    if (line[0])
        ui_text(x, y, px, color, "%s", line);
}

static int progress_cb(void *ctx, unsigned long long done, unsigned long long total)
{
    (void)ctx;

    g_work_done = done;
    if (total)
        g_work_total = total;

    if (SDL_GetTicks() - g_work_last_render > 120)
        render_work_now();

    return 1;
}

/* ----------------------------------------------------------------- work --- */

static int download_to(const char *url, const char *path, const char *label)
{
    Sink sink;
    char err[192];
    int ok;

    snprintf(g_work_file, sizeof(g_work_file), "%s", label);
    g_work_done = 0;
    g_work_total = 0;
    render_work_now();

    if (!sink_open(&sink, path)) {
        snprintf(g_error, sizeof(g_error),
                 "cannot write %s. The sandbox refused it and GoldHEN's FTP server is not answering.", path);
        return 0;
    }

    ok = net_download(url, sink_write, &sink, &g_work_total, progress_cb, NULL, err, sizeof(err));
    ok = sink_close(&sink, ok);

    if (!ok) {
        snprintf(g_error, sizeof(g_error), "%s: %s", label, err);
        return 0;
    }

    return 1;
}

static int write_vram(const char *bootdir, int mb)
{
    char path[512];
    char text[32];

    fs_join(path, sizeof(path), bootdir, "vram.txt");
    snprintf(text, sizeof(text), "%d\n", mb);

    return put_text(path, text);
}

static int write_bootargs(const char *bootdir)
{
    char path[512];
    char text[512];

    fs_join(path, sizeof(path), bootdir, "bootargs.txt");
    snprintf(text, sizeof(text), "%s%s\n",
             DEFAULT_CMDLINE, g_opts.performance ? " mitigations=off" : "");

    return put_text(path, text);
}

/* Queue an unattended install. The patched initramfs consumes this file once. */
static int write_job(const char *bootdir, const char *mode)
{
    char path[512];
    char text[256];
    unsigned int job = make_job_id();

    fs_join(path, sizeof(path), bootdir, "ps4li.conf");
    snprintf(text, sizeof(text),
             "mode=%s\nfs=%s\nsize_gb=%d\njob=%08x\n",
             mode, g_opts.btrfs ? "btrfs" : "ext4", g_opts.size_gb, job);

    return put_text(path, text);
}

static const char *find_source_usb(void)
{
    static char found[512];

    for (int i = 0; i < g_target_count; i++) {
        if (g_targets[i].internal)
            continue;

        if (fs_find_archive(g_targets[i].path, found, sizeof(found)))
            return g_targets[i].path;
    }

    return NULL;
}

static int import_manual_distro(const char *usb, const char *dst)
{
    char found[512];

    if (!usb || !fs_find_archive(usb, found, sizeof(found))) {
        snprintf(g_error, sizeof(g_error),
                 "No .tar.xz found on the USB drive. Copy the distro archive to the USB root, then retry.");
        return 0;
    }

    if (strcmp(found, dst) == 0)
        return 1;

    log_line("copying %s", found);

    if (!put_file(found, dst, "Importing the distro archive")) {
        snprintf(g_error, sizeof(g_error), "Could not move the distro archive to %s.", dst);
        return 0;
    }

    remove(found);
    return 1;
}

static int install_payload_files(void)
{
    char elf[512];
    char bin[512];

    ensure_dir(PAYLOAD_DIR);

    fs_join(elf, sizeof(elf), PAYLOAD_DIR, PAYLOAD_ELF);
    fs_join(bin, sizeof(bin), PAYLOAD_DIR, PAYLOAD_BIN);

    if (!put_file(PAYLOAD_LOCAL_ELF, elf, "Placing the boot payload"))
        log_line("warning: could not copy the payload to %s", elf);

    if (!put_file(PAYLOAD_LOCAL_BIN, bin, NULL))
        log_line("warning: could not copy the payload to %s", bin);

    return 1;
}

static int try_inject(void)
{
    if (!g_have_binloader)
        return 0;

    return net_send_file(g_bin_host, BINLOADER_PORT, PAYLOAD_LOCAL_ELF);
}

static int run_install(void)
{
    const Target *t = &g_targets[g_target_sel];
    const DistroEntry *d = &DISTROS[g_distro_sel];
    char bootdir[512];
    char path[512];
    char dst[512];
    const char *mode;
    int injected;

    if (t->internal) {
        snprintf(bootdir, sizeof(bootdir), "%s", INTERNAL_BOOTDIR);
        ensure_dir(INTERNAL_BOOTDIR);
        ensure_dir(INTERNAL_SYSDIR);
        mode = "internal";
    } else {
        snprintf(bootdir, sizeof(bootdir), "%s", t->path);
        mode = "ext";
    }

    if (!download_to(KERNELS[g_kernel_sel].url,
                     (fs_join(path, sizeof(path), bootdir, "bzImage"), path),
                     "Downloading the kernel (bzImage)"))
        return 0;

    /* The bundled initramfs is upstream plus the unattended installer; without
     * a job file it behaves exactly like the stock one. */
    if (!put_file(INITRAMFS_LOCAL,
                  (fs_join(path, sizeof(path), bootdir, "initramfs.cpio.gz"), path),
                  "Installing the rescue initramfs")) {
        snprintf(g_error, sizeof(g_error), "Could not write initramfs.cpio.gz to %s.", bootdir);
        return 0;
    }

    if (!write_vram(bootdir, INSTALL_VRAM_MB)) {
        snprintf(g_error, sizeof(g_error), "Could not write vram.txt to %s.", bootdir);
        return 0;
    }

    if (!write_bootargs(bootdir)) {
        snprintf(g_error, sizeof(g_error), "Could not write bootargs.txt to %s.", bootdir);
        return 0;
    }

    if (t->internal)
        snprintf(dst, sizeof(dst), "%s", INTERNAL_DISTRO);
    else
        fs_join(dst, sizeof(dst), bootdir, "distro.tar.xz");

    if (d->url) {
        if (!download_to(d->url, dst, "Downloading the distro (this is the big one)"))
            return 0;
    } else if (!import_manual_distro(find_source_usb(), dst)) {
        return 0;
    }

    if (!g_opts.auto_install) {
        /* A stale job from an earlier attempt would still run on boot. */
        char stale[512];

        fs_join(stale, sizeof(stale), bootdir, "ps4li.conf");
        remove(stale);
    }

    if (g_opts.auto_install) {
        if (!write_job(bootdir, mode)) {
            snprintf(g_error, sizeof(g_error),
                     "Could not queue the automatic install in %s. Turn it off in Options to install manually.",
                     bootdir);
            return 0;
        }
    }

    install_payload_files();

    snprintf(g_work_file, sizeof(g_work_file), "Loading the boot payload");
    render_work_now();

    injected = try_inject();

    if (g_opts.auto_install) {
        snprintf(g_done_text, sizeof(g_done_text),
                 "%s\n\n"
                 "Nothing else to type: the kernel boots the rescue initramfs, which %s "
                 "and continues straight into the new desktop.\n\n"
                 "Remove any disc from the drive now. If the payload did not start, use "
                 "GoldHEN > Utilities > PayLoader > %s.",
                 injected ? "Files placed and the boot payload was sent to GoldHEN."
                          : "Files placed. The payload could not be sent automatically.",
                 t->internal
                    ? "creates the Linux image on the internal drive, extracts the distro"
                    : "formats the empty USB drive, copies the boot files and extracts the distro",
                 PAYLOAD_ELF);
    } else {
        snprintf(g_done_text, sizeof(g_done_text),
                 "Files placed. Automatic install is off, so the rescue shell waits for you.\n\n"
                 "Run:  %s\n"
                 "%s\n\n"
                 "If the payload did not start, use GoldHEN > Utilities > PayLoader > %s.",
                 t->internal ? "install-linux-int.sh" : "install-linux-ext.sh",
                 t->internal
                    ? "Pick the size in GB, then 1 for ext4 or 2 for btrfs."
                    : "Select the source drive, then the target drive, then type YES.",
                 PAYLOAD_ELF);
    }

    return 1;
}

static int run_boot(void)
{
    const Target *t = &g_targets[g_target_sel];
    char bootdir[512];
    int injected;

    snprintf(bootdir, sizeof(bootdir), "%s", t->internal ? INTERNAL_BOOTDIR : t->path);

    {
        char kernel[512];
        char initrd[512];

        fs_join(kernel, sizeof(kernel), bootdir, "bzImage");
        fs_join(initrd, sizeof(initrd), bootdir, "initramfs.cpio.gz");

        if (!fs_exists(kernel) || !fs_exists(initrd)) {
            snprintf(g_error, sizeof(g_error),
                     "%s has no bzImage/initramfs.cpio.gz yet. Run Install Linux first, "
                     "or pick the drive that holds them.", bootdir);
            return 0;
        }
    }

    if (!write_vram(bootdir, g_opts.boot_vram))
        log_line("warning: could not write vram.txt, the payload default will be used");

    if (!write_bootargs(bootdir))
        log_line("warning: could not write bootargs.txt");

    if (!fs_exists(PAYLOAD_LOCAL_ELF)) {
        snprintf(g_error, sizeof(g_error), "The bundled payload is missing from the package.");
        return 0;
    }

    install_payload_files();

    snprintf(g_work_file, sizeof(g_work_file), "Loading the boot payload");
    render_work_now();

    injected = try_inject();

    snprintf(g_done_text, sizeof(g_done_text),
             "%s\n\n"
             "VRAM set to %d MB. If nothing happens, launch it from GoldHEN > Utilities > "
             "PayLoader > %s. The loader reads the USB drive before the internal HDD.",
             injected ? "Boot payload sent to GoldHEN. Linux should start booting now."
                      : "The payload could not be sent automatically.",
             g_opts.boot_vram, PAYLOAD_ELF);

    return 1;
}

/* ------------------------------------------------------------ navigation --- */

static void discover_targets(void)
{
    g_target_count = 0;

    for (int i = 0; i < MAX_TARGETS; i++) {
        char path[64];
        char fstype[32];
        char detail[112];
        char free_s[32];
        long long free_bytes;
        Target *t;

        snprintf(path, sizeof(path), "/mnt/usb%d", i);

        if (!fs_is_dir(path))
            continue;

        t = &g_targets[g_target_count];

        memset(t, 0, sizeof(*t));
        snprintf(t->path, sizeof(t->path), "%s", path);
        snprintf(t->label, sizeof(t->label), "USB drive %d", i);

        fs_fstype(path, fstype, sizeof(fstype));
        free_bytes = fs_free(path);
        fmt_size(free_s, sizeof(free_s), free_bytes > 0 ? (unsigned long long)free_bytes : 0);

        t->writable = fs_test_write(path);

        snprintf(detail, sizeof(detail), "%s  -  %s free%s",
                 fstype[0] ? fstype : "unknown filesystem", free_s,
                 t->writable ? "" : "  -  not writable by the app");
        snprintf(t->detail, sizeof(t->detail), "%s", detail);

        g_target_count++;
    }

    if (g_target_count < MAX_TARGETS) {
        Target *t = &g_targets[g_target_count];
        char free_s[32];
        long long free_bytes;

        memset(t, 0, sizeof(*t));
        snprintf(t->path, sizeof(t->path), "/data");
        snprintf(t->label, sizeof(t->label), "Internal HDD");

        t->internal = 1;
        free_bytes = target_free(t);
        fmt_size(free_s, sizeof(free_s), free_bytes > 0 ? (unsigned long long)free_bytes : 0);
        snprintf(t->detail, sizeof(t->detail),
                 "Aeolia/Belize only, slow on the stock HDD  -  %s free", free_s);
        t->writable = g_have_data_write && g_have_user_write;

        g_target_count++;
    }
}

static void cycle_option(int direction)
{
    static const int vram_steps[] = { 1024, 2048, 3072, 4096 };
    static const int size_steps[] = { 0, 16, 32, 64, 128 };
    int index;

    switch (g_option_sel) {
    case 0:
        g_opts.auto_install = !g_opts.auto_install;
        break;

    case 1:
        index = 0;
        for (int i = 0; i < 4; i++)
            if (vram_steps[i] == g_opts.boot_vram)
                index = i;
        index = (index + direction + 4) % 4;
        g_opts.boot_vram = vram_steps[index];
        break;

    case 2:
        g_opts.btrfs = !g_opts.btrfs;
        break;

    case 3:
        index = 0;
        for (int i = 0; i < 5; i++)
            if (size_steps[i] == g_opts.size_gb)
                index = i;
        index = (index + direction + 5) % 5;
        g_opts.size_gb = size_steps[index];
        break;

    case 4:
        g_opts.performance = !g_opts.performance;
        break;

    default:
        break;
    }
}

static void handle_input(UiInput in)
{
    switch (g_screen) {
    case SCR_HOME:
        if (in.up)
            g_menu_sel = (g_menu_sel + 3) % 4;
        if (in.down)
            g_menu_sel = (g_menu_sel + 1) % 4;

        if (in.confirm) {
            if (g_menu_sel == 0 || g_menu_sel == 1) {
                g_boot_mode = (g_menu_sel == 1);
                g_target_sel = 0;
                discover_targets(); /* drives may have been plugged in since boot */
                g_screen = SCR_TARGET;
            } else if (g_menu_sel == 2) {
                g_option_sel = 0;
                g_screen = SCR_OPTIONS;
            } else {
                exit(0);
            }
        }
        break;

    case SCR_OPTIONS:
        if (in.up && g_option_sel > 0)
            g_option_sel--;
        if (in.down && g_option_sel < 5)
            g_option_sel++;
        if (in.left)
            cycle_option(-1);
        if (in.right)
            cycle_option(1);
        if (in.confirm || in.back)
            g_screen = SCR_HOME;
        break;

    case SCR_TARGET:
        if (in.up && g_target_sel > 0)
            g_target_sel--;
        if (in.down && g_target_sel + 1 < g_target_count)
            g_target_sel++;

        if (in.back)
            g_screen = SCR_HOME;

        if (in.confirm && g_target_count > 0) {
            if (g_boot_mode) {
                g_screen = SCR_WORK;
                render_work_now();
                g_screen = run_boot() ? SCR_DONE : SCR_ERROR;
            } else {
                g_screen = SCR_KERNEL;
            }
        }
        break;

    case SCR_KERNEL:
        if (in.up && g_kernel_sel > 0)
            g_kernel_sel--;
        if (in.down && g_kernel_sel + 1 < KERNEL_COUNT)
            g_kernel_sel++;

        if (in.back)
            g_screen = SCR_TARGET;
        if (in.confirm)
            g_screen = SCR_DISTRO;
        break;

    case SCR_DISTRO:
        if (in.up && g_distro_sel > 0)
            g_distro_sel--;
        if (in.down && g_distro_sel + 1 < DISTRO_COUNT)
            g_distro_sel++;

        if (in.back)
            g_screen = SCR_KERNEL;
        if (in.confirm)
            g_screen = SCR_CONFIRM;
        break;

    case SCR_CONFIRM:
        if (in.back)
            g_screen = SCR_DISTRO;

        if (in.confirm) {
            const Target *t = &g_targets[g_target_sel];
            const DistroEntry *d = &DISTROS[g_distro_sel];
            unsigned long long need = d->size ? d->size + (64ULL * 1024ULL * 1024ULL) : 0;
            long long free_bytes = target_free(t);

            if (need && free_bytes > 0 && (unsigned long long)free_bytes < need) {
                snprintf(g_error, sizeof(g_error),
                         "Not enough free space on %s. The download needs about %llu MB more.\n\n"
                         "Free some space, or pick another drive.",
                         t->label,
                         (need - (unsigned long long)free_bytes) / (1024ULL * 1024ULL));
                g_screen = SCR_ERROR;
                break;
            }

            g_screen = SCR_WORK;
            render_work_now();
            g_screen = run_install() ? SCR_DONE : SCR_ERROR;
        }
        break;

    case SCR_DONE:
    case SCR_ERROR:
        if (in.confirm || in.back) {
            g_menu_sel = 0;
            g_screen = SCR_HOME;
        }
        break;

    case SCR_WORK:
        break;
    }
}

static void resolve_servers(void)
{
    g_ftp_host[0] = '\0';
    g_bin_host[0] = '\0';

    for (int i = 0; i < HOST_COUNT; i++) {
        if (!g_ftp_host[0] && ftp_probe_host(HOST_CANDIDATES[i]))
            snprintf(g_ftp_host, sizeof(g_ftp_host), "%s", HOST_CANDIDATES[i]);

        if (!g_bin_host[0] && net_tcp_probe(HOST_CANDIDATES[i], BINLOADER_PORT))
            snprintf(g_bin_host, sizeof(g_bin_host), "%s", HOST_CANDIDATES[i]);
    }

    ftp_set_host(g_ftp_host[0] ? g_ftp_host : "127.0.0.1");

    g_have_ftp = g_ftp_host[0] != '\0';
    g_have_binloader = g_bin_host[0] != '\0';
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    if (!ui_init()) {
        printf("UI init failed\n");
        for (;;)
            sceKernelUsleep(1000 * 1000);
    }

    g_have_data_write = fs_test_write("/data");
    g_have_user_write = fs_test_write("/user/system") || fs_test_write("/user");

    if (!net_init())
        printf("network stack init failed\n");

    resolve_servers();
    discover_targets();

    for (;;) {
        render();

        {
            UiInput in = ui_poll();

            if (in.quit)
                break;

            handle_input(in);
        }

        SDL_Delay(16);
    }

    ui_shutdown();
    net_shutdown();

    return 0;
}
