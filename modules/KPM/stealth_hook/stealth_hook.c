/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * stealth_hook —— 主入口：syscall 通道 + 命令分发 + 生命周期
 *
 * 上游 xiaojianbang-stealth-hook 的 stealth_hook.c（301 行）移植。
 * 相对上游的差异（逐条见开发报告）：
 *   1. 通道号 285 -> 427（285 是 copy_file_range，IO 热路径）；
 *   2. 加受信 uid 表（上游任何 app 都能驱动模块，等于把内核写原语开给全系统）；
 *   3. MapsHide / PtraceSpoof 已并入 memcrc，本模块只保留 HWBP / PTE / Ghost；
 *   4. 去掉 KPM_DESCRIPTION 里的联系方式。
 */

#include <compiler.h>
#include <kpmodule.h>
#include <linux/printk.h>
#include <linux/string.h>
#include <uapi/asm-generic/unistd.h>
#include <syscall.h>
#include <kputils.h>
#include <asm/current.h>
#include <kallsyms.h>
#include <hook.h>
#include <linux/sched.h>
#include <linux/cred.h>
#include <linux/kernel.h>

#include "stealth_hook.h"

KPM_NAME("stealth_hook");
KPM_VERSION(MYKPM_VERSION);
KPM_LICENSE("GPL v2");
KPM_AUTHOR("xiaojianbang");
KPM_DESCRIPTION("HWBP / PTE / ghost memory toolkit (xiaojianbang-stealth-hook port)");

/* ============================================================
 * 信任模型
 * ============================================================ */

static uid_t sh_allow_uids[SH_ALLOW_MAX] = { 0, 2000 };
static int sh_allow_count = 2;

static inline uid_t sh_current_uid(void)
{
    struct cred *cred = *(struct cred **)((uintptr_t)current + task_struct_offset.cred_offset);
    return *(uid_t *)((uintptr_t)cred + cred_offset.uid_offset);
}

static bool sh_is_privileged(uid_t uid)
{
    int i;

    if (uid == 0) {
        return true;
    }

    for (i = 0; i < sh_allow_count; i++) {
        if (sh_allow_uids[i] == uid) {
            return true;
        }
    }

    return false;
}

static int sh_allow_uid(uid_t uid)
{
    if (sh_is_privileged(uid)) {
        return 0;
    }
    if (sh_allow_count >= SH_ALLOW_MAX) {
        return -1;
    }
    sh_allow_uids[sh_allow_count++] = uid;
    return 0;
}

static int sh_deny_uid(uid_t uid)
{
    int i;

    if (uid == 0) {
        return -1;  /* root 不可移除 */
    }

    for (i = 0; i < sh_allow_count; i++) {
        if (sh_allow_uids[i] == uid) {
            sh_allow_uids[i] = sh_allow_uids[--sh_allow_count];
            return 0;
        }
    }
    return -1;
}

static int sh_dump_allow(char *out, unsigned long out_size)
{
    int i;
    int n = 0;

    n += kf_snprintf(out + n, out_size - n, "allowlist:");
    for (i = 0; i < sh_allow_count && n < (int)out_size; i++) {
        n += kf_snprintf(out + n, out_size - n, " %u", sh_allow_uids[i]);
    }
    return n;
}

/* ============================================================
 * 子系统
 * ============================================================ */

/* ghost_mem.c */
extern long sh_ghost_alloc(uint64_t target_va, uint64_t size, uint32_t prot);
extern long sh_ghost_free(uint64_t va);
extern long sh_ghost_write(struct sh_ghost_write_request __user *req);
extern long sh_ghost_list(struct sh_ghost_info __user *out, long max);
extern void sh_ghost_init(void);
extern void sh_ghost_cleanup(void);

/* hwbp.c */
extern long sh_hwbp_hook(struct sh_hwbp_request __user *req);
extern long sh_hwbp_unhook(uint64_t target_addr, uint32_t tid);
extern long sh_hwbp_enable(uint64_t target_addr, uint32_t tid, int enable);
extern long sh_hwbp_query(struct sh_hwbp_query __user *req);
extern long sh_hwbp_set_override(struct sh_hwbp_override __user *req);
extern long sh_hwbp_set_skip(uint64_t target_addr, uint32_t skip_count);
extern void sh_hwbp_init(void);
extern void sh_hwbp_cleanup(void);

