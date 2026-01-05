#pragma once

#include <QObject>
#include <QVector>
#include <QStringList>
#include "TradeTypes.h"

class ConnectionStore : public QObject {
    Q_OBJECT

public:
    enum class Profile {
        MexcSpot,
        MexcFutures,
        UzxSpot,
        UzxSwap,
        BinanceSpot,
        BinanceFutures,
        Lighter
    };

    struct StoredProfile {
        QString id;
        Profile type{Profile::MexcSpot};
        MexcCredentials creds;
    };

    explicit ConnectionStore(QObject *parent = nullptr);

    MexcCredentials loadMexcCredentials(Profile profile = Profile::MexcSpot) const;
    void saveMexcCredentials(const MexcCredentials &creds, Profile profile = Profile::MexcSpot);
    QString storagePath() const;

    void ensureProfilesSchema();

    QVector<StoredProfile> allProfiles() const;
    QVector<StoredProfile> profiles(Profile type) const;
    StoredProfile profileById(const QString &id) const;

    QStringList profileOrder() const;
    void moveProfile(const QString &id, int delta);

    QString activeProfileId(Profile type) const;
    void setActiveProfileId(Profile type, const QString &id);

    StoredProfile createProfile(Profile type, const QString &label, const MexcCredentials &initial = MexcCredentials{});
    void saveProfile(const StoredProfile &profile);
    void deleteProfile(const QString &id);

signals:
    void credentialsChanged(const QString &profileKey, const MexcCredentials &creds);

private:
    QString profileKey(Profile profile) const;
    QString credentialsFilePath() const;
};
