/* menu.h - the Wimp's menu block, as viewer.c (the editor's menus) and
 * clipmenu.c (the Timeline's context menu) lay theirs out: items with
 * indirected texts (longer than 12 characters).
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#ifndef RE_MENU_H
#define RE_MENU_H
#include <stdint.h>

/* Menus: indirected items (the texts longer than 12 characters) */
typedef struct
{
    int flags;          /* bit 0 ticked, bit 7 the last */
    intptr_t sub;       /* a submenu, or -1 */
    int iflags;
    const char *text;
    const char *valid;
    int len;
} menu_item;
#define MENU(n)                                                                                                    \
    struct                                                                                                         \
    {                                                                                                              \
        char title[12];                                                                                            \
        unsigned char tfg, tbg, wfg, wbg;                                                                          \
        int w, h, gap;                                                                                             \
        menu_item item[n];                                                                                         \
    }
#define ITEM_FLAGS 0x07000121 /* text, filled, indirected, black on white */
#define SHADED     (1 << 22)
#define IT(t) ITEM_FLAGS, t, (const char *) -1, sizeof(t)

/* An item shaded (off) or not */
static inline void menu_shade(int *iflags, int off)
{
    *iflags = off ? (*iflags | SHADED) : (*iflags & ~SHADED);
}

/* An item ticked or not */
static inline void menu_tick(int *flags, int on)
{
    *flags = on ? (*flags | 1) : (*flags & ~1);
}

#endif
