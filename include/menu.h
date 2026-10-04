/*
 * menu.h - the completion menu: layout, selection and rendering.
 *
 * Pure with respect to the terminal: it is told how much room it has and it
 * returns the bytes to draw. No file descriptors, no termios, no reading of
 * keys — `line.c` does all three. That is what lets the layout and the
 * selection arithmetic be tested without a pty, leaving only the placement of
 * the cursor to the pty tests.
 */
#ifndef REDSTONE_MENU_H
#define REDSTONE_MENU_H

#include "comp.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct Menu Menu;

/* Where a movement key should take the selection. */
typedef enum { MENU_NEXT = 0, MENU_PREV, MENU_UP, MENU_DOWN } MenuMove;

Menu *menu_new(void);
void menu_free(Menu *m);

/* Take ownership of LIST and select its first candidate. An empty or NULL list
 * leaves the menu closed, and frees the list. */
void menu_open(Menu *m, CompList *list);
void menu_close(Menu *m); /* frees the list; the menu becomes inactive */
bool menu_active(const Menu *m);

const CompList *menu_list(const Menu *m);
const Comp *menu_selected(const Menu *m);
size_t menu_selected_index(const Menu *m);

/* The room the menu may use, in terminal cells. Rows is the number of lines it
 * may draw below the prompt; it scrolls rather than exceeding it. */
void menu_set_size(Menu *m, unsigned cols, unsigned rows);

void menu_move(Menu *m, MenuMove move);

/* The bytes to draw, each row preceded by "\r\n" so the caller can write them
 * straight after the prompt line. Valid until the next call. *rows receives the
 * number of lines written, which is what the caller moves back up by. Returns
 * "" with *rows 0 when the menu is inactive. */
const char *menu_render(Menu *m, unsigned *rows);

#endif /* REDSTONE_MENU_H */
