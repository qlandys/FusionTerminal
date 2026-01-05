#include "ConnectionStore.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>

#ifdef _WIN32
#  include <windows.h>
#  include <wincrypt.h>
#endif

namespace {
constexpr int kSchemaVersion = 2;
const QString kSchemaVersionKey = QStringLiteral("schemaVersion");
const QString kProfilesKey = QStringLiteral("profiles");
const QString kOrderKey = QStringLiteral("order");
const QString kActiveKey = QStringLiteral("active");
const QString kTypeKey = QStringLiteral("type");

QString primaryConfigDir()
{
    const QString roaming = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    const QString local = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);

    auto hasConnections = [](const QString &dir) -> bool {
        if (dir.trimmed().isEmpty()) {
            return false;
        }
        return QFile::exists(dir + QLatin1String("/connections.json"));
    };

    QString path;
    if (hasConnections(roaming)) {
        path = roaming;
    } else if (hasConnections(local)) {
        path = local;
    } else if (!roaming.isEmpty()) {
        path = roaming;
    } else if (!local.isEmpty()) {
        path = local;
    } else {
        path = QDir::homePath() + QLatin1String("/.fusion_terminal");
    }

    QDir().mkpath(path);
    return path;
}

QStringList probeLegacyAppConfigDirs(const QString &primaryDir)
{
    QStringList out;
    const QFileInfo fi(primaryDir);
    const QDir parentDir = fi.dir();
    if (!parentDir.exists()) {
        return out;
    }
    const QStringList entries =
        parentDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &name : entries) {
        if (!name.contains(QStringLiteral("shah"), Qt::CaseInsensitive)) {
            continue;
        }
        out << parentDir.absoluteFilePath(name);
    }
    return out;
}

QString legacyPlasmaConfigDir(QStandardPaths::StandardLocation loc)
{
    const QString current = QStandardPaths::writableLocation(loc);
    if (current.isEmpty()) {
        return {};
    }
    QDir appDir(current);
    if (!appDir.cdUp()) {
        return {};
    }
    QDir orgDir(appDir);
    if (!orgDir.cdUp()) {
        return {};
    }
    const QString root = orgDir.absolutePath();
    return root + QDir::separator()
        + QStringLiteral("Plasma")
        + QDir::separator()
        + QStringLiteral("PlasmaTerminal");
}

QString resolveCredentialsFilePath(const QString &primaryDir)
{
    const QString primaryFile = primaryDir + QLatin1String("/connections.json");
    if (QFile::exists(primaryFile)) {
        return primaryFile;
    }

    QStringList candidates;
    candidates << (primaryDir + QLatin1String("/connections.json"));
    {
        const QString localDir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        if (!localDir.isEmpty()) {
            candidates << (localDir + QLatin1String("/connections.json"));
        }
    }
    candidates << (QDir::homePath() + QLatin1String("/.fusion_terminal/connections.json"));
    const QString plasmaRoaming = legacyPlasmaConfigDir(QStandardPaths::AppConfigLocation);
    if (!plasmaRoaming.isEmpty()) {
        candidates << (plasmaRoaming + QLatin1String("/connections.json"));
    }
    const QString plasmaLocal = legacyPlasmaConfigDir(QStandardPaths::AppLocalDataLocation);
    if (!plasmaLocal.isEmpty()) {
        candidates << (plasmaLocal + QLatin1String("/connections.json"));
    }
    candidates << (QDir::homePath() + QLatin1String("/.plasma_terminal/connections.json"));
    candidates << (QDir::homePath() + QLatin1String("/.ghost_terminal/connections.json"));
    candidates << (QDir::homePath() + QLatin1String("/.shah_terminal/connections.json"));
    const QStringList legacyDirs = probeLegacyAppConfigDirs(primaryDir);
    for (const QString &legacyDir : legacyDirs) {
        candidates << (legacyDir + QLatin1String("/connections.json"));
    }

    for (const QString &candidate : candidates) {
        if (QFile::exists(candidate)) {
            return candidate;
        }
    }
    return primaryFile;
}

bool isV2Root(const QJsonObject &root)
{
    const int ver = root.value(kSchemaVersionKey).toInt(0);
    if (ver < kSchemaVersion) {
        return false;
    }
    return root.value(kProfilesKey).isObject();
}

