/* whoami — print the effective user name. */
#include "../include/stdio.h"
#include "../include/unistd.h"
#include "../include/pwd.h"

int main(void) {
    int uid = geteuid();
    struct passwd *pw = getpwuid((uid_t)uid);
    if (!pw || !pw->pw_name) {
        fprintf(stderr, "whoami: cannot find name for user ID %d\n", uid);
        return 1;
    }
    printf("%s\n", pw->pw_name);
    return 0;
}
