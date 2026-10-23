#include "ui.h"

#include <orbis/Sysmodule.h>

/* The PS4 FreeType build ships a reduced API, declared in this header. */
#include <proto-include.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FRAME_W 1920
#define FRAME_H 1080

#define FONT_REGULAR  "/app0/assets/fonts/Inter-Regular.ttf"
#define FONT_SEMIBOLD "/app0/assets/fonts/Inter-SemiBold.ttf"

/* Raw PS4 pad button order as exposed by SDL's joystick API. */
#define PAD_SQUARE   0
#define PAD_CROSS    1
#define PAD_CIRCLE   2
#define PAD_TRIANGLE 3
#define PAD_OPTIONS  9

static SDL_Window  *g_window;
static SDL_Surface *g_win_surface;
static SDL_Surface *g_canvas;
static SDL_Joystick *g_joy;

static FT_Library g_ft;
static FT_Face    g_regular;
static FT_Face    g_semibold;

static UiInput g_input;

int ui_init(void)
{
    int rc;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK) != 0) {
        printf("SDL_Init failed: %s\n", SDL_GetError());
        return 0;
    }

    /* FreeType is a system module on the PS4; without loading it first the
     * freetype calls below fail with unresolved NIDs. */
    rc = sceSysmoduleLoadModule(ORBIS_SYSMODULE_FREETYPE_OL);
    if (rc < 0) {
        printf("could not load freetype module: 0x%08X\n", rc);
        return 0;
    }

    if (FT_Init_FreeType(&g_ft) != 0) {
        printf("FT_Init_FreeType failed\n");
        return 0;
    }

    if (FT_New_Face(g_ft, FONT_REGULAR, 0, &g_regular) != 0) {
        printf("could not load %s\n", FONT_REGULAR);
        return 0;
    }

    if (FT_New_Face(g_ft, FONT_SEMIBOLD, 0, &g_semibold) != 0) {
        printf("could not load %s\n", FONT_SEMIBOLD);
        return 0;
    }

    g_window = SDL_CreateWindow("PS4 Linux Installer",
                                SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                FRAME_W, FRAME_H, 0);
    if (!g_window) {
        printf("SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 0;
    }

    g_win_surface = SDL_GetWindowSurface(g_window);
    if (!g_win_surface) {
        printf("SDL_GetWindowSurface failed: %s\n", SDL_GetError());
        return 0;
    }

    /* Draw into our own ARGB surface so the pixel format never depends on what
     * the console handed us. */
    g_canvas = SDL_CreateRGBSurface(0, FRAME_W, FRAME_H, 32,
                                    0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000);
    if (!g_canvas) {
        printf("SDL_CreateRGBSurface failed: %s\n", SDL_GetError());
        return 0;
    }

    if (SDL_NumJoysticks() > 0)
        g_joy = SDL_JoystickOpen(0);

    if (!g_joy)
        printf("no controller detected; keyboard input only\n");

    return 1;
}

void ui_shutdown(void)
{
    if (g_joy)
        SDL_JoystickClose(g_joy);

    if (g_canvas)
        SDL_FreeSurface(g_canvas);

    if (g_regular)
        FT_Done_Face(g_regular);

    if (g_semibold)
        FT_Done_Face(g_semibold);

    if (g_ft)
        FT_Done_FreeType(g_ft);

    if (g_window)
        SDL_DestroyWindow(g_window);

    SDL_Quit();
}

void ui_begin(void)
{
    if (g_canvas)
        SDL_FillRect(g_canvas, NULL, SDL_MapRGB(g_canvas->format, 0, 0, 0));
}

void ui_present(void)
{
    if (!g_canvas || !g_win_surface)
        return;

    SDL_BlitSurface(g_canvas, NULL, g_win_surface, NULL);
    SDL_UpdateWindowSurface(g_window);
}

void ui_rect(int x, int y, int w, int h, SDL_Color color)
{
    SDL_Rect r;

    if (!g_canvas)
        return;

    r.x = x;
    r.y = y;
    r.w = w;
    r.h = h;

    SDL_FillRect(g_canvas, &r, SDL_MapRGBA(g_canvas->format, color.r, color.g, color.b, color.a));
}

