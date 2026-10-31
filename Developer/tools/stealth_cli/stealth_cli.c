/*
 * stealth_cli —— stealth_hook 模块的命令行客户端
 *
 * 用途：在设备上通过 syscall 427（io_uring_register）与本仓 modules/KPM/stealth_hook
 * 通信，完成硬件断点（perf hw_breakpoint）、幽灵内存、PTE 能力探测。
 *
 * 与上游 xiaojianbang_hook 的关系：协议结构体逐字段一致，但去掉了
 *   - 自动加载 .kpm（由 APatch 管理器负责，客户端不抢 supercall 通道）
 *   - DBI 整页重编译（本移植未做 PTE 重定向，只保留能力探测）
 *
 * 编译：
 *   aarch64-linux-android31-clang -O2 -static -o stealth_cli stealth_cli.c
 * 或 make（见同目录 Makefile）。
 *
 * 用法见 usage()。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <sys/syscall.h>

#define SH_SYSCALL_MAGIC 0x584A42L
#define SH_SYSCALL_NR    427

#define SH_CMD_STATUS       0x0001
#define SH_CMD_ECHO         0x0002
#define SH_CMD_PTE_QUERY    0x2001
#define SH_CMD_PTE_HOOK     0x2002
#define SH_CMD_PTE_REARM    0x2003
#define SH_CMD_PTE_UNHOOK   0x2004
#define SH_CMD_GHOST_ALLOC  0x3001
#define SH_CMD_GHOST_FREE   0x3002
#define SH_CMD_GHOST_WRITE  0x3003
#define SH_CMD_GHOST_LIST   0x3004
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

#define SH_DUMP_MAX       128
#define SH_GHOST_LIST_MAX 32

struct sh_hwbp_request {
    uint64_t target_addr;
    uint32_t target_pid;
    uint32_t flags;
    uint64_t callback_addr;
    uint64_t trampoline_addr;
    uint32_t dump_size;
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
    uint64_t entry_args[8];
    uint64_t ret_x0;
    uint64_t entry_lr;
    uint8_t  mem_dump[8][SH_DUMP_MAX];
    uint16_t mem_len[8];
    uint32_t node_index;
    uint32_t node_total;
};

struct sh_hwbp_override {
    uint64_t target_addr;
    uint32_t target_pid;
    uint32_t flags;
    uint64_t override_mask;
    uint64_t override_args[8];
    uint64_t ret_value;
};

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

static long sh_call(long cmd, long a1, long a2, long a3, long a4)
{
    errno = 0;
    return syscall(SH_SYSCALL_NR, SH_SYSCALL_MAGIC, cmd, a1, a2, a3, a4);
}

static void die(const char *what, long rc)
{
    /* libc 的 syscall() 把内核负返回值折叠成 -1 + errno，rc == -1 时必须看 errno */
    if (rc == -1) {
        fprintf(stderr, "%s: %s (errno=%d)\n", what, strerror(errno), errno);
    } else {
        fprintf(stderr, "%s: rc=%ld\n", what, rc);
    }
    exit(1);
}

static int parse_u64(const char *s, uint64_t *out)
{
    char *end = NULL;
    errno = 0;
    unsigned long long v = strtoull(s, &end, 0);
    if (errno || !end || *end) {
        return -1;
    }
    *out = (uint64_t)v;
    return 0;
}

static uint32_t parse_tid(const char *s)
{
    uint64_t v = 0;
    if (parse_u64(s, &v) != 0) {
        fprintf(stderr, "bad tid: %s\n", s);
        exit(1);
    }
    return (uint32_t)v;
}

