/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * stealth_hook —— 硬件断点（perf hw_breakpoint）
 *
 * 上游 hwbp.c（879 行）的移植。保留：
 *   - 按线程注册（perf_event_create_kernel_counter / register_user_hw_breakpoint）
 *   - 观察者状态机：入口断点 -> 改到 LR -> 返回时再触发，一次注册同时拿参数和返回值
 *   - 入口捕获 X0-X7 / LR，可选按指针 dump 内存
 *   - INLINE：PC 直接跳到 trampoline（不改目标代码一个字节）
 *   - MODIFY_ARGS / SKIP_ORIGIN：改参数、跳过原函数直接给返回值
 *   - QUERY 分页枚举（每个线程一个节点）
 * 未移植（本仓不需要，见开发报告「与上游的差异」）：
 *   - FULL_HOOK：依赖用户态 DBI 把整页重编译成搬家版，本仓不做 DBI
 *   - AUTO_THREAD：钩 wake_up_new_task 给新线程自动补断点，多一个全局钩子，
 *     收益（新线程自动覆盖）可以由 CLI 轮询线程列表达到同样效果
 */

#include <compiler.h>
#include <kpmodule.h>
#include <linux/printk.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <kputils.h>
#include <asm/current.h>
#include <hook.h>
#include <ktypes.h>
#include <kallsyms.h>

#include "stealth_hook.h"

#define INIT_LIST_HEAD(ptr) do { (ptr)->next = (ptr); (ptr)->prev = (ptr); } while (0)

/* ARM64 pt_regs 的最小视图（perf 溢出回调给的就是这个布局） */
struct sh_pt_regs {
    uint64_t regs[31];   /* x0-x30 */
    uint64_t sp;
    uint64_t pc;
    uint64_t pstate;
};

#define STRIP_PAC(addr) ((uint64_t)(addr) & 0x0000FFFFFFFFFFFFull)

#define PERF_TYPE_BREAKPOINT 5
#define HW_BREAKPOINT_X      4
#define HW_BREAKPOINT_LEN_4  4
#define SH_MAX_ERRNO         4095

struct sh_perf_event_attr {
    uint32_t type;
    uint32_t size;
    uint64_t config;
    uint64_t sample_period;
    uint64_t sample_type;
    uint64_t read_format;
    uint64_t flags;
    uint32_t wakeup_events;
    uint32_t bp_type;
    uint64_t bp_addr;
    uint64_t bp_len;
    uint64_t branch_sample_type;
    uint64_t sample_regs_user;
    uint32_t sample_stack_user;
    int32_t clockid;
    uint64_t sample_regs_intr;
    uint32_t aux_watermark;
    uint16_t sample_max_stack;
    uint16_t reserved_2;
    uint32_t aux_sample_size;
    uint32_t reserved_3;
    uint64_t sig_data;
};

/* 内核侧符号（KP 未导出，全部走 kallsyms） */
static void *(*sh_vmalloc)(unsigned long size);
static void (*sh_vfree)(const void *addr);
static unsigned long (*sh_copy_from_user)(void *to, const void __user *from, unsigned long n);
static struct task_struct *(*sh_find_task_by_vpid)(pid_t nr);
static void *(*sh_register_user_hw_breakpoint)(void *attr, void *triggered, void *context, void *tsk);
static void *(*sh_perf_event_create_kernel_counter)(void *attr, int cpu, void *task, void *callback, void *context);
static int (*sh_modify_user_hw_breakpoint)(void *bp, void *attr);
static void (*sh_unregister_hw_breakpoint)(void *bp);
static void (*sh_perf_event_release_kernel)(void *event);
static void (*sh_perf_event_enable)(void *event);
static void (*sh_perf_event_disable)(void *event);

static struct list_head hwbp_list;
static bool hwbp_list_ready;
static uint32_t hwbp_enter_count;
static uint32_t hwbp_unmatched_count;

struct sh_hwbp_node {
    struct list_head list;
    uint64_t target_addr;
    uint64_t orig_bp_addr;
    uint64_t bp_addr;
    uint32_t tid;
    uint32_t flags;
    uint64_t trampoline_addr;
    void *bp_handle;
    int is_waiting_return;
    int is_disabled;
    uint32_t hit_count;
    uint64_t last_pc;
    uint64_t last_x0;
    uint64_t last_x1;
    uint64_t entry_args[8];
    uint64_t ret_x0;
    uint64_t entry_lr;
    uint32_t dump_size;
    uint8_t mem_dump[8][SH_DUMP_MAX];
    uint16_t mem_len[8];
    uint32_t skip_count;
    uint32_t skipped;
    uint64_t override_mask;
    uint64_t override_args[8];
    uint64_t ret_value;
};

