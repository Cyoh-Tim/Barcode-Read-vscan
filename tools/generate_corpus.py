#!/usr/bin/env python3
"""
generate_corpus.py — 악조건 **대량 코퍼스** 생성기 (수백~수만 장).

## 이게 왜 따로 있나

`generate_stress_images.py`는 손으로 고른 **40종 고정 셋**이다. 회귀 게이트
(세 경로 모두 37/40, PROJECT_NOTES §7)라서 바이트 단위로 재현돼야 하고,
그래서 손대지 않는다. 반면 성능/정확도 튜닝을 할 때 40장은 표본이 너무
작다:

- 조건 하나에 이미지 1장 -> "노이즈에서 3ms 빨라졌다"가 실제 개선인지
  그 한 장의 우연인지 구분이 안 된다.
- 조건 조합(블러+저대비+회전 동시)이 거의 없다. 현장은 조합이 기본이다.
- 40장에 맞춰 튜닝하면 그 40장에 과적합된다. 실제로 있었던 일:
  1D 회전 케이스가 없어서 "회전은 잘 된다"고 잘못 결론냈다(§3.2.15).

이 스크립트는 조건 축(심볼로지 / 개수 / 크기 / 블러 / 노이즈 / 노출 /
대비 / 회전 / 원근 / 곡면 / 반사 / 그림자 / 오염 / 손상 / 인쇄불량 /
DPM / 콰이어트존 / 클러터)을 **난수로 조합**해서 원하는 만큼 뽑는다.
각 이미지의 정답(코드 개수 + 각 코드의 심볼로지/텍스트/위치)과 적용된
조건 태그를 `labels.tsv`에 같이 쓴다. `verify_accuracy`가 이 파일을 읽어
**조건 축별 검출률/시간**을 집계한다 — "무엇이 느린가/무엇을 놓치는가"를
축 단위로 볼 수 있다는 게 40장 셋과의 결정적 차이다.

## 중요: 이 코퍼스는 합격/불합격 게이트가 아니다

40종은 "37/40 미달이면 회귀"라는 절대 기준선이다. 대량 코퍼스는 조건을
난수로 뽑으므로 물리적으로 못 읽는 이미지(45도 1D, 45px + 강블러 등)가
섞인다. **절대 검출률의 목표치는 의미가 없다.** 쓰는 방법은 항상 A/B다:

    같은 --seed로 만든 같은 코퍼스에 대해, 변경 전/후의
    검출률과 시간을 비교한다.

씨드가 같으면 코퍼스는 완전히 동일하게 재생성된다(이미지별 씨드가
(마스터 씨드, 인덱스)에서 파생되므로 --jobs 수나 생성 순서와도 무관).

## 두 가지 모드

**(1) 난수 코퍼스** — 18개 조건축을 난수로 조합. 전반적 A/B 비교용.
**(2) 스윕** — 한 축만 촘촘히 밀고 나머지는 완전 고정(난수 열화 0).
"몇 도부터 끊기나" 같은 임계점을 찾는 용도. 축을 여러 개 주면 데카르트 곱.

## 용량 (중요)

2048x1536 PGM 한 장 = 3.1MB. 1도 간격 360장 x 대비 50단계 = 18,000장 =
**56GB**. 축을 더 걸면 수십만 장 = 수백 GB로 디스크가 그냥 찬다.
그래서 기본 사용법은 파일 저장이 아니라 **스트리밍**이다 — 프레임을 만들어
파이프로 바로 디코더에 먹이고 버린다. 디스크 0, 장수 상한 없음.
파일 모드에는 --max-disk-gb(기본 20GB) 상한이 걸려 있고, 예상 용량이
상한이나 남은 공간의 80%를 넘으면 생성을 시작조차 하지 않는다.

## 사용

    # 스윕: 1도 간격 360장 — 디스크에 한 장도 안 남는다
    python3 tools/generate_corpus.py --sweep angle:0:359:1 --stream \
      | ./verify_accuracy --stdin --paths 2stage

    # 축 두 개 (대비 20단계 x 모듈 27단계 = 540장), 나머지 축은 --base로 고정
    python3 tools/generate_corpus.py --stream \
        --sweep contrast:0.05:1.0:0.05 --sweep module:1.5:8:0.25 \
        --base sym=CODE128 | ./verify_accuracy --stdin

    # 난수 코퍼스 A/B: 변경 전/후에 같은 --seed 로 두 번
    python3 tools/generate_corpus.py -n 5000 --difficulty mixed --stream \
      | ./verify_accuracy --stdin --csv before.csv

    # 용량/시간 미리보기 (아무것도 안 만든다)
    python3 tools/generate_corpus.py --sweep angle:0:359:1 --sweep noise:0:50:2 --est

    # 정말 파일로 남겨야 할 때 (재사용할 고정 코퍼스, 눈으로 볼 이미지)
    python3 tools/generate_corpus.py -o ./corpus -n 2000 --jobs 4 --max-disk-gb 10
    python3 tools/generate_corpus.py -o ./look -n 20 --format png   # 보기용

    # 특정 이미지만 다시 뽑기(실패 케이스 디버깅) — 씨드가 같으면 동일 이미지
    python3 tools/generate_corpus.py -o ./one --only 1734

스윕 축 목록은 --help 참고 (angle module contrast bright blur motion noise
persp curve glare shadow count sym ec).

의존성: pip install qrcode python-barcode pillow numpy
"""
import argparse
import io
import json
import math
import multiprocessing as mp
import os
import sys
import time

import numpy as np
import qrcode
import barcode as pybarcode
from PIL import Image, ImageDraw, ImageFilter, ImageFont

# 선택 의존성 — 없으면 해당 심볼로지만 빠지고 나머지는 그대로 동작한다.
try:
    import treepoem            # BWIPP(+ghostscript): Code93 / DataBar / UPC-E 등
except Exception:
    treepoem = None
try:
    from ppf.datamatrix import DataMatrix
except Exception:
    DataMatrix = None
try:
    import pdf417gen
except Exception:
    pdf417gen = None

# ----------------------------------------------------------------- 폰트
try:
    _FNT = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 28)
    _FNTB = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 44)
except OSError:
    _FNT = _FNTB = ImageFont.load_default()

ALPHA = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-."

# 난이도 프리셋
#   n_degrade: 한 이미지에 몇 개의 조건을 겹쳐 적용할지
#   sev      : 각 조건 파라미터를 최대치 대비 어디까지 밀지 (0~1)
#   mod      : **모듈 하나의 픽셀 크기** 범위. 코드 크기는 여기서 파생된다
#              (모듈 수 x 모듈 픽셀). 검출 난이도를 실제로 지배하는 값이
#              "코드 px"가 아니라 "모듈 px"라서 이걸 1차 손잡이로 삼는다 —
#              1.1px면 원리적으로 못 읽는 영역, 4px 이상이면 여유가 있다.
#   p_deg    : 코드 단위 열화(저대비/반전/DPM/인쇄불량/손상) 확률 배율
#   p_rot    : 회전이 걸릴 확률
# 실측 기준(2048x1536, two_stage, 코드 단위 검출률): easy ~90% / mixed ~55%
# / hard ~15%. A/B 비교는 중간대(mixed)가 가장 민감하다 — 100%나 0%에
# 붙어 있으면 변경 효과가 천장/바닥에 가려서 안 보인다.
DIFFICULTY = {
    "easy":  {"n_degrade": (0, 1), "sev": (0.05, 0.25), "mod": (6.0, 18.0),
              "p_deg": 0.20, "p_rot": 0.18},
    "mixed": {"n_degrade": (1, 3), "sev": (0.10, 0.70), "mod": (3.0, 14.0),
              "p_deg": 0.85, "p_rot": 0.45},
    "hard":  {"n_degrade": (2, 4), "sev": (0.35, 0.95), "mod": (2.2, 8.0),
              "p_deg": 1.20, "p_rot": 0.65},
}


