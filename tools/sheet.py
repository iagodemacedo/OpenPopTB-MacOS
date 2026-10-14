# tools/sheet.py <dir> <out.png> [step]: builds a contact sheet from the screenshots
import sys, os
from PIL import Image, ImageDraw, ImageFile
ImageFile.LOAD_TRUNCATED_IMAGES = True
d, out = sys.argv[1], sys.argv[2]
step = int(sys.argv[3]) if len(sys.argv) > 3 else 1
fs = sorted(f for f in os.listdir(d) if f.endswith('.bmp'))[::step][:16]
W, H = 320, 240
cols = 4
rows = (len(fs) + cols - 1) // cols
sheet = Image.new('RGB', (W * cols, (H + 14) * rows), 'black')
dr = ImageDraw.Draw(sheet)
for i, f in enumerate(fs):
    im = Image.open(os.path.join(d, f)).convert('RGB').resize((W, H))
    x, y = (i % cols) * W, (i // cols) * (H + 14)
    sheet.paste(im, (x, y + 14))
    dr.text((x + 2, y), f, fill='white')
sheet.save(out)
