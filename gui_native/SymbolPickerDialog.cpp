#include "SymbolPickerDialog.h"

#include <QAbstractItemView>
#include <QColor>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QGridLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QHoverEvent>
#include <QModelIndex>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSortFilterProxyModel>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QStyle>
#include <QTableView>
#include <QToolButton>
#include <QVBoxLayout>
#include <QStyledItemDelegate>
#include <algorithm>

namespace {
QString resolveAssetPath(const QString &relative)
{
    const QString rel = QDir::fromNativeSeparators(relative);

    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList bases = {
        appDir,
        QDir(appDir).filePath(QStringLiteral("img")),
        QDir(appDir).filePath(QStringLiteral("assets/img/icons")),
        QDir(appDir).filePath(QStringLiteral("assets/img/icons/outline")),
        QDir(appDir).filePath(QStringLiteral("assets/img/icons/logos")),
        QDir(appDir).filePath(QStringLiteral("../img")),
        QDir(appDir).filePath(QStringLiteral("../assets/img/icons")),
        QDir(appDir).filePath(QStringLiteral("../assets/img/icons/outline")),
        QDir(appDir).filePath(QStringLiteral("../assets/img/icons/logos")),
        QDir(appDir).filePath(QStringLiteral("../../img")),
        QDir(appDir).filePath(QStringLiteral("../../assets/img/icons")),
        QDir(appDir).filePath(QStringLiteral("../../assets/img/icons/outline")),
        QDir(appDir).filePath(QStringLiteral("../../assets/img/icons/logos"))
    };
    for (const QString &base : bases) {
        const QString candidate = QDir(base).filePath(rel);
        if (QFile::exists(candidate)) {
            return candidate;
        }
    }
    const QString resPath = QStringLiteral(":/") + rel;
    if (QFile::exists(resPath)) {
        return resPath;
    }
    return QString();
}

class HoverHighlightDelegate : public QStyledItemDelegate {
public:
    HoverHighlightDelegate(const int *hoverRow, QObject *parent = nullptr)
        : QStyledItemDelegate(parent)
        , m_hoverRow(hoverRow)
    {
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QStyleOptionViewItem opt(option);
        if (m_hoverRow && *m_hoverRow >= 0 && index.row() == *m_hoverRow
            && !(opt.state & QStyle::State_Selected)) {
            opt.state |= QStyle::State_MouseOver;
            opt.state |= QStyle::State_Selected;
        }
        QStyledItemDelegate::paint(painter, opt, index);
    }

private:
    const int *m_hoverRow = nullptr;
};

} // namespace

