#!/usr/bin/env python3
"""
generate_stress_images.py — 정확도 검증용 악조건 테스트 이미지 32종 생성.

정확도에 영향을 줄 수 있는 변경(binarizer, downscale, 옵션 조정 등)은
반드시 이 이미지 전부로 검증할 것. 크고 선명한 코드 이미지 몇 장으로만
검증하면 작은 코드/저대비에서 검출을 잃는 걸 놓친다 (PROJECT_NOTES §7).

파일명 끝의 숫자가 기대 검출 개수다. 예: `11_blur_motion_h_1.pgm` -> 1개.

조건 구성은 비교 대상 리더기가 카탈로그에서 내세우는 사용 시나리오
(컨베이어 이동체, 3단 적층 박스, 다중 코드 동시 판독, 장거리)와
산업 현장의 전형적 열화(반사광, 그림자, 인쇄 불량, 손상, 곡면, DPM)를
함께 담았다.

의존성: pip install qrcode python-barcode pillow numpy
사용:   python3 generate_stress_images.py --outdir ./stress
"""
import argparse
import os

import math
import numpy as np
import io
import qrcode
import barcode as pybarcode
from barcode.writer import ImageWriter
from PIL import Image, ImageDraw, ImageFilter, ImageFont

W, H = 2048, 1536


# ---------------------------------------------------------------- helpers
def _persp_expand(img, coeffs, max_grow=3.0):
    """원근 변환을 **캔버스를 넓혀서** 적용한다 (rotate(expand=True)와 같은 취지).

    coeffs는 PIL 관례대로 "목적지 -> 원본" 사상이다. 원본 네 모서리가 가는
    목적지 좌표의 바운딩 박스를 새 캔버스로 삼고, 그만큼 평행이동을 합성한다.
    강한 왜곡에서 크기가 폭발하면 균일 축소를 함께 합성해서 내용을 전부 담는다.
    """
    w, h = img.size
    M = np.array([[coeffs[0], coeffs[1], coeffs[2]],
                  [coeffs[3], coeffs[4], coeffs[5]],
                  [coeffs[6], coeffs[7], 1.0]], dtype=np.float64)
    F = np.linalg.inv(M)                       # 원본 -> 목적지
    pts = np.array([[0, 0, 1], [w, 0, 1], [w, h, 1], [0, h, 1]], dtype=np.float64).T
    q = F @ pts
    q = q[:2] / q[2]
    x0, y0 = q[0].min(), q[1].min()
    nw, nh = q[0].max() - x0, q[1].max() - y0
    fit = min(1.0, (w * max_grow) / max(1.0, nw), (h * max_grow) / max(1.0, nh))
    nw = max(8, int(math.ceil(nw * fit)))
    nh = max(8, int(math.ceil(nh * fit)))
    S = np.array([[1 / fit, 0, 0], [0, 1 / fit, 0], [0, 0, 1]], dtype=np.float64)
    T = np.array([[1, 0, x0], [0, 1, y0], [0, 0, 1]], dtype=np.float64)
    M2 = M @ T @ S
    M2 = M2 / M2[2, 2]
    return img.transform((nw, nh), Image.PERSPECTIVE, tuple(M2.ravel()[:8]),
                         resample=Image.BICUBIC, fillcolor=255)


def bg(rng, base_level=195):
    """조명 그라디언트 + 약한 센서 노이즈가 있는 배경(골판지/라벨 면 가정)."""
    a = np.full((H, W), float(base_level), dtype=np.float32)
    yy, xx = np.mgrid[0:H, 0:W]
    a += 25 * (1 - (xx / W) * 0.6 - (yy / H) * 0.3)
    a += rng.normal(0, 3, size=(H, W))
    return Image.fromarray(np.clip(a, 0, 255).astype(np.uint8), mode="L")


