#include "PluginsWindow.h"

#include <QLabel>
#include <QListWidget>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPushButton>
#include <QVBoxLayout>

PluginsWindow::PluginsWindow(QWidget *parent)
    : QDialog(parent)
    , m_list(new QListWidget(this))
    , m_status(new QLabel(this))
    , m_net(new QNetworkAccessManager(this))
{
    setWindowTitle(tr("Fusion Terminal - Mods"));
    setModal(false);
    resize(620, 420);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    auto *title = new QLabel(tr("Installed mods"), this);
    layout->addWidget(title);

    m_status->setText(tr("Sign in to see mods."));
    layout->addWidget(m_status);

    layout->addWidget(m_list, 1);

    auto *closeButton = new QPushButton(tr("Close"), this);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::close);
    layout->addWidget(closeButton);
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

void PluginsWindow::requestMods()
{
    m_list->clear();
    if (m_baseUrl.isEmpty()) {
        updateStatus(tr("Set server base URL in Sign in."));
        return;
    }
    if (m_token.isEmpty()) {
        updateStatus(tr("Sign in to see mods."));
        return;
    }

    updateStatus(tr("Loading mods..."));
    QNetworkRequest req(QUrl(m_baseUrl + QStringLiteral("/mods")));
    req.setRawHeader("Authorization", QByteArray("Bearer ") + m_token.toUtf8());
    QNetworkReply *reply = m_net->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
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
        if (arr.isEmpty()) {
            updateStatus(tr("No mods available."));
            return;
        }
        updateStatus(tr("Available mods: %1").arg(arr.size()));
        for (const QJsonValue &v : arr) {
            if (!v.isObject()) {
                continue;
            }
            const QJsonObject obj = v.toObject();
            const QString name = obj.value(QStringLiteral("name")).toString();
            const QString id = obj.value(QStringLiteral("id")).toString();
            const bool owned = obj.value(QStringLiteral("owned")).toBool();
            const double price = obj.value(QStringLiteral("price")).toDouble();
            QString label = name.isEmpty() ? id : name;
            if (!label.isEmpty()) {
                label += QStringLiteral(" ");
            }
            label += owned ? QStringLiteral("[owned]") : (price <= 0.0 ? QStringLiteral("[free]") : QStringLiteral("[locked]"));
            m_list->addItem(label);
        }
    });
}
