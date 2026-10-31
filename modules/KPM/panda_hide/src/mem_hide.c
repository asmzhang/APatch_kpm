/*
 * mem_hide.c — 内存特征擦除
 *
 * hook access_remote_vm，在 /proc/[pid]/mem 读取返回的内核缓冲区中
 * 抹掉 Frida 特征字符串，防止检测 app 通过扫描进程内存发现 Frida。
 *
 * access_remote_vm(mm, addr, buf, len, gup_flags)
 *   - 被 mem_rw (fs/proc/base.c) 调用，处理 /proc/[pid]/mem 的 read
 *   - buf 是内核缓冲区，修改它不影响目标进程的实际内存
 *   - gup_flags & FOLL_WRITE 区分读/写，我们只处理读
 */

#include "mem_hide.h"
#include "common.h"
#include <hook.h>
#include <linux/string.h>
#include <linux/printk.h>
#include <linux/kallsyms.h>

static void *access_remote_vm_fn = 0;

#define FOLL_WRITE 0x01

/* 需要擦除的 Frida 内存特征 */
static const char *mem_sigs[] = {
    "LIBFRIDA",
    "frida-agent",
    "frida-gadget",
    "frida_agent",
    "frida-server",
    "re.frida.server",
    "frida:rpc",
    "gum-js-loop",
    "GumScript",
    "linjector",
};

#define NUM_SIGS (sizeof(mem_sigs) / sizeof(mem_sigs[0]))

/* 扫描内核缓冲区，将匹配的特征串用 0 覆盖 */
static void scrub_frida_signatures(char *buf, int len) {
    for (int s = 0; s < NUM_SIGS; s++) {
        const char *sig = mem_sigs[s];
        int sig_len = strlen(sig);
        if (sig_len > len) continue;

        for (int i = 0; i <= len - sig_len; i++) {
            if (memcmp(buf + i, sig, sig_len) == 0) {
                memset(buf + i, 0, sig_len);
                i += sig_len - 1;
            }
        }
    }
}

/*
 * after hook: access_remote_vm(mm, addr, buf, len, gup_flags)
 * 只在读操作时过滤，不影响 ptrace 写入
 */
static void after_access_remote_vm(hook_fargs5_t *args, void *udata) {
    unsigned int gup_flags = (unsigned int)args->arg4;
    if (gup_flags & FOLL_WRITE) return;

    char *buf = (char *)args->arg2;
    int len = (int)args->arg3;
    long ret = (long)args->ret;
    if (!buf || len <= 0) return;
    /* 只擦真实读到的字节：len 是请求长度，未填充部分是内核缓冲区里的旧数据 */
    if (ret <= 0) return;
    if (ret < len) len = (int)ret;

    scrub_frida_signatures(buf, len);
}

/* --- install / uninstall --- */

void panda_mem_hide_install() {
    access_remote_vm_fn = (void *)kallsyms_lookup_name("access_remote_vm");

    if (access_remote_vm_fn) {
        hook_err_t err = hook_wrap5(access_remote_vm_fn, NULL, after_access_remote_vm, NULL);
        if (err) {
            pr_err("panda_hide: hook access_remote_vm failed %d\n", err);
            access_remote_vm_fn = 0;
        }
    } else {
        pr_warn("panda_hide: access_remote_vm not found, mem hide skipped\n");
    }

    pr_info("panda_hide: mem hide installed\n");
}

void panda_mem_hide_uninstall() {
    if (access_remote_vm_fn)
        hook_unwrap_remove(access_remote_vm_fn, NULL, after_access_remote_vm, 1);
    pr_info("panda_hide: mem hide uninstalled\n");
}
