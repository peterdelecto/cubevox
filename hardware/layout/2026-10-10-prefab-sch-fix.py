"""Schematic side of the 2026-10-10 pre-fab fixes.

  python3 -I hardware/layout/2026-10-10-prefab-sch-fix.py hardware/cubevox.kicad_sch [--apply]

K103 pad contacts move from the NO pins 4/5 to the NC pins 2/7 so the rest state is MIC.
R162 100k 0402 (C25741) pulls MUX_S0 (PE12, the ROM bootloader's SPI4 clock) to GND.
"""
import sys
import uuid


def swap(s, old, new):
    if s.count(old) != 1:
        raise SystemExit('expected one match for %r, found %d' % (old, s.count(old)))
    return s.replace(old, new)


R162 = '''	(symbol
		(lib_id "Device:R")
		(at 111.76 115.57 0)
		(unit 1)
		(exclude_from_sim no)
		(in_bom yes)
		(on_board yes)
		(dnp no)
		(uuid "%(u0)s")
		(property "Reference" "R162"
			(at 114.3 114.3 0)
			(effects
				(font
					(size 1.27 1.27)
				)
				(justify left)
			)
		)
		(property "Value" "100k"
			(at 114.3 116.84 0)
			(effects
				(font
					(size 1.27 1.27)
				)
				(justify left)
			)
		)
		(property "Footprint" "Resistor_SMD:R_0402_1005Metric"
			(at 111.76 115.57 0)
			(hide yes)
			(effects
				(font
					(size 1.27 1.27)
				)
			)
		)
		(property "Datasheet" "~"
			(at 111.76 115.57 0)
			(hide yes)
			(effects
				(font
					(size 1.27 1.27)
				)
			)
		)
		(property "LCSC" "C25741"
			(at 111.76 115.57 0)
			(hide yes)
			(effects
				(font
					(size 1.27 1.27)
				)
			)
		)
		(pin "1"
			(uuid "%(u1)s")
		)
		(pin "2"
			(uuid "%(u2)s")
		)
		(instances
			(project "cubevox"
				(path "/99ad1494-2bf8-5051-8886-2c8b83c86254"
					(reference "R162")
					(unit 1)
				)
			)
		)
	)
	(wire
		(pts
			(xy 111.76 109.22) (xy 111.76 111.76)
		)
		(stroke
			(width 0)
			(type default)
		)
		(uuid "%(u3)s")
	)
	(wire
		(pts
			(xy 111.76 119.38) (xy 111.76 121.92)
		)
		(stroke
			(width 0)
			(type default)
		)
		(uuid "%(u4)s")
	)
	(label "MUX_S0"
		(at 111.76 109.22 90)
		(fields_autoplaced yes)
		(effects
			(font
				(size 1.27 1.27)
			)
			(justify left bottom)
		)
		(uuid "%(u5)s")
	)
	(label "GND"
		(at 111.76 121.92 270)
		(fields_autoplaced yes)
		(effects
			(font
				(size 1.27 1.27)
			)
			(justify right bottom)
		)
		(uuid "%(u6)s")
	)
'''


def main(path, apply):
    s = open(path).read()
    # pole A: IN_P from pin 4 (985.52, 581.66) to pin 2 (970.28, 581.66)
    s = swap(s, '(xy 985.52 581.66) (xy 985.52 594.36)', '(xy 970.28 581.66) (xy 970.28 594.36)')
    s = swap(s, '(label "IN_P"\n\t\t(at 985.52 594.36 270)', '(label "IN_P"\n\t\t(at 970.28 594.36 270)')
    s = swap(s, '(no_connect\n\t\t(at 970.28 581.66)', '(no_connect\n\t\t(at 985.52 581.66)')
    # pole B: LINE_N from pin 5 (985.52, 562.61) to pin 7 (970.28, 562.61)
    s = swap(s, '(xy 985.52 562.61) (xy 985.52 549.91)', '(xy 970.28 562.61) (xy 970.28 549.91)')
    s = swap(s, '(label "LINE_N"\n\t\t(at 985.52 549.91 90)', '(label "LINE_N"\n\t\t(at 970.28 549.91 90)')
    s = swap(s, '(no_connect\n\t\t(at 970.28 562.61)', '(no_connect\n\t\t(at 985.52 562.61)')
    # R162 before the sheet_instances block at the end of the root sheet
    if 'property "Reference" "R162"' in s:
        raise SystemExit('R162 already present')
    u = {'u%d' % i: str(uuid.uuid4()) for i in range(7)}
    anchor = '\t(sheet_instances'
    if s.count(anchor) != 1:
        raise SystemExit('sheet_instances anchor not unique')
    s = s.replace(anchor, R162 % u + anchor)
    print('K103 pins 2/7 wired, pins 4/5 no-connect; R162 added at (111.76, 115.57)')
    if apply:
        open(path, 'w').write(s)
        print('SAVED', path)
    else:
        print('DRY RUN, nothing saved')


if __name__ == '__main__':
    main(sys.argv[1], '--apply' in sys.argv)
