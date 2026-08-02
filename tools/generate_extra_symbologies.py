#!/usr/bin/env python3
"""
generate_extra_symbologies.py — zxing에 없는 1D 심볼로지 시험 이미지 생성기.

## 왜 generate_corpus.py에 안 넣었나

`generate_corpus.py`는 씨드만 같으면 바이트 단위로 재현되는 회귀 코퍼스라
심볼로지를 추가하면 기존 씨드의 이미지 배분이 바뀐다. 그리고 여기 있는
셋(Industrial 2of5 / COOP 2of5 / Pharmacode)은 **기본이 꺼져 있는**
opt-in 심볼로지라, 매 회귀에서 돌 이유가 없다. 그래서 따로 둔다.

## 인코딩 표의 출처와 교차 검증

표는 BWIPP(uk.co.terryburton.bwipp)의 code2of5 / pharmacode 리소스에서
가져왔다. **생성기와 디코더가 같은 표를 쓰면 표가 틀려도 서로 맞아서
통과한다**(공통 원인 오류). 그래서 `--bwipp`를 주면 treepoem으로 진짜
BWIPP를 호출해서 만든다 — 생성 경로가 완전히 독립이므로 그걸로 한 번
확인하면 표가 맞는지 증명된다. 기본 경로는 ghostscript 의존이 없다.

    python3 tools/generate_extra_symbologies.py --outdir /tmp/x            # 자체 생성
    python3 tools/generate_extra_symbologies.py --outdir /tmp/x --bwipp    # BWIPP 생성

## 출력

<outdir>/*.pgm 과 labels.tsv (verify_accuracy가 읽는 형식과 동일:
파일명 <TAB> 코드수 <TAB> 심볼로지:텍스트 ... <TAB> 태그)
"""
import argparse
import math
import os
import random
import sys

import numpy as np

# --- 인코딩 표 (모듈 배수. 1=가는 것, 3=굵은 것, 막대부터 교대) -------------
IND25_DIGIT = [
    "1111313111", "3111111131", "1131111131", "3131111111", "1111311131",
    "3111311111", "1131311111", "1111113131", "3111113111", "1131113111",
]
IND25_START, IND25_STOP = "313111", "31113"

COOP25_DIGIT = [
    "331111", "111331", "113131", "113311", "131131",
    "131311", "133111", "311131", "311311", "313111",
]
COOP25_START, COOP25_STOP = "3131", "133"


def widths_2of5(text, digits, start, stop):
    s = start + "".join(digits[int(c)] for c in text) + stop
    return [int(c) for c in s]


def widths_pharmacode(value):
    """value(3..131070) -> 막대/공백 교대 폭 배열.

    막대만 정보를 갖는다. 왼쪽 막대부터 b0..b(n-1)일 때
        value = sum (b_i + 1) * 2^(n-1-i),  가는 것 0 / 굵은 것 1
    이므로 인코딩은 그 역이다.
    """
    v = int(value)
    if not (3 <= v <= 131070):
        raise ValueError("pharmacode 범위 밖: %d" % v)
    bits = []
    while v > 0:
        if v % 2 == 0:
            bits.append(1)          # 굵은 막대
            v = (v - 2) // 2
        else:
            bits.append(0)          # 가는 막대
            v = (v - 1) // 2
    bits.reverse()
    out = []
    for i, b in enumerate(bits):
        if i:
            out.append(2)           # 공백은 항상 같은 폭(가는 막대의 2배 정도)
        out.append(3 if b else 1)
    return out


def render(widths, module, height, quiet):
    """폭 배열(막대부터 교대)을 PGM용 numpy 배열로."""
    total = sum(widths) * module
    W = total + 2 * quiet
    H = height + 2 * quiet
    img = np.full((H, W), 255, dtype=np.uint8)
    x = quiet
    bar = True
    for w in widths:
        px = w * module
        if bar:
            img[quiet:quiet + height, x:x + px] = 0
        x += px
        bar = not bar
    return img


# --- 열화 (generate_corpus.py와 같은 축을 최소한으로 재현) -------------------

def degrade(img, rng, blur=0.0, noise=0.0, contrast=1.0, rot=0.0):
    a = img.astype(np.float32)
    if contrast < 1.0:
        mid = 128.0
        a = mid + (a - mid) * contrast
    if blur > 0:
        r = int(max(1, round(blur)))
        k = 2 * r + 1
        pad = np.pad(a, r, mode="edge")
        acc = np.zeros_like(a)
        for dy in range(k):
            for dx in range(k):
                acc += pad[dy:dy + a.shape[0], dx:dx + a.shape[1]]
        a = acc / (k * k)
    if rot:
        a = rotate(a, rot)
    if noise > 0:
        a = a + rng.normal(0.0, noise, a.shape)
    return np.clip(a, 0, 255).astype(np.uint8)


