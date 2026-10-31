/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * stealth_hook —— ghost memory
 *
 * 上游 ghost_mem.c（288 行）的移植。上游用 vm_mmap 建正规 VMA，再靠同模块的
 * maps_hide 把它从 /proc/self/maps 抹掉。本仓把 maps_hide 并进了 memcrc 的
 * RANGE:uid:start:end 规则，所以这里只负责「分配 / 写 / 释放 / 列表」，
 * 隐藏由调用方（CLI 或注入器）另外下发一条 RANGE 规则完成。
 *
 * 与上游的两处行为差异：
 *   1. 不自动隐藏：分配后 /proc/self/maps 里能看见，要隐藏请配合 memcrc 的 RANGE；
 *   2. 模块卸载时**不** munmap：卸载发生在管理器进程里，拿调用方进程的地址去
 *      vm_munmap 会误伤管理器的地址空间。节点只回收记账，VMA 由目标进程退出时回收。
 */

#include <compiler.h>
#include <kpmodule.h>
#include <linux/printk.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <kputils.h>
#include <asm/current.h>
#include <ktypes.h>
#include <kallsyms.h>

#include "stealth_hook.h"

#define INIT_LIST_HEAD(ptr) do { (ptr)->next = (ptr); (ptr)->prev = (ptr); } while (0)

struct sh_ghost_node {
    struct list_head list;
    uint64_t va;
    uint64_t size;
    uint32_t pid;
    uint32_t prot;
};

static struct list_head ghost_list;
static bool ghost_list_ready;

/* 用户态拷贝用（KP 不导出 copy_from_user，只导出 __arch_copy_from_user） */
static unsigned long (*sh_copy_from_user)(void *to, const void __user *from, unsigned long n);

/* KP 没有导出 kf_vmalloc / kf_vfree（只有声明），必须自己按 kallsyms 解析 */
static void *(*sh_vmalloc)(unsigned long size);
static void (*sh_vfree)(const void *addr);

static unsigned long (*sh_vm_mmap)(void *file, unsigned long addr, unsigned long len,
                                   unsigned long prot, unsigned long flags,
                                   unsigned long pgoff);
static int (*sh_vm_munmap)(unsigned long start, unsigned long len);

static void sh_ghost_resolve(void)
{
    if (!sh_vm_mmap) {
        sh_vm_mmap = (typeof(sh_vm_mmap))kallsyms_lookup_name("vm_mmap");
        sh_vm_munmap = (typeof(sh_vm_munmap))kallsyms_lookup_name("vm_munmap");
        sh_dbg("[ghost] vm_mmap=%llx vm_munmap=%llx\n",
               (uint64_t)sh_vm_mmap, (uint64_t)sh_vm_munmap);
    }
    if (!sh_copy_from_user) {
        sh_copy_from_user = (typeof(sh_copy_from_user))kallsyms_lookup_name("__arch_copy_from_user");
    }
    if (!sh_vmalloc) {
        sh_vmalloc = (typeof(sh_vmalloc))kallsyms_lookup_name("vmalloc");
        sh_vfree = (typeof(sh_vfree))kallsyms_lookup_name("vfree");
        sh_dbg("[ghost] vmalloc=%llx vfree=%llx\n",
               (uint64_t)sh_vmalloc, (uint64_t)sh_vfree);
    }
}

static unsigned long sh_to_mmap_prot(uint32_t prot)
{
    unsigned long mp = 0;

    if (prot & SH_PROT_READ) {
        mp |= 0x1;   /* PROT_READ */
    }
    if (prot & SH_PROT_WRITE) {
        mp |= 0x2;   /* PROT_WRITE */
    }
    if (prot & SH_PROT_EXEC) {
        mp |= 0x4;   /* PROT_EXEC */
    }
    return mp;
}

void sh_ghost_init(void)
{
    INIT_LIST_HEAD(&ghost_list);
    ghost_list_ready = true;
    sh_ghost_resolve();
}

long sh_ghost_alloc(uint64_t target_va, uint64_t size, uint32_t prot)
{
    struct sh_ghost_node *node;
    unsigned long flags;
    unsigned long result;

    if (!ghost_list_ready) {
        sh_ghost_init();
    }
    sh_ghost_resolve();

    if (!sh_vm_mmap) {
        pr_err("[ghost] vm_mmap not resolved\n");
        return -1;
    }
    if (!size) {
        return -22;   /* -EINVAL */
    }

    size = (size + 4095) & ~4095ull;

    flags = 0x22;   /* MAP_PRIVATE | MAP_ANONYMOUS */
    if (target_va) {
        flags |= 0x10;   /* MAP_FIXED */
    }

    result = sh_vm_mmap(0, (unsigned long)target_va, (unsigned long)size,
                        sh_to_mmap_prot(prot), flags, 0);
    if ((long)result < 0 || result >= 0xFFFFFFFF00000000ull) {
        pr_err("[ghost] vm_mmap failed: %lx\n", result);
        return -1;
    }

    node = (struct sh_ghost_node *)sh_vmalloc(sizeof(*node));
    if (!node) {
        sh_vm_munmap(result, (unsigned long)size);
        return -12;   /* -ENOMEM */
    }
    node->va = result;
    node->size = size;
    node->pid = 0;    /* 跨进程不支持，记 0 表示「分配它的进程」 */
    node->prot = prot;
    node->list.next = ghost_list.next;
    node->list.prev = &ghost_list;
    ghost_list.next->prev = &node->list;
    ghost_list.next = &node->list;

    sh_dbg("[ghost] alloc va=%llx size=%llx prot=%x\n",
           (uint64_t)result, size, prot);
    return (long)result;
}

