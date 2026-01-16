#include "SettingsWindow.h"

#include <QCheckBox>
#include <QComboBox>
#include <QColorDialog>
#include <QFontDatabase>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPushButton>
#include <QStackedWidget>
#include <QTabBar>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QTimer>
#include <QQuickWidget>
#include <QQmlContext>
#include <QtGlobal>
#include <algorithm>
#include <limits>

#include "ThemeManager.h"

namespace {
class HotkeyCaptureDialog : public QDialog
{
public:
    int key = 0;
    Qt::KeyboardModifiers mods = Qt::NoModifier;

    explicit HotkeyCaptureDialog(QWidget *parent = nullptr)
        : QDialog(parent)
    {
        setWindowTitle(QObject::tr("Выбор клавиши"));
        setModal(true);
        setFixedSize(260, 120);

        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(16, 16, 16, 16);
        layout->setSpacing(8);

        auto *label = new QLabel(
            QObject::tr("Нажмите нужную клавишу\n"
                        "или Esc для отмены."),
            this);
        label->setAlignment(Qt::AlignCenter);
        layout->addWidget(label, 1);
    }

protected:
    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->key() == Qt::Key_Escape) {
            reject();
            return;
        }

        key = event->key();
        if (key == Qt::Key_unknown) {
            return;
        }

        mods = event->modifiers();
        if (key == Qt::Key_Shift || key == Qt::Key_Control || key == Qt::Key_Alt ||
            key == Qt::Key_Meta) {
            mods = Qt::NoModifier;
        }

        accept();
    }
};

static QString hotkeyToText(int key, Qt::KeyboardModifiers mods)
{
    QKeySequence seq(mods | key);
    QString text = seq.toString(QKeySequence::NativeText);
    if (key == Qt::Key_Shift && mods == Qt::NoModifier) {
        if (text.isEmpty()) {
            text = QStringLiteral("Shift");
        }
        text += QStringLiteral(" (левый)");
    }
    return text;
}

static QString colorToHex(const QColor &color)
{
    QColor c = color.isValid() ? color : QColor("#ffd54f");
    return QStringLiteral("#%1%2%3")
        .arg(c.red(), 2, 16, QLatin1Char('0'))
        .arg(c.green(), 2, 16, QLatin1Char('0'))
        .arg(c.blue(), 2, 16, QLatin1Char('0'))
        .toUpper();
}
} // namespace

