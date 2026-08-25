#!/usr/bin/env python3
"""
Generate a real KiCad 9/10 .kicad_sch file for Chytrá Budka rev3.2.

Approach:
- Extract real symbol definitions from /usr/share/kicad/symbols/*.kicad_sym
- Place symbols on an A2 sheet in a grid
- Attach a global_label to each pin (label text = net name)
- Save as chytra-budka.kicad_sch

This produces a fully openable, editable schematic with real symbols and
proper net connectivity (via global labels). User can rearrange in GUI.
"""

import os
import re
import uuid

from sexpdata import Symbol, loads

SYMBOL_DIR = "/usr/share/kicad/symbols"
OUT_FILE = "chytra-budka.kicad_sch"

# ─── Library symbol cache ──────────────────────────────────────────
_lib_cache: dict[str, str] = {}


def load_lib(libname: str) -> str:
    if libname not in _lib_cache:
        with open(f"{SYMBOL_DIR}/{libname}.kicad_sym") as f:
            _lib_cache[libname] = f.read()
    return _lib_cache[libname]


def extract_symbol(libname: str, name: str) -> str:
    """Return raw S-expression text of a library symbol."""
    data = load_lib(libname)
    pat = re.compile(r'\(symbol\s+"' + re.escape(name) + r'"')
    m = pat.search(data)
    if not m:
        raise KeyError(f"{libname}:{name}")
    start = m.start()
    depth = 0
    for i in range(start, len(data)):
        if data[i] == "(":
            depth += 1
        elif data[i] == ")":
            depth -= 1
            if depth == 0:
                return data[start : i + 1]
    raise ValueError("unmatched paren")


def get_pins(symbol_text: str):
    """Extract pin info: list of (number, name, x, y, angle)."""
    parsed = loads(symbol_text)
    pins = []

    def walk(node):
        if not isinstance(node, list):
            return
        if node and isinstance(node[0], Symbol) and node[0].value() == "pin":
            x, y, ang = 0.0, 0.0, 0.0
            num, name = "", ""
            for child in node[1:]:
                if isinstance(child, list) and child:
                    head = child[0].value() if isinstance(child[0], Symbol) else ""
                    if head == "at":
                        x, y, ang = float(child[1]), float(child[2]), float(child[3])
                    elif head == "name" and isinstance(child[1], str):
                        name = child[1]
                    elif head == "number" and isinstance(child[1], str):
                        num = child[1]
            pins.append((num, name, x, y, ang))
        for c in node:
            if isinstance(c, list):
                walk(c)

    walk(parsed)
    return pins


def gen_uuid() -> str:
    return str(uuid.uuid4())


# ─── Schematic component instances ─────────────────────────────────
# Each entry: (ref, lib_name, sym_name, value, x_mm, y_mm, pin_to_net_map)
# IMPORTANT: x,y must be multiples of 2.54mm (KiCad schematic grid).
# Helper: G = 2.54 (one grid unit). Layout below uses G-based positions.
G = 2.54

