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
    python3 tools/generate_extra_symbologies.py --outdir /tmp/m --micropdf417
    python3 tools/generate_extra_symbologies.py --outdir /tmp/c --composite
    python3 tools/generate_extra_symbologies.py --outdir /tmp/j --japanpost
    python3 tools/generate_extra_symbologies.py --outdir /tmp/i --imb
    python3 tools/generate_extra_symbologies.py --outdir /tmp/d --dotcode

## MicroPDF417은 BWIPP가 필요하다

--micropdf417은 자체 생성 경로가 없다. 인코더를 직접 쓰면 디코더와 같은
표를 공유하게 돼서(공통 원인 오류) 시험의 의미가 없어지고, 무엇보다
고수준 인코딩까지 직접 쓰는 건 디코더를 두 번 쓰는 일이다. 그래서
BWIPP(treepoem + ghostscript)만 쓴다. 34개 변형 전부 + 열화 축을 낸다.

## 출력

<outdir>/*.pgm 과 labels.tsv. 열 배치는 verify_accuracy의 `loadLabels`가
읽는 것과 **정확히 같아야 한다**:

    파일명 <TAB> 코드수 <TAB> 태그(,) <TAB> 정답텍스트(|) <TAB> 심볼로지(|) <TAB> 코드태그(|)

2026-08-02까지 이 스크립트는 3열에 "심볼로지:텍스트"를, 4열에 태그를 넣고
있었다. verify_accuracy는 3열을 태그로, 4열을 정답 텍스트로 읽으므로
**정답이 "-" 한 글자로 들어갔고, 읽어낸 코드는 전부 "정답에 없는 코드"
= 오디코딩으로 집계됐다**. 실제로 IMB 13장이 검출 13/13인데 misdec=13으로
찍혔다. 검출 수치는 맞았지만 텍스트 대조는 처음부터 아무것도 검증하지
않고 있었다. 지금은 `write_labels()` 하나로 통일해서 그 자리를 막았다.
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


def write_labels(outdir, rows):
    """rows = [(파일명, 심볼로지, 정답텍스트, 태그), ...]

    열 배치는 verify_accuracy의 loadLabels가 읽는 것과 같아야 한다(위 주석).
    태그 열에는 심볼로지 이름도 같이 넣어서 심볼로지별 집계가 되게 한다.

    정답텍스트/심볼로지는 리스트도 된다. 한 장에 코드가 둘 이상 있는
    경우(GS1 Composite는 선형 성분 + 2D 성분 두 개다)에 **둘 다 적어야**
    한다. 하나만 적으면 나머지 하나가 "정답에 없는 코드" = 오디코딩으로
    잘못 집계된다.
    """
    with open(os.path.join(outdir, "labels.tsv"), "w") as f:
        for name, sym, text, tag in rows:
            texts = [text] if isinstance(text, str) else list(text)
            syms = [sym] if isinstance(sym, str) else list(sym)
            tags = ",".join(syms) + ("," + tag if tag and tag != "-" else "")
            f.write("%s\t%d\t%s\t%s\t%s\t%s\n"
                    % (name, len(texts), tags, "|".join(texts), "|".join(syms), tag or "-"))
    print("%d장 생성: %s" % (len(rows), outdir))


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


MPDF_AXES = [
    # (태그, 배율, 블러, 노이즈, 대비, 회전)
    ("mod-12", 6, 0.0,  0.0, 1.00,   0),
    ("mod-8",  4, 0.0,  0.0, 1.00,   0),
    ("mod-4",  2, 0.0,  0.0, 1.00,   0),
    ("mod-2",  1, 0.0,  0.0, 1.00,   0),
    ("blur-1", 4, 1.0,  0.0, 1.00,   0),
    ("blur-2", 4, 2.0,  0.0, 1.00,   0),
    ("blur-3", 4, 3.0,  0.0, 1.00,   0),
    ("noise-10", 4, 0.0, 10.0, 1.00, 0),
    ("noise-20", 4, 0.0, 20.0, 1.00, 0),
    ("noise-30", 4, 0.0, 30.0, 1.00, 0),
    ("contrast-50", 4, 0.0, 0.0, 0.50, 0),
    ("contrast-30", 4, 0.0, 0.0, 0.30, 0),
    ("contrast-15", 4, 0.0, 0.0, 0.15, 0),
    ("rot-15",  4, 0.0, 0.0, 1.00,  15),
    ("rot-45",  4, 0.0, 0.0, 1.00,  45),
    ("rot-90",  4, 0.0, 0.0, 1.00,  90),
    ("rot-180", 4, 0.0, 0.0, 1.00, 180),
    ("rot-270", 4, 0.0, 0.0, 1.00, 270),
]
MPDF_PAYLOAD = "MPDF417-TEST"


def micropdf417_main(outdir, quiet, seed):
    """MicroPDF417 시험셋: 34개 변형(깨끗) + 한 변형에 열화 축."""
    import treepoem
    from PIL import Image
    variants = _mpdf_variants()
    rng = np.random.default_rng(seed)
    os.makedirs(outdir, exist_ok=True)
    rows_out = []

    def render(r, c, scale):
        img = treepoem.generate_barcode("micropdf417", MPDF_PAYLOAD,
                                        options={"version": f"{r}x{c}"}).convert("L")
        w, h = img.size
        return np.array(img.resize((w * scale, h * scale), Image.NEAREST))

    def emit(name, a):
        a = np.clip(a, 0, 255).astype(np.uint8)
        save_pgm(os.path.join(outdir, name), a)
        rows_out.append((name, "MicroPDF417", MPDF_PAYLOAD, name.split("_")[1].rsplit(".", 1)[0]))

    for cols, rows, ec, *_ in variants:
        try:
            a = render(rows, cols, 4)
        except Exception:
            continue                       # 이 변형에 payload가 안 들어가면 건너뛴다
        emit(f"var_{rows}x{cols}.pgm", np.pad(a, quiet, constant_values=255))

    for tag, scale, blur_s, noise_s, contrast, rotdeg in MPDF_AXES:
        a = np.pad(render(14, 2, scale), quiet, constant_values=255)
        emit(f"ax_{tag}.pgm", degrade(a, rng, blur_s, noise_s, contrast, rotdeg))

    write_labels(outdir, rows_out)


def _mpdf_variants():
    """BWIPP 리소스에서 MicroPDF417 변형표를 읽는다.

    tools/extract_micropdf417_tables.py와 같은 표다. 여기서는 "어떤 변형이
    있는가"만 필요하므로 그 스크립트의 함수를 그대로 빌려 쓴다.
    """
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import extract_micropdf417_tables as ex
    return ex.metrics(ex.bwipp_resource("micropdf417"), "nonccametrics")


# GS1 Composite: (선형 종류, 선형 데이터, 2D 페이로드, 기대 2D 문자열)
# 기대 문자열은 AI와 값을 이어 붙이고 가변 길이 뒤에 0x1D를 넣은 것이다
# (gs1.cpp의 parseGS1이 먹는 형식).
#
# Composite는 **한 장에 코드가 둘**이다 — 선형 성분과 2D 성분. 정답에 둘
# 다 적어야 한다. 선형 쪽 기대 문자열은 심볼로지마다 표기가 다르다:
# GS1-128은 괄호 AI 그대로, DataBar는 AI 없이 값만, EAN-13은 숫자 그대로.
# (입력 데이터에서 그대로 유도되는 값이지 출력에서 베껴온 값이 아니다.)
GS = "\x1d"
COMPOSITE_CASES = [
    ("gs1-128composite", "(01)03412345678900", "(99)ABCDEF",        "99ABCDEF",
     "GS1-128", "(01)03412345678900"),
    ("gs1-128composite", "(01)03412345678900", "(99)1234-abcd",     "991234-abcd",
     "GS1-128", "(01)03412345678900"),
    ("gs1-128composite", "(01)03412345678900", "(17)250630(10)L9",  "1725063010L9",
     "GS1-128", "(01)03412345678900"),
    ("gs1-128composite", "(01)03412345678900", "(10)LOT123",        "10LOT123",
     "GS1-128", "(01)03412345678900"),
    ("gs1-128composite", "(01)03412345678900", "(90)12X(21)SER1",   "9012X" + GS + "21SER1",
     "GS1-128", "(01)03412345678900"),
    ("databaromnicomposite", "(01)03412345678900", "(99)ABC123",    "99ABC123",
     "GS1-DataBar", "03412345678900"),
    ("ean13composite", "9771234567003", "(99)Hello",                "99Hello",
     "EAN/UPC", "9771234567003"),
]


def composite_main(outdir, quiet):
    """GS1 Composite 시험셋: CC-A / CC-B / CC-C + 선형 종류 세 가지.

    CC-C는 2D 성분이 MicroPDF417이 아니라 **PDF417**이다. EAN/UPC 위에는
    올릴 수 없어서(규격상 CC-A/CC-B만) 그 조합은 BWIPP가 거부하고, 아래
    try/except가 건너뛴다.
    """
    import treepoem
    from PIL import Image
    os.makedirs(outdir, exist_ok=True)
    rows_out = []
    for idx, (btype, linear, payload, want, linsym, linwant) in enumerate(COMPOSITE_CASES):
        for ver in ("a", "b", "c"):
            try:
                img = treepoem.generate_barcode(btype, linear + "|" + payload,
                                                options={"ccversion": ver}).convert("L")
            except Exception:
                continue
            w, h = img.size
            a = np.array(img.resize((w * 6, h * 6), Image.NEAREST))
            name = "cc%s_%02d.pgm" % (ver, idx)
            save_pgm(os.path.join(outdir, name), np.pad(a, quiet, constant_values=255))
            rows_out.append((name, [linsym, "GS1-Composite"], [linwant, want], "cc" + ver))
    write_labels(outdir, rows_out)


JP_PAYLOADS = ["1234567", "123-4567", "1234567ABC", "9876543A12", "100-0001", "5300012K9"]
JP_AXES = [
    ("clean",      6, 0.0,  0.0, 1.00,   0),
    ("mod-4",      4, 0.0,  0.0, 1.00,   0),
    ("mod-2",      2, 0.0,  0.0, 1.00,   0),
    ("blur-2",     6, 2.0,  0.0, 1.00,   0),
    ("noise-20",   6, 0.0, 20.0, 1.00,   0),
    ("noise-40",   6, 0.0, 40.0, 1.00,   0),
    ("contrast-30", 6, 0.0, 0.0, 0.30,   0),
    ("rot-90",     6, 0.0,  0.0, 1.00,  90),
    ("rot-180",    6, 0.0,  0.0, 1.00, 180),
    ("rot-270",    6, 0.0,  0.0, 1.00, 270),
]


def japanpost_main(outdir, quiet, seed):
    """일본우편 시험셋: 페이로드 6종(깨끗) + 한 페이로드에 열화/회전 축."""
    import treepoem
    from PIL import Image
    os.makedirs(outdir, exist_ok=True)
    rng = np.random.default_rng(seed)
    rows_out = []

    def render(data, scale):
        img = treepoem.generate_barcode("japanpost", data).convert("L")
        w, h = img.size
        return np.array(img.resize((w * scale, h * scale), Image.NEAREST))

    def emit(name, a, want, tag):
        a = np.ascontiguousarray(np.clip(a, 0, 255).astype(np.uint8))
        save_pgm(os.path.join(outdir, name), a)
        rows_out.append((name, "PostalJP", want, tag))

    for i, d in enumerate(JP_PAYLOADS):
        emit("jp_%02d.pgm" % i, np.pad(render(d, 6), quiet, constant_values=255), d, "clean")

    D = "1234567ABC"
    for tag, scale, blur_s, noise_s, contrast, rotdeg in JP_AXES:
        a = np.pad(render(D, scale), quiet, constant_values=255).astype(float)
        # 90의 배수는 회전 보간 없이 정확히 돌린다 — 4-state는 막대 끝이 생명이다
        if rotdeg in (90, 180, 270):
            a = np.rot90(a, rotdeg // 90)
            a = degrade(a, rng, blur_s, noise_s, contrast, 0)
        else:
            a = degrade(a, rng, blur_s, noise_s, contrast, rotdeg)
        emit("jpax_%s.pgm" % tag, a, D, tag)

    write_labels(outdir, rows_out)


IMB_PAYLOADS = [
    "01234567094987654321",
    "0123456709498765432101234",
    "01234567094987654321012345678",
    "0123456709498765432101234567891",
    "12345678901234567890",
]
IMB_AXES = [
    ("clean",       6, 0.0,  0.0, 1.00,   0),
    ("mod-4",       4, 0.0,  0.0, 1.00,   0),
    ("mod-2",       2, 0.0,  0.0, 1.00,   0),
    ("blur-2",      6, 2.0,  0.0, 1.00,   0),
    ("noise-30",    6, 0.0, 30.0, 1.00,   0),
    ("contrast-30", 6, 0.0,  0.0, 0.30,   0),
    ("rot-90",      6, 0.0,  0.0, 1.00,  90),
    ("rot-180",     6, 0.0,  0.0, 1.00, 180),
    ("rot-270",     6, 0.0,  0.0, 1.00, 270),
]


def imb_main(outdir, quiet, seed):
    """IMB(USPS Intelligent Mail) 시험셋."""
    import treepoem
    from PIL import Image
    os.makedirs(outdir, exist_ok=True)
    rng = np.random.default_rng(seed)
    rows_out = []

    def render(data, scale):
        img = treepoem.generate_barcode("onecode", data).convert("L")
        w, h = img.size
        return np.array(img.resize((w * scale, h * scale), Image.NEAREST))

    def emit(name, a, want, tag):
        a = np.ascontiguousarray(np.clip(a, 0, 255).astype(np.uint8))
        save_pgm(os.path.join(outdir, name), a)
        rows_out.append((name, "PostalIMB", want, tag))

    for i, d in enumerate(IMB_PAYLOADS):
        emit("imb_%02d.pgm" % i, np.pad(render(d, 6), quiet, constant_values=255), d, "clean")

    D = IMB_PAYLOADS[2]
    for tag, scale, blur_s, noise_s, contrast, rotdeg in IMB_AXES:
        a = np.pad(render(D, scale), quiet, constant_values=255).astype(float)
        if rotdeg in (90, 180, 270):
            a = degrade(np.rot90(a, rotdeg // 90), rng, blur_s, noise_s, contrast, 0)
        else:
            a = degrade(a, rng, blur_s, noise_s, contrast, rotdeg)
        emit("imbax_%s.pgm" % tag, a, D, tag)

    write_labels(outdir, rows_out)


DOTCODE_PAYLOADS = [
    "DOTCODE-TEST", "Hello, World!", "0123456789012345678",
    "ABCdef123XYZ", "2026-08-02-LOT99", "1234567890123456",
]
DOTCODE_AXES = [
    # (태그, 배율, 블러, 노이즈, 대비, 회전)
    ("clean",       3, 0.0,  0.0, 1.00,   0),
    ("scale-2",     2, 0.0,  0.0, 1.00,   0),
    ("scale-5",     5, 0.0,  0.0, 1.00,   0),
    ("blur-1",      3, 1.0,  0.0, 1.00,   0),
    ("blur-2",      3, 2.0,  0.0, 1.00,   0),
    ("noise-10",    3, 0.0, 10.0, 1.00,   0),
    ("noise-20",    3, 0.0, 20.0, 1.00,   0),
    ("contrast-50", 3, 0.0,  0.0, 0.50,   0),
    ("contrast-30", 3, 0.0,  0.0, 0.30,   0),
    ("rot-15",      3, 0.0,  0.0, 1.00,  15),
    ("rot-30",      3, 0.0,  0.0, 1.00,  30),
    ("rot-45",      3, 0.0,  0.0, 1.00,  45),
    ("rot-90",      3, 0.0,  0.0, 1.00,  90),
    ("rot-180",     3, 0.0,  0.0, 1.00, 180),
]


def dotcode_main(outdir, quiet, seed):
    """DotCode 시험셋: 페이로드 6종(깨끗) + 한 페이로드에 열화/회전 축.

    DotCode는 자체 생성 경로가 없다 — 인코더를 직접 쓰면 디코더와 같은 표를
    공유하게 돼서(공통 원인 오류) 시험의 뜻이 없어진다. BWIPP만 쓴다.
    """
    import treepoem
    from PIL import Image
    os.makedirs(outdir, exist_ok=True)
    rng = np.random.default_rng(seed)
    rows_out = []

    def render(data, scale):
        img = treepoem.generate_barcode("dotcode", data).convert("L")
        w, h = img.size
        return np.array(img.resize((w * scale, h * scale), Image.NEAREST))

    def emit(name, a, want, tag):
        a = np.ascontiguousarray(np.clip(a, 0, 255).astype(np.uint8))
        save_pgm(os.path.join(outdir, name), a)
        rows_out.append((name, "DotCode", want, tag))

    for i, d in enumerate(DOTCODE_PAYLOADS):
        emit("dc_%02d.pgm" % i, np.pad(render(d, 3), quiet, constant_values=255), d, "clean")

    D = DOTCODE_PAYLOADS[0]
    for tag, scale, blur_s, noise_s, contrast, rotdeg in DOTCODE_AXES:
        a = np.pad(render(D, scale), quiet, constant_values=255).astype(float)
        if rotdeg in (90, 180, 270):
            a = degrade(np.rot90(a, rotdeg // 90), rng, blur_s, noise_s, contrast, 0)
        else:
            a = degrade(a, rng, blur_s, noise_s, contrast, rotdeg)
        emit("dcax_%s.pgm" % tag, a, D, tag)

    write_labels(outdir, rows_out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--outdir", required=True)
    ap.add_argument("--dotcode", action="store_true",
                    help="DotCode 시험셋 (BWIPP 필요)")
    ap.add_argument("--imb", action="store_true",
                    help="IMB(USPS Intelligent Mail) 시험셋(BWIPP 필요)")
    ap.add_argument("--japanpost", action="store_true",
                    help="일본우편 고객 바코드 시험셋(BWIPP 필요)")
    ap.add_argument("--micropdf417", action="store_true",
                    help="MicroPDF417 시험셋(BWIPP 필요)")
    ap.add_argument("--composite", action="store_true",
                    help="GS1 Composite 시험셋(BWIPP 필요)")
    ap.add_argument("--bwipp", action="store_true", help="treepoem/BWIPP로 생성(표 교차검증용)")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--height", type=int, default=160)
    ap.add_argument("--quiet", type=int, default=120)
    args = ap.parse_args()

    if args.micropdf417:
        return micropdf417_main(args.outdir, args.quiet, args.seed)
    if args.composite:
        return composite_main(args.outdir, args.quiet)
    if args.japanpost:
        return japanpost_main(args.outdir, args.quiet, args.seed)
    if args.imb:
        return imb_main(args.outdir, args.quiet, args.seed)
    if args.dotcode:
        return dotcode_main(args.outdir, args.quiet, args.seed)

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
            rows.append((name, SYMNAME[kind], text, tag))

    write_labels(args.outdir, rows)


if __name__ == "__main__":
    sys.exit(main())
