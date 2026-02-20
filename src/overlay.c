/**
 * @file overlay.c
 * @brief VideoOut framebuffer overlay — hooks + drawing primitives
 *
 * Hooks sceVideoOutRegisterBuffers to capture framebuffer pointers/dimensions
 * and sceVideoOutSubmitFlip to blit overlay before each frame is displayed.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "plugin_common.h"
#include "overlay.h"
#include "font8x8.h"

#include <Detour.h>
#include <GoldHEN.h>
#include <orbis/libkernel.h>
#include <orbis/VideoOut.h>

/* ─── VideoOut Function Pointer Types ────────────────────────────── */

typedef int32_t (*sceVideoOutRegisterBuffers_t)(
    int32_t handle, int32_t startIndex, void *const *addresses,
    int32_t bufferNum, const OrbisVideoOutBufferAttribute *attribute);

typedef int32_t (*sceVideoOutSubmitFlip_t)(
    int32_t handle, int32_t bufferIndex,
    uint32_t flipMode, int64_t flipArg);

/* ─── Static State ───────────────────────────────────────────────── */

/* Max absolute buffer index PS4 supports */
#define ABS_MAX_BUFFERS 16

typedef struct OverlayState {
    bool     initialized;
    bool     hooks_installed;
    void    *buffers[ABS_MAX_BUFFERS];  /* sparse, indexed by absolute bufIdx */
    int32_t  buffer_count;              /* total registered */
    uint32_t width;
    uint32_t height;
    uint32_t pitch;       /* pixels per row */
    int32_t  tiling_mode;
    int32_t  video_handle;
    bool     first_draw_logged;
    bool     first_flip_logged;
} OverlayState;

static OverlayState g_overlay;

static Detour g_hook_register_buffers;
static Detour g_hook_submit_flip;

static sceVideoOutRegisterBuffers_t g_orig_register_buffers = NULL;
static sceVideoOutSubmitFlip_t      g_orig_submit_flip      = NULL;

static overlay_draw_cb_t g_draw_callback = NULL;

/* Flip timing — used to detect if game is actively flipping */
static uint64_t g_last_flip_us = 0;

/* Track last flipped buffer index — safe to draw to from poll loop
 * because GPU is done with it and game won't touch it until recycled. */
static int32_t g_last_flipped_idx = -1;

/* ─── Performance Instrumentation ─────────────────────────────────── */
static uint64_t g_flip_perf_last_log_us   = 0;
static uint32_t g_flip_perf_count         = 0;
static uint64_t g_flip_perf_draw_total_us = 0;
static uint64_t g_flip_perf_draw_max_us   = 0;
static uint64_t g_fd_perf_last_log_us     = 0;
static uint32_t g_fd_perf_call_count      = 0;
static uint32_t g_fd_perf_buf_total       = 0;
static uint64_t g_fd_perf_total_us        = 0;
static uint64_t g_fd_perf_max_us          = 0;

/* When true, overlay_draw_rect_alpha behaves as opaque (alpha=255).
 * Set during force_draw to prevent alpha compounding on re-draws. */
static bool g_force_opaque = false;

/* ─── Module Resolution Helper ───────────────────────────────────── */

/**
 * Find the module handle for a library by scanning loaded modules.
 * Returns the handle (>= 0) or -1 if not found.
 */
static int32_t find_module_handle(const char *name_substr) {
    OrbisKernelModule handles[128];
    size_t available = 0;

    int32_t rc = sceKernelGetModuleList(handles, sizeof(handles), &available);
    if (rc != 0) {
        LOG_WARN("sceKernelGetModuleList failed: 0x%08X", rc);
        return -1;
    }

    size_t count = available / sizeof(OrbisKernelModule);
    if (count > 128) count = 128;

    LOG_DEBUG("Scanning %zu loaded modules for '%s'", count, name_substr);

    for (size_t i = 0; i < count; i++) {
        OrbisKernelModuleInfo info;
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);

        rc = sceKernelGetModuleInfo(handles[i], &info);
        if (rc != 0) continue;

        /* Check if the module name contains our substring */
        if (strstr(info.name, name_substr)) {
            LOG_INFO("Found module '%s' handle=%u", info.name, handles[i]);
            return (int32_t)handles[i];
        }
    }

    LOG_WARN("Module '%s' not found among %zu loaded modules", name_substr, count);
    return -1;
}

/* ─── Hooked: sceVideoOutRegisterBuffers ─────────────────────────── */

