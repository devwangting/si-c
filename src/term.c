#include "si.h"
#include "term.h"

#if defined(_WIN32)
#include <conio.h>
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#pragma comment(lib, "user32.lib")
#else
#include <errno.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/time.h>
#include <termios.h>
#include <unistd.h>
#endif

static const char TERM_START[] =
    "\x1b[?1049h\x1b[?25l\x1b[2J\x1b[H\x1b[?1002h\x1b[?1003h\x1b[?1006h";
static const char TERM_END[] =
    "\x1b[?2026l\x1b[?1006l\x1b[?1003l\x1b[?1002l\x1b[0m\x1b[?25h\x1b[?1049l";

static bool g_active = false;

static void write_all(const char *data, size_t len)
{
    size_t off = 0;
#if defined(_WIN32)
    while (off < len) {
        size_t n = fwrite(data + off, 1, len - off, stdout);
        if (n == 0)
            break;
        off += n;
    }
    fflush(stdout);
#else
    while (off < len) {
        ssize_t n = write(STDOUT_FILENO, data + off, len - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        off += (size_t)n;
    }
#endif
}

void term_write(const char *data, size_t len)
{
    if (len)
        write_all(data, len);
}

void term_mouse_hover(bool on)
{
    static const char enable[] = "\x1b[?1003h";
    static const char disable[] = "\x1b[?1003l";
    if (on)
        term_write(enable, sizeof(enable) - 1);
    else
        term_write(disable, sizeof(disable) - 1);
}

bool term_is_tty(void)
{
#if defined(_WIN32)
    DWORD in_mode;
    DWORD out_mode;
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    return in != INVALID_HANDLE_VALUE && out != INVALID_HANDLE_VALUE &&
           GetConsoleMode(in, &in_mode) && GetConsoleMode(out, &out_mode);
#else
    return isatty(STDIN_FILENO) != 0 && isatty(STDOUT_FILENO) != 0;
#endif
}


int term_color_mode(void)
{
#if defined(_WIN32)
    return 256;
#else
    const char *term = getenv("TERM");
    const char *colorterm = getenv("COLORTERM");
    if (colorterm &&
        (str_icontains(colorterm, "truecolor") ||
         str_icontains(colorterm, "24bit")))
        return 256;
    if (!term || !*term)
        return 16;
    if (strstr(term, "256color") || strstr(term, "kitty") ||
        strstr(term, "alacritty") || strstr(term, "wezterm") ||
        strstr(term, "foot") || strstr(term, "konsole"))
        return 256;
    if (strcmp(term, "xterm") == 0 || strstr(term, "xterm-"))
        return 256;
    /* Linux virtual console, plain screen/tmux and vt100 understand only
     * the classic 16 colors. */
    return 16;
#endif
}

static int key_for_f(int n)
{
    return KEY_F1 + (n - 1);
}

static int key_for_shift_f(int n)
{
    return KEY_SHIFT_F1 + (n - 1);
}

static int key_for_ctrl_f(int n)
{
    if (n == 3)
        return KEY_CTRL_F3;
    if (n == 4)
        return KEY_CTRL_F4;
    if (n == 5)
        return KEY_CTRL_F5;
    return key_for_f(n);
}

static int csi_key(const char *seq, int len, KeyEvent *ev)
{
    int params[8];
    int np = 0;
    int i = 0;
    int p = 0;
    bool have = false;
    char final = 0;

    memset(params, 0, sizeof(params));
    for (i = 0; i < len; i++) {
        char c = seq[i];
        if (c >= '0' && c <= '9') {
            p = p * 10 + (c - '0');
            have = true;
        } else if (c == ';') {
            if (np < 7)
                params[np++] = have ? p : 0;
            p = 0;
            have = false;
        } else if (c >= 0x40 && c <= 0x7E) {
            if (np < 7)
                params[np++] = have ? p : 0;
            final = c;
            break;
        }
    }
    if (!final)
        return 0;
    {
        int mod = np > 1 ? params[1] : 1;
        int p0 = np > 0 ? params[0] : 0;
        bool ctrl = (mod == 5 || mod == 6);
        bool alt = (mod == 3 || mod == 4);
        bool shift = (mod == 2 || mod == 4 || mod == 6);

        if (final == 'A' || final == 'B' || final == 'C' || final == 'D') {
            int base = final == 'A' ? KEY_UP
                       : final == 'B' ? KEY_DOWN
                       : final == 'C' ? KEY_RIGHT
                                      : KEY_LEFT;
            int code = base;
            if (ctrl && (base == KEY_LEFT || base == KEY_RIGHT ||
                         base == KEY_UP || base == KEY_DOWN)) {
                code = base == KEY_LEFT    ? KEY_CTRL_LEFT
                       : base == KEY_RIGHT ? KEY_CTRL_RIGHT
                       : base == KEY_UP    ? KEY_CTRL_UP
                                           : KEY_CTRL_DOWN;
            } else if (alt && (base == KEY_LEFT || base == KEY_RIGHT ||
                               base == KEY_UP || base == KEY_DOWN)) {
                code = base == KEY_LEFT    ? KEY_ALT_LEFT
                       : base == KEY_RIGHT ? KEY_ALT_RIGHT
                       : base == KEY_UP    ? KEY_ALT_UP
                                           : KEY_ALT_DOWN;
            }
            ev->code = code;
            return 1;
        }
        if (final == 'H' || final == 'F') {
            int base = final == 'H' ? KEY_HOME : KEY_END;
            if (ctrl)
                base = final == 'H' ? KEY_CTRL_HOME : KEY_CTRL_END;
            ev->code = base;
            return 1;
        }
        if (final == 'Z') {
            ev->code = KEY_SHIFT_TAB;
            return 1;
        }
        if (final == 'P' || final == 'Q' || final == 'R' || final == 'S') {
            int n = final == 'P' ? 1 : final == 'Q' ? 2 : final == 'R' ? 3 : 4;
            if (ctrl && (n == 3 || n == 4))
                ev->code = key_for_ctrl_f(n);
            else
                ev->code = shift ? key_for_shift_f(n) : key_for_f(n);
            return 1;
        }
        if (final == '~') {
            int n = p0;
            if (n == 1 || n == 7) {
                ev->code = ctrl ? KEY_CTRL_HOME : KEY_HOME;
                return 1;
            }
            if (n == 4 || n == 8) {
                ev->code = ctrl ? KEY_CTRL_END : KEY_END;
                return 1;
            }
            if (n == 2) {
                ev->code = KEY_INSERT;
                return 1;
            }
            if (n == 3) {
                ev->code = KEY_DELETE;
                return 1;
            }
            if (n == 5) {
                ev->code = KEY_PGUP;
                return 1;
            }
            if (n == 6) {
                ev->code = KEY_PGDN;
                return 1;
            }
            if (n >= 11 && n <= 14) {
                int fn = n - 10;
                if (ctrl && (fn == 3 || fn == 4))
                    ev->code = key_for_ctrl_f(fn);
                else
                    ev->code = shift ? key_for_shift_f(fn) : key_for_f(fn);
                return 1;
            }
            if (n == 15 || (n >= 17 && n <= 21) || n == 23 || n == 24) {
                static const int fn_map[] = {15, 17, 18, 19, 20, 21, 23, 24};
                static const int fn_key[] = {5, 6, 7, 8, 9, 10, 11, 12};
                size_t k;
                int fn = 0;
                for (k = 0; k < SI_ARRAY_LEN(fn_map); k++) {
                    if (fn_map[k] == n) {
                        fn = fn_key[k];
                        break;
                    }
                }
                if (fn == 10 && ctrl && !shift)
                    ev->code = KEY_CTRL_F10;
                else if (ctrl && (fn == 3 || fn == 4 || fn == 5))
                    ev->code = key_for_ctrl_f(fn);
                else
                    ev->code = shift ? key_for_shift_f(fn) : key_for_f(fn);
                return 1;
            }
        }
        (void)p0;
    }
    return 0;
}

static int mouse_key(const char *seq, int len, KeyEvent *ev)
{
    int vals[3] = {0, 0, 0};
    int vi = 0;
    int i;
    char final;
    bool have = false;

    if (len < 4 || seq[0] != '<')
        return 0;
    final = seq[len - 1];
    if (final != 'M' && final != 'm')
        return 0;
    for (i = 1; i < len - 1; i++) {
        char c = seq[i];
        if (c >= '0' && c <= '9') {
            if (vi > 2)
                return 0;
            vals[vi] = vals[vi] * 10 + (c - '0');
            have = true;
        } else if (c == ';') {
            if (!have || vi >= 2)
                return 0;
            vi++;
            have = false;
        } else {
            return 0;
        }
    }
    if (!have || vi != 2)
        return 0;
    ev->code = KEY_MOUSE;
    ev->mouse_button = vals[0] & 0x7F;
    ev->mouse_x = vals[1] > 0 ? vals[1] - 1 : 0;
    ev->mouse_y = vals[2] > 0 ? vals[2] - 1 : 0;
    ev->mouse_release = final == 'm';
    return 1;
}

#if defined(__APPLE__)
/* The default macOS keyboard layouts turn Option+key into printable
 * characters (Option+R is ®, Option+G is ©, and so on).  Treat those
 * characters as the Alt+key shortcuts they stand for, so Mac users do not
 * have to enable "Use Option as Meta Key" in the terminal. */
static char mac_option_base_key(uint32_t cp)
{
    static const struct {
        uint32_t cp;
        char key;
    } map[] = {
        {0x00B5, 'm'}, /* Option+M: micro sign */
        {0x00AE, 'r'}, /* Option+R: registered sign */
        {0x00A9, 'g'}, /* Option+G: copyright sign */
        {0x00A1, '1'}, /* Option+1: inverted exclamation mark */
        {0x2122, '2'}, /* Option+2: trade mark sign */
        {0x00A3, '3'}, /* Option+3: pound sign */
        {0x00A2, '4'}, /* Option+4: cent sign */
        {0x221E, '5'}, /* Option+5: infinity */
        {0x00BA, '0'}, /* Option+0: masculine ordinal indicator */
        {0x2020, 't'}, /* Option+T: dagger */
        {0x0192, 'f'}, /* Option+F: florin sign */
        {0x00DF, 's'}, /* Option+S: sharp s */
        {0x03C0, 'p'}, /* Option+P: pi */
        {0x00B4, 'e'}, /* Option+Shift+E: acute accent (dead-key form) */
        {0x02C6, 'i'}  /* Option+Shift+I: circumflex (dead-key form) */
    };
    size_t i;

    for (i = 0; i < SI_ARRAY_LEN(map); i++) {
        if (map[i].cp == cp)
            return map[i].key;
    }
    return 0;
}
#endif

int term_decode_sequence(const char *seq, int len, KeyEvent *ev)
{
    memset(ev, 0, sizeof(*ev));
    if (len <= 0)
        return 0;
    if (seq[0] == '<')
        return mouse_key(seq, len, ev);
#if defined(__APPLE__)
    {
        uint32_t cp;
        if (utf8_decode(seq, (size_t)len, &cp) > 0) {
            char base = mac_option_base_key(cp);
            if (base) {
                ev->code = KEY_ALT_CHAR;
                ev->text[0] = base;
                ev->len = 1;
                return 1;
            }
        }
        /* Terminal.app maps Option+Left/Right to ESC b / ESC f. */
        if (len == 1 && seq[0] == 'b') {
            ev->code = KEY_ALT_LEFT;
            return 1;
        }
        if (len == 1 && seq[0] == 'f') {
            ev->code = KEY_ALT_RIGHT;
            return 1;
        }
    }
#endif
    /* Linux virtual console sends F1..F5 as ESC [ [ A .. E. */
    if (len == 2 && seq[0] == '[' &&
        seq[1] >= 'A' && seq[1] <= 'E') {
        ev->code = KEY_F1 + (seq[1] - 'A');
        return 1;
    }
    return csi_key(seq, len, ev);
}

#if defined(_WIN32)

static DWORD g_saved_in_mode;
static DWORD g_saved_out_mode;
static bool g_have_modes = false;
static int g_saved_stdout_mode = -1;

static void term_restore_platform(void)
{
    if (g_have_modes) {
        SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), g_saved_in_mode);
        SetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), g_saved_out_mode);
        g_have_modes = false;
    }
    if (g_saved_stdout_mode >= 0) {
        _setmode(_fileno(stdout), g_saved_stdout_mode);
        g_saved_stdout_mode = -1;
    }
}

