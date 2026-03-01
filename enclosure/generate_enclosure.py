"""
UniFi Traffic Monitor - ESP32 + SH1106 OLED Enclosure Generator
Generates two STL files:
  box_body.stl  - main enclosure (front + 4 walls, open back)
  box_lid.stl   - back lid with inset lip

Component references:
  ESP32 DevKit v1 (30-pin): 51.4 x 28.3 x 1.6mm PCB, ~10mm component height
    - micro-USB on one short end (8mm wide x 4.5mm tall), centered
    - BOOT button: 4x4mm tact, on long edge near USB end
    - EN   button: 4x4mm tact, on short face near USB end
    - Blue LED: near USB end
  SH1106 0.96" OLED PCB: 27 x 27mm, ~4.5mm total thickness
    - Active display: 21.7 x 11.4mm, centered horizontally, ~3mm from top edge
    - 4-pin header on one short edge (bottom when mounted)

Mounting strategy (screws inserted from INSIDE the box):
  OLED:
    - Bottom edge of PCB slides into a groove cut into the inner front face
      (slide in from the right / +X side — the groove is open on that wall)
    - Top edge rests on 2x short bosses; M2 self-tap screws driven from
      inside the cavity down into blind pilot holes in each boss
  ESP32:
    - 4x tall standoffs grow from the inner front face (~9 mm), placing the
      ESP32 PCB right behind the OLED assembly
    - 4x M2 self-tap screws driven from inside the cavity down into blind
      pilot holes in each standoff — front face stays completely smooth
  LEDs:
    - 2x LED peek-holes on the front face (right side, clear of standoffs)
"""

import cadquery as cq
import os

OUT_DIR = os.path.dirname(os.path.abspath(__file__))

# ─────────────────────────────────────────────
#  PARAMETERS  (all mm)
# ─────────────────────────────────────────────
WALL  = 2.0    # wall thickness
FLOOR = 2.0    # front-panel thickness

# Interior cavity
INT_W = 55.0   # X — ESP32 51.4 mm + 1.8 mm clearance each side
INT_H = 32.0   # Y — ESP32 28.3 mm + ~1.85 mm clearance each side
INT_D = 24.0   # Z — OLED boss(2) + OLED(4.5) + gap(2.5) + ESP32 comps(10) + PCB+standoff(9)

# Exterior (auto)
EXT_W = INT_W + 2 * WALL   # 62 mm
EXT_H = INT_H + 2 * WALL   # 38 mm
EXT_D = INT_D + FLOOR       # 26 mm  (front wall + interior, back open)

# Inner front-face surface Z coord (box centred at origin, front at -EXT_D/2)
Z_INNER_FRONT = -EXT_D / 2 + FLOOR   # = -11.0

# ── OLED display window ──
OLED_WIN_W  = 23.0   # cut-out width  (active area 21.7 mm, small border)
OLED_WIN_H  = 13.0   # cut-out height (active area 11.4 mm)
OLED_WIN_X  =  0.0   # centred horizontally
OLED_WIN_Y  =  1.5   # slightly up from box centre (INT_H now 32 mm, walls at ±16)

# ── OLED PCB dimensions ──
OLED_PCB_W  = 27.5
OLED_PCB_H  = 27.5
OLED_PCB_X  = OLED_WIN_X
OLED_PCB_Y  =  0.5   # PCB centre slightly up (PCB spans ±13.75, walls at ±16 → 2.25 mm gap)

# ── OLED slide-in groove (bottom edge of PCB, open on right wall) ──
#   The PCB bottom edge (at OLED_PCB_Y - OLED_PCB_H/2) slides into this channel
#   from the +X side.  The groove runs the full box width so the opening faces
#   the right end wall.
GROOVE_SLOT_H    = 2.2    # slightly more than PCB thickness 1.6 mm
GROOVE_SLOT_D    = 3.0    # depth into front face interior (Z direction)
GROOVE_Y_CENTER  = OLED_PCB_Y - OLED_PCB_H / 2   # = 0.5 - 13.75 = -13.25

# ── OLED screw bosses (2×, top of PCB, from inner front face) ──
OLED_BOSS_H      = 2.0    # short boss — OLED PCB rests on these
OLED_BOSS_DIA    = 5.0
OLED_BOSS_HOLE   = 2.2    # M2 self-tap clearance through front face + boss
OLED_BOSS_X      = 9.5    # ± from centre (inside PCB top-corner zone)
OLED_BOSS_Y      = OLED_PCB_Y + OLED_PCB_H / 2 - 4.0   # = 0.5 + 13.75 - 4 = 10.25

