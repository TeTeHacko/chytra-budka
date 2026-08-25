// Chytrá budka — kamera "dekl" — v008
// PoC placka na ABB Tango 80x80x28. Tisk PETG, 0.6 tryska, MK4IS, LÍCEM (přední plochou)
// DOLŮ, bez podpor -> zadní prvky (kapsy, cradle, jazýčky) rostou vzhůru.
//
// ZMĚNA proti v005/006/007 — SPRÁVNÁ geometrie XIAO ESP32-S3 Sense (z fotky uživatele):
//   Kamera je natvrdo na desce a celá sestava (deska + modul kamery + objektiv) je
//   JEDEN tuhý kus o výšce ~15 mm. Deska LEŽÍ NAPLOCHO těsně za plackou, objektiv míří
//   kolmo VEN dírou. Modul kamery je na JEDNOM KONCI desky (čočka u horní hrany, mírně
//   přečnívá), takže tělo desky visí na JEDNU stranu (-Y), opačně než PIR (+Y).
//   -> v006/007 byly špatně: měly „kotvu kamery" ve středu + zvlášť cradle 22 mm dole,
//      s předpokladem že se kamera vyklápí. Nevyklápí. ZRUŠENO.
//
// PROČ v005 „plave": jediný kontakt s deskou je tubus objektivu v díře (bod uprostřed)
//   -> deska se houpe jako kyvadlo a chrastí v Z. Řešení = obvodový cradle, co chytí
//   spodní část desky do X/Y + rotace, čočka v díře drží horní konec, jazýčky drží Z.
//
// Naměřeno posuvkou (zamčeno): kamera tubus 6.94, PIR dóm 10.55, montáž 60, pegy 75x65.
// Z DATASHEETU modulu MJY5OAF-F3M-V1 (sensor OV5640, ne OV3660): tělo modulu 8.5x8.5 mm,
//   celková výška 5.5 mm, kulatý tubus ~6.94 jde dírou 7.24, ale ČTVERCOVÉ tělo 8.5 > 7.24
//   -> dosedne na zadní plochu placky = pevný doraz v Z dopředu (čočka kouká ven).
// Z měření uživatele: STACK (základní deska + addon + spodní čtvereček kamery) = 10.77 mm
//   = hloubka od zadní plochy placky (kde dosedne čtverec 8.5) po ZADNÍ plochu desky.
// IR VYPNUTO (nejsou tranzistory).

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
cam_barrel_d = 6.94;  // změřený Ø tubusu objektivu
pir_dome_d   = 10.55; // změřený Ø PIR dómu
cam_clear    = 0.3;   // vůle čočka -> otvor 7.24
pir_clear    = 0.2;   // vůle PIR  -> otvor 10.75

/* [Pozice — čočka (=otvor) ve středu placky] */
cam_x  = 0;   cam_y  = 0;
pir_dx = 0;   pir_dy = 20;   // PIR nad čočkou (+Y), opačně než tělo desky

/* [IR LED — VYPNUTO (zapni až budou tranzistory)] */
ir_holes = false;
ir_count = 3;  ir_ring_r = 11;  ir_start_ang = -90;
led_d = 3.0;  led_clear = 0.2;

/* [XIAO ESP32-S3 Sense — obvodový cradle (NOVÉ)] */
board_cradle = true;
board_w      = 17.8; // [15:0.1:22] šířka desky X (kratší hrana, fotka 17.8) (mm)
board_l      = 21.0; // [18:0.1:26] délka desky Y (delší hrana) (mm)
board_clear  = 0.30; // [0:0.05:0.8] vůle na stranu (vodicí, ne těsná) (mm)
// poloha: čočka je u horní (kamerové) hrany desky a modul mírně přečnívá ven.
// board_top_off = o kolik je STŘED čočky nad horní hranou desky (přesah modulu).
board_top_off = 0.7; // [-3:0.1:4] čočka nad horní hranou desky (mm)  -> z fotky ~0.7
// Z měření uživatele (řídí hloubku cradle + polohu jazýčků):
stack_h     = 10.77; // [6:0.05:16] hloubka stacku za plackou: zadní plocha placky -> zadní plocha desky (mm)
cam_foot    = 8.5;   // [6:0.1:12] hrana čtvercového těla modulu (datasheet) — > otvor, dosedá na placku (mm)
cradle_wall = 1.8;   // [1:0.1:3] tloušťka stěn cradle (mm)

/* [Zajištění v Z — zacvakávací jazýčky] */
snap_lips   = true;
lip_grab    = 1.0;   // [0.4:0.1:2] o kolik jazýček přesahuje přes zadní hranu desky (mm)
lip_h       = 1.6;   // [0.8:0.1:3] výška náběhové rampy jazýčku (mm)
lip_len     = 8;     // [4:0.5:16] délka jazýčku podél stěny (mm)
lip_preload = 0.15;  // [0:0.05:0.6] lehký předpětí: záchyt o tolik výš -> tlačí desku k placce (mm)