#ifdef _WIN32
QString dpapiProtectToBase64(const QString &plain)
{
    if (plain.isEmpty()) {
        return {};
    }
    const QByteArray in = plain.toUtf8();
    DATA_BLOB inputBlob;
    inputBlob.pbData = reinterpret_cast<BYTE *>(const_cast<char *>(in.data()));
    inputBlob.cbData = static_cast<DWORD>(in.size());

    DATA_BLOB outputBlob;
    outputBlob.pbData = nullptr;
    outputBlob.cbData = 0;

    const BOOL ok = CryptProtectData(&inputBlob,
                                    L"FusionTerminal Credentials",
                                    nullptr,
                                    nullptr,
                                    nullptr,
                                    CRYPTPROTECT_UI_FORBIDDEN,
                                    &outputBlob);
    if (!ok || !outputBlob.pbData || outputBlob.cbData == 0) {
        if (outputBlob.pbData) {
            LocalFree(outputBlob.pbData);
        }
        return {};
    }

    const QByteArray out(reinterpret_cast<const char *>(outputBlob.pbData),
                         static_cast<int>(outputBlob.cbData));
    LocalFree(outputBlob.pbData);
    return QString::fromLatin1(out.toBase64(QByteArray::Base64Encoding));
}

QString dpapiUnprotectFromBase64(const QString &b64)
{
    if (b64.trimmed().isEmpty()) {
        return {};
    }
    const QByteArray enc = QByteArray::fromBase64(b64.toLatin1(), QByteArray::Base64Encoding);
    if (enc.isEmpty()) {
        return {};
    }

    DATA_BLOB inputBlob;
    inputBlob.pbData = reinterpret_cast<BYTE *>(const_cast<char *>(enc.data()));
    inputBlob.cbData = static_cast<DWORD>(enc.size());

    DATA_BLOB outputBlob;
    outputBlob.pbData = nullptr;
    outputBlob.cbData = 0;

    BOOL ok = CryptUnprotectData(&inputBlob,
                                nullptr,
                                nullptr,
                                nullptr,
                                nullptr,
                                CRYPTPROTECT_UI_FORBIDDEN,
                                &outputBlob);
    if (!ok || !outputBlob.pbData || outputBlob.cbData == 0) {
        if (outputBlob.pbData) {
            LocalFree(outputBlob.pbData);
        }
        return {};
    }

    const QByteArray out(reinterpret_cast<const char *>(outputBlob.pbData),
                         static_cast<int>(outputBlob.cbData));
    LocalFree(outputBlob.pbData);
    return QString::fromUtf8(out);
}
#endif

QString defaultColorForProfile(ConnectionStore::Profile profile)
{
    switch (profile) {
    case ConnectionStore::Profile::MexcFutures:
        return QStringLiteral("#f5b642");
    case ConnectionStore::Profile::BinanceSpot:
        return QStringLiteral("#f0b90b");
    case ConnectionStore::Profile::BinanceFutures:
        return QStringLiteral("#f5b642");
    case ConnectionStore::Profile::Lighter:
        return QStringLiteral("#38bdf8");
    case ConnectionStore::Profile::UzxSwap:
        return QStringLiteral("#ff7f50");
    case ConnectionStore::Profile::UzxSpot:
        return QStringLiteral("#8bc34a");
    case ConnectionStore::Profile::MexcSpot:
    default:
        return QStringLiteral("#4c9fff");
    }
}

ConnectionStore::Profile profileFromKeyString(const QString &key, bool *okOut = nullptr)
{
    if (okOut) {
        *okOut = true;
    }
    if (key == QStringLiteral("mexcSpot")) {
        return ConnectionStore::Profile::MexcSpot;
    }
    if (key == QStringLiteral("mexcFutures")) {
        return ConnectionStore::Profile::MexcFutures;
    }
    if (key == QStringLiteral("uzxSpot")) {
        return ConnectionStore::Profile::UzxSpot;
    }
    if (key == QStringLiteral("uzxSwap")) {
        return ConnectionStore::Profile::UzxSwap;
    }
    if (key == QStringLiteral("binanceSpot")) {
        return ConnectionStore::Profile::BinanceSpot;
    }
    if (key == QStringLiteral("binanceFutures")) {
        return ConnectionStore::Profile::BinanceFutures;
    }
    if (key == QStringLiteral("lighter")) {
        return ConnectionStore::Profile::Lighter;
    }
    if (okOut) {
        *okOut = false;
    }
    return ConnectionStore::Profile::MexcSpot;
}

QStringList profileKeyStrings()
{
    return {
        QStringLiteral("mexcSpot"),
        QStringLiteral("mexcFutures"),
        QStringLiteral("uzxSpot"),
        QStringLiteral("uzxSwap"),
        QStringLiteral("binanceSpot"),
        QStringLiteral("binanceFutures"),
        QStringLiteral("lighter")
    };
}