/* ============================================================
 * 命令分发
 * ============================================================ */

/* 5.10 才有、6.1 起被内核移除的厂商 hook 点。存在=PTE 有拦截能力的前提。 */
static void *sh_pte_rvh_tp;

static long sh_pte_query(void)
{
    return sh_pte_rvh_tp ? 1 : 0;
}

static long sh_dispatch(long cmd, long arg1, long arg2, long arg3, long arg4)
{
    /* 硬件断点能读任意进程内存、能改任意进程执行流：只给 uid 0。
     * shell(2000) 仍可用 ghost/status，但跨进程 hook 必须 root（AUD-b-1）。 */
    if (cmd >= SH_CMD_HWBP_HOOK && cmd <= SH_CMD_HWBP_SET_SKIP && sh_current_uid() != 0) {
        return -1;
    }

    (void)arg4;

    switch (cmd) {
    case SH_CMD_STATUS:
        return (long)SH_STATUS_MAGIC + SH_VERSION_CODE;

    case SH_CMD_ECHO:
        return arg1;

    case SH_CMD_PTE_QUERY:
        return sh_pte_query();

    case SH_CMD_PTE_HOOK:
    case SH_CMD_PTE_REARM:
    case SH_CMD_PTE_UNHOOK:
        /* 本移植只做能力探测：真正的缺页重定向还要用户态 DBI 把目标页重编译成
         * 搬家版页并给出 offset_map[1024]，否则钩子无处跳转。不做半套。 */
        sh_dbg("[stealth-hook] PTE cmd 0x%lx rejected (rvh_tp=%d)\n",
               cmd, sh_pte_rvh_tp ? 1 : 0);
        return -95; /* -EOPNOTSUPP */

    case SH_CMD_GHOST_ALLOC:
        return sh_ghost_alloc((uint64_t)arg1, (uint64_t)arg2, (uint32_t)arg3);

    case SH_CMD_GHOST_FREE:
        return sh_ghost_free((uint64_t)arg1);

    case SH_CMD_GHOST_WRITE:
        return sh_ghost_write((struct sh_ghost_write_request __user *)arg1);

    case SH_CMD_GHOST_LIST:
        return sh_ghost_list((struct sh_ghost_info __user *)arg1, arg2);

    case SH_CMD_HWBP_HOOK:
        return sh_hwbp_hook((struct sh_hwbp_request __user *)arg1);

    case SH_CMD_HWBP_UNHOOK:
        return sh_hwbp_unhook((uint64_t)arg1, (uint32_t)arg2);

    case SH_CMD_HWBP_ENABLE:
        return sh_hwbp_enable((uint64_t)arg1, (uint32_t)arg2, 1);

    case SH_CMD_HWBP_DISABLE:
        return sh_hwbp_enable((uint64_t)arg1, (uint32_t)arg2, 0);

    case SH_CMD_HWBP_QUERY:
        return sh_hwbp_query((struct sh_hwbp_query __user *)arg1);

    case SH_CMD_HWBP_SET_OVERRIDE:
        return sh_hwbp_set_override((struct sh_hwbp_override __user *)arg1);

    case SH_CMD_HWBP_SET_SKIP:
        return sh_hwbp_set_skip((uint64_t)arg1, (uint32_t)arg2);

    default:
        return -1;  /* 未实现：上游是 -1，保持一致 */
    }
}

/* ============================================================
 * syscall 通道
 * ============================================================ */

