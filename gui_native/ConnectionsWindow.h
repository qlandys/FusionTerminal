#pragma once

#include <QDialog>
#include <QComboBox>
#include "TradeManager.h"
#include <QVector>
#include <QColor>
#include <QPoint>

class QCheckBox;
class QLineEdit;
class QPlainTextEdit;
class QLabel;
class QPushButton;
class QAction;
class ConnectionStore;
class QVBoxLayout;
class QToolButton;
class QSpinBox;
class QCheckBox;
class QToolButton;
class QWidget;
class QCloseEvent;
class QShowEvent;

class ConnectionsWindow : public QDialog {
    Q_OBJECT

public:
    ConnectionsWindow(ConnectionStore *store, TradeManager *manager, QWidget *parent = nullptr);

    void refreshUi();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void showEvent(QShowEvent *event) override;

public slots:
    void handleManagerStateChanged(ConnectionStore::Profile profile,
                                   TradeManager::ConnectionState state,
                                   const QString &message);
    void appendLogMessage(const QString &message);

private slots:
    void handleConnectClicked(const QString &profileId);
    void handleDisconnectClicked(const QString &profileId);

private:
    void launchLighterSetup();
    static QString writeLighterWizardScript(const QString &dirPath);
    QString lighterConfigPath() const;
    void saveLighterProxyToVaultIfPossible(const MexcCredentials &creds, bool log);
    void updateTitleCounts();
    struct CardWidgets;
    bool loadLighterConfigIntoCard(CardWidgets *card, const QString &path, bool log);
    void openLighterConfigEditor(CardWidgets *card);
    struct CardWidgets {
        QString profileId;
        ConnectionStore::Profile type{ConnectionStore::Profile::MexcSpot};
        QWidget *statusDot = nullptr;
        QLabel *statusBadge = nullptr;
        QLabel *iconLabel = nullptr;
        QLabel *nameLabel = nullptr;
        QLabel *exchangeLabel = nullptr;
        QLineEdit *profileNameEdit = nullptr;
        QLineEdit *apiKeyEdit = nullptr;
        QLineEdit *secretEdit = nullptr;
        QLineEdit *passphraseEdit = nullptr;
        QLineEdit *uidEdit = nullptr;
        QLineEdit *proxyEdit = nullptr;
        QToolButton *proxyPresetButton = nullptr;
        QWidget *proxyGeoWidget = nullptr;
        QLabel *proxyGeoIconLabel = nullptr;
        QLabel *proxyGeoTextLabel = nullptr;
        QLineEdit *proxyNameEdit = nullptr;
        QAction *proxySaveAction = nullptr;
        QLabel *proxyStatusLabel = nullptr;
        QToolButton *proxySaveButton = nullptr;
        QToolButton *proxyCheckButton = nullptr;
        QComboBox *proxyTypeCombo = nullptr;
        QToolButton *proxyGoogleIcon = nullptr;
        QToolButton *proxyFacebookIcon = nullptr;
        QToolButton *proxyYandexIcon = nullptr;
        QLineEdit *baseUrlEdit = nullptr;
        QLineEdit *accountIndexEdit = nullptr;
        QLineEdit *apiKeyIndexEdit = nullptr;
        QLineEdit *seedPhraseEdit = nullptr;
        QLineEdit *lighterConfigPathEdit = nullptr;
        QToolButton *lighterConfigEditButton = nullptr;
        QToolButton *lighterConfigReloadButton = nullptr;
        QToolButton *lighterSetupButton = nullptr;
        QToolButton *lighterMainnetButton = nullptr;
        QToolButton *lighterTestnetButton = nullptr;
        QCheckBox *saveSecretCheck = nullptr;
        QCheckBox *viewOnlyCheck = nullptr;
        QCheckBox *autoConnectCheck = nullptr;
        QToolButton *colorButton = nullptr;
        QToolButton *expandButton = nullptr;
        QToolButton *menuButton = nullptr;
        QToolButton *importButton = nullptr;
        QPushButton *connectButton = nullptr; // visible toggle connect/disconnect
        QPushButton *disconnectButton = nullptr; // legacy, kept hidden
        QColor color;
        QWidget *body = nullptr;
        QWidget *frame = nullptr;
        bool expanded = false;
        TradeManager::ConnectionState currentState = TradeManager::ConnectionState::Disconnected;
        qint64 lastConnectingAtMs = 0;
        bool pendingConnectedApply = false;
    };

    void rebuildCardsFromStore();
    void deleteCard(CardWidgets *card);

    CardWidgets *createCard(const QString &profileId, ConnectionStore::Profile type);
    CardWidgets *ensureCard(const QString &profileId, ConnectionStore::Profile type);
    void applyState(ConnectionStore::Profile profile,
                    TradeManager::ConnectionState state,
                    const QString &message);
    MexcCredentials collectCredentials(const CardWidgets &card) const;
    void persistCard(const CardWidgets &card);
    void setCardExpanded(CardWidgets *card, bool expanded);
    void moveCard(CardWidgets *card, int delta);
    void rebuildLayout();

    ConnectionStore *m_store = nullptr;
    TradeManager *m_manager = nullptr;

    QVector<CardWidgets *> m_cards;
    QVBoxLayout *m_cardsLayout = nullptr;
    QWidget *m_cardsContainer = nullptr;
    QPlainTextEdit *m_logView = nullptr;

    QWidget *m_titleBar = nullptr;
    QLabel *m_titleLabel = nullptr;
    QPoint m_dragStartGlobal;
    QPoint m_dragStartWindowPos;
    bool m_dragging = false;
    bool m_firstShow = true;
    bool m_backdropQueued = false;
};
