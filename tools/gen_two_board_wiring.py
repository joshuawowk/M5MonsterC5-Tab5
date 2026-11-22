#!/usr/bin/env python3
"""Generate the to-scale two-board wiring SVG for the XIAO proto guide.

Geometry sources: M5Stack PROTO-2.54 board drawing/photo (50x50 mm, 260 holes),
Ebyte E07-433M20S / E01-2G4M27SX mechanical drawings (1.27 mm castellations),
Seeed XIAO ESP32-C5 pinout, M5Stack Tab5 M5-Bus table. All coordinates in mm.
"""
S = 9.0                      # px per mm
GAP = 36.0                   # mm between the two boards
MX, MY = 6.0, 13.0           # outer margins (mm)
C = dict(sck="#22b8cf", mosi="#37b24d", miso="#4a7bd6", cs="#e64980", ctl="#9b5de5",
         v33="#f08c00", v5="#e8590c", gnd="#868e96", tx="#12b886", rx="#fab005")
col = lambda c: 2.3 + 2.54 * c
row = lambda r: 2.14 + 2.54 * r
bus = lambda k: 7.22 + 2.54 * (k - 1)       # bus row k = 1..15
XI, XO = 41.5, 47.4                          # inner / outer tap-pad x
# bus row -> (odd/outer silk, even/inner silk)
SILK = {1:("GND","35"),2:("GND","36"),3:("GND","EN"),4:("23","25"),5:("19","26"),6:("18","3V3"),
        7:("3","1/T"),8:("16","17/T"),9:("21","22/K"),10:("2","5"),11:("12","13"),12:("15","0"),
        13:("HPWR","34"),14:("HPWR","5V"),15:("HPWR","BAT")}
# nets carried on the shared bus: row -> (side, net, colour, Tab5 pin note)
LINK = {1:("i","MOSI","mosi"),2:("i","MISO","miso"),4:("i","SCK","sck"),5:("i","nRF CSN","cs"),
        8:("i","nRF CE","ctl"),11:("i","CC CS","cs"),13:("i","CC GDO0","ctl"),
        7:("i","Tab5 TX","rx"),14:("i","5V","v5")}
LINKO = {1:("GND","gnd"),7:("Tab5 RX","tx")}
out = []
def P(x, y, ox): return ((MX + ox + x) * S, (MY + y) * S)
def w(s): out.append(s)

def board(ox, title, sub):
    x, y = P(0, 0, ox)
    w(f'<rect class="pcb" x="{x:.1f}" y="{y:.1f}" width="{50*S:.1f}" height="{50*S:.1f}" rx="{2.2*S:.1f}"/>')
    tx, ty = P(25, -7.2, ox)
    w(f'<text class="d-t" x="{tx:.1f}" y="{ty:.1f}" text-anchor="middle">{title}</text>')
    tx, ty = P(25, -4.2, ox)
    w(f'<text class="d-s" x="{tx:.1f}" y="{ty:.1f}" text-anchor="middle">{sub}</text>')
    for hx, hy in ((3,3),(47,3),(3,47),(47,47)):
        cx, cy = P(hx, hy, ox); w(f'<circle class="mh" cx="{cx:.1f}" cy="{cy:.1f}" r="{1.55*S:.1f}"/>')
    for r in range(19):
        cs = range(3,14) if r == 0 else range(3,16) if r in (1,17,18) else range(0,14)
        for c in cs:
            cx, cy = P(col(c), row(r), ox); w(f'<circle class="hole" cx="{cx:.1f}" cy="{cy:.1f}" r="{0.62*S:.1f}"/>')
    # M-Bus connector body + tap pads + silk
    x, y = P(42.5, bus(1) - 1.6, ox)
    w(f'<rect class="hdr" x="{x:.1f}" y="{y:.1f}" width="{4.0*S:.1f}" height="{(35.56+3.2)*S:.1f}" rx="3"/>')
    tx, ty = P(44.5, bus(1) - 2.6, ox)
    w(f'<text class="d-xs" x="{tx:.1f}" y="{ty:.1f}" text-anchor="middle">M-Bus 2×15</text>')
    for k in range(1, 16):
        for xx, cls in ((XI, "tapi"), (XO, "tapo")):
            cx, cy = P(xx, bus(k), ox)
            w(f'<rect class="tap" x="{cx-0.75*S:.1f}" y="{cy-0.55*S:.1f}" width="{1.5*S:.1f}" height="{1.1*S:.1f}"/>')
        for xx in (43.6, 45.4):
            cx, cy = P(xx, bus(k), ox); w(f'<circle class="pinh" cx="{cx:.1f}" cy="{cy:.1f}" r="{0.45*S:.1f}"/>')

