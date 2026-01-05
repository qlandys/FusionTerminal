# Ladder troubleshooting (Ghost)

This is a quick “when something goes weird” checklist for the ladder/prints alignment and tick math.

## Symptom: prints / ladder shifted by exactly 1 row (“+1 tick”)

### Typical cause

The ladder window is tick-based, but the column is hosted inside a `QScrollArea`.
If any widget height changes (position panel appearing/disappearing, geometry recalculation),
the `QScrollArea` can keep a non-zero pixel scroll offset. Because the ladder rows are uniform,
this often shows up as exactly **one row** drift.

### Permanent fix (current behavior)

- Keep a constant bottom padding:
  - `DomWidget` always reserves 26px (`infoAreaHeight`) even when position UI is hidden.
  - `PrintsWidget` mirrors the same padding.
- Prevent pixel scrolling drift:
  - `MainWindow::pullSnapshotForColumn` forces the `QScrollArea` scrollbars to stay at `0`.

If you remove either part, the 1-row drift can come back.

## Symptom: bestBid/bestAsk highlighting lags or flips unexpectedly

### Typical causes

- Tick size mismatch (wrong `tickSize` source from exchange metadata).
- Float rounding in price→tick conversion.

### Fix guidance

- Prefer `PRICE_FILTER.tickSize` for MEXC spot.
- Use scaled-integer quantization (`quantizeTickFromPrice`) for every price→tick path:
  - REST snapshot
  - WS depth updates
  - WS trades
  - GUI “center to spread” calculations

## Symptom: “works when position exists, breaks when flat”

That’s a strong signal the bug is geometry-related (position panel toggles height).
Check that the bottom padding is still constant and that scrollbars are kept at `0`.
