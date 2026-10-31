#include <hook.h>
#include <linux/printk.h>
#include <uapi/asm-generic/unistd.h>
#include <syscall.h>
#include <kputils.h>
#include "shared_data.h"

hook_err_t err = HOOK_NO_ERR;

// 为什么不直接使用komm通信呢？，因为被注入的往往是普通app，没有root权限
void before_getcwd(hook_fargs1_t *args, void *udata) {
    const char __user *path = (typeof(path))syscall_argn(args, 0);

    char cmd[1024];

    compat_strncpy_from_user(cmd, path, sizeof(cmd));
    // MAP:uid:src_path:dst_path
    shared_data_add_parsed(cmd);
    //pr_info("[yuuki] cmd: %s\n", cmd);

    //args->skip_origin = false;
}

long syscall_init() {

    err = inline_hook_syscalln(__NR_getcwd, 2, before_getcwd, 0, 0);

    if (err) {
        pr_err("[yuuki] Failed to hook getcwd syscall, error: %d\n", err);
        return err;
    }

    pr_info("[yuuki] Successfully hooked getcwd syscall\n");
    return 0;
}

long syscall_exit() {
    inline_unhook_syscalln(__NR_getcwd, before_getcwd, 0);
    pr_info("[yuuki] getcwd syscall uninstalled successfully\n");
    return 0;
}