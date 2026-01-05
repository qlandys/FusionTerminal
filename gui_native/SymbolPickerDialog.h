#pragma once

#include <QColor>
#include <QDialog>
#include <QIcon>
#include <QMap>
#include <QPair>
#include <QRegularExpression>
#include <QSortFilterProxyModel>
#include <QSet>
#include <QStringList>
#include <QVector>

class QLineEdit;
class QTableView;
class QStandardItemModel;
class QListWidget;
class SymbolFilterProxy;
class QModelIndex;
class QSortFilterProxyModel;
class QToolButton;

constexpr int kGroupRole = Qt::UserRole + 1;

class SymbolFilterProxy : public QSortFilterProxyModel {
public:
    explicit SymbolFilterProxy(QObject *parent = nullptr)
        : QSortFilterProxyModel(parent)
    {
        setFilterCaseSensitivity(Qt::CaseInsensitive);
        setSortCaseSensitivity(Qt::CaseInsensitive);
    }

    void setSearchPattern(const QString &pattern)
    {
        if (m_pattern == pattern) {
            return;
        }
        m_pattern = pattern;
        if (m_pattern.isEmpty()) {
            m_regex = QRegularExpression();
        } else {
            m_regex = QRegularExpression(
                QStringLiteral(".*%1.*").arg(QRegularExpression::escape(m_pattern)),
                QRegularExpression::CaseInsensitiveOption);
        }
        invalidateFilter();
    }

    void setGroupFilter(const QString &group)
    {
        if (QString::compare(m_group, group, Qt::CaseInsensitive) == 0) {
            return;
        }
        m_group = group;
        invalidateFilter();
    }

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override
    {
        const QModelIndex symbolIdx = sourceModel()->index(sourceRow, 1, sourceParent);
        if (!symbolIdx.isValid()) {
            return false;
        }
        if (!m_group.isEmpty()) {
            const QString dataGroup = symbolIdx.data(kGroupRole).toString();
            if (QString::compare(dataGroup, m_group, Qt::CaseInsensitive) != 0) {
                return false;
            }
        }
        if (m_regex.isValid() && !m_regex.pattern().isEmpty()) {
            const QString text = symbolIdx.data(Qt::DisplayRole).toString();
            if (!m_regex.match(text).hasMatch()) {
                return false;
            }
        }
        return true;
    }

private:
    QString m_pattern;
    QString m_group;
    QRegularExpression m_regex;
};

class SymbolPickerDialog : public QDialog {
    Q_OBJECT

public:
    explicit SymbolPickerDialog(QWidget *parent = nullptr);

    void setSymbols(const QStringList &symbols, const QSet<QString> &apiOff);
    void setAccounts(const QVector<QPair<QString, QColor>> &accounts);
    void setCurrentSymbol(const QString &symbol);
    void setCurrentAccount(const QString &account);
    QString selectedSymbol() const;
    QString selectedAccount() const;
    void setVenueAppearance(const QString &iconRelativePath, bool isFutures);
    void setRefreshInProgress(bool refreshing);

signals:
    void refreshRequested();
    void accountChanged(const QString &account);

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;

private slots:
    void handleFilterChanged(const QString &text);
    void handleActivated(const QModelIndex &index);
    void handleConnectionsChanged();
    void handleGroupChanged();

private:
    void acceptSelection();
    void selectFirstVisible();
    void rebuildGroupList();
    QString groupForSymbol(const QString &symbol) const;
    void updateTableColumnWidths();

    QLineEdit *m_filterEdit{nullptr};
    QTableView *m_tableView{nullptr};
    QStandardItemModel *m_model{nullptr};
    SymbolFilterProxy *m_proxy{nullptr};
    QListWidget *m_connectionsList{nullptr};
    QListWidget *m_groupList{nullptr};
    QToolButton *m_refreshButton{nullptr};
    QStringList m_allSymbols;
    QMap<QString, int> m_groupCounts;
    QString m_filterText;
    QString m_currentGroup;
    QString m_selected;
    QString m_selectedAccount;
    QSet<QString> m_apiOff;
    QIcon m_venueIcon;
    bool m_venueIsFutures{false};
    bool m_venueIsLighter{false};
    int m_hoverRow{-1};
    bool m_refreshInProgress{false};
};