static void sh_syscall_before(hook_fargs6_t *args, void *udata)
{
    long magic = (long)syscall_argn(args, 0);

    if (magic != SH_SYSCALL_MAGIC) {
        return;  /* 非本模块调用，原样放行 */
    }

    long cmd  = (long)syscall_argn(args, 1);
    long arg1 = (long)syscall_argn(args, 2);
    long arg2 = (long)syscall_argn(args, 3);
    long arg3 = (long)syscall_argn(args, 4);
    long arg4 = (long)syscall_argn(args, 5);

    if (!sh_is_privileged(sh_current_uid())) {
        args->skip_origin = 1;
        args->ret = -1;
        return;
    }

    sh_dbg("[stealth-hook] cmd=0x%lx arg1=0x%lx arg2=0x%lx arg3=0x%lx arg4=0x%lx\n",
           cmd, arg1, arg2, arg3, arg4);

    args->skip_origin = 1;
    args->ret = sh_dispatch(cmd, arg1, arg2, arg3, arg4);
}

/* ============================================================
 * 生命周期
 * ============================================================ */

static long stealth_hook_init(const char *args, const char *event, void *__user reserved)
{
    hook_err_t err;

    (void)reserved;
    sh_dbg("[stealth-hook] init, args: %s, event: %s\n",
           args ? args : "(null)", event ? event : "(null)");

    sh_pte_rvh_tp = (void *)kallsyms_lookup_name("__tracepoint_android_rvh_do_mem_abort");
    sh_dbg("[stealth-hook] pte rvh tracepoint: %llx\n", (unsigned long long)sh_pte_rvh_tp);

    sh_ghost_init();
    sh_hwbp_init();

    err = inline_hook_syscalln(SH_SYSCALL_NR, 6, sh_syscall_before, 0, 0);
    if (err) {
        pr_err("[stealth-hook] hook syscall #%d failed: %d\n", SH_SYSCALL_NR, err);
        return -1;
    }

    sh_dbg("[stealth-hook] syscall bridge on #%d ready\n", SH_SYSCALL_NR);
    return 0;
}

static long stealth_hook_control(const char *args, char *__user out_msg, int outlen)
{
    char buf[128];
    int n = 0;
    int handled;

    (void)out_msg;
    (void)outlen;

    if (!args) {
        return -1;
    }

    handled = 0;
    if (strncmp(args, "allow:", 6) == 0) {
        uid_t uid = 0;
        const char *p = args + 6;
        while (*p >= '0' && *p <= '9') {
            uid = uid * 10 + (uid_t)(*p - '0');
            p++;
        }
        n += kf_snprintf(buf + n, sizeof(buf) - n, "allow %u rc=%d", uid, sh_allow_uid(uid));
        handled = 1;
    } else if (strncmp(args, "deny:", 5) == 0) {
        uid_t uid = 0;
        const char *p = args + 5;
        while (*p >= '0' && *p <= '9') {
            uid = uid * 10 + (uid_t)(*p - '0');
            p++;
        }
        n += kf_snprintf(buf + n, sizeof(buf) - n, "deny %u rc=%d", uid, sh_deny_uid(uid));
        handled = 1;
    } else if (strncmp(args, "allowlist", 9) == 0) {
        n += sh_dump_allow(buf + n, sizeof(buf) - n);
        handled = 1;
    } else if (strncmp(args, "status", 6) == 0) {
        n += kf_snprintf(buf + n, sizeof(buf) - n,
                         "stealth_hook v%x nr=%d", SH_VERSION_CODE, SH_SYSCALL_NR);
        n += sh_dump_allow(buf + n, sizeof(buf) - n);
        handled = 1;
    }

    if (!handled) {
        return -1;
    }

    if (out_msg && outlen > 0) {
        int len = n < outlen - 1 ? n : outlen - 1;
        compat_copy_to_user(out_msg, buf, len);
        compat_copy_to_user(out_msg + len, "\0", 1);
    }
    return 0;
}

static long stealth_hook_exit(void *__user reserved)
{
    (void)reserved;
    sh_dbg("[stealth-hook] exit\n");

    inline_unhook_syscalln(SH_SYSCALL_NR, sh_syscall_before, 0);
    sh_hwbp_cleanup();
    sh_ghost_cleanup();

    return 0;
}

KPM_INIT(stealth_hook_init);
KPM_CTL0(stealth_hook_control);
KPM_EXIT(stealth_hook_exit);
