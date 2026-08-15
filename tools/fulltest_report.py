#!/usr/bin/env python3
"""fulltest_100k.sh의 결과를 **읽을 수 있는 보고서**로 만든다.

풀테스트는 격자별 CSV 수십 개를 남긴다. 그 상태로는 "어디가 무너지는가"를
못 본다 — 표가 심볼로지 x 격자로만 갈려 있어서, 정작 봐야 할 **축 하나가
어디서 끊기는가**가 격자 안에 뭉쳐 있기 때문이다.

이 도구가 만드는 것:

  1. 심볼로지 x 격자 검출률 표      — 어느 심볼로지가 어느 상황에서 죽나
  2. **축별 절단점**               — 각 축의 값별 검출률, 50% 지나는 지점
  3. 모듈 px 계층                  — 크기 교란을 걷어낸 비교(§3.105)
  4. 열화 겹수별                   — 축이 아니라 겹수가 남은 격차다(§3.100)
  5. 오디코딩 전수                 — 미검출 < 오디코딩이므로 한 건씩 다 적는다
  6. 시간                          — x86 실측과 보드 환산(x8)을 같이(§3.60)

**단위 규칙**: 시간 열은 전부 x86 실측이고 보드 값은 x8이다. 이 도구는
열 이름과 값에 둘 다 박는다 — 라벨 없이 숫자만 찍었다가 게이트가 실제로
틀린 적이 있다(§3.99).

사용:
  python3 tools/fulltest_report.py fulltest-out > REPORT.md
"""
import argparse
import csv
import glob
import os
import re
import sys
from collections import defaultdict

BOARD_X = 8          # x86 -> i.MX8MP 환산 배수 (§3.60)


def load(outdir):
    """격자 CSV를 전부 읽어 행 목록으로 만든다. 태그는 dict로 푼다."""
    rows = []
    for path in sorted(glob.glob(os.path.join(outdir, "*.csv"))):
        tag = os.path.basename(path)[:-4]
        parts = tag.split("_")
        wh = ""
        if "x" in parts[-1] and parts[-1].replace("x", "").isdigit():
            wh = parts.pop()
        mod = parts.pop()[1:]
        sym = parts.pop()
        grid = "_".join(parts)
        with open(path, newline="") as f:
            for r in csv.DictReader(f):
                try:
                    r["_grid"], r["_sym"], r["_mod"], r["_wh"] = grid, sym, mod, wh
                    r["found"] = int(r["found"]); r["expected"] = int(r["expected"])
                    r["text_ok"] = int(r["text_ok"]); r["misdecode"] = int(r["misdecode"])
                    r["ms"] = float(r["ms"])
                except (ValueError, KeyError):
                    continue
                ax, flags = {}, []
                for t in (r.get("tags") or "").split(";"):
                    if "=" in t:
                        k, _, v = t.partition("=")
                        ax[k] = v
                    elif t:
                        flags.append(t)
                r["_ax"], r["_flags"] = ax, flags
                r["_modpx"] = next((float(t.split("-", 1)[1]) for t in flags
                                    if t.startswith("modpx-")), None)
                r["_deg"] = _degdepth(r)
                rows.append(r)
    return rows


# 난수 코퍼스는 축이 `k=v`가 아니라 **깃발**로 붙는다. 이 목록에 없는 깃발
# (mixed / img-* / sym-* / mod-* / nN)은 열화가 아니라 분류표라 안 센다.
# 이걸 안 나누면 난수 프레임이 전부 "겹수 0"으로 들어가서 겹수 표가
# 망가진다 — 실제로 첫 보고서에서 겹수 0이 35,100장으로 찍혔다.
_DEG_FLAGS = {
    "lowcontrast", "inverted", "damaged", "printdefect", "dirty", "perspective",
    "perspective-strong", "overexposed", "underexposed", "glare", "quietzone",
    "curved", "shadow", "noise", "noise-strong", "dpm", "defocus", "defocus-strong",
    "motion", "motion-strong", "clutter-label", "clutter-warehouse",
    "clutter-falsepattern", "rot-small", "rot-free", "rot-ortho",
}


