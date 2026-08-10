/** Slim menu grid launcher — 2x2 category picker using ML tab icons (orange). */
#include "dryos.h"
#include "bmp.h"
#include "font.h"
#include "config.h"
#include "menu.h"
#include "menu-grid.h"

#ifdef CONFIG_SLIM_MENUS

#define GRID_COLS       2
#define GRID_ROWS       2
#define GRID_COUNT      (GRID_COLS * GRID_ROWS)
/* One spacing value: left = mid = right = top = between = bottom. */
#define GRID_SPACE      40
#define GRID_RADIUS     36
#define GRID_SEL_BORDER 6
#define GRID_LABEL_PAD  16   /* baseline inset from bottom of each card */
#define GRID_ICON_GAP   14   /* clear space between icon area and label */

static int grid_active = 0;
static int grid_launched = 0;
static CONFIG_INT("menu.grid.sel", grid_sel, 0);

typedef struct
{
    const char *label;
    const char *menu_name;
    int icon;   /* ICON_ML_* from baseline menu tab bar */
} grid_tile_t;

static void grid_fill_round_rect(int x, int y, int w, int h, int r, int color)
{
    if (w <= 0 || h <= 0) return;
    r = MIN(r, MIN(w, h) / 2);

    bmp_fill(color, x + r, y, w - 2 * r, h);
    bmp_fill(color, x, y + r, w, h - 2 * r);

    fill_circle(x + r, y + r, r, color);
    fill_circle(x + w - r - 1, y + r, r, color);
    fill_circle(x + r, y + h - r - 1, r, color);
    fill_circle(x + w - r - 1, y + h - r - 1, r, color);
}

/* Exposure = +/- Expo, Monitoring = Overlay waveform, Movie = camera, Settings = wrench. */
static const grid_tile_t grid_tiles[GRID_COUNT] =
{
    { "Exposure",   "Expo",     ICON_ML_EXPO    },
    { "Monitoring", "Overlay",  ICON_ML_OVERLAY },
    { "Movie",      "Movie",    ICON_ML_MOVIE   },
    { "Settings",   "Settings", ICON_ML_PREFS   },
};

static void grid_layout(int *ox, int *oy, int *cw, int *ch)
{
    *cw = (720 - 3 * GRID_SPACE) / GRID_COLS;
    *ch = (480 - 3 * GRID_SPACE) / GRID_ROWS;
    *ox = GRID_SPACE;
    *oy = GRID_SPACE;
}

static void grid_cell_rect(int idx, int *x, int *y, int *w, int *h)
{
    int ox, oy, cw, ch;
    grid_layout(&ox, &oy, &cw, &ch);
    int col = idx % GRID_COLS;
    int row = idx / GRID_COLS;
    *x = ox + col * (cw + GRID_SPACE);
    *y = oy + row * (ch + GRID_SPACE);
    *w = cw;
    *h = ch;
}

static void grid_draw_ml_icon(int icon, int cx, int cy)
{
    const int scale = 2;
    int iw = bfnt_char_get_width(icon) * scale;
    /* ML tab glyphs are ~40 px tall at 1x; 2x → ~80. */
    int ih = 40 * scale;
    int x = cx - iw / 2;
    int y = cy - ih / 2;
    bfnt_draw_char_scaled(icon, x, y, COLOR_ORANGE, NO_BG_ERASE, scale);
}

int menu_grid_is_active(void)   { return grid_active; }
int menu_grid_is_launched(void) { return grid_launched; }

void menu_grid_open(void)
{
    grid_active = 1;
    grid_launched = 0;
    grid_sel = COERCE(grid_sel, 0, GRID_COUNT - 1);
}

void menu_grid_close(void)
{
    grid_active = 0;
    grid_launched = 0;
}

void menu_grid_return(void)
{
    grid_active = 1;
    grid_launched = 0;
    grid_sel = COERCE(grid_sel, 0, GRID_COUNT - 1);
}

