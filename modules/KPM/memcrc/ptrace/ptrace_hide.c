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
#include <kallsyms.h>
#include <kputils.h>
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

/* ================= PTRACE_REGS：伪造 NT_ARM_HW_* 调试寄存器视图 =================
 *
 * 对手手法：反调试/反作弊进程用 PTRACE_GETREGSET + NT_ARM_HW_BREAK/WATCH 读目标
 * 线程的 DBGBVR/DBGBCR，看到我们挂的硬件断点就直接判定被调试。
 *
 * 做法：只劫持这两个 request/type 组合，给调用者一份自洽的“假账本”，完全不碰真实
 * 调试寄存器；其它 ptrace 请求一律放行（要阻断请用 PTRACE:uid 规则）。
 */

#define MC_PTRACE_GETREGSET 0x4204
#define MC_PTRACE_SETREGSET 0x4205
#define MC_NT_ARM_HW_BREAK  0x402
#define MC_NT_ARM_HW_WATCH  0x403

#define MC_MAX_BREAK   6
#define MC_MAX_WATCH   4
#define MC_BOOK_SLOTS  32

/* before → after 的传递标记（local 每次都是脏的，before 必须无条件写） */
#define MC_TAG_MAGIC   0x5a000000UL
#define MC_TAG_BREAK   1
#define MC_TAG_WATCH   2

struct mc_iovec {
    unsigned long iov_base;
    unsigned long iov_len;
};

/* 与内核 user_hwdebug_state 同布局：8 字节头 + N×16 字节寄存器 */
struct mc_hwdebug_state {
    unsigned int dbg_info;
    unsigned int pad;
    struct {
        unsigned long addr;
        unsigned int ctrl;
        unsigned int pad2;
    } dbg_regs[16];
};

struct mc_regs_slot {
    int used;
    int tid;
    int nbreak;
    int nwatch;
    struct { unsigned long addr; unsigned int ctrl; } bp[MC_MAX_BREAK];
    struct { unsigned long addr; unsigned int ctrl; } wp[MC_MAX_WATCH];
};

static struct mc_regs_slot mc_slots[MC_BOOK_SLOTS];
static int mc_slot_cursor;
static unsigned long (*mc_copy_from_user)(void *to, const void *from, unsigned long n);

static struct mc_regs_slot *mc_slot_get(int tid)
{
    int i;

    for (i = 0; i < MC_BOOK_SLOTS; i++) {
        if (mc_slots[i].used && mc_slots[i].tid == tid)
            return &mc_slots[i];
    }

    for (i = 0; i < MC_BOOK_SLOTS; i++) {
        if (!mc_slots[i].used) {
            memset(&mc_slots[i], 0, sizeof(mc_slots[i]));
            mc_slots[i].used = 1;
            mc_slots[i].tid = tid;
            return &mc_slots[i];
        }
    }

    /* 账本满了就覆盖游标位：反调试线程数有限，这个策略足够 */
    i = mc_slot_cursor++ % MC_BOOK_SLOTS;
    memset(&mc_slots[i], 0, sizeof(mc_slots[i]));
    mc_slots[i].used = 1;
    mc_slots[i].tid = tid;
    return &mc_slots[i];
}

static void mc_slots_clear(void)
{
    memset(mc_slots, 0, sizeof(mc_slots));
    mc_slot_cursor = 0;
}

/* 把用户写进来的寄存器状态记到假账本，返回 0 或负 errno */
static long mc_set_regs(struct mc_regs_slot *s, int is_break, struct mc_iovec *iov, int maxc)
{
    struct mc_hwdebug_state st;
    unsigned long len = iov->iov_len;
    int req, store, active = 0, i;

    if (len < sizeof(unsigned int) * 2)
        req = 0;
    else
        req = (int)((len - sizeof(unsigned int) * 2) / 16);
    if (req > 16)
        req = 16;
    store = req > maxc ? maxc : req;
    if (len > sizeof(st))
        len = sizeof(st);

    memset(&st, 0, sizeof(st));
    if (mc_copy_from_user(&st, (const void *)iov->iov_base, len))
        return -14; /* -EFAULT */

    if (is_break)
        memset(s->bp, 0, sizeof(s->bp));
    else
        memset(s->wp, 0, sizeof(s->wp));

    for (i = 0; i < store; i++) {
        unsigned long addr = st.dbg_regs[i].addr;
        unsigned int ctrl = st.dbg_regs[i].ctrl;

        if (is_break) {
            s->bp[i].addr = addr;
            s->bp[i].ctrl = ctrl;
        } else {
            s->wp[i].addr = addr;
            s->wp[i].ctrl = ctrl;
        }
        if (addr || ctrl)
            active++;
    }

    if (is_break)
        s->nbreak = active;
    else
        s->nwatch = active;

    /* 溢出测试：请求数超过硬件槽位就如实报 -ENOSPC */
    if (req > maxc)
        return -28; /* -ENOSPC */
    return 0;
}

static void mc_handle_set(hook_fargs4_t *args, int tid, int is_break, void *iov_user)
{
    struct mc_iovec iov;
    struct mc_regs_slot *s;
    int maxc = is_break ? MC_MAX_BREAK : MC_MAX_WATCH;

    args->skip_origin = true;

    if (!mc_copy_from_user) {
        /* 拿不到 copy_from_user 就明确报错，不伪装成成功 */
        args->ret = (uint64_t)-1;
        return;
    }

    if (mc_copy_from_user(&iov, iov_user, sizeof(iov))) {
        args->ret = (uint64_t)-14;
        return;
    }

    s = mc_slot_get(tid);
    if (!s) {
        args->ret = (uint64_t)-12;
        return;
    }

    args->ret = (uint64_t)mc_set_regs(s, is_break, &iov, maxc);
}