static void blend_pixel(int x, int y, SDL_Color color, unsigned char coverage)
{
    uint32_t *row;
    uint32_t dst;
    int alpha;
    int dr, dg, db;

    if (x < 0 || y < 0 || x >= g_canvas->w || y >= g_canvas->h || coverage == 0)
        return;

    row = (uint32_t *)((uint8_t *)g_canvas->pixels + (size_t)y * g_canvas->pitch);
    dst = row[x];

    alpha = (coverage * color.a) / 255;
    if (alpha == 0)
        return;

    dr = (dst >> 16) & 0xFF;
    dg = (dst >> 8) & 0xFF;
    db = dst & 0xFF;

    dr = (color.r * alpha + dr * (255 - alpha)) / 255;
    dg = (color.g * alpha + dg * (255 - alpha)) / 255;
    db = (color.b * alpha + db * (255 - alpha)) / 255;

    row[x] = 0xFF000000u | ((uint32_t)dr << 16) | ((uint32_t)dg << 8) | (uint32_t)db;
}

static FT_Face face_for(int bold)
{
    FT_Face face = bold ? g_semibold : g_regular;

    if (!face)
        face = g_regular;

    return face;
}

/* Rasterising a glyph costs a few hundred microseconds on a PS4 CPU and a
 * full screen is a few hundred glyphs, so redrawing text every frame is what
 * made the UI crawl. Keep the coverage maps in a small cache instead. */
#define GLYPH_CACHE 1024

typedef struct {
    int            valid;
    FT_Face        face;
    int            px;
    unsigned int   code;
    int            advance;
    int            left;
    int            top;
    unsigned int   rows;
    unsigned int   width;
    unsigned int   pitch;
    unsigned char *buffer;
} Glyph;

static Glyph g_glyphs[GLYPH_CACHE];

static Glyph *glyph_get(FT_Face face, unsigned char ch, int px)
{
    unsigned int code = (unsigned int)ch;
    unsigned int hash = code * 131u + (unsigned int)px * 7u + (face == g_semibold ? 1u : 0u);
    Glyph *g = &g_glyphs[hash % GLYPH_CACHE];
    FT_GlyphSlot slot;
    size_t bytes;

    if (g->valid && g->face == face && g->px == px && g->code == code)
        return g;

    if (!face || FT_Set_Char_Size(face, 0, px * 64, 0, 0) != 0)
        return NULL;

    if (FT_Load_Glyph(face, FT_Get_Char_Index(face, ch), FT_LOAD_DEFAULT) != 0)
        return NULL;

    if (FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL) != 0)
        return NULL;

    slot = face->glyph;
    bytes = (size_t)slot->bitmap.pitch * slot->bitmap.rows;

    free(g->buffer);
    g->buffer = NULL;

    if (bytes > 0) {
        g->buffer = (unsigned char *)malloc(bytes);
        if (!g->buffer)
            return NULL;
        memcpy(g->buffer, slot->bitmap.buffer, bytes);
    }

    g->valid = 1;
    g->face = face;
    g->px = px;
    g->code = code;
    g->advance = (int)(slot->advance.x >> 6);
    g->left = (int)slot->bitmap_left;
    g->top = (int)slot->bitmap_top;
    g->rows = slot->bitmap.rows;
    g->width = slot->bitmap.width;
    g->pitch = (unsigned int)slot->bitmap.pitch;

    return g;
}

static int glyph_width(FT_Face face, unsigned char ch, int px)
{
    Glyph *g = glyph_get(face, ch, px);

    return g ? g->advance : px / 2;
}

static void draw_one_char(FT_Face face, int *pen_x, int baseline, int px,
                          SDL_Color color, unsigned char ch)
{
    Glyph *g = glyph_get(face, ch, px);
    int gx, gy;

    if (!g)
        return;

    gx = *pen_x + g->left;
    gy = baseline - g->top;

    for (unsigned int row = 0; row < g->rows; row++) {
        for (unsigned int col = 0; col < g->width; col++) {
            unsigned char cov = g->buffer[row * g->pitch + col];
            blend_pixel(gx + (int)col, gy + (int)row, color, cov);
        }
    }

    *pen_x += g->advance;
}

