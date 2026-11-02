#!/usr/bin/env python3
"""两种加载形态的符号面检查（离线，不依赖真机）。

KPM 产物（.kpm）可以装进两种环境：

  kernel  KernelPatch 编进内核（APatch 内核模式）
  lkm     原厂内核 + kernelpatch.ko（APatch LKM 模式）

装载器解析未定义符号（SHN_UNDEF）的顺序是：先查本形态自带的符号表，
查不到再 `kallsyms_lookup_name()` 兜底。所以一个符号能否解析取决于：

  * 它是不是 KernelPatch 自己造的符号（`kf_*`、`cred_offset`、
    `inline_wrap_syscalln` 之类）——这类只能由该形态的符号表提供，
    某个形态没有就是 `unknown symbol`，装载在 init 之前中止；
  * 否则它是内核符号，靠 kallsyms 兜底（各设备不同，可用 --kallsyms 提供
    一份导出表来核对）。

本工具从 KernelPatch 源码离线推导两种形态自带的符号表，再读产物的
.symtab，给出每个产物在两种形态下的可装载结论。

用法：
  python tools/load_forms.py                    # 报告仓库内全部产物
  python tools/load_forms.py --module mount_hidder
  python tools/load_forms.py --artifact path/to/x.kpm
  python tools/load_forms.py --subset           # 打印可移植符号子集
  python tools/load_forms.py --kallsyms dump.txt
  python tools/load_forms.py --json
  python tools/load_forms.py --strict           # 按 metadata 的 load_forms 判定
"""
import argparse
import glob
import io
import json
import os
import re
import struct
import sys