bool sanitizeSecretsInObject(QJsonObject &obj)
{
    bool changed = false;
    const bool saveSecret = obj.value(QStringLiteral("saveSecret")).toBool(false);
    auto removeKey = [&](const QString &key) {
        if (obj.contains(key)) {
            obj.remove(key);
            changed = true;
        }
    };
#ifdef _WIN32
    auto maybeEncrypt = [&](const QString &plainKey, const QString &encKey) {
        const QString plain = obj.value(plainKey).toString();
        const QString enc = obj.value(encKey).toString();
        if (!saveSecret) {
            removeKey(plainKey);
            removeKey(encKey);
            return;
        }
        if (!enc.isEmpty()) {
            removeKey(plainKey);
            return;
        }
        if (plain.isEmpty()) {
            return;
        }
        const QString protectedValue = dpapiProtectToBase64(plain.trimmed());
        if (!protectedValue.isEmpty()) {
            obj.insert(encKey, protectedValue);
            removeKey(plainKey);
            changed = true;
        }
    };
    maybeEncrypt(QStringLiteral("apiKey"), QStringLiteral("apiKeyEnc"));
    maybeEncrypt(QStringLiteral("secretKey"), QStringLiteral("secretKeyEnc"));
    maybeEncrypt(QStringLiteral("passphrase"), QStringLiteral("passphraseEnc"));
    maybeEncrypt(QStringLiteral("seedPhrase"), QStringLiteral("seedEnc"));
#else
    if (!saveSecret) {
        removeKey(QStringLiteral("apiKey"));
        removeKey(QStringLiteral("secretKey"));
        removeKey(QStringLiteral("passphrase"));
        removeKey(QStringLiteral("seedPhrase"));
    }
#endif
    return changed;
}

