/* Pin down libvterm's mouse encoding, which is undocumented and not the xterm
 * wire numbering an implementer would assume.
 *
 * Build:  gcc -o vterm-mouse-probe vterm-mouse-probe.c $(pkg-config --cflags --libs vterm)
 *
 * Measured against 0.3.3:
 *   libvterm 1/2/3      -> ESC[<0M / ESC[<1M / ESC[<2M     left, middle, right
 *   libvterm 4/5        -> ESC[<64M / ESC[<65M             wheel up, down
 *   button 0            -> NOTHING AT ALL
 *
 * So the wire value is the libvterm index minus one, and passing wire numbers
 * straight through swaps the buttons (left becomes silent, right arrives as
 * middle). Release is the same sequence with a lowercase final byte.
 *
 * THE TRAP THIS FILE EXISTS FOR: vterm_input_write CLEARS THE OUTPUT BUFFER, so a
 * read taken after enabling mouse reporting reports empty whether or not the mouse
 * produced anything. Enable first, read last. Three separate probes were wasted
 * concluding "the mouse API does nothing" before this was noticed.
 */
#include <vterm.h>
#include <stdio.h>
#include <string.h>
static char out[512];
static void show(VTerm *vt, const char *label) {
    size_t n = vterm_output_read(vt, out, sizeof out - 1);
    printf("  %-30s %2zu: ", label, n);
    for (size_t i = 0; i < n; ++i) { unsigned char c = out[i];
        if (c == 0x1b) printf("ESC "); else if (c < 32) printf("^%c ", c + 64); else printf("%c", c); }
    printf("\n");
}
int main(void) {
    VTerm *vt = vterm_new(10, 40);
    VTermScreen *screen = vterm_obtain_screen(vt);
    VTermScreenCallbacks cb = {0};
    vterm_screen_set_callbacks(screen, &cb, NULL);
    vterm_screen_reset(screen, 1);
    vterm_input_write(vt, "\x1b[?1000h\x1b[?1006h", 17);
    vterm_screen_flush_damage(screen);
    vterm_output_read(vt, out, sizeof out);

    for (int b = 0; b <= 6; ++b) {
        char label[32];
        snprintf(label, sizeof label, "libvterm button %d press", b);
        vterm_mouse_button(vt, b, true, VTERM_MOD_NONE);
        show(vt, label);
        vterm_mouse_button(vt, b, false, VTERM_MOD_NONE);
        show(vt, "        release");
    }
    vterm_free(vt);
    return 0;
}
