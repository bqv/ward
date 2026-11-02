/* Check the cursor theme without starting the client.
 *
 * Two questions this answers, both of which were guesses before it existed:
 *
 *   1. Which FILE does a cursor name resolve to? Qt 6's xcb platform does not
 *      read XCURSOR_THEME — it calls xcb_cursor_load_cursor(), and
 *      libxcb-cursor searches XCURSOR_PATH for a theme directory named
 *      "default", follows that directory's index.theme `Inherits=`, and looks
 *      for `<theme>/cursors/<name>`. When nothing matches it does NOT fail: it
 *      quietly builds a cursor from the X core cursor FONT, which is the 1980s
 *      bitmap arrow this theme exists to replace. So "the call succeeded" is
 *      not evidence — the file behind it is, and that is what -r prints.
 *
 *   2. What is actually inside a packed .cursor file? An Xcursor file is a
 *      container of per-size ARGB images with their own hotspots, and a hotspot
 *      that drifted with size would click in the wrong place. -i lists them.
 *
 * The theme search here mirrors libxcb-cursor's (it is four lines of path
 * walking, and re-deriving it is cheaper than tracing); the load itself is the
 * real call, so a disagreement between the two lines of output means the mirror
 * has drifted and this tool needs fixing.
 *
 * Build and run: see xcursor-probe.recipe.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <xcb/xcb.h>
#include <xcb/xcb_cursor.h>

/* --- container dump: no display needed ---------------------------------- */

#define XCURSOR_MAGIC 0x72756358u /* "Xcur" */
#define XCURSOR_IMAGE 0xfffd0002u

static int dump_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return 1;
    }

    unsigned int h[4];
    if (fread(h, 4, 4, f) != 4 || h[0] != XCURSOR_MAGIC) {
        fprintf(stderr, "%s: not an Xcursor file\n", path);
        fclose(f);
        return 1;
    }

    printf("%s: %u image(s)\n", path, h[3]);
    for (unsigned int i = 0; i < h[3]; i++) {
        unsigned int toc[3];
        if (fread(toc, 4, 3, f) != 3)
            break; /* truncated table: report what was read */
        long next = ftell(f);
        fseek(f, (long)toc[2], SEEK_SET);

        /* chunk header: size, type, subtype (nominal size), version,
           width, height, xhot, yhot, delay */
        unsigned int ch[9];
        if (fread(ch, 4, 9, f) == 9 && ch[1] == XCURSOR_IMAGE)
            printf("   %ux%u  nominal %u  hotspot (%u,%u)\n",
                   ch[4], ch[5], ch[2], ch[6], ch[7]);

        fseek(f, next, SEEK_SET);
    }
    fclose(f);
    return 0;
}

/* --- theme search: the path libxcb-cursor walks ------------------------- */

/* The list the library uses when XCURSOR_PATH is unset. Kept in step with
   `strings /usr/lib64/libxcb-cursor.so.0 | grep '^~/.cursors'`. */
static const char *default_path(void)
{
    return "~/.cursors:~/.icons:/usr/local/share/cursors/xorg-x11:/usr/local/share/cursors:"
           "/usr/local/share/icons:/usr/local/share/pixmaps:/usr/share/cursors/xorg-x11:"
           "/usr/share/cursors:/usr/share/pixmaps/xorg-x11:/usr/share/icons:/usr/share/pixmaps";
}

/* `Inherits=` from <dir>/index.theme, or NULL. First value only: a
   comma-separated chain is followed hop by hop, which is all the client needs. */
static char *inherits_of(const char *theme_dir)
{
    char path[4096];
    snprintf(path, sizeof path, "%s/index.theme", theme_dir);
    FILE *f = fopen(path, "r");
    if (!f)
        return NULL;

    static char value[256];
    char line[1024];
    value[0] = '\0';
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = '\0';
        if (strncmp(line, "Inherits", 8) != 0)
            continue;
        char *v = eq + 1;
        while (*v == ' ' || *v == '\t')
            v++;
        size_t n = strcspn(v, ",\r\n");
        if (n >= sizeof value)
            n = sizeof value - 1;
        memcpy(value, v, n);
        value[n] = '\0';
        break;
    }
    fclose(f);
    return value[0] ? value : NULL;
}

