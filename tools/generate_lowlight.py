#!/usr/bin/env python3
"""
generate_lowlight.py — **어두운 장면**의 다중 QR 프레임을 만든다.

## 왜 따로 만드나

기존 저대비 코퍼스(§3.57, 17/18번)는 "종이는 밝은데 잉크가 연한" 것이다.
현장에서 온 프레임은 다르다 — **노출이 모자라서 장면 전체가 어둡다.**
둘은 물리가 다르고, 그래서 고쳐야 할 것도 다르다.

    저대비  : 배경 200, 잉크 170  -> 폭 30, 밝기는 정상
    저조도  : 배경 110, 잉크  30  -> 폭 80, **밝기 자체가 낮다**

저조도에서 진짜 문제는 폭이 아니라 **노이즈 대비 폭**이다. 노출을 반으로
줄이면 신호(폭)도 반이 되는데 판독 노이즈는 그대로라 SNR이 반이 된다.
그래서 이 생성기는 노출 계수를 곱하고 **노이즈는 고정 성분 + 샷 성분**으로
따로 넣는다. 밝기만 낮춘 이미지는 어두운 척만 하는 것이라 의미가 없다.

## 장면 구성

현장 프레임을 그대로 흉내낸다: **조밀한 QR 여러 개가 격자로** 놓여 있고,
가장자리 것들은 **잘려 있고**, 전체가 살짝 기울어 있다. 프레임당 온전히
보이는 코드 수를 labels.tsv의 기대값으로 쓴다 — 잘린 것은 기대하지 않는다.

## 쓰는 법

    python3 tools/generate_lowlight.py --outdir /tmp/lowlight
"""
import argparse
import os

import numpy as np
import qrcode
from PIL import Image, ImageFilter

W, H = 1280, 960

# 노출 단계. 1.0이 정상, 0.15가 로그를 보낸 프레임 근처다.
EXPOSURES = [1.0, 0.30, 0.12, 0.06, 0.03, 0.015]

# 모듈 폭(px). 현장 사진의 코드는 매우 조밀하다 — 프레임 안에 QR 6개가
# 들어가는데 각각 70모듈 넘게 쓴다. 모듈이 몇 픽셀이냐가 난이도를 지배한다.
MODULE_PX = [6, 4, 3, 2]

# 실기 로그의 RESULTC 페이로드를 흉내낸 길이(약 250자) — 이 정도면 EC M에서
# QR 버전 13 근처(69모듈)가 된다. 현장 사진의 밀도와 맞는다.
PAYLOAD = ("QC0008DWKBAHQAAAAEQAEAAIK31X031ZAPAAAQJAAFL5555555AAAAAAABRK5555554"
           "FL555555YVP555555AVL555555AACIAAAC0AHCAAA1WAAAMFMAAVFIABGAC4AYBGEHP"
           "A2MAYQ4EBKEAFIQASOAABZSPWDUNEH2ESD555555555T55555314052AAAEMMAB1QHX"
           "AIAAAAAA4AAAA4AAAA%02d")

# 반사율. 종이 0.82, 잉크 0.10 — 정상 노출에서 209 / 26 이 된다.
PAPER, INK = 0.82, 0.10


def qr_img(payload, module_px, ec=qrcode.constants.ERROR_CORRECT_M):
    """**모듈 폭을 고정**하고 코드 크기는 데이터가 정한다.

    코드 전체를 고정 픽셀로 리사이즈하면 데이터가 길수록 모듈이 얇아져서
    "긴 데이터 = 어려움"이라는 가짜 난이도가 생긴다(§generate_stress의
    code128 폭 버그와 같은 함정). 현장에서는 모듈 폭을 고정해 인쇄하고
    라벨이 커진다.
    """
    q = qrcode.QRCode(border=2, box_size=1, error_correction=ec)
    q.add_data(payload)
    q.make(fit=True)
    img = q.make_image(fill_color="black", back_color="white").convert("L")
    n = img.width
    return img.resize((n * module_px, n * module_px), Image.NEAREST), n


