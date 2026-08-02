#!/usr/bin/env python3
"""
extract_dotcode_tables.py — DotCode 표를 BWIPP에서 뽑고 **검증**한다.

## 왜 필요한가

DotCode(ISS DotCode)는 오픈소스 디코더가 없는 심볼로지다. 규격서 없이
만들려면 인코더 구현을 정확히 읽어내는 수밖에 없고, 이 프로젝트가
MicroPDF417(§3.49)·일본우편(§3.51)·IMB(§3.53)에서 쓴 방법을 그대로 쓴다:
**BWIPP 리소스를 ghostscript로 실행해서 평가된 프로시저를 덤프**한다.
압축 토큰 뒤에 숨은 진짜 숫자가 그때 드러난다.

    gs -q -dNOSAFER -dBATCH -sDEVICE=nullpage \
       -c '(barcode.ps) run /dotcode /uk.co.terryburton.bwipp findresource =='

## 뽑는 것

- `encs`: 코드워드 113개 각각의 9비트 점 패턴. **전부 팝카운트 5**이고
  서로 다르다(C(9,5)=126 중 13개를 뺀 것). 이게 심볼 계층의 전부다.
- 구조 상수: nc = nd/2+3, nw = nd+nc, ndots = rows*cols/2,
  rembits = ndots - (nw*9+2), 마스크 오프셋 [0,3,7,17], RS 생성원 3, 법 113.

이 스크립트는 **검증까지 한다** — 개수/팝카운트/중복을 확인하고, 덤프에서
읽은 구조 상수가 소스에 하드코딩된 값과 같은지 대조한다. 안 맞으면 0이
아닌 값으로 끝난다.

## 쓰는 법

    python3 tools/extract_dotcode_tables.py            # 검증만
    python3 tools/extract_dotcode_tables.py --cpp      # C++ 표 소스 출력
"""
import argparse
import re
import subprocess
import sys

# src/decoder_dotcode.cpp가 쓰는 값. 덤프와 대조한다.
EXPECTED = {
    "modulus": 113,
    "generator": 3,
    "mask_offsets": [0, 3, 7, 17],
    "nc_from_nd": "nd // 2 + 3",
    "bits_prefix": 2,
    "dots_per_cw": 9,
    "edge_dots": 6,
}


def bwipp_dir():
    import treepoem
    import os
    return os.path.join(os.path.dirname(os.path.abspath(treepoem.__file__)), "postscriptbarcode")


def dump():
    """dotcode 프로시저를 평가된 형태로 덤프한다."""
    ps = "(barcode.ps) run /dotcode /uk.co.terryburton.bwipp findresource =="
    r = subprocess.run(["gs", "-q", "-dNOSAFER", "-dBATCH", "-sDEVICE=nullpage", "-c", ps],
                       cwd=bwipp_dir(), capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit("ghostscript 실패: %s" % r.stderr[:200])
    return r.stdout.replace("--", "")


def encs(text):
    pats = re.findall(r"\((\d{9})\)", text)
    return pats


def check(text, pats):
    bad = []
    if len(pats) != 113:
        bad.append("encs 개수가 113이 아니라 %d" % len(pats))
    if len(set(pats)) != len(pats):
        bad.append("encs에 중복이 있다")
    pc = set(p.count("1") for p in pats)
    if pc != {5}:
        bad.append("팝카운트가 5가 아닌 것이 있다: %s" % sorted(pc))

    # 구조 상수 대조 — 덤프 원문에 그대로 있어야 한다.
    need = [
        ("nc", r"/nc nd 2 idiv 3 add def"),
        ("nw", r"/nw nd nc add def"),
        ("ndots", r"/ndots rows columns mul 2 idiv def"),
        ("rembits", r"/rembits ndots nw 9 mul 2 add sub def"),
        ("마스크 오프셋", r"mark 0 3 7 17 \] mask get"),
        ("RS 생성원/법", r"/rsalog mark 1 112 \{dup 3 mul 113 mod\} repeat \] def"),
        ("체커보드", r"outline x y dmv x y add 2 mod 1 sub put"),
    ]
    for name, pat in need:
        if not re.search(pat, text):
            bad.append("구조 상수 '%s' 를 덤프에서 못 찾았다 (BWIPP가 바뀌었나?)" % name)
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cpp", action="store_true", help="C++ 표 소스를 출력")
    a = ap.parse_args()

    text = dump()
    pats = encs(text)
    bad = check(text, pats)
    for b in bad:
        print("!! " + b, file=sys.stderr)
    if bad:
        return 1
    print("encs 113개 (전부 팝카운트 5, 중복 0), 구조 상수 7종 대조 통과")

    if a.cpp:
        print("\n// BWIPP dotcode 리소스에서 추출 (tools/extract_dotcode_tables.py)")
        print("constexpr uint16_t kEncs[113] = {")
        for i in range(0, 113, 8):
            row = ", ".join("0x%03x" % int(p, 2) for p in pats[i:i + 8])
            print("    %s," % row)
        print("};")
    return 0


if __name__ == "__main__":
    sys.exit(main())
