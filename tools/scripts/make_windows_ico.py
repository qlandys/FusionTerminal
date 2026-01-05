import struct
import sys
from io import BytesIO
from pathlib import Path


SIZES = [16, 20, 24, 28, 30, 32, 36, 40, 42, 44, 48, 56, 60, 64, 72, 80, 96, 128, 256]
PNG_SIG = b"\x89PNG\r\n\x1a\n"


def _png_dimensions(png_bytes: bytes) -> tuple[int, int] | None:
    if len(png_bytes) < 24 or not png_bytes.startswith(PNG_SIG):
        return None
    # PNG: signature (8) + length (4) + type (4) + IHDR payload (13) ...
    # Width/height are big-endian at the start of IHDR payload.
    chunk_type = png_bytes[12:16]
    if chunk_type != b"IHDR":
        return None
    w = int.from_bytes(png_bytes[16:20], "big")
    h = int.from_bytes(png_bytes[20:24], "big")
    if w <= 0 or h <= 0:
        return None
    return w, h


def _pick_base_png(input_path: Path) -> Path:
    if input_path.is_file():
        return input_path
    if not input_path.is_dir():
        raise FileNotFoundError(input_path)

    # Prefer the largest artwork.
    candidates = [
        input_path / "android-chrome-512x512.png",
        input_path / "apple-touch-icon.png",
        input_path / "android-chrome-192x192.png",
    ]
    for p in candidates:
        if p.exists():
            return p
    pngs = sorted(input_path.glob("*.png"), key=lambda p: p.stat().st_size, reverse=True)
    if pngs:
        return pngs[0]
    raise FileNotFoundError(f"no PNG sources in {input_path}")


def _collect_explicit_size_pngs(input_dir: Path) -> dict[int, Path]:
    out: dict[int, Path] = {}
    for size in (16, 32):
        p = input_dir / f"favicon-{size}x{size}.png"
        if p.exists():
            out[size] = p
    return out


def _alpha_bbox(img, thr: int):
    a = img.getchannel("A")
    return a.point(lambda x: 255 if x >= thr else 0).getbbox()


def _crop_remove_glow(img):
    # This specific logo has a soft outer glow; including it makes pinned taskbar icons look
    # "shrunk" because Windows renders 16/24/32px from that padded bbox.
    bb_lo = _alpha_bbox(img, 1)
    bb_hi = _alpha_bbox(img, 220)
    bb = bb_hi or bb_lo
    if not bb:
        return img
    if bb_lo and bb_hi:
        lo_w = bb_lo[2] - bb_lo[0]
        lo_h = bb_lo[3] - bb_lo[1]
        hi_w = bb_hi[2] - bb_hi[0]
        hi_h = bb_hi[3] - bb_hi[1]
        if hi_w >= lo_w * 0.75 and hi_h >= lo_h * 0.75:
            bb = bb_hi
        else:
            bb = bb_lo
    x0, y0, x1, y1 = bb
    if x1 <= x0 or y1 <= y0:
        return img
    return img.crop((x0, y0, x1, y1))


def _fit_square(img, size: int):
    from PIL import Image  # type: ignore

    w, h = img.size
    if w <= 0 or h <= 0:
        return Image.new("RGBA", (size, size), (0, 0, 0, 0))

    scale = min(size / w, size / h)
    nw = max(1, int(round(w * scale)))
    nh = max(1, int(round(h * scale)))
    resized = img.resize((nw, nh), resample=Image.Resampling.LANCZOS)

    canvas = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    x = (size - nw) // 2
    y = (size - nh) // 2
    canvas.alpha_composite(resized, (x, y))
    return canvas


def _render_small(src, size: int):
    from PIL import Image  # type: ignore
    from PIL import ImageEnhance  # type: ignore
    from PIL import ImageFilter  # type: ignore

    # Multi-stage downscale to preserve contrast at 16/24/32.
    work = _fit_square(src, 256)

    # Reduce high-frequency noise before downscaling further.
    work = work.filter(ImageFilter.MedianFilter(size=3))
    work = ImageEnhance.Contrast(work).enhance(1.35)
    work = ImageEnhance.Color(work).enhance(1.10)

    # Downscale with a box filter first (good for tiny icons), then sharpen.
    mid = work.resize((size * 2, size * 2), resample=Image.Resampling.BOX)
    out = mid.resize((size, size), resample=Image.Resampling.LANCZOS)
    out = out.filter(ImageFilter.UnsharpMask(radius=0.9, percent=220, threshold=2))
    return out


