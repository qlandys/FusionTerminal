#include "ChatWindow.h"

#include <QAbstractItemView>
#include <QEvent>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QScrollBar>
#include <QSizeGrip>
#include <QTimer>
#include <QToolButton>
#include <QUrlQuery>
#include <QVBoxLayout>

ChatWindow::ChatWindow(QWidget *parent)
    : QDialog(parent)
    , m_net(new QNetworkAccessManager(this))
{
    setObjectName(QStringLiteral("ChatWindow"));
    setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    setAttribute(Qt::WA_NativeWindow, true);
    setAttribute(Qt::WA_TransparentForMouseEvents, false);
    setAttribute(Qt::WA_DeleteOnClose, false);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    winId();
    resize(320, 420);

    setStyleSheet(QStringLiteral(
        "QDialog#ChatWindow { background-color: #1c1c1c; color: #e0e0e0; }"
        "QWidget#ChatHeader { background-color: #232323; border-bottom: 1px solid #2b2b2b; }"
        "QLabel#ChatTitle { font-size: 13px; font-weight: 700; letter-spacing: 0.5px; }"
        "QLabel#ChatStatus { color: #9aa0a6; font-size: 11px; }"
        "QLabel#ChatStatus[status=\"error\"] { color: #f16c6c; }"
        "QListWidget#ChatList { border: none; background: #1c1c1c; }"
        "QLabel#ChatUser { color: #79b8ff; font-weight: 600; }"
        "QLabel#ChatText { color: #d5d7da; }"
        "QLabel#ChatTime { color: #7d8590; font-size: 10px; }"
        "QLineEdit#ChatInput { background: #232323; border: 1px solid #2b2b2b; border-radius: 6px; padding: 6px; }"
        "QPushButton#ChatSend { background: #2e8bdc; color: #ffffff; border-radius: 6px; padding: 6px 10px; }"
        "QPushButton#ChatSend:disabled { background: #35516a; color: #9aa0a6; }"
        "QToolButton#ChatClose { background: transparent; color: #cfcfcf; border: none; }"
        "QToolButton#ChatClose:hover { color: #ffffff; }"
    ));

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    m_header = new QWidget(this);
    m_header->setObjectName(QStringLiteral("ChatHeader"));
    m_header->installEventFilter(this);
    auto *headerLayout = new QHBoxLayout(m_header);
    headerLayout->setContentsMargins(10, 6, 8, 6);
    headerLayout->setSpacing(6);

    m_title = new QLabel(tr("CHAT"), m_header);
    m_title->setObjectName(QStringLiteral("ChatTitle"));
    headerLayout->addWidget(m_title, 1);

    auto *closeBtn = new QToolButton(m_header);
    closeBtn->setObjectName(QStringLiteral("ChatClose"));
    closeBtn->setText(QStringLiteral("x"));
    closeBtn->setCursor(Qt::PointingHandCursor);
    closeBtn->setFixedSize(18, 18);
    headerLayout->addWidget(closeBtn, 0, Qt::AlignRight);
    connect(closeBtn, &QToolButton::clicked, this, [this]() {
        emit requestClose(this);
    });

    root->addWidget(m_header);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("ChatStatus"));
    m_status->setProperty("status", QStringLiteral("info"));
    m_status->setText(tr("Ready"));
    m_status->setContentsMargins(10, 6, 10, 0);
    root->addWidget(m_status);

    m_list = new QListWidget(this);
    m_list->setObjectName(QStringLiteral("ChatList"));
    m_list->setSelectionMode(QAbstractItemView::NoSelection);
    m_list->setFocusPolicy(Qt::NoFocus);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    root->addWidget(m_list, 1);

    auto *inputRow = new QHBoxLayout();
    inputRow->setContentsMargins(10, 8, 10, 10);
    inputRow->setSpacing(8);

    m_input = new QLineEdit(this);
    m_input->setObjectName(QStringLiteral("ChatInput"));
    m_input->setPlaceholderText(tr("Typing..."));
    inputRow->addWidget(m_input, 1);

    m_send = new QPushButton(tr("Send"), this);
    m_send->setObjectName(QStringLiteral("ChatSend"));
    inputRow->addWidget(m_send);

    root->addLayout(inputRow);

    connect(m_send, &QPushButton::clicked, this, &ChatWindow::sendMessage);
    connect(m_input, &QLineEdit::returnPressed, this, &ChatWindow::sendMessage);

    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setInterval(5000);
    connect(m_refreshTimer, &QTimer::timeout, this, &ChatWindow::requestHistory);

    m_sizeGrip = new QSizeGrip(this);
    m_sizeGrip->setFixedSize(14, 14);
    m_sizeGrip->setCursor(Qt::SizeFDiagCursor);
    m_sizeGrip->raise();
}