static void fill_execute_attr(struct sh_perf_event_attr *attr, uint64_t addr, int disabled)
{
    memset(attr, 0, sizeof(*attr));
    attr->type = PERF_TYPE_BREAKPOINT;
    attr->size = sizeof(*attr);
    attr->sample_period = 1;
    attr->bp_type = HW_BREAKPOINT_X;
    attr->bp_addr = addr;
    attr->bp_len = HW_BREAKPOINT_LEN_4;
    if (disabled) {
        attr->flags = 1;   /* disabled */
    }
}

static int sh_hwbp_resolve(void)
{
    if (!sh_vmalloc) {
        sh_vmalloc = (typeof(sh_vmalloc))kallsyms_lookup_name("vmalloc");
        sh_vfree = (typeof(sh_vfree))kallsyms_lookup_name("vfree");
        sh_copy_from_user = (typeof(sh_copy_from_user))kallsyms_lookup_name("__arch_copy_from_user");
        sh_find_task_by_vpid = (typeof(sh_find_task_by_vpid))kallsyms_lookup_name("find_task_by_vpid");
    }
    if (!sh_register_user_hw_breakpoint) {
        sh_register_user_hw_breakpoint = (typeof(sh_register_user_hw_breakpoint))
            kallsyms_lookup_name("register_user_hw_breakpoint");
        sh_perf_event_create_kernel_counter = (typeof(sh_perf_event_create_kernel_counter))
            kallsyms_lookup_name("perf_event_create_kernel_counter");
        sh_modify_user_hw_breakpoint = (typeof(sh_modify_user_hw_breakpoint))
            kallsyms_lookup_name("modify_user_hw_breakpoint");
        sh_unregister_hw_breakpoint = (typeof(sh_unregister_hw_breakpoint))
            kallsyms_lookup_name("unregister_hw_breakpoint");
        sh_perf_event_release_kernel = (typeof(sh_perf_event_release_kernel))
            kallsyms_lookup_name("perf_event_release_kernel");
        sh_perf_event_enable = (typeof(sh_perf_event_enable))
            kallsyms_lookup_name("perf_event_enable");
        sh_perf_event_disable = (typeof(sh_perf_event_disable))
            kallsyms_lookup_name("perf_event_disable");
        sh_dbg("[hwbp] register=%llx counter=%llx modify=%llx unregister=%llx release=%llx en=%llx dis=%llx\n",
               (uint64_t)sh_register_user_hw_breakpoint,
               (uint64_t)sh_perf_event_create_kernel_counter,
               (uint64_t)sh_modify_user_hw_breakpoint,
               (uint64_t)sh_unregister_hw_breakpoint,
               (uint64_t)sh_perf_event_release_kernel,
               (uint64_t)sh_perf_event_enable,
               (uint64_t)sh_perf_event_disable);
    }

    if ((!sh_register_user_hw_breakpoint && !sh_perf_event_create_kernel_counter) ||
        (!sh_unregister_hw_breakpoint && !sh_perf_event_release_kernel)) {
        pr_err("[hwbp] required register/release symbols missing\n");
        return 0;
    }
    return 1;
}

static void rearm_to(struct sh_hwbp_node *node, uint64_t addr)
{
    struct sh_perf_event_attr attr;

    if (!sh_modify_user_hw_breakpoint || !node->bp_handle) {
        return;
    }
    node->bp_addr = addr;
    fill_execute_attr(&attr, addr, 0);
    if (sh_perf_event_disable) {
        sh_perf_event_disable(node->bp_handle);
    }
    sh_modify_user_hw_breakpoint(node->bp_handle, &attr);
    if (sh_perf_event_enable) {
        sh_perf_event_enable(node->bp_handle);
    }
}

