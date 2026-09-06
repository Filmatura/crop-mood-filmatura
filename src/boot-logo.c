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
#include "boot-logo-spans-magiclantern.inc"
#include "boot-logo-spans-mlite.inc"

struct boot_logo_set { const struct boot_logo_span *spans; int count; };

#define LOGO_SET(name) { name, sizeof(name) / sizeof(name[0]) }
static const struct boot_logo_set boot_logo_sets[] = {
    LOGO_SET(boot_logo_spans_filmatura),
    LOGO_SET(boot_logo_spans_revo),
    LOGO_SET(boot_logo_spans_rogue),
    LOGO_SET(boot_logo_spans_magiclantern),
    LOGO_SET(boot_logo_spans_mlite),
};
#define BOOT_LOGO_SET_COUNT (sizeof(boot_logo_sets) / sizeof(boot_logo_sets[0]))

/* "Custom" isn't a spans set -- it's loaded from card at draw time -- so it
 * gets one extra index past the end of boot_logo_sets[]. */
#define BOOT_LOGO_CUSTOM_INDEX BOOT_LOGO_SET_COUNT
#define BOOT_LOGO_COUNT (BOOT_LOGO_SET_COUNT + 1)

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
        .choices = CHOICES("Filmatura", "REVO", "Rogue", "Magic Lantern", "M-Lite", "Custom"),
        .help = "Logo shown while the camera boots.",
        .help2 = "Custom: loads \\BOOTLOGO.BMP from the card root (720x480).\n"
                 "Use the installer to prep your own image -- it picks style\n"
                 "and does the color work on the PC, camera just displays it.",
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

/* Custom boot logo: a user-supplied 720x480, 24-bit uncompressed BMP at the
 * card root. Not a PNG -- decoding PNG needs a DEFLATE/zlib implementation,
 * which this firmware doesn't carry and isn't a safe thing to add for a
 * splash screen. Uncompressed BMP is just a fixed 54-byte header followed by
 * raw pixel bytes, so it can be parsed and streamed straight off the card
 * with no decompression at all.
 *
 * The BMP OSD hardware only has a 16-color palette; ML's higher color
 * indices (COLOR_GRAY(0..100) = indices 38-79) reuse a pre-existing 42-step
 * grayscale ramp from Canon's own palette instead. That's plenty to render
 * a real photo recognizably, so custom logos are shown in grayscale rather
 * than trying to build/load a custom color palette. */
#define BOOT_LOGO_CUSTOM_FILE "BOOTLOGO.BMP"
#define BOOT_LOGO_CUSTOM_W 720
#define BOOT_LOGO_CUSTOM_H 480

/* A batch of pixel rows, read in one FIO_ReadFile call rather than one call
 * per row -- 480 tiny sequential reads this early in boot (before the card
 * is necessarily up to full transfer speed) was slow enough to blow past
 * the splash's display budget.
 *
 * Must come from fio_malloc(), not a plain static/stack array: FIO_ReadFile
 * silently routes any read into ordinary cacheable memory through a
 * small-buffer workaround (alloc a temp DMA buffer, read into that, memcpy
 * out) that's hard-capped at 8192 bytes -- ASSERT(count <= 8192) in
 * fio-ml.c. A batch this size (up to 720*3*16 = 34560 bytes) blew straight
 * through that on real hardware. fio_malloc's buffer is already DMA-safe,
 * so FIO_ReadFile takes the direct path instead and the cap doesn't apply. */
#define BOOT_LOGO_CUSTOM_ROWS_PER_READ 16

/* Returns 1 if a valid file was found and fully drawn, 0 otherwise (caller
 * falls back to a plain black screen -- boot_logo_draw already cleared it). */
