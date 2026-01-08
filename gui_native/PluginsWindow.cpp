#include "PluginsWindow.h"

#include <QComboBox>
#include <QDateTime>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPixmap>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStackedWidget>
#include <QUrl>
#include <QVBoxLayout>

PluginsWindow::PluginsWindow(QWidget *parent)
    : QDialog(parent)
    , m_list(new QListWidget(this))
    , m_status(new QLabel(this))
    , m_search(new QLineEdit(this))
    , m_category(new QComboBox(this))
    , m_net(new QNetworkAccessManager(this))
{
    setWindowTitle(tr("Fusion Terminal - Mods"));
    setModal(false);
    resize(980, 640);

    setStyleSheet(QStringLiteral(
        "QDialog { background-color: #1c1c1c; color: #e0e0e0; }"
        "QLabel#ModsTitle { font-size: 18px; font-weight: 700; }"
        "QLabel#ModsHint { color: #9aa0a6; }"
        "QLabel#ModsMeta { color: #9aa0a6; }"
        "QLabel#ModsTag { color: #cfd3d7; }"
        "QLabel#ModsCover {"
        "  background-color: #171717;"
        "  border: 1px solid #2b2b2b;"
        "  border-radius: 6px;"
        "}"
        "QLineEdit {"
        "  background-color: #232323;"
        "  border: 1px solid #343434;"
        "  border-radius: 4px;"
        "  padding: 6px 10px;"
        "  color: #e6e6e6;"
        "}"
        "QComboBox {"
        "  background-color: #232323;"
        "  border: 1px solid #343434;"
        "  border-radius: 4px;"
        "  padding: 6px 10px;"
        "  color: #e6e6e6;"
        "}"
        "QListWidget {"
        "  background-color: #1f1f1f;"
        "  border: 1px solid #2b2b2b;"
        "  border-radius: 6px;"
        "  padding: 4px;"
        "}"
        "QListWidget::item {"
        "  background-color: #1f1f1f;"
        "  border: 1px solid transparent;"
        "  padding: 6px;"
        "}"
        "QListWidget::item:selected {"
        "  background-color: #2b2b2b;"
        "  border: 1px solid #3a3a3a;"
        "}"
        "QPushButton {"
        "  background-color: #2d2d2d;"
        "  border: 1px solid #3a3a3a;"
        "  border-radius: 6px;"
        "  padding: 6px 12px;"
        "  color: #e0e0e0;"
        "}"
        "QPushButton#ModsPrimary {"
        "  background-color: #2e7bdc;"
        "  border: none;"
        "  color: #ffffff;"
        "}"
    ));

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(12, 12, 12, 12);
    mainLayout->setSpacing(10);

    auto *headerRow = new QHBoxLayout();
    auto *title = new QLabel(tr("Marketplace"), this);
    title->setObjectName(QStringLiteral("ModsTitle"));
    headerRow->addWidget(title);
    headerRow->addStretch(1);
    auto *refreshBtn = new QPushButton(tr("Refresh"), this);
    headerRow->addWidget(refreshBtn);
    mainLayout->addLayout(headerRow);

    auto *splitter = new QSplitter(Qt::Horizontal, this);
    mainLayout->addWidget(splitter, 1);

    auto *leftPane = new QWidget(splitter);
    auto *leftLayout = new QVBoxLayout(leftPane);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(8);

    m_search->setPlaceholderText(tr("Search mods"));
    leftLayout->addWidget(m_search);

    m_category->addItem(tr("All"));
    leftLayout->addWidget(m_category);

    leftLayout->addWidget(m_list, 1);

    auto *rightPane = new QWidget(splitter);
    auto *rightLayout = new QVBoxLayout(rightPane);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(10);

    m_detailsStack = new QStackedWidget(rightPane);
    auto *emptyState = new QWidget(m_detailsStack);
    auto *emptyLayout = new QVBoxLayout(emptyState);
    emptyLayout->setAlignment(Qt::AlignCenter);
    auto *emptyLabel = new QLabel(tr("Select a mod to see details"), emptyState);
    emptyLabel->setObjectName(QStringLiteral("ModsHint"));
    emptyLayout->addWidget(emptyLabel);
    m_detailsStack->addWidget(emptyState);

    auto *detailsPage = new QWidget(m_detailsStack);
    auto *detailsLayout = new QVBoxLayout(detailsPage);
    detailsLayout->setContentsMargins(0, 0, 0, 0);
    detailsLayout->setSpacing(8);

    m_detailTitle = new QLabel(detailsPage);
    m_detailTitle->setObjectName(QStringLiteral("ModsTitle"));
    detailsLayout->addWidget(m_detailTitle);

    m_detailCover = new QLabel(detailsPage);
    m_detailCover->setObjectName(QStringLiteral("ModsCover"));
    m_detailCover->setFixedHeight(180);
    m_detailCover->setAlignment(Qt::AlignCenter);
    m_detailCover->setScaledContents(true);
    m_detailCover->setVisible(false);
    detailsLayout->addWidget(m_detailCover);

    m_detailSummary = new QLabel(detailsPage);
    m_detailSummary->setObjectName(QStringLiteral("ModsHint"));
    m_detailSummary->setWordWrap(true);
    detailsLayout->addWidget(m_detailSummary);

    m_detailDesc = new QLabel(detailsPage);
    m_detailDesc->setWordWrap(true);
    detailsLayout->addWidget(m_detailDesc);

    m_detailMeta = new QLabel(detailsPage);
    m_detailMeta->setObjectName(QStringLiteral("ModsMeta"));
    m_detailMeta->setWordWrap(true);
    detailsLayout->addWidget(m_detailMeta);

    m_detailStats = new QLabel(detailsPage);
    m_detailStats->setObjectName(QStringLiteral("ModsMeta"));
    m_detailStats->setWordWrap(true);
    detailsLayout->addWidget(m_detailStats);

    m_detailTags = new QLabel(detailsPage);
    m_detailTags->setObjectName(QStringLiteral("ModsTag"));
    m_detailTags->setWordWrap(true);
    detailsLayout->addWidget(m_detailTags);

    detailsLayout->addStretch(1);

    auto *actionsRow = new QHBoxLayout();
    m_installBtn = new QPushButton(tr("Install"), detailsPage);
    m_installBtn->setObjectName(QStringLiteral("ModsPrimary"));
    m_updateBtn = new QPushButton(tr("Update"), detailsPage);
    m_removeBtn = new QPushButton(tr("Remove"), detailsPage);
    actionsRow->addWidget(m_installBtn);
    actionsRow->addWidget(m_updateBtn);
    actionsRow->addWidget(m_removeBtn);
    actionsRow->addStretch(1);
    detailsLayout->addLayout(actionsRow);

    m_detailsStack->addWidget(detailsPage);
    rightLayout->addWidget(m_detailsStack, 1);

    m_status->setObjectName(QStringLiteral("ModsHint"));
    m_status->setText(tr("Sign in to see mods."));
    mainLayout->addWidget(m_status);

    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 2);

    connect(refreshBtn, &QPushButton::clicked, this, &PluginsWindow::requestMods);
    connect(m_search, &QLineEdit::textChanged, this, &PluginsWindow::applyFilters);
    connect(m_category, &QComboBox::currentIndexChanged, this, &PluginsWindow::applyFilters);
    connect(m_list, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row < 0 || row >= m_filtered.size()) {
            m_detailsStack->setCurrentIndex(0);
            return;
        }
        showModDetails(m_filtered[row]);
    });
    connect(m_installBtn, &QPushButton::clicked, this, &PluginsWindow::installSelected);
    connect(m_updateBtn, &QPushButton::clicked, this, &PluginsWindow::updateSelected);
    connect(m_removeBtn, &QPushButton::clicked, this, &PluginsWindow::removeSelected);
}

