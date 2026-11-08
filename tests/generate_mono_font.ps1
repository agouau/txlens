# Generate the bundled 12 px ASCII font from the OFL-licensed Cascadia Mono.
param([string]$FontPath = 'C:/Windows/Fonts/CascadiaMono.ttf')
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$collection = New-Object System.Drawing.Text.PrivateFontCollection
$collection.AddFontFile($FontPath)
$font = New-Object System.Drawing.Font($collection.Families[0], 12, [System.Drawing.FontStyle]::Regular, [System.Drawing.GraphicsUnit]::Pixel)
$bitmap = New-Object System.Drawing.Bitmap(8, 16)
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
$graphics.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit
$format = [System.Drawing.StringFormat]::GenericTypographic.Clone()
$format.FormatFlags = $format.FormatFlags -bor [System.Drawing.StringFormatFlags]::NoWrap
$bytes = [System.Collections.Generic.List[string]]::new()
$glyphs = [System.Collections.Generic.List[string]]::new()
$glyphs.Add('    {0},')
foreach ($code in 32..126) {
    $graphics.Clear([System.Drawing.Color]::Black)
    $graphics.DrawString([string][char]$code, $font, [System.Drawing.Brushes]::White, 0, 0, $format)
    $offset = $bytes.Count
    foreach ($y in 0..15) {
        foreach ($x in 0..3) {
            $hi = [int][Math]::Round($bitmap.GetPixel($x * 2, $y).R / 17.0)
            $lo = [int][Math]::Round($bitmap.GetPixel($x * 2 + 1, $y).R / 17.0)
            $bytes.Add(('0x{0:x2}' -f (($hi -shl 4) -bor $lo)))
        }
    }
    $glyphs.Add("    {.bitmap_index = $offset, .adv_w = 128, .box_w = 8, .box_h = 16}, // U+$('{0:X4}' -f $code)")
}
$rows = for ($i = 0; $i -lt $bytes.Count; $i += 16) {
    '    ' + ($bytes.GetRange($i, [Math]::Min(16, $bytes.Count - $i)) -join ', ') + ','
}
$source = @"
// App Mono 12: 8 px fixed advance, rasterized from Cascadia Mono.
// Copyright (c) 2019 - Present, Microsoft Corporation.
// Licensed under SIL OFL 1.1; see OFL.txt. Regenerate with tests/generate_mono_font.ps1.
#include "lvgl.h"

static const uint8_t bitmap[] = {
$($rows -join "`n")
};
static const lv_font_fmt_txt_glyph_dsc_t glyphs[] = {
$($glyphs -join "`n")
};
static const lv_font_fmt_txt_cmap_t cmaps[] = {
    {.range_start = 32, .range_length = 95, .glyph_id_start = 1,
     .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY},
};
static const lv_font_fmt_txt_dsc_t data = {
    .glyph_bitmap = bitmap, .glyph_dsc = glyphs, .cmaps = cmaps,
    .cmap_num = 1, .bpp = 4,
};
const lv_font_t app_font_mono_12 = {
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,
    .line_height = 16, .base_line = 0, .dsc = &data,
};
"@
[System.IO.File]::WriteAllText((Join-Path $PSScriptRoot '../main/fonts/app_font_mono_12.c'), $source, [System.Text.UTF8Encoding]::new($false))
$format.Dispose()
$graphics.Dispose()
$bitmap.Dispose()
$font.Dispose()
$collection.Dispose()