def code128(text, module_px=3, h=150):
    """1D Code128 바코드 이미지(회전 테스트용). QR과 달리 1D는 파인더
    패턴이 회전 불변이 아니라서, 회전 케이스에서 QR과는 다른 실패
    모드를 보인다(§3.2.15) — 이게 지금까지 36종에 빠져있던 갭이었다.

    주의: 예전엔 텍스트 길이와 무관하게 강제로 600x150 픽셀로
    리사이즈했는데, 이게 "텍스트가 길수록 막대가 가늘어지는" 비현실적
    아티팩트를 만들었다(실측: 16자 텍스트는 원본 대비 1.01배로 거의
    안 늘어나 막대가 매우 가는 반면, 1자 텍스트는 2.96배로 확대돼
    막대가 굵어짐 — 같은 "600x150 1D 회전 테스트"인데 실제 난이도가
    텍스트 길이에 따라 완전히 달랐다. 39번(45도, 16자)이 rescue
    5단계로도 안 잡히던 진짜 원인이 이거였다 — 알고리즘 한계가 아니라
    테스트 생성 결함). 실제 현장에서는 데이터가 길면 라벨 폭이 넓어
    지지 막대가 가늘어지지 않는다(모듈 폭을 고정해서 인쇄하므로) —
    그래서 모듈 폭(module_px)을 고정하고 폭은 텍스트 길이에 따라
    자연스럽게 늘어나게 한다. [[vscan-lite-code128-width-bug]]
    """
    buf = io.BytesIO()
    pybarcode.Code128(text, writer=ImageWriter()).write(
        buf, options={"module_height": 20, "quiet_zone": 4, "font_size": 0, "write_text": False})
    raw = Image.open(buf).convert("L")
    # 원본 폭 기준 모듈 개수를 역산해 고정 module_px로 다시 스케일 —
    # 텍스트 길이와 무관하게 막대 두께가 일정하게 유지된다.
    native_modules = raw.width / 2.0  # writer 기본 module width가 대략 2px 근방
    w = max(200, int(native_modules * module_px))
    return raw.resize((w, h))


def qr(payload, px, ec=qrcode.constants.ERROR_CORRECT_M):
    q = qrcode.QRCode(border=2, box_size=6, error_correction=ec)
    q.add_data(payload)
    q.make(fit=True)
    return q.make_image(fill_color="black", back_color="white").convert("L").resize(
        (px, px), Image.NEAREST
    )


def apply_motion_blur(img, length, angle_deg):
    """방향성 모션 블러 (컨베이어 이동체 흉내).

    PIL의 ImageFilter.Kernel은 3x3/5x5만 지원하므로, 이동 경로를 따라
    시프트한 복사본들을 누적 평균하는 방식으로 직접 구현한다.
    """
    a = np.array(img).astype(np.float32)
    rad = np.deg2rad(angle_deg)
    dx, dy = np.cos(rad), np.sin(rad)
    acc = np.zeros_like(a)
    half = length // 2
    for t in range(-half, half + 1):
        sx, sy = int(round(dx * t)), int(round(dy * t))
        acc += np.roll(np.roll(a, sy, axis=0), sx, axis=1)
    return acc / (2 * half + 1)


def cylinder_warp(img_arr, strength=0.35):
    """원통(병/시험관) 표면에 감긴 라벨의 수평 압축 왜곡."""
    h, w = img_arr.shape
    xs = np.arange(w, dtype=np.float32)
    cx = w / 2.0
    norm = (xs - cx) / cx                       # -1..1
    warped = cx + np.sin(norm * (np.pi / 2)) * cx * (1 - strength) + norm * cx * strength
    warped = np.clip(warped, 0, w - 1)
    x0 = np.floor(warped).astype(np.int32)
    x1 = np.clip(x0 + 1, 0, w - 1)
    frac = warped - x0
    out = np.empty_like(img_arr, dtype=np.float32)
    for r in range(h):
        row = img_arr[r].astype(np.float32)
        out[r] = row[x0] * (1 - frac) + row[x1] * frac
    return out


def save(arr_or_img, outdir, name):
    if isinstance(arr_or_img, Image.Image):
        arr = np.array(arr_or_img)
    else:
        arr = arr_or_img
    arr = np.clip(arr, 0, 255).astype(np.uint8)
    assert arr.shape == (H, W), f"{name}: shape {arr.shape}"
    with open(os.path.join(outdir, f"{name}.pgm"), "wb") as f:
        f.write(f"P5\n{W} {H}\n255\n".encode())
        f.write(arr.tobytes())
    print(f"  {name}.pgm")


def soft(img, r=0.6):
    """렌즈 MTF 정도의 아주 약한 블러 — 정상 조건 이미지의 공통 마감."""
    return np.array(img.filter(ImageFilter.GaussianBlur(r))).astype(np.float32)


