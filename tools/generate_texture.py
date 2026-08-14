#!/usr/bin/env python3
"""
generate_texture.py — **1D 바코드처럼 보이는 무늬** 프레임을 만든다.

컨베이어에서 유령(오디코딩)이 실제로 태어나는 자리는 빈 종이가 아니라
**줄무늬**다. 벨트 리브, 팔레트 슬랫, 나무결, 그물망 — 전부 "밝고 어두운
띠가 번갈아 나오는" 구조라 1D 디코더가 훑는 신호와 같은 모양이다.

여기 코드는 하나도 없다. 그러므로 **여기서 나오는 것은 전부 유령**이고,
기대 코드 수는 0이다. §3.18(미검출 < 오디코딩)에서 가장 나쁜 쪽이다.

실측(2026-08-03, full + 2stage): 다섯 장 전부 **유령 0**. 체크디짓이 없어
가장 위험한 셋(Industrial 2of5 / COOP 2of5 / Pharmacode)을 **켜고 돌려도
0**이다 — decoder_linear의 방어(§3.48)가 실제로 일하고 있다는 뜻이다.

게이트 `[3.75/5]`가 이 축을 지킨다. 다른 코퍼스는 전부 코드가 있는
프레임이라 "없는 것을 만들어내는" 회귀를 원리적으로 못 잡는다.

    python3 tools/generate_texture.py <출력디렉터리>
"""
import os
import sys

import numpy as np
from PIL import Image, ImageFilter

W, H = 1280, 960


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "/tmp/texture"
    os.makedirs(out, exist_ok=True)
    rng = np.random.default_rng(11)
    yy, xx = np.mgrid[0:H, 0:W]

    # 폭이 불규칙한 세로 줄무늬 — 1D 바코드와 가장 헷갈린다
    slats = np.full((H, W), 210.0)
    pos = 0
    while pos < W:
        wbar = int(rng.integers(3, 14))
        slats[:, pos:min(W, pos + wbar)] = 40
        pos += wbar + int(rng.integers(3, 14))

    cases = {
        "slats_irregular": slats.copy(),
        "belt_ribs":       200 - 150 * ((xx // 9) % 2),
        "woodgrain":       170 + 50 * np.sin(xx / 13.0 + 3 * np.sin(yy / 90.0)),
        "mesh":            210 - 160 * (((xx // 11) % 2) ^ ((yy // 11) % 2)),
        "slats_noisy":     slats + rng.normal(0, 18, (H, W)),
    }

    rows = []
    for nm, arr in cases.items():
        v = np.clip(arr + rng.normal(0, 3, (H, W)), 0, 255).astype(np.uint8)
        v = np.array(Image.fromarray(v).filter(ImageFilter.GaussianBlur(0.6)))
        fn = "tex_%s.pgm" % nm
        with open(os.path.join(out, fn), "wb") as f:
            f.write(b"P5\n%d %d\n255\n" % (W, H))
            f.write(v.tobytes())
        rows.append(fn)
        print("  %s" % fn)

    # labels.tsv가 이미 있으면(빈 프레임과 같은 디렉터리) 이어붙인다.
    mode = "a" if os.path.exists(os.path.join(out, "labels.tsv")) else "w"
    with open(os.path.join(out, "labels.tsv"), mode) as f:
        for fn in rows:
            f.write("%s\t0\t-\t\t\ttexture\n" % fn)


if __name__ == "__main__":
    main()