SettingsWindow::SettingsWindow(QWidget *parent)
    : QDialog(parent)
    , m_categoryList(new QListWidget(this))
    , m_pages(new QStackedWidget(this))
    , m_pluginsTable(new QTableWidget(this))
    , m_volumeRulesTable(new QTableWidget(this))
{
    setWindowTitle(tr("Fusion Terminal Settings"));
    setModal(false);
    resize(900, 560);

    auto applyThemeStyle = [this]() {
        ThemeManager *theme = ThemeManager::instance();
        const bool dark = theme->windowBackground().lightness() < 128;

        const QString dialogBg = theme->windowBackground().name(QColor::HexRgb);
        const QString panelBg = theme->panelBackground().name(QColor::HexRgb);
        const QString listBg = (dark ? theme->panelBackground().darker(112) : theme->panelBackground().darker(104))
                                   .name(QColor::HexRgb);
        const QString text = theme->textPrimary().name(QColor::HexRgb);
        const QString border = theme->borderColor().name(QColor::HexRgb);
        const QString grid = theme->gridColor().name(QColor::HexRgb);

        QColor sel = theme->selectionColor();
        sel.setAlpha(std::max(sel.alpha(), 90));
        const QString selection = sel.name(QColor::HexArgb);

        const QString buttonBg = dark ? QStringLiteral("#3a3d41") : QStringLiteral("#e9edf3");
        const QString buttonHover = dark ? QStringLiteral("#45494e") : QStringLiteral("#dfe6ee");
        const QString buttonPressed = dark ? QStringLiteral("#2d2f33") : QStringLiteral("#cfd7e1");

        setStyleSheet(QStringLiteral(
            "QDialog { background-color: %1; color: %2; }"
            "QLabel { color: %2; }"
            "QListWidget { background-color: %3; color: %2; border: none; }"
            "QListWidget::item { padding: 6px 10px; }"
            "QListWidget::item:selected { background-color: %4; }"
            "QStackedWidget { background-color: %5; }"
            "QPushButton {"
            "  background-color: %6;"
            "  color: %2;"
            "  border: 1px solid %7;"
            "  padding: 4px 14px;"
            "  border-radius: 3px;"
            "}"
            "QPushButton:hover { background-color: %8; }"
            "QPushButton:pressed { background-color: %9; }"
            "QHeaderView::section {"
            "  background-color: %3;"
            "  color: %2;"
            "  border: 0px;"
            "}"
            "QTableView { background-color: %5; gridline-color: %10; }"
            "QTabBar { background-color: %5; }"
            "QTabBar::tab {"
            "  background-color: %3;"
            "  color: %2;"
            "  padding: 4px 10px;"
            "  margin-right: 2px;"
            "  border: 1px solid %7;"
            "  border-bottom: none;"
            "  border-top-left-radius: 3px;"
            "  border-top-right-radius: 3px;"
            "}"
            "QTabBar::tab:selected { background-color: %5; }"
            "QTabBar::tab:!selected:hover { background-color: %6; }")
                              .arg(dialogBg,
                                   text,
                                   listBg,
                                   selection,
                                   panelBg,
                                   buttonBg,
                                   border,
                                   buttonHover,
                                   buttonPressed,
                                   grid));
    };
    applyThemeStyle();
    connect(ThemeManager::instance(), &ThemeManager::themeChanged, this, applyThemeStyle);

    auto *rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(12, 12, 12, 12);
    rootLayout->setSpacing(8);

    auto *content = new QWidget(this);
    auto *contentLayout = new QHBoxLayout(content);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(12);

    m_categoryList->setFixedWidth(200);
    m_categoryList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_categoryList->setFrameShape(QFrame::NoFrame);

    contentLayout->addWidget(m_categoryList);
    contentLayout->addWidget(m_pages, 1);

    rootLayout->addWidget(content, 1);

    QFont titleFont = font();
    titleFont.setPointSize(titleFont.pointSize() + 2);
    titleFont.setBold(true);

    // Общие
    auto *generalPage = new QWidget(m_pages);
    auto *generalLayout = new QVBoxLayout(generalPage);
    generalLayout->setContentsMargins(16, 16, 16, 16);
    generalLayout->setSpacing(12);

    auto *generalTitle = new QLabel(tr("Общие настройки"), generalPage);
    generalTitle->setFont(titleFont);
    generalLayout->addWidget(generalTitle);
    generalLayout->addWidget(
        new QLabel(tr("Здесь будут общие настройки терминала."), generalPage));
    generalLayout->addStretch(1);

    addCategory(tr("Общие"), generalPage);

    // Тема / Appearance (QML)
    auto *appearancePage = new QWidget(m_pages);
    auto *appearanceLayout = new QVBoxLayout(appearancePage);
    appearanceLayout->setContentsMargins(0, 0, 0, 0);
    appearanceLayout->setSpacing(0);

    auto *appearanceQuick = new QQuickWidget(appearancePage);
    appearanceQuick->setResizeMode(QQuickWidget::SizeRootObjectToView);
    appearanceQuick->setClearColor(Qt::transparent);
    appearanceQuick->rootContext()->setContextProperty(QStringLiteral("theme"), ThemeManager::instance());
    appearanceQuick->setSource(QUrl(QStringLiteral("qrc:/qml/SettingsAppearance.qml")));
    appearanceLayout->addWidget(appearanceQuick, 1);

    addCategory(tr("Тема"), appearancePage);

    // Торговля
    auto *tradingPage = new QWidget(m_pages);
    auto *tradingLayout = new QVBoxLayout(tradingPage);
    tradingLayout->setContentsMargins(16, 16, 16, 16);
    tradingLayout->setSpacing(8);

    auto *tradingTitle = new QLabel(tr("Торговля"), tradingPage);
    tradingTitle->setFont(titleFont);
    tradingLayout->addWidget(tradingTitle);

    auto *subTabs = new QTabBar(tradingPage);
    subTabs->addTab(tr("Стакан"));
    subTabs->addTab(tr("Тики"));
    subTabs->addTab(tr("Кластера"));
    subTabs->addTab(tr("График"));
    subTabs->setExpanding(false);
    tradingLayout->addWidget(subTabs);

    auto *subStack = new QStackedWidget(tradingPage);

    auto *ladderPage = new QWidget(subStack);
    auto *ladderLayout = new QVBoxLayout(ladderPage);
    ladderLayout->setContentsMargins(0, 12, 0, 0);
    ladderLayout->setSpacing(8);
    ladderLayout->addWidget(new QLabel(tr("Настройки стакана (DOM)."), ladderPage));

    m_domRefreshChoices = {0, 30, 60, 90, 120, 144, 165, 240};
    auto *fpsRow = new QHBoxLayout();
    auto *fpsLabel = new QLabel(tr("Частота перерисовки DOM"), ladderPage);
    fpsLabel->setStyleSheet(QStringLiteral("font-weight:bold;"));
    fpsRow->addWidget(fpsLabel);
    fpsRow->addStretch(1);
    m_domRefreshCombo = new QComboBox(ladderPage);
    for (int fps : m_domRefreshChoices) {
        if (fps <= 0) {
            m_domRefreshCombo->addItem(tr("По событиям (выкл.)"));
        } else {
            m_domRefreshCombo->addItem(tr("%1 Гц").arg(fps));
        }
    }
    fpsRow->addWidget(m_domRefreshCombo);
    ladderLayout->addLayout(fpsRow);

    QObject::connect(m_domRefreshCombo,
                     qOverload<int>(&QComboBox::currentIndexChanged),
                     this,
                     [this](int index) {
                         if (index < 0 || index >= m_domRefreshChoices.size()) {
                             return;
                         }
                         emit domRefreshRateChanged(m_domRefreshChoices[index]);
                     });

    auto *volumeLabel = new QLabel(tr("Подсветка объёмов (USDT)"), ladderPage);
    volumeLabel->setStyleSheet(QStringLiteral("font-weight: bold;"));
    ladderLayout->addWidget(volumeLabel);

    m_volumeRulesTable->setColumnCount(2);
    m_volumeRulesTable->setHorizontalHeaderLabels(
        QStringList() << tr("Порог, USDT") << tr("Цвет"));
    m_volumeRulesTable->horizontalHeader()->setStretchLastSection(true);
    m_volumeRulesTable->verticalHeader()->setVisible(false);
    m_volumeRulesTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_volumeRulesTable->setSelectionMode(QAbstractItemView::SingleSelection);
    ladderLayout->addWidget(m_volumeRulesTable);

    auto *volumeButtons = new QHBoxLayout();
    auto *addVolumeRule = new QPushButton(tr("Добавить правило"), ladderPage);
    auto *removeVolumeRule = new QPushButton(tr("Удалить правило"), ladderPage);
    volumeButtons->addWidget(addVolumeRule);
    volumeButtons->addWidget(removeVolumeRule);
    volumeButtons->addStretch(1);
    ladderLayout->addLayout(volumeButtons);

    ladderLayout->addStretch(1);

    QObject::connect(addVolumeRule, &QPushButton::clicked, this, [this]() {
        VolumeHighlightRule rule;
        if (m_volumeRules.isEmpty()) {
            rule.threshold = 1000.0;
        } else {
            rule.threshold = std::max(1000.0, m_volumeRules.last().threshold);
        }
        rule.color = QColor("#ffd54f");
        m_volumeRules.append(rule);
        sortVolumeRules();
        refreshVolumeRulesTable();
        emitVolumeRulesChanged();
    });

    QObject::connect(removeVolumeRule, &QPushButton::clicked, this, [this]() {
        if (!m_volumeRulesTable || m_volumeRules.isEmpty()) {
            return;
        }
        int row = m_volumeRulesTable->currentRow();
        if (row < 0 || row >= m_volumeRules.size()) {
            return;
        }
        m_volumeRules.removeAt(row);
        sortVolumeRules();
        refreshVolumeRulesTable();
        emitVolumeRulesChanged();
    });

    QObject::connect(m_volumeRulesTable,
                     &QTableWidget::cellChanged,
                     this,
                     [this](int row, int column) {
                         if (m_updatingVolumeTable) {
                             return;
                         }
                         if (row < 0 || row >= m_volumeRules.size()) {
                             return;
                         }
                         if (column == 0) {
                             QTableWidgetItem *item = m_volumeRulesTable->item(row, column);
                             if (!item) {
                                 return;
                             }
                             bool ok = false;
                             double value = item->text().toDouble(&ok);
                             if (!ok) {
                                 item->setText(QString::number(m_volumeRules[row].threshold, 'f', 0));
                                 return;
                             }
                             m_volumeRules[row].threshold = std::max(0.0, value);
                             sortVolumeRules();
                             refreshVolumeRulesTable();
                         }
                         emitVolumeRulesChanged();
                     });
    refreshVolumeRulesTable();
    QObject::connect(m_volumeRulesTable,
                     &QTableWidget::cellClicked,
                     this,
                     [this](int row, int column) {
                         if (column != 1 || row < 0 || row >= m_volumeRules.size()) {
                             return;
                         }
                         const QColor current = m_volumeRules[row].color.isValid()
                                                    ? m_volumeRules[row].color
                                                    : QColor("#ffd54f");
                         QColorDialog dlg(current, this);
                         dlg.setOptions(QColorDialog::DontUseNativeDialog | QColorDialog::ShowAlphaChannel);
                         if (dlg.exec() == QDialog::Accepted) {
                             QColor selected = dlg.selectedColor();
                             if (!selected.isValid()) {
                                 return;
                             }
                             m_volumeRules[row].color = selected;
                             if (auto *item = m_volumeRulesTable->item(row, 1)) {
                                 item->setData(Qt::UserRole, selected);
                                 item->setText(colorToHex(selected));
                                 QBrush brush(selected);
                                 item->setBackground(brush);
                                 QColor textColor = selected.lightness() < 120 ? QColor("#f0f0f0") : QColor("#1e1e1e");
                                 item->setForeground(QBrush(textColor));
                             }
                             emitVolumeRulesChanged();
                         }
                     });

    auto *ticksPage = new QWidget(subStack);
    auto *ticksLayout = new QVBoxLayout(ticksPage);
    ticksLayout->setContentsMargins(0, 12, 0, 0);
    ticksLayout->setSpacing(8);
    ticksLayout->addWidget(new QLabel(tr("Настройки тиков / принтов."), ticksPage));
    ticksLayout->addStretch(1);

    auto *clustersPage = new QWidget(subStack);
    auto *clustersLayout = new QVBoxLayout(clustersPage);
    clustersLayout->setContentsMargins(0, 12, 0, 0);
    clustersLayout->setSpacing(8);
    clustersLayout->addWidget(new QLabel(tr("Настройки кластеров."), clustersPage));
    clustersLayout->addStretch(1);

    auto *chartPage = new QWidget(subStack);
    auto *chartLayout = new QVBoxLayout(chartPage);
    chartLayout->setContentsMargins(0, 12, 0, 0);
    chartLayout->setSpacing(8);
    chartLayout->addWidget(new QLabel(tr("Настройки графика."), chartPage));
    chartLayout->addStretch(1);

    subStack->addWidget(ladderPage);
    subStack->addWidget(ticksPage);
    subStack->addWidget(clustersPage);
    subStack->addWidget(chartPage);

    tradingLayout->addWidget(subStack, 1);

    QObject::connect(subTabs, &QTabBar::currentChanged, subStack, &QStackedWidget::setCurrentIndex);
    subTabs->setCurrentIndex(0);

    m_tradingCategoryIndex = addCategory(tr("Торговля"), tradingPage);

    // Отображение
    auto *displayPage = new QWidget(m_pages);
    auto *displayLayout = new QVBoxLayout(displayPage);
    displayLayout->setContentsMargins(16, 16, 16, 16);
    displayLayout->setSpacing(12);

    m_activeDomOutlineCheck = new QCheckBox(tr("Выделять контур активного стакана"), displayPage);
    m_activeDomOutlineCheck->setChecked(true);
    QObject::connect(m_activeDomOutlineCheck, &QCheckBox::toggled, this, [this](bool on) {
        emit activeDomOutlineEnabledChanged(on);
    });

    auto *displayTitle = new QLabel(tr("Отображение"), displayPage);
    displayTitle->setFont(titleFont);
    displayLayout->addWidget(displayTitle);

    displayLayout->addWidget(m_activeDomOutlineCheck);
    displayLayout->addWidget(new QLabel(tr("Если выключить, активная колонка не будет подсвечиваться рамкой."),
                                        displayPage));

    displayLayout->addWidget(
        new QLabel(tr("Здесь будут глобальные параметры внешнего вида и тем."),
                   displayPage));
    // Global font family (single font for the whole app)
    {
        auto *row = new QHBoxLayout();
        auto *label = new QLabel(tr("Шрифт"), displayPage);
        m_fontFamilyCombo = new QComboBox(displayPage);
        m_fontFamilyCombo->setEditable(true);
        m_fontFamilyCombo->setInsertPolicy(QComboBox::NoInsert);
        m_fontFamilyCombo->setSizeAdjustPolicy(QComboBox::AdjustToContentsOnFirstShow);

        m_fontFamilyCombo->addItem(tr("(По умолчанию)"), QString());
        const QStringList families = QFontDatabase().families();
        for (const QString &fam : families) {
            const QString t = fam.trimmed();
            if (!t.isEmpty()) {
                m_fontFamilyCombo->addItem(t, t);
            }
        }

        row->addWidget(label);
        row->addWidget(m_fontFamilyCombo, 1);
        displayLayout->addLayout(row);

        QObject::connect(m_fontFamilyCombo, &QComboBox::currentTextChanged, this, [this](const QString &text) {
            if (!m_fontFamilyCombo) {
                return;
            }
            QString family;
            const int idx = m_fontFamilyCombo->currentIndex();
            if (idx >= 0) {
                family = m_fontFamilyCombo->itemData(idx).toString().trimmed();
            }
            if (family.isEmpty()) {
                family = text.trimmed();
                if (family == tr("(По умолчанию)")) {
                    family.clear();
                }
            }
            if (family == m_fontFamily) {
                return;
            }
            m_fontFamily = family;
            emit fontFamilyChanged(m_fontFamily);
        });
    }
    displayLayout->addStretch(1);

    addCategory(tr("Отображение"), displayPage);

    // Горячие клавиши
    auto *hotkeysPage = new QWidget(m_pages);
    auto *hotkeysLayout = new QVBoxLayout(hotkeysPage);
    hotkeysLayout->setContentsMargins(16, 16, 16, 16);
    hotkeysLayout->setSpacing(8);

    auto *hotkeysTitle = new QLabel(tr("Горячие клавиши"), hotkeysPage);
    hotkeysTitle->setFont(titleFont);
    hotkeysLayout->addWidget(hotkeysTitle);

    auto *hotkeysInfo = new QLabel(
        tr("Настройка горячих клавиш терминала."),
        hotkeysPage);
    hotkeysInfo->setWordWrap(true);
    hotkeysLayout->addWidget(hotkeysInfo);

    m_pluginsTable->setColumnCount(2);
    m_pluginsTable->setHorizontalHeaderLabels(QStringList() << tr("Действие")
                                                            << tr("Сочетание"));
    auto *hotkeyHeader = m_pluginsTable->horizontalHeader();
    hotkeyHeader->setStretchLastSection(false);
    hotkeyHeader->setSectionResizeMode(0, QHeaderView::Stretch);
    hotkeyHeader->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_pluginsTable->verticalHeader()->setVisible(false);
    m_pluginsTable->setShowGrid(true);
    m_pluginsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_pluginsTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_pluginsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);

    m_hotkeyEntries.clear();
    m_hotkeyEntries.append({QStringLiteral("centerHotkey"),
                            tr("Центрировать по спреду"),
                            m_centerKey,
                            m_centerMods});
    refreshHotkeysTable();

    // Таблица занимает всё доступное место.
    hotkeysLayout->addWidget(m_pluginsTable, 1);

    auto *centerAllCheck =
        new QCheckBox(tr("Применять ко всем стаканам активной вкладки"), hotkeysPage);
    centerAllCheck->setChecked(m_centerAllLadders);
    hotkeysLayout->addWidget(centerAllCheck);

    QObject::connect(centerAllCheck, &QCheckBox::toggled, this, [this](bool on) {
        m_centerAllLadders = on;
        emit centerHotkeyChanged(m_centerKey, m_centerMods, m_centerAllLadders);
    });

    QObject::connect(m_pluginsTable,
                     &QTableWidget::cellDoubleClicked,
                     this,
                     [this](int row, int col) {
                         if (col != 1 || row < 0 || row >= m_hotkeyEntries.size()) {
                             return;
                         }
                         HotkeyCaptureDialog dlg(this);
                         if (dlg.exec() != QDialog::Accepted || dlg.key == 0) {
                             return;
                         }
                         auto &entry = m_hotkeyEntries[row];
                         entry.key = dlg.key;
                         entry.mods = dlg.mods;
                         if (entry.id == QLatin1String("centerHotkey")) {
                             m_centerKey = entry.key;
                             m_centerMods = entry.mods;
                             emit centerHotkeyChanged(m_centerKey, m_centerMods, m_centerAllLadders);
                         } else {
                             emit customHotkeyChanged(entry.id, entry.key, entry.mods);
                         }
                         refreshHotkeysTable();
                     });


    m_hotkeysCategoryIndex = addCategory(tr("Горячие клавиши"), hotkeysPage);

    // Моды
    auto *modsPage = new QWidget(m_pages);
    auto *modsLayout = new QVBoxLayout(modsPage);
    modsLayout->setContentsMargins(16, 16, 16, 16);
    modsLayout->setSpacing(12);

    auto *modsTitle = new QLabel(tr("Моды"), modsPage);
    modsTitle->setFont(titleFont);
    modsLayout->addWidget(modsTitle);

    modsLayout->addWidget(new QLabel(tr("Здесь позже появятся настройки модов."),
                                     modsPage));
    modsLayout->addStretch(1);

    addCategory(tr("Моды"), modsPage);

    QObject::connect(m_categoryList,
                     &QListWidget::currentRowChanged,
                     this,
                     [this](int row) {
                         if (row >= 0 && row < m_pages->count()) {
                             m_pages->setCurrentIndex(row);
                         }
                     });

    if (m_categoryList->count() > 0) {
        m_categoryList->setCurrentRow(0);
    }

    auto *buttons = new QHBoxLayout();
    buttons->addStretch(1);

    auto *closeButton = new QPushButton(tr("Закрыть"), this);
    QObject::connect(closeButton, &QPushButton::clicked, this, &QDialog::close);
    buttons->addWidget(closeButton);

    rootLayout->addLayout(buttons);
}