def wire(ox, pts, c, dash=False):
    d = " ".join(("M" if i == 0 else "L") + f"{P(x, y, ox)[0]:.1f} {P(x, y, ox)[1]:.1f}" for i, (x, y) in enumerate(pts))
    w(f'<path class="lead" style="stroke:{C[c]}{";stroke-dasharray:5 4" if dash else ""}" d="{d}"/>')

def dot(ox, x, y, c, r=0.5):
    cx, cy = P(x, y, ox); w(f'<circle cx="{cx:.1f}" cy="{cy:.1f}" r="{r*S:.1f}" fill="{C[c]}"/>')

def pad(ox, x, y, c):
    cx, cy = P(x, y, ox); w(f'<circle class="term" cx="{cx:.1f}" cy="{cy:.1f}" r="{0.72*S:.1f}" style="stroke:{C[c] if c else "var(--faint)"}"/>')

def text(ox, x, y, s, cls="d-xs", anchor="start", fill=None):
    cx, cy = P(x, y, ox); st = f' style="fill:{fill}"' if fill else ""
    w(f'<text class="{cls}" x="{cx:.1f}" y="{cy:.1f}" text-anchor="{anchor}"{st}>{s}</text>')

def body(ox, x, y, bw, bh, cls="mod"):
    cx, cy = P(x, y, ox); w(f'<rect class="{cls}" x="{cx:.1f}" y="{cy:.1f}" width="{bw*S:.1f}" height="{bh*S:.1f}" rx="4"/>')

def buslabels(ox):
    for k, (side, net, c) in LINK.items():
        text(ox, 48.9, bus(k) + 0.45, f'{2*k}', "pin-n", "start")
    for k, (net, c) in LINKO.items():
        pass

# ------------------------------------------------------------------ Board A
A = 0.0
board(A, "BOARD A — controller", "Proto Module board · XIAO ESP32-C5 · stacks on the Tab5 M-Bus")
xl, xr = col(5), col(11)                       # XIAO pad columns (15.24 mm apart)
body(A, xl - 1.28, row(1) - 2.88, 17.8, 21.0, "xiao")
bx, by = P(xl + 7.62 - 4.5, row(1) - 4.4, A)
w(f'<rect class="usb" x="{bx:.1f}" y="{by:.1f}" width="{9*S:.1f}" height="{7.3*S:.1f}" rx="5"/>')
text(A, xl + 7.62, row(1) + 0.2, "USB-C", "d-xs", "middle")
text(A, xl + 7.62, row(4) + 0.6, "XIAO", "d-t", "middle")
text(A, xl + 7.62, row(5) + 0.9, "ESP32-C5", "d-s", "middle")
text(A, xl + 7.62, row(6) + 0.9, "top view", "d-xs", "middle")
L = [("D0","G1","cs"),("D1","G0","ctl"),("D2","G25",None),("D3","G7",None),("D4","G23","cs"),("D5","G24","ctl"),("D6","TX","tx")]
R = [("5V","","v5"),("GND","","gnd"),("3V3","",None),("D10","MOSI","mosi"),("D9","MISO","miso"),("D8","SCK","sck"),("D7","RX","rx")]
for i, (n, g, c) in enumerate(L):
    pad(A, xl, row(1 + i), c); text(A, xl + 1.3, row(1 + i) + 0.45, n, "pad-s")
for i, (n, g, c) in enumerate(R):
    pad(A, xr, row(1 + i), c); text(A, xr - 1.3, row(1 + i) + 0.45, n, "pad-s", "end")
