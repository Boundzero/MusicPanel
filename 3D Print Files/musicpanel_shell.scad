// =============================================================================
// MusicPanel enclosure — SHELL v2 (corrected to 85.7 mm, notch A, mount bosses)
//
// Panel presses in from the back; front lip holds it; walls grip it (the fit you
// confirmed on the coupon). Cord-exit notch on all four edges (wide enough that
// the off-center USB-C port clears in any 90° rotation). Four corner bosses form
// a square pattern the wall plate / desk stand screw into.
//
// Print front-face-DOWN (lip on the bed, open back up). No supports.
//
// >>> If the panel isn't exactly 85.7, change panel_side and re-render. <<<
// =============================================================================

/* ---- panel (mm) ---- */
panel_side      = 86.6;    // confirmed: fit at +1% -> 86.6 mm
panel_thickness = 13.0;
fit_gap         = 0.4;     // confirmed on the coupon
depth_gap       = 0.3;

/* ---- shell ---- */
wall          = 5.0;       // thicker walls to house the corner screw bosses
lip_overhang  = 3.0;
lip_thickness = 2.0;
cord_w        = 32.0;      // notch A: wide, centered — clears the offset port in any rotation
boss_r        = 3.0;
pilot_r       = 1.3;       // M3 self-tapping pilot

/* ---- derived ---- */
pocket = panel_side + fit_gap;
outer  = pocket + 2*wall;
pd     = panel_thickness + depth_gap;
window = pocket - 2*lip_overhang;
H      = lip_thickness + pd;
o=outer/2; p=pocket/2; w=window/2; c=cord_w/2; bc=o-3.5;
$fn=48;

module box(x0,x1,y0,y1,z0,z1) translate([x0,y0,z0]) cube([x1-x0,y1-y0,z1-z0]);

difference() {
    union() {
        difference() {
            box(-o,o,-o,o,0,H);                        // outer block
            box(-p,p,-p,p,lip_thickness,H+1);          // panel cavity (open back)
            box(-w,w,-w,w,-1,lip_thickness+0.01);      // screen window
            box(-c,c,p-0.01,o+1,lip_thickness,H+1);    // cord notches, 4 edges
            box(-c,c,-o-1,-p+0.01,lip_thickness,H+1);
            box(p-0.01,o+1,-c,c,lip_thickness,H+1);
            box(-o-1,-p+0.01,-c,c,lip_thickness,H+1);
        }
        for (sx=[-1,1], sy=[-1,1]) translate([sx*bc,sy*bc,0]) cylinder(h=H, r=boss_r);
    }
    for (sx=[-1,1], sy=[-1,1]) translate([sx*bc,sy*bc,2.5]) cylinder(h=H, r=pilot_r);
}
