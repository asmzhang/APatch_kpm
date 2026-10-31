#ifndef MEMORY_CRC_SYMBOLS_H
#define MEMORY_CRC_SYMBOLS_H
#include <linux/printk.h>
#include <kallsyms.h>
#include <linux/spinlock.h>
#include "feature.h"

int (*misc_register)(struct miscdevice *misc);
void (*misc_deregister)(struct miscdevice *misc);

typedef struct file *(*do_filp_open_func_t)(int dfd, struct filename *pathname, const struct open_flags *op);
do_filp_open_func_t original_do_filp_open;

char *(*d_path)(const struct path *path, char *buf, int buflen);
void (*fput)(struct file *file);
void *(*kf_vmalloc)(unsigned long size);
void (*kf_vfree)(const void *addr);

/*typedef int (*vfs_show_func_t)(struct seq_file *, struct vfsmount *);
typedef void (*map_show_func_t)(struct seq_file *, struct vm_area_struct *);
typedef int (*smap_show_func_t)(struct seq_file *, void *);*/

vfs_show_func_t ori_show_vfsmnt;
vfs_show_func_t ori_show_mountinfo;
vfs_show_func_t ori_show_vfsstat;
map_show_func_t ori_show_map_vma;
smap_show_func_t ori_show_smap;

struct mm_struct *(*kf_get_task_mm)(struct task_struct *task);
void (*kf_mmput)(struct mm_struct *mm);

void (*kf_raw_spin_unlock_irqrestore)(spinlock_t *lock, unsigned long flags);
unsigned long (*kf_raw_spin_lock_irqsave)(spinlock_t *lock);

void* ori_proc_pid_status = NULL;
void* ori_do_task_stat = NULL;

int init_symbols() {
    misc_register = (typeof(misc_register))kallsyms_lookup_name("misc_register");
    if (!misc_register) {
        pr_info("[yuuki] kernel func: 'misc_register' does not exist!\n");
        goto exit;
    }

    misc_deregister = (typeof(misc_deregister))kallsyms_lookup_name("misc_deregister");
    if (!misc_deregister) {
        pr_info("[yuuki] kernel func: 'misc_deregister' does not exist!\n");
        goto exit;
    }

    original_do_filp_open = (do_filp_open_func_t)kallsyms_lookup_name("do_filp_open");
    if (!original_do_filp_open) {
        pr_info("[yuuki] kernel func: 'do_filp_open' does not exist!\n");
        goto exit;
    }

    kf_vmalloc = (typeof(kf_vmalloc))kallsyms_lookup_name("vmalloc");
    if (!kf_vmalloc) {
        pr_info("[yuuki] kernel func: 'vmalloc' does not exist!\n");
        goto exit;
    }

    kf_vfree = (typeof(kf_vfree))kallsyms_lookup_name("vfree");
    if (!kf_vfree) {
        pr_info("[yuuki] kernel func: 'vfree' does not exist!\n");
        goto exit;
    }

    d_path = (typeof(d_path))kallsyms_lookup_name("d_path");
    if (!d_path) {
        pr_info("[yuuki] kernel func: 'd_path' does not exist!\n");
        goto exit;
    }

    fput = (typeof(fput))kallsyms_lookup_name("fput");
    if (!fput) {
        pr_info("[yuuki] kernel func: 'fput' does not exist!\n");
        goto exit;
    }

    ori_show_vfsmnt = (vfs_show_func_t)kallsyms_lookup_name("show_vfsmnt");
    if (!ori_show_vfsmnt) {
        pr_info("[yuuki] kernel func: 'show_vfsmnt' does not exist!\n");
        goto exit;
    }

    ori_show_mountinfo = (vfs_show_func_t)kallsyms_lookup_name("show_mountinfo");
    if (!ori_show_mountinfo) {
        pr_info("[yuuki] kernel func: 'show_mountinfo' does not exist!\n");
        goto exit;
    }

    ori_show_vfsstat = (vfs_show_func_t)kallsyms_lookup_name("show_vfsstat");
    if (!ori_show_vfsstat) {
        pr_info("[yuuki] kernel func: 'show_vfsstat' does not exist!\n");
        goto exit;
    }

    ori_show_map_vma = (map_show_func_t)kallsyms_lookup_name("show_map_vma");
    if (!ori_show_map_vma) {
        pr_info("[yuuki] kernel func: 'show_map_vma' does not exist!\n");
        goto exit;
    }

    ori_show_smap = (smap_show_func_t)kallsyms_lookup_name("show_smap");
    if (!ori_show_smap) {
        pr_info("[yuuki] kernel func: 'show_smap' does not exist!\n");
        goto exit;
    }

    kf_get_task_mm = (typeof(kf_get_task_mm))kallsyms_lookup_name("get_task_mm");
    if (!kf_get_task_mm) {
        pr_info("[yuuki] kernel func: 'get_task_mm' does not exist!\n");
        goto exit;
    }

    kf_mmput = (typeof(kf_mmput))kallsyms_lookup_name("mmput");
    if (!kf_mmput) {
        pr_info("[yuuki] kernel func: 'mmput' does not exist!\n");
        goto exit;
    }

    kf_raw_spin_unlock_irqrestore = (typeof(kf_raw_spin_unlock_irqrestore))kallsyms_lookup_name("_raw_spin_unlock_irqrestore");
    if (!kf_raw_spin_unlock_irqrestore) {
        pr_info("[yuuki] kernel func: '_raw_spin_unlock_irqrestore' does not exist!\n");
        goto exit;
    }

    kf_raw_spin_lock_irqsave = (typeof(kf_raw_spin_lock_irqsave))kallsyms_lookup_name("_raw_spin_lock_irqsave");
    if (!kf_raw_spin_lock_irqsave) {
        pr_info("[yuuki] kernel func: '_raw_spin_lock_irqsave' does not exist!\n");
        goto exit;
    }

    ori_proc_pid_status = (typeof(ori_proc_pid_status))kallsyms_lookup_name("proc_pid_status");
    if (!ori_proc_pid_status) {
        pr_info("[yuuki] kernel func: 'proc_pid_status' does not exist!\n");
        goto exit;
    }

    ori_do_task_stat = (typeof(ori_do_task_stat))kallsyms_lookup_name("do_task_stat");
    if (!ori_do_task_stat) {
        pr_info("[yuuki] kernel func: 'do_task_stat' does not exist!\n");
        goto exit;
    }

    return 0;
    exit:
    return 1;
}

#endif