/* [USB-C výřez ve spodní (-Y) stěně cradle] */
usbc_w      = 14;  // [8:0.5:20] šířka výřezu (kabel s gumou do ~12.35) (mm)
usbc_strap  = 1.5; // [0:0.1:4] kolik stěny nechat nahoře u placky (mm)

/* [Render] */
$fn = 96;

// ---- odvozené ----
cam_hole_d = cam_barrel_d + cam_clear;
pir_hole_d = pir_dome_d   + pir_clear;
ir_hole_d  = led_d        + led_clear;

ix = board_w + 2*board_clear;          // vnitřní rozměr cradle X
iy = board_l + 2*board_clear;          // vnitřní rozměr cradle Y
// střed desky v Y: horní hrana desky je board_top_off pod čočkou, deska visí dolů (-Y)
board_cy = cam_y - board_top_off - board_l/2;
board_cx = cam_x;

board_back_z = -stack_h;                                   // zadní plocha desky (z)
cradle_depth = snap_lips ? (stack_h + lip_h) : stack_h;

module rounded_rect(w, h, r) {
    if (r > 0) offset(r) square([w - 2*r, h - 2*r], center = true);
    else square([w, h], center = true);
}
module thru(d) { translate([0,0,-1]) cylinder(h = plate_t + 2, d = d); }   // průchozí
module peg_pocket() {                                                       // slepá kapsa + nálevka
    translate([0,0,-1]) cylinder(h = peg_pocket_depth + 1, d = peg_hole_d);
    cylinder(h = peg_funnel_h, d1 = peg_hole_d + peg_funnel, d2 = peg_hole_d);
}

// jeden zacvakávací jazýček na vnitřní straně boční (±X) stěny
module lip_nib(sx) {
    inner = board_cx + sx*(ix/2);      // vnitřní líc boční stěny
    hull() {
        // záchyt: plný přesah, líc s předpětím nad zadní hranou desky (drží proti vytažení -Z)
        translate([inner - sx*lip_grab/2, board_cy, board_back_z + lip_preload - 0.3])
            cube([lip_grab, lip_len, 0.6], center = true);
        // kořen rampy: u stěny, na dně cradle (náběh při zasouvání)
        translate([inner, board_cy, -cradle_depth + 0.3])
            cube([0.6, lip_len, 0.6], center = true);
    }
}

module cradle_add() {
    oy = iy + 2*cradle_wall;
    // boční stěny (±X), plná délka oy (s rohy); +Y konec ZÁMĚRNĚ otevřený (přesah kamery)
    for (sx = [-1, 1])
        translate([board_cx + sx*(ix/2 + cradle_wall/2), board_cy, -cradle_depth/2])
            cube([cradle_wall, oy, cradle_depth], center = true);
    // spodní (-Y) koncová stěna mezi bočnicemi
    translate([board_cx, board_cy - iy/2 - cradle_wall/2, -cradle_depth/2])
        cube([ix, cradle_wall, cradle_depth], center = true);
    // jazýčky
    if (snap_lips) for (sx = [-1, 1]) lip_nib(sx);
}

module cradle_cut() {
    // vnitřní dutina cradle: od dna (otevřeno do krabice) až k zadní ploše placky (z=0)
    translate([board_cx, board_cy, -cradle_depth - 1])
        linear_extrude(cradle_depth + 1) square([ix, iy], center = true);
    // USB-C výřez ve spodní (-Y) stěně: od dna nahoru, nahoře nechat usbc_strap u placky
    notch_h = cradle_depth - usbc_strap;
    translate([board_cx, board_cy - iy/2 - cradle_wall/2, -cradle_depth + notch_h/2])
        cube([usbc_w, 2*cradle_wall + 4, notch_h + 0.01], center = true);
}

module additive() {
    if (board_cradle) cradle_add();
}
module subtractive() {
    translate([cam_x, cam_y]) thru(cam_hole_d);                       // kamera
    translate([cam_x + pir_dx, cam_y + pir_dy]) thru(pir_hole_d);     // PIR
    if (ir_holes) for (i = [0:ir_count-1]) {
        a = ir_start_ang + i*360/ir_count;
        translate([cam_x + ir_ring_r*cos(a), cam_y + ir_ring_r*sin(a)]) thru(ir_hole_d);
    }
    for (sx = [-1,1]) translate([sx*mount_pitch/2, mount_y_off]) thru(mount_d);   // montáž
    if (peg_holes) for (sx = [-1,1], sy = [-1,1])
        translate([sx*peg_pitch_x/2, sy*peg_pitch_y/2]) peg_pocket();
    if (board_cradle) cradle_cut();
}

difference() {
    union() {
        linear_extrude(plate_t) rounded_rect(plate_w, plate_h, corner_r);
        additive();
    }
    subtractive();
}