int SettingsWindow::addCategory(const QString &title, QWidget *page)
{
    auto *item = new QListWidgetItem(title, m_categoryList);
    item->setData(Qt::UserRole, title);
    m_pages->addWidget(page);
    return m_categoryList->count() - 1;
}

void SettingsWindow::setCenterHotkey(int key, Qt::KeyboardModifiers mods, bool allLadders)
{
    m_centerKey = key;
    m_centerMods = mods;
    m_centerAllLadders = allLadders;

    bool found = false;
    for (auto &entry : m_hotkeyEntries) {
        if (entry.id == QLatin1String("centerHotkey")) {
            entry.key = m_centerKey;
            entry.mods = m_centerMods;
            found = true;
            break;
        }
    }
    if (!found) {
        m_hotkeyEntries.prepend({QStringLiteral("centerHotkey"),
                                 tr("������������ ������ �� ������"),
                                 m_centerKey,
                                 m_centerMods});
    }
    refreshHotkeysTable();
}

void SettingsWindow::focusCenterHotkey()
{
    if (m_hotkeysCategoryIndex >= 0 && m_hotkeysCategoryIndex < m_categoryList->count()) {
        m_categoryList->setCurrentRow(m_hotkeysCategoryIndex);
    }
    m_pages->setCurrentWidget(m_pluginsTable->parentWidget());

    if (m_pluginsTable->rowCount() <= 0) {
        return;
    }

    m_pluginsTable->scrollToItem(m_pluginsTable->item(0, 0), QAbstractItemView::PositionAtTop);
    m_pluginsTable->clearSelection();
    m_pluginsTable->selectRow(0);

    const QColor highlight("#ffd166");
    const QColor highlightText("#1e1e1e");
    const QColor normalText("#f0f0f0");

    for (int c = 0; c < m_pluginsTable->columnCount(); ++c) {
        if (auto *item = m_pluginsTable->item(0, c)) {
            item->setBackground(highlight);
            item->setForeground(highlightText);
        }
    }

    QTimer::singleShot(2000, this, [this, normalText]() {
        for (int c = 0; c < m_pluginsTable->columnCount(); ++c) {
            if (auto *item = m_pluginsTable->item(0, c)) {
                item->setBackground(Qt::NoBrush);
                item->setForeground(normalText);
            }
        }
    });
}

