/* Panda: loops the embedded panda.gif endlessly and counts completed plays
 * until the app is closed. Uses a self-contained GIF89a decoder backed by
 * PSRAM (port_big_alloc) so LVGL's internal heap stays untouched. */
#include "app.h"
#include "assets/assets.h"
#include "port.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define TICK_MS       50
#define LZW_MAX_CODES 4096

typedef struct {
    const uint8_t *data;
    uint32_t       size;
    uint32_t       pos;
    uint32_t       frames_off;

    uint16_t       w;
    uint16_t       h;
    uint16_t       bg_rgb565;
    uint32_t       bg_hex;
    uint16_t       gct[256];
    uint16_t       lct[256];

    /* Previous frame rect & disposal state */
    uint8_t        prev_disp;
    uint16_t       prev_x, prev_y, prev_w, prev_h;

    /* Pending Graphic Control Extension for the next image block */
    uint8_t        gce_disp;
    bool           gce_has_trans;
    uint8_t        gce_trans_idx;
    uint16_t       gce_delay_ms;

    /* LZW decode tables (live in PSRAM inside this struct, never in .bss) */
    uint16_t       lzw_prefix[LZW_MAX_CODES];
    uint8_t        lzw_suffix[LZW_MAX_CODES];
    uint8_t        lzw_stack[LZW_MAX_CODES];
} gif_dec_t;

static gif_dec_t  *s_dec;
static uint16_t   *s_buf;
static lv_obj_t   *s_canvas;
static lv_obj_t   *s_cnt_lbl;
static lv_timer_t *s_timer;

static uint32_t    s_play_count;
static uint32_t    s_elapsed_ms;
static uint32_t    s_frame_delay_ms;

static inline uint16_t rd_u16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint16_t rgb_to_565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((uint16_t)(r >> 3) << 11) |
                      ((uint16_t)(g >> 2) << 5)  |
                      (uint16_t)(b >> 3));
}

static void skip_subblocks(gif_dec_t *g)
{
    while (g->pos < g->size) {
        uint8_t n = g->data[g->pos++];
        if (n == 0) break;
        if (g->pos + n > g->size) {
            g->pos = g->size;
            break;
        }
        g->pos += n;
    }
}

static void fill_rect(uint16_t *fb, uint16_t fw, uint16_t fh,
                      uint16_t rx, uint16_t ry, uint16_t rw, uint16_t rh,
                      uint16_t col)
{
    uint32_t y1 = (uint32_t)ry + rh;
    uint32_t x1 = (uint32_t)rx + rw;
    if (y1 > fh) y1 = fh;
    if (x1 > fw) x1 = fw;
    for (uint32_t y = ry; y < y1; y++) {
        uint16_t *row = fb + y * fw;
        for (uint32_t x = rx; x < x1; x++) row[x] = col;
    }
}

static bool gif_init(gif_dec_t *g, const uint8_t *data, uint32_t size)
{
    memset(g, 0, sizeof(*g));
    if (!data || size < 13) return false;
    if (memcmp(data, "GIF87a", 6) != 0 && memcmp(data, "GIF89a", 6) != 0)
        return false;

    g->data = data;
    g->size = size;
    g->w = rd_u16(data + 6);
    g->h = rd_u16(data + 8);
    if (g->w == 0 || g->h == 0) return false;

    uint8_t packed = data[10];
    uint8_t bg_idx = data[11];
    g->pos = 13;

    g->bg_hex = 0xA599C6;
    g->bg_rgb565 = rgb_to_565(0xA5, 0x99, 0xC6);

    if (packed & 0x80) {
        uint32_t n = 1u << ((packed & 0x07) + 1);
        if (g->pos + n * 3u > g->size) return false;
        for (uint32_t i = 0; i < n; i++) {
            uint8_t r = g->data[g->pos++];
            uint8_t gr = g->data[g->pos++];
            uint8_t b = g->data[g->pos++];
            g->gct[i] = rgb_to_565(r, gr, b);
            if (i == bg_idx) {
                g->bg_hex = ((uint32_t)r << 16) | ((uint32_t)gr << 8) | b;
                g->bg_rgb565 = g->gct[i];
            }
        }
    }

    g->frames_off = g->pos;
    g->gce_delay_ms = 100;
    return true;
}

