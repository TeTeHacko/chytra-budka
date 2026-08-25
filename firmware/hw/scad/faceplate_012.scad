// Chytrá budka — kamera "dekl" — v012  =  v008 + DVĚ ZMĚNY (dle zadání)
// PoC placka na ABB Tango 80x80x28. Tisk PETG, 0.6 tryska, MK4IS, LÍCEM DOLŮ, bez podpor.
//
// Souřadnice: +Y = NAHOŘE (tam je PIR), −Y = DOLE (tam vede USB kabel). Origin = střed placky.
//
// ZMĚNA 1: díra na čočku posunuta k DOLNÍ hraně XIAO desky (k USB/kabelu, −Y).
//          v008 měl čočku u horní hrany (deska visela dolů). Teď je čočka dole a deska
//          trčí NAHORU k PIR. Cradle zůstává na stejném místě placky jako v008 (board_cy=−11.2).
// ZMĚNA 2: kolem kulaté díry čtvercová prohlubeň pro tělo modulu 8.5×8.5 (zapadne do ní).
// Vše ostatní (stěny, USB výřez na −Y, jazýčky, PIR, montáž, pegy) jako v008.
//
// Modul MJY5OAF-F3M-V1 (OV5640): tělo 8.5×8.5, tubus ~6.94 (díra 7.24). Stack za plackou 10.77.

/* [Placka — ABB Tango 80x80] */
plate_w  = 80;   // [60:0.1:90]
plate_h  = 80;   // [60:0.1:90]
plate_t  = 3.0;  // [2.8:0.1:6]
corner_r = 7;    // [0:0.5:15]

/* [Montáž — SEDÍ] */
mount_pitch = 60;   // [40:0.5:70]
mount_y_off = 0;    // [-15:0.5:15]
mount_d     = 3.8;  // [2.5:0.1:6]

/* [Rohové pegy -> slepé kapsy + nálevka — SEDÍ] */
peg_holes        = true;
peg_pitch_x      = 75;   peg_pitch_y = 65;
peg_hole_d       = 3.4;  peg_pocket_depth = 2.3;
peg_funnel       = 1.6;  peg_funnel_h     = 1.2;

/* [Optika + PIR — naměřené Ø + vůle] */
cam_barrel_d = 6.54;  // změřený Ø tubusu objektivu (zapečeno z presetu)
pir_dome_d   = 10.6;  // změřený Ø PIR dómu (zapečeno z presetu)
cam_clear    = 0.3;   // vůle čočka -> otvor 7.24
pir_clear    = 0.2;   // vůle PIR  -> otvor 10.75

/* [Pozice] */
// ZMĚNA 1: čočka dole (u USB hrany desky). cam_y −21 -> cradle vyjde na board_cy=−11.2 (jako v008).
cam_x  = 0;   cam_y  = -21;
pir_dx = 0;   pir_dy = 41;   // PIR nahoře (+Y); 41 nad čočkou -> absolutně PIR na y=+20 (jako v008)

/* [IR LED — VOLNÉ pozice (X,Y na placce, NE kruh kolem čočky)] */
ir_holes  = true;   // zapni díry pro IR LED
ir_n      = 4;      // [0:6] kolik IR LED se použije (z pozic níže)
led_d     = 3.0;    // [2:0.1:6]  Ø LED
led_clear = 0.2;    // [0:0.05:0.8] vůle LED
// Pozice jednotlivých LED (mm). DŮLEŽITÉ pro funkci/mechaniku:
//  - MIMO obrys cradle/desky (jinak je IR LED zacloní XIAO za plackou),
//  - dál od čočky (jinak glare), ale ať pokrývají FOV. Použije se prvních ir_n.
ir1_x = -16;  // [-38:0.5:38]
ir1_y = -21;  // [-38:0.5:38]
ir2_x =  16;  // [-38:0.5:38]
ir2_y = -21;  // [-38:0.5:38]
ir3_x = -20;  // [-38:0.5:38]
ir3_y =  -6;  // [-38:0.5:38]
ir4_x =  20;  // [-38:0.5:38]
ir4_y =  -6;  // [-38:0.5:38]
ir5_x = -24;  // [-38:0.5:38]
ir5_y =   8;  // [-38:0.5:38]
ir6_x =  24;  // [-38:0.5:38]
ir6_y =   8;  // [-38:0.5:38]

