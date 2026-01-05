#include "PrintsModel.h"

PrintCirclesModel::PrintCirclesModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int PrintCirclesModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return m_entries.size();
}

QVariant PrintCirclesModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_entries.size()) {
        return QVariant();
    }
    const Entry &entry = m_entries.at(index.row());
    switch (role) {
    case XRatioRole:
        return entry.xRatio;
    case YRole:
        return entry.y;
    case RadiusRole:
        return entry.radius;
    case FillColorRole:
        return entry.fillColor;
    case BorderColorRole:
        return entry.borderColor;
    case TextRole:
        return entry.text;
    default:
        return QVariant();
    }
}

QHash<int, QByteArray> PrintCirclesModel::roleNames() const
{
    static const QHash<int, QByteArray> roles = {
        {XRatioRole, "xRatio"},
        {YRole, "y"},
        {RadiusRole, "radius"},
        {FillColorRole, "fillColor"},
        {BorderColorRole, "borderColor"},
        {TextRole, "text"},
    };
    return roles;
}

void PrintCirclesModel::setEntries(QVector<Entry> entries)
{
    if (entries.size() != m_entries.size()) {
        beginResetModel();
        m_entries = std::move(entries);
        endResetModel();
        return;
    }
    QVector<int> changed;
    changed.reserve(entries.size());
    for (int i = 0; i < entries.size(); ++i) {
        if (entries[i] != m_entries[i]) {
            m_entries[i] = entries[i];
            changed.append(i);
        }
    }
    if (changed.isEmpty()) {
        return;
    }
    QVector<int> roleList = {
        XRatioRole,
        YRole,
        RadiusRole,
        FillColorRole,
        BorderColorRole,
        TextRole,
    };
    int start = -1;
    int last = -1;
    for (int idx : changed) {
        if (start < 0) {
            start = last = idx;
        } else if (idx == last + 1) {
            last = idx;
        } else {
            const QModelIndex top = createIndex(start, 0);
            const QModelIndex bottom = createIndex(last, 0);
            emit dataChanged(top, bottom, roleList);
            start = last = idx;
        }
    }
    if (start >= 0) {
        const QModelIndex top = createIndex(start, 0);
        const QModelIndex bottom = createIndex(last, 0);
        emit dataChanged(top, bottom, roleList);
    }
}

QVariantMap PrintCirclesModel::get(int index) const
{
    QVariantMap map;
    if (index < 0 || index >= m_entries.size()) {
        return map;
    }
    const Entry &entry = m_entries.at(index);
    map.insert(QStringLiteral("xRatio"), entry.xRatio);
    map.insert(QStringLiteral("y"), entry.y);
    map.insert(QStringLiteral("radius"), entry.radius);
    map.insert(QStringLiteral("fillColor"), entry.fillColor);
    map.insert(QStringLiteral("borderColor"), entry.borderColor);
    map.insert(QStringLiteral("text"), entry.text);
    return map;
}

PrintOrdersModel::PrintOrdersModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int PrintOrdersModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return m_entries.size();
}

QVariant PrintOrdersModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_entries.size()) {
        return QVariant();
    }
    const Entry &entry = m_entries.at(index.row());
    switch (role) {
    case RowRole:
        return entry.row;
    case TextRole:
        return entry.text;
    case FillColorRole:
        return entry.fillColor;
    case BorderColorRole:
        return entry.borderColor;
    case BuyRole:
        return entry.buy;
    case PriceRole:
        return entry.price;
    case OrderIdsRole:
        return entry.orderIds;
    default:
        return QVariant();
    }
}

QHash<int, QByteArray> PrintOrdersModel::roleNames() const
{
    static const QHash<int, QByteArray> roles = {
        {RowRole, "row"},
        {TextRole, "text"},
        {FillColorRole, "fillColor"},
        {BorderColorRole, "borderColor"},
        {BuyRole, "buy"},
        {PriceRole, "price"},
        {OrderIdsRole, "orderIds"},
    };
    return roles;
}

void PrintOrdersModel::setEntries(QVector<Entry> entries)
{
    if (entries.size() != m_entries.size()) {
        beginResetModel();
        m_entries = std::move(entries);
        endResetModel();
        return;
    }
    QVector<int> changed;
    for (int i = 0; i < entries.size(); ++i) {
        if (entries[i] != m_entries[i]) {
            m_entries[i] = entries[i];
            changed.append(i);
        }
    }
    if (changed.isEmpty()) {
        return;
    }
    QVector<int> roleList = {RowRole, TextRole, FillColorRole, BorderColorRole, BuyRole, PriceRole, OrderIdsRole};
    int start = -1;
    int last = -1;
    for (int idx : changed) {
        if (start < 0) {
            start = last = idx;
        } else if (idx == last + 1) {
            last = idx;
        } else {
            const QModelIndex top = createIndex(start, 0);
            const QModelIndex bottom = createIndex(last, 0);
            emit dataChanged(top, bottom, roleList);
            start = last = idx;
        }
    }
    if (start >= 0) {
        const QModelIndex top = createIndex(start, 0);
        const QModelIndex bottom = createIndex(last, 0);
        emit dataChanged(top, bottom, roleList);
    }
}

PrintClustersModel::PrintClustersModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int PrintClustersModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return m_entries.size();
}

QVariant PrintClustersModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_entries.size()) {
        return QVariant();
    }
    const Entry &entry = m_entries.at(index.row());
    switch (role) {
    case RowRole:
        return entry.row;
    case ColRole:
        return entry.col;
    case TextRole:
        return entry.text;
    case TextColorRole:
        return entry.textColor;
    case BgColorRole:
        return entry.bgColor;
    default:
        return QVariant();
    }
}

QHash<int, QByteArray> PrintClustersModel::roleNames() const
{
    static const QHash<int, QByteArray> roles = {
        {RowRole, "row"},
        {ColRole, "col"},
        {TextRole, "text"},
        {TextColorRole, "textColor"},
        {BgColorRole, "bgColor"},
    };
    return roles;
}

void PrintClustersModel::setEntries(QVector<Entry> entries)
{
    if (entries.size() != m_entries.size()) {
        beginResetModel();
        m_entries = std::move(entries);
        endResetModel();
        return;
    }
    QVector<int> changed;
    for (int i = 0; i < entries.size(); ++i) {
        if (entries[i] != m_entries[i]) {
            m_entries[i] = entries[i];
            changed.append(i);
        }
    }
    if (changed.isEmpty()) {
        return;
    }
    QVector<int> roleList = {RowRole, ColRole, TextRole, TextColorRole, BgColorRole};
    int start = -1;
    int last = -1;
    for (int idx : changed) {
        if (start < 0) {
            start = last = idx;
        } else if (idx == last + 1) {
            last = idx;
        } else {
            const QModelIndex top = createIndex(start, 0);
            const QModelIndex bottom = createIndex(last, 0);
            emit dataChanged(top, bottom, roleList);
            start = last = idx;
        }
    }
    if (start >= 0) {
        const QModelIndex top = createIndex(start, 0);
        const QModelIndex bottom = createIndex(last, 0);
        emit dataChanged(top, bottom, roleList);
    }
}
