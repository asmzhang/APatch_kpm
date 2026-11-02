#include <hook.h>
#include "compat.h"
#include <linux/printk.h>
#include <kputils.h>
#include <linux/string.h>
#include <linux/sched.h>
#include <linux/cred.h>
#include "feature.h"
#include "shared_data.h"
#include "portable.h"

/*
 * ★v0.2.4：五个钩子从「整函数替换（hook）」改成「链式 before/after（hook_wrap）」。
 * 原因：hook() 要在原始函数头部重定位，而那里已经被别人打上了跳转（例：5.10 LKM 上
 * mount_hidder 先用 hook_wrap2 钩住 show_vfsmnt），于是直接返回 -HOOK_BAD_RELO；
 * 链式钩子按 KP 的 hook_chain 追加，多个模块可以挂在同一函数上。
 *
 * local 槽位约定（hook_fargs 的 local 是整条链共享的）：
 *   data0 -> mount_hidder（hide_mount.c）
 *   data7 -> memcrc（本文件），存 count + 1，0 表示无效
 * 新模块请从 data1..data6 里取，并更新 docs/process/09-load-forms.md。
 */
#define MC_LOCAL_COUNT_SLOT data7

/* 这些函数都是「调原函数」之后再修改缓冲区，所以天然适合 after 回调。 */
static int hook_installed;

static inline bool is_proc_effective(void) {
    struct mm_struct *mm = NULL;

    if (!kf_get_task_mm || !current) {
        return false;
    }

    mm = kf_get_task_mm(current);
    if (mm) {
        kf_mmput(mm);
        return true;
    }

    return false;
}

static char get_random_char(void) {
    return RANDOM_CHARS[get_random_u64() % RANDOM_CHARS_LEN];
}

static void replace_sensitive(char *str, size_t len, const char *keyword, size_t keyword_len) {
    char *pos;
    char *currents = str;
    size_t remaining = len;

    if (!str || !keyword || keyword_len == 0 || len == 0) {
        return;
    }

    while (remaining > 0 && (pos = mc_strnstr(currents, keyword, remaining)) != NULL) {
        size_t offset = pos - currents;

        for (size_t i = 0; i < keyword_len && (i < remaining - offset); i++) {
            if (keyword[i] != '/' || pos[i] != '/') {
                pos[i] = get_random_char();
            }
        }

        currents = pos + keyword_len;
        remaining = len - (currents - str);

        if (currents >= str + len) {
            break;
        }
    }
}

static inline bool contains_keyword(char *str, size_t len) {
    struct shared_data_entry entry;
    unsigned int count;

    if (!str || len == 0) {
        return false;
    }

    count = shared_data_get_count_by_type(SHARED_DATA_TYPE_SINGLE_PATH);

    for (unsigned int i = 0; i < count; i++) {
        unsigned int global_index;
        if (shared_data_get_type_entry_info(SHARED_DATA_TYPE_SINGLE_PATH, i,
                                            &global_index, &entry) == SHARED_DATA_SUCCESS) {
            if (mc_strnstr(str, entry.data.single.path, len)) {
                return true;
            }
        }
    }
    return false;
}

static inline bool filter_output(struct seq_file *m, size_t old_count, bool replace) {
    char *buf_start;
    size_t len;

    if (!m || !m->buf) {
        return false;
    }

    if (m->count <= old_count) {
        return false;
    }

    buf_start = m->buf + old_count;
    len = m->count - old_count;

    if (contains_keyword(buf_start, len)) {
        if (replace) {
            struct shared_data_entry entry;
            unsigned int count = shared_data_get_count_by_type(SHARED_DATA_TYPE_SINGLE_PATH);

            for (unsigned int i = 0; i < count; i++) {
                unsigned int global_index;
                if (shared_data_get_type_entry_info(SHARED_DATA_TYPE_SINGLE_PATH, i,
                                                    &global_index, &entry) == SHARED_DATA_SUCCESS) {
                    size_t keyword_len = strlen(entry.data.single.path);
                    replace_sensitive(buf_start, len, entry.data.single.path, keyword_len);
                }
            }
        } else {
            m->count = old_count;
        }
        return true;
    }
    return false;
}

