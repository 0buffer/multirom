/*
 * This file is part of MultiROM.
 *
 * MultiROM is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * MultiROM is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with MultiROM.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef H_FRAMEBUFFER
#define H_FRAMEBUFFER

#include <linux/fb.h>
#include <stdarg.h>
#include <pthread.h>

#if defined(RECOVERY_BGRA) || defined(RECOVERY_RGBX)
#define PIXEL_SIZE 4
typedef uint32_t px_type;
#else
#define PIXEL_SIZE 2
#ifndef RECOVERY_RGB_565
  #define RECOVERY_RGB_565
#endif
typedef uint16_t px_type;
#endif

#ifdef RECOVERY_BGRA
#define PX_IDX_A 0
#define PX_IDX_R 1
#define PX_IDX_G 2
#define PX_IDX_B 3
#define PX_GET_R(px) ((px & 0xFF00) >> 8)
#define PX_GET_G(px) ((px & 0xFF0000) >> 16)
#define PX_GET_B(px) ((px & 0xFF000000) >> 24)
#define PX_GET_A(px) (px & 0xFF)
#elif defined(RECOVERY_RGBX)
#define PX_IDX_A 3
#define PX_IDX_R 0
#define PX_IDX_G 1
#define PX_IDX_B 2
#define PX_GET_R(px) (px & 0xFF)
#define PX_GET_G(px) ((px & 0xFF00) >> 8)
#define PX_GET_B(px) ((px & 0xFF0000) >> 16)
#define PX_GET_A(px) ((px & 0xFF000000) >> 24)
#elif defined(RECOVERY_RGB_565)
#define PX_GET_R(px) (((((px & 0x1F)*100)/31)*0xFF)/100)
#define PX_GET_G(px) ((((((px & 0x7E0) >> 5)*100)/63)*0xFF)/100)
#define PX_GET_B(px) ((((((px & 0xF800) >> 11)*100)/31)*0xFF)/100)
#define PX_GET_A(px) (0xFF)
#endif

struct framebuffer {
    px_type *buffer;
    uint32_t size;
    uint32_t stride;
    int fd;
    struct fb_fix_screeninfo fi;
    struct fb_var_screeninfo vi;
    struct fb_impl *impl;
    void *impl_data;
};

struct fb_impl {
    const char *name;
    const int impl_id;

    int (*open)(struct framebuffer *fb);
    void (*close)(struct framebuffer *fb);
    int (*update)(struct framebuffer *fb);
    void *(*get_frame_dest)(struct framebuffer *fb);
};

enum
{
#ifdef MR_USE_QCOM_OVERLAY
    FB_IMPL_QCOM_OVERLAY,
#endif

    FB_IMPL_GENERIC, // must be last

    FB_IMPL_CNT
};

// Colors, 0xAABBGGRR
#define BLACK     0xFF000000
#define WHITE     0xFFFFFFFF
#define LBLUE     0xFFCC9900
#define LBLUE2    0xFFF4DFA8
#define GRAYISH   0xFFBEBEBE
#define GRAY      0xFF7F7F7F
#define DRED      0xFF0000CC

enum
{
    SIZE_SMALL     = 6,
    SIZE_NORMAL    = 10,
    SIZE_BIG       = 13,
    SIZE_EXTRA     = 15,
};

extern uint32_t fb_width;
extern uint32_t fb_height;
extern int fb_rotation;

int fb_open(int rotation);
int fb_open_impl(void);
void fb_close(void);
void fb_update(void);
void fb_dump_info(void);
int fb_get_vi_xres(void);
int fb_get_vi_yres(void);
void fb_force_generic_impl(int force);

enum 
{
    FB_IT_RECT,
    FB_IT_BOX,
    FB_IT_IMG,
};

enum
{
    FB_IMG_TYPE_GENERIC,
    FB_IMG_TYPE_PNG,
    FB_IMG_TYPE_TEXT,
};

enum
{
    JUSTIFY_LEFT,
    JUSTIFY_CENTER,
    JUSTIFY_RIGHT,
};

enum
{
    LEVEL_RECT = 0,
    LEVEL_PNG  = 1,
    LEVEL_TEXT = 2,
};

struct fb_item_header;

#define FB_ITEM_POS \
    int x, y; \
    int w, h;

typedef struct 
{
    FB_ITEM_POS
} fb_item_pos;

extern fb_item_pos DEFAULT_FB_PARENT;

#define FB_ITEM_HEAD \
    FB_ITEM_POS \
    int id; \
    int type; \
    int level; \
    fb_item_pos *parent; \
    struct fb_item_header *prev; \
    struct fb_item_header *next;

struct fb_item_header
{
    FB_ITEM_HEAD
};
typedef struct fb_item_header fb_item_header;

typedef struct
{
    FB_ITEM_HEAD

    uint32_t color;
} fb_rect;

/*
 * fb_img element draws pre-rendered image data, which can come for
 * example from a PNG file.
 * For RECOVERY_BGRA and RECOVERY_BGRX (4 bytes per px), data is just
 * array of pixels in selected px format.
 * For RECOVERY_RGB_565 (2 bytes per px), another 2 bytes with
 * alpha values are added after each pixel. So, one pixel is two uint16_t
 * entries in the result uint16_t array:
 * [0]: (R | (G << 5) | (B << 11))
 * [1]: (alphaForRB | (alphaForG << 8))
 * [2]: (R | (G << 5) | (B << 11))
 * [3]: (alphaForRB | (alphaForG << 8))
 * ...
 */
