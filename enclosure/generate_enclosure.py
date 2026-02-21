"""
UniFi Traffic Monitor - ESP32 + SH1106 OLED Enclosure Generator
Generates two STL files:
  box_body.stl  - main enclosure (front + 4 walls, open back)
  box_lid.stl   - back lid with screw posts

Component references:
  ESP32 DevKit v1 (30-pin): 51.4 x 28.3 x 1.6mm PCB, ~10mm component height
    - micro-USB on one short end (8mm wide x 4.5mm tall), centered
    - BOOT button: 4x4mm tact, on long edge near USB end
    - EN   button: 4x4mm tact, on short face near USB end
    - Blue LED: near USB end
  SH1106 0.96" OLED PCB: 27 x 27mm, ~4.5mm total thickness
    - Active display: 21.7 x 11.4mm, centered horizontally, ~3mm from top edge
    - 4-pin header on one short edge (bottom when mounted)
"""

import cadquery as cq
import os

OUT_DIR = os.path.dirname(os.path.abspath(__file__))

# ─────────────────────────────────────────────
#  PARAMETERS  (all mm)
# ─────────────────────────────────────────────
WALL  = 2.0    # wall thickness
FLOOR = 2.0    # front-panel thickness (where OLED sits)

# Interior cavity
INT_W = 58.0   # X  — fits ESP32 52mm + 6mm clearance
INT_H = 34.0   # Y  — fits ESP32 28mm + 6mm clearance
INT_D = 24.0   # Z  — OLED (5mm) + gap (4mm) + ESP32 comps (10mm) + PCB+standoff (5mm)

# Exterior (auto)
EXT_W = INT_W + 2 * WALL    # 62mm
EXT_H = INT_H + 2 * WALL    # 38mm
EXT_D = INT_D + FLOOR        # 26mm  (front wall + interior, back is open)

# OLED window in front face
# Active display 21.7 x 11.4mm; cut slightly larger with a clean border
OLED_WIN_W  = 23.0   # window width
OLED_WIN_H  = 13.0   # window height
# Centred horizontally and shifted up inside the front face
OLED_WIN_X  =  0.0   # lateral offset from box centre
OLED_WIN_Y  =  6.0   # upward offset from box centre (display in upper half)

# OLED PCB ledge — a thin lip inside the front face the PCB rests against
OLED_PCB_W  = 27.5   # PCB pocket width
OLED_PCB_H  = 27.5   # PCB pocket height
OLED_PCB_X  = OLED_WIN_X
OLED_PCB_Y  = OLED_WIN_Y - 1.0  # centred on pcb (pcb slightly larger than window area)
OLED_POCKET_DEPTH = 1.5          # recessed ledge depth

# LED peek-hole on front face (right side, since OLED is centred)
LED_DIA     =  3.0
LED_X       = 20.0   # from box centre (+X = right)
LED_Y       = -8.0   # from box centre (-Y = lower half)

# micro-USB slot on the RIGHT short end face (+X face after placement)
# ESP32 USB is 8mm wide x 4.5mm tall; add clearance
USB_W       = 10.0   # slot width  (left-right on that face)
USB_H        =  6.0   # slot height (up-down on that face)
USB_Y_OFFSET =  0.0   # push toward +Y (centred)
# Z position of USB slot from FRONT face of box:
#   The USB is on the short end of the ESP32 which sits ~3mm from the back
#   Distance from front face = FLOOR + gap + (INT_D - ESP32_LENGTH/2) ≈
#   Place the slot opening at the full right face; depth cut = WALL (goes through)
# The slot is centred in the face height but offset toward the back
USB_Z        = EXT_D / 2   # centred in depth — just cut all the way through the right wall

# BOOT button hole — right end face, lower portion, closer to front
BOOT_X       = EXT_W / 2             # right face (+X)
BOOT_Y_OFF   = -8.0                   # offset down from centre
BOOT_Z       = -EXT_D / 2 + FLOOR + INT_D * 0.20  # from front — near the main PCB area
BTN_DIA      =  5.0

# EN (reset) button hole — right end face, lower portion, slightly further back
EN_Y_OFF     = -8.0
EN_Z         = -EXT_D / 2 + FLOOR + INT_D * 0.45

# Corner fillet
FILLET_R     = 2.5

# Lid (back plate)
LID_THICK    = 2.5
LID_OVERLAP  = 1.5   # how far lid inset fits inside the box walls
SCREW_DIA    = 2.4   # M2 screw clearance
BOSS_DIA     = 6.0   # boss outer diameter
BOSS_H       = 4.0   # boss height inside box (screw posts on lid protrude into box)

# ─────────────────────────────────────────────
#  BOX BODY
# ─────────────────────────────────────────────