/* [XIAO ESP32-S3 Sense — obvodový cradle] */
board_cradle = true;
board_w      = 17.8; // [15:0.1:22]
board_l      = 21.0; // [18:0.1:26]
board_clear  = 0.30; // [0:0.05:0.8]
// ZMĚNA 1: čočka je `lens_from_edge` nad DOLNÍ hranou desky -> deska trčí nahoru (+Y).
lens_from_edge = 2.0; // [-3:0.1:8] čočka nad dolní (USB) hranou desky (mm) (zapečeno z presetu)
stack_h     = 10.8;  // [6:0.05:16]
cam_foot    = 8.5;   // [6:0.1:12] hrana čtverc. těla modulu (datasheet)
cradle_wall = 1.8;   // [1:0.1:3]

/* [ZMĚNA 2: čtvercová prohlubeň kolem čočky] */
cam_recess       = true;
cam_foot_clear   = 0.3;  // [0:0.05:0.8] vůle prohlubně na stranu (mm)
cam_recess_depth = 2.2;  // [0.5:0.1:2.5] hloubka prohlubně do placky (mm)

/* [Zajištění v Z — zacvakávací jazýčky] */
snap_lips   = true;
lip_grab    = 1.0;   // [0.4:0.1:2]
lip_h       = 1.6;   // [0.8:0.1:3]
lip_len     = 8;     // [4:0.5:16]
lip_preload = 0.15;  // [0:0.05:0.6]

/* [USB-C výřez ve spodní (-Y) stěně cradle] */
usbc_w      = 14;  // [8:0.5:20]
usbc_strap  = 1.5; // [0:0.1:4]

/* [Boční HŘEBEN na dráty z pinů XIAO (±X stěny)] */
wire_comb       = true;  // hřeben: slot na každý pin, výřez z volné (zadní) hrany stěny
wire_pin_count  = 7;     // [1:1:20] kolik slotů na stranu (= bočních pinů XIAO)
wire_pin_pitch  = 2.54;  // [1:0.01:5] rozteč pinů (mm)
wire_slot_w     = 1.4;   // [0.5:0.1:3] šířka slotu = drát + vůle (mm)
wire_slot_depth = 2.7;     // [1:0.5:12] hloubka slotu z volné hrany k placce; zbytek = spojovací hřbet (mm)
wire_comb_off   = 0;     // [-12:0.5:12] posun řady slotů v Y (zarovnání na piny) (mm)

/* [Logo na přední ploše] */
logo_mode  = "engrave";    // [off, engrave, inlay] vypnuto / vygravírovat / výřez pro inlay
logo_file  = "cb_logo.svg";
logo_w     = 39;           // [10:1:78] šířka loga (mm); výška dle poměru ~1.17:1
logo_x     = 0;            // [-38:0.5:38] střed loga X (mm)
logo_y     = 0;            // [-38:0.5:38] střed loga Y (mm)
logo_depth = 0.6;          // [0.3:0.1:2] hloubka gravury / kapsy (mm)
inlay_gap  = 0.15;         // [0:0.05:0.4] vůle inlay dílu na stranu (mm)

/* [Render — co generovat] */
render_part = "plate";     // [plate, inlay] plate = placka | inlay = vkládaný díl loga (druhá barva)

/* [Render] */
$fn = 96;

// ---- odvozené ----
cam_hole_d = cam_barrel_d + cam_clear;
pir_hole_d = pir_dome_d   + pir_clear;
ir_hole_d  = led_d        + led_clear;

ix = board_w + 2*board_clear;
iy = board_l + 2*board_clear;
board_cx = cam_x;
// ZMĚNA 1: čočka u dolní hrany -> střed desky je NAD čočkou (deska trčí +Y, k PIR)
board_cy = cam_y + board_l/2 - lens_from_edge;

// ZMĚNA 2: prohlubeň zapustí čtverec kamery -> deska blíž k placce o jeho hloubku
recess_d     = cam_recess ? cam_recess_depth : 0;
board_back_z = -(stack_h - recess_d);
cradle_depth = snap_lips ? (-board_back_z + lip_h) : -board_back_z;