/* local 在 KP 的 transit 里是栈上未初始化内存，必须先写再读（同 mount_hidder）。 */
static void mc_before_count(hook_fargs2_t *args, void *udata) {
    struct seq_file *m = (struct seq_file *)args->arg0;

    (void)udata;
    args->local.MC_LOCAL_COUNT_SLOT = 0;
    if (m) {
        args->local.MC_LOCAL_COUNT_SLOT = (uint64_t)m->count + 1;
    }
}

static inline size_t mc_saved_count(hook_fargs2_t *args) {
    return (size_t)(args->local.MC_LOCAL_COUNT_SLOT - 1);
}

static void mc_after_vfsmnt(hook_fargs2_t *args, void *udata) {
    struct seq_file *m = (struct seq_file *)args->arg0;

    (void)udata;
    if (!m || !args->local.MC_LOCAL_COUNT_SLOT) {
        return;
    }
    if ((int)args->ret != 0 || !is_proc_effective()) {
        return;
    }
    filter_output(m, mc_saved_count(args), false);
}

static void mc_after_mountinfo(hook_fargs2_t *args, void *udata) {
    struct seq_file *m = (struct seq_file *)args->arg0;

    (void)udata;
    if (!m || !args->local.MC_LOCAL_COUNT_SLOT) {
        return;
    }
    if ((int)args->ret != 0 || !is_proc_effective()) {
        return;
    }
    filter_output(m, mc_saved_count(args), true);
}

static void mc_after_vfsstat(hook_fargs2_t *args, void *udata) {
    struct seq_file *m = (struct seq_file *)args->arg0;

    (void)udata;
    if (!m || !args->local.MC_LOCAL_COUNT_SLOT) {
        return;
    }
    if ((int)args->ret != 0 || !is_proc_effective()) {
        return;
    }
    filter_output(m, mc_saved_count(args), false);
}

/*
 * 类型4（地址区间）用：从 "7f8a1c0000-7f8a1c1000 r-xp ..." 里解析出起止地址。
 * 直接解析已输出的文本，不依赖 struct vm_area_struct 的字段布局。
 */