/* 断点触发回调：perf 溢出处理，运行在调试异常上下文（原子、不可睡眠） */
static void hwbp_handler(void *bp, void *sample_data, struct sh_pt_regs *regs)
{
    struct list_head *pos;
    struct sh_hwbp_node *matched = 0;
    struct sh_hwbp_node *single_active = 0;
    int active = 0;
    int i;

    (void)sample_data;
    hwbp_enter_count++;

    for (pos = hwbp_list.next; pos != &hwbp_list; pos = pos->next) {
        struct sh_hwbp_node *node =
            (struct sh_hwbp_node *)((char *)pos - __builtin_offsetof(struct sh_hwbp_node, list));
        if (!node->is_disabled) {
            single_active = node;
            active++;
        }
        if (!matched && node->bp_handle == bp && !node->is_disabled) {
            matched = node;
            break;
        }
    }
    if ((!matched || matched->is_disabled) && active == 1) {
        matched = single_active;
    }
    if (!matched || matched->is_disabled) {
        hwbp_unmatched_count++;
        return;
    }

    matched->hit_count++;
    matched->last_pc = regs->pc;
    matched->last_x0 = regs->regs[0];
    matched->last_x1 = regs->regs[1];

    /* SKIP_ORIGIN：直接返回假值，跳到 LR */
    if (matched->flags & SH_HWBP_FLAG_SKIP_ORIGIN) {
        regs->regs[0] = matched->ret_value;
        regs->pc = STRIP_PAC(regs->regs[30]);
        return;
    }

    /* MODIFY_ARGS：只在入口改，返回那一拍不改 */
    if ((matched->flags & SH_HWBP_FLAG_MODIFY_ARGS) && !matched->is_waiting_return) {
        for (i = 0; i < 8; i++) {
            if (matched->override_mask & (1ULL << i)) {
                regs->regs[i] = matched->override_args[i];
            }
        }
    }

    /* INLINE：PC 跳到 trampoline */
    if (matched->flags & SH_HWBP_FLAG_INLINE) {
        regs->pc = matched->trampoline_addr;
        return;
    }

    /* 观察者状态机 */
    if (matched->is_waiting_return) {
        matched->ret_x0 = regs->regs[0];
        matched->is_waiting_return = 0;
        rearm_to(matched, matched->orig_bp_addr);
        return;
    }

    if (matched->skip_count > 0 && matched->skipped < matched->skip_count) {
        matched->skipped++;
        if (matched->flags & SH_HWBP_FLAG_LISTEN_RET) {
            uint64_t lr = STRIP_PAC(regs->regs[30]);
            if (lr) {
                matched->is_waiting_return = 1;
                rearm_to(matched, lr);
            }
        }
        return;
    }

    for (i = 0; i < 8; i++) {
        matched->entry_args[i] = regs->regs[i];
    }
    matched->entry_lr = regs->regs[30];

    /* 按指针 dump 内存：关 PAN 才能读用户页 */
    if (matched->dump_size) {
        uint32_t dn = matched->dump_size > SH_DUMP_MAX ? SH_DUMP_MAX : matched->dump_size;
        uint32_t j;

        asm volatile(".inst 0xd500409f" ::: "memory");   /* msr pan, #0 */
        for (i = 0; i < 8; i++) {
            uint64_t ptr = regs->regs[i];
            if (ptr < 0x100000ULL || ptr > 0x7FFFFFFFFFFFULL) {
                matched->mem_len[i] = 0;
                continue;
            }
            for (j = 0; j < dn; j++) {
                matched->mem_dump[i][j] = *(volatile uint8_t *)(ptr + j);
            }
            matched->mem_len[i] = (uint16_t)dn;
        }
        asm volatile(".inst 0xd500419f" ::: "memory");   /* msr pan, #1 */
    }

    /* 入口捕获完，断点挪到 LR，等返回那一拍 */
    {
        uint64_t lr = STRIP_PAC(regs->regs[30]);

        if (!lr) {
            return;
        }
        matched->is_waiting_return = 1;
        rearm_to(matched, lr);
    }
}