static int32_t hooked_register_buffers(
    int32_t handle, int32_t startIndex, void *const *addresses,
    int32_t bufferNum, const OrbisVideoOutBufferAttribute *attribute)
{
    LOG_INFO("sceVideoOutRegisterBuffers: handle=%d start=%d num=%d",
        handle, startIndex, bufferNum);

    if (attribute) {
        LOG_INFO("  attr: %ux%u pitch=%u tmode=%d fmt=0x%08X",
            attribute->width, attribute->height,
            attribute->pixelPitch, attribute->tmode, attribute->format);
    }

    /* Capture framebuffer info — store by absolute buffer index */
    if (addresses && bufferNum > 0 && attribute) {
        g_overlay.video_handle = handle;
        g_overlay.width        = attribute->width;
        g_overlay.height       = attribute->height;
        g_overlay.pitch        = attribute->pixelPitch;
        g_overlay.tiling_mode  = attribute->tmode;

        int32_t stored = 0;
        for (int32_t i = 0; i < bufferNum; i++) {
            int32_t abs_idx = startIndex + i;
            if (abs_idx >= 0 && abs_idx < ABS_MAX_BUFFERS) {
                g_overlay.buffers[abs_idx] = addresses[i];
                stored++;
                LOG_DEBUG("  buffer[abs %d] = %p", abs_idx, addresses[i]);
            }
        }
        g_overlay.buffer_count += stored;

        LOG_INFO("Captured %d buffers (%ux%u pitch=%u tmode=%d start=%d total=%d)",
            stored, g_overlay.width, g_overlay.height,
            g_overlay.pitch, g_overlay.tiling_mode, startIndex,
            g_overlay.buffer_count);
    }

    if (!g_orig_register_buffers) {
        return -1;
    }
    return g_orig_register_buffers(handle, startIndex, addresses,
                                   bufferNum, attribute);
}

/* ─── Hooked: sceVideoOutSubmitFlip ──────────────────────────────── */

static int32_t hooked_submit_flip(
    int32_t handle, int32_t bufferIndex,
    uint32_t flipMode, int64_t flipArg)
{
    /* Track first flip for logging only (no popup) */
    if (!g_overlay.first_flip_logged) {
        g_overlay.first_flip_logged = true;
        LOG_INFO("First flip: idx=%d has_cb=%d bufs=%d",
            bufferIndex, g_draw_callback != NULL,
            g_overlay.buffer_count);
    }

    /* Record flip timestamp and buffer index */
    g_last_flip_us = sceKernelGetProcessTime();

    /* Draw overlay BEFORE original submitFlip.
     * There's a race with the GPU (it may still be rendering), but
     * drawing before the flip is the only approach that produces visible
     * results — drawing after submitFlip is invisible (buffer is handed
     * off to the display subsystem). */
    overlay_draw_cb_t cb = g_draw_callback;
    uint64_t flip_draw_us = 0;
    if (cb && g_overlay.width > 0 &&
        bufferIndex >= 0 && bufferIndex < ABS_MAX_BUFFERS &&
        g_overlay.buffers[bufferIndex])
    {
        uint64_t t0 = sceKernelGetProcessTime();  /* PERF */
        uint32_t *fb = (uint32_t *)g_overlay.buffers[bufferIndex];
        cb(fb, g_overlay.pitch, g_overlay.width, g_overlay.height);
        flip_draw_us = sceKernelGetProcessTime() - t0;  /* PERF */
    }

    /* PERF: accumulate flip draw stats, log once per second */
    {
        g_flip_perf_count++;
        g_flip_perf_draw_total_us += flip_draw_us;
        if (flip_draw_us > g_flip_perf_draw_max_us) g_flip_perf_draw_max_us = flip_draw_us;

        uint64_t now = sceKernelGetProcessTime();
        if (now - g_flip_perf_last_log_us >= 1000000) {
            printf("[CIME] FLIP: flips/s=%u  draw_avg=%luus  draw_max=%luus  buf=%d\n",
                g_flip_perf_count,
                (unsigned long)(g_flip_perf_count ? g_flip_perf_draw_total_us / g_flip_perf_count : 0),
                (unsigned long)g_flip_perf_draw_max_us,
                bufferIndex);
            g_flip_perf_last_log_us   = now;
            g_flip_perf_count         = 0;
            g_flip_perf_draw_total_us = 0;
            g_flip_perf_draw_max_us   = 0;
        }
    }

    /* Track which buffer was just flipped — the poll loop can safely
     * reinforce this buffer since the GPU has moved to the next one. */
    g_last_flipped_idx = bufferIndex;

    if (!g_orig_submit_flip) {
        return -1;
    }
    return g_orig_submit_flip(handle, bufferIndex, flipMode, flipArg);
}

