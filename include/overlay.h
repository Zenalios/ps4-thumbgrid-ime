/**
 * @file overlay.h
 * @brief VideoOut framebuffer overlay hooks and drawing primitives
 */

#ifndef OVERLAY_H
#define OVERLAY_H

#include <stdint.h>
#include <stdbool.h>

/* ─── Overlay State ──────────────────────────────────────────────── */

#define OVERLAY_MAX_BUFFERS 8

typedef void (*overlay_draw_cb_t)(uint32_t *fb, uint32_t pitch,
                                  uint32_t width, uint32_t height);

/* ─── Public API ─────────────────────────────────────────────────── */

int32_t overlay_init(void);
void    overlay_cleanup(void);
void    overlay_set_draw_callback(overlay_draw_cb_t cb);
bool    overlay_is_active(void);      /* true if framebuffers have been captured */
int32_t overlay_get_tiling_mode(void);  /* 0=TILE, 1=LINEAR */
bool    overlay_is_flipping(void);   /* true if game is actively flipping (recent submitFlip) */
void    overlay_force_draw(overlay_draw_cb_t cb); /* draw to ALL buffers (opaque mode) */
void    overlay_force_draw_single(overlay_draw_cb_t cb); /* draw ONE buffer, rotating */
void    overlay_draw_last_flipped(overlay_draw_cb_t cb); /* draw to last-flipped buffer (safe) */

/* ─── Drawing Primitives ─────────────────────────────────────────── */

/* Pixel format: A8B8G8R8_SRGB — MSB to LSB: A(31:24) B(23:16) G(15:8) R(7:0) */
#define OVERLAY_COLOR(r, g, b)  (0xFF000000u | ((uint32_t)(b) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(r))

void overlay_draw_rect(uint32_t *fb, uint32_t pitch,
                       int x, int y, int w, int h, uint32_t color);

void overlay_draw_char(uint32_t *fb, uint32_t pitch,
                       int x, int y, char ch, uint32_t fg, uint32_t bg);

void overlay_draw_text(uint32_t *fb, uint32_t pitch,
                       int x, int y, const char *str,
                       uint32_t fg, uint32_t bg);

void overlay_draw_rect_alpha(uint32_t *fb, uint32_t pitch,
                              int x, int y, int w, int h,
                              uint32_t color, uint8_t alpha);

void overlay_put_pixel_ext(uint32_t *fb, int x, int y, uint32_t color);

/* 2x scaled drawing (16x16 per character) */
void overlay_draw_char_2x(uint32_t *fb, uint32_t pitch,
                          int x, int y, char ch, uint32_t fg, uint32_t bg);

void overlay_draw_text_2x(uint32_t *fb, uint32_t pitch,
                          int x, int y, const char *str,
                          uint32_t fg, uint32_t bg);

#endif /* OVERLAY_H */
