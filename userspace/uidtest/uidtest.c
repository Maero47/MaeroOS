#include <unistd.h>
#include <stdio.h>
static char *const e[] = {
    "LD_LIBRARY_PATH=/lib:/disk/lib:/disk/firefox","DISPLAY=:0",
    "HOME=/home/user","USER=user",(char*)0 };
int main(void){
    printf("uidtest: dropping to uid 1000...\n");
    setgid(100); setuid(1000);
    printf("uidtest: uid=%d euid=%d, exec firefox --version\n", getuid(), geteuid());
    char *a[]={"/disk/firefox/firefox-bin","--version",(char*)0};
    execve(a[0],a,e);
    printf("uidtest: exec failed\n");
    return 1;
}
