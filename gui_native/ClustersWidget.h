#pragma once

#include <QWidget>

class QQuickWidget;
class PrintsWidget;

class ClustersWidget : public QWidget {
    Q_OBJECT
public:
    explicit ClustersWidget(QWidget *parent = nullptr);

    void bindPrints(PrintsWidget *prints);
    void setRowLayout(int rowCount, int rowHeight, int infoAreaHeight);
    void setInfoAreaHeight(int infoAreaHeight) { setRowLayout(m_rowCount, m_rowHeight, infoAreaHeight); }
    int rowCountValue() const { return m_rowCount; }
    int rowHeightValue() const { return m_rowHeight; }

protected:
    void resizeEvent(QResizeEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

private:
    void ensureQuickInitialized();
    void syncQuickProperties();

    PrintsWidget *m_prints = nullptr;
    QQuickWidget *m_quickWidget = nullptr;
    bool m_quickReady = false;
    int m_rowCount = 0;
    int m_rowHeight = 20;
    int m_infoAreaHeight = 26;
};
