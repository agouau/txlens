"""Run IP detail parsing tests in a Visual Studio x64 Developer Command Prompt."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cl", default="cl")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    cjson = root / "managed_components/espressif__cjson/cJSON"
    if not (cjson / "cJSON.c").is_file():
        raise SystemExit("Run idf.py reconfigure first to download cJSON.")
    with tempfile.TemporaryDirectory(prefix="ip-info-") as directory:
        temp = Path(directory)
        (temp / "esp_err.h").write_text("#pragma once\ntypedef int esp_err_t;\n", encoding="utf-8")
        subprocess.run([
            args.cl, "/nologo", "/c", "/O2", "/W3", "/D_CRT_SECURE_NO_WARNINGS",
            str(cjson / "cJSON.c"), f"/Fo{temp / 'cjson.obj'}",
        ], check=True)
        subprocess.run([
            args.cl, "/nologo", "/std:c11", "/O2", "/W4", "/WX", "/D_CRT_SECURE_NO_WARNINGS",
            f"/I{temp}", f"/I{root / 'main'}", f"/I{cjson}",
            str(root / "tests/test_ip_info.c"), str(root / "main/ip_info_parse.c"),
            str(temp / "cjson.obj"), f"/Fo{temp}\\", f"/Fe{temp / 'test.exe'}",
            "/link", "/INCREMENTAL:NO",
        ], check=True)
        subprocess.run([str(temp / "test.exe")], check=True)


if __name__ == "__main__":
    main()
