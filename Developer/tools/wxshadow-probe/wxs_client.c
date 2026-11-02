/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * wxs_client —— wxshadow 探针的对照进程
 *
 * 起一页可读可写的匿名映射，写一段固定内容，把自己的 pid / 页地址写进
 * <outpath>，然后每隔 500 ms 读一次该地址前 16 字节并追加到 <logpath>。
 * 这样从 adb 侧就能看到「原始内容 → 影子内容 → 复原」的切换过程。
 *
 * 用法：wxs_client [outpath] [logpath] [秒数]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

#define PATTERN "ORIGINAL-wxshadow-p1"

int main(int argc, char **argv)
{
    const char *outpath = argc > 1 ? argv[1] : "/data/local/tmp/wxs_client.txt";
    const char *logpath = argc > 2 ? argv[2] : "/data/local/tmp/wxs_client.log";
    int secs = argc > 3 ? atoi(argv[3]) : 60;
    size_t pagesz = (size_t)sysconf(_SC_PAGESIZE);
    unsigned char *base;
    unsigned char *p;
    FILE *f;
    int i;
    int ticks;
    long pid = (long)getpid();

    base = mmap(NULL, pagesz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
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
    fclose(f);

    f = fopen(logpath, "w");
    if (!f) {
        perror("fopen log");
        return 1;
    }
    ticks = secs * 2;
    for (i = 0; i < ticks; i++) {
        int k;
        fprintf(f, "t=%d data=", i);
        for (k = 0; k < 16; k++)
            fprintf(f, "%02x", p[k]);
        fprintf(f, "\n");
        fflush(f);
        usleep(500 * 1000);
    }
    fclose(f);
    return 0;
}