SymbolPickerDialog::SymbolPickerDialog(QWidget *parent)
    : QDialog(parent)
    , m_filterEdit(new QLineEdit(this))
    , m_tableView(new QTableView(this))
    , m_model(new QStandardItemModel(this))
    , m_proxy(new SymbolFilterProxy(this))
    , m_connectionsList(new QListWidget(this))
    , m_groupList(new QListWidget(this))
{
    setWindowTitle(tr("Select symbol"));
    setModal(false);
    setWindowModality(Qt::NonModal);
    setMinimumSize(560, 440);

    auto *grid = new QGridLayout(this);
    grid->setContentsMargins(12, 12, 12, 12);
    grid->setHorizontalSpacing(12);
    grid->setVerticalSpacing(12);

    auto *leftPanel = new QVBoxLayout();
    leftPanel->setContentsMargins(0, 0, 0, 0);
    leftPanel->setSpacing(12);
    auto *connectionsLabel = new QLabel(tr("Connections"), this);
    leftPanel->addWidget(connectionsLabel);
    m_connectionsList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_connectionsList->setUniformItemSizes(true);
    m_connectionsList->setMinimumWidth(190);
    leftPanel->addWidget(m_connectionsList, 1);

    auto *groupsLabel = new QLabel(tr("Instrument groups"), this);
    leftPanel->addWidget(groupsLabel);
    m_groupList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_groupList->setUniformItemSizes(true);
    m_groupList->setMinimumWidth(190);
    leftPanel->addWidget(m_groupList, 2);

    grid->addLayout(leftPanel, 0, 0, 2, 1);

    auto *searchRow = new QHBoxLayout();
    m_filterEdit->setPlaceholderText(tr("Search by name..."));
    searchRow->addWidget(m_filterEdit, 1);

    auto *refreshButton = new QToolButton(this);
    refreshButton->setAutoRaise(true);
    refreshButton->setToolTip(tr("Refresh symbols list from exchange"));
    refreshButton->setCursor(Qt::PointingHandCursor);
    const QString refreshPath = resolveAssetPath(QStringLiteral("icons/outline/refresh.svg"));
    if (!refreshPath.isEmpty()) {
        refreshButton->setIcon(QIcon(refreshPath));
        refreshButton->setIconSize(QSize(16, 16));
    } else {
        refreshButton->setText(QStringLiteral("↻"));
    }
    m_refreshButton = refreshButton;
    searchRow->addWidget(refreshButton);

    grid->addLayout(searchRow, 0, 1);

    m_proxy->setSourceModel(m_model);
    m_proxy->setSortCaseSensitivity(Qt::CaseInsensitive);
    m_proxy->setFilterKeyColumn(1);
    m_proxy->sort(1);

    m_tableView->setModel(m_proxy);
    m_tableView->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tableView->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_tableView->setIconSize(QSize(16, 16));
    m_tableView->setWordWrap(false);
    m_tableView->setShowGrid(true);
    m_tableView->setGridStyle(Qt::SolidLine);
    m_tableView->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_tableView->verticalHeader()->setVisible(false);
    // Disable zebra striping - users found the alternating rows distracting.
    m_tableView->setAlternatingRowColors(false);
    m_tableView->setSortingEnabled(true);
    m_tableView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tableView->setFocusPolicy(Qt::StrongFocus);
    auto *header = m_tableView->horizontalHeader();
    header->setStretchLastSection(false);
    header->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    header->setSectionResizeMode(1, QHeaderView::Stretch);
    header->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    header->setMinimumSectionSize(40);
    m_tableView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_tableView->setMouseTracking(true);
    m_tableView->viewport()->setMouseTracking(true);
    m_tableView->viewport()->setAttribute(Qt::WA_Hover, true);
    m_tableView->viewport()->installEventFilter(this);
    m_tableView->installEventFilter(this);
    m_tableView->setItemDelegate(new HoverHighlightDelegate(&m_hoverRow, m_tableView));
    grid->addWidget(m_tableView, 1, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    grid->addWidget(buttons, 2, 0, 1, 2);

    grid->setRowStretch(0, 0);
    grid->setRowStretch(1, 1);
    grid->setRowStretch(2, 0);
    grid->setColumnStretch(0, 0);
    grid->setColumnStretch(1, 1);

    connect(buttons, &QDialogButtonBox::accepted, this, &SymbolPickerDialog::acceptSelection);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_filterEdit, &QLineEdit::textChanged, this, &SymbolPickerDialog::handleFilterChanged);
    connect(m_filterEdit, &QLineEdit::returnPressed, this, &SymbolPickerDialog::acceptSelection);
    connect(m_tableView, &QTableView::doubleClicked, this, &SymbolPickerDialog::handleActivated);
    connect(refreshButton, &QToolButton::clicked, this, &SymbolPickerDialog::refreshRequested);
    connect(m_connectionsList,
            &QListWidget::itemSelectionChanged,
            this,
            &SymbolPickerDialog::handleConnectionsChanged);
    connect(m_groupList,
            &QListWidget::itemSelectionChanged,
            this,
            &SymbolPickerDialog::handleGroupChanged);

    // Always seed the list with defaults so the picker never "loses" venues,
    // even if the caller hasn't provided accounts yet.
    setAccounts({});
}

bool SymbolPickerDialog::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == m_tableView->viewport()) {
        auto updateHover = [this](int row) {
            if (row == m_hoverRow) {
                return;
            }
            m_hoverRow = row;
            m_tableView->viewport()->update();
        };
        if (event->type() == QEvent::MouseMove) {
            const auto *mouseEvent = static_cast<QMouseEvent *>(event);
            const QModelIndex idx = m_tableView->indexAt(mouseEvent->pos());
            updateHover(idx.isValid() ? idx.row() : -1);
        } else if (event->type() == QEvent::Leave || event->type() == QEvent::HoverLeave) {
            updateHover(-1);
        }
    } else if (obj == m_tableView && event->type() == QEvent::Resize) {
        updateTableColumnWidths();
    }
    return QDialog::eventFilter(obj, event);
}

