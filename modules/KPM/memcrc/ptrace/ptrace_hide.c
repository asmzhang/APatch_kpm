#include <linux/printk.h>
#include <syscall.h>
#include "compat.h"
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/string.h>
#include <linux/random.h>
#include <asm/current.h>
#include <linux/sched.h>
#include <linux/cred.h>
#include "ptrace_hide.h"
#include "shared_data.h"

static hook_err_t hook_err = HOOK_NOT_HOOK;

static int (*backup_proc_pid_status)(struct seq_file *m, struct pid_namespace *ns, struct pid *pid, struct task_struct *task);
static int (*backup_do_task_stat)(struct seq_file *m, struct pid_namespace *ns, struct pid *pid, struct task_struct *task, int whole);

static uid_t get_task_uid(struct task_struct* task) {
    struct cred* cred = *(struct cred**)((uintptr_t)task + task_struct_offset.cred_offset);
    uid_t uid = *(uid_t*)((uintptr_t)cred + cred_offset.uid_offset);
    return uid;
}

static bool is_ptrace_target_uid(uid_t uid) {
    struct shared_data_entry entry;
    unsigned int ptrace_count;
    unsigned int i;

    ptrace_count = shared_data_get_count_by_type(SHARED_DATA_TYPE_PTRACE_UID);

    if (ptrace_count == 0) {
        return false;
    }

    for (i = 0; i < ptrace_count; i++) {
        unsigned int global_index;
        int ret;

        ret = shared_data_get_type_entry_info(SHARED_DATA_TYPE_PTRACE_UID, i,
                                              &global_index, &entry);

        if (ret != SHARED_DATA_SUCCESS) {
            continue;
        }

        if (entry.data.ptrace_uid.uid == uid)
            return true;

    }

    return false;
}

static int rep_proc_pid_status(struct seq_file *m, struct pid_namespace *ns, struct pid *pid, struct task_struct *task){
    size_t old_count = m->count;
    int ret = backup_proc_pid_status(m, ns, pid, task);

    uid_t uid = get_task_uid(task);

    if (!is_ptrace_target_uid(uid))
        return ret;

    if (ret == 0 && m->count > old_count) {
        char *buf_start = m->buf + old_count;
        size_t len = m->count - old_count;

        char *state_pos = strnstr(buf_start, "State:", len);
        if (state_pos) {
            char *state_value = state_pos + 6;
            while (*state_value == ' ' || *state_value == '\t') {
                state_value++;
            }

            char *line_end = strchr(state_value, '\n');
            if (line_end) {
                int replace_len = line_end - state_value;
                char *replacement = "R (running)";
                int new_len = strlen(replacement);

                if (new_len < replace_len) {
                    memmove(state_value + new_len, line_end,
                            m->count - (line_end - m->buf));
                    m->count -= (replace_len - new_len);
                } else if (new_len > replace_len) {
                    memmove(line_end + (new_len - replace_len), line_end,
                            m->count - (line_end - m->buf));
                    m->count += (new_len - replace_len);
                }

                memcpy(state_value, replacement, new_len);
            }
        }

        buf_start = m->buf + old_count;
        len = m->count - old_count;

        char *tracer_pos = strnstr(buf_start, "TracerPid:", len);
        if (tracer_pos) {
            char *tracer_value = tracer_pos + 10;
            while (*tracer_value == ' ' || *tracer_value == '\t') {
                tracer_value++;
            }

            char *line_end = strchr(tracer_value, '\n');
            if (line_end) {
                char *num_end = tracer_value;
                while (num_end < line_end && (*num_end >= '0' && *num_end <= '9')) {
                    num_end++;
                }

                int num_len = num_end - tracer_value;

                if (num_len > 1 || *tracer_value != '0') {
                    if (num_len > 1) {
                        memmove(tracer_value + 1, num_end,
                                m->count - (num_end - m->buf));
                        m->count -= (num_len - 1);
                    }
                    *tracer_value = '0';
                }
            }
        }
    }

    return ret;
}

static int rep_do_task_stat(struct seq_file *m, struct pid_namespace *ns, struct pid *pid, struct task_struct *task, int whole){
    size_t old_count = m->count;
    int ret = backup_do_task_stat(m, ns, pid, task, whole);

    uid_t uid = get_task_uid(task);

    if (!is_ptrace_target_uid(uid))
        return ret;

    if (ret >= 0 && m->count > old_count) {
        char *buf_start = m->buf + old_count;

        char *paren_close = strchr(buf_start, ')');
        if (paren_close) {
            char *state_pos = paren_close + 1;
            while (*state_pos == ' ') {
                state_pos++;
            }

            if (*state_pos != 'R') {
                *state_pos = 'R';
            }
        }
    }

    return ret;
}

void syscall_before_ptrace(hook_fargs4_t *args, void *udata) {
    uid_t current_uid = get_task_uid(current);

    if (!is_ptrace_target_uid(current_uid))
        return;

    pr_info("[yuuki] Blocking ptrace for UID: %u\n", current_uid);
    args->ret = -EPERM;
    args->skip_origin = true;
}

static inline bool hook_stat() {
    if (ori_proc_pid_status && ori_do_task_stat) {

        hook_err = hook((void *)ori_proc_pid_status, (void *) rep_proc_pid_status, (void **)&backup_proc_pid_status);
        if (hook_err != HOOK_NO_ERR) {
            pr_err("[yuuki] hook proc_pid_status error: %d\n", hook_err);
            return false;
        }

        hook_err = hook((void *)ori_do_task_stat, (void *) rep_do_task_stat, (void **)&backup_do_task_stat);
        if (hook_err != HOOK_NO_ERR) {
            pr_err("[yuuki] hook do_task_stat error: %d\n", hook_err);
            unhook((void *)ori_proc_pid_status);
            return false;
        }

        hook_err = fp_hook_syscalln(__NR_ptrace, 4, syscall_before_ptrace, NULL, 0);
        if (hook_err != HOOK_NO_ERR) {
            pr_err("[yuuki] hook ptrace error: %d\n", hook_err);
            unhook((void *)ori_proc_pid_status);
            unhook((void *)ori_do_task_stat);
            return false;
        }

        return true;
    } else {
        hook_err = HOOK_BAD_ADDRESS;
        pr_err("[yuuki] no symbol: proc_pid_status or do_task_stat\n");
    }
    return false;
}

static inline bool install_hook() {
    bool ret = false;

    if (hook_err == HOOK_NO_ERR) {
        pr_info("[yuuki] ptrace_hide hook already installed, skipping...\n");
        return true;
    }

    if (hook_stat()) {
        pr_info("[yuuki] ptrace_hide hook installed successfully\n");
        ret = true;
    } else {
        pr_err("[yuuki] ptrace_hide hook installation failed\n");
    }

    return ret;
}

static inline bool uninstall_hook() {
    if (hook_err != HOOK_NO_ERR) {
        pr_info("[yuuki] ptrace_hide hook not installed, skipping uninstall...\n");
        return true;
    }

    unhook((void *)ori_proc_pid_status);
    unhook((void *)ori_do_task_stat);
    fp_unhook_syscalln(__NR_ptrace, syscall_before_ptrace, NULL);

    hook_err = HOOK_NOT_HOOK;
    pr_info("[yuuki] ptrace_hide hook uninstalled successfully\n");

    return true;
}

static inline bool control_hook(bool enable) {
    return enable ? install_hook() : uninstall_hook();
}

long ptrace_hide_init(void) {
    return control_hook(true) ? 0 : -1;
}

long ptrace_hide_exit(void) {
    return control_hook(false) ? 0 : -1;
}