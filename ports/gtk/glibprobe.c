/* glibprobe — runs GLib (GHashTable/GString/GList) on MaeroOS, proving the
 * foundation library of the whole GTK stack works. */
#include <glib.h>
#include <stdio.h>
int main(void) {
    GHashTable *h = g_hash_table_new(g_str_hash, g_str_equal);
    g_hash_table_insert(h, (gpointer)"k", (gpointer)"v");
    const char *v = g_hash_table_lookup(h, "k");
    GString *s = g_string_new("");
    g_string_append_printf(s, "%d.%d.%d", GLIB_MAJOR_VERSION,
                           GLIB_MINOR_VERSION, GLIB_MICRO_VERSION);
    GList *l = NULL;
    l = g_list_append(l, "a"); l = g_list_append(l, "b"); l = g_list_append(l, "c");
    guint n = g_list_length(l);
    gchar *up = g_ascii_strup("maeros", -1);
    printf("GLIB_OK v%s ht=%s list=%u up=%s\n", s->str, v, n, up);
    fflush(stdout);
    g_free(up); g_hash_table_destroy(h); g_string_free(s, TRUE); g_list_free(l);
    return 0;
}