bool sanitizeCredentialsFile(const QString &path)
{
    QFile file(path);
    if (!file.exists() || !file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return false;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    file.close();
    if (!doc.isObject()) {
        return false;
    }
    QJsonObject root = doc.object();
    bool changed = false;
    const QStringList keys = profileKeyStrings();

    if (isV2Root(root)) {
        QJsonObject profilesObj = root.value(kProfilesKey).toObject();
        for (const QString &id : profilesObj.keys()) {
            QJsonObject obj = profilesObj.value(id).toObject();
            if (sanitizeSecretsInObject(obj)) {
                profilesObj.insert(id, obj);
                changed = true;
            }
        }
        root.insert(kProfilesKey, profilesObj);

        for (const QString &key : keys) {
            QJsonObject obj = root.value(key).toObject();
            if (obj.isEmpty()) {
                continue;
            }
            if (sanitizeSecretsInObject(obj)) {
                root.insert(key, obj);
                changed = true;
            }
        }
    } else {
        for (const QString &key : keys) {
            QJsonObject obj = root.value(key).toObject();
            if (obj.isEmpty()) {
                continue;
            }
            if (sanitizeSecretsInObject(obj)) {
                root.insert(key, obj);
                changed = true;
            }
        }
    }

    if (!changed) {
        return false;
    }

    QSaveFile saveFile(path);
    if (!saveFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        return false;
    }
    saveFile.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return saveFile.commit();
}

MexcCredentials credsFromJsonObject(const QJsonObject &obj, ConnectionStore::Profile profile)
{
    MexcCredentials creds;
    const QString apiKeyPlain = obj.value(QStringLiteral("apiKey")).toString();
    const QString secretKeyPlain = obj.value(QStringLiteral("secretKey")).toString();
    const QString passphrasePlain = obj.value(QStringLiteral("passphrase")).toString();
    const QString seedPlain = obj.value(QStringLiteral("seedPhrase")).toString();
#ifdef _WIN32
    const QString apiEnc = obj.value(QStringLiteral("apiKeyEnc")).toString();
    if (!apiEnc.isEmpty()) {
        creds.apiKey = dpapiUnprotectFromBase64(apiEnc);
    } else {
        creds.apiKey = apiKeyPlain;
    }
    const QString secretEnc = obj.value(QStringLiteral("secretKeyEnc")).toString();
    if (!secretEnc.isEmpty()) {
        creds.secretKey = dpapiUnprotectFromBase64(secretEnc);
    } else {
        creds.secretKey = secretKeyPlain;
    }
    const QString passEnc = obj.value(QStringLiteral("passphraseEnc")).toString();
    if (!passEnc.isEmpty()) {
        creds.passphrase = dpapiUnprotectFromBase64(passEnc);
    } else {
        creds.passphrase = passphrasePlain;
    }
    const QString seedEnc = obj.value(QStringLiteral("seedEnc")).toString();
    if (!seedEnc.isEmpty()) {
        creds.seedPhrase = dpapiUnprotectFromBase64(seedEnc);
    } else {
        creds.seedPhrase = seedPlain;
    }
#else
    creds.apiKey = apiKeyPlain;
    creds.secretKey = secretKeyPlain;
    creds.passphrase = passphrasePlain;
    creds.seedPhrase = seedPlain;
#endif
    creds.uid = obj.value(QStringLiteral("uid")).toString();
    creds.proxy = obj.value(QStringLiteral("proxy")).toString();
    creds.proxyType = obj.value(QStringLiteral("proxyType")).toString();
    creds.baseUrl = obj.value(QStringLiteral("baseUrl")).toString();
    creds.accountIndex = obj.value(QStringLiteral("accountIndex")).toInt(0);
    creds.apiKeyIndex = obj.value(QStringLiteral("apiKeyIndex")).toInt(-1);
    creds.seedAddressIndex = obj.value(QStringLiteral("seedAddressIndex")).toInt(0);
    creds.preferSeedPhrase = obj.value(QStringLiteral("preferSeedPhrase")).toBool(false);
    creds.colorHex = obj.value(QStringLiteral("color")).toString();
    creds.label = obj.value(QStringLiteral("label")).toString();
    creds.saveSecret = obj.value(QStringLiteral("saveSecret")).toBool(false);
    creds.viewOnly = obj.value(QStringLiteral("viewOnly")).toBool(false);
    creds.autoConnect = obj.value(QStringLiteral("autoConnect")).toBool(true);

    if (!creds.saveSecret) {
        creds.apiKey.clear();
        creds.secretKey.clear();
        creds.passphrase.clear();
        creds.seedPhrase.clear();
    }
    if (creds.colorHex.isEmpty()) {
        creds.colorHex = defaultColorForProfile(profile);
    }
    return creds;
}

QJsonObject jsonObjectFromCreds(const MexcCredentials &creds, ConnectionStore::Profile profile)
{
    QJsonObject obj;
    obj.insert(QStringLiteral("uid"), creds.uid);
    obj.insert(QStringLiteral("proxy"), creds.proxy);
    obj.insert(QStringLiteral("proxyType"), creds.proxyType);
    if (!creds.baseUrl.isEmpty()) {
        obj.insert(QStringLiteral("baseUrl"), creds.baseUrl);
    } else {
        obj.remove(QStringLiteral("baseUrl"));
    }
    obj.insert(QStringLiteral("accountIndex"), creds.accountIndex);
    obj.insert(QStringLiteral("apiKeyIndex"), creds.apiKeyIndex);
    obj.insert(QStringLiteral("seedAddressIndex"), creds.seedAddressIndex);
    obj.insert(QStringLiteral("preferSeedPhrase"), creds.preferSeedPhrase);
    obj.insert(QStringLiteral("color"), creds.colorHex);
    obj.insert(QStringLiteral("label"), creds.label);
    obj.insert(QStringLiteral("saveSecret"), creds.saveSecret);
    obj.insert(QStringLiteral("viewOnly"), creds.viewOnly);
    obj.insert(QStringLiteral("autoConnect"), creds.autoConnect);

#ifdef _WIN32
    obj.remove(QStringLiteral("apiKey"));
    obj.remove(QStringLiteral("secretKey"));
    obj.remove(QStringLiteral("passphrase"));
    obj.remove(QStringLiteral("seedPhrase"));
    obj.remove(QStringLiteral("apiKeyEnc"));
    obj.remove(QStringLiteral("secretKeyEnc"));
    obj.remove(QStringLiteral("passphraseEnc"));
    obj.remove(QStringLiteral("seedEnc"));

    if (creds.saveSecret) {
        if (!creds.apiKey.trimmed().isEmpty()) {
            const QString enc = dpapiProtectToBase64(creds.apiKey.trimmed());
            if (!enc.isEmpty()) {
                obj.insert(QStringLiteral("apiKeyEnc"), enc);
            }
        }
        if (!creds.secretKey.trimmed().isEmpty()) {
            const QString enc = dpapiProtectToBase64(creds.secretKey.trimmed());
            if (!enc.isEmpty()) {
                obj.insert(QStringLiteral("secretKeyEnc"), enc);
            }
        }
        if (!creds.passphrase.trimmed().isEmpty()) {
            const QString enc = dpapiProtectToBase64(creds.passphrase.trimmed());
            if (!enc.isEmpty()) {
                obj.insert(QStringLiteral("passphraseEnc"), enc);
            }
        }
        if (!creds.seedPhrase.trimmed().isEmpty()) {
            const QString enc = dpapiProtectToBase64(creds.seedPhrase.trimmed());
            if (!enc.isEmpty()) {
                obj.insert(QStringLiteral("seedEnc"), enc);
            }
        }
    }
#else
    if (creds.saveSecret) {
        obj.insert(QStringLiteral("apiKey"), creds.apiKey);
        obj.insert(QStringLiteral("secretKey"), creds.secretKey);
        if (!creds.passphrase.isEmpty()) {
            obj.insert(QStringLiteral("passphrase"), creds.passphrase);
        }
        if (!creds.seedPhrase.isEmpty()) {
            obj.insert(QStringLiteral("seedPhrase"), creds.seedPhrase);
        }
    }
#endif
    return obj;
}

QString newProfileId(const QString &typeKey, const QJsonObject &profilesObj)
{
    QString id;
    do {
        const QString suffix = QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
        id = typeKey + QStringLiteral(":") + suffix;
    } while (profilesObj.contains(id));
    return id;
}

QStringList readOrderList(const QJsonObject &root)
{
    QStringList out;
    const QJsonArray arr = root.value(kOrderKey).toArray();
    out.reserve(arr.size());
    for (const QJsonValue &v : arr) {
        const QString id = v.toString();
        if (!id.isEmpty()) {
            out.push_back(id);
        }
    }
    return out;
}

QJsonArray writeOrderArray(const QStringList &order)
{
    QJsonArray arr;
    for (const QString &id : order) {
        arr.append(id);
    }
    return arr;
}
} // namespace

