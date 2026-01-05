#include "ThemeManager.h"

#include <QApplication>
#include <QPalette>

ThemeManager *ThemeManager::instance()
{
    static ThemeManager *inst = new ThemeManager(qApp);
    return inst;
}

ThemeManager::ThemeManager(QObject *parent)
    : QObject(parent)
{
}

static QString normalizedModeName(const QString &mode)
{
    const QString m = mode.trimmed().toLower();
    if (m == QStringLiteral("system")) return QStringLiteral("System");
    if (m == QStringLiteral("dark")) return QStringLiteral("Dark");
    if (m == QStringLiteral("light")) return QStringLiteral("Light");
    if (m == QStringLiteral("oled") || m == QStringLiteral("amoled")) return QStringLiteral("OLED");
    return QStringLiteral("Dark");
}

void ThemeManager::setMode(const QString &mode)
{
    const QString next = normalizedModeName(mode);
    if (next == m_mode) {
        return;
    }
    m_mode = next;
    emit themeChanged();
}

void ThemeManager::setAccentColor(const QColor &c)
{
    if (!c.isValid() || c == m_accentColor) {
        return;
    }
    m_accentColor = c;
    emit themeChanged();
}

void ThemeManager::setBidColor(const QColor &c)
{
    if (!c.isValid() || c == m_bidColor) {
        return;
    }
    m_bidColor = c;
    emit themeChanged();
}

void ThemeManager::setAskColor(const QColor &c)
{
    if (!c.isValid() || c == m_askColor) {
        return;
    }
    m_askColor = c;
    emit themeChanged();
}

void ThemeManager::setPrintsSquare(bool enabled)
{
    if (enabled == m_printsSquare) {
        return;
    }
    m_printsSquare = enabled;
    emit themeChanged();
}

bool ThemeManager::isDarkBySystemPalette() const
{
    const QPalette pal = qApp ? qApp->palette() : QPalette();
    const QColor win = pal.color(QPalette::Window);
    return win.isValid() ? (win.lightness() < 128) : true;
}

static bool isEffectiveDark(const QString &mode, bool systemDark)
{
    if (mode == QLatin1String("Dark") || mode == QLatin1String("OLED")) return true;
    if (mode == QLatin1String("Light")) return false;
    return systemDark;
}

QColor ThemeManager::windowBackground() const
{
    const bool systemDark = isDarkBySystemPalette();
    if (m_mode == QLatin1String("OLED")) return QColor("#000000");
    if (isEffectiveDark(m_mode, systemDark)) return QColor("#181818");
    return QColor("#f5f6f8");
}

QColor ThemeManager::panelBackground() const
{
    const bool systemDark = isDarkBySystemPalette();
    if (m_mode == QLatin1String("OLED")) return QColor("#0b0b0b");
    if (isEffectiveDark(m_mode, systemDark)) return QColor("#1f1f1f");
    return QColor("#ffffff");
}

QColor ThemeManager::textPrimary() const
{
    const bool systemDark = isDarkBySystemPalette();
    if (isEffectiveDark(m_mode, systemDark)) return QColor("#e4e4e4");
    return QColor("#111318");
}

QColor ThemeManager::textSecondary() const
{
    const bool systemDark = isDarkBySystemPalette();
    if (isEffectiveDark(m_mode, systemDark)) return QColor("#9aa7ad");
    return QColor("#5b6770");
}

QColor ThemeManager::borderColor() const
{
    const bool systemDark = isDarkBySystemPalette();
    if (isEffectiveDark(m_mode, systemDark)) return QColor("#2b2b2b");
    return QColor("#d8dde3");
}

QColor ThemeManager::gridColor() const
{
    const bool systemDark = isDarkBySystemPalette();
    if (isEffectiveDark(m_mode, systemDark)) return QColor("#303030");
    return QColor("#e3e7ee");
}

QColor ThemeManager::selectionColor() const
{
    QColor c = m_accentColor.isValid() ? m_accentColor : QColor("#007acc");
    c.setAlpha(60);
    return c;
}

DomStyle ThemeManager::domStyle() const
{
    DomStyle s;
    s.background = panelBackground();
    s.text = textPrimary();
    s.grid = gridColor();
    s.bid = m_bidColor.isValid() ? m_bidColor : QColor("#4caf50");
    s.ask = m_askColor.isValid() ? m_askColor : QColor("#e53935");
    return s;
}
