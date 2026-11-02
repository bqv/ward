/* Prove libvterm drives a grid from bytes we supply — the seam QTermWidget
 * lacked. Feed escape sequences in, read the resulting screen out, and check
 * both the text and the cursor, and that a keystroke produces host bytes. */
#include <vterm.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

int main(void) {
    VTerm *vt = vterm_new(4, 20);          /* 4 rows, 20 cols */
    vterm_set_utf8(vt, 1);
    VTermScreen *screen = vterm_obtain_screen(vt);
    vterm_screen_reset(screen, 1);

    /* 1. colour + text + carriage return + cursor move, as a shell would send. */
    const char *stream =
        "\x1b[31mRED\x1b[0m and \x1b[1;32mGREEN\x1b[0m\r\n"
        "second line\r\n"
        "\x1b[2;5Hplaced";
    size_t used = vterm_input_write(vt, stream, strlen(stream));
    printf("input_write accepted %zu of %zu bytes\n", used, strlen(stream));

    /* 2. read the grid back */
    printf("\n--- screen ---\n");
    for (int row = 0; row < 4; ++row) {
        char line[64]; int n = 0;
        for (int col = 0; col < 20; ++col) {
            VTermScreenCell cell; VTermPos pos = { row, col };
            if (!vterm_screen_get_cell(screen, pos, &cell)) { line[n++] = '?'; continue; }
            if (cell.chars[0] == 0) { line[n++] = ' '; continue; }
            /* Resolve a non-default foreground to real RGB. The tag macros are
             * how vterm says "this cell is coloured" versus "use the default". */
            if (!VTERM_COLOR_IS_DEFAULT_FG(&cell.fg)) {
                VTermColor c = cell.fg;
                vterm_screen_convert_color_to_rgb(screen, &c);
                printf("  [colour] row=%d col=%d ch=%lc rgb=(%u,%u,%u)%s\n",
                       row, col, (wint_t)cell.chars[0], c.rgb.red, c.rgb.green, c.rgb.blue,
                       cell.attrs.bold ? " bold" : "");
            }
            for (int k = 0; cell.chars[k] && n < 60; ++k) line[n++] = (char)cell.chars[k];
        }
        line[n] = 0;
        printf("  |%s|\n", line);
    }

    /* 3. cursor position after an explicit move */
    VTermPos cursor; VTermState *state = vterm_obtain_state(vt);
    vterm_state_get_cursorpos(state, &cursor);
    printf("\ncursor at row=%d col=%d (expected 1,4)\n", cursor.row, cursor.col);

    /* 4. a keystroke must produce bytes for the host */
    vterm_keyboard_unichar(vt, 'x', VTERM_MOD_NONE);
    char out[16]; size_t got = vterm_output_read(vt, out, sizeof out);
    printf("typing 'x' produced %zu byte(s): '%.*s'\n", got, (int)got, out);
    vterm_keyboard_key(vt, VTERM_KEY_ENTER, VTERM_MOD_NONE);
    got = vterm_output_read(vt, out, sizeof out);
    printf("Enter produced %zu byte(s) (expect CR 0x0d): ", got);
    for (size_t i = 0; i < got; ++i) printf("%02x ", (unsigned char)out[i]);
    printf("\n");

    vterm_free(vt);
    return 0;
}