def _render_large(src, size: int):
    from PIL import ImageFilter  # type: ignore

    out = _fit_square(src, size)
    if size <= 64:
        out = out.filter(ImageFilter.UnsharpMask(radius=1.0, percent=160, threshold=2))
    return out


def _render_all_sizes(base_png: Path):
    from PIL import Image  # type: ignore

    src = Image.open(base_png).convert("RGBA")
    src = _crop_remove_glow(src)

    images = {}
    for size in SIZES:
        if size <= 32:
            images[size] = _render_small(src, size)
        else:
            images[size] = _render_large(src, size)
    return images


def _write_ico_png_payloads(images, out_path: Path):
    entries = []
    payloads = []
    offset = 6 + 16 * len(images)

    for size in sorted(images.keys()):
        buf = BytesIO()
        images[size].save(buf, format="PNG", optimize=True)
        data = buf.getvalue()

        width_byte = 0 if size >= 256 else int(size)
        height_byte = 0 if size >= 256 else int(size)
        entries.append(
            struct.pack(
                "<BBBBHHII",
                width_byte,
                height_byte,
                0,
                0,
                1,
                32,
                len(data),
                offset,
            )
        )
        payloads.append(data)
        offset += len(data)

    header = struct.pack("<HHH", 0, 1, len(entries))
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(header + b"".join(entries) + b"".join(payloads))


def _write_ico_png_bytes_payloads(png_payloads: dict[int, bytes], out_path: Path):
    entries = []
    payloads = []
    offset = 6 + 16 * len(png_payloads)

    for size in sorted(png_payloads.keys()):
        data = png_payloads[size]
        width_byte = 0 if size >= 256 else int(size)
        height_byte = 0 if size >= 256 else int(size)
        entries.append(
            struct.pack(
                "<BBBBHHII",
                width_byte,
                height_byte,
                0,
                0,
                1,
                32,
                len(data),
                offset,
            )
        )
        payloads.append(data)
        offset += len(data)

    header = struct.pack("<HHH", 0, 1, len(entries))
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(header + b"".join(entries) + b"".join(payloads))


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print("usage: make_windows_ico.py <input.png|input.ico|folder> <output.ico>", file=sys.stderr)
        return 2

    input_path = Path(argv[1])
    out_ico = Path(argv[2])

    if input_path.is_file() and input_path.suffix.lower() == ".ico":
        out_ico.parent.mkdir(parents=True, exist_ok=True)
        out_ico.write_bytes(input_path.read_bytes())
        return 0

    explicit_pngs: dict[int, Path] = {}
    if input_path.is_dir():
        explicit_pngs = _collect_explicit_size_pngs(input_path)

    # Best-effort: if Pillow isn't available, still generate an .ico from the explicit favicon PNGs.
    try:
        base_png = _pick_base_png(input_path)
        images = _render_all_sizes(base_png)

        if explicit_pngs:
            from PIL import Image  # type: ignore
            from PIL import ImageFilter  # type: ignore

            small_src = None
            if 32 in explicit_pngs:
                try:
                    small_src = Image.open(explicit_pngs[32]).convert("RGBA")
                except Exception:
                    small_src = None

            if small_src is not None:
                for size in SIZES:
                    if size > 32:
                        continue
                    if size == 16 and 16 in explicit_pngs:
                        continue
                    try:
                        img = _fit_square(small_src, size)
                        if size <= 32:
                            img = img.filter(ImageFilter.UnsharpMask(radius=0.6, percent=160, threshold=2))
                        images[size] = img
                    except Exception:
                        pass

            for size, p in explicit_pngs.items():
                try:
                    img = Image.open(p).convert("RGBA")
                    if img.size != (size, size):
                        img = _fit_square(img, size)
                    images[size] = img
                except Exception:
                    pass

        _write_ico_png_payloads(images, out_ico)
    except ModuleNotFoundError:
        if not explicit_pngs:
            raise
        payloads: dict[int, bytes] = {}
        for size, p in explicit_pngs.items():
            try:
                data = p.read_bytes()
            except Exception:
                continue
            dims = _png_dimensions(data)
            if dims != (size, size):
                continue
            payloads[size] = data
        if not payloads:
            return 1
        _write_ico_png_bytes_payloads(payloads, out_ico)
        return 0

    # Optional: drop previews next to output for quick sanity check.
    try:
        preview16 = out_ico.with_suffix(".preview16.png")
        preview32 = out_ico.with_suffix(".preview32.png")
        images[16].save(preview16)
        images[32].save(preview32)
    except Exception:
        pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
