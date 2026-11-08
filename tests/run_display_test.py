"""Headless LVGL UI regression test. Run in a VS x64 Developer Command Prompt.

Requires CMake/Ninja on PATH and downloaded managed components (idf.py reconfigure).
Outputs screenshots under the ignored build-ui-host directory.
"""
import argparse
import json
from pathlib import Path
import re
import struct
import subprocess
import zlib


def png(path, width, height):
    raw = path.read_bytes()
    rgb = bytearray(len(raw))
    rgb[0::3], rgb[1::3], rgb[2::3] = raw[2::3], raw[1::3], raw[0::3]
    scanlines = b"".join(b"\0" + rgb[y * width * 3:(y + 1) * width * 3] for y in range(height))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    path.with_suffix(".png").write_bytes(
        b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(scanlines)) + chunk(b"IEND", b""))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--ninja", default="ninja")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    work = root / "build-ui-host"
    work.mkdir(exist_ok=True)
    config = (root / "main/app_config.h").read_text(encoding="utf-8")
    width, height = [int(re.search(rf"^#define\s+UI_{name}\s+(\d+)\b", config, re.M)[1])
                     for name in ("WIDTH", "HEIGHT")]
    (work / "esp_err.h").write_text("#pragma once\ntypedef int esp_err_t;\n#define ESP_OK 0\n", encoding="utf-8")
    (work / "esp_netif.h").write_text("#pragma once\ntypedef struct netif esp_netif_t;\n", encoding="utf-8")
    display = (root / "main/display.c").read_text(encoding="utf-8")
    declarations = display[display.index("// The UI is owned"):display.index("static uint32_t tick_ms(void)")]
    ui = display[display.index("// Shared logarithmic"):display.index("static void init_ui(i2c_master_bus_handle_t bus)\n{")]
    process = display[display.index("void display_process(void)"):]
    (work / "display_ui.inc").write_text(display[display.index("static void configure_panel_rotation("):display.index("void display_init(void)")] + declarations + display[display.index("static void flush("):display.index("// Shared logarithmic")] + ui + process, encoding="utf-8")
    # Run the installed RGB driver's actual pixel-copy macro in the host mock.
    project = json.loads((root / "build/project_description.json").read_text(encoding="utf-8"))
    rotation = (Path(project["idf_path"]) / "components/esp_lcd/rgb/rgb_lcd_rotation_sw.h").read_text(encoding="utf-8")
    rotation = rotation.replace('__attribute__((always_inline))', '')
    rotation = rotation.replace('#include "esp_bit_defs.h"', '#define BIT(n) (1U << (n))')
    (work / "rgb_rotation.inc").write_text(rotation, encoding="utf-8")
    cmake = f'''cmake_minimum_required(VERSION 3.22)
project(display_test C)
set(CMAKE_C_STANDARD 11)
add_compile_definitions(LV_CONF_SKIP=1 LV_USE_STDLIB_MALLOC=1 LV_DRAW_SW_COMPLEX=1 LV_FONT_MONTSERRAT_12=1 LV_FONT_MONTSERRAT_24=1 LV_USE_QRCODE=1 LV_COLOR_DEPTH=24 _CRT_SECURE_NO_WARNINGS)
file(GLOB_RECURSE lv_sources "{root.as_posix()}/managed_components/lvgl__lvgl/src/*.c")
add_library(lvgl_host STATIC ${{lv_sources}})
target_include_directories(lvgl_host PUBLIC "{root.as_posix()}/managed_components/lvgl__lvgl")
foreach(rotation IN ITEMS 0 1)
add_executable(display_test_${{rotation}} "{root.as_posix()}/tests/test_display.c" "{root.as_posix()}/main/ip_info_parse.c" "{root.as_posix()}/main/fonts/app_font_mono_12.c" "{root.as_posix()}/managed_components/espressif__cjson/cJSON/cJSON.c")
target_compile_definitions(display_test_${{rotation}} PRIVATE TEST_UI_ROTATION=${{rotation}})
target_include_directories(display_test_${{rotation}} PRIVATE "{work.as_posix()}" "{root.as_posix()}/main" "{root.as_posix()}/managed_components/espressif__cjson/cJSON")
target_link_libraries(display_test_${{rotation}} PRIVATE lvgl_host)
endforeach()
'''
    (work / "CMakeLists.txt").write_text(cmake, encoding="utf-8")
    subprocess.run([args.cmake, "-S", str(work), "-B", str(work / "out"), "-G", "Ninja",
                    f"-DCMAKE_MAKE_PROGRAM={args.ninja}", "-DCMAKE_BUILD_TYPE=Release"], check=True)
    subprocess.run([args.cmake, "--build", str(work / "out"), "--parallel", "4"], check=True)
    for rotation in (0, 1):
        screenshots = work / f"rotation-{rotation}"
        screenshots.mkdir(exist_ok=True)
        subprocess.run([str(work / f"out/display_test_{rotation}.exe")], cwd=screenshots, check=True)
        for name in ("empty", "first_boot", "traffic", "details", "dense", "choices", "settings", "status"):
            png(screenshots / f"{name}.bgr", width, height)
    print(f"Screenshots: {work}")


if __name__ == "__main__":
    main()