/* ─── Tiled Pixel Write ──────────────────────────────────────────── */

/**
 * Write a single pixel to the framebuffer, handling tiling mode.
 *
 * PS4 TILING_MODE_TILE (0) uses AMD GCN 2D macro-tiled surfaces
 * (ARRAY_2D_TILED_THIN1, Display micro-tile mode).
 *
 * Configuration derived from shadPS4 emulator tile mode tables
 * (Display2DThin, tile mode 10, 32bpp):
 *
 *   numPipes        = 8   (pipe config: P8_32x32_16x16)
 *   numBanks        = 16  (macro tile mode 2)
 *   bankWidth       = 1
 *   bankHeight      = 1
 *   macroTileAspect = 2
 *   pipeInterleave  = 256 bytes (64 uint32_t elements)
 *
 * Micro-tile pixel index (Display, 32bpp):
 *   bit0=x[0] bit1=x[1] bit2=y[0] bit3=x[2] bit4=y[1] bit5=y[2]
 *
 * Pipe (P8_32x32_16x16, from SiLib::ComputePipeFromCoord):
 *   pipeBit0 = x[3] ^ y[3] ^ x[4]
 *   pipeBit1 = x[4] ^ y[4]
 *   pipeBit2 = x[5] ^ y[5]
 *
 * Bank (numBanks=16, from EgBasedLib::ComputeBankFromCoord):
 *   tx = x / (8 * bankWidth * numPipes) = x / 64
 *   ty = y / (8 * bankHeight)           = y / 8
 *   bankBit0 = tx[0] ^ ty[3]  → x[6] ^ y[6]
 *   bankBit1 = tx[1] ^ ty[2] ^ ty[3]  → x[7] ^ y[5] ^ y[6]
 *   bankBit2 = tx[2] ^ ty[1]  → x[8] ^ y[4]
 *   bankBit3 = tx[3] ^ ty[0]  → x[9] ^ y[3]
 *
 * Macro-tile: 128px wide × 64px tall
 *   pitch  = 8 * bankWidth * numPipes * macroAspect = 8*1*8*2 = 128
 *   height = 8 * bankHeight * numBanks / macroAspect = 8*1*16/2 = 64
 *   elements = 128 * 64 = 8192 per macro-tile
 *
 * Element address layout:
 *   [5:0]   = pixel within micro-tile  (64)
 *   [8:6]   = pipe                     (8 slots)
 *   [12:9]  = bank                     (16 slots)
 *   [13+]   = macro-tile index
 */
static inline void overlay_put_pixel(uint32_t *fb, int x, int y,
                                     uint32_t color)
{
    if (x < 0 || y < 0 ||
        (uint32_t)x >= g_overlay.width ||
        (uint32_t)y >= g_overlay.height)
        return;

    uint32_t ux = (uint32_t)x;
    uint32_t uy = (uint32_t)y;
    uint32_t pitch = g_overlay.pitch;

    if (g_overlay.tiling_mode == ORBIS_VIDEO_OUT_TILING_MODE_TILE) {
        uint32_t lx = ux & 7;
        uint32_t ly = uy & 7;

        /* Display micro-tile pixel index, 32bpp:
         *   bit0=x[0] bit1=x[1] bit2=y[0] bit3=x[2] bit4=y[1] bit5=y[2] */
        uint32_t pix = (lx & 3)            /* x[0], x[1] → bits 0, 1 */
                     | ((ly & 1) << 2)      /* y[0] → bit 2 */
                     | ((lx & 4) << 1)      /* x[2] → bit 3 */
                     | ((ly & 2) << 3)      /* y[1] → bit 4 */
                     | ((ly & 4) << 3);     /* y[2] → bit 5 */

        /* Pipe: P8_32x32_16x16 (8 pipes, 3 bits) */
        uint32_t pipeBit0 = ((ux >> 3) ^ (uy >> 3) ^ (ux >> 4)) & 1;
        uint32_t pipeBit1 = ((ux >> 4) ^ (uy >> 4)) & 1;
        uint32_t pipeBit2 = ((ux >> 5) ^ (uy >> 5)) & 1;
        uint32_t pipe = pipeBit0 | (pipeBit1 << 1) | (pipeBit2 << 2);

        /* Bank: numBanks=16 (4 bits)
         * tx = x / 64, ty = y / 8 */
        uint32_t bankBit0 = ((ux >> 6) ^ (uy >> 6)) & 1;
        uint32_t bankBit1 = ((ux >> 7) ^ (uy >> 5) ^ (uy >> 6)) & 1;
        uint32_t bankBit2 = ((ux >> 8) ^ (uy >> 4)) & 1;
        uint32_t bankBit3 = ((ux >> 9) ^ (uy >> 3)) & 1;
        uint32_t bank = bankBit0
                      | (bankBit1 << 1)
                      | (bankBit2 << 2)
                      | (bankBit3 << 3);

        /* Macro-tile: 128px wide, 64px tall (macroAspect=2)
         * 8 pipes × 16 banks × 64 px = 8192 elements per macro-tile */
        uint32_t mtX   = ux >> 7;           /* x / 128 */
        uint32_t mtY   = uy >> 6;           /* y / 64 */
        uint32_t mtRow = pitch >> 7;        /* pitch / 128 */
        uint32_t mtIdx = mtY * mtRow + mtX;

        uint32_t off = (mtIdx << 13)
                     | (bank  << 9)
                     | (pipe  << 6)
                     | pix;

        fb[off] = color;
    } else {
        /* LINEAR mode */
        fb[uy * pitch + ux] = color;
    }
}

