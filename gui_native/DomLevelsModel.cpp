#include "DomLevelsModel.h"

#include <algorithm>
#include <utility>

DomLevelsModel::DomLevelsModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int DomLevelsModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return m_rows.size();
}

QVariant DomLevelsModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size()) {
        return QVariant();
    }
    const Row &row = m_rows.at(index.row());
    switch (role) {
    case PriceRole:
        return row.price;
    case BidQtyRole:
        return row.bidQty;
    case AskQtyRole:
        return row.askQty;
    case PriceTextRole:
        return row.priceText;
    case BookColorRole:
        return row.bookColor;
    case PriceBgColorRole:
        return row.priceBgColor;
    case VolumeTextRole:
        return row.volumeText;
    case VolumeTextColorRole:
        return row.volumeTextColor;
    case VolumeFillColorRole:
        return row.volumeFillColor;
    case VolumeFillRatioRole:
        return row.volumeFillRatio;
    case MarkerTextRole:
        return row.markerText;
    case MarkerFillColorRole:
        return row.markerFillColor;
    case MarkerBorderColorRole:
        return row.markerBorderColor;
    case MarkerBuyRole:
        return row.markerBuy;
    case OrderHighlightRole:
        return row.orderHighlight;
    default:
        return QVariant();
    }
}

QHash<int, QByteArray> DomLevelsModel::roleNames() const
{
    static const QHash<int, QByteArray> roles = {
        {PriceRole, "price"},
        {BidQtyRole, "bidQty"},
        {AskQtyRole, "askQty"},
        {PriceTextRole, "priceText"},
        {BookColorRole, "bookColor"},
        {PriceBgColorRole, "priceBgColor"},
        {VolumeTextRole, "volumeText"},
        {VolumeTextColorRole, "volumeTextColor"},
        {VolumeFillColorRole, "volumeFillColor"},
        {VolumeFillRatioRole, "volumeFillRatio"},
        {MarkerTextRole, "markerText"},
        {MarkerFillColorRole, "markerFillColor"},
        {MarkerBorderColorRole, "markerBorderColor"},
        {MarkerBuyRole, "markerBuy"},
        {OrderHighlightRole, "orderHighlight"},
    };
    return roles;
}

void DomLevelsModel::setRows(QVector<Row> rows)
{
    if (rows.size() != m_rows.size()) {
        beginResetModel();
        m_rows = std::move(rows);
        endResetModel();
        return;
    }
    QVector<int> changed;
    changed.reserve(rows.size());
    for (int i = 0; i < rows.size(); ++i) {
        if (rows[i] != m_rows[i]) {
            m_rows[i] = rows[i];
            changed.append(i);
        }
    }
    if (changed.isEmpty()) {
        return;
    }
    int rangeStart = -1;
    int last = -1;
    const QVector<int> roles = {
        PriceRole,
        BidQtyRole,
        AskQtyRole,
        PriceTextRole,
        BookColorRole,
        PriceBgColorRole,
        VolumeTextRole,
        VolumeTextColorRole,
        VolumeFillColorRole,
        VolumeFillRatioRole,
        MarkerTextRole,
        MarkerFillColorRole,
        MarkerBorderColorRole,
        MarkerBuyRole,
        OrderHighlightRole};
    for (int idx : changed) {
        if (rangeStart < 0) {
            rangeStart = last = idx;
        } else if (idx == last + 1) {
            last = idx;
        } else {
            const QModelIndex top = createIndex(rangeStart, 0);
            const QModelIndex bottom = createIndex(last, 0);
            emit dataChanged(top, bottom, roles);
            rangeStart = last = idx;
        }
    }
    if (rangeStart >= 0) {
        const QModelIndex top = createIndex(rangeStart, 0);
        const QModelIndex bottom = createIndex(last, 0);
        emit dataChanged(top, bottom, roles);
    }
}

const DomLevelsModel::Row &DomLevelsModel::rowAt(int index) const
{
    static const Row empty;
    if (index < 0 || index >= m_rows.size()) {
        return empty;
    }
    return m_rows.at(index);
}

void DomLevelsModel::updateRows(const QVector<int> &indices, const QVector<Row> &rows)
{
    if (indices.isEmpty() || rows.isEmpty() || indices.size() != rows.size()) {
        return;
    }
    if (m_rows.isEmpty()) {
        return;
    }

    QVector<int> changed;
    changed.reserve(indices.size());
    for (int i = 0; i < indices.size(); ++i) {
        const int idx = indices[i];
        if (idx < 0 || idx >= m_rows.size()) {
            continue;
        }
        if (rows[i] != m_rows[idx]) {
            m_rows[idx] = rows[i];
            changed.push_back(idx);
        }
    }
    if (changed.isEmpty()) {
        return;
    }

    std::sort(changed.begin(), changed.end());
    changed.erase(std::unique(changed.begin(), changed.end()), changed.end());

    int rangeStart = -1;
    int last = -1;
    const QVector<int> roles = {
        PriceRole,
        BidQtyRole,
        AskQtyRole,
        PriceTextRole,
        BookColorRole,
        PriceBgColorRole,
        VolumeTextRole,
        VolumeTextColorRole,
        VolumeFillColorRole,
        VolumeFillRatioRole,
        MarkerTextRole,
        MarkerFillColorRole,
        MarkerBorderColorRole,
        MarkerBuyRole,
        OrderHighlightRole};

    for (int idx : changed) {
        if (rangeStart < 0) {
            rangeStart = last = idx;
        } else if (idx == last + 1) {
            last = idx;
        } else {
            const QModelIndex top = createIndex(rangeStart, 0);
            const QModelIndex bottom = createIndex(last, 0);
            emit dataChanged(top, bottom, roles);
            rangeStart = last = idx;
        }
    }
    if (rangeStart >= 0) {
        const QModelIndex top = createIndex(rangeStart, 0);
        const QModelIndex bottom = createIndex(last, 0);
        emit dataChanged(top, bottom, roles);
    }
}
