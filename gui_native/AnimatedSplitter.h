#pragma once

#include <QColor>
#include <QSplitter>

class AnimatedSplitter : public QSplitter {
    Q_OBJECT

public:
    explicit AnimatedSplitter(Qt::Orientation orientation, QWidget *parent = nullptr);

    int durationMs() const { return m_durationMs; }

    int baseThickness() const { return m_baseThickness; }
    void setBaseThickness(int px);

    int expandedThickness() const { return m_expandedThickness; }
    void setExpandedThickness(int px);

    QColor baseColor() const { return m_baseColor; }
    void setBaseColor(const QColor &c);

    QColor highlightColor() const { return m_highlightColor; }
    void setHighlightColor(const QColor &c);

protected:
    QSplitterHandle *createHandle() override;

private:
    int m_baseThickness = 2;
    int m_expandedThickness = 4;
    int m_durationMs = 140;
    QColor m_baseColor = QColor(QStringLiteral("#303030"));
    QColor m_highlightColor = QColor(QStringLiteral("#007acc"));
};
