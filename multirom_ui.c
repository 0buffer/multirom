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

#include <unistd.h>
#include <stdio.h>
#include <assert.h>
#include <pthread.h>
#include <dirent.h>
#include <errno.h>
#include <string.h>

#include "multirom_ui.h"
#include "framebuffer.h"
#include "input.h"
#include "log.h"
#include "listview.h"
#include "util.h"
#include "button.h"
#include "checkbox.h"
#include "version.h"
#include "pong.h"
#include "progressdots.h"
#include "multirom_ui_themes.h"
#include "workers.h"
#include "hooks.h"
#include "containers.h"
#include "animation.h"
#include "notification_card.h"

static struct multirom_status *mrom_status = NULL;
static struct multirom_rom *selected_rom = NULL;
static volatile int exit_ui_code = -1;
static volatile int loop_act = 0;
static multirom_themes_info *themes_info = NULL;
static multirom_theme *cur_theme = NULL;
static int last_selected_int_rom = -1;
static int last_int_listview_pos = -1;

static pthread_mutex_t exit_code_mutex = PTHREAD_MUTEX_INITIALIZER;

uint32_t CLR_PRIMARY = LBLUE;
uint32_t CLR_SECONDARY = LBLUE2;

#define LOOP_UPDATE_USB 0x01
#define LOOP_START_PONG 0x02
#define LOOP_CHANGE_CLR 0x04

static void list_block(char *path, int rec)
{
    ERROR("Listing %s", path);
    DIR *d = opendir(path);
    if(!d)
    {
        ERROR("Failed to open %s", path);
        return;
    }

    struct dirent *dr;
    struct stat info;
    while((dr = readdir(d)))
    {
        if(dr->d_name[0] == '.')
            continue;

        ERROR("%s/%s (%d)", path, dr->d_name, dr->d_type);
        if(dr->d_type == 4 && rec)
        {
            char name[256];
            sprintf(name, "%s/%s", path, dr->d_name);
            list_block(name, 1);
        }
    }

    closedir(d);
}

static void reveal_rect_alpha_step(void *data, float interpolated)
{
    fb_rect *r = data;
    interpolated = 1.f - interpolated;
    r->color = (r->color & ~(0xFF << 24)) | (((int)(0xFF*interpolated)) << 24);
    fb_request_draw();
}

