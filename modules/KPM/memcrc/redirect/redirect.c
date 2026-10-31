#include <hook.h>
#include "compat.h"
#include <asm/current.h>
#include <linux/sched.h>
#include <linux/err.h>
#include <linux/string.h>
#include <linux/printk.h>
#include <uapi/asm-generic/errno-base.h>
#include <linux/fs.h>
#include <linux/cred.h>
#include "redirect.h"
#include "shared_data.h"

static hook_err_t hook_err = HOOK_NOT_HOOK;
static do_filp_open_func_t backup_do_filp_open;

/*
 * 上游在这里用 get_task_ext(task) 取 task_ext 再置 priv_sel_allow，为第二次
 * do_filp_open 临时旁路 SELinux。现行 KernelPatch 已不再导出 get_task_ext，
 * 且 task_ext 已从内核栈顶迁移到 KP 私有的 1024 槽表（kernel/patch/common/taskob.c
 * 顶部注释），模块无法用偏移自行算出，故本移植**去掉 SELinux 旁路**：
 * 重定向只在调用者域本来就能打开 dst 时生效，否则内核返回 EACCES。
 */

static inline uid_t get_current_uid(void) {
    struct cred* cred = *(struct cred**)((uintptr_t)current + task_struct_offset.cred_offset);
    uid_t current_uid = *(uid_t*)((uintptr_t)cred + cred_offset.uid_offset);
    return current_uid;
}

// 使用cat命令读取文件，安卓会有content provider 的代理机制 或 Android 系统的
// “媒体读取委托”机制，需要通过过滤com.android.providers.media.module的uid实现重定向，而非app进程自身的uid
static struct file *replace_do_filp_open(int dfd, struct filename *pathname, const struct open_flags *op) {
    struct file *filp;
    char buf[PATH_MAX];
    char *curr_path;
    struct shared_data_entry entry;
    unsigned int map_count;
    unsigned int i;
    uid_t current_uid;

    filp = backup_do_filp_open(dfd, pathname, op);

    if (unlikely(IS_ERR(filp))) {
        return filp;
    }

    memset(buf, 0, PATH_MAX);
    curr_path = d_path(&filp->f_path, buf, PATH_MAX);


    if (IS_ERR(curr_path)) {
        return filp;
    }

    current_uid = get_current_uid();

    map_count = shared_data_get_count_by_type(SHARED_DATA_TYPE_PATH_MAPPING);

    for (i = 0; i < map_count; i++) {
        unsigned int global_index;
        int ret;

        ret = shared_data_get_type_entry_info(SHARED_DATA_TYPE_PATH_MAPPING, i,
                                              &global_index, &entry);

        if (ret != SHARED_DATA_SUCCESS) {
            continue;
        }

        if (strncmp(curr_path, entry.data.mapping.src_path,
                    strlen(entry.data.mapping.src_path)) == 0) {

            if (entry.data.mapping.uid == 0 || entry.data.mapping.uid == current_uid) {
                struct filename *new_pathname;
                struct file *redirect_filp;

                pr_info("[yuuki] Redirecting path from %s to %s (uid: %u, current: %u)\n",
                        entry.data.mapping.src_path,
                        entry.data.mapping.dst_path,
                        entry.data.mapping.uid,
                        current_uid);

                fput(filp);

                new_pathname = kf_vmalloc(sizeof(struct filename));
                if (!new_pathname) {
                    pr_err("[yuuki] Failed to allocate memory for new pathname\n");
                    return ERR_PTR(-ENOMEM);
                }

                /*
                 * 上游只写 ->name，refcnt/aname 是未初始化内存；若 do_filp_open
                 * 内部对入参调 putname()，refcnt 恰为 0 就会走 __putname() 用
                 * kmem_cache_free 释放一个 vmalloc 指针。这里显式清零并把 refcnt
                 * 置成大值，使 putname() 的 --refcnt == 0 永不成立，释放权始终
                 * 留在本函数（下面 kf_vfree），两种内核行为下都安全。
                 */
                kf_memset(new_pathname, 0, sizeof(struct filename));
                new_pathname->refcnt = 0x40000000;
                new_pathname->name = entry.data.mapping.dst_path;
                redirect_filp = backup_do_filp_open(dfd, new_pathname, op);
                kf_vfree(new_pathname);

                return redirect_filp;
            }
        }
    }

    return filp;
}

static inline bool hook_do_filp_open(void) {
    if (!original_do_filp_open) {
        hook_err = HOOK_BAD_ADDRESS;
        pr_err("[yuuki] no symbol: do_filp_open\n");
        return false;
    }

    hook_err = hook((void *)original_do_filp_open,
                    (void *)replace_do_filp_open,
                    (void **)&backup_do_filp_open);

    if (hook_err != HOOK_NO_ERR) {
        pr_err("[yuuki] hook do_filp_open failed, address: %llx, error: %d\n",
               (unsigned long long)original_do_filp_open, hook_err);
        return false;
    }

    return true;
}

static inline bool install_hook(void) {
    if (hook_err == HOOK_NO_ERR) {
        pr_info("[yuuki] redirect hook already installed, skipping...\n");
        return true;
    }

    if (hook_do_filp_open()) {
        pr_info("[yuuki] redirect hook installed successfully\n");
        return true;
    }

    pr_err("[yuuki] redirect hook installation failed\n");
    return false;
}

static inline bool uninstall_hook(void) {
    if (hook_err != HOOK_NO_ERR) {
        pr_info("[yuuki] redirect hook not installed, skipping uninstall...\n");
        return true;
    }

    unhook((void *)original_do_filp_open);
    hook_err = HOOK_NOT_HOOK;
    pr_info("[yuuki] redirect hook uninstalled successfully\n");
    return true;
}

static inline bool control_hook(bool enable) {
    return enable ? install_hook() : uninstall_hook();
}

long redirect_init(void) {
    return control_hook(true) ? 0 : -1;
}

long redirect_exit(void) {
    return control_hook(false) ? 0 : -1;
}