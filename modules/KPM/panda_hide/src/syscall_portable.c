/*
 * syscall_portable.c — 两种装载形态通用的 syscall 挂钩
 *
 * 为什么不直接用 KP 的 syscall 钩子 API：
 *   - inline_wrap_syscalln / fp_wrap_syscalln 只在 kernel 形态导出，
 *     LKM 形态（原厂内核 + kernelpatch.ko）解析不到，模块会装不上；
 *   - hook_syscalln 两种形态都有，但 kernel 形态下它优先走全局 dispatcher
 *     （el0_svc_common 入口，注册时 allow_skip=0），回调里的 skip_origin
 *     不被采纳，靠它“假装文件不存在”的阻断会失效。
 *
 * 所以这里只用两种形态都导出的 hook_wrap / hook_unwrap 与
 * kallsyms_lookup_name：自己解析 __arm64_sys_xxx 地址后做 inline hook，
 * 与 LKM 形态下 hook_syscalln 的既有行为一致，skip_origin 在两种形态都成立。
 */

#include <kpmodule.h>
#include <kputils.h>
#include <linux/printk.h>
#include <linux/kallsyms.h>
#include <hook.h>
#include <syscall.h>

#include "common.h"

#define PANDA_SYSCALL_NAME_MAX 96

static uintptr_t panda_lookup_name3(const char *prefix, const char *name, const char *suffix) {
    char buffer[PANDA_SYSCALL_NAME_MAX];
    size_t n = 0;

    for (const char *p = prefix; *p && n + 1 < sizeof(buffer); p++) buffer[n++] = *p;
    for (const char *p = name; *p && n + 1 < sizeof(buffer); p++) buffer[n++] = *p;
    for (const char *p = suffix; *p && n + 1 < sizeof(buffer); p++) buffer[n++] = *p;
    buffer[n] = 0;

    return (uintptr_t)kallsyms_lookup_name(buffer);
}

/*
 * 与 KP 的 syscalln_name_addr() 同样的候选顺序：CFI 内核上 __arm64_sys_xxx
 * 可能是跳转表桩（.cfi_jt），真正的函数体在 .cfi 后缀上，所以按
 * .cfi_jt -> .cfi -> 无后缀 依次尝试，前缀先带 __arm64_ 再去掉。
 */
uintptr_t panda_syscall_addr(const char *name) {
    static const char *prefixes[] = {"__arm64_", ""};
    static const char *suffixes[] = {".cfi_jt", ".cfi", ""};

    for (int i = 0; i < 2; i++) {
        for (int j = 0; j < 3; j++) {
            uintptr_t addr = panda_lookup_name3(prefixes[i], name, suffixes[j]);
            if (addr) return addr;
        }
    }
    return 0;
}

hook_err_t panda_hook_syscall(const char *name, int narg, void *before, void *after, void *udata) {
    uintptr_t addr = panda_syscall_addr(name);
    if (!addr) {
        pr_err("panda_hide: syscall %s not found\n", name);
        return -1;
    }
    /* arm64 GKI 的 __arm64_sys_xxx 只收一个 struct pt_regs * 参数，
     * 参数个数必须按 1 记账，syscall_argn() 才不会读错位置。 */
    if (has_syscall_wrapper) narg = 1;
    return hook_wrap((void *)addr, narg, before, after, udata);
}

void panda_unhook_syscall(const char *name, void *before, void *after) {
    uintptr_t addr = panda_syscall_addr(name);
    if (addr) hook_unwrap((void *)addr, before, after);
}
