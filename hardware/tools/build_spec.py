"""Builds hardware/h7core_block.json and hardware/cubevox.json for `foreman schematic`.

Sources (the unmodified fxbox clones) live in tools/src/. Run from anywhere:
  python3 tools/build_spec.py
Coordinates are grid units (50 mil).
"""
import copy
import json
import os

HW = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(HW, "tools", "src")
DATE = "2026-10-02"

# --------------------------------------------------------------------------- codes
R_LCSC = {"10R": "C22859", "33R": "C23140", "47R": "C23182", "100R": "C22775",
          "1k": "C21190", "1.2k": "C22765", "4.7k": "C23162", "10k": "C25804",
          "15k": "C22809", "18k": "C25810", "100k": "C25803"}
C_LCSC = {"47pF": ("C1671", "C0603"), "100pF": ("C14858", "C0603"),
          "2.2nF": ("C2991172", "C0603"), "100nF": ("C14663", "C0603"),
          "10nF": ("C57112", "C0603"), "10uF": ("C14860", "C1206"),
          "22uF": ("C12891", "C1206"), "2.2uF": ("C23630", "C0603")}

_counters = {}


def nxt(prefix, start):
    n = _counters.get(prefix, start)
    _counters[prefix] = n + 1
    return "%s%d" % (prefix, n)


def R(val, a, b, x, y, ref=None):
    return {"ref": ref or nxt("R", 101), "lib_id": "Device:R", "value": val,
            "at": [x, y], "angle": 0, "footprint": "cubevox:R0603",
            "lcsc": R_LCSC[val], "rest": "no_connect",
            "pins": [{"pin": "1", "net": a, "dir": [0, -1], "len": 2},
                     {"pin": "2", "net": b, "dir": [0, 1], "len": 2}]}


def C(val, a, b, x, y, ref=None):
    code, fp = C_LCSC[val]
    return {"ref": ref or nxt("C", 117), "lib_id": "Device:C", "value": val,
            "at": [x, y], "angle": 0, "footprint": "cubevox:" + fp, "lcsc": code,
            "rest": "no_connect",
            "pins": [{"pin": "1", "net": a, "dir": [0, -1], "len": 2},
                     {"pin": "2", "net": b, "dir": [0, 1], "len": 2}]}


def row(pin, net, d, ln=2):
    return {"pin": pin, "net": net, "dir": d, "len": ln}


UP, DN, LF, RT = [0, -1], [0, 1], [-1, 0], [1, 0]


def opamp(ref, x, y, nets):
    """nets: pin -> net for the 8 OPA1678 pins. Left pins stub left, right pins right."""
    rows = []
    for p in "12345678":
        rows.append(row(p, nets[p], LF if p in "1234" else RT, 2))
    return {"ref": ref, "lib_id": "cubevox:OPA1678IDR", "value": "OPA1678IDR",
            "at": [x, y], "angle": 0,
            "footprint": "cubevox:SOIC-8_L5.0-W4.0-P1.27-LS6.0-BL", "lcsc": "C192421",
            "rest": "no_connect", "in_bom": True, "fields": {"MPN": "OPA1678IDR"},
            "pins": rows}


TOGGLE_SYM = "ST-0-102-A01-T000-LF+PJ"


def toggle(ref, x, y, common, throw1, throw3=None):
    """ST-0-102 SPDT ON-ON: pin 2 common (left), pins 1 and 3 the throws (right)."""
    rows = [row("2", common, LF), row("1", throw1, RT)]
    if throw3:
        rows.append(row("3", throw3, RT))
    return {"ref": ref, "lib_id": "cubevox:" + TOGGLE_SYM, "value": "ST-0-102 bat toggle",
            "at": [x, y], "angle": 0,
            "footprint": "cubevox:SW-TH_3P-L8.3-W5.2-P2.54_ST-0-102-A01-T000",
            "lcsc": "C1788487", "rest": "no_connect", "in_bom": True,
            "fields": {"MPN": "ST-0-102-A01-T000-LF"}, "pins": rows}


