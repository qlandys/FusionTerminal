## DOM (Depth Of Market) Overview – Ghost (Qt native)

This note captures the current visual/behavioural contract for the native `DomWidget` and the ladder column around it. Use it as a reference if anything regresses or needs to be rebuilt.

### Layout & structure

- Each ladder column is a `QFrame` with:
  - Header row (symbol, levels spinner, zoom buttons, float/close).
  - Status label.
  - Horizontal splitter (`QSplitter` named `DomPrintsSplitter`) that contains the prints panel on the left and the DOM panel on the right; the splitter handle is 3 px and matches the global resize handle palette.
  - The entire prints+DOM block sits in a `QScrollArea` with a custom `DomScrollBar`.
- The outer column still exposes the original resize handle on the far right for column-wide resizing/floating.

### Row sizing

- Default tick height (`DomWidget::m_rowHeight`) is **12 px** (`DomWidget.h`).
- Zoom buttons call `DomWidget::setRowHeight()` and the prints widget mirrors the same height so ladder rows and prints rows always align.
- Row height is clamped to 10 – 40 px and is only stored in memory (no persistent setting).

### Rendering details (see `DomWidget::paintEvent`)

1. **Grid & price column**
   - Rightmost 48+ px reserved for price text (width = max(48, text width + 4)).
   - Grid lines drawn at column boundaries plus horizontal row separators.
   - Price column background inherits the bid/ask color at 40 alpha when there is liquidity.
   - Price text uses `formatPriceForDisplay` to compress leading zeros and display `(n)` notation for sub-pip prices, matching the screenshot requirement.

2. **Book area shading**
   - Area left of the price column fills the entire width in the selected bid/ask color with alpha 60 when depth exists; there is no gradient or double pass anymore.
   - Colors come from `DomStyle` (`DomWidget.h`). Ask side defaults to soft red, bid side to green.

3. **Per-level volume tags & highlights**
   - For each row we take the dominant side (larger of bid/ask qty) and compute notional `qty * price` (always USDT because prices are quoted in USDT).
   - The notional value is formatted via `formatQty()` (`?.?K`, `?.?M`, etc.) and rendered left-aligned in a bold font.
   - Volume highlight rules (Settings -> Trading -> DOM) define up to five thresholds and colors. Rules are evaluated in ascending order:
     - If notional is below the first threshold there is no highlight.
     - Between two thresholds we fill the central column proportionally (0-100%) using the color of the lower threshold. Once past the highest threshold the entire column width is filled.
     - Text color swaps to dark/light automatically so it stays readable on any swatch.
     - Colors are edited through the palette picker in Settings; the table shows a swatch preview next to each value.

4. **Interaction plumbing**
   - `rowClicked` now emits (button, row, price, bidQty, askQty); `rowHovered` keeps (row, price, bidQty, askQty).
   - Mouse hover updates `m_hoverRow` to drive tooltips/order panels elsewhere.
   - Mouse wheel/drag handled by the surrounding `QScrollArea`.

### Styling highlights

- Column style sheet ensures:
  - `DomPrintsSplitter::handle` uses the same grey palette as the master column resize handle (`#2b2b2b` hover `#3a3a3a`).
  - Splitter spacing between prints & DOM is zero, so there is no dead gap.
  - Prints panel background is managed by `PrintsWidget` (very dark grey) and uses matching horizontal grid lines to stay aligned.

### Saved state / settings

- The only ladder-related user preferences that persist today:
  - Center-to-spread hotkey (key/modifiers, plus "center all ladders" bool) in `ghost_terminal.ini`.
  - Clock offset.
- Book-column width is *not* user adjustable anymore; the entire shading logic assumes the book fills everything to the left of the price column.

### If something breaks

1. **Rendering regression**
   - Check `DomWidget::paintEvent` for the order of operations: grid → shading → text.
   - Ensure `formatQty` and `formatPriceForDisplay` are untouched.
2. **Splitter/prints sizing**
   - Verify `DomPrintsSplitter` creation in `MainWindow::createDomColumn` (lines around 1515).
   - The splitter must be inside the scroll area to keep scroll and zoom aligned.
3. **Row height issues**
   - Defaults live in `DomWidget.h`.
   - Buttons call `DomWidget::setRowHeight`; look at `createDomColumn` for zoom button lambdas.
4. **Scroll/height alignment**
   - `DomWidget` always reserves a fixed 26px bottom padding (position bar area), even when the position UI is hidden.
   - `PrintsWidget` mirrors the same padding so total heights match; this prevents `QScrollArea` drift and the classic “1 tick / 1 row” misalignment between ladder and prints.
   - `MainWindow::pullSnapshotForColumn` should keep the `QScrollArea` scrollbars at `0` (pixel scrolling must not accumulate).
5. **Colors**
   - All palette tweaks are centralized in `DomStyle` or the stylesheet in `MainWindow::buildUi()`.

Keep this document updated when you change the ladder visuals so we always have a “known good” description to reference.