bool term_init(char **err)
{
    HANDLE in;
    HANDLE out;
    DWORD in_mode;
    DWORD out_mode;

    if (!term_is_tty()) {
        if (err)
            *err = xstrdup("stdin/stdout is not a terminal");
        return false;
    }
    in = GetStdHandle(STD_INPUT_HANDLE);
    out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!GetConsoleMode(in, &in_mode) || !GetConsoleMode(out, &out_mode)) {
        if (err)
            *err = xstrdup("cannot read console mode");
        return false;
    }
    g_saved_in_mode = in_mode;
    g_saved_out_mode = out_mode;
    g_have_modes = true;

    in_mode &= ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT |
                 ENABLE_PROCESSED_INPUT);
    in_mode |= ENABLE_EXTENDED_FLAGS;
    SetConsoleMode(in, in_mode);
    SetConsoleMode(out, out_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    SetConsoleOutputCP(CP_UTF8);
    g_saved_stdout_mode = _setmode(_fileno(stdout), _O_BINARY);
    atexit(term_shutdown);

    g_active = true;
    term_write(TERM_START, sizeof(TERM_START) - 1);
    return true;
}

void term_shutdown(void)
{
    if (!g_active)
        return;
    g_active = false;
    term_write(TERM_END, sizeof(TERM_END) - 1);
    term_restore_platform();
}