long sh_hwbp_hook(struct sh_hwbp_request __user *ureq)
{
    struct sh_hwbp_request req;
    struct sh_hwbp_node *node;
    struct sh_perf_event_attr attr;
    void *task;

    if (!hwbp_list_ready) {
        INIT_LIST_HEAD(&hwbp_list);
        hwbp_list_ready = true;
    }
    if (!sh_hwbp_resolve()) {
        return -95;
    }
    if (!sh_copy_from_user || sh_copy_from_user(&req, ureq, sizeof(req))) {
        return -14;
    }
    if (!req.target_addr || !req.target_pid) {
        return -22;
    }
    if (!(req.flags & SH_HWBP_FLAG_EXECUTE)) {
        return -22;
    }
    if (!sh_find_task_by_vpid) {
        return -95;
    }

    task = sh_find_task_by_vpid((pid_t)req.target_pid);
    if (!task) {
        pr_err("[hwbp] tid not found: %d\n", req.target_pid);
        return -3;
    }

    node = (struct sh_hwbp_node *)sh_vmalloc(sizeof(*node));
    if (!node) {
        return -12;
    }
    memset(node, 0, sizeof(*node));
    node->target_addr = req.target_addr;
    node->orig_bp_addr = req.target_addr;
    node->bp_addr = req.target_addr;
    node->tid = req.target_pid;
    node->flags = req.flags;
    node->trampoline_addr = req.trampoline_addr;
    node->dump_size = req.dump_size > SH_DUMP_MAX ? SH_DUMP_MAX : req.dump_size;

    fill_execute_attr(&attr, req.target_addr, (req.flags & SH_HWBP_FLAG_NO_AUTO_ENABLE) ? 1 : 0);
    if (sh_perf_event_create_kernel_counter) {
        node->bp_handle = sh_perf_event_create_kernel_counter(&attr, -1, task, hwbp_handler, 0);
    } else {
        node->bp_handle = sh_register_user_hw_breakpoint(&attr, hwbp_handler, 0, task);
    }
    if (!node->bp_handle || (uint64_t)node->bp_handle >= (uint64_t)-SH_MAX_ERRNO) {
        long err = node->bp_handle ? (long)(uintptr_t)node->bp_handle : -5;

        pr_err("[hwbp] register failed: %llx err=%ld attr_size=%u bp_addr=%llx\n",
               (uint64_t)node->bp_handle, err, attr.size, attr.bp_addr);
        sh_vfree(node);
        return err;
    }

    node->is_disabled = (req.flags & SH_HWBP_FLAG_NO_AUTO_ENABLE) ? 1 : 0;
    node->list.next = hwbp_list.next;
    node->list.prev = &hwbp_list;
    hwbp_list.next->prev = &node->list;
    hwbp_list.next = &node->list;

    sh_dbg("[hwbp] hooked tid=%d addr=%llx handle=%llx flags=%x\n",
           node->tid, node->target_addr, (uint64_t)node->bp_handle, node->flags);
    return 0;
}

long sh_hwbp_unhook(uint64_t target_addr, uint32_t tid)
{
    struct list_head *pos;
    struct list_head *tmp;

    if (!hwbp_list_ready) {
        return -1;
    }
    for (pos = hwbp_list.next; pos != &hwbp_list; pos = tmp) {
        struct sh_hwbp_node *node =
            (struct sh_hwbp_node *)((char *)pos - __builtin_offsetof(struct sh_hwbp_node, list));
        tmp = pos->next;
        if (node->target_addr == target_addr && (tid == 0 || node->tid == tid)) {
            if (node->bp_handle) {
                if (sh_perf_event_release_kernel) {
                    sh_perf_event_release_kernel(node->bp_handle);
                } else if (sh_unregister_hw_breakpoint) {
                    sh_unregister_hw_breakpoint(node->bp_handle);
                }
            }
            pos->prev->next = pos->next;
            pos->next->prev = pos->prev;
            sh_vfree(node);
            return 0;
        }
    }
    return -1;
}

long sh_hwbp_enable(uint64_t target_addr, uint32_t tid, int enable)
{
    struct list_head *pos;

    if (!hwbp_list_ready) {
        return -1;
    }
    for (pos = hwbp_list.next; pos != &hwbp_list; pos = pos->next) {
        struct sh_hwbp_node *node =
            (struct sh_hwbp_node *)((char *)pos - __builtin_offsetof(struct sh_hwbp_node, list));
        if (node->target_addr == target_addr && (tid == 0 || node->tid == tid)) {
            node->is_disabled = enable ? 0 : 1;
            if (node->bp_handle) {
                if (enable && sh_perf_event_enable) {
                    sh_perf_event_enable(node->bp_handle);
                } else if (!enable && sh_perf_event_disable) {
                    sh_perf_event_disable(node->bp_handle);
                }
            }
            return 0;
        }
    }
    return -1;
}

