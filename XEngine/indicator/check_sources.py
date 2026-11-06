"""Static checks only: no compiler, linker, strategy or test process is invoked."""
from pathlib import Path
import hashlib
import json
import re
import xml.etree.ElementTree as ET

root = Path(__file__).resolve().parent
manifest = json.loads((root / "talib-manifest.json").read_text(encoding="utf-8"))
vendor = root / "talib"
errors = []
for name, expected in manifest["files"].items():
    path = vendor / name
    if not path.is_file():
        errors.append(f"Missing upstream file: {name}")
    elif hashlib.sha256(path.read_bytes()).hexdigest() != expected:
        errors.append(f"Modified upstream file: {name}")
actual_files = {path.relative_to(vendor).as_posix() for path in vendor.rglob("*") if path.is_file()}
if actual_files != set(manifest["files"]):
    errors.append("Vendored file set does not match the manifest")

cmake = (root / "IndicatorSources.cmake").read_text(encoding="utf-8")
cmake_sources = re.findall(r'"\$\{indicatorVendorRoot\}/([^"\n]+\.c)"', cmake)
namespace = {"m": "http://schemas.microsoft.com/developer/msbuild/2003"}
props = ET.parse(root / "IndicatorSources.props").getroot()
vs_sources = []
for item in props.findall(".//m:ClCompile[@Include]", namespace):
    name = item.attrib["Include"].replace("\\", "/")
    if name.startswith("indicator/talib/"):
        vs_sources.append(name.removeprefix("indicator/talib/"))
        if item.findtext("m:CompileAs", namespaces=namespace) != "CompileAsC":
            errors.append(f"Vendor source not explicitly compiled as C: {name}")
        options = item.findtext("m:AdditionalOptions", namespaces=namespace) or ""
        if "-std=c11" not in options or "-ffp-contract=off" not in options:
            errors.append(f"Missing vendor-only C options: {name}")
for label, sources in [("CMake", cmake_sources), ("VS", vs_sources)]:
    if len(sources) != len(set(sources)) or set(sources) != set(manifest["sources"]):
        errors.append(f"{label} source list differs from the pinned manifest")

upstream = (vendor / "CMakeLists.txt").read_text(encoding="utf-8")
upstream_sources = []
for variable in ["LIB_SOURCES", "COMMON_SOURCES", "IDL_SOURCES"]:
    body = re.search(r"set\(" + variable + r"\s+(.*?)\n\)", upstream, re.S).group(1)
    upstream_sources.extend(re.findall(r'"\$\{CMAKE_CURRENT_SOURCE_DIR\}/([^"\n]+\.c)"', body))
if set(upstream_sources) != set(manifest["sources"]):
    errors.append("Pinned source list differs from the upstream library source lists")
include_dirs = [vendor / p for p in ["include", "src/ta_common", "src/ta_func", "src/ta_abstract", "src/ta_abstract/frames"]]
for name in manifest["files"]:
    path = vendor / name
    if path.suffix not in [".c", ".h"]:
        continue
    text = path.read_text(encoding="utf-8")
    if path.suffix == ".c" and re.search(r"^\s*(?:int|void)\s+main\s*\(", text, re.M):
        errors.append(f"Program entry point compiled as library source: {name}")
    for header in re.findall(r'^\s*#\s*include\s*"([^"\n]+)"', text, re.M):
        if not any((directory / header).is_file() for directory in [path.parent, *include_dirs]):
            errors.append(f"Missing local include {header} from {name}")

project = root.parent
ET.parse(project / "XEngine.vcxproj.filters")
vcx = ET.parse(project / "XEngine.vcxproj").getroot()
if not any(item.attrib.get("Project") == "indicator\\IndicatorSources.props" for item in vcx.findall("m:Import", namespace)):
    errors.append("VS project does not import IndicatorSources.props")
project_cmake = (project / "CMakeLists.txt").read_text(encoding="utf-8")
if "project(XEngine LANGUAGES C CXX)" not in project_cmake or "${indicatorVendorSources}" not in project_cmake:
    errors.append("CMake project does not enable or include the indicator C sources")
if errors:
    raise SystemExit("\n".join(errors))
print(f"Static source checks passed: {len(manifest['files'])} unchanged upstream files, {len(cmake_sources)} identical CMake/VS C sources, headers and project XML valid.")
