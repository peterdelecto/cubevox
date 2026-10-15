"""1.3in SH1106 I2C OLED module on 9 mm standoffs, from the vendor drawing.

Model origin is the module centre, y up toward the pin row. The footprint puts the
pin row at y 0 and offsets the model by 14.75 mm.
Run with python3.12 (the only interpreter with CadQuery).
"""
import cadquery as cq

OUT = "/Users/j/Documents/Claude/Projects/cubevox/hardware/cubevox.3dshapes/OLED-1.3-SH1106-I2C-4P.step"

STANDOFF = 9.0
PCB_W, PCB_H, PCB_T = 35.4, 33.5, 1.2
HOLE_X, HOLE_Y, HOLE_D = 15.2, 14.25, 3.0
NOTCH_W, NOTCH_H = 14.0, 3.0
GLASS_W, GLASS_H, GLASS_GAP, GLASS_T = 34.5, 23.0, 0.2, 1.3
PIN_Y, PIN_PITCH = 14.75, 2.54
HEADER_T, PIN_TIP = 2.5, 0.5

pcbTop = STANDOFF + PCB_T

pcb = (cq.Workplane().box(PCB_W, PCB_H, PCB_T, centered=(True, True, False))
       .translate((0, 0, STANDOFF))
       .faces(">Z").workplane()
       .pushPoints([(sx * HOLE_X, sy * HOLE_Y) for sx in (-1, 1) for sy in (-1, 1)])
       .hole(HOLE_D)
       .cut(cq.Workplane().box(NOTCH_W, NOTCH_H, 10, centered=(True, False, False))
            .translate((0, -PCB_H / 2, STANDOFF - 1))))

# Glass is centred on the module: 5.25 mm from each long edge, 0.45 mm from each side

glass = (cq.Workplane().box(GLASS_W, GLASS_H, GLASS_T, centered=(True, True, False))
         .translate((0, 0, pcbTop + GLASS_GAP)))

header = (cq.Workplane().box(4 * PIN_PITCH, PIN_PITCH, HEADER_T, centered=(True, True, False))
          .translate((0, PIN_Y, STANDOFF - HEADER_T)))

pinXs = [(i - 1.5) * PIN_PITCH for i in range(4)]
pins = (cq.Workplane().pushPoints([(x, PIN_Y) for x in pinXs])
        .rect(0.64, 0.64).extrude(pcbTop - PIN_TIP)
        .translate((0, 0, PIN_TIP)))

asm = (cq.Assembly(name="OLED-1.3-SH1106-I2C-4P")
       .add(pcb, name="module_pcb", color=cq.Color(0.05, 0.15, 0.55))
       .add(glass, name="glass", color=cq.Color(0.02, 0.02, 0.03))
       .add(header, name="header_body", color=cq.Color(0.05, 0.05, 0.05))
       .add(pins, name="pins", color=cq.Color(0.85, 0.7, 0.3)))
asm.export(OUT)
print("wrote", OUT)