static void usage(void)
{
    printf(
        "stealth_cli —— stealth_hook 客户端（syscall %d, magic 0x%lX）\n"
        "\n"
        "  status                             查询模块状态\n"
        "  hook <addr> [--tid N] [--dump N] [--flags F] [--inline ADDR] [--listen-ret]\n"
        "                                     下硬件执行断点（默认挂当前线程）\n"
        "  unhook <addr> [--tid N]            摘断点\n"
        "  enable|disable <addr> [--tid N]    启停断点\n"
        "  query <addr> [--tid N] [--index N] 读命中记录（参数/返回值/dump）\n"
        "  override <addr> [--tid N] --set N=VAL ... [--modify]\n"
        "                                     改第 N 个参数（X0..X7）\n"
        "  skip <addr> [--count N]           忽略接下来 N 次命中（不记录）\n"
        "  ghost alloc <size> [--va ADDR] [--prot rwx]\n"
        "  ghost write <va> <hex|@file>       把数据写进幽灵内存\n"
        "  ghost list                         列出幽灵内存段\n"
        "  ghost free <va>                    释放\n"
        "  pte query                          查本机是否支持 PTE 重定向\n"
        "\n"
        "地址可写 0x... 或十进制；--tid 省略时用当前线程。\n"
        "所有命令需要受信 uid（root 或 shell），否则内核侧返回 EPERM。\n",
        SH_SYSCALL_NR, (unsigned long)SH_SYSCALL_MAGIC);
}

static int cmd_status(void)
{
    long rc = sh_call(SH_CMD_STATUS, 0, 0, 0, 0);
    if (rc < 0) {
        die("status", rc);
    }
    printf("status: 0x%lx  (magic 0x%X, version %u.%u.%u)\n", rc,
           (unsigned)((rc >> 16) & 0xFFFF), (unsigned)((rc >> 16) & 0xFF),
           (unsigned)((rc >> 8) & 0xFF), (unsigned)(rc & 0xFF));
    return 0;
}

static int cmd_pte(const char *sub)
{
    if (strcmp(sub, "query") != 0) {
        fprintf(stderr, "pte: only 'query' is supported\n");
        return 1;
    }
    long rc = sh_call(SH_CMD_PTE_QUERY, 0, 0, 0, 0);
    if (rc < 0) {
        die("pte query", rc);
    }
    printf("pte supported: %s (0x%lx)\n", rc ? "yes (5.10 vendor hook)" : "no (6.1+ removed)", rc);
    return 0;
}

static int cmd_hook(int argc, char **argv)
{
    struct sh_hwbp_request req;
    uint64_t addr = 0;
    uint32_t tid = (uint32_t)syscall(SYS_gettid);
    uint32_t dump = 0;
    uint32_t flags = SH_HWBP_FLAG_EXECUTE;
    uint64_t tramp = 0;
    int i;
    long rc;

    if (argc < 1 || parse_u64(argv[0], &addr) != 0) {
        fprintf(stderr, "hook: need <addr>\n");
        return 1;
    }
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--tid") && i + 1 < argc) {
            tid = parse_tid(argv[++i]);
        } else if (!strcmp(argv[i], "--dump") && i + 1 < argc) {
            dump = (uint32_t)parse_tid(argv[++i]);
        } else if (!strcmp(argv[i], "--flags") && i + 1 < argc) {
            uint64_t f = 0;
            parse_u64(argv[++i], &f);
            flags = (uint32_t)f;
        } else if (!strcmp(argv[i], "--inline") && i + 1 < argc) {
            parse_u64(argv[++i], &tramp);
            flags |= SH_HWBP_FLAG_INLINE;
        } else if (!strcmp(argv[i], "--listen-ret")) {
            flags |= SH_HWBP_FLAG_LISTEN_RET;
        } else {
            fprintf(stderr, "hook: unknown option %s\n", argv[i]);
            return 1;
        }
    }

    memset(&req, 0, sizeof(req));
    req.target_addr = addr;
    req.target_pid = tid;
    req.flags = flags;
    req.trampoline_addr = tramp;
    req.dump_size = dump;

    rc = sh_call(SH_CMD_HWBP_HOOK, (long)&req, 0, 0, 0);
    if (rc < 0) {
        die("hook", rc);
    }
    printf("hook: rc=%ld addr=0x%llx tid=%u flags=0x%x\n", rc,
           (unsigned long long)addr, tid, flags);
    return 0;
}

