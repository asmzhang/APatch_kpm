/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * wxs_probe —— wxshadow 探针（P0 地址空间 + P1 双视图，不是成品模块）
 *
 * P0（已验证）：从 pid 走到目标 mm 的 leaf PTE，并把 PTE 里的物理地址换算成
 *   内核可读地址（线性映射），读出来的内容与目标进程自己读到的一致。
 *
 * P1（本版新增）：给目标进程的一页 4 KiB 匿名页装一份影子页，把该进程的 PTE
 *   指向影子页 —— 目标进程读同一 va 读到的是影子页内容；release 后 PTE 与内容
 *   逐字节复原。
 *
 * 换算方式不猜版本：kmalloc 一页 → 写魔数 → `at s1e1w` 取物理地址 →
 * linear_voffset = kva - pa → 回读自校验。不依赖 KP 的私有全局
 * （linear_voffset/memstart_addr/pgd_va 都没导出给 KPM），也不假设 PAGE_OFFSET
 * （PAGE_OFFSET 只用于打印）。
 *
 * ctl0：
 *   self                       打印自校验结果与换算因子
 *   probe <pid> <hexva>        打印该 va 的 pgd 项、leaf PTE、pa 与前 16 字节
 *   attach <pid> <hexva>       锁定目标 mm 与该 va 的 PTE（持有 mm 引用）
 *   install <hexbytes>         建影子页、写内容、切换 PTE（最多 16 字节）
 *   release                    复原 PTE 并释放影子页
 *   state                      打印当前状态（pte 现值/原值/影子 pa）
 *
 * 已知限制（P1 范围内有意为之，P2 再补）：
 *   - 只处理 4 KiB leaf，huge/block/table 一律拒绝；
 *   - 不取 mmap_lock（KP 没导出 mm_struct 的 mmap_lock 偏移），因此要求目标页
 *     在 install/release 期间不被 munmap/mremap；客户端是单线程只读，够用；
 *   - 只做数据视图（不涉及可执行页与 icache 同步），P2 再加 UXN fault 与执行视图。
 */

#include <log.h>
#include <compiler.h>
#include <kpmodule.h>
#include <kputils.h>
#include <hook.h>
#include <kallsyms.h>
#include <stdint.h>
#include <linux/printk.h>
#include <linux/string.h>
#include <linux/kernel.h>
#include <linux/mm_types.h>
#include <pgtable.h>
#include <asm/ptrace.h>

KPM_NAME("wxs_probe");
KPM_VERSION("0.0.2");
KPM_LICENSE("GPL v2");
KPM_AUTHOR("APatch_kpm");
KPM_DESCRIPTION("wxshadow probe: pgd walk + double-view PTE swap");

#define WXS_MAGIC "WXSPROBE-P1"
#define WXS_MAGIC_LEN 11

/* GFP_KERNEL = ___GFP_RECLAIM|___GFP_IO|___GFP_FS；5.x/6.x 一致，避免引 gfp 头。 */
#define WXS_GFP_KERNEL 0xcc0UL

#define WXS_PTE_ADDR_MASK 0x0000fffffffff000ULL

/* 状态机：0 空 / 1 已 attach / 2 已 install */
static int wxs_state;

static struct task_struct *(*kf_find_task_by_vpid)(pid_t nr);
static struct mm_struct *(*kf_get_task_mm)(struct task_struct *task);
static void (*kf_mmput)(struct mm_struct *mm);
static void *(*kf___kmalloc)(size_t size, unsigned long flags);
static void (*kf_kfree)(const void *p);

static uint64_t wxs_linear_voffset;
static int wxs_va_bits;
static uint64_t wxs_page_offset;
static int wxs_selfcheck;
static void *wxs_selfpage;

/* attach 期间保持住的现场 */
static pid_t wxs_tpid;
static struct mm_struct *wxs_tmm;
static uint64_t wxs_tva;
static uint64_t wxs_toff;   /* va 在页内的偏移，install 时就写在这个位置 */
static uint64_t *wxs_ptep;
static uint64_t wxs_pte_orig;
static uint64_t wxs_pte_installed;
static void *wxs_shadow;
static uint64_t wxs_shadow_pa;