long sh_hwbp_query(struct sh_hwbp_query __user *ureq)
{
    struct sh_hwbp_query req;
    struct list_head *pos;
    struct sh_hwbp_node *chosen = 0;
    struct sh_hwbp_node *node;
    uint32_t total = 0;
    uint32_t index;
    int i;

    if (!hwbp_list_ready) {
        return -2;
    }
    sh_hwbp_resolve();
    if (!sh_copy_from_user || sh_copy_from_user(&req, ureq, sizeof(req))) {
        return -14;
    }

    index = req.node_index;
    for (pos = hwbp_list.next; pos != &hwbp_list; pos = pos->next) {
        node = (struct sh_hwbp_node *)((char *)pos - __builtin_offsetof(struct sh_hwbp_node, list));
        if (node->target_addr == req.target_addr && (req.tid == 0 || node->tid == req.tid)) {
            if (total == index) {
                chosen = node;
            }
            total++;
        }
    }

    req.node_total = total;
    if (!chosen) {
        req.hit_count = 0;
        req.tid = 0;
        compat_copy_to_user(ureq, &req, sizeof(req));
        return total > 0 ? 0 : -2;
    }

    node = chosen;
    req.tid = node->tid;
    req.hit_count = node->hit_count;
    req.last_pc = node->last_pc;
    req.last_x0 = node->last_x0;
    req.last_x1 = node->last_x1;
    req.is_disabled = (uint32_t)node->is_disabled;
    req.is_waiting_return = (uint32_t)node->is_waiting_return;
    req.handler_enter_count = hwbp_enter_count;
    req.handler_unmatched_count = hwbp_unmatched_count;
    for (i = 0; i < 8; i++) {
        req.entry_args[i] = node->entry_args[i];
    }
    req.ret_x0 = node->ret_x0;
    req.entry_lr = node->entry_lr;
    for (i = 0; i < 8; i++) {
        req.mem_len[i] = node->mem_len[i];
        if (node->mem_len[i]) {
            memcpy(req.mem_dump[i], node->mem_dump[i], node->mem_len[i]);
        }
    }

    if (compat_copy_to_user(ureq, &req, sizeof(req)) != (int)sizeof(req)) {
        return -14;
    }
    return 0;
}

long sh_hwbp_set_override(struct sh_hwbp_override __user *ureq)
{
    struct sh_hwbp_override req;
    struct list_head *pos;
    int found = 0;
    int i;

    if (!hwbp_list_ready) {
        return -2;
    }
    sh_hwbp_resolve();
    if (!sh_copy_from_user || sh_copy_from_user(&req, ureq, sizeof(req))) {
        return -14;
    }

    for (pos = hwbp_list.next; pos != &hwbp_list; pos = pos->next) {
        struct sh_hwbp_node *node =
            (struct sh_hwbp_node *)((char *)pos - __builtin_offsetof(struct sh_hwbp_node, list));
        if (node->target_addr == req.target_addr &&
            (req.target_pid == 0 || node->tid == req.target_pid)) {
            node->override_mask = req.override_mask;
            for (i = 0; i < 8; i++) {
                node->override_args[i] = req.override_args[i];
            }
            node->ret_value = req.ret_value;
            if (req.flags & SH_HWBP_FLAG_MODIFY_ARGS) {
                node->flags |= SH_HWBP_FLAG_MODIFY_ARGS;
            }
            if (req.flags & SH_HWBP_FLAG_SKIP_ORIGIN) {
                node->flags |= SH_HWBP_FLAG_SKIP_ORIGIN;
            }
            found++;
        }
    }
    return found > 0 ? 0 : -2;
}

long sh_hwbp_set_skip(uint64_t target_addr, uint32_t skip_count)
{
    struct list_head *pos;
    int found = 0;

    if (!hwbp_list_ready) {
        return -2;
    }
    for (pos = hwbp_list.next; pos != &hwbp_list; pos = pos->next) {
        struct sh_hwbp_node *node =
            (struct sh_hwbp_node *)((char *)pos - __builtin_offsetof(struct sh_hwbp_node, list));
        if (node->target_addr == target_addr) {
            node->skip_count = skip_count;
            node->skipped = 0;
            node->hit_count = 0;
            found++;
        }
    }
    return found > 0 ? 0 : -2;
}

void sh_hwbp_init(void)
{
    INIT_LIST_HEAD(&hwbp_list);
    hwbp_list_ready = true;
}

void sh_hwbp_cleanup(void)
{
    struct list_head *pos;
    struct list_head *tmp;

    if (!hwbp_list_ready) {
        return;
    }
    for (pos = hwbp_list.next; pos != &hwbp_list; pos = tmp) {
        struct sh_hwbp_node *node =
            (struct sh_hwbp_node *)((char *)pos - __builtin_offsetof(struct sh_hwbp_node, list));
        tmp = pos->next;
        if (node->bp_handle) {
            if (sh_perf_event_release_kernel) {
                sh_perf_event_release_kernel(node->bp_handle);
            } else if (sh_unregister_hw_breakpoint) {
                sh_unregister_hw_breakpoint(node->bp_handle);
            }
        }
        pos->prev->next = pos->next;
        pos->next->prev = pos->prev;
        sh_vfree(node);
    }
}
