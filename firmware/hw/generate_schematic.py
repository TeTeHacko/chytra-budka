#!/usr/bin/env python3
"""
Generate KiCad schematic + netlist for Chytrá Budka rev3.2 carrier board.

Usage:
    python generate_schematic.py
    # → produces chytra-budka.kicad_sch (overwrites!) and chytra-budka.net

Components are mostly external modules (XIAO, WaveShare, INMP441 boards),
represented as pin headers. Discrete chips (MAX17048, SHT41) use real symbols.

Layout from SKiDL is automatic and often messy — open in KiCad GUI and
rearrange manually for a clean diagram. Connectivity is correct regardless.
"""

import os

# Point SKiDL at system KiCad symbol libs BEFORE importing skidl
os.environ["KICAD_SYMBOL_DIR"] = "/usr/share/kicad/symbols"
os.environ["KICAD9_SYMBOL_DIR"] = "/usr/share/kicad/symbols"

from skidl import (
    ERC,
    KICAD9,
    Net,
    Part,
    generate_dot,
    generate_netlist,
    generate_schematic,
    generate_svg,
    lib_search_paths,
    set_default_tool,
)

set_default_tool(KICAD9)
lib_search_paths[KICAD9].append("/usr/share/kicad/symbols")

# ─────────────────────────── Power nets ──────────────────────────────
gnd = Net("GND")
vcc_5v = Net("+5V")
vcc_3v3 = Net("+3V3")
v_bat = Net("V_BAT")
v_solar = Net("V_SOLAR")

# ─────────────────────────── Signal nets ─────────────────────────────
sda = Net("I2C_SDA")
scl = Net("I2C_SCL")
i2s_ws = Net("I2S_WS")
i2s_sd = Net("I2S_SD")
i2s_sck = Net("I2S_SCK")
pir_out = Net("PIR_OUT")
ir_pwm = Net("IR_PWM")

# ─────────────────────────── Components ──────────────────────────────
# XIAO ESP32-S3 Sense — 14 pins (7+7) on castellated edges, but
# represented here as a single 1×14 header.  Pin numbering matches
# Seeed pinout: 1=D0 ... 7=D6 ... 8=D7 ... 11=3V3 ... 12=GND ... 13=5V ... 14=D10.
# We use Connector_Generic:Conn_01x14 as a placeholder; real Seeed XIAO
# symbol from their lib should be swapped in inside KiCad GUI.
xiao = Part(
    "Connector_Generic",
    "Conn_01x14",
    ref="U1",
    value="XIAO_ESP32-S3_Sense",
    footprint="Module:Seeed_XIAO_ESP32S3",
)
xiao[1] += Net("D0_NC")  # D0  GPIO1  (unused)
xiao[2] += Net("D1_NC")  # D1  GPIO2  (unused)
xiao[3] += pir_out  # D2  GPIO3  PIR wake
xiao[4] += ir_pwm  # D3  GPIO4  IR LED PWM (gate)
xiao[5] += sda  # D4  GPIO5  I²C SDA
xiao[6] += scl  # D5  GPIO6  I²C SCL
xiao[7] += Net("UART_TX_NC")  # D6  GPIO43 UART TX (unused)
xiao[8] += Net("UART_RX_NC")  # D7  GPIO44 UART RX (unused)
xiao[9] += i2s_ws  # D8  GPIO7  I²S WS
xiao[10] += i2s_sd  # D9  GPIO8  I²S SD
xiao[11] += vcc_3v3  # 3V3 (output from internal LDO)
xiao[12] += gnd  # GND
xiao[13] += vcc_5v  # 5V in
xiao[14] += i2s_sck  # D10 GPIO9 I²S SCK

# WaveShare Solar Power Manager (basic, CN3791) — module with PH2.0 BAT
# connector + USB OUT + DC solar IN.  Represented as 1×6 header.
ws = Part(
    "Connector_Generic",
    "Conn_01x06",
    ref="U2",
    value="WaveShare_SolarPMS",
    footprint="Connector_PinHeader_2.54mm:PinHeader_1x06_P2.54mm_Vertical",
)
ws[1] += v_solar  # SOLAR+
ws[2] += gnd  # SOLAR− / GND
ws[3] += v_bat  # BAT+
ws[4] += gnd  # BAT−
ws[5] += vcc_5v  # USB OUT 5V
ws[6] += gnd  # USB OUT GND

# Solar panel input (DC barrel jack)
solar_jack = Part(
    "Connector",
    "Barrel_Jack",
    ref="J1",
    value="DC_Solar_18V",
    footprint="Connector_BarrelJack:BarrelJack_Horizontal",
)
solar_jack[1] += v_solar
solar_jack[2] += gnd

# 2× 18650 1S2P holder with PCM (DW01 + 8205A) — represented as 2-pin conn
bat = Part(
    "Connector_Generic",
    "Conn_01x02",
    ref="J2",
    value="2x18650_1S2P_PCM",
    footprint="Connector_JST:JST_PH_S2B-PH-K_1x02_P2.00mm_Horizontal",
)
bat[1] += v_bat
bat[2] += gnd

# MAX17048 fuel gauge — represented as 4-pin breakout (Adafruit/AliExpress).
# Real chip is TDFN-8 but user buys breakout module; place via Conn_01x04.
max17048 = Part(
    "Connector_Generic",
    "Conn_01x04",
    ref="U3",
    value="MAX17048_breakout",
    footprint="Connector_PinHeader_2.54mm:PinHeader_1x04_P2.54mm_Vertical",
)
max17048[1] += v_bat  # VIN (B+)
max17048[2] += gnd  # GND
max17048[3] += sda  # SDA
max17048[4] += scl  # SCL

