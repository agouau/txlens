"""Run LRU and DNS wire parsing tests in an x64 VS Developer Command Prompt."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--cl", default="cl")
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="ip-cache-") as directory:
    temp = Path(directory)
    (temp / "esp_err.h").write_text("#pragma once\ntypedef int esp_err_t;\n")
    subprocess.run([
        args.cl, "/nologo", "/std:c11", "/O2", "/W4", "/WX", "/D_CRT_SECURE_NO_WARNINGS",
        f"/I{temp}", f"/I{root / 'main'}",
        str(root / "tests/test_ip_cache.c"), str(root / "main/ip_cache.c"),
        str(root / "main/dns_observe.c"), f"/Fo{temp}\\", f"/Fe{temp / 'test.exe'}",
        "/link", "/INCREMENTAL:NO",
    ], check=True)
    subprocess.run([str(temp / "test.exe")], check=True)
