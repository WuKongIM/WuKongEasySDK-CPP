"""Consume the published Git registry from an independent application directory."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("--triplet", required=True)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
example = root / "examples/vcpkg-consumer"
config = json.loads((example / "vcpkg-configuration.json").read_text())
registry = config["registries"][0]
# Ensure CI exercises the exact port tree indexed by the documented registry baseline.
version_path = "versions/w-/wukong-easy-sdk.json"
version = json.loads(subprocess.check_output(
    ["git", "show", registry["baseline"] + ":" + version_path], cwd=root, text=True))["versions"][0]
port_tree = subprocess.check_output(
    ["git", "rev-parse", "HEAD:ports/wukong-easy-sdk"], cwd=root, text=True).strip()
assert version["git-tree"] == port_tree, "Consumer baseline must index the candidate port"
workspace = root / "build/vcpkg-acceptance"
source = workspace / "app"
shutil.copytree(example, source, dirs_exist_ok=True)
build = workspace / "build"
toolchain = Path(os.environ["VCPKG_ROOT"]) / "scripts/buildsystems/vcpkg.cmake"
subprocess.run(["cmake", "-S", str(source), "-B", str(build),
    "-DCMAKE_TOOLCHAIN_FILE=" + str(toolchain), "-DVCPKG_TARGET_TRIPLET=" + args.triplet,
    "-DCMAKE_BUILD_TYPE=Release"], check=True, timeout=1500)
for configuration in ("Debug", "Release"):
    subprocess.run(["cmake", "-S", str(source), "-B", str(build),
        "-DCMAKE_BUILD_TYPE=" + configuration], check=True, timeout=120)
    subprocess.run(["cmake", "--build", str(build), "--config", configuration,
        "--parallel", "2"], check=True, timeout=300)
    subprocess.run(["ctest", "--test-dir", str(build), "-C", configuration,
        "--output-on-failure"], check=True, timeout=30)
print("Published vcpkg registry: Debug/Release consumer passed for " + args.triplet)