def scene(payloads, module_px, rot_deg, ec):
    """반사율 평면(0~1)을 만든다. 노출/노이즈는 아직 안 씌운다.

    반환: (반사율 float32 [H,W], 온전히 보이는 코드 payload 목록)
    """
    refl = np.full((H, W), PAPER, dtype=np.float32)
    _, nmod = qr_img(payloads[0], module_px, ec)
    px = nmod * module_px
    cols, rows = 3, 2
    gapx = max(8, (W - cols * px) // (cols + 1))
    gapy = max(8, (H - rows * px) // (rows + 1))
    # 격자를 일부러 위/왼쪽으로 밀어 가장자리 코드를 **반쯤** 자른다 —
    # 현장 프레임이 그랬다. 잘린 코드는 기대값에서 뺀다.
    ox, oy = -(gapx + px // 2), -(gapy + px // 2)
    visible = []
    for i, p in enumerate(payloads[:cols * rows]):
        r, c = divmod(i, cols)
        x = ox + gapx + c * (px + gapx)
        y = oy + gapy + r * (px + gapy)
        img, _ = qr_img(p, module_px, ec)
        if rot_deg:
            img = img.rotate(rot_deg, resample=Image.BICUBIC, expand=True, fillcolor=255)
        a = np.array(img, dtype=np.float32) / 255.0
        ah, aw = a.shape
        # 프레임과의 교집합만 붙인다
        x0, y0 = max(0, x), max(0, y)
        x1, y1 = min(W, x + aw), min(H, y + ah)
        if x1 <= x0 or y1 <= y0:
            continue
        sub = a[y0 - y:y1 - y, x0 - x:x1 - x]
        # 반사율: 흰 1.0 -> PAPER, 검정 0.0 -> INK
        refl[y0:y1, x0:x1] = INK + (PAPER - INK) * sub
        if x >= 0 and y >= 0 and x + aw <= W and y + ah <= H:
            visible.append(p)
    return refl, visible, nmod


def expose(refl, exposure, rng, blur=0.6, agc=True, agc_target=118.0):
    """반사율 -> 화소값. **여기가 이 생성기의 핵심이다.**

    순서가 중요하다. 노이즈는 센서 단계(게인 **전**)에서 섞이고, 게인은
    신호와 노이즈를 **같이** 키운다. 순서를 바꾸면 게인이 SNR을 개선하는
    물리적으로 틀린 이미지가 나온다.

    1. 광자: 신호는 노출에 비례한다. 조명 그라디언트는 반사율이 아니라
       노출 쪽에 곱한다(실제 조명이 그렇다).
    2. 렌즈 MTF 블러.
    3. 센서 노이즈: 판독 노이즈(read)는 노출과 무관하게 일정하고, 샷
       노이즈는 sqrt(신호)에 비례한다.
    4. **AGC 게인**: 카메라는 어두우면 게인을 올려 밝기를 되돌린다. 이게
       현장 사진이 새까맣지 않고 **중간 회색인데 거칠거칠한** 이유다.
       밝기는 정상으로 돌아오지만 SNR은 그대로 나쁘다. 저조도의 진짜
       어려움은 "어둡다"가 아니라 **"신호 대비 노이즈"** 다.
    """
    yy, xx = np.mgrid[0:H, 0:W]
    grad = 1.0 - 0.18 * (xx / W) - 0.10 * (yy / H)
    sig = 255.0 * refl * exposure * grad
    img = Image.fromarray(np.clip(sig, 0, 255).astype(np.uint8), mode="L")
    sig = np.array(img.filter(ImageFilter.GaussianBlur(blur)), dtype=np.float32)
    read = 2.2                       # DN, 노출과 무관
    shot = np.sqrt(np.maximum(sig, 0)) * 0.55
    sig = sig + rng.normal(0, 1, size=sig.shape) * np.sqrt(read ** 2 + shot ** 2)
    if agc:
        # 게인은 1 미만으로 안 내린다. 밝을 때 카메라가 줄이는 것은 게인이
        # 아니라 노출 시간이고, 그건 이미 exposure 인자가 표현한다. 여기서
        # 1 미만을 허용하면 밝은 기준 프레임까지 어두워져 대조가 깨진다.
        sig = sig * max(1.0, min(64.0, agc_target / max(1.0, float(sig.mean()))))
    return np.clip(sig, 0, 255).astype(np.uint8)


def save(arr, outdir, name):
    with open(os.path.join(outdir, name), "wb") as f:
        f.write(("P5\n%d %d\n255\n" % (W, H)).encode())
        f.write(arr.tobytes())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--outdir", default="/tmp/lowlight")
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--no-agc", action="store_true",
                    help="AGC 게인을 끈다 — 밝기만 낮은(노이즈는 안 커진) 대조군")
    a = ap.parse_args()
    os.makedirs(a.outdir, exist_ok=True)
    rng = np.random.default_rng(a.seed)

    rows = []
    for mpx in MODULE_PX:
        payloads = [PAYLOAD % i for i in range(6)]
        refl, visible, nmod = scene(payloads, mpx, 6, qrcode.constants.ERROR_CORRECT_M)
        for e in EXPOSURES:
            name = "ll_m%d_e%03d.pgm" % (mpx, int(e * 100))
            arr = expose(refl, e, rng, agc=not a.no_agc)
            save(arr, a.outdir, name)
            rows.append((name, visible, ["mod%d" % mpx, "e%03d" % int(e * 100)]))
            # 실제로 어려워졌는지 보이게 SNR을 같이 찍는다.
            # **노이즈는 코드가 없는 종이에서만 잰다** — 코드 위에서 재면
            # 모듈 변조를 노이즈로 착각해서 SNR이 늘 좋게 나온다.
            # 조명 그라디언트도 종이 화소를 퍼뜨리므로 그냥 std를 쓰면 노이즈가
            # 과대평가된다. 저역(그라디언트)을 빼고 고역만 남겨서 잰다.
            lo = np.array(Image.fromarray(arr).filter(ImageFilter.GaussianBlur(6)),
                          dtype=np.float32)
            hi = arr.astype(np.float32) - lo
            mask = refl >= PAPER - 1e-6
            paper = arr[mask].astype(np.float32)
            ink = arr[refl <= INK + 1e-6].astype(np.float32)
            sigma = float(hi[mask].std())
            span = float(paper.mean() - ink.mean()) if ink.size else 0.0
            print("  %s  (%d모듈 x %dpx, 코드 %d개, 평균밝기 %3d, 종이-잉크 폭 %3d, "
                  "종이σ %.1f -> SNR %.1f)"
                  % (name, nmod, mpx, len(visible), int(arr.mean()), span, sigma,
                     span / max(0.5, sigma)))

    with open(os.path.join(a.outdir, "labels.tsv"), "w") as f:
        for name, vis, tags in rows:
            f.write("%s\t%d\t%s\t%s\t%s\t%s\n"
                    % (name, len(vis), ",".join(["QR", "lowlight"] + tags),
                       "|".join(vis), "|".join(["QR"] * len(vis)), "lowlight"))
    print("labels.tsv: %d 프레임" % len(rows))


if __name__ == "__main__":
    main()