void SettingsWindow::setCustomHotkeys(const QVector<HotkeyEntry> &entries)
{
    HotkeyEntry centerEntry;
    bool hasCenter = false;
    for (const auto &entry : m_hotkeyEntries) {
        if (entry.id == QLatin1String("centerHotkey")) {
            centerEntry = entry;
            hasCenter = true;
            break;
        }
    }
    QVector<HotkeyEntry> merged;
    if (hasCenter) {
        merged.append(centerEntry);
    }
    for (const auto &entry : entries) {
        if (entry.id == QLatin1String("centerHotkey")) continue;
        merged.append(entry);
    }
    if (!hasCenter) {
        merged.prepend({QStringLiteral("centerHotkey"),
                        tr("������������ ������ �� ������"),
                        m_centerKey,
                        m_centerMods});
    }
    m_hotkeyEntries = merged;
    refreshHotkeysTable();
}

void SettingsWindow::focusVolumeHighlightRules()
{
    if (m_tradingCategoryIndex >= 0 && m_tradingCategoryIndex < m_categoryList->count()) {
        m_categoryList->setCurrentRow(m_tradingCategoryIndex);
    }
    if (!m_volumeRulesTable) {
        return;
    }
    m_volumeRulesTable->setFocus();
    if (m_volumeRulesTable->rowCount() > 0) {
        m_volumeRulesTable->selectRow(0);
    }
}

