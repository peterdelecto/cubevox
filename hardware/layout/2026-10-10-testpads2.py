"""Test pads, pass 2: TP6/TP7 clear of the U4/U103 silk references, TP10/TP11 become the trace corners,
FB101.2 / FB102.2 track ends snapped to the pad centres."""
import importlib.util, sys
spec = importlib.util.spec_from_file_location('fix1', __file__.replace('testpads2', 'prefab-fix'))
fix1 = importlib.util.module_from_spec(spec); spec.loader.exec_module(fix1)
import pcbnew
F = pcbnew.F_Cu

def main(path, apply):
    b = pcbnew.LoadBoard(path); x = fix1.Fix(b)
    x.say('TP6 3V3A south to y 53.2')
    x.move('TP6', 0, 1.4)
    x.say('TP7 5V west of U103.1')
    x.rm('/5V', F, (93.05, 54.1), (93.05, 56.4))
    x.move('TP7', 90.8 - 93.05, 54.1 - 56.4)
    x.add('/5V', F, 0.4, [(93.05, 54.1), (90.8, 54.1)])
    x.say('TP10 VREF_BUF at the lane corner')
    x.move('TP10', -0.45, 0)
    x.rm('/VREF_BUF', F, (155.75, 87.85), (156.2, 87.4))
    x.end('/VREF_BUF', F, (155.75, 93.0), (155.75, 87.85), (155.75, 87.85), (155.75, 87.4))
    x.say('TP11 VDDA at the C164.1 column corner')
    VD = '/h7core_block/VDDA'
    x.move('TP11', 0, -0.6)
    x.rm(VD, F, (104.5, 93.725), (105.1, 94.325))
    x.end(VD, F, (103.0, 93.725), (104.5, 93.725), (104.5, 93.725), (105.1, 93.725))
    x.say('FB pad ends snapped')
    x.end('/5VA', F, (157.6, 61.79), (157.6, 62.62), (157.6, 61.79), (157.6, 61.7875))
    x.end('/5VA_W', F, (68.5, 75.587), (68.5, 86.0), (68.5, 75.587), (68.5, 75.5875))
    x.commit(apply)



def pass3(path, apply):
    """TP11 clear of U1's courtyard (circle radius is 1.295, not 1.0); drop the 5VA_W stub that
    duplicated the existing vertical under TP9."""
    b = pcbnew.LoadBoard(path); x = fix1.Fix(b)
    VD = '/h7core_block/VDDA'
    x.move('TP11', 0, 0.775)
    x.end(VD, F, (103.0, 93.725), (105.1, 93.725), (105.1, 93.725), (104.325, 93.725))
    x.add(VD, F, 0.3, [(104.325, 93.725), (105.1, 94.5)])
    x.rm('/5VA_W', F, (68.5, 75.5875), (68.5, 77.8))
    x.commit(apply)


if __name__ == '__main__':
    (pass3 if '--pass3' in sys.argv else main)(sys.argv[1], '--apply' in sys.argv)