void ChatWindow::setAuthContext(const QString &baseUrl,
                                const QString &token,
                                const QString &user)
{
    m_baseUrl = baseUrl.trimmed();
    if (m_baseUrl.endsWith('/')) {
        m_baseUrl.chop(1);
    }
    m_token = token.trimmed();
    m_user = user.trimmed();
    requestHistory();
}

void ChatWindow::setChatKey(const QString &key, const QString &title)
{
    m_key = key.trimmed();
    m_title->setText(title);
    requestHistory();
}

void ChatWindow::setDocked(bool docked)
{
    if (m_docked == docked) {
        return;
    }
    m_docked = docked;
    if (m_docked) {
        setWindowFlags(Qt::Widget);
        setAttribute(Qt::WA_NativeWindow, false);
    } else {
        setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
        setAttribute(Qt::WA_NativeWindow, true);
        setAttribute(Qt::WA_TransparentForMouseEvents, false);
        setMouseTracking(true);
        setFocusPolicy(Qt::StrongFocus);
        winId();
    }
    show();
}

void ChatWindow::startFloatingDrag(const QPoint &globalPos, const QPoint &offset)
{
    m_dragging = true;
    m_undockRequested = false;
    m_dragStartGlobal = globalPos;
    m_dragStartOffset = offset;
    m_dragStartWindowPos = frameGeometry().topLeft();
}

bool ChatWindow::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == m_header) {
        if (event->type() == QEvent::UngrabMouse || event->type() == QEvent::WindowDeactivate) {
            if (m_dragging) {
                m_dragging = false;
                m_undockRequested = false;
                m_header->releaseMouse();
            }
            return false;
        }
        if (event->type() == QEvent::MouseButtonPress) {
            auto *me = static_cast<QMouseEvent *>(event);
            if (me->button() == Qt::LeftButton) {
                m_header->grabMouse();
                m_dragging = true;
                m_undockRequested = false;
                m_dragStartGlobal = me->globalPos();
                m_dragStartWindowPos = frameGeometry().topLeft();
                m_dragStartOffset = me->globalPos() - frameGeometry().topLeft();
                return true;
            }
        } else if (event->type() == QEvent::MouseMove) {
            if (m_dragging) {
                auto *me = static_cast<QMouseEvent *>(event);
                const QPoint delta = me->globalPos() - m_dragStartGlobal;
                if (m_docked) {
                    if (!m_undockRequested && delta.manhattanLength() > 6) {
                        m_undockRequested = true;
                        emit requestUndock(this, me->globalPos(), m_dragStartOffset);
                    }
                    return true;
                }
                move(m_dragStartWindowPos + delta);
                raise();
                emit dragMoved(me->globalPos());
                return true;
            }
        } else if (event->type() == QEvent::MouseButtonRelease) {
            if (!m_docked && m_dragging) {
                auto *me = static_cast<QMouseEvent *>(event);
                emit dragReleased(me->globalPos());
            }
            m_dragging = false;
            m_undockRequested = false;
            m_header->releaseMouse();
            return true;
        }
    }
    return QDialog::eventFilter(obj, event);
}

void ChatWindow::resizeEvent(QResizeEvent *event)
{
    QDialog::resizeEvent(event);
    if (!m_sizeGrip) {
        return;
    }
    const int margin = 4;
    const QSize gripSize = m_sizeGrip->size();
    m_sizeGrip->move(width() - gripSize.width() - margin, height() - gripSize.height() - margin);
    m_sizeGrip->raise();
}