int multirom_ui(struct multirom_status *s, struct multirom_rom **to_boot)
{
    if(multirom_init_fb(s->rotation) < 0)
        return UI_EXIT_BOOT_ROM;

    fb_freeze(1);

    mrom_status = s;

    exit_ui_code = -1;
    selected_rom = NULL;
    last_selected_int_rom = -1;
    last_int_listview_pos = -1;

    multirom_ui_select_color(s->colors);
    themes_info = multirom_ui_init_themes();
    if((cur_theme = multirom_ui_select_theme(themes_info, fb_width, fb_height)) == NULL)
    {
        fb_freeze(0);

        ERROR("Couldn't find theme for resolution %dx%d!\n", fb_width, fb_height);
        fb_add_text(0, 0, WHITE, SIZE_SMALL, "Couldn't find theme for resolution %dx%d!\nPress POWER to reboot.", fb_width, fb_height);
        fb_force_draw();

        start_input_thread();
        while(wait_for_key() != KEY_POWER);
        stop_input_thread();

        fb_clear();
        fb_close();
        return UI_EXIT_REBOOT;
    }

    workers_start();
    anim_init(s->anim_duration_coef);

    multirom_ui_init_theme(TAB_INTERNAL);

    add_touch_handler(&multirom_ui_touch_handler, NULL);
    start_input_thread();
    keyaction_enable(1);

    multirom_set_brightness(s->brightness);

    fb_freeze(0);

    if(s->auto_boot_rom && s->auto_boot_seconds > 0)
        multirom_ui_auto_boot();
    else
    {
        fb_rect *r = fb_add_rect_lvl(1000, 0, 0, fb_width, fb_height, BLACK);
        call_anim *a = call_anim_create(r, reveal_rect_alpha_step, 500, INTERPOLATOR_ACCELERATE);
        a->on_finished_call = fb_remove_item;
        a->on_finished_data = r;
        call_anim_add(a);
    }

    fb_request_draw();

    while(1)
    {
        pthread_mutex_lock(&exit_code_mutex);
        if(exit_ui_code != -1)
        {
            pthread_mutex_unlock(&exit_code_mutex);
            break;
        }

        if(loop_act & LOOP_UPDATE_USB)
        {
            multirom_find_usb_roms(mrom_status);
            if(themes_info->data->selected_tab == TAB_USB)
                multirom_ui_tab_rom_update_usb(themes_info->data->tab_data);
            loop_act &= ~(LOOP_UPDATE_USB);
        }

        if(loop_act & LOOP_START_PONG)
        {
            loop_act &= ~(LOOP_START_PONG);
            keyaction_enable(0);
            input_push_context();
            anim_push_context();
            fb_push_context();

            pong();

            fb_pop_context();
            anim_pop_context();
            input_pop_context();
            keyaction_enable(1);
        }

        if(loop_act & LOOP_CHANGE_CLR)
        {
            fb_freeze(1);

            multirom_ui_destroy_theme();
            multirom_ui_select_color(s->colors);
            multirom_ui_init_theme(TAB_MISC);

            fb_freeze(0);
            fb_request_draw();

            loop_act &= ~(LOOP_CHANGE_CLR);
        }

        pthread_mutex_unlock(&exit_code_mutex);

        usleep(100000);
    }

    keyaction_enable(0);
    keyaction_clear();

    rm_touch_handler(&multirom_ui_touch_handler, NULL);

    ncard_builder *b = ncard_create_builder();
    ncard_set_pos(b, NCARD_POS_CENTER);

    switch(exit_ui_code)
    {
        case UI_EXIT_BOOT_ROM:
        {
            *to_boot = selected_rom;
            ncard_set_title(b, "Booting...");

            char buff[64];
            snprintf(buff, sizeof(buff), "<i>%s</i>", selected_rom->name);
            ncard_set_text(b, buff);
            break;
        }
        case UI_EXIT_REBOOT:
            ncard_set_text(b, "\nRebooting...\n\n");
            break;
        case UI_EXIT_REBOOT_RECOVERY:
            ncard_set_text(b, "\nRebooting to recovery...\n\n");
            break;
        case UI_EXIT_REBOOT_BOOTLOADER:
            ncard_set_text(b, "\nRebooting to bootloader...\n\n");
            break;
        case UI_EXIT_SHUTDOWN:
            ncard_set_text(b, "\nShutting down...\n\n");
            break;
    }

    ncard_show(b, 1);
    anim_stop(1);
    fb_freeze(1);
    fb_force_draw();

    multirom_ui_destroy_theme();
    multirom_ui_free_themes(themes_info);
    themes_info = NULL;

    stop_input_thread();
    workers_stop();
    
#if MR_DEVICE_HOOKS >= 2
    mrom_hook_before_fb_close();
#endif
    fb_close();
    return exit_ui_code;
}

void multirom_ui_init_theme(int tab)
{
    memset(themes_info->data, 0, sizeof(multirom_theme_data));
    themes_info->data->selected_tab = -1;

    multirom_ui_init_header();
    multirom_ui_switch(tab);
    fb_set_background(C_BACKGROUND);
}

void multirom_ui_destroy_theme(void)
{
    cur_theme->destroy(themes_info->data);

    int i;
    for(i = 0; i < TAB_COUNT; ++i)
    {
        button_destroy(themes_info->data->tab_btns[i]);
        themes_info->data->tab_btns[i] = NULL;
    }

    multirom_ui_destroy_tab(themes_info->data->selected_tab);

    fb_clear();
}

void multirom_ui_init_header(void)
{
    cur_theme->init_header(themes_info->data);
}

void multirom_ui_header_select(int tab)
{
    cur_theme->header_select(themes_info->data, tab);
}

void multirom_ui_destroy_tab(int tab)
{
    switch(tab)
    {
        case -1:
            break;
        case TAB_USB:
        case TAB_INTERNAL:
            multirom_ui_tab_rom_destroy(themes_info->data->tab_data);
            break;
        case TAB_MISC:
            multirom_ui_tab_misc_destroy(themes_info->data->tab_data);
            break;
        default:
            assert(0);
            break;
    }
    themes_info->data->tab_data = NULL;
}

void multirom_ui_switch(int tab)
{
    if(tab == themes_info->data->selected_tab)
        return;

    fb_freeze(1);

    multirom_ui_header_select(tab);

    // destroy old tab
    multirom_ui_destroy_tab(themes_info->data->selected_tab);

    // init new tab
    switch(tab)
    {
        case TAB_USB:
        case TAB_INTERNAL:
            themes_info->data->tab_data = multirom_ui_tab_rom_init(tab);
            break;
        case TAB_MISC:
            themes_info->data->tab_data = multirom_ui_tab_misc_init();
            break;
    }

    themes_info->data->selected_tab = tab;

    fb_freeze(0);
    fb_request_draw();
}