void term_get_size(int *rows, int *cols)
{
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info)) {
        *cols = info.srWindow.Right - info.srWindow.Left + 1;
        *rows = info.srWindow.Bottom - info.srWindow.Top + 1;
    } else {
        *cols = 100;
        *rows = 30;
    }
}

static int read_byte_timeout(unsigned char *c, int ms)
{
    int i;
    for (i = 0; i < ms; i++) {
        if (_kbhit()) {
            int ch = _getch();
            if (ch < 0)
                return -1;
            *c = (unsigned char)ch;
            return 1;
        }
        Sleep(1);
    }
    return 0;
}

static int extended_key(int code)
{
    switch (code) {
    case 72:
        return KEY_UP;
    case 80:
        return KEY_DOWN;
    case 75:
        return KEY_LEFT;
    case 77:
        return KEY_RIGHT;
    case 71:
        return KEY_HOME;
    case 79:
        return KEY_END;
    case 73:
        return KEY_PGUP;
    case 81:
        return KEY_PGDN;
    case 82:
        return KEY_INSERT;
    case 83:
        return KEY_DELETE;
    case 59:
        return KEY_F1;
    case 60:
        return KEY_F2;
    case 61:
        return KEY_F3;
    case 62:
        return KEY_F4;
    case 63:
        return KEY_F5;
    case 64:
        return KEY_F6;
    case 65:
        return KEY_F7;
    case 66:
        return KEY_F8;
    case 67:
        return KEY_F9;
    case 68:
        return KEY_F10;
    case 133:
        return KEY_F11;
    case 134:
        return KEY_F12;
    case 135:
        return KEY_SHIFT_F11;
    case 136:
        return KEY_SHIFT_F12;
    default:
        return KEY_NONE;
    }
}

