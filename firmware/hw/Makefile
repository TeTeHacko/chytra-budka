# Chytrá Budka rev3.2 — Hardware (KiCad project)
#
# Targets:
#   make gerbers     - Export JLCPCB-ready Gerbers + drill + BOM + CPL into out/
#   make bom         - Export plain BOM CSV (KiCad native)
#   make schematic   - Export schematic to PDF
#   make 3d          - Generate 3D HTML preview via kikit present
#   make erc         - Run Electrical Rules Check on schematic
#   make drc         - Run Design Rules Check on PCB
#   make clean       - Remove out/ directory
#
# Requires: kicad >= 10, python-kikit >= 1.8

PROJECT     := chytra-budka
SCH         := $(PROJECT).kicad_sch
PCB         := $(PROJECT).kicad_pcb
OUT_DIR     := out
JLC_PRESET  := presets/jlcpcb.json

.PHONY: all gerbers bom schematic 3d erc drc clean help netlist blockdiagram wiring fritzing fritzing-open

all: netlist blockdiagram schematic bom wiring

wiring:  ## Render block wiring diagram (wiring.svg + wiring.png)
	dot -Tsvg wiring.dot -o wiring.svg
	dot -Tpng -Gdpi=140 wiring.dot -o wiring.png
	@echo "→ open wiring.svg in browser"

fritzing:  ## Regenerate Fritzing breadboard project (chytra-budka.fzz) from wiring.dot
	python3 fritzing/generate_fritzing.py

fritzing-open: fritzing  ## Regenerate, then open the bundled .fzz in Fritzing GUI
	Fritzing fritzing/chytra-budka.fzz &

help:
	@grep -E '^[a-zA-Z_-]+:.*?#' $(MAKEFILE_LIST) | awk -F':.*?#' '{printf "  %-12s %s\n", $$1, $$2}'

$(OUT_DIR):
	mkdir -p $(OUT_DIR)

netlist: | $(OUT_DIR)  ## Generate KiCad netlist from generate_schematic.py
	python generate_schematic.py 2>&1 | grep -vE "WARNING.*KICAD[0-9]+_SYMBOL|fp-lib-table|Missing tag|Random tag" || true

blockdiagram: netlist  ## Render component connectivity as SVG (graphviz)
	dot -Tsvg $(OUT_DIR)/$(PROJECT).dot -o $(OUT_DIR)/blockdiagram.svg
	@echo "→ open $(OUT_DIR)/blockdiagram.svg in browser"

gerbers: | $(OUT_DIR)  ## JLCPCB-ready Gerbers + drill + BOM + CPL (requires .kicad_pcb)
	kikit fab jlcpcb --no-drc --schematic $(SCH) $(PCB) $(OUT_DIR)
	@echo "Upload $(OUT_DIR)/gerbers.zip to JLCPCB → BOM/CPL files in $(OUT_DIR)/"

bom: | $(OUT_DIR)  ## Plain BOM CSV from .kicad_sch
	kicad-cli sch export bom --output $(OUT_DIR)/bom.csv $(SCH)

schematic: | $(OUT_DIR)  ## Schematic PDF
	kicad-cli sch export pdf --output $(OUT_DIR)/$(PROJECT)-schematic.pdf $(SCH)

3d: | $(OUT_DIR)  ## 3D HTML preview
	kikit present boardpage \
		--name "Chytrá Budka rev3.2" \
		--description README.md \
		--board $(PCB) \
		--resource README.md \
		$(OUT_DIR)/3d

erc:  ## Electrical Rules Check
	kicad-cli sch erc --severity-error --severity-warning $(SCH)

drc:  ## Design Rules Check
	kicad-cli pcb drc --severity-error --severity-warning $(PCB)

clean:  ## Remove out/ directory
	rm -rf $(OUT_DIR)
