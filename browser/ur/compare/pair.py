from pathlib import Path
import sys

from PIL import Image, ImageDraw

out = Path(__file__).resolve().parents[2] / ".build" / "ur-comparison"
variant = sys.argv[1] if len(sys.argv) > 1 else "day"
desktop = Image.open(out / f"desktop-{variant}.ppm").convert("RGB")
browser = Image.open(out / f"webgpu-{variant}.png").convert("RGB")
if desktop.size != browser.size:
    raise ValueError(f"Capture sizes differ: {desktop.size} and {browser.size}")

if not all(any(lo != hi for lo, hi in frame.getextrema()) for frame in (desktop, browser)):
    raise ValueError("A capture contains only a flat background.")

width, height = desktop.size
header, gutter = 28, 8
pair = Image.new("RGB", (2 * width + gutter, height + header), "#151c20")
draw = ImageDraw.Draw(pair)
for image, title, x in ((desktop, "DESKTOP  /  VULKAN", 0), (browser, "BROWSER  /  WEBGPU", width + gutter)):
    draw.text((x + 8, 8), title, fill="#e4ecf1")
    pair.paste(image, (x, header))
pair.save(out / f"comparison-{variant}.png")
