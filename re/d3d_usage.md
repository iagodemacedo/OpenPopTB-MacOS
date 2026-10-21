# D3DPopTB.exe: DirectDraw / Direct3D usage

Static analysis of `game/D3DPopTB.exe` (base 0x400000). Sources: `re/export_d3d/decomp.c`, plus
capstone disassembly of the whole `.text` section. A COM call site was found by the pattern
`mov R,[obj]; push obj; call [R+off]` and by tracking the register that holds each known interface
field. Every row cites the function (`FUN_` start) or the instruction address. **Inferred** marks
anything not read straight from code.

Vtable offsets used below: IUnknown 0/4/8. IDirectDraw: 0x10 CreateClipper, 0x14 CreatePalette,
0x18 CreateSurface, 0x20 EnumDisplayModes, 0x2c GetCaps, 0x30 GetDisplayMode, 0x4c RestoreDisplayMode,
0x50 SetCooperativeLevel, 0x54 SetDisplayMode(3 args, v1), 0x5c GetAvailableVidMem (DD2),
0x6c GetDeviceIdentifier (DD4). IDirectDrawSurface: 0xc AddAttachedSurface, 0x14 Blt, 0x1c BltFast,
0x2c Flip, 0x30 GetAttachedSurface, 0x50 GetPalette, 0x58 GetSurfaceDesc, 0x60 IsLost, 0x64 Lock,
0x6c Restore, 0x70 SetClipper, 0x74 SetColorKey, 0x7c SetPalette, 0x80 Unlock, 0x94 PageLock (Surface4).

---

## 0. Object model and globals

The game wraps DirectDraw/D3D in one C++ "shell" object (`CDDShell`, extended for D3D). The shell
vtable is 0x58f6c8. The shell is allocated in `FUN_004b1f60` (size 0x21c0) and stored in
`DAT_00afc2f4` (004b2031). `DAT_0098ed34` holds the same pointer (004b035a).

| Holder | Content | Set at | Evidence |
|---|---|---|---|
| shell+0x04 | **IDirectDraw (v1)** from `DirectDrawCreate` | 004b0290 | `DirectDrawCreate(guid,this+4,0)` at 004b0390 |
| shell+0x08 | IDirectDraw2 (QI `IID_IDirectDraw2`) | 004b03ae | used only for GetAvailableVidMem (+0x5c): 004b4930, 004b5630, 004b6820, 004b7980, 004e45d0, 005002e0 |
| shell+0x0c | back-buffer wrapper (+0x4c = surface, +0x18 = locked ptr) | 004b1370 via `FUN_00529740` | getter `FUN_004b2650` |
| shell+0x5c | primary wrapper | 004b1370 | getter `FUN_004b2660` |
| shell+0xb4 | IDirectDrawPalette for an 8-bit display | `FUN_004b1970` | only when back buffer is DDPF_PALETTEINDEXED8 |
| shell+0xb8 / +0xbc | width / height of current mode | `FUN_004b0fa0` | |
| shell+0xec | z-buffer surface | `FUN_00520a10` | |
| shell+0x510 | 1 = D3D enabled (HAL GUID passed to `FUN_004b0290`) | 004b0290 | |
| shell+0x514 (= +0x108) | 1 = HW device (`ddHwDesc.dcmColorModel != 0`) | 00520c87 | picks VIDEOMEMORY vs SYSTEMMEMORY |
| shell+0x518 | pointer to device GUID (IID_IDirect3DHALDevice) | 004b0290 | |
| shell+0x640 | card id: 1 PowerVR, 2 Riva128, 3 TNT, 4 RagePro, 5 Voodoo2, 7 G200, 8 Permedia2, 9 Banshee | 00521002..00521164 | from DD4 GetDeviceIdentifier vendor/device id |
| shell+0x644 | **IDirect3D2** | 00520b8b (QI on +4) | |
| shell+0x648 | **IDirect3DDevice2** | 00520d77 (CreateDevice) | |
| shell+0x64c | **IDirect3DViewport2** | 00521f25 | |
| shell+0x6c0 | IDirectDrawPalette, 256 entries, `DDPCAPS_8BIT|DDPCAPS_INITIALIZE` (0xc) | 00520d2c | `SetEntries` wrapper `FUN_005209c0` |
| shell+0x6f0 | IDirectDrawColorControl (QI on primary) | 005216f3 | failure only logged |
| shell+0x6f4 | IDirectDrawGammaControl (QI on primary) | 00521755 | null-guarded by every user (004a2810, 004a304a, 004a3566, 004c49d4) |
| shell+0xd20 | IDirect3DMaterial2 (viewport background) | 00521223 | |
| shell+0xd24 / +0xea0 | DDCAPS HAL / HEL, dwSize 0x17c (DX6 size) | `FUN_00520900` | |
| shell+0x101c / +0x1118 / +0x1214 | D3DDEVICEDESC HW / SW / pointer to the one used, size 0xfc (DX6 size) | 00520c93..00520cb3 | |
| shell+0x1220 / +0x1620 | up to 32 DDPIXELFORMATs / per-format bit info (0x3c each) | callback 00521d20 | |
| shell+0x1da0 / +0x1da4 / +0x1da8 / +0x1dac | opaque / opaque / blend / transparent texture formats | 005213c5.., `FUN_005218d0` | |
| `DAT_0098ea28` | primary surface | 004b1370 | |
| `DAT_0098f13c` | back buffer (attached, or offscreen in windowed) | 004b1370 | **CreateDevice target** |
| `DAT_0098f644` | IDirectDrawClipper (windowed only) | 004b1370 | |
| `DAT_005d6c04` | **IDirectDraw v1** (shell+4), not DD2 | `FUN_00509460` from 0052094e | texture manager |
| `DAT_005d6c08` | IDirect3DDevice2 | `FUN_00509470` from 005211a8 | texture manager |
| `DAT_005d6c0c` | IDirect3D2 from Device2::GetDirect3D, **Released at once, then still used** | `FUN_00509470` | CreateMaterial in 005095c0, 00509860 |
| `DAT_0059df74` | IDirect3DMaterial2 for untextured polys | `FUN_0047c6e0` | GetHandle at 0047cc60 |
| draw-list obj+0x26 | IDirect3DDevice2 copy | `FUN_0047cc60` | all DIP calls of the main renderer |
| `DAT_005d5304` | system-memory staging surface for landscape textures | `FUN_004e45d0` | Lock/Unlock/Blt at `FUN_004e4c50` |
| `DAT_005d5718` / `DAT_005d571c` | 32x32 cursor image / cursor save-under, `OFFSCREENPLAIN|SYSTEMMEMORY` (0x840) | `FUN_004fccb0` | wrappers 0xa68f80 / 0xa68fd8 |