void ChatWindow::updateStatus(const QString &text, bool isError)
{
    if (!m_status) {
        return;
    }
    m_status->setText(text);
    m_status->setProperty("status", isError ? QStringLiteral("error") : QStringLiteral("info"));
    m_status->style()->unpolish(m_status);
    m_status->style()->polish(m_status);
}

void ChatWindow::requestHistory()
{
    if (m_baseUrl.isEmpty() || m_token.isEmpty()) {
        updateStatus(tr("Sign in to use chat."), true);
        return;
    }
    if (m_key.isEmpty()) {
        updateStatus(tr("Chat key is missing."), true);
        return;
    }

    QUrl url(m_baseUrl + QStringLiteral("/chat/history"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("key"), m_key);
    url.setQuery(query);

    QNetworkRequest req(url);
    req.setRawHeader("Authorization", QByteArray("Bearer ") + m_token.toUtf8());
    QNetworkReply *reply = m_net->get(req);

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError || status >= 400) {
            const QByteArray raw = reply->readAll();
            const QJsonDocument doc = QJsonDocument::fromJson(raw);
            const QString err =
                doc.isObject() ? doc.object().value(QStringLiteral("error")).toString() : QString();
            if (status == 403 || err == QStringLiteral("mod_not_installed")) {
                updateStatus(tr("Install the chat mod to use this feature."), true);
            } else if (status == 401) {
                updateStatus(tr("Sign in to use chat."), true);
            } else {
                updateStatus(tr("Failed to load chat."), true);
            }
            return;
        }
        const QByteArray raw = reply->readAll();
        const QJsonDocument doc = QJsonDocument::fromJson(raw);
        if (!doc.isArray()) {
            updateStatus(tr("Invalid chat response."), true);
            return;
        }
        QVector<ChatItem> items;
        const QJsonArray arr = doc.array();
        items.reserve(arr.size());
        for (const QJsonValue &v : arr) {
            if (!v.isObject()) {
                continue;
            }
            const QJsonObject obj = v.toObject();
            ChatItem item;
            item.user = obj.value(QStringLiteral("user")).toString();
            item.text = obj.value(QStringLiteral("text")).toString();
            const qint64 ts = static_cast<qint64>(obj.value(QStringLiteral("created_at")).toDouble());
            if (ts > 0) {
                item.ts = QDateTime::fromSecsSinceEpoch(ts, Qt::UTC).toLocalTime();
            }
            items.push_back(item);
        }
        renderMessages(items);
        updateStatus(tr(""), false);
        if (!m_refreshTimer->isActive()) {
            m_refreshTimer->start();
        }
    });
}

