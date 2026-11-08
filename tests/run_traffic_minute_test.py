"""Run minute aggregation and CSV tests from a Visual Studio developer shell."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="traffic-minute-") as directory:
    work = Path(directory)
    exe = work / "test.exe"
    subprocess.run([
        "cl", "/nologo", "/std:c11", "/O2", "/W4", "/WX",
        "/D_CRT_SECURE_NO_WARNINGS", f"/I{root / 'main'}",
        str(root / "tests/test_traffic_minute.c"),
        str(root / "main/traffic_minute.c"), str(root / "main/traffic_csv.c"),
        f"/Fe{exe}", f"/Fo{work}\\",
    ], cwd=work, check=True)
    subprocess.run([str(exe)], cwd=work, check=True)
