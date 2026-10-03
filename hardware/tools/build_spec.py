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
          "15k": "C22809", "22k": "C31850", "100k": "C25803",
          "470R": "C23179", "330R": "C23138", "22R": "C23345",
          "4.53k": "C25971", "10k 0.1%": "C309083"}
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
    mpn = {"10k 0.1%": "ARG03BTC1002"}.get(val)
    return {"ref": ref or nxt("R", 101), "lib_id": "Device:R", "value": val,
            "at": [x, y], "angle": 0, "footprint": "cubevox:R0603",
            "lcsc": R_LCSC[val], "rest": "no_connect",
            **({"fields": {"MPN": mpn}} if mpn else {}),
            "pins": [{"pin": "1", "net": a, "dir": [0, -1], "len": 2},
                     {"pin": "2", "net": b, "dir": [0, 1], "len": 2}]}


def C(val, a, b, x, y, ref=None):
    code, fp = C_LCSC[val]
    return {"ref": ref or nxt("C", 117), "lib_id": "Device:C", "value": val,
            "at": [x, y], "angle": 0, "footprint": "cubevox:" + fp, "lcsc": code,
            "rest": "no_connect",
            "pins": [{"pin": "1", "net": a, "dir": [0, -1], "len": 2},
                     {"pin": "2", "net": b, "dir": [0, 1], "len": 2}]}


# Aluminium electrolytics (ROQANG 105 C). Pin 1 = +, pin 2 = -.
ELEC = {"10uF": ("C72484", "CAP-SMD_BD4.0-L4.3-W4.3-FD", "RVT1E100M0405", "10uF 25V", "RVT1E100M0405-C72484"),
        "100uF": ("C191859", "CAP-SMD_BD5.0-L5.3-W5.3-LS6.3-FD", "VT1A101M0505", "100uF 10V", "VT1A101M0505"),
        "22uF": ("C72502", "CAP-SMD_BD4.0-L4.3-W4.3-LS5.3-FD", "RVT1C220M0405", "22uF 16V", "RVT1C220M0405"),
        "47uF": ("C72521", "CAP-SMD_BD6.3-L6.6-W6.6-LS7.6-FD", "RVT1E470M0605", "47uF 25V", "RVT1E470M0605")}


def CE(val, plus, minus, x, y, ref):
    code, fp, mpn, label, sym = ELEC[val]
    return {"ref": ref, "lib_id": "cubevox:" + sym, "value": label, "at": [x, y], "angle": 0,
            "footprint": "cubevox:" + fp, "lcsc": code, "rest": "no_connect",
            "fields": {"MPN": mpn},
            "pins": [{"pin": "1", "net": plus, "dir": [0, -1], "len": 2},
                     {"pin": "2", "net": minus, "dir": [0, 1], "len": 2}]}


def row(pin, net, d, ln=2):
    return {"pin": pin, "net": net, "dir": d, "len": ln}


UP, DN, LF, RT = [0, -1], [0, 1], [-1, 0], [1, 0]