def pot(ref, x, y, end1, wiper, end3, val="10k"):
    return {"ref": ref, "lib_id": "cubevox:RK09D1130C1B", "value": val,
            "at": [x, y], "angle": 0, "footprint": "cubevox:RES-ADJ-TH_RK09D1130C3C",
            "lcsc": "C470304", "rest": "no_connect", "in_bom": True,
            "fields": {"MPN": "RK09D1130C1B"},
            "pins": [row("1", end1, LF), row("2", wiper, UP), row("3", end3, RT)]}


def flag(x, y, net, n):
    return {"lib_id": "power:PWR_FLAG", "value": "PWR_FLAG", "prefix": "#FLG%d" % n,
            "at": [x, y], "stub": {"net": net, "dir": [0, -1], "len": 2}}


# =========================================================================== CHILD
CHILD_DROP = (
    ["U7", "U10", "U11", "U12", "L2", "D4", "D5"]
    + ["C%d" % i for i in list(range(38, 44)) + [36, 37, 50, 51, 52, 53] + list(range(63, 74))]
    + ["R%d" % i for i in [14, 15, 17, 18, 19, 21, 22, 23, 24, 25, 26] + list(range(31, 43))]
)
U1_DROP_NETS = {"MUX2_SIG", "ADC_PC2C", "ADC_PC3C", "MUX3_SIG", "MUX4_SIG", "SPARE_PE7",
                "SYNC_OUT", "SYNC_IN", "HP_EN", "SPARE_PE14", "MIDI_OUT", "MIDI_IN",
                "SPARE_PC11", "OLED_SCK", "OLED_CS", "OLED_MOSI", "OLED_RST", "OLED_DC",
                "SPARE_PA15", "WS2812_DATA", "SPARE_PB5", "UART8_RX", "UART8_TX"}
# pin -> (net, stub len)   (left-side pins 1-50 stub left, right-side 51-100 stub right)
U1_ADD = {"28": ("TOGGLE1", 10), "29": ("TOGGLE2", 12), "30": ("TOGGLE3", 10),
          "31": ("TOGGLE4", 12), "34": ("TOGGLE5", 10), "35": ("TOGGLE6", 12),
          "38": ("TOGGLE7", 10), "46": ("TOGGLE8", 10), "47": ("JACK_TRS_N", 12),
          "51": ("ENC_KEY_A", 2), "52": ("ENC_KEY_B", 2), "53": ("ENC_SEMI_A", 2),
          "57": ("ENC_SEMI_B", 2), "63": ("FX_ON_SENSE", 2)}
CHILD_PORTS = [
    ("5V", "output"), ("5V_PRODUCT", "input"), ("5VA", "input"), ("3V3", "output"),
    ("3V3A", "output"), ("GND", "passive"),
    ("ADC_VINL", "input"), ("ADC_VINR", "input"), ("DAC_OUTL", "output"),
    ("SWDIO", "bidirectional"), ("SWCLK", "input"), ("SWO", "output"),
    ("NRST", "bidirectional"), ("BOOT0", "input"),
    ("I2C1_SCL", "bidirectional"), ("I2C1_SDA", "bidirectional"),
    ("MUX_S0", "output"), ("MUX_S1", "output"), ("MUX_S2", "output"),
    ("MUX_S3", "output"), ("POT_MUX_OUT", "input"),
    ("ENC_MENU_A", "input"), ("ENC_MENU_B", "input"), ("ENC_MENU_SW", "input"),
    ("ENC_KEY_A", "input"), ("ENC_KEY_B", "input"),
    ("ENC_SEMI_A", "input"), ("ENC_SEMI_B", "input"),
] + [("TOGGLE%d" % i, "input") for i in range(1, 9)] + [
    ("JACK_TRS_N", "input"), ("FX_ON_SENSE", "input")]
NET_RENAME = {"ENC_A": "ENC_MENU_A", "ENC_B": "ENC_MENU_B", "ENC_SW": "ENC_MENU_SW",
              "MUX1_SIG": "POT_MUX_OUT", "LINE_L": "DAC_OUTL"}