/* Print the file the loader would use for `name`, or say that there is none.
 *
 * Two passes, in the order the loader follows them:
 *   1. resolve the theme CHAIN (default -> its Inherits -> its Inherits, at
 *      most four hops), reading each index.theme from the first entry that
 *      carries it;
 *   2. search every XCURSOR_PATH entry for each theme in the chain.
 *
 * Doing it per entry instead loses an inherited theme as soon as the first
 * entry does not carry it — which is exactly this layout, where our own
 * themes/ directory inherits Adwaita from /usr/share/icons. */
static int report_file(const char *name)
{
    const char *env = getenv("XCURSOR_PATH");
    const char *path = env && *env ? env : default_path();

    char themes[4][256];
    int count = 0;
    snprintf(themes[count++], sizeof themes[0], "default");

    char theme[256] = "default";
    for (int hop = 0; hop < 3 && count < 4; hop++) {
        char dir[4200] = "";
        char copy[4096];
        snprintf(copy, sizeof copy, "%s", path);
        for (char *entry = strtok(copy, ":"); entry; entry = strtok(NULL, ":")) {
            char candidate[4200];
            snprintf(candidate, sizeof candidate, "%s/%s", entry, theme);
            if (inherits_of(candidate)) {
                snprintf(dir, sizeof dir, "%s", candidate);
                break;
            }
        }
        if (!dir[0])
            break;
        char *next = inherits_of(dir);
        if (!next || !*next)
            break;
        snprintf(theme, sizeof theme, "%s", next);
        snprintf(themes[count++], sizeof themes[0], "%s", theme);
    }

    char found[4200] = "";
    for (int t = 0; t < count && !found[0]; t++) {
        char copy[4096];
        snprintf(copy, sizeof copy, "%s", path);
        for (char *entry = strtok(copy, ":"); entry; entry = strtok(NULL, ":")) {
            char file[4200];
            snprintf(file, sizeof file, "%s/%s/cursors/%s", entry, themes[t], name);
            FILE *probe = fopen(file, "rb");
            if (probe) {
                fclose(probe);
                snprintf(found, sizeof found, "%s", file);
                break;
            }
        }
    }

    if (found[0])
        printf("   file: %s\n", found);
    else
        printf("   file: none -> libxcb-cursor builds a FONT cursor (the old bitmap arrow)\n");
    return found[0] ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr,
                "usage: %s -i FILE...    list the sizes and hotspots in a .cursor file\n"
                "       %s NAME...       show which file each name resolves to\n",
                argv[0], argv[0]);
        return 2;
    }

    if (strcmp(argv[1], "-i") == 0) {
        int rc = 0;
        for (int i = 2; i < argc; i++)
            rc |= dump_file(argv[i]);
        return rc;
    }

    printf("XCURSOR_PATH=%s\n",
           getenv("XCURSOR_PATH") ? getenv("XCURSOR_PATH") : "(unset: library default)");
    printf("note: xcb_cursor_load_cursor never fails - a missing file becomes a FONT\n"
           "      cursor - so the file line, not the load line, is the answer\n");

    /* The load itself, so the file line above can be cross-checked against the
       library's own answer. Both lines should agree: a file plus a NONE means
       the mirror has drifted. */
    xcb_connection_t *c = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(c)) {
        fprintf(stderr, "cannot reach $DISPLAY: file resolution is still valid\n");
        int rc = 0;
        for (int i = 1; i < argc; i++) {
            printf("%-20s\n", argv[i]);
            rc |= report_file(argv[i]);
        }
        return rc;
    }
    xcb_screen_t *screen = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
    xcb_cursor_context_t *ctx = NULL;
    if (xcb_cursor_context_new(c, screen, &ctx) < 0)
        ctx = NULL;

    int misses = 0;
    for (int i = 1; i < argc; i++) {
        printf("%-20s\n", argv[i]);
        misses |= report_file(argv[i]);
        if (ctx) {
            xcb_cursor_t cursor = xcb_cursor_load_cursor(ctx, argv[i]);
            if (cursor == XCB_CURSOR_NONE)
                printf("   load: NONE - no cursor at all\n");
            else
                printf("   load: cursor %u\n", cursor);
            if (cursor != XCB_CURSOR_NONE)
                xcb_free_cursor(c, cursor);
        }
    }

    if (ctx)
        xcb_cursor_context_free(ctx);
    xcb_disconnect(c);
    return misses ? 1 : 0;
}
