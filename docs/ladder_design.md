# Ghost ladder behavior (backend + Qt GUI)

This document describes the current ladder (order book engine + rendering) behavior.
If you change backend or GUI, keep these invariants unless you intentionally want
different behavior.

## Price / tick model

- All internal book keys are integer ticks (`int64_t`).
- Tick size source:
  - **MEXC spot**: REST `exchangeInfo` → `filters[]` → `filterType == "PRICE_FILTER"` → `tickSize`.
    - Do not rely on `quotePrecision` as a primary source (it can be misleading).
  - **MEXC futures**: contract detail → `priceUnit` (fallback: `priceScale → 10^-priceScale`).
- Price ↔ tick conversion:
  - Must use a *scaled-integer* quantization path to avoid float rounding drift.
  - Backend: `quantizeTickFromPrice()` and `tickFromPrice()` in `backend/src/main.cpp`.
  - GUI: prefer `tick` carried in protocol over recomputing from `price`.

## OrderBook model

- Backend book storage:
  - `std::map<Tick, double> bids_`, `asks_` (quantity in base asset).
- Best bid / ask:
  - `bestBidPrice = max(bids_.keys) * tickSize`
  - `bestAskPrice = min(asks_.keys) * tickSize`

## Ladder window (no jumping)

- `OrderBook` maintains a persistent ladder center (ticks):
  - `centerTick_` and `hasCenter_`
  - Mid-tick per update:
    - both sides: `(bestBidTick + bestAskTick) / 2`
    - else: best tick of the existing side
- `ladder(levelsPerSide)` produces a stable window:
  - window: `[centerTick_ - padding, centerTick_ + padding]`, where `padding = levelsPerSide`
  - inner band: `[windowMin + padding/4, windowMax - padding/4]`
  - center shifts only when mid-tick leaves the inner band

## JSON protocol (backend → GUI)

Backend emits one JSON object per line on stdout.

### Ladder snapshot (`type: "ladder"`)

- `symbol`, `timestamp`, `bestBid`, `bestAsk`, `tickSize`
- window ticks: `windowMinTick`, `windowMaxTick`, `centerTick`
- `rows`: array of levels, top→bottom, each item:
  - `tick` (int64)
  - `price` (= `tick * tickSize`, numeric, convenience)
  - `bid`, `ask` (quantities in base asset)

### Ladder delta (`type: "ladder_delta"`)

- Same metadata as full snapshot
- `updates`: array of row updates (each includes `tick`)
- `removals`: array of removed ticks

### Trades (`type: "trade"`)

- `tick` (int64) when available, and snapped `price` consistent with that tick.
- `qty` is base quantity; GUI can compute quote notional (`qty * price`) for display.

## Backend depth pipeline

All of this lives in `backend/src/main.cpp`.

- On startup:
  - Resolve tick size from exchange metadata.
  - `OrderBook::setTickSize(tickSize)`.
- Snapshot (REST):
  - Spot: `/api/v3/depth`.
  - Futures: contract depth endpoint.
  - Convert every price string to `Tick` via `tickFromPrice(price, tickSize)`.
- WebSocket depth:
  - Decode protobuf depth updates (price/qty strings).
  - Convert using the same `tickFromPrice()` logic and apply to `OrderBook`.
  - Emit ladder at throttle (`Config::throttle`).
- Trades:
  - Quantize trades using `quantizeTickFromPrice` so trade ticks match depth ticks.

## Qt GUI model

- `gui_native/LadderClient.cpp` runs the backend via `QProcess` and keeps a tick-keyed map.
- Display compression (N ticks per row):
  - `LadderClient::buildSnapshot()` bucketizes ticks and builds `DomSnapshot.levels` including `DomLevel.tick`.
- Rendering:
  - `DomWidget` renders the snapshot via QML model (`DomLevelsModel`).
  - `PrintsWidget` aligns prints/clusters by `rowTicks` derived from `DomSnapshot.levels[*].tick`.

## Alignment invariants (avoid “1 tick drift”)

- The ladder window is tick-based; the surrounding `QScrollArea` must not introduce pixel scrolling drift.
- Keep a constant 26px bottom padding in `DomWidget` and mirror it in `PrintsWidget`.
- `MainWindow::pullSnapshotForColumn` should keep the `QScrollArea` scrollbars at `0`.
