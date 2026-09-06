#include <ft2build.h>
#include FT_FREETYPE_H
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
int main(void){
    const char *fp = "/usr/share/fonts/DejaVuSans.ttf";
    int fd = open(fp, O_RDONLY);
    if (fd < 0) { printf("FT_FAIL open errno=%d\n", errno); return 1; }
    unsigned char hdr[8]={0}; int n = read(fd, hdr, 8);
    off_t sz = lseek(fd, 0, SEEK_END);
    printf("FT_OPEN ok fd=%d read=%d magic=%02x%02x%02x%02x size=%ld\n",
           fd, n, hdr[0],hdr[1],hdr[2],hdr[3], (long)sz);
    close(fd);
    FT_Library lib; FT_Face face;
    if (FT_Init_FreeType(&lib)) { printf("FT_FAIL init\n"); return 1; }
    FT_Error e = FT_New_Face(lib, fp, 0, &face);
    if (e) { printf("FT_FAIL newface err=%d\n", e); return 1; }
    FT_Set_Pixel_Sizes(face, 0, 24);
    if (FT_Load_Char(face, 'A', FT_LOAD_RENDER)) { printf("FT_FAIL loadchar\n"); return 1; }
    printf("FT_OK glyphs=%ld bitmap=%dx%d\n", face->num_glyphs,
           face->glyph->bitmap.width, face->glyph->bitmap.rows);
    return 0;
}
