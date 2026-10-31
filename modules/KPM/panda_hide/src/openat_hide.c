/*
 * openat_hide.c — 拦截 openat/faccessat 系统调用
 *
 * 当应用尝试打开或检测 frida 相关文件路径时，返回 -ENOENT 假装文件不存在。
 * 同时拦截 readlinkat，防止通过 /proc/self/exe 等路径泄露信息。
 */

#include "openat_hide.h"

#include <compiler.h>
#include <kpmodule.h>
#include <kputils.h>
#include <linux/printk.h>
#include <uapi/asm-generic/unistd.h>
#include <linux/uaccess.h>
#include <syscall.h>
#include <linux/string.h>
#include <asm/current.h>
#include <hook.h>
#include <linux/kallsyms.h>

#include "common.h"

static char *(*__get_task_comm_fn)(char *buf, size_t buf_size, struct task_struct *tsk) = 0;

static int hook_openat_status = 0;
static int hook_faccessat_status = 0;

/* 只拦截壳实际检测的具体路径，不用宽泛子串匹配 */
static int is_hidden_path(const char *path) {
    /* memfd 是 Frida 自己的内存文件描述符，放行 */
    if (strstr(path, "/memfd:"))
        return 0;

    const char *block_paths[] = {
        /* 梆梆壳检测的 frida-server 文件 */
        "re.frida.server",
        "frida-agent-32.so",
        "frida-agent-64.so",
        "frida-agent.so",
        /* frida gadget */
        "frida-gadget",
        /* linjector */
        "linjector",
    };
    for (int i = 0; i < sizeof(block_paths) / sizeof(block_paths[0]); i++) {
        if (strstr(path, block_paths[i]))
            return 1;
    }
    return 0;
}

/* --- hook openat: 拦截文件打开 --- */

static void before_openat(hook_fargs4_t *args, void *udata) {
    const char __user *filename = (const char __user *)syscall_argn(args, 1);
    if (!filename) return;

    char buf[256];
    long len = compat_strncpy_from_user(buf, filename, sizeof(buf));
    if (len <= 0) return;

    if (is_hidden_path(buf)) {
        pr_info("panda_hide: openat BLOCKED: %s\n", buf);
        args->ret = -2;  /* -ENOENT */
        args->skip_origin = 1;
    }
}

/* --- hook faccessat: 拦截文件存在性检测 --- */

static void before_faccessat(hook_fargs4_t *args, void *udata) {
    const char __user *filename = (const char __user *)syscall_argn(args, 1);
    if (!filename) return;

    char buf[256];
    long len = compat_strncpy_from_user(buf, filename, sizeof(buf));
    if (len <= 0) return;

    if (is_hidden_path(buf)) {
        pr_info("panda_hide: faccessat BLOCKED: %s\n", buf);
        args->ret = -2;  /* -ENOENT */
        args->skip_origin = 1;
    }
}

/* --- install / uninstall --- */

void panda_openat_hide_install() {
    hook_err_t err;

    __get_task_comm_fn = (void *)kallsyms_lookup_name("__get_task_comm");
    if (!__get_task_comm_fn)
        pr_warn("panda_hide: openat_hide: __get_task_comm not found\n");

    err = fp_hook_syscalln(__NR_openat, 4, before_openat, NULL, NULL);
    if (err) {
        pr_err("panda_hide: hook openat failed %d\n", err);
    } else {
        hook_openat_status = 1;
    }

    err = fp_hook_syscalln(__NR_faccessat, 4, before_faccessat, NULL, NULL);
    if (err) {
        pr_err("panda_hide: hook faccessat failed %d\n", err);
    } else {
        hook_faccessat_status = 1;
    }

    pr_info("panda_hide: openat hide installed\n");
}

void panda_openat_hide_uninstall() {
    if (hook_openat_status) {
        fp_unhook_syscalln(__NR_openat, before_openat, NULL);
        hook_openat_status = 0;
    }
    if (hook_faccessat_status) {
        fp_unhook_syscalln(__NR_faccessat, before_faccessat, NULL);
        hook_faccessat_status = 0;
    }
    pr_info("panda_hide: openat hide uninstalled\n");
}