static int cmd_unhook(int argc, char **argv)
{
    uint64_t addr = 0;
    uint32_t tid = (uint32_t)syscall(SYS_gettid);
    long rc;

    if (argc < 1 || parse_u64(argv[0], &addr) != 0) {
        fprintf(stderr, "unhook: need <addr>\n");
        return 1;
    }
    if (argc >= 3 && !strcmp(argv[1], "--tid")) {
        tid = parse_tid(argv[2]);
    }
    rc = sh_call(SH_CMD_HWBP_UNHOOK, (long)addr, (long)tid, 0, 0);
    if (rc < 0) {
        die("unhook", rc);
    }
    printf("unhook: rc=%ld addr=0x%llx tid=%u\n", rc, (unsigned long long)addr, tid);
    return 0;
}

static int cmd_enable(int argc, char **argv, int enable)
{
    uint64_t addr = 0;
    uint32_t tid = (uint32_t)syscall(SYS_gettid);
    long rc;

    if (argc < 1 || parse_u64(argv[0], &addr) != 0) {
        fprintf(stderr, "%s: need <addr>\n", enable ? "enable" : "disable");
        return 1;
    }
    if (argc >= 3 && !strcmp(argv[1], "--tid")) {
        tid = parse_tid(argv[2]);
    }
    rc = sh_call(enable ? SH_CMD_HWBP_ENABLE : SH_CMD_HWBP_DISABLE, (long)addr, (long)tid, 0, 0);
    if (rc < 0) {
        die(enable ? "enable" : "disable", rc);
    }
    printf("%s: rc=%ld\n", enable ? "enable" : "disable", rc);
    return 0;
}

static void dump_hex(const uint8_t *p, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        printf("%02x", p[i]);
        if ((i + 1) % 32 == 0) {
            printf("\n");
        }
    }
    if (n % 32) {
        printf("\n");
    }
}

static int cmd_query(int argc, char **argv)
{
    struct sh_hwbp_query q;
    uint64_t addr = 0;
    uint32_t tid = (uint32_t)syscall(SYS_gettid);
    uint32_t index = 0;
    int i;
    long rc;

    if (argc < 1 || parse_u64(argv[0], &addr) != 0) {
        fprintf(stderr, "query: need <addr>\n");
        return 1;
    }
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--tid") && i + 1 < argc) {
            tid = parse_tid(argv[++i]);
        } else if (!strcmp(argv[i], "--index") && i + 1 < argc) {
            index = parse_tid(argv[++i]);
        }
    }

    memset(&q, 0, sizeof(q));
    q.target_addr = addr;
    q.tid = tid;
    q.node_index = index;

    rc = sh_call(SH_CMD_HWBP_QUERY, (long)&q, 0, 0, 0);
    if (rc < 0) {
        die("query", rc);
    }
    printf("query: rc=%ld total=%u index=%u\n", rc, q.node_total, q.node_index);
    printf("  hits=%u enter=%u unmatched=%u disabled=%u waiting_ret=%u\n",
           q.hit_count, q.handler_enter_count, q.handler_unmatched_count,
           q.is_disabled, q.is_waiting_return);
    printf("  last_pc=0x%llx last_x0=0x%llx last_x1=0x%llx\n",
           (unsigned long long)q.last_pc, (unsigned long long)q.last_x0,
           (unsigned long long)q.last_x1);
    printf("  entry lr=0x%llx ret_x0=0x%llx\n",
           (unsigned long long)q.entry_lr, (unsigned long long)q.ret_x0);
    printf("  args:");
    for (i = 0; i < 8; i++) {
        printf(" x%d=0x%llx", i, (unsigned long long)q.entry_args[i]);
    }
    printf("\n");
    for (i = 0; i < 8; i++) {
        if (q.mem_len[i]) {
            printf("  mem[x%d] len=%u: ", i, q.mem_len[i]);
            dump_hex(q.mem_dump[i], q.mem_len[i] > SH_DUMP_MAX ? SH_DUMP_MAX : q.mem_len[i]);
        }
    }
    return 0;
}

