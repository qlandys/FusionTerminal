#include "FinrezWindow.h"
#include "TradeManager.h"

#include <QComboBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QToolButton>
#include <QVBoxLayout>

namespace {
QString formatMoney(double v)
{
    return QString::number(v, 'f', std::abs(v) >= 1.0 ? 2 : 4);
}

QString profileKeyText(ConnectionStore::Profile profile)
{
    switch (profile) {
    case ConnectionStore::Profile::MexcFutures:
        return QStringLiteral("MEXC: Futures");
    case ConnectionStore::Profile::UzxSwap:
        return QStringLiteral("UZX: Swap");
    case ConnectionStore::Profile::UzxSpot:
        return QStringLiteral("UZX: Spot");
    case ConnectionStore::Profile::MexcSpot:
    default:
        return QStringLiteral("MEXC: Spot");
    }
}
} // namespace

FinrezWindow::FinrezWindow(TradeManager *manager, QWidget *parent)
    : QDialog(parent)
    , m_manager(manager)
{
    setWindowTitle(QStringLiteral("финрез"));
    setModal(false);
    resize(820, 520);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(8);

    auto *top = new QHBoxLayout();
    top->setContentsMargins(0, 0, 0, 0);
    top->setSpacing(10);

    m_statusDot = new QLabel(this);
    m_statusDot->setFixedSize(12, 12);
    m_statusDot->setStyleSheet(QStringLiteral("border-radius: 6px; background-color: #616161;"));
    top->addWidget(m_statusDot, 0, Qt::AlignVCenter);

    m_profileCombo = new QComboBox(this);
    m_profileCombo->setMinimumWidth(190);
    m_profileCombo->addItem(profileKeyText(ConnectionStore::Profile::MexcSpot),
                            static_cast<int>(ConnectionStore::Profile::MexcSpot));
    m_profileCombo->addItem(profileKeyText(ConnectionStore::Profile::MexcFutures),
                            static_cast<int>(ConnectionStore::Profile::MexcFutures));
    m_profileCombo->addItem(profileKeyText(ConnectionStore::Profile::UzxSpot),
                            static_cast<int>(ConnectionStore::Profile::UzxSpot));
    m_profileCombo->addItem(profileKeyText(ConnectionStore::Profile::UzxSwap),
                            static_cast<int>(ConnectionStore::Profile::UzxSwap));
    top->addWidget(m_profileCombo, 0, Qt::AlignVCenter);

    m_profileLabel = new QLabel(this);
    m_profileLabel->setText(profileTitleWithTag(currentProfile()));
    m_profileLabel->setStyleSheet(QStringLiteral("color: #e0e0e0; font-weight: 600;"));
    top->addWidget(m_profileLabel, 1, Qt::AlignVCenter);

    m_resetBtn = new QPushButton(QStringLiteral("Сбросить финрез"), this);
    top->addWidget(m_resetBtn, 0, Qt::AlignRight | Qt::AlignVCenter);

    root->addLayout(top);

    m_table = new QTableWidget(this);
    m_table->setColumnCount(7);
    m_table->setHorizontalHeaderLabels({QStringLiteral("Монета"),
                                        QStringLiteral("Финрез"),
                                        QStringLiteral("Комиссия"),
                                        QStringLiteral("Средства"),
                                        QStringLiteral("Доступно"),
                                        QStringLiteral("Заблокировано"),
                                        QStringLiteral("Скрыть")});
    m_table->verticalHeader()->setVisible(false);
    m_table->setSelectionMode(QAbstractItemView::NoSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setShowGrid(true);
    m_table->setAlternatingRowColors(false);
    m_table->horizontalHeader()->setStretchLastSection(false);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(6, QHeaderView::Fixed);
    m_table->setColumnWidth(6, 64);

    root->addWidget(m_table, 1);

    connect(m_profileCombo,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this,
            [this]() { refreshUi(); });
    connect(m_resetBtn, &QPushButton::clicked, this, [this]() {
        if (!m_manager) {
            return;
        }
        m_manager->resetFinrez(currentProfile());
    });

    if (m_manager) {
        connect(m_manager,
                &TradeManager::finrezChanged,
                this,
                [this](ConnectionStore::Profile profile) {
                    if (profile == currentProfile()) {
                        refreshUi();
                    }
                });
        connect(m_manager,
                &TradeManager::connectionStateChanged,
                this,
                [this](ConnectionStore::Profile profile, TradeManager::ConnectionState, const QString &) {
                    if (profile == currentProfile()) {
                        refreshUi();
                    }
                });
    }

    refreshUi();
}

ConnectionStore::Profile FinrezWindow::currentProfile() const
{
    const QVariant v = m_profileCombo ? m_profileCombo->currentData() : QVariant();
    if (!v.isValid()) {
        return ConnectionStore::Profile::MexcSpot;
    }
    return static_cast<ConnectionStore::Profile>(v.toInt());
}

QString FinrezWindow::profileTitle(ConnectionStore::Profile profile) const
{
    return profileKeyText(profile);
}

QString FinrezWindow::profileTitleWithTag(ConnectionStore::Profile profile) const
{
    if (!m_manager) {
        return profileTitle(profile);
    }
    const MexcCredentials creds = m_manager->credentials(profile);
    QString tag = creds.label.trimmed();
    if (tag.isEmpty()) {
        tag = creds.uid.trimmed();
    }
    if (tag.isEmpty()) {
        tag = m_manager->accountName(profile);
    }
    return QStringLiteral("%1 [%2]").arg(profileTitle(profile), tag);
}

QColor FinrezWindow::profileStateColor(ConnectionStore::Profile profile) const
{
    if (!m_manager) {
        return QColor("#616161");
    }
    switch (m_manager->state(profile)) {
    case TradeManager::ConnectionState::Connected:
        return QColor("#2e7d32");
    case TradeManager::ConnectionState::Connecting:
        return QColor("#f9a825");
    case TradeManager::ConnectionState::Error:
        return QColor("#c62828");
    case TradeManager::ConnectionState::Disconnected:
    default:
        return QColor("#616161");
    }
}

void FinrezWindow::refreshUi()
{
    const ConnectionStore::Profile profile = currentProfile();
    m_profileLabel->setText(profileTitleWithTag(profile));
    const QColor dot = profileStateColor(profile);
    m_statusDot->setStyleSheet(QStringLiteral("border-radius: 6px; background-color: %1;").arg(dot.name()));
    rebuildTable(profile);
}

void FinrezWindow::rebuildTable(ConnectionStore::Profile profile)
{
    m_table->setRowCount(0);
    if (!m_manager) {
        return;
    }
    const auto rows = m_manager->finrezRows(profile);
    if (rows.isEmpty()) {
        return;
    }

    m_table->setRowCount(rows.size());
    for (int i = 0; i < rows.size(); ++i) {
        const auto &r = rows[i];

        auto *asset = new QTableWidgetItem(r.asset);
        m_table->setItem(i, 0, asset);

        auto *pnl = new QTableWidgetItem(formatMoney(r.pnl));
        pnl->setTextAlignment(Qt::AlignCenter);
        if (std::abs(r.pnl) > 1e-12) {
            const QColor bg = r.pnl >= 0.0 ? QColor("#7CFC90") : QColor("#ff9ea3");
            pnl->setBackground(bg);
            pnl->setForeground(QColor("#000000"));
        }
        m_table->setItem(i, 1, pnl);

        auto *fee = new QTableWidgetItem(formatMoney(r.commission));
        fee->setTextAlignment(Qt::AlignCenter);
        m_table->setItem(i, 2, fee);

        auto *funds = new QTableWidgetItem(formatMoney(r.funds));
        funds->setTextAlignment(Qt::AlignCenter);
        m_table->setItem(i, 3, funds);

        auto *available = new QTableWidgetItem(formatMoney(r.available));
        available->setTextAlignment(Qt::AlignCenter);
        m_table->setItem(i, 4, available);

        auto *locked = new QTableWidgetItem(formatMoney(r.locked));
        locked->setTextAlignment(Qt::AlignCenter);
        m_table->setItem(i, 5, locked);

        auto *closeBtn = new QToolButton(this);
        closeBtn->setText(QStringLiteral("×"));
        closeBtn->setToolTip(QStringLiteral("Закрыть подключение"));
        closeBtn->setAutoRaise(true);
        closeBtn->setCursor(Qt::PointingHandCursor);
        closeBtn->setProperty("profile", static_cast<int>(profile));
        connect(closeBtn, &QToolButton::clicked, this, [this, profile]() {
            if (m_manager) {
                m_manager->disconnect(profile);
            }
        });
        m_table->setCellWidget(i, 6, closeBtn);
    }
}

