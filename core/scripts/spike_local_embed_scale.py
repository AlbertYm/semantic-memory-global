#!/usr/bin/env python3
"""Spike 2: scale up the separability test for a local sentence model.

The first spike used only the 12 C-eval cases. This one uses a larger,
independent corpus (~40 memories) with semantic queries whose wording shares
little/no surface vocabulary with the target (so we measure MEANING bridging,
not lexical overlap), plus a dedicated negative set. Reports top1 / top3 /
MRR over the corpus and the positive vs negative score bands.

Local model, offline-first. No API. Go/no-go for local-model integration.
"""
import sys, os
import numpy as np

CANDIDATES = [
    "BAAI/bge-m3",
    "BAAI/bge-small-zh-v1.5",
    "paraphrase-multilingual-MiniLM-L12-v2",
]

# 40 memories spanning prefs / decisions / constraints / lessons / events,
# mixed zh/en. Each is a distinct topic so a query has ONE right target.
MEMORIES = [
    "用户偏好用 pnpm 而非 npm，所有新项目默认使用 pnpm 作为包管理器",
    "项目数据统一存储在 SQLite，知识图谱和长期记忆都走同一个库",
    "构建产物必须是单一静态二进制，不允许引入运行时外部依赖",
    "认证服务采用 JWT 令牌，使用 RS256 算法对所有接口签名",
    "中文检索失效的根因是分词器把整段中文当成一个词，需要切成字符二元组",
    "前端状态管理统一用 Zustand，不再引入 Redux",
    "所有时间戳一律存 UTC 毫秒，展示层再按本地时区转换",
    "日志默认输出 JSON 结构化格式，方便后续采集分析",
    "数据库迁移通过版本号顺序脚本执行，启动时自动比对 user_version",
    "缓存层用 Redis，键统一加业务前缀和过期时间，禁止裸键",
    "图片上传后异步生成多档缩略图，原图冷存归档",
    "团队代码评审至少一人批准才能合并，禁止自审合并",
    "接口错误码统一三位数字加业务前缀，文档集中维护",
    "移动端列表一律虚拟滚动，超过五十条不允许整列渲染",
    "敏感配置只存环境变量和密钥管理服务，严禁写进仓库",
    "Prefer ripgrep over grep for code search across the repository",
    "The HTTP server binds to localhost only and requires no authentication in dev mode",
    "All database writes must go through the WAL journal mode for durability",
    "Use feature flags to gate unfinished work instead of long-lived branches",
    "Retry network calls with exponential backoff capped at five attempts",
    "Container images are pinned to a digest, never the latest tag",
    "Background jobs run on a separate worker pool, never on the request thread",
    "Secrets rotate every ninety days via the central vault, no manual edits",
    "Frontend bundles are code-split per route to keep first paint fast",
    "API responses are paginated with a cursor, offset paging is forbidden",
    "用户希望每天早上九点收到前一天的数据汇总邮件",
    "报表导出统一走后台队列，生成完成后通知用户下载",
    "搜索结果默认按相关度排序，可切换为按时间倒序",
    "新员工入职第一周分配一名导师做结对编程",
    "线上事故复盘必须在三个工作日内产出书面报告",
    "支付流程失败要保留半截订单并允许用户重试",
    "用户密码用 Argon2 哈希存储，绝不明文落库",
    "前端所有文案走多语言资源文件，禁止硬编码字符串",
    "数据备份每日凌晨全量加每小时增量，异地保留三十天",
    "灰度发布先放百分之五流量，观察半小时无异常再全量",
    "Mobile push notifications batch hourly to avoid waking users at night",
    "Use structured tracing with a request id propagated across services",
    "Disk-heavy tests run nightly, not on every pull request, to save CI time",
    "The design system ships components one at a time, never a bulk replace",
    "Customer data deletion requests must complete within thirty days end to end",
]

