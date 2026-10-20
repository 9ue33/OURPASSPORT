<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# 资源目录（Assets）

本目录集中存放可复用的资源（字库、图片、音乐等），按资源类型分子目录管理。每个资源放在其类型对应的子目录，并记录放置路径、命名方式、集成方式与来源/许可。二进制资源（字体、图片、音频）不属于纯 markdown 文档，请勿与文档混放。涉及版权/授权的资源需注明来源与许可。

## 字库（fonts）

可复用的字库文件与生成的字库源码放在 `fonts/`。

- 命名要能反映字族、字重、字级与格式。
- 记录来源、许可、字符范围、转换命令与目标放置路径。
- 添加字库前评估 Flash 与内部 RAM 影响；ESP32-C3 无 PSRAM。
- 不提交许可不允许分发的字库。

### 情侣终端子集字库

情侣心动终端通过两套未压缩的 LVGL 子集字库显示中文，二者由同一份码点清单生成。`main/couple_fonts.h` 负责声明，并把它们映射到小号／正文／标题／数字四个角色；仅当 `main/CMakeLists.txt` 定义 `COUPLE_USE_APP_FONT` 时才参与编译。

| 文件 | 符号 | 字级 | 位图 | 角色 |
| --- | --- | --- | --- | --- |
| [`fonts/app_font_16.c`](fonts/app_font_16.c) | `app_font_16` | 16 px，4 bpp | 52,940 B | `COUPLE_FONT_SMALL`、`COUPLE_FONT_BODY` |
| [`fonts/app_font_20.c`](fonts/app_font_20.c) | `app_font_20` | 20 px，4 bpp | 81,528 B | `COUPLE_FONT_TITLE`、`COUPLE_FONT_NUM` |
| [`fonts/ui_symbols.txt`](fonts/ui_symbols.txt) | — | — | — | 字符清单：512 个码点，每行一个 `U+XXXX` |

- 源字体：Noto Sans SC，`C:/Windows/Fonts/Noto Sans SC (TrueType).otf`（8,482,020 字节）。直接使用未作修改，许可为 SIL Open Font License 1.1，允许嵌入并再分发生成的子集。
- 转换工具：`lv_font_conv` 1.5.3，按 [LVGL 中文文本指南](../docs/development/engineering/lvgl-chinese-fonts.md) 的要求使用 `--format lvgl --no-compress --bpp 4`。
- 字符清单：由 `tools/ui_codepoints.py` 从 `main/` 下全部编译单元的字符串与字符字面量中提取（注释与文档不计入）。其中汉字 413 个、ASCII 89 个、其他 10 个。
- 分尺寸遵循指南要求，每个字级使用不同符号，因此两个尺寸必须一起重新生成，并与 `main/couple_fonts.h` 中的别名保持一致。

在仓库根目录、且 `lv_font_conv` 已在 `PATH` 中时，重新生成两个尺寸：

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

每个已提交文件都在自身的 `Opts:` 头注释中记录了生成它的确切命令；上面的清单写法与那里记录的合并 `--range` 写法等价。文案改动后刷新清单：

```bash
python3 tools/ui_codepoints.py assets/fonts/ui_symbols.txt \
  "main/*.c" "main/*.h"
```

重新生成后验证覆盖（负例必须缺席）：

```bash
python3 tools/check_font_glyphs.py assets/fonts/app_font_16.c \
  --symbols-file assets/fonts/ui_symbols.txt --range 0x20-0x7E --no-art
```

- 清单之外的码点会显示为占位方框，固件不会就此告警。网络下发的文本（例如对方昵称）不在本清单覆盖范围内。
- 不要 `#include` 生成的字库 `.c`；应把它登记为组件源文件，并用 `LV_FONT_DECLARE` 声明符号。

## 图片（images）

可复用的源图与生成的显示资产放在 `images/`。

| 文件 | 尺寸与格式 | 用途与来源 |
| --- | --- | --- |
| [`images/home.jpg`](images/home.jpg) | 3840 × 2160，JPEG | 嵌入中英文项目 README 的产品主图，突出 AI Passport 产品形象与开放、人人可创作的理念。 |
| [`images/readme-hardware-specs.png`](images/readme-hardware-specs.png) | 2172 × 724，PNG RGBA | 保留为可选技术参考图，不再用于首页主视觉。于 2026-09-17 使用内置图像生成工具为本仓库生成；已根据文档中的硬件能力契约核对图中的六项标签与参数。 |
| [`images/logo-wordmark.png`](images/logo-wordmark.png) | 1648 × 336，PNG RGBA | 从仓库原始 `images/logo.png` 中精确裁切并去除背景的黑色字标；用于中英文项目 README 的浅色主题。 |
| [`images/logo-wordmark-dark.png`](images/logo-wordmark-dark.png) | 1648 × 336，PNG RGBA | 提取字标的白色版本；README 使用 `<picture>` 在 GitHub 深色主题下显示。 |

- 使用描述性命名，并记录尺寸、像素格式、转换步骤与目标路径。
- 优先采用适合 240 × 320 RGB565 显示的格式，并纳入 Flash 与内部 RAM 考量。
- 许可允许时保留可编辑源文件，并记录来源与许可。
- 图片中不得包含设备二维码秘密、凭证或个人数据。

## 音乐与音效（music）

可复用的音乐与音效源码放在 `music/`。

- 记录来源、许可、采样率、位深、声道、转换命令与目标路径。
- 与当前 BSP 音频路径匹配时优先采用 16 kHz、16 位单声道 PCM。
- 嵌入音频前评估 Flash 与内部 RAM 成本；长录音应流式或分块。
- 无再分发许可不提交媒体文件。