/* ─── Tiled Pixel Read (inverse of put_pixel) ────────────────────── */

static inline uint32_t overlay_read_pixel(const uint32_t *fb, int x, int y)
{
    if (x < 0 || y < 0 ||
        (uint32_t)x >= g_overlay.width ||
        (uint32_t)y >= g_overlay.height)
        return 0;

    uint32_t ux = (uint32_t)x;
    uint32_t uy = (uint32_t)y;
    uint32_t pitch = g_overlay.pitch;

    if (g_overlay.tiling_mode == ORBIS_VIDEO_OUT_TILING_MODE_TILE) {
        uint32_t lx = ux & 7;
        uint32_t ly = uy & 7;

        uint32_t pix = (lx & 3)
                     | ((ly & 1) << 2)
                     | ((lx & 4) << 1)
                     | ((ly & 2) << 3)
                     | ((ly & 4) << 3);

        uint32_t pipeBit0 = ((ux >> 3) ^ (uy >> 3) ^ (ux >> 4)) & 1;
        uint32_t pipeBit1 = ((ux >> 4) ^ (uy >> 4)) & 1;
        uint32_t pipeBit2 = ((ux >> 5) ^ (uy >> 5)) & 1;
        uint32_t pipe = pipeBit0 | (pipeBit1 << 1) | (pipeBit2 << 2);

        uint32_t bankBit0 = ((ux >> 6) ^ (uy >> 6)) & 1;
        uint32_t bankBit1 = ((ux >> 7) ^ (uy >> 5) ^ (uy >> 6)) & 1;
        uint32_t bankBit2 = ((ux >> 8) ^ (uy >> 4)) & 1;
        uint32_t bankBit3 = ((ux >> 9) ^ (uy >> 3)) & 1;
        uint32_t bank = bankBit0
                      | (bankBit1 << 1)
                      | (bankBit2 << 2)
                      | (bankBit3 << 3);

        uint32_t mtX   = ux >> 7;
        uint32_t mtY   = uy >> 6;
        uint32_t mtRow = pitch >> 7;
        uint32_t mtIdx = mtY * mtRow + mtX;

        uint32_t off = (mtIdx << 13)
                     | (bank  << 9)
                     | (pipe  << 6)
                     | pix;

        return fb[off];
    } else {
        return fb[uy * pitch + ux];
    }
}

/* ─── Alpha Blend Pixel ──────────────────────────────────────────── */

static inline void overlay_blend_pixel(uint32_t *fb, int x, int y,
                                       uint32_t color, uint8_t alpha)
{
    if (alpha == 255) {
        overlay_put_pixel(fb, x, y, color);
        return;
    }
    if (alpha == 0) return;

    uint32_t bg = overlay_read_pixel(fb, x, y);

    uint32_t sr = color & 0xFF;
    uint32_t sg = (color >> 8) & 0xFF;
    uint32_t sb = (color >> 16) & 0xFF;

    uint32_t dr = bg & 0xFF;
    uint32_t dg = (bg >> 8) & 0xFF;
    uint32_t db = (bg >> 16) & 0xFF;

    uint32_t a = alpha;
    uint32_t inv_a = 255 - a;

    uint32_t rr = (sr * a + dr * inv_a) / 255;
    uint32_t rg = (sg * a + dg * inv_a) / 255;
    uint32_t rb = (sb * a + db * inv_a) / 255;

    uint32_t blended = 0xFF000000u | (rb << 16) | (rg << 8) | rr;
    overlay_put_pixel(fb, x, y, blended);
}

/* ─── Fast Tiled Rect Fill ───────────────────────────────────────── */