static int cmd_override(int argc, char **argv)
{
    struct sh_hwbp_override o;
    uint64_t addr = 0;
    uint32_t tid = 0;
    int i;
    long rc;

    if (argc < 1 || parse_u64(argv[0], &addr) != 0) {
        fprintf(stderr, "override: need <addr>\n");
        return 1;
    }
    memset(&o, 0, sizeof(o));
    o.target_addr = addr;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--tid") && i + 1 < argc) {
            tid = parse_tid(argv[++i]);
        } else if (!strcmp(argv[i], "--set") && i + 1 < argc) {
            char *spec = argv[++i];
            char *eq = strchr(spec, '=');
            uint64_t idx = 0, val = 0;
            if (!eq) {
                fprintf(stderr, "--set needs N=VAL\n");
                return 1;
            }
            *eq = 0;
            if (parse_u64(spec, &idx) != 0 || parse_u64(eq + 1, &val) != 0 || idx > 7) {
                fprintf(stderr, "bad --set %s\n", spec);
                return 1;
            }
            o.override_mask |= (1ull << idx);
            o.override_args[idx] = val;
        } else if (!strcmp(argv[i], "--modify")) {
            o.flags |= SH_HWBP_FLAG_MODIFY_ARGS;
        } else if (!strcmp(argv[i], "--skip-ret") && i + 1 < argc) {
            uint64_t rv = 0;
            parse_u64(argv[++i], &rv);
            o.flags |= SH_HWBP_FLAG_SKIP_ORIGIN;
            o.ret_value = rv;
        } else {
            fprintf(stderr, "override: unknown option %s\n", argv[i]);
            return 1;
        }
    }
    o.target_pid = tid;
    if (!o.flags) {
        o.flags = SH_HWBP_FLAG_MODIFY_ARGS;
    }

    rc = sh_call(SH_CMD_HWBP_SET_OVERRIDE, (long)&o, 0, 0, 0);
    if (rc < 0) {
        die("override", rc);
    }
    printf("override: rc=%ld mask=0x%llx\n", rc, (unsigned long long)o.override_mask);
    return 0;
}

static int cmd_skip(int argc, char **argv)
{
    uint64_t addr = 0;
    uint32_t count = 1;
    int i;
    long rc;

    if (argc < 1 || parse_u64(argv[0], &addr) != 0) {
        fprintf(stderr, "skip: need <addr> [--count N]\n");
        return 1;
    }
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--count") && i + 1 < argc) {
            count = parse_tid(argv[++i]);
        }
    }
    rc = sh_call(SH_CMD_HWBP_SET_SKIP, (long)addr, (long)count, 0, 0);
    if (rc < 0) {
        die("skip", rc);
    }
    printf("skip: rc=%ld count=%u (接下来 %u 次命中不记录参数/返回值)\n", rc, count, count);
    return 0;
}

