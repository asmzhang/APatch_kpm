#include <hook.h>
#include <asm/current.h>
#include <linux/sched.h>
#include <linux/cred.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/printk.h>
#include <uapi/asm-generic/unistd.h>
#include <syscall.h>
#include <kputils.h>
#include "control.h"
#include "kstring.h"
#include "shared_data.h"

hook_err_t err = HOOK_NO_ERR;

/*
 * 注入通道的信任模型
 * -----------------
 * getcwd 通道是给「被注入的普通 app」用的：它们没有 root，也不能走 komm / supercall。
 * 但通道对普通 app 开放，不等于让它改别人的视图，所以这里维护一份受信 uid 表：
 *   - 受信调用者可以给任意 uid / pid 下规则（MAP / PATH / PTRACE / RANGE 全部）；
 *   - 非受信调用者只能改自己（见 shared_data_add_parsed_scoped 的注释）。
 * 默认受信：0(root)、2000(shell)。可用 ctl0 的 ALLOW: / DENY: / ALLOWLIST 维护。
 */
#define CM_MAX_ALLOW 8

static uid_t cm_allow_uids[CM_MAX_ALLOW] = { 0, 2000 };
static int cm_allow_count = 2;

static inline uid_t cm_current_uid(void) {
    struct cred *cred = *(struct cred **)((uintptr_t)current + task_struct_offset.cred_offset);
    return *(uid_t *)((uintptr_t)cred + cred_offset.uid_offset);
}

static bool cm_is_privileged(uid_t uid) {
    int i;

    if (uid == 0) {
        return true;
    }

    for (i = 0; i < cm_allow_count; i++) {
        if (cm_allow_uids[i] == uid) {
            return true;
        }
    }

    return false;
}

int control_allow_uid(uid_t uid) {
    if (cm_is_privileged(uid)) {
        return 0;
    }

    if (cm_allow_count >= CM_MAX_ALLOW) {
        return -1;
    }

    cm_allow_uids[cm_allow_count++] = uid;
    pr_info("[yuuki] control: uid %u added to allow list\n", uid);
    return 0;
}

int control_deny_uid(uid_t uid) {
    int i, j;

    if (uid == 0) {
        return -1;   /* root 永远受信 */
    }

    for (i = 0; i < cm_allow_count; i++) {
        if (cm_allow_uids[i] == uid) {
            for (j = i; j < cm_allow_count - 1; j++) {
                cm_allow_uids[j] = cm_allow_uids[j + 1];
            }
            cm_allow_count--;
            pr_info("[yuuki] control: uid %u removed from allow list\n", uid);
            return 0;
        }
    }

    return 0;
}

int control_dump_allow(char *buf, unsigned long size) {
    int i, n = 0;

    if (!buf || size == 0) {
        return 0;
    }

    n += snprintf(buf + n, size - (unsigned long)n, "allow(%d):", cm_allow_count);
    for (i = 0; i < cm_allow_count && n > 0 && (unsigned long)n < size; i++) {
        n += snprintf(buf + n, size - (unsigned long)n, " %u", cm_allow_uids[i]);
    }

    return n;
}

int control_handle_command(const char *cmd, char *out, unsigned long out_size) {
    unsigned long value;

    if (!cmd) {
        return 0;
    }

    if (strncmp(cmd, "ALLOW:", 6) == 0) {
        if (kstring_strtoul(cmd + 6, 10, &value) != 0) {
            return SHARED_DATA_ERR_PARSE;
        }
        return control_allow_uid((uid_t)value) == 0 ? 1 : SHARED_DATA_ERR_FULL;
    }

    if (strncmp(cmd, "DENY:", 5) == 0) {
        if (kstring_strtoul(cmd + 5, 10, &value) != 0) {
            return SHARED_DATA_ERR_PARSE;
        }
        return control_deny_uid((uid_t)value) == 0 ? 1 : SHARED_DATA_ERR_INVALID;
    }

    if (strcmp(cmd, "ALLOWLIST") == 0) {
        if (out && out_size) {
            control_dump_allow(out, out_size);
        }
        return 1;
    }

    return 0;
}

// 为什么不直接使用komm通信呢？，因为被注入的往往是普通app，没有root权限
void before_getcwd(hook_fargs1_t *args, void *udata) {
    const char __user *path = (typeof(path))syscall_argn(args, 0);

    char cmd[1024];
    uid_t uid;

    if (!path) {
        return;
    }

    compat_strncpy_from_user(cmd, path, sizeof(cmd));
    cmd[sizeof(cmd) - 1] = '\0';

    /* getcwd 是高频调用：不是规则就立刻返回，别做后面的取值和加锁 */
    if (strncmp(cmd, "MAP:", 4) != 0 &&
        strncmp(cmd, "PATH:", 5) != 0 &&
        strncmp(cmd, "PTRACE:", 7) != 0 &&
        strncmp(cmd, "RANGE:", 6) != 0) {
        return;
    }

    uid = cm_current_uid();
    shared_data_add_parsed_scoped(cmd, uid, cm_is_privileged(uid));
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