static void draw_text(FT_Face face, int x, int y, int px, SDL_Color color, const char *text)
{
    int pen_x = x;
    int baseline = y + px;

    if (!g_canvas || !face)
        return;

    for (const char *p = text; *p; p++) {
        if (*p == '\n') {
            pen_x = x;
            baseline += px + (px / 3);
            continue;
        }
        draw_one_char(face, &pen_x, baseline, px, color, (unsigned char)*p);
    }
}

static void vtext(int bold, int x, int y, int px, SDL_Color color, const char *fmt, va_list args)
{
    char buffer[1024];

    vsnprintf(buffer, sizeof(buffer), fmt, args);
    draw_text(face_for(bold), x, y, px, color, buffer);
}

void ui_text(int x, int y, int px, SDL_Color color, const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    vtext(0, x, y, px, color, fmt, args);
    va_end(args);
}

void ui_text_b(int x, int y, int px, SDL_Color color, const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    vtext(1, x, y, px, color, fmt, args);
    va_end(args);
}

int ui_text_width(int px, const char *text)
{
    int w = 0;

    if (!text)
        return 0;

    for (const char *p = text; *p; p++)
        w += glyph_width(face_for(0), (unsigned char)*p, px);

    return w;
}

void ui_heading(int x, int y, int px, SDL_Color color, SDL_Color accent, const char *fmt, ...)
{
    va_list args;
    char buffer[512];
    int width;

    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    draw_text(face_for(1), x, y, px, color, buffer);

    width = 0;
    for (const char *p = buffer; *p; p++)
        width += glyph_width(face_for(1), (unsigned char)*p, px);

    if (width > 0)
        ui_rect(x, y + px + 18, width < 120 ? width : 120, 5, accent);
}

UiInput ui_poll(void)
{
    SDL_Event ev;
    UiInput out;

    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
        case SDL_JOYBUTTONDOWN:
            if (ev.jbutton.button == PAD_CROSS || ev.jbutton.button == PAD_SQUARE)
                g_input.confirm = 1;
            else if (ev.jbutton.button == PAD_CIRCLE || ev.jbutton.button == PAD_TRIANGLE)
                g_input.back = 1;
            else if (ev.jbutton.button == PAD_OPTIONS)
                g_input.quit = 1;
            break;

        case SDL_JOYHATMOTION:
            if (ev.jhat.value & SDL_HAT_UP)
                g_input.up = 1;
            if (ev.jhat.value & SDL_HAT_DOWN)
                g_input.down = 1;
            if (ev.jhat.value & SDL_HAT_LEFT)
                g_input.left = 1;
            if (ev.jhat.value & SDL_HAT_RIGHT)
                g_input.right = 1;
            break;

        case SDL_JOYAXISMOTION:
            if (ev.jaxis.axis == 1) {
                if (ev.jaxis.value < -24000)
                    g_input.up = 1;
                else if (ev.jaxis.value > 24000)
                    g_input.down = 1;
            } else if (ev.jaxis.axis == 0) {
                if (ev.jaxis.value < -24000)
                    g_input.left = 1;
                else if (ev.jaxis.value > 24000)
                    g_input.right = 1;
            }
            break;

        case SDL_KEYDOWN:
            if (ev.key.keysym.sym == SDLK_ESCAPE)
                g_input.quit = 1;
            else if (ev.key.keysym.sym == SDLK_UP)
                g_input.up = 1;
            else if (ev.key.keysym.sym == SDLK_DOWN)
                g_input.down = 1;
            else if (ev.key.keysym.sym == SDLK_LEFT)
                g_input.left = 1;
            else if (ev.key.keysym.sym == SDLK_RIGHT)
                g_input.right = 1;
            else if (ev.key.keysym.sym == SDLK_RETURN || ev.key.keysym.sym == SDLK_SPACE)
                g_input.confirm = 1;
            else if (ev.key.keysym.sym == SDLK_BACKSPACE)
                g_input.back = 1;
            break;

        case SDL_QUIT:
            g_input.quit = 1;
            break;

        default:
            break;
        }
    }

    out = g_input;
    memset(&g_input, 0, sizeof(g_input));

    return out;
}
