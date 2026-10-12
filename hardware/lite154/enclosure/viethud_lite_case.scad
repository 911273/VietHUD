// =============================================================================
// VietHUD Lite enclosure - Xingzhi Cube 1.54" board, external GPS, speaker, no battery
// Parametric: measure your parts, edit the "MEASURE" block, re-export the STLs.
//
//   Front shell : screen bezel + side walls + sun visor + GPS pod (top, behind the buttons)
//   Back cover  : plugs into the shell, holds the speaker, pushes the PCB forward,
//                 closes the GPS pod; 2x M2 screws through top/bottom walls
//   Plungers    : 3 top buttons + power key
//
// Print in PETG or ASA (NOT PLA: a parked car's dashboard passes 70 C and PLA sags ~55 C).
// Axes: X = width, Y = up, Z = depth (Z=0 is the front face, +Z goes back).
// Facing the screen, the viewer's RIGHT is -X. All "left/right" parameters below are as
// seen when looking at the screen; fx() converts them to model X.
// =============================================================================

part = "assembly";   // "assembly" | "exploded" | "front" | "cover" | "plungers"

/* ---------------- MEASURE (mm) - defaults are estimates ---------------- */
pcb_w       = 40.0;   // main board width (incl. anything sticking out sideways)
pcb_h       = 44.0;   // main board height
pcb_t       = 1.6;    // board thickness
disp_t      = 4.0;    // display module thickness in front of the board (glass to PCB)
comp_t      = 4.0;    // tallest parts behind the board (USB socket, caps, connectors)
disp_out_w  = 31.5;   // display glass outline (seated in a 1 mm pocket)
disp_out_h  = 33.5;
scr_w       = 27.8;   // visible / active area -> window
scr_h       = 27.8;
scr_dx      = 0;      // window centre offset from board centre (+ = right, facing the screen)
scr_dy      = 1.5;    // (+ = up)

spk_w       = 25.0;   // rectangular speaker
spk_l       = 25.0;
spk_h       = 8.0;

gps_w       = 15.0;   // GPS module footprint (antenna side up)
gps_l       = 15.0;
gps_h       = 10.0;
jst_slot    = [8, 5]; // cable pass-through, fits a JST SH 1.0 mm 4-pin plug (~6 x 4.3 mm)

usb_w       = 9.0;    // USB-C receptacle opening, LEFT wall (seen from the front)
usb_h       = 3.4;
usb_y       = 0;      // height of the socket from the centre
usb_z_off   = 1.8;    // receptacle centre behind the PCB back face

btn_x       = [-12, 0, 12];  // buttons on the TOP edge, x offsets from centre (+ = right, facing the screen)
btn_z_off   = 1.6;    // button centre behind the PCB back face
btn_d       = 3.6;    // hole diameter

mic_pos     = [-14, -19];    // microphone port on the front face (x, y from centre; + = right/up)
pwr_side    = 1;      // power key: -1 = left wall, +1 = right wall (seen from the front)
pwr_y       = 10;     // height of the power key from the centre
pwr_z_off   = 1.6;    // power key centre behind the PCB back face
post_inset  = 3.0;    // PCB hold-down posts, inset from the board corners

/* ---------------- design choices ---------------- */
wall        = 2.0;
clr         = 0.4;    // fit clearance per side
roof_t      = 1.2;    // GPS pod roof: thin, no metal above the antenna
visor_len   = 12;     // sun visor overhang
visor_t     = 1.6;
lip_h       = 4.0;    // back-cover plug depth
air         = 2.0;    // space between board stack and speaker
$fn         = 48;

/* ---------------- derived ---------------- */
function fx(x) = -x;                // viewer x (right = +) -> model x
W_in = pcb_w + 2*clr;
H_in = pcb_h + 2*clr;
D_in = disp_t + pcb_t + comp_t + air + spk_h;
W    = W_in + 2*wall;
H    = H_in + 2*wall;
pcb_back_z = wall + disp_t + pcb_t; // z of the board's back face
pod_w = gps_w + 2*clr + 2*wall;
pod_l = gps_l + 2*clr + wall;       // open at the back, the cover closes it
pod_h = gps_h + clr + roof_t;
btn_zone_end = pcb_back_z + btn_z_off + btn_d/2 + 2;     // keep the top buttons clear of the pod
Ds   = max(wall + D_in, btn_zone_end + pod_l);           // shell depth (back cover closes it)
pod_z0 = Ds - pod_l;

