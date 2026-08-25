// Chytrá budka — kamera "dekl" — v006
// PoC placka na ABB Tango 80x80x28 (vnitřní hloubka 26). Tisk PETG, 0.6, MK4IS,
// LÍCEM (přední plochou) DOLŮ -> zadní prvky (kapsy, kotva, cradle) rostou vzhůru, bez podpor.
//
// ZMĚNY proti v005 (z reálného přiměření + dohledání):
//  - PIR -> 10.75 (mezi v004 10.85 a v005 10.65). Jinak na PIR nesaháno.
//  - PEG kapsy: větší Ø (3.0->3.4) + NÁLEVKA na zadním ústí (zavedení čudlíků).
//  - KOTVA KAMERY: čtvercová kapsa na zadní straně, modul zapadne, čočka do otvoru.
//  - XIAO CRADLE (první pokus, parametrický): stěny okolo desky, USB-C výřez ven
//    na opačnou stranu než PIR, otevřené boky na dráty/SD. POZOR: poloha je odhad,
//    cradle sedí na "standoff" hloubce za rovinou LED -> v 2D se překrývá s optikou,
//    ale je za ní. Zkontroluj render, klidně posuň (xiao_x/xiao_y) nebo vypni.
//
// Naměřeno: kamera tubus 6.94, PIR dóm 10.55, IR LED 3.0, čudlík 2.5/v2.1.
// Dohledáno: XIAO 21x17.5(x15 stack), USB-C kabel do ~12.35mm š.

/* [Placka] */
plate_w = 80;  plate_h = 80;  plate_t = 3.0;  corner_r = 7;   // [mm]

/* [Montáž — SEDÍ] */
mount_pitch = 60;  mount_y_off = 0;  mount_d = 3.8;            // [mm]

/* [Pegy -> slepé kapsy + nálevka] */
peg_holes        = true;
peg_pitch_x      = 75;   peg_pitch_y = 65;                      // SEDÍ
peg_hole_d       = 3.4;  // [2.5:0.1:5] Ø kapsy (zvětšeno z 3.0) (mm)
peg_pocket_depth = 2.3;  // [1:0.1:4] hloubka (mm)
peg_funnel       = 1.6;  // [0:0.1:3] o kolik je ústí širší (nálevka) (mm)
peg_funnel_h     = 1.2;  // [0:0.1:3] hloubka nálevky od zadního ústí (mm)

/* [Naměřené + vůle] */
cam_barrel_d = 6.94;  pir_dome_d = 10.55;  led_d = 3.0;         // [mm]
cam_clear = 0.3;  pir_clear = 0.2;  led_clear = 0.2;           // PIR 0.2 -> 10.75

/* [Pozice — kamera ve středu] */
cam_x = 0;  cam_y = 0;  pir_dx = 0;  pir_dy = 20;               // [mm]

/* [IR LED kruh] */
ir_count = 3;  ir_ring_r = 11;  ir_start_ang = -90;

/* [Kotva kamery (zadní kapsa na pouzdro OV3660)] */
cam_anchor       = true;
cam_body         = 8.8;  // [6:0.1:14] hrana čtverc. pouzdra OV3660 (ODHAD, potvrď) (mm)
cam_body_clear   = 0.4;  // [0:0.05:1] vůle pouzdra (mm)
cam_anchor_wall  = 1.4;  // [1:0.1:3] tloušťka stěny kotvy (mm)
cam_anchor_depth = 4;    // [2:0.5:8] hloubka kapsy = jak hluboko modul sedí (mm)

/* [XIAO cradle — PRVNÍ POKUS] */
xiao_cradle  = true;
xiao_w       = 18.5; // [15:0.5:25] šířka desky X (=hrana s USB-C) (mm)
xiao_l       = 21.5; // [18:0.5:28] délka desky Y (mm)
xiao_reserve = 1.2;  // [0:0.1:3] vůle na stranu (dráty) (mm)
xiao_wall    = 1.6;  // [1:0.1:3] tloušťka stěn cradle (mm)
xiao_x       = 0;    // [-20:20] poloha cradle X (mm)
xiao_y       = -25;  // [-28:5] poloha cradle Y (dole, mimo optiku; USB-C ven na -Y) (mm)
xiao_h       = 13;   // [6:0.5:20] výška stěn cradle (drží ~10-15mm stack) (mm)
usbc_w       = 14;   // [10:0.5:20] šířka USB-C výřezu (kabel do ~12.35) (mm)

