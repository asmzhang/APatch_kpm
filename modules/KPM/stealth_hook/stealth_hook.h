/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * stealth_hook —— 硬件断点 / 幽灵内存 / 缺页重定向 工具集
 *
 * 来源：xiaojianbang-stealth-hook（GPL-2.0-or-later）的移植。
 * 上游是一个整体模块（syscall #285 通道 + HWBP + PTE + Ghost + MapsHide + PtraceSpoof）。
 * 本仓的分工见 Developer/reports/2026-10-31-stealth-hook-port.md：
 *   - MapsHide 已并入 memcrc 的 RANGE:uid:start:end 规则；
 *   - PtraceSpoof 已并入 memcrc 的 REGS:uid 规则；
 *   - 本模块只保留 HWBP / PTE / Ghost 三块，用独立的 syscall 通道。
 *
 * 通道：syscall 427（io_uring_register）。上游用 285（copy_file_range）——
 * 那是 IO 热路径，挂上去等于给每次 IO 加一次 magic 判断，故选冷门号。
 * 非本模块调用（magic 不匹配）原样放行，行为与未加载时一致。
 */

#ifndef _STEALTH_HOOK_H_
#define _STEALTH_HOOK_H_

#include <ktypes.h>

/* 调试日志：编译时加 -DSH_DEBUG 打开 */
#ifdef SH_DEBUG
#define sh_dbg(fmt, ...) pr_info(fmt, ##__VA_ARGS__)
#else
#define sh_dbg(fmt, ...) do { } while (0)
#endif

/* ============================================================
 * syscall 通道
 * ============================================================ */

#define SH_SYSCALL_MAGIC    0x584A42    /* "XJB"，沿用上游取值，CLI 兼容 */
#define SH_SYSCALL_NR       427         /* io_uring_register */

#define SH_CMD_STATUS       0x0001
#define SH_CMD_ECHO         0x0002

/* 硬件断点（perf hw_breakpoint，按线程注册） */
#define SH_CMD_HWBP_HOOK         0x1001
#define SH_CMD_HWBP_UNHOOK       0x1002
#define SH_CMD_HWBP_ENABLE       0x1003
#define SH_CMD_HWBP_DISABLE      0x1004
#define SH_CMD_HWBP_QUERY        0x1005
#define SH_CMD_HWBP_SET_OVERRIDE 0x1006
#define SH_CMD_HWBP_SET_SKIP     0x1007

#define SH_HWBP_FLAG_EXECUTE        (1u << 0)
#define SH_HWBP_FLAG_READ           (1u << 1)
#define SH_HWBP_FLAG_WRITE          (1u << 2)
#define SH_HWBP_FLAG_LISTEN_RET     (1u << 3)
#define SH_HWBP_FLAG_INLINE         (1u << 4)
#define SH_HWBP_FLAG_NO_AUTO_ENABLE (1u << 5)
#define SH_HWBP_FLAG_MODIFY_ARGS    (1u << 7)
#define SH_HWBP_FLAG_SKIP_ORIGIN    (1u << 8)

#define SH_DUMP_MAX         128

/* Ghost memory：在当前进程里建一段正规 VMA（vm_mmap），可读写可执行。
 * 「幽灵」这层不在这里做——由 memcrc 的 RANGE:uid:start:end 规则把它从
 * /proc/self/maps 抹掉。两块模块各管一件事，见开发报告「与上游的差异」。 */
/* 缺页重定向（PTE）：需要 5.10 厂商 hook `__tracepoint_android_rvh_do_mem_abort`。
 * 6.1 及以后该 hook 已移除，PTE 只能报「不支持」——不猜、不降级。 */
#define SH_CMD_PTE_QUERY    0x2001   /* 返回 1=本机支持 PTE，0=不支持 */
#define SH_CMD_PTE_HOOK     0x2002
#define SH_CMD_PTE_REARM    0x2003
#define SH_CMD_PTE_UNHOOK   0x2004

#define SH_CMD_GHOST_ALLOC  0x3001
#define SH_CMD_GHOST_FREE   0x3002
#define SH_CMD_GHOST_WRITE  0x3003
#define SH_CMD_GHOST_LIST   0x3004

#define SH_PROT_READ        (1u << 0)
#define SH_PROT_WRITE       (1u << 1)
#define SH_PROT_EXEC        (1u << 2)

#define SH_STATUS_MAGIC     0x53484F4B  /* "SHOK" */
#define SH_VERSION_CODE     0x00030000  /* 0.3.0 */

/* 受信 uid 表：只有这些 uid 能下发命令（默认 root / shell） */
#define SH_ALLOW_MAX        8

/* 一次 GHOST_LIST 最多回多少条 */
#define SH_GHOST_LIST_MAX   32

struct sh_ghost_write_request {
    uint64_t target_va;
    uint64_t src_data;
    uint64_t size;
    uint32_t target_pid;
    uint32_t _pad;
};

struct sh_ghost_info {
    uint64_t va;
    uint64_t size;
    uint32_t pid;
    uint32_t prot;
};

/* ============================================================
 * 硬件断点：用户态结构体（与上游 xiaojianbang-stealth-hook 逐字段一致，
 * 方便直接拿上游 CLI 的协议对表）
 * ============================================================ */

struct sh_hwbp_request {
    uint64_t target_addr;       /* 要下断点的地址 */
    uint32_t target_pid;        /* 目标线程 tid */
    uint32_t flags;             /* SH_HWBP_FLAG_* */
    uint64_t callback_addr;     /* 保留（上游 inline 模式用） */
    uint64_t trampoline_addr;   /* INLINE 模式下 PC 要跳去的地址 */
    uint32_t dump_size;         /* 每个指针参数 dump 多少字节（0 = 不 dump） */
    uint32_t _pad;
};

struct sh_hwbp_query {
    uint64_t target_addr;
    uint32_t tid;
    uint32_t hit_count;
    uint64_t last_pc;
    uint64_t last_x0;
    uint64_t last_x1;
    uint32_t is_disabled;
    uint32_t is_waiting_return;
    uint32_t handler_enter_count;
    uint32_t handler_unmatched_count;
    uint64_t entry_args[8];     /* 入口时的 X0-X7 */
    uint64_t ret_x0;            /* 返回时的 X0 */
    uint64_t entry_lr;          /* 入口时的 LR */
    uint8_t  mem_dump[8][SH_DUMP_MAX];
    uint16_t mem_len[8];
    uint32_t node_index;        /* in：取第几个匹配节点（0 起） */
    uint32_t node_total;        /* out：匹配节点总数 */
};

struct sh_hwbp_override {
    uint64_t target_addr;
    uint32_t target_pid;        /* 0 = 不限 */
    uint32_t flags;             /* MODIFY_ARGS / SKIP_ORIGIN */
    uint64_t override_mask;     /* 第 N 位 = 覆盖 regs[N] */
    uint64_t override_args[8];
    uint64_t ret_value;         /* SKIP_ORIGIN 时的返回值 */
};

#endif /* _STEALTH_HOOK_H_ */