Interfaces never referenced (no GUID, no call): IDirect3D (v1), IDirect3DDevice (v1),
IDirect3DExecuteBuffer, IDirect3DLight. The full GUID reference list is in section 8.

---

## 1. COM methods called (D3D build)

### IDirectDraw (v1, shell+4 and `DAT_005d6c04`)
| Method | Call sites | Notes |
|---|---|---|
| QueryInterface IID_IDirectDraw2 | 004b03ae | |
| QueryInterface IID_IDirect3D2 | 00520b8b, 0042f027 (enum) | |
| QueryInterface IID_IDirectDraw4 | 00520e67 | result used only for GetDeviceIdentifier, then Released (00521189). Failure is logged and ignored (00521195). |
| EnumDisplayModes(0,NULL,this,cb=0x4b0e80) | 004b041c area | |
| GetDisplayMode | 004b0290 (+0x30) | desktop mode for windowed |
| GetCaps(HAL,HEL) | 00520943 | |
| SetCooperativeLevel | 004b0fa0, `FUN_004b1330` | 0x11 fullscreen exclusive, 8 normal |
| SetDisplayMode(w,h,bpp) | 004b0fa0 | v1 3-arg form |
| RestoreDisplayMode | 004b0fa0, 004b05e0 | |
| CreateSurface | 004b1370 (primary/back), 00520ae1 (z), 0042f565 (sys-mem tex), 0042f5a0 (cursor), 00509700 (vid-mem tex), 00521b55 (PVRC probe), 0049cb30 (DX6 probe) | |
| CreatePalette | 00520d2c, 004b1970 | |
| CreateClipper | 004b1370 | windowed |
| AddRef/Release | 004b05b0 | prints the ref count, harmless |

### IDirectDraw2 (shell+8)
| GetAvailableVidMem(0x1000 TEXTURE / 0x10004000 LOCALVIDMEM|VIDEOMEMORY) | 004b5677, 004b4a69, 004b695a, 004e4654, 00500384 | logged only (004b567e) |
|---|---|---|

### IDirectDraw4 (temp)
| GetDeviceIdentifier(flags = card==PVR ? 0 : DDGDI_GETHOSTIDENTIFIER) | 00520e8f | vendor/device id select card quirks (section 2) |
|---|---|---|

### IDirectDrawSurface (primary, back, z, textures, staging)
| Method | Sites | Notes |
|---|---|---|
| GetAttachedSurface(DDSCAPS_BACKBUFFER) | 004b1370 (0x4b145d) | fullscreen flip chain |
| AddAttachedSurface(z) | 00520af6 | z-buffer onto back buffer |
| SetClipper | 004b16bd, 004b1b71 | windowed |
| SetPalette | 004b1970 (8-bit display), 0042f582 (only if format palettized), 00509700 | not reached for the 16-bit texture formats |
| GetSurfaceDesc | 00529740 (every wrapper), 004b1970, 004b1c00, 00509860, `FUN_004b1300` | |
| Lock(NULL, desc, DDLOCK_WAIT, 0) | `FUN_00529dd0` (generic wrapper; retries after Restore on SURFACELOST) | back buffer, cursor surface |
| Lock(.., 0x21 WAIT|WRITEONLY) | 004e4c50 (staging), 004e4d70 | |
| Lock(.., 0x821 WAIT|WRITEONLY|NOSYSLOCK) | 0042f800, 00429810, 004613a0, 00476cb0, 004b7de0 | texture fill |
| Unlock | `FUN_00529e60` and the paired sites above | |
| Blt COLORFILL (DDBLT_COLORFILL|WAIT 0x1000400) | `FUN_004b1c00` (primary+back), `FUN_00529940` | screen clear in 2D modes |
| Blt sys-mem tex to vid-mem tex (DDBLT_WAIT) | `FUN_00509b10`, 004e4ca3 | **texture upload path** |
| Blt back to primary (windowed present) | `FUN_004b0b20` (WM_PAINT) | ignores SURFACELOST/SURFACEBUSY |
| BltFast(.., flags ^ DDBLTFAST_WAIT) | `FUN_00529c70` | cursor save/draw, sprite copies |
| SetColorKey(DDCKEY_SRCBLT) | 00529c70 (cursor), 004878fa (vid-mem tex), 004607ed | |
| Flip(back, DDFLIP_WAIT) | `FUN_004b0ae0` | loops while DDERR_SURFACEBUSY or DDERR_WASSTILLDRAWING |
| IsLost | `FUN_00521c40`, `FUN_00529910` | |
| Restore | `FUN_00529910` | |
| QI IID_IDirectDrawSurface4, then PageLock(0) | 004607a2/b4, 00487526/38, 004b65af/c4, 004e46fd/4712, 00487769 | PageLock only if `DAT_005d54dc` (set when HAL caps lack DDCAPS2_NOPAGELOCKREQUIRED, 005217df) |
| QI IID_IDirectDrawSurface4 on primary | `FUN_0049cb30` (startup) | **must succeed**; failure shows a MessageBox and quits (004a3b96..004a3bd3) |
| QI IID_IDirectDrawSurface2 | 0049ce1f, 0049ce3b (`FUN_0049cc80`), 0057c126, 0057d44f.. | FMV player, outside the D3D path |
| QI IID_IDirect3DTexture2 | 005097a5, 0050995d, 00521bba | |