void SettingsWindow::setVolumeHighlightRules(const QVector<VolumeHighlightRule> &rules)
{
    m_volumeRules = rules;
    sortVolumeRules();
    refreshVolumeRulesTable();
}

void SettingsWindow::setDomRefreshRate(int fps)
{
    if (!m_domRefreshCombo || m_domRefreshChoices.isEmpty()) {
        return;
    }
    int bestIndex = 0;
    int bestDelta = std::numeric_limits<int>::max();
    for (int i = 0; i < m_domRefreshChoices.size(); ++i) {
        const int delta = qAbs(m_domRefreshChoices[i] - fps);
        if (delta < bestDelta) {
            bestDelta = delta;
            bestIndex = i;
        }
    }
    const QSignalBlocker blocker(m_domRefreshCombo);
    m_domRefreshCombo->setCurrentIndex(bestIndex);
}

void SettingsWindow::setActiveDomOutlineEnabled(bool enabled)
{
    if (!m_activeDomOutlineCheck) {
        return;
    }
    const QSignalBlocker blocker(m_activeDomOutlineCheck);
    m_activeDomOutlineCheck->setChecked(enabled);
}

void SettingsWindow::setFontFamily(const QString &family)
{
    m_fontFamily = family.trimmed();
    if (!m_fontFamilyCombo) {
        return;
    }
    const QSignalBlocker blocker(m_fontFamilyCombo);
    const QString want = m_fontFamily;
    int found = -1;
    for (int i = 0; i < m_fontFamilyCombo->count(); ++i) {
        if (m_fontFamilyCombo->itemData(i).toString() == want) {
            found = i;
            break;
        }
    }
    if (found >= 0) {
        m_fontFamilyCombo->setCurrentIndex(found);
        return;
    }
    if (want.isEmpty()) {
        m_fontFamilyCombo->setCurrentIndex(0);
        return;
    }
    m_fontFamilyCombo->setCurrentText(want);
}