extern struct mm_struct_offset mm_struct_offset;

/* AT S1E1W：用当前页表把内核 VA 翻成物理地址（成功时 PAR[0]=0）。 */
static uint64_t wxs_at_s1e1w(uint64_t va)
{
    uint64_t par;

    asm volatile("at s1e1w, %0" ::"r"(va) : "memory");
    asm volatile("isb" ::: "memory");
    asm volatile("mrs %0, par_el1" : "=r"(par));
    if (par & 1)
        return 0;
    return par & WXS_PTE_ADDR_MASK;
}

/*
 * 让所有 CPU 丢掉这个 va 的 TLB 项。用 VAAE1IS（VA, All ASID, EL1, inner shareable）：
 * 目标 mm 在别的核上也可能有该 va 的项，按 ASID 区分反而漏刷，所以整 ASID 一起刷。
 * VA 编码与内核 __TLBI_VADDR 一致：va>>12 取低 44 位，ASID 字段在这个变体里被忽略。
 */
static void wxs_tlbi_va(uint64_t va)
{
    uint64_t x = (va >> 12) & ((1ULL << 44) - 1);

    asm volatile("dsb ishst" ::: "memory");
    asm volatile("tlbi vaae1is, %0" ::"r"(x) : "memory");
    asm volatile("dsb ish" ::: "memory");
    asm volatile("isb" ::: "memory");
}

/* 把刚写过的影子页按缓存行清到 PoC，保证目标进程（EL0）之后读到新内容。 */
static void wxs_dcache_clean(const void *addr, size_t len)
{
    uint64_t p = (uint64_t)addr & ~63ULL;
    uint64_t end = ((uint64_t)addr + len + 63ULL) & ~63ULL;

    for (; p < end; p += 64)
        asm volatile("dc cvau, %0" ::"r"(p) : "memory");
    asm volatile("dsb ish" ::: "memory");
}

static int wxs_parse_u64(const char *s, uint64_t *out);
static int wxs_hexval(char c);

static int wxs_read_va_bits(void)
{
    uint64_t tcr;
    int t1sz;
    int bits;

    asm volatile("mrs %0, tcr_el1" : "=r"(tcr));
    t1sz = (int)((tcr >> 16) & 0x3f);
    if (t1sz == 0)
        return 48;
    bits = 64 - t1sz;
    if (bits < 32 || bits > 52)
        return 48;
    return bits;
}

static void wxs_selfcheck_do(void)
{
    uint64_t kva;
    uint64_t pa;

    wxs_selfpage = kf___kmalloc(4096, WXS_GFP_KERNEL);
    if (!wxs_selfpage) {
        logke("wxs_probe: kmalloc failed\n");
        return;
    }
    memcpy(wxs_selfpage, WXS_MAGIC, WXS_MAGIC_LEN);
    kva = (uint64_t)wxs_selfpage & ~0xfffULL;
    pa = wxs_at_s1e1w(kva);
    if (!pa) {
        logke("wxs_probe: at s1e1w failed\n");
        return;
    }
    wxs_linear_voffset = kva - pa;
    if (memcmp((void *)(pa + wxs_linear_voffset), WXS_MAGIC, WXS_MAGIC_LEN) == 0)
        wxs_selfcheck = 1;
    logki("wxs_probe: selfcheck=%d linear_voffset=0x%llx va_bits=%d\n", wxs_selfcheck,
          (unsigned long long)wxs_linear_voffset, wxs_va_bits);
}

