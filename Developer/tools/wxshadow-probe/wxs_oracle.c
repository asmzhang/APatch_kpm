/*
 * wxs_oracle —— 自检 oracle：期望值编译进代码，不回头读磁盘。
 *
 * 为什么不用 runtime-sense 的 own_text 做 A 组：它把「磁盘」实现成读 base.apk 里那段
 * 偏移，而 .so 是 stored 方式存在 APK 里、被直接 mmap 的 —— 内核线性映射写 page cache
 * 之后，它读到的就是被改过的同一页，于是「内存 == 磁盘」，永远 CLEAN。
 * 真实的自查必须把期望值（常量摘要/常量字节）编进代码，这里就是这么做的。
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* 精确控制指令，避免编译器插序言：目标函数体就是这三条。 */
__asm__(".globl oracle_target\n"
        ".p2align 2\n"
        ".type oracle_target, %function\n"
        "oracle_target:\n"
        "    add w0, w0, #1\n"
        "    add w0, w0, w0\n"
        "    ret\n"
        ".size oracle_target, .-oracle_target\n");

extern uint32_t oracle_target(uint32_t x);

/* 期望值：与上面三条指令逐字节一致（小端字序） */
static const uint32_t oracle_ref[] = {
    0x11000400u, /* add w0, w0, #1 */
    0x0B000000u, /* add w0, w0, w0 */
    0xD65F03C0u, /* ret            */
};

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    const void *addr = (const void *)(uintptr_t)&oracle_target;
    printf("oracle target=%p len=%zu expect_ret=%u\n", addr, sizeof(oracle_ref), 42u);

    for (int i = 0; i < 600; i++) {
        int same = memcmp(addr, oracle_ref, sizeof(oracle_ref)) == 0;
        printf("t=%d state=%s ret=%u\n", i, same ? "CLEAN" : "PATCHED",
               oracle_target(20u));
        sleep(2);
    }
    return 0;
}