def opamp(ref, x, y, nets):
    """nets: pin -> net for the 8 OPA2197 pins. Left pins stub left, right pins right."""
    rows = []
    for p in "12345678":
        rows.append(row(p, nets[p], LF if p in "1234" else RT, 2))
    return {"ref": ref, "lib_id": "cubevox:OPA2197IDR", "value": "OPA2197IDR",
            "at": [x, y], "angle": 0,
            "footprint": "cubevox:SOIC-8_L4.9-W3.9-P1.27-LS6.0-BL", "lcsc": "C139363",
            "rest": "no_connect", "in_bom": True, "fields": {"MPN": "OPA2197IDR"},
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
    return {"ref": ref, "lib_id": "cubevox:RK09D1130C2P", "value": val,
            "at": [x, y], "angle": 0, "footprint": "cubevox:RES-ADJ-TH_RK09D1130C2P",
            "lcsc": "C361173", "rest": "no_connect", "in_bom": True,
            "fields": {"MPN": "RK09D1130C2P"},
            "pins": [row("1", end1, LF), row("2", wiper, UP), row("3", end3, RT)]}


def flag(x, y, net, n):
    return {"lib_id": "power:PWR_FLAG", "value": "PWR_FLAG", "prefix": "#FLG%d" % n,
            "at": [x, y], "stub": {"net": net, "dir": [0, -1], "len": 2}}


# =========================================================================== CHILD
CHILD_DROP = (
    ["U2", "U5", "U7", "U10", "U11", "U12", "L2", "D4", "D5"]     # U2 = TPS2116 mux, U5 = QSPI flash
    + ["C%d" % i for i in list(range(38, 44)) + [17, 26, 36, 37, 50, 51, 52, 53, 60] + list(range(63, 74))]
    + ["R%d" % i for i in [8, 9, 11, 12, 13, 14, 15, 17, 18, 19, 21, 22, 23, 24, 25, 26, 28] + list(range(31, 43))]
)
U1_DROP_NETS = {"CC1", "CC2",                 # USB-C is data only; the 5.1k Rd alone make it a sink
                "QSPI_CLK_SRC", "MUX2_SIG", "ADC_PC2C", "ADC_PC3C", "MUX3_SIG", "MUX4_SIG", "SPARE_PE7",
                "SYNC_OUT", "SYNC_IN", "HP_EN", "SPARE_PE14", "MIDI_OUT", "MIDI_IN",
                "SPARE_PC11", "OLED_SCK", "OLED_CS", "OLED_MOSI", "OLED_RST", "OLED_DC",
                "SPARE_PA15", "WS2812_DATA", "SPARE_PB5", "UART8_RX", "UART8_TX"}
# pin -> (net, stub len). Every movable net sits on the U1 edge facing its destination, in the
# order that lets the fan-out leave without crossings (PINMAP.md section 2). Left-side pins
# 1-50 stub left, right-side pins 51-100 stub right.
U1_PINS = {"28": ("TOGGLE3", 10), "29": ("TOGGLE4", 12), "30": ("MUX_S2", 10), "31": ("MUX_S3", 12),
           "32": ("MUX_S1", 10), "33": ("MUX_S0", 12), "34": ("TOGGLE5", 10),
           "35": ("VA_SENSE", 12),                     # PB1 ADC12_INP5
           "36": ("TOGGLE6", 10), "37": ("TOGGLE7", 12), "38": ("TOGGLE8", 10), "39": ("XSMT", 12),
           "43": ("ENC_MENU_SW", 10), "44": ("ENC_MENU_B", 12), "45": ("ENC_MENU_A", 10),
           "46": ("I2C_SCL", 12), "47": ("I2C_SDA", 10),  # PB10/PB11 I2C2 AF4
           "63": ("FX_ON_SENSE", 2),
           "41": ("USER_LED", 10),                     # LED east of U1, between the MENU and toggle fans
           "91": ("MUTE_N", None),                     # not PB4: NJTRST pulls up at reset
           "92": ("TOGGLE1", None), "93": ("ENC_KEY_B", None), "95": ("ENC_KEY_A", None),
           "96": ("ENC_SEMI_B", None), "97": ("ENC_SEMI_A", None), "98": ("TOGGLE2", None)}
CHILD_PORTS = [
    ("5V", "input"), ("5VA", "input"), ("3V3", "output"),
    ("3V3A", "output"), ("GND", "passive"),
    ("ADC_VINL", "input"), ("ADC_VINR", "input"), ("DAC_OUTL", "output"),
    ("SWDIO", "bidirectional"), ("SWCLK", "input"), ("SWO", "output"),
    ("NRST", "bidirectional"), ("BOOT0", "input"),
    ("I2C_SCL", "bidirectional"), ("I2C_SDA", "bidirectional"),
    ("MUX_S0", "output"), ("MUX_S1", "output"), ("MUX_S2", "output"),
    ("MUX_S3", "output"), ("POT_MUX_OUT", "input"),
    ("ENC_MENU_A", "input"), ("ENC_MENU_B", "input"), ("ENC_MENU_SW", "input"),
    ("ENC_KEY_A", "input"), ("ENC_KEY_B", "input"),
    ("ENC_SEMI_A", "input"), ("ENC_SEMI_B", "input"),
] + [("TOGGLE%d" % i, "input") for i in range(1, 9)] + [
    ("FX_ON_SENSE", "input"), ("MUTE_N", "output"), ("VA_SENSE", "input")]
# I01 (PINMAP.md section 5): SCKI from SAI1_MCLK_A on PE2. No external flash.
# Net per pin number (names resolved from cubevox_h7core.kicad_sym); None leaves the pin NC.
U1_REMAP = {"1": ("ADC_SCKI_SRC", 12),                 # PE2 SAI1_MCLK_A
            "40": None, "58": None, "59": None,        # fxbox QSPI / spare rows freed
            "60": None, "61": None, "79": None}
NET_RENAME = {"ENC_A": "ENC_MENU_A", "ENC_B": "ENC_MENU_B", "ENC_SW": "ENC_MENU_SW",
              "MUX1_SIG": "POT_MUX_OUT", "LINE_L": "DAC_OUTL",
              "I2C1_SCL": "I2C_SCL", "I2C1_SDA": "I2C_SDA"}


CHILD_REF_RENAME = {"SW1": "SWRESET1", "SW2": "SWBOOT1"}   # SW1 on NRST, SW2 on BOOT0


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
            placed = {net for net, _ in U1_PINS.values()}
            rows = [r for r in rows if r["pin"] not in U1_PINS and r.get("net") not in placed
                    and r["pin"] not in U1_REMAP]
            for pin, (net, ln) in U1_PINS.items():
                rows.append({"pin": pin, "net": net, **({"len": ln} if ln else {})})
            for pin, v in U1_REMAP.items():
                if v:
                    rows.append({"pin": pin, "net": v[0], **({"len": v[1]} if v[1] else {})})
            p["pins"] = rows
        if p["ref"] == "U6":     # OUTL leaves on DAC_OUTL, OUTR unused
            p["pins"] = [r for r in p["pins"] if r["pin"] != "7"]
            for r in p["pins"]:
                if r["pin"] == "6":
                    r["net"] = "DAC_OUTL"
        if p["ref"] == "R20":    # PCM1808 VCC from 5VA through 10 R
            p["value"], p["lcsc"] = "10R", "C22859"
            p["pins"][0]["net"] = "5VA"
        if p["ref"] == "R6":     # VBUS sense 33k/82k into PA9
            p["value"], p["lcsc"] = "33k", "C4216"
        if p["ref"] == "R7":
            p["value"], p["lcsc"] = "82k", "C23254"
        if p["ref"] == "C18":    # USB-C VBUS bypass, 1 uF X7R, beside J1/D2
            p["lcsc"], p["at"] = "C90540", [205, 45]
        if p["ref"] == "C59":
            p["fields"]["Group"] = "stitch ADC_SCKI via A"
        if p["ref"] == "R30":
            p["pins"][0]["net"] = "POT_MUX_OUT"
        if p["ref"] == "FB1":    # VDDA from VDD through the ferrite, so VDDA cannot lead VDD (AN4938 2.1.2)
            p["pins"][0]["net"] = "3V3"
        for r in p.get("pins", []):
            r["net"] = NET_RENAME.get(r.get("net"), r.get("net")) if r.get("net") else r.get("net")
        p["ref"] = CHILD_REF_RENAME.get(p["ref"], p["ref"])
        parts.append(p)
    c164 = C("10uF", "VDDA", "GND", 155, 185, "C164")   # damps the FB1 LC and lowers its corner
    c164.update(footprint="Capacitor_SMD:C_0805_2012Metric", lcsc="C15850")   # CL21A106KAYNNNE 25 V X5R
    parts.append(c164)
    d["parts"] = parts
    d["ports"] = [{"name": n, "shape": s} for n, s in CHILD_PORTS]
    used = {p["lib_id"] for p in parts} | {r["lib_id"] for r in d["power_symbols"]}
    d["lib_symbols"] = [ls for ls in d["lib_symbols"] if "%s:%s" % tuple(ls) in used]
    d["texts"] = [
        {"text": "A. Compute core: U1 and its decoupling, HSE, reset, BOOT0, user LED", "at": [12, 20]},
        {"text": "B. Power entry: USB-C data and ESD, 3V3 buck, 3V3A LDO, VBUS sense, power LED. 5V comes from the 9 V buck on the top sheet", "at": [120, 20]},
        {"text": "D. Audio out: PCM5102A, OUTL leaves on DAC_OUTL, OUTR unused", "at": [280, 20]},
        {"text": "One ground, GND.", "at": [285, 160]},
        {"text": "SAI1 block A master, MCLK=256fs on PE2. Settings live in internal flash bank 2", "at": [165, 176]},
        {"text": "SWD on Tag-Connect pads J3", "at": [120, 228]},
        {"text": "Supply flags: nets fed through passives or from the connector", "at": [285, 190]},
        {"text": "F. Audio in: PCM1808 on SAI1 block B, slave on BCLK/LRCLK. VINL/VINR arrive AC-coupled from the top sheet", "at": [12, 254]},
        {"text": "U8 AGND and DGND both on GND. VCC from 5VA through R20", "at": [12, 309]},
        {"text": "PC0 ADC RC, AN2834", "at": [95, 32]},
        {"text": "Clock source series R at U1", "at": [95, 62]},
        {"text": "G. Reference-transition stitching, 10 nF 3V3-GND, one per via group:", "at": [320, 222]},
        {"text": "C56 U1-east I2S escapes; C57 U6-west I2S rises; C58 ADC_DOUT rise;", "at": [320, 225]},
        {"text": "C59 ADC_SCKI via A; C61 ADC_SCKI via B", "at": [320, 228]},
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
    u103["pins"] = [row("1", "5V", LF, 4), row("2", "VIN_9V", LF, 4), row("3", "VIN_9V", LF, 4),
                    row("4", "GND", RT), row("5", "BUCK5_SW", RT), row("6", "BUCK5_BST", RT)]
    add(u103)
    add(C("100nF", "BUCK5_BST", "BUCK5_SW", X0 + 62, 112, "C116"))
    l101 = copy.deepcopy(keep["L101"]); l101.update(at=[X0 + 95, 112])
    l101["pins"] = [row("1", "BUCK5_SW", LF), row("2", "5V", RT)]
    add(l101)
    add(C("22uF", "5V", "GND", X0 + 122, 112, "C114"), C("22uF", "5V", "GND", X0 + 132, 112, "C115"))
    fb = {"ref": "FB101", "lib_id": "Device:FerriteBead", "value": "120R@100MHz",
          "at": [X0 + 10, 162], "angle": 0, "footprint": "Inductor_SMD:L_0603_1608Metric",
          "lcsc": "C14709", "rest": "no_connect", "fields": {"MPN": "BLM18PG121SN1D"},
          "pins": [row("1", "5V", UP), row("2", "5VA", DN)]}
    add(fb)
    add(C("10uF", "5VA", "GND", X0 + 30, 162), C("100nF", "5VA", "GND", X0 + 42, 162))
    note("5VA analog rail from 5V through FB101", X0, 150)
    flags += [flag(X0 + 90, 160, "VIN_9V", 1), flag(X0 + 110, 160, "5V", 2),
              flag(X0 + 130, 160, "5VA", 3)]

    # ------------------------------------------------------------------ MIC / LINE INPUT
    X1, Y1 = 300, 24
    note("2. MIC / LINE INPUT", X1, Y1)
    note("XLR leg 47R, BAT54S clamp, 100pF, 100k to GND, 47uF + IN_x. TRS leg 10uF + LINE_x, 22k pad, 100k to GND. 1.2k bias to VREF", X1, Y1 + 4)
    jin = {"ref": "J106A", "lib_id": "cubevox:NCJ6FA-H", "value": "J_IN NCJ6FA-H", "at": [X1 + 12, 120],
           "angle": 180, "footprint": "cubevox:CONN-TH_NCJ6FA-H", "lcsc": "C368458",
           "rest": "no_connect", "in_bom": True, "fields": {"MPN": "NCJ6FA-H"},
           "pins": [row("1", "GND", RT), row("2", "XLR_2", RT), row("3", "XLR_3", RT),
                    row("4", "GND", RT), row("5", "TRS_R", RT), row("6", "TRS_T", RT),
                    row("7", "TRS_T", RT), row("8", "GND", RT)]}
    jin["ref"] = "J101"
    add(jin)
    note("Both T holes are the tip (Neutrik: one tip circuit, no switch). No plug detection", X1 + 4, 150)
    legs = {"P": ("XLR_2", "TRS_T", 66, ["R101", "C119", "C120", "C121", "R102", "R103", "R144"]),
            "N": ("XLR_3", "TRS_R", 140, ["R104", "C123", "C124", "C125", "R105", "R106", "R145"])}
    for tag, (xlr, trs, ya, (rs, cs, cx, ct, rp, rb, rl)) in legs.items():
        x = X1 + 40
        add(R("47R", xlr, "MIC_" + tag, x, ya, rs))
        bat = {"ref": nxt("D", 107), "lib_id": "cubevox_h7core:BAT54SLT1G", "value": "BAT54S",
               "at": [x + 22, ya], "angle": 0,
               "footprint": "cubevox_h7core:SOT-23_L2.9-W1.3-P1.90-LS2.4-BR", "lcsc": "C19726",
               "rest": "no_connect", "in_bom": True, "fields": {"MPN": "BAT54SLT1G"},
               "pins": [row("1", "GND", LF), row("2", "5VA", RT), row("3", "MIC_" + tag, DN)]}
        add(bat)
        add(C("100pF", "MIC_" + tag, "GND", x + 48, ya, cs))
        add(CE("47uF", "IN_" + tag, "MIC_" + tag, x + 60, ya, cx))     # + on the IN_x side (VREF)
        add(CE("10uF", "LINE_" + tag, trs, x + 80, ya, ct))            # + on the resistor side
        add(R("22k", "LINE_" + tag, "IN_" + tag, x + 92, ya, rp))
        add(R("1.2k", "IN_" + tag, "VREF", x + 104, ya, rb))
        add(R("100k", "MIC_" + tag, "GND", x + 116, ya, rl))           # defines 0 V on the jack side
        add(R("100k", trs, "GND", x + 128, ya + 14, "R148" if tag == "P" else "R149"))   # unplugged TRS bias

    # ------------------------------------------------------------------ PREAMP
    X2, Y2 = 468, 24
    note("3. PREAMP", X2, Y2)
    note("Stage 1 INA 20.05 dB (U104, U105A). Stage 2 0-29.9 dB on Input Gain pot (U105B)", X2, Y2 + 4)
    note("Chain 20-50 dB XLR / -5.6 to +24.3 dB TRS; DC gain 1 (AC-coupled legs)", X2, Y2 + 8)
    add(opamp("U104", X2 + 30, 72, {"1": "INA_O1", "2": "A1_FB", "3": "IN_P", "4": "GND",
                                    "5": "IN_N", "6": "A2_FB", "7": "INA_O2", "8": "5VA"}))
    add(C("100nF", "5VA", "GND", X2 + 62, 80, "C127"))
    xs = X2 + 84
    add(R("4.53k", "INA_O1", "A1_FB", xs, 72, "R108"), C("47pF", "INA_O1", "A1_FB", xs + 12, 72, "C128"),
        R("1k", "RG_MID", "A2_FB", xs + 24, 72, "R109"),
        R("4.53k", "INA_O2", "A2_FB", xs + 36, 72, "R110"), C("47pF", "INA_O2", "A2_FB", xs + 48, 72, "C129"))
    c158 = C("22uF", "A1_FB", "RG_MID", xs + 60, 72, "C158")      # bipolar: the RG leg sees both polarities
    c158.update(value="22uF 25V NP", footprint="cubevox:CAP-SMD_BD6.3-L6.6-W6.6-FD", lcsc="C413679",
                fields={"MPN": "EEEHP1E220P"})
    add(c158)
    note("RG = R109 1k + C158 22uF bipolar in series, DC gain 1. Gain 1 + 2 x 4.53k / 1k = 10.06", xs, 52)
    add(opamp("U105", X2 + 30, 172, {"1": "PRE1", "2": "DA_N", "3": "DA_P", "4": "GND",
                                     "5": "PRE1", "6": "G2_INV", "7": "PRE_OUT", "8": "5VA"}))
    add(C("100nF", "5VA", "GND", X2 + 62, 180, "C130"))
    add(R("10k 0.1%", "INA_O1", "DA_P", xs, 172, "R111"), R("10k 0.1%", "DA_P", "VREF", xs + 12, 172, "R112"),
        R("10k 0.1%", "INA_O2", "DA_N", xs + 24, 172, "R113"), R("10k 0.1%", "DA_N", "PRE1", xs + 36, 172, "R114"))
    add(pot("RVGAIN1", X2 + 40, 252, "PRE_OUT", "G2_INV", "G2_INV", "10k GAIN"))
    add(R("330R", "G2_INV", "G2_RG", xs + 4, 252, "R116"),
        C("47pF", "PRE_OUT", "G2_INV", xs + 28, 252, "C132"),
        CE("100uF", "G2_RG", "GND", xs + 40, 252, "C159"))        # + on the R116 side
    note("Input Gain pot is the A4 feedback resistor, wiper tied to end 3. G = 1 + Rpot/330", X2, 232)
    add(opamp("U106", X2 + 30, 322, {"1": "VREF_BUF", "2": "VREF_BUF", "3": "VREF_DIV", "4": "GND",
                                     "5": "BUF_IN", "6": "BUF_OUT", "7": "BUF_OUT", "8": "5VA"}))
    add(C("100nF", "5VA", "GND", X2 + 62, 330, "C134"))
    add(R("10k", "5VA", "VREF_DIV", xs, 322, "R117"), R("10k", "VREF_DIV", "GND", xs + 12, 322, "R118"),
        C("10uF", "VREF_DIV", "GND", xs + 24, 322, "C135"),
        R("22R", "VREF_BUF", "VREF", xs + 36, 322, "R146"),
        CE("10uF", "VREF", "GND", xs + 48, 322, "C160"))
    note("VREF: 5VA divided 10k/10k, 10 uF, follower U106A, 22R into VREF with 10 uF. U106B drives block 6", X2, 292)

    # ------------------------------------------------------------------ ADC FEED
    X3, Y3 = 640, 24
    note("4. ADC FEED", X3, Y3); note("PRE_OUT always feeds the ADC", X3, Y3 + 4)
    add(R("100R", "PRE_OUT", "ADC_RC", X3 + 4, 66, "R119"), C("2.2nF", "ADC_RC", "GND", X3 + 16, 66, "C136"),
        C("10uF", "ADC_RC", "ADC_VINL", X3 + 28, 66, "C137"))
    add(C("10uF", "ADC_VINR", "GND", X3 + 28, 140, "C138"))
    note("VINR unused, AC-coupled to GND", X3 + 4, 170)

    # ------------------------------------------------------------------ BYPASS
    X4, Y4 = 722, 24
    note("5. BYPASS", X4, Y4); note("Pole A COM BUF_IN, NC PRE_OUT, NO DAC_ATT. Pole B COM GND, NO FX_ON_SENSE (low = effect on). Coil on = effect on", X4, Y4 + 4)
    k = copy.deepcopy(keep["K101"]); k.update(at=[X4 + 28, 76], value="G6K-2F-Y DC5")
    k["pins"] = [row("1", "SW_BYPASS", DN, 4), row("2", "PRE_OUT", DN, 6), row("3", "BUF_IN", DN, 8),
                 row("4", "DAC_ATT", DN, 10), row("5", "FX_ON_SENSE", UP, 10), row("6", "GND", UP, 8),
                 row("8", "GND", UP, 4)]
    add(k)
    d104 = copy.deepcopy(keep["D104"]); d104.update(at=[X4 + 90, 76])
    d104["pins"] = [row("1", "SW_BYPASS", LF), row("2", "GND", RT)]
    add(d104)
    add(toggle("SWBYPASS1", X4 + 80, 130, "SW_BYPASS", "5V"))
    note("Bypass toggle. Common to the coil, one throw to 5V", X4 + 96, 130)
    add(R("10k", "3V3", "FX_ON_SENSE", X4 + 90, 180, "R120"), C("100nF", "FX_ON_SENSE", "GND", X4 + 102, 180, "C162"))

    # ------------------------------------------------------------------ DAC / OUTPUT
    X5, Y5 = 840, 24
    note("6. DAC / OUTPUT", X5, Y5); note("470R / 2.2nF filter, 10uF, DAC_ATT = 0.6 x DAC about VREF", X5, Y5 + 4)
    note("U106B buffers to the jack", X5, Y5 + 8)
    add(CE("10uF", "DAC_AC", "DAC_LP", X5 + 4, 66, "C139"),          # + on the divider side
        R("10k", "DAC_AC", "DAC_ATT", X5 + 16, 66, "R122"), R("15k", "DAC_ATT", "VREF", X5 + 28, 66, "R123"),
        R("470R", "DAC_OUTL", "DAC_LP", X5 + 40, 66, "R147"), C("2.2nF", "DAC_LP", "GND", X5 + 52, 66, "C161"))
    add(R("100R", "BUF_OUT", "BUF_R", X5 + 4, 140, "R124"), CE("10uF", "BUF_R", "OUT_AC", X5 + 16, 140, "C141"),
        R("100k", "OUT_AC", "GND", X5 + 28, 140, "R125"))
    jout = {"ref": "J108", "lib_id": "cubevox:PJ-611E", "value": "OUT 6.35mm",     # HOOYA PJ-611E (owner 2026-10-02)
            "at": [X5 + 20, 200], "angle": 0, "footprint": "cubevox:AUDIO-TH_PJ-611E_1",
            "lcsc": "C309282", "rest": "no_connect", "in_bom": True, "fields": {"MPN": "PJ-611E"},
            "pins": [row("6", "OUT_TIP", RT), row("2", "GND", RT)]}
    add(jout)
    note("OUT_TIP via K102 (section 8)", X5 + 36, 200)

    # ------------------------------------------------------------------ MUTE / 5VA SENSE
    X7, Y7 = 740, 300
    note("8. OUTPUT MUTE", X7, Y7)
    note("K102 closes OUT_AC to OUT_TIP when MUTE_N is high, else OUT_TIP is grounded", X7, Y7 + 4)
    k2 = copy.deepcopy(keep["K101"]); k2.update(ref="K102", at=[X7 + 28, Y7 + 52], value="G6K-2F-Y DC5")
    k2["pins"] = [row("1", "5V", DN, 4), row("3", "OUT_AC", DN, 8), row("4", "OUT_TIP", DN, 10),
                  row("6", "OUT_TIP", UP, 8), row("7", "GND", UP, 6), row("8", "MUTE_COIL", UP, 4)]
    add(k2)
    d109 = copy.deepcopy(keep["D104"]); d109.update(ref="D109", at=[X7 + 90, Y7 + 52])
    d109["pins"] = [row("1", "5V", LF), row("2", "MUTE_COIL", RT)]
    add(d109)
    q101 = {"ref": "Q101", "lib_id": "cubevox:MMBT3904-C20526", "value": "MMBT3904",
            "at": [X7 + 60, Y7 + 100], "angle": 0, "footprint": "cubevox:SOT-23-3_L2.9-W1.3-P1.90-LS2.4-BR",
            "lcsc": "C20526", "rest": "no_connect", "in_bom": True, "fields": {"MPN": "MMBT3904"},
            "pins": [row("3", "MUTE_COIL", UP), row("1", "MUTE_BASE", LF), row("2", "GND", DN)]}
    add(q101)
    add(R("1k", "MUTE_N", "MUTE_BASE", X7 + 40, Y7 + 100, "R150"), R("100k", "MUTE_BASE", "GND", X7 + 40, Y7 + 120, "R153"))

    # ------------------------------------------------------------------ 5VA SENSE
    X8, Y8 = 108, 190
    note("5VA SENSE", X8, Y8)
    add(R("10k", "5VA", "VA_SENSE", X8 + 4, Y8 + 24, "R151"), R("10k", "VA_SENSE", "GND", X8 + 16, Y8 + 24, "R152"),
        C("100nF", "VA_SENSE", "GND", X8 + 28, Y8 + 24, "C163"))

    # ------------------------------------------------------------------ PANEL
    X6, Y6 = 108, 280
    _counters["R"], _counters["C"] = 126, 142   # panel refs stay as in revision 1; new analog refs are R144+, C158+
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
    add(dict(_enc("ENC101", "EC11E15244B2", "C470754", X6 + 20, enc_y, "ENC_MENU_A", "ENC_MENU_B", "ENC_MENU_SW")))
    add(dict(_enc("ENC102", "EC11E15204A3", "C470710", X6 + 160, enc_y, "ENC_KEY_A", "ENC_KEY_B", None)))
    add(dict(_enc("ENC103", "EC11E15204A3", "C470710", X6 + 300, enc_y, "ENC_SEMI_A", "ENC_SEMI_B", None)))
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
        add(toggle("SW%d" % (101 + i), tx + 36, ty, "TOGGLE%d" % (i + 1), "GND"))
    # OLED
    oled = {"ref": "J117", "lib_id": "cubevox:OLED-1.3-SH1106-I2C-4P", "value": "OLED SH1106",
            "at": [X6 + 520, 470], "angle": 0, "footprint": "cubevox:OLED-1.3-SH1106-I2C-4P",
            "lcsc": "owner-supplied module, hand-plugged", "rest": "no_connect", "in_bom": True, "fields": {"MPN": "SH1106 1.3in I2C 128x64"},
            "pins": [row("1", "GND", LF), row("2", "3V3", LF), row("3", "I2C_SCL", LF), row("4", "I2C_SDA", LF)]}
    add(oled)
    add(R("4.7k", "3V3", "I2C_SCL", X6 + 560, 470), R("4.7k", "3V3", "I2C_SDA", X6 + 572, 470))

    # mounting holes kept from fxbox
    for i, r in enumerate(["H101", "H102", "H103", "H104"]):
        h = copy.deepcopy(keep[r]); h["at"] = [X6 + 16 * i, 640]
        parts.append(h)

    top = {k: old[k] for k in ("project", "uuid_namespace", "paper", "libraries")}
    top.update(title="cubevox vocal FX box", date=DATE, rev="1")
    top["paper"] = "A0"
    top["libraries"]["cubevox_h7core"] = "${BOARD_DIR}/cubevox_h7core.kicad_sym"
    top["lib_symbols"] = [
        ["cubevox", "OPA2197IDR"], ["cubevox", "SS34_C8678"], ["cubevox", "SMBJ15CA_C19077570"],
        ["cubevox", "PJ-002A"], ["cubevox", "AP63205WU-7"], ["cubevox", "ANR5040T4R7M"],
        ["cubevox", TOGGLE_SYM], ["cubevox", "PZ254V-11-04P_C2691448"], ["cubevox", "OLED-1.3-SH1106-I2C-4P"],
        ["cubevox", "NCJ6FA-H"], ["cubevox", "G6K-2F-Y-DC5"], ["cubevox", "1N4148W_C81598"],
        ["cubevox", "CD74HC4067SM96"], ["cubevox", "RK09D1130C2P"], ["cubevox", "EC11E15244B2"],
        ["cubevox", "EC11E15204A3"], ["cubevox_h7core", "BAT54SLT1G"],
        ["Device", "R"], ["Device", "C"],
        ["cubevox", "RVT1E100M0405-C72484"], ["cubevox", "VT1A101M0505"], ["cubevox", "RVT1C220M0405"],
        ["cubevox", "RVT1E470M0605"], ["cubevox", "MMBT3904-C20526"], ["Device", "FerriteBead"],
        ["Mechanical", "MountingHole"], ["power", "PWR_FLAG"], ["cubevox", "PJ-611E"]]
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
            "footprint": "cubevox:SW-TH_EC11E", "lcsc": lcsc, "rest": "no_connect",
            "in_bom": True, "fields": {"MPN": mpn}, "pins": rows}


if __name__ == "__main__":
    top, child = build_top()
    json.dump(top, open(os.path.join(HW, "cubevox.json"), "w"), indent=1)
    json.dump(child, open(os.path.join(HW, "h7core_block.json"), "w"), indent=1)
    print("wrote", len(top["parts"]), "top parts,", len(child["parts"]), "child parts")