/**
 * Compute the framebuffer offset for an 8-pixel aligned span.
 * Within a micro-tile (8×8 block), pipe and bank are constant,
 * so we compute tiling once and return the base offset.
 * Caller writes 8 pixels at base + {0,1,2,3,8,9,10,11}.
 */
static inline uint32_t tile_span_offset(uint32_t ux, uint32_t uy,
                                         uint32_t pix_y, uint32_t mtY,
                                         uint32_t mtRow)
{
    uint32_t pipeBit0 = ((ux >> 3) ^ (uy >> 3) ^ (ux >> 4)) & 1;
    uint32_t pipeBit1 = ((ux >> 4) ^ (uy >> 4)) & 1;
    uint32_t pipeBit2 = ((ux >> 5) ^ (uy >> 5)) & 1;
    uint32_t pipe = pipeBit0 | (pipeBit1 << 1) | (pipeBit2 << 2);

    uint32_t bankBit0 = ((ux >> 6) ^ (uy >> 6)) & 1;
    uint32_t bankBit1 = ((ux >> 7) ^ (uy >> 5) ^ (uy >> 6)) & 1;
    uint32_t bankBit2 = ((ux >> 8) ^ (uy >> 4)) & 1;
    uint32_t bankBit3 = ((ux >> 9) ^ (uy >> 3)) & 1;
    uint32_t bank = bankBit0 | (bankBit1 << 1) | (bankBit2 << 2) | (bankBit3 << 3);

    uint32_t mtIdx = mtY * mtRow + (ux >> 7);

    return (mtIdx << 13) | (bank << 9) | (pipe << 6) | pix_y;
}

/* ─── Drawing Primitives ─────────────────────────────────────────── */

void overlay_draw_rect(uint32_t *fb, uint32_t pitch,
                       int x, int y, int w, int h, uint32_t color)
{
    (void)pitch;

    /* Clamp to screen bounds once */
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w;
    int y1 = y + h;
    if ((uint32_t)x1 > g_overlay.width)  x1 = (int)g_overlay.width;
    if ((uint32_t)y1 > g_overlay.height) y1 = (int)g_overlay.height;
    if (x0 >= x1 || y0 >= y1) return;

    if (g_overlay.tiling_mode != ORBIS_VIDEO_OUT_TILING_MODE_TILE) {
        /* Linear mode — direct row writes */
        uint32_t p = g_overlay.pitch;
        for (int row = y0; row < y1; row++) {
            uint32_t *dst = fb + (uint32_t)row * p + (uint32_t)x0;
            for (int col = x0; col < x1; col++) *dst++ = color;
        }
        return;
    }

    /* Tiled mode — process in 8-pixel aligned spans.
     * Within each span, pipe/bank/macro-tile are constant,
     * so we compute tiling once and write 8 pixels directly. */
    uint32_t mtRow = g_overlay.pitch >> 7;

    for (int row = y0; row < y1; row++) {
        uint32_t uy = (uint32_t)row;
        uint32_t ly = uy & 7;
        uint32_t pix_y = ((ly & 1) << 2) | ((ly & 2) << 3) | ((ly & 4) << 3);
        uint32_t mtY = uy >> 6;

        int col = x0;

        /* Unaligned start — per-pixel */
        while (col < x1 && (col & 7) != 0) {
            overlay_put_pixel(fb, col, row, color);
            col++;
        }

        /* Aligned 8-pixel spans — batch tiling */
        while (col + 8 <= x1) {
            uint32_t base = tile_span_offset((uint32_t)col, uy, pix_y, mtY, mtRow);
            fb[base + 0] = color;
            fb[base + 1] = color;
            fb[base + 2] = color;
            fb[base + 3] = color;
            fb[base + 8] = color;
            fb[base + 9] = color;
            fb[base + 10] = color;
            fb[base + 11] = color;
            col += 8;
        }

        /* Unaligned end — per-pixel */
        while (col < x1) {
            overlay_put_pixel(fb, col, row, color);
            col++;
        }
    }
}

void overlay_draw_char(uint32_t *fb, uint32_t pitch,
                       int x, int y, char ch, uint32_t fg, uint32_t bg)
{
    (void)pitch;
    uint8_t idx = (uint8_t)ch;
    if (idx > 127) idx = '?';

    const uint8_t *glyph = font8x8_basic[idx];

    for (int row = 0; row < 8; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            /* MSB = leftmost pixel */
            uint32_t color = (bits & (0x80 >> col)) ? fg : bg;
            overlay_put_pixel(fb, x + col, y + row, color);
        }
    }
}

