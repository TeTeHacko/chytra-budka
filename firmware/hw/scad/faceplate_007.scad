// Chytrá budka — kamera "dekl" — v007  (BEZ IR LED)
// PoC placka na ABB Tango 80x80x28. Tisk PETG, 0.6, MK4IS, LÍCEM DOLŮ, bez podpor.
//
// ZMĚNA proti v006: IR LED otvory VYPNUTY (zatím nejsou tranzistory na zapojení).
//   -> střed volný -> XIAO cradle se vejde pod kameru bez kolizí.
// Drží: PIR 10.75, kotva kamery, pegy s nálevkou, montáž 60, pegy 75x65.

/* [Placka] */
plate_w = 80;  plate_h = 80;  plate_t = 3.0;  corner_r = 7;

/* [Montáž — SEDÍ] */
mount_pitch = 60;  mount_y_off = 0;  mount_d = 3.8;

/* [Pegy -> slepé kapsy + nálevka] */
peg_holes        = true;
peg_pitch_x      = 75;   peg_pitch_y = 65;
peg_hole_d       = 3.4;  peg_pocket_depth = 2.3;
peg_funnel       = 1.6;  peg_funnel_h     = 1.2;

/* [Naměřené + vůle] */
cam_barrel_d = 6.94;  pir_dome_d = 10.55;
cam_clear = 0.3;  pir_clear = 0.2;

/* [Pozice — kamera ve středu] */
cam_x = 0;  cam_y = 0;  pir_dx = 0;  pir_dy = 20;

/* [IR LED — VYPNUTO] */
ir_holes     = false;  // zapni až budou tranzistory
ir_count     = 3;  ir_ring_r = 11;  ir_start_ang = -90;
led_d = 3.0;  led_clear = 0.2;

/* [Kotva kamery] */
cam_anchor       = true;
cam_body         = 8.8;  // ODHAD hrany pouzdra OV3660 — potvrď
cam_body_clear   = 0.4;  cam_anchor_wall = 1.4;  cam_anchor_depth = 4;

/* [XIAO cradle — pod kamerou, volné místo] */
xiao_cradle  = true;
xiao_w = 18.5;  xiao_l = 21.5;  xiao_reserve = 1.2;  xiao_wall = 1.6;
xiao_x = 0;   xiao_y = -22;   // pod kamerou, USB-C ven na -Y (opačně než PIR)
xiao_h = 13;  usbc_w = 14;

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
module peg_pocket() {
    translate([0,0,-1]) cylinder(h = peg_pocket_depth + 1, d = peg_hole_d);
    cylinder(h = peg_funnel_h, d1 = peg_hole_d + peg_funnel, d2 = peg_hole_d);
}

module additive() {
    if (cam_anchor)
        translate([cam_x, cam_y, -cam_anchor_depth])
            linear_extrude(cam_anchor_depth) square(anchor_out, center = true);
    if (xiao_cradle)
        translate([xiao_x, xiao_y, -xiao_h])
            linear_extrude(xiao_h) square([cr_out_x, cr_out_y], center = true);
}

module subtractive() {
    translate([cam_x, cam_y]) thru(cam_hole_d);                       // kamera
    translate([cam_x + pir_dx, cam_y + pir_dy]) thru(pir_hole_d);     // PIR
    if (ir_holes) for (i = [0:ir_count-1]) {                          // IR (vypnuto)
        a = ir_start_ang + i*360/ir_count;
        translate([cam_x + ir_ring_r*cos(a), cam_y + ir_ring_r*sin(a)]) thru(ir_hole_d);
    }
    for (sx = [-1,1]) translate([sx*mount_pitch/2, mount_y_off]) thru(mount_d);   // montáž
    if (peg_holes) for (sx = [-1,1], sy = [-1,1])
        translate([sx*peg_pitch_x/2, sy*peg_pitch_y/2]) peg_pocket();

    if (cam_anchor)
        translate([cam_x, cam_y, -cam_anchor_depth-1])
            linear_extrude(cam_anchor_depth+1) square(anchor_in, center = true);

    if (xiao_cradle) {
        // vnitřní dutina (otevřená v Z)
        translate([xiao_x, xiao_y, -xiao_h-1])
            linear_extrude(xiao_h+2) square([cr_in_x, cr_in_y], center = true);
        // vyříznout střed všech 4 stěn -> zůstanou jen rohové držáky (volné hrany)
        nh = xiao_h - 2;                 // 2mm lipa u plochy drží rohy
        zc = -xiao_h + nh/2;
        wt = 2*xiao_wall + 4;            // přesah skrz stěnu
        translate([xiao_x, xiao_y - cr_out_y/2, zc]) cube([usbc_w,        wt, nh], center=true); // -Y: USB-C
        translate([xiao_x, xiao_y + cr_out_y/2, zc]) cube([cr_in_x*0.6,   wt, nh], center=true); // +Y: SD/dráty
        for (sx = [-1,1])
            translate([xiao_x + sx*cr_out_x/2, xiao_y, zc]) cube([wt, cr_in_y*0.6, nh], center=true); // ±X: dráty
    }
}

difference() {
    union() {
        linear_extrude(plate_t) rounded_rect(plate_w, plate_h, corner_r);
        additive();
    }
    subtractive();
}
