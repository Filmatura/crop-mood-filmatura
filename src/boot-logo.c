/* Boot splash: pick one of three logos via a Prefs-tab menu setting. */
#include "dryos.h"
#include "bmp.h"
#include "config.h"
#include "gui-common.h"
#include "lvinfo.h"
#include "menu.h"
#include "zebra.h"

struct boot_logo_span { uint16_t y; uint16_t x; uint16_t width; uint8_t color; };

#include "boot-logo-spans-filmatura.inc"
#include "boot-logo-spans-revo.inc"
#include "boot-logo-spans-rogue.inc"

struct boot_logo_set { const struct boot_logo_span *spans; int count; };

#define LOGO_SET(name) { name, sizeof(name) / sizeof(name[0]) }
static const struct boot_logo_set boot_logo_sets[] = {
    LOGO_SET(boot_logo_spans_filmatura),
    LOGO_SET(boot_logo_spans_revo),
    LOGO_SET(boot_logo_spans_rogue),
};
#define BOOT_LOGO_COUNT (sizeof(boot_logo_sets) / sizeof(boot_logo_sets[0]))

/* Shown in Prefs; also persisted (see below) since boot_logo_show() runs
 * before config_load(), so this variable's value isn't loaded from the
 * card yet at the moment the splash actually needs to pick a logo. */
CONFIG_INT("boot.logo", boot_logo_choice, 0);

#define BOOT_LOGO_MIRROR_FILE "ML/BOOTLOGO.DAT"

/* Written immediately whenever the menu setting changes, and read directly
 * (bypassing the normal config system) at the very start of boot_logo_show(),
 * which runs too early for config_load() to have populated boot_logo_choice
 * yet. Keeping this as a tiny standalone file -- rather than, say, moving
 * config_load() earlier -- avoids touching the boot sequence ordering that
 * took a long time to get stable with crop_rec/mlv_lite. */
static void boot_logo_save_choice(int choice)
{
    FILE *f = FIO_CreateFile(BOOT_LOGO_MIRROR_FILE);
    if (f)
    {
        uint8_t v = (uint8_t) choice;
        FIO_WriteFile(f, &v, 1);
        FIO_CloseFile(f);
    }
}

static int boot_logo_load_choice(void)
{
    uint8_t v = 0;
    if (read_file(BOOT_LOGO_MIRROR_FILE, &v, 1) != 1)
        return 0;
    if (v >= BOOT_LOGO_COUNT)
        return 0;
    return v;
}

static MENU_SELECT_FUNC(boot_logo_toggle)
{
    menu_numeric_toggle(priv, delta, 0, BOOT_LOGO_COUNT - 1);
    boot_logo_save_choice(boot_logo_choice);
}

static struct menu_entry boot_logo_menu[] = {
    {
        .name = "Boot Logo",
        .priv = &boot_logo_choice,
        .max = BOOT_LOGO_COUNT - 1,
        .select = boot_logo_toggle,
        .choices = CHOICES("Filmatura", "REVO", "Rogue"),
        .help = "Logo shown while the camera boots.",
    },
};

static void boot_logo_menu_init(void)
{
    /* CONFIG_SLIM_MENUS' grid launcher shows a curated "Settings" tab (see
     * custom_menu.c), not the classic "Prefs" tab -- entries added there
     * are invisible from the grid UI. Real entries for that curated tab
     * get added directly to "Settings"; custom_menu.c's placeholder list
     * controls their order. */
    menu_add("Settings", boot_logo_menu, COUNT(boot_logo_menu));
}
INIT_FUNC(__FILE__, boot_logo_menu_init);

#define BOOT_LOGO_SCALE 1
#define BOOT_LOGO_W (720 * BOOT_LOGO_SCALE)
#define BOOT_LOGO_H (480 * BOOT_LOGO_SCALE)
#define BOOT_LOGO_X ((720 - BOOT_LOGO_W) / 2)
#define BOOT_LOGO_Y ((480 - BOOT_LOGO_H) / 2)

extern int ml_started;

static int boot_logo_selected = 0;