static inline bool parse_maps_range(const char *s, size_t len, unsigned long *start, unsigned long *end) {
    unsigned long v = 0;
    size_t i = 0;
    bool has_digit = false;

    if (!s || len < 3 || !start || !end) {
        return false;
    }

    while (i < len && (s[i] == ' ' || s[i] == '\t')) {
        i++;
    }

    for (; i < len; i++) {
        char c = s[i];
        unsigned long d;

        if (c >= '0' && c <= '9') {
            d = (unsigned long)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            d = (unsigned long)(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            d = (unsigned long)(c - 'A' + 10);
        } else {
            break;
        }

        v = (v << 4) | d;
        has_digit = true;
    }

    if (!has_digit || i >= len || s[i] != '-') {
        return false;
    }

    *start = v;
    v = 0;
    has_digit = false;
    i++;

    for (; i < len; i++) {
        char c = s[i];
        unsigned long d;

        if (c >= '0' && c <= '9') {
            d = (unsigned long)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            d = (unsigned long)(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            d = (unsigned long)(c - 'A' + 10);
        } else {
            break;
        }

        v = (v << 4) | d;
        has_digit = true;
    }

    if (!has_digit) {
        return false;
    }

    *end = v;
    return *start < *end;
}

static inline uid_t current_uid_value(void) {
    /* ★v0.2.3：见 control_current_uid，改用 current_uid() 去掉 cred_offset 依赖 */
    return mc_current_uid();
}

static void filter_maps_range(struct seq_file *m, size_t old_count) {
    unsigned long start, end;

    if (!m || !m->buf || m->count <= old_count) {
        return;
    }

    if (!parse_maps_range(m->buf + old_count, m->count - old_count, &start, &end)) {
        return;
    }

    if (shared_data_find_addr_range(current_uid_value(), start, end) >= 0) {
        m->count = old_count;
    }
}

static void mc_after_map_vma(hook_fargs2_t *args, void *udata) {
    struct seq_file *m = (struct seq_file *)args->arg0;

    (void)udata;
    if (!m || !args->local.MC_LOCAL_COUNT_SLOT || !is_proc_effective()) {
        return;
    }
    filter_output(m, mc_saved_count(args), false);
    filter_maps_range(m, mc_saved_count(args));
}

static void mc_after_smap(hook_fargs2_t *args, void *udata) {
    struct seq_file *m = (struct seq_file *)args->arg0;

    (void)udata;
    if (!m || !args->local.MC_LOCAL_COUNT_SLOT) {
        return;
    }
    if ((int)args->ret != 0 || !is_proc_effective()) {
        return;
    }
    filter_output(m, mc_saved_count(args), false);
}

struct mc_hook_desc {
    void *func;
    void *after;
    const char *name;
};

static void mc_fill_hooks(struct mc_hook_desc *out) {
    out[0].func = (void *)ori_show_vfsmnt;
    out[0].after = (void *)mc_after_vfsmnt;
    out[0].name = "show_vfsmnt";
    out[1].func = (void *)ori_show_mountinfo;
    out[1].after = (void *)mc_after_mountinfo;
    out[1].name = "show_mountinfo";
    out[2].func = (void *)ori_show_vfsstat;
    out[2].after = (void *)mc_after_vfsstat;
    out[2].name = "show_vfsstat";
    out[3].func = (void *)ori_show_map_vma;
    out[3].after = (void *)mc_after_map_vma;
    out[3].name = "show_map_vma";
    out[4].func = (void *)ori_show_smap;
    out[4].after = (void *)mc_after_smap;
    out[4].name = "show_smap";
}

static bool hook_all(void) {
    struct mc_hook_desc hooks[5];
    hook_err_t err = HOOK_NO_ERR;
    size_t i;

    mc_fill_hooks(hooks);

    for (i = 0; i < ARRAY_SIZE(hooks); i++) {
        if (!hooks[i].func) {
            pr_err("[yuuki] missing symbol for hook %s\n", hooks[i].name);
            return false;
        }
    }

    for (i = 0; i < ARRAY_SIZE(hooks); i++) {
        err = hook_wrap(hooks[i].func, 2, (void *)mc_before_count, hooks[i].after, 0);
        if (err != HOOK_NO_ERR) {
            pr_err("[yuuki] hook %s failed: %d\n", hooks[i].name, err);

            for (size_t j = 0; j < i; j++) {
                hook_unwrap(hooks[j].func, (void *)mc_before_count, hooks[j].after);
            }

            return false;
        }
    }

    hook_installed = 1;
    return true;
}

static inline bool install_hook(void) {
    if (hook_installed) {
        pr_info("[yuuki] hook already installed, skipping...\n");
        return true;
    }

    if (hook_all()) {
        pr_info("[yuuki] feature hook installed successfully\n");
        return true;
    }

    pr_err("[yuuki] hook installation failed\n");
    return false;
}

static inline bool uninstall_hook(void) {
    if (!hook_installed) {
        pr_info("[yuuki] not hooked, skipping uninstall...\n");
        return true;
    }

    if (ori_show_vfsmnt) hook_unwrap((void *)ori_show_vfsmnt, (void *)mc_before_count, (void *)mc_after_vfsmnt);
    if (ori_show_mountinfo) hook_unwrap((void *)ori_show_mountinfo, (void *)mc_before_count, (void *)mc_after_mountinfo);
    if (ori_show_vfsstat) hook_unwrap((void *)ori_show_vfsstat, (void *)mc_before_count, (void *)mc_after_vfsstat);
    if (ori_show_map_vma) hook_unwrap((void *)ori_show_map_vma, (void *)mc_before_count, (void *)mc_after_map_vma);
    if (ori_show_smap) hook_unwrap((void *)ori_show_smap, (void *)mc_before_count, (void *)mc_after_smap);

    hook_installed = 0;
    pr_info("[yuuki] feature hooks uninstalled successfully\n");
    return true;
}

static inline bool control_hook(bool enable) {
    return enable ? install_hook() : uninstall_hook();
}

long feature_init() {
    return control_hook(true) ? 0 : -1;
}

long feature_exit() {
    return control_hook(false) ? 0 : -1;
}