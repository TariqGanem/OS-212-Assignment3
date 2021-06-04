#include "kernel/param.h"
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"
#include "kernel/syscall.h"
#include "kernel/memlayout.h"
#include "kernel/riscv.h"
#define PAGESIZE 4096

void 
sanity()
{
    char *pages = malloc(PAGESIZE * 20);
    for(int i = 0; i < 20; i++) {
        pages[i * PAGESIZE] = i;
        printf("%d written to page %d\n", i, i);
    }
    for (int i = 0; i < 20; i++)
        printf("reading %d from page %d\n", i, pages[i * PAGESIZE]);
    free(pages);
}

void 
nfua_or_lapa()
{
    char *pages = malloc(PAGESIZE * 17);
    for (int i = 0; i < 16; i++){
        pages[i * PAGESIZE] = i;
    }
    // in order to update aging foreach page
    sleep(3);
    for (int i = 0; i < 15; i++){
        pages[i * PAGESIZE] = i;
    }
    sleep(3);
    pages[16 * PAGESIZE] = 16;
}

void 
forkCheck()
{
    char *pages = malloc(PAGESIZE * 17);
    for (int i = 0; i < 17; i++){
        pages[i * PAGESIZE] = i;
    }
    for (int i = 0; i < 17; i++){
        printf("pages[%d * PG_SIZE] = %d\n", i, pages[i * PAGESIZE]);
    }
    int pid = fork();
    if(pid == 0)
        for (int i = 0; i < 17; i++)
            printf("pages[%d * PG_SIZE] = %d\n", i, pages[i * PAGESIZE]);
    else{
        int status;
        wait(&status);
    }
}

int 
main()
{
    printf("Start Running Tests:\n");
    sanity();
    nfua_or_lapa();
    forkCheck();
    exit(0);
    printf("Everything is Done.\n");
}
