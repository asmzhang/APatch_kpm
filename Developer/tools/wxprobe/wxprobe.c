/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * wxprobe —— register_user_break_hook 的 ABI 探针（一次性诊断，不入模块体系）
 *
 * 目的：wxshadow 移植前唯一无法从源码判断的事，是内核 struct break_hook 的布局
 * 是否与上游那份 { list_head node; fn; u16 imm; u16 mask; } 一致。源码（v5.10 与
 * v6.1 的 arch/arm64/include/asm/debug-monitors.h）两边一致，但按本仓规矩：
 * 板子上的证据才算证据。这个模块只做三件事：
 *   1. kallsyms 解析 register_user_break_hook / unregister_user_break_hook；
 *   2. 按候选布局注册一个 imm=0x5758 的用户态 BRK 钩子；
 *   3. ctl0 回读「回调是否被调到」——被调到即布局正确。
 *
 * 客户端 brk_client 在自己进程里执行 BRK #0x5758 触发；没有钩子时进程会吃 SIGTRAP，
 * 客户端自己接住并报告，所以失败不会崩内核。
 */

#include <log.h>
#include <compiler.h>
#include <kpmodule.h>
#include <kputils.h>
#include <kallsyms.h>
#include <stdint.h>
#include <asm/ptrace.h>
#include <linux/list.h>
#include <linux/printk.h>
#include <linux/string.h>
#include <linux/kernel.h>

KPM_NAME("wxprobe");
KPM_VERSION("0.0.1");
KPM_LICENSE("GPL v2");
KPM_AUTHOR("APatch_kpm");
KPM_DESCRIPTION("ABI probe: struct break_hook layout for register_user_break_hook");

#define WXP_BRK_IMM 0x5758u

/* 候选布局，与 linux v5.10 / v6.1 的 arch/arm64/include/asm/debug-monitors.h 同序：
 *   struct list_head node;      offset 0x00
 *   int (*fn)(...);             offset 0x10
 *   u16 imm;                    offset 0x18
 *   u16 mask;                   offset 0x1a
 * esr 参数在 5.10 是 unsigned int、6.1 是 unsigned long，这里用 unsigned long 接，
 * 5.10 上高位可能是垃圾，回调里只取低 32 位用。
 */
struct wxp_break_hook {
    struct list_head node;
    int (*fn)(struct pt_regs *regs, unsigned long esr);
    unsigned short imm;
    unsigned short mask;
};

static struct wxp_break_hook wxp_hook;
static void (*wxp_register)(struct wxp_break_hook *hook);
static void (*wxp_unregister)(struct wxp_break_hook *hook);

static int wxp_registered;
static unsigned long wxp_hits;
static unsigned long wxp_last_pc;
static unsigned long wxp_last_esr;

static int wxp_brk_fn(struct pt_regs *regs, unsigned long esr)
{
    wxp_hits++;
    wxp_last_esr = esr;
    wxp_last_pc = (unsigned long)regs->pc;
    regs->pc += 4; /* 越过 BRK，让用户进程继续执行；不这么做会一直重入 */
    logki("wxprobe: BRK hit pc=%llx esr=%llx\n",
          (unsigned long long)wxp_last_pc, (unsigned long long)esr);
    return 0; /* DBG_HOOK_HANDLED */
}

static long wxp_init(const char *args, const char *event, void *__user reserved)
{
    wxp_hits = 0;
    wxp_last_pc = 0;
    wxp_last_esr = 0;
    wxp_registered = 0;

    wxp_register = (void (*)(struct wxp_break_hook *))kallsyms_lookup_name("register_user_break_hook");
    wxp_unregister = (void (*)(struct wxp_break_hook *))kallsyms_lookup_name("unregister_user_break_hook");

    if (!wxp_register || !wxp_unregister) {
        logke("wxprobe: register/unregister_user_break_hook not found (reg=%llx unreg=%llx)\n",
              (unsigned long long)(uintptr_t)wxp_register, (unsigned long long)(uintptr_t)wxp_unregister);
        return -1;
    }

    memset(&wxp_hook, 0, sizeof(wxp_hook));
    wxp_hook.fn = wxp_brk_fn;
    wxp_hook.imm = (unsigned short)WXP_BRK_IMM;
    /* 匹配式是 (comment & ~mask) == imm（v5.10 debug-monitors.c:318、v6.1:317），
     * mask 里置 1 的位表示"不比较"。要精确匹配 0x5758 必须 mask=0；
     * 先前写 0xffff 让 ~mask=0xffff0000，等于只比较高位，永不成立。 */
    wxp_hook.mask = 0x0000;

    wxp_register(&wxp_hook);
    wxp_registered = 1;

    logki("wxprobe: registered hook imm=0x%04x mask=0x%04x fn=%llx\n",
          WXP_BRK_IMM, 0x0000, (unsigned long long)(uintptr_t)wxp_brk_fn);
    return 0;
}

static long wxp_control0(const char *args, char *__user out_msg, int outlen)
{
    char buf[160];
    int n;

    if (args && args[0] && strncmp(args, "reset", 5) == 0) {
        wxp_hits = 0;
    }

    n = snprintf(buf, sizeof(buf),
                 "wxprobe v0.0.1 reg=%d imm=0x%04x hits=%lu pc=%llx esr=%llx",
                 wxp_registered, WXP_BRK_IMM, wxp_hits,
                 (unsigned long long)wxp_last_pc, (unsigned long long)wxp_last_esr);

    if (out_msg && outlen > 0) {
        int len = n;
        if (len > outlen - 1) len = outlen - 1;
        if (len > 0) compat_copy_to_user(out_msg, buf, (int)len);
    }

    return 0;
}

static long wxp_exit(void *__user reserved)
{
    if (wxp_registered && wxp_unregister) {
        wxp_unregister(&wxp_hook);
        wxp_registered = 0;
        logki("wxprobe: unregistered\n");
    }
    return 0;
}

KPM_INIT(wxp_init);
KPM_CTL0(wxp_control0);
KPM_EXIT(wxp_exit);
