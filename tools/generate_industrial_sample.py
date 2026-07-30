#!/usr/bin/env python3
"""
generate_industrial_sample.py — 산업현장 컨베이어 라벨 스타일의 합성 테스트
이미지를 생성한다. QR + Code128을 실제 규격대로 그려 넣고, 골판지 텍스처/
조명 그라디언트/비네팅/모션 블러/센서 노이즈를 얹어서 "깨끗한 스캐너 이미지"가
아니라 어느 정도 현실적인 캡처 조건을 흉내낸다.

의존성: pip install --break-system-packages qrcode python-barcode pillow numpy
"""
import argparse
import numpy as np
import qrcode
import barcode
from barcode.writer import ImageWriter
from PIL import Image, ImageDraw, ImageFilter


def generate(width: int, height: int, seed: int, qr_payload: str, code128_payload: str) -> Image.Image:
    rng = np.random.default_rng(seed)

    base = np.full((height, width), 195, dtype=np.float32)
    yy, xx = np.mgrid[0:height, 0:width]
    gradient = 25 * (1 - (xx / width) * 0.6 - (yy / height) * 0.3)
    base += gradient

    texture = np.repeat(
        np.repeat(rng.normal(0, 3, size=(height // 4, width // 4)), 4, axis=0), 4, axis=1
    )[:height, :width]
    base += texture
    base += rng.normal(0, 3, size=(height, width))

    cy, cx = height / 2, width / 2
    dist = np.sqrt((xx - cx) ** 2 / (width / 2) ** 2 + (yy - cy) ** 2 / (height / 2) ** 2)
    vignette = 1 - 0.15 * np.clip(dist - 0.6, 0, 1)
    base *= vignette

    base = np.clip(base, 0, 255).astype(np.uint8)
    canvas = Image.fromarray(base, mode="L")

    qr = qrcode.QRCode(border=2, box_size=6, error_correction=qrcode.constants.ERROR_CORRECT_M)
    qr.add_data(qr_payload)
    qr.make(fit=True)
    qr_img = qr.make_image(fill_color="black", back_color="white").convert("L").resize((320, 320), Image.NEAREST)

    code128 = barcode.get("code128", code128_payload, writer=ImageWriter())
    code128.writer.set_options({"module_height": 18, "quiet_zone": 4, "font_size": 0, "write_text": False})
    bc_img = code128.render().convert("L").resize((560, 140), Image.NEAREST)

    label_w, label_h = 950, 480
    label = Image.new("L", (label_w, label_h), 250)
    label.paste(qr_img, (40, 80))
    label.paste(bc_img, (400, 90))
    draw = ImageDraw.Draw(label)
    draw.rectangle([0, 0, label_w - 1, label_h - 1], outline=80, width=3)
    draw.text((400, 250), code128_payload, fill=30)
    draw.text((40, 410), f"LOT / QR: {qr_payload[:24]}...", fill=30)

    label = label.rotate(-3, expand=True, fillcolor=250, resample=Image.BICUBIC)
    canvas.paste(label, (width * 3 // 10, height * 3 // 10))

    canvas = canvas.filter(ImageFilter.GaussianBlur(radius=0.6))
    arr = np.array(canvas).astype(np.int16)
    arr += rng.normal(0, 2, size=arr.shape).astype(np.int16)
    return Image.fromarray(np.clip(arr, 0, 255).astype(np.uint8), mode="L")


def save_pgm(img: Image.Image, path: str) -> None:
    arr = np.array(img)
    h, w = arr.shape
    with open(path, "wb") as f:
        f.write(f"P5\n{w} {h}\n255\n".encode())
        f.write(arr.tobytes())


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--width", type=int, default=2048)
    ap.add_argument("--height", type=int, default=1536)
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--qr-payload", default="010812345678905317260930")
    ap.add_argument("--code128-payload", default="SN-8837221049")
    ap.add_argument("--out-prefix", default="industrial_sample")
    args = ap.parse_args()

    img = generate(args.width, args.height, args.seed, args.qr_payload, args.code128_payload)
    img.save(f"{args.out_prefix}.png")
    save_pgm(img, f"{args.out_prefix}.pgm")
    print(f"saved {args.out_prefix}.png / {args.out_prefix}.pgm ({args.width}x{args.height})")