static int cmd_ghost(int argc, char **argv)
{
    if (argc < 1) {
        fprintf(stderr, "ghost: need subcommand (alloc|write|list|free)\n");
        return 1;
    }
    if (!strcmp(argv[0], "alloc")) {
        uint64_t size = 0, va = 0;
        uint32_t prot = 3; /* rw */
        int i;
        long rc;
        if (argc < 2 || parse_u64(argv[1], &size) != 0) {
            fprintf(stderr, "ghost alloc: need <size>\n");
            return 1;
        }
        for (i = 2; i < argc; i++) {
            if (!strcmp(argv[i], "--va") && i + 1 < argc) {
                parse_u64(argv[++i], &va);
            } else if (!strcmp(argv[i], "--prot") && i + 1 < argc) {
                const char *p = argv[++i];
                prot = 0;
                if (strchr(p, 'r')) prot |= 1;
                if (strchr(p, 'w')) prot |= 2;
                if (strchr(p, 'x')) prot |= 4;
            }
        }
        rc = sh_call(SH_CMD_GHOST_ALLOC, (long)va, (long)size, (long)prot, 0);
        if (rc < 0) {
            die("ghost alloc", rc);
        }
        printf("ghost alloc: va=0x%lx size=%llu prot=%u\n", rc,
               (unsigned long long)size, prot);
        return 0;
    }
    if (!strcmp(argv[0], "free")) {
        uint64_t va = 0;
        long rc;
        if (argc < 2 || parse_u64(argv[1], &va) != 0) {
            fprintf(stderr, "ghost free: need <va>\n");
            return 1;
        }
        rc = sh_call(SH_CMD_GHOST_FREE, (long)va, 0, 0, 0);
        if (rc < 0) {
            die("ghost free", rc);
        }
        printf("ghost free: rc=%ld\n", rc);
        return 0;
    }
    if (!strcmp(argv[0], "write")) {
        struct sh_ghost_write_request w;
        uint64_t va = 0, size = 0;
        uint8_t buf[4096];
        const char *data;
        size_t n = 0, i;
        long rc;

        if (argc < 3 || parse_u64(argv[1], &va) != 0) {
            fprintf(stderr, "ghost write: need <va> <hex|@file>\n");
            return 1;
        }
        data = argv[2];
        if (data[0] == '@') {
            FILE *fp = fopen(data + 1, "rb");
            if (!fp) {
                perror("open");
                return 1;
            }
            n = fread(buf, 1, sizeof(buf), fp);
            fclose(fp);
        } else {
            size_t len = strlen(data);
            if (len % 2) {
                fprintf(stderr, "hex string must have even length\n");
                return 1;
            }
            for (i = 0; i < len; i += 2) {
                unsigned int b = 0;
                if (sscanf(data + i, "%2x", &b) != 1) {
                    fprintf(stderr, "bad hex at %zu\n", i);
                    return 1;
                }
                if (n < sizeof(buf)) {
                    buf[n++] = (uint8_t)b;
                }
            }
        }
        size = n;
        memset(&w, 0, sizeof(w));
        w.target_va = va;
        w.src_data = (uint64_t)(uintptr_t)buf;
        w.size = size;
        rc = sh_call(SH_CMD_GHOST_WRITE, (long)&w, 0, 0, 0);
        if (rc < 0) {
            die("ghost write", rc);
        }
        printf("ghost write: rc=%ld bytes=%llu\n", rc, (unsigned long long)size);
        return 0;
    }
    if (!strcmp(argv[0], "list")) {
        struct sh_ghost_info info[SH_GHOST_LIST_MAX];
        long rc;
        int i;
        memset(info, 0, sizeof(info));
        rc = sh_call(SH_CMD_GHOST_LIST, (long)info, SH_GHOST_LIST_MAX, 0, 0);
        if (rc < 0) {
            die("ghost list", rc);
        }
        printf("ghost list: %ld segment(s)\n", rc);
        for (i = 0; i < rc && i < SH_GHOST_LIST_MAX; i++) {
            printf("  [%d] va=0x%llx size=%llu pid=%u prot=%u\n", i,
                   (unsigned long long)info[i].va, (unsigned long long)info[i].size,
                   info[i].pid, info[i].prot);
        }
        return 0;
    }
    fprintf(stderr, "ghost: unknown subcommand %s\n", argv[0]);
    return 1;
}

/* ---------------------------------------------------------------
 * selftest：对自己进程里的 target() 下断点，把整条链路跑一遍。
 * 用来在一台陌生设备上确认「通道通、断点能装、参数能抓、能改、能跳」。
 * --------------------------------------------------------------- */

static volatile int st_a = 0x11;
static volatile int st_b = 0x22;

__attribute__((noinline)) static int st_target(int a, int b)
{
    return a * 2 + b;
}

