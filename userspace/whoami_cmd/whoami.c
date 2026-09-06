/* whoami — print the effective user name. */
#include "../include/stdio.h"
#include "../include/unistd.h"
#include "../include/pwd.h"

int main(void) {
    struct passwd *pw = getpwuid((uid_t)geteuid());
    printf("%s\n", pw && pw->pw_name ? pw->pw_name :
           (geteuid() == 0 ? "root" : "user"));
    return 0;
}
