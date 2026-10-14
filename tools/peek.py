import sys, numpy as np
from PIL import Image
import os
G = os.environ.get('POPTB_GAME', os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))) + '/'
O = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'out') + '/'
pal = np.fromfile(G+'data/pal0-0.dat', np.uint8).reshape(256,4)
print('pal first', pal[:4], 'alpha uniq', np.unique(pal[:,3])[:5])
rgb = pal[:,:3]
lv = np.fromfile(G+'levels/levl2001.dat', np.uint8)
h = lv[:32768].view('<u2').reshape(128,128)
print('height min/max', h.min(), h.max())
Image.fromarray((h.astype(float)/max(1,h.max())*255).astype(np.uint8)).resize((512,512),0).save(O+'levl2001_height.png')
for i,off in enumerate(range(32768, 32768+16384*4, 16384)):
    b = lv[off:off+16384]; print('block',i,'uniq',len(np.unique(b)), np.unique(b)[:10])
sky = np.fromfile(G+'data/sky0-0.dat', np.uint8).reshape(480,640)
Image.fromarray(rgb[sky]).save(O+'sky0-0.png')
bl = np.fromfile(G+'data/BL320-0.DAT', np.uint8).reshape(-1,256)
Image.fromarray(rgb[bl]).save(O+'bl320-0.png')