void multirom_ui_fill_rom_list(listview *view, int mask)
{
    int i;
    struct multirom_rom *rom;
    void *data;
    listview_item *it, *select = NULL;
    char part_desc[64];
    for(i = 0; mrom_status->roms && mrom_status->roms[i]; ++i)
    {
        rom = mrom_status->roms[i];

        if(!(M(rom->type) & mask))
            continue;

        if(rom->partition)
            sprintf(part_desc, "%s (%s)", rom->partition->name, rom->partition->fs);

        if(rom->type == ROM_DEFAULT && mrom_status->hide_internal)
            continue;

        data = rom_item_create(rom->name, rom->partition ? part_desc : NULL, rom->icon_path);
        it = listview_add_item(view, rom->id, data);

        if (!select &&
            ((mrom_status->auto_boot_rom && rom == mrom_status->auto_boot_rom) ||
            (!mrom_status->auto_boot_rom && rom == mrom_status->current_rom)))
        {
            select = it;
        }

        if(rom->id == last_selected_int_rom)
            select = it;
    }
}

int multirom_ui_touch_handler(touch_event *ev, void *data)
{
    static int touch_count = 0;
    if(ev->changed & TCHNG_ADDED)
    {
        if(++touch_count == 4)
        {
            multirom_take_screenshot();
            touch_count = 0;
        }
    }

    if((ev->changed & TCHNG_REMOVED) && touch_count > 0)
        --touch_count;

    return -1;
}

struct auto_boot_data
{
    ncard_builder *b;
    int seconds;
    uint32_t anim_id;
    pthread_mutex_t mutex;
    int destroy;
};

static void multirom_ui_destroy_auto_boot_data(struct auto_boot_data *d)
{
    ncard_destroy_builder(d->b);
    pthread_mutex_destroy(&d->mutex);
    free(d);
}

static void multirom_ui_auto_boot_hidden(void *data)
{
    struct auto_boot_data *d = data;
    pthread_mutex_lock(&d->mutex);
    if(d->anim_id == UINT_MAX)
    {
        pthread_mutex_unlock(&d->mutex);
        multirom_ui_destroy_auto_boot_data(d);
        return;
    }
    else
    {
        d->destroy = 1;
    }
    pthread_mutex_unlock(&d->mutex);
}

static void multirom_ui_auto_boot_now(void *data)
{
    multirom_ui_auto_boot_hidden(data);

    pthread_mutex_lock(&exit_code_mutex);
    selected_rom = mrom_status->auto_boot_rom;
    exit_ui_code = UI_EXIT_BOOT_ROM;
    pthread_mutex_unlock(&exit_code_mutex);
}

static void multirom_ui_auto_boot_tick(void *data)
{
    char buff[128];
    struct auto_boot_data *d = data;

    pthread_mutex_lock(&d->mutex);

    if(d->destroy)
    {
        pthread_mutex_unlock(&d->mutex);
        multirom_ui_destroy_auto_boot_data(d);
        return;
    }

    if(--d->seconds == 0)
    {
        d->anim_id = UINT_MAX;

        pthread_mutex_lock(&exit_code_mutex);
        selected_rom = mrom_status->auto_boot_rom;
        exit_ui_code = UI_EXIT_BOOT_ROM;
        pthread_mutex_unlock(&exit_code_mutex);
    }
    else
    {
        call_anim *a = call_anim_create(NULL, NULL, 1000, INTERPOLATOR_LINEAR);
        d->anim_id = a->id;
        a->duration = 1000; // in call_anim_create, duration is multiplied by coef - we don't want that here
        a->on_finished_call = multirom_ui_auto_boot_tick;
        a->on_finished_data = d;
        call_anim_add(a);

        snprintf(buff, sizeof(buff), "\n<b>ROM:</b> <y>%s</y>\n\nBooting in %d second%s.", mrom_status->auto_boot_rom->name, d->seconds, d->seconds != 1 ? "s" : "");
        ncard_set_text(d->b, buff);
        ncard_show(d->b, 0);
    }

    pthread_mutex_unlock(&d->mutex);
}