static void gif_rewind(gif_dec_t *g, uint16_t *fb)
{
    g->pos = g->frames_off;
    g->prev_disp = 0;
    g->prev_x = g->prev_y = g->prev_w = g->prev_h = 0;
    g->gce_disp = 0;
    g->gce_has_trans = false;
    g->gce_trans_idx = 0;
    g->gce_delay_ms = 100;
    if (fb) fill_rect(fb, g->w, g->h, 0, 0, g->w, g->h, g->bg_rgb565);
}

/* Decode the next GIF frame into RGB565 framebuffer `fb`.
 * Returns 1 if a frame was decoded (setting *out_delay_ms), or 0 at end-of-GIF. */
static int gif_decode_next(gif_dec_t *g, uint16_t *fb, uint32_t *out_delay_ms)
{
    while (g->pos < g->size) {
        uint8_t intro = g->data[g->pos++];

        if (intro == 0x3B) {
            return 0; /* GIF trailer */
        }

        if (intro == 0x21) {
            if (g->pos >= g->size) return 0;
            uint8_t label = g->data[g->pos++];
            if (label == 0xF9) {
                if (g->pos >= g->size) return 0;
                uint8_t bsz = g->data[g->pos++];
                if (bsz >= 4 && g->pos + bsz <= g->size) {
                    uint8_t flags = g->data[g->pos];
                    g->gce_disp = (flags >> 2) & 0x07;
                    g->gce_has_trans = (flags & 0x01) != 0;
                    uint16_t cs = rd_u16(g->data + g->pos + 1);
                    g->gce_delay_ms = (cs > 0) ? (uint16_t)(cs * 10u) : 100u;
                    g->gce_trans_idx = g->data[g->pos + 3];
                }
                g->pos += bsz;
                skip_subblocks(g);
            } else {
                skip_subblocks(g);
            }
            continue;
        }

        if (intro != 0x2C) {
            continue;
        }

        /* Image Descriptor (9 bytes) */
        if (g->pos + 9u > g->size) return 0;
        uint16_t ix = rd_u16(g->data + g->pos + 0);
        uint16_t iy = rd_u16(g->data + g->pos + 2);
        uint16_t iw = rd_u16(g->data + g->pos + 4);
        uint16_t ih = rd_u16(g->data + g->pos + 6);
        uint8_t  packed = g->data[g->pos + 8];
        g->pos += 9u;

        /* Apply previous frame's disposal method before drawing this frame */
        if (g->prev_disp == 2) {
            fill_rect(fb, g->w, g->h, g->prev_x, g->prev_y,
                      g->prev_w, g->prev_h, g->bg_rgb565);
        }

        const uint16_t *pal = g->gct;
        if (packed & 0x80) {
            uint32_t n = 1u << ((packed & 0x07) + 1);
            if (g->pos + n * 3u > g->size) return 0;
            for (uint32_t i = 0; i < n; i++) {
                uint8_t r = g->data[g->pos++];
                uint8_t gr = g->data[g->pos++];
                uint8_t b = g->data[g->pos++];
                g->lct[i] = rgb_to_565(r, gr, b);
            }
            pal = g->lct;
        }

        if (g->pos >= g->size) return 0;
        uint8_t min_cs = g->data[g->pos++];
        if (min_cs < 2 || min_cs > 8) {
            skip_subblocks(g);
            return 0;
        }

        bool interlaced = (packed & 0x40) != 0;
        uint32_t clear_code = 1u << min_cs;
        uint32_t eoi_code   = clear_code + 1u;
        uint32_t code_size  = (uint32_t)min_cs + 1u;
        uint32_t next_code  = eoi_code + 1u;
        int32_t  prev_code  = -1;
        uint8_t  first_char = 0;

        /* Sub-block bit stream state */
        uint32_t bit_buf = 0;
        uint32_t bit_cnt = 0;
        uint8_t  blk_rem = 0;
        bool     sub_eof = false;

        /* Destination cursor inside (ix, iy, iw, ih) */
        uint16_t px = 0, py = 0;
        static const uint8_t IL_START[4] = { 0, 4, 2, 1 };
        static const uint8_t IL_STEP[4]  = { 8, 8, 4, 2 };
        int il_pass = 0;

        while (true) {
            while (bit_cnt < code_size) {
                if (blk_rem == 0) {
                    if (g->pos >= g->size) { sub_eof = true; break; }
                    blk_rem = g->data[g->pos++];
                    if (blk_rem == 0) { sub_eof = true; break; }
                }
                if (g->pos >= g->size) { sub_eof = true; break; }
                bit_buf |= ((uint32_t)g->data[g->pos++] << bit_cnt);
                bit_cnt += 8u;
                blk_rem--;
            }
            if (sub_eof && bit_cnt < code_size) break;

            uint32_t code = bit_buf & ((1u << code_size) - 1u);
            bit_buf >>= code_size;
            bit_cnt -= code_size;

            if (code == clear_code) {
                code_size = (uint32_t)min_cs + 1u;
                next_code = eoi_code + 1u;
                prev_code = -1;
                continue;
            }
            if (code == eoi_code) break;

            uint32_t sp = 0;
            uint32_t cur = code;
            if (prev_code < 0) {
                if (code >= clear_code) break;
                first_char = (uint8_t)code;
                g->lzw_stack[sp++] = first_char;
                prev_code = (int32_t)code;
            } else {
                if (cur >= next_code) {
                    if (cur > next_code) break;
                    g->lzw_stack[sp++] = first_char;
                    cur = (uint32_t)prev_code;
                }
                while (cur >= clear_code && sp < LZW_MAX_CODES) {
                    g->lzw_stack[sp++] = g->lzw_suffix[cur];
                    cur = g->lzw_prefix[cur];
                }
                first_char = (uint8_t)cur;
                if (sp < LZW_MAX_CODES) g->lzw_stack[sp++] = first_char;

                if (next_code < LZW_MAX_CODES) {
                    g->lzw_prefix[next_code] = (uint16_t)prev_code;
                    g->lzw_suffix[next_code] = first_char;
                    next_code++;
                    if (next_code == (1u << code_size) && code_size < 12u)
                        code_size++;
                }
                prev_code = (int32_t)code;
            }

            /* Blit decoded pixel run directly into the RGB565 canvas */
            while (sp > 0 && py < ih) {
                uint8_t idx = g->lzw_stack[--sp];
                if (!g->gce_has_trans || idx != g->gce_trans_idx) {
                    uint32_t dx = (uint32_t)ix + px;
                    uint32_t dy = (uint32_t)iy + py;
                    if (dx < g->w && dy < g->h) {
                        fb[dy * g->w + dx] = pal[idx];
                    }
                }
                if (++px >= iw) {
                    px = 0;
                    if (!interlaced) {
                        py++;
                    } else {
                        py = (uint16_t)(py + IL_STEP[il_pass]);
                        while (py >= ih && il_pass < 3) {
                            il_pass++;
                            py = IL_START[il_pass];
                        }
                    }
                }
            }
        }

        /* Drain remaining bytes in the current sub-block and any trailing ones */
        if (!sub_eof) {
            if (blk_rem > 0) {
                g->pos = (g->pos + blk_rem <= g->size) ? (g->pos + blk_rem) : g->size;
            }
            skip_subblocks(g);
        }

        if (out_delay_ms) *out_delay_ms = g->gce_delay_ms;

        g->prev_disp = g->gce_disp;
        g->prev_x = ix; g->prev_y = iy; g->prev_w = iw; g->prev_h = ih;

        g->gce_disp = 0;
        g->gce_has_trans = false;
        g->gce_trans_idx = 0;
        g->gce_delay_ms = 100;
        return 1;
    }
    return 0;
}