# right-hand pads -> bus (lanes between XIAO edge 31.5 and taps 41.5)
wire(A, [(xr, row(4)), (33.4, row(4)), (33.4, bus(1)), (XI, bus(1))], "mosi")
wire(A, [(xr, row(5)), (34.4, row(5)), (34.4, bus(2)), (XI, bus(2))], "miso")
wire(A, [(xr, row(6)), (35.4, row(6)), (35.4, bus(4)), (XI, bus(4))], "sck")
wire(A, [(xr, row(7)), (36.4, row(7)), (36.4, bus(7)), (XI, bus(7))], "rx")
wire(A, [(xr, row(1)), (39.6, row(1)), (39.6, bus(14)), (XI, bus(14))], "v5")
wire(A, [(xr, row(2)), (32.4, row(2)), (32.4, 3.6), (48.9, 3.6), (48.9, bus(1)), (XO, bus(1))], "gnd")
# left-hand pads -> bus (run left of the XIAO, under it, then up the lanes)
wire(A, [(xl, row(1)), (11.6, row(1)), (11.6, 24.6), (37.4, 24.6), (37.4, bus(5)), (XI, bus(5))], "cs")
wire(A, [(xl, row(2)), (10.6, row(2)), (10.6, 25.6), (38.4, 25.6), (38.4, bus(8)), (XI, bus(8))], "ctl")
wire(A, [(xl, row(5)), (9.6, row(5)), (9.6, bus(11)), (XI, bus(11))], "cs")
wire(A, [(xl, row(6)), (8.6, row(6)), (8.6, bus(13)), (XI, bus(13))], "ctl")
wire(A, [(xl, row(7)), (12.6, row(7)), (12.6, 46.4), (48.9, 46.4), (48.9, bus(7)), (XO, bus(7))], "tx")
for k, (side, net, c) in LINK.items(): dot(A, XI, bus(k), c)
for k, (net, c) in LINKO.items(): dot(A, XO, bus(k), c)
text(A, 0.5, 53.4, "D2 · D3 spare · XIAO 3V3 pad unused · USB-C faces the board edge", "d-xs")
text(A, 0.5, 55.6, "Odd pins (GND, “3”) are the outer column — run round the header end, or solder at the back pads.", "d-xs")

# ------------------------------------------------------------------ Board B
B = 50.0 + GAP
board(B, "BOARD B — radios", "added Proto Board · E07 + E01 + 3V3 buck module · same M-Bus pins")
PX = 33.0                                       # castellation edge (modules rotated 90° CW)
e7y, e1y = 6.4, 27.4
body(B, PX - 32.0, e7y, 32.0, 18.0)             # E07-433M20S 18x32 rotated
body(B, PX - 18.0, e1y, 18.0, 14.5)             # E01-2G4M27SX 14.5x18 rotated
E7 = {12:("GND","gnd"),11:("VCC","v33"),10:("NC",None),9:("RX_EN","v33"),8:("TX_EN","gnd"),7:("CSN","cs"),
      6:("GDO0","ctl"),5:("GDO2",None),4:("MISO","miso"),3:("SCK","sck"),2:("MOSI","mosi"),1:("GND","gnd")}
E1 = {8:("GND","gnd"),7:("IRQ",None),6:("MISO","miso"),5:("MOSI","mosi"),4:("SCK","sck"),3:("CSN","cs"),2:("CE","ctl"),1:("VCC","v33")}
y7 = lambda k: e7y + 2.01 + (12 - k) * 1.27
y1 = lambda k: e1y + 2.80 + (8 - k) * 1.27
for k, (n, c) in E7.items():
    cx, cy = P(PX, y7(k), B); w(f'<rect class="cast" x="{cx-0.9*S:.1f}" y="{cy-0.42*S:.1f}" width="{0.9*S:.1f}" height="{0.84*S:.1f}" style="fill:{C[c] if c else "var(--faint)"}"/>')
    text(B, PX - 1.3, y7(k) + 0.38, f"{k} {n}", "pad-x", "end")
for k, (n, c) in E1.items():
    cx, cy = P(PX, y1(k), B); w(f'<rect class="cast" x="{cx-0.9*S:.1f}" y="{cy-0.42*S:.1f}" width="{0.9*S:.1f}" height="{0.84*S:.1f}" style="fill:{C[c] if c else "var(--faint)"}"/>')
    text(B, PX - 1.3, y1(k) + 0.38, f"{k} {n}", "pad-x", "end")
