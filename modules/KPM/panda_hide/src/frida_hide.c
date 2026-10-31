#include "frida_hide.h"

#include "common.h"
#include <hook.h>
#include <kputils.h>
#include <syscall.h>

#include <linux/string.h>
#include <linux/printk.h>
#include <linux/kallsyms.h>

static void *show_map_vma_fn = 0;
static void *get_task_comm_fn = 0;

/* --- maps 隐藏: 从 /proc/[pid]/maps 中移除 frida 相关映射 --- */

/* 仅搜索新写入的部分，避免在残留数据中误匹配 */
static int __attribute__((optnone)) is_hidden_map(struct seq_file *m, size_t prev_count) {
    const char *block_str[] = {
        "frida",
        "gadget",
        "linjector",
        "gmain",
    };
    char *start = m->buf + prev_count;
    size_t new_len = m->count - prev_count;
    if (new_len == 0) return 0;

    /* 临时截断，防止 strstr 越界读到旧数据 */
    {
        char line[256];
        size_t copy_len = new_len < sizeof(line) - 1 ? new_len : sizeof(line) - 1;

        /* m->count == m->size 时 m->buf 没有第 new_len 字节可写：拷到本地缓冲再查（AUD-c-4） */
        memcpy(line, start, copy_len);
        line[copy_len] = '\0';

        for (int k = 0; k < (int)(sizeof(block_str) / sizeof(block_str[0])); k++) {
            if (strstr(line, block_str[k])) {
                return 1;
            }
        }
        return 0;
    }
}

static void before_show_map_vma(hook_fargs2_t *args, void *udata) {
    struct seq_file *m = (struct seq_file *)args->arg0;
    if (m && m->buf) {
        args->local.data0 = m->count;
        args->local.data1 = 1;  /* 标记有效 */
    } else {
        args->local.data1 = 0;
    }
}

static void after_show_map_vma(hook_fargs2_t *args, void *udata) {
    struct seq_file *m = (struct seq_file *)args->arg0;
    if (m && m->buf && args->local.data1) {
        size_t prev_count = (size_t)args->local.data0;
        if (is_hidden_map(m, prev_count))
            m->count = prev_count;
    }
}

/* --- 线程名隐藏: 隐藏 frida 特征线程名 --- */

static int is_hidden_comm(const char *comm) {
    const char *ban_names[] = {
        "gum-js-loop",
        "pool-frida",
        "pool-spawner",
        "linjector",
        "gmain",
        "gdbus",
        "frida",
    };
    for (int i = 0; i < sizeof(ban_names) / sizeof(ban_names[0]); i++) {
        if (strstr(comm, ban_names[i]))
            return 1;
    }
    return 0;
}

static void __attribute__((optnone)) after_get_task_comm(hook_fargs3_t *args, void *udata) {
    char *comm = (char *)args->arg0;
    size_t comm_buf_len = (size_t)args->arg1;
    if (comm && comm_buf_len && is_hidden_comm(comm)) {
        /* 用正常线程名替换，避免全空格被检测为异常 */
        const char *fake = "binder";
        size_t fake_len = strlen(fake);
        size_t len = strlen(comm);
        if (len >= fake_len) {
            memcpy(comm, fake, fake_len);
            comm[fake_len] = '\0';
        } else {
            memcpy(comm, fake, len);
            comm[len] = '\0';
        }
    }
}

/* --- install / uninstall --- */

void panda_frida_hide_install() {
    show_map_vma_fn = (void *)kallsyms_lookup_name("show_map_vma");
    get_task_comm_fn = (void *)kallsyms_lookup_name("__get_task_comm");

    if (show_map_vma_fn) {
        hook_err_t err = hook_wrap2(show_map_vma_fn, before_show_map_vma, after_show_map_vma, NULL);
        if (err) {
            pr_err("panda_hide: hook show_map_vma failed %d\n", err);
            show_map_vma_fn = 0;
        }
    } else {
        pr_warn("panda_hide: show_map_vma not found\n");
    }

    if (get_task_comm_fn) {
        hook_err_t err = hook_wrap3(get_task_comm_fn, NULL, after_get_task_comm, NULL);
        if (err) {
            pr_err("panda_hide: hook __get_task_comm failed %d\n", err);
            get_task_comm_fn = 0;
        }
    } else {
        pr_warn("panda_hide: __get_task_comm not found\n");
    }

    pr_info("panda_hide: frida hide installed\n");
}

void panda_frida_hide_uninstall() {
    if (show_map_vma_fn)
        hook_unwrap_remove(show_map_vma_fn, before_show_map_vma, after_show_map_vma, 1);
    if (get_task_comm_fn)
        hook_unwrap_remove(get_task_comm_fn, NULL, after_get_task_comm, 1);
    pr_info("panda_hide: frida hide uninstalled\n");
}