static void update_counter_label(void)
{
    if (s_cnt_lbl) {
        lv_label_set_text_fmt(s_cnt_lbl, "Plays: %lu", (unsigned long)s_play_count);
    }
}

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    /* Skip work while display is off — never call lv_timer_set_period() here */
    if (launcher_screen_is_off()) return;
    if (!s_dec || !s_buf || !s_canvas) return;

    s_elapsed_ms += TICK_MS;
    if (s_elapsed_ms < s_frame_delay_ms) return;
    s_elapsed_ms -= s_frame_delay_ms;
    if (s_elapsed_ms > s_frame_delay_ms) s_elapsed_ms = 0;

    uint32_t delay = 100;
    int got = gif_decode_next(s_dec, s_buf, &delay);
    if (got == 0) {
        s_play_count++;
        update_counter_label();
        gif_rewind(s_dec, s_buf);
        if (gif_decode_next(s_dec, s_buf, &delay) == 0) return;
    }
    s_frame_delay_ms = (delay < TICK_MS) ? TICK_MS : delay;
    lv_obj_invalidate(s_canvas);
}

static void enter(lv_obj_t *root)
{
    port_crumb(CRUMB_GAME);
    s_play_count = 0;
    s_elapsed_ms = 0;
    s_frame_delay_ms = 100;

    s_dec = (gif_dec_t *)port_big_alloc(sizeof(gif_dec_t));
    if (!s_dec || !gif_init(s_dec, panda_gif_data, panda_gif_size)) {
        if (s_dec) { port_big_free(s_dec); s_dec = NULL; }
        lv_obj_t *err = lv_label_create(root);
        lv_label_set_text(err, "GIF load error");
        lv_obj_set_style_text_color(err, lv_color_white(), 0);
        lv_obj_center(err);
        return;
    }

    size_t fb_bytes = (size_t)s_dec->w * s_dec->h * sizeof(uint16_t);
    s_buf = (uint16_t *)port_big_alloc(fb_bytes);
    if (!s_buf) {
        port_big_free(s_dec); s_dec = NULL;
        lv_obj_t *err = lv_label_create(root);
        lv_label_set_text(err, "Out of memory");
        lv_obj_set_style_text_color(err, lv_color_white(), 0);
        lv_obj_center(err);
        return;
    }

    /* Decode frame 0 first so we can match the screen background to pixel (0,0) */
    gif_rewind(s_dec, s_buf);
    uint32_t delay = 100;
    if (gif_decode_next(s_dec, s_buf, &delay) > 0) {
        s_frame_delay_ms = (delay < TICK_MS) ? TICK_MS : delay;
    }

    /* Convert corner RGB565 pixel to 24-bit color for a seamless round screen */
    uint16_t c0 = s_buf[0];
    uint8_t br = (uint8_t)(((c0 >> 11) & 0x1F) * 255u / 31u);
    uint8_t bg_g = (uint8_t)(((c0 >> 5) & 0x3F) * 255u / 63u);
    uint8_t bb = (uint8_t)((c0 & 0x1F) * 255u / 31u);

    lv_obj_t *bg = lv_obj_create(root);
    lv_obj_remove_style_all(bg);
    lv_obj_set_size(bg, 466, 466);
    lv_obj_center(bg);
    lv_obj_set_style_bg_color(bg, lv_color_make(br, bg_g, bb), 0);
    lv_obj_set_style_bg_opa(bg, LV_OPA_COVER, 0);

    s_canvas = lv_canvas_create(bg);
    lv_canvas_set_buffer(s_canvas, s_buf, s_dec->w, s_dec->h, LV_COLOR_FORMAT_RGB565);
    lv_image_set_scale(s_canvas, 320); /* 1.25x: 220x198 -> 275x248 on 466px screen */
    lv_obj_align(s_canvas, LV_ALIGN_CENTER, 0, -24);
    lv_obj_remove_flag(s_canvas, LV_OBJ_FLAG_CLICKABLE);

    /* Play-count pill cleanly above the bottom home handle zone */
    lv_obj_t *pill = lv_obj_create(bg);
    lv_obj_remove_style_all(pill);
    lv_obj_set_size(pill, 168, 42);
    lv_obj_set_style_radius(pill, 21, 0);
    lv_obj_set_style_bg_color(pill, lv_color_hex(0x2B2440), 0);
    lv_obj_set_style_bg_opa(pill, 225, 0);
    lv_obj_set_style_border_color(pill, lv_color_hex(0xF6D365), 0);
    lv_obj_set_style_border_width(pill, 2, 0);
    lv_obj_align(pill, LV_ALIGN_CENTER, 0, 136);
    lv_obj_remove_flag(pill, LV_OBJ_FLAG_CLICKABLE);

    s_cnt_lbl = lv_label_create(pill);
    lv_obj_set_style_text_font(s_cnt_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_cnt_lbl, lv_color_hex(0xFFF8E7), 0);
    lv_obj_center(s_cnt_lbl);
    update_counter_label();

    s_timer = lv_timer_create(tick_cb, TICK_MS, NULL);
}

static void leave(void)
{
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    /* Always delete the canvas BEFORE freeing its backing buffer */
    if (s_canvas) {
        if (lv_obj_is_valid(s_canvas)) lv_obj_delete(s_canvas);
        s_canvas = NULL;
    }
    if (s_buf) {
        port_big_free(s_buf);
        s_buf = NULL;
    }
    if (s_dec) {
        port_big_free(s_dec);
        s_dec = NULL;
    }
    s_cnt_lbl = NULL;
}

static lv_color_t tint(void) { return lv_color_hex(0xF6D365); }

const badge_app_t app_panda = {
    .name       = "Panda",
    .art        = &app_icon_panda,
    .icon       = LV_SYMBOL_PLAY,
    .tint       = tint,
    .radio      = RADIO_OFF,
    .keep_awake = true,
    .enter      = enter,
    .leave      = leave,
};
