#include "../include/unistd.h"
#include "../include/stdio.h"
#include "../include/string.h"
#include "../include/stdlib.h"

static int flag_i = 0;  /* -i case insensitive */
static int flag_v = 0;  /* -v invert */
static int flag_n = 0;  /* -n line numbers */
static int flag_c = 0;  /* -c count only */

static int tolower_c(int c) {
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

static int str_contains(const char *haystack, const char *needle) {
    if (!*needle) return 1;
    for (; *haystack; haystack++) {
        const char *h = haystack, *n = needle;
        while (*h && *n) {
            int hc = flag_i ? tolower_c((unsigned char)*h) : (unsigned char)*h;
            int nc = flag_i ? tolower_c((unsigned char)*n) : (unsigned char)*n;
            if (hc != nc) break;
            h++; n++;
        }
        if (!*n) return 1;
    }
    return 0;
}

static int grep_fd(int fd, const char *pattern, const char *fname, int nfiles) {
    char buf[4096];
    char line[1024];
    int li = 0, lnum = 0, matches = 0;
    int n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n' || li >= (int)sizeof(line) - 1) {
                line[li] = '\0'; li = 0; lnum++;
                int hit = str_contains(line, pattern);
                if (flag_v) hit = !hit;
                if (hit) {
                    matches++;
                    if (!flag_c) {
                        if (nfiles > 1 && fname)  { write(1, fname, strlen(fname)); write(1, ":", 1); }
                        if (flag_n) {
                            int v=lnum; char tmp[12]; int tl=0;
                            if (!v) { tmp[tl++]='0'; }
                            else { while(v){tmp[tl++]='0'+v%10;v/=10;} for(int a=0,b=tl-1;a<b;a++,b--){char t=tmp[a];tmp[a]=tmp[b];tmp[b]=t;} }
                            tmp[tl]='\0';
                            write(1,tmp,tl); write(1,":",1);
                        }
                        write(1, line, strlen(line)); write(1, "\n", 1);
                    }
                }
                continue;
            }
            line[li++] = c;
        }
    }
    /* flush last line if no newline at end */
    if (li > 0) {
        line[li] = '\0'; lnum++;
        int hit = str_contains(line, pattern);
        if (flag_v) hit = !hit;
        if (hit) {
            matches++;
            if (!flag_c) {
                if (nfiles > 1 && fname) { write(1, fname, strlen(fname)); write(1, ":", 1); }
                if (flag_n) {
                    char tmp[12]; int tl=0, v=lnum;
                    if (!v){tmp[tl++]='0';}
                    else{while(v){tmp[tl++]='0'+v%10;v/=10;}for(int a=0,b=tl-1;a<b;a++,b--){char t=tmp[a];tmp[a]=tmp[b];tmp[b]=t;}}
                    tmp[tl]='\0'; write(1,tmp,tl); write(1,":",1);
                }
                write(1, line, strlen(line)); write(1, "\n", 1);
            }
        }
    }
    if (flag_c) {
        char tmp[16]; int tl=0, v=matches;
        if (!v){tmp[tl++]='0';}
        else{while(v){tmp[tl++]='0'+v%10;v/=10;}for(int a=0,b=tl-1;a<b;a++,b--){char t=tmp[a];tmp[a]=tmp[b];tmp[b]=t;}}
        tmp[tl]='\0'; write(1,tmp,tl); write(1,"\n",1);
    }
    return matches > 0 ? 0 : 1;
}

int main(int argc, char *argv[]) {
    int ai = 1;
    while (ai < argc && argv[ai][0] == '-' && argv[ai][1]) {
        char *f = argv[ai]+1;
        while (*f) {
            if (*f=='i') flag_i=1;
            else if (*f=='v') flag_v=1;
            else if (*f=='n') flag_n=1;
            else if (*f=='c') flag_c=1;
            f++;
        }
        ai++;
    }
    if (ai >= argc) { write(2, "usage: grep [-ivnc] PATTERN [FILE...]\n", 37); exit(2); }
    const char *pattern = argv[ai++];
    int nfiles = argc - ai;
    int ret = 1;
    if (nfiles == 0) {
        ret = grep_fd(0, pattern, (char *)0, 0);
    } else {
        for (int i = ai; i < argc; i++) {
            int fd = open(argv[i], 0);
            if (fd < 0) { write(2, argv[i], strlen(argv[i])); write(2, ": open failed\n", 14); continue; }
            int r = grep_fd(fd, pattern, argv[i], nfiles);
            if (r == 0) ret = 0;
            close(fd);
        }
    }
    exit(ret);
    return ret;
}
