/* pangoprobe — runs Pango (text layout + HarfBuzz shaping + fontconfig + Cairo)
 * on MaeroOS: lays out real text with a real font and measures it. */
#include <pango/pangocairo.h>
#include <stdlib.h>
#include <stdio.h>
int main(void) {
    setenv("FONTCONFIG_PATH", "/etc/fonts", 1);
    setenv("G_SLICE", "always-malloc", 1);
    setenv("MALLOC_CHECK_", "0", 1);
    setenv("HOME", "/tmp", 1);
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 320, 80);
    cairo_t *cr = cairo_create(s);
    PangoLayout *layout = pango_cairo_create_layout(cr);
    pango_layout_set_text(layout, "MaeroOS GTK!", -1);
    PangoFontDescription *desc = pango_font_description_from_string("Sans 20");
    pango_layout_set_font_description(layout, desc);
    int w = 0, h = 0;
    pango_layout_get_pixel_size(layout, &w, &h);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_move_to(cr, 8, 8);
    pango_cairo_show_layout(cr, layout);
    cairo_surface_flush(s);
    printf("PANGO_OK v%s text='MaeroOS GTK!' w=%d h=%d\n",
           pango_version_string(), w, h);
    pango_font_description_free(desc);
    g_object_unref(layout); cairo_destroy(cr); cairo_surface_destroy(s);
    return 0;
}