void SymbolPickerDialog::setSymbols(const QStringList &symbols, const QSet<QString> &apiOff)
{
    m_apiOff = apiOff;
    m_hoverRow = -1;
    QStringList cleaned;
    cleaned.reserve(symbols.size());
    for (const QString &sym : symbols) {
        const QString s = sym.trimmed().toUpper();
        if (s.isEmpty() || cleaned.contains(s, Qt::CaseInsensitive)) {
            continue;
        }
        cleaned.push_back(s);
    }
    std::sort(cleaned.begin(), cleaned.end(), [](const QString &a, const QString &b) {
        return a.toUpper() < b.toUpper();
    });
    m_allSymbols = cleaned;
    m_model->clear();
    m_model->setColumnCount(3);
    m_model->setHorizontalHeaderLabels({tr("Venue"), tr("Symbol"), QString()});
    QMap<QString, int> counts;
    const QString apiOffPath = resolveAssetPath(QStringLiteral("icons/outline/api-off.svg"));
    const QIcon apiOffIcon = apiOffPath.isEmpty() ? QIcon() : QIcon(apiOffPath);
    const bool isLighterVenue =
        m_venueIsLighter || m_selectedAccount.trimmed().toLower().contains(QStringLiteral("lighter"));
    for (const QString &sym : cleaned) {
        const bool isFuturesSymbol =
            isLighterVenue ? !sym.contains(QLatin1Char('/')) : m_venueIsFutures;
        auto *iconItem = new QStandardItem(isFuturesSymbol ? QStringLiteral("F")
                                                           : QStringLiteral("S"));
        iconItem->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
        iconItem->setData(sym, Qt::UserRole + 100);
        if (!m_venueIcon.isNull()) {
            iconItem->setIcon(m_venueIcon);
        }
        iconItem->setTextAlignment(Qt::AlignCenter);

        auto *symbolItem = new QStandardItem(sym);
        symbolItem->setEditable(false);
        symbolItem->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        const QString group = groupForSymbol(sym);
        symbolItem->setData(group, kGroupRole);
        counts[group] = counts.value(group) + 1;

        auto *statusItem = new QStandardItem(QString());
        statusItem->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
        statusItem->setTextAlignment(Qt::AlignCenter);
        if (m_apiOff.contains(sym)) {
            if (!apiOffIcon.isNull()) {
                statusItem->setIcon(apiOffIcon);
            }
            statusItem->setData(sym, Qt::UserRole + 200);
            symbolItem->setToolTip(tr("Symbol not supported for API trading"));
            symbolItem->setForeground(QColor(QStringLiteral("#f26b6b")));
            statusItem->setToolTip(tr("Symbol not supported for API trading"));
        }

        m_model->appendRow(QList<QStandardItem *>{iconItem, symbolItem, statusItem});
    }
    m_groupCounts = counts;
    rebuildGroupList();
    m_proxy->setSearchPattern(m_filterText);
    m_proxy->setGroupFilter(m_currentGroup);
    m_proxy->invalidate();
    m_proxy->sort(1);
    updateTableColumnWidths();
    selectFirstVisible();
}

