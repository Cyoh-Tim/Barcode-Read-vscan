#!/usr/bin/env python3
"""
extract_micropdf417_tables.py — MicroPDF417 표를 뽑고 **실측으로 검증**한다.

`src/decoder_micropdf417.cpp` 안의 표(`kRapSide` / `kRapCenter` / `kVariants`)를
만든 스크립트다. 표를 손으로 옮겨 적지 않으려고 남긴다 — 규격 문서를 보고
타이핑하면 어디서 틀렸는지 나중에 못 찾는다.

## 두 경로를 다 쓰고, 서로 대조한다

**변형표**(열/행/EC 개수/RAP 시작 인덱스)는 BWIPP의 `micropdf417` PostScript
리소스에서 파싱한다. 그런데 파싱이 맞다는 보장이 없으므로, **34개 변형
전부를 raw 모드로 생성해서 데이터 코드워드 수를 직접 재고** 표의
`행 x 열 - EC`와 맞는지 확인한다(2026-08-02 기준 34/34 일치).

**RAP 패턴**은 아예 파싱하지 않는다. 각 변형을 렌더해서 이미지의 왼쪽
10모듈을 그대로 읽는다. 행 r의 RAP 인덱스는 (rapL-1+r) mod 52이므로
여러 변형에서 같은 인덱스를 여러 번 관측하게 되는데, **전부 일치해야
한다**(관측 52/52, 충돌 0). 한 번이라도 어긋나면 세그먼트 오프셋이나
변형표가 틀린 것이다.

## 필요한 것

    pip install treepoem   +   ghostscript (BWIPP 실행용)

개발 환경 전용이다. 런타임/배포 산출물에는 안 들어간다.
BWIPP 라이선스 고지는 THIRD_PARTY_NOTICES.md 참고.

## 사용

    python3 tools/extract_micropdf417_tables.py            # 검증만
    python3 tools/extract_micropdf417_tables.py --emit     # C++ 표 출력
"""
import argparse
import base64
import re
import sys

BARCODE_PS = "/usr/local/lib/python3.11/dist-packages/treepoem/postscriptbarcode/barcode.ps"

# 한 행의 구조: 좌RAP(10) + 데이터(17*k1) + [중앙RAP(10) + 데이터(17*k2)] + 우RAP(10) + 정지(1)
# 열 수별 (좌RAP끝, 우RAP시작, 우RAP끝, 중앙RAP구간) — 전부 모듈 단위
SEG = {1: (10, 27, 37, None),
       2: (10, 44, 54, None),
       3: (10, 71, 81, (27, 37)),
       4: (10, 88, 98, (44, 54))}
WIDTH = {1: 38, 2: 55, 3: 82, 4: 99}


def bwipp_resource(name):
    """barcode.ps에서 리소스 하나를 ASCII85 해제해 돌려준다."""
    src = open(BARCODE_PS, encoding="latin-1").read()
    m = re.search(r"%%BeginResource: uk\.co\.terryburton\.bwipp " + name +
                  r" .*?currentfile /ASCII85Decode filter cvx exec\n(.*?)~>", src, re.S)
    if not m:
        raise SystemExit(f"BWIPP 리소스 {name}을 못 찾았다: {BARCODE_PS}")
    return base64.a85decode(re.sub(r"\s+", "", m.group(1)).encode())


def bwipp_ints(b):
    """BWIPP 압축 정수: 0x88 뒤 1바이트, 0x86 뒤 2바이트(빅엔디언)."""
    out, i = [], 0
    while i < len(b):
        c = b[i]
        if c == 0x88:
            out.append(b[i + 1]); i += 2
        elif c == 0x86:
            out.append((b[i + 1] << 8) | b[i + 2]); i += 3
        else:
            i += 1
    return out


def metrics(raw, name):
    m = re.search((r"/" + name + r"\s*\[").encode(), raw)
    i, d, j = m.end(), 1, m.end()
    while d:
        if raw[j:j + 1] == b"[":
            d += 1
        elif raw[j:j + 1] == b"]":
            d -= 1
        j += 1
    return [bwipp_ints(p) for p in re.findall(rb"\[([^\]]*)\]", raw[i:j - 1])]