module rounded_rect(w, h, r) {
    if (r > 0) offset(r) square([w - 2*r, h - 2*r], center = true);
    else square([w, h], center = true);
}
module thru(d) { translate([0,0,-1]) cylinder(h = plate_t + 2, d = d); }
module peg_pocket() {
    translate([0,0,-1]) cylinder(h = peg_pocket_depth + 1, d = peg_hole_d);
    cylinder(h = peg_funnel_h, d1 = peg_hole_d + peg_funnel, d2 = peg_hole_d);
}
module lip_nib(sx) {
    inner = board_cx + sx*(ix/2);
    hull() {
        translate([inner - sx*lip_grab/2, board_cy, board_back_z + lip_preload - 0.3])
            cube([lip_grab, lip_len, 0.6], center = true);
        translate([inner, board_cy, -cradle_depth + 0.3])
            cube([0.6, lip_len, 0.6], center = true);
    }
}
module cradle_add() {
    oy = iy + 2*cradle_wall;
    for (sx = [-1, 1])                                            // boční stěny (±X)
        translate([board_cx + sx*(ix/2 + cradle_wall/2), board_cy, -cradle_depth/2])
            cube([cradle_wall, oy, cradle_depth], center = true);
    translate([board_cx, board_cy - iy/2 - cradle_wall/2, -cradle_depth/2])  // -Y stěna (USB/kamera)
        cube([ix, cradle_wall, cradle_depth], center = true);
    if (snap_lips) for (sx = [-1, 1]) lip_nib(sx);
}
module cradle_cut() {
    translate([board_cx, board_cy, -cradle_depth - 1])
        linear_extrude(cradle_depth + 1) square([ix, iy], center = true);
    notch_h = cradle_depth - usbc_strap;                          // USB-C výřez v -Y stěně
    translate([board_cx, board_cy - iy/2 - cradle_wall/2, -cradle_depth + notch_h/2])
        cube([usbc_w, 2*cradle_wall + 4, notch_h + 0.01], center = true);
}
// boční HŘEBEN na dráty: v obou ±X stěnách, výřez z volné (zadní) hrany v místě každého pinu
module wire_comb_cut() {
    if (wire_comb) for (sx = [-1, 1])
        for (i = [0 : wire_pin_count - 1]) {
            yi = board_cy + wire_comb_off + (i - (wire_pin_count - 1)/2) * wire_pin_pitch;
            translate([board_cx + sx*(ix/2 + cradle_wall/2), yi,
                       -cradle_depth + wire_slot_depth/2 - 0.05])
                cube([2*cradle_wall + 4, wire_slot_w, wire_slot_depth + 0.1], center = true);
        }
}
logo_aspect = 423/362;   // poměr loga (z trimnutého cb_logo.svg)
logo_h = logo_w / logo_aspect;
// 2D logo, zmenšené na cílovou velikost a vystředěné na (logo_x, logo_y)
module logo_2d() {
    translate([logo_x - logo_w/2, logo_y - logo_h/2])
        resize([logo_w, logo_h]) import(logo_file);
}
// gravura / kapsa do PŘEDNÍ plochy (z=plate_t dolů o logo_depth)
module logo_cut() {
    translate([0, 0, plate_t - logo_depth])
        linear_extrude(logo_depth + 0.1) logo_2d();
}
// vkládaný díl (inlay): logo zmenšené o vůli, vytlačené na hloubku kapsy (tiskni druhou barvou)
module logo_inlay_part() {
    linear_extrude(logo_depth)
        offset(-inlay_gap)
            resize([logo_w, logo_h]) import(logo_file);
}

module additive() {
    if (board_cradle) cradle_add();
}
module subtractive() {
    translate([cam_x, cam_y, 1]) thru(cam_hole_d);                       // kamera (kulatý tubus)
    if (cam_recess)                                                   // ZMĚNA 2: čtvercová prohlubeň
        translate([cam_x, cam_y, -2])
            linear_extrude(cam_recess_depth + 2)
                square(cam_foot + 2*cam_foot_clear, center = true);
    translate([cam_x + pir_dx, cam_y + pir_dy]) thru(pir_hole_d);     // PIR (nahoře +Y)
    ir_pos = [[ir1_x,ir1_y],[ir2_x,ir2_y],[ir3_x,ir3_y],[ir4_x,ir4_y],[ir5_x,ir5_y],[ir6_x,ir6_y]];
    if (ir_holes) for (i = [0:ir_n-1]) translate(ir_pos[i]) thru(ir_hole_d);   // IR LED — volné pozice
    for (sx = [-1,1]) translate([sx*mount_pitch/2, mount_y_off]) thru(mount_d);   // montáž
    if (peg_holes) for (sx = [-1,1], sy = [-1,1])
        translate([sx*peg_pitch_x/2, sy*peg_pitch_y/2]) peg_pocket();
    if (board_cradle) cradle_cut();
    if (board_cradle) wire_comb_cut();           // boční hřeben na dráty z pinů
    if (logo_mode != "off") logo_cut();          // gravura/kapsa loga do přední plochy
}

if (render_part == "inlay") {
    logo_inlay_part();                            // jen vkládaný díl (druhá barva)
} else {
    difference() {
        union() {
            linear_extrude(plate_t) rounded_rect(plate_w, plate_h, corner_r);
            additive();
        }
        subtractive();
    }
    translate([-(board_w+1)/2,-4,-1.5]) cube([board_w+1,4,1.5]);
}