void SymbolPickerDialog::setAccounts(const QVector<QPair<QString, QColor>> &accounts)
{
    QSignalBlocker blocker(m_connectionsList);
    m_connectionsList->clear();

    QVector<QPair<QString, QColor>> merged = accounts;
    QSet<QString> seen;
    for (const auto &pair : merged) {
        seen.insert(pair.first.trimmed().toLower());
    }

    auto ensureAccount = [&](const QString &name, const QColor &color) {
        const QString key = name.trimmed().toLower();
        if (key.isEmpty() || seen.contains(key)) {
            return;
        }
        merged.push_back({name, color});
        seen.insert(key);
    };

    ensureAccount(QStringLiteral("MEXC Spot"), QColor("#4c9fff"));
    ensureAccount(QStringLiteral("MEXC Futures"), QColor("#f5b642"));
    ensureAccount(QStringLiteral("UZX Spot"), QColor("#8bc34a"));
    ensureAccount(QStringLiteral("UZX Swap"), QColor("#ff7f50"));
    ensureAccount(QStringLiteral("Binance Spot"), QColor("#f0b90b"));
    ensureAccount(QStringLiteral("Binance Futures"), QColor("#f5b642"));
    ensureAccount(QStringLiteral("Lighter"), QColor("#38bdf8"));

    for (const auto &pair : merged) {
        const QString name = pair.first;
        const QColor color = pair.second;
        QPixmap px(14, 14);
        px.fill(Qt::transparent);
        QPainter painter(&px);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setBrush(color.isValid() ? color : QColor(QStringLiteral("#4c9fff")));
        painter.setPen(Qt::NoPen);
        painter.drawEllipse(px.rect().adjusted(1, 1, -1, -1));
        painter.end();
        auto *item = new QListWidgetItem(QIcon(px), name);
        item->setData(Qt::UserRole, name);
        m_connectionsList->addItem(item);
    }

    // Safety net: never allow the picker to drop the Binance entries.
    // (We had reports where caller-provided account lists were incomplete.)
    auto ensureListItem = [&](const QString &name, const QColor &color) {
        for (int i = 0; i < m_connectionsList->count(); ++i) {
            const QString existing = m_connectionsList->item(i)->data(Qt::UserRole).toString();
            if (existing.trimmed().compare(name, Qt::CaseInsensitive) == 0) {
                return;
            }
        }
        QPixmap px(14, 14);
        px.fill(Qt::transparent);
        QPainter painter(&px);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setBrush(color.isValid() ? color : QColor(QStringLiteral("#4c9fff")));
        painter.setPen(Qt::NoPen);
        painter.drawEllipse(px.rect().adjusted(1, 1, -1, -1));
        painter.end();
        auto *item = new QListWidgetItem(QIcon(px), name);
        item->setData(Qt::UserRole, name);
        m_connectionsList->addItem(item);
    };
    ensureListItem(QStringLiteral("Binance Spot"), QColor("#f0b90b"));
    ensureListItem(QStringLiteral("Binance Futures"), QColor("#f5b642"));

    if (m_connectionsList->count() > 0) {
        m_connectionsList->setCurrentRow(0);
    }
    QString target = m_selectedAccount;
    if (target.isEmpty() && !merged.isEmpty()) {
        target = merged.first().first;
    }
    if (m_connectionsList->count() > 0) {
        setCurrentAccount(target.isEmpty() ? m_connectionsList->item(0)->data(Qt::UserRole).toString()
                                           : target);
    }
}

void SymbolPickerDialog::setCurrentSymbol(const QString &symbol)
{
    const QString target = symbol.trimmed().toUpper();
    if (target.isEmpty()) {
        selectFirstVisible();
        return;
    }
    int sourceRow = -1;
    for (int i = 0; i < m_model->rowCount(); ++i) {
        QStandardItem *symbolItem = m_model->item(i, 1);
        if (!symbolItem) {
            continue;
        }
        const QString val = symbolItem->text();
        if (val.compare(target, Qt::CaseInsensitive) == 0) {
            sourceRow = i;
            break;
        }
    }
    if (sourceRow < 0) {
        selectFirstVisible();
        return;
    }
    const QModelIndex srcIdx = m_model->index(sourceRow, 1);
    const QModelIndex proxyIdx = m_proxy->mapFromSource(srcIdx);
    if (proxyIdx.isValid()) {
        m_tableView->setCurrentIndex(proxyIdx);
        m_tableView->scrollTo(proxyIdx, QAbstractItemView::PositionAtCenter);
    } else {
        selectFirstVisible();
    }
}

void SymbolPickerDialog::setCurrentAccount(const QString &account)
{
    if (m_connectionsList->count() == 0) {
        m_selectedAccount = account.trimmed();
        return;
    }
    const QString target = account.trimmed();
    int row = -1;
    for (int i = 0; i < m_connectionsList->count(); ++i) {
        QListWidgetItem *item = m_connectionsList->item(i);
        const QString data = item->data(Qt::UserRole).toString();
        if ((!data.isEmpty() && data.compare(target, Qt::CaseInsensitive) == 0)
            || item->text().compare(target, Qt::CaseInsensitive) == 0) {
            row = i;
            break;
        }
    }
    QSignalBlocker blocker(m_connectionsList);
    if (row >= 0) {
        m_connectionsList->setCurrentRow(row);
    } else if (m_connectionsList->count() > 0) {
        m_connectionsList->setCurrentRow(0);
    }
    handleConnectionsChanged();
}

QString SymbolPickerDialog::selectedSymbol() const
{
    return m_selected;
}