void ChatWindow::renderMessages(const QVector<ChatItem> &items)
{
    QVector<QString> keys;
    keys.reserve(items.size());
    for (const ChatItem &item : items) {
        keys.push_back(messageKey(item));
    }
    if (keys == m_messageKeys) {
        return;
    }

    int prefix = 0;
    while (prefix < m_messageKeys.size() && prefix < keys.size()
           && m_messageKeys[prefix] == keys[prefix]) {
        ++prefix;
    }
    if (prefix == m_messageKeys.size() && keys.size() >= m_messageKeys.size()) {
        const bool atBottom = m_list->verticalScrollBar()->value()
                                  >= m_list->verticalScrollBar()->maximum() - 2;
        m_list->setUpdatesEnabled(false);
        for (int i = prefix; i < items.size(); ++i) {
            const ChatItem &item = items[i];
            auto *row = new QWidget(m_list);
            auto *rowLayout = new QHBoxLayout(row);
            rowLayout->setContentsMargins(8, 6, 8, 6);
            rowLayout->setSpacing(8);

            auto *leftCol = new QVBoxLayout();
            leftCol->setSpacing(2);
            auto *userLabel = new QLabel(item.user.isEmpty() ? tr("user") : item.user, row);
            userLabel->setObjectName(QStringLiteral("ChatUser"));
            auto *textLabel = new QLabel(item.text, row);
            textLabel->setObjectName(QStringLiteral("ChatText"));
            textLabel->setWordWrap(true);
            leftCol->addWidget(userLabel);
            leftCol->addWidget(textLabel);
            rowLayout->addLayout(leftCol, 1);

            auto *timeLabel = new QLabel(item.ts.isValid() ? item.ts.toString(QStringLiteral("HH:mm"))
                                                           : QString(),
                                         row);
            timeLabel->setObjectName(QStringLiteral("ChatTime"));
            rowLayout->addWidget(timeLabel, 0, Qt::AlignTop);

            auto *listItem = new QListWidgetItem(m_list);
            listItem->setSizeHint(row->sizeHint());
            m_list->addItem(listItem);
            m_list->setItemWidget(listItem, row);
        }
        m_list->setUpdatesEnabled(true);
        if (atBottom) {
            m_list->scrollToBottom();
        }
        m_messageKeys = std::move(keys);
        return;
    }

    const bool atBottom = m_list->verticalScrollBar()->value()
                              >= m_list->verticalScrollBar()->maximum() - 2;
    m_list->setUpdatesEnabled(false);
    m_list->clear();
    for (const ChatItem &item : items) {
        auto *row = new QWidget(m_list);
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(8, 6, 8, 6);
        rowLayout->setSpacing(8);

        auto *leftCol = new QVBoxLayout();
        leftCol->setSpacing(2);
        auto *userLabel = new QLabel(item.user.isEmpty() ? tr("user") : item.user, row);
        userLabel->setObjectName(QStringLiteral("ChatUser"));
        auto *textLabel = new QLabel(item.text, row);
        textLabel->setObjectName(QStringLiteral("ChatText"));
        textLabel->setWordWrap(true);
        leftCol->addWidget(userLabel);
        leftCol->addWidget(textLabel);
        rowLayout->addLayout(leftCol, 1);

        auto *timeLabel = new QLabel(item.ts.isValid() ? item.ts.toString(QStringLiteral("HH:mm"))
                                                       : QString(),
                                     row);
        timeLabel->setObjectName(QStringLiteral("ChatTime"));
        rowLayout->addWidget(timeLabel, 0, Qt::AlignTop);

        auto *listItem = new QListWidgetItem(m_list);
        listItem->setSizeHint(row->sizeHint());
        m_list->addItem(listItem);
        m_list->setItemWidget(listItem, row);
    }
    m_list->setUpdatesEnabled(true);
    if (atBottom) {
        m_list->scrollToBottom();
    }
    m_messageKeys = std::move(keys);
}

void ChatWindow::sendMessage()
{
    if (m_baseUrl.isEmpty() || m_token.isEmpty()) {
        updateStatus(tr("Sign in to use chat."), true);
        return;
    }
    if (m_key.isEmpty()) {
        updateStatus(tr("Chat key is missing."), true);
        return;
    }
    const QString text = m_input->text().trimmed();
    if (text.isEmpty()) {
        return;
    }
    m_send->setEnabled(false);

    QNetworkRequest req(QUrl(m_baseUrl + QStringLiteral("/chat/send")));
    req.setRawHeader("Authorization", QByteArray("Bearer ") + m_token.toUtf8());
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    const QJsonObject obj{
        {QStringLiteral("key"), m_key},
        {QStringLiteral("text"), text},
    };
    QNetworkReply *reply = m_net->post(req, QJsonDocument(obj).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        m_send->setEnabled(true);
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError || status >= 400) {
            updateStatus(tr("Send failed."), true);
            return;
        }
        m_input->clear();
        requestHistory();
    });
}

QString ChatWindow::messageKey(const ChatItem &item) const
{
    const QString ts = item.ts.isValid()
        ? QString::number(item.ts.toSecsSinceEpoch())
        : QStringLiteral("0");
    return ts + QChar(0x1F) + item.user + QChar(0x1F) + item.text;
}
