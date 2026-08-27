# Fritzing breadboard view (rev 3.2)

Pictorial wiring diagram in Fritzing format, generated from
`../wiring.dot`. The schematic source of truth remains the KiCad
project (`../chytra-budka.kicad_sch`); this view exists purely for
visually friendly README / docs renders.

## Files

| File                                  | Role                                                                                                           |
| ------------------------------------- | -------------------------------------------------------------------------------------------------------------- |
| `parts/seeed_xiao_esp32s3_sense.fzpz` | vendored Seeed Studio XIAO ESP32-S3 Sense part (not in Fritzing core)                                          |
| `partmap.yaml`                        | maps wiring.dot nodes → moduleIdRef + connector ids                                                            |
| `generate_fritzing.py`                | parses `wiring.dot` + partmap → emits `chytra-budka.fzz` (project bundle = `.fz` XML + embedded vendor parts)  |
| `chytra-budka.fzz`                    | generated project bundle (committed, regenerable, opens directly in Fritzing without any user-library install) |

## One-time install (Arch / AUR)

```bash
yay -S fritzing                       # 1.0.7+
python3 -m pip install --user PyYAML  # generator dep
```

## Workflow

```bash
make -C firmware/hw fritzing       # regenerate chytra-budka.fzz from wiring.dot
Fritzing firmware/hw/fritzing/chytra-budka.fzz
# or:
make -C firmware/hw fritzing-open  # regen + open GUI
```

The `.fzz` is a ZIP containing the `.fz` XML plus every vendored part
file (`part.<id>.fzp`, `svg.<view>.<id>.svg`) extracted from
`parts/*.fzpz`. Fritzing resolves the embedded parts at project scope,
so you don't need to import anything into the user parts library.

After the project opens:

1. Drag components into a clean layout (parts start in a coarse grid).
2. Add bend points to wires for nicer routing (`right-click → add bend point`).
3. `File → Save` writes back to `chytra-budka.fzz` with your manual layout
   preserved.
4. `File → Export → as Image → PNG` → save into
   `firmware/hw/wiring_breadboard.png` and reference from the top-level README.

> ⚠ Re-running `make fritzing` overwrites `chytra-budka.fzz` from
> scratch and discards any manual layout. Commit your hand-arranged
> `.fzz` once you're happy with it, and only regenerate when
> `wiring.dot` connectivity changes.

## Known substitutions

The default partmap uses Fritzing-core stand-ins where vendoring is not
worth the cost. Each is documented in `partmap.yaml` under the `note:`
key. To upgrade the diagram to fully accurate parts:

| Logical node | Default stand-in                                | Upgrade path                                                                                                      |
| ------------ | ----------------------------------------------- | ----------------------------------------------------------------------------------------------------------------- |
| `SHT41`      | HMC5883L breakout (same GND/VCC/SDA/SCL pinout) | Drop an Adafruit SHT4x `.fzpz` into `parts/`, swap the `moduleIdRef:` in `partmap.yaml`, regen                    |
| `TP4056`     | SparkFun LiPo USB Charger v21 (BAT + OUT only)  | Drop a community TP4056 `.fzpz` into `parts/`, update partmap (gains IN+/IN- terminals)                           |
| `IRDRV`      | HIH-4030 3-pin breakout (placeholder)           | Replace with discrete AO3400 N-MOSFET + 220 Ω resistor + IR LED in Fritzing GUI so the view shows the sub-circuit |

Adding a new vendor part is just: drop `.fzpz` into `parts/`, point
`partmap.yaml` at its moduleId + connector ids, run `make fritzing` —
the embedder picks up every `parts/*.fzpz` automatically.

## Limitations

- Generator emits straight wires; bend points are added manually.
- Schematic / PCB views are not populated. KiCad remains the schematic
  source of truth (`../generate_kicad_sch.py`).
- Wire colour palette is mapped from `wiring.dot` `color=` attributes
  via a small lookup in `generate_fritzing.py::WIRE_COLOUR` — extend it
  when you add new colours to the .dot.
