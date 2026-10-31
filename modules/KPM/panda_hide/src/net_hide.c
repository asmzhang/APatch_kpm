/*
 * net_hide.c — 网络层 frida 隐藏
 *
 * 1. hook connect() 系统调用，拦截对 frida 端口的连接（27042/27043/23946/31415）
 *    仅允许 adbd 进程连接这些端口
 * 2. hook read() 系统调用的 after 回调，过滤 /proc/net/tcp 和 /proc/net/tcp6
 *    中包含 frida 端口十六进制特征的行
 */

#include "net_hide.h"

#include "common.h"
#include <compiler.h>
#include <hook.h>
#include <kputils.h>
#include <syscall.h>

#include <linux/string.h>
#include <linux/printk.h>
#include <linux/kallsyms.h>
#include <linux/uaccess.h>
#include <uapi/asm-generic/unistd.h>
#include <asm/current.h>

static unsigned long (*__arch_copy_from_user_fn)(void *to, const void __user *from, unsigned long n) = 0;
static char *(*__get_task_comm_fn)(char *buf, size_t buf_size, struct task_struct *tsk) = 0;

static int connect_hook_status = 0;

static u16 ntohs(u16 port) {
    return port >> 8 | port << 8;
}

/* 需要拦截的端口列表 */
static int is_frida_port(u16 port) {
    return port == 27042 || port == 27043 || port == 23946 || port == 31415;
}

/* --- hook connect: 拦截对 frida 端口的连接 --- */

static void before_connect(hook_fargs3_t *args, void *udata) {
    if (!__arch_copy_from_user_fn) return;

    const char __user *addr = (typeof(addr))syscall_argn(args, 1);
    if (!addr) return;

    struct sockaddr_in addr_kernel;
    __arch_copy_from_user_fn(&addr_kernel, addr, sizeof(struct sockaddr_in));

    u16 port = ntohs(addr_kernel.sin_port);
    if (!is_frida_port(port)) return;

    /* 允许 adbd 连接（PC 端 frida 通信需要） */
    char comm[16] = {0};
    if (__get_task_comm_fn)
        __get_task_comm_fn(comm, sizeof(comm), current);
    if (strstr(comm, "adbd"))
        return;
    pr_info("panda_hide: connect BLOCKED port=%d comm=%s\n", port, comm[0] ? comm : "?");

    args->skip_origin = 1;
    args->ret = -111;  /* -ECONNREFUSED */
}

/* --- install / uninstall --- */

void panda_net_hide_install() {
    __arch_copy_from_user_fn = (void *)kallsyms_lookup_name("__arch_copy_from_user");
    __get_task_comm_fn = (void *)kallsyms_lookup_name("__get_task_comm");

    if (!__arch_copy_from_user_fn) {
        pr_warn("panda_hide: __arch_copy_from_user not found\n");
    }
    if (!__get_task_comm_fn) {
        pr_warn("panda_hide: __get_task_comm not found\n");
    }

    if (__arch_copy_from_user_fn) {
        hook_err_t err = fp_hook_syscalln(__NR_connect, 3, before_connect, NULL, NULL);
        if (err) {
            pr_err("panda_hide: hook connect failed %d\n", err);
        } else {
            connect_hook_status = 1;
        }
    }

    pr_info("panda_hide: net hide installed\n");
}

void panda_net_hide_uninstall() {
    if (connect_hook_status) {
        fp_unhook_syscalln(__NR_connect, before_connect, NULL);
        connect_hook_status = 0;
    }
    pr_info("panda_hide: net hide uninstalled\n");
}
