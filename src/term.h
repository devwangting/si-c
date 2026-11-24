#ifndef SI_TERM_H
#define SI_TERM_H

#include "si.h"

enum {
    KEY_NONE = 0,
    KEY_TAB = 9,
    KEY_ENTER = 13,
    KEY_ESC = 27,
    KEY_BACKSPACE = 127,
    KEY_CHAR = 2000,
    KEY_UP = 2001,
    KEY_DOWN,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_HOME,
    KEY_END,
    KEY_PGUP,
    KEY_PGDN,
    KEY_DELETE,
    KEY_INSERT,
    KEY_CTRL_LEFT,
    KEY_CTRL_RIGHT,
    KEY_CTRL_UP,
    KEY_CTRL_DOWN,
    KEY_CTRL_HOME,
    KEY_CTRL_END,
    KEY_ALT_LEFT,
    KEY_ALT_RIGHT,
    KEY_ALT_UP,
    KEY_ALT_DOWN,
    KEY_SHIFT_TAB,
    KEY_F1,
    KEY_F2,
    KEY_F3,
    KEY_F4,
    KEY_F5,
    KEY_F6,
    KEY_F7,
    KEY_F8,
    KEY_F9,
    KEY_F10,
    KEY_F11,
    KEY_F12,
    KEY_CTRL_F10,
    KEY_CTRL_F5,
    KEY_SHIFT_F1,
    KEY_SHIFT_F2,
    KEY_SHIFT_F3,
    KEY_SHIFT_F4,
    KEY_SHIFT_F5,
    KEY_SHIFT_F6,
    KEY_SHIFT_F7,
    KEY_SHIFT_F8,
    KEY_SHIFT_F9,
    KEY_SHIFT_F10,
    KEY_SHIFT_F11,
    KEY_SHIFT_F12,
    KEY_CTRL_F3,
    KEY_CTRL_F4,
    KEY_MOUSE,
    KEY_ALT_CHAR
};

typedef struct {
    int code;
    char text[8];
    int len;
    int mouse_x;      /* 0-based screen column */
    int mouse_y;      /* 0-based screen row */
    int mouse_button; /* 0 left, 1 middle, 2 right, 64/65 wheel */
    bool mouse_release;
} KeyEvent;

bool term_is_tty(void);
int term_color_mode(void); /* 16 or 256 */
bool term_init(char **err);
void term_shutdown(void);
void term_get_size(int *rows, int *cols);
int term_read_key(KeyEvent *ev);
int term_poll_key(KeyEvent *ev); /* 0 = no pending input */
void term_write(const char *data, size_t len);
void term_mouse_hover(bool on);

/* Decode a terminal sequence without reading stdin.  Exposed for tests:
 * seq is the bytes after ESC (for example "A", "12~", "[B", "<0;5;3M"). */
int term_decode_sequence(const char *seq, int len, KeyEvent *ev);

#endif /* SI_TERM_H */
