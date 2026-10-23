#pragma once

#include <SDL2/SDL.h>

typedef struct {
    int up;
    int down;
    int left;
    int right;
    int confirm;
    int back;
    int quit;
} UiInput;

int  ui_init(void);
void ui_shutdown(void);

void ui_begin(void);
void ui_present(void);

void ui_rect(int x, int y, int w, int h, SDL_Color color);

/* Regular body text. */
void ui_text(int x, int y, int px, SDL_Color color, const char *fmt, ...);
/* SemiBold, for headings and labels. */
void ui_text_b(int x, int y, int px, SDL_Color color, const char *fmt, ...);
/* Also draws an accent underline sized to the rendered text. */
void ui_heading(int x, int y, int px, SDL_Color color, SDL_Color accent, const char *fmt, ...);

int  ui_text_width(int px, const char *text);

UiInput ui_poll(void);
