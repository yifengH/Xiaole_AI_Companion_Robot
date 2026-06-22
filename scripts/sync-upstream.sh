#!/usr/bin/env bash
# 上游同步脚本(由"上游同步负责人"运行,低频:上游发版/需要某 bugfix 时)。
# 作用:确保 upstream 远程存在 → fetch → 从 main 起一个带日期的同步分支 → merge 上游。
# 它【不】直接动 main、【不】自动解冲突、【不】push。解冲突 + 上机冒烟 + 开 PR 仍由人来。
# 用法:  bash scripts/sync-upstream.sh
set -euo pipefail

UPSTREAM_URL="https://github.com/78/xiaozhi-esp32.git"
UPSTREAM_REMOTE="upstream"
BASE_BRANCH="main"   # 我们的产品主线;若你们叫 master 改这里

# 1) 确保 upstream 远程存在(每台机器一次性,不随仓库分发,所以这里幂等补上)
if ! git remote get-url "$UPSTREAM_REMOTE" >/dev/null 2>&1; then
  echo "==> 添加 upstream 远程: $UPSTREAM_URL"
  git remote add "$UPSTREAM_REMOTE" "$UPSTREAM_URL"
else
  echo "==> upstream 远程已存在: $(git remote get-url "$UPSTREAM_REMOTE")"
fi

# 2) 拉上游(只读)
echo "==> git fetch $UPSTREAM_REMOTE"
git fetch "$UPSTREAM_REMOTE"

# 3) 从最新 main 起一个同步分支(不在 main 上直接 merge)
if [ -n "$(git status --porcelain)" ]; then
  echo "!! 工作区不干净,请先提交/暂存再来。" >&2
  exit 1
fi
DATE_TAG="$(git log -1 --format=%cd --date=format:%Y%m%d "$UPSTREAM_REMOTE/$BASE_BRANCH" 2>/dev/null || echo manual)"
SYNC_BRANCH="sync/upstream-${DATE_TAG}"
echo "==> 从 $BASE_BRANCH 起同步分支 $SYNC_BRANCH"
git switch "$BASE_BRANCH"
git switch -c "$SYNC_BRANCH"

# 4) 合并上游(产生合并提交"圆";有冲突会停在这里,交给人解)
echo "==> git merge $UPSTREAM_REMOTE/$BASE_BRANCH"
if git merge --no-edit "$UPSTREAM_REMOTE/$BASE_BRANCH"; then
  echo ""
  echo "✅ 自动合并成功(无冲突)。下一步:"
else
  echo ""
  echo "⚠️  有冲突。请:① git status 看冲突文件 ② 解冲突(保留我方 hook + 上游新代码)"
  echo "    ③ git add <文件> ④ git merge --continue。然后:"
fi
echo "   - 本机 build + 烧录冒烟测:Bootstrap→配对→连麦→打断→OTA→重连"
echo "   - git push -u origin $SYNC_BRANCH 并开 PR: $SYNC_BRANCH → $BASE_BRANCH(请另一位同事 review)"
