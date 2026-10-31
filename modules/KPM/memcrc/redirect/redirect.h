#ifndef MEMORY_CRC_REDIRECT_H
#define MEMORY_CRC_REDIRECT_H

#include <linux/llist.h>
#include <ktypes.h>

#define PATH_MAX 256

long redirect_init();
long redirect_exit();

struct vfsmount;
struct dentry;
struct inode;
struct filename;

struct path {
    struct vfsmount *mnt;
    struct dentry *dentry;
};

struct file {
    union {
        struct llist_node    fu_llist;
        struct rcu_head      fu_rcuhead;
    } f_u;
    struct path     f_path;
    struct inode    *f_inode;
};

struct open_flags {
    int open_flag;
    umode_t mode;
    int acc_mode;
    int intent;
    int lookup_flags;
};

typedef struct file *(*do_filp_open_func_t)(int dfd, struct filename *pathname, const struct open_flags *op);
extern do_filp_open_func_t original_do_filp_open;

extern char *(*d_path)(const struct path *path, char *buf, int buflen);
extern void (*fput)(struct file *file);

extern void *(*kf_vmalloc)(unsigned long size);
extern void (*kf_vfree)(const void *addr);
#endif
