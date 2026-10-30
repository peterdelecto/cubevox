#!/usr/bin/env python3
"""Checks the build, flash, bring-up and release scripts without a board, a toolchain or a network.

It covers the version bump, the output-dir and variant rules of firmware/build.sh, the bench symbol
check, the boot-line match of term.py, the dfu-util check of bringup.py, the DIAG.TXT blocks line
that bringup.py parses against the one the bootloader's diag.c writes, the version guard of
flash.sh, the .clangd rewrite, the bootloader's font against the application's 5x7, and the pins
and release steps that must agree across build.sh and the workflows. Nothing here opens a serial
port, a window or a drive.

  python3 -I tools/scripts_check.py
"""

import importlib.util
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
FW = os.path.join(ROOT, "firmware")
TOOLS = os.path.join(ROOT, "tools")
WORKFLOWS = os.path.join(ROOT, ".github", "workflows")
sys.path.insert(0, FW)  # -I drops the script dir
import bringup  # noqa: E402
import term  # noqa: E402


def read(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


def run(argv, env=None, cwd=None):
    """Runs a command with a clean variant environment. Returns (exit code, stdout plus stderr)."""
    base = {k: v for k, v in os.environ.items() if k not in ("OUT", "VARIANT", "BENCH", "NM")}
    r = subprocess.run(argv, env={**base, **(env or {})}, cwd=cwd, capture_output=True, text=True)
    return r.returncode, r.stdout + r.stderr


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def bump_failures():
    """tools/bump_version.sh: decimal fields, carry, and a refusal that leaves the file alone."""
    cases = [("2.00.27", "2.00.28"), ("2.00.99", "2.01.00"), ("2.08.00", "2.08.01"), ("2.09.50", "2.09.51"),
             ("2.08.99", "2.09.00"), ("2.09.99", "2.10.00")]
    refused = ["2.99.99", "2.0.1", "junk"]
    failed = []
    with tempfile.TemporaryDirectory() as d:
        os.mkdir(os.path.join(d, "tools"))
        shutil.copy(os.path.join(TOOLS, "bump_version.sh"), os.path.join(d, "tools"))
        version = os.path.join(d, "VERSION.txt")
        for before, after in cases + [(r, r) for r in refused]:
            with open(version, "w") as f:
                f.write(before + "\n")
            code, out = run(["bash", os.path.join(d, "tools", "bump_version.sh")])
            now = read(version).strip()
            if before == after:
                if code == 0 or "[ERROR]" not in out or now != before:
                    failed.append(f"bump_version.sh {before}: exit {code}, file '{now}', wanted a refusal")
            elif code != 0 or now != after or out.strip() != after:
                failed.append(f"bump_version.sh {before}: exit {code}, file '{now}', printed '{out.strip()}', wanted {after}")
    return failed


def build_query_failures():
    """firmware/build.sh answers --variant, --outdir and --help before it needs a toolchain."""
    script = os.path.join(FW, "build.sh")
    failed = []
    with tempfile.TemporaryDirectory() as d:
        cases = [
            (["--outdir"], {}, "/tmp/cubevox-fw-release"),
            (["--outdir"], {"BENCH": "1"}, "/tmp/cubevox-fw-bench"),
            (["--outdir"], {"VARIANT": "bench"}, "/tmp/cubevox-fw-bench"),
            (["--outdir"], {"OUT": "/tmp/cubevox-check"}, "/tmp/cubevox-check"),
            (["--outdir"], {"OUT": "/tmp/cubevox-check/", "BENCH": "1"}, "/tmp/cubevox-check"),
            (["--outdir"], {"OUT": "here/out"}, os.path.join(os.path.realpath(d), "here/out")),
            (["--variant"], {}, "release"),
            (["--variant"], {"BENCH": "1"}, "bench"),
        ]
        for args, env, want in cases:
            code, out = run(["bash", script, *args], env, cwd=d)
            if code != 0 or os.path.realpath(out.strip()) != os.path.realpath(want):
                failed.append(f"build.sh {' '.join(args)} {env}: exit {code}, printed '{out.strip()}', wanted {want}")
        for args, env in [([], {"VARIANT": "release", "BENCH": "1"}), (["--bogus"], {}), ([], {"VARIANT": "x"})]:
            code, out = run(["bash", script, *args], env, cwd=d)
            if code == 0 or "[ERROR]" not in out:
                failed.append(f"build.sh {' '.join(args)} {env}: exit {code}, wanted a refusal with [ERROR]")
    for flag in ("--help", "-h"):
        code, out = run(["bash", script, flag])
        marked = [line for line in out.splitlines() if line.startswith("#")]
        if code != 0 or not out.startswith("Compile the cubevox firmware") or marked:
            failed.append(f"build.sh {flag}: exit {code}, first line '{out.splitlines()[0] if out else ''}'")
    return failed


def pin_failures():
    """The pins in build.sh against the workflows' copies and the bootloader build's."""
    build = read(os.path.join(FW, "build.sh"))
    pins = {k: re.search(rf'^{k}="([^"]+)"', build, re.M) for k in ("CLI_VERSION", "CORE_VERSION", "XPACK_VERSION")}
    if not all(pins.values()):
        return [f"build.sh lacks a pin line for {[k for k, m in pins.items() if not m]}"]
    pins = {k: m.group(1) for k, m in pins.items()}
    failed = []
    for name in sorted(os.listdir(WORKFLOWS)):
        text = read(os.path.join(WORKFLOWS, name))
        for key, pin in (("ARDUINO_CLI_VERSION", "CLI_VERSION"), ("STM32_CORE_VERSION", "CORE_VERSION")):
            for have in re.findall(rf"^\s+{key}:\s*(\S+)\s*$", text, re.M):
                if have != pins[pin]:
                    failed.append(f"{name} {key} is {have}, build.sh {pin} is {pins[pin]}")
    boot = read(os.path.join(FW, "bootloader", "build.sh"))
    literal = re.search(r'^XPACK_VERSION="([^"]+)"', boot, re.M)
    if literal and literal.group(1) != pins["XPACK_VERSION"]:
        failed.append(f"bootloader/build.sh XPACK_VERSION is {literal.group(1)}, build.sh has {pins['XPACK_VERSION']}")
    if not literal and "--toolchain" not in boot:
        failed.append("bootloader/build.sh neither pins XPACK_VERSION nor asks build.sh --toolchain")
    return failed


def symbol_failures():
    """tools/check_bench_symbols.sh with a stand-in for nm, and its names against the sources."""
    script = os.path.join(TOOLS, "check_bench_symbols.sh")
    names = re.search(r"BENCH_ONLY=\(([^)]*)\)", read(script)).group(1).split()
    sources = "".join(read(os.path.join(d, f)) for d, _, files in os.walk(os.path.join(FW, "cubevox"))
                      for f in files if f.endswith((".cpp", ".h", ".ino")))
    failed = [f"{n} is in BENCH_ONLY but no firmware source defines it" for n in names
              if not re.search(rf"\b{n}\b", sources)]
    with tempfile.TemporaryDirectory() as d:
        failed += symbol_case_failures(script, names, d)
    return failed


def symbol_case_failures(script, names, d):
    """Runs the symbol check on made-up symbol lists. A stand-in for nm prints the file it is given."""
    fake_nm = os.path.join(d, "fake-nm")
    with open(fake_nm, "w") as f:
        f.write('#!/bin/sh\ncat "$1"\n')
    os.chmod(fake_nm, os.stat(fake_nm).st_mode | stat.S_IXUSR)
    elf = os.path.join(d, "symbols.txt")

    def check(variant, lines):
        with open(elf, "w") as f:
            f.write("".join(f"08000000 T {s}\n" for s in lines))
        return run(["bash", script, variant, elf], {"NM": fake_nm})

    common = ["main", "loop", "_Z5setupv"]
    mangled = [f"_Z{len(n)}{n}v" for n in names]
    cases = [
        ("release", common, True, ""), ("release", common + mangled[:1], False, names[0]),
        ("release", common + mangled, False, names[-1]), ("release", [], False, "no symbols"),
        ("bench", common + mangled, True, ""), ("bench", common + mangled[1:], False, names[0]),
        ("bench", common, False, names[0]),
    ]
    failed = []
    for variant, lines, ok, word in cases:
        code, out = check(variant, lines)
        if (code == 0) != ok or (word and word not in out) or (not ok and "[ERROR]" not in out):
            failed.append(f"check_bench_symbols.sh {variant} with {len(lines)} symbols: exit {code}, '{out.strip()}'")
    code, out = run(["bash", script, "other", elf])
    if code == 0 or "usage" not in out:
        failed.append("check_bench_symbols.sh accepts an unknown variant")
    return failed


def term_failures():
    """The boot-line match term.py uses to judge a flash, and the firmware line it matches."""
    good = "[boot] image v2.00.27 crc ok, boot fails 0, last update reason none"
    cases = [
        (good, "v2.00.27", True), (good, "v2.00.2", False), (good, "v2.00.27-bench", False),
        ("[boot] image v2.00.27-bench crc ok, boot fails 0, last update reason none", "v2.00.27-bench", True),
        ("[boot] image v2.00.27-bench crc ok, boot fails 0, last update reason none", "v2.00.27", False),
        ("[boot] image v2.00.27 crc BAD, boot fails 0, last update reason none", "v2.00.27", False),
        ("[boot] build release", "v2.00.27", False),
    ]
    failed = [f"term.image_line_matches('{line}', '{ver}') is not {want}"
              for line, ver, want in cases if term.image_line_matches(line, ver) != want]
    fault = read(os.path.join(FW, "cubevox", "src", "board", "fault.cpp"))
    for literal in ('"[boot] image "', '" crc ok"'):
        if literal not in fault:
            failed.append(f"board/fault.cpp no longer prints {literal}, which term.py --expect-image matches")
    return failed


def bringup_failures():
    """bringup.py refuses --dfu without dfu-util before it touches a board, and names its checklist."""
    failed = []
    with tempfile.TemporaryDirectory() as empty:
        before = set(os.listdir(bringup.BRINGUP_DIR)) if os.path.isdir(bringup.BRINGUP_DIR) else None
        code, out = run([sys.executable, "-I", os.path.join(FW, "bringup.py"), "--board", "check", "--dfu"],
                        {"PATH": empty})
        after = set(os.listdir(bringup.BRINGUP_DIR)) if os.path.isdir(bringup.BRINGUP_DIR) else None
    if code != 1 or "[ERROR] dfu-util not installed" not in out:
        failed.append(f"bringup.py --dfu without dfu-util: exit {code}, '{out.strip()[:120]}'")
    if before != after:
        failed.append("bringup.py --dfu without dfu-util wrote into hardware/bringup")
    if not os.path.isfile(bringup.CHECKLIST):
        failed.append(f"{os.path.relpath(bringup.CHECKLIST, ROOT)} is missing; bringup.py names it in its summary")
    return failed


def diag_text_failures():
    """bringup.py reads the DIAG.TXT that the bootloader's diag.c writes.

    The bootloader's host tool drivetext renders sample files with the real diag.c. It is built here
    in a private build dir, so the check needs only cmake and a compiler and never reuses a stale
    binary. The wanted values are the block counts in drivetext.c: DIAG.TXT is the sample box (12
    refused blocks, last family 0x6DB66082) and DIAG-clock.TXT has all counts zero.
    """
    wanted = {"DIAG.TXT": (12, 0x6DB66082), "DIAG-clock.TXT": (0, None)}

    def describe(result):
        refused, family = result
        return f"{refused} refused, family {'none' if family is None else hex(family)}"

    tools = os.path.join(FW, "bootloader", "tools")
    failed = []
    with tempfile.TemporaryDirectory() as d:
        build, drive = os.path.join(d, "build"), os.path.join(d, "drive")
        os.mkdir(drive)
        steps = [["cmake", "-S", tools, "-B", build],
                 ["cmake", "--build", build, "--target", "drivetext", "-j"],
                 [os.path.join(build, "drivetext"), drive]]
        for step in steps:
            code, out = run(step)
            if code:
                return [f"{os.path.basename(step[0])} {step[1]} for drivetext: exit {code}, '{out.strip()[-160:]}'"]
        for name, want in wanted.items():
            got = bringup.parse_refused(bringup.parse_fields(read(os.path.join(drive, name))))
            if got != want:
                failed.append(f"bringup.parse_refused on drivetext's {name} gives {describe(got)}, "
                              f"drivetext.c writes {describe(want)}")
    return failed


def flash_failures():
    """flash.sh refuses an image from another version before it looks for a board."""
    stamp = load("stamp_image", os.path.join(TOOLS, "stamp_image.py"))
    contract = stamp.Contract(stamp.DEFAULT_HEADER)
    failed = []
    with tempfile.TemporaryDirectory() as d:
        binary = os.path.join(d, "cubevox.ino.bin")
        with open(binary, "wb") as f:
            f.write(stamp.fake_image(contract, 4096, b"v0.00.01"))
        steps = [["stamp", binary], ["uf2", binary, os.path.join(d, "cubevox.uf2")]]
        for step in steps:
            code, out = run([sys.executable, "-I", os.path.join(TOOLS, "stamp_image.py"), *step])
            if code != 0:
                return [f"stamp_image.py {step[0]} on the fake image: {out.strip()}"]
        # If the guard ever regressed, the script would go on to the board. A port that cannot open and a
        # cp that copies nothing keep this test from touching a real box or drive.
        fake_bin = os.path.join(d, "bin")
        os.mkdir(fake_bin)
        with open(os.path.join(fake_bin, "cp"), "w") as f:
            f.write("#!/bin/sh\nexit 1\n")
        os.chmod(os.path.join(fake_bin, "cp"), 0o755)
        env = {"OUT": d, "WAIT": "1", "CUBEVOX_PORT": "/dev/null/none", "PATH": fake_bin + os.pathsep + os.environ["PATH"]}
        code, out = run(["bash", os.path.join(FW, "flash.sh")], env)
        want = f"VERSION.txt says v{read(os.path.join(ROOT, 'VERSION.txt')).strip()}"
        if code != 1 or "[ERROR]" not in out or want not in out or "v0.00.01" not in out:
            failed.append(f"flash.sh on a v0.00.01 image: exit {code}, '{out.strip()[-160:]}'")
    return failed


def clangd_failures():
    """tools/clangd_paths.py leaves a file alone when its prefixes are already right and moves all three."""
    paths = load("clangd_paths", os.path.join(TOOLS, "clangd_paths.py"))
    failed = []
    for name in paths.CLANGD_FILES:
        text = read(os.path.join(ROOT, name))
        data = paths.DATA_PREFIX.search(text)
        repo = paths.REPO_PREFIX.search(text)
        user = paths.USER_PREFIX.search(text)
        if not data or not repo:
            failed.append(f"{name} holds no data-dir or checkout prefix the rewrite can find")
            continue
        here = (data.group(1), user.group(1) if user else "/unused", repo.group(1))
        if paths.rewrite(text, *here) != text:
            failed.append(f"{name} changes when rewritten with its own prefixes")
        moved = paths.rewrite(text, "/opt/data dir", "/opt/user dir", "/work/checkout")
        stale = [p for p in here if p != "/unused" and re.search(re.escape(p) + r"(?=/|\s|$)", moved)]
        if stale or paths.rewrite(moved, *here) != text:
            failed.append(f"{name} does not move cleanly to other prefixes and back ({stale})")
    return failed


def bootloader_font_failures():
    """The bootloader's font is the application's 5x7 font, so both draw the same glyphs."""
    app_rel = "firmware/cubevox/src/core/fonts.cpp"
    boot_rel = "firmware/bootloader/boards/cubevox/fonts.cpp"
    app_text = read(os.path.join(ROOT, app_rel))
    app = re.search(r"^constexpr FontDesc k5x7 = CUBEVOX_DESCRIBE\((\w+),", app_text, re.M)
    boot = sorted(set(re.findall(r"\bf::(\w+)::", read(os.path.join(ROOT, boot_rel)))))
    if not app:
        return [f"{app_rel} has no k5x7 CUBEVOX_DESCRIBE line the check can read"]
    if boot != [app.group(1)]:
        return [f"{boot_rel} uses {', '.join(boot) or 'no f:: namespace'}, "
                f"but {app_rel} names the 5x7 font {app.group(1)}"]
    return []


def workflow_failures():
    """The release gate and the CI test flag stay in the workflows."""
    release = read(os.path.join(WORKFLOWS, "release.yml"))
    ci = read(os.path.join(WORKFLOWS, "ci.yml"))
    needs = re.search(r"^  publish:\n(?:    .*\n)*?    needs:\s*\[([^\]]*)\]", release, re.M)
    checks = [
        ("ci.yml runs ctest with --no-tests=error", "--no-tests=error" in ci),
        ("release.yml has a tests job", re.search(r"^  tests:", release, re.M) is not None),
        ("release.yml tests job runs ctest with --no-tests=error", release.count("--no-tests=error") >= 1),
        ("release.yml publish needs tests", bool(needs) and "tests" in needs.group(1)),
        ("release.yml creates a draft release", "--draft" in release),
        ("release.yml never clobbers an asset", "--clobber" not in release),
        ("release.yml attaches the elf", ".elf" in release),
        ("ci.yml uploads the elf", ci.count("cubevox.ino.elf") >= 2),
        ("ci.yml runs the bench-symbol rule on both images with its control",
         "check_bench_symbols.sh bench" in ci and ci.count("check_bench_symbols.sh release") >= 2),
    ]
    return [f"{what} fails" for what, ok in checks if not ok]


def syntax_failures():
    failed = []
    scripts = [os.path.join(FW, n) for n in ("build.sh", "flash.sh", "cv_define.sh", "dfu_util.sh")]
    scripts += [os.path.join(TOOLS, n) for n in os.listdir(TOOLS) if n.endswith(".sh")]
    for path in scripts:
        code, out = run(["bash", "-n", path])
        if code:
            failed.append(f"bash -n {os.path.relpath(path, ROOT)}: {out.strip()}")
    for path in [os.path.join(FW, n) for n in os.listdir(FW) if n.endswith(".py")] + \
                [os.path.join(TOOLS, n) for n in os.listdir(TOOLS) if n.endswith(".py")]:
        try:
            compile(read(path), path, "exec")
        except SyntaxError as e:
            failed.append(f"{os.path.relpath(path, ROOT)}: {e}")
    return failed


CHECKS = [bump_failures, build_query_failures, pin_failures, symbol_failures, term_failures,
          bringup_failures, diag_text_failures, flash_failures, clangd_failures, bootloader_font_failures,
          workflow_failures, syntax_failures]


def main():
    failed = []
    for check in CHECKS:
        try:
            failed += check()
        except (OSError, ValueError, AttributeError, subprocess.SubprocessError) as e:
            failed.append(f"{check.__name__}: {type(e).__name__}: {e}")
    for msg in failed:
        print("FAIL", msg)
    if not failed:
        print(f"PASS the scripts agree with their rules ({len(CHECKS)} groups)")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
