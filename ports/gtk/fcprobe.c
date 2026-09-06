#include <fontconfig/fontconfig.h>
#include <stdlib.h>
#include <stdio.h>
int main(void){
    setenv("FONTCONFIG_PATH","/etc/fonts",1); setenv("HOME","/tmp",1);
    if (!FcInit()) { printf("FC_FAIL init\n"); return 1; }
    FcPattern *p = FcNameParse((const FcChar8*)"Sans");
    FcConfigSubstitute(NULL, p, FcMatchPattern);
    FcDefaultSubstitute(p);
    FcResult r; FcPattern *m = FcFontMatch(NULL, p, &r);
    FcChar8 *file = NULL;
    if (m) FcPatternGetString(m, FC_FILE, 0, &file);
    printf("FC_OK match=%s\n", file ? (char*)file : "(none)");
    return 0;
}
