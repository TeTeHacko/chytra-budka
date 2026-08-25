#!/usr/bin/env python3
"""
Generate `chytra-budka.fzz` (Fritzing project bundle, breadboard view) from
`../wiring.dot` connectivity + `partmap.yaml` part bindings.

A .fzz is a ZIP containing:
  - the main `.fz` XML file (project)
  - vendored part files (`part.<id>.fzp`, `svg.<view>.<id>.svg`)
    extracted from `parts/*.fzpz`, so Fritzing resolves vendor moduleIds
    at project level without needing them pre-installed in the user
    parts library.

The `.fz` schema we emit:
  - top-level <views> declaring the three views with default settings
  - one part <instance> per partmap entry, with <connectors> listing
    each used connector and *bidirectional* <connects> back-references
    to wire endpoints
  - one wire <instance> (moduleIdRef="WireModuleID") per wiring.dot
    edge, layer="breadboardWire", with straight geometry and <connects>
    referencing the two part connectors

Initial part placement is a coarse grid (col,row from partmap.yaml).

Usage:
    python3 generate_fritzing.py [--out chytra-budka.fzz]
"""

from __future__ import annotations

import argparse
import re
import uuid
import zipfile
from pathlib import Path
from xml.sax.saxutils import escape

import yaml  # PyYAML

HERE = Path(__file__).resolve().parent
WIRING_DOT = HERE.parent / "wiring.dot"
PARTMAP_YAML = HERE / "partmap.yaml"
PARTS_DIR = HERE / "parts"
DEFAULT_OUT = HERE / "chytra-budka.fzz"
INNER_FZ_NAME = "chytra-budka.fz"

# Grid spacing in 1/72 inch (Fritzing internal unit).
GRID_X = 432.0
GRID_Y = 288.0
ORIGIN_X = 100.0
ORIGIN_Y = 100.0
WIRE_PIN_OFFSET = 12.0  # rough pin offset within a part for wire start

WIRE_COLOUR = {
    "red": "#e6492c",
    "black": "#1d1d1d",
    "blue": "#418fde",
    "green": "#3aa54c",
    "orange": "#ec9b1f",
    "goldenrod": "#daa520",
    "purple": "#a35cc1",
}


def parse_dot_edges(path: Path) -> list[dict]:
    text = path.read_text()
    edge_re = re.compile(
        r"(?P<sn>\w+)\s*:\s*(?P<sp>\w+)\s*->\s*(?P<dn>\w+)\s*:\s*(?P<dp>\w+)"
        r"\s*\[(?P<attrs>[^\]]*)\]"
    )
    attr_re = re.compile(r'(\w+)\s*=\s*"([^"]*)"')
    out: list[dict] = []
    for m in edge_re.finditer(text):
        attrs = dict(attr_re.findall(m.group("attrs")))
        out.append(
            dict(
                src_node=m.group("sn"),
                src_pin=m.group("sp"),
                dst_node=m.group("dn"),
                dst_pin=m.group("dp"),
                label=attrs.get("label", ""),
                color=attrs.get("color", "black"),
            )
        )
    return out


def colour_hex(name: str) -> str:
    if name.startswith("#"):
        return name
    return WIRE_COLOUR.get(name, "#1d1d1d")