### IDirect3D2 (shell+0x644, `DAT_005d6c0c`)
| FindDevice(D3DFINDDEVICESEARCH{0x5c, D3DFDS_GUID, HAL}, result size 0x20c) | 00520c37, 0042f096 |
|---|---|
| CreateDevice(IID_IDirect3DHALDevice, back buffer, &dev) | 00520d77 |
| CreateViewport | 00521f25 |
| CreateMaterial | 00521223 (background), 005095c0, 00509860 (one per texture), 0047c6e0 (untextured) |

Not called: EnumDevices, CreateLight.

### IDirect3DDevice2 (shell+0x648, `DAT_005d6c08`, list obj+0x26)
| Method | Sites |
|---|---|
| GetCaps(HW,SW, both 0xfc) | 00509470 |
| AddViewport | 00521f3c |
| EnumTextureFormats(cb 0x521d20) | 005211e3 |
| BeginScene | `FUN_00522360` (game, logs SURFACELOST), `FUN_004284d0` (planet screen) |
| EndScene | `FUN_005223b0`, `FUN_00428570` |
| GetDirect3D | 00509470 |
| SetCurrentViewport | 005212fc, 00521fbb |
| SetRenderState | section 6 |
| SetLightState(D3DLIGHTSTATE_MATERIAL=1, hMat) | `FUN_00509820`, 0047ce50 (3 sites) |
| DrawPrimitive | 004285b4, 0042874b, 0042ef4a |
| DrawIndexedPrimitive | 0047d39d, 0047d3da, 0047d5be, 0047d5f9, 0047d69d, 0047d6df, 004284c8, 004287b7, 00429331, 0047f45b, 004e411d, 004e451f |

Not called anywhere: Begin/BeginIndexed/Vertex/Index/End, GetRenderState, SetTransform,
GetTransform, MultiplyTransform, GetLightState, SwapTextureHandles, GetStats, Next/DeleteViewport,
Set/GetRenderTarget, Set/GetClipStatus.

### Others
| Interface | Method | Sites |
|---|---|---|
| IDirect3DViewport2 | SetViewport2 (+0x44) | 00521fa8, 005220df (`FUN_00522010`, sub-rect from `FUN_00517220`) |
| | Clear (+0x30) | `FUN_005206c0` |
| | SetBackground (+0x20) | 00521fe8 |
| IDirect3DMaterial2 | SetMaterial (+0xc) | 0052127b, 005221b8, 00509700, 00509860 |
| | GetHandle (+0x14) | 00521fd7, 005095c0, 00509860, 0047cc60 |
| IDirect3DTexture2 | GetHandle(dev) (+0xc) only | 00509700, 00509860, 00521bf6 |
| IDirectDrawPalette | SetEntries | `FUN_005209c0` |
| | GetEntries | `FUN_0042f640`, `FUN_0042f800` (palette lookups for software conversion) |
| IDirectDrawClipper | SetHWnd | 004b16e2 |
| IDirectDrawGammaControl | GetGammaRamp(0, shell+0x6f8) | 00521782 |
| | SetGammaRamp | 00521a60 (restore on teardown), `FUN_004a3440` |

`IDirect3DTexture2::Load` is never called. Uploads go through `IDirectDrawSurface::Blt`.

---

## 2. Device selection

### Enumeration and suitability (`FUN_0042ef60`, startup at 004a4547)
1. `DirectDrawEnumerateA(cb 0x42f320)` stores up to 16 GUIDs. The NULL (primary) driver is stored as the all-zero GUID at 0x58fc78.
2. Per device: `DirectDrawCreate` (zero GUID passed as NULL), QI IID_IDirect3D2, `FindDevice` with HAL GUID.
3. **Accepted iff** (0042f0a1..0042f0d6), on the HW desc of the result:

| Field (D3DDEVICEDESC offset) | Required bit |
|---|---|
| dcmColorModel (+0x08) | non-zero |
| dpcTriCaps.dwTextureCaps (+0x84) | D3DPTEXTURECAPS_PERSPECTIVE (0x1) |
| dpcTriCaps.dwShadeCaps (+0x80) | D3DPSHADECAPS_ALPHAFLATBLEND (0x1000) |
| dpcTriCaps.dwTextureFilterCaps (+0x88) | D3DPTFILTERCAPS_LINEAR (0x2) |

At least one device must pass. With zero devices, `FUN_0042f190` leaves index -1 and `FUN_0042f4e0` returns a pointer before the table (**inferred** from the arithmetic).

### Registry (`FUN_0042f190`, `FUN_0042f440`)
Key `Software\Bullfrog Productions Ltd\Populous: The Beginning\Display`, value **`DDDevice`** is a 16-byte binary GUID. If the value is missing or does not match an enumerated GUID, the game picks the first device whose GUID is not the empty-string bytes (it skips the primary when a secondary card such as a Voodoo exists) and writes the value back.

