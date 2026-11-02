/* wxprobe 的客户端：在自己进程里执行一条用户态 BRK，看内核侧钩子是否被调到。
 *
 *   brk_client          执行 BRK #0x5758（探针注册的 imm）
 *   brk_client control  执行 BRK #0x1234（对照，没注册，应当吃 SIGTRAP）
 *
 * 钩子正确注册时，wxprobe 会越过这条 BRK 并把 pc 前移 4 字节，于是本进程继续往下跑；
 * 钩子没生效时内核照常投递 SIGTRAP，这里接住后退出码 3，避免把进程直接打死看不到输出。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

static void on_trap(int sig)
{
    (void)sig;
    /* 信号处理里只用 write，避免 stdio 状态不确定 */
    if (write(1, "SIGTRAP (no hook)\n", 18) < 0) { /* 忽略 */ }
    _exit(3);
}

int main(int argc, char **argv)
{
    int control = (argc > 1 && strcmp(argv[1], "control") == 0);

    signal(SIGTRAP, on_trap);

    printf("before brk (%s)\n", control ? "0x1234" : "0x5758");
    fflush(stdout);

    if (control) {
        asm volatile(".inst 0xd4246800"); /* BRK #0x1234 */
    } else {
        asm volatile(".inst 0xd42aeb00"); /* BRK #0x5758 */
    }

    printf("after brk (returned)\n");
    return 0;
}
