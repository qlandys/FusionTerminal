#pragma once

#include <QDialog>
#include <QDateTime>
#include <QPoint>
#include <QVector>

class QLabel;
class QListWidget;
class QLineEdit;
class QPushButton;
class QNetworkAccessManager;
class QSizeGrip;
class QTimer;
class QWidget;

class ChatWindow : public QDialog {
    Q_OBJECT

public:
    explicit ChatWindow(QWidget *parent = nullptr);

    void setAuthContext(const QString &baseUrl,
                        const QString &token,
                        const QString &user);
    void setChatKey(const QString &key, const QString &title);
    void setDocked(bool docked);
    bool isDocked() const { return m_docked; }
    void startFloatingDrag(const QPoint &globalPos, const QPoint &offset);

signals:
    void dragMoved(const QPoint &globalPos);
    void dragReleased(const QPoint &globalPos);
    void requestUndock(ChatWindow *window, const QPoint &globalPos, const QPoint &offset);
    void requestClose(ChatWindow *window);

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    struct ChatItem {
        QString user;
        QString text;
        QDateTime ts;
    };

    void requestHistory();
    void sendMessage();
    void renderMessages(const QVector<ChatItem> &items);
    void updateStatus(const QString &text, bool isError);
    QString messageKey(const ChatItem &item) const;

    QString m_baseUrl;
    QString m_token;
    QString m_user;
    QString m_key;

    QWidget *m_header = nullptr;
    QLabel *m_title = nullptr;
    QLabel *m_status = nullptr;
    QListWidget *m_list = nullptr;
    QLineEdit *m_input = nullptr;
    QPushButton *m_send = nullptr;
    QSizeGrip *m_sizeGrip = nullptr;
    QTimer *m_refreshTimer = nullptr;
    QNetworkAccessManager *m_net = nullptr;
    QVector<QString> m_messageKeys;

    bool m_dragging = false;
    bool m_docked = false;
    bool m_undockRequested = false;
    QPoint m_dragStartGlobal;
    QPoint m_dragStartWindowPos;
    QPoint m_dragStartOffset;
};