### Caps the init path reads (`FUN_00520b60` / 00520e37.., from the chosen desc)
| Check | Field.bit | Effect | Site |
|---|---|---|---|
| HW vs SW desc | hw.dcmColorModel | +0x514 HW flag; SW desc used if 0 | 00520c77 |
| MODULATEALPHA | dwTextureBlendCaps 0x8 | +0xd08; enables TEXTUREMAPBLEND=MODULATEALPHA path | 00521060 |
| DECALALPHA | dwTextureBlendCaps 0x4 | +0xd0c (Rage Pro test) | 0052106f |
| Colorkey mode | DDCAPS.dwCaps 0x400000 COLORKEY + dwCKeyCaps 0x200 SRCBLT + dwTextureCaps 0x8 TRANSPARENCY | +0x6c8 bit 1 | 00521443 |
| Alpha blend mode | dwShadeCaps 0x1000 + dwTextureCaps 0x4 ALPHA | +0x6c8 bit 2 | 00521470 |
| Alpha test mode | dwAlphaCmpCaps 0x10 GREATER | +0x6c8 bit 4 | 00521491 |
| Clamp | dwTextureAddressCaps 0x4 | +0x6d4; else TEXTUREADDRESS never set to CLAMP | 005214b4 |
| Specular gouraud | dwShadeCaps 0x200 (forced off on PowerVR) | +0x6dc | 005214d7 |
| Color gouraud | dwShadeCaps 0x8 | +0x6e0 | 00521517 |
| Alpha gouraud | dwShadeCaps 0x4000 (forced off on Riva128) | +0x6e4 | 0052153a |
| Mipmap | dwTextureFilterCaps 0x10 LINEARMIPNEAREST | +0x6d8 | 005215b3 |
| Subpixel | dwRasterCaps 0x20 / 0x40 | +0xcf8/+0xcfc: 1/16 step if set, else whole pixel | 005215f8.. |
| Gamma | DDCAPS.dwCaps2 0x20000 PRIMARYGAMMA (+0x100000 CANCALIBRATEGAMMA) | QI GammaControl | 00521737 |
| Page lock | DDCAPS.dwCaps2 0x800 NOPAGELOCKREQUIRED absent | `DAT_005d54dc`=1, PageLock calls | 005217df |
| Alpha filtering | card id 1/4/6 | +0xd14 | 00521822 |
| Z depth | dwDeviceZBufferBitDepth: DDBD_16 > 24 > 32 > 8 | z-buffer bpp; none = error 1 | `FUN_00520a10` |
| Render depth | dwDeviceRenderBitDepth (desc+0x9c) | filters display modes, windowed check | 004b0efc, 004b1542 |

**Fatal**: "device does not support appropriate texture formats" (00521801) unless +0x1da0, +0x1da8, +0x1dac and +0x1da4 are all set. That needs 565 or 555 (opaque) from EnumTextureFormats; see section 4.

**PowerVR probe** `FUN_00521af0(DD, dev)`: CreateSurface 2x2 `DDSCAPS_TEXTURE` with FourCC `PVRC`. If CreateSurface or Lock fails, the card is not PowerVR. A fake device should fail this CreateSurface (unsupported FourCC).

### Display modes
| Rule | Site |
|---|---|
| EnumDisplayModes callback rejects width or height above 1600 | 004b0e86 |
| With D3D enabled, keeps only bpp present in dwDeviceRenderBitDepth (1:0x4000, 2:0x2000, 4:0x1000, 8:0x800, 16:0x400, 24:0x200, 32:0x100) | 004b0ea4..004b0efc |
| Max 64 modes; sorted by bpp, then w, then h, descending | 004b0f3f, cmp 004b0e20 |
| Resolution menu (`FUN_0049c570`): only bpp == 16 with w >= 512 and h >= 384 | 0049c570 |
| Unless `DAT_0089c66d & 4`, menu limited to 640x480 and 800x600 | 0049c570 |
| Requested mode must exist in the enumerated list, else "mode unavailable" | `FUN_004b12b0`, 004b2366 |
| Front end always 640x480x8. Planet screen 640x480x16. Game uses the menu mode (16 bpp). | 00457a60, 00410ee4, 00500580 |
| Windowed D3D only if desktop bpp is in dwDeviceRenderBitDepth and desktop width >= mode width, else forced fullscreen | 004b1542..004b155f |

Every switch between front end (2D) and 3D tears down and recreates the whole DirectDraw object
(`FUN_004b24f0` init 3D, `FUN_004b2540` deinit, both call `FUN_004b05b0` then `FUN_004b0290`).

---

## 3. Render target, z-buffer, viewport

Order in `FUN_004b0fa0` / `FUN_004b1370`: release old surfaces, SetCooperativeLevel, SetDisplayMode, create surfaces, palette setup (`FUN_004b1970`), clear (`FUN_004b1c00`), z-buffer (vt+0x20), 3D device (vt+0x18 = `FUN_00520cd0`).

| Surface | DDSD flags | Caps | Site |
|---|---|---|---|
| Primary, fullscreen | 0x21 CAPS|BACKBUFFERCOUNT, count 1 | PRIMARYSURFACE|FLIP|COMPLEX (0x218) | 3DDEVICE (0x2000 if D3D) | VIDEOMEMORY 0x4000 (HW) or SYSTEMMEMORY 0x800 | 004b1370 |
| Back, fullscreen | GetAttachedSurface(DDSCAPS_BACKBUFFER) | | 004b145d |
| Primary, windowed | 0x1 CAPS | PRIMARYSURFACE | 004b15c4 |
| Back, windowed | 0x7 CAPS|HEIGHT|WIDTH (w = +0xb8, h = +0xbc) | OFFSCREENPLAIN | 3DDEVICE | VIDEOMEMORY or SYSTEMMEMORY | 004b15f0.. |
| Z-buffer | 0x47 CAPS|HEIGHT|WIDTH|ZBUFFERBITDEPTH, same w/h | ZBUFFER|VIDEOMEMORY (0x24000) HW, ZBUFFER|SYSTEMMEMORY (0x20800) SW | `FUN_00520a10`, attached to back at 00520af6 |
| Clipper (windowed) | CreateClipper, primary SetClipper, SetHWnd | | 004b16bd..004b16e2 |

Pixel format of the render target is whatever the display mode gives (16 bpp in 3D). The 2D code reads the masks from GetSurfaceDesc (`FUN_00529740` copies ddpfPixelFormat, `FUN_00546d90` converts colors), so 565 works.

