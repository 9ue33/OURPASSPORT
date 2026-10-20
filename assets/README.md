<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Assets

This directory stores reusable fonts, images, music, and sound effects, organized by asset type.

Keep each asset in the matching subdirectory and document its destination, naming, integration method, and source/license. Do not mix binary assets with Markdown documentation.

## Fonts

Store reusable font files and generated font sources in `fonts/`.

- Use descriptive names that include the family, weight, size, and format when relevant.
- Document the source, license, character range, conversion command, and expected destination.
- Check Flash and internal-RAM impact before adding a font; the ESP32-C3 has no PSRAM.
- Do not commit fonts whose license does not permit redistribution.

### Couple terminal subset fonts

The couple heart-link terminal draws Chinese through two uncompressed LVGL subset fonts generated from one code point list. `main/couple_fonts.h` declares them and aliases them onto the small/body/title/number roles; they are compiled only when `main/CMakeLists.txt` defines `COUPLE_USE_APP_FONT`.

| File | Symbol | Size | Bitmap | Role |
| --- | --- | --- | --- | --- |
| [`fonts/app_font_16.c`](fonts/app_font_16.c) | `app_font_16` | 16 px, 4 bpp | 52,940 B | `COUPLE_FONT_SMALL`, `COUPLE_FONT_BODY` |
| [`fonts/app_font_20.c`](fonts/app_font_20.c) | `app_font_20` | 20 px, 4 bpp | 81,528 B | `COUPLE_FONT_TITLE`, `COUPLE_FONT_NUM` |
| [`fonts/ui_symbols.txt`](fonts/ui_symbols.txt) | — | — | — | Character inventory: 512 code points, one `U+XXXX` per line |

- Source font: Noto Sans SC, `C:/Windows/Fonts/Noto Sans SC (TrueType).otf` (8,482,020 bytes). It is used unmodified and is licensed under the SIL Open Font License 1.1, which permits embedding and redistributing the generated subsets.
- Converter: `lv_font_conv` 1.5.3, invoked with `--format lvgl --no-compress --bpp 4` as required by the [LVGL Chinese text guide](../docs/development/engineering/lvgl-chinese-fonts.md).
- Character inventory: extracted by `tools/ui_codepoints.py` from the string and character literals of every compiled unit under `main/` (comments and documentation are excluded). 413 Han, 89 ASCII, and 10 other code points.
- Size split follows the guide: one distinct symbol per size, so both sizes must be regenerated together and stay in sync with the aliases in `main/couple_fonts.h`.

Regenerate both sizes from the repository root, with `lv_font_conv` on `PATH`:

```bash
RANGE=$(awk '{print "0x" substr($0,3)}' assets/fonts/ui_symbols.txt | paste -sd, -)
for size in 16 20; do
  lv_font_conv \
    --font "C:/Windows/Fonts/Noto Sans SC (TrueType).otf" \
    --range "$RANGE,0x20-0x7E" \
    --size "$size" --bpp 4 --format lvgl --no-compress \
    --lv-font-name "app_font_$size" --lv-include lvgl.h \
    --output "assets/fonts/app_font_$size.c"
done
```

Each committed file embeds the exact command that produced it in its `Opts:` header comment; the inventory list above is equivalent to the merged `--range` form recorded there. Refreshing the inventory after a text change:

```bash
python3 tools/ui_codepoints.py assets/fonts/ui_symbols.txt \
  "main/*.c" "main/*.h"
```

Prove coverage after regenerating (the negative control must fail):

```bash
python3 tools/check_font_glyphs.py assets/fonts/app_font_16.c \
  --symbols-file assets/fonts/ui_symbols.txt --range 0x20-0x7E --no-art
```

- A code point outside the inventory renders as a placeholder box; the firmware does not warn about it. Network-supplied text such as a partner name is not covered by this inventory.
- Do not `#include` a generated font `.c`; register it as a component source and declare the symbol with `LV_FONT_DECLARE`.

## Images

Store reusable source images and generated display assets in `images/`.

| File | Dimensions and format | Use and source |
| --- | --- | --- |
| [`images/home.jpg`](images/home.jpg) | 3840 × 2160, JPEG | Product hero image embedded in both project README files to foreground AI Passport and its open, maker-oriented identity. |
| [`images/readme-hardware-specs.png`](images/readme-hardware-specs.png) | 2172 × 724, PNG RGBA | Optional technical infographic retained as a reference asset; it is no longer used as the homepage hero. Generated for this repository with the built-in image generation tool on 2026-09-17; the six labels and values were checked against the documented hardware contract. |
| [`images/logo-wordmark.png`](images/logo-wordmark.png) | 1648 × 336, PNG RGBA | Transparent black wordmark extracted from the repository's original `images/logo.png`; embedded in both project README files for light backgrounds. |
| [`images/logo-wordmark-dark.png`](images/logo-wordmark-dark.png) | 1648 × 336, PNG RGBA | White version of the extracted wordmark, used by the README `<picture>` element when GitHub is in dark mode. |

- Use descriptive names and document dimensions, pixel format, conversion steps, and destination.
- Prefer formats suitable for the 240 × 320 RGB565 display and account for Flash and internal RAM.
- Preserve editable sources where licensing permits, and record the source and license.
- Never commit device QR secrets, credentials, or personal data in images.

## Music and sound effects

Store reusable music and sound-effect sources in `music/`.

- Document the source, license, sample rate, bit depth, channels, conversion command, and destination.
- Prefer 16 kHz, 16-bit mono PCM when it matches the current BSP audio path.
- Check Flash and internal-RAM cost before embedding audio; stream or chunk long recordings.
- Do not commit media without redistribution permission.