static void boot_logo_draw(void)
{
    /* Keep splash writes inside ML's normal LCD canvas.  The surrounding
     * 960x540 backing surface is changed by Canon during LV/zoom switches. */
    bmp_fill(COLOR_BLACK, 0, 0, 720, 480);
    const struct boot_logo_set *set = &boot_logo_sets[boot_logo_selected];
    for (int i = 0; i < set->count; i++)
    {
        const struct boot_logo_span *s = &set->spans[i];
        bmp_fill(s->color,
            BOOT_LOGO_X + s->x * BOOT_LOGO_SCALE,
            BOOT_LOGO_Y + s->y * BOOT_LOGO_SCALE,
            s->width * BOOT_LOGO_SCALE,
            BOOT_LOGO_SCALE);
    }
}

static volatile int boot_logo_active = 0;
static int boot_logo_hide_time = 0;
static volatile int boot_logo_handoff_pending = 0;
static volatile int boot_logo_hud_mask = 0;

int boot_logo_is_active(void)
{
    return boot_logo_active;
}

/* Keep ML's own status bars off the splash.  They are permitted only for
 * the final handoff frame, while Canon remains masked. */
int boot_logo_allows_overlay_draw(void)
{
    return !boot_logo_active || boot_logo_handoff_pending;
}

/* Called by the normal ML status-bar renderer.  Do not reveal Canon's
 * overlay until both status bars have had a chance to replace the splash. */
void boot_logo_overlay_updated(int top, int bottom)
{
    if (!boot_logo_handoff_pending) return;
    if (top)    boot_logo_hud_mask |= 1;
    if (bottom) boot_logo_hud_mask |= 2;
}

static void boot_logo_present(void)
{
    bmp_draw_to_idle(1);
    boot_logo_draw();
    bmp_idle_copy(1, 0);
    bmp_draw_to_idle(0);
}

static void boot_logo_clear(void)
{
    bmp_draw_to_idle(1);
    /* Keep the canvas opaque during the handoff.  A transparent frame here
     * exposes a stale Canon fragment before ML draws its own HUD. */
    bmp_fill(COLOR_BLACK, 0, 0, 720, 480);
    bmp_idle_copy(1, 0);
    bmp_draw_to_idle(0);
}

static void boot_logo_release_canvas(void)
{
    bmp_draw_to_idle(1);
    bmp_fill(COLOR_EMPTY, 0, 0, 720, 480);
    bmp_idle_copy(1, 0);
    bmp_draw_to_idle(0);
}

static void boot_logo_task(void *unused)
{
    (void) unused;

    const int fallback_handoff_time = boot_logo_hide_time + 500;
    while (boot_logo_active)
    {
        int splash_time_done = get_ms_clock() >= boot_logo_hide_time;
        int ml_display_ready = ml_started &&
            (liveview_display_idle() || get_ms_clock() >= fallback_handoff_time);
        if (splash_time_done && ml_display_ready) break;
        msleep(20);
    }

    if (boot_logo_active)
    {
        /* Request ML's first HUD redraw while Canon remains masked. */
        boot_logo_hud_mask = 0;
        boot_logo_handoff_pending = 1;
        lens_display_set_dirty();
        menu_set_dirty();
        BMP_LOCK( boot_logo_clear(); )
        const int hud_deadline = get_ms_clock() + 1000;
        while (boot_logo_hud_mask != 3 && get_ms_clock() < hud_deadline)
            msleep(20);
        boot_logo_handoff_pending = 0;
        BMP_LOCK( boot_logo_release_canvas(); )
        boot_logo_active = 0;
    }
}

void boot_logo_show(void)
{
    if (!bmp_vram_raw()) return;

    boot_logo_selected = boot_logo_load_choice();

    /* Keep Canon's dialogs from overwriting the splash while it is visible. */
    boot_logo_active = 1;
    canon_gui_disable_front_buffer();
    boot_logo_hide_time = get_ms_clock() + 2000;
    BMP_LOCK( boot_logo_present(); )
    task_create("boot_logo", 0x1e, 0x1000, boot_logo_task, 0);
}
