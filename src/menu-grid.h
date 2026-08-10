#ifndef MENU_GRID_H
#define MENU_GRID_H

/** Slim EOS M menu launcher: 2x2 grid shown on long-press TRASH / menu open. */

int menu_grid_is_active(void);
int menu_grid_is_launched(void);
void menu_grid_open(void);
void menu_grid_close(void);
void menu_grid_return(void);
void menu_grid_draw(void);
/** Returns 0 if handled, 1 if caller should continue normal menu key handling. */
int menu_grid_handle_key(int button_code, int *needs_full_redraw);

#endif