int term_read_key(KeyEvent *ev)
{
    int c;
    int i;

    memset(ev, 0, sizeof(*ev));
    c = _getch();
    if (c < 0)
        return -1;
    if (c == 0 || c == 224) {
        int e = _getch();
        ev->code = extended_key(e);
        if (e == 68 && (GetAsyncKeyState(VK_CONTROL) & 0x8000))
            ev->code = KEY_CTRL_F10;
        else if (e == 61 && (GetAsyncKeyState(VK_CONTROL) & 0x8000))
            ev->code = KEY_CTRL_F3;
        else if (e == 62 && (GetAsyncKeyState(VK_CONTROL) & 0x8000))
            ev->code = KEY_CTRL_F4;
        return ev->code ? 1 : 0;
    }
    if (c == 8 || c == 127) {
        ev->code = KEY_BACKSPACE;
        return 1;
    }
    if (c == 13 || c == 10) {
        ev->code = KEY_ENTER;
        return 1;
    }
    if (c == 9) {
        ev->code = KEY_TAB;
        return 1;
    }
    if (c == 27) {
        ev->code = KEY_ESC;
        return 1;
    }
    if (c < 32) {
        ev->code = c;
        return 1;
    }
    if (c < 128) {
        ev->code = KEY_CHAR;
        ev->text[0] = (char)c;
        ev->len = 1;
        return 1;
    }
    {
        char buf[8];
        int need = 1;
        buf[0] = (char)c;
        if ((c & 0xE0) == 0xC0)
            need = 2;
        else if ((c & 0xF0) == 0xE0)
            need = 3;
        else if ((c & 0xF8) == 0xF0)
            need = 4;
        for (i = 1; i < need; i++) {
            unsigned char b;
            if (read_byte_timeout(&b, 30) != 1)
                break;
            buf[i] = (char)b;
        }
        if (i == need) {
            uint32_t cp;
            int n = utf8_decode(buf, (size_t)need, &cp);
            if (n > 0) {
                memcpy(ev->text, buf, (size_t)need);
                ev->len = need;
                ev->code = KEY_CHAR;
                return 1;
            }
        }
        ev->code = KEY_CHAR;
        ev->text[0] = (char)c;
        ev->len = 1;
        return 1;
    }
}