# I²C pull-ups (10 kΩ each)
r_sda = Part("Device", "R", ref="R1", value="10k", footprint="Resistor_SMD:R_0603_1608Metric")
r_scl = Part("Device", "R", ref="R2", value="10k", footprint="Resistor_SMD:R_0603_1608Metric")
r_sda[1] += vcc_3v3
r_sda[2] += sda
r_scl[1] += vcc_3v3
r_scl[2] += scl

# SHT41 temperature + humidity sensor — real symbol from default lib
sht41 = Part(
    "Sensor_Humidity",
    "SHT4x",
    ref="U4",
    value="SHT41-AD1B",
    footprint="Package_DFN_QFN:DFN-4-1EP_1.5x1.5mm_P0.8mm_EP0.7x1.1mm",
)
sht41["VDD"] += vcc_3v3
sht41["VSS"] += gnd
sht41["SDA"] += sda
sht41["SCL"] += scl

# Decoupling caps for SHT41 and MAX17048
c_sht = Part("Device", "C", ref="C1", value="100nF", footprint="Capacitor_SMD:C_0603_1608Metric")
c_sht[1] += vcc_3v3
c_sht[2] += gnd
c_max = Part("Device", "C", ref="C2", value="100nF", footprint="Capacitor_SMD:C_0603_1608Metric")
c_max[1] += v_bat
c_max[2] += gnd

# INMP441 I²S MEMS microphone — 4-pin header (VDD, GND, SD, SCK, WS, L/R)
# breakout has 6 pins: VDD GND L/R WS SCK SD
inmp = Part(
    "Connector_Generic",
    "Conn_01x06",
    ref="J3",
    value="INMP441_breakout",
    footprint="Connector_JST:JST_PH_S6B-PH-K_1x06_P2.00mm_Horizontal",
)
inmp[1] += vcc_3v3  # VDD
inmp[2] += gnd  # GND
inmp[3] += gnd  # L/R = GND → mono left
inmp[4] += i2s_ws  # WS
inmp[5] += i2s_sck  # SCK
inmp[6] += i2s_sd  # SD

# AM312 PIR motion sensor — 3-pin header
pir = Part(
    "Connector_Generic",
    "Conn_01x03",
    ref="J4",
    value="AM312_PIR",
    footprint="Connector_JST:JST_PH_S3B-PH-K_1x03_P2.00mm_Horizontal",
)
pir[1] += vcc_3v3  # VCC
pir[2] += pir_out  # OUT
pir[3] += gnd  # GND

# Optional ESD protection on PIR_OUT (1N4148 to 3V3)
d_pir = Part("Device", "D", ref="D1", value="1N4148", footprint="Diode_SMD:D_SOD-123")
d_pir[1] += pir_out  # anode
d_pir[2] += vcc_3v3  # cathode (clamps PIR_OUT to 3V3 + Vf)

# IR LED 940 nm + N-MOSFET driver + current-limit resistor
mosfet = Part(
    "Device",
    "Q_NMOS",
    ref="Q1",
    value="AO3400",
    footprint="Package_TO_SOT_SMD:SOT-23",
)
mosfet["G"] += ir_pwm  # gate
mosfet["S"] += gnd  # source
ir_drain = Net("IR_DRAIN")
mosfet["D"] += ir_drain  # drain

r_ir = Part(
    "Device",
    "R",
    ref="R3",
    value="220R/0.5W",
    footprint="Resistor_SMD:R_1206_3216Metric",
)
r_ir[1] += vcc_5v  # from 5V rail
ir_anode = Net("IR_LED_A")
r_ir[2] += ir_anode

ir_led_conn = Part(
    "Connector_Generic",
    "Conn_01x02",
    ref="J5",
    value="IR_LED_940nm",
    footprint="Connector_JST:JST_PH_S2B-PH-K_1x02_P2.00mm_Horizontal",
)
ir_led_conn[1] += ir_anode  # anode through 220Ω
ir_led_conn[2] += ir_drain  # cathode → MOSFET drain

# Mounting holes (4× M3) — added directly in Pcbnew, not in schematic
# (KiCad 9 best practice: mounting holes are PCB-only, no schematic symbol)

# ─────────────────────────── ERC + outputs ───────────────────────────
ERC()
generate_netlist(file_="out/chytra-budka.net")

# Visual outputs (Graphviz block diagram + SVG schematic attempt)
try:
    generate_dot(file_="out/chytra-budka.dot")
    print("✓ out/chytra-budka.dot (run `dot -Tsvg out/chytra-budka.dot -o out/blockdiagram.svg`)")
except Exception as e:
    print(f"⚠ generate_dot failed: {e}")

try:
    generate_svg(file_="out/chytra-budka.svg")
    print("✓ out/chytra-budka.svg")
except Exception as e:
    print(f"⚠ generate_svg failed: {e}")

# generate_schematic() in SKiDL 2.2.x is experimental and unreliable on KiCad 9.
# Workflow instead: import the netlist directly into Pcbnew
# (File → Import → Netlist) — places all components with ratsnest connectivity.
# Schematic in Eeschema can be drawn later from the netlist for documentation.
try:
    generate_schematic(file_="chytra-budka.kicad_sch", flatness=0.4)
    print("✓ chytra-budka.kicad_sch generated")
except Exception as e:
    print(f"⚠ generate_schematic failed (known SKiDL/KiCad9 bug): {e}")
    print("  → Use out/chytra-budka.net via Pcbnew 'Import netlist' instead.")