/* [Render] */
$fn = 96;

// odvozené
cam_hole_d   = cam_barrel_d + cam_clear;
pir_hole_d   = pir_dome_d   + pir_clear;
ir_hole_d    = led_d        + led_clear;
anchor_in    = cam_body + cam_body_clear;
anchor_out   = anchor_in + 2*cam_anchor_wall;
cr_in_x = xiao_w + 2*xiao_reserve;   cr_in_y = xiao_l + 2*xiao_reserve;
cr_out_x = cr_in_x + 2*xiao_wall;    cr_out_y = cr_in_y + 2*xiao_wall;

module rounded_rect(w, h, r) {
    if (r > 0) offset(r) square([w - 2*r, h - 2*r], center = true);
    else square([w, h], center = true);
}
module thru(d) { translate([0,0,-1]) cylinder(h = plate_t + 2, d = d); }

// slepá kapsa + nálevka na zadním ústí (z=0 zadní plocha, +Z do placky)
module peg_pocket() {
    translate([0,0,-1]) cylinder(h = peg_pocket_depth + 1, d = peg_hole_d);
    cylinder(h = peg_funnel_h, d1 = peg_hole_d + peg_funnel, d2 = peg_hole_d);
}

module additive() {
    // kotva kamery (čtvercové stěny na zadní straně, z=0..-depth)
    if (cam_anchor)
        translate([cam_x, cam_y, -cam_anchor_depth])
            linear_extrude(cam_anchor_depth) square(anchor_out, center = true);
    // XIAO cradle (stěny na zadní straně)
    if (xiao_cradle)
        translate([xiao_x, xiao_y, -xiao_h])
            linear_extrude(xiao_h) square([cr_out_x, cr_out_y], center = true);
}

module subtractive() {
    translate([cam_x, cam_y]) thru(cam_hole_d);                       // kamera
    translate([cam_x + pir_dx, cam_y + pir_dy]) thru(pir_hole_d);     // PIR
    for (i = [0:ir_count-1]) {                                        // IR LED
        a = ir_start_ang + i*360/ir_count;
        translate([cam_x + ir_ring_r*cos(a), cam_y + ir_ring_r*sin(a)]) thru(ir_hole_d);
    }
    for (sx = [-1,1]) translate([sx*mount_pitch/2, mount_y_off]) thru(mount_d);   // montáž
    if (peg_holes) for (sx = [-1,1], sy = [-1,1])                     // pegy
        translate([sx*peg_pitch_x/2, sy*peg_pitch_y/2]) peg_pocket();

    // dutina kotvy kamery (modul zapadne zezadu)
    if (cam_anchor)
        translate([cam_x, cam_y, -cam_anchor_depth-1])
            linear_extrude(cam_anchor_depth+1) square(anchor_in, center = true);

    // dutina cradle (otevřená oběma směry v Z)
    if (xiao_cradle) {
        translate([xiao_x, xiao_y, -xiao_h-1])
            linear_extrude(xiao_h+2) square([cr_in_x, cr_in_y], center = true);
        // USB-C výřez v -Y stěně cradle (od dna nahoru, nahoře 2mm lipa drží stěnu)
        notch_h = xiao_h - 2;
        translate([xiao_x, xiao_y - cr_out_y/2, -xiao_h + notch_h/2])
            cube([usbc_w, 2*xiao_wall + 4, notch_h], center = true);
    }
}

difference() {
    union() {
        linear_extrude(plate_t) rounded_rect(plate_w, plate_h, corner_r);
        additive();
    }
    subtractive();
}