`FUN_00520cd0` (shell vt+0x18):
1. CreatePalette (0xc, zeroed) into +0x6c0 (00520d2c).
2. `CreateDevice(*(+0x518)=IID_IDirect3DHALDevice, DAT_0098f13c, +0x648)` (00520d77).
3. PowerVR probe, DD4 identifier, texture manager init (`FUN_00509470`), EnumTextureFormats.
4. CreateMaterial (+0xd20). If `DAT_005dd3e4` set: SetMaterial (diffuse 0,0,0,1, ramp 1).
5. `FUN_00521ef0`: CreateViewport, AddViewport, `SetViewport2{dwX=0,dwY=0,dwWidth=w,dwHeight=h, dvClipX=0, dvClipY=0, dvClipWidth=(float)w, dvClipHeight=(float)h, dvMinZ=0, dvMaxZ=1}`, SetCurrentViewport, Material GetHandle, Viewport SetBackground.
6. SetCurrentViewport again (005212fc), format selection, caps flags, gamma, initial render states (`FUN_005221e0`), cursor surfaces (`FUN_004fccb0`).

Note the clip rectangle is in screen units (0..w), not the usual -1..1 range. Only TL vertices are drawn, so treat the viewport as a scissor rectangle.

Clear calls (`FUN_005206c0(flags, rect)`; one rect = (0,0,w,h); ZBUFFER flag dropped when no z surface):

| Caller | Flags | When |
|---|---|---|
| `FUN_00517100` | 2 ZBUFFER | every game frame, right after BeginScene |
| `FUN_00524a30` | 1 TARGET, after `FUN_00522100(sky colour)` sets the background material | game frame (modes 0 and 2 of `DAT_0089c6c1`) |
| `FUN_004284d0` | 3 TARGET|ZBUFFER | planet screen frame |

`FUN_00522010` re-sets the viewport to a sub-rectangle (clamped to the screen, stores x/y at +0x6e8/+0x6ec). It is called from `FUN_00517220` with the 3D view size each game frame.

---

## 4. Texture pipeline

### Format selection (EnumTextureFormats callback 00521d20, analysis 00521d90, choice 00521327..)
The callback copies `ddpfPixelFormat` (DDSURFACEDESC+0x48) and returns CANCEL after 32 formats. For each format the game counts bits per channel (it does not check DDPF_RGB).

| Slot | Condition | Site |
|---|---|---|
| `DAT_00afc328` (8-bit palettized) | DDPF_PALETTEINDEXED8 | recorded, **never used for textures** |
| `DAT_00afc2f0` 565 | R5 G6 B5 | 0052137b |
| `DAT_00afc2ec` 555 | R5 G5 B5, A != 1 | 00521394 |
| `DAT_00afc2f8` 1555 | R5 G5 B5 A1 | 0052138c |
| `DAT_00afc2e8` 4444 | R4 G4 B4 A4 | 005213ad |

| Use | Choice | Site |
|---|---|---|
| +0x1da0, +0x1da4 opaque | 565, else 555 | 005213c5 |
| +0x6cc transparent mode | alpha test 4 > alpha blend 2 > colorkey 1 > 0; forced to colorkey when MODULATEALPHA is missing and colorkey exists | `FUN_005218d0` |
| +0x1dac transparent format | modes 4/2: 1555, else 4444, else falls back to opaque with mode 1/0 | 00521932.. |
| +0x6d0 blend mode | blend 2 > test 4 > colorkey 1 | 0052197f.. |
| +0x1da8 blend format | modes 4/2: 4444, else 1555, else opaque | 005219c3.. |

A fake HAL should expose at least **565, 1555 and 4444**, and advertise alpha test (GREATER) and alpha blend. Then +0x6cc = 4 (1555 sprites with alpha test) and +0x6d0 = 2 (4444 blended).

### Creation and upload
| Step | Detail | Site |
|---|---|---|
| Sys-mem source | `FUN_0042f4f0(DD, w, h, palette, pixfmt*, caps, out)` with DDSD 0x1007 and caps **0x1800 TEXTURE|SYSTEMMEMORY**. SetPalette only if format is palettized (never with the formats above). | callers 00428dbe, 00429837, 0047f01b, 004b5ff8, 004b6275, 004b658d, 004b664b, 004b6f44, 004b7e39, 004e46db |
| Sizes seen | 64x64 (00429837), 32x32 (004b6f44, 004b7e39); others from data | |
| Pixel fill | Lock (0x821) on the sys-mem surface, CPU conversion (palette lookups via GetEntries), Unlock | 0042f800, 00429810, 004b6e60, 004b7de0, 00476cb0 |
| Vid-mem texture (HW only) | `FUN_00509700`: DDSD 0x1007 same w/h/format, caps **0x5000 TEXTURE|VIDEOMEMORY**; with arg != 0: DDSD_MIPMAPCOUNT=arg, caps 0x405008 (+MIPMAP|COMPLEX) | 00509700 |
| Handle | QI IID_IDirect3DTexture2, `GetHandle(dev)`; a Material per texture, SetMaterial(white, ramp 16), GetHandle | 00509700, 00509860 |
| Upload | `Blt(vidTex, NULL, sysSurf, &srcRect, DDBLT_WAIT)` | `FUN_00509b10` |
| SW device | the sys-mem surface itself is the texture | 00509860 (`DAT_00a690b0`) |
| Landscape tiles | staging `DAT_005d5304` Lock 0x21, convert (`FUN_0042fe70`), Unlock, Blt into each dirty tile texture, up to 512 tiles per frame, **inside the scene** | `FUN_004e4c50` called at 00522b48, between BeginScene and EndScene |
| Colour key | `SetColorKey(DDCKEY_SRCBLT)` on vid-mem textures when the sprite bank uses colourkey | 004878fa |
| Bind | `SetRenderState(TEXTUREHANDLE, h)` then `SetLightState(MATERIAL, hMat)` | `FUN_00509820` |
| Untextured | `SetRenderState(TEXTUREHANDLE, 0)` + `SetLightState(MATERIAL, h of DAT_0059df74)` | 0047ce50 |
| Palette change | `D3D_OnPaletteChange` re-converts textures (BltFast at 004b6421) | `FUN_004b6320` |