ConnectionStore::ConnectionStore(QObject *parent)
    : QObject(parent)
{
}

QString ConnectionStore::profileKey(Profile profile) const
{
    switch (profile) {
    case Profile::BinanceSpot:
        return QStringLiteral("binanceSpot");
    case Profile::BinanceFutures:
        return QStringLiteral("binanceFutures");
    case Profile::Lighter:
        return QStringLiteral("lighter");
    case Profile::UzxSpot:
        return QStringLiteral("uzxSpot");
    case Profile::UzxSwap:
        return QStringLiteral("uzxSwap");
    case Profile::MexcFutures:
        return QStringLiteral("mexcFutures");
    case Profile::MexcSpot:
    default:
        return QStringLiteral("mexcSpot");
    }
}

QString ConnectionStore::storagePath() const
{
    return primaryConfigDir();
}

QString ConnectionStore::credentialsFilePath() const
{
    return storagePath() + QLatin1String("/connections.json");
}

void ConnectionStore::ensureProfilesSchema()
{
    const QString primaryDir = storagePath();
    const QString primaryPath = credentialsFilePath();
    const QString readPath = resolveCredentialsFilePath(primaryDir);

    QFile file(readPath);
    QJsonObject root;
    if (file.exists() && file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QJsonDocument existing = QJsonDocument::fromJson(file.readAll());
        if (existing.isObject()) {
            root = existing.object();
        }
        file.close();
    }

    if (isV2Root(root)) {
        sanitizeCredentialsFile(readPath);
        return;
    }

    QJsonObject profilesObj;
    QJsonObject activeObj;
    QStringList order;

    const QVector<Profile> types = {
        Profile::MexcSpot,
        Profile::MexcFutures,
        Profile::Lighter,
        Profile::BinanceSpot,
        Profile::BinanceFutures,
        Profile::UzxSwap,
        Profile::UzxSpot
    };

    for (Profile t : types) {
        const QString typeKey = profileKey(t);
        QJsonObject legacyObj = root.value(typeKey).toObject();

        if (legacyObj.isEmpty()) {
            MexcCredentials blank;
            blank.colorHex = defaultColorForProfile(t);
            blank.label = typeKey;
            blank.autoConnect = true;
            legacyObj = jsonObjectFromCreds(blank, t);
        }

        QString id = typeKey + QStringLiteral(":default");
        int n = 2;
        while (profilesObj.contains(id)) {
            id = typeKey + QStringLiteral(":default") + QString::number(n++);
        }

        legacyObj.insert(kTypeKey, typeKey);
        if (!legacyObj.contains(QStringLiteral("label"))) {
            legacyObj.insert(QStringLiteral("label"), typeKey);
        }
        if (legacyObj.value(QStringLiteral("color")).toString().isEmpty()) {
            legacyObj.insert(QStringLiteral("color"), defaultColorForProfile(t));
        }

        profilesObj.insert(id, legacyObj);
        activeObj.insert(typeKey, id);
        order.push_back(id);
    }

    root.insert(kSchemaVersionKey, kSchemaVersion);
    root.insert(kProfilesKey, profilesObj);
    root.insert(kOrderKey, writeOrderArray(order));
    root.insert(kActiveKey, activeObj);

    QSaveFile saveFile(primaryPath);
    if (!saveFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        return;
    }
    saveFile.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    saveFile.commit();
}