module rbox(size, r=2) {            // box with rounded vertical (Z) edges, origin at corner
    hull() for (x=[r, size[0]-r], y=[r, size[1]-r]) translate([x, y, 0]) cylinder(r=r, h=size[2]);
}

// ---------------------------------------------------------------- front shell
module front_shell() {
    difference() {
        union() {
            translate([-W/2, -H/2, 0]) rbox([W, H, Ds], 3);
            // GPS pod on the roof
            translate([-pod_w/2, H/2 - wall, pod_z0]) cube([pod_w, pod_h + wall, pod_l]);
            // sun visor + cheeks
            translate([-W/2, H/2 - visor_t, -visor_len]) cube([W, visor_t, visor_len + 1]);
            for (s=[-1, 1]) translate([s*(W/2 - visor_t/2) - visor_t/2, 0, 0])
                hull() {
                    translate([0, H/2 - visor_t, -visor_len]) cube([visor_t, visor_t, 0.1]);
                    translate([0, H/2 - visor_t - 14, -0.1]) cube([visor_t, 14, 0.1]);
                }
        }
        // cavity
        translate([-W_in/2, -H_in/2, wall]) cube([W_in, H_in, Ds]);
        // display pocket + bevelled window
        translate([fx(scr_dx) - disp_out_w/2 - clr, scr_dy - disp_out_h/2 - clr, wall - 1])
            cube([disp_out_w + 2*clr, disp_out_h + 2*clr, 2]);
        hull() {
            translate([fx(scr_dx) - scr_w/2 - 0.3, scr_dy - scr_h/2 - 0.3, wall - 1]) cube([scr_w + 0.6, scr_h + 0.6, 0.1]);
            translate([fx(scr_dx) - scr_w/2 - 1.6, scr_dy - scr_h/2 - 1.6, -0.01]) cube([scr_w + 3.2, scr_h + 3.2, 0.1]);
        }
        // microphone port
        translate([fx(mic_pos[0]), mic_pos[1], -1]) cylinder(d=1.4, h=wall + 2);
        // USB-C opening (left wall) + recess for the plug overmould
        translate([W/2 + 1, usb_y, pcb_back_z + usb_z_off]) rotate([0, -90, 0]) rotate([0, 0, 90]) {
            hull() for (s=[-1, 1]) translate([s*(usb_w + 0.8 - usb_h - 0.8)/2, 0, 0]) cylinder(d=usb_h + 0.8, h=wall + 2);
            hull() for (s=[-1, 1]) translate([s*(12.5 - 7)/2, 0, 0]) cylinder(d=7, h=1.6);
        }
        // power key (side wall)
        translate([fx(pwr_side)*(W/2 - wall - 1), pwr_y, pcb_back_z + pwr_z_off]) rotate([0, fx(pwr_side)*90, 0]) cylinder(d=btn_d, h=wall + 2);
        // buttons (top edge)
        for (x=btn_x) translate([fx(x), H/2 - wall - 1, pcb_back_z + btn_z_off]) rotate([-90, 0, 0]) cylinder(d=btn_d, h=wall + 2);
        // GPS pod interior, open to the back; cable slot into the main cavity
        translate([-gps_w/2 - clr, H/2, pod_z0 + wall]) cube([gps_w + 2*clr, gps_h + clr, pod_l]);
        translate([-jst_slot[0]/2, H/2 - wall - 1, pod_z0 + wall]) cube([jst_slot[0], wall + 2, jst_slot[1]]);
        // vents: floor (behind the board) and roof (in front of the pod, only where no button is)
        for (i=[-2:2])
            translate([i*6 - 1, -H/2 - 1, pcb_back_z + 1]) cube([2, wall + 2, Ds - pcb_back_z - lip_h - 3]);
        for (vx=[-18:3:18])
            if (min([for (b=btn_x) abs(fx(b) - vx)]) > btn_d/2 + 2.5 && abs(vx) < W_in/2 - 2)
                translate([vx - 1, H/2 - wall - 1, wall + 3]) cube([2, wall + 2, pod_z0 - wall - 4]);
        // M2 cover screws through top and bottom walls
        for (s=[-1, 1]) translate([0, s*(H/2 + 1), Ds - lip_h/2]) rotate([90, 0, 0]) cylinder(d=2.4, h=wall + 2, center=true);
    }
}