QString SymbolPickerDialog::selectedAccount() const
{
    if (m_selectedAccount.isEmpty() && m_connectionsList->currentItem()) {
        return m_connectionsList->currentItem()->data(Qt::UserRole).toString();
    }
    return m_selectedAccount;
}

void SymbolPickerDialog::handleFilterChanged(const QString &text)
{
    const QString trimmed = text.trimmed();
    m_filterText = trimmed;
    m_proxy->setSearchPattern(trimmed);
    selectFirstVisible();
}

void SymbolPickerDialog::handleActivated(const QModelIndex &index)
{
    if (!index.isValid()) {
        return;
    }
    QModelIndex symbolIdx = index;
    if (symbolIdx.column() != 1) {
        symbolIdx = m_proxy->index(symbolIdx.row(), 1);
    }
    if (!symbolIdx.isValid()) {
        return;
    }
    m_selected = symbolIdx.data().toString().trimmed().toUpper();
    accept();
}

void SymbolPickerDialog::acceptSelection()
{
    QModelIndex idx = m_tableView->currentIndex();
    if (!idx.isValid() && m_proxy->rowCount() > 0) {
        idx = m_proxy->index(0, 1);
    }
    if (idx.isValid()) {
        QModelIndex symbolIdx = idx.column() == 1 ? idx : m_proxy->index(idx.row(), 1);
        if (symbolIdx.isValid()) {
            m_selected = symbolIdx.data().toString().trimmed().toUpper();
        } else {
            m_selected.clear();
        }
    } else {
        m_selected.clear();
    }
    if (auto *item = m_connectionsList->currentItem()) {
        m_selectedAccount = item->data(Qt::UserRole).toString();
        if (m_selectedAccount.isEmpty()) {
            m_selectedAccount = item->text();
        }
    }
    accept();
}

void SymbolPickerDialog::selectFirstVisible()
{
    const QModelIndex idx = m_proxy->index(0, 1);
    if (idx.isValid()) {
        m_tableView->setCurrentIndex(idx);
    }
}

void SymbolPickerDialog::handleConnectionsChanged()
{
    QListWidgetItem *item = m_connectionsList->currentItem();
    if (!item) {
        return;
    }
    QString account = item->data(Qt::UserRole).toString();
    if (account.isEmpty()) {
        account = item->text();
    }
    if (account.isEmpty()) {
        return;
    }
    if (QString::compare(account, m_selectedAccount, Qt::CaseInsensitive) == 0) {
        m_selectedAccount = account;
        return;
    }
    m_selectedAccount = account;
    emit accountChanged(account);
}

void SymbolPickerDialog::handleGroupChanged()
{
    QListWidgetItem *item = m_groupList->currentItem();
    QString group;
    if (item) {
        group = item->data(Qt::UserRole).toString();
    }
    m_currentGroup = group;
    m_proxy->setGroupFilter(group);
    selectFirstVisible();
}

void SymbolPickerDialog::rebuildGroupList()
{
    QSignalBlocker blocker(m_groupList);
    const QString previous = m_currentGroup;
    m_groupList->clear();
    const int total = m_allSymbols.size();
    auto *allItem =
        new QListWidgetItem(QStringLiteral("%1 %2").arg(tr("All"), QString::number(total)));
    allItem->setData(Qt::UserRole, QString());
    m_groupList->addItem(allItem);

    QList<QPair<QString, int>> groups;
    groups.reserve(m_groupCounts.size());
    for (auto it = m_groupCounts.constBegin(); it != m_groupCounts.constEnd(); ++it) {
        groups.push_back({it.key(), it.value()});
    }
    std::sort(groups.begin(), groups.end(), [](const auto &a, const auto &b) {
        if (a.second == b.second) {
            return a.first < b.first;
        }
        return a.second > b.second;
    });
    for (const auto &pair : groups) {
        auto *item = new QListWidgetItem(
            QStringLiteral("%1 %2").arg(pair.first, QString::number(pair.second)));
        item->setData(Qt::UserRole, pair.first);
        m_groupList->addItem(item);
    }

    int rowToSelect = 0;
    if (!previous.isEmpty()) {
        for (int i = 0; i < m_groupList->count(); ++i) {
            QListWidgetItem *entry = m_groupList->item(i);
            const QString val = entry->data(Qt::UserRole).toString();
            if (!val.isEmpty() && val.compare(previous, Qt::CaseInsensitive) == 0) {
                rowToSelect = i;
                break;
            }
        }
    }
    m_groupList->setCurrentRow(rowToSelect);
    blocker.unblock();
    handleGroupChanged();
}

