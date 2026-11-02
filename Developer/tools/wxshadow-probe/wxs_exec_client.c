/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * wxs_exec_client —— wxshadow P2 执行视图的对照进程
 *
 * 把两条指令写进一页匿名可执行内存：
 *     mov w0, #1
 *     ret
 * 然后每 500 ms 调一次，把返回值追加到日志。装上影子页之后返回值应变成 2，
 * release 后回到 1 —— 这就是「目标进程执行的是影子页里的代码」。
 *
 * 用法：wxs_exec_client [outpath] [logpath] [秒数]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <signal.h>
#include <setjmp.h>

/* mov w0, #imm ; ret */
static const unsigned int CODE_ONE[2] = { 0x52800020u, 0xd65f03c0u };

static sigjmp_buf g_jmp;
static FILE *g_log;
static volatile int g_fault_hit;

static volatile int g_in_probe;

static void on_segv(int sig)
{
    (void)sig;
    g_fault_hit = 1;
    if (!g_in_probe) {
        if (g_log)
            fprintf(g_log, "segv outside probe (sig=%d), exiting\n", sig);
        _exit(9);
    }
    siglongjmp(g_jmp, 1);
}

int main(int argc, char **argv)
{
    const char *outpath = argc > 1 ? argv[1] : "/data/local/tmp/wxs_exec.txt";
    const char *logpath = argc > 2 ? argv[2] : "/data/local/tmp/wxs_exec.log";
    int secs = argc > 3 ? atoi(argv[3]) : 90;
    int nofault = (argc > 4 && argv[4][0] == 'n');
    size_t pagesz = (size_t)sysconf(_SC_PAGESIZE);
    unsigned char *base;
    unsigned int *code;
    int (*fn)(void);
    FILE *f;
    int i;
    int ticks;
    int last = -1;
    long pid = (long)getpid();

    base = mmap(NULL, pagesz, PROT_READ | PROT_WRITE | PROT_EXEC,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    code = (unsigned int *)(base + 0x100);
    memcpy(code, CODE_ONE, sizeof(CODE_ONE));
    __builtin___clear_cache((char *)code, (char *)code + sizeof(CODE_ONE));
    fn = (int (*)(void))code;

    f = fopen(outpath, "w");
    if (!f) {
        perror("fopen");
        return 1;
    }
    g_log = f;
    fprintf(f, "pid=%ld\n", pid);
    fprintf(f, "va=%p\n", (void *)code);
    fprintf(f, "expect=%d\n", fn());
    fclose(f);

    f = fopen(logpath, "w");
    if (!f) {
        perror("fopen log");
        return 1;
    }
    ticks = secs * 2;
    signal(SIGSEGV, on_segv);
    signal(SIGBUS, on_segv);
    for (i = 0; i < ticks; i++) {
        int v;
        if (i == 6 && !nofault) {
            /* 制造一次可控缺页：把这页设成 PROT_NONE 再读它，看内核侧能否记到。 */
            volatile unsigned char sink;
            if (mprotect(base, pagesz, PROT_NONE) == 0) {
                g_in_probe = 1;
                if (sigsetjmp(g_jmp, 1) == 0) {
                    sink = base[0x100];
                    (void)sink;
                    fprintf(f, "fault-probe: no fault (unexpected)\n");
                } else {
                    fprintf(f, "fault-probe: caught signal, hit=%d\n", g_fault_hit);
                }
                g_in_probe = 0;
                mprotect(base, pagesz, PROT_READ | PROT_WRITE | PROT_EXEC);
                __builtin___clear_cache((char *)code, (char *)code + sizeof(CODE_ONE));
            }
            fflush(f);
        }
        v = fn();
        if (v != last) {
            fprintf(f, "t=%d ret=%d rd=%02x%02x%02x%02x (changed)\n", i, v,
                    ((volatile unsigned char *)code)[0], ((volatile unsigned char *)code)[1],
                    ((volatile unsigned char *)code)[2], ((volatile unsigned char *)code)[3]);
            last = v;
        } else {
            fprintf(f, "t=%d ret=%d rd=%02x%02x%02x%02x\n", i, v,
                    ((volatile unsigned char *)code)[0], ((volatile unsigned char *)code)[1],
                    ((volatile unsigned char *)code)[2], ((volatile unsigned char *)code)[3]);
        }
        fflush(f);
        usleep(500 * 1000);
    }
    fclose(f);
    return 0;
}
