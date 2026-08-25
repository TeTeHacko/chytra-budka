// Chytrá budka — kamera "dekl" — v004
// PoC placka co LEŽÍ na vršku ABB Tango surface boxu 80x80x28 (vnitřní hloubka 26).
// Tisk: PETG, 0.6 tryska, MK4IS, 1.2 mm, naplocho lícem dolů, díry svisle, bez podpor.
//
// ZMĚNY proti v003 (z reálného přiměření tisku):
//  - LED Ø3.2 sedí přesně -> beze změny.
//  - PIR utáhnout -> vlastní (menší) vůle pir_clear.
//  - Kamera "plus minus" -> beze změny.
//  - NOVÉ: rohové výstupky krabice Ø2.5 / v2.1 mm, rozteč 75x65 -> 4 průchozí díry,
//          aby placka dosedla na okraj (peg je vyšší než tloušťka placky -> musí být skrz).
//
// Naměřeno posuvkou: kamera tubus 6.94, PIR dóm 10.55, IR LED 3.0.
// POZN.: IR LED u čočky = riziko glare v noci -> ir_ring_r parametrický.

/* [Placka — ABB Tango 80x80, leží na vršku] */
plate_w     = 80;    // [60:0.1:90] šířka X (mm)
plate_h     = 80;    // [60:0.1:90] výška Y (mm)
plate_t     = 1.2;   // [0.8:0.2:6] tloušťka (mm)
corner_r    = 7;     // [0:0.5:15] zaoblení rohů (mm)

/* [Montáž — body na vypínač/zásuvku] */
mount_pitch = 60;    // [40:0.5:70] rozteč šroubů vodorovně, střed-střed (mm)
mount_y_off = 0;     // [-15:0.5:15] svislý posun obou děr od středu (mm)
mount_d     = 3.8;   // [2.5:0.1:6] Ø díry = vůle pro samořez (mm)

/* [Rohové výstupky krabice -> průchozí díry] */
peg_holes    = true; // díry pro rohové výstupky, aby placka dosedla
peg_pitch_x  = 75;   // [50:0.5:80] rozteč výstupků X (vodorovně) (mm)
peg_pitch_y  = 65;   // [50:0.5:80] rozteč výstupků Y (svisle) (mm)
peg_hole_d   = 3.0;  // [2.5:0.1:5] Ø díry (výstupek 2.5 + vůle) (mm)

/* [Naměřené rozměry + vůle] */
cam_barrel_d = 6.94;  // [4:0.01:14] změřený Ø tubusu objektivu (mm)
pir_dome_d   = 10.55; // [6:0.01:16] změřený Ø PIR dómu (mm)
led_d        = 3.0;   // [2:0.1:6]  nominální Ø LED (mm)
cam_clear    = 0.3;   // [0:0.05:0.8] vůle kamera ("plus minus" -> beze změny) (mm)
pir_clear    = 0.1;   // [-0.2:0.05:0.6] vůle PIR (utáhnuto z 0.3) (mm)
led_clear    = 0.2;   // [0:0.05:0.8] vůle LED (sedí přesně) (mm)

/* [Pozice — kamera ve středu] */
cam_x       = 0;     // [-30:30] střed kamery X (mm)
cam_y       = 0;     // [-30:30] střed kamery Y (mm)
pir_dx      = 0;     // [-40:40] PIR posun od kamery X (mm)
pir_dy      = 20;    // [-40:40] PIR posun od kamery Y (mm)

/* [IR LED kruh] */
ir_count     = 3;    // [1:6] počet IR LED
ir_ring_r    = 11;   // [5:30] poloměr kruhu okolo kamery (mm)
ir_start_ang = -90;  // [-180:180] úhel první LED (0=vpravo, -90=dole)

/* [Render] */
$fn = 96;

// odvozené průměry otvorů
cam_hole_d = cam_barrel_d + cam_clear;
pir_hole_d = pir_dome_d   + pir_clear;
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

        for (sx = [-1, 1])                                                // 2x montáž (60 vodorovně)
            translate([sx * mount_pitch/2, mount_y_off]) thru(mount_d);

        if (peg_holes)                                                    // 4x rohový výstupek (75x65)
            for (sx = [-1, 1], sy = [-1, 1])
                translate([sx * peg_pitch_x/2, sy * peg_pitch_y/2]) thru(peg_hole_d);
    }
}

faceplate();