COMPONENTS = [
    # XIAO ESP32-S3 Sense — Conn_01x14, pins on left side
    (
        "U1",
        "Connector_Generic",
        "Conn_01x14",
        "XIAO ESP32-S3 Sense",
        20 * G,
        30 * G,
        {
            "1": "",
            "2": "",  # D0, D1 unused
            "3": "PIR_OUT",  # D2 GPIO3 PIR wake
            "4": "IR_PWM",  # D3 GPIO4 IR PWM
            "5": "I2C_SDA",  # D4 GPIO5 SDA
            "6": "I2C_SCL",  # D5 GPIO6 SCL
            "7": "",
            "8": "",  # D6,D7 UART unused
            "9": "I2S_WS",  # D8 GPIO7
            "10": "I2S_SD",  # D9 GPIO8
            "11": "+3V3",  # 3V3 out (LDO)
            "12": "GND",
            "13": "+5V",  # 5V in
            "14": "I2S_SCK",  # D10 GPIO9
        },
    ),
    # WaveShare Solar PMS — Conn_01x06
    (
        "U2",
        "Connector_Generic",
        "Conn_01x06",
        "WaveShare SolarPMS",
        60 * G,
        30 * G,
        {
            "1": "V_SOLAR",
            "2": "GND",
            "3": "V_BAT",
            "4": "GND",
            "5": "+5V",
            "6": "GND",
        },
    ),
    # MAX17048 breakout — Conn_01x04
    (
        "U3",
        "Connector_Generic",
        "Conn_01x04",
        "MAX17048 breakout",
        95 * G,
        30 * G,
        {
            "1": "V_BAT",
            "2": "GND",
            "3": "I2C_SDA",
            "4": "I2C_SCL",
        },
    ),
    # SHT41 — Sensor_Humidity:SHT4x
    (
        "U4",
        "Sensor_Humidity",
        "SHT4x",
        "SHT41-AD1B",
        130 * G,
        30 * G,
        {
            "1": "I2C_SDA",
            "2": "I2C_SCL",
            "3": "+3V3",
            "4": "GND",
        },
    ),
    # Solar input barrel jack — Connector:Barrel_Jack
    (
        "J1",
        "Connector",
        "Barrel_Jack",
        "DC Solar 18V",
        20 * G,
        60 * G,
        {
            "1": "V_SOLAR",
            "2": "GND",
        },
    ),
    # 2x18650 holder — Conn_01x02
    (
        "J2",
        "Connector_Generic",
        "Conn_01x02",
        "2x18650 1S2P PCM",
        50 * G,
        60 * G,
        {
            "1": "V_BAT",
            "2": "GND",
        },
    ),
    # INMP441 mic — Conn_01x06
    (
        "J3",
        "Connector_Generic",
        "Conn_01x06",
        "INMP441 mic",
        80 * G,
        60 * G,
        {
            "1": "+3V3",  # VDD
            "2": "GND",
            "3": "GND",  # L/R = GND for mono left
            "4": "I2S_WS",
            "5": "I2S_SCK",
            "6": "I2S_SD",
        },
    ),
    # AM312 PIR — Conn_01x03
    (
        "J4",
        "Connector_Generic",
        "Conn_01x03",
        "AM312 PIR",
        110 * G,
        60 * G,
        {
            "1": "+3V3",
            "2": "PIR_OUT",
            "3": "GND",
        },
    ),
    # IR LED 940nm — Conn_01x02
    (
        "J5",
        "Connector_Generic",
        "Conn_01x02",
        "IR LED 940nm",
        145 * G,
        60 * G,
        {
            "1": "IR_LED_A",
            "2": "IR_DRAIN",
        },
    ),
    # I2C SDA pull-up
    (
        "R1",
        "Device",
        "R",
        "10k",
        110 * G,
        18 * G,
        {
            "1": "+3V3",
            "2": "I2C_SDA",
        },
    ),
    # I2C SCL pull-up
    (
        "R2",
        "Device",
        "R",
        "10k",
        120 * G,
        18 * G,
        {
            "1": "+3V3",
            "2": "I2C_SCL",
        },
    ),
    # IR LED current limit
    (
        "R3",
        "Device",
        "R",
        "220R/0.5W",
        145 * G,
        50 * G,
        {
            "1": "+5V",
            "2": "IR_LED_A",
        },
    ),
    # SHT41 decoupling
    (
        "C1",
        "Device",
        "C",
        "100nF",
        140 * G,
        42 * G,
        {
            "1": "+3V3",
            "2": "GND",
        },
    ),
    # MAX17048 decoupling
    (
        "C2",
        "Device",
        "C",
        "100nF",
        95 * G,
        50 * G,
        {
            "1": "V_BAT",
            "2": "GND",
        },
    ),
    # PIR ESD diode
    (
        "D1",
        "Device",
        "D",
        "1N4148",
        110 * G,
        75 * G,
        {
            "1": "PIR_OUT",
            "2": "+3V3",
        },
    ),
    # IR LED MOSFET driver
    (
        "Q1",
        "Device",
        "Q_NMOS",
        "AO3400",
        155 * G,
        75 * G,
        {
            "G": "IR_PWM",
            "S": "GND",
            "D": "IR_DRAIN",
        },
    ),
    # PWR_FLAG markers — required by ERC for power nets driven from outside
    # the schematic (USB rail, LDO output, ground reference).
    ("#FLG1", "power", "PWR_FLAG", "PWR_FLAG", 30 * G, 12 * G, {"1": "+5V"}),
    ("#FLG2", "power", "PWR_FLAG", "PWR_FLAG", 40 * G, 12 * G, {"1": "+3V3"}),
    ("#FLG3", "power", "PWR_FLAG", "PWR_FLAG", 50 * G, 12 * G, {"1": "GND"}),
    ("#FLG4", "power", "PWR_FLAG", "PWR_FLAG", 60 * G, 12 * G, {"1": "V_BAT"}),
    ("#FLG5", "power", "PWR_FLAG", "PWR_FLAG", 70 * G, 12 * G, {"1": "V_SOLAR"}),
]