void PluginsWindow::setAuthContext(const QString &baseUrl,
                                   const QString &token,
                                   const QString &role,
                                   const QString &user)
{
    m_baseUrl = baseUrl.trimmed();
    if (m_baseUrl.endsWith('/')) {
        m_baseUrl.chop(1);
    }
    m_token = token.trimmed();
    m_role = role.trimmed();
    m_user = user.trimmed();
    requestMods();
}

void PluginsWindow::updateStatus(const QString &text)
{
    if (m_status) {
        m_status->setText(text);
    }
}

PluginsWindow::ModEntry PluginsWindow::parseMod(const QJsonObject &obj) const
{
    ModEntry mod;
    mod.id = obj.value(QStringLiteral("id")).toString();
    mod.name = obj.value(QStringLiteral("name")).toString();
    mod.summary = obj.value(QStringLiteral("summary")).toString();
    mod.description = obj.value(QStringLiteral("description")).toString();
    mod.author = obj.value(QStringLiteral("author")).toString();
    mod.category = obj.value(QStringLiteral("category")).toString();
    mod.version = obj.value(QStringLiteral("version")).toString();
    mod.latestVersion = obj.value(QStringLiteral("latest_version")).toString();
    mod.iconUrl = obj.value(QStringLiteral("icon_url")).toString();
    mod.coverUrl = obj.value(QStringLiteral("cover_url")).toString();
    mod.sizeBytes = static_cast<qint64>(obj.value(QStringLiteral("size_bytes")).toDouble());
    mod.price = obj.value(QStringLiteral("price")).toDouble();
    mod.owned = obj.value(QStringLiteral("owned")).toBool();
    mod.installed = obj.value(QStringLiteral("installed")).toBool();
    mod.updateAvailable = obj.value(QStringLiteral("update_available")).toBool();
    const QString updatedRaw = obj.value(QStringLiteral("updated_at")).toString();
    if (!updatedRaw.isEmpty()) {
        mod.updatedAt = QDateTime::fromString(updatedRaw, Qt::ISODate);
    }
    const QJsonArray tags = obj.value(QStringLiteral("tags")).toArray();
    for (const QJsonValue &v : tags) {
        if (v.isString()) {
            mod.tags.push_back(v.toString());
        }
    }
    return mod;
}

