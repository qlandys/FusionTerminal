#pragma once

#include <QMainWindow>
#include "DomTypes.h"
#include "DomWidget.h"
#include "SettingsWindow.h"
#include "TradeManager.h"

#include <QList>
#include <QVector>
#include <QStringList>
#include <QRect>
#include <QPoint>
#include <QTimer>
#include <QNetworkAccessManager>
#include <QSet>
#include <QHash>
#include <QPointer>
#include <QByteArray>
#include <QUrl>
#include <array>

class QLabel;
class QTabBar;
class QFrame;
class QPropertyAnimation;
class QStackedWidget;
class QToolButton;
class QListWidget;
class QSplitter;
class QSoundEffect;
class QTimer;
class QWidget;
class QLineEdit;
class QCompleter;
class QPushButton;
class QIcon;
class QEvent;
class QKeyEvent;
class QScrollArea;
class QScrollBar;
class QDialog;
class DomWidget;
class LadderClient;
class PrintsWidget;
class PluginsWindow;
class ConnectionStore;
class ConnectionsWindow;
class TradesWindow;
class FinrezWindow;
class SymbolPickerDialog;
class QSplitter;
class QNetworkReply;
class QFile;

struct SavedColumn {
    QString symbol;
    int compression = 1;
    QString account;
    int leverage = 20;
};

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(const QString &backendPath,
                        const QString &symbol,
                        int levels,
                        QWidget *parent = nullptr);

    ~MainWindow() override;

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;
    void changeEvent(QEvent *event) override;
    void updateMaximizeIcon();
    void showEvent(QShowEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
#ifdef Q_OS_WIN
    bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;
#endif

private slots:
    void handleTabChanged(int index);
    void handleTabCloseRequested(int index);
    void handleNewTabRequested();
    void handleNewLadderRequested();
    void handleLadderStatusMessage(const QString &msg);
    void handleLadderPingUpdated(int ms);
    void handleDomRowClicked(Qt::MouseButton button,
                             int row,
                             double price,
                             double bidQty,
                             double askQty);
    void handlePrintsCancelRequested(const QStringList &orderIds,
                                     const QString &label,
                                     double price,
                                     bool buy);
    void handlePrintsMoveBeginRequested(const QString &orderId);
    void handlePrintsMoveCommitRequested(const QString &orderId, double newPrice, double fallbackPrice);
    void handlePrintsSltpMoveBeginRequested(const QString &kind, double originPrice);
    void handlePrintsSltpMoveCommitRequested(const QString &kind, double newPrice, double fallbackPrice);
    void handlePrintsDragPreviewPriceRequested(double price);
    void logLadderStatus(const QString &msg);
    void logMarkerEvent(const QString &msg);
    void updateTimeLabel();
    void openConnectionsWindow();
    void openFinrezWindow();
    void openTradesWindow();
    void openPluginsWindow();
    void openSettingsWindow();
    void openAuthWindow();
    void handleConnectionStateChanged(ConnectionStore::Profile profile,
                                      TradeManager::ConnectionState state,
                                      const QString &message);
    void handlePositionChanged(const QString &accountName,
                               const QString &symbol,
                               const TradePosition &position);
    void handleLocalOrdersUpdated(const QString &accountName,
                                  const QString &symbol,
                                  const QVector<DomWidget::LocalOrderMarker> &markers);
    void applyNotionalPreset(int presetIndex);
    void startNotionalEdit(QWidget *columnContainer, int presetIndex);
    void commitNotionalEdit(QWidget *columnContainer, bool apply);
    void applyLeverageForColumn(QWidget *columnContainer, int leverage);
    void requestLighterLeverageLimit(const QString &symbolUpper);
    void applyCompressionForColumn(QWidget *columnContainer, int compression);
    void toggleAlertsPanel();
    void syncWatchedSymbols();

private:
    void appendConnectionsLog(const QString &msg);
    struct MarkerBucket {
        QHash<QString, DomWidget::LocalOrderMarker> confirmed;
        QHash<QString, DomWidget::LocalOrderMarker> pending;
        QString accountLabel;
    };

    struct DomColumn {
        QWidget *container = nullptr;
        QFrame *header = nullptr;
        QString symbol;
        DomWidget *dom = nullptr;
        LadderClient *client = nullptr;
        PrintsWidget *prints = nullptr;
        class ClustersWidget *clusters = nullptr;
        QSplitter *printsDomSplitter = nullptr;
        QSplitter *clustersPrintsSplitter = nullptr;
        QScrollArea *scrollArea = nullptr;
        QScrollBar *scrollBar = nullptr;
        QWidget *floatingWindow = nullptr;
        QLabel *venueIconLabel = nullptr;
        QLabel *marketTypeLabel = nullptr;
        QLabel *readOnlyLabel = nullptr;
        QLabel *tickerLabel = nullptr;
        QLabel *statusLabel = nullptr;
        bool isFloating = false;
        int lastSplitterIndex = -1;
        QList<int> lastSplitterSizes;
        double orderNotional = 10.0;
        int leverage = 20;
        QWidget *notionalOverlay = nullptr;
        class QButtonGroup *notionalGroup = nullptr;
        QToolButton *leverageButton = nullptr;
        QFrame *positionOverlay = nullptr;
        QLabel *positionAvgLabel = nullptr;
        QLabel *positionValueLabel = nullptr;
        QLabel *positionPctLabel = nullptr;
        QLabel *positionPnlLabel = nullptr;
        QToolButton *positionExitButton = nullptr;
        TradePosition cachedPosition;
        bool hasCachedPosition = false;
        QString lastOverlayValueText;
        QString lastOverlayPctText;
        QString lastOverlayPnlText;
        int lastOverlayPnlSign = 0;
        qint64 lastOverlayUpdateMs = 0;
        QWidget *notionalEditOverlay = nullptr;
        class QLineEdit *notionalEditField = nullptr;
        int editingNotionalIndex = -1;
        std::array<double, 5> notionalValues{};
        QVector<DomWidget::LocalOrderMarker> localOrders;
        bool sltpHasSl = false;
        double sltpSlPrice = 0.0;
        qint64 sltpSlCreatedMs = 0;
        int sltpSlMissCount = 0;
        bool sltpHasTp = false;
        double sltpTpPrice = 0.0;
        qint64 sltpTpCreatedMs = 0;
        int sltpTpMissCount = 0;
        // SL/TP placement is optimistic (UI updates before server confirms).
        // Track the last user placement to resolve "ghost" opposite markers quickly if we misclassified.
        qint64 sltpLastPlaceMs = 0;
        bool sltpLastPlaceWasSl = true;
        double sltpLastPlacePrice = 0.0;
        bool dragPreviewActive = false;
        double dragPreviewPrice = 0.0;
        int tickCompression = 1;
        QToolButton *compressionButton = nullptr;
        QToolButton *closeButton = nullptr;
        QString accountName;
        QColor accountColor;
        qint64 bufferMinTick = 0;
        qint64 bufferMaxTick = 0;
        qint64 displayMinTick = 0;
        qint64 displayMaxTick = 0;
        qint64 visibleCenterTick = 0;
        bool hasBuffer = false;
        int visibleRowCount = 0;
        double bufferTickSize = 0.0;
        bool userScrolling = false;
        bool pendingExtendUp = false;
        bool pendingExtendDown = false;
        bool pendingAutoCenter = false;
        bool pendingAutoCenterTickValid = false;
        qint64 pendingAutoCenterTick = 0;
        bool pendingViewportUpdate = false;
        qint64 pendingViewportBottom = 0;
        qint64 pendingViewportTop = 0;
        quint64 pendingViewportRevision = 0;
        qint64 lastViewportBottom = 0;
        qint64 lastViewportTop = 0;
        quint64 lastViewportRevision = 0;
        quint64 bufferRevision = 0;
        qint64 lastSnapshotTimestampMs = 0;
        double avgSnapshotIntervalMs = 0.0;
        qint64 lastSnapshotLogMs = 0;
        qint64 lastFpsLabelUpdateMs = 0;
        double lastFpsHz = 0.0;
        qint64 lastPrintsBottomTick = 0;
        qint64 lastPrintsTopTick = 0;
        int lastPrintsRowHeight = 0;
        int lastPrintsCompression = 0;
        double lastPrintsTick = 0.0;
        int lastPrintsRowCount = 0;
        QString lastStatusMessage;
        qint64 extendQueuedShiftUp = 0;
        qint64 extendQueuedShiftDown = 0;
        int lastScrollBarValue = 0;
        bool scrollValueValid = false;
        bool backendManualPinned = false;
    };

    void applyDomHighlightPrices(DomColumn &col);

    struct WorkspaceTab {
        int id = 0;
        QWidget *workspace = nullptr;
        QSplitter *columns = nullptr;
        QWidget *columnsSpacer = nullptr; // invisible splitter tail so the right edge stays resizable
        QVector<DomColumn> columnsData;
    };

    void updatePositionOverlay(DomColumn &col, const TradePosition &position);

    void buildUi();
    QWidget *buildTopBar(QWidget *parent);
    QWidget *buildMainArea(QWidget *parent);

    void createInitialWorkspace();
    void updateTabUnderline(int index);
    WorkspaceTab createWorkspaceTab(const QVector<SavedColumn> &columns);
    void refreshTabCloseButtons();
    DomColumn createDomColumn(const QString &symbol, const QString &account, WorkspaceTab &tab);
    void toggleDomColumnFloating(QWidget *container, const QPoint &globalPos = QPoint());
    void floatDomColumn(WorkspaceTab &tab, DomColumn &col, int indexInSplitter, const QPoint &globalPos = QPoint());
    void dockDomColumn(WorkspaceTab &tab, DomColumn &col, int preferredIndex = -1);
    bool locateColumn(QWidget *container, WorkspaceTab *&tabOut, DomColumn *&colOut, int &splitIndex);
    void removeDomColumn(QWidget *container);
    void updateDomColumnResize(int delta);
    void endDomColumnResize();
    void cancelPendingDomResize();
    void releaseDomResizeMouseGrab();
    enum class AddAction {
        WorkspaceTab,
        LadderColumn
    };
    void triggerAddAction(AddAction action);
    void setLastAddAction(AddAction action);
    void updateAddButtonsToolTip();
    void centerActiveLaddersToSpread();
    void handleSettingsSearch();
    void handleSettingsSearchFromCompleter(const QString &value);
    void toggleSettingsSearchDrawer(bool open, bool focusEdit = true);
    struct SettingEntry {
        QString id;
        QString name;
        QStringList keywords;
    };
    const SettingEntry *matchSettingEntry(const QString &query) const;
    void openSettingEntry(const QString &id);
    void loadUserSettings();
    void saveUserSettings() const;
    QVector<VolumeHighlightRule> defaultVolumeHighlightRules() const;
    void applyVolumeRulesToAllDoms();
    void refreshActiveLadder();
    DomColumn *focusedDomColumn();
    void setActiveDomContainer(QWidget *container);
    void applyActiveDomOutline();
    void scheduleSaveUserSettings(int delayMs = 500);
    void adjustVolumeRulesBySteps(int steps);
    void initializeDomFrameTimer();
    void applyDomFrameRate(int fps);
    void handleDomFrameTick();
    bool handleSltpKeyPress(QKeyEvent *event);
    bool handleSltpKeyRelease(QKeyEvent *event);
    bool matchesSltpHotkey(int eventKey, Qt::KeyboardModifiers eventMods) const;
    DomColumn *activeColumnForSltpHotkey();
    void setSltpHoldUi(bool held);
    void pollSltpHoldKey();
    void updateColumnStatusLabel(DomColumn &col);
    void refreshDomColumnFrame(DomColumn &col);
    bool pullSnapshotForColumn(DomColumn &col, qint64 bottomTick, qint64 topTick);
    void maybeTriggerSltpForColumn(DomColumn &col, const DomSnapshot &snap);
    QVector<SettingsWindow::HotkeyEntry> currentCustomHotkeys() const;
    void updateCustomHotkey(const QString &id, int key, Qt::KeyboardModifiers mods);
    static bool matchesHotkey(int eventKey,
                              Qt::KeyboardModifiers eventMods,
                              int key,
                              Qt::KeyboardModifiers mods);
    int baseLevelsPerSide() const;
    int guiLevelsPerSide() const;
    int backgroundLevelsPerSide() const;
    int effectiveLevelsForColumn(const DomColumn &col) const;
    qint64 targetDisplaySpanTicks(const DomColumn &col) const;
    qint64 displaySlideStepTicks(const DomColumn &col) const;
    qint64 smoothSlideStepTicks(const DomColumn &col) const;
    bool slideDisplayWindow(DomColumn &col, int direction, qint64 overrideStep = 0);
    void maybePrefetchBuffer(DomColumn &col, bool upwards);
    void queueColumnDepthShift(DomColumn &col, bool upwards, double multiplier);
    void recenterDisplayWindow(DomColumn &col, qint64 centerTick);
    void handleDomScroll(QWidget *columnContainer, int value);
    void restartColumnClient(DomColumn &col);
    void updateColumnViewport(DomColumn &col, bool forceCenter = false);
    void flushPendingColumnViewport(DomColumn &col);
    void updateColumnScrollRange(DomColumn &col);
    int desiredVisibleRows(const DomColumn &col) const;
    void applyColumnCenterTick(DomColumn &col, qint64 centerTick);
    void handleColumnBufferRange(QWidget *container,
                                 qint64 minTick,
                                 qint64 maxTick,
                                 qint64 centerTick,
                                 double tickSize);
    void bootstrapColumnFromClient(QWidget *container);
    QWidget *columnContainerForObject(QObject *obj) const;
    void setColumnUserScrolling(QWidget *container, bool scrolling);
    void addLocalOrderMarker(const QString &accountName,
                             const QString &symbol,
                             OrderSide side,
                             double price,
                             double quantity,
                             const QString &orderId,
                             qint64 createdMs);
    void removeLocalOrderMarker(const QString &accountName,
                                const QString &symbol,
                                const QString &orderId);
    void refreshColumnMarkers(DomColumn &col);
    void refreshSltpMarkers(DomColumn &col);
    void clearSltpMarkers(DomColumn &col);
    void clearColumnLocalMarkers(DomColumn &col);
    void clearSymbolLocalMarkers(const QString &symbolUpper);
    bool clearSymbolLocalMarkersInternal(const QString &symbolUpper, bool markPending);
    void markPendingCancelForSymbol(const QString &symbol);
    void clearPendingCancelForSymbol(const QString &symbol);
    void schedulePendingCancelTimer(const QString &symbol);
    void refreshColumnsForSymbol(const QString &symbolUpper);
    QVector<DomWidget::LocalOrderMarker> aggregateMarkersForDisplay(const MarkerBucket &bucket,
                                                                    bool suppressRemote) const;
    void enforcePendingLimit(MarkerBucket &bucket);
    void pruneExpiredPending(MarkerBucket &bucket, bool allowRemoval) const;
    void cancelDelayedMarkers(const QString &symbolUpper, const QString &accountKey = QString());
    void removeMarkerDelayTimer(const QString &timerKey, QTimer *timer);
    enum class SymbolSource { Mexc, MexcFutures, UzxSpot, UzxSwap, BinanceSpot, BinanceFutures, Lighter };
    void fetchSymbolLibrary();
    void fetchSymbolLibrary(SymbolSource source, SymbolPickerDialog *dlg = nullptr);
    void fetchMexcFuturesSymbols();
    void requestDefaultSymbolAllowList(QStringList symbols,
                                       QSet<QString> apiOff,
                                       int fetchedCount);
    void finalizeSymbolFetch(QStringList symbols, QSet<QString> apiOff, int fetchedCount);
    void broadcastSymbolsToPickers(SymbolSource source);
    void setPickersRefreshState(SymbolSource source, bool refreshing);
    void trackSymbolPicker(SymbolPickerDialog *dlg);
    void untrackSymbolPicker(SymbolPickerDialog *dlg);
    SymbolSource symbolSourceForAccount(const QString &accountName) const;
    void mergeSymbolLibrary(const QStringList &symbols, const QSet<QString> &apiOff);
    void addNotification(const QString &text, bool unread = true);
    void updateAlertsBadge();
    void refreshAlertsList();
    void markAllNotificationsRead();
    void repositionAlertsPanel();
    void handleUpdateButtonClicked();
    void checkForUpdates();
    void startUpdateDownload();
    void applyDownloadedUpdate();

    WorkspaceTab *currentWorkspaceTab();
    int findTabIndexById(int id) const;
    QIcon loadIcon(const QString &name) const;
    QIcon loadIconTinted(const QString &name, const QColor &color, const QSize &size) const;
    QString resolveAssetPath(const QString &relative) const;
    void retargetDomColumn(DomColumn &col, const QString &symbol);
    SymbolPickerDialog *createSymbolPicker(const QString &title,
                                           const QString &currentSymbol,
                                           const QString &currentAccount);
    void applyVenueAppearance(SymbolPickerDialog *dlg,
                              SymbolSource source,
                              const QString &accountName) const;
    void applySymbolToColumn(DomColumn &col, const QString &symbol, const QString &accountName);
    void refreshAccountColors();
    QColor accountColorFor(const QString &accountName) const;
    void applyAccountColorsToColumns();
    void applyTickerLabelStyle(QLabel *label, const QColor &accent, bool hovered = false);
    void applyHeaderAccent(DomColumn &col);
    void showCompressionMenu(DomColumn &col, const QPoint &globalPos);
    enum class UpdateState {
        NotChecked,
        Checking,
        UpdateAvailable,
        Downloading,
        DownloadPaused,
        Failed,
        Installed,
        UpToDate
    };
    struct UpdateInfo {
        QString version;
        QUrl url;
        QByteArray sha256;
        QUrl backendUrl;
        QByteArray backendSha256;
        QStringList notes;
        bool mandatory = false;
    };
    void setUpdateState(UpdateState state, const QString &tooltip = QString());
    QString updateIconName(UpdateState state) const;
    QString updateTooltip(UpdateState state) const;
    QString currentUpdateVersion() const;
    bool parseLatestJson(const QByteArray &raw, UpdateInfo &info, QString &err) const;
    void ensureUpdatePanel();
    void toggleUpdatePanel();
    void refreshUpdatePanel();
    void startBackendDownload();
    bool verifyUpdateHash(const QString &path, const QByteArray &expected, QString &err) const;

    QString m_backendPath;
    QStringList m_symbols;
    QStringList m_symbolLibrary;
    QStringList m_mexcFuturesSymbols;
    QHash<QString, int> m_mexcFuturesMaxLeverageBySymbol; // SYMBOL -> maxLeverage
    QHash<QString, QList<int>> m_mexcFuturesLeverageTagsBySymbol; // SYMBOL -> suggested leverages
    QHash<QString, int> m_lighterMaxLeverageBySymbol; // SYMBOL -> maxLeverage (perps only)
    QHash<QString, int> m_uzxSwapMaxLeverageBySymbol; // SYMBOL -> maxLeverage
    QHash<QString, double> m_uzxSwapContractValueBySymbol; // SYMBOL -> swap_value
    QHash<QString, double> m_uzxSwapPriceUnitBySymbol; // SYMBOL -> price_unit
    QHash<QString, int> m_uzxSwapNumPrecisionBySymbol; // SYMBOL -> num_precision
    QStringList m_uzxSpotSymbols;
    QStringList m_uzxSwapSymbols;
    QStringList m_binanceSpotSymbols;
    QStringList m_binanceFuturesSymbols;
    QStringList m_lighterSymbols;
    QSet<QString> m_uzxSpotApiOff;
    QSet<QString> m_uzxSwapApiOff;
    QSet<QString> m_apiOffSymbols;
    QSet<QString> m_mexcFuturesApiOff;
    QSet<QString> m_lighterApiOff;
    int m_levels;
    QVector<QVector<SavedColumn>> m_savedLayout;
    QNetworkAccessManager m_symbolFetcher;
    bool m_symbolRequestInFlight = false;
    bool m_mexcFuturesRequestInFlight = false;
    bool m_uzxSpotRequestInFlight = false;
    bool m_uzxSwapRequestInFlight = false;
    bool m_binanceSpotRequestInFlight = false;
    bool m_binanceFuturesRequestInFlight = false;
    bool m_lighterRequestInFlight = false;
    QSet<QString> m_lighterLeverageInFlight;

    QTabBar *m_workspaceTabs;
    QFrame *m_tabUnderline;
    QPropertyAnimation *m_tabUnderlineAnim;
    bool m_tabUnderlineHiddenForDrag;
    QStackedWidget *m_workspaceStack;
    QToolButton *m_addTabButton;
    QToolButton *m_addMenuButton;
    QSet<QString> m_pendingCancelSymbols;
    QHash<QString, QTimer *> m_pendingCancelTimers;
    QHash<QString, QHash<QString, MarkerBucket>> m_markerBuckets;
    QHash<QString, QVector<QString>> m_lastLimitOrderStackByKey;
    QHash<QString, QHash<QString, qint64>> m_pendingLimitCancelByKey;
    QHash<QString, QString> m_lastCancelAttemptByKey;
    QHash<QString, QPointer<QTimer>> m_markerDelayTimers;
    QHash<QString, DomWidget::LocalOrderMarker> m_moveMarkerCache;
    QToolButton *m_settingsSearchButton = nullptr;
    QWidget *m_settingsSearchContainer = nullptr;
    QPointer<QPropertyAnimation> m_settingsSearchAnim;
    bool m_settingsSearchExpanded = false;
    QLineEdit *m_settingsSearchEdit;
    QCompleter *m_settingsCompleter = nullptr;
    QToolButton *m_connectionButton;
    QLabel *m_timeLabel;
    QHash<QString, QColor> m_accountColors;
    QHash<QString, SymbolSource> m_accountSources;
    QVector<QPointer<SymbolPickerDialog>> m_symbolPickers;
    QToolButton *m_updateButton = nullptr;
    UpdateState m_updateState = UpdateState::NotChecked;
    UpdateInfo m_updateInfo;
    QString m_updateStatusText;
    QNetworkAccessManager *m_updateNetwork = nullptr;
    QPointer<QNetworkReply> m_updateReply;
    QFile *m_updateFile = nullptr;
    QString m_updateFilePath;
    QString m_updateBackendFilePath;
    QFrame *m_updatePanel = nullptr;
    QLabel *m_updatePanelTitle = nullptr;
    QLabel *m_updatePanelBody = nullptr;
    QPushButton *m_updateInstallButton = nullptr;
    QPushButton *m_updateLaterButton = nullptr;
    QPushButton *m_updateCheckButton = nullptr;

    QLabel *m_statusLabel;
    QLabel *m_pingLabel;
    QToolButton *m_alertsButton = nullptr;
    QLabel *m_alertsBadge = nullptr;
    QWidget *m_alertsPanel = nullptr;
    QPointer<QPropertyAnimation> m_alertsPanelAnim;
    QRect m_alertsPanelTargetRect;
    bool m_alertsPanelExpanded = false;
    QListWidget *m_alertsList = nullptr;
    class QMediaPlayer *m_notificationPlayer = nullptr;
    class QAudioOutput *m_notificationOutput = nullptr;
    class QSoundEffect *m_notificationEffect = nullptr;
    class QSoundEffect *m_successEffect = nullptr;
    class QMediaPlayer *m_successPlayer = nullptr;
    class QAudioOutput *m_successOutput = nullptr;
    bool m_soundsInitialized = false;
    qint64 m_startupMs = 0;

    QWidget *m_orderPanel;
    QLabel *m_orderSymbolLabel;
    QLineEdit *m_orderPriceEdit;
    QLineEdit *m_orderQuantityEdit;
    QPushButton *m_buyButton;
    QPushButton *m_sellButton;

    PluginsWindow *m_pluginsWindow;
    FinrezWindow *m_finrezWindow = nullptr;
    SettingsWindow *m_settingsWindow;
    ConnectionStore *m_connectionStore;
    TradeManager *m_tradeManager;
    ConnectionsWindow *m_connectionsWindow;
    TradesWindow *m_tradesWindow = nullptr;
    QStringList m_connectionsLogBacklog;
    QPointer<QDialog> m_authDialog;

    QVector<WorkspaceTab> m_tabs;
    int m_nextTabId;
    QVector<int> m_recycledTabIds;
    QIcon m_tabCloseIconNormal;
    QIcon m_tabCloseIconHover;
    AddAction m_lastAddAction;
    std::array<double, 5> m_defaultNotionalPresets{{1.0, 2.5, 5.0, 10.0, 25.0}};
    QHash<QString, int> m_futuresLeverageBySymbol; // key: SYMBOL (upper), value: leverage
    QHash<QString, std::array<double, 5>> m_notionalPresetsByKey; // key: account|symbol, value: quick size presets
    bool m_notionalEditActive = false;

    QTimer *m_timeTimer;
    QWidget *m_topBar;
    int m_timeOffsetMinutes = 0;
    // Window buttons (stored so we can update icon dynamically)
    QToolButton *m_minButton;
    QToolButton *m_maxButton;
    QToolButton *m_closeButton;

    bool m_nativeSnapEnabled = false;
    int m_resizeBorderWidth = 6; // resize area thickness in px
    QWidget *m_activeDomContainer = nullptr;
    bool m_activeDomOutlineEnabled = true;
    QHash<QString, QSet<QString>> m_lastWatchedSymbolsByAccount;
    QList<int> m_savedDomPrintsSplitterSizes;
    QList<int> m_savedClustersPrintsSplitterSizes;
    bool m_clustersPrintsSplitterEverShown = false;
    QVector<QList<int>> m_savedWorkspaceColumnSizes;
    QTimer *m_saveSettingsDebounce = nullptr;

    // (no custom dragging state ? we use native system move/snap)
    QWidget *m_draggingDomContainer = nullptr;
    QPoint m_domDragStartGlobal;
    QPoint m_domDragStartWindowOffset;
    bool m_domDragActive = false;
    QWidget *m_domResizeContainer = nullptr;
    WorkspaceTab *m_domResizeTab = nullptr;
    QList<int> m_domResizeInitialSizes;
    QPoint m_domResizeStartPos;
    int m_domResizeSplitterIndex = -1;
    bool m_domResizeFromLeftEdge = false;
    bool m_domResizeActive = false;

    void ensureSoundsInitialized();
    bool m_domResizePending = false;
    QWidget *m_domResizeHandle = nullptr;

    // Remember last normal (non-maximized) geometry so we can restore it
    // reliably after system-maximize (including Aero Snap) without ending up
    // with a clipped/offset window.
    QRect m_lastNormalGeometry;
    bool m_haveLastNormalGeometry = false;
    Qt::WindowStates m_prevWindowState = Qt::WindowNoState;

    // Hotkey: ????????????? ???????? ?? ??????.
    int m_centerKey = Qt::Key_Shift;
    Qt::KeyboardModifiers m_centerMods = Qt::NoModifier;
    bool m_centerAllLadders = true;
    QVector<SettingEntry> m_settingEntries;
    QVector<VolumeHighlightRule> m_volumeRules;
    bool m_capsAdjustMode = false;
    bool m_ctrlExitArmed = false;
    int m_newTabKey = Qt::Key_T;
    Qt::KeyboardModifiers m_newTabMods = Qt::ControlModifier;
    int m_addLadderKey = Qt::Key_E;
    Qt::KeyboardModifiers m_addLadderMods = Qt::ControlModifier;
    int m_refreshLadderKey = Qt::Key_R;
    Qt::KeyboardModifiers m_refreshLadderMods = Qt::ControlModifier;
    int m_cancelLastLimitKey = Qt::Key_G;
    Qt::KeyboardModifiers m_cancelLastLimitMods = Qt::NoModifier;
    int m_volumeAdjustKey = Qt::Key_CapsLock;
    Qt::KeyboardModifiers m_volumeAdjustMods = Qt::NoModifier;
    int m_sltpPlaceKey = Qt::Key_C;
    Qt::KeyboardModifiers m_sltpPlaceMods = Qt::NoModifier;
    int m_settingsSearchKey = Qt::Key_F3;
    Qt::KeyboardModifiers m_settingsSearchMods = Qt::NoModifier;
    bool m_sltpPlaceHeld = false;
    QPointer<DomWidget> m_sltpPlaceDom;
    QTimer *m_sltpHoldPollTimer = nullptr;
    QTimer *m_domFrameTimer = nullptr;
    int m_domTargetFps = 60;
    std::array<int, 5> m_notionalPresetKeys{
        {Qt::Key_1, Qt::Key_2, Qt::Key_3, Qt::Key_4, Qt::Key_5}};
    std::array<Qt::KeyboardModifiers, 5> m_notionalPresetMods{
        {Qt::NoModifier, Qt::NoModifier, Qt::NoModifier, Qt::NoModifier, Qt::NoModifier}};
    struct NotificationEntry {
        QString text;
        QDateTime timestamp;
        bool read = false;
    };
    QVector<NotificationEntry> m_notifications;
    int m_unreadNotifications = 0;
};
