#include "debugger_hide.h"

#include <ktypes.h>
#include <hook.h>
#include <kputils.h>
#include <syscall.h>

#include <linux/printk.h>
#include <linux/kallsyms.h>
#include <linux/string.h>

#include "common.h"

static void *seq_put_decimal_ull_fn = 0;
static void *seq_puts_fn = 0;
static void *proc_pid_wchan_fn = 0;
static void *do_task_stat_fn = 0;

/* /proc/[pid]/status -> TracerPid: 0 */
static void before_seq_put_decimal_ull(hook_fargs3_t *args, void *udata) {
    if (strcmp((char *)args->arg1, "\nTracerPid:\t") == 0 && args->arg2 != 0) {
        args->arg2 = 0;
    }
}

/* /proc/[pid]/status -> S (sleeping) instead of t (tracing stop) */
static void before_seq_puts(hook_fargs2_t *args, void *udata) {
    if (strcmp((char *)args->arg1, "t (tracing stop)") == 0) {
        args->arg1 = (uint64_t)"S (sleeping)";
    }
}

/* /proc/[pid]/wchan -> 0 instead of ptrace_stop */
static void after_proc_pid_wchan(hook_fargs4_t *args, void *udata) {
    struct seq_file *m = (struct seq_file *)args->arg0;
    if (m && m->buf) {
        /* AUD-c-6：m->buf 不保证 NUL 结尾，按 count 做有界比较 */
        if (m->count >= 11 && memcmp(m->buf, "ptrace_stop", 11) == 0) {
            m->buf[0] = '0';
            m->buf[1] = '\0';
            m->count = 1;
        }
    }
}

static void after_do_task_stat(hook_fargs5_t *args, void *udata) {
    struct seq_file *m = (struct seq_file *)args->arg0;
    if (m && m->buf) {
        for (size_t i = 0; i + 2 < m->count; i++) {
            if (m->buf[i] == ')' && m->buf[i + 1] == ' ') {
                if (m->buf[i + 2] == 't')
                    m->buf[i + 2] = 'S';
                break;
            }
        }
    }
}

void panda_debugger_hide_install() {
    seq_put_decimal_ull_fn = (void *)kallsyms_lookup_name("seq_put_decimal_ull");
    seq_puts_fn = (void *)kallsyms_lookup_name("seq_puts");
    proc_pid_wchan_fn = (void *)kallsyms_lookup_name("proc_pid_wchan");
    do_task_stat_fn = (void *)kallsyms_lookup_name("do_task_stat");

    if (seq_put_decimal_ull_fn) {
        hook_err_t err = hook_wrap3(seq_put_decimal_ull_fn, before_seq_put_decimal_ull, NULL, NULL);
        if (err) {
            pr_err("panda_hide: hook seq_put_decimal_ull failed %d\n", err);
            seq_put_decimal_ull_fn = 0;
        }
    } else {
        pr_warn("panda_hide: seq_put_decimal_ull not found\n");
    }

    if (seq_puts_fn) {
        hook_err_t err = hook_wrap2(seq_puts_fn, before_seq_puts, NULL, NULL);
        if (err) {
            pr_err("panda_hide: hook seq_puts failed %d\n", err);
            seq_puts_fn = 0;
        }
    } else {
        pr_warn("panda_hide: seq_puts not found\n");
    }

    if (proc_pid_wchan_fn) {
        hook_err_t err = hook_wrap4(proc_pid_wchan_fn, NULL, after_proc_pid_wchan, NULL);
        if (err) {
            pr_err("panda_hide: hook proc_pid_wchan failed %d\n", err);
            proc_pid_wchan_fn = 0;
        }
    } else {
        pr_warn("panda_hide: proc_pid_wchan not found\n");
    }

    if (do_task_stat_fn) {
        hook_err_t err = hook_wrap5(do_task_stat_fn, NULL, after_do_task_stat, NULL);
        if (err) {
            pr_err("panda_hide: hook do_task_stat failed %d\n", err);
            do_task_stat_fn = 0;
        }
    } else {
        pr_warn("panda_hide: do_task_stat not found\n");
    }

    pr_info("panda_hide: debugger hide installed\n");
}

void panda_debugger_hide_uninstall() {
    if (seq_put_decimal_ull_fn)
        hook_unwrap_remove(seq_put_decimal_ull_fn, before_seq_put_decimal_ull, NULL, 1);
    if (seq_puts_fn)
        hook_unwrap_remove(seq_puts_fn, before_seq_puts, NULL, 1);
    if (proc_pid_wchan_fn)
        hook_unwrap_remove(proc_pid_wchan_fn, NULL, after_proc_pid_wchan, 1);
    if (do_task_stat_fn)
        hook_unwrap_remove(do_task_stat_fn, NULL, after_do_task_stat, 1);
    pr_info("panda_hide: debugger hide uninstalled\n");
}