text(B, PX - 19.5, e7y + 8.6, "E07-433M20S", "d-t", "middle")
text(B, PX - 19.5, e7y + 11.0, "CC1101 + PA · top view", "d-xs", "middle")
text(B, PX - 19.5, e7y + 13.0, "14–16 = GND", "d-xs", "middle")
body(B, PX - 31.0, e7y + 0.6, 3.6, 3.6, "ipx"); text(B, PX - 29.2, e7y + 2.8, "IPX", "pad-x", "middle")
for d, lab, c in ((5.0, "13", None), (7.0, "14", "gnd"), (11.0, "15", "gnd"), (12.27, "16", "gnd")):
    cx, cy = P(PX - 32.0, e7y + d, B); w(f'<rect class="cast" x="{cx:.1f}" y="{cy-0.42*S:.1f}" width="{0.9*S:.1f}" height="{0.84*S:.1f}" style="fill:{C[c] if c else "var(--faint)"}"/>')
    text(B, PX - 30.7, e7y + d + 0.38, lab + (" ANT" if lab == "13" else ""), "pad-x")
text(B, PX - 12.9, e1y + 6.4, "E01-2G4M27SX", "pad-s", "middle")
body(B, PX - 17.0, e1y + 1.0, 3.8, 3.8, "ipx"); text(B, PX - 15.1, e1y + 3.3, "IPX", "pad-x", "middle")
for gx, gy, lab in ((PX - 13.0, e1y, "9"), (PX - 13.0, e1y + 14.5, "10")):
    cx, cy = P(gx, gy, B); w(f'<circle cx="{cx:.1f}" cy="{cy:.1f}" r="{0.7*S:.1f}" fill="{C["gnd"]}"/>')
    text(B, gx + 1.3, gy + 1.9, "9 GND", "pad-x") if lab == "9" else text(B, gx - 1.3, gy - 0.8, "10 GND", "pad-x", "end")
# 3V3 buck module (pads on its right edge)
RX0, RXP = 1.6, 11.6
body(B, RX0, 29.0, 10.0, 12.0, "reg")
text(B, 5.3, 33.0, "3V3", "d-t", "middle"); text(B, 5.3, 37.2, "buck", "d-xs", "middle"); text(B, 5.3, 39.2, "≥600 mA", "d-xs", "middle")
for nm, yy, c in (("IN", 31.0, "v5"), ("GND", 35.0, "gnd"), ("OUT", 39.0, "v33")):
    pad(B, RXP, yy, c); text(B, RXP - 1.2, yy + 0.45, nm, "pad-x", "end")
LN = dict(v33=34.2, gnd=35.0, mosi=35.8, miso=36.6, sck=37.4, ncs=38.2, nce=39.0, ccs=39.8, cgd=40.6)
def chain(lane, c, k, ys):
    lo, hi = min([bus(k)] + ys), max([bus(k)] + ys)
    wire(B, [(XI, bus(k)), (lane, bus(k))], c); wire(B, [(lane, lo), (lane, hi)], c)
    for yy in ys: wire(B, [(lane, yy), (PX, yy)], c); dot(B, lane, yy, c, 0.36)
    dot(B, XI, bus(k), c); dot(B, lane, bus(k), c, 0.36)
chain(LN["mosi"], "mosi", 1, [y7(2), y1(5)])
chain(LN["miso"], "miso", 2, [y7(4), y1(6)])
chain(LN["sck"], "sck", 4, [y7(3), y1(4)])
chain(LN["ncs"], "cs", 5, [y1(3)])
chain(LN["nce"], "ctl", 8, [y1(2)])
chain(LN["ccs"], "cs", 11, [y7(7)])
chain(LN["cgd"], "ctl", 13, [y7(6)])
# 5V: bus pin 28 -> along the bottom -> regulator IN
wire(B, [(XI, bus(14)), (XI, 45.0), (13.7, 45.0), (13.7, 31.0), (RXP, 31.0)], "v5"); dot(B, XI, bus(14), "v5")
# 3V3 rail: regulator OUT -> under the E01 -> lane -> E01 VCC, E07 VCC + RX_EN
wire(B, [(RXP, 39.0), (12.7, 39.0), (12.7, 43.5), (LN["v33"], 43.5), (LN["v33"], y7(11))], "v33")
for yy in (y1(1), y7(9), y7(11)): wire(B, [(LN["v33"], yy), (PX, yy)], "v33"); dot(B, LN["v33"], yy, "v33", 0.36)
# GND: bus pin 1 (outer column, round the end of the header) -> lane -> E07 12/8/1, E01 8, regulator
wire(B, [(XO, bus(1)), (48.9, bus(1)), (48.9, 3.6), (LN["gnd"], 3.6), (LN["gnd"], y1(8))], "gnd"); dot(B, XO, bus(1), "gnd")
for yy in (y7(12), y7(8), y7(1), y1(8)): wire(B, [(LN["gnd"], yy), (PX, yy)], "gnd"); dot(B, LN["gnd"], yy, "gnd", 0.36)
wire(B, [(LN["gnd"], 25.9), (14.5, 25.9), (14.5, 35.0), (RXP, 35.0)], "gnd"); dot(B, LN["gnd"], 25.9, "gnd", 0.36)
# decoupling / pull badges
def badge(x, y, s, c):
    cx, cy = P(x, y, B)
    w(f'<rect x="{cx-9:.1f}" y="{cy-6.5:.1f}" width="18" height="13" rx="3" style="fill:var(--panel);stroke:{C[c]};stroke-width:1.2"/>')
    w(f'<text class="pad-x" x="{cx:.1f}" y="{cy+2.9:.1f}" text-anchor="middle" style="fill:{C[c]};font-weight:700">{s}</text>')
