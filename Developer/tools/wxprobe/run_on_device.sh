#!/usr/bin/env bash
# wxprobe 上机脚本：推文件、加载、跑客户端、回读 hits。
# 用法：bash Developer/tools/wxprobe/run_on_device.sh <serial> <manager_uid>
set -u
DEV="${1:?need serial}"
UID_MGR="${2:?need manager uid}"
HERE="$(cd "$(dirname "$0")" && pwd)"
D=/data/local/tmp

export MSYS_NO_PATHCONV=1

if [ ! -f "$HERE/wxprobe_0.0.1.kpm" ]; then
    echo "先构建：make -C $HERE" >&2
    exit 1
fi

echo "== 推文件到 $DEV"
adb -s "$DEV" push "$HERE/wxprobe_0.0.1.kpm" "$D/wxprobe_0.0.1.kpm" >/dev/null || exit 1
adb -s "$DEV" push "$HERE/brk_client" "$D/brk_client" >/dev/null || exit 1
adb -s "$DEV" shell "chmod 755 $D/brk_client"

echo "== 加载"
adb -s "$DEV" shell "su -c '$D/kpmctl $UID_MGR load $D/wxprobe_0.0.1.kpm'" 2>&1

echo "== 初始 status"
adb -s "$DEV" shell "su -c '$D/kpmctl $UID_MGR ctl wxprobe status'" 2>&1

echo "== 客户端（BRK #0x5758，期望 after brk）"
adb -s "$DEV" shell "$D/brk_client; echo rc=\$?" 2>&1

echo "== status（hits 应为 1）"
adb -s "$DEV" shell "su -c '$D/kpmctl $UID_MGR ctl wxprobe status'" 2>&1

echo "== 对照（BRK #0x1234，期望 SIGTRAP）"
adb -s "$DEV" shell "$D/brk_client control; echo rc=\$?" 2>&1

echo "== status（hits 应仍为 1）"
adb -s "$DEV" shell "su -c '$D/kpmctl $UID_MGR ctl wxprobe status'" 2>&1
