#include "../include/unistd.h"
#include "../include/stdio.h"
#include "../include/string.h"
#include "../include/stdlib.h"

static void print_num(long v) {
    char tmp[20]; int tl=0;
    if (!v){tmp[tl++]='0';}
    else{while(v){tmp[tl++]='0'+(int)(v%10);v/=10;}for(int a=0,b=tl-1;a<b;a++,b--){char t=tmp[a];tmp[a]=tmp[b];tmp[b]=t;}}
    tmp[tl]='\0'; write(1,tmp,tl);
}

static void count_fd(int fd, long *lines, long *words, long *bytes) {
    *lines=0; *words=0; *bytes=0;
    char buf[4096]; int n; int in_word=0;
    while ((n=read(fd,buf,sizeof(buf)))>0) {
        *bytes+=n;
        for (int i=0;i<n;i++) {
            char c=buf[i];
            if (c=='\n') (*lines)++;
            if (c==' '||c=='\t'||c=='\n'||c=='\r') { in_word=0; }
            else { if(!in_word){(*words)++;in_word=1;} }
        }
    }
}

int main(int argc, char *argv[]) {
    int show_l=0,show_w=0,show_c=0;
    int ai=1;
    while (ai<argc && argv[ai][0]=='-' && argv[ai][1]) {
        char *f=argv[ai]+1;
        while(*f){if(*f=='l')show_l=1;else if(*f=='w')show_w=1;else if(*f=='c')show_c=1;f++;}
        ai++;
    }
    if (!show_l&&!show_w&&!show_c) { show_l=1;show_w=1;show_c=1; }

    long tl=0,tw=0,tc=0;
    int nfiles=argc-ai;
    if (nfiles==0) {
        long l=0,w=0,c=0;
        count_fd(0,&l,&w,&c);
        if(show_l){print_num(l);write(1," ",1);}
        if(show_w){print_num(w);write(1," ",1);}
        if(show_c){print_num(c);write(1," ",1);}
        write(1,"\n",1);
    } else {
        for (int i=ai;i<argc;i++) {
            int fd=open(argv[i],0);
            if (fd<0){write(2,argv[i],strlen(argv[i]));write(2,": open failed\n",14);continue;}
            long l=0,w=0,c=0; count_fd(fd,&l,&w,&c); close(fd);
            tl+=l;tw+=w;tc+=c;
            if(show_l){print_num(l);write(1," ",1);}
            if(show_w){print_num(w);write(1," ",1);}
            if(show_c){print_num(c);write(1," ",1);}
            write(1,argv[i],strlen(argv[i]));write(1,"\n",1);
        }
        if (nfiles>1) {
            if(show_l){print_num(tl);write(1," ",1);}
            if(show_w){print_num(tw);write(1," ",1);}
            if(show_c){print_num(tc);write(1," ",1);}
            write(1,"total\n",6);
        }
    }
    exit(0);
    return 0;
}
