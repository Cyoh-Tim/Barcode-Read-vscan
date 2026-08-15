#!/usr/bin/env python3
"""같은 내용 라벨 2장을 세로로 붙인 프레임 — dedup 규칙의 **양성 대조**.

dedup의 밴드 규칙들([[vscan-lite-dedup-stacked-band]],
[[vscan-lite-dedup-thin-slice]], [[vscan-lite-dedup-twin-band]],
[[vscan-lite-dedup-single-band]])은 전부 "같은 텍스트 + 기하 조건"으로
지운다. 이 규칙들이 깨뜨릴 수 있는 **유일한 실제 배치**가 박스에 같은
라벨이 여러 장 붙은 경우다.

그래서 그 배치를 직접 만들어 둔다. 규칙을 새로 넣거나 문턱을 만질 때
이 코퍼스에서 "둘 다 나오는가"가 바뀌면 그건 대가를 치른 것이고,
치를 만한지 따로 판단해야 한다.

간격을 여러 단으로 두는 이유: 라벨이 바짝 붙으면 어차피
isStackedBand가 합친다(그 주석에 적힌 알려진 대가다). 기준선은
"어느 간격부터 둘로 남는가"이고, 바뀌면 안 되는 것은 그 경계다.

사용:
  python3 tools/generate_stacked_labels.py --outdir DIR
"""
import argparse
import os
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import generate_corpus as g  # noqa: E402

# 간격(px). 라벨 높이가 161px이므로 10~240은 "거의 붙음"부터 "확실히 떨어짐"까지다.
GAPS = (10, 30, 60, 120, 240)
WIDTH, HEIGHT = 1600, 1400
TEXT = "312245984327"          # EAN13 12자리 + 체크디짓 자동
MODULE = 4


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--outdir", required=True)
    args = ap.parse_args()
    os.makedirs(args.outdir, exist_ok=True)

    cls, symname, wopts = g._LINEAR["EAN13"]
    # box_h를 크게 줘서 ratio(0.35)가 높이를 정하게 한다 — 실제 인쇄 비율.
    img, full = g._linear_render(cls, TEXT, MODULE, 0.35, 10 ** 9, **wopts)
    lab = np.array(img.convert("L"), dtype=np.uint8)
    lh, lw = lab.shape

    for gap in GAPS:
        canvas = np.full((HEIGHT, WIDTH), 235, dtype=np.uint8)
        x = (WIDTH - lw) // 2
        top = (HEIGHT - (2 * lh + gap)) // 2
        for k in range(2):
            y = top + k * (lh + gap)
            canvas[y:y + lh, x:x + lw] = lab
        Image.fromarray(canvas).save(os.path.join(args.outdir, f"gap{gap:03d}_2.pgm"))

    with open(os.path.join(args.outdir, "labels.tsv"), "w") as f:
        f.write("# file\texpected\ttags\ttexts\tsymbologies\tcodetags\n")
        for gap in GAPS:
            # 구분자는 texts/symbologies가 '|', tags가 ',' 다 (verify_accuracy 참고).
            f.write(f"gap{gap:03d}_2.pgm\t2\tstacked-same-label\t{full}|{full}\t"
                    f"EAN_13|EAN_13\tstacked|stacked\n")

    print(f"라벨 {lw}x{lh} ({full}) / 간격 {list(GAPS)} / {len(GAPS)}장")


if __name__ == "__main__":
    main()
