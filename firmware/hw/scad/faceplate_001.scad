// Chytrá budka — kamera "dekl" (faceplate) — v001
// PRVNÍ TESTOVACÍ PLACKA. Nikdy netištěno/neověřeno v reálu.
// Vše parametrické. Rozměry otvorů z dokumentace:
//   kamera OV3660  Ø10  (SCHEMATIC.md:305)
//   PIR AM312      Ø8   (SHOPPING.md:102)
//   IR LED 3mm     Ø3.5 (SHOPPING.md:103)
// POZN.: IR LED hned vedle čočky = riziko odlesku/glare v noci.
//        ir_ring_r / úhly jsou parametrické právě kvůli ladění odstupu.

/* [Placka] */
plate_w     = 70;    // [40:120] šířka X (mm)
plate_h     = 50;    // [30:90]  výška Y (mm)
plate_t     = 3;     // [2:0.5:6] tloušťka (mm)
corner_r    = 4;     // [0:0.5:12] zaoblení rohů (mm)

/* [Kamera OV3660] */
cam_hole_d  = 10;    // [6:0.5:14] otvor objektivu Ø (mm)
cam_x       = 0;     // [-30:30] střed kamery X (mm)
cam_y       = 0;     // [-30:30] střed kamery Y (mm)

/* [PIR AM312] */
pir_hole_d  = 8;     // [6:0.5:12] otvor Fresnel dómu Ø (mm)
pir_dx      = 0;     // [-40:40] PIR posun od kamery X (mm)
pir_dy      = 18;    // [-40:40] PIR posun od kamery Y (mm)

/* [IR LED 3x 940nm] */
ir_count     = 3;    // [1:6] počet IR LED
ir_hole_d    = 3.5;  // [3:0.25:5] otvor LED Ø (mm)
ir_ring_r    = 11;   // [5:25] poloměr kruhu LED okolo kamery (mm)
ir_start_ang = -90;  // [-180:180] úhel první LED (0=vpravo, -90=dole) (mm)

/* [Montážní díry] */
mount_holes = true;  // rohové díry pro šroub
mount_d     = 3.2;   // [2:0.25:5] Ø díry (M3 vůle = 3.2) (mm)
mount_inset = 6;     // [3:15] odsazení od rohu (mm)

/* [Render] */
$fn = 64;

// --- helpers ---
module rounded_rect(w, h, r) {
    if (r > 0) offset(r) square([w - 2*r, h - 2*r], center = true);
    else square([w, h], center = true);
}

module thru(d) {
    // díra skrz placku (s přesahem na čistý průnik)
    translate([0, 0, -1])
        cylinder(h = plate_t + 2, d = d);
}

// --- díly ---
module faceplate() {
    difference() {
        linear_extrude(plate_t)
            rounded_rect(plate_w, plate_h, corner_r);

        // kamera
        translate([cam_x, cam_y]) thru(cam_hole_d);

        // PIR
        translate([cam_x + pir_dx, cam_y + pir_dy]) thru(pir_hole_d);

        // IR LED kruh okolo kamery
        for (i = [0 : ir_count - 1]) {
            a = ir_start_ang + i * 360 / ir_count;
            translate([cam_x + ir_ring_r * cos(a),
                       cam_y + ir_ring_r * sin(a)])
                thru(ir_hole_d);
        }

        // rohové montážní díry
        if (mount_holes) {
            for (sx = [-1, 1], sy = [-1, 1])
                translate([sx * (plate_w/2 - mount_inset),
                           sy * (plate_h/2 - mount_inset)])
                    thru(mount_d);
        }
    }
}

faceplate();