// ---------------------------------------------------------------- back cover
module back_cover() {
    cov_h = H + pod_h;              // also closes the GPS pod
    difference() {
        union() {
            // plate (outside face at z = Ds + wall)
            translate([-W/2, -H/2, Ds]) rbox([W, H, wall], 3);
            translate([-pod_w/2, H/2 - 3, Ds]) cube([pod_w, pod_h + 3, wall]);
            // plug lip (hollow frame) that slides into the shell
            translate([0, 0, Ds - lip_h]) difference() {
                translate([-W_in/2 + clr, -H_in/2 + clr, 0]) cube([W_in - 2*clr, H_in - 2*clr, lip_h]);
                translate([-W_in/2 + clr + 1.6, -H_in/2 + clr + 1.6, -1]) cube([W_in - 2*clr - 3.2, H_in - 2*clr - 3.2, lip_h + 2]);
            }
            // speaker frame (rectangular)
            translate([0, -4, Ds - spk_h + 2]) difference() {
                translate([-(spk_w + 0.6)/2 - 1.2, -(spk_l + 0.6)/2 - 1.2, 0]) cube([spk_w + 0.6 + 2.4, spk_l + 0.6 + 2.4, spk_h - 2]);
                translate([-(spk_w + 0.6)/2, -(spk_l + 0.6)/2, -1]) cube([spk_w + 0.6, spk_l + 0.6, spk_h + 2]);
            }
            // posts that press the board against the bezel
            for (x=[-1, 1], y=[-1, 1])
                translate([x*(pcb_w/2 - post_inset), y*(pcb_h/2 - post_inset), pcb_back_z + comp_t * 0 + 0.2])
                    cylinder(d=4, h=Ds - pcb_back_z - 0.2);
        }
        // speaker grille: square grid inside the speaker outline
        gnx = floor((spk_w - 8)/3.5) + 1;
        gny = floor((spk_l - 8)/3.5) + 1;
        translate([0, -4, Ds - 1]) for (i=[0:gnx - 1], j=[0:gny - 1])
            translate([(i - (gnx - 1)/2)*3.5, (j - (gny - 1)/2)*3.5, 0]) cylinder(d=2.2, h=wall + 2);
        // screw pilot holes (self-tapping M2) in the lip
        for (s=[-1, 1]) translate([0, s*(H_in/2 - clr), Ds - lip_h/2]) rotate([90, 0, 0]) cylinder(d=1.7, h=8, center=true);
    }
}

// ---------------------------------------------------------------- button plungers
module plunger() {
    cylinder(d=btn_d - 0.5, h=wall + 1.5);          // pokes 1.5 mm out of the case
    translate([0, 0, 0]) cylinder(d=btn_d + 1.6, h=0.8);   // flange stays inside
}

// ---------------------------------------------------------------- dummy parts for the preview
module dummies() {
    color("darkgreen") translate([-pcb_w/2, -pcb_h/2, wall + disp_t]) cube([pcb_w, pcb_h, pcb_t]);
    color("black") translate([fx(scr_dx) - disp_out_w/2, scr_dy - disp_out_h/2, wall]) cube([disp_out_w, disp_out_h, disp_t]);
    color("dimgray") translate([-spk_w/2, -4 - spk_l/2, Ds - spk_h]) cube([spk_w, spk_l, spk_h]);
    color("tan") translate([-gps_w/2, H/2, pod_z0 + wall + clr]) cube([gps_w, gps_h, gps_l]);
}

if (part == "front") front_shell();
else if (part == "cover") translate([0, 0, Ds + wall]) rotate([180, 0, 0]) back_cover();
else if (part == "plungers") for (i=[0:len(btn_x)]) translate([i*8, 0, 0]) plunger();   // +1 for the power key
else if (part == "exploded") rotate([90, 0, 0]) {   // previews: stand it up (Y-up -> Z-up)
    color("lightsteelblue") front_shell();
    translate([0, 0, 30]) color("lightslategray") back_cover();
    translate([0, 0, 15]) dummies();
} else rotate([90, 0, 0]) {
    color("lightsteelblue", 0.9) front_shell();
    color("lightslategray") back_cover();
}
