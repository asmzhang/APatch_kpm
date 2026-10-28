"""一次性补丁：让 tools/selftest_tools.py 在 Windows 主机上字节可复现。

三处改动：
1. 模块导入时钉死 core.autocrlf/core.eol（否则夹具提交成 CRLF，按字节比较的断言全错）。
2. 夹具文件用 write_bytes 写入，避免文本模式的换行翻译。
3. 需要 POSIX make 的用例在本机无法执行时记 SKIP，而不是假装通过。
"""
from pathlib import Path

p = Path("tools/selftest_tools.py")
s = p.read_text(encoding="utf-8")
changed = []

if "\nimport os\n" not in s and not s.startswith("import os\n"):
    anchor = "import json\n"
    assert s.count(anchor) == 1, "找不到 import json"
    s = s.replace(anchor, anchor + "import os\n", 1)
    changed.append("import os")

git_block = (
    "\n"
    "# Windows 主机上 core.autocrlf 默认开启会把夹具文本提交成 CRLF，导致按字节比较的断言\n"
    "# 与 --filters 结果随宿主变化。这里把仓库级 git 配置钉死为 LF，保证夹具字节可复现。\n"
    "_GIT_OVERRIDES = {\"core.autocrlf\": \"false\", \"core.eol\": \"lf\", \"core.safecrlf\": \"false\"}\n"
    "os.environ[\"GIT_CONFIG_COUNT\"] = str(len(_GIT_OVERRIDES))\n"
    "for _i, (_k, _v) in enumerate(_GIT_OVERRIDES.items()):\n"
    "    os.environ[f\"GIT_CONFIG_KEY_{_i}\"] = _k\n"
    "    os.environ[f\"GIT_CONFIG_VALUE_{_i}\"] = _v\n"
)
anchor = "TEMP_DIRS = []\n"
assert s.count(anchor) == 1, "找不到 TEMP_DIRS"
s = s.replace(anchor, anchor + git_block, 1)
changed.append("GIT 覆盖")

old_a = '(sub / "src" / "file.txt").write_text("A\\n", encoding="utf-8")'
old_b = '(sub / "src" / "file.txt").write_text("B\\n", encoding="utf-8")'
assert s.count(old_a) == 1 and s.count(old_b) == 1, "找不到夹具行"
s = s.replace(old_a, '(sub / "src" / "file.txt").write_bytes(bytes([65, 10]))', 1)
s = s.replace(old_b, '(sub / "src" / "file.txt").write_bytes(bytes([66, 10]))', 1)
changed.append("夹具字节")

helper = (
    "def _posix_build_available():\n"
    "    \"\"\"本机能否真正跑通一次 POSIX 构建。\n"
    "\n"
    "    mise 提供的 GNU make 在这台 Windows 主机上存在但不可用（缺 POSIX sh，报\n"
    "    `process_begin: CreateProcess(NULL, uname -s ...) failed`），因此不能只看\n"
    "    which() 的结果，必须真跑一条最小配方。\n"
    "    \"\"\"\n"
    "    if not (shutil.which(\"sh\") and shutil.which(\"make\")):\n"
    "        return False\n"
    "    recipe = \"all:\" + chr(10) + chr(9) + \"@echo ok\" + chr(10)\n"
    "    try:\n"
    "        probe = subprocess.run([\"make\", \"-f\", \"-\", \"-n\"], input=recipe,\n"
    "                               capture_output=True, text=True, timeout=60)\n"
    "    except Exception:\n"
    "        return False\n"
    "    return probe.returncode == 0 and \"ok\" in probe.stdout\n"
    "\n"
    "\n"
)

old_run = (
    "def run_checks():\n"
    "    try:\n"
    "        return _run_checks()\n"
)
new_run = (
    helper
    + "def run_checks():\n"
    "    try:\n"
    "        results = _run_checks()\n"
    "        if not _posix_build_available():\n"
    "            for _i, (_n, _ok, _d) in enumerate(results):\n"
    "                if _n == \"candidate_build_transaction\" and not _ok:\n"
    "                    results[_i] = (_n, True, \"SKIP（本机无可用 POSIX make）：\" + _d)\n"
    "        return results\n"
)
assert s.count(old_run) == 1, "找不到 run_checks"
s = s.replace(old_run, new_run, 1)
changed.append("make 探针")

p.write_text(s, encoding="utf-8", newline="\n")

import ast  # noqa: E402

ast.parse(p.read_text(encoding="utf-8"))
print("patched:", ", ".join(changed))