typedef struct
{
    FB_ITEM_HEAD

    int img_type;
    px_type *data;
    void *extra;
} fb_img;

typedef fb_img fb_text;

typedef struct
{
    FB_ITEM_HEAD

    fb_img **imgs;
    fb_rect *background[3];
} fb_msgbox;

typedef struct
{
    fb_item_header *first_item;
    fb_msgbox *msgbox;
    pthread_mutex_t mutex;
} fb_context_t;

void fb_remove_item(void *item);
int fb_generate_item_id(void);

fb_img *fb_add_text_lvl_justified(int level, int x, int y, uint32_t color, int size, int justify, const char *fmt, ...);
#define fb_add_text_justified(x, y, color, size, justify, fmt, args...) fb_add_text_lvl_justified(LEVEL_TEXT, x, y, color, size, justify, fmt, ##args)
#define fb_add_text(x, y, color, size, fmt, args...) fb_add_text_lvl_justified(LEVEL_TEXT, x, y, color, size, JUSTIFY_LEFT, fmt, ##args)
#define fb_add_text_lvl(level, x, y, color, size, fmt, args...) fb_add_text_lvl_justified(level, x, y, color, size, JUSTIFY_LEFT, fmt, ##args)

fb_img *fb_add_text_long_lvl_justified(int level, int x, int y, uint32_t color, int size, int justify, const char *text);
#define fb_add_text_long(x, y, color, size, text) fb_add_text_long_lvl_justified(LEVEL_TEXT, x, y, color, size, JUSTIFY_LEFT, text)

fb_img *fb_text_create_item(int x, int y, uint32_t color, int size, int justify, const char *txt);
void fb_text_set_content(fb_img *img, const char *text);
void fb_text_set_color(fb_img *img, uint32_t color);
void fb_text_drop_cache_unused(void);
void fb_text_destroy(fb_img *i);

fb_rect *fb_add_rect_lvl(int level, int x, int y, int w, int h, uint32_t color);
#define fb_add_rect(x, y, w, h, color) fb_add_rect_lvl(LEVEL_RECT, x, y, w, h, color)

fb_img *fb_add_img(int x, int y, int w, int h, int img_type, px_type *data);
fb_img *fb_add_png_img(int x, int y, int w, int h, const char *path);
void fb_add_rect_notfilled(int x, int y, int w, int h, uint32_t color, int thickness, fb_rect ***list);
fb_msgbox *fb_create_msgbox(int w, int h, int bgcolor);
fb_text *fb_msgbox_add_text(int x, int y, int size, char *txt, ...);
void fb_msgbox_rm_text(fb_img *text);
void fb_destroy_msgbox(void);
void fb_rm_text(fb_img *i);
void fb_rm_rect(fb_rect *r);
void fb_rm_img(fb_img *i);
px_type fb_convert_color(uint32_t c);

void fb_draw_overlay(void);
void fb_draw_rect(fb_rect *r);
void fb_draw_img(fb_img *i);
void fb_fill(uint32_t color);
void fb_request_draw(void);
void fb_force_draw(void);
void fb_clear(void);
void fb_freeze(int freeze);
int fb_clone(char **buff);

void fb_push_context(void);
void fb_pop_context(void);

void fb_ctx_add_item(fb_context_t *ctx, void *item);
void fb_ctx_rm_item(fb_context_t *ctx, void *item);

px_type *fb_png_get(const char *path, int w, int h);
void fb_png_release(px_type *data);
void fb_png_drop_unused(void);

inline void center_text(fb_img *text, int targetX, int targetY, int targetW, int targetH);

int vt_set_mode(int graphics);

#endif
