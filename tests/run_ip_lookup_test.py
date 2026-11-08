"""Run real detail lookup logic with mocked HTTPS, in a VS x64 prompt."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
cjson = root / "managed_components/espressif__cjson/cJSON"
with tempfile.TemporaryDirectory(prefix="ip-lookup-") as directory:
    work = Path(directory)
    (work / "esp_err.h").write_text("#pragma once\ntypedef int esp_err_t;\n")
    source = (root / "main/ip_info.c").read_text(encoding="utf-8")
    helpers = source[source.index("static bool current("):source.index("static void log_response(")]
    lookup = source[source.index("static void lookup("):source.index("esp_err_t ip_info_init(")]
    visit = source[source.index("void ip_info_visit("):source.index("bool ip_info_name(")]
    (work / "lookup.inc").write_text(helpers + visit + lookup, encoding="utf-8")
    subprocess.run(["cl", "/nologo", "/c", "/O2", "/W3", "/D_CRT_SECURE_NO_WARNINGS",
                    str(cjson / "cJSON.c"), f"/Fo{work / 'cjson.obj'}"], check=True)
    exe = work / "test.exe"
    subprocess.run(["cl", "/nologo", "/std:c11", "/W4", "/WX", "/D_CRT_SECURE_NO_WARNINGS",
                    f"/I{work}", f"/I{root / 'main'}", f"/I{cjson}",
                    str(root / "tests/test_ip_lookup.c"), str(root / "main/ip_info_parse.c"),
                    str(root / "main/ip_cache.c"), str(work / "cjson.obj"),
                    f"/Fo{work}\\", f"/Fe{exe}"], check=True)
    subprocess.run([str(exe)], check=True)
