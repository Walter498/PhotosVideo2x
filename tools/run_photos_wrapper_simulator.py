#!/usr/bin/env python3
"""Build and execute real PhotosPlayer wrapper checks on iOS 17.5 Simulator.

Default requires the production native command bridge. --raw-wrapper-only is
explicitly partial coverage and never reports a full native integration pass."""
import argparse
import json
import os
from pathlib import Path
import platform
import plistlib
import re
import shutil
import subprocess
import sys
import time
import traceback

ROOT = Path(__file__).resolve().parents[1]
BUNDLE = "org.pv2.PhotosWrapperRuntimeTests"
EXECUTABLE = "PV2PhotosWrapperSimulator"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--timeout", type=float, default=300,
                        help="global build/boot/test deadline in seconds (default 300)")
    parser.add_argument("--udid", help="available iOS Simulator UUID (never a physical device)")
    parser.add_argument("--runtime", default="17.5", help="must select iOS 17.5 (default 17.5)")
    parser.add_argument("--raw-wrapper-only", action="store_true",
                        help="run only real wrapper checks; excludes native bridge acceptance")
    parser.add_argument("--device", default="iPhone", help="device name substring when auto selecting")
    parser.add_argument("--output", type=Path, default=ROOT / "build/photos-wrapper-simulator")
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    out = args.output.expanduser().resolve()
    out.mkdir(parents=True, exist_ok=True)
    deadline = time.monotonic() + args.timeout
    transcript = out / "commands.log"
    udid = None
    container = None
    launched = False
    metadata = {"passed": False, "platform": platform.platform(), "timeout": args.timeout,
                "coverage": "raw-wrapper-only" if args.raw_wrapper_only else "native-bridge-required"}
    # A prior run's successful result must never satisfy this invocation.
    for name in ("result.json", "result.txt", "photos-wrapper-visible.png", "failure.png"):
        (out / name).unlink(missing_ok=True)

    def run(command, cap=None, check=True, diagnostic=False):
        remaining = deadline - time.monotonic()
        if not diagnostic and remaining <= 0:
            raise TimeoutError("global simulator harness deadline exceeded")
        timeout = (cap or 20) if diagnostic else min(cap or args.timeout, remaining)
        with transcript.open("a") as log:
            log.write("\n$ " + " ".join(map(str, command)) + "\n")
            log.flush()
            result = subprocess.run(list(map(str, command)), stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, text=True, timeout=max(.1, timeout))
            log.write(result.stdout)
        if check and result.returncode:
            raise RuntimeError(f"command exited {result.returncode}: {' '.join(map(str, command))}\n{result.stdout[-6000:]}")
        return result

    def sim(*parts, **kwargs):
        return run(["xcrun", "simctl", *parts], **kwargs)

    def copy_documents():
        if container is not None:
            for name in ("result.json", "result.txt"):
                path = container / "Documents" / name
                if path.exists():
                    shutil.copy2(path, out / name)

    try:
        if platform.system() != "Darwin":
            raise RuntimeError("Requires macOS + full Xcode + an installed iOS Simulator runtime; iSH cannot run this test")
        if not (ROOT / "NativeTimeline.h").is_file():
            raise RuntimeError("Production NativeTimeline.h is absent; no fake fallback is permitted")
        inventory = json.loads(sim("list", "--json").stdout)
        runtimes = {r["identifier"]: r for r in inventory["runtimes"]
                    if r.get("isAvailable") and ".iOS-" in r["identifier"]}
        candidates = []
        for runtime, devices in inventory["devices"].items():
            if runtime not in runtimes:
                continue
            for device in devices:
                if not device.get("isAvailable"):
                    continue
                if args.udid and device["udid"] != args.udid:
                    continue
                if not args.udid and args.device.lower() not in device["name"].lower():
                    continue
                rt = runtimes[runtime]
                if not re.fullmatch(r"17\.5(?:\.\d+)?", rt.get("version", "")):
                    continue
                if args.runtime and args.runtime not in runtime and args.runtime not in rt.get("version", ""):
                    continue
                version = tuple(int(x) for x in re.findall(r"\d+", rt.get("version", "0")))
                candidates.append((version, device["state"] == "Booted", device["udid"], runtime, device))
        if not candidates:
            raise RuntimeError("No matching available iOS 17.5 simulator; install a runtime/create a device in Xcode first")
        _, _, udid, runtime, device = sorted(candidates, reverse=True)[0]
        metadata.update(udid=udid, device=device["name"], runtime=runtimes[runtime])
        (out / "selection.json").write_text(json.dumps(metadata, indent=2))
        sdk = run(["xcrun", "--sdk", "iphonesimulator", "--show-sdk-path"]).stdout.strip()
        app = out / "PV2PhotosWrapperSimulator.app"
        if app.exists():
            shutil.rmtree(app)
        app.mkdir()
        info = {
            "CFBundleIdentifier": BUNDLE, "CFBundleExecutable": EXECUTABLE,
            "CFBundleName": "PV2 Photos Wrapper Test", "CFBundlePackageType": "APPL",
            "CFBundleVersion": "1", "CFBundleShortVersionString": "1.0",
            "MinimumOSVersion": "17.5", "LSRequiresIPhoneOS": True,
            "UIDeviceFamily": [1, 2], "UILaunchScreen": {},
            "UISupportedInterfaceOrientations": ["UIInterfaceOrientationPortrait",
                "UIInterfaceOrientationLandscapeLeft", "UIInterfaceOrientationLandscapeRight"],
        }
        with (app / "Info.plist").open("wb") as f:
            plistlib.dump(info, f)
        # Native simctl processes and clang target agree on Apple Silicon / Intel.
        arch = platform.machine()
        if arch not in ("arm64", "x86_64"):
            raise RuntimeError(f"unsupported macOS architecture {arch}")
        command = ["xcrun", "--sdk", "iphonesimulator", "clang++",
                   "-target", f"{arch}-apple-ios17.5-simulator", "-isysroot", sdk,
                   "-std=c++17", "-fobjc-arc", "-fblocks", "-O0", "-g",
                   "-Wall", "-Wextra", "-Wno-unused-parameter", "-I", ROOT,
                   ROOT / "tests/photos-wrapper-simulator/PhotosWrapperSimulator.mm",
                   "-o", app / EXECUTABLE]
        for framework in ("UIKit", "Foundation", "AVFoundation", "AVKit", "CoreMedia", "CoreVideo", "QuartzCore", "CoreGraphics"):
            command += ["-framework", framework]
        run(command)
        run(["/usr/bin/codesign", "--force", "--sign", "-", app])
        if device["state"] != "Booted":
            sim("boot", udid)
        sim("bootstatus", udid, "-b")
        sim("terminate", udid, BUNDLE, check=False)
        sim("install", udid, app)
        container = Path(sim("get_app_container", udid, BUNDLE, "data").stdout.strip())
        documents = container / "Documents"
        documents.mkdir(parents=True, exist_ok=True)
        for name in ("result.json", "result.txt", "screenshot-ready", "screenshot-captured"):
            (documents / name).unlink(missing_ok=True)
        launch = sim("launch", "--terminate-running-process",
                     "--stdout=" + str(out / "app.stdout.log"),
                     "--stderr=" + str(out / "app.stderr.log"), udid, BUNDLE, *(["--raw-wrapper-only"] if args.raw_wrapper_only else []))
        launched = True
        metadata["launch"] = launch.stdout.strip()
        while time.monotonic() < deadline:
            result_path = documents / "result.json"
            if result_path.exists():
                result = json.loads(result_path.read_text())
                if result.get("completed"):
                    # result.json is atomically committed slightly before result.txt.
                    if not (documents / "result.txt").exists():
                        time.sleep(.1)
                        continue
                    copy_documents()
                    metadata["appRuntimeVersion"] = result.get("runtime")
                    if not result.get("passed"):
                        raise RuntimeError("Photos wrapper integration assertions failed; inspect result.json/result.txt")
                    if not result.get("realPhotosWrapper") or not result.get("rawWrapperPassed"):
                        raise RuntimeError("Missing completed real Photos wrapper coverage")
                    if not args.raw_wrapper_only and not (result.get("bridgeTested") and result.get("intentTested") and result.get("fullAcceptance")):
                        raise RuntimeError("Full acceptance requires production bridge and real PXVideoSession intent coverage")
                    sim("io", udid, "screenshot", out / "photos-wrapper-visible.png", cap=10)
                    metadata["passed"] = True
                    print(f"PASS iOS {result.get('runtime')} [{udid}] — {len(result['checks'])} checks; coverage={metadata['coverage']}; {out}")
                    return 0
            time.sleep(.2)
        raise TimeoutError(f"No completed result within {args.timeout:g}s")
    except Exception as error:
        metadata["error"] = str(error)
        (out / "runner-error.txt").write_text(traceback.format_exc())
        copy_documents()
        if udid:
            diagnostics = [
                (["io", udid, "screenshot", str(out / "failure.png")], "screenshot-error.log"),
                (["spawn", udid, "log", "show", "--last", "5m", "--style", "compact",
                  "--predicate", f'process == "{EXECUTABLE}" OR eventMessage CONTAINS "{BUNDLE}"'], "simulator.log"),
                (["diagnose", "-l", "--udid", udid, "--output", str(out / "diagnostics")], "diagnose.log"),
            ]
            # Best-effort diagnostic commands get a separate bounded budget: at most 30s total.
            for parts, filename in diagnostics:
                try:
                    r = sim(*parts, cap=10, check=False, diagnostic=True)
                    (out / filename).write_text(r.stdout)
                except Exception as diagnostic_error:
                    (out / filename).write_text(str(diagnostic_error))
            crash_dir = Path.home() / "Library/Logs/DiagnosticReports"
            for path in crash_dir.glob(EXECUTABLE + "*"):
                if path.is_file():
                    target = out / "crashes"
                    target.mkdir(exist_ok=True)
                    shutil.copy2(path, target / path.name)
        print(f"FAIL: {error}\nArtifacts: {out}", file=sys.stderr)
        return 1
    finally:
        # Never erase/create/delete/shutdown devices. Only terminate this test app.
        if udid and launched:
            try:
                sim("terminate", udid, BUNDLE, check=False, cap=5, diagnostic=True)
            except Exception:
                pass
        (out / "runner-result.json").write_text(json.dumps(metadata, indent=2))


if __name__ == "__main__":
    sys.exit(main())
