#pragma once

#include <QAbstractListModel>
#include <QColor>
#include <QVector>
#include <QtMath>

class DomLevelsModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Role {
        PriceRole = Qt::UserRole + 1,
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
        OrderHighlightRole
    };

    struct Row {
        bool operator==(const Row &other) const
        {
            return price == other.price &&
                   bidQty == other.bidQty &&
                   askQty == other.askQty &&
                   priceText == other.priceText &&
                   bookColor == other.bookColor &&
                   priceBgColor == other.priceBgColor &&
                   volumeText == other.volumeText &&
                   volumeTextColor == other.volumeTextColor &&
                   volumeFillColor == other.volumeFillColor &&
                   qFuzzyCompare(1.0 + volumeFillRatio, 1.0 + other.volumeFillRatio) &&
                   markerText == other.markerText &&
                   markerFillColor == other.markerFillColor &&
                   markerBorderColor == other.markerBorderColor &&
                   markerBuy == other.markerBuy &&
                   orderHighlight == other.orderHighlight;
        }
        bool operator!=(const Row &other) const { return !(*this == other); }

        double price = 0.0;
        double bidQty = 0.0;
        double askQty = 0.0;
        QString priceText;
        QColor bookColor;
        QColor priceBgColor;
        QString volumeText;
        QColor volumeTextColor;
        QColor volumeFillColor;
        double volumeFillRatio = 0.0;
        QString markerText;
        QColor markerFillColor;
        QColor markerBorderColor;
        bool markerBuy = true;
        bool orderHighlight = false;
    };

    explicit DomLevelsModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setRows(QVector<Row> rows);
    const Row &rowAt(int index) const;
    void updateRows(const QVector<int> &indices, const QVector<Row> &rows);

private:
    QVector<Row> m_rows;
};