MexcCredentials ConnectionStore::loadMexcCredentials(Profile profile) const
{
    const QString primaryDir = storagePath();
    const QString readPath = resolveCredentialsFilePath(primaryDir);
    sanitizeCredentialsFile(readPath);

    QFile file(readPath);
    if (!file.exists() || !file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    const QByteArray data = file.readAll();
    file.close();

    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &error);
    if (doc.isNull() || !doc.isObject()) {
        return {};
    }
    const QJsonObject root = doc.object();
    const QString typeKey = profileKey(profile);
    if (isV2Root(root)) {
        const QJsonObject profilesObj = root.value(kProfilesKey).toObject();
        const QJsonObject activeObj = root.value(kActiveKey).toObject();
        const QString activeId = activeObj.value(typeKey).toString();
        if (!activeId.isEmpty() && profilesObj.contains(activeId)) {
            return credsFromJsonObject(profilesObj.value(activeId).toObject(), profile);
        }
        const QStringList order = readOrderList(root);
        for (const QString &id : order) {
            const QJsonObject obj = profilesObj.value(id).toObject();
            if (obj.value(kTypeKey).toString() == typeKey) {
                return credsFromJsonObject(obj, profile);
            }
        }
    }

    return credsFromJsonObject(root.value(typeKey).toObject(), profile);
}

void ConnectionStore::saveMexcCredentials(const MexcCredentials &creds, Profile profile)
{
    ensureProfilesSchema();

    const QString primaryDir = storagePath();
    const QString primaryPath = credentialsFilePath();
    const QString readPath = resolveCredentialsFilePath(primaryDir);

    QFile file(readPath);
    QJsonObject root;
    if (file.exists() && file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QJsonDocument existing = QJsonDocument::fromJson(file.readAll());
        if (existing.isObject()) {
            root = existing.object();
        }
        file.close();
    }

    const QString typeKey = profileKey(profile);
    QJsonObject obj = jsonObjectFromCreds(creds, profile);
    obj.insert(kTypeKey, typeKey);

    if (!isV2Root(root)) {
        root.insert(typeKey, obj);
    } else {
        QJsonObject profilesObj = root.value(kProfilesKey).toObject();
        QJsonObject activeObj = root.value(kActiveKey).toObject();

        QString activeId = activeObj.value(typeKey).toString();
        if (activeId.isEmpty() || !profilesObj.contains(activeId)) {
            activeId = newProfileId(typeKey, profilesObj);
            activeObj.insert(typeKey, activeId);
            QStringList order = readOrderList(root);
            order.push_back(activeId);
            root.insert(kOrderKey, writeOrderArray(order));
        }

        profilesObj.insert(activeId, obj);
        root.insert(kProfilesKey, profilesObj);
        root.insert(kActiveKey, activeObj);
        root.insert(typeKey, obj); // legacy mirror
    }

    QSaveFile saveFile(primaryPath);
    if (!saveFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        return;
    }
    saveFile.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    saveFile.commit();
    emit credentialsChanged(typeKey, creds);
}