QString PluginsWindow::formatSize(qint64 bytes)
{
    if (bytes <= 0) {
        return QStringLiteral("-");
    }
    const double kb = 1024.0;
    const double mb = kb * 1024.0;
    const double gb = mb * 1024.0;
    if (bytes >= gb) {
        return QStringLiteral("%1 GB").arg(bytes / gb, 0, 'f', 2);
    }
    if (bytes >= mb) {
        return QStringLiteral("%1 MB").arg(bytes / mb, 0, 'f', 1);
    }
    return QStringLiteral("%1 KB").arg(bytes / kb, 0, 'f', 0);
}

QString PluginsWindow::formatPrice(double price, bool owned)
{
    if (owned) {
        return QStringLiteral("Owned");
    }
    if (price <= 0.0) {
        return QStringLiteral("Free");
    }
    return QStringLiteral("$%1").arg(price, 0, 'f', 2);
}

void PluginsWindow::applyFilters()
{
    m_list->clear();
    m_filtered.clear();

    QString query = m_search->text().trimmed().toLower();
    QString category = m_category->currentText();
    if (category == tr("All")) {
        category.clear();
    }

    for (int i = 0; i < m_mods.size(); ++i) {
        const ModEntry &mod = m_mods[i];
        const QString name = mod.name.isEmpty() ? mod.id : mod.name;
        const QString summary = mod.summary;
        const QString haystack = (name + " " + summary + " " + mod.author + " " + mod.category).toLower();
        if (!query.isEmpty() && !haystack.contains(query)) {
            continue;
        }
        if (!category.isEmpty() && mod.category.compare(category, Qt::CaseInsensitive) != 0) {
            continue;
        }
        m_filtered.push_back(i);

        auto *item = new QListWidgetItem(m_list);
        item->setData(Qt::UserRole, i);
        item->setSizeHint(QSize(200, 62));

        auto *widget = new QWidget(m_list);
        auto *row = new QHBoxLayout(widget);
        row->setContentsMargins(8, 6, 8, 6);
        row->setSpacing(8);

        auto *textCol = new QVBoxLayout();
        auto *title = new QLabel(name, widget);
        title->setStyleSheet(QStringLiteral("font-weight: 700;"));
        textCol->addWidget(title);
        auto *desc = new QLabel(summary.isEmpty() ? tr("No description") : summary, widget);
        desc->setObjectName(QStringLiteral("ModsHint"));
        desc->setWordWrap(true);
        textCol->addWidget(desc);
        row->addLayout(textCol, 1);

        const QString badge = formatPrice(mod.price, mod.owned);
        auto *priceTag = new QLabel(badge, widget);
        priceTag->setObjectName(QStringLiteral("ModsMeta"));
        row->addWidget(priceTag, 0, Qt::AlignTop);

        if (mod.installed) {
            auto *inst = new QLabel(tr("Installed"), widget);
            inst->setObjectName(QStringLiteral("ModsMeta"));
            row->addWidget(inst, 0, Qt::AlignTop);
        }

        m_list->addItem(item);
        m_list->setItemWidget(item, widget);
    }

    if (m_filtered.isEmpty()) {
        updateStatus(tr("No mods match your filters."));
        m_detailsStack->setCurrentIndex(0);
        return;
    }
    updateStatus(tr("Mods loaded: %1").arg(m_filtered.size()));
    if (m_list->currentRow() < 0) {
        m_list->setCurrentRow(0);
    }
    const int selectedRow = m_list->currentRow();
    if (selectedRow >= 0 && selectedRow < m_filtered.size()) {
        showModDetails(m_filtered[selectedRow]);
    }
}

