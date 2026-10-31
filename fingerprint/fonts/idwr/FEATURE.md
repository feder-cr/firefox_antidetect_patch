# fingerprint/fonts/idwr

Category: build
Prefs: none read directly
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `gfx/moz.build` (before TEST_DIRS, already-hook): Builds gfx/idwr (invisible_DWriteCore's DirectWrite rasterizer) into libxul, on a line of its own outside Mozilla's DIRS list.

## Our own files

- `gfx/idwr/IdwrHost.cpp`
- `gfx/idwr/moz.build`
- `gfx/idwr/moz.yaml`
- `gfx/idwr/sources.mozbuild`
- `gfx/idwr/src/cff/cff_math.c`
- `gfx/idwr/src/cff/cff_port.h`
- `gfx/idwr/src/cff/cscan.c`
- `gfx/idwr/src/cff/cscan.h`
- `gfx/idwr/src/cff/dropout.c`
- `gfx/idwr/src/cff/front.c`
- `gfx/idwr/src/cff/front.h`
- `gfx/idwr/src/cff/glue.c`
- `gfx/idwr/src/cff/hint.c`
- `gfx/idwr/src/cff/interp.c`
- `gfx/idwr/src/cff/interp.h`
- `gfx/idwr/src/cff/interp_procs.c`
- `gfx/idwr/src/cff/parse.c`
- `gfx/idwr/src/cff/path.c`
- `gfx/idwr/src/cff/qreduce.c`
- `gfx/idwr/src/cff/setup.c`
- `gfx/idwr/src/idwr/cffraster.c`
- `gfx/idwr/src/idwr/cffraster.h`
- `gfx/idwr/src/idwr/color.c`
- `gfx/idwr/src/idwr/face.c`
- `gfx/idwr/src/idwr/face.h`
- `gfx/idwr/src/idwr/fallback.c`
- `gfx/idwr/src/idwr/fallback_nls.h`
- `gfx/idwr/src/idwr/fallback_tables.h`
- `gfx/idwr/src/idwr/font.c`
- `gfx/idwr/src/idwr/fullwidth_tables.h`
- `gfx/idwr/src/idwr/gdimetrics.c`
- `gfx/idwr/src/idwr/glyphrun.c`
- `gfx/idwr/src/idwr/idwr.h`
- `gfx/idwr/src/idwr/idwr_fallback.h`
- `gfx/idwr/src/idwr/idwr_internal.h`
- `gfx/idwr/src/idwr/idwr_math.c`
- `gfx/idwr/src/idwr/idwr_math.h`
- `gfx/idwr/src/idwr/idwr_math_tables.h`
- `gfx/idwr/src/idwr/lcid_table.h`
- `gfx/idwr/src/idwr/metrics.c`
- `gfx/idwr/src/idwr/mode.c`
- `gfx/idwr/src/idwr/nameweight.c`
- `gfx/idwr/src/idwr/outline.c`
- `gfx/idwr/src/idwr/sbit.c`
- `gfx/idwr/src/idwr/texture.c`
- `gfx/idwr/src/idwr/ttraster.c`
- `gfx/idwr/src/idwr/ttraster.h`
- `gfx/idwr/src/idwr/variation.c`
- `gfx/idwr/src/truetype/fatal.h`
- `gfx/idwr/src/truetype/fp_exact.h`
- `gfx/idwr/src/truetype/glyphrun_helpers.c`
- `gfx/idwr/src/truetype/glyphrun_layout.c`
- `gfx/idwr/src/truetype/glyphrun_tables.h`
- `gfx/idwr/src/truetype/glyphrun_texture.c`
- `gfx/idwr/src/truetype/interp.c`
- `gfx/idwr/src/truetype/native.c`
- `gfx/idwr/src/truetype/native.h`
- `gfx/idwr/src/truetype/orig.c`
- `gfx/idwr/src/truetype/orig.h`
- `gfx/idwr/src/truetype/orig_ports.h`
- `gfx/idwr/src/truetype/outline_driver.c`
- `gfx/idwr/src/truetype/outline_driver.h`
- `gfx/idwr/src/truetype/outline_stream.c`
- `gfx/idwr/src/truetype/portable.h`
- `gfx/idwr/src/truetype/scaler_fs.c`
- `gfx/idwr/src/truetype/scaler_fsg.c`
- `gfx/idwr/src/truetype/scaler_init.c`
- `gfx/idwr/src/truetype/scaler_orient.c`
- `gfx/idwr/src/truetype/scaler_sbit.c`
- `gfx/idwr/src/truetype/scaler_scl.c`
- `gfx/idwr/src/truetype/scaler_sfac.c`
- `gfx/idwr/src/truetype/scan_contour.c`
- `gfx/idwr/src/truetype/scan_fill.c`
- `gfx/idwr/src/truetype/scan_glyph.c`
- `gfx/idwr/src/truetype/scan_gray.c`
- `gfx/idwr/src/truetype/scan_line.c`
- `gfx/idwr/src/truetype/scan_measure.c`
- `gfx/idwr/src/truetype/scan_setup.c`
- `gfx/idwr/src/truetype/scan_spline.c`
- `gfx/idwr/src/truetype/tt_state.h`
