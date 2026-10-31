#include <hook.h>
#include "compat.h"
#include <linux/printk.h>
#include <kputils.h>
#include <linux/string.h>
#include <linux/sched.h>
#include <linux/cred.h>
#include "feature.h"
#include "shared_data.h"

static vfs_show_func_t backup_show_vfsmnt;
static vfs_show_func_t backup_show_mountinfo;
static vfs_show_func_t backup_show_vfsstat;
static map_show_func_t backup_show_map_vma;
static smap_show_func_t backup_show_smap;

static hook_err_t hook_err = HOOK_NOT_HOOK;

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

    while (remaining > 0 && (pos = strnstr(currents, keyword, remaining)) != NULL) {
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
            if (strnstr(str, entry.data.single.path, len)) {
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

static int rep_show_vfsmnt(struct seq_file *m, struct vfsmount *mnt) {
    size_t old_count;
    int ret;

    old_count = m->count;
    ret = backup_show_vfsmnt(m, mnt);

    if (ret == 0 && is_proc_effective()) {
        filter_output(m, old_count, false);
    }

    return ret;
}

static int rep_show_mountinfo(struct seq_file *m, struct vfsmount *mnt) {
    size_t old_count;
    int ret;

    old_count = m->count;
    ret = backup_show_mountinfo(m, mnt);

    if (ret == 0 && is_proc_effective()) {
        filter_output(m, old_count, true);
    }

    return ret;
}

static int rep_show_vfsstat(struct seq_file *m, struct vfsmount *mnt) {
    size_t old_count;
    int ret;

    old_count = m->count;
    ret = backup_show_vfsstat(m, mnt);

    if (ret == 0 && is_proc_effective()) {
        filter_output(m, old_count, false);
    }

    return ret;
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
    struct cred *cred = *(struct cred **)((uintptr_t)current + task_struct_offset.cred_offset);
    return *(uid_t *)((uintptr_t)cred + cred_offset.uid_offset);
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

static void rep_show_map_vma(struct seq_file *m, struct vm_area_struct *vma) {
    size_t old_count;

    old_count = m->count;
    backup_show_map_vma(m, vma);

    if (is_proc_effective()) {
        filter_output(m, old_count, false);
        filter_maps_range(m, old_count);
    }
}

static int rep_show_smap(struct seq_file *m, void *v) {
    size_t old_count;
    int ret;

    old_count = m->count;
    ret = backup_show_smap(m, v);

    if (ret == 0 && is_proc_effective()) {
        filter_output(m, old_count, false);
    }

    return ret;
}

static bool hook_all(void) {
    struct hook_funcs hooks[] = {
            { ori_show_vfsmnt, rep_show_vfsmnt, (void **)&backup_show_vfsmnt },
            { ori_show_mountinfo, rep_show_mountinfo, (void **)&backup_show_mountinfo },
            { ori_show_vfsstat, rep_show_vfsstat, (void **)&backup_show_vfsstat },
            { ori_show_map_vma, rep_show_map_vma, (void **)&backup_show_map_vma },
            { ori_show_smap, rep_show_smap, (void **)&backup_show_smap },
    };

    for (size_t i = 0; i < ARRAY_SIZE(hooks); i++) {
        if (!hooks[i].original) {
            pr_err("[yuuki] missing symbol for hook %zu\n", i);
            return false;
        }
    }

    for (size_t i = 0; i < ARRAY_SIZE(hooks); i++) {
        hook_err = hook(hooks[i].original, hooks[i].replacement, hooks[i].backup);
        if (hook_err != HOOK_NO_ERR) {
            pr_err("[yuuki] hook failed at %zu with error %d\n", i, hook_err);

            for (size_t j = 0; j < i; j++) {
                unhook(hooks[j].original);
            }

            hook_err = HOOK_NOT_HOOK;
            return false;
        }
    }

    return true;
}

static inline bool install_hook(void) {
    if (hook_err == HOOK_NO_ERR) {
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
    if (hook_err != HOOK_NO_ERR) {
        pr_info("[yuuki] not hooked, skipping uninstall...\n");
        return true;
    }

    if (ori_show_vfsmnt) unhook(ori_show_vfsmnt);
    if (ori_show_mountinfo) unhook(ori_show_mountinfo);
    if (ori_show_vfsstat) unhook(ori_show_vfsstat);
    if (ori_show_map_vma) unhook(ori_show_map_vma);
    if (ori_show_smap) unhook(ori_show_smap);

    hook_err = HOOK_NOT_HOOK;
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