badge(PX - 9.6, (y7(11) + y7(12)) / 2, "C1", "v33")
badge(PX - 9.6, y7(7), "R1", "cs")
badge(PX - 10.6, y1(1) + 0.9, "C2", "v33")
badge(PX - 7.4, y1(3) - 0.2, "R2", "cs")
badge(PX - 10.6, y1(2) - 0.9, "R3", "ctl")
text(B, 0.5, 53.4, "C1 · C2 = 470 µF + 0.1 µF across VCC–GND at the pads (E07 11↔12, E01 1↔8)", "d-xs")
text(B, 0.5, 55.6, "R1 · R2 = 10k CSN→3V3 · R3 = 4.7k CE (pad 2)→GND · E01 pads 9, 10 are GND too", "d-xs")

# ------------------------------------------------------------ shared-pin band
def tag(mx, y, c, s, wd=140):
    w(f'<rect class="tag" x="{mx-wd/2:.1f}" y="{y-8:.1f}" width="{wd}" height="16" rx="8"/>')
    w(f'<text class="tag-t" x="{mx:.1f}" y="{y+3.6:.1f}" text-anchor="middle" style="fill:{C[c]}">{s}</text>')
for k in sorted(set(LINK) | set(LINKO)):
    x1, y = P(50.0, bus(k), A); x2, _ = P(0.0, bus(k), B); mx = (x1 + x2) / 2
    c0 = LINK[k][2] if k in LINK else LINKO[k][1]
    w(f'<line x1="{x1+4:.1f}" y1="{y:.1f}" x2="{x2-4:.1f}" y2="{y:.1f}" style="stroke:{C[c0]};stroke-width:2;stroke-dasharray:2 5;stroke-linecap:round"/>')
    if k in LINK and k in LINKO:
        tag(mx - 74, y, LINKO[k][1], f'{2*k-1} · “{SILK[k][0]}” · {LINKO[k][0]}')
        tag(mx + 74, y, LINK[k][2], f'{2*k} · “{SILK[k][1]}” · {LINK[k][1]}')
    elif k in LINK:
        tag(mx, y, LINK[k][2], f'pin {2*k} · “{SILK[k][1]}” · {LINK[k][1]}', 168)
    else:
        tag(mx, y, LINKO[k][1], f'pin {2*k-1} · “{SILK[k][0]}” · {LINKO[k][0]}', 168)
gx, gy = P(50.0 + GAP / 2, -7.2, A)
w(f'<text class="d-t" x="{gx:.1f}" y="{gy:.1f}" text-anchor="middle">shared M-Bus pins</text>')
gx, gy = P(50.0 + GAP / 2, -4.2, A)
w(f'<text class="d-xs" x="{gx:.1f}" y="{gy:.1f}" text-anchor="middle">same pin on both boards when stacked</text>')

W = (MX * 2 + 100 + GAP) * S; H = (MY + 50 + 7.6) * S
svg = (f'<svg viewBox="0 0 {W:.0f} {H:.0f}" role="img" aria-label="To-scale two-board wiring. Board A carries the XIAO ESP32-C5; Board B carries the '
       f'E07-433M20S, the E01-2G4M27SX and a 3.3 volt buck module. Every wire runs from a part pad to an M-Bus tap pad; the two boards share those bus pins when stacked.">\n'
       + "\n".join(out) + "\n</svg>\n")
import sys
open(sys.argv[1] if len(sys.argv) > 1 else "two_board.svg", "w").write(svg)
print(f"wrote {len(svg)} bytes, viewBox {W:.0f}x{H:.0f}")
