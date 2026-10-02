"""
Generate the KUTSU Sense welcome-screen logo for a 128x64 1-bit SSD1306 OLED.

Outputs (in this assets/ folder):
  - welcome_logo.bmp        1-bit, 128x64  -> load on device via displayio.OnDiskBitmap
  - welcome_logo_preview.png 8x upscaled, for human viewing
  - welcome_logo_framebuf.py  byte array, for MicroPython/CircuitPython framebuf fallback

Run:  python make_welcome_logo.py
Placeholder art only - swap the drawing code below for the real brand logo later.
"""
from PIL import Image, ImageDraw, ImageFont
import os

W, H = 128, 64
img = Image.new("1", (W, H), 0)          # 0 = black/off background
d = ImageDraw.Draw(img)

def font(path_candidates, size):
    for p in path_candidates:
        if os.path.exists(p):
            return ImageFont.truetype(p, size)
    return ImageFont.load_default()

bold = font([r"C:\Windows\Fonts\arialbd.ttf", r"C:\Windows\Fonts\segoeuib.ttf"], 28)
small = font([r"C:\Windows\Fonts\arial.ttf", r"C:\Windows\Fonts\segoeui.ttf"], 12)

def center_text(text, y, fnt):
    l, t, r, b = d.textbbox((0, 0), text, font=fnt)
    d.text(((W - (r - l)) / 2 - l, y), text, fill=1, font=fnt)
    return (r - l), (b - t)

# Outer rounded frame
d.rounded_rectangle([1, 1, W - 2, H - 2], radius=6, outline=1, width=1)

# Title
center_text("KUTSU", 6, bold)

# Divider
d.line([24, 40, W - 24, 40], fill=1, width=1)

# Subtitle (letter-spaced manually for a cleaner look)
sub = "S E N S E"
center_text(sub, 44, small)

# Small "air wave" glyphs flanking the subtitle divider
def wave(cx, cy):
    pts = [(cx + i, cy + (2 if (i // 2) % 2 == 0 else -2)) for i in range(0, 12, 2)]
    d.line(pts, fill=1, width=1)
wave(8, 40)
wave(W - 20, 40)

# Save 1-bit BMP for displayio.OnDiskBitmap
bmp_path = os.path.join(os.path.dirname(__file__), "welcome_logo.bmp")
img.save(bmp_path)

# Save an upscaled preview for humans
preview = img.convert("L").resize((W * 8, H * 8), Image.NEAREST)
preview_path = os.path.join(os.path.dirname(__file__), "welcome_logo_preview.png")
preview.save(preview_path)

# Emit a framebuf byte array (MONO_HLSB) fallback
buf = bytearray()
for y in range(H):
    for xb in range(0, W, 8):
        byte = 0
        for bit in range(8):
            if img.getpixel((xb + bit, y)):
                byte |= 1 << (7 - bit)
        buf.append(byte)
fb_path = os.path.join(os.path.dirname(__file__), "welcome_logo_framebuf.py")
with open(fb_path, "w") as f:
    f.write("# Auto-generated. 128x64 MONO_HLSB framebuffer for the welcome logo.\n")
    f.write("WIDTH = 128\nHEIGHT = 64\n")
    f.write("DATA = bytes(" + repr(bytes(buf)) + ")\n")

print("Wrote:", bmp_path)
print("Wrote:", preview_path)
print("Wrote:", fb_path)