Texture lifetime is linked in `DAT_005d6c00` (`FUN_00509630`, `FUN_00509a40`).

---

## 5. Draw path

Vertex type is always **D3DVT_TLVERTEX (3)**, 32-byte stride, filled exactly as D3DTLVERTEX:

| Offset | Field | Value written | Site |
|---|---|---|---|
| 0x00 | sx | screen x (float) | 004f9bc0 |
| 0x04 | sy | screen y | 004f9bc0 |
| 0x08 | sz | one z per primitive (object +0x14), copied to all vertices | 004f940d, 004fa1e4, 004fa504 |
| 0x0c | rhw | **1.0 constant** in every emitter found | 004f9bc0, 004f9416, 004fa1c6, 004fa4da |
| 0x10 | color | D3DCOLOR ARGB (alpha used for blending) | 004f9bc0 (+0x4e) |
| 0x14 | specular | D3DCOLOR (object +0x52) | 004f9bc0 |
| 0x18 / 0x1c | tu / tv | per corner | 004f9bc0 |

Emitter vtables: 0x58f50c (`FUN_004f9e00`), 0x58f514 (`FUN_004f9380`, tri), 0x58f520 (`FUN_004fa2e0`), 0x58f52c (`FUN_004f9bc0`, quad, optional rotation). With rhw 1.0 and TEXTUREPERSPECTIVE FALSE, texturing is affine. Depth order comes from z plus the game's 1024 sort buckets (`FUN_0047cc60`).

| Call | Primitive | Flags | Site |
|---|---|---|---|
| Main list flush: DIP(TRIANGLELIST 4 or LINELIST 2 when wireframe flag +0x248042, TLVERTEX, verts, n, WORD idx, n, flags obj+0x1e) | 4 / 2 | obj+0x1e; bit D3DDP_DONOTCLIP (4) toggled per batch (material flag 0x100) | 0047ce50, `FUN_0047d660` |
| Quads, 4 verts, 6 indices | 4 | 8 DONOTUPDATEEXTENTS | 004284c8, 00429331, 0047f45b |
| Sprite batch | 4 | 8 | 004287b7, 004e411d, 004e451f |
| DrawPrimitive POINTLIST (1) | 1 | 8 or 0xc (DONOTCLIP|DONOTUPDATEEXTENTS) | 004285b4, 0042874b, 0042ef4a |

Limits: index buffer up to 0x2000 indices per flush (004f9395). "Surface lost in drawip" is logged on SURFACELOST from DIP (0047d3a4).

Fog: **FOGENABLE (28) and the FOGTABLE states are never set.** Specular is enabled per batch (SPECULARENABLE), so specular RGB is added. Specular alpha has no effect.

---

## 6. Render states

D3D state ids: 1 TEXTUREHANDLE, 2 ANTIALIAS, 3 TEXTUREADDRESS, 4 TEXTUREPERSPECTIVE, 5 WRAPU, 6 WRAPV, 7 ZENABLE, 9 SHADEMODE, 14 ZWRITEENABLE, 15 ALPHATESTENABLE, 17 TEXTUREMAG, 18 TEXTUREMIN, 19 SRCBLEND, 20 DESTBLEND, 21 TEXTUREMAPBLEND, 22 CULLMODE, 23 ZFUNC, 24 ALPHAREF, 25 ALPHAFUNC, 26 DITHERENABLE, 27 ALPHABLENDENABLE, 29 SPECULARENABLE, 31 SUBPIXEL, 32 SUBPIXELX, 41 COLORKEYENABLE.

| State | Values used | Where |
|---|---|---|
| 1 TEXTUREHANDLE | handle, 0 | 00509820, 0047ce50, 004284a2, 00428590, 0042ef28, 004292df, 0047f435, 004e4174 |
| 2 ANTIALIAS | 0, or 2 SORTINDEPENDENT if `DAT_005d54cc` | 00517100 |
| 3 TEXTUREADDRESS | 3 CLAMP (init, only if clamp cap), 1 WRAP / 3 CLAMP per batch (flag 0x80) | 005221e0, 0047d848, 0047ce0b, 0042936e, 004e40d6 |
| 4 TEXTUREPERSPECTIVE | **0 only** | 00522299 |
| 5 WRAPU, 6 WRAPV | 0 | 005221e0, 00429384/9a |
| 7 ZENABLE | 1 | 005221e0, 00517100, 0047cca2, 004284d0 |
| 9 SHADEMODE | 2 GOURAUD; 1 FLAT per batch (flag 0x10) | 005221e0, 0047d7a0, 004284d0, 004291a0 |
| 14 ZWRITEENABLE | 1; 0/1 for translucent pass | 00517100, 0047ccaf, 0047d213, 0047d64d |
| 15 ALPHATESTENABLE | 0; 1 per batch (flag 0x2) | 00517100, 0047d74d, 004e3acd |
| 17/18 TEXTUREMAG/MIN | 2 LINEAR; 1 NEAREST per batch (flag 0x20, not on PowerVR); PowerVR 1/2 by `DAT_005d54c8` | 0047cde8, 0047d7e4.., 004284d0, 0047f2df, 004292f5 |
| 19 SRCBLEND | 5 SRCALPHA | 005221e0, 004e3a5f, 004e4125 |
| 20 DESTBLEND | 6 INVSRCALPHA; 2 ONE (additive, flag 0x200) | 005221e0, 0047ce16, 0047d884 |
| 21 TEXTUREMAPBLEND | 2 MODULATE; 4 MODULATEALPHA per batch (flag 0x40), both only if MODULATEALPHA cap | 005221e0, 0047ce04, 0047d82a |
| 22 CULLMODE | 1 NONE | 005221e0, 004293c6 |
| 23 ZFUNC | 4 LESSEQUAL | 0047ccc9 |
| 24 ALPHAREF | 0x7f (init, +0x6c4), 0x7f / 0 per batch | 0052230e, 0047d754 |
| 25 ALPHAFUNC | 5 GREATER | 005222f9 |
| 26 DITHERENABLE | 1 | 00522210 |
| 27 ALPHABLENDENABLE | 1 init; 0 opaque pass, 1 translucent pass | 005221e0, 00517100, 0047ccbc, 0047d207, 00429188 |
| 29 SPECULARENABLE | 0 init; per batch (flag 0x8) | 00522200, 0047d7b9, 004e40eb |
| 31 SUBPIXEL, 32 SUBPIXELX | 1 | 00522230, 00522220 |
| 41 COLORKEYENABLE | 0; per batch (flag 0x4) | 005222a9, 0047d782, 004e3ab7 |