def build_child():
    d = json.load(open(os.path.join(SRC, "fxbox_h7core.json")))
    d["project"] = "cubevox"
    d["title"] = "cubevox H7 core block"
    d["date"] = DATE
    d["libraries"]["cubevox"] = "${BOARD_DIR}/cubevox.kicad_sym"
    parts = []
    for p in d["parts"]:
        if p["ref"] in CHILD_DROP:
            continue
        if p["ref"] == "U1":
            rows = [r for r in p["pins"] if r.get("net") not in U1_DROP_NETS]
            for r in rows:
                r["net"] = NET_RENAME.get(r["net"], r["net"])
            for pin, (net, ln) in U1_ADD.items():
                rows.append({"pin": pin, "net": net, "len": ln})
            p["pins"] = rows
        if p["ref"] == "U6":     # OUTL leaves on DAC_OUTL, OUTR unused
            p["pins"] = [r for r in p["pins"] if r["pin"] != "7"]
            for r in p["pins"]:
                if r["pin"] == "6":
                    r["net"] = "DAC_OUTL"
        if p["ref"] == "R20":    # PCM1808 VCC from 5VA through 10 R
            p["value"], p["lcsc"] = "10R", "C22859"
            p["pins"][0]["net"] = "5VA"
        if p["ref"] == "R30":
            p["pins"][0]["net"] = "POT_MUX_OUT"
        for r in p.get("pins", []):
            r["net"] = NET_RENAME.get(r.get("net"), r.get("net")) if r.get("net") else r.get("net")
        parts.append(p)
    d["parts"] = parts
    d["ports"] = [{"name": n, "shape": s} for n, s in CHILD_PORTS]
    used = {p["lib_id"] for p in parts} | {r["lib_id"] for r in d["power_symbols"]}
    d["lib_symbols"] = [ls for ls in d["lib_symbols"] if "%s:%s" % tuple(ls) in used]
    d["texts"] = [
        {"text": "A. Compute core: U1 and its decoupling, HSE, reset, BOOT0, user LED", "at": [12, 20]},
        {"text": "B. Power entry: USB-C, ESD, 5 V mux, 3V3 buck, 3V3A LDO, VBUS sense, power LED", "at": [120, 20]},
        {"text": "C. Settings store: QSPI NOR for the EEPROM emulation", "at": [165, 170]},
        {"text": "D. Audio out: PCM5102A, OUTL leaves on DAC_OUTL, OUTR unused", "at": [280, 20]},
        {"text": "One ground, GND.", "at": [285, 160]},
        {"text": "SWD on Tag-Connect pads J3", "at": [120, 228]},
        {"text": "Supply flags: nets fed through passives or from the connector", "at": [285, 190]},
        {"text": "F. Audio in: PCM1808 on SAI1 block B, slave on BCLK/LRCLK. VINL/VINR arrive AC-coupled from the top sheet", "at": [12, 254]},
        {"text": "U8 AGND and DGND both on GND. VCC from 5VA through R20", "at": [12, 309]},
        {"text": "PC0 ADC RC, AN2834", "at": [95, 32]},
        {"text": "Clock source series R at U1", "at": [95, 62]},
        {"text": "R28 22R, 33R to tune", "at": [95, 65]},
        {"text": "G. Reference-transition stitching, 10 nF 3V3-GND, one per via group:", "at": [320, 222]},
        {"text": "C56 U1-east I2S escapes; C57 U6-west I2S rises; C58 ADC_DOUT rise;", "at": [320, 225]},
        {"text": "C59 QSPI_IO1 hop, ADC_SCKI via A; C60 QSPI_IO1, IO2, NCS hops, flash end; C61 ADC_SCKI via B", "at": [320, 228]},
    ]
    return d