static int boot_logo_draw_custom(void)
{
    FILE *f = FIO_OpenFile(BOOT_LOGO_CUSTOM_FILE, O_RDONLY | O_SYNC);
    if (!f) return 0;

    int ok = 0;
    struct bmp_file_t hdr;
    if (FIO_ReadFile(f, &hdr, sizeof(hdr)) == sizeof(hdr) &&
        hdr.signature == 0x4D42 && /* 'BM' */
        hdr.hdr_size == 40 &&
        hdr.planes == 1 &&
        (hdr.bits_per_pixel == 24 || hdr.bits_per_pixel == 8) &&
        hdr.compression == 0 &&
        hdr.width == BOOT_LOGO_CUSTOM_W)
    {
        int32_t signed_height = (int32_t) hdr.height;
        int top_down = (signed_height < 0);
        int height = top_down ? -signed_height : signed_height;

        /* "image" is a byte offset from the start of the file (per the BMP
         * spec), not a real pointer -- see bmp_load_ram() for precedent. */
        uint32_t data_offset = (uint32_t)(uintptr_t) hdr.image;

        uint8_t *rows_buf = fio_malloc(BOOT_LOGO_CUSTOM_W * 3 * BOOT_LOGO_CUSTOM_ROWS_PER_READ);

        if (rows_buf && height == BOOT_LOGO_CUSTOM_H &&
            FIO_SeekSkipFile(f, data_offset, SEEK_SET) == data_offset)
        {
            /* 8bpp: a pre-quantized indexed BMP, e.g. from the installer's
             * off-camera dithering step (host CPU, real dithering libs, no
             * boot-time budget to blow through). Each pixel byte IS the
             * exact ML color index already -- no per-pixel math at all,
             * just a copy. This is the fast/robust path; prefer it. The
             * BMP's own embedded color table (between header and pixel
             * data) is for desktop previewers only and is skipped over by
             * the seek above -- the firmware never reads it. */
            int fast_path = (hdr.bits_per_pixel == 8);

            static uint8_t gray_lut[256];
            if (!fast_path)
                for (int i = 0; i < 256; i++)
                    gray_lut[i] = 38 + i * 41 / 255; // COLOR_GRAY(0..100) range

            uint8_t * const bvram = bmp_vram();
            const int bytes_per_pixel = fast_path ? 1 : 3;
            const int row_bytes = BOOT_LOGO_CUSTOM_W * bytes_per_pixel; // 720 or 2160, both already 4-byte aligned

            ok = 1;
            for (int row = 0; row < BOOT_LOGO_CUSTOM_H && ok; row += BOOT_LOGO_CUSTOM_ROWS_PER_READ)
            {
                int rows_this_batch = MIN(BOOT_LOGO_CUSTOM_ROWS_PER_READ, BOOT_LOGO_CUSTOM_H - row);
                int batch_bytes = rows_this_batch * row_bytes;
                if (FIO_ReadFile(f, rows_buf, batch_bytes) != batch_bytes)
                {
                    ok = 0;
                    break;
                }
                for (int i = 0; i < rows_this_batch; i++)
                {
                    uint8_t *src = rows_buf + i * row_bytes;
                    /* BMP rows are bottom-up unless height was negative. */
                    int y = top_down ? (row + i) : (BOOT_LOGO_CUSTOM_H - 1 - (row + i));

                    if (fast_path)
                    {
                        for (int x = 0; x < BOOT_LOGO_CUSTOM_W; x++)
                            bmp_putpixel_fast(bvram, BOOT_LOGO_X + x, BOOT_LOGO_Y + y, src[x]);
                        continue;
                    }

                    for (int x = 0; x < BOOT_LOGO_CUSTOM_W; x++)
                    {
                        uint8_t b = src[x*3 + 0];
                        uint8_t g = src[x*3 + 1];
                        uint8_t r = src[x*3 + 2];
                        int luma = (77*r + 150*g + 29*b) >> 8; // ~0.299/0.587/0.114
                        bmp_putpixel_fast(bvram, BOOT_LOGO_X + x, BOOT_LOGO_Y + y, gray_lut[luma]);
                    }
                }
            }
        }

        if (rows_buf)
            fio_free(rows_buf);
    }

    FIO_CloseFile(f);
    return ok;
}

static void boot_logo_draw(void)
{
    /* Keep splash writes inside ML's normal LCD canvas.  The surrounding
     * 960x540 backing surface is changed by Canon during LV/zoom switches. */
    bmp_fill(COLOR_BLACK, 0, 0, 720, 480);

    if (boot_logo_selected == BOOT_LOGO_CUSTOM_INDEX)
    {
        boot_logo_draw_custom();
        return;
    }

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
    BMP_LOCK( boot_logo_present(); )

    /* Start the on-screen timer only once the splash is actually drawn and
     * flipped to front -- the Custom (BMP) logo streams a ~1MB file off the
     * card and can take a while, so setting this beforehand could let the
     * 2s budget elapse mid-draw and have the splash torn down again almost
     * immediately after it finally appears. */
    boot_logo_hide_time = get_ms_clock() + 2000;
    task_create("boot_logo", 0x1e, 0x1000, boot_logo_task, 0);
}