def grid_xy(spec: dict) -> tuple[float, float]:
    col, row = spec["grid"]
    return ORIGIN_X + col * GRID_X, ORIGIN_Y + row * GRID_Y


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT)
    args = ap.parse_args()

    partmap: dict = yaml.safe_load(PARTMAP_YAML.read_text())
    edges = parse_dot_edges(WIRING_DOT)

    nodes = list(partmap.keys())
    node_index = {n: i for i, n in enumerate(nodes)}
    wire_offset = len(nodes)

    # Validate
    errors: list[str] = []
    for e in edges:
        for side in ("src", "dst"):
            n, p = e[f"{side}_node"], e[f"{side}_pin"]
            if n not in partmap:
                errors.append(f"unknown node: {n}")
                continue
            if p not in partmap[n]["pins"]:
                errors.append(f"node {n} has no pin {p}")
    if errors:
        for err in sorted(set(errors)):
            print(f"ERROR: {err}")
        return 1

    # Build connectivity index:
    #   per-part used connectors -> list of (wire_modelIndex, wire_endpoint_id)
    #   wire endpoints: connector0 = src side, connector1 = dst side
    part_conn_uses: dict[tuple[int, str], list[tuple[int, str]]] = {}
    for i, e in enumerate(edges):
        wire_idx = wire_offset + i
        src_inst = node_index[e["src_node"]]
        dst_inst = node_index[e["dst_node"]]
        src_conn = partmap[e["src_node"]]["pins"][e["src_pin"]]
        dst_conn = partmap[e["dst_node"]]["pins"][e["dst_pin"]]
        part_conn_uses.setdefault((src_inst, src_conn), []).append((wire_idx, "connector0"))
        part_conn_uses.setdefault((dst_inst, dst_conn), []).append((wire_idx, "connector1"))

    # ─── Render parts ───────────────────────────────────────────────
    parts_xml = ""
    for n in nodes:
        idx = node_index[n]
        spec = partmap[n]
        x, y = grid_xy(spec)
        used_connectors = sorted({conn for (pidx, conn) in part_conn_uses if pidx == idx})
        connectors_xml = ""
        for conn in used_connectors:
            connects = part_conn_uses.get((idx, conn), [])
            connects_xml = "".join(
                f'                                <connect connectorId="{wc}" '
                f'modelIndex="{wi}" layer="breadboardWire"/>\n'
                for (wi, wc) in connects
            )
            connectors_xml += (
                f'                        <connector connectorId="{conn}" '
                f'layer="breadboard">\n'
                f"                            <connects>\n"
                f"{connects_xml}"
                f"                            </connects>\n"
                f"                        </connector>\n"
            )
        parts_xml += (
            f'        <instance moduleIdRef="{escape(spec["moduleIdRef"])}" '
            f'modelIndex="{idx}" path="" flippedSMD="false">\n'
            f"            <title>{escape(n)}</title>\n"
            f"            <views>\n"
            f'                <breadboardView layer="breadboard">\n'
            f'                    <geometry z="2.0" x="{x:.1f}" y="{y:.1f}"/>\n'
            f"                    <connectors>\n"
            f"{connectors_xml}"
            f"                    </connectors>\n"
            f"                </breadboardView>\n"
            f"            </views>\n"
            f"        </instance>\n"
        )

    # ─── Render wires ───────────────────────────────────────────────
    wires_xml = ""
    for i, e in enumerate(edges):
        wire_idx = wire_offset + i
        src_inst = node_index[e["src_node"]]
        dst_inst = node_index[e["dst_node"]]
        src_conn = partmap[e["src_node"]]["pins"][e["src_pin"]]
        dst_conn = partmap[e["dst_node"]]["pins"][e["dst_pin"]]
        sx, sy = grid_xy(partmap[e["src_node"]])
        dx, dy = grid_xy(partmap[e["dst_node"]])
        # Wire start = src part origin + small pin offset; end is delta to dst.
        wx, wy = sx + WIRE_PIN_OFFSET, sy + WIRE_PIN_OFFSET
        x2, y2 = (dx - sx), (dy - sy)
        col = colour_hex(e["color"])
        title = escape(e["label"]) if e["label"] else f"wire{i}"
        wires_xml += (
            f'        <instance moduleIdRef="WireModuleID" '
            f'modelIndex="{wire_idx}" '
            f'path=":/resources/parts/core/wire.fzp" flippedSMD="false">\n'
            f"            <title>{title}</title>\n"
            f"            <views>\n"
            f'                <breadboardView layer="breadboardWire">\n'
            f'                    <geometry z="3.5" x="{wx:.1f}" y="{wy:.1f}" '
            f'x1="0" y1="0" x2="{x2:.1f}" y2="{y2:.1f}" wireFlags="64"/>\n'
            f'                    <wireExtras mils="22.2222" color="{col}" '
            f'opacity="1.0" banded="0"/>\n'
            f"                    <connectors>\n"
            f'                        <connector connectorId="connector0" '
            f'layer="breadboardWire">\n'
            f'                            <geometry x="0" y="0"/>\n'
            f"                            <connects>\n"
            f'                                <connect connectorId="{src_conn}" '
            f'modelIndex="{src_inst}" layer="breadboard"/>\n'
            f"                            </connects>\n"
            f"                        </connector>\n"
            f'                        <connector connectorId="connector1" '
            f'layer="breadboardWire">\n'
            f'                            <geometry x="0" y="0"/>\n'
            f"                            <connects>\n"
            f'                                <connect connectorId="{dst_conn}" '
            f'modelIndex="{dst_inst}" layer="breadboard"/>\n'
            f"                            </connects>\n"
            f"                        </connector>\n"
            f"                    </connectors>\n"
            f"                </breadboardView>\n"
            f"            </views>\n"
            f"        </instance>\n"
        )

    project_uuid = str(uuid.uuid4())
    fz = f"""<?xml version="1.0" encoding="UTF-8" standalone="no"?>
<module fritzingVersion="1.0.0" moduleId="{project_uuid}">
    <title>Chytrá Budka rev3.2</title>
    <author>generate_fritzing.py</author>
    <description>Generated from firmware/hw/wiring.dot. Open in Fritzing, arrange visually, save as .fzz.</description>
    <views>
        <view name="breadboardView" backgroundColor="#ffffff" gridSize="0.1in" showGrid="1" alignToGrid="0" viewFromBelow="0" colorWiresByLength="0"/>
        <view name="schematicView" backgroundColor="#ffffff" gridSize="0.1in" showGrid="1" alignToGrid="1" viewFromBelow="0"/>
        <view name="pcbView" backgroundColor="#333333" gridSize="0.025in" showGrid="1" alignToGrid="1" viewFromBelow="0"/>
    </views>
    <instances>
{parts_xml}{wires_xml}    </instances>
</module>
"""
    args.out.write_text  # noqa: B018  (kept for IDE; real write below)

    # Collect every vendored .fzpz's internal files for embedding in the .fzz.
    # The .fzpz layout is identical to what a .fzz expects: part.X.fzp + svg.<view>.Y.svg.
    embedded: list[tuple[str, bytes]] = []
    for fzpz in sorted(PARTS_DIR.glob("*.fzpz")):
        with zipfile.ZipFile(fzpz, "r") as z:
            for member in z.namelist():
                if member.endswith("/"):
                    continue
                embedded.append((member, z.read(member)))

    with zipfile.ZipFile(args.out, "w", zipfile.ZIP_DEFLATED) as out_zip:
        out_zip.writestr(INNER_FZ_NAME, fz)
        seen: set[str] = {INNER_FZ_NAME}
        for name, data in embedded:
            if name in seen:
                continue
            out_zip.writestr(name, data)
            seen.add(name)

    print(
        f"Wrote {args.out} ({len(nodes)} parts, {len(edges)} wires, "
        f"{len(embedded)} embedded part files, "
        f"{args.out.stat().st_size:,} bytes)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
