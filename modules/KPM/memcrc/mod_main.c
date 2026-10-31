#include <compiler.h>
#include <kpmodule.h>
#include <linux/printk.h>
#include <linux/kernel.h>
#include <linux/err.h>
#include <linux/string.h>
#include <kputils.h>
#include "./komm/komm.h"
#include "./control/control.h"
#include "./redirect/redirect.h"
#include "./feature/feature.h"
#include "./ptrace/ptrace_hide.h"
#include "./data/parser.h"
#include "symbols.h"

KPM_NAME("memcrc");
KPM_VERSION(MYKPM_VERSION);
KPM_AUTHOR("yuuki");
KPM_DESCRIPTION("original intention is to bypass the memory crc check");

static long mod_init(const char *args, const char *event, void *__user reserved){
    pr_info("[yuuki] Initializing...\n");
    if(init_symbols()) {
        pr_info("[yuuki] init_symbols failed\n");
        goto exit;
    }

    ioctl_init();
    feature_init();
    redirect_init();
    syscall_init();
    ptrace_hide_init();

    exit:
    return 0;
}

/*
 * control0 与 /dev/yuuki_misc 共用同一个 parser：args 是内核侧字符串，
 * 既可以是 CMD:xxx 管理命令，也可以是 MAP:/PATH:/PTRACE: 数据条目。
 * 返回：数据条目数（添加成功）或 0（命令成功），<0 为 parser 错误码。
 */
/*
 * control0 与 /dev/yuuki_misc 共用同一个入口：args 既可以是管理命令
 * （CMD:xxx / ALLOW: / DENY: / ALLOWLIST），也可以是 MAP:/PATH:/PTRACE:/RANGE: 数据条目。
 * 返回：数据条目数（添加成功）或 0（命令成功），<0 为错误码。
 */
static long mod_control0(const char *args, char __user *out_msg, int outlen) {
    char buf[256];
    int rc;
    int handled;

    if (!args || !args[0]) {
        return 0;
    }

    buf[0] = '\0';
    handled = control_handle_command(args, buf, sizeof(buf));

    if (handled != 0) {
        rc = handled > 0 ? 0 : handled;
    } else if (parser_is_command(args)) {
        rc = parser_process_command(args);
    } else {
        rc = shared_data_add_parsed(args);
    }

    if (out_msg && outlen > 0) {
        int len;

        if (buf[0]) {
            snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " rc=%d", rc);
        } else {
            snprintf(buf, sizeof(buf), "rc=%d", rc);
        }

        len = (int)strlen(buf);
        if (len > outlen) {
            len = outlen;
        }
        if (len > 0) {
            compat_copy_to_user(out_msg, buf, len);
        }
    }

    return rc;
}

static long mod_exit(void *__user reserved) {
    pr_info("[yuuki] mod_exit, uninstalled hook.\n");
    ioctl_exit();
    feature_exit();
    redirect_exit();
    syscall_exit();
    ptrace_hide_exit();
    return 0;
}

KPM_INIT(mod_init);
KPM_CTL0(mod_control0);
KPM_EXIT(mod_exit);