# (semantic query, substring of the intended target). Wording deliberately
# avoids the target's surface tokens.
POSITIVES = [
    ("怎么给后端接口加身份校验", "JWT 令牌"),
    ("登录态用什么方式核验", "RS256"),
    ("程序最终如何打包交付给用户", "单一静态二进制"),
    ("怎样安装依赖能少占硬盘", "pnpm"),
    ("为什么中文搜出来总是空的", "分词器把整段中文"),
    ("时间应该按什么标准保存", "UTC 毫秒"),
    ("怎么防止把口令明文写进数据库", "Argon2"),
    ("改表结构怎么不丢老数据", "user_version"),
    ("放量上线怎么稳一点", "灰度发布先放百分之五"),
    ("出了线上问题之后要做什么", "复盘必须在三个工作日"),
    ("怎么让首屏更快", "code-split per route"),
    ("网络请求失败了怎么办", "exponential backoff"),
    ("怎么翻页拿后面的数据", "cursor"),
    ("机器宕机了写入还安全吗", "WAL journal mode"),
    ("怎么找代码里的一段文字", "ripgrep"),
    ("本地开发服务别人能连吗", "binds to localhost"),
    ("怎么避免半夜推送吵醒用户", "batch hourly"),
    ("跨服务怎么追一个请求的链路", "request id propagated"),
    ("用户注销后数据多久清干净", "deletion requests must complete"),
    ("怎么管没写完的功能不影响主干", "feature flags"),
]

NEGATIVES = [
    "区块链智能合约怎么写",
    "kubernetes deployment yaml 怎么配",
    "今天天气怎么样适合钓鱼吗",
    "推荐一部好看的科幻电影",
    "怎么做红烧肉好吃",
    "明天股票会涨吗",
]


def load_model():
    from sentence_transformers import SentenceTransformer
    last = None
    for name in CANDIDATES:
        try:
            try:
                m = SentenceTransformer(name, local_files_only=True); print(f"[model] {name} (cached)"); return m, name
            except Exception:
                m = SentenceTransformer(name); print(f"[model] {name} (downloaded)"); return m, name
        except Exception as e:
            last = e; print(f"[skip] {name}: {type(e).__name__}")
    raise SystemExit(f"no model ({last})")


def main():
    model, name = load_model()
    mv = model.encode(MEMORIES, normalize_embeddings=True)

    ranks, pos_target_sims = [], []
    misses = []
    for q, expect in POSITIVES:
        ti = next((i for i, t in enumerate(MEMORIES) if expect in t), None)
        if ti is None:
            print(f"[warn] target not found for: {q}"); continue
        qv = model.encode([q], normalize_embeddings=True)[0]
        sims = mv @ qv
        order = list(np.argsort(-sims))
        rank = order.index(ti) + 1
        ranks.append(rank)
        pos_target_sims.append(float(sims[ti]))
        if rank > 1:
            top = MEMORIES[order[0]][:30]
            misses.append((rank, q, float(sims[ti]), top))

    neg_sims = []
    for q in NEGATIVES:
        qv = model.encode([q], normalize_embeddings=True)[0]
        neg_sims.append(float((mv @ qv).max()))

    n = len(ranks)
    top1 = sum(1 for r in ranks if r == 1) / n
    top3 = sum(1 for r in ranks if r <= 3) / n
    mrr = sum(1.0 / r for r in ranks) / n

    print(f"\n=== {n} semantic positives (no surface-word overlap) ===")
    print(f"  top1={top1:.3f}  top3={top3:.3f}  mrr={mrr:.3f}")
    print(f"  positive target sim: min={min(pos_target_sims):.3f} mean={np.mean(pos_target_sims):.3f}")
    print(f"  negative best  sim: max={max(neg_sims):.3f} mean={np.mean(neg_sims):.3f}")
    if misses:
        print("  --- rank>1 misses ---")
        for r, q, ts, top in misses:
            print(f"    rank={r} sim={ts:.3f}  q={q}  (top was: {top})")
    gap = min(pos_target_sims) - max(neg_sims)
    print(f"\n  separability gap (worst_pos - best_neg) = {gap:.3f}")
    print(f"  VERDICT: top1={top1:.3f} over {n} cases, model={name}")


if __name__ == "__main__":
    main()
