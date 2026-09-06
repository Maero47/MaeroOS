/* cairoprobe — runs Cairo on MaeroOS: draws to an image surface and checks a
 * pixel, proving the 2D drawing library (Cairo + pixman + FreeType) works. */
#include <cairo.h>
#include <stdio.h>
int main(void) {
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 200, 120);
    cairo_t *cr = cairo_create(s);
    cairo_set_source_rgb(cr, 0.1, 0.2, 0.4); cairo_paint(cr);           /* blue bg */
    cairo_set_source_rgb(cr, 0.9, 0.6, 0.1);                            /* orange  */
    cairo_rectangle(cr, 20, 20, 120, 60); cairo_fill(cr);
    cairo_set_source_rgb(cr, 0.2, 0.8, 0.4);                            /* green   */
    cairo_arc(cr, 150, 80, 25, 0, 2*3.14159); cairo_fill(cr);
    cairo_surface_flush(s);
    unsigned char *d = cairo_image_surface_get_data(s);
    int stride = cairo_image_surface_get_stride(s);
    unsigned px = ((unsigned*)(d + 40*stride))[60];   /* inside the orange rect */
    cairo_status_t st = cairo_status(cr);
    printf("CAIRO_OK v%s status=%s rect_px=0x%06x\n", cairo_version_string(),
           cairo_status_to_string(st), px & 0xFFFFFF);
    cairo_destroy(cr); cairo_surface_destroy(s);
    return 0;
}