# =========================================================================== TOP
def build_top():
    old = json.load(open(os.path.join(SRC, "fxbox_top.json")))
    keep = {p["ref"]: p for p in old["parts"]}
    parts, flags, texts = [], [], []

    def add(*ps):
        parts.extend(ps)

    def note(msg, x, y):
        texts.append({"text": msg, "at": [x, y]})

    # ------------------------------------------------------------------ POWER IN
    X0, Y0 = 108, 24
    note("1. POWER IN", X0, Y0); note("9 V Boss supply, centre pin GND, SS34, TVS, 5 V buck, 5VA rail", X0, Y0 + 4)
    j = copy.deepcopy(keep["J106"]); j.update(at=[X0 + 10, 62], value="DC 9V")
    j["pins"] = [row("1", "GND", RT), row("2", "9V_JACK", RT), row("3", "9V_JACK", RT)]
    add(j)
    ss34 = {"ref": "D106", "lib_id": "cubevox:SS34_C8678", "value": "SS34", "at": [X0 + 52, 62],
            "angle": 180, "footprint": "cubevox:SMA_L4.3-W2.6-LS5.2-RD", "lcsc": "C8678",
            "rest": "no_connect", "in_bom": True, "fields": {"MPN": "SS34"},
            "pins": [row("2", "9V_JACK", LF), row("1", "VIN_9V", RT)]}
    add(ss34)
    d105 = copy.deepcopy(keep["D105"]); d105.update(at=[X0 + 100, 62])
    d105["pins"] = [row("1", "VIN_9V", LF), row("2", "GND", RT)]
    add(d105)
    add(C("22uF", "VIN_9V", "GND", X0 + 122, 62, "C112"), C("100nF", "VIN_9V", "GND", X0 + 132, 62, "C113"))
    u103 = copy.deepcopy(keep["U103"]); u103.update(at=[X0 + 20, 112])
    u103["pins"] = [row("1", "5V_PRODUCT", LF, 4), row("2", "VIN_9V", LF, 4), row("3", "VIN_9V", LF, 4),
                    row("4", "GND", RT), row("5", "BUCK5_SW", RT), row("6", "BUCK5_BST", RT)]
    add(u103)
    add(C("100nF", "BUCK5_BST", "BUCK5_SW", X0 + 62, 112, "C116"))
    l101 = copy.deepcopy(keep["L101"]); l101.update(at=[X0 + 95, 112])
    l101["pins"] = [row("1", "BUCK5_SW", LF), row("2", "5V_PRODUCT", RT)]
    add(l101)
    add(C("22uF", "5V_PRODUCT", "GND", X0 + 122, 112, "C114"), C("22uF", "5V_PRODUCT", "GND", X0 + 132, 112, "C115"))
    fb = {"ref": "FB101", "lib_id": "Device:FerriteBead", "value": "120R@100MHz",
          "at": [X0 + 10, 162], "angle": 0, "footprint": "Inductor_SMD:L_0603_1608Metric",
          "lcsc": "C14709", "rest": "no_connect", "fields": {"MPN": "BLM18PG121SN1D"},
          "pins": [row("1", "5V", UP), row("2", "5VA", DN)]}
    add(fb)
    add(C("10uF", "5VA", "GND", X0 + 30, 162), C("100nF", "5VA", "GND", X0 + 42, 162))
    note("5VA analog rail from 5V through FB101", X0, 150)
    flags += [flag(X0 + 90, 160, "VIN_9V", 1), flag(X0 + 110, 160, "5V_PRODUCT", 2),
              flag(X0 + 130, 160, "5VA", 3)]

    # ------------------------------------------------------------------ MIC / LINE INPUT
    X1, Y1 = 300, 24
    note("2. MIC / LINE INPUT", X1, Y1); note("XLR leg 47R, BAT54S clamp, 100pF, 10uF. TRS leg 10uF, 10k pad. 1.2k bias to VREF", X1, Y1 + 4)
    jin = {"ref": "J106A", "lib_id": "cubevox:NCJ6FA-H", "value": "J_IN NCJ6FA-H", "at": [X1 + 12, 120],
           "angle": 180, "footprint": "cubevox:CONN-TH_NCJ6FA-H", "lcsc": "C368458",
           "rest": "no_connect", "in_bom": True, "fields": {"MPN": "NCJ6FA-H"},
           "pins": [row("1", "GND", RT), row("2", "XLR_2", RT), row("3", "XLR_3", RT),
                    row("4", "GND", RT), row("5", "TRS_R", RT), row("6", "TRS_T", RT),
                    row("7", "JACK_TRS_N", RT), row("8", "GND", RT)]}
    jin["ref"] = "J101"
    add(jin)
    for leg, (xlr, trs, tag, ya) in {"P": ("XLR_2", "TRS_T", "P", 66), "N": ("XLR_3", "TRS_R", "N", 140)}.items():
        x = X1 + 40
        add(R("47R", xlr, "MIC_" + tag, x, ya))
        bat = {"ref": nxt("D", 107), "lib_id": "cubevox_h7core:BAT54SLT1G", "value": "BAT54S",
               "at": [x + 22, ya], "angle": 0,
               "footprint": "cubevox_h7core:SOT-23_L2.9-W1.3-P1.90-LS2.4-BR", "lcsc": "C19726",
               "rest": "no_connect", "in_bom": True, "fields": {"MPN": "BAT54SLT1G"},
               "pins": [row("1", "GND", LF), row("2", "5VA", RT), row("3", "MIC_" + tag, DN)]}
        add(bat)
        add(C("100pF", "MIC_" + tag, "GND", x + 48, ya))
        add(C("10uF", "MIC_" + tag, "IN_" + tag, x + 60, ya))
        add(C("10uF", trs, "LINE_" + tag, x + 80, ya))
        add(R("10k", "LINE_" + tag, "IN_" + tag, x + 92, ya))
        add(R("1.2k", "IN_" + tag, "VREF", x + 104, ya))
        add(C("100nF", "VREF", "GND", x + 116, ya))
    add(R("10k", "3V3", "JACK_TRS_N", X1 + 40, 190))
    note("JACK_TRS_N low when a 1/4 in plug is in", X1 + 52, 188)

    # ------------------------------------------------------------------ PREAMP
    X2, Y2 = 468, 24
    note("3. PREAMP", X2, Y2); note("Stage 1 INA 20 dB (U104, U105A). Stage 2 0-40 dB on Input Gain pot (U105B)", X2, Y2 + 4)
    add(opamp("U104", X2 + 30, 72, {"1": "INA_O1", "2": "A1_FB", "3": "IN_P", "4": "GND",
                                    "5": "IN_N", "6": "A2_FB", "7": "INA_O2", "8": "5VA"}))
    add(C("100nF", "5VA", "GND", X2 + 62, 80))
    xs = X2 + 84
    add(R("4.7k", "INA_O1", "A1_FB", xs, 72), C("47pF", "INA_O1", "A1_FB", xs + 12, 72),
        R("1k", "A1_FB", "A2_FB", xs + 24, 72),
        R("4.7k", "INA_O2", "A2_FB", xs + 36, 72), C("47pF", "INA_O2", "A2_FB", xs + 48, 72))
    add(opamp("U105", X2 + 30, 172, {"1": "PRE1", "2": "DA_N", "3": "DA_P", "4": "GND",
                                     "5": "PRE1", "6": "G2_INV", "7": "PRE_OUT", "8": "5VA"}))
    add(C("100nF", "5VA", "GND", X2 + 62, 180))
    add(R("10k", "INA_O1", "DA_P", xs, 172), R("10k", "DA_P", "VREF", xs + 12, 172),
        R("10k", "INA_O2", "DA_N", xs + 24, 172), R("10k", "DA_N", "PRE1", xs + 36, 172),
        C("100nF", "VREF", "GND", xs + 48, 172))
    add(pot("RV115", X2 + 40, 252, "PRE_OUT", "G2_FB", "G2_FB", "10k GAIN"))
    add(R("100R", "G2_FB", "G2_INV", xs + 4, 252), R("100R", "G2_INV", "VREF", xs + 16, 252),
        C("47pF", "PRE_OUT", "G2_INV", xs + 28, 252), C("100nF", "VREF", "GND", xs + 40, 252))
    note("Input Gain pot in the A4 feedback leg, wiper tied to end 3", X2, 232)
    add(opamp("U106", X2 + 30, 322, {"1": "VREF", "2": "VREF", "3": "VREF_DIV", "4": "GND",
                                     "5": "BUF_IN", "6": "BUF_OUT", "7": "BUF_OUT", "8": "5VA"}))
    add(C("100nF", "5VA", "GND", X2 + 62, 330))
    add(R("10k", "5VA", "VREF_DIV", xs, 322), R("10k", "VREF_DIV", "GND", xs + 12, 322),
        C("10uF", "VREF_DIV", "GND", xs + 24, 322))
    note("VREF: 5VA divided 10k/10k, 10 uF, buffered by U106A. U106B drives block 6", X2, 292)

    # ------------------------------------------------------------------ ADC FEED
    X3, Y3 = 640, 24
    note("4. ADC FEED", X3, Y3); note("PRE_OUT always feeds the ADC", X3, Y3 + 4)
    add(R("100R", "PRE_OUT", "ADC_RC", X3 + 4, 66), C("2.2nF", "ADC_RC", "GND", X3 + 16, 66),
        C("10uF", "ADC_RC", "ADC_VINL", X3 + 28, 66))
    add(C("10uF", "ADC_VINR", "GND", X3 + 28, 140))
    note("VINR unused, AC-coupled to GND", X3 + 4, 170)

    # ------------------------------------------------------------------ BYPASS
    X4, Y4 = 722, 24
    note("5. BYPASS", X4, Y4); note("COM BUF_IN, NC PRE_OUT, NO DAC_ATT. Coil on = effect on", X4, Y4 + 4)
    k = copy.deepcopy(keep["K101"]); k.update(at=[X4 + 28, 76], value="G6K-2F-Y DC5")
    k["pins"] = [row("1", "SW_BYPASS", DN, 4), row("2", "PRE_OUT", DN, 6), row("3", "BUF_IN", DN, 8),
                 row("4", "DAC_ATT", DN, 10), row("5", "DAC_ATT", UP, 10), row("6", "BUF_IN", UP, 8),
                 row("7", "PRE_OUT", UP, 6), row("8", "GND", UP, 4)]
    add(k)
    d104 = copy.deepcopy(keep["D104"]); d104.update(at=[X4 + 90, 76])
    d104["pins"] = [row("1", "SW_BYPASS", LF), row("2", "GND", RT)]
    add(d104)
    add(toggle("SW115", X4 + 80, 130, "SW_BYPASS", "5V"))
    note("Bypass toggle. Common to the coil, one throw to 5V", X4 + 96, 130)
    add(R("10k", "SW_BYPASS", "FX_ON_SENSE", X4 + 90, 180), R("18k", "FX_ON_SENSE", "GND", X4 + 102, 180))

    # ------------------------------------------------------------------ DAC / OUTPUT
    X5, Y5 = 840, 24
    note("6. DAC / OUTPUT", X5, Y5); note("DAC_ATT = 0.6 x DAC about VREF. U106B buffers to the jack", X5, Y5 + 4)
    add(C("10uF", "DAC_OUTL", "DAC_AC", X5 + 4, 66), R("10k", "DAC_AC", "DAC_ATT", X5 + 16, 66),
        R("15k", "DAC_ATT", "VREF", X5 + 28, 66), C("100nF", "VREF", "GND", X5 + 40, 66))
    add(R("100R", "BUF_OUT", "BUF_R", X5 + 4, 140), C("10uF", "BUF_R", "OUT_TIP", X5 + 16, 140),
        R("100k", "OUT_TIP", "GND", X5 + 28, 140))
    nmj = {"ref": "J108", "lib_id": "cubevox:NMJ6HCD2", "value": "OUT 6.35mm",
           "at": [X5 + 20, 200], "angle": 0, "footprint": "cubevox:AUDIO-TH_NMJ6HCD2",
           "lcsc": "C368502", "rest": "no_connect", "in_bom": True, "fields": {"MPN": "NMJ6HCD2"},
           "pins": [row("1", "OUT_TIP", LF), row("3", "GND", LF)]}
    add(nmj)

    # ------------------------------------------------------------------ PANEL
    X6, Y6 = 108, 280
    note("7. PANEL", X6, Y6); note("14 pots to U101 (C14, C15 on GND), 3 encoders, 8 toggles, OLED header", X6, Y6 + 4)
    u101 = copy.deepcopy(keep["U101"]); u101.update(at=[X6 + 40, 340])
    ch = {0: "9", 1: "8", 2: "7", 3: "6", 4: "5", 5: "4", 6: "3", 7: "2",
          8: "23", 9: "22", 10: "21", 11: "20", 12: "19", 13: "18", 14: "17", 15: "16"}
    rows = []
    for n in range(16):
        rows.append(row(ch[n], "POT%02d" % (n + 1) if n < 14 else "GND", LF if ch[n] in "98765432" else RT, 2))
    rows += [row("1", "MUX_COM", LF), row("10", "MUX_S0", LF), row("11", "MUX_S1", LF),
             row("12", "GND", LF), row("13", "MUX_S3", RT), row("14", "MUX_S2", RT),
             row("15", "GND", RT), row("24", "3V3A", RT)]
    u101["pins"] = rows
    add(u101)
    add(R("1k", "MUX_COM", "POT_MUX_OUT", X6 + 6, 400), C("10nF", "POT_MUX_OUT", "GND", X6 + 18, 400))
    add(C("100nF", "3V3A", "GND", X6 + 50, 400, "C101"), C("2.2uF", "3V3A", "GND", X6 + 62, 400, "C111"))
    for i in range(14):
        px = X6 + 120 + (i % 7) * 40
        py = 330 + (i // 7) * 60
        add(pot("RV%d" % (101 + i), px, py, "GND", "POT%02d" % (i + 1), "3V3A"))

    # encoders
    enc_y = 470
    add(dict(_enc("SW104", "EC11N1525404", "C470748", X6 + 20, enc_y, "ENC_MENU_A", "ENC_MENU_B", "ENC_MENU_SW")))
    add(dict(_enc("SW105", "EC11N1520401", "C470703", X6 + 160, enc_y, "ENC_KEY_A", "ENC_KEY_B", None)))
    add(dict(_enc("SW106", "EC11N1520401", "C470703", X6 + 300, enc_y, "ENC_SEMI_A", "ENC_SEMI_B", None)))
    xe = X6 + 30
    for sigs, base in ((["ENC_MENU_A", "ENC_MENU_B", "ENC_MENU_SW"], X6 + 20),
                       (["ENC_KEY_A", "ENC_KEY_B"], X6 + 160), (["ENC_SEMI_A", "ENC_SEMI_B"], X6 + 300)):
        for i, s in enumerate(sigs):
            add(R("10k", "3V3", s, base + 40 + i * 24, enc_y + 4), C("100nF", s, "GND", base + 40 + i * 24 + 10, enc_y + 4))
    # toggles
    for i in range(8):
        tx = X6 + 40 + (i % 4) * 110
        ty = 560 + (i // 4) * 50
        add(R("10k", "3V3", "TOGGLE%d" % (i + 1), tx, ty), C("100nF", "TOGGLE%d" % (i + 1), "GND", tx + 12, ty))
        add(toggle("SW%d" % (107 + i), tx + 36, ty, "TOGGLE%d" % (i + 1), "GND"))
    # OLED
    oled = {"ref": "J117", "lib_id": "cubevox:PZ254V-11-04P_C2691448", "value": "OLED SH1106",
            "at": [X6 + 520, 470], "angle": 0, "footprint": "cubevox:HDR-TH_4P-P2.54-V-M",
            "lcsc": "C2691448", "rest": "no_connect", "in_bom": True, "fields": {"MPN": "PZ254V-11-04P"},
            "pins": [row("1", "3V3", LF), row("2", "GND", LF), row("3", "I2C1_SCL", LF), row("4", "I2C1_SDA", LF)]}
    add(oled)
    add(R("4.7k", "3V3", "I2C1_SCL", X6 + 560, 470), R("4.7k", "3V3", "I2C1_SDA", X6 + 572, 470))

    # mounting holes kept from fxbox
    for i, r in enumerate(["H101", "H102", "H103", "H104", "H105", "H106", "H107", "H108"]):
        h = copy.deepcopy(keep[r]); h["at"] = [X6 + 16 * i, 640]
        parts.append(h)

    top = {k: old[k] for k in ("project", "uuid_namespace", "paper", "libraries")}
    top.update(title="cubevox vocal FX box", date=DATE, rev="1")
    top["paper"] = "A0"
    top["libraries"]["cubevox_h7core"] = "${BOARD_DIR}/cubevox_h7core.kicad_sym"
    top["lib_symbols"] = [
        ["cubevox", "OPA1678IDR"], ["cubevox", "SS34_C8678"], ["cubevox", "SMBJ15CA_C19077570"],
        ["cubevox", "PJ-002A"], ["cubevox", "AP63205WU-7"], ["cubevox", "ANR5040T4R7M"],
        ["cubevox", TOGGLE_SYM], ["cubevox", "PZ254V-11-04P_C2691448"],
        ["cubevox", "NMJ6HCD2"], ["cubevox", "NCJ6FA-H"], ["cubevox", "G6K-2F-Y-DC5"], ["cubevox", "1N4148W_C81598"],
        ["cubevox", "CD74HC4067SM96"], ["cubevox", "RK09D1130C1B"], ["cubevox", "EC11N1525404"],
        ["cubevox", "EC11N1520401"], ["cubevox_h7core", "BAT54SLT1G"],
        ["Device", "R"], ["Device", "C"], ["Device", "FerriteBead"],
        ["Mechanical", "MountingHole"], ["power", "PWR_FLAG"]]
    top["libraries"].pop("Connector", None)
    top["libraries"].pop("Connector_Generic", None)

    child = build_child()
    names = [p["name"] for p in [{"name": n} for n, _ in CHILD_PORTS]]
    pins = []
    off = 4
    for n in names:
        if n in ("SWDIO", "SWCLK", "SWO", "NRST", "BOOT0"):
            pins.append({"name": n, "side": "right", "offset": off, "no_connect": True})
        else:
            pins.append({"name": n, "side": "right", "offset": off, "len": 2})
        off += 3
    top["sheets"] = [{"name": "h7core_block", "spec": "h7core_block.json", "at": [20, 40],
                      "size": [46, off + 2], "pins": pins}]
    top["parts"] = parts
    top["power_symbols"] = flags
    top["texts"] = texts
    return top, child


def _enc(ref, mpn, lcsc, x, y, a, b, sw):
    rows = [row("A", a, DN), row("B", b, DN), row("C", "GND", DN), row("EH", "GND", RT)]
    if sw:
        rows += [row("D", sw, UP), row("E", "GND", UP)]
    return {"ref": ref, "lib_id": "cubevox:" + mpn, "value": mpn, "at": [x, y], "angle": 0,
            "footprint": "cubevox:SW-TH_EC11NXXXX", "lcsc": lcsc, "rest": "no_connect",
            "in_bom": True, "fields": {"MPN": mpn}, "pins": rows}


if __name__ == "__main__":
    top, child = build_top()
    json.dump(top, open(os.path.join(HW, "cubevox.json"), "w"), indent=1)
    json.dump(child, open(os.path.join(HW, "h7core_block.json"), "w"), indent=1)
    print("wrote", len(top["parts"]), "top parts,", len(child["parts"]), "child parts")