def make_body():
    # Outer solid — origin centred at box centre, front face at -Z=EXT_D/2
    outer = (
        cq.Workplane("XY")
        .box(EXT_W, EXT_H, EXT_D, centered=(True, True, True))
    )

    # Interior cavity — cut from BACK face, leaving FLOOR on front
    inner = (
        cq.Workplane("XY")
        .box(INT_W, INT_H, INT_D, centered=(True, True, True))
        # shift toward back so front wall (FLOOR thick) remains
        .translate((0, 0, FLOOR / 2))
    )
    body = outer.cut(inner)

    # Open the BACK face entirely (clean back opening for lid)
    back_opening = (
        cq.Workplane("XY")
        .box(INT_W, INT_H, WALL * 2, centered=(True, True, True))
        .translate((0, 0, EXT_D / 2))
    )
    body = body.cut(back_opening)

    # ── Front face: OLED window ──
    oled_window = (
        cq.Workplane("XZ")
        .center(OLED_WIN_X, -EXT_D / 2)
        .rect(OLED_WIN_W, FLOOR * 2 + 1)
        .extrude(OLED_WIN_H, combine=False)
        .translate((0, OLED_WIN_Y, 0))
    )
    # Simpler: just cut a rectangular prism through front face
    oled_win_cut = (
        cq.Workplane("XY")
        .box(OLED_WIN_W, OLED_WIN_H, FLOOR + 2, centered=(True, True, True))
        .translate((OLED_WIN_X, OLED_WIN_Y, -(EXT_D / 2)))
    )
    body = body.cut(oled_win_cut)

    # ── Front face: OLED PCB pocket (ledge the PCB rests in) ──
    pcb_pocket = (
        cq.Workplane("XY")
        .box(OLED_PCB_W, OLED_PCB_H, OLED_POCKET_DEPTH + 1, centered=(True, True, True))
        .translate((OLED_PCB_X, OLED_PCB_Y,
                    -(EXT_D / 2) + FLOOR - OLED_POCKET_DEPTH / 2))
    )
    body = body.cut(pcb_pocket)

    # ── Front face: LED peek-hole ──
    led_hole = (
        cq.Workplane("XY")
        .cylinder(FLOOR + 2, LED_DIA / 2)
        .translate((LED_X, LED_Y, -(EXT_D / 2)))
    )
    body = body.cut(led_hole)

    # ── Right end face: micro-USB slot ──
    # Cut a rectangular slot through the right wall (+X)
    usb_slot = (
        cq.Workplane("YZ")
        .box(USB_H, WALL + 2, USB_W, centered=(True, True, True))
        .translate((EXT_W / 2, USB_Y_OFFSET, -(EXT_D / 2) + FLOOR + INT_D * 0.75))
    )
    body = body.cut(usb_slot)

    # ── Right end face: BOOT button hole ──
    boot_hole = (
        cq.Workplane("YZ")
        .cylinder(WALL + 2, BTN_DIA / 2)
        .rotateAboutCenter((0, 1, 0), 90)
        .translate((EXT_W / 2, BOOT_Y_OFF,
                    -(EXT_D / 2) + FLOOR + INT_D * 0.20))
    )
    body = body.cut(boot_hole)

    # ── Right end face: EN button hole ──
    en_hole = (
        cq.Workplane("YZ")
        .cylinder(WALL + 2, BTN_DIA / 2)
        .rotateAboutCenter((0, 1, 0), 90)
        .translate((EXT_W / 2, EN_Y_OFF,
                    -(EXT_D / 2) + FLOOR + INT_D * 0.45))
    )
    body = body.cut(en_hole)

    # ── Corner fillets on exterior edges (vertical = Z-axis edges) ──
    try:
        body = body.edges("|Z").fillet(FILLET_R)
    except Exception:
        pass  # skip if topology prevents it

    return body


# ─────────────────────────────────────────────
#  LID  (back plate with inset lip + screw bosses)
# ─────────────────────────────────────────────

def make_lid():
    # Outer plate matches box back opening exactly
    lid = (
        cq.Workplane("XY")
        .box(EXT_W, EXT_H, LID_THICK, centered=(True, True, True))
    )

    # Inset lip that fits inside the box opening
    lip = (
        cq.Workplane("XY")
        .box(INT_W - 0.4, INT_H - 0.4, LID_OVERLAP, centered=(True, True, True))
        .translate((0, 0, (LID_THICK + LID_OVERLAP) / 2))
    )
    lid = lid.union(lip)

    # Four screw bosses inside the box at corners
    boss_offsets = [
        ( INT_W / 2 - 4,  INT_H / 2 - 4),
        (-INT_W / 2 + 4,  INT_H / 2 - 4),
        ( INT_W / 2 - 4, -INT_H / 2 + 4),
        (-INT_W / 2 + 4, -INT_H / 2 + 4),
    ]
    for bx, by in boss_offsets:
        boss = (
            cq.Workplane("XY")
            .cylinder(BOSS_H, BOSS_DIA / 2)
            .translate((bx, by, (LID_THICK + BOSS_H) / 2))
        )
        lid = lid.union(boss)

    # Screw holes through bosses and lid
    for bx, by in boss_offsets:
        hole = (
            cq.Workplane("XY")
            .cylinder(LID_THICK + BOSS_H + 1, SCREW_DIA / 2)
            .translate((bx, by, 0))
        )
        lid = lid.cut(hole)

    # Corner fillets on lid outer plate
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
print(f"  OLED window:     {OLED_WIN_W:.0f} x {OLED_WIN_H:.0f} mm")
print(f"  USB slot:        {USB_W:.0f} x {USB_H:.0f} mm  (right end face)")
print(f"  Button holes:    {BTN_DIA:.0f} mm dia x2   (right end face)")
print(f"  LED hole:        {LED_DIA:.0f} mm dia         (front face, lower right)")
print(f"  Fasteners:       M2 screws into 4 corner bosses on lid")
