// Chytrá budka — kamera "dekl" (faceplate) — v002
// Krabička: ABB Tango instalační, 80.6 x 80.6 mm, hloubka 26 mm (změřeno na stole).
// Otvory navázány na NAMĚŘENÉ rozměry posuvkou + vůle (FDM tisk drobně podměřuje).
//   kamera tubus  6.94  (změřeno)
//   PIR dóm      10.55  (změřeno)
//   IR LED 3mm    3.0   (nominál)
// POZN.: IR LED u čočky = riziko odlesku/glare v noci -> ir_ring_r parametrický.

/* [Placka — dle ABB Tango] */
plate_w     = 80.6;  // [40:0.1:120] šířka X (mm)
plate_h     = 80.6;  // [40:0.1:120] výška Y (mm)
plate_t     = 3;     // [2:0.5:6] tloušťka (mm)
corner_r    = 5;     // [0:0.5:15] zaoblení rohů (mm)

/* [Naměřené rozměry + vůle] */
cam_barrel_d = 6.94;  // [4:0.01:14] změřený Ø tubusu objektivu (mm)
pir_dome_d   = 10.55; // [6:0.01:16] změřený Ø PIR dómu (mm)
led_d        = 3.0;   // [2:0.1:6]  nominální Ø LED (mm)
fit_clear    = 0.3;   // [0:0.05:0.8] vůle kamera/PIR (mm)
led_clear    = 0.2;   // [0:0.05:0.8] vůle LED (mm)

/* [Pozice — kamera ve středu] */
cam_x       = 0;     // [-30:30] střed kamery X (mm)
cam_y       = 0;     // [-30:30] střed kamery Y (mm)
pir_dx      = 0;     // [-40:40] PIR posun od kamery X (mm)
pir_dy      = 20;    // [-40:40] PIR posun od kamery Y (mm)

/* [IR LED kruh] */
ir_count     = 3;    // [1:6] počet IR LED
ir_ring_r    = 11;   // [5:30] poloměr kruhu okolo kamery (mm)
ir_start_ang = -90;  // [-180:180] úhel první LED (0=vpravo, -90=dole)

/* [Montážní díry] */
mount_holes = true;  // rohové díry pro šroub
mount_d     = 3.2;   // [2:0.25:5] Ø díry (M3 vůle = 3.2) (mm)
mount_inset = 7;     // [3:20] odsazení od rohu (mm)

/* [Render] */
$fn = 96;

// odvozené průměry otvorů
cam_hole_d = cam_barrel_d + fit_clear;
pir_hole_d = pir_dome_d   + fit_clear;
ir_hole_d  = led_d        + led_clear;

module rounded_rect(w, h, r) {
    if (r > 0) offset(r) square([w - 2*r, h - 2*r], center = true);
    else square([w, h], center = true);
}

module thru(d) {
    translate([0, 0, -1]) cylinder(h = plate_t + 2, d = d);
}

module faceplate() {
    difference() {
        linear_extrude(plate_t)
            rounded_rect(plate_w, plate_h, corner_r);

        translate([cam_x, cam_y]) thru(cam_hole_d);                       // kamera
        translate([cam_x + pir_dx, cam_y + pir_dy]) thru(pir_hole_d);     // PIR

        for (i = [0 : ir_count - 1]) {                                    // IR LED kruh
            a = ir_start_ang + i * 360 / ir_count;
            translate([cam_x + ir_ring_r * cos(a),
                       cam_y + ir_ring_r * sin(a)]) thru(ir_hole_d);
        }

        if (mount_holes)                                                  // rohy
            for (sx = [-1, 1], sy = [-1, 1])
                translate([sx * (plate_w/2 - mount_inset),
                           sy * (plate_h/2 - mount_inset)]) thru(mount_d);
    }
}

faceplate();