static int cmd_selftest(void)
{
    struct sh_hwbp_request req;
    struct sh_hwbp_query q;
    struct sh_hwbp_override o;
    uint64_t addr = (uint64_t)(uintptr_t)&st_target;
    uint32_t tid = (uint32_t)syscall(SYS_gettid);
    long rc;
    int v;

    printf("selftest: target=%p tid=%u\n", (void *)st_target, tid);

    v = st_target(st_a, st_b);
    printf("call before hook: %d (expect 68)\n", v);

    memset(&req, 0, sizeof(req));
    req.target_addr = addr;
    req.target_pid = tid;
    req.flags = SH_HWBP_FLAG_EXECUTE;
    rc = sh_call(SH_CMD_HWBP_HOOK, (long)&req, 0, 0, 0);
    if (rc < 0) {
        die("selftest hook", rc);
    }
    printf("hook: rc=%ld\n", rc);

    v = st_target(st_a, st_b);
    printf("call with hook: %d (expect 68)\n", v);

    memset(&q, 0, sizeof(q));
    q.target_addr = addr;
    q.tid = tid;
    rc = sh_call(SH_CMD_HWBP_QUERY, (long)&q, 0, 0, 0);
    if (rc < 0) {
        die("selftest query", rc);
    }
    printf("query: rc=%ld hits=%u enter=%u total=%u x0=0x%llx x1=0x%llx lr=0x%llx ret_x0=0x%llx\n",
           rc, q.hit_count, q.handler_enter_count, q.node_total,
           (unsigned long long)q.entry_args[0], (unsigned long long)q.entry_args[1],
           (unsigned long long)q.entry_lr, (unsigned long long)q.ret_x0);

    memset(&o, 0, sizeof(o));
    o.target_addr = addr;
    o.target_pid = tid;
    o.flags = SH_HWBP_FLAG_MODIFY_ARGS;
    o.override_mask = 1ull << 0;
    o.override_args[0] = 0x100;
    rc = sh_call(SH_CMD_HWBP_SET_OVERRIDE, (long)&o, 0, 0, 0);
    printf("override modify: rc=%ld\n", rc);
    v = st_target(st_a, st_b);
    printf("call with modify: %d (expect 546)\n", v);

    rc = sh_call(SH_CMD_HWBP_SET_SKIP, (long)addr, 1, 0, 0);
    printf("skip count=1: rc=%ld\n", rc);
    v = st_target(st_a, st_b);
    printf("call with skip(1): %d (仍执行原函数，skip 只跳过记录)\n", v);
    v = st_target(st_a, st_b);
    printf("call after skip used up: %d (expect 546)\n", v);

    memset(&o, 0, sizeof(o));
    o.target_addr = addr;
    o.target_pid = tid;
    o.flags = SH_HWBP_FLAG_SKIP_ORIGIN;
    o.ret_value = 0x777;
    rc = sh_call(SH_CMD_HWBP_SET_OVERRIDE, (long)&o, 0, 0, 0);
    printf("override skip: rc=%ld\n", rc);
    v = st_target(st_a, st_b);
    printf("call with skip: %d (expect 1911)\n", v);

    rc = sh_call(SH_CMD_HWBP_UNHOOK, (long)addr, (long)tid, 0, 0);
    printf("unhook: rc=%ld\n", rc);
    v = st_target(st_a, st_b);
    printf("call after unhook: %d (expect 68)\n", v);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage();
        return 1;
    }
    if (!strcmp(argv[1], "status")) {
        return cmd_status();
    }
    if (!strcmp(argv[1], "hook")) {
        return cmd_hook(argc - 2, argv + 2);
    }
    if (!strcmp(argv[1], "unhook")) {
        return cmd_unhook(argc - 2, argv + 2);
    }
    if (!strcmp(argv[1], "enable")) {
        return cmd_enable(argc - 2, argv + 2, 1);
    }
    if (!strcmp(argv[1], "disable")) {
        return cmd_enable(argc - 2, argv + 2, 0);
    }
    if (!strcmp(argv[1], "query")) {
        return cmd_query(argc - 2, argv + 2);
    }
    if (!strcmp(argv[1], "override")) {
        return cmd_override(argc - 2, argv + 2);
    }
    if (!strcmp(argv[1], "skip")) {
        return cmd_skip(argc - 2, argv + 2);
    }
    if (!strcmp(argv[1], "ghost")) {
        return cmd_ghost(argc - 2, argv + 2);
    }
    if (!strcmp(argv[1], "selftest")) {
        return cmd_selftest();
    }
    if (!strcmp(argv[1], "pte")) {
        if (argc < 3) {
            fprintf(stderr, "pte: need 'query'\n");
            return 1;
        }
        return cmd_pte(argv[2]);
    }
    if (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
        usage();
        return 0;
    }
    fprintf(stderr, "unknown command: %s\n", argv[1]);
    usage();
    return 1;
}
