#include <asm/current.h>
#include <linux/sched.h>
#include <linux/cred.h>
#include <linux/printk.h>
#include <linux/string.h>
#include <kallsyms.h>
#include <kputils.h>
#include <syscall.h>
#include "portable.h"

/*
 * 手写字符串函数：<linux/string.h> 里的 strnstr / strspn / strpbrk / strsep
 * 在这个内核里是 inline 版，会展开成 kf_strnstr / kf_strspn / kf_strpbrk / kf_strsep
 * 的函数调用，而 LKM 兼容表里没有这四个 kf_* 槽位 —— 用了就整模块 unknown symbol。
 * 下面只依赖普通的逐字节比较，编译器可能把循环合成到 kf_memcpy / kf_strlen，
 * 这两个符号两形态都有。
 */

static int mc_char_in(char c, const char *set)
{
    const char *p = set;

    if (!p) {
        return 0;
    }

    while (*p) {
        if (*p == c) {
            return 1;
        }
        p++;
    }

    return 0;
}

char *mc_strnstr(const char *s, const char *needle, size_t len)
{
    size_t nlen = 0;
    size_t i;
    size_t j;

    if (!s || !needle) {
        return 0;
    }

    while (needle[nlen]) {
        nlen++;
    }

    if (nlen == 0) {
        return (char *)s;
    }

    if (len < nlen) {
        return 0;
    }

    for (i = 0; i + nlen <= len; i++) {
        for (j = 0; j < nlen; j++) {
            if (s[i + j] != needle[j]) {
                break;
            }
        }
        if (j == nlen) {
            return (char *)(s + i);
        }
    }

    return 0;
}

size_t mc_strspn(const char *s, const char *accept)
{
    size_t n = 0;

    if (!s) {
        return 0;
    }

    while (s[n] && mc_char_in(s[n], accept)) {
        n++;
    }

    return n;
}

char *mc_strpbrk(const char *s, const char *accept)
{
    if (!s) {
        return 0;
    }

    while (*s) {
        if (mc_char_in(*s, accept)) {
            return (char *)s;
        }
        s++;
    }

    return 0;
}

char *mc_strsep(char **sp, const char *delim)
{
    char *start;
    char *p;

    if (!sp) {
        return 0;
    }

    start = *sp;
    if (!start) {
        return 0;
    }

    p = start;
    while (*p && !mc_char_in(*p, delim)) {
        p++;
    }

    if (*p) {
        *p = 0;
        *sp = p + 1;
    } else {
        *sp = 0;
    }

    return start;
}

/*
 * uid 读取
 * -------
 * 内核形态有 KP 导出的 cred_offset（struct cred_offset），LKM 形态没有。
 * 因此这里不引用那个符号，而是从 init_cred（内核符号，两形态都能用 kallsyms 查到）
 * 里把 uid 的位置推导出来：
 *   struct cred 的前几个字段是 usage / uid / gid / suid / sgid / euid / egid /
 *   fsuid / fsgid / securebits（均为 32 位），init_cred 的 uid..fsgid 恒为 0，
 *   所以「前一个 4 字节非零（usage）、随后连续 8 个 4 字节为 0」的位置就是 uid。
 * 找到一个候选后还要用当前进程的 cred 交叉验证（该位置必须等于 current_uid()），
 * 校验不过就继续往后找；全程找不到就把偏移记为 -1，读不到 uid 时按「非目标」处理。
 * 这是从证据推导，不是按内核版本猜偏移。
 */
#define MC_CRED_SCAN_MAX 0x100

static int mc_uid_off = -2;

static int mc_derive_uid_offset(void)
{
    uintptr_t init_cred;
    uintptr_t current_cred;
    uid_t uid_now = mc_current_uid();
    int off;
    int i;

    init_cred = (uintptr_t)kallsyms_lookup_name("init_cred");
    if (!init_cred) {
        pr_warn("[yuuki] no init_cred, task uid unavailable\n");
        return -1;
    }

    current_cred = *(uintptr_t *)((uintptr_t)current + task_struct_offset.cred_offset);

    for (off = 4; off + 8 * 4 <= MC_CRED_SCAN_MAX; off += 4) {
        const unsigned int *p = (const unsigned int *)(init_cred + off);

        if (*(const unsigned int *)(init_cred + off - 4) == 0) {
            continue;
        }

        for (i = 0; i < 8; i++) {
            if (p[i] != 0) {
                break;
            }
        }
        if (i != 8) {
            continue;
        }

        if (current_cred && *(uid_t *)(current_cred + off) != uid_now) {
            continue;
        }

        return off;
    }

    pr_warn("[yuuki] cred uid offset not found\n");
    return -1;
}

uid_t mc_current_uid(void)
{
    return current_uid();
}

uid_t mc_task_uid(struct task_struct *task)
{
    uintptr_t cred;

    if (!task || task == current) {
        return mc_current_uid();
    }

    if (mc_uid_off == -2) {
        mc_uid_off = mc_derive_uid_offset();
        if (mc_uid_off >= 0) {
            pr_info("[yuuki] cred uid offset: %d\n", mc_uid_off);
        }
    }

    if (mc_uid_off < 0) {
        return (uid_t)-1;
    }

    cred = *(uintptr_t *)((uintptr_t)task + task_struct_offset.cred_offset);
    if (!cred) {
        return (uid_t)-1;
    }

    return *(uid_t *)(cred + mc_uid_off);
}

/*
 * syscall 钩子
 * -----------
 * 上游用 fp_hook_syscalln / inline_hook_syscalln，两者分别展开成
 * fp_wrap_syscalln / inline_wrap_syscalln，这两个符号只有内核集成形态导出。
 * syscalln_addr 两形态都导出（LKM 侧映射到 kp_kpm_syscalln_addr），
 * 拿到 __arm64_sys_xxx 的地址后再 hook_wrap 即可；narg 的取值与 KP 内部一致。
 */
hook_err_t mc_hook_syscall(int nr, int narg, void *before, void *after, void *udata)
{
    uintptr_t addr = syscalln_addr(nr, 0);

    if (!addr) {
        pr_err("[yuuki] resolve syscall #%d failed\n", nr);
        return (hook_err_t)-1;
    }

    return hook_wrap((void *)addr, has_syscall_wrapper ? 1 : narg, before, after, udata);
}

void mc_unhook_syscall(int nr, void *before, void *after)
{
    uintptr_t addr = syscalln_addr(nr, 0);

    if (addr) {
        hook_unwrap((void *)addr, before, after);
    }
}
