#pragma once

#include <QDialog>
#include <QDateTime>
#include <QStringList>
#include <QVector>

class QListWidget;
class QLabel;
class QLineEdit;
class QComboBox;
class QPushButton;
class QStackedWidget;

class PluginsWindow : public QDialog {
    Q_OBJECT

public:
    explicit PluginsWindow(QWidget *parent = nullptr);
    void setAuthContext(const QString &baseUrl,
                        const QString &token,
                        const QString &role,
                        const QString &user);

signals:
    void installedModsChanged(const QStringList &installedMods);

private:
    struct ModEntry {
        QString id;
        QString name;
        QString summary;
        QString description;
        QString author;
        QString category;
        QString version;
        QString latestVersion;
        QStringList tags;
        QString iconUrl;
        QString coverUrl;
        qint64 sizeBytes = 0;
        double price = 0.0;
        bool owned = false;
        bool installed = false;
        bool updateAvailable = false;
        QDateTime updatedAt;
    };

    void requestMods();
    void updateStatus(const QString &text);
    void applyFilters();
    void showModDetails(int index);
    void installSelected();
    void updateSelected();
    void removeSelected();
    void sendModAction(const QString &modId, const QString &endpoint, const QString &successMessage);
    int currentModIndex() const;
    void emitInstalledMods();
    ModEntry parseMod(const QJsonObject &obj) const;
    static QString formatSize(qint64 bytes);
    static QString formatPrice(double price, bool owned);

    QListWidget *m_list;
    QLabel *m_status;
    QLineEdit *m_search;
    QComboBox *m_category;
    QStackedWidget *m_detailsStack = nullptr;
    QLabel *m_detailTitle = nullptr;
    QLabel *m_detailSummary = nullptr;
    QLabel *m_detailCover = nullptr;
    QLabel *m_detailDesc = nullptr;
    QLabel *m_detailMeta = nullptr;
    QLabel *m_detailStats = nullptr;
    QLabel *m_detailTags = nullptr;
    QPushButton *m_installBtn = nullptr;
    QPushButton *m_updateBtn = nullptr;
    QPushButton *m_removeBtn = nullptr;
    QVector<ModEntry> m_mods;
    QVector<int> m_filtered;
    QString m_loadedCoverUrl;
    QString m_baseUrl;
    QString m_token;
    QString m_role;
    QString m_user;
    class QNetworkAccessManager *m_net = nullptr;
};