void overlay_draw_text(uint32_t *fb, uint32_t pitch,
                       int x, int y, const char *str,
                       uint32_t fg, uint32_t bg)
{
    (void)pitch;
    if (!str) return;
    int cx = x;
    while (*str) {
        overlay_draw_char(fb, pitch, cx, y, *str, fg, bg);
        cx += 8;
        str++;
    }
}

void overlay_draw_rect_alpha(uint32_t *fb, uint32_t pitch,
                              int x, int y, int w, int h,
                              uint32_t color, uint8_t alpha)
{
    (void)pitch;
    /* In opaque mode (force_draw), skip alpha blending to prevent compounding */
    if (alpha == 255 || g_force_opaque) {
        overlay_draw_rect(fb, pitch, x, y, w, h, color);
        return;
    }
    for (int row = y; row < y + h; row++) {
        for (int col = x; col < x + w; col++) {
            overlay_blend_pixel(fb, col, row, color, alpha);
        }
    }
}

void overlay_put_pixel_ext(uint32_t *fb, int x, int y, uint32_t color)
{
    overlay_put_pixel(fb, x, y, color);
}

void overlay_draw_char_2x(uint32_t *fb, uint32_t pitch,
                          int x, int y, char ch, uint32_t fg, uint32_t bg)
{
    (void)pitch;
    uint8_t idx = (uint8_t)ch;
    if (idx > 127) idx = '?';

    const uint8_t *glyph = font8x8_basic[idx];

    /* 2x glyph = 16×16 pixels. Each glyph row produces 2 screen rows
     * of 16 pixels each. If 8-pixel aligned, we can use fast tiled spans.
     * Each 16-pixel row = two 8-pixel spans. */
    bool use_fast = (g_overlay.tiling_mode == ORBIS_VIDEO_OUT_TILING_MODE_TILE)
                 && (x & 7) == 0
                 && x >= 0 && y >= 0
                 && (uint32_t)(x + 16) <= g_overlay.width;

    if (use_fast) {
        uint32_t mtRow = g_overlay.pitch >> 7;
        uint32_t ux0 = (uint32_t)x;

        for (int grow = 0; grow < 8; grow++) {
            uint8_t bits = glyph[grow];
            /* Pre-compute 8 colors for this glyph row */
            uint32_t c[8];
            for (int gc = 0; gc < 8; gc++)
                c[gc] = (bits & (0x80 >> gc)) ? fg : bg;

            for (int dy = 0; dy < 2; dy++) {
                int py = y + grow * 2 + dy;
                if (py < 0 || (uint32_t)py >= g_overlay.height) continue;
                uint32_t uy = (uint32_t)py;
                uint32_t ly = uy & 7;
                uint32_t pix_y = ((ly & 1) << 2) | ((ly & 2) << 3) | ((ly & 4) << 3);
                uint32_t mtY = uy >> 6;

                /* Span 0: pixels 0-7 (glyph cols 0-3, each doubled) */
                uint32_t b0 = tile_span_offset(ux0, uy, pix_y, mtY, mtRow);
                fb[b0 + 0] = c[0]; fb[b0 + 1] = c[0];
                fb[b0 + 2] = c[1]; fb[b0 + 3] = c[1];
                fb[b0 + 8] = c[2]; fb[b0 + 9] = c[2];
                fb[b0 + 10] = c[3]; fb[b0 + 11] = c[3];

                /* Span 1: pixels 8-15 (glyph cols 4-7, each doubled) */
                uint32_t b1 = tile_span_offset(ux0 + 8, uy, pix_y, mtY, mtRow);
                fb[b1 + 0] = c[4]; fb[b1 + 1] = c[4];
                fb[b1 + 2] = c[5]; fb[b1 + 3] = c[5];
                fb[b1 + 8] = c[6]; fb[b1 + 9] = c[6];
                fb[b1 + 10] = c[7]; fb[b1 + 11] = c[7];
            }
        }
    } else {
        /* Fallback: per-pixel (unaligned or linear mode) */
        for (int row = 0; row < 8; row++) {
            uint8_t bits = glyph[row];
            for (int col = 0; col < 8; col++) {
                uint32_t color = (bits & (0x80 >> col)) ? fg : bg;
                int px = x + col * 2;
                int py = y + row * 2;
                overlay_put_pixel(fb, px,     py,     color);
                overlay_put_pixel(fb, px + 1, py,     color);
                overlay_put_pixel(fb, px,     py + 1, color);
                overlay_put_pixel(fb, px + 1, py + 1, color);
            }
        }
    }
}