def _degdepth(r):
    """이 프레임에 몇 겹의 열화가 걸렸나. 스윕은 축에서, 난수는 깃발에서 센다."""
    if r["_ax"]:
        return sum(1 for k, v in r["_ax"].items()
                   if k not in ("module", "angle") and _nonzero(k, v))
    return sum(1 for t in r["_flags"] if t in _DEG_FLAGS)


def _nonzero(axis, val):
    """그 축이 실제로 열화를 걸었는가. 기본값이 1인 축(대비/밝기)은 1이 무열화다."""
    try:
        v = float(val)
    except ValueError:
        return False
    return abs(v - 1.0) > 1e-9 if axis in ("contrast", "bright", "shadow") else v > 0


def rate(rows):
    tot = sum(r["expected"] for r in rows)
    ok = sum(r["text_ok"] for r in rows)
    return (100.0 * ok / tot) if tot else 0.0, ok, tot


def pctl(vals, q):
    if not vals:
        return 0.0
    s = sorted(vals)
    return s[min(len(s) - 1, int(q * len(s)))]


def h(title, level=2):
    print(f"\n{'#' * level} {title}\n")


def table(header, body):
    print("| " + " | ".join(header) + " |")
    print("|" + "|".join(["---"] * len(header)) + "|")
    for row in body:
        print("| " + " | ".join(str(c) for c in row) + " |")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("outdir")
    ap.add_argument("--top-axis", type=int, default=8, help="축별 표에 찍을 값 개수")
    a = ap.parse_args()

    rows = load(a.outdir)
    if not rows:
        sys.exit(f"{a.outdir} 에 격자 CSV가 없다")

    syms = sorted({r["_sym"] for r in rows})
    grids = sorted({r["_grid"] for r in rows})
    overall, ok, tot = rate(rows)
    mis = sum(r["misdecode"] for r in rows)
    ms = [r["ms"] for r in rows]

    print("# vscan-lite 풀테스트 보고서")
    print(f"\n프레임 {len(rows):,}장 / 코드 {tot:,}개 / 심볼로지 {len(syms)}종 / 격자 {len(grids)}종")
    print(f"\n**텍스트 일치 {overall:.1f}%** ({ok:,}/{tot:,}) · **오디코딩 {mis}건**")
    print(f"\n시간(x86 실측): 평균 {sum(ms)/len(ms):.1f}ms · p95 {pctl(ms,0.95):.1f}ms")
    print(f"  → 보드(i.MX8MP) 환산 x{BOARD_X}: 평균 {sum(ms)/len(ms)*BOARD_X:.0f}ms · "
          f"p95 {pctl(ms,0.95)*BOARD_X:.0f}ms")
    print("\n> 격자에는 물리적으로 불가능한 모서리를 **일부러** 넣는다. 검출률의"
          " 절대값이 아니라 심볼로지·축 사이의 **차이**를 볼 것.")

    # --- 1. 심볼로지 x 격자 ------------------------------------------------
    h("1. 심볼로지 x 격자 (텍스트 일치 %)")
    body = []
    for s in syms:
        line = [s]
        for g in grids:
            sub = [r for r in rows if r["_sym"] == s and r["_grid"] == g]
            line.append(f"{rate(sub)[0]:.1f}" if sub else "-")
        sub = [r for r in rows if r["_sym"] == s]
        line.append(f"**{rate(sub)[0]:.1f}**")
        body.append(line)
    table(["심볼로지"] + [g.split("_", 1)[-1] for g in grids] + ["전체"], body)

    # --- 2. 축별 절단점 ----------------------------------------------------
    h("2. 축별 절단점 — 값이 커질수록 어디서 끊기는가")
    print("각 축의 값별 텍스트 일치율이다. **50%를 지나는 값**이 그 축의 벽이다.\n")
    axes = defaultdict(lambda: defaultdict(list))
    for r in rows:
        for k, v in r["_ax"].items():
            axes[k][v].append(r)
    for ax in sorted(axes):
        vals = sorted(axes[ax], key=lambda x: float(x) if _isnum(x) else 0)
        if len(vals) < 2:
            continue
        step = max(1, len(vals) // a.top_axis)
        picked = vals[::step]
        body = [[v, f"{rate(axes[ax][v])[0]:.1f}", len(axes[ax][v])] for v in picked]
        wall = next((v for v in vals if rate(axes[ax][v])[0] < 50.0), None)
        h(f"축 `{ax}` — 50% 아래로 내려가는 첫 값: **{wall if wall else '없음(끝까지 버팀)'}**", 3)
        table([ax, "일치%", "프레임"], body)

    # --- 3. 모듈 px 계층 ---------------------------------------------------
    withmod = [r for r in rows if r["_modpx"] is not None]
    if withmod:
        h("3. 실제 모듈 px별 — 크기 교란을 걷어낸 비교")
        print("§3.105: 요청 모듈과 실제 모듈이 다를 수 있어서 태그의 `modpx`를 쓴다.\n")
        buckets = [(0, 2), (2, 3), (3, 4), (4, 6), (6, 8), (8, 12), (12, 99)]
        body = []
        for lo, hi in buckets:
            sub = [r for r in withmod if lo <= r["_modpx"] < hi]
            if sub:
                body.append([f"{lo}~{hi}px", f"{rate(sub)[0]:.1f}", len(sub),
                             sum(r['misdecode'] for r in sub)])
        table(["실제 모듈", "일치%", "프레임", "오디코딩"], body)

        h("3.1 심볼로지 x 실제 모듈 — 같은 크기에서 비교해야 심볼로지 비교다", 3)
        body = []
        cols = [(1.5, 2.5), (2.5, 3.5), (3.5, 4.5), (4.5, 6.5), (6.5, 9)]
        for s in syms:
            line = [s]
            for lo, hi in cols:
                sub = [r for r in withmod if r["_sym"] == s and lo <= r["_modpx"] < hi]
                line.append(f"{rate(sub)[0]:.1f}" if sub else "-")
            body.append(line)
        table(["심볼로지"] + [f"{lo}~{hi}px" for lo, hi in cols], body)

    # --- 4. 열화 겹수 ------------------------------------------------------
    h("4. 열화 겹수별 — 남은 격차는 축이 아니라 겹수다 (§3.100)")
    body = []
    for d in sorted({r["_deg"] for r in rows}):
        sub = [r for r in rows if r["_deg"] == d]
        v = [r["ms"] for r in sub]
        body.append([d, f"{rate(sub)[0]:.1f}", len(sub),
                     f"{sum(v)/len(v):.1f}", f"{sum(v)/len(v)*BOARD_X:.0f}"])
    table(["겹수", "일치%", "프레임", "평균 ms(x86)", "평균 ms(보드)"], body)

    # --- 5. 오디코딩 전수 --------------------------------------------------
    h("5. 오디코딩 전수 — 미검출보다 나쁘다, 그래서 한 건씩 적는다")
    bad = [r for r in rows if r["misdecode"] > 0]
    if not bad:
        print("**0건.**")
    else:
        print(f"**{len(bad)}건** (프레임 {len(rows):,}장 중 {100.0*len(bad)/len(rows):.3f}%)\n")
        body = [[r["_sym"], r["_grid"], r["file"], r["misdecode"],
                 (r.get("tags") or "")[:70]] for r in bad[:60]]
        table(["심볼로지", "격자", "프레임", "건수", "태그"], body)
        if len(bad) > 60:
            print(f"\n(위 60건만 표시 — 전체 {len(bad)}건)")

    # --- 6. 시간 -----------------------------------------------------------
    h("6. 시간 — x86 실측과 보드 환산")
    print("보드(i.MX8MP)는 x86의 약 8배 느리다(§3.60). **두 값을 같이 적는다.**\n")
    body = []
    for s in syms:
        v = [r["ms"] for r in rows if r["_sym"] == s]
        if v:
            body.append([s, f"{sum(v)/len(v):.1f}", f"{pctl(v,0.95):.1f}",
                         f"{sum(v)/len(v)*BOARD_X:.0f}", f"{pctl(v,0.95)*BOARD_X:.0f}"])
    table(["심볼로지", "평균(x86)", "p95(x86)", "평균(보드)", "p95(보드)"], body)


def _isnum(x):
    try:
        float(x); return True
    except ValueError:
        return False


if __name__ == "__main__":
    main()