# ─── Build .kicad_sch ──────────────────────────────────────────────
def build_lib_symbols():
    """Build (lib_symbols ...) section with all unique symbols used."""
    seen = set()
    chunks = []
    for ref, libname, symname, *_ in COMPONENTS:
        key = f"{libname}:{symname}"
        if key in seen:
            continue
        seen.add(key)
        # Extract symbol and rewrite ONLY the outer name to "Lib:Name"
        # (inner sub-symbols like "Conn_01x04_1_1" must keep their original name)
        text = extract_symbol(libname, symname)
        text = text.replace(f'(symbol "{symname}"', f'(symbol "{libname}:{symname}"', 1)
        chunks.append("\t\t" + text.replace("\n", "\n\t\t"))
    return "\t(lib_symbols\n" + "\n".join(chunks) + "\n\t)\n"


def build_instance(ref, libname, symname, value, x, y, pin_map):
    """Build (symbol ...) instance + global_labels for each connected pin."""
    inst_uuid = gen_uuid()
    # Property positions are slight offset from origin
    parts = [
        "\t(symbol",
        f'\t\t(lib_id "{libname}:{symname}")',
        f"\t\t(at {x} {y} 0)",
        "\t\t(unit 1)",
        "\t\t(exclude_from_sim no)",
        "\t\t(in_bom yes)",
        "\t\t(on_board yes)",
        "\t\t(dnp no)",
        f'\t\t(uuid "{inst_uuid}")',
        f'\t\t(property "Reference" "{ref}" (at {x + 5} {y - 15} 0) (effects (font (size 1.27 1.27))))',
        f'\t\t(property "Value" "{value}" (at {x + 5} {y - 12} 0) (effects (font (size 1.27 1.27))))',
        f'\t\t(property "Footprint" "" (at {x} {y} 0) (effects (font (size 1.27 1.27)) hide))',
        f'\t\t(property "Datasheet" "" (at {x} {y} 0) (effects (font (size 1.27 1.27)) hide))',
        f'\t\t(property "Description" "" (at {x} {y} 0) (effects (font (size 1.27 1.27)) hide))',
    ]
    # Pin UUIDs
    pins = get_pins(extract_symbol(libname, symname))
    for num, name, *_ in pins:
        parts.append(f'\t\t(pin "{num}" (uuid "{gen_uuid()}"))')
    parts.append(
        f'\t\t(instances (project "chytra-budka" (path "/" (reference "{ref}") (unit 1))))'
    )
    parts.append("\t)")
    inst_block = "\n".join(parts)

    # Global labels at each connected pin + no_connect markers for unconnected
    labels = []
    for num, name, px, py, ang in pins:
        net = pin_map.get(num, "")
        wx = x + px
        wy = y - py
        if not net:
            # Add no_connect marker on intentionally unconnected pin
            labels.append(f'\t(no_connect (at {wx} {wy}) (uuid "{gen_uuid()}"))')
            continue
        # Label angle = opposite to pin direction (text reads outward)
        label_ang = (ang + 180) % 360
        lbl_uuid = gen_uuid()
        # global_label shape for power = "input", for signals = "bidirectional"
        shape = "input" if net.startswith("+") or net == "GND" else "bidirectional"
        labels.append(
            f'\t(global_label "{net}"\n'
            f"\t\t(shape {shape})\n"
            f"\t\t(at {wx} {wy} {int(label_ang)})\n"
            f"\t\t(effects (font (size 1.27 1.27)) (justify left))\n"
            f'\t\t(uuid "{lbl_uuid}")\n'
            f'\t\t(property "Intersheetrefs" "${{INTERSHEET_REFS}}" (at {wx} {wy} 0) (effects (font (size 1.27 1.27)) hide))\n'
            f"\t)"
        )
    return inst_block + "\n" + "\n".join(labels)


# ─── Assemble final document ───────────────────────────────────────
header = f'''(kicad_sch
\t(version 20250114)
\t(generator "eeschema")
\t(generator_version "10.0")
\t(uuid "{gen_uuid()}")
\t(paper "A2")
\t(title_block
\t\t(title "Chytrá Budka rev3.2")
\t\t(date "2026-05-07")
\t\t(rev "3.2")
\t)
'''

body_lib = build_lib_symbols()
body_instances = "\n".join(
    build_instance(ref, libname, symname, value, x, y, pin_map)
    for ref, libname, symname, value, x, y, pin_map in COMPONENTS
)
footer = '\t(sheet_instances\n\t\t(path "/" (page "1"))\n\t)\n\t(embedded_fonts no)\n)\n'

with open(OUT_FILE, "w") as f:
    f.write(header)
    f.write(body_lib)
    f.write(body_instances)
    f.write("\n")
    f.write(footer)

print(f"✓ {OUT_FILE} written ({os.path.getsize(OUT_FILE)} bytes)")
print("  Open: kicad firmware/hw/chytra-budka.kicad_pro")
