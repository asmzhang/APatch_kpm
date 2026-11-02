/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * wxs_client —— wxshadow P0 探针的对照进程
 *
 * 起一页可读可写可执行的匿名映射，写一段固定内容，把自己的 pid、页地址
 * 和内容打印出来（同时写文件，方便 adb 侧读取），然后睡住不动。
 *
 * 用法：wxs_client [输出文件路径] [睡眠秒数]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

#define PATTERN "ORIGINAL-0123456789ABCDEF"

int main(int argc, char **argv)
{
    const char *outpath = argc > 1 ? argv[1] : "/data/local/tmp/wxs_client.txt";
    int secs = argc > 2 ? atoi(argv[2]) : 300;
    size_t pagesz = (size_t)sysconf(_SC_PAGESIZE);
    unsigned char *base;
    unsigned char *p;
    FILE *f;
    int i;
    long pid = (long)getpid();

    base = mmap(NULL, pagesz, PROT_READ | PROT_WRITE | PROT_EXEC,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    p = base + 0x100;
    memcpy(p, PATTERN, sizeof(PATTERN) - 1);

    f = fopen(outpath, "w");
    if (!f) {
        perror("fopen");
        return 1;
    }
    fprintf(f, "pid=%ld\n", pid);
    fprintf(f, "va=%p\n", (void *)p);
    fprintf(f, "pattern=%s\n", PATTERN);
    fprintf(f, "data=");
    for (i = 0; i < 16; i++)
        fprintf(f, "%02x", p[i]);
    fprintf(f, "\n");
    fclose(f);

    printf("pid=%ld va=%p pattern=%s\n", pid, (void *)p, PATTERN);
    fflush(stdout);
    sleep((unsigned)secs);
    return 0;
}