Per-batch material flag word (`FUN_0047d6f0`, state change only on diff): 0x2 alpha test, 0x4 colour key, 0x8 specular, 0x10 flat, 0x20 point filter, 0x40 MODULATEALPHA, 0x80 wrap, 0x100 DONOTCLIP, 0x200 additive.

| Other | |
|---|---|
| SetLightState | only D3DLIGHTSTATE_MATERIAL (1) with a material handle (00509820, 0047ce50). Lighting is irrelevant for TL vertices; accept and ignore. |
| GetRenderState, SetTransform | never called |

---

## 7. Frame composition (decides GPU readback)

`DAT_005da078` is the "inside 3D scene" switch. 0 means 2D primitives (text, sprites, glyphs) become D3D quads. 1 means they are drawn by the CPU into a locked surface. Dispatchers: `FUN_005162e0`, `FUN_00516370`, `FUN_00516430`, `FUN_005168e0`, `FUN_00516d30`, `FUN_00451ba0`, `FUN_00452610`. Set to 0 at 00522624 / 004284d5, to 1 at 00522b32 / 004286c1.

### In-game frame (main loop `FUN_004a4450`, state 2, `FUN_004a4960`)
| # | Action | Surface | Site |
|---|---|---|---|
| 1 | `DAT_005da078 = 0` | | 00522624 |
| 2 | BeginScene, Clear(ZBUFFER), initial states | back (RT) | `FUN_00517100` |
| 3 | Sky colour + Clear(TARGET), SetViewport2 sub-rect | RT | `FUN_00524a30` (0052264e/005226dc), `FUN_00517220` (00522676/00522704) |
| 4 | World, objects, **HUD panel, minimap, sprites and text queued as D3D quads** | | 00522660..00522b2c |
| 5 | `DAT_005da078 = 1` | | 00522b32 |
| 6 | Landscape texture uploads: Lock sys-mem staging, Blt to vid-mem textures (if `DAT_005ce0c4 == 0`) | textures | `FUN_004e4c50` at 00522b48 |
| 7 | Sort and flush: SetRenderState / SetLightState / DIP | RT | `FUN_005171d0` -> `FUN_0047cc60`, `FUN_0047ce50` |
| 8 | EndScene | | `FUN_00517200` -> `FUN_005223b0` |
| 9 | Take cursor semaphore, **Lock back buffer** (DDLOCK_WAIT, whole surface), set it as the CPU draw target | **RT** | 004a4c29..004a4c50 |
| 10 | CPU-draw FPS text if `DAT_0089c671 & 8`, and debug/network text if `DAT_0089c661` bit 16 | RT | `FUN_0047ffc0`, `FUN_00480520` |
| 11 | **Unlock** back buffer, release semaphore | RT | 004a4c86, 004a4c95 |
| 12 | Cursor composite (`FUN_004fd370`): **BltFast back -> sys-mem save-under** (32x32 at cursor), then **BltFast cursor -> back** with DDBLTFAST_SRCCOLORKEY | RT read, RT write | 004fd469, 004fd4d9 |
| 13 | Flip(back, WAIT) fullscreen, or InvalidateRect then WM_PAINT Blt back -> primary windowed | | `FUN_004b0ae0`, `FUN_004b0b20` |

Step 9 runs every frame. `DAT_005cd91c` skips it, and it is set only from a debug variable (`DAT_00749930` at 004b7896). The game counts these locks ("BackBuffer Locks" at 004b7760).

### Asynchronous cursor moves
Callback `FUN_004fd590` is registered for event type 5 on the dispatcher at 0xd05e58 (`FUN_004fc9a0` -> `FUN_0052bfb0`, which takes a critical section). It does the following on the **front buffer (primary, `FUN_004b2660`)** under the semaphore 0xa68f78:
1. BltFast save-under -> front (restore the old spot).
2. BltFast front -> save-under.
3. BltFast cursor -> front with colour key.

So the cursor is drawn directly on the visible surface between flips. Whether event 5 fires on another thread is **inferred** from the critical section and the semaphore. The cursor image itself is CPU-drawn into the 32x32 sys-mem surface (`FUN_004fca00`: Lock, `FUN_0052a7e0`, Unlock).

### Planet screen (`FUN_00410e20`, 640x480x16, D3D)
`FUN_004110e0` -> `FUN_004284d0` (BeginScene, Clear TARGET|Z, states, `DAT_005da078=0`). Then sprites and points as D3D, then `FUN_00428570` (DrawPrimitive points, quads, EndScene, `DAT_005da078=1`). No back-buffer Lock in this state. The cursor composite and Flip are the same as the game (steps 12 and 13).