void PluginsWindow::showModDetails(int index)
{
    if (index < 0 || index >= m_mods.size()) {
        m_detailsStack->setCurrentIndex(0);
        return;
    }
    const ModEntry &mod = m_mods[index];
    const QString name = mod.name.isEmpty() ? mod.id : mod.name;

    m_detailTitle->setText(name);
    m_detailSummary->setText(mod.summary.isEmpty() ? tr("No short description") : mod.summary);
    m_detailDesc->setText(mod.description.isEmpty() ? tr("No detailed description") : mod.description);

    QString meta = QStringLiteral("Author: %1\nCategory: %2\nVersion: %3")
        .arg(mod.author.isEmpty() ? tr("Unknown") : mod.author)
        .arg(mod.category.isEmpty() ? tr("General") : mod.category)
        .arg(mod.version.isEmpty() ? QStringLiteral("-") : mod.version);
    if (!mod.latestVersion.isEmpty() && mod.latestVersion != mod.version) {
        meta += QStringLiteral("\nLatest: %1").arg(mod.latestVersion);
    }
    if (mod.updatedAt.isValid()) {
        meta += QStringLiteral("\nUpdated: %1")
                    .arg(QLocale().toString(mod.updatedAt.toLocalTime(), QLocale::ShortFormat));
    }
    m_detailMeta->setText(meta);

    const QString sizeText = formatSize(mod.sizeBytes);
    const QString priceText = formatPrice(mod.price, mod.owned);
    const QString stateText = mod.installed ? tr("Installed") : tr("Not installed");
    const QString updateText = mod.updateAvailable ? tr("Update available") : tr("Up to date");
    m_detailStats->setText(QStringLiteral("Size: %1\nPrice: %2\nState: %3\nUpdate: %4")
        .arg(sizeText, priceText, stateText, updateText));

    if (!mod.tags.isEmpty()) {
        m_detailTags->setText(tr("Tags: %1").arg(mod.tags.join(QStringLiteral(", "))));
    } else {
        m_detailTags->setText(tr("Tags: -"));
    }

    m_installBtn->setEnabled(!mod.installed && mod.owned);
    m_updateBtn->setEnabled(mod.installed && mod.updateAvailable);
    m_removeBtn->setEnabled(mod.installed);

    if (m_detailCover) {
        const QString coverUrl = mod.coverUrl.trimmed();
        if (coverUrl.isEmpty()) {
            m_loadedCoverUrl.clear();
            m_detailCover->setVisible(false);
            m_detailCover->setText(QString());
            m_detailCover->setPixmap(QPixmap());
        } else {
            if (coverUrl != m_loadedCoverUrl) {
                m_loadedCoverUrl = coverUrl;
                m_detailCover->setVisible(true);
                m_detailCover->setText(tr("Loading preview..."));
                m_detailCover->setPixmap(QPixmap());
                QNetworkRequest request{QUrl(coverUrl)};
                QNetworkReply *reply = m_net->get(request);
                connect(reply, &QNetworkReply::finished, this, [this, reply, coverUrl]() {
                    reply->deleteLater();
                    if (coverUrl != m_loadedCoverUrl || !m_detailCover) {
                        return;
                    }
                    if (reply->error() != QNetworkReply::NoError) {
                        m_detailCover->setText(tr("Preview unavailable"));
                        return;
                    }
                    QPixmap pix;
                    pix.loadFromData(reply->readAll());
                    if (pix.isNull()) {
                        m_detailCover->setText(tr("Preview unavailable"));
                        return;
                    }
                    m_detailCover->setPixmap(pix);
                    m_detailCover->setText(QString());
                });
            } else {
                m_detailCover->setVisible(true);
            }
        }
    }

    m_detailsStack->setCurrentIndex(1);
}