def rotate(a, deg):
    """캔버스를 넓혀서 자르지 않는 최근접 회전 (생성기 결함 방지 — §3.29)."""
    h, w = a.shape
    t = math.radians(deg)
    c, s = math.cos(t), math.sin(t)
    nw = int(abs(w * c) + abs(h * s)) + 2
    nh = int(abs(w * s) + abs(h * c)) + 2
    yy, xx = np.mgrid[0:nh, 0:nw]
    cx0, cy0 = nw / 2.0, nh / 2.0
    cx1, cy1 = w / 2.0, h / 2.0
    sx = (xx - cx0) * c + (yy - cy0) * s + cx1
    sy = -(xx - cx0) * s + (yy - cy0) * c + cy1
    sx = np.clip(np.round(sx).astype(int), -1, w)
    sy = np.clip(np.round(sy).astype(int), -1, h)
    out = np.full((nh, nw), 255.0, dtype=np.float32)
    ok = (sx >= 0) & (sx < w) & (sy >= 0) & (sy < h)
    out[ok] = a[sy[ok], sx[ok]]
    return out


def save_pgm(path, img):
    with open(path, "wb") as f:
        f.write(b"P5\n%d %d\n255\n" % (img.shape[1], img.shape[0]))
        f.write(img.tobytes())


# --- BWIPP 경로 (독립 검증용) -----------------------------------------------

def bwipp_render(kind, text, module, quiet):
    import treepoem
    from PIL import Image
    btype = {"IND25": "industrial2of5", "COOP25": "coop2of5",
             "PHARMA": "pharmacode"}[kind]
    img = treepoem.generate_barcode(barcode_type=btype, data=text).convert("L")
    w, h = img.size
    img = img.resize((w * module, h * module), Image.NEAREST)
    a = np.array(img)
    return np.pad(a, quiet, mode="constant", constant_values=255)


CASES = [
    # (태그, 모듈, 블러, 노이즈, 대비, 회전)
    ("clean",      8, 0.0,  0.0, 1.00,  0),
    ("module-4",   4, 0.0,  0.0, 1.00,  0),
    ("module-3",   3, 0.0,  0.0, 1.00,  0),
    ("module-2",   2, 0.0,  0.0, 1.00,  0),
    ("blur-2",     8, 2.0,  0.0, 1.00,  0),
    ("blur-4",     8, 4.0,  0.0, 1.00,  0),
    ("noise-20",   8, 0.0, 20.0, 1.00,  0),
    ("noise-40",   8, 0.0, 40.0, 1.00,  0),
    ("contrast-30", 8, 0.0, 0.0, 0.30,  0),
    ("contrast-15", 8, 0.0, 0.0, 0.15,  0),
    ("rot-15",     8, 0.0,  0.0, 1.00, 15),
    ("rot-45",     8, 0.0,  0.0, 1.00, 45),
    ("rot-90",     8, 0.0,  0.0, 1.00, 90),
]

PAYLOAD = {"IND25": "1234567890", "COOP25": "1234567890", "PHARMA": "1234"}
SYMNAME = {"IND25": "Industrial-2of5", "COOP25": "COOP-2of5", "PHARMA": "Pharmacode"}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--outdir", required=True)
    ap.add_argument("--bwipp", action="store_true", help="treepoem/BWIPP로 생성(표 교차검증용)")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--height", type=int, default=160)
    ap.add_argument("--quiet", type=int, default=120)
    args = ap.parse_args()

    os.makedirs(args.outdir, exist_ok=True)
    rng = np.random.default_rng(args.seed)
    rows = []

    for kind, text in PAYLOAD.items():
        for tag, module, blur, noise, contrast, rot in CASES:
            if args.bwipp:
                base = bwipp_render(kind, text, module, args.quiet)
            elif kind == "PHARMA":
                base = render(widths_pharmacode(int(text)), module, args.height, args.quiet)
            elif kind == "IND25":
                base = render(widths_2of5(text, IND25_DIGIT, IND25_START, IND25_STOP),
                              module, args.height, args.quiet)
            else:
                base = render(widths_2of5(text, COOP25_DIGIT, COOP25_START, COOP25_STOP),
                              module, args.height, args.quiet)
            img = degrade(base, rng, blur, noise, contrast, rot)
            name = "%s_%s.pgm" % (kind.lower(), tag)
            save_pgm(os.path.join(args.outdir, name), img)
            rows.append((name, 1, "%s:%s" % (SYMNAME[kind], text), tag))

    with open(os.path.join(args.outdir, "labels.tsv"), "w") as f:
        for name, n, code, tag in rows:
            f.write("%s\t%d\t%s\t%s\n" % (name, n, code, tag))
    print("%d장 생성: %s" % (len(rows), args.outdir))


if __name__ == "__main__":
    sys.exit(main())