### Front end and other 2D states
`FUN_00457a60` (front end) deinitialises 3D (`FUN_004b2540`) and sets **640x480x8**. Everything there is CPU-drawn into the locked back buffer (`FUN_00529dd0` callers 0042ab50, 00457a60, 00480100, 00480400, 004a3ea0, 004a4df0, 004a9380, 0052d840). `FUN_004a4df0` and `FUN_004a9380` (other main-loop states) do Blt colour fill, Lock, CPU text and graphics, then Unlock, in whatever mode is active, possibly the 16-bit 3D mode. No BeginScene caller was found in them.

### Consequences for an SDL_GPU backend
1. The render target must support **CPU Lock/Unlock after EndScene every game frame**. A CPU draw can read the destination (blended text), so Lock needs the GPU image. The cheapest correct option is a synchronous download of the RT on Lock and an upload of the full surface on Unlock (640x480x2 = 600 KB each way per frame). An optimization (**inferred**, needs measuring): compare the buffer against the downloaded copy on Unlock and skip the upload when nothing changed, which is the normal case without debug flags.
2. **BltFast RT -> sys-mem** (32x32 save-under) happens every frame, and also from the front buffer on cursor moves. It needs a small region readback or a CPU shadow of what was presented.
3. **BltFast sys-mem -> RT/front with source colour key** must work on the GPU surface. An alternative is to emulate the cursor as an overlay at present time and still honour the save-under contents (**inferred** design choice).
4. Blt sys-mem -> vid-mem texture **inside a scene** must work (step 6).
5. In the 8-bit front end, DirectDraw is the existing software path. There is no D3D device there.

---

## 8. Other things a DX5 implementation must get right

| Item | Detail | Site |
|---|---|---|
| Struct sizes are DX6 | DDCAPS 0x17c, D3DDEVICEDESC 0xfc, D3DFINDDEVICERESULT 0x20c; DDSURFACEDESC 0x6c | 00520900, 00509470, 00520c18 |
| DX6 interfaces probed | QI IDirectDraw4 (optional), IDirectDrawSurface4 on primary at startup (**required**), Surface4 on textures (QI result unchecked, PageLock only if page-lock flag) | 00520e67, 0049cb30, 004b65af |
| Report DDCAPS2_NOPAGELOCKREQUIRED | avoids PageLock (0x94) on Surface4 | 005217df |
| Surface4 vtable | must reach index 37 PageLock if that flag is absent, or QI can return an object with Release only if the flag is present | |
| GetDeviceIdentifier | optional, sets card quirks; returning E_NOINTERFACE from QI DD4 is safe | 00521195 |
| ColorControl / GammaControl QI | failure is safe (logged, pointers null-guarded) | 005216f9, 005217ad |
| DDERR_SURFACELOST (0x887601C2) | Lock wrapper calls IsLost/Restore and retries; BeginScene, EndScene and DIP treat it as non-fatal | 00529dd0, 00522360, 00517100, 0047d3a4 |
| Lost-surface recovery | `FUN_00521c40` IsLost on back/primary/z, then a full recreate; checks DDERR_OUTOFVIDEOMEMORY / E_OUTOFMEMORY | 00521c40 |
| Flip | Flip(back, DDFLIP_WAIT=1); retries on DDERR_SURFACEBUSY (0x887601AE) and DDERR_WASSTILLDRAWING (0x8876021C) | 004b0ae0 |
| Return checks | most calls test `!= 0` (DD_OK only) or `< 0`; CreateDevice, FindDevice and EnumTextureFormats failures abort init | 00520c3c, 00520d7c, 005211e8 |
| Re-entrancy | DirectDraw is created and destroyed on every front end <-> 3D switch and at startup probes (0042ef60 per device, 0049cb30, 005002e0) | 004b05b0, 004b0290 |
| D3D2 from GetDirect3D | Released immediately, then used; keep it alive via the shell's own reference | 00509470 |
| Colour keys | SetColorKey(DDCKEY_SRCBLT) on textures with COLORKEYENABLE, and on BltFast sources (DDBLTFAST_SRCCOLORKEY) | 004878fa, 00529c70 |
| Windowed present | Blt(primary, client screen rect, back, NULL-origin rect, DDBLT_WAIT) on WM_PAINT; uses the clipper | 004b0b20 |
| Display palette | 8-bit modes only: CreatePalette 0xc, SetPalette on back and primary, re-set on WM_ACTIVATE | 004b1970, 004b0870 |
| SetDisplayMode | v1 3-argument form on the IDirectDraw v1 object | 004b0fa0 |
| Viewport | SetViewport2 with screen-unit clip rect; Clear uses the background material colour (SetMaterial diffuse = 0x00RRGGBB / 255) | 00521ef0, 00522100 |
| GUIDs referenced | DD2 b3a6f3e0 (0x58fee8), DD4 9c59509a (0x58fef8), Surface2 57805885 (0x58ff18), Surface4 0b2b8630 (0x58ff38), ColorControl 4b9f0ee0 (0x58ffc8), GammaControl 69c11c3e (0x58ffd8), Direct3D2 6aae1ec1 (0x590208), HALDevice 84e63de0 (0x590248), Texture2 93281502 (0x5902c8) | relocation scan of 0x58fe00..0x590400 |
| Logging | everything goes to `c:\d3d_pop3.txt` via `FUN_004574d0` when `D3D_DEBUG` is set (`FUN_004a5310`); useful as a trace oracle | |

## Open points (not verified)
- The terrain may have its own emitter beyond the four vtables listed in section 5. All DIP/DP call sites were enumerated, so any such emitter must feed `FUN_0047ce50`. Only the four vtables above were checked for the rhw value.
- The mipmap-count argument of `FUN_00509700` for callers other than `D3D_InitMap` (004b65df passes `+0x6d8 != 0`).
- Initial value of the DIP flags word obj+0x1e before `FUN_0047cc60` clears bit 4.
- Which thread fires cursor event type 5.