void overlay_draw_text_2x(uint32_t *fb, uint32_t pitch,
                          int x, int y, const char *str,
                          uint32_t fg, uint32_t bg)
{
    (void)pitch;
    if (!str) return;
    int cx = x;
    while (*str) {
        overlay_draw_char_2x(fb, pitch, cx, y, *str, fg, bg);
        cx += 16;
        str++;
    }
}

/* ─── Hook Installation ──────────────────────────────────────────── */

int32_t overlay_init(void) {
    if (g_overlay.initialized) {
        LOG_WARN("Overlay already initialized");
        return IME_OK;
    }

    LOG_INFO("Installing VideoOut overlay hooks...");

    memset(&g_overlay, 0, sizeof(g_overlay));

    void *addr_register = NULL;
    void *addr_flip     = NULL;

    /*
     * Strategy 1: Load module directly (works if not yet loaded,
     * or returns handle for already-loaded modules on some FW).
     */
    int mod_handle = sceKernelLoadStartModule(
        "libSceVideoOut.sprx", 0, NULL, 0, NULL, NULL);

    if (mod_handle >= 0) {
        LOG_INFO("libSceVideoOut.sprx handle: 0x%08X (LoadStartModule)", mod_handle);
        sceKernelDlsym(mod_handle, "sceVideoOutRegisterBuffers", &addr_register);
        sceKernelDlsym(mod_handle, "sceVideoOutSubmitFlip", &addr_flip);
    } else {
        LOG_WARN("LoadStartModule returned 0x%08X", mod_handle);
    }

    /*
     * Strategy 2: If direct load failed, scan loaded modules to find
     * libSceVideoOut by name and resolve from there.
     */
    if (!addr_register || !addr_flip) {
        LOG_INFO("Trying module list scan...");
        int32_t found_handle = find_module_handle("libSceVideoOut");
        if (found_handle >= 0) {
            if (!addr_register)
                sceKernelDlsym(found_handle, "sceVideoOutRegisterBuffers", &addr_register);
            if (!addr_flip)
                sceKernelDlsym(found_handle, "sceVideoOutSubmitFlip", &addr_flip);
        }
    }

    /*
     * Strategy 3: Try with full filesystem path.
     */
    if (!addr_register || !addr_flip) {
        LOG_INFO("Trying full path load...");
        mod_handle = sceKernelLoadStartModule(
            "/system/common/lib/libSceVideoOut.sprx", 0, NULL, 0, NULL, NULL);
        if (mod_handle >= 0) {
            LOG_INFO("Full path handle: 0x%08X", mod_handle);
            if (!addr_register)
                sceKernelDlsym(mod_handle, "sceVideoOutRegisterBuffers", &addr_register);
            if (!addr_flip)
                sceKernelDlsym(mod_handle, "sceVideoOutSubmitFlip", &addr_flip);
        } else {
            LOG_WARN("Full path load returned 0x%08X", mod_handle);
        }
    }

    LOG_INFO("Resolved: RegisterBuffers=%p SubmitFlip=%p",
        addr_register, addr_flip);

    if (addr_register) {
        Detour_Construct(&g_hook_register_buffers, DetourMode_x64);
        Detour_DetourFunction(&g_hook_register_buffers,
            (uint64_t)addr_register, hooked_register_buffers);
        g_orig_register_buffers =
            (sceVideoOutRegisterBuffers_t)g_hook_register_buffers.StubPtr;
        LOG_INFO("Hooked sceVideoOutRegisterBuffers @ %p", addr_register);
    } else {
        LOG_ERROR("Failed to resolve sceVideoOutRegisterBuffers");
    }

    if (addr_flip) {
        Detour_Construct(&g_hook_submit_flip, DetourMode_x64);
        Detour_DetourFunction(&g_hook_submit_flip,
            (uint64_t)addr_flip, hooked_submit_flip);
        g_orig_submit_flip =
            (sceVideoOutSubmitFlip_t)g_hook_submit_flip.StubPtr;
        LOG_INFO("Hooked sceVideoOutSubmitFlip @ %p", addr_flip);
    } else {
        LOG_ERROR("Failed to resolve sceVideoOutSubmitFlip");
    }

    g_overlay.hooks_installed = (addr_register != NULL && addr_flip != NULL);
    g_overlay.initialized = true;

    if (g_overlay.hooks_installed) {
        LOG_INFO("Overlay hooks installed - waiting for RegisterBuffers call");
    } else {
        LOG_WARN("Overlay hooks INCOMPLETE - overlay will not render");
    }

    return IME_OK;
}