long sh_ghost_free(uint64_t va)
{
    struct list_head *pos;
    struct sh_ghost_node *node;

    if (!ghost_list_ready) {
        return -1;
    }

    for (pos = ghost_list.next; pos != &ghost_list; pos = pos->next) {
        node = (struct sh_ghost_node *)((char *)pos - __builtin_offsetof(struct sh_ghost_node, list));
        if (node->va == va) {
            if (sh_vm_munmap) {
                sh_vm_munmap((unsigned long)node->va, (unsigned long)node->size);
            }
            pos->prev->next = pos->next;
            pos->next->prev = pos->prev;
            sh_vfree(node);
            sh_dbg("[ghost] free va=%llx\n", va);
            return 0;
        }
    }
    return -2;   /* -ENOENT */
}

long sh_ghost_write(struct sh_ghost_write_request __user *ureq)
{
    struct sh_ghost_write_request req;
    struct list_head *pos;
    struct sh_ghost_node *node;
    char buf[256];
    uint64_t done;

    if (!ghost_list_ready) {
        return -1;
    }
    sh_ghost_resolve();

    if (!sh_copy_from_user || sh_copy_from_user(&req, ureq, sizeof(req))) {
        return -14;   /* -EFAULT */
    }
    if (!req.target_va || !req.src_data || !req.size) {
        return -22;
    }

    for (pos = ghost_list.next; pos != &ghost_list; pos = pos->next) {
        node = (struct sh_ghost_node *)((char *)pos - __builtin_offsetof(struct sh_ghost_node, list));
        if (req.target_va >= node->va && req.target_va + req.size <= node->va + node->size) {
            for (done = 0; done < req.size; ) {
                uint64_t chunk = req.size - done;

                if (chunk > sizeof(buf)) {
                    chunk = sizeof(buf);
                }
                if (sh_copy_from_user(buf, (void __user *)(req.src_data + done), chunk)) {
                    return -14;
                }
                if (compat_copy_to_user((void __user *)(req.target_va + done), buf, (int)chunk) != (int)chunk) {
                    return -14;
                }
                done += chunk;
            }
            return 0;
        }
    }
    return -2;
}

long sh_ghost_list(struct sh_ghost_info __user *uout, long max)
{
    struct list_head *pos;
    struct sh_ghost_node *node;
    struct sh_ghost_info info;
    long n = 0;

    if (!ghost_list_ready) {
        return 0;
    }
    if (max <= 0) {
        max = SH_GHOST_LIST_MAX;
    }

    for (pos = ghost_list.next; pos != &ghost_list && n < max; pos = pos->next) {
        node = (struct sh_ghost_node *)((char *)pos - __builtin_offsetof(struct sh_ghost_node, list));
        info.va = node->va;
        info.size = node->size;
        info.pid = node->pid;
        info.prot = node->prot;
        if (compat_copy_to_user((void __user *)((uintptr_t)uout + (uintptr_t)n * sizeof(info)),
                                &info, sizeof(info)) != (int)sizeof(info)) {
            break;
        }
        n++;
    }
    return n;
}

/*
 * 模块卸载：只回收记账节点，不 munmap。
 * 卸载发生在管理器进程的上下文里，拿调用方进程的地址去 munmap 会误伤管理器的
 * 地址空间；目标进程退出时内核会自己回收这段 VMA。
 */
void sh_ghost_cleanup(void)
{
    struct list_head *pos;
    struct list_head *tmp;
    struct sh_ghost_node *node;

    if (!ghost_list_ready) {
        return;
    }

    for (pos = ghost_list.next; pos != &ghost_list; pos = tmp) {
        tmp = pos->next;
        node = (struct sh_ghost_node *)((char *)pos - __builtin_offsetof(struct sh_ghost_node, list));
        sh_dbg("[ghost] drop bookkeeping va=%llx (vma left to owner)\n", node->va);
        pos->prev->next = pos->next;
        pos->next->prev = pos->prev;
        sh_vfree(node);
    }
}