void SettingsWindow::refreshVolumeRulesTable()
{
    if (!m_volumeRulesTable) {
        return;
    }
    m_updatingVolumeTable = true;
    m_volumeRulesTable->setRowCount(m_volumeRules.size());
    for (int row = 0; row < m_volumeRules.size(); ++row) {
        const auto &rule = m_volumeRules[row];
        auto *thresholdItem = new QTableWidgetItem(QString::number(rule.threshold, 'f', 0));
        thresholdItem->setFlags(thresholdItem->flags() | Qt::ItemIsEditable);
        auto *colorItem = new QTableWidgetItem(colorToHex(rule.color));
        colorItem->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
        colorItem->setData(Qt::UserRole, rule.color);
        QColor bg = rule.color.isValid() ? rule.color : QColor("#ffd54f");
        QColor fg = bg.lightness() < 120 ? QColor("#f0f0f0") : QColor("#1e1e1e");
        colorItem->setBackground(QBrush(bg));
        colorItem->setForeground(QBrush(fg));
        colorItem->setTextAlignment(Qt::AlignCenter);
        m_volumeRulesTable->setItem(row, 0, thresholdItem);
        m_volumeRulesTable->setItem(row, 1, colorItem);
    }
    m_updatingVolumeTable = false;
}

void SettingsWindow::emitVolumeRulesChanged()
{
    emit volumeHighlightRulesChanged(m_volumeRules);
}

void SettingsWindow::sortVolumeRules()
{
    std::sort(m_volumeRules.begin(), m_volumeRules.end(), [](const VolumeHighlightRule &a, const VolumeHighlightRule &b) {
        return a.threshold < b.threshold;
    });
}

void SettingsWindow::refreshHotkeysTable()
{
    if (!m_pluginsTable) {
        return;
    }
    m_pluginsTable->setRowCount(m_hotkeyEntries.size());
    for (int row = 0; row < m_hotkeyEntries.size(); ++row) {
        const auto &entry = m_hotkeyEntries[row];
        auto *actionItem = new QTableWidgetItem(entry.label);
        auto *keyItem = new QTableWidgetItem(hotkeyToText(entry.key, entry.mods));
        keyItem->setTextAlignment(Qt::AlignCenter);
        m_pluginsTable->setItem(row, 0, actionItem);
        m_pluginsTable->setItem(row, 1, keyItem);
    }
}