QString SymbolPickerDialog::groupForSymbol(const QString &symbol) const
{
    const QString sym = symbol.trimmed().toUpper();
    if (sym.isEmpty()) {
        return QStringLiteral("Other");
    }
    const int sep = sym.lastIndexOf(QLatin1Char('_'));
    if (sep >= 0 && sep + 1 < sym.size()) {
        return sym.mid(sep + 1);
    }
    static const QStringList knownSuffixes = {QStringLiteral("USDT"),
                                              QStringLiteral("USDC"),
                                              QStringLiteral("USD"),
                                              QStringLiteral("EUR"),
                                              QStringLiteral("BTC"),
                                              QStringLiteral("ETH"),
                                              QStringLiteral("TRY"),
                                              QStringLiteral("MX"),
                                              QStringLiteral("BNB"),
                                              QStringLiteral("BUSD"),
                                              QStringLiteral("TUSD")};
    for (const QString &suffix : knownSuffixes) {
        if (sym.endsWith(suffix, Qt::CaseInsensitive)) {
            return suffix.toUpper();
        }
    }
    return QStringLiteral("Other");
}

void SymbolPickerDialog::updateTableColumnWidths()
{
    if (!m_tableView) {
        return;
    }
    auto applyMinWidth = [this](int column, int minimum) {
        if (m_tableView->columnWidth(column) < minimum) {
            m_tableView->setColumnWidth(column, minimum);
        }
    };
    m_tableView->resizeColumnToContents(0);
    m_tableView->resizeColumnToContents(2);
    applyMinWidth(0, 70);
    applyMinWidth(2, 28);
    const QScrollBar *scrollBar = m_tableView->verticalScrollBar();
    const int scrollWidth =
        (scrollBar && scrollBar->isVisible()) ? scrollBar->sizeHint().width() : 0;
    const int totalWidth = m_tableView->viewport()->width();
    if (totalWidth <= 0) {
        m_tableView->setColumnWidth(1, 200);
        return;
    }
    const int frame = m_tableView->style()->pixelMetric(QStyle::PM_DefaultFrameWidth) * 2;
    int used = m_tableView->columnWidth(0) + m_tableView->columnWidth(2) + scrollWidth + frame;
    int available = totalWidth - used;
    if (available < 120) {
        const int deficit = 120 - available;
        available = 120;
        int adjust = deficit / 2;
        int newVenueWidth = std::max(60, m_tableView->columnWidth(0) - adjust);
        int newStatusWidth = std::max(24, m_tableView->columnWidth(2) - (deficit - adjust));
        m_tableView->setColumnWidth(0, newVenueWidth);
        m_tableView->setColumnWidth(2, newStatusWidth);
    }
    m_tableView->setColumnWidth(1, available);
}

void SymbolPickerDialog::setRefreshInProgress(bool refreshing)
{
    if (m_refreshInProgress == refreshing) {
        return;
    }
    m_refreshInProgress = refreshing;
    if (!m_refreshButton) {
        return;
    }
    m_refreshButton->setEnabled(!refreshing);
    m_refreshButton->setCursor(refreshing ? Qt::BusyCursor : Qt::PointingHandCursor);
    m_refreshButton->setToolTip(refreshing ? tr("Refreshing symbols list...")
                                           : tr("Refresh symbols list from exchange"));
}
void SymbolPickerDialog::setVenueAppearance(const QString &iconRelativePath, bool isFutures)
{
    QString resolved;
    if (!iconRelativePath.isEmpty()) {
        const QString normalized = QDir::fromNativeSeparators(iconRelativePath);
        if (QDir::isAbsolutePath(normalized) && QFile::exists(normalized)) {
            resolved = normalized;
        } else {
            resolved = resolveAssetPath(normalized);
        }
    }
    if (!resolved.isEmpty()) {
        m_venueIcon = QIcon(resolved);
    } else {
        m_venueIcon = QIcon();
    }
    m_venueIsFutures = isFutures;
    const QString lowerPath = resolved.toLower();
    m_venueIsLighter = lowerPath.contains(QStringLiteral("lighter"));
}