def module_grid(rows, cols, data="A" * 3):
    import numpy as np
    import treepoem
    a = np.array(treepoem.generate_barcode(
        "micropdf417", data, options={"version": f"{rows}x{cols}"}).convert("L"))
    H, W = a.shape
    mw, rh = W // 2, H // rows            # BWIPP는 가로 2px/모듈로 그린다
    if mw != WIDTH[cols]:
        raise SystemExit(f"{rows}x{cols}: 모듈 폭 {mw}, 기대 {WIDTH[cols]}")
    return [[1 if a[r * rh + rh // 2, x * 2] < 128 else 0 for x in range(mw)]
            for r in range(rows)]


def fits_raw(rows, cols, n):
    import treepoem
    d = "".join("^%03d" % ((i * 7) % 900) for i in range(n))
    try:
        treepoem.generate_barcode("micropdf417", d,
                                  options={"raw": True, "version": f"{rows}x{cols}",
                                           "parsefnc": True})
        return True
    except Exception:
        return False


def capacity(rows, cols):
    lo, hi = 0, 200
    while lo < hi:
        mid = (lo + hi + 1) // 2
        if fits_raw(rows, cols, mid):
            lo = mid
        else:
            hi = mid - 1
    return lo


def runs(bits):
    out, cur, n = [], bits[0], 0
    for b in bits:
        if b == cur:
            n += 1
        else:
            out.append(n); cur = b; n = 1
    out.append(n)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--emit", action="store_true", help="C++ 표를 stdout에 출력")
    ap.add_argument("--skip-capacity", action="store_true", help="용량 실측 생략(느림)")
    args = ap.parse_args()

    raw = bwipp_resource("micropdf417")
    var = metrics(raw, "nonccametrics")
    cca = metrics(raw, "ccametrics")
    print(f">> 변형표: 일반 {len(var)}개, CC-A {len(cca)}개", file=sys.stderr)

    # (1) 변형표 검증 — raw 모드로 데이터 코드워드 수를 직접 잰다
    if not args.skip_capacity:
        bad = 0
        for cols, rows, ec, *_ in var:
            want, got = rows * cols - ec, capacity(rows, cols)
            if want != got:
                bad += 1
                print(f"!! {rows}x{cols} 데이터 기대 {want} 실측 {got}", file=sys.stderr)
        print(f">> 변형표 실측 검증: 불일치 {bad}건", file=sys.stderr)
        if bad:
            return 1

    # (2) RAP 패턴 — 이미지에서 읽고, 같은 인덱스가 여러 변형에서 일치하는지 본다
    side, center = {}, {}
    conflict = 0
    for cols, rows, ec, rl, rc, rr in var:
        try:
            g = module_grid(rows, cols)
        except Exception as e:
            print(f"!! {rows}x{cols} 렌더 실패: {e}", file=sys.stderr)
            continue
        l1, r0, r1, ctr = SEG[cols]
        for r in range(rows):
            for tbl, idx, seg in ((side, (rl - 1 + r) % 52, g[r][0:l1]),
                                  (side, (rr - 1 + r) % 52, g[r][r0:r1]),
                                  (center, (rc - 1 + r) % 52,
                                   g[r][ctr[0]:ctr[1]] if ctr else None)):
                if seg is None:
                    continue
                s = "".join(map(str, seg))
                if idx in tbl and tbl[idx] != s:
                    conflict += 1
                    print(f"!! RAP {idx} 충돌: {tbl[idx]} vs {s}", file=sys.stderr)
                tbl[idx] = s
    print(f">> RAP: 좌우 {len(side)}/52, 중앙 {len(center)}/52, 충돌 {conflict}건",
          file=sys.stderr)
    if conflict or len(side) != 52 or len(center) != 52:
        return 1

    if not args.emit:
        print(">> 검증만 수행했다. C++ 표를 얻으려면 --emit", file=sys.stderr)
        return 0

    def emit(name, tbl):
        print(f"const int {name}[52][6] = {{")
        for i in range(0, 52, 4):
            print("    " + " ".join(
                "{%s}," % ",".join(map(str, runs(tbl[j]))) for j in range(i, min(i + 4, 52))))
        print("};")

    emit("kRapSide", side)
    print()
    emit("kRapCenter", center)
    print()
    print("// {열, 행, EC 코드워드 수, 좌 RAP(1기반), 중앙 RAP, 우 RAP}")
    print(f"const int kVariants[{len(var)}][6] = {{")
    for r in var:
        print("    {%d,%2d,%2d,%2d,%2d,%2d}," % tuple(r))
    print("};")
    return 0


if __name__ == "__main__":
    sys.exit(main())