void PluginsWindow::requestMods()
{
    QString lastModId;
    const int currentIndex = currentModIndex();
    if (currentIndex >= 0 && currentIndex < m_mods.size()) {
        lastModId = m_mods[currentIndex].id;
    }

    m_mods.clear();
    m_filtered.clear();
    m_list->clear();
    m_detailsStack->setCurrentIndex(0);

    if (m_baseUrl.isEmpty()) {
        updateStatus(tr("Set server base URL in Sign in."));
        return;
    }
    if (m_token.isEmpty()) {
        updateStatus(tr("Sign in to see mods."));
        return;
    }

    updateStatus(tr("Loading mods..."));
    QNetworkRequest request{QUrl(m_baseUrl + QStringLiteral("/mods"))};
    request.setRawHeader("Authorization", QByteArray("Bearer ") + m_token.toUtf8());
    QNetworkReply *reply = m_net->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, lastModId]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            updateStatus(tr("Failed to load mods."));
            return;
        }
        const QByteArray raw = reply->readAll();
        const QJsonDocument doc = QJsonDocument::fromJson(raw);
        if (!doc.isArray()) {
            updateStatus(tr("Invalid mods response."));
            return;
        }
        const QJsonArray arr = doc.array();
        for (const QJsonValue &v : arr) {
            if (!v.isObject()) {
                continue;
            }
            m_mods.push_back(parseMod(v.toObject()));
        }

        QSignalBlocker blocker(m_category);
        m_category->clear();
        m_category->addItem(tr("All"));
        QStringList categories;
        for (const ModEntry &mod : m_mods) {
            if (!mod.category.isEmpty() && !categories.contains(mod.category, Qt::CaseInsensitive)) {
                categories.push_back(mod.category);
            }
        }
        categories.sort();
        for (const QString &cat : categories) {
            m_category->addItem(cat);
        }

        applyFilters();
        if (!lastModId.isEmpty()) {
            for (int row = 0; row < m_filtered.size(); ++row) {
                const int modIndex = m_filtered[row];
                if (modIndex >= 0 && modIndex < m_mods.size()
                    && m_mods[modIndex].id == lastModId) {
                    m_list->setCurrentRow(row);
                    break;
                }
            }
        }
        emitInstalledMods();
    });
}