void multirom_ui_auto_boot(void)
{
    ncard_builder *b = ncard_create_builder();

    struct auto_boot_data *d = mzalloc(sizeof(struct auto_boot_data));
    d->b = b;
    d->seconds = mrom_status->auto_boot_seconds + 1;
    pthread_mutex_init(&d->mutex, NULL);

    ncard_set_pos(b, NCARD_POS_CENTER);
    ncard_set_cancelable(b, 1);
    ncard_set_title(b, "Auto-boot");
    ncard_add_btn(b, BTN_NEGATIVE, "Cancel", ncard_hide_callback, NULL);
    ncard_add_btn(b, BTN_POSITIVE, "Boot now", multirom_ui_auto_boot_now, d);
    ncard_set_on_hidden(b, multirom_ui_auto_boot_hidden, d);
    ncard_set_from_black(b, 1);

    multirom_ui_auto_boot_tick(d);
}

void multirom_ui_refresh_usb_handler(void)
{
    pthread_mutex_lock(&exit_code_mutex);
    loop_act |= LOOP_UPDATE_USB;
    pthread_mutex_unlock(&exit_code_mutex);
}

void multirom_ui_start_pong(int action)
{
    pthread_mutex_lock(&exit_code_mutex);
    loop_act |= LOOP_START_PONG;
    pthread_mutex_unlock(&exit_code_mutex);
}

void *multirom_ui_tab_rom_init(int tab_type)
{
    tab_data_roms *t = mzalloc(sizeof(tab_data_roms));
    themes_info->data->tab_data = t;

    t->list = mzalloc(sizeof(listview));
    t->list->item_draw = &rom_item_draw;
    t->list->item_hide = &rom_item_hide;
    t->list->item_height = &rom_item_height;
    t->list->item_destroy = &rom_item_destroy;
    t->list->item_selected = &multirom_ui_tab_rom_selected;
    t->list->item_confirmed = &multirom_ui_tab_rom_confirmed;

    cur_theme->tab_rom_init(themes_info->data, t, tab_type);

    listview_init_ui(t->list);

    if(tab_type == TAB_INTERNAL)
        multirom_ui_fill_rom_list(t->list, MASK_INTERNAL);

    listview_update_ui(t->list);

    if(tab_type == TAB_INTERNAL && last_int_listview_pos != -1)
    {
        t->list->pos = last_int_listview_pos;
        listview_update_ui(t->list);
    }
    else if(listview_ensure_selected_visible(t->list))
        listview_update_ui(t->list);

    int has_roms = (int)(t->list->items == NULL);
    multirom_ui_tab_rom_set_empty((void*)t, has_roms);

    if(tab_type == TAB_USB)
    {
        multirom_set_usb_refresh_handler(&multirom_ui_refresh_usb_handler);
        multirom_set_usb_refresh_thread(mrom_status, 1);
    }
    return t;
}

void multirom_ui_tab_rom_destroy(void *data)
{
    multirom_set_usb_refresh_thread(mrom_status, 0);
    pthread_mutex_lock(&exit_code_mutex);
    loop_act &= ~(LOOP_UPDATE_USB);
    pthread_mutex_unlock(&exit_code_mutex);

    tab_data_roms *t = (tab_data_roms*)data;

    list_clear(&t->buttons, &button_destroy);
    list_clear(&t->ui_elements, &fb_remove_item);

    if(themes_info->data->selected_tab == TAB_INTERNAL)
        last_int_listview_pos = t->list->pos;

    listview_destroy(t->list);

    if(t->usb_prog)
        progdots_destroy(t->usb_prog);

    free(t);
}

void multirom_ui_tab_rom_selected(listview_item *prev, listview_item *now)
{
    if(!now)
        return;

    struct multirom_rom *rom = multirom_get_rom_by_id(mrom_status, now->id);
    if(!rom || !themes_info->data->tab_data)
        return;

    if(M(rom->type) & MASK_INTERNAL)
        last_selected_int_rom = now->id;
}

void multirom_ui_tab_rom_confirmed(listview_item *it)
{
    multirom_ui_tab_rom_boot_btn(0);
}