QVector<ConnectionStore::StoredProfile> ConnectionStore::allProfiles() const
{
    const QString primaryDir = storagePath();
    const QString readPath = resolveCredentialsFilePath(primaryDir);

    QFile file(readPath);
    if (!file.exists() || !file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    file.close();
    if (!doc.isObject()) {
        return {};
    }

    const QJsonObject root = doc.object();
    if (!isV2Root(root)) {
        return {};
    }

    QVector<StoredProfile> out;
    const QJsonObject profilesObj = root.value(kProfilesKey).toObject();
    const QStringList order = readOrderList(root);
    out.reserve(order.size());
    for (const QString &id : order) {
        const QJsonObject obj = profilesObj.value(id).toObject();
        bool ok = false;
        const Profile type = profileFromKeyString(obj.value(kTypeKey).toString(), &ok);
        if (!ok) {
            continue;
        }
        StoredProfile p;
        p.id = id;
        p.type = type;
        p.creds = credsFromJsonObject(obj, type);
        out.push_back(p);
    }
    return out;
}

QVector<ConnectionStore::StoredProfile> ConnectionStore::profiles(Profile type) const
{
    const QVector<StoredProfile> all = allProfiles();
    QVector<StoredProfile> out;
    for (const StoredProfile &p : all) {
        if (p.type == type) {
            out.push_back(p);
        }
    }
    return out;
}

ConnectionStore::StoredProfile ConnectionStore::profileById(const QString &id) const
{
    StoredProfile out;
    out.id = id;

    const QString primaryDir = storagePath();
    const QString readPath = resolveCredentialsFilePath(primaryDir);

    QFile file(readPath);
    if (!file.exists() || !file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return out;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    file.close();
    if (!doc.isObject()) {
        return out;
    }

    const QJsonObject root = doc.object();
    if (!isV2Root(root)) {
        return out;
    }
    const QJsonObject profilesObj = root.value(kProfilesKey).toObject();
    const QJsonObject obj = profilesObj.value(id).toObject();
    bool ok = false;
    const Profile type = profileFromKeyString(obj.value(kTypeKey).toString(), &ok);
    if (!ok) {
        return out;
    }
    out.type = type;
    out.creds = credsFromJsonObject(obj, type);
    return out;
}

QStringList ConnectionStore::profileOrder() const
{
    const QString primaryDir = storagePath();
    const QString readPath = resolveCredentialsFilePath(primaryDir);

    QFile file(readPath);
    if (!file.exists() || !file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    file.close();
    if (!doc.isObject()) {
        return {};
    }
    const QJsonObject root = doc.object();
    if (!isV2Root(root)) {
        return {};
    }
    return readOrderList(root);
}

void ConnectionStore::moveProfile(const QString &id, int delta)
{
    ensureProfilesSchema();
    const QString primaryDir = storagePath();
    const QString primaryPath = credentialsFilePath();
    const QString readPath = resolveCredentialsFilePath(primaryDir);

    QFile file(readPath);
    QJsonObject root;
    if (file.exists() && file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QJsonDocument existing = QJsonDocument::fromJson(file.readAll());
        if (existing.isObject()) {
            root = existing.object();
        }
        file.close();
    }
    if (!isV2Root(root)) {
        return;
    }

    QStringList order = readOrderList(root);
    const int idx = order.indexOf(id);
    if (idx < 0) {
        return;
    }
    const int next = idx + delta;
    if (next < 0 || next >= order.size()) {
        return;
    }
    order.swapItemsAt(idx, next);
    root.insert(kOrderKey, writeOrderArray(order));

    QSaveFile saveFile(primaryPath);
    if (!saveFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        return;
    }
    saveFile.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    saveFile.commit();
}

QString ConnectionStore::activeProfileId(Profile type) const
{
    const QString primaryDir = storagePath();
    const QString readPath = resolveCredentialsFilePath(primaryDir);

    QFile file(readPath);
    if (!file.exists() || !file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    file.close();
    if (!doc.isObject()) {
        return {};
    }
    const QJsonObject root = doc.object();
    if (!isV2Root(root)) {
        return {};
    }
    const QJsonObject activeObj = root.value(kActiveKey).toObject();
    return activeObj.value(profileKey(type)).toString();
}

void ConnectionStore::setActiveProfileId(Profile type, const QString &id)
{
    ensureProfilesSchema();
    const QString primaryDir = storagePath();
    const QString primaryPath = credentialsFilePath();
    const QString readPath = resolveCredentialsFilePath(primaryDir);

    QFile file(readPath);
    QJsonObject root;
    if (file.exists() && file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QJsonDocument existing = QJsonDocument::fromJson(file.readAll());
        if (existing.isObject()) {
            root = existing.object();
        }
        file.close();
    }
    if (!isV2Root(root)) {
        return;
    }

    QJsonObject profilesObj = root.value(kProfilesKey).toObject();
    if (!profilesObj.contains(id)) {
        return;
    }
    const QJsonObject obj = profilesObj.value(id).toObject();
    const QString typeKey = profileKey(type);
    if (obj.value(kTypeKey).toString() != typeKey) {
        return;
    }

    QJsonObject activeObj = root.value(kActiveKey).toObject();
    activeObj.insert(typeKey, id);
    root.insert(kActiveKey, activeObj);
    root.insert(typeKey, obj); // legacy mirror

    QSaveFile saveFile(primaryPath);
    if (!saveFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        return;
    }
    saveFile.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    saveFile.commit();
    emit credentialsChanged(typeKey, credsFromJsonObject(obj, type));
}

ConnectionStore::StoredProfile ConnectionStore::createProfile(Profile type,
                                                              const QString &label,
                                                              const MexcCredentials &initial)
{
    ensureProfilesSchema();
    const QString primaryDir = storagePath();
    const QString primaryPath = credentialsFilePath();
    const QString readPath = resolveCredentialsFilePath(primaryDir);

    QFile file(readPath);
    QJsonObject root;
    if (file.exists() && file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QJsonDocument existing = QJsonDocument::fromJson(file.readAll());
        if (existing.isObject()) {
            root = existing.object();
        }
        file.close();
    }
    if (!isV2Root(root)) {
        return {};
    }

    QJsonObject profilesObj = root.value(kProfilesKey).toObject();
    const QString typeKey = profileKey(type);
    const QString id = newProfileId(typeKey, profilesObj);

    MexcCredentials creds = initial;
    if (creds.colorHex.isEmpty()) {
        creds.colorHex = defaultColorForProfile(type);
    }
    if (!label.trimmed().isEmpty()) {
        creds.label = label.trimmed();
    }
    if (creds.label.trimmed().isEmpty()) {
        creds.label = typeKey;
    }

    QJsonObject obj = jsonObjectFromCreds(creds, type);
    obj.insert(kTypeKey, typeKey);

    profilesObj.insert(id, obj);
    root.insert(kProfilesKey, profilesObj);

    QStringList order = readOrderList(root);
    order.push_back(id);
    root.insert(kOrderKey, writeOrderArray(order));

    QJsonObject activeObj = root.value(kActiveKey).toObject();
    const bool hadActive = !activeObj.value(typeKey).toString().isEmpty();
    if (!hadActive) {
        activeObj.insert(typeKey, id);
        root.insert(typeKey, obj); // legacy mirror
    }
    root.insert(kActiveKey, activeObj);

    QSaveFile saveFile(primaryPath);
    if (!saveFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        return {};
    }
    saveFile.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    saveFile.commit();

    if (!hadActive) {
        emit credentialsChanged(typeKey, creds);
    }

    StoredProfile p;
    p.id = id;
    p.type = type;
    p.creds = creds;
    return p;
}

void ConnectionStore::saveProfile(const StoredProfile &profile)
{
    ensureProfilesSchema();
    const QString primaryDir = storagePath();
    const QString primaryPath = credentialsFilePath();
    const QString readPath = resolveCredentialsFilePath(primaryDir);

    QFile file(readPath);
    QJsonObject root;
    if (file.exists() && file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QJsonDocument existing = QJsonDocument::fromJson(file.readAll());
        if (existing.isObject()) {
            root = existing.object();
        }
        file.close();
    }
    if (!isV2Root(root)) {
        return;
    }

    const QString typeKey = profileKey(profile.type);
    QJsonObject profilesObj = root.value(kProfilesKey).toObject();
    QJsonObject obj = jsonObjectFromCreds(profile.creds, profile.type);
    obj.insert(kTypeKey, typeKey);
    profilesObj.insert(profile.id, obj);
    root.insert(kProfilesKey, profilesObj);

    QStringList order = readOrderList(root);
    if (!order.contains(profile.id)) {
        order.push_back(profile.id);
        root.insert(kOrderKey, writeOrderArray(order));
    }

    const QJsonObject activeObj = root.value(kActiveKey).toObject();
    const QString activeId = activeObj.value(typeKey).toString();
    if (activeId == profile.id) {
        root.insert(typeKey, obj); // legacy mirror
        emit credentialsChanged(typeKey, profile.creds);
    }

    QSaveFile saveFile(primaryPath);
    if (!saveFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        return;
    }
    saveFile.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    saveFile.commit();
}

void ConnectionStore::deleteProfile(const QString &id)
{
    ensureProfilesSchema();
    const QString primaryDir = storagePath();
    const QString primaryPath = credentialsFilePath();
    const QString readPath = resolveCredentialsFilePath(primaryDir);

    QFile file(readPath);
    QJsonObject root;
    if (file.exists() && file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QJsonDocument existing = QJsonDocument::fromJson(file.readAll());
        if (existing.isObject()) {
            root = existing.object();
        }
        file.close();
    }
    if (!isV2Root(root)) {
        return;
    }

    QJsonObject profilesObj = root.value(kProfilesKey).toObject();
    const QJsonObject removedObj = profilesObj.value(id).toObject();
    bool ok = false;
    const Profile removedType = profileFromKeyString(removedObj.value(kTypeKey).toString(), &ok);
    if (!ok) {
        return;
    }
    const QString removedTypeKey = profileKey(removedType);

    profilesObj.remove(id);
    root.insert(kProfilesKey, profilesObj);

    QStringList order = readOrderList(root);
    order.removeAll(id);
    root.insert(kOrderKey, writeOrderArray(order));

    QJsonObject activeObj = root.value(kActiveKey).toObject();
    const QString activeId = activeObj.value(removedTypeKey).toString();
    bool activeChanged = false;
    MexcCredentials newActiveCreds;
    if (activeId == id) {
        QString replacementId;
        for (const QString &candId : order) {
            const QJsonObject candObj = profilesObj.value(candId).toObject();
            if (candObj.value(kTypeKey).toString() == removedTypeKey) {
                replacementId = candId;
                newActiveCreds = credsFromJsonObject(candObj, removedType);
                root.insert(removedTypeKey, candObj); // legacy mirror
                break;
            }
        }
        if (!replacementId.isEmpty()) {
            activeObj.insert(removedTypeKey, replacementId);
        } else {
            activeObj.remove(removedTypeKey);
            root.remove(removedTypeKey);
        }
        activeChanged = true;
    }
    root.insert(kActiveKey, activeObj);

    QSaveFile saveFile(primaryPath);
    if (!saveFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        return;
    }
    saveFile.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    saveFile.commit();

    if (activeChanged) {
        emit credentialsChanged(removedTypeKey, newActiveCreds);
    }
}