# ── ESP32 tall standoffs (4×, from inner front face) ──
#   Height chosen so ESP32 PCB sits at Z_INNER_FRONT + ESP32_BOSS_H = -2 mm
#   OLED back face ≈ Z_INNER_FRONT + OLED_BOSS_H + 4.5 = -4.5 mm
#   Gap between OLED back and ESP32 front ≈ 2.5 mm (clears I²C header & cable)
ESP32_BOSS_H     =  9.0
ESP32_BOSS_DIA   =  6.0
ESP32_BOSS_HOLE  =  2.2   # M2 self-tap
ESP32_MOUNT_POS  = [       # (X, Y) — pushed toward +X (USB/right wall), within ±27.5 / ±16
    ( 23.0,  12.0),   # right-top    (standoff edge at X=26, 1.5 mm from inner wall)
    (-22.0,  12.0),   # left-top
    ( 23.0, -12.0),   # right-bottom
    (-22.0, -12.0),   # left-bottom
]

# ── LED peek-holes on front face ──
#   Right-of-OLED (OLED spans X ±11.5), clear of standoff at (23, ±12)
LED_DIA  = 3.0
LED_HOLES = [
    ( 24.0,  4.0),   # right side, slightly above centre
    ( 24.0, -4.0),   # right side, slightly below centre
]

# ── micro-USB slot, right end face (+X wall) ──
#   Z aligned to ESP32 PCB surface = FLOOR + ESP32_BOSS_H from front face
USB_W        = 10.0
USB_H        =  6.0
USB_Y_OFFSET =  0.0
USB_Z        = -EXT_D / 2 + FLOOR + ESP32_BOSS_H   # = -2 mm from box centre
# (BOOT and EN button holes removed — only USB cut on side wall)

# ── Blind pilot holes (screws from inside) ──
SCREW_PILOT      = 2.0    # M2 self-tap pilot diameter
OLED_BLIND_DEPTH = OLED_BOSS_H - 0.5          # 1.5 mm — leaves 0.5 mm floor in boss
ESP32_BLIND_DEPTH = ESP32_BOSS_H - 1.0        # 8.0 mm — leaves 1.0 mm floor in standoff

# ── Common fastener / cosmetic ──
FILLET_R    = 2.5
SCREW_DIA   = 2.4   # M2 clearance (lid corners)
LID_THICK   = 2.5
LID_OVERLAP = 1.5


# ─────────────────────────────────────────────
#  BOX BODY
# ─────────────────────────────────────────────

def make_body():
    # ── Outer shell ──
    outer = (
        cq.Workplane("XY")
        .box(EXT_W, EXT_H, EXT_D, centered=(True, True, True))
    )

    # ── Interior cavity (leaves FLOOR on front) ──
    inner = (
        cq.Workplane("XY")
        .box(INT_W, INT_H, INT_D, centered=(True, True, True))
        .translate((0, 0, FLOOR / 2))
    )
    body = outer.cut(inner)

    # ── Open back face entirely ──
    back_opening = (
        cq.Workplane("XY")
        .box(INT_W, INT_H, WALL * 2, centered=(True, True, True))
        .translate((0, 0, EXT_D / 2))
    )
    body = body.cut(back_opening)

    # ── Front face: OLED display window ──
    oled_win_cut = (
        cq.Workplane("XY")
        .box(OLED_WIN_W, OLED_WIN_H, FLOOR + 2, centered=(True, True, True))
        .translate((OLED_WIN_X, OLED_WIN_Y, -(EXT_D / 2)))
    )
    body = body.cut(oled_win_cut)

    # ── Front face + inner cavity: OLED slide-in groove ──
    #   Runs full box width (+X direction is open) so the PCB bottom edge
    #   can be slid in from the right side before the lid is fitted.
    groove_cut = (
        cq.Workplane("XY")
        .box(EXT_W + 2, GROOVE_SLOT_H, GROOVE_SLOT_D, centered=(True, True, True))
        .translate((0, GROOVE_Y_CENTER, Z_INNER_FRONT + GROOVE_SLOT_D / 2))
    )
    body = body.cut(groove_cut)

    # ── Inner front face: OLED screw bosses (2×) ──
    for sx in (OLED_BOSS_X, -OLED_BOSS_X):
        boss = (
            cq.Workplane("XY")
            .cylinder(OLED_BOSS_H, OLED_BOSS_DIA / 2)
            .translate((sx, OLED_BOSS_Y, Z_INNER_FRONT + OLED_BOSS_H / 2))
        )
        body = body.union(boss)

    # ── OLED bosses: blind pilot holes from cavity side (screw from inside) ──
    #   Hole opens at top of boss, stops 0.5 mm before the front face floor.
    for sx in (OLED_BOSS_X, -OLED_BOSS_X):
        hole = (
            cq.Workplane("XY")
            .cylinder(OLED_BLIND_DEPTH, SCREW_PILOT / 2)
            .translate((sx, OLED_BOSS_Y,
                        Z_INNER_FRONT + 0.5 + OLED_BLIND_DEPTH / 2))
        )
        body = body.cut(hole)

    # ── Inner front face: ESP32 tall standoffs (4×) ──
    for ex, ey in ESP32_MOUNT_POS:
        standoff = (
            cq.Workplane("XY")
            .cylinder(ESP32_BOSS_H, ESP32_BOSS_DIA / 2)
            .translate((ex, ey, Z_INNER_FRONT + ESP32_BOSS_H / 2))
        )
        body = body.union(standoff)

    # ── ESP32 standoffs: blind pilot holes from cavity side (screw from inside) ──
    #   Hole opens at top of standoff, stops 1 mm before the front face floor.
    for ex, ey in ESP32_MOUNT_POS:
        hole = (
            cq.Workplane("XY")
            .cylinder(ESP32_BLIND_DEPTH, SCREW_PILOT / 2)
            .translate((ex, ey,
                        Z_INNER_FRONT + 1.0 + ESP32_BLIND_DEPTH / 2))
        )
        body = body.cut(hole)

    # ── Front face: LED peek-holes ──
    for lx, ly in LED_HOLES:
        led_hole = (
            cq.Workplane("XY")
            .cylinder(FLOOR + 2, LED_DIA / 2)
            .translate((lx, ly, -(EXT_D / 2)))
        )
        body = body.cut(led_hole)

    # ── Right end face: micro-USB slot ──
    usb_slot = (
        cq.Workplane("YZ")
        .box(USB_H, WALL + 2, USB_W, centered=(True, True, True))
        .translate((EXT_W / 2, USB_Y_OFFSET, USB_Z))
    )
    body = body.cut(usb_slot)

    # ── Corner fillets ──
    try:
        body = body.edges("|Z").fillet(FILLET_R)
    except Exception:
        pass

    return body