void overlay_cleanup(void) {
    if (!g_overlay.initialized) {
        return;
    }

    LOG_INFO("Removing VideoOut overlay hooks...");

    g_draw_callback = NULL;

    if (g_overlay.hooks_installed) {
        if (g_orig_register_buffers) {
            Detour_RestoreFunction(&g_hook_register_buffers);
            Detour_Destroy(&g_hook_register_buffers);
        }
        if (g_orig_submit_flip) {
            Detour_RestoreFunction(&g_hook_submit_flip);
            Detour_Destroy(&g_hook_submit_flip);
        }
    }

    memset(&g_overlay, 0, sizeof(g_overlay));
    g_orig_register_buffers = NULL;
    g_orig_submit_flip      = NULL;

    LOG_INFO("Overlay hooks removed");
}

void overlay_set_draw_callback(overlay_draw_cb_t cb) {
    g_draw_callback = cb;
}

bool overlay_is_active(void) {
    return g_overlay.hooks_installed && g_overlay.buffer_count > 0;
}

int32_t overlay_get_tiling_mode(void) {
    return g_overlay.tiling_mode;
}

bool overlay_is_flipping(void) {
    if (!g_overlay.hooks_installed || g_overlay.buffer_count == 0)
        return false;
    uint64_t now = sceKernelGetProcessTime();
    /* Consider the game "flipping" if a submitFlip happened within 100ms */
    return (now - g_last_flip_us) < 100000;
}

void overlay_force_draw(overlay_draw_cb_t cb) {
    if (!cb || g_overlay.width == 0 || g_overlay.buffer_count == 0) return;

    uint64_t fd_start = sceKernelGetProcessTime();  /* PERF */
    uint32_t buf_drawn = 0;

    /* Enable opaque mode — prevents alpha compounding when
     * re-drawing to buffers the game hasn't re-rendered. */
    g_force_opaque = true;

    for (int i = 0; i < ABS_MAX_BUFFERS; i++) {
        if (g_overlay.buffers[i]) {
            uint32_t *fb = (uint32_t *)g_overlay.buffers[i];
            cb(fb, g_overlay.pitch, g_overlay.width, g_overlay.height);
            buf_drawn++;
        }
    }

    g_force_opaque = false;

    /* PERF: accumulate force_draw stats */
    uint64_t fd_us = sceKernelGetProcessTime() - fd_start;
    g_fd_perf_call_count++;
    g_fd_perf_buf_total += buf_drawn;
    g_fd_perf_total_us += fd_us;
    if (fd_us > g_fd_perf_max_us) g_fd_perf_max_us = fd_us;

    uint64_t now = sceKernelGetProcessTime();
    if (now - g_fd_perf_last_log_us >= 1000000) {
        printf("[CIME] FDRAW: calls/s=%u  bufs/call=%u  avg=%luus  max=%luus  total_buf=%u\n",
            g_fd_perf_call_count,
            g_fd_perf_call_count ? g_fd_perf_buf_total / g_fd_perf_call_count : 0,
            (unsigned long)(g_fd_perf_call_count ? g_fd_perf_total_us / g_fd_perf_call_count : 0),
            (unsigned long)g_fd_perf_max_us,
            g_fd_perf_buf_total);
        g_fd_perf_last_log_us  = now;
        g_fd_perf_call_count   = 0;
        g_fd_perf_buf_total    = 0;
        g_fd_perf_total_us     = 0;
        g_fd_perf_max_us       = 0;
    }
}

/* Rotation index for single-buffer drawing */
static int g_force_draw_next = 0;

void overlay_force_draw_single(overlay_draw_cb_t cb) {
    if (!cb || g_overlay.width == 0 || g_overlay.buffer_count == 0) return;

    /* Find the next valid buffer starting from our rotation index */
    for (int attempt = 0; attempt < ABS_MAX_BUFFERS; attempt++) {
        int idx = (g_force_draw_next + attempt) % ABS_MAX_BUFFERS;
        if (g_overlay.buffers[idx]) {
            g_force_opaque = true;
            uint32_t *fb = (uint32_t *)g_overlay.buffers[idx];
            cb(fb, g_overlay.pitch, g_overlay.width, g_overlay.height);
            g_force_opaque = false;
            g_force_draw_next = (idx + 1) % ABS_MAX_BUFFERS;
            return;
        }
    }
}

void overlay_draw_last_flipped(overlay_draw_cb_t cb) {
    if (!cb || g_overlay.width == 0) return;
    int32_t idx = g_last_flipped_idx;
    if (idx < 0 || idx >= ABS_MAX_BUFFERS || !g_overlay.buffers[idx]) return;

    g_force_opaque = true;
    uint32_t *fb = (uint32_t *)g_overlay.buffers[idx];
    cb(fb, g_overlay.pitch, g_overlay.width, g_overlay.height);
    g_force_opaque = false;
}
