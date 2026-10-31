#ifndef MEMORY_CRC_FEATURE_H
#define MEMORY_CRC_FEATURE_H

#include <asm/current.h>
#include "ptrace_hide.h"
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
#endif

long feature_init();
long feature_exit();

#define RANDOM_CHARS "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
#define RANDOM_CHARS_LEN (sizeof(RANDOM_CHARS) - 1)

/*struct seq_file {
    char *buf;
    size_t size;
    size_t from;
    size_t count;
    //...
};*/

struct hook_funcs {
    void *original;
    void *replacement;
    void **backup;
};

struct filter_keyword {
    const char *keyword;
    size_t len;
};

/* 前置声明：下面 typedef 的参数里出现这些指针，若不先在文件作用域声明，
 * 编译器会在原型作用域里各造一个新的不完整类型，与真实定义不兼容
 * （-Wincompatible-pointer-types）。 */
struct vfsmount;
struct vm_area_struct;

typedef int (*vfs_show_func_t)(struct seq_file *, struct vfsmount *);
typedef void (*map_show_func_t)(struct seq_file *, struct vm_area_struct *);
typedef int (*smap_show_func_t)(struct seq_file *, void *);

extern vfs_show_func_t ori_show_vfsmnt;
extern vfs_show_func_t ori_show_mountinfo;
extern vfs_show_func_t ori_show_vfsstat;
extern map_show_func_t ori_show_map_vma;
extern smap_show_func_t ori_show_smap;

/*extern void *(*kf_vmalloc)(unsigned long size);
extern void (*kf_vfree)(const void *addr);*/
extern struct mm_struct *(*kf_get_task_mm)(struct task_struct *task);
extern void (*kf_mmput)(struct mm_struct *);

#endif
