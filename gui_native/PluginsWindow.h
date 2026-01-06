#pragma once

#include <QDialog>

class QListWidget;
class QLabel;

class PluginsWindow : public QDialog {
    Q_OBJECT

public:
    explicit PluginsWindow(QWidget *parent = nullptr);
    void setAuthContext(const QString &baseUrl,
                        const QString &token,
                        const QString &role,
                        const QString &user);

private:
    void requestMods();
    void updateStatus(const QString &text);

    QListWidget *m_list;
    QLabel *m_status;
    QString m_baseUrl;
    QString m_token;
    QString m_role;
    QString m_user;
    class QNetworkAccessManager *m_net = nullptr;
};
