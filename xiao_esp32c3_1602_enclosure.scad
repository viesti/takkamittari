// Parametric Enclosure v2 for Seeed Studio XIAO ESP32C3 + 1602 I2C LCD + DS18B20
// v2 Updates:
// 1. 4x Corner-only suspension L-pillars for XIAO ESP32C3 (leaves all edge pins & connectors unobstructed)
// 2. DS18B20 probe hole relocated to the bottom of the right wall (z=2..7mm) away from upper LCD components
$fn = 48;

W = 88.0;
D = 50.0;
H_base = 30.0;
wall = 2.0;
corner_r = 3.0;
yc = D / 2.0;

module rounded_box_2d(w, d, r) {
    offset(r = r) square([w - 2*r, d - 2*r], center = true);
}

module xiao_corner_pillars() {
    x0 = wall;
    x1 = wall + 21.5;
    y0 = yc - 9.15;
    y1 = yc + 9.15;

    difference() {
        union() {
            translate([x0, y0 - 1.8, wall]) cube([3.2, 4.6, 7.0]);
            translate([x0, y1 - 2.8, wall]) cube([3.2, 4.6, 7.0]);
            translate([x1 - 3.2, y0 - 1.8, wall]) cube([5.0, 4.6, 7.0]);
            translate([x1 - 3.2, y1 - 2.8, wall]) cube([5.0, 4.6, 7.0]);
        }
        // Under-board suspension clearance (z = 2.0 to 6.0): leaves 2.0x1.5mm corner ledges
        translate([x0 + 2.0, y0 - 2.0, wall - 0.1]) cube([17.5, 22.3, 4.2]);
        translate([x0 - 0.1, y0 + 1.5, wall - 0.1]) cube([22.0, 15.3, 4.2]);
        // Upper PCB pocket (z = 6.0 to 9.5)
        translate([x0, y0, wall + 4.0]) cube([21.5, 18.3, 4.0]);
        translate([x0 - 1.0, yc - 5.5, wall + 4.0]) cube([3.0, 11.0, 4.0]);
        translate([x1 - 1.0, yc - 6.0, wall + 4.0]) cube([4.0, 12.0, 4.0]);
    }
}

module base() {
    difference() {
        union() {
            // Outer shell with cavity
            difference() {
                linear_extrude(H_base) translate([W/2, D/2]) rounded_box_2d(W, D, corner_r);
                translate([0, 0, wall]) linear_extrude(H_base) translate([W/2, D/2]) rounded_box_2d(W - 2*wall, D - 2*wall, corner_r - wall);
            }
            // 4x Corner M3 screw bosses
            for (x = [3.5, W - 3.5], y = [3.5, D - 3.5]) {
                translate([x, y, wall]) cylinder(h = H_base - wall, r = 3.5);
            }
            // XIAO ESP32C3 4-corner suspension pillars
            xiao_corner_pillars();
            // DS18B20 cable tie-down posts inside bottom right wall
            for (dy = [-3.5, 3.5]) {
                translate([W - 9.0, yc + dy, wall]) cylinder(h = 7.0, r = 1.8);
            }
        }
        // M3 pilot holes (2.8mm dia)
        for (x = [3.5, W - 3.5], y = [3.5, D - 3.5]) {
            translate([x, y, wall + 2.0]) cylinder(h = H_base, r = 1.4);
        }
        // USB-C cutout on left wall (aligned with 4mm suspended XIAO board: z = 5.5 to 11.0mm)
        translate([-1.0, yc - 5.2, 5.5]) cube([wall + 2.0, 10.4, 5.5]);
        // DS18B20 probe hole at the BOTTOM of the right wall (z = 2.0 to 7.0mm, 5.0x5.0mm)
        translate([W - wall - 1.0, yc - 2.5, wall]) cube([wall + 2.0, 5.0, 5.0]);
    }
}

module lid() {
    difference() {
        union() {
            linear_extrude(2.0) translate([W/2, D/2]) rounded_box_2d(W, D, corner_r);
            for (dx = [-37.5, 37.5], dy = [-15.5, 15.5]) {
                translate([W/2 + dx, D/2 + dy, 2.0]) cylinder(h = 6.0, r = 2.8);
            }
        }
        translate([W/2 - 36.0, D/2 - 12.5, -1.0]) cube([72.0, 25.0, 4.0]);
        for (x = [3.5, W - 3.5], y = [3.5, D - 3.5]) {
            translate([x, y, -1.0]) cylinder(h = 5.0, r = 1.7);
            translate([x, y, -0.1]) cylinder(h = 1.1, r = 3.0);
        }
        for (dx = [-37.5, 37.5], dy = [-15.5, 15.5]) {
            translate([W/2 + dx, D/2 + dy, 2.0]) cylinder(h = 7.0, r = 1.2);
        }
    }
}

base();
translate([0, D + 6.0, 0]) lid();