# ─────────────────────────────────────────────
#  LID  (back plate with inset lip only — no bosses needed)
# ─────────────────────────────────────────────

def make_lid():
    lid = (
        cq.Workplane("XY")
        .box(EXT_W, EXT_H, LID_THICK, centered=(True, True, True))
    )

    # Inset lip fits inside box back opening
    lip = (
        cq.Workplane("XY")
        .box(INT_W - 0.4, INT_H - 0.4, LID_OVERLAP, centered=(True, True, True))
        .translate((0, 0, (LID_THICK + LID_OVERLAP) / 2))
    )
    lid = lid.union(lip)

    # Four corner screw holes in lid (align with box-corner lid-retention if desired)
    lid_screw_offsets = [
        ( INT_W / 2 - 4,  INT_H / 2 - 4),
        (-INT_W / 2 + 4,  INT_H / 2 - 4),
        ( INT_W / 2 - 4, -INT_H / 2 + 4),
        (-INT_W / 2 + 4, -INT_H / 2 + 4),
    ]
    for bx, by in lid_screw_offsets:
        hole = (
            cq.Workplane("XY")
            .cylinder(LID_THICK + LID_OVERLAP + 1, SCREW_DIA / 2)
            .translate((bx, by, 0))
        )
        lid = lid.cut(hole)

    # Corner fillets
    try:
        lid = lid.edges("|Z").fillet(FILLET_R)
    except Exception:
        pass

    return lid


# ─────────────────────────────────────────────
#  EXPORT
# ─────────────────────────────────────────────

print("Building body...")
body = make_body()
body_path = os.path.join(OUT_DIR, "box_body.stl")
cq.exporters.export(body, body_path)
print(f"  Saved: {body_path}")

print("Building lid...")
lid = make_lid()
lid_path = os.path.join(OUT_DIR, "box_lid.stl")
cq.exporters.export(lid, lid_path)
print(f"  Saved: {lid_path}")

print("Done.")
print("")
print("Assembly notes:")
print(f"  Box exterior:    {EXT_W:.0f} x {EXT_H:.0f} x {EXT_D:.0f} mm (W x H x D)")
print(f"  Interior cavity: {INT_W:.0f} x {INT_H:.0f} x {INT_D:.0f} mm")
print(f"  OLED window:     {OLED_WIN_W:.0f} x {OLED_WIN_H:.0f} mm  (front face)")
print(f"  OLED slide groove: Y={GROOVE_Y_CENTER:.1f} mm, opens on RIGHT end face")
print(f"  OLED screw bosses: 2× M2 at X=±{OLED_BOSS_X:.1f}, Y={OLED_BOSS_Y:.2f}, blind depth={OLED_BLIND_DEPTH:.1f} mm")
print(f"  ESP32 standoffs:   4× M2 pushed toward USB wall, H={ESP32_BOSS_H:.0f} mm, blind depth={ESP32_BLIND_DEPTH:.1f} mm")
print(f"    positions: {ESP32_MOUNT_POS}")
print(f"  LED holes:         {LED_DIA:.0f} mm dia × {len(LED_HOLES)}  (front face, right of OLED window)")
print(f"  USB slot:          {USB_W:.0f} x {USB_H:.0f} mm  (right end face only — BOOT/EN button holes removed)")
print(f"  Screws inserted from INSIDE the box (blind M2 pilot holes in bosses/standoffs)")