/*
 * GETREGSET 走「真实调用 + 事后擦除」：先让内核按原样填好 user_hwdebug_state
 * （这样 dbg_info / iov_len 与真实内核逐位一致，不会留下破绽），再把寄存器数组
 * 换成本线程的假账本。
 */
static void mc_scrub_get(int tid, int is_break, void *iov_user)
{
    struct mc_iovec iov;
    struct mc_hwdebug_state st;
    struct mc_regs_slot *s;
    unsigned long len;
    int n, cap, i;

    if (!mc_copy_from_user)
        return;

    if (mc_copy_from_user(&iov, iov_user, sizeof(iov)))
        return;

    len = iov.iov_len;
    if (len > sizeof(st))
        len = sizeof(st);
    if (len < sizeof(unsigned int) * 2)
        return;

    if (mc_copy_from_user(&st, (const void *)iov.iov_base, len))
        return;

    s = mc_slot_get(tid);
    if (!s)
        return;

    n = is_break ? s->nbreak : s->nwatch;
    cap = (int)((len - sizeof(unsigned int) * 2) / 16);
    if (n > cap)
        n = cap;

    for (i = 0; i < n; i++) {
        st.dbg_regs[i].addr = is_break ? s->bp[i].addr : s->wp[i].addr;
        st.dbg_regs[i].ctrl = is_break ? s->bp[i].ctrl : s->wp[i].ctrl;
    }
    for (i = n; i < 16; i++) {
        st.dbg_regs[i].addr = 0;
        st.dbg_regs[i].ctrl = 0;
    }

    compat_copy_to_user((void *)iov.iov_base, &st, (int)len);
}

/* after 回调：靠 local.data0 传递「这一笔 GET 需要擦除」的标记 */
static void syscall_after_ptrace(hook_fargs0_t *args, void *udata)
{
    unsigned long tag = (unsigned long)args->local.data0;

    if ((tag & 0xffffff00UL) != MC_TAG_MAGIC)
        return;
    if ((long)args->ret < 0)
        return;

    mc_scrub_get((int)args->local.data1, (tag & 0xff) == MC_TAG_BREAK,
                 (void *)args->local.data2);
}

static void mc_regs_init(void)
{
    if (mc_copy_from_user)
        return;

    mc_copy_from_user = (unsigned long (*)(void *, const void *, unsigned long))
        kallsyms_lookup_name("__arch_copy_from_user");

    if (mc_copy_from_user)
        pr_info("[yuuki] PTRACE_REGS ready (__arch_copy_from_user=%llx)\n",
                (unsigned long long)(unsigned long)mc_copy_from_user);
    else
        pr_warn("[yuuki] no __arch_copy_from_user, PTRACE_REGS will report -1\n");
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
                    size_t need = (size_t)(new_len - replace_len);

                    /* 扩容必须先确认 seq_file 还有余量，否则写到 m->buf + m->size 之外（AUD-a-3） */
                    if (m->count + need > m->size) {
                        pr_warn("[yuuki] seq buffer full, skip State rewrite\n");
                        return ret;
                    }
                    memmove(line_end + need, line_end,
                            m->count - (line_end - m->buf));
                    m->count += need;
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
    long request = (long)syscall_argn(args, 0);
    long type = (long)syscall_argn(args, 2);

    /* local 每笔都是脏值，必须无条件重置，否则 after 会误判 */
    args->local.data0 = 0;

    /* 调试寄存器视图伪造：只处理 GETREGSET/SETREGSET + NT_ARM_HW_*，其余放行 */
    if ((request == MC_PTRACE_GETREGSET || request == MC_PTRACE_SETREGSET) &&
        (type == MC_NT_ARM_HW_BREAK || type == MC_NT_ARM_HW_WATCH) &&
        shared_data_find_ptrace_regs_uid(current_uid) >= 0) {
        int is_break = (type == MC_NT_ARM_HW_BREAK);
        int tid = (int)syscall_argn(args, 1);
        void *iov_user = (void *)syscall_argn(args, 3);

        if (request == MC_PTRACE_SETREGSET) {
            mc_handle_set(args, tid, is_break, iov_user);
        } else {
            /* GET 放行真实调用，after 里擦成假账本（保住 dbg_info/iov_len） */
            args->local.data0 = MC_TAG_MAGIC | (is_break ? MC_TAG_BREAK : MC_TAG_WATCH);
            args->local.data1 = (uint64_t)(unsigned int)tid;
            args->local.data2 = (uint64_t)(unsigned long)iov_user;
        }
        return;
    }

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

        hook_err = fp_hook_syscalln(__NR_ptrace, 4, syscall_before_ptrace, syscall_after_ptrace, 0);
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
    fp_unhook_syscalln(__NR_ptrace, syscall_before_ptrace, syscall_after_ptrace);

    hook_err = HOOK_NOT_HOOK;
    pr_info("[yuuki] ptrace_hide hook uninstalled successfully\n");

    return true;
}

static inline bool control_hook(bool enable) {
    return enable ? install_hook() : uninstall_hook();
}

long ptrace_hide_init(void) {
    mc_regs_init();
    return control_hook(true) ? 0 : -1;
}

long ptrace_hide_exit(void) {
    bool ret = control_hook(false);
    mc_slots_clear();
    return ret ? 0 : -1;
}