REPO = os.environ.get("LOAD_FORMS_REPO") or os.path.abspath(
    os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
KP = os.path.join(REPO, "KernelPatch")


def read(path):
    return io.open(path, "r", encoding="utf-8", errors="replace").read()


def _u16(b, o):
    return struct.unpack_from("<H", b, o)[0]


def _u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def _u64(b, o):
    return struct.unpack_from("<Q", b, o)[0]


def elf_und_symbols(path):
    """产物里 st_shndx == SHN_UNDEF 的符号名（需要装载器解析的名字）。"""
    data = io.open(path, "rb").read()
    if data[:4] != b"\x7fELF":
        raise ValueError("不是 ELF")
    if data[4] != 2 or data[5] != 1:
        raise ValueError("只支持 ELF64 小端")
    shoff = _u64(data, 0x28)
    shentsize = _u16(data, 0x3A)
    shnum = _u16(data, 0x3C)
    sections = []
    for i in range(shnum):
        off = shoff + i * shentsize
        sections.append({
            "type": _u32(data, off + 4),
            "offset": _u64(data, off + 24),
            "size": _u64(data, off + 32),
            "link": _u32(data, off + 40),
        })
    names = []
    for sh in sections:
        if sh["type"] != 2:  # SHT_SYMTAB
            continue
        strbase = sections[sh["link"]]["offset"]
        for i in range(sh["size"] // 24):
            o = sh["offset"] + i * 24
            st_name = _u32(data, o)
            st_shndx = _u16(data, o + 6)
            if st_shndx != 0 or st_name == 0:
                continue
            end = data.index(b"\x00", strbase + st_name)
            names.append(data[strbase + st_name:end].decode("utf-8", "replace"))
    return sorted(set(names))


_KP_EXPORT = re.compile(r"KP_EXPORT_SYMBOL\(\s*(kfunc\(\s*(\w+)\s*\)|(\w+))\s*\)")


def kernel_surface():
    """kernel 形态：KernelPatch 编进内核时导出的符号。"""
    names = set()
    for path in glob.glob(os.path.join(KP, "kernel", "**", "*.c"), recursive=True):
        for m in _KP_EXPORT.finditer(read(path)):
            names.add("kf_" + m.group(2) if m.group(2) else m.group(3))
    return names


_KF_SLOT = re.compile(r"\(\s*\*\s*(kf_\w+)\s*\)")

_SHADOWED_CACHE = []


def kernel_shadowed():
    """kernel 形态里「同名影子」导出的符号。

    KP 在内核形态自己实现了一小批与内核同名的函数（例如 `get_random_u64()` 内部
    `kfunc_call(get_random_u64)` 拿真实内核值），并把同名符号导出。这些名字在 lkm 形态
    没有表项，但装载器会继续走 `kallsyms_lookup_name()` 拿到真实内核符号，因此不能算
    「本形态必然装不上」。判据：同一文件里既 `KP_EXPORT_SYMBOL(X)` 又 `kfunc_call(X)`。
    """
    if _SHADOWED_CACHE:
        return _SHADOWED_CACHE[0]
    names = set()
    for path in glob.glob(os.path.join(KP, "kernel", "**", "*.c"), recursive=True):
        text = read(path)
        if "kfunc_call" not in text:
            continue
        for m in re.finditer(r"KP_EXPORT_SYMBOL\(\s*(\w+)\s*\)", text):
            name = m.group(1)
            if re.search(r"kfunc_call\(\s*" + re.escape(name) + r"\s*\)", text):
                names.add(name)
    _SHADOWED_CACHE.append(names)
    return names


def lkm_surface():
    """lkm 形态：kernelpatch.ko 的 KPM 兼容表 + 它代理的 kf_* 槽位。"""
    text = read(os.path.join(KP, "lkm", "kpm", "symbols.c"))
    names = set()
    if "kp_kpm_symbols[]" in text:
        names |= set(re.findall(r'\{\s*"([^"]+)"\s*,',
                                text[text.index("kp_kpm_symbols[]"):]))
    return names | {n for n in _KF_SLOT.findall(text) if "#" not in n and n != "kf_name"}


def classify(und, loader, other, kallsyms):
    """返回 (该形态必然装不上的符号, 无法判定的符号)。

    顺序与装载器一致：先查本形态自带表，再走 kallsyms 兜底。给了导出表时
    只认表；没给表时，落在另一形态自带表里的符号判为「KP 自造、本形态没有」，
    其余判为「无法判定」。落在另一形态表里、但属于「同名影子」导出的（KP 自己实现
    却 `kfunc_call()` 真实内核同名函数的，如 `get_random_u64`）也归入「无法判定」，
    因为 lkm 形态会经 kallsyms 拿到真实内核符号。
    """
    shadowed = kernel_shadowed() if kallsyms is None else set()
    missing, unknown = [], []
    for name in und:
        if name in loader:
            continue
        if kallsyms is not None:
            if name not in kallsyms:
                missing.append(name)
        elif name in other and name not in shadowed:
            missing.append(name)
        else:
            unknown.append(name)
    return sorted(missing), sorted(unknown)


def artifact_records():
    """(模块, 版本, 变体, 路径, 声明的 load_forms)，来自 metadata。"""
    out = []
    for meta_path in sorted(glob.glob(os.path.join(REPO, "metadata", "modules", "*.json"))):
        record = json.load(io.open(meta_path, "r", encoding="utf-8"))
        module = record.get("module") or os.path.basename(meta_path)[:-5]
        for build in record.get("builds", []):
            for art in build.get("artifacts", []):
                rel = art.get("path")
                if not rel:
                    continue
                out.append({
                    "module": module,
                    "version": build.get("version"),
                    "variant": build.get("variant"),
                    "name": art.get("name") or os.path.basename(rel),
                    "path": os.path.join(REPO, rel),
                    "declared": build.get("load_forms"),
                })
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--module")
    ap.add_argument("--artifact")
    ap.add_argument("--kallsyms", help="内核符号导出表（一行一个符号）")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--subset", action="store_true")
    ap.add_argument("--strict", action="store_true")
    args = ap.parse_args()

    kernel, lkm = kernel_surface(), lkm_surface()
    portable = kernel & lkm

    if args.subset:
        print("kernel 形态自带符号表: %d" % len(kernel))
        print("lkm 形态自带符号表   : %d" % len(lkm))
        print("可移植子集（两形态都有）: %d" % len(portable))
        for name in sorted(portable):
            print("  " + name)
        return 0

    kallsyms = None
    if args.kallsyms:
        kallsyms = set(read(args.kallsyms).split())

    if args.artifact:
        items = [{"module": "-", "version": "-", "variant": "-",
                  "name": os.path.basename(args.artifact),
                  "path": args.artifact, "declared": None}]
    else:
        items = artifact_records()
        if args.module:
            items = [x for x in items if x["module"] == args.module]

    rows, failures = [], []
    for item in items:
        if not os.path.exists(item["path"]):
            continue
        try:
            und = elf_und_symbols(item["path"])
        except Exception as exc:
            rows.append(dict(item, error=str(exc)))
            continue
        miss_k, unk_k = classify(und, kernel, lkm, kallsyms)
        miss_l, unk_l = classify(und, lkm, kernel, kallsyms)
        forms = []
        if not miss_k:
            forms.append("kernel")
        if not miss_l:
            forms.append("lkm")
        rows.append(dict(item, und=len(und), forms=forms,
                         missing_kernel=miss_k, missing_lkm=miss_l,
                         unknown_kernel=unk_k, unknown_lkm=unk_l))
        for form in item.get("declared") or []:
            if form not in forms:
                failures.append((item, form))

    if args.json:
        print(json.dumps({"kernel_surface": len(kernel), "lkm_surface": len(lkm),
                          "portable": sorted(portable), "artifacts": rows},
                         ensure_ascii=False, indent=1))
        return 1 if failures else 0

    print("kernel 形态自带符号表 %d，lkm 形态自带符号表 %d，可移植子集 %d"
          % (len(kernel), len(lkm), len(portable)))
    if kallsyms is None:
        print("（未提供 --kallsyms：不在任何自带表里的符号算「无法判定」，"
              "实际能否解析取决于目标内核）")
    print()
    for row in rows:
        if "error" in row:
            print("%-46s 解析失败: %s" % (row["name"][:46], row["error"]))
            continue
        note = ""
        if row["unknown_kernel"] or row["unknown_lkm"]:
            note = " 待定=%d" % len(set(row["unknown_kernel"]) | set(row["unknown_lkm"]))
        print("%-46s und=%-3d 形态=%-13s kernel缺=%-2d lkm缺=%-2d%s"
              % (row["name"][:46], row["und"], ",".join(row["forms"]) or "-",
                 len(row["missing_kernel"]), len(row["missing_lkm"]), note))
        for name in row["missing_lkm"]:
            print("        lkm 缺: %s" % name)
    if failures:
        print()
        for item, form in failures:
            print("声明为 %s 形态却装不上: %s" % (form, item["name"]))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