void multirom_ui_tab_rom_boot_btn(int action)
{
    if(!themes_info->data->tab_data)
        return;

    tab_data_roms *t = (tab_data_roms*)themes_info->data->tab_data;
    if(!t->list->selected)
        return;

    struct multirom_rom *rom = multirom_get_rom_by_id(mrom_status, t->list->selected->id);
    if(!rom)
        return;

    int error = 0;
    ncard_builder *b = ncard_create_builder();
    ncard_set_pos(b, NCARD_POS_CENTER);
    ncard_add_btn(b, BTN_NEGATIVE, "ok", ncard_hide_callback, NULL);
    ncard_set_cancelable(b, 1);
    ncard_set_title(b, "Error");

    int m = M(rom->type);
    if(m & MASK_UNSUPPORTED)
    {
        ncard_set_text(b, "Unsupported ROM type, see XDA thread for more info!");
        error = 1;
    }
    else if (((m & MASK_KEXEC) || ((m & MASK_ANDROID) && rom->has_bootimg)) &&
        multirom_has_kexec() != 0)
    {
        ncard_set_text(b, "Kexec-hardboot support is required to boot this ROM.\n\nInstall kernel with kexec-hardboot support to your Internal ROM!");
        error = 1;
    }
    else if((m & MASK_KEXEC) && strchr(rom->name, ' '))
    {
        ncard_set_text(b, "ROM's name contains spaces. Please remove spaces from this ROM's name");
        error = 1;
    }

    if(error)
    {
        ncard_show(b, 1);
        return;
    }
    else
        ncard_destroy_builder(b);

    pthread_mutex_lock(&exit_code_mutex);
    selected_rom = rom;
    exit_ui_code = UI_EXIT_BOOT_ROM;
    pthread_mutex_unlock(&exit_code_mutex);
}

void multirom_ui_tab_rom_update_usb(void *data)
{
    tab_data_roms *t = (tab_data_roms*)themes_info->data->tab_data;
    listview_clear(t->list);

    multirom_ui_fill_rom_list(t->list, MASK_USB_ROMS);
    listview_update_ui(t->list);

    multirom_ui_tab_rom_set_empty(data, (int)(t->list->items == NULL));
    fb_request_draw();
}

void multirom_ui_tab_rom_refresh_usb(int action)
{
    multirom_update_partitions(mrom_status);
}

void multirom_ui_tab_rom_set_empty(void *data, int empty)
{
    assert(empty == 0 || empty == 1);

    tab_data_roms *t = (tab_data_roms*)data;

    if(t->boot_btn)
        button_enable(t->boot_btn, !empty);

    if(empty && !t->usb_text)
    {
        fb_text_proto *p = fb_text_create(0, 0, C_TEXT, SIZE_NORMAL, "This list is refreshed automagically, just plug in the USB drive and wait.");
        p->wrap_w = t->list->w - 100*DPI_MUL;
        p->justify = JUSTIFY_CENTER;
        t->usb_text = fb_text_finalize(p);
        list_add(t->usb_text, &t->ui_elements);

        center_text(t->usb_text, t->list->x, -1, t->list->w, -1);
        t->usb_text->y = t->list->y + t->list->h*0.2;

        int x = t->list->x + ((t->list->w/2) - (PROGDOTS_W/2));
        t->usb_prog = progdots_create(x, t->usb_text->y+100*DPI_MUL);
    }
    else if(!empty && t->usb_text)
    {
        progdots_destroy(t->usb_prog);
        t->usb_prog = NULL;

        list_rm(t->usb_text, &t->ui_elements, &fb_remove_item);
        t->usb_text = NULL;
    }
}

void *multirom_ui_tab_misc_init(void)
{
    tab_data_misc *t = mzalloc(sizeof(tab_data_misc));
    cur_theme->tab_misc_init(themes_info->data, t, mrom_status->colors);
    return t;
}

void multirom_ui_tab_misc_destroy(void *data)
{
    tab_data_misc *t = (tab_data_misc*)data;

    list_clear(&t->ui_elements, &fb_remove_item);
    list_clear(&t->buttons, &button_destroy);

    free(t);
}

void multirom_ui_tab_misc_change_clr(int clr)
{
    if((loop_act & LOOP_CHANGE_CLR) || mrom_status->colors == clr)
        return;

    pthread_mutex_lock(&exit_code_mutex);
    mrom_status->colors = clr;
    loop_act |= LOOP_CHANGE_CLR;
    pthread_mutex_unlock(&exit_code_mutex);
}

void multirom_ui_reboot_btn(int action)
{
    pthread_mutex_lock(&exit_code_mutex);
    exit_ui_code = action;
    pthread_mutex_unlock(&exit_code_mutex);
}

void multirom_ui_tab_misc_copy_log(int action)
{
    multirom_dump_status(mrom_status);

    int res = multirom_copy_log(NULL, "../multirom_log.txt");

    static const char *text[] = { "Failed to copy log to sdcard!", "Error log was saved to:\n\n<s>/sdcard/multirom_log.txt</s>" };

    ncard_builder *b = ncard_create_builder();
    ncard_set_pos(b, NCARD_POS_CENTER);
    ncard_add_btn(b, BTN_NEGATIVE, "ok", ncard_hide_callback, NULL);
    ncard_set_title(b, "Save error log");
    ncard_set_text(b, text[res+1]);
    ncard_set_cancelable(b, 1);
    ncard_show(b, 1);
}
