#pragma once

#include <QAbstractListModel>
#include <QColor>
#include <QStringList>
#include <QVector>
#include <QtMath>

class PrintCirclesModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Role {
        XRatioRole = Qt::UserRole + 1,
        YRole,
        RadiusRole,
        FillColorRole,
        BorderColorRole,
        TextRole
    };

    struct Entry {
        double xRatio = 0.0;
        double y = 0.0;
        double radius = 0.0;
        QColor fillColor;
        QColor borderColor;
        QString text;

        bool operator==(const Entry &other) const
        {
            return qFuzzyCompare(1.0 + xRatio, 1.0 + other.xRatio)
                   && qFuzzyCompare(1.0 + y, 1.0 + other.y)
                   && qFuzzyCompare(1.0 + radius, 1.0 + other.radius)
                   && fillColor == other.fillColor
                   && borderColor == other.borderColor
                   && text == other.text;
        }
        bool operator!=(const Entry &other) const { return !(*this == other); }
    };

    explicit PrintCirclesModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setEntries(QVector<Entry> entries);

    Q_INVOKABLE QVariantMap get(int index) const;

private:
    QVector<Entry> m_entries;
};

class PrintOrdersModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Role {
        RowRole = Qt::UserRole + 1,
        TextRole,
        FillColorRole,
        BorderColorRole,
        BuyRole,
        PriceRole,
        OrderIdsRole
    };

    struct Entry {
        int row = -1;
        double price = 0.0;
        QString text;
        QStringList orderIds;
        QColor fillColor;
        QColor borderColor;
        bool buy = true;

        bool operator==(const Entry &other) const
        {
            return row == other.row
                   && qFuzzyCompare(1.0 + price, 1.0 + other.price)
                   && text == other.text
                   && orderIds == other.orderIds
                   && fillColor == other.fillColor
                   && borderColor == other.borderColor
                   && buy == other.buy;
        }
        bool operator!=(const Entry &other) const { return !(*this == other); }
    };

    explicit PrintOrdersModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setEntries(QVector<Entry> entries);

private:
    QVector<Entry> m_entries;
};

class PrintClustersModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Role {
        RowRole = Qt::UserRole + 1,
        ColRole,
        TextRole,
        TextColorRole,
        BgColorRole
    };

    struct Entry {
        int row = -1;
        int col = 0;
        QString text;
        QColor textColor;
        QColor bgColor;

        bool operator==(const Entry &other) const
        {
            return row == other.row
                   && col == other.col
                   && text == other.text
                   && textColor == other.textColor
                   && bgColor == other.bgColor;
        }
        bool operator!=(const Entry &other) const { return !(*this == other); }
    };

    explicit PrintClustersModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setEntries(QVector<Entry> entries);

private:
    QVector<Entry> m_entries;
};
