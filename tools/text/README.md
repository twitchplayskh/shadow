# CRISP TEXT tools

- `extract_sheets.py <game folder> <out>`: decodes the lettering sheets (subtitle font, clock, place names,
  "Time passed") to PNG.
- `crisp_subtitle_font.py <fonte_all.png> <77.a8> [Medium|Regular] [preview.png]`: the subtitle font redrawn from
  Inter at 8x, every letter on its original baseline and left edge; accented letters and button symbols traced.
- `crisp_timefont.py <timefont.png> <488.a8> [preview.png]`: the clock digits redrawn from Liberation Serif / Sans.

The `.a8` files go to `SoDFix\crisp\`. Every other text image is redrawn by the renderer as it loads
(`dxvk/src/d3d9/d3d9_sod_crisp.cpp`). Font licences: `licences/`.