#else

static struct termios g_orig;
static bool g_have_termios = false;
static volatile sig_atomic_t g_resized = 0;

static void on_winch(int sig)
{
    (void)sig;
    g_resized = 1;
}

static void on_fatal(int sig)
{
    static const char restore[] =
        "\x1b[?2026l\x1b[?1006l\x1b[?1003l\x1b[?1002l\x1b[0m\x1b[?25h\x1b[?1049l";
    ssize_t ignored = write(STDOUT_FILENO, restore, sizeof(restore) - 1);
    (void)ignored;
    _exit(128 + sig);
}

static void term_restore_platform(void)
{
    if (g_have_termios) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig);
        g_have_termios = false;
    }
}

static void atexit_restore(void)
{
    term_shutdown();
}

bool term_init(char **err)
{
    struct termios raw;

    if (!term_is_tty()) {
        if (err)
            *err = xstrdup("stdin/stdout is not a terminal");
        return false;
    }
    if (tcgetattr(STDIN_FILENO, &g_orig) != 0) {
        if (err)
            *err = xasprintf("tcgetattr: %s", strerror(errno));
        return false;
    }
    g_have_termios = true;
    raw = g_orig;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= CS8;
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) {
        if (err)
            *err = xasprintf("tcsetattr: %s", strerror(errno));
        g_have_termios = false;
        return false;
    }
    signal(SIGWINCH, on_winch);
    signal(SIGTERM, on_fatal);
    signal(SIGHUP, on_fatal);
    signal(SIGINT, on_fatal);
    atexit(atexit_restore);

    g_active = true;
    term_write(TERM_START, sizeof(TERM_START) - 1);
    return true;
}

void term_shutdown(void)
{
    if (!g_active)
        return;
    g_active = false;
    term_write(TERM_END, sizeof(TERM_END) - 1);
    term_restore_platform();
}

void term_get_size(int *rows, int *cols)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 &&
        ws.ws_col > 0) {
        *rows = ws.ws_row;
        *cols = ws.ws_col;
    } else {
        *cols = 100;
        *rows = 30;
    }
}

