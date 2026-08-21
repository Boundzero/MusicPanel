# MusicPanel — 3D Print Files

Printable enclosure for the MusicPanel (Guition/JCZN **ESP32-4848S040**, ~86.6 mm
square panel). The shell holds the panel; the same shell mounts to **either** the
wall frame **or** the desk stand, and can be rotated 90° at a time so the cord
exits whichever side you want.

## Files

| File | What it is | ~Filament | Supports |
|---|---|---|---|
| `musicpanel_shell.stl` | Holds the panel: front lip + walls, cord notch on all four edges, four corner screw bosses. **Print front-face-DOWN.** | ~27 g | No |
| `musicpanel_wall_frame.stl` | Wall mount: open frame, keyhole slots, header clearance. Bolts to the shell's four bosses; hangs on two wall screws. **Print wall-side-DOWN.** | ~36 g | No |
| `musicpanel_stand.stl` | Desk stand (30°): two hooks the frame's keyholes drop onto, cord runs down and out the fully open bottom. **Print base-DOWN.** | ~55 g | Pegs only (build-plate) |
| `musicpanel_shell.scad` | Editable OpenSCAD source for the shell — change `panel_side` etc. if your panel differs. | — | — |
| `musicpanel_corner_coupon.stl` | Optional test-fit coupon: prints one corner so you can check the panel grip before committing to a full shell. | ~4 g | No |

## Print settings

- Material: PLA or PETG
- Layer height: 0.2 mm
- Perimeters: 3, Infill: ~15%
- No supports except the two pegs on the stand (use "supports touching build plate only")

## Assembly

1. Press the panel into the **shell** from the open back (front glass seats against the lip).
2. Bolt the **wall frame** to the shell's four corner bosses with **M3 × 12 mm** screws, in whatever rotation puts the cord where you want it.
3. **Wall:** two screws in the wall (36 mm apart, heads ≤7 mm), hang the assembly on the keyholes.
   **Desk:** drop the assembly's keyholes onto the **stand's** two hooks; the cord runs down through the open bottom.

## Notes

- Dimensions are tuned to a panel that measured **86.6 mm** square, **13 mm** thick. If yours differs, edit `panel_side` / `panel_thickness` in the `.scad` and re-export.
- The cord notch is sized for a ~12 mm USB-C plug boot with clearance in all four rotations.
