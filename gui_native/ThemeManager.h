#pragma once

#include <QObject>
#include <QColor>

#include "DomWidget.h"

class ThemeManager final : public QObject {
    Q_OBJECT

    Q_PROPERTY(QString mode READ mode WRITE setMode NOTIFY themeChanged)
    Q_PROPERTY(QColor accentColor READ accentColor WRITE setAccentColor NOTIFY themeChanged)
    Q_PROPERTY(QColor bidColor READ bidColor WRITE setBidColor NOTIFY themeChanged)
    Q_PROPERTY(QColor askColor READ askColor WRITE setAskColor NOTIFY themeChanged)
    Q_PROPERTY(bool printsSquare READ printsSquare WRITE setPrintsSquare NOTIFY themeChanged)

    Q_PROPERTY(QColor windowBackground READ windowBackground NOTIFY themeChanged)
    Q_PROPERTY(QColor panelBackground READ panelBackground NOTIFY themeChanged)
    Q_PROPERTY(QColor textPrimary READ textPrimary NOTIFY themeChanged)
    Q_PROPERTY(QColor textSecondary READ textSecondary NOTIFY themeChanged)
    Q_PROPERTY(QColor borderColor READ borderColor NOTIFY themeChanged)
    Q_PROPERTY(QColor gridColor READ gridColor NOTIFY themeChanged)
    Q_PROPERTY(QColor selectionColor READ selectionColor NOTIFY themeChanged)

public:
    static ThemeManager *instance();

    QString mode() const { return m_mode; }
    void setMode(const QString &mode);

    QColor accentColor() const { return m_accentColor; }
    void setAccentColor(const QColor &c);

    QColor bidColor() const { return m_bidColor; }
    void setBidColor(const QColor &c);

    QColor askColor() const { return m_askColor; }
    void setAskColor(const QColor &c);

    bool printsSquare() const { return m_printsSquare; }
    void setPrintsSquare(bool enabled);

    QColor windowBackground() const;
    QColor panelBackground() const;
    QColor textPrimary() const;
    QColor textSecondary() const;
    QColor borderColor() const;
    QColor gridColor() const;
    QColor selectionColor() const;

    DomStyle domStyle() const;

signals:
    void themeChanged();

private:
    explicit ThemeManager(QObject *parent = nullptr);
    bool isDarkBySystemPalette() const;

    QString m_mode = QStringLiteral("Dark"); // System | Dark | Light | OLED
    QColor m_accentColor = QColor(QStringLiteral("#007acc"));
    QColor m_bidColor = QColor(QStringLiteral("#4caf50"));
    QColor m_askColor = QColor(QStringLiteral("#e53935"));
    bool m_printsSquare = false;
};