# ============================================================ 심볼 렌더링
def _grid_from_image(img):
    """렌더된 심볼 이미지를 **모듈 격자(불리언 행렬)**로 되돌린다.

    심볼로지마다 인코더가 제각각이다(비트열을 주는 것, PIL 이미지를 주는 것,
    행렬을 주는 것). 이미지를 그대로 리사이즈하면 모듈 폭 비율이 깨지므로
    (§3.8의 1D 렌더링 사고), 일단 전부 모듈 격자로 정규화한 다음 우리가
    원하는 모듈 픽셀 크기로 다시 그린다.

    최소 런 길이 = 모듈 하나의 픽셀 크기라는 성질을 이용한다.
    """
    a = np.array(img.convert("L")) < 128          # True = 검정 모듈
    ys, xs = np.where(a)
    if len(xs) == 0:
        raise ValueError("빈 심볼 이미지")
    a = a[ys.min():ys.max() + 1, xs.min():xs.max() + 1]

    def min_run(arr):
        best = None
        step = max(1, arr.shape[0] // 9)
        for r in range(0, arr.shape[0], step):
            row = arr[r]
            idx = np.flatnonzero(np.diff(row.astype(np.int8)))
            if idx.size == 0:
                continue
            runs = np.diff(np.concatenate(([-1], idx, [len(row) - 1])))
            m = int(runs.min())
            if m > 0:
                best = m if best is None else min(best, m)
        return best or 1

    u = max(1, min(min_run(a), min_run(a.T)))     # 모듈은 정사각이라고 본다
    h = max(1, int(round(a.shape[0] / u)))
    w = max(1, int(round(a.shape[1] / u)))
    yi = np.clip(((np.arange(h) + 0.5) * u).astype(int), 0, a.shape[0] - 1)
    xi = np.clip(((np.arange(w) + 0.5) * u).astype(int), 0, a.shape[1] - 1)
    return a[np.ix_(yi, xi)]                       # 셀 중심 샘플링


def _grid_to_image(grid, module_px, quiet=4, rows=None, quiet_y=None):
    """모듈 격자를 목표 모듈 픽셀 크기로 렌더 (면적 샘플링).

    rows: 1D용. 한 줄짜리 격자를 이 모듈 수만큼 세로로 복제한 뒤 콰이어트존을
          붙인다. **복제를 먼저 해야 한다** — 콰이어트존을 먼저 붙이고 늘리면
          막대가 1모듈 높이로 남아 세로로 잡아늘린 실뭉치가 된다(실측: 1D
          각도 스윕이 90/91에서 2/91로 무너졌다).
    quiet_y: 세로 콰이어트존(모듈). 미지정이면 quiet와 같다.

    정수배가 아닌 모듈 폭도 경계 위치가 정확하다 — 8배로 정확히 키운 뒤
    BOX(면적 평균)로 목표 크기에 맞춘다. 카메라가 하는 일과 같다.
    """
    g = np.asarray(grid, dtype=bool)
    if rows:
        g = np.repeat(g[:1], max(1, int(rows)), axis=0)
    qy = quiet if quiet_y is None else quiet_y
    g = np.pad(g, ((qy, qy), (quiet, quiet)), constant_values=False)
    H, W = g.shape
    base = Image.fromarray(np.where(g, 0, 255).astype(np.uint8), mode="L")
    up = base.resize((W * 8, H * 8), Image.NEAREST)
    tw = max(8, int(round(W * module_px)))
    th = max(8, int(round(H * module_px)))
    return up.resize((tw, th), Image.BOX)


def _mod_tag(mod_px):
    """모듈 하나가 몇 픽셀인지 — 난이도를 실제로 지배하는 값이라 태그로 남긴다.

    "코드 크기 px"는 데이터 길이에 따라 의미가 달라진다(같은 400px여도
    21모듈 QR과 57모듈 QR은 완전히 다른 문제다). 집계는 모듈 픽셀 기준으로
    봐야 비교가 된다.
    """
    if mod_px < 1.6:
        return "mod-1px"
    if mod_px < 2.6:
        return "mod-2px"
    if mod_px < 4.5:
        return "mod-3px"
    return "mod-5px+"


def _qr_img(payload, px, ec):
    """QR 렌더. 모듈 수를 알아야 모듈 픽셀 크기를 태깅할 수 있어서 같이 반환."""
    q = qrcode.QRCode(border=2, box_size=6, error_correction=ec)
    q.add_data(payload)
    q.make(fit=True)
    modules = q.modules_count + 2 * q.border
    img = q.make_image(fill_color="black", back_color="white").convert("L")
    return img.resize((px, px), Image.NEAREST), px / modules


def _bits_to_image(bits, module_px, height_px, quiet=10):
    """모듈 비트열('1'=검정 막대)을 **면적 샘플링**으로 렌더.

    왜 python-barcode 이미지를 리사이즈하지 않는가:
    미리 렌더된 2px/모듈 이미지를 정수배가 아닌 비율로 확대하면(NEAREST면
    열 복제, BILINEAR면 번짐) **막대 폭 비율이 흐트러진다**. 1D 디코딩은 폭
    비율로 문자를 판별하므로, 눈으로는 멀쩡한 바코드가 아예 안 읽힌다.
    실측으로 module 2.2 / 3.6 / 4.0 / 4.8 처럼 특정 값에서만 검출률이 0%로
    떨어지는 톱니 곡선이 나왔고, 코드만 잘라 단독 디코드해도 실패했다 —
    라이브러리 문제가 아니라 **생성기가 만든 가짜 실패**였다.
    모듈 폭이 스윕 축인 도구에서 이건 치명적이라, 비트열에서 직접 그린다.

    출력 픽셀 하나가 덮는 모듈 구간의 평균값을 그대로 쓴다(= 카메라가 하는
    일과 같은 면적 적분). 모듈 폭이 소수여도 경계 위치가 정확하고, 경계
    픽셀만 중간 밝기가 된다.
    """
    pattern = "0" * quiet + bits + "0" * quiet          # 콰이어트존(흰색)
    n = len(pattern)
    white = np.array([0.0 if c == "1" else 1.0 for c in pattern], dtype=np.float64)
    cum = np.concatenate([[0.0], np.cumsum(white)])     # 모듈 단위 적분값

    w = max(60, int(round(n * module_px)))
    edges = np.arange(w + 1, dtype=np.float64) / module_px   # 픽셀 경계 -> 모듈 좌표

    def integral(t):
        t = np.clip(t, 0.0, n)
        i = np.floor(t).astype(np.int64)
        frac = t - i
        i_safe = np.clip(i, 0, n - 1)
        return cum[np.minimum(i, n)] + frac * white[i_safe] * (i < n)

    row = (integral(edges[1:]) - integral(edges[:-1])) * module_px
    row = np.clip(row * 255.0, 0, 255).astype(np.uint8)
    return Image.fromarray(np.tile(row, (max(24, int(height_px)), 1)), mode="L")


def _linear_render(cls, payload, module_px, ratio, box_h, **wopts):
    """1D 심볼을 **모듈 폭 고정**으로 렌더 (폭은 데이터 길이에 따라 늘어난다).

    목표 폭에 억지로 맞춰 리사이즈하면 데이터가 길수록 막대가 가늘어지는
    비현실적 아티팩트가 생긴다 — 같은 "1D 회전 테스트"인데 실제 난이도가
    텍스트 길이에 따라 완전히 달라져서, 알고리즘 한계로 오진했던 전례가
    있다(generate_stress_images.py의 code128() 주석, §3.2.15).
    실제 인쇄는 모듈 폭을 고정하고 라벨 폭이 넓어진다.
    """
    obj = cls(payload, **wopts)
    bits = obj.build()[0]
    w = int(round((len(bits) + 20) * module_px))
    h = int(max(24, min(box_h, w * ratio)))
    return _bits_to_image(bits, module_px, h), obj.get_fullcode()


_LINEAR = {
    "CODE128": (pybarcode.Code128, "CODE_128", {}),
    "EAN13":   (pybarcode.EAN13,   "EAN_13",   {}),
    "EAN8":    (pybarcode.EAN8,    "EAN_8",    {}),
    "UPCA":    (pybarcode.UPCA,    "UPC_A",    {}),
    "CODE39":  (pybarcode.Code39,  "CODE_39",  {"add_checksum": False}),
    "ITF":     (pybarcode.ITF,     "ITF",      {}),
    "CODABAR": (pybarcode.CODABAR, "CODABAR",  {}),
}

# BWIPP(treepoem)로만 만들 수 있는 것들. (BWIPP 타입, vscan 심볼로지 이름)
_BWIPP = {
    # (BWIPP 타입, vscan 심볼로지 이름, BWIPP 옵션)
    # Code93은 C/K 체크문자가 규격상 필수인데 BWIPP 기본이 꺼짐이라,
    # 켜지 않으면 zxing이 (정당하게) 거부한다 — 실측으로 확인.
    "CODE93":     ("code93",          "CODE_93",            {"includecheck": True}),
    "UPCE":       ("upce",            "UPC_E",              {}),
    "DATABAR":    ("databaromni",     "DATA_BAR",           {}),
    "DATABAREXP": ("databarexpanded", "DATA_BAR_EXPANDED",  {}),
}

ALL_SYMBOLOGIES = (["QR", "DATAMATRIX", "PDF417"] + sorted(_LINEAR) + sorted(_BWIPP))


def _payload_for(kind, rng):
    """심볼로지별 유효 페이로드와 **디코더가 돌려줄 문자열**(정답)을 만든다.

    체크디짓 규칙이 심볼로지마다 다르고, zxing이 돌려주는 표기도 제각각이라
    (UPC-E는 12자리 UPC-A로 펼쳐서 준다) 여기서 한 번에 맞춘다.
    """
    D = lambda n: "".join(str(int(d)) for d in rng.integers(0, 10, size=n))
    if kind == "EAN13":
        return str(int(rng.integers(1, 10))) + D(11), None      # 선행 0은 UPC-A 축약을 부른다
    if kind == "EAN8":
        return D(7), None
    if kind == "UPCA":
        return D(11), None
    if kind == "ITF":
        return D(max(2, int(rng.integers(2, 8)) * 2)), None
    if kind == "CODABAR":
        # python-barcode는 시작/정지 문자를 포함해서 넣어야 한다.
        body = D(int(rng.integers(4, 10)))
        return "A" + body + "B", None
    if kind == "CODE39":
        return "".join(rng.choice(list("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"),
                                  size=int(rng.integers(6, 14)))), None
    if kind == "CODE93":
        t = "".join(rng.choice(list("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"),
                               size=int(rng.integers(6, 12))))
        return t, t
    if kind == "UPCE":
        # BWIPP은 체크디짓 포함 8자리를 받는다. 체크디짓은 UPC-A로 펼친 뒤 계산.
        body = D(6)
        chk = _gtin_check(_upce_expand("0", body))
        return "0" + body + str(chk), None
    if kind == "DATABAR":
        # BWIPP databaromni는 GS1 AI 표기를 받는다: "(01)" + GTIN-14.
        # 앞자리를 0으로 채워야(=GTIN-13 이하) 실제 유통 코드와 형태가 맞는다.
        base = "000" + D(10)
        return "(01)" + base + str(_gtin_check(base)), None
    if kind == "DATABAREXP":
        base = D(13)
        return "(01)" + base + str(_gtin_check(base)), None
    if kind == "PDF417":
        return "VSCAN-" + "".join(rng.choice(list(ALPHA), size=int(rng.integers(6, 24)))), None
    if kind == "DATAMATRIX":
        return "VSCAN-" + "".join(rng.choice(list(ALPHA), size=int(rng.integers(6, 24)))), None
    return "".join(rng.choice(list(ALPHA), size=int(rng.integers(6, 20)))), None


def _gtin_check(digits):
    """GS1 표준 체크디짓 (EAN-8/13, UPC-A, GTIN-14 공통).
    체크 자리를 뺀 데이터에서 오른쪽부터 3,1,3,1... 가중합."""
    t = sum(int(c) * (3 if i % 2 == 0 else 1) for i, c in enumerate(reversed(digits)))
    return (10 - t % 10) % 10


def _upce_expand(ns, body):
    """UPC-E 6자리 본문을 UPC-A 11자리(체크 제외)로 펼친다. 마지막 자리 규칙."""
    d = body
    x = d[5]
    if x in "012":
        return ns + d[0] + d[1] + x + "0000" + d[2] + d[3] + d[4]
    if x == "3":
        return ns + d[0] + d[1] + d[2] + "00000" + d[3] + d[4]
    if x == "4":
        return ns + d[0] + d[1] + d[2] + d[3] + "00000" + d[4]
    return ns + d[0] + d[1] + d[2] + d[3] + d[4] + "0000" + x


def _grid_for(kind, payload):
    """심볼로지 -> (모듈 격자, 정답 텍스트, 2D 여부).

    1D는 python-barcode의 비트열(build())을 그대로 쓰고, 나머지는 인코더가
    주는 행렬이나 렌더 이미지를 _grid_from_image()로 정규화한다.
    """
    if kind in _LINEAR:
        cls, symname, wopts = _LINEAR[kind]
        obj = cls(payload, **wopts)
        bits = obj.build()[0]
        grid = np.array([[c == "1" for c in bits]], dtype=bool)
        text = obj.get_fullcode()
        # Codabar는 디코더가 시작/정지 문자를 빼고 준다(실측).
        if kind == "CODABAR" and len(text) > 2 and text[0].isalpha() and text[-1].isalpha():
            text = text[1:-1]
        return grid, symname, text, False
    if kind == "QR":
        q = qrcode.QRCode(border=0, box_size=1)
        q.add_data(payload); q.make(fit=True)
        return np.array(q.get_matrix(), dtype=bool), "QR_CODE", payload, True
    if kind == "DATAMATRIX":
        if DataMatrix is None:
            raise RuntimeError("ppf-datamatrix 미설치")
        m = DataMatrix(payload).matrix
        return np.array(m, dtype=bool), "DATA_MATRIX", payload, True
    if kind == "PDF417":
        if pdf417gen is None:
            raise RuntimeError("pdf417gen 미설치")
        img = pdf417gen.render_image(pdf417gen.encode(payload, columns=6),
                                     scale=3, ratio=3, padding=0)
        return _grid_from_image(img), "PDF417", payload, True
    if kind in _BWIPP:
        if treepoem is None:
            raise RuntimeError("treepoem/ghostscript 미설치")
        btype, symname, bopts = _BWIPP[kind]
        img = treepoem.generate_barcode(barcode_type=btype, data=payload, options=dict(bopts))
        grid = _grid_from_image(img)
        # 디코더가 돌려주는 표기에 맞춘다(실측으로 확인):
        #  - Codabar: 시작/정지 문자(A..D)를 빼고 준다
        #  - DataBar Omnidirectional: AI 표기 없이 GTIN-14만 준다
        #    (반면 DataBar Expanded는 "(01)..." 형태를 그대로 준다)
        text = payload
        if kind == "DATABAR" and text.startswith("(01)"):
            text = text[4:]
        is2d = kind not in ("CODE93", "UPCE", "DATABAR", "DATABAREXP")
        if not is2d:
            # 1D는 모든 행이 같으므로 대표 행 하나로 접는다 — 이후 렌더에서
            # 원하는 높이로 늘린다(늘리기는 손실이 없다).
            grid = grid[grid.shape[0] // 2:grid.shape[0] // 2 + 1]
        return grid, symname, text, is2d
    raise ValueError(kind)


def make_symbol(kind, rng, box_w, box_h, mod_range):
    """(이미지, 심볼로지 이름, 정답 텍스트, 태그리스트, 실제 모듈 픽셀크기).

    모든 심볼로지가 같은 경로를 탄다: 인코더 -> 모듈 격자 -> 목표 모듈
    픽셀 크기로 면적 샘플링 렌더. 그래야 "모듈 px"가 심볼로지를 가로질러
    같은 의미를 갖는다(리포트가 심볼로지를 비교하려면 이게 전제다).
    """
    # 로그 균등: 작은 모듈(어려움) 쪽 표본이 선형 균등보다 충분히 나온다
    mod_px = float(math.exp(rng.uniform(math.log(mod_range[0]), math.log(mod_range[1]))))

    payload, _ = _payload_for(kind, rng)
    for _ in range(4):
        try:
            grid, symname, text, is2d = _grid_for(kind, payload)
        except Exception:
            raise
        gh, gw = grid.shape
        quiet = 2 if is2d else 10          # 1D는 규격상 10모듈 콰이어트존
        if is2d:
            total_w = (gw + 2 * quiet) * mod_px
            total_h = (gh + 2 * quiet) * mod_px
            limit = min(box_w, box_h)
            if max(total_w, total_h) > limit:
                mod_px = max(mod_range[0], mod_px * limit / max(total_w, total_h))
            img = _grid_to_image(grid, mod_px, quiet=quiet)
            if img.width <= box_w and img.height <= box_h:
                break
        else:
            ratio = float(rng.uniform(0.16, 0.45))
            total_w = (gw + 2 * quiet) * mod_px
            if total_w > box_w:
                mod_px = max(mod_range[0], mod_px * box_w / total_w)
            # 막대 높이를 **모듈 수로** 정한 뒤 렌더한다(복제 -> 콰이어트존 순서).
            bar_rows = max(4, int(round(total_w * ratio / max(0.5, mod_px))))
            img = _grid_to_image(grid, mod_px, quiet=quiet, rows=bar_rows, quiet_y=2)
            if img.height > box_h:
                bar_rows = max(4, int(bar_rows * box_h / img.height))
                img = _grid_to_image(grid, mod_px, quiet=quiet, rows=bar_rows, quiet_y=2)
            if img.width <= box_w:
                break
        # 안 들어가면 페이로드를 줄여 다시(길이 고정 심볼은 모듈만 줄어든다)
        if len(payload) > 6 and kind not in ("EAN13", "EAN8", "UPCA", "UPCE", "DATABAR"):
            payload = payload[:max(4, len(payload) * 2 // 3)]
        else:
            break
    if img.width > box_w or img.height > box_h:
        img = img.resize((min(img.width, int(box_w)), min(img.height, int(box_h))), Image.BOX)
    return img, symname, text, [_mod_tag(mod_px), "sym-" + kind], mod_px


# ============================================================ 코드 단위 열화
def _to_dpm(arr, step=5.0):
    """도트 각인(DPM) 흉내 — 모듈을 점으로만 찍는다.

    [step은 모듈 폭이어야 한다 — 2026-08-03 수정]
    실제 도트 각인은 **모듈 하나에 점 하나**를 찍는다. 그런데 여기 step이
    5로 고정돼 있어서, 모듈이 5px보다 작으면 한 모듈에 점이 하나도 안
    들어가고 코드가 **생성 단계에서 파괴됐다.**

    [격자를 코드에 맞춘다 — 2026-08-04 수정]
    step을 모듈 폭으로 고친 뒤에도 DPM 축은 안 열렸다(구제를 켜도
    45/82 그대로). 남아 있던 결함이 둘 더 있었다.

      (1) **위상.** 격자를 픽셀 (0,0)에서 시작했다. 그런데 이 배열에는
          정지대와(반전 케이스라면) 덧댄 여백이 앞에 붙어 있어서, 격자가
          모듈 격자와 어긋난다. 그러면 표본 블록이 두 모듈에 걸쳐 평균이
          나오고, 점이 엉뚱한 자리에 찍힌다.
      (2) **누적 드리프트.** step을 `int(round(mod_px))`로 정수화했다.
          모듈이 3.62px인데 4로 반올림하면 20모듈 코드에서 7.6px, 즉
          **모듈 두 개**가 밀린다. 코드 끝은 통째로 어긋난다.

    둘 다 "실제 각인기가 하지 않는 일"이다. 각인기는 코드의 모듈 격자를
    알고 그 위에 찍는다. 그래서 여기서도 **코드 상자를 먼저 찾아** 격자를
    거기에 맞추고, 피치를 실수로 유지한다(정수화하지 않는다).

    §3.29(원근)·§3.41(반전)·§3.83(step)에 이어 **네 번째로 "축이 생성기
    결함이었다"** 가 나온 자리다.
    """
    dark = arr < 128
    if not dark.any():
        return arr.copy()
    ys, xs = np.where(dark)
    y0, y1 = int(ys.min()), int(ys.max()) + 1
    x0, x1 = int(xs.min()), int(xs.max()) + 1
    step = max(2.0, float(step))
    # 코드 상자를 모듈 수로 나눠 **실제 피치**를 얻는다. 반올림은 여기
    # 한 번뿐이라 누적 드리프트가 없다.
    nx = max(1, int(round((x1 - x0) / step)))
    ny = max(1, int(round((y1 - y0) / step)))
    sx = (x1 - x0) / nx
    sy = (y1 - y0) / ny

    dot = np.full_like(arr, 255)
    # 점 지름은 피치의 약 70%. 고정 3x3이면 모듈이 커질수록 성글어져서
    # 실제 각인과 달라진다.
    rx = max(1, int(round(sx * 0.35)))
    ry = max(1, int(round(sy * 0.35)))
    h, w = arr.shape
    for j in range(ny):
        ty0 = y0 + sy * j
        for i in range(nx):
            tx0 = x0 + sx * i
            a0, a1 = int(round(ty0)), int(round(ty0 + sy))
            b0, b1 = int(round(tx0)), int(round(tx0 + sx))
            if a1 <= a0 or b1 <= b0:
                continue
            if arr[a0:a1, b0:b1].mean() < 128:
                cy = int(round(ty0 + sy / 2.0))
                cx = int(round(tx0 + sx / 2.0))
                dot[max(0, cy - ry):min(h, cy + ry + 1),
                    max(0, cx - rx):min(w, cx + rx + 1)] = 70
    return dot


def degrade_symbol(img, rng, sev, tags, allow_dpm, is_2d, p_deg=1.0, phys=None, mod_px=1.0):
    """코드 이미지 자체에 걸리는 열화(대비/반전/DPM/인쇄불량/손상).

    프레임 전체가 아니라 코드별로 적용한다 — 한 프레임에 '깨끗한 코드 +
    망가진 코드'가 섞이는 상황(부분 검출 함정, §3.2.10)이 자연히 생긴다.
    """
    a = np.array(img).astype(np.float32)
    if phys is None:
        phys = {}

    if rng.random() < 0.28 * p_deg:                                   # 저대비
        ratio = float(np.interp(sev, [0, 1], [0.75, 0.07]))
        a = 128 + (a - 128) * ratio
        phys["contrast"] = ratio
        tags.append("lowcontrast" + ("-strong" if ratio < 0.2 else ""))

    if rng.random() < 0.15 * p_deg:                                   # 흑백 반전
        # 반전 전에 흰 여백을 덧대야 반전 후에 **어두운 콰이어트존**이 남는다.
        # 안 그러면 반전된 코드가 밝은 배경에 바로 붙어서 콰이어트존이
        # 사라지고, "반전"이 아니라 "콰이어트존 파괴" 케이스가 돼버린다.
        # 여백은 모듈 10개 이상 (규격 정지대). 스윕 경로의 같은 자리 주석 참고.
        pad = max(8, int(round(10 * mod_px)), int(min(a.shape) * 0.06))
        a = np.pad(a, pad, mode="constant", constant_values=255)
        a = 255 - a
        tags.append("inverted")

    if allow_dpm and rng.random() < 0.10 * p_deg:                     # DPM 도트 각인
        # 피치는 **실수 그대로** 넘긴다(정수화하면 코드 끝이 밀린다).
        a = _to_dpm(np.clip(a, 0, 255).astype(np.uint8),
                    step=max(3.0, float(mod_px))).astype(np.float32)
        phys["dpm"] = 1
        tags.append("dpm")

    if rng.random() < 0.14 * p_deg:                                   # 인쇄 불량(잉크 끊김)
        h = a.shape[0]
        gap = max(6, int(h * float(np.interp(sev, [0, 1], [0.10, 0.035]))))
        for y in range(0, h, gap):
            a[y:y + max(1, int(h * 0.006)), :] = 235
        tags.append("printdefect")

    if rng.random() < 0.16 * p_deg:                                   # 물리 손상(긁힘/결손)
        im = Image.fromarray(np.clip(a, 0, 255).astype(np.uint8), mode="L")
        d = ImageDraw.Draw(im)
        h, w = a.shape
        for _ in range(int(np.interp(sev, [0, 1], [2, 9]))):
            x0, y0 = int(rng.integers(0, w)), int(rng.integers(0, h))
            d.line([x0, y0, x0 + int(rng.integers(-w // 4, w // 4)),
                    y0 + int(rng.integers(-h // 4, h // 4))],
                   fill=235, width=int(rng.integers(3, 4 + int(6 * sev))))
        # 모서리 결손은 2D에만. 1D는 ECC가 없어서 시작/정지 패턴이 잘리면
        # **무조건** 못 읽는다 — 라이브러리 성능이 아니라 물리를 측정하게 된다.
        if is_2d and rng.random() < 0.4 * sev + 0.1:
            cut = int(min(w, h) * float(np.interp(sev, [0, 1], [0.10, 0.30])))
            d.polygon([(w, h), (w - cut, h), (w, h - cut)], fill=200)
            phys["cut"] = float(np.interp(sev, [0, 1], [0.10, 0.30]))
        a = np.array(im).astype(np.float32)
        phys["damaged"] = 1
        tags.append("damaged")

    return Image.fromarray(np.clip(a, 0, 255).astype(np.uint8), mode="L")


def place_transform(img, rng, sev, tags, cell_min, p_rot=0.45):
    """코드 단위 기하 변환(회전/원근). 회전 후 bbox가 셀을 넘지 않게 미리 축소.

    반환은 (이미지, 각도, **모듈 배율**)이다. 배율을 같이 돌려주는 이유:
    여기서 하는 축소(회전 여유분 1/1.42, 셀 맞춤, 원근 압축)가 전부
    모듈 크기를 줄이는데 지금까지 module_px에 반영되지 않았다. 그래서
    판독 가능성 버킷이 실제보다 후하게 'ok'를 줬다.
    """
    ang = 0.0
    scale = 1.0
    if rng.random() < p_rot:
        # 1D는 회전에 원리적으로 취약하다(§3.2.15) — 작은 기울기와 큰 각도를
        # 둘 다 뽑는다. 90도 근방은 TryRotate 회귀 감시용으로 일부러 자주.
        pick = rng.random()
        if pick < 0.45:
            ang = float(rng.uniform(-18, 18))
        elif pick < 0.70:
            ang = float(rng.choice([90.0, 180.0, 270.0]))
        else:
            ang = float(rng.uniform(0, 360))
        tags.append("rot" + ("-ortho" if ang % 90 == 0 else
                             ("-small" if abs(((ang + 180) % 360) - 180) <= 20 else "-free")))

    if rng.random() < 0.18 * p_rot / 0.45:                     # 원근
        s = float(np.interp(sev, [0, 1], [0.06, 0.34]))
        w, h = img.size
        coeffs = (1, s * 0.9, -w * s * 0.12, s * 0.35, 1, -h * s * 0.10,
                  s * 0.0011, s * 0.00035)
        pw0, ph0 = img.size
        img, pscale = _perspective(img, coeffs)
        scale *= pscale
        if img.width > pw0 or img.height > ph0:
            k = min(pw0 / img.width, ph0 / img.height)
            img = img.resize((max(24, int(img.width * k)),
                              max(24, int(img.height * k))), Image.LANCZOS)
            scale *= k
        tags.append("perspective" + ("-strong" if s > 0.2 else ""))

    if ang:
        if ang % 90 != 0:
            # 회전 bbox 확대분(최대 sqrt(2))만큼 미리 줄여야 셀 안에 들어간다
            k = 1.0 / 1.42
            img = img.resize((max(30, int(img.width * k)), max(20, int(img.height * k))))
            scale *= k
        img = img.rotate(ang, expand=True, fillcolor=255, resample=Image.BICUBIC)
        if max(img.size) > cell_min:
            k = cell_min / max(img.size)
            img = img.resize((max(20, int(img.width * k)), max(20, int(img.height * k))))
            scale *= k
    return img, ang, scale


# ============================================================ 판독 가능성 분류
#
# 왜 이게 필요한가
# ----------------
# 난수로 조건을 뽑으면 **물리적으로 아무도 못 읽는 이미지**가 반드시 섞인다
# (모듈이 1px, 대비가 노이즈보다 작음, 블러 시그마가 모듈보다 큼...).
# 이걸 섞어놓고 "검출률 46%"라고 하면 그 숫자가 우리 라이브러리 성능인지
# 물리 한계인지 알 수 없다 — 개선해도 숫자가 안 움직이니 판단이 불가능하다.
#
# 그래서 생성 레시피(정확한 수치를 우리가 알고 있다)로 코드마다
#   ok         : 읽을 수 있어야 정상  <- **여기 성공률이 진짜 지표**
#   borderline : 리더에 따라 갈리는 경계
#   impossible : 원래 안 되는 게 맞음(난독). 실패해도 감점 아님
# 을 매기고, 파일 모드에서는 **폴더까지 나눠서** 떨어뜨린다.
#
# 매우 중요한 선긋기
# ------------------
# 분류 기준은 **물리(신호가 남아있는가)** 뿐이다. "우리 zxing이 못 읽는
# 조건"은 절대 impossible로 넣지 않는다. 예를 들어 20~30도 회전한 1D는
# 현재 우리가 못 읽지만(§8) 상용 리더는 읽는다 — 이건 ok로 분류돼서
# 실패로 잡혀야 한다. 그게 개선 대상이기 때문이다.
# 라이브러리 한계를 impossible에 넣기 시작하면, 못 읽는 걸 못 읽는다고
# 선언하는 것만으로 성공률이 올라가는 자기기만이 된다.

BUCKETS = ("ok", "borderline", "impossible")


def classify_code(mod_px, cphys, fphys):
    """코드 하나의 판독 가능성. (버킷, 사유태그들) 반환.

    판정은 전부 "신호 대 열화" 비교다. 상수의 근거는 각 항목 주석 참고.
    """
    reasons = []
    worst = 0                                   # 0=ok 1=borderline 2=impossible

    def mark(level, reason):
        nonlocal worst
        if level > worst:
            worst = level
        if level:
            reasons.append(reason)

    m = float(mod_px)
    contrast = float(cphys.get("contrast", 1.0))
    gain = float(fphys.get("gain", 1.0))
    shadow = float(fphys.get("shadow", 1.0))
    noise = float(fphys.get("noise", 3.0))
    blur = float(fphys.get("blur", 0.6))
    motion = float(fphys.get("motion", 0.0))
    glare = float(fphys.get("glare", 0.0))

    # [손상/오염 — 오류정정 용량과 견준다]
    #
    # 이 축은 오래 버킷이 **아예 안 봤다**. 90%가 지워진 QR도 img-ok였고,
    # 그래서 `--bucket ok`가 이 축에서는 "읽혀야 정상"을 보증하지 않았다
    # (생성기 주석 [[vscan-lite-sweep-damage-uncalibrated]]에 그렇게
    # 적혀 있었는데, 보고서를 쓰면서 그 경고를 어기고 F격자 절대값을
    # 성능처럼 실었다 — QR 14.7%가 그것이다).
    #
    # 파괴 비율은 모델링하지 않고 **생성기가 그리기 전후를 빼서 잰 값**을
    # 받는다(cphys["dmg_frac"]). 남은 것은 용량과 견주는 일뿐이다.
    #
    # 용량 근거(규격): QR은 L 7% / M 15% / Q 25% / H 30%,
    # DataMatrix ECC200은 크기에 따라 25~30%라 0.28로 둔다.
    # PDF417은 보안수준에 따라 다르고 생성기가 기본값을 쓰므로 0.18로 둔다.
    #
    # 실측이 이 값을 뒷받침한다 — 같은 손상 프레임에서 QR의 오류정정만
    # 바꾸면 EC L 4/11, EC H 6/11이고, 오염 축은 L 4/11 -> H 10/11이다.
    # 라벨이 EC를 모르면 이 차이가 통째로 "검출 실패"로 잡힌다.
    dmg_frac = float(cphys.get("dmg_frac", 0.0))
    if dmg_frac > 0:
        if bool(cphys.get("ecc2d", False)):
            # [규격 용량을 ok 문턱으로 쓰면 안 된다 — 실측으로 확인]
            #
            # 처음에는 규격 오류정정 용량을 그대로 문턱으로 썼다. 재보니
            # 전부 그보다 훨씬 먼저 끊긴다(모듈 6px, 긁힘 축):
            #
            #   심볼로지      마지막으로 읽힌 파괴율   규격 용량
            #   DataMatrix          5.8%               28%
            #   QR                  7.1%               15%
            #   PDF417              8.9%               18%
            #
            # 이유는 이 축의 긁힘이 **코드 전체를 가로지르는 대각선**이라
            # 데이터가 아니라 구조 패턴(타이밍·정렬·파인더)을 부수기
            # 때문이다. 오류정정은 데이터 오류를 살리는 것이고 구조가
            # 깨지면 애초에 격자를 못 세운다. 즉 규격 용량은 **이 열화에
            # 대한 척도가 아니다.**
            #
            # 그래서 세 구간으로 나눈다:
            #   - 0.05 이하        ok        (셋 다 실측으로 읽힌 구간)
            #   - 0.05 ~ 규격용량  borderline (읽힐 수도 있다)
            #   - 규격용량 초과    impossible (어떤 리더도 못 읽는다)
            #
            # borderline을 넓게 두는 것이 핵심이다. 실측 절단점에 딱 맞추면
            # `ok`가 정의상 100%가 되어 이 축이 아무것도 못 재게 된다 —
            # 우리 디코더로 라벨을 만들고 그 라벨로 우리 디코더를 채점하는
            # 순환이다. 불확실한 구간은 **불확실하다고 적는 것**이 맞다.
            cap = {"L": 0.07, "M": 0.15, "Q": 0.25, "H": 0.30}.get(
                str(cphys.get("ec", "M")), 0.15)
            if str(cphys.get("sym", "")) == "DATA_MATRIX":
                cap = 0.28
            elif str(cphys.get("sym", "")) == "PDF417":
                cap = 0.18
            if dmg_frac > cap:
                mark(2, "x-damage")
            elif dmg_frac > 0.05:
                mark(1, "b-damage")
        else:
            # 1D는 오류정정이 없는 대신 세로 여유가 있다 — 스캔 라인 한 줄만
            # 성하면 읽힌다. 그래서 면적이 아니라 **깨끗한 행의 비율**로 본다.
            clean = float(cphys.get("clean_rows", 1.0))
            if clean < 0.02:
                mark(2, "x-damage")
            elif clean < 0.15:
                mark(1, "b-damage")

    # 1) 모듈 크기 — 샘플링 한계. 모듈당 1픽셀 미만이면 정보가 사라진다.
    #    2D 코드는 실무적으로 모듈당 2px는 있어야 안정적으로 읽힌다.
    if m < 1.3:
        mark(2, "x-module")
    elif m < 2.0:
        mark(1, "b-module")

    # 2) 대비 대 노이즈 — 코드 진폭은 127*contrast, 여기에 노출/그림자가 곱해진다.
    #    중요: 디코더는 모듈 하나를 m x m 픽셀로 보므로 노이즈가 그만큼
    #    평균화된다(실효 시그마 ~ sigma/m). 이걸 빼먹으면 "큰 모듈 + 저대비"를
    #    난독으로 잘못 분류한다 — 실제로는 잘 읽힌다.
    amp = 127.0 * contrast * gain * shadow
    noise_eff = noise / max(1.0, 0.8 * m)
    if amp < 1.0 * noise_eff:
        mark(2, "x-contrast")
    elif amp < 2.5 * noise_eff:
        mark(1, "b-contrast")

    # 3) 노출 클리핑 — 밝은 쪽/어두운 쪽이 같은 값으로 뭉개지면 코드가 사라진다.
    dark = (128.0 - 127.0 * contrast) * gain * shadow
    bright = (128.0 + 127.0 * contrast) * gain * shadow + glare
    if dark > 248.0 or bright < 8.0 or (bright - dark) < 6.0:
        mark(2, "x-exposure")
    elif dark > 235.0 or bright < 18.0:
        mark(1, "b-exposure")

    # 4) 블러 — 가우시안 시그마가 모듈 크기에 육박하면 인접 모듈이 섞인다.
    #    (실측 보정: sigma > 0.75*module 이면 검출률 0%, > 0.45*module 부터 흔들린다)
    if blur > 0.75 * m:
        mark(2, "x-blur")
    elif blur > 0.45 * m:
        mark(1, "b-blur")

    # 5) 모션 블러 — 이동 길이가 모듈 크기를 크게 넘으면 막대 구분이 사라진다.
    #    다만 블러 **방향**이 막대와 나란하면 거의 손해가 없다(1D는 특히).
    #    [방향을 이제는 안다] 예전에는 "방향을 모른 채 판정하므로" 임계를
    #    넉넉히 잡았다(5.0*m 난독 / 2.2*m 경계). 그때 2.5*m을 난독으로
    #    잡았더니 17%가 읽혀서 부적합했는데, 그건 블러가 막대와 나란한
    #    경우가 섞여 있었기 때문이다. 생성기는 모션을 **항상 x축으로**
    #    걸고 코드 회전각도 알고 있으므로, 막대에 수직인 성분만 골라낼 수
    #    있다: 1D는 |L*cos(회전각)|, 2D는 방향과 무관하게 L 전체.
    #
    #    수직 성분에 대한 한계는 재서 정했다. 코드 중앙행의 흑백 런 개수가
    #    L을 키우면서 어디서 무너지는지 본 것이다(14종, module 3~8):
    #      Code128 1.72배 / Code39 2.00 / PDF417 1.80 / EAN13 2.00 /
    #      EAN8 2.00 / UPC-A 2.00 / QR 2.00
    #    즉 **수직 성분이 모듈의 2배**에서 막대 구분이 사라진다. 그 아래
    #    1.5배부터는 진폭이 크게 깎이므로 경계로 둔다.
    rot = float(cphys.get("rot", 0.0))
    is2d = bool(cphys.get("is2d", False))
    motion_eff = motion if is2d else abs(motion * math.cos(math.radians(rot)))
    if motion_eff > 2.0 * m:
        mark(2, "x-motion")
    elif motion_eff > 1.5 * m:
        mark(1, "b-motion")

    # 6) 물리적 결손 — 2D 모서리 결손이 30%에 가까우면 ECC 한계를 넘는다.
    #    (QR ECC-H가 복원 가능한 최대가 약 30%, 실무 안전선은 25%)
    if float(cphys.get("cut", 0.0)) > 0.26:
        mark(2, "x-damage")

    # 주의: 회전/DPM/콰이어트존 침범/클러터는 여기서 절대 impossible이 아니다.
    # 물리적으로 정보는 남아있고, 상용 리더가 읽는 조건이다 — 우리가 못 읽으면
    # 그건 개선 대상(§8)이지 난독이 아니다.
    return BUCKETS[worst], reasons


def image_bucket(code_buckets):
    """이미지 단위 폴더 분류. 코드가 섞여 있으면 mixed."""
    if not code_buckets:
        return "empty"
    if all(b == "impossible" for b in code_buckets):
        return "impossible"
    if all(b == "ok" for b in code_buckets):
        return "ok"
    if any(b == "impossible" for b in code_buckets):
        return "mixed"
    return "borderline"


# ============================================================ 프레임 배경
def background(rng, w, h, style, tags):
    """배경. 'plain'은 조명 그라디언트 + 센서 노이즈, 나머지는 현장형 클러터.

    클러터(텍스트/표/체커보드/줄무늬)는 가짜 후보를 만들어 full 경로를
    40~60% 느리게 한다(§3.4) — 속도 측정에 반드시 섞여 있어야 하는 축이다.
    """
    base = float(rng.integers(120, 230))
    xs = np.arange(w, dtype=np.float32)[None, :] / w
    ys = np.arange(h, dtype=np.float32)[:, None] / h
    a = base + 25 * (1 - xs * 0.6 - ys * 0.3) + rng.normal(0, 3, size=(h, w))
    img = Image.fromarray(np.clip(a, 0, 255).astype(np.uint8), mode="L")
    if style == "plain":
        return img
    d = ImageDraw.Draw(img)
    sc = w / 2048.0
    if style == "label":                                   # 물류 라벨(텍스트/표)
        d.rectangle([int(60 * sc), int(60 * sc), w - int(60 * sc), h - int(60 * sc)],
                    outline=10, width=max(2, int(8 * sc)))
        for i in range(int(rng.integers(6, 14))):
            y = int(rng.integers(0, h))
            d.line([0, y, w, y], fill=int(rng.integers(10, 40)), width=max(1, int(4 * sc)))
        for _ in range(int(rng.integers(10, 26))):
            d.text((int(rng.integers(0, w * 0.8)), int(rng.integers(0, h * 0.95))),
                   f"LOT {int(rng.integers(1000, 9999))}  PART-{int(rng.integers(100, 999))}",
                   font=_FNT, fill=int(rng.integers(15, 60)))
    elif style == "warehouse":                             # 박스 모서리/테이프 조각
        for _ in range(int(rng.integers(8, 20))):
            x0, y0 = int(rng.integers(0, w)), int(rng.integers(0, h))
            d.rectangle([x0, y0, x0 + int(rng.integers(120, 600) * sc),
                         y0 + int(rng.integers(100, 450) * sc)],
                        outline=int(rng.integers(40, 110)), width=int(rng.integers(3, 9)))
        for _ in range(int(rng.integers(3, 9))):
            x0, y0 = int(rng.integers(0, w)), int(rng.integers(0, h))
            d.rectangle([x0, y0, x0 + int(rng.integers(200, 600) * sc), y0 + int(22 * sc)],
                        fill=235)
    elif style == "falsepattern":                          # 파인더/1D 미끼
        for _ in range(int(rng.integers(1, 4))):
            bx, by = int(rng.integers(0, w * 0.8)), int(rng.integers(0, h * 0.8))
            cs = int(rng.integers(14, 30) * sc) or 1
            for i in range(8):
                for j in range(8):
                    if (i + j) % 2 == 0:
                        d.rectangle([bx + i * cs, by + j * cs,
                                     bx + i * cs + cs - 1, by + j * cs + cs - 1], fill=20)
        for _ in range(int(rng.integers(1, 4))):
            x, sy = int(rng.integers(0, w * 0.8)), int(rng.integers(0, h * 0.85))
            for _ in range(30):
                bw = int(rng.integers(3, 12))
                d.rectangle([x, sy, x + bw, sy + int(130 * sc)], fill=15)
                x += bw + int(rng.integers(3, 10))
    tags.append("clutter-" + style)
    return img


# ============================================================ 센서 노이즈 바닥
def sensor_noise_floor(arr, rng, extra_sigma=0.0):
    """실기 센서의 **항상 있는** 노이즈를 얹는다(읽기 + 샷).

    왜 필요한가. 이 생성기는 코드를 **순수 0/255**로 그리고, 노이즈는 그
    축이 뽑혔을 때만 넣었다. 실기 센서는 밝은 데서도 읽기 노이즈와 샷
    노이즈가 항상 있다 — 그 한 가지 차이로 전처리 결론이 실제로 뒤집혔다
    (§3.101: "극단 화소만 남기고 번져 채우기"가 합성에서는 실패 프레임을
    열었는데 센서 물리를 모델링한 저조도 코퍼스에서는 30 -> 0코드로
    전멸했다).

    [샷 계수는 저조도 모델의 것을 그대로 쓰면 안 된다]
    처음에 `generate_lowlight.py`의 계수(0.55)를 그대로 썼다가 되돌렸다.
    그 값은 **AGC 이전의 어두운 신호**에 대한 것이라, 이미 잘 노출된
    프레임에 걸면 게인을 두 번 세는 셈이다. 실제로 그 값이면 밝은 영역
    시그마가 8.4가 되고, 게이트의 ITF 각도 스윕이 검출 94.7% / 오디코딩 1로
    떨어졌다 — 물리가 아니라 과장이다.

    잘 노출된 프레임의 물리로 다시 잡는다. 풀웰 1만 e-가 255 DN에
    대응한다고 보면 DN 220에서 샷 시그마는 sqrt(8600) e- = 93 e- =
    **2.4 DN**이다. 계수 0.16이 그 값을 준다(sqrt(220) x 0.16 = 2.4).
    읽기 노이즈 2.2와 합치면 밝은 영역 총 시그마 약 3.3 DN이다.

    extra_sigma: 노이즈 축이 따로 잡혀 있으면 그 값을 제곱합으로 더한다.
    [[vscan-lite-sensor-noise-floor]]
    """
    read = 2.2
    shot = np.sqrt(np.maximum(arr, 0.0)) * 0.16
    sigma = np.sqrt(read ** 2 + shot ** 2 + float(extra_sigma) ** 2)
    return arr + rng.normal(0, 1, size=arr.shape) * sigma


# ============================================================ 프레임 열화
def _motion_blur(a, length, angle_deg):
    rad = math.radians(angle_deg)
    dx, dy = math.cos(rad), math.sin(rad)
    acc = np.zeros_like(a)
    half = max(1, length // 2)
    for t in range(-half, half + 1):
        acc += np.roll(np.roll(a, int(round(dy * t)), axis=0), int(round(dx * t)), axis=1)
    return acc / (2 * half + 1)



def _perspective(img, coeffs):
    """원근 변환 — **캔버스를 넓혀서** 적용한다 (rotate(expand=True)와 같은 취지).

    PIL의 transform(size, PERSPECTIVE, ...)은 출력 크기를 그대로 두므로,
    전단으로 밀려난 부분이 캔버스 밖으로 잘려 나간다. 바코드에서 그건
    "왜곡"이 아니라 **코드 일부가 없어지는 것**이라 어떤 리더로도 못 읽는다.

    실측(Code128 module 8, 코드 중앙행의 흑백 런 개수 — 정상은 103개):
        persp 0     런 103   코드 오른쪽 끝 x=951 (캔버스 여유 있음)
        persp 0.1   런  97   오른쪽 끝 x=997  <- 캔버스 끝(999)에 붙었다
        persp 0.3   런  81   오른쪽 끝 x=997
        persp 0.6   런  65   오른쪽 끝 x=997
    즉 생성 단계에서 이미 코드가 잘려 있었고, 버킷 분류는 그걸 'ok'로
    표시하고 있었다 — 리더의 약점이 아니라 테스트 하네스의 결함이었다.
    (3배 슈퍼샘플링도 해봤지만 런 개수가 한 개도 안 변했다. 리샘플 품질
    문제가 아니라 잘림 문제라는 확인이다.)

    coeffs는 PIL 규약대로 **목적지 -> 원본** 사상이다. 그 역행렬로 원본
    네 모서리가 어디로 가는지 구해 출력 상자를 잡고, 그만큼 평행이동을
    합성해서 전부 담는다.
    """
    w, h = img.size
    M = np.array([[coeffs[0], coeffs[1], coeffs[2]],
                  [coeffs[3], coeffs[4], coeffs[5]],
                  [coeffs[6], coeffs[7], 1.0]], dtype=np.float64)
    F = np.linalg.inv(M)                      # 원본 -> 목적지
    pts = np.array([[0, 0, 1], [w, 0, 1], [w, h, 1], [0, h, 1]], dtype=np.float64).T
    q = F @ pts
    q = q[:2] / q[2]
    x0, y0 = q[0].min(), q[1].min()
    x1, y1 = q[0].max(), q[1].max()
    nw = x1 - x0
    nh = y1 - y0
    # [상한을 자르지 말고 줄일 것] 강한 왜곡에서는 결과가 폭발한다 —
    # 실측: persp 0.6에서 자연 크기 4051x1150, 0.7에서 8066x2271,
    # 0.9에서는 소실선이 이미지를 지나 5810460x117384가 된다. 예전에는
    # 여기서 크기를 min()으로 잘랐는데, 그러면 캔버스 확장이 무의미해지고
    # persp 0.6 이상이 다시 잘려 나갔다(EAN13 중앙행 런 59 -> 13).
    # 자르는 대신 균일 축소를 합성해서 **내용을 전부 담는다**. 코드가
    # 작아지는 것은 아래 pscale에 반영되므로 판독 가능성 버킷이 알아서
    # 걸러낸다.
    maxW, maxH = w * 4, h * 4
    fit = min(1.0, maxW / max(1.0, nw), maxH / max(1.0, nh))
    nw = max(8, int(math.ceil(nw * fit)))
    nh = max(8, int(math.ceil(nh * fit)))
    # 새 목적지 좌표 -> (축소 해제) -> 평행이동 -> 원본
    S = np.array([[1 / fit, 0, 0], [0, 1 / fit, 0], [0, 0, 1]], dtype=np.float64)
    T = np.array([[1, 0, x0], [0, 1, y0], [0, 0, 1]], dtype=np.float64)
    M2 = M @ T @ S                            # 새 목적지 -> 원본
    M2 = M2 / M2[2, 2]
    out = img.transform((nw, nh), Image.PERSPECTIVE, tuple(M2.ravel()[:8]),
                        resample=Image.BICUBIC, fillcolor=255)

    # [모듈이 얼마나 눌리는가] 원근은 코드 **안에서** 배율을 바꾼다. 한쪽은
    # 늘어나고 반대쪽은 눌리는데, 눌리는 쪽 모듈이 1px 아래로 내려가면
    # 그 부분의 바는 실제로 사라진다 — 실측(EAN13 persp 0.7): 중앙행의
    # 흑백 런이 59개여야 하는데 9개만 남았다. module_px가 스칼라 하나라
    # 그걸 표현하지 못해서 판독 가능성 버킷이 계속 'ok'라고 했다.
    #
    # 바 폭은 "면적 배율 / 높이 배율"이다(평행사변형이므로). 원본 격자에서
    # 표본을 떠서 그 최솟값을 돌려준다.
    def local_scale(px, py):
        def fwd(x, y):
            v = F @ np.array([x, y, 1.0])
            return v[:2] / v[2]
        p0 = fwd(px, py)
        dx = fwd(px + 1.0, py) - p0
        dy = fwd(px, py + 1.0) - p0
        area = abs(dx[0] * dy[1] - dx[1] * dy[0])
        hgt = math.hypot(dy[0], dy[1])
        return area / hgt if hgt > 1e-9 else 0.0

    smin = min(local_scale(x, y)
               for x in (0.0, w * 0.5, float(w))
               for y in (0.0, h * 0.5, float(h)))
    return out, max(1e-4, smin * fit)


def _cylinder_warp(a, strength):
    """원통 라벨의 수평 압축. 행 루프 없이 전부 벡터화 (대량 생성용)."""
    h, w = a.shape
    xs = np.arange(w, dtype=np.float32)
    cx = w / 2.0
    norm = (xs - cx) / cx
    warped = np.clip(cx + np.sin(norm * (np.pi / 2)) * cx * (1 - strength) + norm * cx * strength,
                     0, w - 1)
    x0 = np.floor(warped).astype(np.int32)
    x1 = np.clip(x0 + 1, 0, w - 1)
    frac = (warped - x0)[None, :]
    return a[:, x0] * (1 - frac) + a[:, x1] * frac


def frame_degrade(img, rng, sev, tags, n_degrade, w, h, boxes, phys=None):
    """프레임 전체에 걸리는 열화를 n_degrade개 골라 적용. 반환은 float32 배열.

    boxes = 배치된 코드들의 (x, y, w, h). 국소 열화(반사광/오염/콰이어트존
    침범)는 **코드 위에** 걸려야 의미가 있다 — 빈 배경에 뿌리면 난이도가
    올라가지 않아서 측정값이 그냥 희석된다.
    """
    if phys is None:
        phys = {}
    pool = ["defocus", "motion", "noise", "overexp", "underexp",
            "glare", "shadow", "dirty", "curved", "quietzone"]
    picks = list(rng.choice(pool, size=min(n_degrade, len(pool)), replace=False)) if n_degrade else []

    def near_code():
        """코드 하나를 골라 그 중심 근처 좌표를 돌려준다(코드가 없으면 아무 곳)."""
        if not boxes:
            return float(rng.uniform(0, w)), float(rng.uniform(0, h))
        bx, by, bw, bh = boxes[int(rng.integers(0, len(boxes)))][:4]
        return (bx + bw * float(rng.uniform(0.2, 0.8)), by + bh * float(rng.uniform(0.2, 0.8)))

    if "quietzone" in picks:      # 코드에 밀착한 테두리/텍스트 (드로잉이므로 먼저)
        d = ImageDraw.Draw(img)
        for bx, by, bw, bh, bmod, b2d in (boxes or []):
            if rng.random() < 0.7:
                # [여백은 모듈 단위 — 픽셀 고정은 정지대 파괴가 된다]
                # 스윕 경로의 같은 자리 주석 참고. 픽셀 2~10px는 모듈 8px
                # 코드에서 0.25~1.25모듈이라, 규격 정지대(1D 10모듈 / 2D
                # 2모듈)를 통째로 깨고 있었다. 그러면 이 태그가 재는 것이
                # "잡동사니를 견디나"가 아니라 "정지대를 부수면 못 읽는다"라는
                # 당연한 물리가 된다. [[vscan-lite-sweep-quietzone-modules]]
                mods = float(rng.uniform(2.0, 6.0)) if b2d else float(rng.uniform(10.0, 14.0))
                pad = max(2, int(round(float(bmod) * mods)))
                d.rectangle([bx - pad, by - pad, bx + bw + pad, by + bh + pad],
                            outline=20, width=max(2, int(round(float(bmod) * 0.8))))
                d.text((bx, max(0, by - pad - 40)),
                       f"LOT {int(rng.integers(10000, 99999))} / GTIN 008123456",
                       font=_FNT, fill=15)
        tags.append("quietzone")

    if "dirty" in picks:
        d = ImageDraw.Draw(img)
        for _ in range(int(np.interp(sev, [0, 1], [30, 220]))):
            x, y = near_code() if rng.random() < 0.75 else (rng.uniform(0, w), rng.uniform(0, h))
            r = int(rng.integers(3, 4 + int(14 * sev)))
            d.ellipse([x, y, x + r, y + r], fill=int(rng.integers(50, 215)))
        tags.append("dirty")

    if "defocus" in picks:
        r = float(np.interp(sev, [0, 1], [1.0, 6.5]))
        img = img.filter(ImageFilter.GaussianBlur(r))
        phys["blur"] = r
        tags.append("defocus" + ("-strong" if r > 3.8 else ""))
        a = np.array(img).astype(np.float32)
    else:
        a = np.array(img.filter(ImageFilter.GaussianBlur(0.6))).astype(np.float32)  # 렌즈 MTF

    if "motion" in picks:
        length = int(np.interp(sev, [0, 1], [7, 27]))
        a = _motion_blur(a, length, float(rng.uniform(0, 180)))
        phys["motion"] = float(length)
        tags.append("motion" + ("-strong" if length > 17 else ""))

    if "curved" in picks:
        s = float(np.interp(sev, [0, 1], [0.15, 0.5]))
        a = _cylinder_warp(a, s)
        phys["curve"] = s
        tags.append("curved")

    if "glare" in picks:
        yy = np.arange(h, dtype=np.float32)[:, None]
        xx = np.arange(w, dtype=np.float32)[None, :]
        for _ in range(int(rng.integers(1, 3))):
            gx, gy = near_code()                    # 반사광은 코드 위에 걸려야 의미가 있다
            sx, sy = float(rng.uniform(90, 260)), float(rng.uniform(60, 180))
            amp = float(np.interp(sev, [0, 1], [90, 215]))
            a = a + amp * np.exp(-(((xx - gx) ** 2) / (2 * sx ** 2) +
                                   ((yy - gy) ** 2) / (2 * sy ** 2)))
            phys["glare"] = max(phys.get("glare", 0.0), amp)
        tags.append("glare")

    if "shadow" in picks:
        band = np.ones((h, w), dtype=np.float32)
        x0 = int(rng.integers(0, w * 0.7))
        band[:, x0:x0 + int(rng.integers(300, 900))] = float(np.interp(sev, [0, 1], [0.62, 0.28]))
        band = np.array(Image.fromarray((band * 255).astype(np.uint8), mode="L")
                        .filter(ImageFilter.GaussianBlur(12))).astype(np.float32) / 255.0
        a = a * band
        phys["shadow"] = float(np.interp(sev, [0, 1], [0.62, 0.28]))
        tags.append("shadow")

    if "overexp" in picks:
        g = float(np.interp(sev, [0, 1], [1.15, 1.7]))
        a = a * g
        phys["gain"] = g
        tags.append("overexposed")
    elif "underexp" in picks:
        g = float(np.interp(sev, [0, 1], [0.55, 0.15]))
        a = a * g + rng.normal(0, 4, size=(h, w))
        phys["gain"] = g
        tags.append("underexposed" + ("-strong" if g < 0.25 else ""))

    extra = 0.0
    if "noise" in picks:
        extra = float(np.interp(sev, [0, 1], [12, 58]))
        phys["noise"] = extra
        tags.append("noise" + ("-strong" if extra > 35 else ""))
    # 센서 노이즈는 **항상** 있다([[vscan-lite-sensor-noise-floor]]).
    # 축이 잡혔으면 그 값을 제곱합으로 함께 넣는다.
    a = sensor_noise_floor(a, rng, extra)

    return a


# ============================================================ 한 장 생성
def sample_layout(rng, w, h, k):
    """k개 코드를 겹치지 않게 놓을 셀 목록. (셀 x0,y0,셀폭,셀높이)"""
    cols = int(min(4, max(1, math.ceil(math.sqrt(k * w / h)))))
    rows = int(max(1, math.ceil(k / cols)))
    cw, ch = w // cols, h // rows
    cells = [(c * cw, r * ch, cw, ch) for r in range(rows) for c in range(cols)]
    idx = rng.choice(len(cells), size=min(k, len(cells)), replace=False)
    return [cells[int(i)] for i in idx]


def build_one(index, cfg):
    """인덱스 하나에 대응하는 이미지를 만들고 (파일명, 레코드)를 돌려준다.

    씨드는 (마스터 씨드, 인덱스)에서 파생 — 생성 순서/병렬도와 무관하게
    같은 인덱스는 항상 같은 이미지가 된다.
    """
    rng = np.random.default_rng([cfg["seed"], index])
    w, h = cfg["width"], cfg["height"]
    prof = DIFFICULTY[cfg["difficulty"]]
    sev = float(rng.uniform(*prof["sev"]))
    tags = [cfg["difficulty"]]

    # 코드 개수: 1개가 가장 흔하고, 다중도 충분히 섞는다
    r = rng.random()
    k = 1 if r < 0.40 else (int(rng.integers(2, 4)) if r < 0.70
                            else (int(rng.integers(4, 7)) if r < 0.90
                                  else int(rng.integers(7, cfg["max_codes"] + 1))))
    k = max(1, min(k, cfg["max_codes"]))

    style = str(rng.choice(["plain", "label", "warehouse", "falsepattern"],
                           p=[0.55, 0.15, 0.15, 0.15]))
    img = background(rng, w, h, style, tags)

    kinds = cfg["symbologies"]
    codes = []
    for (cx0, cy0, cw, ch) in sample_layout(rng, w, h, k):
        margin = int(min(cw, ch) * 0.08) + 8
        avail_w, avail_h = cw - 2 * margin, ch - 2 * margin
        if avail_w < 60 or avail_h < 60:
            continue
        kind = str(rng.choice(kinds))
        try:
            sym, symname, text, ctags, mod_px = make_symbol(kind, rng, avail_w, avail_h,
                                                            prof["mod"])
        except Exception:
            continue
        cphys = {}
        sym = degrade_symbol(sym, rng, sev, ctags, allow_dpm=(kind == "QR"),
                             is_2d=(kind == "QR"), p_deg=prof["p_deg"], phys=cphys,
                             mod_px=mod_px)
        sym, ang, gscale = place_transform(sym, rng, sev, ctags, min(avail_w, avail_h),
                                           p_rot=prof["p_rot"])
        mod_px *= gscale
        px = cx0 + margin + int(rng.integers(0, max(1, avail_w - sym.width + 1)))
        py = cy0 + margin + int(rng.integers(0, max(1, avail_h - sym.height + 1)))
        img.paste(sym, (px, py))
        tags.extend(ctags)
        codes.append({"symbology": symname, "text": text, "x": px, "y": py,
                      "w": sym.width, "h": sym.height, "rot": round(ang, 1),
                      "tags": ctags, "module_px": round(mod_px, 2), "phys": cphys})

    nd = int(rng.integers(prof["n_degrade"][0], prof["n_degrade"][1] + 1))
    # 정지대 침범을 모듈 단위로 걸려면 코드마다 모듈 크기가 필요하다
    # ([[vscan-lite-sweep-quietzone-modules]]).
    boxes = [(c["x"], c["y"], c["w"], c["h"], c.get("module_px", 8.0),
              c["symbology"] in ("QR_CODE", "DATA_MATRIX", "PDF417")) for c in codes]
    fphys = {}
    a = frame_degrade(img, rng, sev, tags, nd, w, h, boxes, phys=fphys)

    # 판독 가능성 분류 — 프레임 열화까지 정해진 뒤에야 판정할 수 있다
    for c in codes:
        c["phys"]["rot"] = float(c.get("rot", 0.0))
        c["phys"]["is2d"] = c["symbology"] in ("QR_CODE", "DATA_MATRIX")
        # 손상 판정은 **오류정정이 있느냐**로 갈리므로 PDF417도 2D 쪽이다
        # (is2d는 다른 물리 판정에 쓰이는 값이라 그대로 둔다).
        c["phys"]["sym"] = c["symbology"]
        c["phys"]["ecc2d"] = c["symbology"] in ("QR_CODE", "DATA_MATRIX", "PDF417")
        b, reasons = classify_code(c["module_px"], c["phys"], fphys)
        c["bucket"] = b
        c["tags"] = c["tags"] + ["dec-" + b] + reasons
    bucket = image_bucket([c["bucket"] for c in codes])

    # 태그 중복 제거(순서 유지) + 심볼로지 태그
    seen, utags = set(), []
    for t in tags + sorted({"sym-" + c["symbology"] for c in codes}) \
             + [f"n{len(codes)}", "img-" + bucket]:
        if t not in seen:
            seen.add(t); utags.append(t)

    name = f"c{index:06d}_{len(codes)}"
    rec = {"file": name + ("." + cfg["format"]), "index": index, "expected": len(codes),
           "width": w, "height": h, "severity": round(sev, 3), "bucket": bucket,
           "difficulty": cfg["difficulty"], "tags": utags, "codes": codes,
           "frame_phys": {k2: round(v, 3) for k2, v in fphys.items()}}
    return name, np.clip(a, 0, 255).astype(np.uint8), rec


# ============================================================ 스윕(격자) 생성
# 난수 조합은 "전반적으로 어떤가"를 보지만, **어디서 끊기는지**는 못 짚는다.
# 1도 간격 360장, 대비 0.02 간격 50장처럼 한 축만 촘촘히 밀면 임계점이
# 그래프로 보인다 (예: "38도까지 되고 39도부터 안 됨").
#
# 규칙: 스윕은 **결정적**이다. 스윕 축 외의 모든 값은 baseline으로 고정되고
# 난수 열화가 전혀 안 들어간다. 안 그러면 축 하나를 움직였을 때의 차이가
# 난수 잡음에 묻힌다.
SWEEP_BASE = {
    "sym": "QR", "ec": "M", "count": 1, "module": 4.0, "angle": 0.0,
    "contrast": 1.0, "bright": 1.0, "blur": 0.6, "motion": 0.0, "noise": 3.0,
    "persp": 0.0, "curve": 0.0, "glare": 0.0, "shadow": 1.0, "invert": 0.0,
    "dpm": 0.0, "printdefect": 0.0, "damaged": 0.0, "quietzone": 0.0, "dirty": 0.0,
    "clutter": 0.0,
}
SWEEP_HELP = {
    "sym": "심볼로지 (QR/CODE128/EAN13/CODE39/ITF)", "ec": "QR 오류정정 (L/M/Q/H)",
    "count": "코드 개수", "module": "모듈 하나의 픽셀 크기", "angle": "코드 회전각(도)",
    "contrast": "코드 대비 비율 (1.0=원본, 0.05=초저대비)", "bright": "노출 배율",
    "blur": "디포커스 가우시안 시그마(px)", "motion": "모션 블러 길이(px)",
    "noise": "가우시안 노이즈 시그마", "persp": "원근 왜곡 강도",
    "curve": "원통 곡면 강도", "glare": "반사광 세기(0=없음)",
    "shadow": "그림자 밝기 배율 (1.0=없음)",
    "clutter": "배경 잡동사니 0=없음 / 1=물류라벨 / 2=창고 / 3=가짜 파인더+1D 미끼",
    "printdefect": "인쇄 불량(잉크 끊김) 세기 0~1. 가로 줄이 규칙적으로 빠진다",
    "damaged": "물리 손상(긁힘) 세기 0~1. 2D는 모서리 결손도 같이 난다",
    "quietzone": "정지대 침범 세기 0~1. 코드에 밀착한 테두리와 텍스트",
    "dirty": "오염(얼룩) 세기 0~1. 코드 주변에 반점을 뿌린다",
    "dpm": "도트 각인 (0=없음, 1=적용). 모듈 하나에 점 하나를 찍는다 — "
            "격자는 코드 상자에 맞추고 피치는 실수로 유지한다(_to_dpm 주석)",
    "invert": "흑백 반전 (0=없음, 1=반전). 반전 전에 흰 여백을 덧대므로 "
              "반전 후 어두운 정지대가 남는다 — '정지대 파괴'가 아닌 순수 반전 케이스",
}
_SWEEP_PAYLOAD = {
    "QR": "VSCAN-SWEEP-0001", "DATAMATRIX": "VSCAN-SWEEP-01", "PDF417": "VSCAN-SWEEP-01",
    "CODE128": "VSCAN-SWEEP-01", "CODE39": "VSCANSWEEP01", "CODE93": "VSCANSWEEP01",
    "EAN13": "1234567890128", "EAN8": "12345670", "UPCA": "123456789012",
    "UPCE": "01234565", "ITF": "12345670", "CODABAR": "A12345670B",
    "DATABAR": "(01)00012345678905", "DATABAREXP": "(01)00012345678905",
}

_EC = {"L": qrcode.constants.ERROR_CORRECT_L, "M": qrcode.constants.ERROR_CORRECT_M,
       "Q": qrcode.constants.ERROR_CORRECT_Q, "H": qrcode.constants.ERROR_CORRECT_H}


def parse_sweep(spec):
    """'angle:0:359:1' -> ('angle', [0,1,...,359]) / 'sym:QR,CODE128' -> 목록"""
    name, _, rest = spec.partition(":")
    if name not in SWEEP_BASE:
        raise SystemExit(f"알 수 없는 스윕 축: {name}\n"
                         + "\n".join(f"  {k:9} {v}" for k, v in SWEEP_HELP.items()))
    if not rest:
        raise SystemExit(f"스윕 형식: {name}:START:STOP:STEP 또는 {name}:v1,v2,v3")
    if ":" in rest:
        a, b, st = (float(x) for x in rest.split(":"))
        if st <= 0:
            raise SystemExit("STEP은 0보다 커야 합니다")
        n = int(math.floor((b - a) / st + 1e-9)) + 1
        vals = [a + i * st for i in range(max(1, n))]
        if name == "count":
            vals = [int(v) for v in vals]
        return name, vals
    vals = [x.strip() for x in rest.split(",") if x.strip()]
    if name in ("sym", "ec"):
        return name, [v.upper() for v in vals]
    return name, [int(v) if name == "count" else float(v) for v in vals]


def _fmt_val(v):
    return f"{v:g}" if isinstance(v, float) else str(v)


_SWEEP_1D_RATIO = 0.32          # 1D 높이/폭 (스윕 내내 고정)


def sweep_fit_box(sym, cell_w, cell_h):
    """회전각과 무관하게 셀 안에 들어가는 최대 코드 폭.

    w x h 박스를 임의 각도로 돌리면 bbox는 최악(45도)에 0.707*(w+h)다.
    그래서 **정사각(QR)은 /1.41, 납작한 1D는 /0.93** — 1D에 QR 기준을
    쓰면 폭이 2/3로 깎여서 모듈이 1px대로 얇아지고, 각도 축을 재는 게
    아니라 "너무 작아서 못 읽음"을 재게 된다.
    각도와 무관하게 같은 값을 쓰는 게 핵심이다(각도별로 크기가 달라지면
    각도 축 비교가 성립하지 않는다).
    """
    ratio = 1.0 if sym == "QR" else _SWEEP_1D_RATIO
    limit = min(cell_w, cell_h) * 0.92 / (0.7072 * (1.0 + ratio))
    return max(60, int(min(limit, cell_w * 0.95)))


# 캔버스를 넓혀서라도 맞춰 줄 상한. 4096x3072 = 12MP로, 이 저장소가 다루는
# 산업용 카메라(3.1MP 차트, 5MP 급)보다 위다. 더 키우면 프레임 하나가
# 12MB를 넘고 디코딩 시간이 면적에 비례해 늘어 격자가 안 끝난다.
_SWEEP_CANVAS_MAX = (4096, 3072)


def _sweep_canvas_for_module(p, w, h):
    """요청한 module px가 실제로 나오도록 캔버스를 넓힌다.

    [왜 필요한가 — 2026-08-15에 찾은 하네스 결함]

    sweep_symbol()은 코드가 셀에 안 들어가면 **말없이 모듈을 줄인다**
    (`if span * mod > box: mod = box / span`). 그래서 폭이 넓은 1D는
    요청값과 무관하게 같은 그림이 나왔다. 풀테스트에서 CODE39와 CODABAR은
    기준 모듈 4와 8의 CSV가 **바이트 단위로 같았다** — 3.66px에서 둘 다
    포화한 것이다.

    결과가 두 가지로 나빴다:

      (1) 격자의 절반이 중복이다. 두 줄이 서로 다른 크기의 측정인 것처럼
          요약에 실린다.
      (2) **심볼로지 사이의 비교가 크기 교란을 받는다.** 같은 "모듈 8"에서
          EAN13은 8.00px을 받고 CODE39는 3.66px을 받는다. 그 상태로
          검출률을 나란히 놓으면 심볼로지가 아니라 크기를 비교하는 것이다.
          실제로 "CODE128 56% vs CODABAR 100%"를 심볼로지 특성으로 읽을
          뻔했다.

    고치는 방향은 **모듈을 깎지 말고 캔버스를 넓히는 것**이다. 코드와
    프레임의 비율이 그대로라 "모듈당 표본 수"라는 축이 혼자 움직인다.
    센서를 키운 것이 아니라 **같은 라벨을 더 크게 찍은 것**에 해당한다.

    상한(_SWEEP_CANVAS_MAX)에 걸리면 예전처럼 모듈이 깎인다. 그때는
    태그의 modpx가 실제값을 들고 있으므로 분석에서 걸러낼 수 있다 —
    조용히 틀리는 것과 다른 점이 그것이다.
    """
    try:
        mod = float(p["module"])
        span = _sweep_span(p["sym"])
    except Exception:
        return w, h
    if span <= 0 or mod <= 0:
        return w, h
    k = max(1, int(p["count"]))
    cols = int(min(4, max(1, math.ceil(math.sqrt(k * w / h)))))
    rows = int(max(1, math.ceil(k / cols)))
    need_box = span * mod                      # 코드가 차지해야 할 폭(px)
    have_box = sweep_fit_box(p["sym"], w // cols, h // rows)
    if need_box <= have_box:
        return w, h
    grow = need_box / have_box
    mw, mh = _SWEEP_CANVAS_MAX
    grow = min(grow, mw / w, mh / h)
    if grow <= 1.0:
        return w, h
    # 짝수로 맞춘다 — 홀수 폭은 일부 경로에서 반올림이 갈린다.
    return (int(w * grow) // 2) * 2, (int(h * grow) // 2) * 2


def _sweep_span(kind):
    """페이로드가 고정이므로 모듈 수도 고정이다. 캐시해 둔다(인코딩이 비싸다)."""
    if kind in _SWEEP_SPAN_CACHE:
        return _SWEEP_SPAN_CACHE[kind]
    payload = _SWEEP_PAYLOAD.get(kind)
    if payload is None:
        _SWEEP_SPAN_CACHE[kind] = 0
        return 0
    if kind == "QR":
        q = qrcode.QRCode(border=0, box_size=1, error_correction=_EC["M"])
        q.add_data(payload); q.make(fit=True)
        grid, is2d = np.array(q.get_matrix(), dtype=bool), True
    else:
        grid, _, _, is2d = _grid_for(kind, payload)
    quiet = 2 if is2d else 10
    gh, gw = grid.shape
    span = max((gw + 2 * quiet), (gh + 2 * quiet) if is2d else 0)
    _SWEEP_SPAN_CACHE[kind] = span
    return span


_SWEEP_SPAN_CACHE = {}


def sweep_symbol(p, box):
    """스윕용 결정적 심볼 렌더. 페이로드가 고정이라 모듈 수도 고정이고,
    따라서 크기는 module 값에만 비례한다(축 하나만 움직인다는 보장).

    난수 코퍼스와 **같은 렌더 경로**(인코더 -> 모듈 격자 -> 면적 샘플링)를
    쓴다. 그래야 "모듈 px"가 심볼로지를 가로질러 같은 의미를 갖는다.
    """
    kind = p["sym"]
    payload = _SWEEP_PAYLOAD.get(kind)
    if payload is None:
        raise ValueError("스윕 미지원 심볼로지: " + kind)
    if kind == "QR":
        q = qrcode.QRCode(border=0, box_size=1, error_correction=_EC[p["ec"]])
        q.add_data(payload); q.make(fit=True)
        grid, symname, text, is2d = np.array(q.get_matrix(), dtype=bool), "QR_CODE", payload, True
    else:
        grid, symname, text, is2d = _grid_for(kind, payload)

    mod = float(p["module"])
    quiet = 2 if is2d else 10
    gh, gw = grid.shape
    span = max((gw + 2 * quiet), (gh + 2 * quiet) if is2d else 0)
    if span * mod > box:
        mod = box / span
    if is2d:
        img = _grid_to_image(grid, mod, quiet=quiet)
    else:
        bar_rows = max(6, int(round((gw + 2 * quiet) * _SWEEP_1D_RATIO)))
        img = _grid_to_image(grid, mod, quiet=quiet, rows=bar_rows, quiet_y=2)
    if img.width > box:
        k = box / img.width
        mod *= k
        img = img.resize((int(box), max(24, int(img.height * k))), Image.BOX)
    return img, symname, text, mod


def build_sweep(index, combo, cfg):
    """스윕 조합 하나 -> (이름, 이미지배열, 레코드). 난수는 배경 노이즈에만 쓴다."""
    p = dict(SWEEP_BASE)
    p.update(cfg["base"])
    p.update(combo)
    w, h = cfg["width"], cfg["height"]
    if not cfg.get("fixed_canvas"):
        w, h = _sweep_canvas_for_module(p, w, h)
    rng = np.random.default_rng([cfg["seed"], 0xC0FFEE, index])

    xs = np.arange(w, dtype=np.float32)[None, :] / w
    ys = np.arange(h, dtype=np.float32)[:, None] / h
    a = 195 + 25 * (1 - xs * 0.6 - ys * 0.3) + rng.normal(0, 3, size=(h, w))
    img = Image.fromarray(np.clip(a, 0, 255).astype(np.uint8), mode="L")
    # [배경 잡동사니] 난수 경로에만 있던 축을 스윕으로도 낸다. 값은 세기가
    # 아니라 **종류**다(1=물류라벨 / 2=창고 / 3=가짜 파인더+1D 미끼) —
    # 이 열화는 연속량이 아니라 장면 종류이기 때문이다. 코드를 얹기 전에
    # 그려야 잡동사니가 코드를 덮지 않는다.
    _cl = int(round(float(p["clutter"])))
    if _cl > 0:
        img = background(rng, w, h, {1: "label", 2: "warehouse", 3: "falsepattern"}[
            min(3, _cl)], [])

    k = max(1, int(p["count"]))
    cols = int(min(4, max(1, math.ceil(math.sqrt(k * w / h)))))
    rows = int(max(1, math.ceil(k / cols)))
    cw, ch = w // cols, h // rows
    codes, cx0 = [], None
    for i in range(k):
        r, c = divmod(i, cols)
        cell = (c * cw, r * ch, cw, ch)
        box = sweep_fit_box(p["sym"], cw, ch)
        sym, symname, text, eff_mod = sweep_symbol(p, box)
        dmg = {"frac": 0.0, "clean_rows": 1.0}
        s = np.array(sym).astype(np.float32)
        if p["contrast"] != 1.0:
            s = 128 + (s - 128) * float(p["contrast"])
        sym = Image.fromarray(np.clip(s, 0, 255).astype(np.uint8), mode="L")
        if p["persp"]:
            v = float(p["persp"])
            sym, pscale = _perspective(sym, (1, v * 0.9, -sym.width * v * 0.12, v * 0.35, 1,
                                             -sym.height * v * 0.10, v * 0.0011, v * 0.00035))
            eff_mod *= pscale
            # 캔버스를 넓혔으므로 셀을 넘칠 수 있다. 넘치면 줄여서 담는다 —
            # 고정 화각 카메라 앞에서 라벨을 기울이면 실제로 그렇게 된다.
            # 모듈 크기도 같은 비율로 줄어드므로 eff_mod를 함께 환산해야
            # 판독 가능성 버킷이 정직해진다.
            if sym.width > cw or sym.height > ch:
                k = min(cw / sym.width, ch / sym.height)
                sym = sym.resize((max(24, int(sym.width * k)),
                                  max(24, int(sym.height * k))), Image.LANCZOS)
                eff_mod *= k
        if float(p["printdefect"]) > 0:
            # 난수 경로(_degrade_symbol)와 같은 모양: 가로 줄이 규칙적으로
            # 빠진다. 세기가 셀수록 줄 간격이 좁아진다.
            v = float(p["printdefect"])
            a2 = np.array(sym).astype(np.float32)
            hh = a2.shape[0]
            gap = max(6, int(hh * float(np.interp(v, [0, 1], [0.10, 0.035]))))
            for yy2 in range(0, hh, gap):
                a2[yy2:yy2 + max(1, int(hh * 0.006)), :] = 235
            sym = Image.fromarray(np.clip(a2, 0, 255).astype(np.uint8), mode="L")
        if float(p["damaged"]) > 0:
            """[긁힘 굵기는 **모듈 단위**여야 한다]

            처음에는 굵기를 픽셀 고정(3~10px)으로 뒀다. 그러면 같은 세기가
            모듈 크기에 따라 전혀 다른 손상이 된다 — 모듈 3px에 10px 긁힘은
            세 모듈을 통째로 지우고, 모듈 20px에는 흠집도 안 된다. 오류정정
            용량은 **모듈 수** 기준이므로 축도 그 단위여야 A/B가 성립한다.

            그래도 이 축은 판독 가능성 버킷이 **모델링하지 않는다**(아래
            classify_code는 흐림/노이즈/대비/각도만 본다). 즉 `--bucket ok`가
            이 축에서는 "읽혀야 정상"을 보증하지 않는다. 축은 A/B 비교용으로
            쓰고, 절대 수준을 성능 주장에 쓰지 말 것.
            [[vscan-lite-sweep-damage-uncalibrated]]
            """
            v = float(p["damaged"])
            im2 = sym.copy()
            d2 = ImageDraw.Draw(im2)
            ww, hh = im2.size
            n = int(np.interp(v, [0, 1], [1, 8]))
            wdt = max(1, int(round(eff_mod * float(np.interp(v, [0, 1], [0.3, 1.2])))))
            for k in range(n):
                fx = (k + 1) / (n + 1)
                d2.line([int(ww * fx), 0, int(ww * (1.0 - fx)), hh], fill=235, width=wdt)
            if symname in ("QR_CODE", "DATA_MATRIX", "PDF417") and v >= 0.6:
                cut = int(min(ww, hh) * float(np.interp(v, [0.6, 1], [0.05, 0.20])))
                d2.polygon([(ww, hh), (ww - cut, hh), (ww, hh - cut)], fill=200)
            # [파괴량을 모델링하지 말고 **잰다**]
            # 예전에는 이 축을 버킷이 아예 안 봤다 — 90%가 지워진 QR도
            # img-ok였다. 세기 v에서 파괴 비율을 역산하려면 선 개수·굵기·
            # 모서리 결손을 다시 모델링해야 하는데, 그럴 필요가 없다.
            # **그리기 전후를 빼면 정확한 값이 나온다.**
            before = np.array(sym, dtype=np.int16)
            after = np.array(im2, dtype=np.int16)
            code_px = before < 128                     # 원래 검정이던 모듈
            killed = code_px & (np.abs(after - before) > 64)
            dmg["frac"] = float(killed.sum()) / max(1, int(code_px.sum()))
            # 1D는 오류정정이 없는 대신 **세로 여유**가 있다. 스캔 라인 한
            # 줄만 성하면 읽힌다. 그래서 "깨끗한 행이 하나라도 있는가"가
            # 판정이고, 2D의 면적 비율과는 다른 물리다.
            rows_dirty = (np.abs(after - before) > 64).any(axis=1)
            dmg["clean_rows"] = float((~rows_dirty).sum()) / max(1, rows_dirty.size)
            sym = im2
        if float(p["dpm"]) >= 0.5:
            # 반전보다 **먼저** 찍는다 — 실제 공정도 각인한 뒤에 촬영 극성이
            # 정해지지, 반전된 이미지를 각인하지는 않는다.
            sym = Image.fromarray(
                _to_dpm(np.array(sym).astype(np.uint8), step=max(3.0, float(eff_mod))),
                mode="L")
        if float(p["invert"]) >= 0.5:
            # 난수 경로(_degrade)와 같은 방식: 반전 전에 흰 여백을 덧대야
            # 반전 후에 어두운 정지대가 남는다. 안 그러면 "반전"이 아니라
            # "정지대 파괴" 케이스가 된다.
            #
            # 여백은 **모듈 10개 이상**이어야 한다. 규격이 요구하는 정지대가
            # 1D 대부분(ITF/Code39/Code128 등)에서 최소 폭 요소의 10배다.
            # 크기 비율(6%)만 쓰면 코드가 커질수록 모듈 대비 정지대가
            # 얇아진다 — 실측(ITF module 8, 코드 1012x376): 6%는 20px인데
            # 규격은 59px을 요구한다. 그 상태로 "읽혀야 정상(ok)"으로
            # 분류하면 리더가 아니라 하네스가 틀린 것이다.
            a2 = np.array(sym).astype(np.uint8)
            pad = max(8, int(round(10 * eff_mod)), int(min(a2.shape) * 0.06))
            a2 = np.pad(a2, pad, mode="constant", constant_values=255)
            sym = Image.fromarray(255 - a2, mode="L")
        if p["angle"]:
            sym = sym.rotate(float(p["angle"]), expand=True, fillcolor=255,
                             resample=Image.BICUBIC)
        px = cell[0] + (cw - sym.width) // 2
        py = cell[1] + (ch - sym.height) // 2
        img.paste(sym, (max(0, px), max(0, py)))
        if cx0 is None:
            cx0 = (max(0, px) + sym.width / 2, max(0, py) + sym.height / 2)
        codes.append({"symbology": symname, "text": text, "x": max(0, px), "y": max(0, py),
                      "w": sym.width, "h": sym.height, "rot": float(p["angle"]),
                      "tags": [], "module_px": round(eff_mod, 2),
                      "phys": {"contrast": float(p["contrast"]),
                               "ec": str(p.get("ec", "M")),
                               "dmg_frac": round(dmg["frac"], 4),
                               "clean_rows": round(dmg["clean_rows"], 4)}})

    if float(p["quietzone"]) > 0 or float(p["dirty"]) > 0:
        # 프레임 단계 열화 둘. 난수 경로(_degrade_frame)와 같은 모양인데,
        # 스윕은 재현 가능해야 하므로 위치를 난수가 아니라 격자에서 뽑는다.
        d3 = ImageDraw.Draw(img)
        for c in codes:
            bx, by, bw, bh = c["x"], c["y"], c["w"], c["h"]
            if float(p["quietzone"]) > 0:
                """[여백은 **모듈 단위**여야 한다 — 안 그러면 정지대 파괴다]

                처음에는 pad를 픽셀 고정(10 -> 2px)으로 뒀다. 모듈 8px짜리
                ITF에서 10px는 **1.25모듈**이다. 그런데 ITF/Code39/Code128의
                규격 정지대는 최소 폭 요소의 **10배**다. 즉 가장 약한 단계도
                이미 규격을 깨고 있었고, 실측이 그대로 나왔다 — ITF가 0.1부터
                1.0까지 **전 구간 0/1**로 계단 없이 죽었다. 계단이 없다는 것
                자체가 "축이 아니라 벽"이라는 신호다.

                §3.41에서 반전 축이 정확히 같은 이유로 하네스 결함이었다.
                여기가 **다섯 번째**다.

                고친 뒤: 1D는 14 -> 10모듈, 2D는 6 -> 2모듈 범위. 가장 센
                단계가 규격 하한에 정확히 닿는다. 그러면 이 축이 재는 것이
                "정지대를 부쉈나"가 아니라 **"정지대 바로 밖에 붙은 잡동사니를
                견디나"** 가 된다 — ITF-14의 베어러 바가 실제로 그 모양이다.
                [[vscan-lite-sweep-quietzone-modules]]
                """
                v = float(p["quietzone"])
                mod = float(c.get("module_px", 8.0)) or 8.0
                is2d_c = c["symbology"] in ("QR_CODE", "DATA_MATRIX", "PDF417")
                mods = np.interp(v, [0, 1], (6.0, 2.0) if is2d_c else (14.0, 10.0))
                pad = max(2, int(round(mod * float(mods))))
                wdt = max(2, int(round(mod * float(np.interp(v, [0, 1], [0.5, 1.2])))))
                d3.rectangle([bx - pad, by - pad, bx + bw + pad, by + bh + pad],
                             outline=20, width=wdt)
                # 텍스트도 정지대 밖에 둔다(라벨의 사람 읽는 문자열 위치).
                d3.text((bx, max(0, by - pad - 40)),
                        "LOT 42315 / GTIN 008123456", font=_FNT, fill=15)
            if float(p["dirty"]) > 0:
                # 얼룩 크기도 모듈 단위다(위 damaged 주석과 같은 이유).
                # [파괴량은 손상 축과 같은 방식으로 **잰다**] 얼룩을 뿌리기
                # 전에 코드 영역을 떠 두고, 뿌린 뒤 빼서 몇 %가 덮였는지
                # 센다. 세기 v에서 역산하려면 개수·반지름·배치를 다시
                # 모델링해야 하는데 그럴 필요가 없다.
                _pre = np.array(img.crop((max(0, bx), max(0, by),
                                          bx + bw, by + bh)), dtype=np.int16)
                v = float(p["dirty"])
                mod = float(c.get("module_px", 8.0)) or 8.0
                n = int(np.interp(v, [0, 1], [20, 160]))
                rmax = max(1, int(round(mod * float(np.interp(v, [0, 1], [0.3, 1.5])))))
                """[뿌리는 자리를 난수 경로와 맞춘다 — 2026-08-04]

                처음에는 코드 상자 안팎(-15%~+115%)에 고르게 흩뿌렸다.
                그런데 난수 경로(_degrade_frame)는 얼룩의 **75%를 코드
                중앙 60% 안**에 넣는다. 면적으로 환산하면 코드 위 밀도가
                약 3.5배 차이 난다.

                그래서 두 경로가 같은 이름의 축을 다르게 재고 있었다 —
                통제 스윕에서는 `dirty`가 순한데(6종 0.4~1.0에서 5~6/6)
                난수 코퍼스에서는 겹수를 고정해도 리프트 1.38로 1위였다
                (§3.101). 축이 프록시 노릇을 못 한 것이다.

                난수 쪽 배치를 그대로 옮긴다: 75%는 중앙 60%, 25%는 넓게.
                [[vscan-lite-sweep-dirty-placement]]
                """
                for k in range(n):
                    fx = ((k * 0.6180339887) % 1.0)
                    fy = ((k * 0.7548776662) % 1.0)
                    if (k % 4) != 3:              # 4개 중 3개 = 75%
                        x2 = bx + bw * (0.2 + 0.6 * fx)
                        y2 = by + bh * (0.2 + 0.6 * fy)
                    else:
                        x2 = bx - bw * 0.15 + bw * 1.30 * fx
                        y2 = by - bh * 0.15 + bh * 1.30 * fy
                    r2 = 1 + (k % max(1, rmax))
                    d3.ellipse([x2, y2, x2 + r2, y2 + r2], fill=50 + (k * 37) % 165)
                _post = np.array(img.crop((max(0, bx), max(0, by),
                                           bx + bw, by + bh)), dtype=np.int16)
                if _pre.shape == _post.shape and _pre.size:
                    _codepx = _pre < 128                     # 원래 검정이던 모듈
                    _hit = _codepx & (np.abs(_post - _pre) > 64)
                    _frac = float(_hit.sum()) / max(1, int(_codepx.sum()))
                    # 손상과 오염은 **같은 예산(오류정정)을 갉아먹는다.**
                    # 한 프레임에 둘이 같이 걸리면 합쳐서 봐야 한다 —
                    # F격자가 정확히 그 곱이다(dirty x damaged).
                    c["phys"]["dmg_frac"] = round(
                        min(1.0, float(c["phys"].get("dmg_frac", 0.0)) + _frac), 4)
                    _rows_hit = (np.abs(_post - _pre) > 64).any(axis=1)
                    _clean = float((~_rows_hit).sum()) / max(1, _rows_hit.size)
                    c["phys"]["clean_rows"] = round(
                        min(float(c["phys"].get("clean_rows", 1.0)), _clean), 4)
    if p["blur"]:
        img = img.filter(ImageFilter.GaussianBlur(float(p["blur"])))
    arr = np.array(img).astype(np.float32)
    if p["curve"]:
        arr = _cylinder_warp(arr, float(p["curve"]))
    if p["motion"]:
        arr = _motion_blur(arr, int(p["motion"]), 0.0)
    if p["glare"]:
        yy = np.arange(h, dtype=np.float32)[:, None]
        xx = np.arange(w, dtype=np.float32)[None, :]
        arr = arr + float(p["glare"]) * np.exp(-(((xx - cx0[0]) ** 2) / (2 * 150.0 ** 2) +
                                                 ((yy - cx0[1]) ** 2) / (2 * 95.0 ** 2)))
    if p["shadow"] != 1.0:
        band = np.ones((h, w), dtype=np.float32)
        band[:, int(cx0[0]):] = float(p["shadow"])
        band = np.array(Image.fromarray((band * 255).astype(np.uint8), mode="L")
                        .filter(ImageFilter.GaussianBlur(12))).astype(np.float32) / 255.0
        arr = arr * band
    if p["bright"] != 1.0:
        arr = arr * float(p["bright"])
    # 센서 노이즈 바닥은 축과 무관하게 항상 얹는다
    # ([[vscan-lite-sensor-noise-floor]]). 축이 있으면 제곱합으로 합친다.
    arr = sensor_noise_floor(arr, rng, float(p["noise"]))

    fphys = {"blur": float(p["blur"]), "motion": float(p["motion"]),
             "noise": float(p["noise"]), "gain": float(p["bright"]),
             "shadow": float(p["shadow"]), "glare": float(p["glare"]),
             "curve": float(p["curve"])}
    for c in codes:
        c["phys"]["rot"] = float(c.get("rot", 0.0))
        c["phys"]["is2d"] = c["symbology"] in ("QR_CODE", "DATA_MATRIX")
        c["phys"]["sym"] = c["symbology"]
        c["phys"]["ecc2d"] = c["symbology"] in ("QR_CODE", "DATA_MATRIX", "PDF417")
        b, reasons = classify_code(c["module_px"], c["phys"], fphys)
        c["bucket"] = b
        c["tags"] = c["tags"] + ["dec-" + b] + reasons
    bucket = image_bucket([c["bucket"] for c in codes])

    tags = [f"{ax}={_fmt_val(v)}" for ax, v in sorted(combo.items())]
    tags.append("sym-" + codes[0]["symbology"])
    # **실제로 렌더된 모듈 px를 남긴다.** 요청값(`module=`)은 셀에 안 들어가면
    # 조용히 깎이므로 요청값만 보면 크기를 모른다 — 그래서 심볼로지끼리
    # 검출률을 나란히 놓을 때 크기 교란을 못 본다(§3.105). 캔버스를 넓혀
    # 대부분 요청대로 나오지만, 상한에 걸린 경우는 여기에 드러난다.
    tags.append(f"modpx-{eff_mod:.2f}")
    tags.append("img-" + bucket)
    name = "sw" + "".join(f"_{ax}-{_fmt_val(v)}" for ax, v in sorted(combo.items())) \
           + f"_{len(codes)}"
    params = {k2: _fmt_val(v) for k2, v in p.items()}
    params["module_effective"] = f"{eff_mod:.2f}"   # 셀에 안 들어가 클램프됐을 수 있다
    rec = {"file": name + "." + cfg["format"], "index": index, "expected": len(codes),
           "width": w, "height": h, "severity": 0.0, "difficulty": "sweep",
           "bucket": bucket, "tags": tags, "params": params, "codes": codes,
           "frame_phys": fphys}
    return name, np.clip(arr, 0, 255).astype(np.uint8), rec


def write_image(outdir, name, arr, fmt):
    path = os.path.join(outdir, f"{name}.{fmt}")
    if fmt == "pgm":
        h, w = arr.shape
        with open(path, "wb") as f:
            f.write(f"P5\n{w} {h}\n255\n".encode())
            f.write(arr.tobytes())
    else:
        Image.fromarray(arr, mode="L").save(path, optimize=True)
    return path


# ============================================================ 병렬 구동 / 출력
#
# 용량 문제에 대해 (중요)
# ----------------------
# 2048x1536 PGM 한 장 = 3.1MB. 1도 간격 360장 x 대비 50단계 = 18,000장 =
# **56GB**. 축을 두어 개만 더 걸면 수십만 장 = 수백 GB — 데스크탑 디스크가
# 그냥 찬다. 그래서 기본 사용법은 파일로 떨구는 게 아니라 **스트리밍**이다:
#
#   python3 tools/generate_corpus.py --sweep angle:0:359:1 --stream \
#     | ./verify_accuracy --stdin
#
# 프레임을 만들어 파이프로 바로 디코더에 먹이고 버린다. 디스크 사용량 0,
# 이미지 수에 상한이 없다. 정답(개수/텍스트/태그)은 프레임마다 헤더 한 줄로
# 같이 흘러가므로 라벨 파일도 필요 없다.
#
# 파일로 저장하는 건 "눈으로 볼 소수의 이미지"나 "여러 번 재사용할 고정
# 코퍼스"일 때만. 그래서 파일 모드에는 용량 상한(--max-disk-gb)이 걸려 있고,
# 예상 용량이 상한이나 남은 공간을 넘으면 아예 시작하지 않는다.

STREAM_MAGIC = "FRAME"

_CFG = None
_COMBOS = None


def _init(cfg, combos):
    global _CFG, _COMBOS
    _CFG, _COMBOS = cfg, combos


def _build(index):
    if _COMBOS is not None:
        return build_sweep(index, _COMBOS[index], _CFG)
    return build_one(index, _CFG)


def _work_file(index):
    try:
        name, arr, rec = _build(index)
        # 버킷별 하위 폴더로 나눠 떨어뜨린다. verify_accuracy는 디렉토리 하나를
        # 받으므로, 폴더가 나뉘어 있으면 "읽을 수 있어야 하는 것만" 골라
        # 채점하는 게 그냥 경로 하나 바꾸는 일이 된다.
        if _CFG["buckets"] and rec["bucket"] not in _CFG["buckets"]:
            return {"index": index, "skipped": True}
        sub = os.path.join(_CFG["outdir"], rec["bucket"])
        os.makedirs(sub, exist_ok=True)
        write_image(sub, name, arr, _CFG["format"])
        return rec
    except Exception as e:                      # 한 장 실패로 전체를 죽이지 않는다
        return {"index": index, "error": f"{type(e).__name__}: {e}"}


def _work_stream(index):
    """스트리밍용: 파일을 안 쓰고 (헤더, 원본바이트)를 부모로 돌려준다."""
    try:
        name, arr, rec = _build(index)
        if _CFG["buckets"] and rec["bucket"] not in _CFG["buckets"]:
            return b"", b"", 0                 # 필터로 걸러진 프레임
        hdr = "\t".join([STREAM_MAGIC, str(rec["width"]), str(rec["height"]), name,
                         str(rec["expected"]), ",".join(rec["tags"]),
                         "|".join(c["text"] for c in rec["codes"]),
                         codetags_field(rec)]) + "\n"
        return hdr.encode("utf-8"), arr.tobytes(), rec["expected"]
    except Exception as e:
        return None, f"{type(e).__name__}: {e}".encode(), index


def codetags_field(rec):
    """코드별 태그를 '&'로 묶어 코드 순서대로 '|' 구분.

    구분자가 '&'인 이유: 태그 이름 자체에 '+'가 들어간다("mod-5px+").
    '+'로 묶으면 파싱할 때 쪼개져서 유령 태그가 생긴다.

    모듈 크기/반전/DPM 같은 건 **코드의 속성**이지 이미지의 속성이 아니다.
    이미지 단위로만 집계하면 "2px 코드가 하나 섞인 이미지"의 나머지 큰
    코드들까지 mod-2px 행에 들어가서 축이 뭉개진다. verify_accuracy는 이
    열이 있으면 정답 텍스트 대조 결과를 코드별로 태그에 귀속시킨다.
    """
    return "|".join("&".join(c.get("tags") or ["-"]) for c in rec["codes"])


def write_labels(outdir, recs):
    """labels.tsv — C++ 쪽(verify_accuracy)이 파싱하기 쉬운 평면 포맷.

        file <TAB> expected <TAB> tags(,) <TAB> texts(|) <TAB> symbologies(|)

    JSON을 안 쓰는 이유는 verify_accuracy가 의존성 없이 읽어야 하기 때문.
    풍부한 정답(좌표/회전각/코드별 태그/스윕 파라미터)은 labels.jsonl에 쓴다.
    """
    with open(os.path.join(outdir, "labels.tsv"), "w", encoding="utf-8") as f:
        f.write("# file\texpected\ttags\ttexts\tsymbologies\tcodetags\n")
        for r in recs:
            f.write("\t".join([
                r["file"], str(r["expected"]), ",".join(r["tags"]),
                "|".join(c["text"] for c in r["codes"]),
                "|".join(c["symbology"] for c in r["codes"]),
                codetags_field(r),
            ]) + "\n")
    with open(os.path.join(outdir, "labels.jsonl"), "w", encoding="utf-8") as f:
        for r in recs:
            f.write(json.dumps(r, ensure_ascii=False) + "\n")


def build_combos(specs):
    """--sweep 여러 개의 데카르트 곱. 순서는 지정한 순서 그대로."""
    axes = [parse_sweep(s) for s in specs]
    combos = [{}]
    for name, vals in axes:
        combos = [dict(c, **{name: v}) for c in combos for v in vals]
    return axes, combos


def main():
    ap = argparse.ArgumentParser(
        description="악조건 대량 코퍼스/스윕 생성 (파일 저장 또는 스트리밍)",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="스윕 축:\n" + "\n".join(f"  {k:9} {v}" for k, v in SWEEP_HELP.items()))
    ap.add_argument("-o", "--outdir", default="corpus")
    ap.add_argument("-n", "--count", type=int, default=1000,
                    help="난수 모드에서 생성할 이미지 수 (--sweep을 쓰면 무시)")
    ap.add_argument("--start", type=int, default=0, help="시작 인덱스(코퍼스 이어붙이기)")
    ap.add_argument("--only", type=int, default=None, help="이 인덱스 한 장만 재생성")
    ap.add_argument("--seed", type=int, default=20260730, help="마스터 씨드")
    ap.add_argument("--difficulty", choices=sorted(DIFFICULTY), default="mixed")
    ap.add_argument("--width", type=int, default=2048)
    ap.add_argument("--height", type=int, default=1536)
    ap.add_argument("--max-codes", type=int, default=12)
    ap.add_argument("--symbologies", default="QR,QR,QR,CODE128,CODE128,EAN13,CODE39,ITF",
                    help="난수 모드 심볼로지. 중복해서 쓰면 그만큼 가중치가 올라간다")
    ap.add_argument("--sweep", action="append", default=[], metavar="AXIS:START:STOP:STEP",
                    help="격자 스윕 축. 여러 번 쓰면 데카르트 곱 "
                         "(예: --sweep angle:0:359:1 --sweep contrast:0.05:1:0.05)")
    ap.add_argument("--base", default="", metavar="k=v,k=v",
                    help="스윕에서 고정할 나머지 축 값 (예: sym=CODE128,module=3)")
    ap.add_argument("--format", choices=["pgm", "png"], default="pgm",
                    help="pgm=C 도구가 바로 읽는 원본, png=보관/눈으로 확인용(약 1/5 용량)")
    ap.add_argument("--stream", action="store_true",
                    help="파일 대신 stdout으로 프레임을 흘린다 (디스크 0). "
                         "받는 쪽: ./verify_accuracy --stdin")
    ap.add_argument("--max-disk-gb", type=float, default=20.0,
                    help="파일 모드 용량 상한(GB). 예상치가 넘으면 시작하지 않는다")
    ap.add_argument("--bucket", default="", metavar="ok|borderline|mixed|impossible",
                    help="이 버킷만 내보낸다(쉼표로 여러 개). 스트리밍에서 "
                         "'읽을 수 있어야 하는 것'만 채점할 때 --bucket ok")
    ap.add_argument("--fixed-canvas", action="store_true",
                    help="요청 모듈이 안 들어가도 캔버스를 넓히지 않는다(예전 동작). "
                         "**해상도 자체가 축일 때** 쓴다 — 안 쓰면 생성기가 "
                         "모듈을 맞추려고 프레임을 키워서 해상도 축이 무너진다.")
    ap.add_argument("--jobs", type=int, default=0, help="0=CPU 수")
    ap.add_argument("--est", action="store_true", help="생성 없이 개수/용량/시간만 추정")
    a = ap.parse_args()

    base = {}
    for kv in filter(None, (x.strip() for x in a.base.split(","))):
        k, _, v = kv.partition("=")
        k = k.strip()
        if k not in SWEEP_BASE:
            raise SystemExit(f"--base: 알 수 없는 축 {k}")
        base[k] = v.strip().upper() if k in ("sym", "ec") else (
            int(v) if k == "count" else float(v))

    axes, combos = (None, None)
    if a.sweep:
        axes, combos = build_combos(a.sweep)
        # [빠른 실패] 스윕이 지원하지 않는 심볼로지 이름을 그냥 두면
        # 워커 프로세스 안에서 예외가 나고 그게 삼켜져서 **0바이트를
        # 조용히 내보낸다** — 실제로 `sym=QRCODE`(정답은 `QR`)로 스윕을
        # 돌렸다가 빈 스트림을 받고 한참 헤맸다. 여기서 미리 잡는다.
        wanted = set()
        if "sym" in base:
            wanted.add(base["sym"])
        for combo in combos:
            if "sym" in combo:
                wanted.add(str(combo["sym"]).upper())
        unknown = sorted(w for w in wanted if w not in _SWEEP_PAYLOAD)
        if unknown:
            raise SystemExit(
                "스윕 미지원 심볼로지: " + ", ".join(unknown) +
                "\n사용 가능: " + ", ".join(sorted(_SWEEP_PAYLOAD)))
        idxs = list(range(len(combos)))
        if a.only is not None:
            idxs = [a.only]
    else:
        idxs = [a.only] if a.only is not None else list(range(a.start, a.start + a.count))

    jobs = max(1, a.jobs or os.cpu_count() or 1)
    per_mb = a.width * a.height / 1e6 * (1.0 if a.format == "pgm" else 0.22)
    total_gb = per_mb * len(idxs) / 1024.0
    # 실측 0.22s/장(2048x1536, 1코어)에서 해상도 비례 환산
    secs = len(idxs) * 0.22 * (a.width * a.height) / (2048 * 1536) / jobs

    if a.sweep:
        print(">> 스윕: " + " x ".join(f"{n}({len(v)}단계)" for n, v in axes)
              + f" = {len(combos)}장", file=sys.stderr)
    if a.est:
        print(f"{len(idxs)}장 / 파일 저장 시 {per_mb:.1f}MB x {len(idxs)} = {total_gb:.1f}GB "
              f"/ 생성 약 {secs / 60:.1f}분 ({jobs} jobs)\n"
              f"--stream 이면 디스크 0GB (프레임을 파이프로 바로 디코더에 넘김)",
              file=sys.stderr)
        return

    # ---- 용량 가드 (파일 모드에서만)
    if not a.stream:
        os.makedirs(a.outdir, exist_ok=True)
        free_gb = __import__("shutil").disk_usage(a.outdir).free / 1024 ** 3
        if total_gb > a.max_disk_gb:
            raise SystemExit(
                f"!! 예상 용량 {total_gb:.1f}GB > 상한 {a.max_disk_gb:.1f}GB — 중단합니다.\n"
                f"   해결책 (권장 순):\n"
                f"   1) --stream 으로 파이프에 흘리기 (디스크 0GB):\n"
                f"      python3 {os.path.basename(__file__)} ... --stream | ./verify_accuracy --stdin\n"
                f"   2) --width 1024 --height 768 (용량 1/4)\n"
                f"   3) --format png (약 1/5)\n"
                f"   4) 정말 저장해야 하면 --max-disk-gb {math.ceil(total_gb) + 1}")
        if total_gb > free_gb * 0.8:
            raise SystemExit(f"!! 예상 용량 {total_gb:.1f}GB 가 남은 공간 {free_gb:.1f}GB 의 "
                             f"80%를 넘습니다 — 중단합니다. --stream 을 쓰세요.")

    cfg = {"seed": a.seed, "width": a.width, "height": a.height, "outdir": a.outdir,
           "fixed_canvas": bool(a.fixed_canvas),
           "difficulty": a.difficulty, "max_codes": a.max_codes, "format": a.format,
           "base": base,
           "buckets": {b.strip() for b in a.bucket.split(",") if b.strip()},
           "symbologies": [s.strip().upper() for s in a.symbologies.split(",") if s.strip()]}

    log = sys.stderr                    # 스트리밍 중에는 stdout이 프레임 전용이다
    print(f">> {len(idxs)}장 ({'sweep' if a.sweep else a.difficulty}, seed={a.seed}, "
          f"{a.width}x{a.height}, jobs={jobs}) -> "
          + ("stdout 스트림 (디스크 0)" if a.stream else f"{a.outdir}/ ({total_gb:.1f}GB)"),
          file=log, flush=True)

    t0 = time.time()
    recs, errs, ncodes = [], 0, 0
    out = sys.stdout.buffer if a.stream else None

    def progress(i):
        if i % 200 == 0 or i == len(idxs):
            el = time.time() - t0
            print(f"  {i}/{len(idxs)}  {el:.0f}s  (남은 시간 약 "
                  f"{el / max(1, i) * (len(idxs) - i):.0f}s)", file=log, flush=True)

    if jobs > 1 and len(idxs) > 1:
        # 배치로 끊어서 돌린다 — 스트리밍에서 워커가 앞서 달려 나가면
        # 3MB짜리 프레임이 부모 메모리에 무한정 쌓인다. 배치 크기로 상한을 건다.
        batch = jobs * 2
        done = 0
        with mp.Pool(jobs, initializer=_init, initargs=(cfg, combos)) as pool:
            for s in range(0, len(idxs), batch):
                chunk = idxs[s:s + batch]
                if a.stream:
                    for hdr, payload, exp in pool.imap(_work_stream, chunk):
                        done += 1
                        if hdr is None:
                            errs += 1
                            print(f"  !! {exp}: {payload.decode()}", file=log)
                        elif hdr:
                            out.write(hdr); out.write(payload)
                            ncodes += exp
                        progress(done)
                    out.flush()
                else:
                    for rec in pool.imap(_work_file, chunk):
                        done += 1
                        if "error" in rec:
                            errs += 1
                            print(f"  !! {rec['index']}: {rec['error']}", file=log)
                        elif not rec.get("skipped"):
                            recs.append(rec)
                        progress(done)
    else:
        _init(cfg, combos)
        for i, ix in enumerate(idxs, 1):
            if a.stream:
                hdr, payload, exp = _work_stream(ix)
                if hdr is None:
                    errs += 1
                elif hdr:
                    out.write(hdr); out.write(payload); ncodes += exp
            else:
                rec = _work_file(ix)
                if "error" in rec:
                    errs += 1
                elif not rec.get("skipped"):
                    recs.append(rec)
            progress(i)
        if a.stream:
            out.flush()

    if not a.stream:
        recs.sort(key=lambda r: r["index"])
        by_bucket = {}
        for r in recs:
            by_bucket.setdefault(r["bucket"], []).append(r)
        for b, rs in by_bucket.items():
            write_labels(os.path.join(a.outdir, b), rs)
        write_labels(a.outdir, recs)          # 전체 합본(원하면 통으로 채점)
        ncodes = sum(r["expected"] for r in recs)
        print(">> 버킷별: " + ", ".join(
            f"{b}/ {len(rs)}장" for b, rs in sorted(by_bucket.items())), file=log)
    n_done = len(idxs) - errs
    print(f">> 완료: {n_done}장 / 코드 {ncodes}개 / {time.time() - t0:.0f}s"
          + (f" / 실패 {errs}장" if errs else ""), file=log)
    if not a.stream:
        print(f">> 정답: {a.outdir}/labels.tsv (verify_accuracy가 자동으로 읽는다), "
              f"labels.jsonl", file=log)


if __name__ == "__main__":
    main()