static int wxs_hexval(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* 解析十六进制或十进制；返回消耗的字符数，0 表示解析失败。 */
static int wxs_parse_u64(const char *s, uint64_t *out)
{
    uint64_t v = 0;
    int i = 0;
    int base = 10;
    int d;

    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s += 2;
    }
    for (; s[i]; i++) {
        if (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')
            break;
        d = wxs_hexval(s[i]);
        if (d < 0 || d >= base)
            break;
        v = v * (uint64_t)base + (uint64_t)d;
    }
    if (i == 0)
        return 0;
    *out = v;
    return i;
}

/* 取目标 mm 的 pgd 与 va 对应的 leaf PTE 指针；返回 0 成功。 */
static int wxs_leaf_pte(struct mm_struct *mm, uint64_t va, uint64_t *ptep, uint64_t *pte_out,
                        const char **err)
{
    uint64_t pgd = *(uint64_t *)((char *)mm + mm_struct_offset.pgd_offset);
    uint64_t *peg = pgtable_entry(pgd, va);
    uint64_t pte;

    if (!peg) {
        *err = "no leaf entry";
        return -1;
    }
    pte = *peg;
    if (!(pte & PTE_VALID)) {
        *err = "page not resident";
        return -1;
    }
    if ((pte & PTE_TYPE_MASK) != PTE_TYPE_PAGE) {
        *err = "not a 4K leaf (block/table)";
        return -1;
    }
    *ptep = (uint64_t)peg;
    *pte_out = pte;
    return 0;
}

static int wxs_dump_page(pid_t pid, uint64_t va, char *out, int outlen)
{
    struct task_struct *task;
    struct mm_struct *mm;
    uint64_t ptep;
    uint64_t pte;
    uint64_t pa;
    const unsigned char *p;
    const char *err = 0;
    int i;
    int n;

    task = kf_find_task_by_vpid(pid);
    if (!task)
        return snprintf(out, outlen, "no such pid %d\n", pid);
    mm = kf_get_task_mm(task);
    if (!mm)
        return snprintf(out, outlen, "pid %d has no mm\n", pid);
    if (wxs_leaf_pte(mm, va, &ptep, &pte, &err) < 0) {
        n = snprintf(out, outlen, "pte=0x%llx: %s\n", (unsigned long long)pte, err);
        kf_mmput(mm);
        return n;
    }
    pa = (pte & WXS_PTE_ADDR_MASK) | (va & 0xfffULL);
    p = (const unsigned char *)(pa + wxs_linear_voffset);
    n = snprintf(out, outlen, "pid=%d va=0x%llx pte=0x%llx pa=0x%llx data=", pid,
                 (unsigned long long)va, (unsigned long long)pte, (unsigned long long)pa);
    for (i = 0; i < 16 && n < outlen - 4; i++)
        n += snprintf(out + n, outlen - n, "%02x", p[i]);
    if (n < outlen - 1)
        out[n++] = '\n';
    out[n] = 0;
    kf_mmput(mm);
    return n;
}

/* ---- P2b 第一步：只挂钩、只记录，不改任何 PTE ---- */
static uint64_t wxs_pte_uxn;
static int wxs_uxn_on;
static int wxs_hold;
static int wxs_xo_guard;
static unsigned long wxs_switch_count;
static unsigned long wxs_n_iabt, wxs_n_dabt, wxs_n_other;
static uint64_t wxs_pte_shadow_exec, wxs_pte_orig_exec;
static int wxs_dual;
static int wxs_dual_mode;
static unsigned long wxs_dual_sw;
static void *wxs_do_mem_abort;
static int wxs_hook_on;
static unsigned long wxs_fault_count;
static unsigned int wxs_fault_logged;

static void wxs_write_pte(uint64_t pte);

static void wxs_mem_abort_before(hook_fargs2_t *args, void *udata)
{
    uint64_t far = args->arg0;
    uint32_t esr = (uint32_t)args->arg1;
    uint32_t ec;
    uint32_t fsc;
    struct pt_regs *regs = (struct pt_regs *)args->arg2;

    (void)udata;
    if (wxs_state < 1)
        return;
    if (far < wxs_tva || far >= wxs_tva + 4096)
        return;
    wxs_fault_count++;
    ec = (esr >> 26) & 0x3f;
    if (ec == 0x20 || ec == 0x21)
        wxs_n_iabt++;
    else if (ec == 0x24 || ec == 0x25)
        wxs_n_dabt++;
    else
        wxs_n_other++;
    fsc = esr & 0x3f;

    /*
     * 单向切换：取指异常（EC=0x20 来自 EL0 的 IABT）+ 当前是 hold 状态
     * → 把 PTE 换成影子页、跳过内核默认处理，让 CPU 重试这条指令。
     * 若同一页连续切换太多次（说明影子页本身也取不到指），放弃处理并报错，
     * 避免把设备拖进死循环。
     */
    if (wxs_dual && (ec == 0x20 || ec == 0x24)) {
        if (wxs_dual_sw < 200000) {
            wxs_dual_sw++;
            if (ec == 0x20)
                wxs_write_pte(wxs_pte_shadow_exec);
            else
                wxs_write_pte(wxs_pte_orig_exec);
            if (wxs_dual_sw <= 6)
                logki("wxs_probe: dual sw=%lu ec=0x%x far=0x%llx\n",
                      wxs_dual_sw, ec, (unsigned long long)far);
            args->skip_origin = 1;
            return;
        }
        logke("wxs_probe: dual switch storm, disabling\n");
        wxs_dual = 0;
        wxs_write_pte(wxs_pte_orig);
        args->skip_origin = 1;
        return;
    }

    if (wxs_xo_guard && ec == 0x24) {
        logki("wxs_probe: xo probe ec=0x%x fsc=0x%x far=0x%llx pc=0x%llx (no exec-only)\n",
              ec, fsc, (unsigned long long)far,
              (unsigned long long)(regs ? regs->pc : 0));
        wxs_xo_guard = 0;
        wxs_write_pte(wxs_pte_orig);
        args->skip_origin = 1;
        return;
    }

    if (ec == 0x20 && wxs_hold && wxs_pte_installed) {
        if (wxs_switch_count < 64) {
            wxs_switch_count++;
            wxs_write_pte(wxs_pte_installed);
            wxs_hold = 0;
            args->skip_origin = 1;
            logki("wxs_probe: iabt switch far=0x%llx pc=0x%llx n=%lu\n",
                  (unsigned long long)far,
                  (unsigned long long)(regs ? regs->pc : 0), wxs_switch_count);
            return;
        }
        logke("wxs_probe: too many switches, giving up far=0x%llx\n",
              (unsigned long long)far);
        wxs_hold = 0;
    }

    if (wxs_fault_logged >= 8)
        return;
    wxs_fault_logged++;
    logki("wxs_probe: fault far=0x%llx esr=0x%x ec=0x%x fsc=0x%x pc=0x%llx\n",
          (unsigned long long)far, esr, ec, fsc,
          (unsigned long long)(regs ? regs->pc : 0));
}

static void wxs_detach(void)
{
    if (wxs_hook_on) {
        hook_unwrap(wxs_do_mem_abort, (void *)wxs_mem_abort_before, 0);
        wxs_hook_on = 0;
    }
    if (wxs_state == 2) {
        /* 复原失败也要把引用放掉，但保留日志让调用方看见。 */
        if (*wxs_ptep == wxs_pte_installed) {
            *wxs_ptep = wxs_pte_orig;
            wxs_tlbi_va(wxs_tva);
        } else {
            logke("wxs_probe: detach: pte changed under us (now 0x%llx)\n",
                  (unsigned long long)*wxs_ptep);
        }
    }
    if (wxs_shadow) {
        kf_kfree(wxs_shadow);
        wxs_shadow = 0;
    }
    if (wxs_tmm) {
        kf_mmput(wxs_tmm);
        wxs_tmm = 0;
    }
    wxs_ptep = 0;
    wxs_pte_orig = 0;
    wxs_pte_installed = 0;
    wxs_shadow_pa = 0;
    wxs_tpid = 0;
    wxs_tva = 0;
    wxs_toff = 0;
    wxs_state = 0;
}

static void wxs_write_pte(uint64_t pte)
{
    *(volatile uint64_t *)wxs_ptep = pte;
    dsb(ishst);
    asm volatile("tlbi vaae1is, %0" : : "r"((wxs_tva >> 12) & 0xffffffffffUL));
    dsb(ish);
    isb();
}

static int wxs_attach(pid_t pid, uint64_t va, char *out, int outlen)
{
    struct task_struct *task;
    struct mm_struct *mm;
    uint64_t ptep;
    uint64_t pte;
    const char *err = 0;

    wxs_detach();
    task = kf_find_task_by_vpid(pid);
    if (!task)
        return snprintf(out, outlen, "attach: no such pid %d\n", pid);
    mm = kf_get_task_mm(task);
    if (!mm)
        return snprintf(out, outlen, "attach: pid %d has no mm\n", pid);
    if (wxs_leaf_pte(mm, va, &ptep, &pte, &err) < 0) {
        kf_mmput(mm);
        return snprintf(out, outlen, "attach: %s\n", err);
    }
    wxs_tpid = pid;
    wxs_tmm = mm;
    wxs_tva = va & ~0xfffULL;
    wxs_toff = va & 0xfffULL;
    wxs_ptep = (uint64_t *)ptep;
    wxs_pte_orig = pte;
    wxs_state = 1;
    return snprintf(out, outlen, "attach pid=%d va=0x%llx off=0x%llx pte=0x%llx\n", pid,
                    (unsigned long long)wxs_tva, (unsigned long long)(va & 0xfffULL),
                    (unsigned long long)pte);
}

static int wxs_install(const char *hexpat, char *out, int outlen, int dual)
{
    uint64_t src_pa;
    uint64_t off;
    uint64_t newpte;
    unsigned char pat[16];
    int patlen = 0;
    int i;

    if (wxs_state < 1)
        return snprintf(out, outlen, "install: not attached\n");
    if (wxs_state == 2)
        return snprintf(out, outlen, "install: already installed\n");
    for (i = 0; hexpat[i] && patlen < 16; i += 2) {
        int hi = wxs_hexval(hexpat[i]);
        int lo = hexpat[i + 1] ? wxs_hexval(hexpat[i + 1]) : -1;
        if (hi < 0 || lo < 0)
            return snprintf(out, outlen, "install: bad hex\n");
        pat[patlen++] = (unsigned char)((hi << 4) | lo);
    }
    if (!patlen)
        return snprintf(out, outlen, "install: empty pattern\n");

    wxs_shadow = kf___kmalloc(4096, WXS_GFP_KERNEL);
    if (!wxs_shadow)
        return snprintf(out, outlen, "install: kmalloc failed\n");
    if ((uint64_t)wxs_shadow & 0xfffULL) {
        kf_kfree(wxs_shadow);
        wxs_shadow = 0;
        return snprintf(out, outlen, "install: shadow page not aligned\n");
    }

    /* 整页复制原内容，再在 va 所在偏移处写新内容。 */
    src_pa = wxs_pte_orig & WXS_PTE_ADDR_MASK;
    memcpy(wxs_shadow, (void *)(src_pa + wxs_linear_voffset), 4096);
    off = wxs_toff;
    memcpy((char *)wxs_shadow + off, pat, (size_t)patlen);
    wxs_dcache_clean(wxs_shadow, 4096);
    /*
     * 影子页可能被当成代码执行（P2 的执行视图）。I-cache 按 VA+ASID 维护，
     * 目标进程在别的 ASID 上，按 VA 刷刷不到它，只能用 IALLUIS 全刷这个
     * inner-shareable 域。代价大，但 Probe 阶段够用；成品要给每页记 ASID 再精确刷。
     */
    asm volatile("ic ialluis" ::: "memory");
    asm volatile("dsb ish" ::: "memory");
    asm volatile("isb" ::: "memory");

    wxs_shadow_pa = wxs_at_s1e1w((uint64_t)wxs_shadow);
    if (!wxs_shadow_pa) {
        kf_kfree(wxs_shadow);
        wxs_shadow = 0;
        return snprintf(out, outlen, "install: at s1e1w on shadow failed\n");
    }

    newpte = (wxs_pte_orig & ~WXS_PTE_ADDR_MASK) | (wxs_shadow_pa & WXS_PTE_ADDR_MASK);
    wxs_pte_installed = newpte;
    if (wxs_dual_mode) {
        /*
         * 双视图：影子页做成「EL0 只能取指、不能读写」（清 UXN 与用户位），
         * 原页做成「EL0 只读、不可执行」（置 UXN）。两条路都靠缺页来回切。
         */
        wxs_pte_shadow_exec = (newpte & ~PTE_UXN & ~PTE_USER) | PTE_AF;
        wxs_pte_orig_exec = wxs_pte_orig | PTE_UXN;
        wxs_dual = 1;
        wxs_dual_sw = 0;
        wxs_write_pte(wxs_pte_orig_exec);
        wxs_state = 2;
        return snprintf(out, outlen,
                        "dual ok pid=%d va=0x%llx shadow_pa=0x%llx exec=0x%llx read=0x%llx\n",
                        wxs_tpid, (unsigned long long)wxs_tva,
                        (unsigned long long)wxs_shadow_pa,
                        (unsigned long long)wxs_pte_shadow_exec,
                        (unsigned long long)wxs_pte_orig_exec);
    }
    asm volatile("dsb ishst" ::: "memory");
    *wxs_ptep = newpte;
    wxs_tlbi_va(wxs_tva);
    wxs_state = 2;

    return snprintf(out, outlen,
                    "install ok pid=%d va=0x%llx shadow_pa=0x%llx pte 0x%llx -> 0x%llx\n",
                    wxs_tpid, (unsigned long long)wxs_tva, (unsigned long long)wxs_shadow_pa,
                    (unsigned long long)wxs_pte_orig, (unsigned long long)newpte);
}

static int wxs_release(char *out, int outlen)
{
    uint64_t now;

    if (wxs_state != 2)
        return snprintf(out, outlen, "release: nothing installed\n");
    now = *wxs_ptep;
    if (now != wxs_pte_installed) {
        /* PTE 被别人改过：不盲目录写，留给人工判断。 */
        return snprintf(out, outlen, "release: pte changed (now 0x%llx expect 0x%llx)\n",
                        (unsigned long long)now, (unsigned long long)wxs_pte_installed);
    }
    asm volatile("dsb ishst" ::: "memory");
    *wxs_ptep = wxs_pte_orig;
    wxs_tlbi_va(wxs_tva);
    wxs_state = 1;
    kf_kfree(wxs_shadow);
    wxs_shadow = 0;
    return snprintf(out, outlen, "release ok pid=%d va=0x%llx pte back to 0x%llx\n", wxs_tpid,
                    (unsigned long long)wxs_tva, (unsigned long long)wxs_pte_orig);
}

static int wxs_state_line(char *out, int outlen)
{
    if (!wxs_state)
        return snprintf(out, outlen, "state=none\n");
    if (wxs_state == 1)
        return snprintf(out, outlen, "state=attached pid=%d va=0x%llx pte=0x%llx\n", wxs_tpid,
                        (unsigned long long)wxs_tva, (unsigned long long)wxs_pte_orig);
    return snprintf(out, outlen,
                    "state=installed pid=%d va=0x%llx pte=0x%llx orig=0x%llx shadow_pa=0x%llx\n",
                    wxs_tpid, (unsigned long long)wxs_tva, (unsigned long long)*wxs_ptep,
                    (unsigned long long)wxs_pte_orig, (unsigned long long)wxs_shadow_pa);
}

static long wxs_probe_control0(const char *args, char *__user out_msg, int outlen)
{
    char buf[256];
    const char *p = args;
    uint64_t pid64 = 0;
    uint64_t va = 0;
    int adv;
    int n = 0;

    buf[0] = 0;
    if (!args) {
        snprintf(buf, sizeof(buf), "wxs_probe: no args\n");
        goto out;
    }
    while (*p == ' ')
        p++;

    if (!strncmp(p, "self", 4)) {
        snprintf(buf, sizeof(buf),
                 "wxs_probe v" MYKPM_VERSION
                 " selfcheck=%d linear_voffset=0x%llx va_bits=%d page_offset=0x%llx pgd_off=%d\n",
                 wxs_selfcheck, (unsigned long long)wxs_linear_voffset, wxs_va_bits,
                 (unsigned long long)wxs_page_offset, (int)mm_struct_offset.pgd_offset);
        goto out;
    }
    if (!strncmp(p, "arm", 3)) {
        if (wxs_state < 1) {
            snprintf(buf, sizeof(buf), "arm: attach first\n");
            goto out;
        }
        if (!wxs_do_mem_abort) {
            snprintf(buf, sizeof(buf), "arm: do_mem_abort not found\n");
            goto out;
        }
        if (wxs_hook_on) {
            snprintf(buf, sizeof(buf), "arm: already armed\n");
            goto out;
        }
        {
            long rc = hook_wrap2(wxs_do_mem_abort, wxs_mem_abort_before, 0, 0);
            if (rc) {
                snprintf(buf, sizeof(buf), "arm: hook_wrap2 rc=%ld\n", rc);
                goto out;
            }
        }
        wxs_hook_on = 1;
        wxs_fault_count = 0;
        wxs_fault_logged = 0;
        snprintf(buf, sizeof(buf), "arm ok hook=%d va=0x%llx\n", wxs_hook_on,
                 (unsigned long long)wxs_tva);
        goto out;
    }
    if (!strncmp(p, "disarm", 6)) {
        if (wxs_hook_on) {
            hook_unwrap(wxs_do_mem_abort, (void *)wxs_mem_abort_before, 0);
            wxs_hook_on = 0;
        }
        snprintf(buf, sizeof(buf), "disarm ok\n");
        goto out;
    }
    if (!strncmp(p, "xo", 2)) {
        uint64_t cur;
        if (wxs_state < 1) {
            snprintf(buf, sizeof(buf), "xo: not attached\n");
            goto out;
        }
        cur = *(volatile uint64_t *)wxs_ptep;
        wxs_xo_guard = 1;
        wxs_write_pte((cur & ~PTE_USER) | PTE_AF);
        snprintf(buf, sizeof(buf), "xo ok pte=0x%llx (user bit cleared)\n",
                 (unsigned long long)((cur & ~PTE_USER) | PTE_AF));
        goto out;
    }
    if (!strncmp(p, "hold", 4)) {
        if (wxs_state != 2) {
            snprintf(buf, sizeof(buf), "hold: install first\n");
            goto out;
        }
        wxs_hold = 1;
        wxs_switch_count = 0;
        wxs_write_pte(wxs_pte_orig | PTE_UXN);
        snprintf(buf, sizeof(buf), "hold ok pte=0x%llx shadow=0x%llx\n",
                 (unsigned long long)(wxs_pte_orig | PTE_UXN),
                 (unsigned long long)wxs_pte_installed);
        goto out;
    }
    if (!strncmp(p, "uxnclear", 8)) {
        if (wxs_state < 1) {
            snprintf(buf, sizeof(buf), "uxnclear: not attached\n");
            goto out;
        }
        if (!wxs_uxn_on) {
            snprintf(buf, sizeof(buf), "uxnclear: not set\n");
            goto out;
        }
        wxs_write_pte(wxs_pte_orig);
        wxs_uxn_on = 0;
        wxs_dual = 0;
        snprintf(buf, sizeof(buf), "uxnclear ok pte=0x%llx\n",
                 (unsigned long long)wxs_pte_orig);
        goto out;
    }
    if (!strncmp(p, "uxn", 3)) {
        uint64_t cur;
        if (wxs_state < 1) {
            snprintf(buf, sizeof(buf), "uxn: not attached\n");
            goto out;
        }
        cur = *(volatile uint64_t *)wxs_ptep;
        if ((cur & (PTE_VALID | PTE_TYPE_MASK)) != (PTE_VALID | PTE_TYPE_PAGE)) {
            snprintf(buf, sizeof(buf), "uxn: pte changed 0x%llx\n",
                     (unsigned long long)cur);
            goto out;
        }
        wxs_pte_uxn = cur | PTE_UXN;
        wxs_write_pte(wxs_pte_uxn);
        wxs_uxn_on = 1;
        snprintf(buf, sizeof(buf), "uxn ok old=0x%llx new=0x%llx\n",
                 (unsigned long long)cur, (unsigned long long)wxs_pte_uxn);
        goto out;
    }
    if (!strncmp(p, "stats", 5)) {
        snprintf(buf, sizeof(buf), "stats iabt=%lu dabt=%lu other=%lu sw=%lu\n",
                 wxs_n_iabt, wxs_n_dabt, wxs_n_other, wxs_switch_count);
        goto out;
    }
    if (!strncmp(p, "faults", 6)) {
        snprintf(buf, sizeof(buf), "faults=%lu logged=%u hook=%d sw=%lu hold=%d va=0x%llx\n",
                 wxs_fault_count, wxs_fault_logged, wxs_hook_on, wxs_switch_count, wxs_hold,
                 (unsigned long long)wxs_tva);
        goto out;
    }
    if (!strncmp(p, "state", 5)) {
        wxs_state_line(buf, (int)sizeof(buf));
        goto out;
    }
    if (!strncmp(p, "release", 7)) {
        wxs_release(buf, (int)sizeof(buf));
        goto out;
    }
    if (!strncmp(p, "dual", 4)) {
        p += 4;
        while (*p == ' ')
            p++;
        wxs_dual_mode = 1;
        n = wxs_install(p, buf, (int)sizeof(buf), 1);
        (void)n;
        goto out;
    }
    if (!strncmp(p, "install", 7)) {
        p += 7;
        while (*p == ' ')
            p++;
        wxs_dual_mode = 0;
        wxs_install(p, buf, (int)sizeof(buf), 0);
        goto out;
    }

    if (!strncmp(p, "probe", 5) || !strncmp(p, "attach", 6)) {
        int is_attach = (p[0] == 'a');
        p += is_attach ? 6 : 5;
        while (*p == ' ')
            p++;
        adv = wxs_parse_u64(p, &pid64);
        if (!adv) {
            snprintf(buf, sizeof(buf), "bad pid\n");
            goto out;
        }
        p += adv;
        while (*p == ' ')
            p++;
        adv = wxs_parse_u64(p, &va);
        if (!adv) {
            snprintf(buf, sizeof(buf), "bad va\n");
            goto out;
        }
        if (is_attach)
            wxs_attach((pid_t)pid64, va, buf, (int)sizeof(buf));
        else
            n = wxs_dump_page((pid_t)pid64, va, buf, (int)sizeof(buf));
        (void)n;
        goto out;
    }

    snprintf(buf, sizeof(buf), "unknown cmd\n");

out:
    /* 只能经 compat_copy_to_user 回写；直接给 out_msg[i] 赋值是内核态写用户地址，
     * 在开了 PAN 的 arm64 上会触发内核缺页（P0 第一次上机就是这么把 Pixel 6 打掉的）。 */
    if (out_msg && outlen > 0) {
        int len = (int)strlen(buf);
        if (len > outlen - 1)
            len = outlen - 1;
        compat_copy_to_user(out_msg, buf, len + 1);
    }
    return 0;
}

static long wxs_probe_init(const char *args, const char *event, void *__user reserved)
{
    kf_find_task_by_vpid = (void *)kallsyms_lookup_name("find_task_by_vpid");
    kf_get_task_mm = (void *)kallsyms_lookup_name("get_task_mm");
    kf_mmput = (void *)kallsyms_lookup_name("mmput");
    kf___kmalloc = (void *)kallsyms_lookup_name("__kmalloc");
    kf_kfree = (void *)kallsyms_lookup_name("kfree");
    if (!kf_find_task_by_vpid || !kf_get_task_mm || !kf_mmput || !kf___kmalloc || !kf_kfree) {
        logke("wxs_probe: symbol missing\n");
        return -1;
    }
    wxs_do_mem_abort = (void *)kallsyms_lookup_name("do_mem_abort");
    if (!wxs_do_mem_abort)
        logkw("wxs_probe: do_mem_abort not found (arm disabled)\n");
    wxs_va_bits = wxs_read_va_bits();
    /* arm64: PAGE_OFFSET = ~0 << VA_BITS（39 → 0xffffff8000000000，48 → 0xffff000000000000） */
    wxs_page_offset = ~0ULL << wxs_va_bits;
    wxs_selfcheck_do();
    logki("wxs_probe: init done\n");
    return 0;
}

static long wxs_probe_exit(void *__user reserved)
{
    wxs_detach();
    if (wxs_selfpage && kf_kfree)
        kf_kfree(wxs_selfpage);
    wxs_selfpage = 0;
    return 0;
}

KPM_INIT(wxs_probe_init);
KPM_CTL0(wxs_probe_control0);
KPM_EXIT(wxs_probe_exit);
