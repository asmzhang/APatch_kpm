#include <compiler.h>
#include <kpmodule.h>
#include <linux/printk.h>
#include <common.h>
#include <kputils.h>
#include <linux/string.h>

#include "debugger_hide.h"
#include "frida_hide.h"
#include "openat_hide.h"
#include "net_hide.h"
#include "mem_hide.h"

KPM_NAME("panda_hide");
KPM_VERSION(MYKPM_VERSION);
KPM_LICENSE("GPL v2");
KPM_AUTHOR("pandaos");
KPM_DESCRIPTION("panda_hide: comprehensive frida & debugger hiding.");

static long panda_hide_init(const char *args, const char *event, void *__user reserved)
{
    pr_info("panda_hide init, event: %s, args: %s\n", event ? event : "(null)", args ? args : "(null)");
    pr_info("kernelpatch version: %x\n", kpver);

    panda_debugger_hide_install();
    panda_frida_hide_install();
    panda_openat_hide_install();
    panda_net_hide_install();
    panda_mem_hide_install();

    pr_info("panda_hide: all modules installed\n");
    return 0;
}

static long panda_hide_control0(const char *args, char *__user out_msg, int outlen)
{
    pr_info("panda_hide control0, args: %s\n", args);
    char echo[64] = "echo: ";
    if (args)
        strncat(echo, args, sizeof(echo) - sizeof("echo: ") - 1);
    compat_copy_to_user(out_msg, echo, sizeof(echo));
    return 0;
}

static long panda_hide_control1(void *a1, void *a2, void *a3)
{
    return 0;
}

static long panda_hide_exit(void *__user reserved)
{
    pr_info("panda_hide exit\n");
    panda_mem_hide_uninstall();
    panda_net_hide_uninstall();
    panda_openat_hide_uninstall();
    panda_frida_hide_uninstall();
    panda_debugger_hide_uninstall();
    pr_info("panda_hide: all modules uninstalled\n");
    return 0;
}

KPM_INIT(panda_hide_init);
KPM_CTL0(panda_hide_control0);
KPM_CTL1(panda_hide_control1);
KPM_EXIT(panda_hide_exit);
