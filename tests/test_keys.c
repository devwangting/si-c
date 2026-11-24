#include "si.h"
#include "term.h"

static int failures;
static int checks;

static void check_key(const char *seq, int want)
{
    KeyEvent ev;
    int ok;
    checks++;
    ok = term_decode_sequence(seq, (int)strlen(seq), &ev);
    if (want == KEY_NONE) {
        if (ok) {
            failures++;
            fprintf(stderr, "FAIL: sequence %-12s decoded unexpectedly\n",
                    seq);
        }
        return;
    }
    if (!ok || ev.code != want) {
        failures++;
        fprintf(stderr, "FAIL: sequence %-12s -> %d (want %d)\n",
                seq, ok ? ev.code : -1, want);
    }
}

#if defined(__APPLE__)
static void check_alt_symbol(const char *seq, char want)
{
    KeyEvent ev;
    int ok;
    checks++;
    ok = term_decode_sequence(seq, (int)strlen(seq), &ev);
    if (!ok || ev.code != KEY_ALT_CHAR || ev.len != 1 ||
        ev.text[0] != want) {
        failures++;
        fprintf(stderr,
                "FAIL: option symbol %s -> code=%d len=%d ch=%d (want %d)\n",
                seq, ok ? ev.code : -1, ok ? ev.len : -1,
                ok && ev.len > 0 ? ev.text[0] : -1, want);
    }
}
#endif

static void check_mouse(const char *seq, int x, int y, int button,
                        bool release)
{
    KeyEvent ev;
    int ok;
    checks++;
    ok = term_decode_sequence(seq, (int)strlen(seq), &ev);
    if (!ok || ev.code != KEY_MOUSE || ev.mouse_x != x ||
        ev.mouse_y != y || ev.mouse_button != button ||
        ev.mouse_release != release) {
        failures++;
        fprintf(stderr, "FAIL: mouse %s -> code=%d x=%d y=%d b=%d r=%d\n",
                seq, ok ? ev.code : -1, ev.mouse_x, ev.mouse_y,
                ev.mouse_button, ev.mouse_release);
    }
}

int main(void)
{
    check_key("A", KEY_UP);
    check_key("B", KEY_DOWN);
    check_key("C", KEY_RIGHT);
    check_key("D", KEY_LEFT);
    check_key("[A", KEY_F1); /* Linux virtual console */
    check_key("[B", KEY_F2);
    check_key("[C", KEY_F3);
    check_key("[D", KEY_F4);
    check_key("[E", KEY_F5);
    check_key("Q", KEY_F2); /* xterm application mode: ESC O Q */
    check_key("12~", KEY_F2); /* xterm CSI mode */
    check_key("24~", KEY_F12);
    check_key("24;2~", KEY_SHIFT_F12);
    check_key("21~", KEY_F10);
    check_key("21;5~", KEY_CTRL_F10);
    check_key("15;5~", KEY_CTRL_F5);
    check_key("15;6~", KEY_CTRL_F5);
    check_key("1;5R", KEY_CTRL_F3);
    check_key("1;5S", KEY_CTRL_F4);
    check_key("13;5~", KEY_CTRL_F3);
    check_key("14;5~", KEY_CTRL_F4);
    check_key("1;5C", KEY_CTRL_RIGHT);
    check_key("1;3D", KEY_ALT_LEFT);
    check_key("5~", KEY_PGUP);
    check_key("6~", KEY_PGDN);
    check_key("1~", KEY_HOME);
    check_key("4~", KEY_END);
    check_key("Z", KEY_SHIFT_TAB);
    check_key("not-a-key", KEY_NONE);
#if defined(__APPLE__)
    /* macOS Terminal sends Option+Left/Right as ESC b / ESC f. */
    check_key("b", KEY_ALT_LEFT);
    check_key("f", KEY_ALT_RIGHT);
    /* Option+key symbols from the US/ABC layout. */
    check_alt_symbol("\xC2\xB5", 'm'); /* Option+M: micro sign */
    check_alt_symbol("\xC2\xAE", 'r'); /* Option+R: registered sign */
    check_alt_symbol("\xC2\xA9", 'g'); /* Option+G: copyright sign */
    check_alt_symbol("\xC2\xA1", '1'); /* Option+1: inverted exclamation */
    check_alt_symbol("\xE2\x84\xA2", '2'); /* Option+2: trade mark */
    check_alt_symbol("\xC2\xA3", '3'); /* Option+3: pound sign */
    check_alt_symbol("\xC2\xA2", '4'); /* Option+4: cent sign */
    check_alt_symbol("\xE2\x88\x9E", '5'); /* Option+5: infinity */
    check_alt_symbol("\xC2\xBA", '0'); /* Option+0: masculine ordinal */
    check_alt_symbol("\xE2\x80\xA0", 't'); /* Option+T: dagger */
    check_alt_symbol("\xC6\x92", 'f'); /* Option+F: florin */
    check_alt_symbol("\xC3\x9F", 's'); /* Option+S: sharp s */
    check_alt_symbol("\xCF\x80", 'p'); /* Option+P: pi */
    check_alt_symbol("\xC2\xB4", 'e'); /* Option+Shift+E: acute accent */
    check_alt_symbol("\xCB\x86", 'i'); /* Option+Shift+I: circumflex */
#endif
    check_mouse("<0;20;5M", 19, 4, 0, false);
    check_mouse("<64;10;3M", 9, 2, 64, false);
    check_mouse("<0;20;5m", 19, 4, 0, true);

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