void PluginsWindow::installSelected()
{
    const int index = currentModIndex();
    if (index < 0 || index >= m_mods.size()) {
        updateStatus(tr("Select a mod first."));
        return;
    }
    const ModEntry &mod = m_mods[index];
    if (mod.installed) {
        updateStatus(tr("Mod already installed."));
        return;
    }
    if (!mod.owned && mod.price > 0.0) {
        updateStatus(tr("Purchase required."));
        return;
    }
    sendModAction(mod.id, QStringLiteral("/mods/install"), tr("Mod installed."));
}

void PluginsWindow::updateSelected()
{
    const int index = currentModIndex();
    if (index < 0 || index >= m_mods.size()) {
        updateStatus(tr("Select a mod first."));
        return;
    }
    const ModEntry &mod = m_mods[index];
    if (!mod.installed) {
        updateStatus(tr("Install the mod first."));
        return;
    }
    if (!mod.updateAvailable) {
        updateStatus(tr("Already up to date."));
        return;
    }
    sendModAction(mod.id, QStringLiteral("/mods/install"), tr("Mod updated."));
}

void PluginsWindow::removeSelected()
{
    const int index = currentModIndex();
    if (index < 0 || index >= m_mods.size()) {
        updateStatus(tr("Select a mod first."));
        return;
    }
    const ModEntry &mod = m_mods[index];
    if (!mod.installed) {
        updateStatus(tr("Mod is not installed."));
        return;
    }
    sendModAction(mod.id, QStringLiteral("/mods/remove"), tr("Mod removed."));
}

void PluginsWindow::sendModAction(const QString &modId,
                                  const QString &endpoint,
                                  const QString &successMessage)
{
    if (m_baseUrl.isEmpty()) {
        updateStatus(tr("Set server base URL in Sign in."));
        return;
    }
    if (m_token.isEmpty()) {
        updateStatus(tr("Sign in to manage mods."));
        return;
    }

    updateStatus(tr("Working..."));
    m_installBtn->setEnabled(false);
    m_updateBtn->setEnabled(false);
    m_removeBtn->setEnabled(false);

    QNetworkRequest request{QUrl(m_baseUrl + endpoint)};
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Authorization", QByteArray("Bearer ") + m_token.toUtf8());
    QJsonObject payload;
    payload.insert(QStringLiteral("mod_id"), modId);
    QNetworkReply *reply =
        m_net->post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));

    connect(reply, &QNetworkReply::finished, this, [this, reply, successMessage]() {
        reply->deleteLater();
        const QByteArray raw = reply->readAll();
        QString errText;
        if (!raw.isEmpty()) {
            const QJsonDocument doc = QJsonDocument::fromJson(raw);
            if (doc.isObject()) {
                errText = doc.object().value(QStringLiteral("error")).toString();
            }
        }

        if (reply->error() != QNetworkReply::NoError) {
            updateStatus(errText.isEmpty() ? tr("Request failed.") : errText);
            const int index = currentModIndex();
            if (index >= 0 && index < m_mods.size()) {
                showModDetails(index);
            }
            return;
        }
        if (!errText.isEmpty()) {
            updateStatus(tr("Request failed: %1").arg(errText));
            const int index = currentModIndex();
            if (index >= 0 && index < m_mods.size()) {
                showModDetails(index);
            }
            return;
        }
        updateStatus(successMessage);
        requestMods();
    });
}

int PluginsWindow::currentModIndex() const
{
    if (!m_list) {
        return -1;
    }
    const int row = m_list->currentRow();
    if (row < 0 || row >= m_filtered.size()) {
        return -1;
    }
    return m_filtered[row];
}

void PluginsWindow::emitInstalledMods()
{
    QStringList installed;
    installed.reserve(m_mods.size());
    for (const ModEntry &mod : m_mods) {
        if (mod.installed) {
            installed.push_back(mod.id);
        }
    }
    emit installedModsChanged(installed);
}
