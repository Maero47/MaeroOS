/*
 * test — evaluate expression, exit 0 if true, 1 if false
 * Also installed as [ (invoked as "[ EXPR ]")
 */
#include "../include/unistd.h"
#include "../include/string.h"
#include "../include/stdlib.h"
#include "../include/sys/stat.h"

static int do_test(char *argv[], int argc);

int main(int argc, char *argv[]) {
    /* If invoked as "[", strip trailing "]" */
    int last = argc - 1;
    if (last > 0 && strcmp(argv[last], "]") == 0) last--;
    exit(do_test(argv + 1, last));
    return 1;
}

static int file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static int file_is_regular(const char *path) {
    struct stat st;
    if (stat(path, &st) < 0) return 0;
    return (st.st_mode & 0170000) == 0100000;  /* S_IFREG */
}

static int file_is_dir(const char *path) {
    struct stat st;
    if (stat(path, &st) < 0) return 0;
    return (st.st_mode & 0170000) == 0040000;  /* S_IFDIR */
}

static int str_to_int(const char *s) { return atoi(s); }

static int do_test(char *argv[], int argc) {
    if (argc == 0) return 1;  /* empty → false */

    /* Unary operators */
    if (argc == 2) {
        const char *op  = argv[0];
        const char *arg = argv[1];
        if (strcmp(op,"-e")==0) return file_exists(arg) ? 0 : 1;
        if (strcmp(op,"-f")==0) return file_is_regular(arg) ? 0 : 1;
        if (strcmp(op,"-d")==0) return file_is_dir(arg) ? 0 : 1;
        if (strcmp(op,"-z")==0) return (arg[0]=='\0') ? 0 : 1;
        if (strcmp(op,"-n")==0) return (arg[0]!='\0') ? 0 : 1;
        if (strcmp(op,"!")==0)  return (arg[0]!='\0') ? 1 : 0;
    }

    /* ! test */
    if (argc >= 2 && strcmp(argv[0],"!")==0)
        return do_test(argv+1, argc-1) == 0 ? 1 : 0;

    /* Binary operators */
    if (argc == 3) {
        const char *a  = argv[0];
        const char *op = argv[1];
        const char *b  = argv[2];
        if (strcmp(op,"=")==0  || strcmp(op,"==")==0) return strcmp(a,b)==0 ? 0 : 1;
        if (strcmp(op,"!=")==0) return strcmp(a,b)!=0 ? 0 : 1;
        int ia=str_to_int(a), ib=str_to_int(b);
        if (strcmp(op,"-eq")==0) return ia==ib ? 0 : 1;
        if (strcmp(op,"-ne")==0) return ia!=ib ? 0 : 1;
        if (strcmp(op,"-lt")==0) return ia<ib  ? 0 : 1;
        if (strcmp(op,"-le")==0) return ia<=ib ? 0 : 1;
        if (strcmp(op,"-gt")==0) return ia>ib  ? 0 : 1;
        if (strcmp(op,"-ge")==0) return ia>=ib ? 0 : 1;
        if (strcmp(op,"-a")==0)  return (a[0]!='\0' && b[0]!='\0') ? 0 : 1;
        if (strcmp(op,"-o")==0)  return (a[0]!='\0' || b[0]!='\0') ? 0 : 1;
    }

    /* Single string: true if non-empty */
    if (argc == 1) return (argv[0][0] != '\0') ? 0 : 1;

    return 1;
}