static void menu_grid_launch(int idx)
{
    if (idx < 0 || idx >= GRID_COUNT) return;

    select_menu_by_name((char *) grid_tiles[idx].menu_name, 0);
    grid_active = 0;
    grid_launched = 1;
    grid_sel = idx;
}

void menu_grid_draw(void)
{
    bmp_fill(COLOR_BLACK, 0, 0, 720, 480);

    int fnt = FONT(FONT_CANON, COLOR_WHITE, NO_BG_ERASE);
    int label_h = fontspec_font(FONT_CANON)->height;
    int b = GRID_SEL_BORDER;

    for (int i = 0; i < GRID_COUNT; i++)
    {
        int x, y, w, h;
        grid_cell_rect(i, &x, &y, &w, &h);
        int selected = (i == grid_sel);
        int r = MIN(GRID_RADIUS, MIN(w, h) / 2);

        if (selected)
            grid_fill_round_rect(x - b, y - b, w + 2 * b, h + 2 * b, r + b, COLOR_ORANGE);

        grid_fill_round_rect(x, y, w, h, r, COLOR_GRAY(20));

        /* Shared bottom baseline for all four labels. */
        int label_y = y + h - GRID_LABEL_PAD - label_h;
        int label_w = bmp_string_width(FONT_CANON, (char *) grid_tiles[i].label);
        int label_x = x + (w - label_w) / 2;

        /* Icon centered in the remaining space above the label. */
        int icon_zone_top = y + 10;
        int icon_zone_bot = label_y - GRID_ICON_GAP;
        int icon_cy = (icon_zone_top + icon_zone_bot) / 2;
        grid_draw_ml_icon(grid_tiles[i].icon, x + w / 2, icon_cy);

        bmp_printf(fnt, label_x, label_y, "%s", grid_tiles[i].label);
    }
}

int menu_grid_handle_key(int button_code, int *needs_full_redraw)
{
    if (!grid_active)
        return 1;

    int col = grid_sel % GRID_COLS;
    int row = grid_sel / GRID_COLS;

    switch (button_code)
    {
    case BGMT_PRESS_UP:
    case BGMT_WHEEL_UP:
        row = (row + GRID_ROWS - 1) % GRID_ROWS;
        grid_sel = row * GRID_COLS + col;
        break;

    case BGMT_PRESS_DOWN:
    case BGMT_WHEEL_DOWN:
        row = (row + 1) % GRID_ROWS;
        grid_sel = row * GRID_COLS + col;
        break;

    case BGMT_PRESS_LEFT:
    case BGMT_WHEEL_LEFT:
        col = (col + GRID_COLS - 1) % GRID_COLS;
        grid_sel = row * GRID_COLS + col;
        break;

    case BGMT_PRESS_RIGHT:
    case BGMT_WHEEL_RIGHT:
        col = (col + 1) % GRID_COLS;
        grid_sel = row * GRID_COLS + col;
        break;

    case BGMT_PRESS_SET:
#if defined(CONFIG_7D)
    case BGMT_JOY_CENTER:
#endif
#ifdef BGMT_Q_SET
    case BGMT_Q_SET:
#endif
        menu_grid_launch(grid_sel);
        *needs_full_redraw = 1;
        return 0;

    case BGMT_MENU:
        return 1;

    default:
        return 1;
    }

    *needs_full_redraw = 1;
    return 0;
}

#else /* !CONFIG_SLIM_MENUS */

int menu_grid_is_active(void)   { return 0; }
int menu_grid_is_launched(void) { return 0; }
void menu_grid_open(void)       { }
void menu_grid_close(void)      { }
void menu_grid_return(void)    { }
void menu_grid_draw(void)       { }
int menu_grid_handle_key(int button_code, int *needs_full_redraw)
{
    (void) button_code;
    (void) needs_full_redraw;
    return 1;
}

#endif