static int read_one(unsigned char *c)
{
    for (;;) {
        ssize_t n = read(STDIN_FILENO, c, 1);
        if (n < 0) {
            if (errno == EINTR) {
                if (g_resized) {
                    g_resized = 0;
                    return 0;
                }
                continue;
            }
            return -1;
        }
        if (n == 0)
            return -1;
        return 1;
    }
}

static int read_timeout(unsigned char *c, int ms)
{
    int rc;
    for (;;) {
        fd_set fds;
        struct timeval tv;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        tv.tv_sec = ms / 1000;
        tv.tv_usec = (ms % 1000) * 1000;
        rc = select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);
        if (rc < 0 && errno == EINTR)
            continue;
        break;
    }
    if (rc <= 0)
        return rc;
    return read_one(c);
}

int term_read_key(KeyEvent *ev)
{
    unsigned char c;
    int n;

    memset(ev, 0, sizeof(*ev));
    n = read_one(&c);
    if (n <= 0)
        return n < 0 ? -1 : 0;

    if (c == 27) {
        unsigned char c2;
        if (read_timeout(&c2, 60) != 1) {
            ev->code = KEY_ESC;
            return 1;
        }
        if (c2 == '[' || c2 == 'O') {
            char seq[64];
            int len = 0;
            int r;
            while (len < (int)sizeof(seq) - 1) {
                unsigned char b;
                r = read_timeout(&b, 35);
                if (r <= 0)
                    break;
                seq[len++] = (char)b;
                if (len == 1 && b == '[')
                    continue;
                if (b >= 0x40 && b <= 0x7E)
                    break;
            }
            seq[len] = '\0';
            if (term_decode_sequence(seq, len, ev))
                return 1;
            return 0;
        }
        if (c2 >= 32 && c2 < 127) {
#if defined(__APPLE__)
            if (c2 == 'b' || c2 == 'f') {
                char one[1];
                one[0] = (char)c2;
                if (term_decode_sequence(one, 1, ev))
                    return 1;
            }
#endif
            ev->code = KEY_ALT_CHAR;
            ev->text[0] = (char)c2;
            ev->len = 1;
            return 1;
        }
        ev->code = KEY_ESC;
        return 1;
    }
    if (c == 127) {
        ev->code = KEY_BACKSPACE;
        return 1;
    }
    if (c == 8) {
        ev->code = 8; /* Ctrl+H; most terminals send 127 for Backspace */
        return 1;
    }
    if (c == 13 || c == 10) {
        ev->code = KEY_ENTER;
        return 1;
    }
    if (c == 9) {
        ev->code = KEY_TAB;
        return 1;
    }
    if (c < 32) {
        ev->code = c;
        return 1;
    }
    if (c < 0x80) {
        ev->code = KEY_CHAR;
        ev->text[0] = (char)c;
        ev->len = 1;
        return 1;
    }
    {
        char buf[8];
        int need = 1;
        int i;
        buf[0] = (char)c;
        if ((c & 0xE0) == 0xC0)
            need = 2;
        else if ((c & 0xF0) == 0xE0)
            need = 3;
        else if ((c & 0xF8) == 0xF0)
            need = 4;
        for (i = 1; i < need; i++) {
            unsigned char b;
            if (read_timeout(&b, 30) != 1)
                break;
            buf[i] = (char)b;
        }
        if (i == need) {
            uint32_t cp;
            if (utf8_decode(buf, (size_t)need, &cp) > 0) {
                if (term_decode_sequence(buf, need, ev))
                    return 1;
                memcpy(ev->text, buf, (size_t)need);
                ev->len = need;
                ev->code = KEY_CHAR;
                return 1;
            }
        }
        ev->code = KEY_CHAR;
        ev->text[0] = (char)c;
        ev->len = 1;
        return 1;
    }
}

#endif

int term_poll_key(KeyEvent *ev)
{
#if defined(_WIN32)
    if (!_kbhit())
        return 0;
#else
    fd_set fds;
    struct timeval tv;
    int rc;

    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    rc = select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);
    if (rc <= 0)
        return 0;
#endif
    return term_read_key(ev);
}