# ---------------------------------------------------------------- generators
def gen(outdir, seed):
    rng = np.random.default_rng(seed)
    P = "VSCAN-TEST"
    yy, xx = np.mgrid[0:H, 0:W]

    # ---- [기본/다중] ------------------------------------------------
    c = bg(rng)
    c.paste(qr(f"{P}-BASE", 400), (800, 550))
    save(soft(c), outdir, "01_baseline_1")

    c = bg(rng)
    for i, (x, y) in enumerate(
        [(200, 200), (800, 200), (1400, 200), (200, 900), (800, 900), (1400, 900)]
    ):
        c.paste(qr(f"{P}-M6-{i:02d}", 280), (x, y))
    save(soft(c), outdir, "02_multi_6")

    c = bg(rng)
    n = 0
    for row in range(3):
        for col in range(4):
            c.paste(qr(f"{P}-M12-{n:02d}", 200), (150 + col * 460, 180 + row * 460))
            n += 1
    save(soft(c), outdir, "03_multi_12")

    # 3단 적층 박스 — 비교 대상 리더기 카탈로그 시나리오. 면마다 거리가 달라
    # 코드 크기와 초점(피사계 심도)이 각각 다르다.
    c = bg(rng)
    for i, (px, pos, blur) in enumerate(
        [(420, (180, 240), 0.6), (300, (820, 620), 1.6), (200, (1500, 1000), 2.8)]
    ):
        layer = Image.new("L", (W, H), 255)
        layer.paste(qr(f"{P}-STACK-{i}", px), pos)
        mask = Image.new("L", (W, H), 0)
        mask.paste(Image.new("L", (px, px), 255), pos)
        c = Image.composite(layer.filter(ImageFilter.GaussianBlur(blur)), c, mask)
    save(soft(c, 0.4), outdir, "04_stacked_boxes_3")

    # ---- [거리/크기] ------------------------------------------------
    for px, name in [(1200, "05_huge_1"), (90, "06_small_90px_1"),
                     (60, "07_tiny_60px_1"), (45, "08_micro_45px_1")]:
        c = bg(rng)
        pos = (420, 160) if px > 800 else (900, 700)
        c.paste(qr(f"{P}-{px}PX", px), pos)
        save(soft(c), outdir, name)

    # ---- [블러] -----------------------------------------------------
    c = bg(rng); c.paste(qr(f"{P}-DEFOCUS", 400), (800, 550))
    save(np.array(c.filter(ImageFilter.GaussianBlur(3.5))).astype(np.float32),
         outdir, "09_blur_defocus_1")

    c = bg(rng); c.paste(qr(f"{P}-DEFOCUS-X", 400), (800, 550))
    save(np.array(c.filter(ImageFilter.GaussianBlur(6.0))).astype(np.float32),
         outdir, "10_blur_defocus_extreme_1")

    c = bg(rng); c.paste(qr(f"{P}-MOTION-H", 400), (800, 550))
    save(apply_motion_blur(c, 15, 0), outdir, "11_blur_motion_h_1")

    c = bg(rng); c.paste(qr(f"{P}-MOTION-D", 400), (800, 550))
    save(apply_motion_blur(c, 21, 30), outdir, "12_blur_motion_diag_1")

    # ---- [노이즈/노출] ----------------------------------------------
    c = bg(rng); c.paste(qr(f"{P}-NOISE", 400), (800, 550))
    save(np.array(c).astype(np.float32) + rng.normal(0, 28, (H, W)), outdir, "13_noisy_1")

    c = bg(rng); c.paste(qr(f"{P}-NOISE-X", 400), (800, 550))
    save(np.array(c).astype(np.float32) + rng.normal(0, 55, (H, W)), outdir, "14_noisy_extreme_1")

    c = bg(rng, base_level=225); c.paste(qr(f"{P}-OVEREXP", 400), (800, 550))
    save(soft(c) * 1.55, outdir, "15_overexposed_1")

    c = bg(rng); c.paste(qr(f"{P}-UNDEREXP", 400), (800, 550))
    save(soft(c) * 0.18 + rng.normal(0, 4, (H, W)), outdir, "16_underexposed_1")

    # ---- [대비/인쇄품질] --------------------------------------------
    for ratio, name in [(0.18, "17_lowcontrast_1"), (0.08, "18_lowcontrast_extreme_1")]:
        c = bg(rng)
        q = np.array(qr(f"{P}-LOWCON-{int(ratio * 100)}", 400)).astype(np.float32)
        q = 128 + (q - 128) * ratio
        c.paste(Image.fromarray(np.clip(q, 0, 255).astype(np.uint8), mode="L"), (800, 550))
        save(soft(c), outdir, name)

    # 흑백 반전 (다크 라벨 / 각인)
    c = bg(rng, base_level=60)
    q = 255 - np.array(qr(f"{P}-INVERT", 400)).astype(np.float32)
    c.paste(Image.fromarray(q.astype(np.uint8), mode="L"), (800, 550))
    save(soft(c), outdir, "19_inverted_1")

    # 인쇄 불량 — 잉크 끊김(가로줄 누락)
    c = bg(rng); c.paste(qr(f"{P}-PRINTDEF", 400), (800, 550))
    a = soft(c)
    for y in range(560, 950, 17):
        a[y:y + 2, 800:1200] = 235
    save(a, outdir, "20_print_defect_1")

    # DPM 흉내 — 도트 각인(점으로만 구성) + 금속 표면 거칠기
    c = bg(rng, base_level=150)
    qa = np.array(qr(f"{P}-DPM", 400))
    dot = np.full_like(qa, 255)
    step = 5
    for y0 in range(0, qa.shape[0], step):
        for x0 in range(0, qa.shape[1], step):
            if qa[y0:y0 + step, x0:x0 + step].mean() < 128:
                cy, cx = y0 + step // 2, x0 + step // 2
                dot[max(0, cy - 1):cy + 2, max(0, cx - 1):cx + 2] = 70
    c.paste(Image.fromarray(dot, mode="L"), (800, 550))
    save(soft(c, 0.9) + rng.normal(0, 9, (H, W)), outdir, "21_dpm_dotpeen_1")

    # ---- [기하 왜곡] ------------------------------------------------
    for ang, name in [(15, "22_rot_15deg_1"), (45, "23_rot_45deg_1"), (90, "24_rot_90deg_1")]:
        c = bg(rng)
        r = qr(f"{P}-ROT{ang}", 400).rotate(ang, expand=True, fillcolor=255,
                                             resample=Image.BICUBIC)
        c.paste(r, (760, 480))
        save(soft(c), outdir, name)

    for coeffs, name in [
        ((1, 0.12, -40, 0.04, 1, -20, 0.00012, 0.00004), "25_perspective_mild_1"),
        ((1, 0.34, -110, 0.13, 1, -60, 0.00042, 0.00013), "26_perspective_strong_1"),
    ]:
        # [캔버스를 넓혀서 변환한다] PIL의 transform(size, PERSPECTIVE, ...)은
        # 출력 크기를 그대로 두므로 전단으로 밀려난 부분이 잘려 나간다.
        # 바코드에서 그건 "왜곡"이 아니라 **코드 일부가 없어지는 것**이라
        # 어떤 리더로도 못 읽는다 — 실측(26번, 460x460 고정 출력): 원본
        # 오른쪽 위 모서리가 목적지 x=716으로 가서 캔버스(460) 밖이었다.
        # 즉 QR의 오른쪽 열이 통째로 사라진 이미지를 "읽어야 정상"으로
        # 세고 있었다. 코퍼스 생성기는 같은 결함을 이미 고쳤다(§3.29).
        q = _persp_expand(qr(f"{P}-PERSP", 460), coeffs)
        c = bg(rng); c.paste(q, (790, 530))
        save(soft(c), outdir, name)

    # 곡면 (병/시험관에 감긴 라벨)
    c = bg(rng); c.paste(qr(f"{P}-CURVED", 420), (814, 558))
    save(cylinder_warp(soft(c), strength=0.42), outdir, "27_curved_surface_1")

    # ---- [조명/오염/손상] -------------------------------------------
    # 정반사 (라미네이트 라벨 위 광원 반사)
    c = bg(rng); c.paste(qr(f"{P}-GLARE", 400), (800, 550))
    glare = 190 * np.exp(-(((xx - 1010) ** 2) / (2 * 150 ** 2) +
                           ((yy - 700) ** 2) / (2 * 95 ** 2)))
    save(soft(c) + glare, outdir, "28_glare_1")

    # 강한 그림자 경계가 코드를 가로지름
    c = bg(rng); c.paste(qr(f"{P}-SHADOW", 400), (800, 550))
    shade = np.ones((H, W), dtype=np.float32)
    shade[(xx > 990) & (xx < 1500)] = 0.34
    shade = np.array(Image.fromarray((shade * 255).astype(np.uint8), mode="L")
                     .filter(ImageFilter.GaussianBlur(12))) / 255.0
    save(soft(c) * shade, outdir, "29_shadow_1")

    # 오염 (먼지/얼룩)
    c = bg(rng); c.paste(qr(f"{P}-DIRTY", 400), (800, 550))
    d = ImageDraw.Draw(c)
    for _ in range(90):
        x = int(rng.integers(790, 1210)); y = int(rng.integers(540, 960))
        r = int(rng.integers(3, 13))
        d.ellipse([x, y, x + r, y + r], fill=int(rng.integers(60, 210)))
    save(soft(c), outdir, "30_dirty_1")

    # 물리적 손상 — 긁힘 + 모서리 결손 (QR ECC 한계 시험)
    c = bg(rng); c.paste(qr(f"{P}-DAMAGED", 400), (800, 550))
    d = ImageDraw.Draw(c)
    for _ in range(7):
        x0 = int(rng.integers(800, 1150)); y0 = int(rng.integers(560, 900))
        d.line([x0, y0, x0 + int(rng.integers(60, 190)), y0 + int(rng.integers(-50, 90))],
               fill=235, width=int(rng.integers(4, 10)))
    d.polygon([(1200, 950), (1080, 950), (1200, 830)], fill=200)
    save(soft(c), outdir, "31_damaged_1")

    # 품질 혼합 다중 코드 — 깨끗한 코드 + 반전 코드가 한 프레임에.
    # 빠른 경로의 "부분 검출" 문제를 잡는 케이스: locate가 쉬운 것만 찾고
    # 비어있지 않으니 폴백이 안 걸리는 상황. min_expected_codes 검증용.
    a2 = np.full((H, W), 140, dtype=np.float32)
    a2 += 20 * (1 - (xx / W) * 0.5); a2 += rng.normal(0, 3, (H, W))
    c = Image.fromarray(np.clip(a2, 0, 255).astype(np.uint8), mode="L")
    c.paste(Image.fromarray(np.array(qr(f"{P}-MIX-CLEAN", 400)), mode="L"), (250, 550))
    inv2 = 255 - np.array(qr(f"{P}-MIX-INV", 400)).astype(np.float32)
    c.paste(Image.fromarray(inv2.astype(np.uint8), mode="L"), (1350, 550))
    save(soft(c), outdir, "33_mixed_difficulty_2")

    # ---- [1D 바코드 회전] ---------------------------------------------
    # 지금까지 22~24번(회전)은 전부 QR이었다. QR의 파인더 패턴은 회전
    # 불변이라 회전에 강한데, 1D 바코드(Code128 등)는 원리적으로 다르다
    # — 스캔 자체가 방향 의존적이라 결과가 전혀 다르게 나온다(§3.2.15).
    # 37: 0도(기준) / 38: 15도(작은 기울기 허용 범위 안) /
    # 39: 45도(풀옵션으로도 못 읽음 — 알려진 zxing 한계, DPM과 같은 성격) /
    # 40: 90도(TryRotate가 반드시 있어야 읽힘 — 회귀 테스트 핵심 케이스)
    for ang, name in [(0, "37_rot1d_0deg_1"), (8, "38_rot1d_8deg_1"),
                      (45, "39_rot1d_45deg_1"), (90, "40_rot1d_90deg_1")]:
        c = bg(rng)
        bc = code128(f"{P}-1D-{ang}")
        bc = bc.rotate(ang, expand=True, fillcolor=255, resample=Image.BICUBIC)
        c.paste(bc, (700, 650))
        save(soft(c), outdir, name)

    # 콰이어트존 침범 — 코드에 밀착한 테두리/텍스트
    c = bg(rng); c.paste(qr(f"{P}-QUIETZONE", 400), (800, 550))
    d = ImageDraw.Draw(c)
    d.rectangle([772, 522, 1228, 978], outline=20, width=6)
    d.text((786, 500), "LOT 20260726 / GTIN 00812345678905", fill=15)
    d.line([778, 545, 778, 955], fill=25, width=8)
    save(soft(c), outdir, "32_quiet_zone_violation_1")

    # ---- [현장형 클러터] --------------------------------------------
    # 지금까지의 이미지는 "코드만 달랑" 있어 배경 클러터가 만드는 가짜
    # 후보(텍스트/표/체커보드 로고/줄무늬)의 비용을 못 잰다. 실측: 클러터는
    # full 경로를 40~60% 느리게 하지만 two_stage는 거의 영향 없음.
    try:
        FNT = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 28)
        FNTB = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 44)
    except OSError:
        FNT = FNTB = ImageFont.load_default()

    # 34. 물류 라벨: 빽빽한 텍스트/표/굵은 선 사이에 코드 2개
    c = bg(rng, base_level=210); d = ImageDraw.Draw(c)
    d.rectangle([100, 100, 1948, 1436], outline=10, width=8)
    d.line([100, 320, 1948, 320], fill=10, width=6)
    d.line([100, 760, 1948, 760], fill=10, width=6)
    d.line([1020, 100, 1020, 760], fill=10, width=6)
    d.text((140, 140), "SHIP TO:", font=FNTB, fill=15)
    for i, t in enumerate(["GYEONGGI LOGISTICS CENTER 3F", "1234-56 SANEOP-RO, ANYANG-SI",
                            "GYEONGGI-DO 14118 KOREA", "TEL 031-000-0000  DOCK 07"]):
        d.text((140, 210 + i * 24), t, font=FNT, fill=20)
    d.text((1060, 140), "PO# 2026-078812", font=FNTB, fill=15)
    for i, t in enumerate(["ITEM: BRG-6204-ZZ  QTY: 480",
                            "LOT: L2607A  NW: 12.4kg  GW: 13.1kg",
                            "MADE IN KOREA  INSPECTOR: 07"]):
        d.text((1060, 210 + i * 24), t, font=FNT, fill=20)
    for i in range(9):
        d.line([140, 820 + i * 60, 1000, 820 + i * 60], fill=25, width=2)
        d.text((150, 830 + i * 60), f"ROW {i:02d}  PART-{1000 + i}  {int(rng.integers(1, 99)):02d} EA",
               font=FNT, fill=30)
    d.line([300, 820, 300, 1300], fill=25, width=2)
    d.line([650, 820, 650, 1300], fill=25, width=2)
    c.paste(qr(f"{P}-CLT-QR1", 340), (1160, 560))
    c.paste(qr(f"{P}-CLT-QR2", 250), (1600, 1120))
    save(soft(c), outdir, "34_clutter_label_2")

    # 35. 창고 장면: 박스 모서리/테이프/이웃 라벨 조각 + 코드 1개
    c = bg(rng, base_level=170); d = ImageDraw.Draw(c)
    for _ in range(14):
        x0 = int(rng.integers(0, 1700)); y0 = int(rng.integers(0, 1200))
        d.rectangle([x0, y0, x0 + int(rng.integers(150, 500)), y0 + int(rng.integers(120, 400))],
                    outline=int(rng.integers(40, 110)), width=int(rng.integers(3, 9)))
    for _ in range(6):
        x0 = int(rng.integers(0, 1800)); y0 = int(rng.integers(0, 1400))
        d.rectangle([x0, y0, x0 + int(rng.integers(200, 600)), y0 + 22], fill=235)
    for _ in range(10):
        d.text((int(rng.integers(50, 1700)), int(rng.integers(50, 1400))),
               f"BOX-{int(rng.integers(100, 999))} FRAGILE", font=FNT, fill=int(rng.integers(30, 90)))
    c.paste(qr(f"{P}-CLT-WH", 360), (820, 540))
    save(soft(c), outdir, "35_clutter_warehouse_1")

    # 36. 가짜 패턴 함정: 체커보드(QR 파인더 미끼) + 줄무늬(1D 미끼) + 코드 1개
    c = bg(rng, base_level=200); d = ImageDraw.Draw(c)
    for bx, by in [(150, 150), (1550, 200), (200, 1100)]:
        for i in range(8):
            for j in range(8):
                if (i + j) % 2 == 0:
                    d.rectangle([bx + i * 24, by + j * 24, bx + i * 24 + 23, by + j * 24 + 23], fill=20)
    for sx, sy in [(500, 1250), (1300, 1300), (1500, 700)]:
        x = sx
        for _ in range(30):
            w = int(rng.integers(3, 12))
            d.rectangle([x, sy, x + w, sy + 130], fill=15)
            x += w + int(rng.integers(3, 10))
    d.text((500, 180), "|||| |||| |||| ||||  SERIAL 883-7721  |||| ||||", font=FNTB, fill=20)
    c.paste(qr(f"{P}-CLT-FAKE", 340), (850, 500))
    save(soft(c), outdir, "36_clutter_falsepattern_1")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--outdir", default="stress")
    ap.add_argument("--seed", type=int, default=11)
    a = ap.parse_args()
    os.makedirs(a.outdir, exist_ok=True)
    print(f"generating 40 images into {a.outdir}/ (파일명 끝 숫자 = 기대 검출 개수)")
    gen(a.outdir, a.seed)


if __name__ == "__main__":
    main()
