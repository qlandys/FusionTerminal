#include "MainWindow.h"
#include "ConnectionStore.h"
#include "ConnectionsWindow.h"
#include "TradesWindow.h"
#include "FinrezWindow.h"
#include "DomWidget.h"
#include "LadderClient.h"
#include "PluginsWindow.h"
#include "ChatWindow.h"
#include "PrintsWidget.h"
#include "ClustersWidget.h"
#include "SettingsWindow.h"
#include "ThemeManager.h"
#include "TradeManager.h"
#include "SymbolPickerDialog.h"
#include "AnimatedSplitter.h"
#include <QApplication>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QDateTime>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QScreen>
#include <QColor>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QInputDialog>
#include <QLabel>
#include <QAction>
#include <QActionGroup>
#include <QAbstractSpinBox>
#include <QComboBox>
#include <QDialog>
#include <QLineEdit>
#include <QMenu>
#include <QButtonGroup>
#include <QMouseEvent>
#include <QWheelEvent>
#include <climits>
#include <cstdlib>
#include <utility>
#include <QPlainTextEdit>
#include <QTextEdit>
#include <cmath>
#include <QPixmap>
#include <QPainter>
#include <QPaintEvent>
#include <QCursor>
#include <QPushButton>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTabBar>
#include <QTimer>
#include <QToolButton>
#include <QSignalBlocker>
#include <QScrollArea>
#include <QScrollBar>
#include <QPointer>
#include <QSizeGrip>
#include <QVariant>
#include <QVBoxLayout>
#include <QVariantAnimation>
#include <QStyleOptionSlider>
#include <QEnterEvent>
#include <QWidget>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif
#include <QWindow>
#include <QDebug>
#include <QFile>
#include <QFile>
#include <QTextStream>
#include <QProcessEnvironment>
#include <QProcess>
#include <QPropertyAnimation>
#include <QEasingCurve>

namespace {
QString formatClusterQtyShort(double v)
{
    const double av = std::abs(v);
    if (av >= 1000000.0) {
        const double m = av / 1000000.0;
        const int prec = (m >= 100.0) ? 0 : (m >= 10.0 ? 1 : 2);
        return QString::number(m, 'f', prec) + "M";
    }
    if (av >= 1000.0) {
        const double k = av / 1000.0;
        const int prec = (k >= 100.0) ? 0 : (k >= 10.0 ? 1 : 2);
        return QString::number(k, 'f', prec) + "K";
    }
    const int prec = (av >= 100.0) ? 0 : (av >= 10.0 ? 1 : 2);
    return QString::number(av, 'f', prec);
}

class ClustersFooterOverlay final : public QWidget {
public:
    explicit ClustersFooterOverlay(QWidget *parent, PrintsWidget *prints, ClustersWidget *clusters)
        : QWidget(parent)
        , m_prints(prints)
        , m_clusters(clusters)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
        setAttribute(Qt::WA_NoSystemBackground, true);
        setAutoFillBackground(false);
        setFixedHeight(26);
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        Q_UNUSED(event);
        auto *theme = ThemeManager::instance();
        QPainter p(this);
        p.fillRect(rect(), theme->panelBackground());
        p.setPen(theme->gridColor());
        p.drawLine(0, 0, width(), 0);
        p.drawLine(0, height() / 2, width(), height() / 2);

        if (!m_prints) {
            return;
        }

        const int bucketCount = std::clamp(m_prints->clusterBucketCount(), 1, PrintsWidget::kMaxClusterBuckets);
        int colWidth = std::max(18, width() / std::max(1, bucketCount));
        int visibleCount = bucketCount;
        int bucketOffset = 0;
        int xOrigin = 0;
        if (m_clusters) {
            colWidth = std::max(18, m_clusters->effectiveColumnWidth());
            visibleCount = std::clamp(m_clusters->visibleBucketCount(), 1, bucketCount);
            bucketOffset = std::clamp(m_clusters->bucketOffset(), 0, std::max(0, bucketCount - visibleCount));
            xOrigin = m_clusters->xOrigin();
        }
        const int ms = std::max(100, m_prints->clusterWindowMs());
        const QVector<qint64> startMs = m_prints->clusterBucketStartMs();
        const QVector<double> totals = m_prints->clusterBucketTotals();
        const bool showSeconds = ms < 60000;

        QFont f = font();
        const int basePx = f.pixelSize() > 0 ? f.pixelSize() : std::max(10, f.pointSize() + 2);
        const int maxPx = std::max(8, basePx - 2);

        auto drawFit = [&](const QRect &r, const QString &text, const QColor &color, bool bold) {
            if (text.isEmpty() || r.width() <= 2 || r.height() <= 2) {
                return;
            }
            int px = maxPx;
            QFont tf = font();
            tf.setBold(bold);
            for (; px >= 5; --px) {
                tf.setPixelSize(px);
                const QFontMetrics m(tf);
                if (m.horizontalAdvance(text) <= r.width() && m.height() <= r.height()) {
                    break;
                }
            }
            tf.setPixelSize(std::max(5, px));
            p.setFont(tf);
            p.setPen(color);
            p.drawText(r, Qt::AlignCenter, text);
        };

        for (int c = 0; c < visibleCount; ++c) {
            const int srcCol = bucketOffset + c;
            const QRect colRect(xOrigin + c * colWidth, 0, colWidth, height());
            p.save();
            p.setClipRect(colRect);

            const double tv = (srcCol < totals.size()) ? totals[srcCol] : 0.0;
            const QString totText = tv > 0.0 ? formatClusterQtyShort(tv) : QString();

            QString timeText;
            const qint64 ts = (srcCol < startMs.size()) ? startMs[srcCol] : 0;
            if (ts > 0) {
                const QDateTime dt = QDateTime::fromMSecsSinceEpoch(ts);
                if (ms >= 86400000) {
                    timeText = dt.toString(QStringLiteral("dd.MM"));
                } else {
                    if (showSeconds) {
                        timeText = (colWidth < 44) ? dt.toString(QStringLiteral("mm:ss"))
                                                   : dt.toString(QStringLiteral("HH:mm:ss"));
                    } else {
                        timeText = dt.toString(QStringLiteral("HH:mm"));
                    }
                }
            }

            const QRect topRect(colRect.left() + 1, 1, colRect.width() - 2, (height() / 2) - 2);
            const QRect botRect(colRect.left() + 1, (height() / 2) + 1, colRect.width() - 2, (height() / 2) - 2);

            drawFit(topRect, totText, theme->textPrimary(), true);
            drawFit(botRect, timeText, theme->textSecondary(), false);
            p.restore();
        }

        p.setPen(theme->gridColor());
        for (int c = 0; c <= visibleCount; ++c) {
            const qreal x = static_cast<qreal>(xOrigin + c * colWidth) + 0.5;
            p.drawLine(QPointF(x, 0.0), QPointF(x, static_cast<qreal>(height())));
        }
    }

private:
    QPointer<PrintsWidget> m_prints;
    QPointer<ClustersWidget> m_clusters;
};

Qt::Edges resizeEdgesForPos(QWidget *window, const QPoint &globalPos, int borderWidth)
{
    if (!window || borderWidth <= 0) {
        return Qt::Edges();
    }
    const QRect rect = window->frameGeometry();
    Qt::Edges edges;
    if (globalPos.x() - rect.left() <= borderWidth) {
        edges |= Qt::LeftEdge;
    } else if (rect.right() - globalPos.x() <= borderWidth) {
        edges |= Qt::RightEdge;
    }
    if (globalPos.y() - rect.top() <= borderWidth) {
        edges |= Qt::TopEdge;
    } else if (rect.bottom() - globalPos.y() <= borderWidth) {
        edges |= Qt::BottomEdge;
    }
    return edges;
}

Qt::CursorShape cursorForEdges(Qt::Edges edges)
{
    const bool left = edges.testFlag(Qt::LeftEdge);
    const bool right = edges.testFlag(Qt::RightEdge);
    const bool top = edges.testFlag(Qt::TopEdge);
    const bool bottom = edges.testFlag(Qt::BottomEdge);

    if ((left && top) || (right && bottom)) {
        return Qt::SizeFDiagCursor;
    }
    if ((right && top) || (left && bottom)) {
        return Qt::SizeBDiagCursor;
    }
    if (left || right) {
        return Qt::SizeHorCursor;
    }
    if (top || bottom) {
        return Qt::SizeVerCursor;
    }
    return Qt::ArrowCursor;
}
}
#include <QTransform>
#include <QUrl>
#include <QCloseEvent>
#include <QCompleter>
#include <QKeyEvent>
#include <QStandardPaths>
#include <QSettings>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QCryptographicHash>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardItemModel>
#include <QComboBox>
#include <QPointer>
#include <QPair>
#include <QPainterPath>
#include <QRandomGenerator>
#include <QElapsedTimer>
#include <QDesktopServices>
#include <QTabWidget>
#include <QQuickWidget>
#include <cmath>
#include <algorithm>
#include <limits>
#include <QAbstractSlider>
#include <QListWidget>
#include <QAbstractItemView>
#include <QRegularExpression>
#include <QVersionNumber>
#include <memory>

namespace {
int compareVersionStrings(const QString &lhs, const QString &rhs)
{
    const QString a = lhs.trimmed();
    const QString b = rhs.trimmed();
    auto normalize = [](const QString &in) {
        const QString s = in.trimmed();
        return s.startsWith(QLatin1Char('v')) ? s.mid(1) : s;
    };
    auto splitPre = [](const QString &in, QString &base, QString &pre) {
        const int dash = in.indexOf(QLatin1Char('-'));
        if (dash >= 0) {
            base = in.left(dash);
            pre = in.mid(dash + 1);
        } else {
            base = in;
            pre.clear();
        }
    };
    auto parsePre = [](const QString &pre) {
        QString prefix;
        int number = -1;
        int i = 0;
        while (i < pre.size() && pre[i].isLetter()) {
            prefix.append(pre[i].toLower());
            ++i;
        }
        if (i < pre.size()) {
            bool ok = false;
            const int n = pre.mid(i).toInt(&ok);
            if (ok) {
                number = n;
            }
        }
        return qMakePair(prefix, number);
    };

    const QString na = normalize(a);
    const QString nb = normalize(b);
    QString baseA;
    QString preA;
    QString baseB;
    QString preB;
    splitPre(na, baseA, preA);
    splitPre(nb, baseB, preB);

    const QVersionNumber va = QVersionNumber::fromString(baseA);
    const QVersionNumber vb = QVersionNumber::fromString(baseB);
    if (!va.isNull() && !vb.isNull()) {
        const int baseCmp = QVersionNumber::compare(va, vb);
        if (baseCmp != 0) {
            return baseCmp;
        }
        if (preA.isEmpty() && preB.isEmpty()) {
            return 0;
        }
        if (preA.isEmpty()) {
            return 1;
        }
        if (preB.isEmpty()) {
            return -1;
        }
        const auto pa = parsePre(preA);
        const auto pb = parsePre(preB);
        const int preCmp = QString::compare(pa.first, pb.first, Qt::CaseInsensitive);
        if (preCmp != 0) {
            return preCmp;
        }
        if (pa.second == pb.second) {
            return 0;
        }
        if (pa.second < 0 || pb.second < 0) {
            return QString::compare(preA, preB, Qt::CaseInsensitive);
        }
        return pa.second < pb.second ? -1 : 1;
    }
    return QString::compare(na, nb, Qt::CaseInsensitive);
}
} // namespace

class PillLabel final : public QLabel {
public:
    explicit PillLabel(QWidget *parent = nullptr) : QLabel(parent)
    {
        setAttribute(Qt::WA_OpaquePaintEvent, false);
    }

    void setPillColors(const QColor &bg, const QColor &border, const QColor &fg)
    {
        m_bg = bg;
        m_border = border;
        m_fg = fg;
        update();
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        Q_UNUSED(event);
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setRenderHint(QPainter::TextAntialiasing, true);

        QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        const qreal radius = std::max<qreal>(2.0, r.height() / 2.0);

        QPen pen(m_border);
        pen.setCosmetic(true);
        p.setPen(pen);
        p.setBrush(m_bg);
        p.drawRoundedRect(r, radius, radius);

        p.setPen(m_fg);
        p.setFont(font());
        p.drawText(rect(), alignment(), text());
    }

private:
    QColor m_bg = QColor(0, 0, 0, 60);
    QColor m_border = QColor(255, 255, 255, 120);
    QColor m_fg = QColor(Qt::white);
};

class PillToolButton final : public QToolButton {
public:
    explicit PillToolButton(QWidget *parent = nullptr) : QToolButton(parent)
    {
        setAttribute(Qt::WA_OpaquePaintEvent, false);
        setMouseTracking(true);
    }

    void setPillColors(const QColor &bg, const QColor &border, const QColor &fg)
    {
        m_bg = bg;
        m_border = border;
        m_fg = fg;
        update();
    }

protected:
    void enterEvent(QEnterEvent *event) override
    {
        m_hovered = true;
        update();
        QToolButton::enterEvent(event);
    }

    void leaveEvent(QEvent *event) override
    {
        m_hovered = false;
        update();
        QToolButton::leaveEvent(event);
    }

    void paintEvent(QPaintEvent *event) override
    {
        Q_UNUSED(event);
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setRenderHint(QPainter::TextAntialiasing, true);

        QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        const qreal radius = std::max<qreal>(2.0, r.height() / 2.0);

        QColor bg = m_bg;
        if (m_hovered) {
            bg = bg.isValid() ? bg.lighter(112) : QColor(255, 255, 255, 24);
        }
        if (isDown()) {
            bg = bg.isValid() ? bg.darker(112) : QColor(0, 0, 0, 80);
        }

        QPen pen(m_border);
        pen.setCosmetic(true);
        p.setPen(pen);
        p.setBrush(bg);
        p.drawRoundedRect(r, radius, radius);

        p.setPen(m_fg);
        p.setFont(font());
        p.drawText(rect(), Qt::AlignCenter, text());
    }

private:
    QColor m_bg = QColor(0, 0, 0, 60);
    QColor m_border = QColor(255, 255, 255, 120);
    QColor m_fg = QColor(Qt::white);
    bool m_hovered = false;
};

namespace {
constexpr int kPendingCancelRetryMs = 250;
constexpr int kMaxPendingMarkersPerBucket = 20;
constexpr double kMarkerPriceTolerance = 1e-8;
constexpr int kLocalMarkerDelayMs = 120;
constexpr qint64 kPendingMarkerTimeoutMs = 8000;
constexpr int kMinBaseLadderLevels = 50;
constexpr int kMaxBaseLadderLevels = 4000;
constexpr int kMaxEffectiveLadderLevels = 20000;

class UiStallMonitor final : public QObject {
    Q_OBJECT
public:
    explicit UiStallMonitor(QObject *parent = nullptr)
        : QObject(parent)
    {
        int thresholdMs = qEnvironmentVariableIntValue("FUSION_STALL_THRESHOLD_MS");
        if (thresholdMs <= 0) {
            thresholdMs = 40;
        }
        m_thresholdMs = std::clamp(thresholdMs, 5, 1000);
        m_log = qEnvironmentVariableIntValue("FUSION_STALL_LOG") > 0;

        m_timer.setTimerType(Qt::PreciseTimer);
        m_timer.setInterval(8);
        connect(&m_timer, &QTimer::timeout, this, &UiStallMonitor::tick);
        m_elapsed.start();
        m_lastMs = m_elapsed.elapsed();
        m_timer.start();
    }

private slots:
    void tick()
    {
        const qint64 now = m_elapsed.elapsed();
        const qint64 dt = now - m_lastMs;
        m_lastMs = now;
        if (dt > m_thresholdMs) {
            ++m_stallCount;
            m_maxStallMs = std::max(m_maxStallMs, dt);
            if (m_log) {
                qWarning() << "[ui-stall]" << dt << "ms (max" << m_maxStallMs << "ms) count" << m_stallCount;
            }
        }
    }

private:
    QTimer m_timer;
    QElapsedTimer m_elapsed;
    qint64 m_lastMs = 0;
    int m_thresholdMs = 40;
    bool m_log = false;
    qint64 m_stallCount = 0;
    qint64 m_maxStallMs = 0;
};

static QStringList toStringList(const QList<int> &sizes)
{
    QStringList out;
    out.reserve(sizes.size());
    for (int v : sizes) {
        out.push_back(QString::number(v));
    }
    return out;
}

static QList<int> toIntList(const QVariant &value)
{
    QList<int> out;
    if (!value.isValid()) {
        return out;
    }
    if (value.userType() == QMetaType::QVariantList) {
        const QVariantList list = value.toList();
        out.reserve(list.size());
        for (const QVariant &v : list) {
            out.push_back(v.toInt());
        }
        return out;
    }
    if (value.userType() == QMetaType::QStringList) {
        const QStringList list = value.toStringList();
        out.reserve(list.size());
        for (const QString &s : list) {
            out.push_back(s.toInt());
        }
        return out;
    }
    const QString s = value.toString();
    if (!s.isEmpty() && s.contains(QLatin1Char(','))) {
        const QStringList parts = s.split(QLatin1Char(','), Qt::SkipEmptyParts);
        out.reserve(parts.size());
        for (const QString &p : parts) {
            out.push_back(p.trimmed().toInt());
        }
    }
    return out;
}

static void applySplitterSizes(QSplitter *splitter, const QList<int> &sizes, const QList<int> &fallback)
{
    if (!splitter) {
        return;
    }
    if (sizes.size() == splitter->count()) {
        splitter->setSizes(sizes);
        return;
    }
    if (fallback.size() == splitter->count()) {
        splitter->setSizes(fallback);
    }
}

static void applyNewColumnWidthDefaults(QSplitter *columnsSplitter, QWidget *spacerWidget, QWidget *newColumnWidget)
{
    if (!columnsSplitter || !newColumnWidget) {
        return;
    }
    const int count = columnsSplitter->count();
    if (count <= 1) {
        return;
    }
    QList<int> sizes = columnsSplitter->sizes();
    if (sizes.size() != count) {
        return;
    }

    int spacerIndex = -1;
    if (spacerWidget) {
        spacerIndex = columnsSplitter->indexOf(spacerWidget);
    }
    const int newIndex = columnsSplitter->indexOf(newColumnWidget);
    if (newIndex < 0 || newIndex >= count) {
        return;
    }

    QVector<int> columnIndices;
    columnIndices.reserve(count);
    for (int i = 0; i < count; ++i) {
        if (i == spacerIndex) {
            continue;
        }
        columnIndices.push_back(i);
    }
    if (columnIndices.size() <= 1) {
        return;
    }

    int totalColumnsSize = 0;
    for (int idx : columnIndices) {
        totalColumnsSize += std::max(0, sizes[idx]);
    }
    if (totalColumnsSize <= 0) {
        return;
    }

    const int colCount = static_cast<int>(columnIndices.size());
    const int desiredNew = std::max(60, totalColumnsSize / colCount);
    const int existingSum = std::max(1, totalColumnsSize - std::max(0, sizes[newIndex]));
    const int remaining = std::max(60 * (colCount - 1), totalColumnsSize - desiredNew);
    const double scale = static_cast<double>(remaining) / static_cast<double>(existingSum);

    for (int idx : columnIndices) {
        if (idx == newIndex) continue;
        const int next = std::max(60, static_cast<int>(std::round(std::max(0, sizes[idx]) * scale)));
        sizes[idx] = next;
    }
    sizes[newIndex] = desiredNew;
    columnsSplitter->setSizes(sizes);
}

static QColor contrastTextColor(const QColor &bg)
{
    const QColor c = bg.isValid() ? bg : QColor("#3a7bd5");
    // YIQ-like quick heuristic using lightness; good enough for solid header fills.
    return c.lightness() < 140 ? QColor("#ffffff") : QColor("#111318");
}

static QColor badgeBackgroundFor(const QColor &bg)
{
    const QColor fg = contrastTextColor(bg);
    QColor badge = (fg.lightness() < 128) ? QColor(255, 255, 255, 170) : QColor(0, 0, 0, 90);
    return badge;
}

QString normalizedSymbolKey(const QString &symbol)
{
    return symbol.trimmed().toUpper();
}

QString normalizedAccountLabel(const QString &account)
{
    const QString trimmed = account.trimmed();
    return trimmed.isEmpty() ? QStringLiteral("MEXC Spot") : trimmed;
}

QString normalizedAccountKey(const QString &account)
{
    return normalizedAccountLabel(account).toLower();
}

bool isPlaceholderAccountLabel(const QString &account)
{
    const QString lower = account.trimmed().toLower();
    if (lower.isEmpty()) {
        return true;
    }
    static const QStringList placeholders = {
        QStringLiteral("mexc spot"),
        QStringLiteral("mexc futures"),
        QStringLiteral("uzx spot"),
        QStringLiteral("uzx swap")
    };
    return placeholders.contains(lower);
}

QString markerTimerKey(const QString &symbolUpper,
                       const QString &accountKey,
                       const QString &orderId = QString())
{
    QString key = symbolUpper + QLatin1Char('|') + accountKey;
    if (!orderId.isEmpty()) {
        key += QLatin1Char('|') + orderId;
    }
    return key;
}

QString markerTimerPrefix(const QString &symbolUpper, const QString &accountKey = QString())
{
    QString prefix = symbolUpper + QLatin1Char('|');
    if (!accountKey.isEmpty()) {
        prefix += accountKey + QLatin1Char('|');
    }
    return prefix;
}

QString lastLimitOrderKey(const QString &symbolUpper, const QString &accountKey)
{
    return symbolUpper + QLatin1Char('|') + accountKey;
}

}
#if __has_include(<QMediaPlayer>)
#include <QMediaPlayer>
#define HAS_QMEDIAPLAYER 1
#else
#define HAS_QMEDIAPLAYER 0
#endif

#if __has_include(<QAudioOutput>)
#include <QAudioOutput>
#define HAS_QAUDIOOUTPUT 1
#else
#define HAS_QAUDIOOUTPUT 0
#endif

#if __has_include(<QSoundEffect>)
#include <QSoundEffect>
#define HAS_QSOUNDEFFECT 1
#else
#define HAS_QSOUNDEFFECT 0
#endif

#if HAS_QSOUNDEFFECT
static void restartSound(QSoundEffect *effect)
{
    if (!effect) {
        return;
    }
    effect->stop();
    effect->play();
}
#endif

#if HAS_QMEDIAPLAYER && HAS_QAUDIOOUTPUT
static void restartSound(QMediaPlayer *player)
{
    if (!player) {
        return;
    }
    // For MP3, "seeking" to 0 without stopping can sound glitchy/trimmed on some backends.
    // Stop -> position=0 -> play matches the notification sound behavior and is more reliable.
    player->stop();
    player->setPosition(0);
    player->play();
}
#endif

void MainWindow::ensureSoundsInitialized()
{
    if (m_soundsInitialized) {
        return;
    }
    m_soundsInitialized = true;

    // Lazily initialize audio on first use to avoid startup "beep" on some systems/drivers.
#if HAS_QSOUNDEFFECT
    if (!m_notificationEffect) {
        m_notificationEffect = new QSoundEffect(this);
        m_notificationEffect->setLoopCount(1);
        m_notificationEffect->setSource(QUrl(QStringLiteral("qrc:/sounds/notification.wav")));
        m_notificationEffect->setVolume(0.85);
    }

    if (!m_successEffect) {
        m_successEffect = new QSoundEffect(this);
        m_successEffect->setLoopCount(1);
        m_successEffect->setSource(QUrl(QStringLiteral("qrc:/sounds/success.wav")));
        m_successEffect->setVolume(0.90);
    }
#else
#if HAS_QMEDIAPLAYER && HAS_QAUDIOOUTPUT
    if (!m_notificationPlayer) {
        const QString notifSoundPath = resolveAssetPath(QStringLiteral("assets/sounds/notification.mp3"));
        if (!notifSoundPath.isEmpty() && QFile::exists(notifSoundPath)) {
            m_notificationOutput = new QAudioOutput(this);
            m_notificationOutput->setVolume(0.85);
            m_notificationPlayer = new QMediaPlayer(this);
            m_notificationPlayer->setAudioOutput(m_notificationOutput);
            m_notificationPlayer->setSource(QUrl::fromLocalFile(notifSoundPath));
        }
    }
    if (!m_successPlayer) {
        const QString successSoundPath = resolveAssetPath(QStringLiteral("assets/sounds/success.mp3"));
        if (!successSoundPath.isEmpty() && QFile::exists(successSoundPath)) {
            m_successOutput = new QAudioOutput(this);
            m_successOutput->setVolume(0.9);
            m_successPlayer = new QMediaPlayer(this);
            m_successPlayer->setAudioOutput(m_successOutput);
            m_successPlayer->setSource(QUrl::fromLocalFile(successSoundPath));
        }
    }
#endif
#endif
}

namespace {
constexpr int kDomColumnMinWidth = 140;
constexpr int kBaseDepthChunkTicks = 1200;
constexpr double kDepthExtendThreshold = 0.18;
constexpr int kGuiLevelsPerSide = 3000;
constexpr int kBackgroundMarginLevels = 1500;
constexpr double kDisplayPrefetchGuardRatio = 0.35;

qint64 depthChunkTicks(int tickCompression,
                       const QScrollBar *scrollBar,
                       bool hasBuffer,
                       qint64 bufferMinTick,
                       qint64 bufferMaxTick)
{
    const qint64 base = static_cast<qint64>(kBaseDepthChunkTicks);
    qint64 page = 0;
    if (scrollBar) {
        page = static_cast<qint64>(std::max(1, scrollBar->pageStep()));
    }
    const qint64 compression = std::max<qint64>(1, tickCompression);
    if (page <= 0) {
        page = compression * 200;
    }

    qint64 chunk = std::max(base, page);

    const qint64 bufferSpan = (hasBuffer && bufferMaxTick >= bufferMinTick)
                                  ? (bufferMaxTick - bufferMinTick + 1)
                                  : 0;
    if (bufferSpan > 0) {
        const qint64 half = std::max<qint64>(base, bufferSpan / 2);
        if (chunk >= bufferSpan) {
            chunk = half;
        } else {
            const qint64 maxWithOverlap = std::max<qint64>(base, bufferSpan - page);
            if (maxWithOverlap > 0) {
                chunk = std::min(chunk, maxWithOverlap);
            }
        }
    }

    return std::max(base, chunk);
}

qint64 maxQueuedShiftTicks(int tickCompression,
                           const QScrollBar *scrollBar,
                           bool hasBuffer,
                           qint64 bufferMinTick,
                           qint64 bufferMaxTick)
{
    return depthChunkTicks(tickCompression, scrollBar, hasBuffer, bufferMinTick, bufferMaxTick) * 12;
}
}

#if 0
class DomScrollBar : public QScrollBar {
    Q_OBJECT
public:
    explicit DomScrollBar(Qt::Orientation orientation, QWidget *parent = nullptr)
        : QScrollBar(orientation, parent)
        , m_hoverAnim(new QVariantAnimation(this))
        , m_throttleTimer(new QTimer(this))
        , m_visualTimer(new QTimer(this))
    {
        setMouseTracking(true);
        setAttribute(Qt::WA_Hover, true);
        setFixedWidth(6);
        m_hoverAnim->setDuration(140);
        m_hoverAnim->setEasingCurve(QEasingCurve::InOutCubic);
        connect(m_hoverAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
            m_hoverProgress = value.toReal();
            update();
        });
        m_throttleTimer->setInterval(16);
        connect(m_throttleTimer, &QTimer::timeout, this, &DomScrollBar::handleThrottleTick);
        m_visualTimer->setInterval(16);
        connect(m_visualTimer, &QTimer::timeout, this, &DomScrollBar::tickVisualEffects);
        m_visualTimer->start();
        connect(this, &QScrollBar::valueChanged, this, [this]() {
            updateSliderRatio();
        });
        connect(this, &QScrollBar::rangeChanged, this, [this]() {
            updateSliderRatio();
        });
        updateSliderRatio();
    }

protected:
    void enterEvent(QEnterEvent *event) override
    {
        startHoverAnimation(1.0);
        QScrollBar::enterEvent(event);
    }

    void leaveEvent(QEvent *event) override
    {
        if (!m_dragActive) {
            startHoverAnimation(0.0);
        }
        QScrollBar::leaveEvent(event);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            m_dragActive = true;
            m_dragStartPos = event->pos();
            m_dragOffset = 0.0;
            m_velocityAccumulator = 0.0;
            startHoverAnimation(1.0);
            m_throttleTimer->start();
            emit sliderPressed();
            update();
            event->accept();
            return;
        }
        startHoverAnimation(1.0);
        QScrollBar::mousePressEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (m_dragActive && event->button() == Qt::LeftButton) {
            m_dragActive = false;
            m_dragOffset = 0.0;
            m_velocityAccumulator = 0.0;
            m_throttleTimer->stop();
            startHoverAnimation(underMouse() ? 1.0 : 0.0);
            emit sliderReleased();
            update();
            event->accept();
            return;
        }
        startHoverAnimation(underMouse() ? 1.0 : 0.0);
        QScrollBar::mouseReleaseEvent(event);
    }

    void paintEvent(QPaintEvent *event) override
    {
        Q_UNUSED(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

        const QRect trackRect = rect();
        const QRectF inner = trackRect.adjusted(1, 1, -1, -1);
        const double strength = m_visualStrength;
        QPainterPath trackPath;
        trackPath.addRoundedRect(inner, 2.5, 2.5);
        QColor trackColor(0x10, 0x10, 0x10);
        const double flicker =
            0.6 + 0.4 * std::sin(m_glowPhase * (1.2 + strength * 1.6));
        const double glowAlpha =
            std::clamp<double>(strength * 0.35 * flicker, 0.0, 0.45);
        QColor glowColor(0x5a, 0xab, 0xff);
        glowColor.setAlphaF(glowAlpha);
        painter.fillPath(trackPath, trackColor);
        if (glowAlpha > 0.0) {
            painter.save();
            painter.setClipPath(trackPath);
            painter.fillRect(inner.adjusted(-1, 0, 1, 0), glowColor);
            painter.restore();
        }
        painter.setPen(QPen(QColor(0x28, 0x28, 0x28), 1));
        painter.drawPath(trackPath);

        const QPointF center(inner.center());
        const double haloStrength = std::max(strength, static_cast<double>(m_hoverProgress));
        if (haloStrength > 0.0) {
            const double haloFlicker =
                0.7 + 0.3 * std::sin(m_glowPhase * (1.7 + haloStrength * 1.6));
            QRadialGradient halo(center, inner.width() * 0.55);
            QColor haloColor(0x66, 0xb7, 0xff);
            haloColor.setAlphaF(
                std::clamp((0.15 + haloStrength * 0.4) * haloFlicker, 0.0, 0.75));
            halo.setColorAt(0.0, haloColor);
            halo.setColorAt(1.0, QColor(0x00, 0x00, 0x00, 0));
            painter.setPen(Qt::NoPen);
            painter.setBrush(halo);
            painter.drawEllipse(center, inner.width() * 0.45, inner.width() * 0.45);
        }

        const double beamLength = strength * inner.height() * 0.52;
        const double beamWidth = inner.width() * 0.9;
        const int direction =
            (m_particleDirection != 0) ? m_particleDirection : ((m_lastNorm < 0.0) ? -1 : 1);
        QRectF beamRect;
        if (beamLength > 2.0) {
            if (direction < 0) {
                beamRect = QRectF(center.x() - beamWidth / 2.0,
                                  center.y() - beamLength,
                                  beamWidth,
                                  beamLength);
            } else {
                beamRect = QRectF(center.x() - beamWidth / 2.0,
                                  center.y(),
                                  beamWidth,
                                  beamLength);
            }
            QLinearGradient grad(beamRect.topLeft(), beamRect.bottomLeft());
            QColor topColor(0x8e, 0xd9, 0xff, static_cast<int>(120 + 110 * strength));
            QColor bottomColor(0x3a, 0x58, 0xff, static_cast<int>(70 + 120 * strength));
            if (direction < 0) {
                std::swap(topColor, bottomColor);
            }
            grad.setColorAt(0.0, topColor);
            grad.setColorAt(1.0, bottomColor);
            QPainterPath beamPath;
            beamPath.addRoundedRect(beamRect, beamWidth * 0.4, beamWidth * 0.4);
            painter.fillPath(beamPath, grad);
        }

        if (!m_particles.isEmpty() && beamLength > 2.0) {
            painter.setPen(Qt::NoPen);
            for (const auto &particle : m_particles) {
                const double pos = std::clamp(particle.t, 0.0, 1.0);
                double y = center.y();
                if (direction < 0) {
                    y = beamRect.bottom() - pos * beamRect.height();
                } else {
                    y = beamRect.top() + pos * beamRect.height();
                }
                const double lateral = particle.lateral * (inner.width() * 0.25);
                const double radius = particle.size;
                QRectF r(center.x() + lateral - radius, y - radius, radius * 2.0, radius * 2.0);
                QColor particleColor(0xa8, 0xd6, 0xff);
                particleColor.setAlphaF(std::clamp(particle.alpha, 0.0, 1.0));
                painter.setBrush(particleColor);
                painter.drawEllipse(r);
            }
        }

        Q_UNUSED(direction);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (m_dragActive) {
            const double maxOffset = maxDragDistance();
            m_dragOffset = event->pos().y() - m_dragStartPos.y();
            if (m_dragOffset > maxOffset) {
                m_dragOffset = maxOffset;
            } else if (m_dragOffset < -maxOffset) {
                m_dragOffset = -maxOffset;
            }
            update();
            event->accept();
            return;
        }
        QScrollBar::mouseMoveEvent(event);
    }

private:
    void startHoverAnimation(qreal target)
    {
        if (qFuzzyCompare(m_hoverProgress, target)) {
            return;
        }
        m_hoverAnim->stop();
        m_hoverAnim->setStartValue(m_hoverProgress);
        m_hoverAnim->setEndValue(target);
        m_hoverAnim->start();
    }

    double maxDragDistance() const
    {
        return std::max(20.0, height() * 0.45);
    }

    double normalizedOffset() const
    {
        const double maxDist = maxDragDistance();
        if (maxDist <= 0.0) {
            return 0.0;
        }
        double norm = m_dragOffset / maxDist;
        if (norm > 1.0) norm = 1.0;
        if (norm < -1.0) norm = -1.0;
        return norm;
    }

    void tickVisualEffects()
    {
        const double smoothing = 0.35;
        if (m_dragActive) {
            m_visualStrength += (m_targetStrength - m_visualStrength) * smoothing;
        } else {
            m_targetStrength = 0.0;
            m_visualStrength *= 0.90;
            if (m_visualStrength < 0.01) {
                m_visualStrength = 0.0;
            }
        }
        m_glowPhase += 0.08 + m_visualStrength * 0.12;
        if (m_glowPhase > 6.283185307179586) {
            m_glowPhase -= 6.283185307179586;
        }
        const double norm = m_dragActive ? m_lastNorm : 0.0;
        advanceEnergyParticles(norm);
        update();
    }

    void advanceEnergyParticles(double norm)
    {
        const double magnitude = std::abs(norm);
        if (magnitude > 0.02) {
            m_particleDirection = (norm < 0.0) ? -1 : 1;
        }
        const double strength = m_visualStrength;
        for (int i = m_particles.size() - 1; i >= 0; --i) {
            auto &p = m_particles[i];
            const double travelFactor = 0.18 + strength * 0.85;
            p.t += p.speed * travelFactor;
            if (p.t >= 1.0) {
                m_particles.removeAt(i);
                continue;
            }
            if (strength <= 0.01) {
                p.alpha *= 0.9;
                if (p.alpha < 0.03) {
                    m_particles.removeAt(i);
                }
            }
        }
        if (strength <= 0.01) {
            return;
        }
        const int targetParticles = static_cast<int>(2 + strength * 9);
        while (m_particles.size() < targetParticles) {
            spawnEnergyParticle(strength);
        }
    }

    void spawnEnergyParticle(double strength)
    {
        EnergyParticle p;
        p.t = QRandomGenerator::global()->bounded(200) / 1000.0;
        p.speed = 0.006 + QRandomGenerator::global()->bounded(12) / 1400.0 + strength * 0.03;
        p.alpha = 0.18 + strength * 0.4;
        p.size = 0.7 + strength * 1.6 + QRandomGenerator::global()->bounded(80) / 200.0;
        p.lateral = (QRandomGenerator::global()->bounded(2000) / 1000.0) - 1.0;
        m_particles.push_back(p);
    }

signals:
    void joystickEdgeHold(int direction, double strength);

private:
    struct EnergyParticle {
        double t = 0.0;
        double speed = 0.02;
        double alpha = 0.4;
        double size = 2.0;
        double lateral = 0.0;
    };

    void handleThrottleTick()
    {
        if (!m_dragActive) {
            return;
        }
        const double norm = normalizedOffset();
        if (qFuzzyIsNull(norm)) {
            return;
        }
        const double magnitude = std::abs(norm);
        const double shaped = norm * magnitude; // sign * mag^2 keeps center slow
        const double baseSpeed =
            std::max<double>(0.6, static_cast<double>(pageStep()) * kJoystickBaseSpeedFactor);
        const double speed = baseSpeed * (1.0 + magnitude * kJoystickMaxBoost);
        m_velocityAccumulator += shaped * speed;
        const int delta = static_cast<int>(m_velocityAccumulator);
        m_targetStrength = magnitude;
        m_lastNorm = norm;
        advanceEnergyParticles(norm);
        update();
        if (delta == 0) {
            if (value() == minimum() && norm < -0.02) {
                emit joystickEdgeHold(+1, std::abs(norm));
            } else if (value() == maximum() && norm > 0.02) {
                emit joystickEdgeHold(-1, std::abs(norm));
            }
            return;
        }
        m_velocityAccumulator -= delta;
        const int newValue = std::clamp(value() + delta, minimum(), maximum());
        if (newValue == value()) {
            if (delta < 0 && value() == minimum()) {
                emit joystickEdgeHold(+1, std::abs(norm));
            } else if (delta > 0 && value() == maximum()) {
                emit joystickEdgeHold(-1, std::abs(norm));
            }
            return;
        }
        setValue(newValue);
    }

    void updateSliderRatio()
    {
        const int span = maximum() - minimum();
        if (span <= 0) {
            m_sliderRatio = 0.0;
        } else {
            m_sliderRatio =
                static_cast<double>(value() - minimum()) / static_cast<double>(span);
        }
        update();
    }

    qreal m_hoverProgress = 0.0;
    QVariantAnimation *m_hoverAnim;
    QTimer *m_throttleTimer;
    QTimer *m_visualTimer;
    bool m_dragActive = false;
    QPoint m_dragStartPos;
    double m_dragOffset = 0.0;
    double m_sliderRatio = 0.0;
    double m_velocityAccumulator = 0.0;
    double m_visualStrength = 0.0;
    double m_targetStrength = 0.0;
    double m_glowPhase = 0.0;
    double m_lastNorm = 0.0;
    int m_particleDirection = 0;
    QVector<EnergyParticle> m_particles;
};
#endif

static QIcon mirrorIconHorizontally(const QIcon &icon, const QSize &size);
#ifdef Q_OS_WIN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <windowsx.h>
#endif

// Simple append-only file logger to capture geometry/state traces when
// DebugView is not available. Helpful for reproducing issues on user's
// machine where reading OutputDebugString may be inconvenient.
static QString fusionLogFilePath()
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (base.isEmpty()) {
        base = QDir::homePath() + QLatin1String("/.fusion_terminal");
    }
    QDir().mkpath(base);
    const QString logDir = QDir(base).filePath(QStringLiteral("logs"));
    QDir().mkpath(logDir);
    return QDir(logDir).filePath(QStringLiteral("fusion_terminal.log"));
}

static void logToFile(const QString &msg)
{
    static const bool enabled = qEnvironmentVariableIntValue("FUSION_FILE_LOG") > 0;
    if (!enabled) {
        return;
    }
    const QString path = fusionLogFilePath();
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) return;
    QTextStream ts(&f);
    ts << QDateTime::currentDateTime().toString(Qt::ISODate) << " " << msg << "\n";
}

static void appendSpamLogToFile(const QString &msg)
{
    static const bool enabled = qEnvironmentVariableIntValue("FUSION_FILE_LOG") > 0;
    if (!enabled) {
        return;
    }
    const QString path = fusionLogFilePath();
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) return;
    QTextStream ts(&f);
    ts << QDateTime::currentDateTime().toString(Qt::ISODate) << " [spam] " << msg << "\n";
}

static bool isSpamConnectionLog(const QString &msg)
{
    const QString trimmed = msg.trimmed();
    if (trimmed.isEmpty()) {
        return true;
    }
    const QString lower = trimmed.toLower();
    if (trimmed.startsWith(QStringLiteral("[orders]"))) {
        return true;
    }
    if (trimmed.contains(QStringLiteral("WS: {"))) {
        return true;
    }
    if (lower.contains(QStringLiteral("order update"))) {
        return true;
    }
    return false;
}

static QString summarizeMarkers(const QVector<DomWidget::LocalOrderMarker> &markers)
{
    QString result = QStringLiteral("count=%1").arg(markers.size());
    const int limit = std::min<int>(3, markers.size());
    if (limit > 0) {
        QStringList samples;
        for (int i = 0; i < limit; ++i) {
            const auto &m = markers.at(i);
            samples << QStringLiteral("%1@%2%3")
                           .arg(m.quantity, 0, 'f', 4)
                           .arg(m.price, 0, 'f', 8)
                           .arg(m.side == OrderSide::Buy ? QLatin1String("B") : QLatin1String("S"));
        }
        result += QStringLiteral(" sample=[%1]").arg(samples.join(QStringLiteral(", ")));
    }
    return result;
}

static QString rectToString(const QRect &r)
{
    return QString("%1,%2 %3x%4").arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height());
}

static bool geometrySeemsOff(const QRect &geom, const QRect &work, const QSize &minSize)
{
    if (!geom.isValid()) {
        return true;
    }
    if (geom.width() < minSize.width() || geom.height() < minSize.height()) {
        return true;
    }
    if (!work.isValid()) {
        return false;
    }
    const QRect inter = work.intersected(geom);
    if (inter.isEmpty()) {
        return true;
    }
    if (inter.width() < geom.width() / 2 || inter.height() < geom.height() / 2) {
        return true;
    }
    return false;
}

static QScreen *screenByName(const QString &name)
{
    if (name.isEmpty()) {
        return nullptr;
    }
    const auto screens = QGuiApplication::screens();
    for (QScreen *scr : screens) {
        if (scr && scr->name() == name) {
            return scr;
        }
    }
    return nullptr;
}

MainWindow::MainWindow(const QString &backendPath,
                       const QString &symbol,
                       int levels,
                       QWidget *parent)
    : QMainWindow(parent)
    , m_startupMs(QDateTime::currentMSecsSinceEpoch())
    , m_backendPath(backendPath)
    , m_symbols(QStringList{symbol})
    , m_levels(levels)
    , m_workspaceTabs(nullptr)
    , m_tabUnderline(nullptr)
    , m_tabUnderlineAnim(nullptr)
    , m_tabUnderlineHiddenForDrag(false)
    , m_workspaceStack(nullptr)
    , m_addTabButton(nullptr)
    , m_addMenuButton(nullptr)
    , m_settingsSearchButton(nullptr)
    , m_settingsSearchContainer(nullptr)
    , m_settingsSearchAnim(nullptr)
    , m_settingsSearchExpanded(false)
    , m_connectionButton(nullptr)
    , m_timeLabel(nullptr)
    , m_statusLabel(nullptr)
    , m_pingLabel(nullptr)
    , m_orderPanel(nullptr)
    , m_orderSymbolLabel(nullptr)
    , m_orderPriceEdit(nullptr)
    , m_orderQuantityEdit(nullptr)
    , m_buyButton(nullptr)
    , m_sellButton(nullptr)
    , m_pluginsWindow(nullptr)
    , m_settingsWindow(nullptr)
    , m_connectionStore(new ConnectionStore(this))
    , m_tradeManager(new TradeManager(this))
    , m_connectionsWindow(nullptr)
    , m_tabs()
    , m_nextTabId(1)
    , m_recycledTabIds()
    , m_tabCloseIconNormal()
    , m_tabCloseIconHover()
    , m_lastAddAction(AddAction::WorkspaceTab)
    , m_draggingDomContainer(nullptr)
    , m_domDragStartGlobal()
    , m_domDragStartWindowOffset()
    , m_domDragActive(false)
    , m_timeTimer(nullptr)
        , m_topBar(nullptr)
        , m_minButton(nullptr)
    , m_maxButton(nullptr)
    , m_closeButton(nullptr)
{
    if (qEnvironmentVariableIntValue("FUSION_STALL_MONITOR") > 0) {
        new UiStallMonitor(this);
    }

    // Default splitter ratios (treated as weights and scaled by QSplitter).
    m_savedClustersPrintsSplitterSizes = {110, 220};
    m_savedDomPrintsSplitterSizes = {200, 600};

    {
        QString title = QStringLiteral("Fusion Terminal");
        const QString ver = QCoreApplication::applicationVersion().trimmed();
        if (!ver.isEmpty()) {
            title = QStringLiteral("%1 (%2)").arg(title, ver);
        }
        setWindowTitle(title);
    }
    resize(1920, 1080);
    setMinimumSize(800, 400);

    // Use frameless window but keep system menu and min/max buttons so
    // native snap & window controls behave as expected.
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint | Qt::WindowSystemMenuHint
                   | Qt::WindowMinMaxButtonsHint);
    // Use custom TitleBar instead of native OS strip.
    setWindowFlag(Qt::FramelessWindowHint);

    loadUserSettings();
    if (!m_authToken.isEmpty()) {
        requestInstalledMods();
    }
    if (m_tradeManager) {
        connect(m_tradeManager, &TradeManager::logMessage, this, &MainWindow::appendConnectionsLog);
    }
    connect(ThemeManager::instance(), &ThemeManager::themeChanged, this, [this]() {
        saveUserSettings();
    });
    if (m_volumeRules.isEmpty()) {
        m_volumeRules = defaultVolumeHighlightRules();
    }
    initializeDomFrameTimer();

    if (m_connectionStore) {
        connect(m_connectionStore,
                &ConnectionStore::credentialsChanged,
                this,
                [this](const QString &profileKey, const MexcCredentials &) {
                    refreshAccountColors();
                    // Proxy affects backend connectivity (orderbook_backend.exe) for Lighter in restricted regions.
                    // Push new proxy settings into active ladder clients and restart them.
                    if (profileKey.trimmed().compare(QStringLiteral("lighter"), Qt::CaseInsensitive) != 0) {
                        return;
                    }
                    for (auto &tab : m_tabs) {
                        for (auto &col : tab.columnsData) {
                            if (!col.client) {
                                continue;
                            }
                            if (symbolSourceForAccount(col.accountName) != SymbolSource::Lighter) {
                                continue;
                            }
                            restartColumnClient(col);
                        }
                    }
                });
    }
    refreshAccountColors();

    m_symbolLibrary = m_symbols;
    const QStringList defaults = {
        QStringLiteral("BTCUSDT"), QStringLiteral("ETHUSDT"), QStringLiteral("SOLUSDT"),
        QStringLiteral("BNBUSDT"), QStringLiteral("XRPUSDT"), QStringLiteral("LTCUSDT"),
        QStringLiteral("BIOUSDT")
    };
    for (const QString &sym : defaults) {
        if (!m_symbolLibrary.contains(sym, Qt::CaseInsensitive)) {
            m_symbolLibrary.push_back(sym);
        }
    }
    fetchSymbolLibrary();
    if (m_mexcFuturesSymbols.isEmpty()) {
        const QStringList futuresDefaults = {
            QStringLiteral("BTC_USDT"), QStringLiteral("ETH_USDT"), QStringLiteral("SOL_USDT"),
            QStringLiteral("BNB_USDT"), QStringLiteral("XRP_USDT"), QStringLiteral("DOGE_USDT"),
            QStringLiteral("LTC_USDT"), QStringLiteral("LINK_USDT"), QStringLiteral("ADA_USDT"),
            QStringLiteral("OP_USDT")
        };
        m_mexcFuturesSymbols = futuresDefaults;
    }
    fetchMexcFuturesSymbols();

    // ?????????? ???????? ?????? (Shift ? ?.?.).
    qApp->installEventFilter(this);

    buildUi();

    // Auto-check updates shortly after UI is ready.
    QTimer::singleShot(1500, this, &MainWindow::checkForUpdates);

    // Fallback: poll the SL/TP "hold" hotkey so it works even when key events
    // are swallowed by focus navigation (e.g. Tab) or other widgets.
    if (!m_sltpHoldPollTimer) {
        m_sltpHoldPollTimer = new QTimer(this);
        m_sltpHoldPollTimer->setInterval(16);
        connect(m_sltpHoldPollTimer, &QTimer::timeout, this, &MainWindow::pollSltpHoldKey);
        m_sltpHoldPollTimer->start();
    }

    // Ensure a window icon is set explicitly; on Windows, the running taskbar icon comes from WM_SETICON.
    QString appLogo = resolveAssetPath(QStringLiteral("logo/favicon-32x32.png"));
    if (appLogo.isEmpty()) {
        appLogo = resolveAssetPath(QStringLiteral("logo/favicon.ico"));
    }
    if (!appLogo.isEmpty()) {
        setWindowIcon(QIcon(appLogo));
    }

    // Sounds are initialized lazily on first use to avoid startup audio artifacts.

    if (m_tradeManager) {
        connect(m_tradeManager,
                &TradeManager::connectionStateChanged,
                this,
                &MainWindow::handleConnectionStateChanged);
        connect(m_tradeManager,
                &TradeManager::positionChanged,
                this,
                &MainWindow::handlePositionChanged);
        connect(m_tradeManager,
                &TradeManager::orderPlaced,
                this,
                [this](const QString &account,
                       const QString &sym,
                       OrderSide side,
                       double price,
                       double qty,
                       const QString &orderId) {
                    const QString symUpper = normalizedSymbolKey(sym);
                    const QString accountKey = normalizedAccountKey(account);
                    if (symUpper.isEmpty() || orderId.isEmpty()) {
                        return;
                    }
                    const QString lastKey = lastLimitOrderKey(symUpper, accountKey);
                    auto &stack = m_lastLimitOrderStackByKey[lastKey];
                    if (stack.isEmpty() || stack.back() != orderId) {
                        stack.push_back(orderId);
                    }
                    const QString timerKey = markerTimerKey(symUpper, accountKey, orderId);
                    auto *delayTimer = new QTimer(this);
                    delayTimer->setSingleShot(true);
                    connect(delayTimer,
                            &QTimer::timeout,
                            this,
                            [this, delayTimer, timerKey, account, symUpper, side, price, qty, orderId]() {
                                removeMarkerDelayTimer(timerKey, delayTimer);
                                addLocalOrderMarker(account,
                                                    symUpper,
                                                    side,
                                                    price,
                                                    qty,
                                                    orderId,
                                                    QDateTime::currentMSecsSinceEpoch());
                            });
                    m_markerDelayTimers.insert(timerKey, delayTimer);
                    delayTimer->start(kLocalMarkerDelayMs);
                    const QString msg = tr("Order placed: %1 %2 @ %3")
                                            .arg(side == OrderSide::Buy ? QStringLiteral("BUY")
                                                                        : QStringLiteral("SELL"))
                                            .arg(qty, 0, 'g', 6)
                                            .arg(price, 0, 'f', 5);
                    statusBar()->showMessage(msg, 3000);
                });
        connect(m_tradeManager,
                &TradeManager::orderFailed,
                this,
                [this](const QString &account, const QString &sym, const QString &message) {
                    const QString msg = tr("Order failed: %1").arg(message);
                    statusBar()->showMessage(msg, 4000);
                    addNotification(msg, true);
                    const QString lower = message.toLower();
                    if (lower.contains(QStringLiteral("too many requests"))
                        || lower.contains(QStringLiteral("ratelimit"))
                        || lower.contains(QStringLiteral("23000"))) {
                        const QString symUpper = normalizedSymbolKey(sym);
                        const QString accountLabel = normalizedAccountLabel(account);
                        const QString accountKey = normalizedAccountKey(accountLabel);
                        const QString lastKey = lastLimitOrderKey(symUpper, accountKey);
                        const QString retryId = m_lastCancelAttemptByKey.value(lastKey);
                        if (retryId.isEmpty()) {
                            return;
                        }
                        m_pendingLimitCancelByKey[lastKey].remove(retryId);
                        m_lastCancelAttemptByKey.remove(lastKey);
                    }
                });
        connect(m_tradeManager,
                &TradeManager::tradeExecuted,
                this,
                [this](const ExecutedTrade &trade) {
                    // Lighter UI reacts primarily to position polling; play sound on position-open
                    // transition there so it lines up with the position overlay.
                    if (symbolSourceForAccount(trade.accountName) == SymbolSource::Lighter) {
                        return;
                    }
                    ensureSoundsInitialized();
#if HAS_QSOUNDEFFECT
                    if (m_successEffect) {
                        restartSound(m_successEffect);
                        return;
                    }
#endif
#if HAS_QMEDIAPLAYER && HAS_QAUDIOOUTPUT
                    if (m_successPlayer && m_successOutput) {
                        restartSound(m_successPlayer);
                    }
#endif
                });
        connect(m_tradeManager,
                &TradeManager::orderCanceled,
                this,
                [this](const QString &account,
                       const QString &sym,
                       OrderSide side,
                       double price,
                       const QString &orderId) {
                    Q_UNUSED(side);
                    Q_UNUSED(price);
                    const QString symUpper = normalizedSymbolKey(sym);
                    const QString accountKey = normalizedAccountKey(account);
                    const QString timerKey = markerTimerKey(symUpper, accountKey, orderId);
                    removeMarkerDelayTimer(timerKey, nullptr);
                    removeLocalOrderMarker(account, sym, orderId);
                    const QString lastKey = lastLimitOrderKey(symUpper, accountKey);
                    auto lastIt = m_lastLimitOrderStackByKey.find(lastKey);
                    if (lastIt != m_lastLimitOrderStackByKey.end()) {
                        auto &stack = lastIt.value();
                        stack.erase(std::remove(stack.begin(), stack.end(), orderId), stack.end());
                        if (stack.isEmpty()) {
                            m_lastLimitOrderStackByKey.erase(lastIt);
                        }
                    }
                    auto pendingIt = m_pendingLimitCancelByKey.find(lastKey);
                    if (pendingIt != m_pendingLimitCancelByKey.end()) {
                        pendingIt.value().remove(orderId);
                        if (pendingIt.value().isEmpty()) {
                            m_pendingLimitCancelByKey.erase(pendingIt);
                        }
                    }
                    m_lastCancelAttemptByKey.remove(lastKey);
                });
        connect(m_tradeManager,
                &TradeManager::localOrdersUpdated,
                this,
                &MainWindow::handleLocalOrdersUpdated);
        connect(m_tradeManager,
                &TradeManager::privatePingUpdated,
                this,
                [this](const QString &accountName, int ms) {
                    const QString accountKey = normalizedAccountKey(accountName);
                    for (auto &tab : m_tabs) {
                        for (auto &col : tab.columnsData) {
                            if (!accountKey.isEmpty()
                                && normalizedAccountKey(col.accountName) != accountKey) {
                                continue;
                            }
                            col.lastOrderPingMs = ms;
                            updatePerfOverlay(col);
                        }
                    }
                });
        connect(m_tradeManager,
                &TradeManager::lighterStopOrdersUpdated,
                this,
                [this](const QString &accountName,
                       const QString &symbol,
                       bool hasSl,
                       double slTriggerPrice,
                       bool hasTp,
                       double tpTriggerPrice) {
                    const QString symUpper = normalizedSymbolKey(symbol);
                    if (symUpper.isEmpty()) {
                        return;
                    }
                    if (symbolSourceForAccount(accountName) != SymbolSource::Lighter) {
                        return;
                    }
                    const QString accountLower = accountName.trimmed().toLower();
                    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
                    bool touched = false;
                    for (auto &tab : m_tabs) {
                        for (auto &col : tab.columnsData) {
                            if (col.symbol.compare(symUpper, Qt::CaseInsensitive) != 0) {
                                continue;
                            }
                            if (!accountLower.isEmpty()
                                && col.accountName.trimmed().toLower() != accountLower) {
                                continue;
                            }
                            bool changed = false;
                            if (!hasSl) {
                                if (col.sltpHasSl) {
                                    col.sltpSlMissCount++;
                                    static constexpr qint64 kGraceMs = 30000;
                                    if (col.sltpSlMissCount >= 6
                                        && (col.sltpSlCreatedMs <= 0
                                            || nowMs - col.sltpSlCreatedMs >= kGraceMs)) {
                                        col.sltpHasSl = false;
                                        col.sltpSlPrice = 0.0;
                                        col.sltpSlCreatedMs = 0;
                                        col.sltpSlMissCount = 0;
                                        changed = true;
                                    }
                                }
                            } else {
                                if (!col.sltpHasSl) {
                                    col.sltpHasSl = true;
                                    changed = true;
                                }
                                if (col.sltpSlCreatedMs != nowMs) {
                                    col.sltpSlCreatedMs = nowMs; // last-seen timestamp to avoid flapping clears
                                    changed = true;
                                }
                                if (slTriggerPrice > 0.0 && std::abs(col.sltpSlPrice - slTriggerPrice) > 1e-12) {
                                    col.sltpSlPrice = slTriggerPrice;
                                    changed = true;
                                }
                                if (col.sltpSlMissCount != 0) {
                                    col.sltpSlMissCount = 0;
                                    changed = true;
                                }
                            }

                             if (!hasTp) {
                                 if (col.sltpHasTp) {
                                     col.sltpTpMissCount++;
                                     static constexpr qint64 kGraceMs = 30000;
                                     if (col.sltpTpMissCount >= 6
                                         && (col.sltpTpCreatedMs <= 0
                                             || nowMs - col.sltpTpCreatedMs >= kGraceMs)) {
                                         col.sltpHasTp = false;
                                         col.sltpTpPrice = 0.0;
                                         col.sltpTpCreatedMs = 0;
                                         col.sltpTpMissCount = 0;
                                         changed = true;
                                     }
                                 }
                             } else {
                                 if (!col.sltpHasTp) {
                                     col.sltpHasTp = true;
                                     changed = true;
                                 }
                                 if (col.sltpTpCreatedMs != nowMs) {
                                     col.sltpTpCreatedMs = nowMs; // last-seen timestamp to avoid flapping clears
                                     changed = true;
                                 }
                                 if (tpTriggerPrice > 0.0 && std::abs(col.sltpTpPrice - tpTriggerPrice) > 1e-12) {
                                     col.sltpTpPrice = tpTriggerPrice;
                                     changed = true;
                                 }
                                 if (col.sltpTpMissCount != 0) {
                                     col.sltpTpMissCount = 0;
                                     changed = true;
                                 }
                             }

                             // Reconcile optimistic local placement: if server confirms only one type shortly after
                             // the user placed a stop, clear any opposite "ghost" marker immediately (no long grace).
                             static constexpr qint64 kReconcileWindowMs = 20000;
                             if (col.sltpLastPlaceMs > 0 && nowMs - col.sltpLastPlaceMs < kReconcileWindowMs) {
                                 const qint64 kFudgeMs = 1200;
                                 if (hasSl && !hasTp) {
                                     if (col.sltpHasTp && col.sltpTpCreatedMs > 0
                                         && col.sltpTpCreatedMs + kFudgeMs >= col.sltpLastPlaceMs) {
                                         col.sltpHasTp = false;
                                         col.sltpTpPrice = 0.0;
                                         col.sltpTpCreatedMs = 0;
                                         col.sltpTpMissCount = 0;
                                         changed = true;
                                     }
                                 } else if (hasTp && !hasSl) {
                                     if (col.sltpHasSl && col.sltpSlCreatedMs > 0
                                         && col.sltpSlCreatedMs + kFudgeMs >= col.sltpLastPlaceMs) {
                                         col.sltpHasSl = false;
                                         col.sltpSlPrice = 0.0;
                                         col.sltpSlCreatedMs = 0;
                                         col.sltpSlMissCount = 0;
                                         changed = true;
                                     }
                                 }
                             }
                             touched = touched || changed;
                         }
                     }
                    if (touched) {
                        refreshColumnsForSymbol(symUpper);
                    }
                });
    }

    createInitialWorkspace();

    if (m_connectionStore && m_tradeManager) {
        auto initProfile = [&](ConnectionStore::Profile profile) {
            MexcCredentials creds = m_connectionStore->loadMexcCredentials(profile);
            m_tradeManager->setCredentials(profile, creds);
            const bool hasKey = !creds.apiKey.isEmpty();
            const bool hasSecret = !creds.secretKey.isEmpty();
            const bool uzxProfile = (profile == ConnectionStore::Profile::UzxSpot
                                     || profile == ConnectionStore::Profile::UzxSwap);
            const bool isLighter = (profile == ConnectionStore::Profile::Lighter);
            bool canAuto = false;
            if (isLighter) {
                const QString baseDir =
                    m_connectionStore ? m_connectionStore->storagePath()
                                      : QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
                const QString vaultPath = baseDir + QDir::separator() + QStringLiteral("lighter_vault.dat");
                const bool hasVault = QFile::exists(vaultPath);
                canAuto = creds.autoConnect && (hasVault || (hasKey && !creds.baseUrl.trimmed().isEmpty()));
            } else {
                canAuto = creds.autoConnect && hasKey && hasSecret;
            }
            if (uzxProfile) {
                canAuto = canAuto && !creds.passphrase.isEmpty();
            }
            if (canAuto) {
                QTimer::singleShot(0, this, [this, profile]() {
                    if (m_tradeManager) {
                        m_tradeManager->connectToExchange(profile);
                    }
                });
            }
        };
        initProfile(ConnectionStore::Profile::MexcSpot);
        initProfile(ConnectionStore::Profile::MexcFutures);
        initProfile(ConnectionStore::Profile::UzxSwap);
        initProfile(ConnectionStore::Profile::UzxSpot);
        initProfile(ConnectionStore::Profile::Lighter);
        handleConnectionStateChanged(ConnectionStore::Profile::MexcSpot,
                                     m_tradeManager->overallState(),
                                     QString());
    } else {
        handleConnectionStateChanged(ConnectionStore::Profile::MexcSpot,
                                     TradeManager::ConnectionState::Disconnected,
                                     QString());
    }
}

MainWindow::~MainWindow() = default;

void MainWindow::buildUi()
{
    auto *central = new QWidget(this);
    setCentralWidget(central);

    auto *rootLayout = new QVBoxLayout(central);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    QWidget *topBar = buildTopBar(central);
    QWidget *mainArea = buildMainArea(central);

    rootLayout->addWidget(topBar);
    rootLayout->addWidget(mainArea, 1);

    // Status bar removed (ping not needed)
    statusBar()->hide();

    // Time label update timer (UTC clock + offset).
    m_timeTimer = new QTimer(this);
    m_timeTimer->setInterval(1000);
    connect(m_timeTimer, &QTimer::timeout, this, &MainWindow::updateTimeLabel);
    m_timeTimer->start();
    updateTimeLabel();

    if (!m_perfFpsTimer.isValid()) {
        m_perfFpsTimer.start();
    }

    // Window buttons style + TitleBar bottom border, ?????? ? VSCode.
    // ????? ?????? ???? ??????? ? ????????? ????? ?? ?????? ? ??????? ??????.
    setStyleSheet(
        // Base dark surface colors.
        // MainWindow has a stylesheet, so without an explicit background some widgets can become transparent
        // and "leak" the Windows light theme (white navbars/panels/dialogs).
        "QMainWindow, QDialog {"
        "  background-color: #181818;"
        "  color: #e4e4e4;"
        "}"
        "QWidget {"
        "  color: #e4e4e4;"
        "}"
        "QLineEdit, QListWidget, QTableView, QTreeView {"
        "  background-color: #1a1a1a;"
        "  color: #e4e4e4;"
        "  selection-background-color: #2860ff;"
        "  selection-color: #ffffff;"
        "}"
        "QHeaderView::section {"
        "  background-color: #252526;"
        "  color: #e4e4e4;"
        "}"
        "QFrame#TitleBar {"
        "  background-color: #252526;"
        "  border-bottom: none;" /* ??????? ?????? ??????? ? TitleBar */
        "}"
        "QLabel#PerfOverlayLine {"
        "  color: #9aa2b1;"
        "  font-size: 10px;"
        "}"
        "QFrame#SideToolbar {"
        "  background-color: transparent;"
        "  border-right: 1px solid #444444;"
        "}"
        "QTabBar::tab:first:selected {"
        "  border-left: none;"
        "}"
        "QFrame#MainAreaFrame {"
        "  border-top: 1px solid #444444;"
        "}"
        "QToolButton#WindowButtonMin, QToolButton#WindowButtonMax, QToolButton#WindowButtonClose {"
        "  background: transparent;"
        "  border: none;"
        "  color: #ffffff;"
        "  padding: 0px;"
        "}"
        "QToolButton#WindowButtonMin:hover, QToolButton#WindowButtonMax:hover {"
        "  background-color: #2a2a2a;"
        "}"
        "QToolButton#WindowButtonMin:pressed, QToolButton#WindowButtonMax:pressed {"
        "  background-color: #1f1f1f;"
        "}"
        "QToolButton#WindowButtonClose:hover {"
        "  background-color: #e81123;"
        "  color: #ffffff;"
        "}"
        "QToolButton#WindowButtonClose:pressed {"
        "  background-color: #c50f1f;"
        "  color: #ffffff;"
        "}"
        // ??????? hover-????????? ? ??????? ?????? ? ?????? ???? ?????? ????? ??? ?????????.
                "QToolButton#SideNavButton {"
        "  background: transparent;"
        "  border: none;"
        "  padding: 0px;"
        "  margin: 0px;"
        "}"
        // ????? ??? ??????? ? ????? VSCode
        "QFrame#TabsContainer {"
        "  background-color: #252526;"          /* ??? ?? ???, ??? ? ? TitleBar */
        "  border-bottom: none;"   /* ??????? ?????? ??????? ? TabsContainer */
        "}"
        "QTabBar {"
        "  border: none;" /* ??????? ??? ??????? ? ?????? QTabBar */
        "  background-color: #252526;" /* ????????????? ??? QTabBar ????? ??, ??? ? TitleBar */
        "}"
        "QTabBar::tab {"
        "  background-color: #252526;" /* ??? ?????????? ???????, ??? ? ?????-???? */
        "  color: #cccccc;" /* ???? ?????? ?????????? ??????? */
        "  padding: 0px 12px;" /* ???????, ????? ??????? ???????? ??? ?????? */
        "  border: none;" /* ??????? ??? ??????? ? ??????? */
        "  margin-left: 0px;" /* ??????? ?????? ????? ????????? */
        "  height: 100%;" /* ??????? ?? ??? ?????? ?????-???? */
        "}"
                "QTabBar::tab:selected {"
        "  background-color: #1e1e1e;"
        "  color: #ffffff;"
        "  border-top: none;"
        "  border-bottom: none;"
        "  border-left: 1px solid #444444;"
        "  border-right: 1px solid #444444;"
        "}"
        "QTabBar::tab:first:selected {"
        "  border-left: none;"
        "}"
        "QTabBar::tab:!selected:hover {"
        "  background-color: #2d2d2d;" /* ??? ??? ????????? ?? ?????????? ??????? */
        "}"
        "QTabBar::close-button {"
        "  border: none;"
        "  background: transparent;"
        "  margin: 2px;"
        "  padding: 0;"
        "}"
        "QTabBar::close-button:hover {"
        "  background: #555555;"
        "}"
        ""
        "QFrame#DomColumnFrame {"
        "  background-color: #1e1e1e;"
        "  border: 1px solid #444444;"
        "  border-radius: 0px;"
        "}"
        "QFrame#DomColumnFrame[active=\"true\"] {"
        "  border: 2px solid #007acc;"
        "}"
        "QScrollBar:vertical {"
        "  background: transparent;"
        "  width: 6px;"
        "  margin: 0px;"
        "}"
        "QScrollBar::handle:vertical {"
        "  background: #555555;"
        "  border-radius: 3px;"
        "  min-height: 24px;"
        "}"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {"
        "  height: 0px;"
        "  subcontrol-origin: margin;"
        "}"
        "QScrollBar::sub-page:vertical, QScrollBar::add-page:vertical {"
        "  background: transparent;"
        "}"
        );

    // ?????????????? ?????: ??????? ??????????? ????? QTabBar::pane,
    // ????? ?? ???? ?????? ??????? ?????? ??? ?????????.
    const QString extraTabPaneStyle = QStringLiteral(
        "QFrame#TabsContainer {"
        "  background-color: #252526;"
        "  border: none;"
        "}"
        "QTabBar {"
        "  border: none;"
        "  background: transparent;"
        "  qproperty-drawBase: 0;"
        "}"
        "QTabBar::pane {"
        "  border: none;"
        "  margin: 0px;"
        "  padding: 0px;"
        "}"
        "QTabBar::tab {"
        "  background: #252526;"
        "  color: #cccccc;"
        "  padding: 3px 10px;"
        "  margin: 0px;"
        "  border-left: 1px solid #444444;"
        "  border-right: none;"
        "  border-radius: 0px;"
        "}"
        "QTabBar::tab:last {"
        "  border-right: 1px solid #444444;"
        "}"
        "QTabBar::tab:only-one {"
        "  border-right: 1px solid #444444;"
        "}"
                "QTabBar::tab:selected {"
        "  background-color: #1e1e1e;"
        "  color: #ffffff;"
        "}"
        "QTabBar::tab:!selected:hover {"
        "  background: #2a2a2a;"
        "}"
        "QTabBar::close-button, QTabBar::close-button:hover {"
        "  background: transparent;"
        "  border: none;"
        "  margin: 0px;"
        "  padding: 0px;"
        "}");

    setStyleSheet(styleSheet() + extraTabPaneStyle);

    // ??????? ??????????? ?????????? ?????????.
    const QString vscodeTabsStyle = QStringLiteral(
        "QTabBar {"
        "  border: none;"
        "  background-color: #252526;"
        "  qproperty-drawBase: 0;"
        "  margin: 0px;"
        "  padding: 0px;"
        "}"
        "QTabBar::tab {"
        "  background-color: #252526;"
        "  color: #cccccc;"
        "  padding: 0px 12px;"
        "  border-left: 1px solid #444444;"
        "  border-right: none;"
        "  border-radius: 0px;"
        "  margin-left: 0px;"
        "  margin-right: 0px;"
        "  height: 100%;"
        "}"
        "QTabBar::tab:last {"
        "  border-right: 1px solid #444444;"
        "}"
        "QTabBar::tab:only-one {"
        "  border-right: 1px solid #444444;"
        "}"
        "QTabBar::tab:selected {"
        "  background-color: #1e1e1e;"
        "  color: #ffffff;"
        "}"
        "QTabBar::tab:!selected:hover {"
        "  background-color: #2d2d2d;"
        "}"
        "QTabBar::close-button, QTabBar::close-button:hover {"
        "  background: transparent;"
        "  border: none;"
        "  margin: 0px;"
        "  padding: 0px;"
        "}");

    setStyleSheet(styleSheet() + vscodeTabsStyle);

    }

    QWidget *MainWindow::buildTopBar(QWidget *parent)
{
    auto *top = new QFrame(parent);
    top->setObjectName(QStringLiteral("TitleBar"));
    m_topBar = top;
    auto *mainTopLayout = new QHBoxLayout(top);
    mainTopLayout->setContentsMargins(0, 0, 0, 0);
    mainTopLayout->setSpacing(0);
    // ????????????? ????????? ?????? ?????????,
    // ????? ?? ???? ?????? ???????? ?????? ??????.
    top->setFixedHeight(32);

    // top->setStyleSheet("border-bottom: 1px solid #444444;"); /* ????????? ?????? ??????? ??? ???? ?????????? */

    // ????? ?????? ??? ???????? (????????? ? ???????? ????)
    auto *logoContainer = new QFrame(top);
    logoContainer->setObjectName(QStringLiteral("SideToolbar"));
    logoContainer->setFixedWidth(42); // ?? ?? ??????, ??? ? ? SideToolbar
    auto *logoLayout = new QHBoxLayout(logoContainer);
    logoLayout->setContentsMargins(0, 0, 0, 0);
    logoLayout->setSpacing(0);
    logoLayout->setAlignment(Qt::AlignCenter); // ?????????? ??????? ??????????? ? ?????????????

    auto *logoLabel = new QLabel(logoContainer);
    logoLabel->setFixedSize(28, 28);
    const QString logoPath = resolveAssetPath(QStringLiteral("logo/favicon-32x32.png"));
    if (!logoPath.isEmpty()) {
        QPixmap pix(logoPath);
        logoLabel->setPixmap(pix.scaled(28, 28, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    } else {
        logoLabel->setText(tr("Fusion Terminal"));
    }
    logoLayout->addWidget(logoLabel);
    mainTopLayout->addWidget(logoContainer);

    // ??????? ?????? ??? ???????
    // ??????? ?????? ??? ???????
    auto *tabsContainer = new QFrame(top);
    tabsContainer->setObjectName(QStringLiteral("TabsContainer")); // << ???????? ???
    auto *tabsLayout = new QHBoxLayout(tabsContainer);
    tabsLayout->setContentsMargins(0, 0, 0, 0);
    tabsLayout->setSpacing(0);


    m_workspaceTabs = new QTabBar(tabsContainer);
    m_workspaceTabs->setExpanding(false);
    m_workspaceTabs->setTabsClosable(true);
    m_workspaceTabs->setMovable(true);
    m_workspaceTabs->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_workspaceTabs->setFocusPolicy(Qt::NoFocus);
    m_workspaceTabs->setStyleSheet(
        "QTabBar { background-color: #252526; } "
        "QTabBar::tab { "
        "  color: #ffffff; min-width: 80px; padding: 4px 12px; height: 28px; margin: 0px; "
        "  border-left: 1px solid #444444; border-right: none; alignment: center; } "
        "QTabBar::tab:last { border-right: 1px solid #444444; } "
        "QTabBar::tab:only-one { border-right: 1px solid #444444; } "
        "QTabBar::tab:selected { "
        "  background-color: #1e1e1e; color: #ffffff; "
        "  border-left: 1px solid #444444; } ");
    QObject::connect(m_workspaceTabs, &QTabBar::tabMoved, this, [this](int from, int to) {
        Q_UNUSED(from);
        Q_UNUSED(to);
        if (m_tabUnderlineAnim) {
            m_tabUnderlineAnim->stop();
        }
        if (m_tabUnderline) {
            m_tabUnderline->hide();
            if (QApplication::mouseButtons() & Qt::LeftButton) {
                m_tabUnderlineHiddenForDrag = true;
            } else {
                QTimer::singleShot(0, this, [this]() {
                    if (m_workspaceTabs) {
                        updateTabUnderline(m_workspaceTabs->currentIndex());
                    }
                });
            }
        }
        QTimer::singleShot(0, this, [this]() { refreshTabCloseButtons(); });
    });
    connect(m_workspaceTabs, &QTabBar::currentChanged, this, &MainWindow::handleTabChanged);
    connect(m_workspaceTabs,
            &QTabBar::tabCloseRequested,
            this,
            &MainWindow::handleTabCloseRequested);
    tabsLayout->addWidget(m_workspaceTabs);

    m_addTabButton = new QToolButton(tabsContainer);
    m_addTabButton->setAutoRaise(true);
    const QSize addIconSize(16, 16);
    m_addTabButton->setIcon(loadIconTinted(QStringLiteral("plus"), QColor("#cfcfcf"), addIconSize));
    m_addTabButton->setIconSize(addIconSize);
    m_addTabButton->setCursor(Qt::PointingHandCursor);
    m_addTabButton->setFixedSize(28, 28);
    connect(m_addTabButton, &QToolButton::clicked, this, [this]() {
        triggerAddAction(m_lastAddAction);
    });

    m_addMenuButton = new QToolButton(tabsContainer);
    m_addMenuButton->setAutoRaise(true);
    const QSize chevronSize(10, 10);
    m_addMenuButton->setIcon(loadIconTinted(QStringLiteral("chevron-down"), QColor("#cfcfcf"), chevronSize));
    m_addMenuButton->setIconSize(chevronSize);
    m_addMenuButton->setFixedSize(24, 28);
    m_addMenuButton->setPopupMode(QToolButton::InstantPopup);
    m_addMenuButton->setCursor(Qt::PointingHandCursor);

    auto *menu = new QMenu(m_addMenuButton);
    QAction *newTabAction = menu->addAction(tr("New workspace tab"));
    QAction *newLadderAction = menu->addAction(tr("Add ladder column"));
    connect(newTabAction, &QAction::triggered, this, [this]() { triggerAddAction(AddAction::WorkspaceTab); });
    connect(newLadderAction, &QAction::triggered, this, [this]() { triggerAddAction(AddAction::LadderColumn); });
    m_addMenuButton->setMenu(menu);

    tabsLayout->addWidget(m_addTabButton, 0, Qt::AlignVCenter);
    tabsLayout->addWidget(m_addMenuButton, 0, Qt::AlignVCenter);
    updateAddButtonsToolTip();
    mainTopLayout->addWidget(tabsContainer, 1);

    auto *right = new QHBoxLayout();
    right->setSpacing(8);

    m_settingsSearchButton = new QToolButton(top);
    m_settingsSearchButton->setObjectName(QStringLiteral("SettingsSearchButton"));
    m_settingsSearchButton->setAutoRaise(true);
    m_settingsSearchButton->setCursor(Qt::PointingHandCursor);
    m_settingsSearchButton->setCheckable(true);
    const QSize searchIconSize(16, 16);
    m_settingsSearchButton->setIcon(loadIconTinted(QStringLiteral("settings-search"), QColor("#cfcfcf"), searchIconSize));
    m_settingsSearchButton->setIconSize(searchIconSize);
    m_settingsSearchButton->setFixedSize(28, 28);
    m_settingsSearchButton->setStyleSheet(
        "QToolButton#SettingsSearchButton {"
        "  border: none;"
        "  border-radius: 8px;"
        "  padding: 4px;"
        "}"
        "QToolButton#SettingsSearchButton:hover {"
        "  background-color: #333333;"
        "}"
        "QToolButton#SettingsSearchButton:checked {"
        "  background-color: #3c3c3c;"
        "}"
    );

    m_settingsSearchContainer = new QFrame(top);
    m_settingsSearchContainer->setObjectName(QStringLiteral("SettingsSearchContainer"));
    auto *searchLayout = new QHBoxLayout(m_settingsSearchContainer);
    searchLayout->setContentsMargins(0, 0, 0, 0);
    searchLayout->setSpacing(0);

    m_settingsSearchEdit = new QLineEdit(m_settingsSearchContainer);
    m_settingsSearchEdit->setPlaceholderText(tr("Поиск настроек..."));
    m_settingsSearchEdit->setClearButtonEnabled(true);
    m_settingsSearchEdit->setMinimumWidth(260);
    m_settingsSearchEdit->setMaxLength(128);
    m_settingsSearchEdit->setObjectName(QStringLiteral("SettingsSearchEdit"));
    m_settingsSearchEdit->setStyleSheet(
        "QLineEdit#SettingsSearchEdit {"
        "  background-color: #252526;"
        "  color: #f0f0f0;"
        "  border-radius: 10px;"
        "  border: 1px solid #3c3c3c;"
        "  padding: 2px 10px;"
        "}"
        "QLineEdit#SettingsSearchEdit:focus {"
        "  border: 1px solid #007acc;"
        "}");
    connect(m_settingsSearchEdit, &QLineEdit::returnPressed, this, &MainWindow::handleSettingsSearch);
    connect(m_settingsSearchEdit, &QLineEdit::editingFinished, this, [this]() {
        if (m_settingsSearchExpanded && m_settingsSearchEdit && m_settingsSearchEdit->text().trimmed().isEmpty()) {
            toggleSettingsSearchDrawer(false, false);
        }
    });
    m_settingsSearchEdit->installEventFilter(this);

    searchLayout->addWidget(m_settingsSearchEdit);
    m_settingsSearchContainer->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_settingsSearchContainer->setMaximumWidth(0);
    m_settingsSearchContainer->setVisible(false);

    connect(m_settingsSearchButton, &QToolButton::clicked, this, [this]() {
        toggleSettingsSearchDrawer(!m_settingsSearchExpanded, true);
    });

    right->addWidget(m_settingsSearchButton);
    right->addWidget(m_settingsSearchContainer);

    // ????????? ?????? ? ???? Settings.
    m_settingEntries.append(
        {QStringLiteral("centerHotkey"),
         tr("Центрировать по спреду"),
         {QStringLiteral("горячие"), QStringLiteral("клавиши"), QStringLiteral("center"), QStringLiteral("spread")}});
    m_settingEntries.append(
        {QStringLiteral("volumeHighlight"),
         tr("Подсветка объема DOM"),
         {QStringLiteral("подсветка"), QStringLiteral("объем"), QStringLiteral("volume"), QStringLiteral("highlight")}});
    m_settingEntries.append(
        {QStringLiteral("domFrameRate"),
         tr("Частота обновления DOM"),
         {QStringLiteral("fps"), QStringLiteral("частота"), QStringLiteral("обновление"), QStringLiteral("dom")}});
    QStringList completionNames;
    for (const auto &entry : m_settingEntries) {
        completionNames << entry.name;
    }
    m_settingsCompleter = new QCompleter(completionNames, this);
    m_settingsCompleter->setCaseSensitivity(Qt::CaseInsensitive);
    m_settingsCompleter->setFilterMode(Qt::MatchContains);
    m_settingsCompleter->setCompletionMode(QCompleter::PopupCompletion);
    connect(m_settingsCompleter,
            QOverload<const QString &>::of(&QCompleter::activated),
            this,
            &MainWindow::handleSettingsSearchFromCompleter);
    m_settingsSearchEdit->setCompleter(m_settingsCompleter);

    m_timeLabel = new QLabel(top);
    m_timeLabel->setCursor(Qt::PointingHandCursor);
    right->addWidget(m_timeLabel);

    auto makeWinButton = [top](const QString &text, const char *objectName) {
        auto *btn = new QToolButton(top);
        btn->setText(text);
        btn->setObjectName(QLatin1String(objectName));
        btn->setAutoRaise(true);
        btn->setFixedSize(42, 32);
        return btn;
    };

    // Helper to fix window geometry if it ends up off-screen or clipped.
    auto fixWindowGeometry = [this]() {
        QRect geom = geometry();
        QScreen *scr = QGuiApplication::screenAt(geom.center());
        if (!scr) scr = QGuiApplication::primaryScreen();
        if (!scr) return;
        const QRect work = scr->availableGeometry();

        const QRect inter = work.intersected(geom);
        // If intersection is too small or window is outside work area, recenter/resize
        if (inter.width() < geom.width() / 2 || inter.height() < geom.height() / 2 || !work.contains(geom)) {
            int w = std::min(geom.width(), work.width());
            int h = std::min(geom.height(), work.height());
            int x = work.x() + (work.width() - w) / 2;
            int y = work.y() + (work.height() - h) / 2;
            qDebug() << "[MainWindow] correcting geometry to" << QRect(x, y, w, h) << " work=" << work << " geom=" << geom;
            setGeometry(x, y, w, h);
            raise();
            activateWindow();
        }
    };

    auto *minButton = makeWinButton(QString(), "WindowButtonMin");
    auto *maxButton = makeWinButton(QString(), "WindowButtonMax");
    auto *closeButton = makeWinButton(QString(), "WindowButtonClose");

    const QSize winIconSize(16, 16);
    const QSize maxIconSize(14, 14);
    const QColor winIconColor("#ffffff");
    minButton->setIcon(loadIconTinted(QStringLiteral("minus"), winIconColor, winIconSize));
    minButton->setIconSize(winIconSize);
    maxButton->setIcon(loadIconTinted(QStringLiteral("square"), winIconColor, maxIconSize));
    maxButton->setIconSize(maxIconSize);
    closeButton->setIcon(loadIconTinted(QStringLiteral("x"), winIconColor, winIconSize));
    closeButton->setIconSize(winIconSize);

    // store pointers to buttons so we can update icons on state changes
    m_minButton = minButton;
    m_maxButton = maxButton;
    m_closeButton = closeButton;

    QObject::connect(minButton, &QToolButton::clicked, this, [this]() {
        qDebug() << "[MainWindow] Min button clicked. windowState=" << windowState();
        logToFile(QStringLiteral("Min button clicked. state=%1 geometry=%2").arg(QString::number((int)windowState())).arg(rectToString(geometry())));
        showMinimized();
    });

    QObject::connect(maxButton, &QToolButton::clicked, this, [this]() {
        const bool maximized = isMaximized();
        qDebug() << "[MainWindow] Max button clicked. before isMaximized=" << maximized
                 << " geometry=" << geometry() << " windowState=" << windowState();
        const QString s =
            QStringLiteral("[MainWindow] Max clicked before: isMaximized=%1")
                .arg(maximized ? QStringLiteral("1") : QStringLiteral("0"));
        {
            const std::wstring ws = s.toStdWString();
            OutputDebugStringW(ws.c_str());
        }

        if (!maximized) {
            showMaximized();
        } else {
            showNormal();
        }

        updateMaximizeIcon();
        QTimer::singleShot(100, this, [this]() {
            qDebug() << "[MainWindow] geometry after action:" << geometry() << " state=" << windowState();
            logToFile(QStringLiteral("Post-max action geometry: %1 state=%2")
                          .arg(rectToString(geometry()))
                          .arg((int)windowState()));
        });
    });
    QObject::connect(closeButton, &QToolButton::clicked, this, &QWidget::close);

    // set initial maximize/restore icon
    updateMaximizeIcon();

    right->addWidget(minButton);
    right->addWidget(maxButton);
    right->addWidget(closeButton);

    mainTopLayout->addLayout(right);

    // Caption area for dragging/maximizing.
    top->installEventFilter(this);
    m_workspaceTabs->installEventFilter(this);
    m_addTabButton->installEventFilter(this);
    m_timeLabel->installEventFilter(this);

    return top;
}

void MainWindow::updatePerfOverlay(DomColumn &col)
{
    if (!col.perfOverlay) {
        return;
    }

    auto setText = [](QLabel *label, const QString &text) {
        if (!label) {
            return;
        }
        if (label->text() != text) {
            label->setText(text);
        }
    };

    const QString pingText =
        (col.lastPingMs >= 0) ? QStringLiteral("Ping: %1 ms").arg(col.lastPingMs)
                              : QStringLiteral("Ping: -");
    const QString ordersText =
        (col.lastOrderPingMs >= 0) ? QStringLiteral("Orders: %1 ms").arg(col.lastOrderPingMs)
                                   : QStringLiteral("Orders: -");
    const QString fpsText =
        (m_lastUiFps > 0.0) ? QStringLiteral("FPS: %1").arg(m_lastUiFps, 0, 'f', 1)
                            : QStringLiteral("FPS: -");
    const QString domText =
        (col.lastFpsHz > 0.0) ? QStringLiteral("DOM Hz: %1").arg(col.lastFpsHz, 0, 'f', 1)
                              : QStringLiteral("DOM Hz: -");

    setText(col.perfPingLabel, pingText);
    setText(col.perfOrderPingLabel, ordersText);
    setText(col.perfFpsLabel, fpsText);
    setText(col.perfDomHzLabel, domText);
}

void MainWindow::updateAllPerfOverlays()
{
    for (auto &tab : m_tabs) {
        for (auto &col : tab.columnsData) {
            updatePerfOverlay(col);
        }
    }
}

void MainWindow::toggleSettingsSearchDrawer(bool open, bool focusEdit)
{
    if (!m_settingsSearchContainer || !m_settingsSearchEdit || !m_settingsSearchButton) {
        return;
    }

    if (m_settingsSearchAnim) {
        m_settingsSearchAnim->stop();
        m_settingsSearchAnim->deleteLater();
    }

    const int expandedWidth = 260;
    const int startW = m_settingsSearchContainer->maximumWidth();
    const int endW = open ? expandedWidth : 0;

    m_settingsSearchExpanded = open;
    m_settingsSearchButton->setChecked(open);

    if (open) {
        m_settingsSearchContainer->setVisible(true);
        m_settingsSearchEdit->setEnabled(true);
    }

    m_settingsSearchAnim = new QPropertyAnimation(m_settingsSearchContainer, "maximumWidth", this);
    m_settingsSearchAnim->setDuration(open ? 180 : 140);
    m_settingsSearchAnim->setEasingCurve(open ? QEasingCurve::OutCubic : QEasingCurve::InCubic);
    m_settingsSearchAnim->setStartValue(startW);
    m_settingsSearchAnim->setEndValue(endW);
    connect(m_settingsSearchAnim, &QPropertyAnimation::finished, this, [this, open, focusEdit]() {
        if (!m_settingsSearchContainer || !m_settingsSearchEdit || !m_settingsSearchButton) {
            return;
        }
        if (!open) {
            m_settingsSearchContainer->setVisible(false);
            m_settingsSearchEdit->clearFocus();
        } else if (focusEdit) {
            m_settingsSearchEdit->setFocus(Qt::ShortcutFocusReason);
            m_settingsSearchEdit->selectAll();
        }
        if (m_settingsSearchAnim) {
            m_settingsSearchAnim->deleteLater();
            m_settingsSearchAnim = nullptr;
        }
    });
    m_settingsSearchAnim->start();
}
void MainWindow::keyPressEvent(QKeyEvent *event)
{
    const int key = event->key();
    const Qt::KeyboardModifiers mods = event->modifiers();
    QWidget *fw = QApplication::focusWidget();
    if (fw && (qobject_cast<QLineEdit *>(fw)
               || qobject_cast<QPlainTextEdit *>(fw)
               || qobject_cast<QTextEdit *>(fw)
               || qobject_cast<QAbstractSpinBox *>(fw))) {
        QMainWindow::keyPressEvent(event);
        return;
    }

    if (m_notionalEditActive) {
        QMainWindow::keyPressEvent(event);
        return;
    }

    if (matchesHotkey(key, mods, m_settingsSearchKey, m_settingsSearchMods)) {
        toggleSettingsSearchDrawer(!m_settingsSearchExpanded, true);
        event->accept();
        return;
    }

    if (matchesHotkey(key, mods, m_newTabKey, m_newTabMods)) {
        handleNewTabRequested();
        event->accept();
        return;
    }
    if (matchesHotkey(key, mods, m_addLadderKey, m_addLadderMods)) {
        handleNewLadderRequested();
        event->accept();
        return;
    }
    if (matchesHotkey(key, mods, m_refreshLadderKey, m_refreshLadderMods)) {
        refreshActiveLadder();
        event->accept();
        return;
    }
    if (mods == Qt::KeypadModifier && (key == Qt::Key_Plus || key == Qt::Key_Minus)) {
        if (DomColumn *col = focusedDomColumn()) {
            const int cur = std::max(1, col->tickCompression);
            int next = cur;
            if (key == Qt::Key_Plus) {
                next = (cur < 10) ? 10 : (cur + 10);
            } else {
                next = (cur <= 10) ? 1 : (cur - 10);
            }
            applyCompressionForColumn(col->container, next);
            event->accept();
            return;
        }
    }
    auto maybeHandleCancelLastLimit = [&](QKeyEvent *ke, bool fromEventFilter) -> bool {
        const int evKey = ke->key();
        const Qt::KeyboardModifiers evMods = ke->modifiers();
        bool cancelMatch = matchesHotkey(evKey, evMods, m_cancelLastLimitKey, m_cancelLastLimitMods);
        if (!cancelMatch && m_cancelLastLimitMods == Qt::NoModifier && evMods == Qt::NoModifier
            && m_cancelLastLimitKey == Qt::Key_G) {
            const QString text = ke->text().toLower();
            if (text == QStringLiteral("g") || text == QStringLiteral("\u043F")) {
                cancelMatch = true;
            }
#if defined(Q_OS_WIN)
            if (!cancelMatch && ke->nativeVirtualKey() == 0x47) {
                cancelMatch = true;
            }
#endif
        }
        logToFile(QStringLiteral("[hotkey] cancelLastLimit key=%1 mods=%2 text='%3' nativeVK=%4 match=%5 filter=%6")
                      .arg(evKey)
                      .arg(int(evMods))
                      .arg(ke->text())
                      .arg(ke->nativeVirtualKey())
                      .arg(cancelMatch ? 1 : 0)
                      .arg(fromEventFilter ? 1 : 0));
        if (!cancelMatch) {
            return false;
        }
        if (m_tradeManager) {
            if (DomColumn *col = focusedDomColumn()) {
                const QString symUpper = normalizedSymbolKey(col->symbol);
                const QString accountLabel = normalizedAccountLabel(col->accountName);
                const QString accountKey = normalizedAccountKey(accountLabel);
                const QString lastKey = lastLimitOrderKey(symUpper, accountKey);
                auto lastIt = m_lastLimitOrderStackByKey.find(lastKey);
                if (lastIt != m_lastLimitOrderStackByKey.end()) {
                    auto &stack = lastIt.value();
                    const MarkerBucket *bucketPtr = nullptr;
                    auto symIt = m_markerBuckets.find(symUpper);
                    if (symIt != m_markerBuckets.end()) {
                        auto accIt = symIt.value().find(accountKey);
                        if (accIt != symIt.value().end()) {
                            bucketPtr = &accIt.value();
                        }
                    }
                    auto isLive = [&](const QString &id) -> bool {
                        if (!bucketPtr) {
                            return false;
                        }
                        return bucketPtr->pending.contains(id) || bucketPtr->confirmed.contains(id);
                    };

                    QString orderId;
                    bool found = false;
                    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
                    for (int idx = stack.size() - 1; idx >= 0; --idx) {
                        const QString candidate = stack.at(idx);
                        if (candidate.isEmpty() || !isLive(candidate)) {
                            stack.removeAt(idx);
                            continue;
                        }
                        orderId = candidate;
                        found = true;
                        break;
                    }

                    if (found) {
                        DomWidget::LocalOrderMarker marker;
                        bool haveMarker = false;
                        if (bucketPtr) {
                            if (bucketPtr->pending.contains(orderId)) {
                                marker = bucketPtr->pending.value(orderId);
                                haveMarker = true;
                            } else if (bucketPtr->confirmed.contains(orderId)) {
                                marker = bucketPtr->confirmed.value(orderId);
                                haveMarker = true;
                            }
                        }
                        m_tradeManager->cancelOrder(col->symbol, accountLabel, orderId);
                        removeMarkerDelayTimer(markerTimerKey(symUpper, accountKey, orderId), nullptr);
                        m_pendingLimitCancelByKey[lastKey].insert(orderId, nowMs);
                        m_lastCancelAttemptByKey.insert(lastKey, orderId);
                        if (haveMarker) {
                            const QString sideName =
                                marker.side == OrderSide::Buy ? QStringLiteral("BUY") : QStringLiteral("SELL");
                            statusBar()->showMessage(
                                tr("Cancel requested: %1 @ %2")
                                    .arg(sideName, QString::number(marker.price, 'f', 6)),
                                1800);
                        } else {
                            statusBar()->showMessage(
                                tr("Cancel requested (%1)").arg(orderId),
                                1800);
                        }
                    } else {
                        QString fallbackId;
                        DomWidget::LocalOrderMarker fallbackMarker;
                        bool haveFallback = false;
                        if (bucketPtr) {
                            auto consider = [&](const DomWidget::LocalOrderMarker &m) {
                                if (m.orderId.isEmpty()) {
                                    return;
                                }
                                if (haveFallback && m.createdMs <= fallbackMarker.createdMs) {
                                    return;
                                }
                                fallbackMarker = m;
                                fallbackId = m.orderId;
                                haveFallback = true;
                            };
                            for (auto it = bucketPtr->pending.begin(); it != bucketPtr->pending.end(); ++it) {
                                consider(it.value());
                            }
                            for (auto it = bucketPtr->confirmed.begin(); it != bucketPtr->confirmed.end(); ++it) {
                                consider(it.value());
                            }
                        }
                        if (haveFallback) {
                            if (stack.isEmpty()) {
                                stack.push_back(fallbackId);
                            }
                            m_tradeManager->cancelOrder(col->symbol, accountLabel, fallbackId);
                            removeMarkerDelayTimer(markerTimerKey(symUpper, accountKey, fallbackId), nullptr);
                            m_pendingLimitCancelByKey[lastKey].insert(fallbackId, nowMs);
                            m_lastCancelAttemptByKey.insert(lastKey, fallbackId);
                            const QString sideName =
                                fallbackMarker.side == OrderSide::Buy ? QStringLiteral("BUY") : QStringLiteral("SELL");
                            statusBar()->showMessage(
                                tr("Cancel requested: %1 @ %2")
                                    .arg(sideName, QString::number(fallbackMarker.price, 'f', 6)),
                                1800);
                        } else {
                            if (stack.isEmpty()) {
                                m_lastLimitOrderStackByKey.erase(lastIt);
                                statusBar()->showMessage(tr("No recent limit order to cancel"), 1800);
                            } else {
                                statusBar()->showMessage(tr("No recent limit order to cancel"), 1800);
                            }
                        }
                    }
                } else {
                    statusBar()->showMessage(tr("No recent limit order to cancel"), 1800);
                }
            }
        }
        ke->accept();
        return true;
    };

    bool cancelMatch = matchesHotkey(key, mods, m_cancelLastLimitKey, m_cancelLastLimitMods);
    if (!cancelMatch && m_cancelLastLimitMods == Qt::NoModifier && mods == Qt::NoModifier
        && m_cancelLastLimitKey == Qt::Key_G) {
        const QString text = event->text().toLower();
        if (text == QStringLiteral("g") || text == QStringLiteral("\u043F")) {
            cancelMatch = true;
        }
#if defined(Q_OS_WIN)
        if (!cancelMatch && event->nativeVirtualKey() == 0x47) {
            cancelMatch = true;
        }
#endif
    }
    if (cancelMatch) {
        if (maybeHandleCancelLastLimit(event, false)) {
            return;
        }
    }
    if (matchesHotkey(key, mods, m_volumeAdjustKey, m_volumeAdjustMods)) {
        m_capsAdjustMode = true;
        event->accept();
        return;
    }
    if (handleSltpKeyPress(event)) {
        return;
    }
    for (int i = 0; i < static_cast<int>(m_notionalPresetKeys.size()); ++i)
    {
        if (matchesHotkey(key, mods, m_notionalPresetKeys[i], m_notionalPresetMods[i]))
        {
            applyNotionalPreset(i);
            event->accept();
            return;
        }
    }

    bool match = false;
    if (key == Qt::Key_Space && mods == Qt::NoModifier) {
        if (m_tradeManager) {
            DomColumn *col = focusedDomColumn();
            if (col) {
                markPendingCancelForSymbol(col->symbol);
                clearColumnLocalMarkers(*col);
                m_tradeManager->cancelAllOrders(col->symbol, col->accountName);
            }
        }
        event->accept();
        return;
    }
    if (key == m_centerKey) {
        if (m_centerMods == Qt::NoModifier) {
            match = true;
        } else {
            Qt::KeyboardModifiers cleaned = mods & ~Qt::KeypadModifier;
            match = (cleaned == m_centerMods);
        }
    }

    if (match) {
        centerActiveLaddersToSpread();
        event->accept();
        return;
    }

    QMainWindow::keyPressEvent(event);
}

void MainWindow::keyReleaseEvent(QKeyEvent *event)
{
    if (matchesHotkey(event->key(), event->modifiers(), m_volumeAdjustKey, m_volumeAdjustMods)) {
        m_capsAdjustMode = false;
    }
    handleSltpKeyRelease(event);
    QMainWindow::keyReleaseEvent(event);
}

void MainWindow::changeEvent(QEvent *event)
{
    auto scheduleRestoreRefresh = [this]() {
        if (m_restoreRefreshQueued) {
            return;
        }
        m_restoreRefreshQueued = true;
        QTimer::singleShot(0, this, [this]() {
            m_restoreRefreshQueued = false;
            if (isMinimized()) {
                return;
            }
            QWidget *central = centralWidget();
            if (central) {
                central->setUpdatesEnabled(false);
                central->setVisible(false);
            }
            QTimer::singleShot(0, this, [this, central]() {
                if (central) {
                    central->setVisible(true);
                    central->setUpdatesEnabled(true);
                    central->update();
                }
                const auto quicks = findChildren<QQuickWidget *>();
                for (QQuickWidget *qw : quicks) {
                    if (qw) {
                        qw->setUpdatesEnabled(true);
                        qw->update();
                    }
                }
                update();
            });
        });
    };

    if (event && event->type() == QEvent::WindowStateChange) {
        const Qt::WindowStates cur = windowState();
        const bool wasMinimized = m_prevWindowState.testFlag(Qt::WindowMinimized);
        updateMaximizeIcon();
        m_prevWindowState = cur;
        if (cur.testFlag(Qt::WindowMinimized)) {
            m_restoreOnShow = true;
        }
        if (wasMinimized && !cur.testFlag(Qt::WindowMinimized)) {
            if (!m_restoreRefreshQueued) {
                QRect geom = frameGeometry();
                QScreen *scr = nullptr;
                if (windowHandle()) {
                    scr = windowHandle()->screen();
                }
                if (!scr) {
                    scr = screenByName(m_lastNormalScreenName);
                }
                if (!scr) {
                    scr = QGuiApplication::screenAt(geom.center());
                }
                if (!scr) {
                    scr = QGuiApplication::primaryScreen();
                }
                const QRect work = scr ? scr->availableGeometry() : QRect();
                if (geometrySeemsOff(geom, work, minimumSize())) {
                    if (m_haveLastNormalGeometry) {
                        setGeometry(m_lastNormalGeometry);
                    } else if (scr) {
                        const int w = std::min(geom.width(), work.width());
                        const int h = std::min(geom.height(), work.height());
                        const int x = work.x() + (work.width() - w) / 2;
                        const int y = work.y() + (work.height() - h) / 2;
                        setGeometry(x, y, w, h);
                    }
                }
                scheduleRestoreRefresh();
            }
        }
    } else if (event && (event->type() == QEvent::Show || event->type() == QEvent::WindowActivate)) {
        if (m_restoreOnShow && !isMinimized()) {
            m_restoreOnShow = false;
            scheduleRestoreRefresh();
        }
    }
    QMainWindow::changeEvent(event);
}

void MainWindow::moveEvent(QMoveEvent *event)
{
    if (!isMinimized() && !isMaximized()) {
        m_lastNormalGeometry = geometry();
        m_haveLastNormalGeometry = m_lastNormalGeometry.isValid();
        if (windowHandle() && windowHandle()->screen()) {
            m_lastNormalScreenName = windowHandle()->screen()->name();
        }
    }
    QMainWindow::moveEvent(event);
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
    if (!isMinimized() && !isMaximized()) {
        m_lastNormalGeometry = geometry();
        m_haveLastNormalGeometry = m_lastNormalGeometry.isValid();
        if (windowHandle() && windowHandle()->screen()) {
            m_lastNormalScreenName = windowHandle()->screen()->name();
        }
    }
    QMainWindow::resizeEvent(event);
}

void MainWindow::updateMaximizeIcon()
{
    if (!m_maxButton) return;
    const bool maximized = windowState().testFlag(Qt::WindowMaximized);
    const QSize maxIconSize(14, 14);
    if (maximized) {
        QIcon icon = loadIconTinted(QStringLiteral("squares"), QColor("#ffffff"), maxIconSize);
        m_maxButton->setIcon(mirrorIconHorizontally(icon, maxIconSize));
    } else {
        m_maxButton->setIcon(loadIconTinted(QStringLiteral("square"), QColor("#ffffff"), maxIconSize));
    }
}

#ifdef Q_OS_WIN
bool MainWindow::nativeEvent(const QByteArray &eventType, void *message, qintptr *result)
{
    Q_UNUSED(eventType);
    MSG *msg = static_cast<MSG *>(message);
    if (!msg) return QMainWindow::nativeEvent(eventType, message, result);

    if (msg->message == WM_NCCALCSIZE) {
        // Remove standard title bar and non-client area ? we draw our own TitleBar.
        // For maximized/snapped windows Windows may leave space for the standard
        // non-client frame. When that happens we must explicitly set the client
        // rect to the monitor work area so there are no stray gaps on the right/bottom
        // (classic frameless-window vs Aero Snap issue).
        if (msg->wParam == TRUE && msg->lParam) {
            NCCALCSIZE_PARAMS* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(msg->lParam);
            RECT &r = params->rgrc[0];

            HWND hwnd = msg->hwnd;
            if (hwnd && IsZoomed(hwnd)) {
                // Window is maximized (this also covers Aero Snap to top):
                // align client rect to monitor work area (respects taskbar).
                HMONITOR hMonitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
                MONITORINFO mi;
                mi.cbSize = sizeof(mi);
                if (GetMonitorInfoW(hMonitor, &mi)) {
                    // Use rcWork to avoid covering the taskbar
                    r = mi.rcWork;
                    logToFile(QStringLiteral("WM_NCCALCSIZE applied rcWork: %1").arg(rectToString(QRect(r.left, r.top, r.right - r.left, r.bottom - r.top))));
                }
            }

            *result = 0;
            return true;
        }
        *result = 0;
        return true;
    }

    if (msg->message == WM_NCHITTEST) {
        HWND hwndHit = msg->hwnd;
        const LONG x = GET_X_LPARAM(msg->lParam);
        const LONG y = GET_Y_LPARAM(msg->lParam);
        const QPoint globalPt(x, y);

        const QRect g = frameGeometry();
        const QPoint localPt = mapFromGlobal(globalPt);

        const int w = g.width();
        const int h = g.height();
        const int bw = m_resizeBorderWidth;

        auto hitTestTopBar = [this, &globalPt]() -> int {
            if (!m_topBar) {
                return -1;
            }
            const QPoint localInTop = m_topBar->mapFromGlobal(globalPt);
            if (!m_topBar->rect().contains(localInTop)) {
                return -1;
            }
            QWidget *child = m_topBar->childAt(localInTop);
            bool interactive = false;

            if (!child) {
                interactive = false;
            } else if (child == m_minButton || child == m_maxButton || child == m_closeButton
                       || child == m_addTabButton || child == m_timeLabel) {
                interactive = true;
            } else if (child == m_workspaceTabs) {
                QPoint inTabs = m_workspaceTabs->mapFrom(m_topBar, localInTop);
                int tabIndex = m_workspaceTabs->tabAt(inTabs);
                interactive = (tabIndex != -1);
            } else {
                interactive = true;
            }

            return interactive ? HTCLIENT : HTCAPTION;
        };

        const bool isZoomed = hwndHit && IsZoomed(hwndHit);
        if (isZoomed) {
            const int topHit = hitTestTopBar();
            if (topHit == HTCAPTION) {
                *result = HTCAPTION;
                return true;
            }
            *result = HTCLIENT;
            return true;
        }

        // Corners first
        if (localPt.x() < bw && localPt.y() < bw) { *result = HTTOPLEFT; return true; }
        if (localPt.x() >= w - bw && localPt.y() < bw) { *result = HTTOPRIGHT; return true; }
        if (localPt.x() < bw && localPt.y() >= h - bw) { *result = HTBOTTOMLEFT; return true; }
        if (localPt.x() >= w - bw && localPt.y() >= h - bw) { *result = HTBOTTOMRIGHT; return true; }

        // Edges
        if (localPt.x() < bw) { *result = HTLEFT; return true; }
        if (localPt.x() >= w - bw) { *result = HTRIGHT; return true; }
        if (localPt.y() < bw) { *result = HTTOP; return true; }
        if (localPt.y() >= h - bw) { *result = HTBOTTOM; return true; }

        // Title bar: empty area => caption drag, interactive widgets => client.
        const int topHit = hitTestTopBar();
        if (topHit == HTCAPTION) {
            *result = HTCAPTION;
            return true;
        }

        *result = HTCLIENT;
        return true;
    }
if (msg->message == WM_GETMINMAXINFO) {
        // Ensure maximized size/position match the monitor work area so
        // Windows maximization (and Aero Snap) doesn't leave gaps or shift
        // the window. Classic fix for frameless windows.
        if (msg->lParam) {
            MINMAXINFO *mmi = reinterpret_cast<MINMAXINFO *>(msg->lParam);
            HMONITOR hMonitor = MonitorFromWindow(msg->hwnd, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi;
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(hMonitor, &mi)) {
                const RECT &rcWork = mi.rcWork;
                const RECT &rcMonitor = mi.rcMonitor;

                mmi->ptMaxPosition.x = rcWork.left - rcMonitor.left;
                mmi->ptMaxPosition.y = rcWork.top - rcMonitor.top;
                mmi->ptMaxSize.x = rcWork.right - rcWork.left;
                mmi->ptMaxSize.y = rcWork.bottom - rcWork.top;
                mmi->ptMaxTrackSize = mmi->ptMaxSize;
                logToFile(QStringLiteral("WM_GETMINMAXINFO set ptMaxSize=%1 pos=%2").arg(QString::number(mmi->ptMaxSize.x) + 'x' + QString::number(mmi->ptMaxSize.y), QString::number(mmi->ptMaxPosition.x) + "," + QString::number(mmi->ptMaxPosition.y)));
            }
            *result = 0;
            return true;
        }
    }

    return QMainWindow::nativeEvent(eventType, message, result);
}
#endif

QWidget *MainWindow::buildMainArea(QWidget *parent)
{
    auto *main = new QFrame(parent);
    main->setObjectName(QStringLiteral("MainAreaFrame"));
    auto *layout = new QHBoxLayout(main);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto *sidebar = new QFrame(main);
    sidebar->setObjectName(QStringLiteral("SideToolbar"));
    // ???? ????, ??? ? VSCode (????????? ?? 2px ?? ???????)
        sidebar->setFixedWidth(42);
    auto *sideLayout = new QVBoxLayout(sidebar);
    sideLayout->setContentsMargins(0, 12, 0, 0);
    sideLayout->setSpacing(12);

    const QSize navIconSize(28, 28);
    const QColor navIconColor("#c0c0c0");

    auto makeSideButton = [this, sidebar, navIconSize, navIconColor](const QString &iconName,
                                                                     const QString &tooltip) {
        auto *btn = new QToolButton(sidebar);
        btn->setObjectName(QStringLiteral("SideNavButton"));
        btn->setAutoRaise(true);
        btn->setFixedSize(42, 32);
        // Default icon: light gray
        btn->setIcon(loadIconTinted(iconName, navIconColor, navIconSize));
        btn->setIconSize(navIconSize);
        btn->setToolTip(tooltip);
        // Make cursor indicate clickable and allow handling hover in eventFilter
        btn->setCursor(Qt::PointingHandCursor);
        btn->setProperty("iconName", iconName);
        btn->installEventFilter(this);
        return btn;
    };

    // ??????????? / ??????
    {
        m_connectionButton = makeSideButton(QStringLiteral("plug-connected"), tr("Connection"));
        sideLayout->addWidget(m_connectionButton, 0, Qt::AlignHCenter);
        connect(m_connectionButton, &QToolButton::clicked, this, &MainWindow::openConnectionsWindow);
    }

    // ?????????? ????????? / PnL
    {
        QToolButton *b = makeSideButton(QStringLiteral("report-money"), tr("P&L / Results"));
        sideLayout->addWidget(b, 0, Qt::AlignHCenter);
        connect(b, &QToolButton::clicked, this, &MainWindow::openFinrezWindow);
    }

    // ?????? / ??????
    {
        QToolButton *b = makeSideButton(QStringLiteral("arrows-exchange"), tr("Trades"));
        sideLayout->addWidget(b, 0, Qt::AlignHCenter);
        connect(b, &QToolButton::clicked, this, &MainWindow::openTradesWindow);
    }

    // ???? (?????? cube-plus)
    auto *modsButton = makeSideButton(QStringLiteral("cube-plus"), tr("Mods"));
    sideLayout->addWidget(modsButton, 0, Qt::AlignHCenter);
    connect(modsButton, &QToolButton::clicked, this, &MainWindow::openPluginsWindow);

    // ??????
    {
        m_alertsButton = makeSideButton(QStringLiteral("bell"), tr("Alerts"));
        sideLayout->addWidget(m_alertsButton, 0, Qt::AlignHCenter);
        connect(m_alertsButton, &QToolButton::clicked, this, &MainWindow::toggleAlertsPanel);

        m_alertsBadge = new QLabel(m_alertsButton);
        m_alertsBadge->setObjectName(QStringLiteral("AlertsBadge"));
        m_alertsBadge->setAlignment(Qt::AlignCenter);
        m_alertsBadge->setMinimumWidth(16);
        m_alertsBadge->setFixedHeight(16);
        m_alertsBadge->setStyleSheet(
            "QLabel#AlertsBadge {"
            "  background-color: #2e8bdc;"
            "  color: #ffffff;"
            "  font-weight: 700;"
            "  border-radius: 8px;"
            "  border: 1px solid #0f1f30;"
            "  font-size: 9px;"
            "  padding: 0 3px;"
            "}");
        m_alertsBadge->setAttribute(Qt::WA_TransparentForMouseEvents);
        m_alertsBadge->hide();
        updateAlertsBadge();
    }

    // ?????? / ??????????
    {
        QToolButton *b = makeSideButton(QStringLiteral("alarm"), tr("Timer"));
        sideLayout->addWidget(b, 0, Qt::AlignHCenter);
    }

    sideLayout->addStretch(1);

    // Updates (above profile)
    m_updateButton = makeSideButton(QStringLiteral("cloud-question"), tr("Updates"));
    sideLayout->addWidget(m_updateButton, 0, Qt::AlignHCenter | Qt::AlignBottom);
    connect(m_updateButton, &QToolButton::clicked, this, &MainWindow::handleUpdateButtonClicked);
    setUpdateState(UpdateState::NotChecked);

    // Auth / account (above settings)
    m_authNav = makeSideButton(QStringLiteral("user-circle"), tr("Sign in"));
    sideLayout->addWidget(m_authNav, 0, Qt::AlignHCenter | Qt::AlignBottom);
    m_authBadge = new QLabel(m_authNav);
    m_authBadge->setObjectName(QStringLiteral("AuthBadge"));
    m_authBadge->setFixedSize(8, 8);
    m_authBadge->setStyleSheet(
        "QLabel#AuthBadge {"
        "  background-color: #35c46a;"
        "  border-radius: 4px;"
        "  border: 1px solid #0f1f30;"
        "}"
    );
    m_authBadge->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_authBadge->hide();
    m_authMenu = new QMenu(m_authNav);
    QAction *signOut = m_authMenu->addAction(tr("Sign out"));
    connect(signOut, &QAction::triggered, this, [this]() {
        m_authToken.clear();
        m_authRole.clear();
        m_authUser.clear();
        m_installedMods.clear();
        saveUserSettings();
        updateAuthNavUi();
        updateChatButtons();
        if (statusBar()) {
            statusBar()->showMessage(tr("Signed out."), 2500);
        }
    });
    connect(m_authNav, &QToolButton::clicked, this, [this]() {
        if (m_authToken.isEmpty()) {
            openAuthWindow();
            return;
        }
        if (m_authMenu) {
            m_authMenu->exec(m_authNav->mapToGlobal(QPoint(0, m_authNav->height())));
        }
    });
    updateAuthNavUi();

    // ????????? ? ????? ???? (????????? ?????)
    auto *settingsNav = makeSideButton(QStringLiteral("settings"), tr("Settings"));
    sideLayout->addWidget(settingsNav, 0, Qt::AlignHCenter | Qt::AlignBottom);
    connect(settingsNav, &QToolButton::clicked, this, &MainWindow::openSettingsWindow);

    QString buildVersion = QCoreApplication::applicationVersion().trimmed();
    if (buildVersion.startsWith(QLatin1Char('v'), Qt::CaseInsensitive)) {
        buildVersion = buildVersion.mid(1);
    }
    if (buildVersion.isEmpty()) {
        buildVersion = QStringLiteral("0.0.0");
    }
    auto *buildLabel = new QLabel(buildVersion, sidebar);
    buildLabel->setObjectName(QStringLiteral("SideBuildLabel"));
    buildLabel->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    buildLabel->setStyleSheet(
        "QLabel#SideBuildLabel {"
        "  color: #9aa0a6;"
        "  font-family: \"JetBrains Mono\";"
        "  font-size: 9px;"
        "  font-weight: 600;"
        "  padding: 0;"
        "}"
    );
    sideLayout->addSpacing(0);
    sideLayout->addWidget(buildLabel, 0, Qt::AlignHCenter | Qt::AlignBottom);

    layout->addWidget(sidebar);

    m_workspaceStack = new QStackedWidget(main);
    layout->addWidget(m_workspaceStack, 1);
    m_workspaceStack->installEventFilter(this);

    // Alerts panel overlay (hidden by default)
    m_alertsPanel = new QFrame(m_workspaceStack);
    m_alertsPanel->setObjectName(QStringLiteral("AlertsPanel"));
    m_alertsPanel->setVisible(false);
    m_alertsPanel->setStyleSheet(
        "QFrame#AlertsPanel {"
        "  background-color: #2b2b2b;"
        "  border: 1px solid #3c3c3c;"
        "  border-radius: 8px;"
        "}"
        "QListWidget#AlertsList {"
        "  background: transparent;"
        "  border: none;"
        "  outline: none;"
        "  color: #e0e0e0;"
        "}"
        "QPushButton#AlertsMarkReadButton {"
        "  background-color: #3a3a3a;"
        "  color: #e0e0e0;"
        "  padding: 4px 8px;"
        "  border: 1px solid #4a4a4a;"
        "  border-radius: 4px;"
        "}"
        "QPushButton#AlertsMarkReadButton:hover {"
        "  background-color: #444444;"
        "}"
        "QToolButton#AlertsCloseButton {"
        "  border: none;"
        "  background: transparent;"
        "  border-radius: 4px;"
        "  padding: 2px;"
        "}"
        "QToolButton#AlertsCloseButton:hover {"
        "  background-color: #3a3a3a;"
        "}");

    auto *alertsLayout = new QVBoxLayout(m_alertsPanel);
    alertsLayout->setContentsMargins(10, 8, 10, 10);
    alertsLayout->setSpacing(6);

    auto *alertsHeader = new QHBoxLayout();
    alertsHeader->setContentsMargins(0, 0, 0, 0);
    alertsHeader->setSpacing(6);
    auto *alertsTitle = new QLabel(tr("Notifications"), m_alertsPanel);
    alertsTitle->setStyleSheet("color: #ffffff; font-weight: 600;");
    auto *markRead = new QPushButton(tr("Mark all read"), m_alertsPanel);
    markRead->setObjectName(QStringLiteral("AlertsMarkReadButton"));
    auto *closeAlerts = new QToolButton(m_alertsPanel);
    closeAlerts->setObjectName(QStringLiteral("AlertsCloseButton"));
    closeAlerts->setAutoRaise(true);
    closeAlerts->setCursor(Qt::PointingHandCursor);
    closeAlerts->setFixedSize(22, 22);
    closeAlerts->setIcon(loadIconTinted(QStringLiteral("x"), QColor("#e0e0e0"), QSize(12, 12)));
    closeAlerts->setIconSize(QSize(12, 12));
    alertsHeader->addWidget(alertsTitle);
    alertsHeader->addStretch(1);
    alertsHeader->addWidget(markRead);
    alertsHeader->addWidget(closeAlerts);
    alertsLayout->addLayout(alertsHeader);

    m_alertsList = new QListWidget(m_alertsPanel);
    m_alertsList->setObjectName(QStringLiteral("AlertsList"));
    m_alertsList->setSelectionMode(QAbstractItemView::NoSelection);
    m_alertsList->setFocusPolicy(Qt::NoFocus);
    m_alertsList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_alertsList->setTextElideMode(Qt::ElideNone);
    m_alertsList->setWordWrap(true);
    alertsLayout->addWidget(m_alertsList, 1);

    connect(markRead, &QPushButton::clicked, this, &MainWindow::markAllNotificationsRead);
    connect(closeAlerts, &QToolButton::clicked, this, &MainWindow::toggleAlertsPanel);

    repositionAlertsPanel();

    return main;
}

void MainWindow::createInitialWorkspace()
{
    if (!m_savedLayout.isEmpty()) {
        for (const auto &cols : m_savedLayout) {
            createWorkspaceTab(cols);
        }
    } else {
        QVector<SavedColumn> cols;
        cols.reserve(m_symbols.size());
        for (const QString &s : m_symbols) {
            const QString trimmed = s.trimmed();
            if (trimmed.isEmpty()) {
                continue;
            }
            SavedColumn sc;
            sc.symbol = trimmed;
            sc.compression = 1;
            sc.account = QStringLiteral("MEXC Spot");
            cols.push_back(sc);
        }
        createWorkspaceTab(cols);
    }
}

void MainWindow::updateTabUnderline(int index)
{
    if (!m_workspaceTabs || index < 0) {
        return;
    }

    if (!m_tabUnderline) {
        m_tabUnderline = new QFrame(m_workspaceTabs);
        m_tabUnderline->setObjectName(QStringLiteral("TabUnderline"));
        m_tabUnderline->setStyleSheet(QStringLiteral("background-color: #007acc; border: none;"));
        m_tabUnderline->setFixedHeight(2);
    }
    if (!m_tabUnderlineAnim) {
        m_tabUnderlineAnim = new QPropertyAnimation(m_tabUnderline, "geometry", this);
        m_tabUnderlineAnim->setDuration(350);

        m_tabUnderlineAnim->setEasingCurve(QEasingCurve::InOutCubic);
    }

    const QRect tabRect = m_workspaceTabs->tabRect(index);
    if (!tabRect.isValid()) {
        return;
    }

    QRect startRect(tabRect.left(), 0, 0, m_tabUnderline->height());
    QRect endRect(tabRect.left(), 0, tabRect.width(), m_tabUnderline->height());

    m_tabUnderline->setGeometry(startRect);
    m_tabUnderline->show();
    m_tabUnderline->raise();

    m_tabUnderlineAnim->stop();
    m_tabUnderlineAnim->setStartValue(startRect);
    m_tabUnderlineAnim->setEndValue(endRect);
    m_tabUnderlineAnim->start();
}

MainWindow::WorkspaceTab MainWindow::createWorkspaceTab(const QVector<SavedColumn> &columnsSpec)
{
    const int savedIndex = m_tabs.size();
    int tabId = 0;
    if (!m_recycledTabIds.isEmpty()) {
        std::sort(m_recycledTabIds.begin(), m_recycledTabIds.end());
        tabId = m_recycledTabIds.takeFirst();
    } else {
        tabId = m_nextTabId++;
    }

    auto *workspace = new QFrame(m_workspaceStack);
    auto *wsLayout = new QVBoxLayout(workspace);
    wsLayout->setContentsMargins(0, 0, 0, 0);
    wsLayout->setSpacing(0);

    auto *columnsContainer = new QFrame(workspace);
    auto *columnsRow = new QHBoxLayout(columnsContainer);
    columnsRow->setContentsMargins(0, 0, 0, 0);
    columnsRow->setSpacing(0);

    auto *columnsSplitter = new AnimatedSplitter(Qt::Horizontal, columnsContainer);
    columnsSplitter->setObjectName(QStringLiteral("WorkspaceColumnsSplitter"));
    columnsSplitter->setChildrenCollapsible(false);
    columnsSplitter->setHandleWidth(2);
    columnsSplitter->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    columnsRow->addWidget(columnsSplitter, 1);

    WorkspaceTab tab;
    tab.id = tabId;
    tab.workspace = workspace;
    tab.columns = columnsSplitter;

    QVector<SavedColumn> specs = columnsSpec;
    if (specs.isEmpty()) {
        specs.reserve(m_symbols.size());
        for (const QString &s : m_symbols) {
            const QString trimmed = s.trimmed();
            if (trimmed.isEmpty()) {
                continue;
            }
            SavedColumn sc;
            sc.symbol = trimmed;
            sc.compression = 1;
            sc.account = QStringLiteral("MEXC Spot");
            specs.push_back(sc);
        }
    }

    QVector<QPointer<QWidget>> createdContainers;
    createdContainers.reserve(specs.size());

    for (const auto &spec : specs) {
        const QString symbol = spec.symbol.trimmed().toUpper();
        const QString account = spec.account.isEmpty() ? QStringLiteral("MEXC Spot") : spec.account;
        DomColumn col = createDomColumn(symbol, account, tab);

        const auto src = symbolSourceForAccount(account);
        const bool lighterPerp = (src == SymbolSource::Lighter) && !symbol.contains(QLatin1Char('/'));
        if ((src == SymbolSource::MexcFutures || lighterPerp) && spec.leverage > 0) {
            m_futuresLeverageBySymbol.insert(symbol, spec.leverage);
        }

        applySymbolToColumn(col, symbol, account);
        if (col.tickerLabel) {
            col.tickerLabel->setProperty("accountColor", col.accountColor);
            applyTickerLabelStyle(col.tickerLabel, col.accountColor, false);
        }
        applyHeaderAccent(col);

        col.tickCompression = std::max(1, spec.compression);
        if (col.compressionButton) {
            col.compressionButton->setText(QStringLiteral("%1x").arg(col.tickCompression));
        }
        if (col.client) {
            col.client->setCompression(col.tickCompression);
            restartColumnClient(col);
        }
        tab.columnsData.push_back(col);
        columnsSplitter->addWidget(col.container);
        columnsSplitter->setStretchFactor(columnsSplitter->indexOf(col.container), 0);
        createdContainers.push_back(col.container);
    }

    auto *splitterSpacer = new QWidget(columnsSplitter);
    splitterSpacer->setObjectName(QStringLiteral("DomSplitterSpacer"));
    splitterSpacer->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    splitterSpacer->setFocusPolicy(Qt::NoFocus);
    splitterSpacer->setMinimumWidth(1);
    splitterSpacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    splitterSpacer->setStyleSheet(QStringLiteral("background: transparent;"));
    columnsSplitter->addWidget(splitterSpacer);
    columnsSplitter->setStretchFactor(columnsSplitter->indexOf(splitterSpacer), 1);
    tab.columnsSpacer = splitterSpacer;

    if (savedIndex >= 0 && savedIndex < m_savedWorkspaceColumnSizes.size()) {
        const QList<int> savedSizes = m_savedWorkspaceColumnSizes[savedIndex];
        if (savedSizes.size() == columnsSplitter->count()) {
            columnsSplitter->setSizes(savedSizes);
        }
    } else {
        // Default: equal widths for columns (spacer stays small).
        QList<int> sizes;
        const int count = columnsSplitter->count();
        const int colsCount = std::max(1, count - 1);
        sizes.reserve(count);
        for (int i = 0; i < colsCount; ++i) sizes.push_back(1);
        sizes.push_back(0);
        columnsSplitter->setSizes(sizes);
    }

    connect(columnsSplitter, &QSplitter::splitterMoved, this, [this]() {
        scheduleSaveUserSettings(600);
    });

    wsLayout->addWidget(columnsContainer, 1);

    const int stackIndex = m_workspaceStack->addWidget(workspace);

    const int tabIndex = m_workspaceTabs->addTab(QStringLiteral("Tab %1").arg(tabId));
    m_workspaceTabs->setTabData(tabIndex, tabId);
    m_workspaceTabs->setTabText(tabIndex, QStringLiteral("Tab %1").arg(tabId));

    m_tabs.push_back(tab);
    // Catch missed initial `bookRangeUpdated` (can happen if the backend emits before the UI is fully wired);
    // without this, Prints can stay collapsed until the first scroll triggers a new range event.
    for (const auto &c : createdContainers) {
        if (!c) continue;
        QPointer<QWidget> guard = c;
        QTimer::singleShot(0, this, [this, guard]() {
            if (guard) bootstrapColumnFromClient(guard.data());
        });
        QTimer::singleShot(350, this, [this, guard]() {
            if (guard) bootstrapColumnFromClient(guard.data());
        });
    }

    m_workspaceTabs->setCurrentIndex(tabIndex);
    m_workspaceStack->setCurrentIndex(stackIndex);
    refreshTabCloseButtons();
    syncWatchedSymbols();
    return tab;
}

void MainWindow::triggerAddAction(AddAction action)
{
    setLastAddAction(action);
    switch (action) {
    case AddAction::WorkspaceTab:
        handleNewTabRequested();
        break;
    case AddAction::LadderColumn:
        handleNewLadderRequested();
        break;
    }
}

void MainWindow::setLastAddAction(AddAction action)
{
    m_lastAddAction = action;
    updateAddButtonsToolTip();
}

void MainWindow::updateAddButtonsToolTip()
{
    if (!m_addTabButton) {
        return;
    }
    QString text;
    switch (m_lastAddAction) {
    case AddAction::WorkspaceTab:
        text = tr("Add workspace tab");
        break;
    case AddAction::LadderColumn:
        text = tr("Add ladder column");
        break;
    }
    m_addTabButton->setToolTip(text);
}

void MainWindow::refreshTabCloseButtons()
{
    if (!m_workspaceTabs) {
        return;
    }
    const QSize iconSize(14, 14);
    if (m_tabCloseIconNormal.isNull()) {
        m_tabCloseIconNormal = loadIconTinted(QStringLiteral("x"), QColor("#bfbfbf"), iconSize);
    }
    if (m_tabCloseIconHover.isNull()) {
        m_tabCloseIconHover = loadIconTinted(QStringLiteral("x"), QColor("#ffffff"), iconSize);
    }

    for (int i = 0; i < m_workspaceTabs->count(); ++i) {
        QToolButton *button = qobject_cast<QToolButton *>(m_workspaceTabs->tabButton(i, QTabBar::RightSide));
        if (!button || !button->property("WorkspaceTabCloseButton").toBool()) {
            button = new QToolButton(m_workspaceTabs);
            button->setAutoRaise(true);
            button->setObjectName(QStringLiteral("WorkspaceTabCloseButton"));
            button->setProperty("WorkspaceTabCloseButton", true);
            button->setCursor(Qt::PointingHandCursor);
            button->setFocusPolicy(Qt::NoFocus);
            button->setStyleSheet(QStringLiteral(
                "QToolButton#WorkspaceTabCloseButton {"
                "  border: none;"
                "  background: transparent;"
                "  margin: 0px;"
                "  padding: 0px;"
                "}"
                "QToolButton#WorkspaceTabCloseButton:hover {"
                "  background-color: #3a3a3a;"
                "}"));
            connect(button, &QToolButton::clicked, this, [this, button]() {
                if (!m_workspaceTabs) {
                    return;
                }
                for (int idx = 0; idx < m_workspaceTabs->count(); ++idx) {
                    if (m_workspaceTabs->tabButton(idx, QTabBar::RightSide) == button) {
                        handleTabCloseRequested(idx);
                        break;
                    }
                }
            });
            button->installEventFilter(this);
            m_workspaceTabs->setTabButton(i, QTabBar::RightSide, button);
        }

        if (!m_tabCloseIconNormal.isNull()) {
            button->setIcon(m_tabCloseIconNormal);
        }
        button->setIconSize(QSize(12, 12));
        button->setFixedSize(18, 18);
        button->setContentsMargins(0, 0, 0, 0);
        button->setToolTip(tr("Close tab"));
    }
}

MainWindow::DomColumn MainWindow::createDomColumn(const QString &symbol,
                                                  const QString &accountName,
                                                  WorkspaceTab &tab)
{
    DomColumn result;
    result.symbol = symbol.toUpper();
    result.accountName = accountName.isEmpty() ? QStringLiteral("MEXC Spot") : accountName;

    auto *column = new QFrame(tab.workspace);
    column->setProperty("domContainerPtr",
                        QVariant::fromValue<quintptr>(reinterpret_cast<quintptr>(column)));
    column->setObjectName(QStringLiteral("DomColumnFrame"));
    column->setMouseTracking(true);
    column->installEventFilter(this);

    auto *columnRowLayout = new QHBoxLayout(column);
    columnRowLayout->setContentsMargins(0, 0, 0, 0);
    columnRowLayout->setSpacing(0);

    auto *columnContent = new QWidget(column);
    auto *layout = new QVBoxLayout(columnContent);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    columnRowLayout->addWidget(columnContent, 1);

    auto *header = new QFrame(column);
    header->setObjectName(QStringLiteral("DomTitleBar"));
    header->setProperty("domContainerPtr",
                        QVariant::fromValue<quintptr>(reinterpret_cast<quintptr>(column)));
    header->setMouseTracking(true);
    header->installEventFilter(this);
    auto *hLayout = new QHBoxLayout(header);
    hLayout->setContentsMargins(7, 1, 0, 1);
    hLayout->setSpacing(5);
    header->setFixedHeight(22);

    result.accountColor = accountColorFor(result.accountName);
    result.header = header;

    // Header: [venue icon] [S/F] [TICKER] .......... [X]
    auto *venueIcon = new QLabel(header);
    venueIcon->setFixedSize(16, 16);
    venueIcon->setScaledContents(true);
    hLayout->addWidget(venueIcon);
    result.venueIconLabel = venueIcon;

    auto *marketType = new PillLabel(header);
    marketType->setObjectName(QStringLiteral("MarketTypeBadge"));
    marketType->setFixedSize(16, 16);
    marketType->setAlignment(Qt::AlignCenter);
    marketType->setText(QStringLiteral("S"));
    {
        QFont f = marketType->font();
        f.setWeight(QFont::Black);
        f.setPixelSize(10);
        marketType->setFont(f);
    }
    hLayout->addWidget(marketType);
    result.marketTypeLabel = marketType;

    auto *tickerLabel = new QLabel(result.symbol, header);
    tickerLabel->setProperty("accountColor", result.accountColor);
    applyTickerLabelStyle(tickerLabel, result.accountColor, false);
    tickerLabel->setCursor(Qt::PointingHandCursor);
    tickerLabel->setMouseTracking(true);
    tickerLabel->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);
    tickerLabel->installEventFilter(this);
    hLayout->addWidget(tickerLabel);
    result.tickerLabel = tickerLabel;

    hLayout->addStretch(1);

    auto *readOnly = new PillLabel(header);
    readOnly->setObjectName(QStringLiteral("ReadOnlyBadge"));
    readOnly->setFixedSize(16, 16);
    readOnly->setAlignment(Qt::AlignCenter);
    readOnly->setText(QStringLiteral("!"));
    readOnly->setToolTip(tr("Binance работает только в режиме read-only (торговля отключена)."));
    {
        QFont f = readOnly->font();
        f.setWeight(QFont::Black);
        f.setPixelSize(11);
        readOnly->setFont(f);
    }
    hLayout->addWidget(readOnly);
    result.readOnlyLabel = readOnly;

    auto *compressionButton = new PillToolButton(header);
    compressionButton->setObjectName(QStringLiteral("DomHeaderCompressionButton"));
    compressionButton->setAutoRaise(true);
    compressionButton->setText(QStringLiteral("%1x").arg(std::max(1, result.tickCompression)));
    compressionButton->setCursor(Qt::PointingHandCursor);
    compressionButton->setFixedHeight(16);
    compressionButton->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    hLayout->addWidget(compressionButton);
    result.compressionButton = compressionButton;

    auto *chatButton = new QToolButton(header);
    chatButton->setObjectName(QStringLiteral("DomHeaderChatButton"));
    chatButton->setAutoRaise(true);
    chatButton->setIconSize(QSize(12, 12));
    chatButton->setCursor(Qt::PointingHandCursor);
    chatButton->setFixedSize(20, 20);
    chatButton->setVisible(isChatModAvailable());
    hLayout->addWidget(chatButton, 0, Qt::AlignVCenter);
    result.chatButton = chatButton;
    connect(chatButton, &QToolButton::clicked, this, [this, column]() {
        openChatForContainer(column);
    });

    auto *closeButton = new QToolButton(header);
    closeButton->setObjectName(QStringLiteral("DomHeaderCloseButton"));
    closeButton->setAutoRaise(true);
    closeButton->setIconSize(QSize(12, 12));
    closeButton->setCursor(Qt::PointingHandCursor);
    closeButton->setFixedSize(20, 20);
    hLayout->addWidget(closeButton, 0, Qt::AlignVCenter);
    result.closeButton = closeButton;

    applyHeaderAccent(result);
    layout->addWidget(header);
    result.statusLabel = nullptr;
    result.lastStatusMessage.clear();

    auto *prints = new PrintsWidget(column);
    prints->setMinimumWidth(0);
    prints->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    prints->setProperty("domContainerPtr",
                        QVariant::fromValue<quintptr>(reinterpret_cast<quintptr>(column)));

    auto *printsWrap = new QWidget(column);
    printsWrap->setProperty("domContainerPtr",
                            QVariant::fromValue<quintptr>(reinterpret_cast<quintptr>(column)));
    printsWrap->setMinimumWidth(0);
    printsWrap->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    auto *printsWrapLayout = new QGridLayout(printsWrap);
    printsWrapLayout->setContentsMargins(0, 0, 0, 0);
    printsWrapLayout->setSpacing(0);
    printsWrapLayout->addWidget(prints, 0, 0);

    auto *perfOverlay = new QWidget(printsWrap);
    perfOverlay->setObjectName(QStringLiteral("PerfOverlay"));
    perfOverlay->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    perfOverlay->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum);
    auto *perfLayout = new QVBoxLayout(perfOverlay);
    perfLayout->setContentsMargins(6, 4, 6, 4);
    perfLayout->setSpacing(2);

    auto makePerfLine = [perfOverlay, perfLayout](const QString &text) {
        auto *label = new QLabel(text, perfOverlay);
        label->setObjectName(QStringLiteral("PerfOverlayLine"));
        label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        perfLayout->addWidget(label);
        return label;
    };

    auto *perfPing = makePerfLine(QStringLiteral("Ping: -"));
    auto *perfOrders = makePerfLine(QStringLiteral("Orders: -"));
    auto *perfFps = makePerfLine(QStringLiteral("FPS: -"));
    auto *perfDomHz = makePerfLine(QStringLiteral("DOM Hz: -"));

    printsWrapLayout->addWidget(perfOverlay, 0, 0, Qt::AlignTop | Qt::AlignLeft);

    auto *clusters = new ClustersWidget(column);
    clusters->setProperty("domContainerPtr",
                          QVariant::fromValue<quintptr>(reinterpret_cast<quintptr>(column)));
    clusters->bindPrints(prints);

    auto *dom = new DomWidget(column);
    dom->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    dom->setVolumeHighlightRules(m_volumeRules);
    dom->setProperty("domContainerPtr",
                     QVariant::fromValue<quintptr>(reinterpret_cast<quintptr>(column)));
    prints->setRowHeightOnly(dom->rowHeight());
    clusters->setRowLayout(0, dom->rowHeight(), dom->infoAreaHeight());

    auto *printsDomSplitter = new AnimatedSplitter(Qt::Horizontal, column);
    printsDomSplitter->setObjectName(QStringLiteral("DomPrintsSplitter"));
    printsDomSplitter->setChildrenCollapsible(false);
    printsDomSplitter->setHandleWidth(2);
    printsDomSplitter->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    auto *printsContainer = new AnimatedSplitter(Qt::Horizontal, printsDomSplitter);
    printsContainer->setObjectName(QStringLiteral("ClustersPrintsSplitter"));
    printsContainer->setChildrenCollapsible(true);
    printsContainer->setHandleWidth(2);
    printsContainer->setOpaqueResize(true);
    printsContainer->addWidget(clusters);
    printsContainer->addWidget(printsWrap);
    printsContainer->setStretchFactor(0, 0);
    printsContainer->setStretchFactor(1, 1);
    printsContainer->setCollapsible(0, true);
    printsContainer->setCollapsible(1, true);
    applySplitterSizes(printsContainer, m_savedClustersPrintsSplitterSizes, {110, 220});
    if (auto *handle = printsContainer->handle(1)) {
        handle->setObjectName(QStringLiteral("ClustersResizeHandle"));
        handle->setCursor(Qt::SizeHorCursor);
    }

    auto *domContainer = new QWidget(printsDomSplitter);
    auto *domLayout = new QVBoxLayout(domContainer);
    domLayout->setContentsMargins(0, 0, 0, 0);
    domLayout->setSpacing(0);
    domLayout->addWidget(dom);

    printsDomSplitter->addWidget(printsContainer);
    printsDomSplitter->addWidget(domContainer);
    printsDomSplitter->setStretchFactor(0, 1);
    printsDomSplitter->setStretchFactor(1, 3);
    applySplitterSizes(printsDomSplitter, m_savedDomPrintsSplitterSizes, {200, 600});

    auto *contentWidget = new QWidget(column);
    auto *contentRow = new QHBoxLayout(contentWidget);
    contentRow->setContentsMargins(0, 0, 0, 0);
    contentRow->setSpacing(0);
    contentRow->addWidget(printsDomSplitter);
    auto *domScrollBar = new QScrollBar(Qt::Vertical, contentWidget);
    domScrollBar->setObjectName(QStringLiteral("DomScrollBar"));
    domScrollBar->setVisible(false);
    if (auto *handle = printsDomSplitter->handle(1)) {
        handle->setObjectName(QStringLiteral("DomResizeHandle"));
        handle->setCursor(Qt::SizeHorCursor);
    }

    auto *scroll = new QScrollArea(column);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(contentWidget);
    scroll->setProperty("domContainerPtr",
                        QVariant::fromValue<quintptr>(reinterpret_cast<quintptr>(column)));
    if (scroll->viewport()) {
        scroll->viewport()->setProperty(
            "domContainerPtr",
            QVariant::fromValue<quintptr>(reinterpret_cast<quintptr>(column)));
    }

    layout->addWidget(scroll, 1);

    // Pinned clusters footer (totals + time labels), fixed to viewport bottom.
    auto *clustersFooterOverlay = new ClustersFooterOverlay(scroll->viewport(), prints, clusters);
    clustersFooterOverlay->setProperty("domContainerPtr",
                                       QVariant::fromValue<quintptr>(reinterpret_cast<quintptr>(column)));
    scroll->viewport()->setProperty(
        "clustersFooterOverlayPtr",
        QVariant::fromValue<quintptr>(reinterpret_cast<quintptr>(clustersFooterOverlay)));
    clustersFooterOverlay->show();

    auto repositionClustersFooter =
        [this,
         columnGuard = QPointer<QWidget>(column),
         clustersFooterOverlay,
         scroll,
         clusters]() {
        if (!columnGuard) {
            return;
        }
        WorkspaceTab *tab = nullptr;
        DomColumn *col = nullptr;
        int idx = -1;
        if (locateColumn(columnGuard.data(), tab, col, idx) && col) {
            repositionClustersFooterOverlay(*col);
        } else if (clustersFooterOverlay && scroll && scroll->viewport() && clusters) {
            const int footerH = clustersFooterOverlay->height() > 0 ? clustersFooterOverlay->height() : 34;
            const int y = std::max(0, scroll->viewport()->height() - footerH);
            const QPoint clustersPos = clusters->mapTo(scroll->viewport(), QPoint(0, 0));
            int x = clustersPos.x();
            int w = clusters->width();
            const int maxW = std::max(0, scroll->viewport()->width() - x);
            w = std::clamp(w, 0, maxW);
            if (x < 0) {
                x = 0;
            }
            clustersFooterOverlay->setGeometry(x, y, w, footerH);
            clustersFooterOverlay->raise();
            clustersFooterOverlay->show();
        }
    };
    connect(prints, &PrintsWidget::clusterBucketsChanged, clustersFooterOverlay, qOverload<>(&QWidget::update));
    connect(prints, &PrintsWidget::clusterBucketsChanged, this, repositionClustersFooter);
    connect(clusters, &ClustersWidget::layoutChanged, clustersFooterOverlay, qOverload<>(&QWidget::update));
    connect(clusters, &ClustersWidget::layoutChanged, this, repositionClustersFooter);
    repositionClustersFooter();
    QTimer::singleShot(0, this, repositionClustersFooter);

    // Notional presets pinned to viewport (always visible regardless of scroll position).
    const QString notionalPresetKey =
        result.accountName.trimmed().toLower() + QLatin1Char('|') + result.symbol.trimmed().toUpper();
    const auto savedPresetsIt = m_notionalPresetsByKey.constFind(notionalPresetKey);
    if (savedPresetsIt != m_notionalPresetsByKey.constEnd()) {
        result.notionalValues = savedPresetsIt.value();
    } else {
        for (std::size_t i = 0; i < result.notionalValues.size(); ++i) {
            result.notionalValues[i] = m_defaultNotionalPresets[i];
        }
    }
    result.orderNotional = result.notionalValues.at(std::min<std::size_t>(3, result.notionalValues.size() - 1));
    const int presetCount = static_cast<int>(result.notionalValues.size());
    auto *notionalOverlay = new QWidget(scroll->viewport());
    notionalOverlay->setAttribute(Qt::WA_TranslucentBackground, true);
    notionalOverlay->setStyleSheet(QStringLiteral("background: transparent;"));
    auto *notionalLayout = new QVBoxLayout(notionalOverlay);
    notionalLayout->setContentsMargins(0, 0, 0, 6);
    notionalLayout->setSpacing(6);
    notionalLayout->addStretch();

    const auto sourceForLeverage = symbolSourceForAccount(result.accountName);
    const bool lighterPerp =
        (sourceForLeverage == SymbolSource::Lighter) && !result.symbol.contains(QLatin1Char('/'));
    const bool hasLeverage = (sourceForLeverage == SymbolSource::MexcFutures)
                             || (sourceForLeverage == SymbolSource::UzxSwap)
                             || lighterPerp;
    const QString levSym = result.symbol.toUpper();
    const int maxLev = (sourceForLeverage == SymbolSource::MexcFutures)
                           ? std::max(1, m_mexcFuturesMaxLeverageBySymbol.value(levSym, 200))
                           : (sourceForLeverage == SymbolSource::UzxSwap)
                               ? std::max(1, m_uzxSwapMaxLeverageBySymbol.value(levSym, 125))
                               : (lighterPerp ? std::max(1, m_lighterMaxLeverageBySymbol.value(levSym, 10)) : 200);
    result.leverage = std::clamp(m_futuresLeverageBySymbol.value(levSym, 20), 1, maxLev);
    auto *levBtn = new QToolButton(notionalOverlay);
    levBtn->setText(QStringLiteral("%1x").arg(result.leverage));
    levBtn->setFixedSize(52, 21);
    levBtn->setCursor(Qt::PointingHandCursor);
    levBtn->setToolTip(tr("Leverage (per symbol)"));
    levBtn->setStyleSheet(QStringLiteral(
        "QToolButton {"
        "  border: 1px solid #f5b642;"
        "  border-radius: 0px;"
        "  padding: 4px 6px;"
        "  background: rgba(0,0,0,0.22);"
        "  color: #ffe4b3;"
        "  font-weight: 700;"
        "}"
        "QToolButton:hover {"
        "  background: rgba(245,182,66,0.18);"
        "  color: #fff3db;"
        "}"));
    levBtn->setProperty("domContainerPtr",
                        QVariant::fromValue<quintptr>(reinterpret_cast<quintptr>(column)));
    auto *levMenu = new QMenu(levBtn);
    levMenu->setStyleSheet(QStringLiteral("QMenu { background:#1f1f1f; color:#e0e0e0; }"
                                          "QMenu::item:selected { background:#2c2c2c; }"));
    connect(levMenu, &QMenu::aboutToShow, this,
            [this, columnGuard = QPointer<QWidget>(column), levBtn, levMenu]() {
        levMenu->clear();
        if (!columnGuard) {
            return;
        }
        WorkspaceTab *tab = nullptr;
        DomColumn *col = nullptr;
        int idx = -1;
        locateColumn(columnGuard.data(), tab, col, idx);
        if (!col) {
            return;
        }
        const auto src = symbolSourceForAccount(col->accountName);
        const bool isLighterPerp =
            (src == SymbolSource::Lighter) && !col->symbol.contains(QLatin1Char('/'));
        if (src != SymbolSource::MexcFutures && src != SymbolSource::UzxSwap && !isLighterPerp) {
            return;
        }

        const QString sym = col->symbol.toUpper();
        if (src == SymbolSource::UzxSwap && !m_uzxSwapMaxLeverageBySymbol.contains(sym) && m_uzxSwapSymbols.isEmpty()) {
            fetchSymbolLibrary(SymbolSource::UzxSwap, nullptr);
        }
        if (isLighterPerp && !m_lighterMaxLeverageBySymbol.contains(sym)) {
            requestLighterLeverageLimit(sym);
            QAction *loading = levMenu->addAction(tr("Loading limits..."));
            loading->setEnabled(false);
            return;
        }
        const int maxLev = (src == SymbolSource::MexcFutures)
                               ? std::max(1, m_mexcFuturesMaxLeverageBySymbol.value(sym, 200))
                               : (src == SymbolSource::UzxSwap)
                                   ? std::max(1, m_uzxSwapMaxLeverageBySymbol.value(sym, 125))
                                   : (isLighterPerp ? std::max(1, m_lighterMaxLeverageBySymbol.value(sym, 10)) : 200);

        QList<int> presets;
        if (src == SymbolSource::MexcFutures) {
            const auto tagsIt = m_mexcFuturesLeverageTagsBySymbol.constFind(sym);
            if (tagsIt != m_mexcFuturesLeverageTagsBySymbol.constEnd() && !tagsIt->isEmpty()) {
                presets = *tagsIt;
            } else {
                presets = {1, 3, 5, 10, 20, 25, 50, 75, 100, 125, 150, 200, 250, 300, 400};
            }
        } else if (src == SymbolSource::UzxSwap) {
            presets = {1, 2, 3, 5, 10, 20, 25, 50, 75, 100, 125};
        } else {
            presets = {1, 2, 3, 5, 10, 20, 50, 100, 200};
        }
        presets.erase(std::remove_if(presets.begin(),
                                     presets.end(),
                                     [maxLev](int v) { return v < 1 || v > maxLev; }),
                      presets.end());
        std::sort(presets.begin(), presets.end());
        presets.erase(std::unique(presets.begin(), presets.end()), presets.end());
        if (!presets.contains(maxLev) && maxLev > 0) {
            presets.push_back(maxLev);
        }
        std::sort(presets.begin(), presets.end());

        for (int v : presets) {
            QAction *a = levMenu->addAction(QStringLiteral("%1x").arg(v));
            connect(a, &QAction::triggered, this, [this, columnGuard, v]() {
                if (!columnGuard) {
                    return;
                }
                applyLeverageForColumn(columnGuard.data(), v);
            });
        }
        levMenu->addSeparator();
        QAction *custom = levMenu->addAction(tr("Custom..."));
        connect(custom, &QAction::triggered, this, [this, columnGuard]() {
            if (!columnGuard) {
                return;
            }
            WorkspaceTab *tab = nullptr;
            DomColumn *col = nullptr;
            int idx = -1;
            if (!locateColumn(columnGuard.data(), tab, col, idx) || !col) {
                return;
            }
            const auto src = symbolSourceForAccount(col->accountName);
            const bool isLighterPerp =
                (src == SymbolSource::Lighter) && !col->symbol.contains(QLatin1Char('/'));
            if (src != SymbolSource::MexcFutures && src != SymbolSource::UzxSwap && !isLighterPerp) {
                return;
            }
            const QString sym = col->symbol.toUpper();
            const int maxLev = (src == SymbolSource::MexcFutures)
                                   ? std::max(1, m_mexcFuturesMaxLeverageBySymbol.value(sym, 200))
                                   : (src == SymbolSource::UzxSwap)
                                       ? std::max(1, m_uzxSwapMaxLeverageBySymbol.value(sym, 125))
                                       : std::max(1, m_lighterMaxLeverageBySymbol.value(sym, 10));
            bool ok = false;
            const int v = QInputDialog::getInt(this,
                                               tr("Leverage"),
                                               tr("Set leverage (x):"),
                                               col->leverage > 0 ? col->leverage : 20,
                                               1,
                                               maxLev,
                                               1,
                                               &ok);
            if (ok) {
                applyLeverageForColumn(columnGuard.data(), v);
            }
        });
    });
    connect(levBtn, &QToolButton::clicked, this, [levBtn, levMenu]() {
        if (!levBtn || !levMenu) {
            return;
        }
        levMenu->popup(levBtn->mapToGlobal(QPoint(0, levBtn->height())));
    });
    levBtn->setVisible(hasLeverage);
    notionalLayout->addWidget(levBtn);
    result.leverageButton = levBtn;

    auto *notionalGroup = new QButtonGroup(notionalOverlay);
    notionalGroup->setExclusive(true);
    for (int i = 0; i < presetCount; ++i)
    {
        const double preset = result.notionalValues[i];
        auto *btn = new QToolButton(notionalOverlay);
        btn->setCheckable(true);
        btn->setText(QString::number(preset, 'g', 6));
        btn->setFixedSize(52, 21);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setStyleSheet(QStringLiteral(
            "QToolButton {"
            "  border: 1px solid #4a90e2;"
            "  border-radius: 0px;"
            "  padding: 4px 6px;"
            "  background: rgba(0,0,0,0.18);"
            "  color: #cfd8dc;"
            "}"
            "QToolButton:checked {"
            "  border-color: #4aa3ff;"
            "  background: rgba(74,163,255,0.28);"
            "  color: #e3f2fd;"
            "}"));
        notionalGroup->addButton(btn, i);
        if (preset == 10.0)
        {
            btn->setChecked(true);
            result.orderNotional = preset;
        }
        connect(btn, &QToolButton::clicked, this, [this, columnGuard = QPointer<QWidget>(column), i]() {
            if (!columnGuard) {
                return;
            }
            WorkspaceTab *tab = nullptr;
            DomColumn *col = nullptr;
            int idx = -1;
            if (locateColumn(columnGuard.data(), tab, col, idx) && col)
            {
                if (i >= 0 && i < static_cast<int>(col->notionalValues.size()))
                {
                    col->orderNotional = col->notionalValues[i];
                }
            }
        });
        btn->setProperty("domContainerPtr",
                         QVariant::fromValue<quintptr>(reinterpret_cast<quintptr>(column)));
        btn->setProperty("notionalIndex", i);
        btn->installEventFilter(this);
        notionalLayout->addWidget(btn);
    }
    notionalOverlay->adjustSize();

    auto repositionOverlay = [scroll, notionalOverlay, printsWrap]() {
        if (!notionalOverlay || !scroll) return;
        notionalOverlay->adjustSize();
        const int pad = 0;
        int x = pad;
        if (auto *vp = scroll->viewport()) {
            if (printsWrap) {
                const QPoint printsPos = printsWrap->mapTo(vp, QPoint(0, 0));
                x = printsPos.x() + pad;
                const int maxInPrints =
                    printsPos.x() + std::max(0, printsWrap->width() - notionalOverlay->width() - pad);
                if (x > maxInPrints) {
                    x = maxInPrints;
                }
            }
            const int maxX = std::max(pad, vp->width() - notionalOverlay->width() - pad);
            x = std::clamp(x, pad, maxX);
        }
        const int bottomMargin = 36;
        int y = scroll->viewport()->height() - notionalOverlay->height() - bottomMargin;
        if (y < 8) y = 8;
        const int maxY = std::max(0, scroll->viewport()->height() - notionalOverlay->height() - 6);
        if (y > maxY) y = maxY;
        notionalOverlay->move(x, y);
        notionalOverlay->raise();
        notionalOverlay->show();
    };
    scroll->viewport()->installEventFilter(this);
    scroll->viewport()->setProperty("notionalOverlayPtr",
                                    QVariant::fromValue<quintptr>(reinterpret_cast<quintptr>(notionalOverlay)));
    scroll->viewport()->setProperty(
        "domContainerPtr",
        QVariant::fromValue<quintptr>(reinterpret_cast<quintptr>(column)));
    repositionOverlay();
    QTimer::singleShot(0, this, repositionOverlay);

    // Position overlay over prints (fixed relative to column)
    auto *positionOverlay = new QFrame(scroll->viewport());
    positionOverlay->setObjectName(QStringLiteral("PositionOverlay"));
    positionOverlay->setAttribute(Qt::WA_TranslucentBackground, false);
    positionOverlay->setStyleSheet(QStringLiteral(
        "QFrame#PositionOverlay {"
        "  background: transparent;"
        "  border: none;"
        "}"
        "QLabel { color: #ffffff; font-weight: 800; }"
        "QToolButton#PositionExitButton {"
        "  background-color: #b71c1c;"
        "  color: #ffffff;"
        "  border: none;"
        "  border-radius: 0px;"
        "  padding: 2px 10px;"
        "  font-weight: 800;"
        "}"
        "QToolButton#PositionExitButton:hover { background-color: #d32f2f; }"));

    auto *posLayout = new QHBoxLayout(positionOverlay);
    posLayout->setContentsMargins(0, 0, 0, 0);
    posLayout->setSpacing(0);
    auto *avgLabel = new QLabel(positionOverlay);
    auto *valueLabel = new QLabel(positionOverlay);
    auto *pctLabel = new QLabel(positionOverlay);
    auto *pnlLabel = new QLabel(positionOverlay);

    auto setupCellLabel = [](QLabel *lbl) {
        lbl->setAlignment(Qt::AlignCenter);
        lbl->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        lbl->setMinimumHeight(24);
        lbl->setStyleSheet(QStringLiteral(
            "QLabel {"
            "  padding: 3px 6px;"
            "  border-radius: 0px;"
            "  border: none;"
            "  background-color: rgba(11,11,11,235);"
            "}"));
    };
    setupCellLabel(avgLabel);
    setupCellLabel(valueLabel);
    setupCellLabel(pnlLabel);
    pctLabel->setVisible(false);
    pctLabel->setText(QString());

    auto *exitBtn = new QToolButton(positionOverlay);
    exitBtn->setObjectName(QStringLiteral("PositionExitButton"));
    exitBtn->setText(tr("EXIT"));
    exitBtn->setCursor(Qt::PointingHandCursor);
    exitBtn->setVisible(false);

    posLayout->addWidget(avgLabel, 1);
    posLayout->addWidget(valueLabel, 1);
    posLayout->addWidget(pnlLabel, 1);

    positionOverlay->hide();

    auto repositionPositionOverlay = [scroll, positionOverlay]() {
        if (!positionOverlay || !scroll || !scroll->viewport()) return;
        const int pad = 0;
        const int bottomMargin = 0;
        const int w = std::max(80, scroll->viewport()->width());
        positionOverlay->setFixedWidth(w);
        positionOverlay->adjustSize();
        int x = pad;
        int y = scroll->viewport()->height() - positionOverlay->height() - bottomMargin;
        if (y < 4) y = 4;
        positionOverlay->move(x, y);
        positionOverlay->raise();
    };

    scroll->viewport()->setProperty("positionOverlayPtr",
                                    QVariant::fromValue<quintptr>(reinterpret_cast<quintptr>(positionOverlay)));
    repositionPositionOverlay();
    QTimer::singleShot(0, this, repositionPositionOverlay);

    QPointer<QWidget> columnGuard(column);

    connect(exitBtn, &QToolButton::clicked, this, [this, columnGuard]() {
        if (!columnGuard) {
            return;
        }
        if (!m_tradeManager) {
            return;
        }
        WorkspaceTab *tab = nullptr;
        DomColumn *col = nullptr;
        int idx = -1;
        if (!locateColumn(columnGuard.data(), tab, col, idx) || !col) {
            return;
        }
        markPendingCancelForSymbol(col->symbol);
        m_tradeManager->cancelAllOrders(col->symbol, col->accountName);
        double priceHint = 0.0;
        if (col->dom) {
            const TradePosition pos =
                m_tradeManager->positionForSymbol(col->symbol, col->accountName);
            priceHint = (pos.side == OrderSide::Buy) ? col->dom->bestBid() : col->dom->bestAsk();
        }
        m_tradeManager->closePositionMarket(col->symbol, col->accountName, priceHint);
    });

    result.tickCompression = 1;
    const QString symbolUpper = result.symbol;
    const auto source = symbolSourceForAccount(result.accountName.isEmpty() ? QStringLiteral("MEXC Spot")
                                                                            : result.accountName);
    QString exchangeArg = QStringLiteral("mexc");
    if (source == SymbolSource::UzxSwap) {
        exchangeArg = QStringLiteral("uzxswap");
    } else if (source == SymbolSource::UzxSpot) {
        exchangeArg = QStringLiteral("uzxspot");
    } else if (source == SymbolSource::MexcFutures) {
        exchangeArg = QStringLiteral("mexc_futures");
    } else if (source == SymbolSource::BinanceSpot) {
        exchangeArg = QStringLiteral("binance");
    } else if (source == SymbolSource::BinanceFutures) {
        exchangeArg = QStringLiteral("binance_futures");
    } else if (source == SymbolSource::Lighter) {
        exchangeArg = QStringLiteral("lighter");
    }
    const int effectiveLevels = effectiveLevelsForColumn(result);

    QString proxyType;
    QString proxyRaw;
    if (m_connectionStore) {
        ConnectionStore::Profile prof = ConnectionStore::Profile::MexcSpot;
        switch (source) {
        case SymbolSource::MexcFutures:
            prof = ConnectionStore::Profile::MexcFutures;
            break;
        case SymbolSource::UzxSpot:
            prof = ConnectionStore::Profile::UzxSpot;
            break;
        case SymbolSource::UzxSwap:
            prof = ConnectionStore::Profile::UzxSwap;
            break;
        case SymbolSource::BinanceSpot:
            prof = ConnectionStore::Profile::BinanceSpot;
            break;
        case SymbolSource::BinanceFutures:
            prof = ConnectionStore::Profile::BinanceFutures;
            break;
        case SymbolSource::Lighter:
            prof = ConnectionStore::Profile::Lighter;
            break;
        case SymbolSource::Mexc:
        default:
            prof = ConnectionStore::Profile::MexcSpot;
            break;
        }
        const MexcCredentials creds = m_connectionStore->loadMexcCredentials(prof);
        proxyType = creds.proxyType.trimmed().toLower();
        if (proxyType == QStringLiteral("https")) {
            proxyType = QStringLiteral("http");
        }
        proxyRaw = creds.proxy.trimmed();
    }
    auto *client =
        new LadderClient(m_backendPath,
                         symbolUpper,
                         effectiveLevels,
                         exchangeArg,
                         column,
                         prints,
                         proxyType,
                         proxyRaw);
    client->setCompression(result.tickCompression);

    connect(client,
            &LadderClient::statusMessage,
            this,
            &MainWindow::handleLadderStatusMessage);
    connect(client, &LadderClient::pingUpdated, this, &MainWindow::handleLadderPingUpdated);
    connect(client,
            &LadderClient::bookRangeUpdated,
            this,
            [this, columnGuard](qint64 minTick, qint64 maxTick, qint64 centerTick, double tickSize) {
                if (!columnGuard) {
                    return;
                }
                handleColumnBufferRange(columnGuard.data(), minTick, maxTick, centerTick, tickSize);
            });
    connect(client,
            &LadderClient::bookUpdated,
            this,
            [this, columnGuard](quint64) {
                if (!columnGuard) {
                    return;
                }
                WorkspaceTab *tab = nullptr;
                DomColumn *colPtr = nullptr;
                int idx = -1;
                if (!locateColumn(columnGuard.data(), tab, colPtr, idx) || !colPtr) {
                    return;
                }
                // Mark that the book changed even when the window didn't; this drives DOM Hz.
                ++colPtr->bufferRevision;

                if (m_domTargetFps > 0) {
                    return;
                }
                // Event-driven mode: coalesce snapshot pulls via the existing pending viewport queue.
                if (!colPtr->hasBuffer || !colPtr->dom || !colPtr->client) {
                    return;
                }
                if (!colPtr->container || !colPtr->container->isVisible()) {
                    return;
                }
                if (colPtr->lastViewportTop < colPtr->lastViewportBottom) {
                    return;
                }
                if (colPtr->pendingViewportUpdate) {
                    return;
                }
                colPtr->pendingViewportUpdate = true;
                colPtr->pendingViewportBottom = colPtr->lastViewportBottom;
                colPtr->pendingViewportTop = colPtr->lastViewportTop;
                colPtr->pendingViewportRevision = colPtr->bufferRevision;
                QTimer::singleShot(0, this, [this, columnGuard]() {
                    if (!columnGuard) {
                        return;
                    }
                    WorkspaceTab *tab2 = nullptr;
                    DomColumn *col2 = nullptr;
                    int idx2 = -1;
                    if (!locateColumn(columnGuard.data(), tab2, col2, idx2) || !col2) {
                        return;
                    }
                    flushPendingColumnViewport(*col2);
                });
            });
    connect(client,
            &LadderClient::bucketTicksUpdated,
            dom,
            &DomWidget::notifyBucketTicksUpdated);

    connect(dom,
            &DomWidget::rowClicked,
            this,
            &MainWindow::handleDomRowClicked);
    connect(dom, &DomWidget::hoverInfoChanged, prints, &PrintsWidget::setHoverInfo);
    connect(dom, &DomWidget::infoAreaHeightChanged, prints, &PrintsWidget::setDomInfoAreaHeight);
    connect(dom, &DomWidget::infoAreaHeightChanged, clusters, &ClustersWidget::setInfoAreaHeight);
    connect(prints, &PrintsWidget::cancelMarkerRequested, this, &MainWindow::handlePrintsCancelRequested);
    connect(prints, &PrintsWidget::beginMoveMarkerRequested, this, &MainWindow::handlePrintsMoveBeginRequested);
    connect(prints, &PrintsWidget::commitMoveMarkerRequested, this, &MainWindow::handlePrintsMoveCommitRequested);
    connect(prints, &PrintsWidget::beginMoveSltpMarkerRequested, this, &MainWindow::handlePrintsSltpMoveBeginRequested);
    connect(prints, &PrintsWidget::commitMoveSltpMarkerRequested, this, &MainWindow::handlePrintsSltpMoveCommitRequested);
    connect(prints, &PrintsWidget::dragPreviewPriceRequested, this, &MainWindow::handlePrintsDragPreviewPriceRequested);
    prints->setDomInfoAreaHeight(dom->infoAreaHeight());
    connect(dom, &DomWidget::infoAreaHeightChanged, this, [this, columnGuard](int) {
        if (!columnGuard) {
            return;
        }
        WorkspaceTab *tab = nullptr;
        DomColumn *col = nullptr;
        int idx = -1;
        if (locateColumn(columnGuard.data(), tab, col, idx) && col) {
            updateColumnViewport(*col, false);
        }
    });
    connect(dom, &DomWidget::exitPositionRequested, this, [this, columnGuard]() {
        if (!columnGuard) {
            return;
        }
        if (!m_tradeManager) {
            return;
        }
        WorkspaceTab *tab = nullptr;
        DomColumn *col = nullptr;
        int idx = -1;
        if (!locateColumn(columnGuard.data(), tab, col, idx) || !col) {
            return;
        }
        // Flatten this symbol: cancel orders first, then close by market.
        markPendingCancelForSymbol(col->symbol);
        m_tradeManager->cancelAllOrders(col->symbol, col->accountName);
        double priceHint = 0.0;
        if (col->dom) {
            const TradePosition pos =
                m_tradeManager->positionForSymbol(col->symbol, col->accountName);
            priceHint = (pos.side == OrderSide::Buy) ? col->dom->bestBid() : col->dom->bestAsk();
        }
        m_tradeManager->closePositionMarket(col->symbol, col->accountName, priceHint);
    });

    connect(client,
            &LadderClient::statusMessage,
            this,
            [this, columnGuard](const QString &msg) {
                if (!columnGuard) {
                    return;
                }
                WorkspaceTab *tab = nullptr;
                DomColumn *colPtr = nullptr;
                int idx = -1;
                if (locateColumn(columnGuard.data(), tab, colPtr, idx) && colPtr) {
                    colPtr->lastStatusMessage = msg;
                    updateColumnStatusLabel(*colPtr);
                }
            });

    connect(closeButton, &QToolButton::clicked, this, [this, columnGuard]() {
        if (!columnGuard) {
            return;
        }
        QTimer::singleShot(0, this, [this, columnGuard]() {
            if (columnGuard) {
                removeDomColumn(columnGuard.data());
            }
        });
    });
    connect(compressionButton, &QToolButton::clicked, this, [this, columnGuard, compressionButton]() {
        if (!columnGuard) {
            return;
        }
        WorkspaceTab *tab = nullptr;
        DomColumn *col = nullptr;
        int idx = -1;
        if (!locateColumn(columnGuard.data(), tab, col, idx) || !col) {
            return;
        }
        showCompressionMenu(*col,
                            compressionButton->mapToGlobal(QPoint(0, compressionButton->height())));
    });
    header->installEventFilter(this);

    result.container = column;
    result.dom = dom;
    result.prints = prints;
    result.printsWrap = printsWrap;
    result.clusters = clusters;
    result.perfOverlay = perfOverlay;
    result.perfPingLabel = perfPing;
    result.perfOrderPingLabel = perfOrders;
    result.perfFpsLabel = perfFps;
    result.perfDomHzLabel = perfDomHz;
    result.printsDomSplitter = printsDomSplitter;
    result.clustersPrintsSplitter = printsContainer;
    result.scrollArea = scroll;
    result.scrollBar = domScrollBar;
    if (domScrollBar) {
        connect(domScrollBar, &QScrollBar::valueChanged, this, [this, columnGuard](int value) {
            if (!columnGuard) {
                return;
            }
            handleDomScroll(columnGuard.data(), value);
        });
    }
    connect(printsDomSplitter, &QSplitter::splitterMoved, this, [this, printsDomSplitter, columnGuard]() {
        m_savedDomPrintsSplitterSizes = printsDomSplitter->sizes();
        if (columnGuard) {
            WorkspaceTab *tab = nullptr;
            DomColumn *col = nullptr;
            int idx = -1;
            if (locateColumn(columnGuard.data(), tab, col, idx) && col) {
                repositionNotionalOverlay(*col);
                repositionClustersFooterOverlay(*col);
            }
        }
        scheduleSaveUserSettings(600);
    });
    connect(printsContainer, &QSplitter::splitterMoved, this, [this, printsContainer, columnGuard]() {
        m_savedClustersPrintsSplitterSizes = printsContainer->sizes();
        if (m_savedClustersPrintsSplitterSizes.size() >= 2 && m_savedClustersPrintsSplitterSizes.at(0) > 0) {
            m_clustersPrintsSplitterEverShown = true;
        }
        if (columnGuard) {
            WorkspaceTab *tab = nullptr;
            DomColumn *col = nullptr;
            int idx = -1;
            if (locateColumn(columnGuard.data(), tab, col, idx) && col) {
                enforceNotionalPrintsMinWidth(*col);
                repositionNotionalOverlay(*col);
                repositionClustersFooterOverlay(*col);
            }
        }
        scheduleSaveUserSettings(600);
    });
    result.client = client;
    result.accountName = QStringLiteral("MEXC Spot");
    result.accountColor = accountColorFor(result.accountName);
    result.notionalOverlay = notionalOverlay;
    result.notionalGroup = notionalGroup;
    result.clustersFooterOverlay = clustersFooterOverlay;
    result.positionOverlay = positionOverlay;
    result.positionAvgLabel = avgLabel;
    result.positionValueLabel = valueLabel;
    result.positionPctLabel = pctLabel;
    result.positionPnlLabel = pnlLabel;
    result.positionExitButton = exitBtn;
    result.localOrders.clear();
    result.dom->setLocalOrders(result.localOrders);
    if (result.prints) {
        QVector<LocalOrderMarker> empty;
        result.prints->setLocalOrders(empty);
    }
    updateColumnStatusLabel(result);
    updatePerfOverlay(result);
    enforceNotionalPrintsMinWidth(result);
    repositionNotionalOverlay(result);
    return result;
}

MainWindow::WorkspaceTab *MainWindow::currentWorkspaceTab()
{
    if (!m_workspaceTabs) {
        return nullptr;
    }
    const int index = m_workspaceTabs->currentIndex();
    if (index < 0) {
        return nullptr;
    }

    const QVariant data = m_workspaceTabs->tabData(index);
    const int id = data.isValid() ? data.toInt() : 0;
    for (auto &tab : m_tabs) {
        if (tab.id == id) {
            return &tab;
        }
    }
    return nullptr;
}

int MainWindow::findTabIndexById(int id) const
{
    for (int i = 0; i < m_tabs.size(); ++i) {
        if (m_tabs[i].id == id) {
            return i;
        }
    }
    return -1;
}

void MainWindow::handleTabChanged(int index)
{
    if (!m_workspaceTabs || !m_workspaceStack) {
        return;
    }
    if (index < 0) {
        return;
    }
    const QVariant data = m_workspaceTabs->tabData(index);
    const int id = data.isValid() ? data.toInt() : 0;
    const int stackIndex = findTabIndexById(id);
    if (stackIndex >= 0) {
        m_workspaceStack->setCurrentIndex(stackIndex);
    }
    if (!m_tabUnderlineHiddenForDrag) {
        updateTabUnderline(index);
    }
}

void MainWindow::handleTabCloseRequested(int index)
{
    if (!m_workspaceTabs || !m_workspaceStack) {
        return;
    }
    if (m_workspaceTabs->count() <= 1 || index < 0) {
        return;
    }

    const QVariant data = m_workspaceTabs->tabData(index);
    const int id = data.isValid() ? data.toInt() : 0;
    const int tabIdx = findTabIndexById(id);
    if (tabIdx < 0) {
        return;
    }

    WorkspaceTab tab = m_tabs.takeAt(tabIdx);
    m_recycledTabIds.push_back(tab.id);

    QWidget *wsWidget = tab.workspace;
    const int stackIndex = m_workspaceStack->indexOf(wsWidget);
    if (stackIndex >= 0) {
        QWidget *widget = m_workspaceStack->widget(stackIndex);
        m_workspaceStack->removeWidget(widget);
        widget->deleteLater();
    }

    m_workspaceTabs->removeTab(index);
    QTimer::singleShot(0, this, [this]() { refreshTabCloseButtons(); });

    if (m_workspaceTabs->count() > 0) {
        const int newIndex = std::min(index, m_workspaceTabs->count() - 1);
        m_workspaceTabs->setCurrentIndex(newIndex);
    }
}

void MainWindow::handleNewTabRequested()
{
    setLastAddAction(AddAction::WorkspaceTab);
    createWorkspaceTab(QVector<SavedColumn>());
}

void MainWindow::handleNewLadderRequested()
{
    WorkspaceTab *tab = currentWorkspaceTab();
    if (!tab || !tab->columns) {
        return;
    }

    const int tabId = tab->id;
    SymbolPickerDialog *picker =
        createSymbolPicker(tr("Add ladder"),
                           !m_symbols.isEmpty() ? m_symbols.first() : QString(),
                           QStringLiteral("MEXC Spot"));
    if (!picker) {
        return;
    }
    connect(picker, &QDialog::accepted, this, [this, picker, tabId]() {
        const QString symbol = picker->selectedSymbol().trimmed().toUpper();
        if (symbol.isEmpty()) {
            picker->deleteLater();
            return;
        }
        const QString account = picker->selectedAccount().trimmed().isEmpty()
                                    ? QStringLiteral("MEXC Spot")
                                    : picker->selectedAccount();

        const int idx = findTabIndexById(tabId);
        if (idx < 0 || idx >= m_tabs.size()) {
            picker->deleteLater();
            return;
        }
        setLastAddAction(AddAction::LadderColumn);
        WorkspaceTab &targetTab = m_tabs[idx];
        DomColumn col = createDomColumn(symbol, account, targetTab);
        applySymbolToColumn(col, symbol, account);
        targetTab.columnsData.push_back(col);
        syncWatchedSymbols();
        if (targetTab.columns) {
            const int spacerIndex =
                (targetTab.columnsSpacer ? targetTab.columns->indexOf(targetTab.columnsSpacer) : -1);
            const int insertIndex = spacerIndex >= 0 ? spacerIndex : targetTab.columns->count();
            targetTab.columns->insertWidget(insertIndex, col.container);
            targetTab.columns->setStretchFactor(targetTab.columns->indexOf(col.container), 0);
            applyNewColumnWidthDefaults(targetTab.columns, targetTab.columnsSpacer, col.container);
        }
        {
            // Catch missed initial `bookRangeUpdated` so Prints doesn't stay collapsed until the first scroll.
            QPointer<QWidget> guard = col.container;
            QTimer::singleShot(0, this, [this, guard]() {
                if (guard) bootstrapColumnFromClient(guard.data());
            });
            QTimer::singleShot(350, this, [this, guard]() {
                if (guard) bootstrapColumnFromClient(guard.data());
            });
        }
        scheduleSaveUserSettings(700);
        picker->deleteLater();
    });
    connect(picker, &QDialog::rejected, picker, &QObject::deleteLater);
    picker->open();
    picker->raise();
    picker->activateWindow();
}

void MainWindow::handleLadderStatusMessage(const QString &msg)
{
    if (msg.isEmpty()) {
        return;
    }
    const QString lower = msg.toLower();
    // Ignore noisy heartbeat-style messages
    if (lower.contains(QStringLiteral("ping")) || lower.contains(QStringLiteral("receiving data"))) {
        return;
    }

    static const QStringList keywords = {
        QStringLiteral("error"),
        QStringLiteral("fail"),
        QStringLiteral("reject"),
        QStringLiteral("denied"),
        QStringLiteral("invalid"),
        QStringLiteral("timeout"),
        QStringLiteral("disconnect"),
        QStringLiteral("order")
    };
    bool important = false;
    for (const auto &k : keywords) {
        if (lower.contains(k)) {
            important = true;
            break;
        }
    }
    if (!important) {
        return;
    }

    logLadderStatus(msg);
    addNotification(msg);
}

void MainWindow::logLadderStatus(const QString &msg)
{
    appendConnectionsLog(msg);
}

void MainWindow::logMarkerEvent(const QString &msg)
{
    appendConnectionsLog(msg);
}

void MainWindow::appendConnectionsLog(const QString &msg)
{
    if (isSpamConnectionLog(msg)) {
        appendSpamLogToFile(msg);
        return;
    }
    logToFile(msg);
    if (m_connectionsWindow) {
        m_connectionsWindow->appendLogMessage(msg);
        return;
    }
    m_connectionsLogBacklog.push_back(msg);
    static constexpr int kMaxBacklog = 1200;
    if (m_connectionsLogBacklog.size() > kMaxBacklog) {
        m_connectionsLogBacklog.erase(m_connectionsLogBacklog.begin(),
                                      m_connectionsLogBacklog.begin()
                                          + (m_connectionsLogBacklog.size() - kMaxBacklog));
    }
}

void MainWindow::handleLadderPingUpdated(int ms)
{
    auto *client = qobject_cast<LadderClient *>(sender());
    if (!client) {
        return;
    }
    for (auto &tab : m_tabs) {
        for (auto &col : tab.columnsData) {
            if (col.client != client) {
                continue;
            }
            col.lastPingMs = ms;
            updatePerfOverlay(col);
            return;
        }
    }
}

void MainWindow::handleDomRowClicked(Qt::MouseButton button,
                                     int row,
                                     double price,
                                     double bidQty,
                                     double askQty)
{
    Q_UNUSED(row);
    Q_UNUSED(bidQty);
    Q_UNUSED(askQty);
    if (!m_tradeManager || (button != Qt::LeftButton && button != Qt::RightButton)) {
        return;
    }
    auto *dom = qobject_cast<DomWidget *>(sender());
    if (!dom) {
        return;
    }
    DomColumn *column = nullptr;
    for (auto &tab : m_tabs) {
        for (auto &col : tab.columnsData) {
            if (col.dom == dom) {
                column = &col;
                break;
            }
        }
        if (column) {
            break;
        }
    }
    if (!column) {
        return;
    }
    const SymbolSource src = symbolSourceForAccount(column->accountName);
    const bool lighterPerp = (src == SymbolSource::Lighter) && !column->symbol.contains(QLatin1Char('/'));
    const bool uzxAccount = (src == SymbolSource::UzxSwap || src == SymbolSource::UzxSpot);
    if (m_sltpPlaceHeld) {
        if (!lighterPerp && !uzxAccount) {
            statusBar()->showMessage(tr("SL/TP placement is available for Lighter Perp and UZX"), 2200);
            return;
        }
        if (price <= 0.0) {
            return;
        }
        const TradePosition pos = m_tradeManager->positionForSymbol(column->symbol, column->accountName);
        const bool hasPosition =
            pos.hasPosition && pos.quantity > 0.0 && pos.averagePrice > 0.0;
        if (!hasPosition) {
            statusBar()->showMessage(tr("No active position for SL/TP"), 1800);
            return;
        }
        // Decide SL/TP using:
        // - The trigger-side market (long->ask, short->bid) for normal cases.
        // - If the user clicks *inside the spread*, decide by entry price so a level above entry becomes TP
        //   even when it's still below ask (wide spread / quote gap).
        double bestBid = 0.0;
        double bestAsk = 0.0;
        if (column->dom) {
            bestBid = column->dom->bestBid();
            bestAsk = column->dom->bestAsk();
        }
        const bool hasSpread = (bestBid > 0.0 && bestAsk > 0.0 && bestAsk >= bestBid);
        const bool inSpread = hasSpread && (price > bestBid) && (price < bestAsk);
        const double entry = pos.averagePrice;
        const double triggerSide =
            (pos.side == OrderSide::Buy) ? bestAsk : bestBid; // long->ask, short->bid
        const bool isSl = [&]() -> bool {
            if (pos.side == OrderSide::Buy) {
                if (inSpread) {
                    return price < entry;
                }
                if (triggerSide > 0.0) {
                    return price < triggerSide;
                }
                return price < entry;
            } else {
                if (inSpread) {
                    return price > entry;
                }
                if (triggerSide > 0.0) {
                    return price > triggerSide;
                }
                return price > entry;
            }
        }();

        // Safety: if the trigger condition is already satisfied at placement time,
        // the exchange will execute the reduce-only stop immediately (looks like a "random close").
        // For LONG: SL triggers on ask<=P, TP triggers on bid>=P.
        // For SHORT: SL triggers on bid>=P, TP triggers on ask<=P.
        const double tick = (column->dom ? std::max(1e-12, column->dom->tickSize()) : 1e-12);
        const double eps = tick * 0.25;
        bool triggersNow = false;
        QString triggerWhy;
        if (pos.side == OrderSide::Buy) {
            if (isSl) {
                triggersNow = (bestAsk > 0.0 && bestAsk <= price + eps);
                triggerWhy = QStringLiteral("ask");
            } else {
                triggersNow = (bestBid > 0.0 && bestBid >= price - eps);
                triggerWhy = QStringLiteral("bid");
            }
        } else {
            if (isSl) {
                triggersNow = (bestBid > 0.0 && bestBid >= price - eps);
                triggerWhy = QStringLiteral("bid");
            } else {
                triggersNow = (bestAsk > 0.0 && bestAsk <= price + eps);
                triggerWhy = QStringLiteral("ask");
            }
        }
        if (triggersNow) {
            statusBar()->showMessage(
                tr("%1 @ %2 would trigger immediately (current %3 already crossed). Move the level further away or use market close.")
                    .arg(isSl ? QStringLiteral("SL") : QStringLiteral("TP"))
                    .arg(QString::number(price, 'f', 6))
                    .arg(triggerWhy),
                4500);
            return;
        }

        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        column->sltpLastPlaceMs = nowMs;
        column->sltpLastPlaceWasSl = isSl;
        column->sltpLastPlacePrice = price;
        if (isSl) {
            column->sltpHasSl = true;
            column->sltpSlPrice = price;
            column->sltpSlCreatedMs = nowMs;
            column->sltpSlMissCount = 0;
        } else {
            column->sltpHasTp = true;
            column->sltpTpPrice = price;
            column->sltpTpCreatedMs = nowMs;
            column->sltpTpMissCount = 0;
        }
        refreshSltpMarkers(*column);
        refreshColumnsForSymbol(column->symbol);
        m_tradeManager->placeLighterStopOrder(column->symbol, column->accountName, price, isSl);
        statusBar()->showMessage(
            tr("%1 set @ %2")
                .arg(isSl ? QStringLiteral("SL") : QStringLiteral("TP"))
                .arg(QString::number(price, 'f', 6)),
            2000);
        return;
    }
    const double notional = column->orderNotional > 0.0 ? column->orderNotional : 0.0;
    if (price <= 0.0 || notional <= 0.0) {
        statusBar()->showMessage(tr("Set a positive order size before trading"), 2000);
        return;
    }
    const QString symUpper = column->symbol.trimmed().toUpper().replace(QStringLiteral("-"), QString());
    double quantity = notional / price;
    if (quantity <= 0.0) {
        statusBar()->showMessage(tr("Calculated order quantity is zero"), 2000);
        return;
    }
    if (src == SymbolSource::UzxSwap) {
        if (m_uzxSwapContractValueBySymbol.isEmpty() && !m_uzxSwapRequestInFlight) {
            fetchSymbolLibrary(SymbolSource::UzxSwap, nullptr);
        }
        const bool hasSpecs = !m_uzxSwapContractValueBySymbol.isEmpty();
        if (!hasSpecs) {
            appendConnectionsLog(QStringLiteral("[UZX Swap] Specs not loaded yet; placing order without rounding."));
            statusBar()->showMessage(tr("UZX swap specs not loaded yet"), 2000);
        } else {
            double priceUnit = m_uzxSwapPriceUnitBySymbol.value(symUpper, 0.0);
            if (priceUnit > 0.0) {
                price = std::floor(price / priceUnit + 1e-9) * priceUnit;
            }
            double step = m_uzxSwapContractValueBySymbol.value(symUpper, 0.0);
            const int numPrecision = m_uzxSwapNumPrecisionBySymbol.value(symUpper, -1);
            if (step <= 0.0 && numPrecision >= 0) {
                step = std::pow(10.0, -numPrecision);
            }
            if (step > 0.0) {
                quantity = std::floor(quantity / step + 1e-12) * step;
            }
            if (quantity <= 0.0 || (step > 0.0 && quantity + 1e-12 < step)) {
                const double minNotional = (step > 0.0) ? step * price : 0.0;
                appendConnectionsLog(QStringLiteral("[UZX Swap] Min size blocked: %1 step=%2 price=%3")
                                         .arg(symUpper)
                                         .arg(QString::number(step, 'f', 8))
                                         .arg(QString::number(price, 'f', 8)));
                statusBar()->showMessage(
                    tr("UZX Swap min size for %1: %2 (about %3 USDT)")
                        .arg(symUpper)
                        .arg(QString::number(step, 'f', 8))
                        .arg(QString::number(minNotional, 'f', 4)),
                    3500);
                return;
            }
        }
    }
    const OrderSide side = (button == Qt::LeftButton) ? OrderSide::Buy : OrderSide::Sell;
    clearPendingCancelForSymbol(column->symbol);
    int leverage = column->leverage;
    if (src == SymbolSource::UzxSwap) {
        const int maxLev = m_uzxSwapMaxLeverageBySymbol.value(symUpper, 0);
        if (maxLev > 0 && leverage > maxLev) {
            leverage = maxLev;
            column->leverage = maxLev;
            m_futuresLeverageBySymbol.insert(symUpper, maxLev);
            if (column->leverageButton) {
                column->leverageButton->setText(QStringLiteral("%1x").arg(maxLev));
            }
            saveUserSettings();
        }
    }
    m_tradeManager->placeLimitOrder(column->symbol, column->accountName, price, quantity, side, leverage);
    statusBar()->showMessage(
        tr("Submitting %1 %2 @ %3")
            .arg(side == OrderSide::Buy ? QStringLiteral("BUY") : QStringLiteral("SELL"))
            .arg(QString::number(quantity, 'f', 4))
            .arg(QString::number(price, 'f', 5)),
        2000);

    // Optional UX: when clicking at/through the top-of-book on Lighter perps, the order usually fills immediately
    // but private position updates can arrive with a short delay. When enabled, show an optimistic position overlay
    // and let the true WS position update override it.
    const bool allowOptimisticPositionHint =
        qEnvironmentVariableIntValue("DOM_OPTIMISTIC_POSITION_HINT") > 0;
    if (allowOptimisticPositionHint && lighterPerp && column->dom) {
        const double bestAsk = column->dom->bestAsk();
        const double bestBid = column->dom->bestBid();
        const double tick = std::max(1e-12, column->dom->tickSize());
        const double tol = tick * 0.25;
        const bool marketable =
            (side == OrderSide::Buy && bestAsk > 0.0 && price >= bestAsk - tol)
            || (side == OrderSide::Sell && bestBid > 0.0 && price <= bestBid + tol);
        if (marketable) {
            const TradePosition live = m_tradeManager->positionForSymbol(column->symbol, column->accountName);
            const bool hasLive =
                live.hasPosition && live.quantity > 0.0 && live.averagePrice > 0.0;
            const bool hasCached =
                column->hasCachedPosition && column->cachedPosition.hasPosition
                && column->cachedPosition.quantity > 0.0 && column->cachedPosition.averagePrice > 0.0;
            if (!hasLive && !hasCached) {
                TradePosition hint;
                hint.hasPosition = true;
                hint.side = side;
                hint.quantity = quantity;
                hint.averagePrice = price;
                hint.qtyMultiplier = 1.0;
                column->cachedPosition = hint;
                column->hasCachedPosition = true;
                column->dom->setTradePosition(hint);
                updatePositionOverlay(*column, hint);

                const QString sym = column->symbol;
                const QString accountName = column->accountName;
                QPointer<QWidget> columnGuard = column->container;
                QTimer::singleShot(2500, this, [this, columnGuard, sym, accountName]() {
                    if (!columnGuard || !m_tradeManager) {
                        return;
                    }
                    WorkspaceTab *tab = nullptr;
                    DomColumn *col = nullptr;
                    int idx = -1;
                    if (!locateColumn(columnGuard.data(), tab, col, idx) || !col) {
                        return;
                    }
                    const TradePosition livePos = m_tradeManager->positionForSymbol(sym, accountName);
                    const bool hasLivePos =
                        livePos.hasPosition && livePos.quantity > 0.0 && livePos.averagePrice > 0.0;
                    if (hasLivePos) {
                        return;
                    }
                    // If we still have no confirmed position, clear the optimistic hint.
                    TradePosition empty;
                    col->cachedPosition = empty;
                    col->hasCachedPosition = true;
                    if (col->dom) {
                        col->dom->setTradePosition(empty);
                    }
                    updatePositionOverlay(*col, empty);
                });
            }
        }
    }
}

void MainWindow::handlePrintsCancelRequested(const QStringList &orderIds,
                                             const QString &label,
                                             double price,
                                             bool buy)
{
    Q_UNUSED(buy);
    if (!m_tradeManager) {
        return;
    }
    auto *prints = qobject_cast<PrintsWidget *>(sender());
    if (!prints) {
        return;
    }
    DomColumn *column = nullptr;
    for (auto &tab : m_tabs) {
        for (auto &col : tab.columnsData) {
            if (col.prints == prints) {
                column = &col;
                break;
            }
        }
        if (column) {
            break;
        }
    }
    if (!column) {
        return;
    }
    const QString sym = column->symbol;
    const QString account = column->accountName;
    const QString labelTrim = label.trimmed();

    // SL/TP markers are local UI state; clear them immediately so the marker disappears.
    if (labelTrim.compare(QStringLiteral("SL"), Qt::CaseInsensitive) == 0) {
        column->sltpHasSl = false;
        column->sltpSlPrice = 0.0;
        column->sltpSlCreatedMs = 0;
        column->sltpSlMissCount = 0;
    } else if (labelTrim.compare(QStringLiteral("TP"), Qt::CaseInsensitive) == 0) {
        column->sltpHasTp = false;
        column->sltpTpPrice = 0.0;
        column->sltpTpCreatedMs = 0;
        column->sltpTpMissCount = 0;
    }
    if (labelTrim.compare(QStringLiteral("SL"), Qt::CaseInsensitive) == 0
        || labelTrim.compare(QStringLiteral("TP"), Qt::CaseInsensitive) == 0) {
        refreshSltpMarkers(*column);
        refreshColumnsForSymbol(sym);
    }

    if (!orderIds.isEmpty()) {
        for (const auto &id : orderIds) {
            m_tradeManager->cancelOrder(sym, account, id);
            const QString symUpper = normalizedSymbolKey(sym);
            const QString accountKey = normalizedAccountKey(account);
            removeMarkerDelayTimer(markerTimerKey(symUpper, accountKey, id), nullptr);
        }
        statusBar()->showMessage(tr("Cancel requested (%1)").arg(orderIds.join(QStringLiteral(","))), 1800);
        return;
    }

    // Fallback: if we don't know the order ids (e.g. SL/TP), cancel-all for this symbol.
    if (price > 0.0) {
        statusBar()->showMessage(tr("Cancel requested @ %1").arg(QString::number(price, 'f', 6)), 1800);
    }
    markPendingCancelForSymbol(sym);
    m_tradeManager->cancelAllOrders(sym, account);
}

void MainWindow::handlePrintsMoveBeginRequested(const QString &orderId)
{
    if (!m_tradeManager) {
        return;
    }
    auto *prints = qobject_cast<PrintsWidget *>(sender());
    if (!prints) {
        return;
    }
    DomColumn *column = nullptr;
    for (auto &tab : m_tabs) {
        for (auto &col : tab.columnsData) {
            if (col.prints == prints) {
                column = &col;
                break;
            }
        }
        if (column) {
            break;
        }
    }
    if (!column) {
        return;
    }
    const QString sym = column->symbol;
    const QString account = column->accountName;
    const QString id = orderId.trimmed();
    if (id.isEmpty()) {
        return;
    }

    logMarkerEvent(QStringLiteral("[Move] begin acc=%1 sym=%2 id=%3")
                       .arg(account, sym, id));

    const QString cacheKey =
        QStringLiteral("%1|%2|%3")
            .arg(account.trimmed().toLower(), sym.trimmed().toUpper(), id);
    for (const auto &m : column->localOrders) {
        if (m.orderId == id) {
            m_moveMarkerCache.insert(cacheKey, m);
            break;
        }
    }

    // Cancel immediately (drag ghost keeps UI visible).
    m_tradeManager->cancelOrder(sym, account, id);
    statusBar()->showMessage(tr("Move: canceled %1").arg(id), 1200);
}

void MainWindow::handlePrintsMoveCommitRequested(const QString &orderId, double newPrice, double fallbackPrice)
{
    if (!m_tradeManager) {
        return;
    }
    auto *prints = qobject_cast<PrintsWidget *>(sender());
    if (!prints) {
        return;
    }
    DomColumn *column = nullptr;
    for (auto &tab : m_tabs) {
        for (auto &col : tab.columnsData) {
            if (col.prints == prints) {
                column = &col;
                break;
            }
        }
        if (column) {
            break;
        }
    }
    if (!column) {
        return;
    }
    const QString id = orderId.trimmed();
    if (id.isEmpty()) {
        return;
    }
    const double targetPrice = (newPrice > 0.0 ? newPrice : fallbackPrice);
    if (targetPrice <= 0.0) {
        logMarkerEvent(QStringLiteral("[Move] commit invalid price acc=%1 sym=%2 id=%3 new=%4 fallback=%5")
                           .arg(column->accountName,
                                column->symbol,
                                id,
                                QString::number(newPrice, 'f', 8),
                                QString::number(fallbackPrice, 'f', 8)));
        return;
    }

    const QString sym = column->symbol;
    const QString account = column->accountName;
    const QString cacheKey =
        QStringLiteral("%1|%2|%3")
            .arg(account.trimmed().toLower(), sym.trimmed().toUpper(), id);

    // Find the original marker snapshot to reuse side + quantity (the live marker might already be removed).
    DomWidget::LocalOrderMarker src;
    bool found = false;
    auto it = m_moveMarkerCache.constFind(cacheKey);
    if (it != m_moveMarkerCache.constEnd()) {
        src = *it;
        found = true;
    } else {
        for (const auto &m : column->localOrders) {
            if (m.orderId == id) {
                src = m;
                found = true;
                break;
            }
        }
    }

    if (!found) {
        logMarkerEvent(QStringLiteral("[Move] commit can't resolve acc=%1 sym=%2 id=%3 target=%4")
                           .arg(account,
                                sym,
                                id,
                                QString::number(targetPrice, 'f', 8)));
        statusBar()->showMessage(tr("Move failed: can't resolve order %1").arg(id), 2200);
        return;
    }

    const OrderSide side = src.side;
    // LocalOrderMarker::quantity is stored as quote notional (e.g. USDC),
    // while TradeManager::placeLimitOrder expects base quantity.
    const double notional = src.quantity;
    if (notional <= 0.0) {
        logMarkerEvent(QStringLiteral("[Move] commit invalid notional acc=%1 sym=%2 id=%3 target=%4 notional=%5")
                           .arg(account,
                                sym,
                                id,
                                QString::number(targetPrice, 'f', 8),
                                QString::number(notional, 'f', 8)));
        statusBar()->showMessage(tr("Move failed: invalid notional for %1").arg(id), 2200);
        return;
    }

    const double qtyBase = std::abs(notional / std::max(1e-12, targetPrice));
    if (!(qtyBase > 0.0)) {
        logMarkerEvent(QStringLiteral("[Move] commit invalid baseQty acc=%1 sym=%2 id=%3 target=%4 notional=%5 base=%6")
                           .arg(account,
                                sym,
                                id,
                                QString::number(targetPrice, 'f', 8),
                                QString::number(notional, 'f', 8),
                                QString::number(qtyBase, 'f', 12)));
        statusBar()->showMessage(tr("Move failed: invalid size for %1").arg(id), 2200);
        return;
    }

    logMarkerEvent(QStringLiteral("[Move] commit acc=%1 sym=%2 id=%3 side=%4 qty=%5 target=%6")
                       .arg(account,
                            sym,
                            id,
                            side == OrderSide::Buy ? QStringLiteral("BUY") : QStringLiteral("SELL"),
                            QString::number(notional, 'f', 8),
                            QString::number(targetPrice, 'f', 8)));

    m_tradeManager->placeLimitOrder(sym, account, targetPrice, qtyBase, side, column->leverage);
    m_moveMarkerCache.remove(cacheKey);
    column->dragPreviewActive = false;
    column->dragPreviewPrice = 0.0;
    applyDomHighlightPrices(*column);
    statusBar()->showMessage(tr("Move: placed @ %1").arg(QString::number(targetPrice, 'f', 6)), 1800);
}

void MainWindow::handlePrintsSltpMoveBeginRequested(const QString &kind, double originPrice)
{
    Q_UNUSED(originPrice);
    if (!m_tradeManager) {
        return;
    }
    auto *prints = qobject_cast<PrintsWidget *>(sender());
    if (!prints) {
        return;
    }
    DomColumn *column = nullptr;
    for (auto &tab : m_tabs) {
        for (auto &col : tab.columnsData) {
            if (col.prints == prints) {
                column = &col;
                break;
            }
        }
        if (column) {
            break;
        }
    }
    if (!column) {
        return;
    }

    const QString k = kind.trimmed().toUpper();
    const bool isSl = (k == QStringLiteral("SL"));
    const bool isTp = (k == QStringLiteral("TP"));
    if (!isSl && !isTp) {
        return;
    }

    const QString sym = column->symbol;
    const QString account = column->accountName;

    logMarkerEvent(QStringLiteral("[SLTP Move] begin acc=%1 sym=%2 kind=%3")
                       .arg(account, sym, k));

    // Clear local UI state immediately (drag ghost keeps marker visible).
    if (isSl) {
        column->sltpHasSl = false;
        column->sltpSlPrice = 0.0;
        column->sltpSlCreatedMs = 0;
        column->sltpSlMissCount = 0;
    } else {
        column->sltpHasTp = false;
        column->sltpTpPrice = 0.0;
        column->sltpTpCreatedMs = 0;
        column->sltpTpMissCount = 0;
    }
    refreshSltpMarkers(*column);
    refreshColumnsForSymbol(sym);

    column->dragPreviewActive = true;
    column->dragPreviewPrice = originPrice > 0.0 ? originPrice : 0.0;
    applyDomHighlightPrices(*column);

    // Best-effort cancel of existing stop orders of this type so it can't trigger while dragging.
    m_tradeManager->cancelLighterStopOrders(sym, account, isSl);
    statusBar()->showMessage(tr("Move: canceled %1").arg(k), 1200);
}

void MainWindow::handlePrintsSltpMoveCommitRequested(const QString &kind, double newPrice, double fallbackPrice)
{
    if (!m_tradeManager) {
        return;
    }
    auto *prints = qobject_cast<PrintsWidget *>(sender());
    if (!prints) {
        return;
    }
    DomColumn *column = nullptr;
    for (auto &tab : m_tabs) {
        for (auto &col : tab.columnsData) {
            if (col.prints == prints) {
                column = &col;
                break;
            }
        }
        if (column) {
            break;
        }
    }
    if (!column) {
        return;
    }

    const QString k = kind.trimmed().toUpper();
    const bool isSl = (k == QStringLiteral("SL"));
    const bool isTp = (k == QStringLiteral("TP"));
    if (!isSl && !isTp) {
        return;
    }

    const double targetPrice = (newPrice > 0.0 ? newPrice : fallbackPrice);
    if (!(targetPrice > 0.0)) {
        return;
    }

    const SymbolSource src = symbolSourceForAccount(column->accountName);
    const bool lighterPerp = (src == SymbolSource::Lighter) && !column->symbol.contains(QLatin1Char('/'));
    if (!lighterPerp) {
        statusBar()->showMessage(tr("SL/TP move is available for Lighter Perp only"), 2200);
        return;
    }
    const TradePosition pos = m_tradeManager->positionForSymbol(column->symbol, column->accountName);
    const bool hasPosition =
        pos.hasPosition && pos.quantity > 0.0 && pos.averagePrice > 0.0;
    if (!hasPosition) {
        statusBar()->showMessage(tr("No active position for SL/TP"), 1800);
        return;
    }

    auto triggersImmediately = [&](double price, QString *whyOut) -> bool {
        double bestBid = 0.0;
        double bestAsk = 0.0;
        if (column->dom) {
            bestBid = column->dom->bestBid();
            bestAsk = column->dom->bestAsk();
        }
        const double tick = (column->dom ? std::max(1e-12, column->dom->tickSize()) : 1e-12);
        const double eps = tick * 0.25;
        bool triggersNow = false;
        QString triggerWhy;
        if (pos.side == OrderSide::Buy) {
            if (isSl) {
                triggersNow = (bestAsk > 0.0 && bestAsk <= price + eps);
                triggerWhy = QStringLiteral("ask");
            } else {
                triggersNow = (bestBid > 0.0 && bestBid >= price - eps);
                triggerWhy = QStringLiteral("bid");
            }
        } else {
            if (isSl) {
                triggersNow = (bestBid > 0.0 && bestBid >= price - eps);
                triggerWhy = QStringLiteral("bid");
            } else {
                triggersNow = (bestAsk > 0.0 && bestAsk <= price + eps);
                triggerWhy = QStringLiteral("ask");
            }
        }
        if (whyOut) {
            *whyOut = triggerWhy;
        }
        return triggersNow;
    };

    auto tryPlace = [&](double price) -> bool {
        QString why;
        if (triggersImmediately(price, &why)) {
            statusBar()->showMessage(
                tr("%1 @ %2 would trigger immediately (current %3 already crossed).")
                    .arg(isSl ? QStringLiteral("SL") : QStringLiteral("TP"))
                    .arg(QString::number(price, 'f', 6))
                    .arg(why),
                4500);
            return false;
        }
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        column->sltpLastPlaceMs = nowMs;
        column->sltpLastPlaceWasSl = isSl;
        column->sltpLastPlacePrice = price;
        if (isSl) {
            column->sltpHasSl = true;
            column->sltpSlPrice = price;
            column->sltpSlCreatedMs = nowMs;
            column->sltpSlMissCount = 0;
        } else {
            column->sltpHasTp = true;
            column->sltpTpPrice = price;
            column->sltpTpCreatedMs = nowMs;
            column->sltpTpMissCount = 0;
        }
        refreshSltpMarkers(*column);
        refreshColumnsForSymbol(column->symbol);
        m_tradeManager->placeLighterStopOrder(column->symbol, column->accountName, price, isSl);
        column->dragPreviewActive = false;
        column->dragPreviewPrice = 0.0;
        applyDomHighlightPrices(*column);
        statusBar()->showMessage(
            tr("Move: %1 set @ %2")
                .arg(isSl ? QStringLiteral("SL") : QStringLiteral("TP"))
                .arg(QString::number(price, 'f', 6)),
            2000);
        return true;
    };

    if (tryPlace(targetPrice)) {
        return;
    }
    // If the new level is invalid (e.g. would trigger now), fall back to the original price so the user
    // doesn't accidentally delete SL/TP by dragging too close.
    if (fallbackPrice > 0.0 && std::abs(fallbackPrice - targetPrice) > 1e-12) {
        tryPlace(fallbackPrice);
    }
}

void MainWindow::handlePrintsDragPreviewPriceRequested(double price)
{
    auto *prints = qobject_cast<PrintsWidget *>(sender());
    if (!prints) {
        return;
    }
    DomColumn *column = nullptr;
    for (auto &tab : m_tabs) {
        for (auto &col : tab.columnsData) {
            if (col.prints == prints) {
                column = &col;
                break;
            }
        }
        if (column) {
            break;
        }
    }
    if (!column || !column->dom) {
        return;
    }

    if (price > 0.0) {
        column->dragPreviewActive = true;
        column->dragPreviewPrice = price;
    } else {
        column->dragPreviewActive = false;
        column->dragPreviewPrice = 0.0;
    }
    applyDomHighlightPrices(*column);
}

void MainWindow::handlePositionChanged(const QString &accountName,
                                       const QString &symbol,
                                       const TradePosition &position)
{
    const QString symUpper = symbol.toUpper();
    const QString accountLower = accountName.trimmed().toLower();
    const bool active =
        position.hasPosition && position.quantity > 0.0 && position.averagePrice > 0.0;
    const bool isLighter = (symbolSourceForAccount(accountName) == SymbolSource::Lighter);
    bool playedSound = false;
    for (auto &tab : m_tabs) {
        for (auto &col : tab.columnsData) {
            if (col.symbol.compare(symUpper, Qt::CaseInsensitive) != 0) {
                continue;
            }
            if (!accountLower.isEmpty()
                && col.accountName.trimmed().toLower() != accountLower) {
                continue;
            }
            const TradePosition prev = col.cachedPosition;
            const bool wasActive = col.hasCachedPosition
                                   && prev.hasPosition
                                   && prev.quantity > 0.0
                                   && prev.averagePrice > 0.0;
            col.cachedPosition = position;
            col.hasCachedPosition = true;
            if (col.dom) {
                col.dom->setTradePosition(position);
            }
            updatePositionOverlay(col, position);
            if (isLighter && !playedSound && col.hasCachedPosition) {
                bool shouldPlay = false;
                if (!wasActive && active) {
                    // Position opened.
                    shouldPlay = true;
                } else if (wasActive && !active) {
                    // Position closed.
                    shouldPlay = true;
                } else if (wasActive && active) {
                    // Filled/updated while staying in position.
                    const double dq = std::abs(prev.quantity - position.quantity);
                    const double dp = std::abs(prev.averagePrice - position.averagePrice);
                    if (dq > 1e-12 || dp > 1e-12 || prev.side != position.side) {
                        shouldPlay = true;
                    }
                }

                if (shouldPlay) {
                    ensureSoundsInitialized();
#if HAS_QSOUNDEFFECT
                    if (m_successEffect) {
                        restartSound(m_successEffect);
                        playedSound = true;
                    }
#endif
#if HAS_QMEDIAPLAYER && HAS_QAUDIOOUTPUT
                    if (!playedSound && m_successPlayer && m_successOutput) {
                        restartSound(m_successPlayer);
                        playedSound = true;
                    }
#endif
                }
            }
            if (!active) {
                clearSltpMarkers(col);
            }
        }
    }
}

void MainWindow::updatePositionOverlay(DomColumn &col, const TradePosition &position)
{
    if (!col.positionOverlay || !col.positionAvgLabel || !col.positionValueLabel || !col.positionPnlLabel) {
        return;
    }
    const bool active =
        position.hasPosition && position.quantity > 0.0 && position.averagePrice > 0.0;
    if (!active) {
        col.positionOverlay->hide();
        col.lastOverlayPnlSign = 0;
        col.lastOverlayValueText.clear();
        col.lastOverlayPctText.clear();
        col.lastOverlayPnlText.clear();
        if (col.positionPctLabel) {
            col.positionPctLabel->clear();
            col.positionPctLabel->hide();
        }
        return;
    }

    double ref = 0.0;
    if (col.dom) {
        ref = (position.side == OrderSide::Buy) ? col.dom->bestBid() : col.dom->bestAsk();
    }
    if (!(ref > 0.0)) {
        ref = position.averagePrice;
    }

    const double entry = position.averagePrice;
    const double qtyUnits = position.quantity * std::max(1e-12, position.qtyMultiplier);
    const bool isLong = (position.side == OrderSide::Buy);
    const double entryNotionalUsd = std::abs(entry * qtyUnits);
    const double signedNotional = isLong ? entryNotionalUsd : -entryNotionalUsd;
    const double pnlUsd = isLong ? ((ref - entry) * qtyUnits) : ((entry - ref) * qtyUnits);

    auto fmtTrim = [](double v, int decimals) -> QString {
        if (!std::isfinite(v)) return QStringLiteral("0");
        const int dec = std::clamp(decimals, 0, 8);
        QString s = QString::number(v, 'f', dec);
        while (s.contains(QLatin1Char('.')) && s.endsWith(QLatin1Char('0'))) s.chop(1);
        if (s.endsWith(QLatin1Char('.'))) s.chop(1);
        if (s == QStringLiteral("-0")) s = QStringLiteral("0");
        return s;
    };
    auto tickPrecision = [](double tick) -> int {
        if (!(tick > 0.0) || !std::isfinite(tick)) {
            return 2;
        }
        // Determine decimals for tick size (e.g. 0.01 -> 2). Cap to avoid runaway on weird floats.
        double t = tick;
        int prec = 0;
        for (; prec < 8; ++prec) {
            const double rounded = std::round(t);
            if (std::abs(t - rounded) < 1e-9) {
                break;
            }
            t *= 10.0;
        }
        return prec;
    };
    const int tickPrec =
        (col.dom && col.dom->tickSize() > 0.0) ? tickPrecision(col.dom->tickSize()) : 2;
    // User requested: don't round entry price; keep full tick precision (up to our cap).
    const int pricePrec = std::clamp(tickPrec, 0, 8);

    const QString entryText = fmtTrim(entry, pricePrec);
    const QString valueText = fmtTrim(signedNotional, 2) + QStringLiteral("$");
    const QString pnlText = fmtTrim(pnlUsd, 2) + QStringLiteral("$");

    col.positionAvgLabel->setText(entryText);
    col.positionValueLabel->setText(valueText);
    col.positionPnlLabel->setText(pnlText);

    const QColor neutralBg("#0b0b0b");
    const QColor neutralBorder("#1a1a1a");
    const QColor longBg("#1e5b38");
    const QColor longBorder("#2f6c37");
    const QColor shortBg("#6a1b1b");
    const QColor shortBorder("#992626");
    const QColor pnlBg = (pnlUsd >= 0.0) ? longBg : shortBg;
    const QColor pnlBorder = (pnlUsd >= 0.0) ? longBorder : shortBorder;

    col.positionAvgLabel->setStyleSheet(
        QStringLiteral("QLabel { color:#ffffff; font-weight:800; padding:3px 6px; border-radius:0px; border:none; background-color: rgba(%1,%2,%3,235); }")
            .arg(neutralBg.red())
            .arg(neutralBg.green())
            .arg(neutralBg.blue()));
    const QColor sideBg = isLong ? longBg : shortBg;
    col.positionValueLabel->setStyleSheet(
        QStringLiteral("QLabel { color:#ffffff; font-weight:800; padding:3px 6px; border-radius:0px; border:none; background-color: rgba(%1,%2,%3,235); }")
            .arg(sideBg.red())
            .arg(sideBg.green())
            .arg(sideBg.blue()));
    col.positionPnlLabel->setStyleSheet(
        QStringLiteral("QLabel { color:#ffffff; font-weight:800; padding:3px 6px; border-radius:0px; border:none; background-color: rgba(%1,%2,%3,235); }")
            .arg(pnlBg.red())
            .arg(pnlBg.green())
            .arg(pnlBg.blue()));

    col.positionOverlay->show();
    col.positionOverlay->raise();
}

void MainWindow::retargetDomColumn(DomColumn &col, const QString &symbol)
{
    QString sym = symbol.trimmed().toUpper();
    if (sym.isEmpty()) {
        // Non-blocking picker to avoid freezing the main window while selecting.
        SymbolPickerDialog *picker = createSymbolPicker(tr("Select symbol"),
                                                        col.symbol,
                                                        col.accountName.isEmpty() ? QStringLiteral("MEXC Spot")
                                                                                  : col.accountName);
        if (!picker) {
            return;
        }
        QPointer<QWidget> container = col.container;
        connect(picker, &QDialog::accepted, this, [this, picker, container]() {
            if (!container) {
                picker->deleteLater();
                return;
            }
            WorkspaceTab *tab = nullptr;
            DomColumn *colPtr = nullptr;
            int splitIndex = -1;
            if (!locateColumn(container, tab, colPtr, splitIndex) || !colPtr) {
                picker->deleteLater();
                return;
            }
            applySymbolToColumn(*colPtr, picker->selectedSymbol(), picker->selectedAccount());
            picker->deleteLater();
            saveUserSettings();
        });
        connect(picker, &QDialog::rejected, picker, &QObject::deleteLater);
        picker->open();
        picker->raise();
        picker->activateWindow();
        return;
    }

    if (sym == col.symbol) {
        return;
    }

    applySymbolToColumn(col, sym, col.accountName);
    saveUserSettings();
}

void MainWindow::applyNotionalPreset(int presetIndex)
{
    if (presetIndex < 0) {
        return;
    }
    DomColumn *column = focusedDomColumn();
    if (!column) {
        if (auto *tab = currentWorkspaceTab()) {
            if (!tab->columnsData.isEmpty()) {
                column = &tab->columnsData.front();
            }
        }
    }
    if (!column) {
        return;
    }
    if (presetIndex >= static_cast<int>(column->notionalValues.size())) {
        return;
    }

    column->orderNotional = column->notionalValues[presetIndex];
    if (column->notionalGroup) {
        if (auto *btn = column->notionalGroup->button(presetIndex)) {
            btn->setChecked(true);
        }
    }

    statusBar()->showMessage(
        tr("Size preset set to %1").arg(column->orderNotional, 0, 'g', 6),
        800);
}

void MainWindow::applyLeverageForColumn(QWidget *columnContainer, int leverage)
{
    if (!columnContainer) {
        return;
    }
    WorkspaceTab *tab = nullptr;
    DomColumn *col = nullptr;
    int splitIndex = -1;
    if (!locateColumn(columnContainer, tab, col, splitIndex) || !col) {
        return;
    }
    const auto src = symbolSourceForAccount(col->accountName);
    const bool lighterPerp = (src == SymbolSource::Lighter) && !col->symbol.contains(QLatin1Char('/'));
    if (src != SymbolSource::MexcFutures && src != SymbolSource::UzxSwap && !lighterPerp) {
        return;
    }
    const QString sym = col->symbol.toUpper();
    if (lighterPerp && !m_lighterMaxLeverageBySymbol.contains(sym)) {
        requestLighterLeverageLimit(sym);
        statusBar()->showMessage(tr("Loading Lighter leverage limits..."), 1200);
        return;
    }
    if (src == SymbolSource::UzxSwap && !m_uzxSwapMaxLeverageBySymbol.contains(sym) && m_uzxSwapSymbols.isEmpty()) {
        fetchSymbolLibrary(SymbolSource::UzxSwap, nullptr);
        statusBar()->showMessage(tr("Loading UZX leverage limits..."), 1200);
    }
    const int maxLev = (src == SymbolSource::MexcFutures)
                           ? std::max(1, m_mexcFuturesMaxLeverageBySymbol.value(sym, 200))
                           : (src == SymbolSource::UzxSwap)
                               ? std::max(1, m_uzxSwapMaxLeverageBySymbol.value(sym, 125))
                               : std::max(1, m_lighterMaxLeverageBySymbol.value(sym, 10));
    const int clamped = std::clamp(leverage, 1, maxLev);
    col->leverage = clamped;
    m_futuresLeverageBySymbol.insert(sym, clamped);
    if (col->leverageButton) {
        col->leverageButton->setText(QStringLiteral("%1x").arg(clamped));
    }
    saveUserSettings();
    statusBar()->showMessage(tr("Leverage set to %1x").arg(clamped), 1200);
}

void MainWindow::requestLighterLeverageLimit(const QString &symbolUpper)
{
    const QString sym = symbolUpper.trimmed().toUpper();
    if (sym.isEmpty() || sym.contains(QLatin1Char('/'))) {
        return;
    }
    if (m_lighterMaxLeverageBySymbol.contains(sym) || m_lighterLeverageInFlight.contains(sym)) {
        return;
    }
    m_lighterLeverageInFlight.insert(sym);

    QString baseUrl = QStringLiteral("https://mainnet.zklighter.elliot.ai");
    // DefaultProxy lets Qt use system proxy settings (TradeManager enables them globally).
    QNetworkProxy proxy(QNetworkProxy::DefaultProxy);
    if (m_connectionStore) {
        const MexcCredentials creds = m_connectionStore->loadMexcCredentials(ConnectionStore::Profile::Lighter);
        const QString candidate = creds.baseUrl.trimmed();
        if (!candidate.isEmpty()) {
            baseUrl = candidate;
        }
        QString proxyType = creds.proxyType.trimmed().toLower();
        if (proxyType == QStringLiteral("https")) {
            proxyType = QStringLiteral("http");
        }
        const QString proxyRaw = creds.proxy.trimmed();
        if (proxyRaw.compare(QStringLiteral("disabled"), Qt::CaseInsensitive) == 0
            || proxyRaw.compare(QStringLiteral("none"), Qt::CaseInsensitive) == 0
            || proxyRaw.compare(QStringLiteral("direct"), Qt::CaseInsensitive) == 0) {
            proxy = QNetworkProxy(QNetworkProxy::NoProxy);
        } else if (!proxyRaw.isEmpty()) {
            // Formats supported: host:port, user:pass@host:port, host:port:user:pass, user:pass:host:port
            QString host;
            QString portStr;
            QString user;
            QString pass;
            const QString trimmed = proxyRaw.trimmed();
            const QStringList atSplit = trimmed.split('@');
            if (atSplit.size() == 2) {
                const QStringList cp = atSplit.at(0).split(':');
                const QStringList hp = atSplit.at(1).split(':');
                if (cp.size() >= 2 && hp.size() == 2) {
                    user = cp.at(0);
                    pass = cp.at(1);
                    host = hp.at(0);
                    portStr = hp.at(1);
                }
            } else {
                const QStringList parts = trimmed.split(':');
                if (parts.size() == 2) {
                    host = parts.at(0);
                    portStr = parts.at(1);
                } else if (parts.size() == 4) {
                    host = parts.at(0);
                    portStr = parts.at(1);
                    user = parts.at(2);
                    pass = parts.at(3);
                    bool okPort = false;
                    portStr.toInt(&okPort);
                    if (!okPort) {
                        // host:user:pass:port
                        host = parts.at(0);
                        user = parts.at(1);
                        pass = parts.at(2);
                        portStr = parts.at(3);
                    }
                }
            }
            bool okPort = false;
            const int port = portStr.toInt(&okPort);
            if (!host.isEmpty() && okPort && port > 0) {
                const auto ptype =
                    (proxyType == QStringLiteral("socks5")) ? QNetworkProxy::Socks5Proxy : QNetworkProxy::HttpProxy;
                proxy = QNetworkProxy(ptype, host, port, user, pass);
            }
        }
    }
    while (baseUrl.endsWith(QLatin1Char('/'))) {
        baseUrl.chop(1);
    }

    auto *nam = new QNetworkAccessManager(this);
    nam->setProxy(proxy);

    auto done = [this, nam, sym]() {
        m_lighterLeverageInFlight.remove(sym);
        nam->deleteLater();
    };

    auto reportFail = [this, sym, done](const QString &msg) {
        done();
        qWarning() << msg;
        logLadderStatus(msg);
        addNotification(msg);
    };

    auto parseApiErr = [](const QByteArray &raw, int *outCode, QString *outMsg) {
        if (outCode) *outCode = 0;
        if (outMsg) outMsg->clear();
        if (raw.isEmpty()) return;
        const QJsonDocument doc = QJsonDocument::fromJson(raw);
        if (!doc.isObject()) return;
        const QJsonObject obj = doc.object();
        if (outCode) *outCode = obj.value(QStringLiteral("code")).toInt(0);
        if (outMsg) *outMsg = obj.value(QStringLiteral("message")).toString();
    };

    auto toInt = [](const QJsonValue &v) -> int {
        if (v.isDouble()) return static_cast<int>(v.toDouble());
        if (v.isString()) {
            bool ok = false;
            const int out = v.toString().trimmed().toInt(&ok);
            return ok ? out : -1;
        }
        return v.toInt(-1);
    };

    std::function<void(const QString &paramName, bool allowRetry)> fetchOrderBooks;
    fetchOrderBooks = [this, nam, baseUrl, sym, done, reportFail, parseApiErr, toInt, &fetchOrderBooks](const QString &paramName, bool allowRetry) {
        QUrl url(baseUrl + QStringLiteral("/api/v1/orderBooks"));
        QUrlQuery q;
        q.addQueryItem(paramName, QStringLiteral("255"));
        url.setQuery(q);
        QNetworkRequest req(url);
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        req.setRawHeader("User-Agent", "FusionTerminal/1.0");
        auto *reply = nam->get(req);
        auto *t = new QTimer(reply);
        t->setSingleShot(true);
        t->setInterval(15000);
        connect(t, &QTimer::timeout, reply, [reply]() {
            if (reply && !reply->isFinished()) {
                reply->setProperty("fusion_timeout", true);
                reply->abort();
            }
        });
        t->start();

        connect(reply, &QNetworkReply::finished, this, [this, reply, nam, baseUrl, sym, paramName, allowRetry, done, reportFail, parseApiErr, toInt, &fetchOrderBooks]() {
            const auto err = reply->error();
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QString errStr = reply->errorString();
            const QByteArray raw = reply->readAll();
            const bool timedOut = reply->property("fusion_timeout").toBool();
            reply->deleteLater();

            int apiCode = 0;
            QString apiMsg;
            parseApiErr(raw, &apiCode, &apiMsg);
            const bool invalidParam =
                (apiCode == 20001 || apiMsg.contains(QStringLiteral("invalid param"), Qt::CaseInsensitive));
            if (allowRetry && invalidParam) {
                const QString otherParam =
                    (paramName == QStringLiteral("market_id")) ? QStringLiteral("market_index")
                                                                : QStringLiteral("market_id");
                fetchOrderBooks(otherParam, false);
                return;
            }

            if ((timedOut && err == QNetworkReply::OperationCanceledError) || err != QNetworkReply::NoError || status >= 400) {
                reportFail(QStringLiteral("[limits] Lighter leverage lookup failed for %1: %2")
                               .arg(sym, raw.isEmpty() ? errStr : QString::fromUtf8(raw).left(220)));
                return;
            }

            const QJsonDocument doc = QJsonDocument::fromJson(raw);
            if (doc.isNull() || !doc.isObject()) {
                reportFail(QStringLiteral("[limits] Lighter leverage lookup failed for %1: invalid JSON").arg(sym));
                return;
            }
            const QJsonObject obj = doc.object();
            if (obj.value(QStringLiteral("code")).toInt(0) != 200) {
                reportFail(QStringLiteral("[limits] Lighter leverage lookup failed for %1: %2")
                               .arg(sym, obj.value(QStringLiteral("message")).toString(QStringLiteral("unknown"))));
                return;
            }

            int marketId = -1;
            QJsonValue booksVal = obj.value(QStringLiteral("order_books"));
            if (!booksVal.isArray()) {
                booksVal = obj.value(QStringLiteral("orderBooks"));
            }
            const QJsonArray books = booksVal.toArray();
            for (const auto &v : books) {
                const QJsonObject o = v.toObject();
                const QString s = o.value(QStringLiteral("symbol")).toString().trimmed().toUpper();
                if (s.compare(sym, Qt::CaseInsensitive) == 0) {
                    marketId = toInt(o.value(QStringLiteral("market_id")));
                    if (marketId < 0) marketId = toInt(o.value(QStringLiteral("market_index")));
                    if (marketId < 0) marketId = toInt(o.value(QStringLiteral("marketId")));
                    break;
                }
            }
            if (marketId < 0) {
                reportFail(QStringLiteral("[limits] Lighter leverage lookup failed: market not found for %1").arg(sym));
                return;
            }

            std::function<void(const QString &detailParam, bool allowDetailRetry)> fetchDetails;
            fetchDetails = [this, nam, baseUrl, sym, marketId, done, reportFail, parseApiErr, toInt, &fetchDetails](const QString &detailParam, bool allowDetailRetry) {
                QUrl url(baseUrl + QStringLiteral("/api/v1/orderBookDetails"));
                QUrlQuery q;
                q.addQueryItem(detailParam, QString::number(marketId));
                url.setQuery(q);
                QNetworkRequest req2(url);
                req2.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
                req2.setRawHeader("User-Agent", "FusionTerminal/1.0");
                auto *reply2 = nam->get(req2);
                auto *t2 = new QTimer(reply2);
                t2->setSingleShot(true);
                t2->setInterval(15000);
                connect(t2, &QTimer::timeout, reply2, [reply2]() {
                    if (reply2 && !reply2->isFinished()) {
                        reply2->setProperty("fusion_timeout", true);
                        reply2->abort();
                    }
                });
                t2->start();

                connect(reply2, &QNetworkReply::finished, this, [this, reply2, sym, marketId, detailParam, allowDetailRetry, done, reportFail, parseApiErr, toInt, &fetchDetails]() {
                    const auto err = reply2->error();
                    const int status = reply2->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                    const QString errStr = reply2->errorString();
                    const QByteArray raw = reply2->readAll();
                    const bool timedOut = reply2->property("fusion_timeout").toBool();
                    reply2->deleteLater();

                    int apiCode = 0;
                    QString apiMsg;
                    parseApiErr(raw, &apiCode, &apiMsg);
                    const bool invalidParam =
                        (apiCode == 20001 || apiMsg.contains(QStringLiteral("invalid param"), Qt::CaseInsensitive));
                    if (allowDetailRetry && invalidParam) {
                        const QString otherParam =
                            (detailParam == QStringLiteral("market_id")) ? QStringLiteral("market_index")
                                                                          : QStringLiteral("market_id");
                        fetchDetails(otherParam, false);
                        return;
                    }

                    if ((timedOut && err == QNetworkReply::OperationCanceledError) || err != QNetworkReply::NoError || status >= 400) {
                        reportFail(QStringLiteral("[limits] Lighter leverage lookup failed for %1: %2")
                                       .arg(sym, raw.isEmpty() ? errStr : QString::fromUtf8(raw).left(220)));
                        return;
                    }
                    const QJsonDocument doc = QJsonDocument::fromJson(raw);
                    if (!doc.isObject()) {
                        reportFail(QStringLiteral("[limits] Lighter leverage lookup failed for %1: invalid JSON").arg(sym));
                        return;
                    }
                    const QJsonObject obj = doc.object();
                    if (obj.value(QStringLiteral("code")).toInt(0) != 200) {
                        reportFail(QStringLiteral("[limits] Lighter leverage lookup failed for %1: %2")
                                       .arg(sym, obj.value(QStringLiteral("message")).toString(QStringLiteral("unknown"))));
                        return;
                    }

                    int minImf = 0;
                    const QJsonArray perps = obj.value(QStringLiteral("order_book_details")).toArray();
                    for (const auto &v : perps) {
                        const QJsonObject o = v.toObject();
                        const QString s = o.value(QStringLiteral("symbol")).toString().trimmed().toUpper();
                        if (s.compare(sym, Qt::CaseInsensitive) != 0) {
                            continue;
                        }
                        const QJsonValue vImf = o.value(QStringLiteral("min_initial_margin_fraction"));
                        if (vImf.isDouble()) {
                            minImf = static_cast<int>(vImf.toDouble(0.0));
                        } else if (vImf.isString()) {
                            bool ok = false;
                            minImf = vImf.toString().trimmed().toInt(&ok);
                            if (!ok) minImf = 0;
                        } else {
                            minImf = vImf.toInt(0);
                        }
                        break;
                    }
                    int maxLev = 10;
                    if (minImf > 0) {
                        maxLev = std::max(1, 10000 / minImf);
                    }
                    maxLev = std::clamp(maxLev, 1, 200);
                    m_lighterMaxLeverageBySymbol.insert(sym, maxLev);

                    // Clamp existing columns for this symbol.
                    for (auto &tab : m_tabs) {
                        for (auto &col : tab.columnsData) {
                            const auto src = symbolSourceForAccount(col.accountName);
                            const bool isLighterPerp =
                                (src == SymbolSource::Lighter) && !col.symbol.contains(QLatin1Char('/'));
                            if (!isLighterPerp || col.symbol.compare(sym, Qt::CaseInsensitive) != 0) {
                                continue;
                            }
                            const int desired = std::max(1, m_futuresLeverageBySymbol.value(sym, col.leverage > 0 ? col.leverage : 20));
                            const int clamped = std::clamp(desired, 1, maxLev);
                            col.leverage = clamped;
                            m_futuresLeverageBySymbol.insert(sym, clamped);
                            if (col.leverageButton) {
                                col.leverageButton->setText(QStringLiteral("%1x").arg(clamped));
                            }
                        }
                    }

                    done();
                });
            };

            fetchDetails(QStringLiteral("market_id"), true);
        });
    };

    fetchOrderBooks(QStringLiteral("market_id"), true);
}

void MainWindow::applyCompressionForColumn(QWidget *columnContainer, int compression)
{
    if (!columnContainer) {
        return;
    }
    WorkspaceTab *tab = nullptr;
    DomColumn *col = nullptr;
    int splitIndex = -1;
    if (!locateColumn(columnContainer, tab, col, splitIndex) || !col) {
        return;
    }

    const int clamped = std::max(1, compression);
    if (clamped == col->tickCompression) {
        return;
    }
    col->tickCompression = clamped;
    if (col->compressionButton) {
        col->compressionButton->setText(QStringLiteral("%1x").arg(clamped));
    }
    if (col->client) {
        col->client->setCompression(clamped);
        col->client->resetManualCenter();
        col->backendManualPinned = false;
        restartColumnClient(*col);
    }
    saveUserSettings();
    statusBar()->showMessage(tr("Compression set to %1x").arg(clamped), 1200);
}

void MainWindow::showCompressionMenu(DomColumn &col, const QPoint &globalPos)
{
    QMenu menu;
    menu.setStyleSheet(QStringLiteral("QMenu { background:#1f1f1f; color:#e0e0e0; }"
                                      "QMenu::item:selected { background:#2c2c2c; }"));

    const QList<int> presets = {1, 2, 5, 10, 25, 50, 100, 200};
    QActionGroup *group = new QActionGroup(&menu);
    group->setExclusive(true);

    QWidget *container = col.container;
    for (int v : presets) {
        QAction *a = menu.addAction(QStringLiteral("%1x").arg(v));
        a->setCheckable(true);
        a->setChecked(v == std::max(1, col.tickCompression));
        group->addAction(a);
        connect(a, &QAction::triggered, this, [this, container, v]() {
            applyCompressionForColumn(container, v);
        });
    }
    menu.addSeparator();
    QAction *custom = menu.addAction(tr("Custom..."));
    connect(custom, &QAction::triggered, this, [this, container]() {
        WorkspaceTab *tab = nullptr;
        DomColumn *colPtr = nullptr;
        int idx = -1;
        if (!locateColumn(container, tab, colPtr, idx) || !colPtr) {
            return;
        }
        bool ok = false;
        const int v = QInputDialog::getInt(this,
                                           tr("Compression"),
                                           tr("Set tick compression (x):"),
                                           std::max(1, colPtr->tickCompression),
                                           1,
                                           1000,
                                           1,
                                           &ok);
        if (ok) {
            applyCompressionForColumn(container, v);
        }
    });

    menu.exec(globalPos);
}

void MainWindow::startNotionalEdit(QWidget *columnContainer, int presetIndex)
{
    if (!columnContainer) {
        return;
    }
    WorkspaceTab *tab = nullptr;
    DomColumn *col = nullptr;
    int splitIndex = -1;
    if (!locateColumn(columnContainer, tab, col, splitIndex) || !col) {
        return;
    }
    if (presetIndex < 0 || presetIndex >= static_cast<int>(col->notionalValues.size())) {
        return;
    }

    if (!col->notionalEditOverlay) {
        auto *overlay = new QWidget(columnContainer);
        overlay->setObjectName(QStringLiteral("NotionalEditOverlay"));
        overlay->setStyleSheet(QStringLiteral("background-color: rgba(0,0,0,0.65);"));
        overlay->hide();
        overlay->setProperty(
            "domContainerPtr",
            QVariant::fromValue<quintptr>(reinterpret_cast<quintptr>(columnContainer)));

        auto *overlayLayout = new QVBoxLayout(overlay);
        overlayLayout->setContentsMargins(0, 0, 0, 0);
        overlayLayout->setSpacing(0);
        overlayLayout->addStretch();

        auto *panel = new QWidget(overlay);
        panel->setStyleSheet(QStringLiteral(
            "background-color:#1f1f1f; border:1px solid #4a90e2; border-radius:6px;"));
        panel->setFixedWidth(220);
        auto *panelLayout = new QVBoxLayout(panel);
        panelLayout->setContentsMargins(14, 14, 14, 14);
        panelLayout->setSpacing(8);

        auto *label = new QLabel(tr("Edit preset (USDT)"), panel);
        label->setAlignment(Qt::AlignCenter);
        panelLayout->addWidget(label);

        auto *line = new QLineEdit(panel);
        line->setAlignment(Qt::AlignCenter);
        line->setObjectName(QStringLiteral("NotionalEditField"));
        line->setProperty(
            "domContainerPtr",
            QVariant::fromValue<quintptr>(reinterpret_cast<quintptr>(columnContainer)));
        panelLayout->addWidget(line);

        auto *hint = new QLabel(tr("Enter a value and press Enter"), panel);
        hint->setAlignment(Qt::AlignCenter);
        hint->setStyleSheet(QStringLiteral("color:#bbbbbb; font-size:11px;"));
        panelLayout->addWidget(hint);

        overlayLayout->addWidget(panel, 0, Qt::AlignHCenter | Qt::AlignBottom);
        overlayLayout->addSpacing(30);

        col->notionalEditOverlay = overlay;
        col->notionalEditField = line;
        overlay->installEventFilter(this);
        line->installEventFilter(this);
        connect(line, &QLineEdit::returnPressed, this, [this, columnContainer]() {
            commitNotionalEdit(columnContainer, true);
        });
    }

    col->editingNotionalIndex = presetIndex;
    if (col->notionalEditOverlay) {
        col->notionalEditOverlay->setGeometry(columnContainer->rect());
        col->notionalEditOverlay->show();
        col->notionalEditOverlay->raise();
    }
    if (col->notionalEditField) {
        col->notionalEditField->setText(
            QString::number(col->notionalValues[presetIndex], 'g', 8));
        col->notionalEditField->selectAll();
        col->notionalEditField->setFocus(Qt::OtherFocusReason);
    }
    m_notionalEditActive = true;
}

void MainWindow::commitNotionalEdit(QWidget *columnContainer, bool apply)
{
    if (!columnContainer) {
        return;
    }
    WorkspaceTab *tab = nullptr;
    DomColumn *col = nullptr;
    int splitIndex = -1;
    if (!locateColumn(columnContainer, tab, col, splitIndex) || !col) {
        return;
    }
    if (!col->notionalEditOverlay) {
        return;
    }

    const int index = col->editingNotionalIndex;
    if (apply && col->notionalEditField && index >= 0 &&
        index < static_cast<int>(col->notionalValues.size())) {
        bool ok = false;
        const double value = col->notionalEditField->text().toDouble(&ok);
        if (ok && value > 0.0) {
            col->notionalValues[index] = value;
            if (col->notionalGroup) {
                if (auto *btn = col->notionalGroup->button(index)) {
                    btn->setText(QString::number(value, 'g', 6));
                    if (btn->isChecked()) {
                        col->orderNotional = value;
                    }
                }
            }
            const QString presetKey =
                col->accountName.trimmed().toLower() + QLatin1Char('|') + col->symbol.trimmed().toUpper();
            if (!presetKey.trimmed().isEmpty()) {
                m_notionalPresetsByKey.insert(presetKey, col->notionalValues);
                scheduleSaveUserSettings(250);
            }
        }
    }

    col->editingNotionalIndex = -1;
    col->notionalEditOverlay->hide();
    m_notionalEditActive = false;
}
void MainWindow::updateTimeLabel()
{
    if (!m_timeLabel) {
        return;
    }
    QDateTime now = QDateTime::currentDateTimeUtc();
    if (m_timeOffsetMinutes != 0) {
        now = now.addSecs(m_timeOffsetMinutes * 60);
    }

    const int hoursOffset = m_timeOffsetMinutes / 60;
    QString suffix;
    if (hoursOffset == 0) {
        suffix = QStringLiteral("UTC");
    } else {
        suffix = QStringLiteral("UTC%1%2")
                     .arg(hoursOffset > 0 ? QStringLiteral("+") : QString())
                     .arg(hoursOffset);
    }

    m_timeLabel->setText(
        now.toString(QStringLiteral("HH:mm:ss '") + suffix + QLatin1Char('\'')));
}

void MainWindow::openConnectionsWindow()
{
    if (!m_connectionStore || !m_tradeManager) {
        return;
    }
    if (!m_connectionsWindow) {
        m_connectionsWindow = new ConnectionsWindow(m_connectionStore, m_tradeManager, this);
        if (!m_connectionsLogBacklog.isEmpty()) {
            for (const auto &line : m_connectionsLogBacklog) {
                m_connectionsWindow->appendLogMessage(line);
            }
            m_connectionsLogBacklog.clear();
        } else {
            // If user opens the log view later (or after restart), show the recent file tail too.
            const QString path = fusionLogFilePath();
            QFile f(path);
            if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
                static constexpr qint64 kMaxBytes = 256 * 1024;
                if (f.size() > kMaxBytes) {
                    f.seek(f.size() - kMaxBytes);
                    // Skip partial first line.
                    f.readLine();
                }
                const QByteArray tail = f.readAll();
                const QList<QByteArray> lines = tail.split('\n');
                for (const auto &raw : lines) {
                    const QString line = QString::fromUtf8(raw).trimmed();
                    if (!line.isEmpty()) {
                        m_connectionsWindow->appendLogMessage(line);
                    }
                }
            }
        }
    } else {
        m_connectionsWindow->refreshUi();
    }
    m_connectionsWindow->show();
    m_connectionsWindow->raise();
}

void MainWindow::openTradesWindow()
{
    if (!m_tradeManager) {
        return;
    }
    if (!m_tradesWindow) {
        m_tradesWindow = new TradesWindow(m_tradeManager, this);
    } else {
        m_tradesWindow->refreshUi();
    }
    m_tradesWindow->show();
    m_tradesWindow->raise();
    m_tradesWindow->activateWindow();
}

void MainWindow::openFinrezWindow()
{
    if (!m_tradeManager) {
        return;
    }
    if (!m_finrezWindow) {
        m_finrezWindow = new FinrezWindow(m_tradeManager, this);
    } else {
        m_finrezWindow->refreshUi();
    }
    m_finrezWindow->show();
    m_finrezWindow->raise();
    m_finrezWindow->activateWindow();
}

void MainWindow::handleConnectionStateChanged(ConnectionStore::Profile profile,
                                              TradeManager::ConnectionState state,
                                              const QString &message)
{
    auto profileLabel = [profile]() {
        switch (profile) {
        case ConnectionStore::Profile::MexcFutures:
            return QStringLiteral("MEXC Futures");
        case ConnectionStore::Profile::UzxSwap:
            return QStringLiteral("UZX Swap");
        case ConnectionStore::Profile::UzxSpot:
            return QStringLiteral("UZX Spot");
        case ConnectionStore::Profile::MexcSpot:
        default:
            return QStringLiteral("MEXC Spot");
        }
    };
    if (!message.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("%1: %2").arg(profileLabel(), message), 2500);
    }

    if (state == TradeManager::ConnectionState::Error
        || state == TradeManager::ConnectionState::Disconnected) {
        const QString note = message.isEmpty()
                                 ? tr("%1 connection lost").arg(profileLabel())
                                 : QStringLiteral("%1: %2").arg(profileLabel(), message);
        addNotification(note);
    }
}

void MainWindow::addNotification(const QString &text, bool unread)
{
    static QString lastText;
    static QDateTime lastTime;
    const QDateTime now = QDateTime::currentDateTime();
    if (text == lastText && lastTime.isValid() && lastTime.msecsTo(now) < 3000) {
        return;
    }
    lastText = text;
    lastTime = now;

    NotificationEntry entry;
    entry.text = text;
    entry.timestamp = now;
    entry.read = !unread;
    const int maxStored = 99;
    while (m_notifications.size() >= maxStored) {
        if (!m_notifications.front().read && m_unreadNotifications > 0) {
            --m_unreadNotifications;
        }
        m_notifications.pop_front();
    }
    m_notifications.push_back(entry);
    if (unread) {
        ++m_unreadNotifications;
    }
    refreshAlertsList();
    updateAlertsBadge();
    if (!unread) {
        return;
    }

    ensureSoundsInitialized();
#if HAS_QSOUNDEFFECT
    if (m_notificationEffect) {
        restartSound(m_notificationEffect);
    }
#elif HAS_QMEDIAPLAYER && HAS_QAUDIOOUTPUT
    if (m_notificationPlayer && m_notificationOutput) {
        m_notificationPlayer->stop();
        m_notificationPlayer->setPosition(0);
        m_notificationPlayer->play();
    }
#endif
}

void MainWindow::updateAlertsBadge()
{
    if (!m_alertsBadge || !m_alertsButton) {
        return;
    }

    if (m_unreadNotifications <= 0) {
        m_alertsBadge->hide();
        return;
    }

    const int capped = std::min(m_unreadNotifications, 99);
    const QString text = QString::number(capped);
    m_alertsBadge->setText(text);
    const int badgeWidth = std::max(16, m_alertsBadge->fontMetrics().horizontalAdvance(text) + 6);
    m_alertsBadge->setFixedWidth(badgeWidth);
    const int x = m_alertsButton->width() - badgeWidth - 4;
    m_alertsBadge->move(std::max(0, x), 2);
    m_alertsBadge->show();
}

void MainWindow::refreshAlertsList()
{
    if (!m_alertsList) {
        return;
    }

    m_alertsList->clear();
    // Newest first
    for (auto it = m_notifications.crbegin(); it != m_notifications.crend(); ++it) {
        const auto &n = *it;
        const QString text = QStringLiteral("%1\n%2")
                                 .arg(n.timestamp.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")),
                                      n.text);
        auto *item = new QListWidgetItem(text);
        QFont f = item->font();
        f.setBold(!n.read);
        item->setFont(f);
        item->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        // Allow multiline and prevent eliding: size the row to fit text within current viewport width.
        const int viewWidth = m_alertsList->viewport() ? m_alertsList->viewport()->width() : 300;
        const int usableWidth = std::max(80, viewWidth - 12);
        QFontMetrics fm(f);
        const QRect br = fm.boundingRect(QRect(0, 0, usableWidth, 0),
                                         Qt::TextWordWrap,
                                         text);
        item->setSizeHint(QSize(usableWidth, br.height() + 6));
        m_alertsList->addItem(item);
    }
}

void MainWindow::markAllNotificationsRead()
{
    bool changed = false;
    for (auto &n : m_notifications) {
        if (!n.read) {
            n.read = true;
            changed = true;
        }
    }
    if (changed) {
        m_unreadNotifications = 0;
        refreshAlertsList();
        updateAlertsBadge();
    } else {
        m_unreadNotifications = 0;
        updateAlertsBadge();
    }
}

void MainWindow::repositionAlertsPanel()
{
    if (!m_alertsPanel || !m_workspaceStack) {
        return;
    }

    const int margin = 12;
    const int panelWidth = 320;
    const int minHeight = 160;
    const int maxHeight = 340;
    const int availableHeight = std::max(minHeight, m_workspaceStack->height() - margin * 2);
    const int panelHeight = std::min(availableHeight, maxHeight);
    const int x = margin;
    const int y = margin;

    m_alertsPanelTargetRect = QRect(x, y, panelWidth, panelHeight);
    QRect hiddenRect = m_alertsPanelTargetRect;
    hiddenRect.moveLeft(-panelWidth - margin);

    if (m_alertsPanel->isVisible()) {
        if (m_alertsPanelAnim && m_alertsPanelAnim->state() == QAbstractAnimation::Running) {
            m_alertsPanelAnim->setEndValue(m_alertsPanelExpanded ? m_alertsPanelTargetRect : hiddenRect);
        } else {
            m_alertsPanel->setGeometry(m_alertsPanelExpanded ? m_alertsPanelTargetRect : hiddenRect);
        }
    } else {
        m_alertsPanel->setGeometry(hiddenRect);
    }
    m_alertsPanel->raise();
}

void MainWindow::handleUpdateButtonClicked()
{
    toggleUpdatePanel();
}

void MainWindow::checkForUpdates()
{
    if (!m_updateNetwork) {
        m_updateNetwork = new QNetworkAccessManager(this);
    }
    if (m_updateReply) {
        m_updateReply->abort();
    }
    setUpdateState(UpdateState::Checking);

    const QUrl url(QStringLiteral("https://raw.githubusercontent.com/qlandys/FusionTerminal/main/latest.json"));
    QNetworkRequest req(url);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setRawHeader("User-Agent", "FusionTerminal/Updater");
    QNetworkReply *reply = m_updateNetwork->get(req);
    m_updateReply = reply;

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        const QNetworkReply::NetworkError err = reply->error();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray raw = reply->readAll();
        reply->deleteLater();
        if (reply == m_updateReply) {
            m_updateReply = nullptr;
        }

        if (err != QNetworkReply::NoError || status >= 400) {
            const QString msg = err == QNetworkReply::NoError
                                    ? tr("Update check failed (HTTP %1)").arg(status)
                                    : tr("Update check failed: %1").arg(reply->errorString());
            setUpdateState(UpdateState::Failed, msg);
            statusBar()->showMessage(msg, 3000);
            return;
        }

        UpdateInfo info;
        QString parseErr;
        if (!parseLatestJson(raw, info, parseErr)) {
            const QString msg = tr("Update check failed: %1").arg(parseErr);
            setUpdateState(UpdateState::Failed, msg);
            statusBar()->showMessage(msg, 3000);
            return;
        }

        const QString current = currentUpdateVersion();
        const int cmp = compareVersionStrings(current, info.version);
        if (cmp >= 0) {
            m_updateInfo = UpdateInfo();
            setUpdateState(UpdateState::UpToDate, tr("No updates available"));
            return;
        }

        m_updateInfo = info;
        const QString msg = tr("Update available: %1").arg(info.version);
        setUpdateState(UpdateState::UpdateAvailable, msg);
        statusBar()->showMessage(msg, 3500);
    });
}

void MainWindow::startUpdateDownload()
{
    if (!m_updateNetwork) {
        m_updateNetwork = new QNetworkAccessManager(this);
    }
    if (!m_updateInfo.url.isValid()) {
        setUpdateState(UpdateState::Failed, tr("Update URL is missing"));
        return;
    }

    if (m_updateReply) {
        m_updateReply->abort();
        m_updateReply = nullptr;
    }
    if (m_updateFile) {
        m_updateFile->deleteLater();
        m_updateFile = nullptr;
    }

    const QString fileName = QFileInfo(m_updateInfo.url.path()).fileName().isEmpty()
                                 ? QStringLiteral("FusionTerminal-%1.exe").arg(m_updateInfo.version)
                                 : QFileInfo(m_updateInfo.url.path()).fileName();
    const QString targetDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    if (targetDir.isEmpty()) {
        setUpdateState(UpdateState::Failed, tr("Update temp path unavailable"));
        return;
    }
    QDir().mkpath(targetDir);
    m_updateFilePath = QDir(targetDir).filePath(fileName);
    m_updateBackendFilePath.clear();

    m_updateFile = new QFile(m_updateFilePath, this);
    if (!m_updateFile->open(QIODevice::WriteOnly)) {
        setUpdateState(UpdateState::Failed, tr("Cannot write update file"));
        m_updateFile->deleteLater();
        m_updateFile = nullptr;
        return;
    }

    setUpdateState(UpdateState::Downloading);

    QNetworkRequest req(m_updateInfo.url);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setRawHeader("User-Agent", "FusionTerminal/Updater");
    QNetworkReply *reply = m_updateNetwork->get(req);
    m_updateReply = reply;

    connect(reply, &QNetworkReply::readyRead, this, [this, reply]() {
        if (!m_updateFile) {
            return;
        }
        const QByteArray chunk = reply->readAll();
        if (!chunk.isEmpty()) {
            m_updateFile->write(chunk);
        }
    });
    connect(reply, &QNetworkReply::downloadProgress, this, [this](qint64, qint64) {
        if (m_updateState != UpdateState::Downloading) {
            setUpdateState(UpdateState::Downloading);
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        const QNetworkReply::NetworkError err = reply->error();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (m_updateFile) {
            const QByteArray tail = reply->readAll();
            if (!tail.isEmpty()) {
                m_updateFile->write(tail);
            }
            m_updateFile->close();
        }

        reply->deleteLater();
        if (reply == m_updateReply) {
            m_updateReply = nullptr;
        }

        if (err != QNetworkReply::NoError || status >= 400) {
            const QString msg = err == QNetworkReply::NoError
                                    ? tr("Download failed (HTTP %1)").arg(status)
                                    : tr("Download failed: %1").arg(reply->errorString());
            setUpdateState(UpdateState::Failed, msg);
            if (m_updateFile) {
                m_updateFile->remove();
                m_updateFile->deleteLater();
                m_updateFile = nullptr;
            }
            statusBar()->showMessage(msg, 3500);
            return;
        }

        QString hashErr;
        if (!verifyUpdateHash(m_updateFilePath, m_updateInfo.sha256, hashErr)) {
            const QString msg = tr("Update hash mismatch");
            setUpdateState(UpdateState::Failed, msg);
            if (m_updateFile) {
                m_updateFile->remove();
                m_updateFile->deleteLater();
                m_updateFile = nullptr;
            }
            statusBar()->showMessage(msg, 3500);
            return;
        }

        if (m_updateInfo.backendUrl.isValid()) {
            startBackendDownload();
            return;
        }

        applyDownloadedUpdate();
    });
}

void MainWindow::startBackendDownload()
{
    if (!m_updateInfo.backendUrl.isValid()) {
        applyDownloadedUpdate();
        return;
    }
    if (!m_updateNetwork) {
        m_updateNetwork = new QNetworkAccessManager(this);
    }
    if (m_updateReply) {
        m_updateReply->abort();
        m_updateReply = nullptr;
    }
    if (m_updateFile) {
        m_updateFile->deleteLater();
        m_updateFile = nullptr;
    }

    const QString fileName = QFileInfo(m_updateInfo.backendUrl.path()).fileName().isEmpty()
                                 ? QStringLiteral("orderbook_backend.exe")
                                 : QFileInfo(m_updateInfo.backendUrl.path()).fileName();
    const QString targetDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    if (targetDir.isEmpty()) {
        setUpdateState(UpdateState::Failed, tr("Update temp path unavailable"));
        return;
    }
    QDir().mkpath(targetDir);
    m_updateBackendFilePath = QDir(targetDir).filePath(fileName);

    m_updateFile = new QFile(m_updateBackendFilePath, this);
    if (!m_updateFile->open(QIODevice::WriteOnly)) {
        setUpdateState(UpdateState::Failed, tr("Cannot write update file"));
        m_updateFile->deleteLater();
        m_updateFile = nullptr;
        return;
    }

    setUpdateState(UpdateState::Downloading);

    QNetworkRequest req(m_updateInfo.backendUrl);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setRawHeader("User-Agent", "FusionTerminal/Updater");
    QNetworkReply *reply = m_updateNetwork->get(req);
    m_updateReply = reply;

    connect(reply, &QNetworkReply::readyRead, this, [this, reply]() {
        if (!m_updateFile) {
            return;
        }
        const QByteArray chunk = reply->readAll();
        if (!chunk.isEmpty()) {
            m_updateFile->write(chunk);
        }
    });
    connect(reply, &QNetworkReply::downloadProgress, this, [this](qint64, qint64) {
        if (m_updateState != UpdateState::Downloading) {
            setUpdateState(UpdateState::Downloading);
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        const QNetworkReply::NetworkError err = reply->error();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (m_updateFile) {
            const QByteArray tail = reply->readAll();
            if (!tail.isEmpty()) {
                m_updateFile->write(tail);
            }
            m_updateFile->close();
        }

        reply->deleteLater();
        if (reply == m_updateReply) {
            m_updateReply = nullptr;
        }

        if (err != QNetworkReply::NoError || status >= 400) {
            const QString msg = err == QNetworkReply::NoError
                                    ? tr("Download failed (HTTP %1)").arg(status)
                                    : tr("Download failed: %1").arg(reply->errorString());
            setUpdateState(UpdateState::Failed, msg);
            if (m_updateFile) {
                m_updateFile->remove();
                m_updateFile->deleteLater();
                m_updateFile = nullptr;
            }
            statusBar()->showMessage(msg, 3500);
            return;
        }

        QString hashErr;
        if (!verifyUpdateHash(m_updateBackendFilePath, m_updateInfo.backendSha256, hashErr)) {
            const QString msg = tr("Update hash mismatch");
            setUpdateState(UpdateState::Failed, msg);
            if (m_updateFile) {
                m_updateFile->remove();
                m_updateFile->deleteLater();
                m_updateFile = nullptr;
            }
            statusBar()->showMessage(msg, 3500);
            return;
        }

        applyDownloadedUpdate();
    });
}

bool MainWindow::verifyUpdateHash(const QString &path, const QByteArray &expected, QString &err) const
{
    if (expected.trimmed().isEmpty()) {
        return true;
    }
    QFile verifyFile(path);
    if (!verifyFile.open(QIODevice::ReadOnly)) {
        err = tr("Cannot verify update hash");
        return false;
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!verifyFile.atEnd()) {
        hash.addData(verifyFile.read(8192));
    }
    const QByteArray actual = hash.result().toHex();
    if (actual != expected.trimmed().toLower()) {
        err = tr("Hash mismatch");
        return false;
    }
    return true;
}

void MainWindow::applyDownloadedUpdate()
{
    if (m_updateFilePath.isEmpty()) {
        setUpdateState(UpdateState::Failed, tr("Update file missing"));
        return;
    }

#ifdef Q_OS_WIN
    const QString targetExe = QCoreApplication::applicationFilePath();
    const QString updaterExe = QDir(QCoreApplication::applicationDirPath())
                                   .filePath(QStringLiteral("FusionUpdater.exe"));
    if (!QFile::exists(updaterExe)) {
        setUpdateState(UpdateState::Failed, tr("Updater missing"));
        return;
    }

    setUpdateState(UpdateState::Installed, tr("Update installed"));
    statusBar()->showMessage(tr("Applying update..."), 2000);
    const QString pid = QString::number(QCoreApplication::applicationPid());
    QStringList args = {
        QStringLiteral("--src"), m_updateFilePath,
        QStringLiteral("--dst"), targetExe,
        QStringLiteral("--launch"), targetExe,
        QStringLiteral("--pid"), pid
    };
    if (!m_updateBackendFilePath.isEmpty() && QFile::exists(m_updateBackendFilePath)) {
        QString backendDst = m_backendPath;
        if (!backendDst.isEmpty()) {
            QFileInfo dstInfo(backendDst);
            if (!dstInfo.isAbsolute()) {
                backendDst = QDir(QCoreApplication::applicationDirPath()).filePath(backendDst);
            }
            args << QStringLiteral("--backend-src") << m_updateBackendFilePath;
            args << QStringLiteral("--backend-dst") << backendDst;
        }
    }
    QProcess::startDetached(updaterExe, args);
    qApp->quit();
#else
    setUpdateState(UpdateState::Installed, tr("Update downloaded"));
    statusBar()->showMessage(tr("Update ready. Please restart manually."), 4000);
#endif
}

void MainWindow::setUpdateState(UpdateState state, const QString &tooltip)
{
    m_updateState = state;
    m_updateStatusText = tooltip.isEmpty() ? updateTooltip(state) : tooltip;
    if (!m_updateButton) {
        return;
    }
    const QSize iconSize(28, 28);
    const QString iconName = updateIconName(state);
    m_updateButton->setIcon(loadIconTinted(iconName, QColor("#c0c0c0"), iconSize));
    m_updateButton->setProperty("iconName", iconName);
    m_updateButton->setToolTip(m_updateStatusText);
    refreshUpdatePanel();
}

QString MainWindow::updateIconName(UpdateState state) const
{
    switch (state) {
    case UpdateState::NotChecked:
        return QStringLiteral("cloud-question");
    case UpdateState::Checking:
        return QStringLiteral("cloud-search");
    case UpdateState::UpdateAvailable:
        return QStringLiteral("cloud-plus");
    case UpdateState::Downloading:
        return QStringLiteral("cloud-download");
    case UpdateState::DownloadPaused:
        return QStringLiteral("cloud-pause");
    case UpdateState::Failed:
        return QStringLiteral("cloud-x");
    case UpdateState::Installed:
        return QStringLiteral("cloud-check");
    case UpdateState::UpToDate:
        return QStringLiteral("cloud-check");
    }
    return QStringLiteral("cloud-question");
}

QString MainWindow::updateTooltip(UpdateState state) const
{
    switch (state) {
    case UpdateState::NotChecked:
        return tr("Updates not checked");
    case UpdateState::Checking:
        return tr("Checking for updates...");
    case UpdateState::UpdateAvailable:
        return tr("Update available");
    case UpdateState::Downloading:
        return tr("Downloading update");
    case UpdateState::DownloadPaused:
        return tr("Download paused");
    case UpdateState::Failed:
        return tr("Update failed");
    case UpdateState::Installed:
        return tr("Update installed");
    case UpdateState::UpToDate:
        return tr("No updates available");
    }
    return tr("Updates");
}

void MainWindow::ensureUpdatePanel()
{
    if (m_updatePanel) {
        return;
    }
    m_updatePanel = new QFrame(nullptr);
    m_updatePanel->setObjectName(QStringLiteral("UpdatePanel"));
    m_updatePanel->setWindowFlags(Qt::Popup | Qt::FramelessWindowHint);
    m_updatePanel->setStyleSheet(
        "QFrame#UpdatePanel {"
        "  background-color: #2b2b2b;"
        "  border: 1px solid #3c3c3c;"
        "  border-radius: 8px;"
        "}"
        "QLabel#UpdatePanelTitle {"
        "  color: #e6e6e6;"
        "  font-weight: 600;"
        "}"
        "QLabel#UpdatePanelBody {"
        "  color: #cfcfcf;"
        "}"
        "QPushButton#UpdatePanelButton {"
        "  background-color: #3a3a3a;"
        "  color: #e0e0e0;"
        "  padding: 4px 10px;"
        "  border: 1px solid #4a4a4a;"
        "  border-radius: 6px;"
        "}"
        "QPushButton#UpdatePanelButton:hover {"
        "  background-color: #454545;"
        "}"
    );

    auto *root = new QVBoxLayout(m_updatePanel);
    root->setContentsMargins(12, 10, 12, 10);
    root->setSpacing(8);

    m_updatePanelTitle = new QLabel(tr("Updates"), m_updatePanel);
    m_updatePanelTitle->setObjectName(QStringLiteral("UpdatePanelTitle"));
    root->addWidget(m_updatePanelTitle);

    m_updatePanelBody = new QLabel(m_updatePanel);
    m_updatePanelBody->setObjectName(QStringLiteral("UpdatePanelBody"));
    m_updatePanelBody->setWordWrap(true);
    m_updatePanelBody->setMinimumWidth(240);
    root->addWidget(m_updatePanelBody);

    auto *buttonRow = new QHBoxLayout();
    buttonRow->setSpacing(8);
    root->addLayout(buttonRow);

    m_updateInstallButton = new QPushButton(tr("Install"), m_updatePanel);
    m_updateInstallButton->setObjectName(QStringLiteral("UpdatePanelButton"));
    buttonRow->addWidget(m_updateInstallButton);
    connect(m_updateInstallButton, &QPushButton::clicked, this, [this]() {
        if (m_updateState == UpdateState::UpdateAvailable
            || m_updateState == UpdateState::DownloadPaused) {
            startUpdateDownload();
        }
    });

    m_updateCheckButton = new QPushButton(tr("Check"), m_updatePanel);
    m_updateCheckButton->setObjectName(QStringLiteral("UpdatePanelButton"));
    buttonRow->addWidget(m_updateCheckButton);
    connect(m_updateCheckButton, &QPushButton::clicked, this, [this]() {
        checkForUpdates();
    });

    m_updateLaterButton = new QPushButton(tr("Later"), m_updatePanel);
    m_updateLaterButton->setObjectName(QStringLiteral("UpdatePanelButton"));
    buttonRow->addWidget(m_updateLaterButton);
    connect(m_updateLaterButton, &QPushButton::clicked, this, [this]() {
        if (m_updatePanel) {
            m_updatePanel->hide();
        }
    });
}

void MainWindow::toggleUpdatePanel()
{
    ensureUpdatePanel();
    if (!m_updatePanel) {
        return;
    }
    if (m_updatePanel->isVisible()) {
        m_updatePanel->hide();
        return;
    }
    refreshUpdatePanel();
    m_updatePanel->adjustSize();

    const QPoint anchor =
        m_updateButton ? m_updateButton->mapToGlobal(QPoint(m_updateButton->width(), m_updateButton->height() / 2))
                       : mapToGlobal(QPoint(0, 0));
    const int x = anchor.x() + 10;
    const int y = qMax(10, anchor.y() - (m_updatePanel->height() / 2));
    m_updatePanel->move(x, y);
    m_updatePanel->show();
    m_updatePanel->raise();
    m_updatePanel->activateWindow();
}

void MainWindow::refreshUpdatePanel()
{
    if (!m_updatePanel || !m_updatePanelTitle || !m_updatePanelBody) {
        return;
    }
    QString title = tr("Updates");
    QString body;

    switch (m_updateState) {
    case UpdateState::NotChecked:
        body = tr("Updates have not been checked yet.");
        break;
    case UpdateState::Checking:
        body = tr("Checking for updates...");
        break;
    case UpdateState::UpToDate:
        body = tr("You are on the latest version.");
        break;
    case UpdateState::Failed:
        body = m_updateStatusText.isEmpty() ? tr("Update check failed. Try again later.") : m_updateStatusText;
        break;
    case UpdateState::UpdateAvailable: {
        title = tr("Update available");
        body = tr("New version: %1").arg(m_updateInfo.version);
        if (!m_updateInfo.notes.isEmpty()) {
            body += "\n\n" + m_updateInfo.notes.join("\n");
        }
        break;
    }
    case UpdateState::Downloading:
        title = tr("Downloading update");
        body = tr("Downloading the update...");
        break;
    case UpdateState::DownloadPaused:
        title = tr("Download paused");
        body = tr("Download paused. Resume when ready.");
        break;
    case UpdateState::Installed:
        title = tr("Update ready");
        body = tr("Update is being installed...");
        break;
    }

    m_updatePanelTitle->setText(title);
    m_updatePanelBody->setText(body);

    const bool canInstall = (m_updateState == UpdateState::UpdateAvailable
                             || m_updateState == UpdateState::DownloadPaused);
    const bool canCheck = (m_updateState == UpdateState::NotChecked
                           || m_updateState == UpdateState::Failed
                           || m_updateState == UpdateState::UpToDate);

    m_updateInstallButton->setVisible(canInstall || m_updateState == UpdateState::Downloading);
    m_updateInstallButton->setEnabled(canInstall);
    m_updateCheckButton->setVisible(canCheck || m_updateState == UpdateState::Checking);
    m_updateCheckButton->setEnabled(canCheck);
    m_updateLaterButton->setVisible(true);
}

QString MainWindow::currentUpdateVersion() const
{
    QString version = QCoreApplication::applicationVersion().trimmed();
    if (version.startsWith(QLatin1Char('v'), Qt::CaseInsensitive)) {
        version = version.mid(1);
    }
    if (version.isEmpty()) {
        version = QStringLiteral("0.0.0");
    }
    return version;
}

bool MainWindow::parseLatestJson(const QByteArray &raw, UpdateInfo &info, QString &err) const
{
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        err = tr("Invalid JSON");
        return false;
    }
    const QJsonObject obj = doc.object();
    const QString version = obj.value(QStringLiteral("version")).toString().trimmed();
    const QString urlStr = obj.value(QStringLiteral("url")).toString().trimmed();
    if (version.isEmpty() || urlStr.isEmpty()) {
        err = tr("Missing version or url");
        return false;
    }
    const QUrl url(urlStr);
    if (!url.isValid()) {
        err = tr("Invalid update URL");
        return false;
    }

    info.version = version;
    info.url = url;
    info.sha256 = obj.value(QStringLiteral("sha256")).toString().toLatin1();
    info.backendUrl = QUrl(obj.value(QStringLiteral("backend_url")).toString().trimmed());
    info.backendSha256 = obj.value(QStringLiteral("backend_sha256")).toString().toLatin1();
    info.notes.clear();
    const QJsonValue notesVal = obj.value(QStringLiteral("notes"));
    if (notesVal.isArray()) {
        const QJsonArray notesArr = notesVal.toArray();
        for (const QJsonValue &noteVal : notesArr) {
            const QString note = noteVal.toString().trimmed();
            if (!note.isEmpty()) {
                info.notes.append(note);
            }
        }
    }
    info.mandatory = obj.value(QStringLiteral("mandatory")).toBool(false);
    return true;
}

QIcon MainWindow::loadIcon(const QString &name) const
{
    // ???? ?????? "<name>.svg" ? ?????????? ??????? ?????? (appDir, img/, img/icons/, img/icons/outline/).
    const QString relFile = QStringLiteral("%1.svg").arg(name);
    const QString path = resolveAssetPath(relFile);
    if (!path.isEmpty()) {
        return QIcon(path);
    }
    return QIcon();
}

QIcon MainWindow::loadIconTinted(const QString &name, const QColor &color, const QSize &size) const
{
    const QIcon base = loadIcon(name);
    if (base.isNull()) {
        return base;
    }

    const QSize iconSize = size.isValid() ? size : QSize(16, 16);
    // Always render at >=2x so icons stay crisp on fractional DPI (125/150/175%) and secondary screens.
    const qreal dpr =
        std::max<qreal>(2.0,
                        (QGuiApplication::primaryScreen() ? QGuiApplication::primaryScreen()->devicePixelRatio()
                                                          : 1.0));
    const QSize pxSize(std::max(1, qRound(static_cast<qreal>(iconSize.width()) * dpr)),
                       std::max(1, qRound(static_cast<qreal>(iconSize.height()) * dpr)));
    QPixmap src = base.pixmap(pxSize, QIcon::Normal, QIcon::Off);
    if (src.isNull()) {
        return base;
    }
    src.setDevicePixelRatio(dpr);

    QPixmap tinted(src.size());
    tinted.setDevicePixelRatio(dpr);
    tinted.fill(Qt::transparent);

    QPainter p(&tinted);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    p.drawPixmap(0, 0, src);
    p.setCompositionMode(QPainter::CompositionMode_SourceIn);
    p.fillRect(tinted.rect(), color);
    p.end();

    QIcon result;
    result.addPixmap(tinted);
    return result;
}

static QIcon mirrorIconHorizontally(const QIcon &icon, const QSize &size)
{
    if (icon.isNull()) {
        return icon;
    }
    const qreal dpr =
        std::max<qreal>(2.0,
                        (QGuiApplication::primaryScreen() ? QGuiApplication::primaryScreen()->devicePixelRatio()
                                                          : 1.0));
    const QSize pxSize(std::max(1, qRound(static_cast<qreal>(size.width()) * dpr)),
                       std::max(1, qRound(static_cast<qreal>(size.height()) * dpr)));
    QPixmap pix = icon.pixmap(pxSize, QIcon::Normal, QIcon::Off);
    if (pix.isNull()) {
        return icon;
    }
    pix.setDevicePixelRatio(dpr);
    QTransform t;
    t.scale(-1.0, 1.0);
    QPixmap mirrored = pix.transformed(t, Qt::SmoothTransformation);
    mirrored.setDevicePixelRatio(dpr);
    QIcon mirroredIcon;
    mirroredIcon.addPixmap(mirrored);
    return mirroredIcon;
}

QString MainWindow::resolveAssetPath(const QString &relative) const
{
    const QString rel = QDir::fromNativeSeparators(relative);

    const QString appDir = QCoreApplication::applicationDirPath();

    // ???? ?????? ? ?????????? ?????????? ? ?? ??????????????
    const QStringList bases = {
        appDir,
        QDir(appDir).filePath(QStringLiteral("assets")),
        QDir(appDir).filePath(QStringLiteral("assets/img")),
        QDir(appDir).filePath(QStringLiteral("assets/img/icons")),
        QDir(appDir).filePath(QStringLiteral("assets/img/icons/outline")),
        QDir(appDir).filePath(QStringLiteral("assets/img/outline")),
        QDir(appDir).filePath(QStringLiteral("../assets")),
        QDir(appDir).filePath(QStringLiteral("../assets/img")),
        QDir(appDir).filePath(QStringLiteral("../assets/img/icons")),
        QDir(appDir).filePath(QStringLiteral("../assets/img/icons/outline")),
        QDir(appDir).filePath(QStringLiteral("../assets/img/outline")),
        QDir(appDir).filePath(QStringLiteral("../../assets")),
        QDir(appDir).filePath(QStringLiteral("../../assets/img")),
        QDir(appDir).filePath(QStringLiteral("../../assets/img/icons")),
        QDir(appDir).filePath(QStringLiteral("../../assets/img/icons/outline")),
        QDir(appDir).filePath(QStringLiteral("../../assets/img/outline"))
    };

    for (const QString &base : bases) {
        const QString candidate = QDir(base).filePath(rel);
        if (QFile::exists(candidate)) {
            return candidate;
        }
    }
    const QString resPath = QStringLiteral(":/") + rel;
    if (QFile::exists(resPath)) {
        return resPath;
    }

    return QString();
}

bool MainWindow::locateColumn(QWidget *container, WorkspaceTab *&tabOut, DomColumn *&colOut, int &splitIndex)
{
    QSplitter *splitter = nullptr;
    return locateColumnEx(container, tabOut, colOut, splitter, splitIndex);
}

bool MainWindow::locateColumnEx(QWidget *container,
                                WorkspaceTab *&tabOut,
                                DomColumn *&colOut,
                                QSplitter *&splitterOut,
                                int &splitIndex)
{
    tabOut = nullptr;
    colOut = nullptr;
    splitterOut = nullptr;
    splitIndex = -1;
    if (!container) {
        return false;
    }
    for (auto &tab : m_tabs) {
        for (auto &col : tab.columnsData) {
            if (col.container == container) {
                tabOut = &tab;
                colOut = &col;
                splitterOut = findParentSplitter(container);
                if (splitterOut) {
                    splitIndex = splitterOut->indexOf(container);
                }
                return true;
            }
        }
    }
    return false;
}

void MainWindow::updateDomColumnResize(int delta)
{
    if (!m_domResizeActive || !m_domResizeTab || !m_domResizeTab->columns || m_domResizeSplitterIndex < 0) {
        return;
    }
    const QList<int> &baseSizes = m_domResizeInitialSizes;
    if (m_domResizeSplitterIndex >= baseSizes.size()) {
        return;
    }

    const int columnIndex = m_domResizeSplitterIndex;
    const int neighborIndex = m_domResizeFromLeftEdge ? columnIndex - 1 : columnIndex + 1;
    if (neighborIndex < 0 || neighborIndex >= baseSizes.size()) {
        return;
    }

    auto *splitter = m_domResizeTab->columns;
    const bool neighborIsSpacer =
        (m_domResizeTab->columnsSpacer && splitter->widget(neighborIndex) == m_domResizeTab->columnsSpacer);

    const int columnBase = baseSizes.value(columnIndex);
    const int neighborBase = baseSizes.value(neighborIndex);
    const int columnMin = kDomColumnMinWidth;
    const int neighborMin = neighborIsSpacer ? 0 : kDomColumnMinWidth;

    int clampedDelta = delta;
    if (m_domResizeFromLeftEdge) {
        const int minDelta = neighborMin - neighborBase;
        const int maxDelta = columnBase - columnMin;
        clampedDelta = std::clamp(clampedDelta, minDelta, maxDelta);
    } else {
        const int minDelta = columnMin - columnBase;
        const int maxDelta = neighborBase - neighborMin;
        clampedDelta = std::clamp(clampedDelta, minDelta, maxDelta);
    }

    int newColumnSize = columnBase;
    int newNeighborSize = neighborBase;
    if (m_domResizeFromLeftEdge) {
        newNeighborSize = neighborBase + clampedDelta;
        newColumnSize = columnBase - clampedDelta;
    } else {
        newColumnSize = columnBase + clampedDelta;
        newNeighborSize = neighborBase - clampedDelta;
    }

    QList<int> newSizes = baseSizes;
    newSizes[columnIndex] = std::max(columnMin, newColumnSize);
    newSizes[neighborIndex] = std::max(neighborMin, newNeighborSize);
    splitter->setSizes(newSizes);
}

void MainWindow::endDomColumnResize()
{
    m_domResizeActive = false;
    m_domResizePending = false;
    m_domResizeContainer = nullptr;
    m_domResizeTab = nullptr;
    m_domResizeInitialSizes.clear();
    m_domResizeSplitterIndex = -1;
    m_domResizeFromLeftEdge = false;
    releaseDomResizeMouseGrab();
}

void MainWindow::cancelPendingDomResize()
{
    m_domResizePending = false;
    m_domResizeContainer = nullptr;
    m_domResizeTab = nullptr;
    m_domResizeInitialSizes.clear();
    m_domResizeSplitterIndex = -1;
    m_domResizeFromLeftEdge = false;
    releaseDomResizeMouseGrab();
}

void MainWindow::releaseDomResizeMouseGrab()
{
    if (m_domResizeHandle) {
        m_domResizeHandle->releaseMouse();
        m_domResizeHandle = nullptr;
    }
}

void MainWindow::removeDomColumn(QWidget *container)
{
    if (!container) {
        return;
    }
    if (container->property("closingDom").toBool()) {
        return;
    }
    container->setProperty("closingDom", true);

    WorkspaceTab *tab = nullptr;
    DomColumn *col = nullptr;
    int splitIndex = -1;
    if (!locateColumn(container, tab, col, splitIndex) || !tab || !col) {
        return;
    }
    if (col->floatingWindow) {
        col->floatingWindow->removeEventFilter(this);
        col->floatingWindow->close();
        col->floatingWindow = nullptr;
        col->isFloating = false;
    }
    if (tab->columns && col->container) {
        if (m_domResizeContainer == col->container) {
            endDomColumnResize();
        } else if (m_domResizePending && m_domResizeContainer == col->container) {
            cancelPendingDomResize();
        }
        col->container->setParent(nullptr);
    }
    if (col->client) {
        col->client->stop();
        col->client->deleteLater();
        col->client = nullptr;
    }
    if (col->dom) {
        col->dom->deleteLater();
        col->dom = nullptr;
    }
    if (col->container && col->container == m_activeDomContainer) {
        m_activeDomContainer = nullptr;
    }
    if (col->container) {
        col->container->deleteLater();
        col->container = nullptr;
    }
    for (int i = 0; i < tab->columnsData.size(); ++i) {
        if (&tab->columnsData[i] == col) {
            tab->columnsData.removeAt(i);
            break;
        }
    }
    syncWatchedSymbols();
}

void MainWindow::toggleDomColumnFloating(QWidget *container, const QPoint &globalPos)
{
    WorkspaceTab *tab = nullptr;
    DomColumn *col = nullptr;
    int splitIndex = -1;
    if (!locateColumn(container, tab, col, splitIndex) || !tab || !col) {
        return;
    }
    if (col->isFloating) {
        dockDomColumn(*tab, *col, splitIndex);
    } else {
        floatDomColumn(*tab, *col, splitIndex, globalPos);
    }
}

void MainWindow::floatDomColumn(WorkspaceTab &tab, DomColumn &col, int indexInSplitter, const QPoint &globalPos)
{
    if (!tab.columns || col.isFloating || !col.container) {
        return;
    }
    QSplitter *parentSplitter = findParentSplitter(col.container);
    col.lastParentSplitter = parentSplitter;
    if (parentSplitter) {
        col.lastSplitterIndex =
            indexInSplitter >= 0 ? indexInSplitter : parentSplitter->indexOf(col.container);
        col.lastSplitterSizes = parentSplitter->sizes();
    } else {
        col.lastSplitterIndex = -1;
        col.lastSplitterSizes.clear();
    }
    if (m_domResizeContainer == col.container) {
        endDomColumnResize();
    } else if (m_domResizePending && m_domResizeContainer == col.container) {
        cancelPendingDomResize();
    }
    col.container->setParent(nullptr);
    if (parentSplitter) {
        collapseSplitterIfSingle(parentSplitter);
    }

    QWidget *win = new QWidget(nullptr,
                               Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    win->setObjectName(QStringLiteral("DomFloatingWindow"));
    win->setAttribute(Qt::WA_DeleteOnClose, false);
    win->setAttribute(Qt::WA_NoMousePropagation, true);
    win->setAttribute(Qt::WA_TransparentForMouseEvents, false);
    win->setAttribute(Qt::WA_NativeWindow, true);
    win->setMouseTracking(true);
    win->setFocusPolicy(Qt::StrongFocus);
    win->setWindowTitle(col.symbol.isEmpty() ? tr("DOM") : col.symbol);
    win->setStyleSheet(QStringLiteral(
        "QWidget#DomFloatingWindow {"
        "  background-color: #1c1c1c;"
        "  border: 1px solid #2b2b2b;"
        "}"));
    win->setProperty("domContainerPtr",
                     QVariant::fromValue<quintptr>(reinterpret_cast<quintptr>(col.container)));
    col.container->setAttribute(Qt::WA_TransparentForMouseEvents, false);
    col.container->setMouseTracking(true);
    col.container->setFocusPolicy(Qt::StrongFocus);
    win->winId();
    auto *layout = new QGridLayout(win);
    layout->setContentsMargins(1, 1, 1, 1);
    layout->setSpacing(0);
    layout->addWidget(col.container, 0, 0);
    auto *grip = new QSizeGrip(win);
    grip->setObjectName(QStringLiteral("DomFloatSizeGrip"));
    grip->setFixedSize(14, 14);
    grip->setCursor(Qt::SizeFDiagCursor);
    layout->addWidget(grip, 0, 0, Qt::AlignRight | Qt::AlignBottom);
    col.floatingWindow = win;
    col.isFloating = true;
    win->installEventFilter(this);
    win->resize(col.container->size());
    if (globalPos.isNull()) {
        win->move(mapToGlobal(QPoint(40, 60)));
    } else {
        win->move(globalPos - m_domDragStartWindowOffset);
    }
    win->show();
    win->raise();
    win->activateWindow();
    if (m_domDragGrabber) {
        m_domDragGrabber->releaseMouse();
        m_domDragGrabber = win;
        win->grabMouse();
    }
}

void MainWindow::dockDomColumn(WorkspaceTab &tab, DomColumn &col, int preferredIndex)
{
    if (!tab.columns || !col.container) {
        return;
    }
    if (col.floatingWindow) {
        col.floatingWindow->removeEventFilter(this);
        col.floatingWindow->hide();
        col.floatingWindow = nullptr;
    }

    QSplitter *targetSplitter = col.lastParentSplitter ? col.lastParentSplitter.data() : tab.columns;
    if (!targetSplitter) {
        targetSplitter = tab.columns;
    }
    int insertIndex = preferredIndex >= 0 ? preferredIndex : col.lastSplitterIndex;
    int maxInsertIndex = targetSplitter->count();
    if (targetSplitter == tab.columns && tab.columnsSpacer) {
        const int spacerIndex = tab.columns->indexOf(tab.columnsSpacer);
        if (spacerIndex >= 0) {
            maxInsertIndex = spacerIndex;
        }
    }
    if (insertIndex < 0 || insertIndex > maxInsertIndex) {
        insertIndex = maxInsertIndex;
    }
    targetSplitter->insertWidget(insertIndex, col.container);
    targetSplitter->setStretchFactor(targetSplitter->indexOf(col.container), 0);
    if (!col.lastSplitterSizes.isEmpty() && targetSplitter == col.lastParentSplitter.data()) {
        targetSplitter->setSizes(col.lastSplitterSizes);
    }
    col.isFloating = false;
}

void MainWindow::openPluginsWindow()
{
    if (!m_pluginsWindow) {
        m_pluginsWindow = new PluginsWindow(this);
        connect(m_pluginsWindow,
                &PluginsWindow::installedModsChanged,
                this,
                &MainWindow::updateInstalledMods);
    }
    m_pluginsWindow->setAuthContext(m_authBaseUrl, m_authToken, m_authRole, m_authUser);
    m_pluginsWindow->show();
    m_pluginsWindow->raise();
    m_pluginsWindow->activateWindow();
}

QSplitter *MainWindow::findParentSplitter(QWidget *widget) const
{
    QWidget *parent = widget ? widget->parentWidget() : nullptr;
    while (parent) {
        if (auto *splitter = qobject_cast<QSplitter *>(parent)) {
            return splitter;
        }
        parent = parent->parentWidget();
    }
    return nullptr;
}

void MainWindow::collapseSplitterIfSingle(QSplitter *splitter)
{
    if (!splitter) {
        return;
    }
    if (splitter->count() != 1) {
        return;
    }
    QWidget *child = splitter->widget(0);
    auto *parentSplitter = qobject_cast<QSplitter *>(splitter->parentWidget());
    if (!child || !parentSplitter) {
        return;
    }
    const int index = parentSplitter->indexOf(splitter);
    if (index < 0) {
        return;
    }
    child->setParent(parentSplitter);
    parentSplitter->insertWidget(index, child);
    splitter->deleteLater();
}

void MainWindow::applyDockDrop(WorkspaceTab &dragTab,
                               DomColumn &dragCol,
                               WorkspaceTab &targetTab,
                               DomColumn &targetCol,
                               int targetIndex,
                               QSplitter *targetSplitter,
                               DockZone dropZone)
{
    if (dropZone == DockZone::None || !targetSplitter) {
        return;
    }
    if (&dragTab != &targetTab || &dragCol == &targetCol) {
        return;
    }
    if (!dragCol.container || !targetCol.container) {
        return;
    }

    QSplitter *dragParent = findParentSplitter(dragCol.container);
    int dragIndex = dragParent ? dragParent->indexOf(dragCol.container) : -1;
    if (dragParent && dragParent == targetSplitter && dragIndex >= 0 && dragIndex < targetIndex) {
        targetIndex -= 1;
    }

    if (dragCol.floatingWindow) {
        dragCol.floatingWindow->removeEventFilter(this);
        dragCol.floatingWindow->hide();
        dragCol.floatingWindow = nullptr;
    }
    dragCol.isFloating = false;

    if (dragCol.container->parentWidget()) {
        dragCol.container->setParent(nullptr);
    }
    if (dragParent && dragParent != targetSplitter) {
        collapseSplitterIfSingle(dragParent);
    }

    QWidget *targetWidget = targetCol.container;
    if (targetWidget->parentWidget()) {
        targetWidget->setParent(nullptr);
    }

    const bool vertical = (dropZone == DockZone::Top || dropZone == DockZone::Bottom);
    auto *splitter = new AnimatedSplitter(vertical ? Qt::Vertical : Qt::Horizontal, targetSplitter);
    splitter->setObjectName(QStringLiteral("DomDockSplitter"));
    splitter->setChildrenCollapsible(false);
    splitter->setHandleWidth(2);

    if (dropZone == DockZone::Left || dropZone == DockZone::Top) {
        splitter->addWidget(dragCol.container);
        splitter->addWidget(targetWidget);
    } else {
        splitter->addWidget(targetWidget);
        splitter->addWidget(dragCol.container);
    }
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({1, 1});

    if (targetIndex < 0) {
        targetIndex = targetSplitter->count();
    }
    targetSplitter->insertWidget(targetIndex, splitter);
    targetSplitter->setStretchFactor(targetSplitter->indexOf(splitter), 0);

    dragCol.lastParentSplitter = splitter;
    dragCol.lastSplitterIndex = splitter->indexOf(dragCol.container);
    dragCol.lastSplitterSizes = splitter->sizes();
}

void MainWindow::applyDockDropWidget(QWidget *dragWidget,
                                     QWidget *targetWidget,
                                     QSplitter *targetSplitter,
                                     int targetIndex,
                                     DockZone dropZone)
{
    if (!dragWidget || !targetWidget || !targetSplitter || dropZone == DockZone::None) {
        return;
    }
    if (dragWidget == targetWidget) {
        return;
    }

    QSplitter *dragParent = findParentSplitter(dragWidget);
    int dragIndex = dragParent ? dragParent->indexOf(dragWidget) : -1;
    if (dragParent && dragParent == targetSplitter && dragIndex >= 0 && dragIndex < targetIndex) {
        targetIndex -= 1;
    }

    if (dragWidget->parentWidget()) {
        dragWidget->setParent(nullptr);
    }
    if (dragParent && dragParent != targetSplitter) {
        collapseSplitterIfSingle(dragParent);
    }
    if (targetWidget->parentWidget()) {
        targetWidget->setParent(nullptr);
    }

    const bool vertical = (dropZone == DockZone::Top || dropZone == DockZone::Bottom);
    auto *splitter = new AnimatedSplitter(vertical ? Qt::Vertical : Qt::Horizontal, targetSplitter);
    splitter->setObjectName(QStringLiteral("DomDockSplitter"));
    splitter->setChildrenCollapsible(false);
    splitter->setHandleWidth(2);

    if (dropZone == DockZone::Left || dropZone == DockZone::Top) {
        splitter->addWidget(dragWidget);
        splitter->addWidget(targetWidget);
    } else {
        splitter->addWidget(targetWidget);
        splitter->addWidget(dragWidget);
    }
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({1, 1});

    if (targetIndex < 0) {
        targetIndex = targetSplitter->count();
    }
    targetSplitter->insertWidget(targetIndex, splitter);
    targetSplitter->setStretchFactor(targetSplitter->indexOf(splitter), 0);
}

void MainWindow::dockChatWindow(ChatWindow *chat,
                                QWidget *targetWidget,
                                QSplitter *targetSplitter,
                                int targetIndex,
                                DockZone dropZone)
{
    if (!chat || !targetWidget || !targetSplitter) {
        return;
    }
    chat->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    chat->setDocked(true);
    applyDockDropWidget(chat, targetWidget, targetSplitter, targetIndex, dropZone);
}

void MainWindow::floatChatWindow(ChatWindow *chat, const QPoint &globalPos, const QPoint &offset)
{
    if (!chat) {
        return;
    }
    QSplitter *parentSplitter = findParentSplitter(chat);
    if (chat->parentWidget()) {
        chat->setParent(nullptr);
    }
    if (parentSplitter) {
        collapseSplitterIfSingle(parentSplitter);
    }
    chat->setDocked(false);
    chat->move(globalPos - offset);
    chat->show();
    chat->raise();
    chat->startFloatingDrag(globalPos, offset);
}

void MainWindow::closeChatWindow(ChatWindow *chat)
{
    if (!chat) {
        return;
    }
    QSplitter *parentSplitter = findParentSplitter(chat);
    if (chat->parentWidget()) {
        chat->setParent(nullptr);
    }
    if (parentSplitter) {
        collapseSplitterIfSingle(parentSplitter);
    }
    chat->setDocked(false);
    chat->hide();
}

void MainWindow::updateDockOverlay(const QPoint &globalPos, QWidget *draggingContainer)
{
    WorkspaceTab *tab = currentWorkspaceTab();
    if (!tab) {
        hideDockOverlay();
        return;
    }

    QWidget *target = nullptr;
    QRect targetRect;
    const auto findBestTarget = [&](bool requireContains) {
        QWidget *best = nullptr;
        QRect bestRect;
        int bestDist = INT_MAX;
        for (auto &col : tab->columnsData) {
            if (!col.container || col.container == draggingContainer || col.isFloating) {
                continue;
            }
            if (!col.container->isVisible()) {
                continue;
            }
            const QRect rect(col.container->mapToGlobal(QPoint(0, 0)), col.container->size());
            if (requireContains) {
                if (rect.contains(globalPos)) {
                    return std::pair<QWidget *, QRect>(col.container, rect);
                }
                continue;
            }
            const int snapDistance = 48;
            const QRect snapRect = rect.adjusted(-snapDistance, -snapDistance, snapDistance, snapDistance);
            if (!snapRect.contains(globalPos)) {
                continue;
            }
            const int dist = std::abs(globalPos.x() - rect.center().x());
            if (dist < bestDist) {
                bestDist = dist;
                best = col.container;
                bestRect = rect;
            }
        }
        return std::pair<QWidget *, QRect>(best, bestRect);
    };

    auto [directTarget, directRect] = findBestTarget(true);
    target = directTarget;
    targetRect = directRect;

    if (!target && tab->columns) {
        const QRect splitterRect(tab->columns->mapToGlobal(QPoint(0, 0)), tab->columns->size());
        if (splitterRect.contains(globalPos)) {
            auto [nearestTarget, nearestRect] = findBestTarget(false);
            target = nearestTarget;
            targetRect = nearestRect;
        }
    }

    if (!target) {
        hideDockOverlay();
        return;
    }

    if (!m_dockOverlay) {
        auto *overlay = new QWidget(this, Qt::ToolTip | Qt::FramelessWindowHint);
        overlay->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        overlay->setAttribute(Qt::WA_TranslucentBackground, true);
        overlay->setObjectName(QStringLiteral("DomDockOverlay"));
        overlay->setFixedSize(96, 96);
        auto *grid = new QGridLayout(overlay);
        grid->setContentsMargins(6, 6, 6, 6);
        grid->setSpacing(6);
        auto makeCell = [overlay, this](const QString &iconName) {
            auto *cell = new QLabel(overlay);
            cell->setObjectName(QStringLiteral("DockZoneCell"));
            cell->setFixedSize(18, 18);
            cell->setAlignment(Qt::AlignCenter);
            cell->setProperty("active", false);
            cell->setProperty("iconName", iconName);
            const QIcon icon = loadIconTinted(iconName, QColor("#c9cfd8"), QSize(16, 16));
            cell->setPixmap(icon.pixmap(16, 16));
            return cell;
        };
        auto *top = makeCell(QStringLiteral("square-chevron-up"));
        auto *left = makeCell(QStringLiteral("square-chevron-left"));
        auto *right = makeCell(QStringLiteral("square-chevron-right"));
        auto *bottom = makeCell(QStringLiteral("square-chevron-down"));
        top->setProperty("zone", static_cast<int>(DockZone::Top));
        left->setProperty("zone", static_cast<int>(DockZone::Left));
        right->setProperty("zone", static_cast<int>(DockZone::Right));
        bottom->setProperty("zone", static_cast<int>(DockZone::Bottom));
        grid->addWidget(top, 0, 1, Qt::AlignCenter);
        grid->addWidget(left, 1, 0, Qt::AlignCenter);
        grid->addWidget(right, 1, 2, Qt::AlignCenter);
        grid->addWidget(bottom, 2, 1, Qt::AlignCenter);
        overlay->setStyleSheet(QStringLiteral(
            "QLabel#DockZoneCell {"
            "  background: transparent;"
            "}"));
        m_dockOverlay = overlay;
    }

    const QSize overlaySize = m_dockOverlay->size();
    const QPoint center = targetRect.center() - QPoint(overlaySize.width() / 2, overlaySize.height() / 2);
    m_dockOverlay->move(center);
    m_dockOverlay->show();
    m_dockOverlay->raise();

    DockZone zone = DockZone::None;
    const QList<QLabel *> cells = m_dockOverlay->findChildren<QLabel *>(QStringLiteral("DockZoneCell"));
    if (targetRect.contains(globalPos)) {
        const QPoint local = m_dockOverlay->mapFromGlobal(globalPos);
        for (QLabel *cell : cells) {
            if (!cell) {
                continue;
            }
            const QRect cellRect = cell->geometry();
            if (cellRect.contains(local)) {
                zone = static_cast<DockZone>(cell->property("zone").toInt());
                break;
            }
        }
    } else {
        zone = (globalPos.x() < targetRect.center().x()) ? DockZone::Left : DockZone::Right;
    }

    for (QLabel *cell : cells) {
        if (!cell) {
            continue;
        }
        const DockZone cellZone = static_cast<DockZone>(cell->property("zone").toInt());
        cell->setProperty("active", cellZone == zone);
        cell->style()->unpolish(cell);
        cell->style()->polish(cell);
        const QString iconName = cell->property("iconName").toString();
        const QColor iconColor = (cellZone == zone) ? QColor("#e9f1ff") : QColor("#c9cfd8");
        const QIcon icon = loadIconTinted(iconName, iconColor, QSize(16, 16));
        cell->setPixmap(icon.pixmap(16, 16));
    }

    m_dockOverlayTarget = target;
    m_dockOverlayZone = zone;
}

void MainWindow::hideDockOverlay()
{
    m_dockOverlayTarget = nullptr;
    m_dockOverlayZone = DockZone::None;
    if (m_dockOverlay) {
        m_dockOverlay->hide();
    }
}

void MainWindow::finishDomDrag()
{
    if (m_draggingDomContainer && m_domDragActive) {
        WorkspaceTab *dragTab = nullptr;
        DomColumn *dragCol = nullptr;
        QSplitter *dragSplitter = nullptr;
        int dragIndex = -1;
        locateColumnEx(m_draggingDomContainer, dragTab, dragCol, dragSplitter, dragIndex);
        if (dragTab && dragCol && m_dockOverlayTarget && m_dockOverlayZone != DockZone::None) {
            WorkspaceTab *targetTab = nullptr;
            DomColumn *targetCol = nullptr;
            QSplitter *targetSplitter = nullptr;
            int targetIndex = -1;
            locateColumnEx(m_dockOverlayTarget, targetTab, targetCol, targetSplitter, targetIndex);
            if (targetTab && targetCol && targetSplitter) {
                applyDockDrop(*dragTab,
                              *dragCol,
                              *targetTab,
                              *targetCol,
                              targetIndex,
                              targetSplitter,
                              m_dockOverlayZone);
            }
        }
    }
    hideDockOverlay();
    if (m_domDragGrabber) {
        m_domDragGrabber->releaseMouse();
    }
    m_domDragGrabber = nullptr;
    m_draggingDomContainer = nullptr;
    m_domDragActive = false;
    m_pendingTickerLabel = nullptr;
    m_pendingTickerContainer = nullptr;
    m_pendingTickerDrag = false;
}

void MainWindow::abortDomDrag()
{
    hideDockOverlay();
    if (m_domDragGrabber) {
        m_domDragGrabber->releaseMouse();
    }
    m_domDragGrabber = nullptr;
    m_draggingDomContainer = nullptr;
    m_domDragActive = false;
    m_pendingTickerLabel = nullptr;
    m_pendingTickerContainer = nullptr;
    m_pendingTickerDrag = false;
}

void MainWindow::openChatForContainer(QWidget *container)
{
    if (!container) {
        return;
    }
    if (m_authToken.isEmpty()) {
        if (statusBar()) {
            statusBar()->showMessage(tr("Sign in to use chat."), 2500);
        }
        return;
    }
    if (!isModInstalled(QStringLiteral("chat-ladder"))) {
        if (statusBar()) {
            statusBar()->showMessage(tr("Install the chat mod to use chat."), 2500);
        }
        return;
    }
    WorkspaceTab *tab = nullptr;
    DomColumn *col = nullptr;
    int splitIndex = -1;
    if (!locateColumn(container, tab, col, splitIndex) || !col) {
        return;
    }
    if (!col->chatWindow) {
        col->chatWindow = new ChatWindow(this);
        connect(col->chatWindow,
                &ChatWindow::dragMoved,
                this,
                [this](const QPoint &pos) {
                    updateDockOverlay(pos, nullptr);
                });
        connect(col->chatWindow,
                &ChatWindow::dragReleased,
                this,
                [this](const QPoint &pos) {
                    auto *chat = qobject_cast<ChatWindow *>(sender());
                    if (chat && m_dockOverlayTarget && m_dockOverlayZone != DockZone::None) {
                        WorkspaceTab *targetTab = nullptr;
                        DomColumn *targetCol = nullptr;
                        QSplitter *targetSplitter = nullptr;
                        int targetIndex = -1;
                        locateColumnEx(m_dockOverlayTarget, targetTab, targetCol, targetSplitter, targetIndex);
                        if (targetTab && targetCol && targetSplitter) {
                            dockChatWindow(chat,
                                           targetCol->container,
                                           targetSplitter,
                                           targetIndex,
                                           m_dockOverlayZone);
                        }
                    }
                    hideDockOverlay();
                });
        connect(col->chatWindow,
                &ChatWindow::requestUndock,
                this,
                [this](ChatWindow *chat, const QPoint &pos, const QPoint &offset) {
                    floatChatWindow(chat, pos, offset);
                });
        connect(col->chatWindow,
                &ChatWindow::requestClose,
                this,
                [this](ChatWindow *chat) {
                    closeChatWindow(chat);
                });
    }
    col->chatWindow->setAuthContext(m_authBaseUrl, m_authToken, m_authUser);
    const QString key = chatKeyForColumn(*col);
    const QString title = chatTitleForColumn(*col);
    col->chatWindow->setChatKey(key, title);
    col->chatWindow->show();
    col->chatWindow->raise();
    col->chatWindow->activateWindow();
}

QString MainWindow::chatKeyForColumn(const DomColumn &col) const
{
    const SymbolSource source = symbolSourceForAccount(col.accountName);
    QString exchange = QStringLiteral("UNKNOWN");
    QString market = QStringLiteral("SPOT");
    const bool lighterPerp = (source == SymbolSource::Lighter) && !col.symbol.contains(QLatin1Char('/'));
    switch (source) {
    case SymbolSource::Mexc:
        exchange = QStringLiteral("MEXC");
        market = QStringLiteral("SPOT");
        break;
    case SymbolSource::MexcFutures:
        exchange = QStringLiteral("MEXC");
        market = QStringLiteral("FUTURES");
        break;
    case SymbolSource::BinanceSpot:
        exchange = QStringLiteral("BINANCE");
        market = QStringLiteral("SPOT");
        break;
    case SymbolSource::BinanceFutures:
        exchange = QStringLiteral("BINANCE");
        market = QStringLiteral("FUTURES");
        break;
    case SymbolSource::UzxSpot:
        exchange = QStringLiteral("UZX");
        market = QStringLiteral("SPOT");
        break;
    case SymbolSource::UzxSwap:
        exchange = QStringLiteral("UZX");
        market = QStringLiteral("SWAP");
        break;
    case SymbolSource::Lighter:
        exchange = QStringLiteral("LIGHTER");
        market = lighterPerp ? QStringLiteral("PERP") : QStringLiteral("SPOT");
        break;
    }
    return QStringLiteral("%1:%2:%3")
        .arg(exchange.toLower(), market.toLower(), col.symbol.toUpper());
}

QString MainWindow::chatTitleForColumn(const DomColumn &col) const
{
    const QString key = chatKeyForColumn(col);
    const QStringList parts = key.split(QLatin1Char(':'), Qt::SkipEmptyParts);
    const QString exchange = parts.isEmpty() ? QStringLiteral("UNKNOWN") : parts.at(0).toUpper();
    return QStringLiteral("%1: %2 CHAT").arg(exchange, col.symbol.toUpper());
}

void MainWindow::requestInstalledMods()
{
    if (m_modsRequestInFlight) {
        return;
    }
    if (m_authToken.trimmed().isEmpty()) {
        if (!m_installedMods.isEmpty()) {
            m_installedMods.clear();
            updateChatButtons();
        }
        return;
    }
    QString base = m_authBaseUrl.trimmed();
    if (base.isEmpty()) {
        base = QStringLiteral("https://api.fusionterminal.xyz");
    }
    if (base.endsWith('/')) {
        base.chop(1);
    }

    QNetworkRequest request(QUrl(base + QStringLiteral("/mods")));
    request.setRawHeader("Authorization", QByteArray("Bearer ") + m_authToken.toUtf8());
    m_modsRequestInFlight = true;
    QNetworkReply *reply = m_modsFetcher.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        m_modsRequestInFlight = false;
        const QByteArray raw = reply->readAll();
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            if (!m_installedMods.isEmpty()) {
                m_installedMods.clear();
                updateChatButtons();
            }
            return;
        }

        const QJsonDocument doc = QJsonDocument::fromJson(raw);
        if (!doc.isArray()) {
            return;
        }
        QStringList installed;
        const QJsonArray arr = doc.array();
        for (const QJsonValue &v : arr) {
            if (!v.isObject()) {
                continue;
            }
            const QJsonObject obj = v.toObject();
            if (!obj.value(QStringLiteral("installed")).toBool()) {
                continue;
            }
            const QString id = obj.value(QStringLiteral("id")).toString();
            if (!id.isEmpty()) {
                installed.push_back(id);
            }
        }
        updateInstalledMods(installed);
    });
}

void MainWindow::updateInstalledMods(const QStringList &installedMods)
{
    QSet<QString> next = QSet<QString>(installedMods.begin(), installedMods.end());
    if (next == m_installedMods) {
        return;
    }
    m_installedMods = std::move(next);
    updateChatButtons();
}

bool MainWindow::isModInstalled(const QString &modId) const
{
    return m_installedMods.contains(modId);
}

bool MainWindow::isChatModAvailable() const
{
    return !m_authToken.isEmpty() && isModInstalled(QStringLiteral("chat-ladder"));
}

void MainWindow::updateChatButtons()
{
    const bool enabled = isChatModAvailable();
    for (auto &tab : m_tabs) {
        for (auto &col : tab.columnsData) {
            if (col.chatButton) {
                col.chatButton->setVisible(enabled);
                col.chatButton->setEnabled(enabled);
            }
        }
    }
}

void MainWindow::openSettingsWindow()
{
    if (!m_settingsWindow) {
        m_settingsWindow = new SettingsWindow(this);
        m_settingsWindow->setCenterHotkey(m_centerKey, m_centerMods, m_centerAllLadders);
        m_settingsWindow->setVolumeHighlightRules(m_volumeRules);
        m_settingsWindow->setCustomHotkeys(currentCustomHotkeys());
        m_settingsWindow->setDomRefreshRate(m_domTargetFps);
        m_settingsWindow->setActiveDomOutlineEnabled(m_activeDomOutlineEnabled);
        connect(m_settingsWindow,
                &SettingsWindow::centerHotkeyChanged,
                this,
                [this](int key, Qt::KeyboardModifiers mods, bool allLadders) {
                    m_centerKey = key;
                    m_centerMods = mods;
                    m_centerAllLadders = allLadders;
                    saveUserSettings();
                });
        connect(m_settingsWindow,
                &SettingsWindow::volumeHighlightRulesChanged,
                this,
                [this](const QVector<VolumeHighlightRule> &rules) {
                    m_volumeRules = rules;
                    std::sort(m_volumeRules.begin(),
                              m_volumeRules.end(),
                              [](const VolumeHighlightRule &a, const VolumeHighlightRule &b) {
                                  return a.threshold < b.threshold;
                              });
                    applyVolumeRulesToAllDoms();
                    saveUserSettings();
                });
        connect(m_settingsWindow,
                &SettingsWindow::customHotkeyChanged,
                this,
                [this](const QString &id, int key, Qt::KeyboardModifiers mods) {
                    updateCustomHotkey(id, key, mods);
                });
        connect(m_settingsWindow,
                &SettingsWindow::domRefreshRateChanged,
                this,
                [this](int fps) {
                    applyDomFrameRate(fps);
                    saveUserSettings();
                    const QString text =
                        fps > 0 ? tr("DOM FPS: %1").arg(fps) : tr("DOM FPS: по событиям");
                    statusBar()->showMessage(text, 1800);
                });
        connect(m_settingsWindow,
                &SettingsWindow::activeDomOutlineEnabledChanged,
                this,
                [this](bool on) {
                    m_activeDomOutlineEnabled = on;
                    applyActiveDomOutline();
                    saveUserSettings();
                });
    } else {
        m_settingsWindow->setCenterHotkey(m_centerKey, m_centerMods, m_centerAllLadders);
        m_settingsWindow->setVolumeHighlightRules(m_volumeRules);
        m_settingsWindow->setCustomHotkeys(currentCustomHotkeys());
        m_settingsWindow->setDomRefreshRate(m_domTargetFps);
        m_settingsWindow->setActiveDomOutlineEnabled(m_activeDomOutlineEnabled);
    }
    m_settingsWindow->show();
    m_settingsWindow->raise();
    m_settingsWindow->activateWindow();
}

void MainWindow::updateAuthNavUi()
{
    if (!m_authNav) {
        return;
    }
    if (m_authToken.isEmpty()) {
        m_authNav->setToolTip(tr("Sign in"));
        if (m_authBadge) {
            m_authBadge->hide();
        }
    } else {
        const QString label = m_authUser.isEmpty() ? tr("Account") : tr("Account: %1").arg(m_authUser);
        m_authNav->setToolTip(label);
        if (m_authBadge) {
            const int x = std::max(0, m_authNav->width() - m_authBadge->width() - 2);
            m_authBadge->move(x, 2);
            m_authBadge->show();
        }
    }
    updateChatButtons();
}

void MainWindow::openAuthWindow()
{
    if (m_authDialog) {
        m_authDialog->show();
        m_authDialog->raise();
        m_authDialog->activateWindow();
        return;
    }

    auto *dlg = new QDialog(this);
    m_authDialog = dlg;
    dlg->setAttribute(Qt::WA_DeleteOnClose, true);
    dlg->setWindowTitle(tr("Sign in"));
    dlg->setModal(false);
    dlg->setWindowModality(Qt::NonModal);
    dlg->setMinimumWidth(460);
    dlg->setStyleSheet(QStringLiteral(
        "QDialog { background-color: #1f1f1f; color: #e0e0e0; }"
        "QLabel#AuthTitle { font-size: 22px; font-weight: 700; color: #ffffff; }"
        "QLabel#AuthHint { color: #9aa0a6; }"
        "QLabel#AuthStatus[status=\"error\"] { color: #f28b82; }"
        "QLabel#AuthStatus[status=\"ok\"] { color: #9ad29a; }"
        "QLabel#AuthStatus[status=\"info\"] { color: #9aa0a6; }"
        "QLabel#AuthSection { color: #cfd3d7; font-weight: 600; }"
        "QLineEdit {"
        "  background-color: #262626;"
        "  border: 1px solid #3a3a3a;"
        "  border-radius: 4px;"
        "  padding: 6px 10px;"
        "  color: #e6e6e6;"
        "}"
        "QLineEdit:focus { border-color: #4aa3ff; }"
        "QPushButton#AuthPrimary {"
        "  background-color: #2e7bdc;"
        "  color: #ffffff;"
        "  border: none;"
        "  border-radius: 6px;"
        "  padding: 6px 10px;"
        "  font-weight: 600;"
        "}"
        "QPushButton#AuthPrimary:hover { background-color: #3a88e5; }"
        "QPushButton#AuthSecondary {"
        "  background-color: #2a2a2a;"
        "  color: #e0e0e0;"
        "  border: 1px solid #3a3a3a;"
        "  border-radius: 4px;"
        "  padding: 5px 10px;"
        "}"
        "QPushButton#AuthSecondary:hover { background-color: #333333; }"
        "QPushButton#AuthGoogle {"
        "  background-color: #232323;"
        "  color: #e0e0e0;"
        "  border: 1px solid #2f2f2f;"
        "  border-radius: 6px;"
        "  padding: 6px 10px;"
        "}"
        "QPushButton#AuthGoogle:hover { background-color: #2a2a2a; }"
        "QToolButton#AuthLink {"
        "  color: #4aa3ff;"
        "  border: none;"
        "  padding: 0px;"
        "}"
        "QToolButton#AuthLink:hover { color: #6bb6ff; }"
    ));

    auto *layout = new QVBoxLayout(dlg);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(12);

    auto *headerRow = new QHBoxLayout();
    headerRow->setSpacing(10);
    auto *icon = new QLabel(dlg);
    icon->setFixedSize(22, 22);
    icon->setPixmap(loadIconTinted(QStringLiteral("user-circle"),
                                   QColor("#e0e0e0"),
                                   QSize(22, 22)).pixmap(22, 22));
    auto *title = new QLabel(tr("Sign in"), dlg);
    title->setObjectName(QStringLiteral("AuthTitle"));
    headerRow->addWidget(icon);
    headerRow->addWidget(title, 1);
    layout->addLayout(headerRow);

    auto *hint = new QLabel(tr("Unlock extensions & sync"), dlg);
    hint->setObjectName(QStringLiteral("AuthHint"));
    hint->setWordWrap(true);
    layout->addWidget(hint);

    auto *statusLabel = new QLabel(dlg);
    statusLabel->setObjectName(QStringLiteral("AuthStatus"));
    statusLabel->setWordWrap(true);
    statusLabel->setText(QString());
    statusLabel->setProperty("status", QStringLiteral("info"));
    layout->addWidget(statusLabel);

    auto *pages = new QStackedWidget(dlg);
    auto *signInPage = new QWidget(pages);
    auto *registerPage = new QWidget(pages);
    pages->addWidget(signInPage);
    pages->addWidget(registerPage);
    layout->addWidget(pages);

    auto *signLayout = new QVBoxLayout(signInPage);
    signLayout->setContentsMargins(0, 0, 0, 0);
    signLayout->setSpacing(10);

    const QString eyePath = resolveAssetPath(QStringLiteral("icons/outline/eye.svg"));
    const QString eyeOffPath = resolveAssetPath(QStringLiteral("icons/outline/eye-off.svg"));
    const QIcon eyeIcon = eyePath.isEmpty() ? QIcon() : QIcon(eyePath);
    const QIcon eyeOffIcon = eyeOffPath.isEmpty() ? QIcon() : QIcon(eyeOffPath);
    auto addEyeToggle = [this, eyeIcon, eyeOffIcon](QLineEdit *edit) {
        if (!edit || eyeIcon.isNull() || eyeOffIcon.isNull()) {
            return;
        }
        QAction *action = edit->addAction(eyeIcon, QLineEdit::TrailingPosition);
        action->setCheckable(true);
        action->setToolTip(tr("Show password"));
        connect(action, &QAction::toggled, edit, [edit, action, eyeIcon, eyeOffIcon, this](bool on) {
            edit->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password);
            action->setIcon(on ? eyeOffIcon : eyeIcon);
            action->setToolTip(on ? tr("Hide password") : tr("Show password"));
        });
    };

    auto *signEmailLabel = new QLabel(tr("Email or username"), signInPage);
    signEmailLabel->setObjectName(QStringLiteral("AuthSection"));
    signLayout->addWidget(signEmailLabel);
    auto *signUserEdit = new QLineEdit(signInPage);
    signUserEdit->setPlaceholderText(tr("user@example.com or login"));
    signLayout->addWidget(signUserEdit);

    auto *signPassLabel = new QLabel(tr("Password"), signInPage);
    signPassLabel->setObjectName(QStringLiteral("AuthSection"));
    signLayout->addWidget(signPassLabel);
    auto *signPassEdit = new QLineEdit(signInPage);
    signPassEdit->setPlaceholderText(tr("Password"));
    signPassEdit->setEchoMode(QLineEdit::Password);
    addEyeToggle(signPassEdit);
    signLayout->addWidget(signPassEdit);

    auto *signInPasswordBtn = new QPushButton(tr("Sign in"), signInPage);
    signInPasswordBtn->setObjectName(QStringLiteral("AuthPrimary"));
    signLayout->addWidget(signInPasswordBtn);

    auto *sepRow = new QHBoxLayout();
    auto *sepLeft = new QFrame(signInPage);
    sepLeft->setFrameShape(QFrame::HLine);
    sepLeft->setFrameShadow(QFrame::Sunken);
    auto *sepRight = new QFrame(signInPage);
    sepRight->setFrameShape(QFrame::HLine);
    sepRight->setFrameShadow(QFrame::Sunken);
    auto *sepLabel = new QLabel(tr("or"), signInPage);
    sepLabel->setObjectName(QStringLiteral("AuthHint"));
    sepRow->addWidget(sepLeft, 1);
    sepRow->addWidget(sepLabel);
    sepRow->addWidget(sepRight, 1);
    signLayout->addLayout(sepRow);

    auto *signGoogleBtn = new QPushButton(tr("Continue with Google"), signInPage);
    signGoogleBtn->setObjectName(QStringLiteral("AuthGoogle"));
    {
        const QString googleIconPath = resolveAssetPath(QStringLiteral("icons/outline/brand-google.svg"));
        if (!googleIconPath.isEmpty()) {
            signGoogleBtn->setIcon(QIcon(googleIconPath));
        }
        signGoogleBtn->setIconSize(QSize(16, 16));
    }
    signLayout->addWidget(signGoogleBtn);

    auto *advancedToggle = new QToolButton(signInPage);
    advancedToggle->setText(tr("Advanced sign-in"));
    advancedToggle->setCheckable(true);
    advancedToggle->setAutoRaise(true);
    signLayout->addWidget(advancedToggle, 0, Qt::AlignLeft);

    auto *advancedContainer = new QWidget(signInPage);
    auto *advancedLayout = new QVBoxLayout(advancedContainer);
    advancedLayout->setContentsMargins(0, 0, 0, 0);
    advancedLayout->setSpacing(8);
    advancedContainer->setVisible(false);
    signLayout->addWidget(advancedContainer);

    auto *signSep = new QLabel(tr("Code login"), signInPage);
    signSep->setObjectName(QStringLiteral("AuthHint"));
    advancedLayout->addWidget(signSep);

    auto *signChannelRow = new QHBoxLayout();
    auto *signChannelBox = new QComboBox(signInPage);
    signChannelBox->addItem(QStringLiteral("email"));
    signChannelRow->addWidget(signChannelBox, 0);
    auto *signCodeEdit = new QLineEdit(signInPage);
    signCodeEdit->setPlaceholderText(tr("Code"));
    signChannelRow->addWidget(signCodeEdit, 1);
    advancedLayout->addLayout(signChannelRow);

    auto *signCodeRow = new QHBoxLayout();
    auto *sendSignCodeBtn = new QPushButton(tr("Send code"), signInPage);
    auto *verifySignCodeBtn = new QPushButton(tr("Verify code"), signInPage);
    sendSignCodeBtn->setObjectName(QStringLiteral("AuthSecondary"));
    verifySignCodeBtn->setObjectName(QStringLiteral("AuthSecondary"));
    signCodeRow->addWidget(sendSignCodeBtn);
    signCodeRow->addWidget(verifySignCodeBtn);
    advancedLayout->addLayout(signCodeRow);

    auto *signTokenEdit = new QLineEdit(signInPage);
    signTokenEdit->setPlaceholderText(tr("Paste token (OAuth)"));
    advancedLayout->addWidget(signTokenEdit);
    auto *signUseTokenBtn = new QPushButton(tr("Use token"), signInPage);
    signUseTokenBtn->setObjectName(QStringLiteral("AuthSecondary"));
    advancedLayout->addWidget(signUseTokenBtn);

    auto *registerRow = new QHBoxLayout();
    auto *registerText = new QLabel(tr("Don't have an account?"), signInPage);
    registerText->setObjectName(QStringLiteral("AuthHint"));
    auto *registerLink = new QToolButton(signInPage);
    registerLink->setObjectName(QStringLiteral("AuthLink"));
    registerLink->setText(tr("Register"));
    registerRow->addWidget(registerText);
    registerRow->addWidget(registerLink);
    registerRow->addStretch(1);
    signLayout->addLayout(registerRow);

    auto *regLayout = new QVBoxLayout(registerPage);
    regLayout->setContentsMargins(0, 0, 0, 0);
    regLayout->setSpacing(10);

    auto *regPages = new QStackedWidget(registerPage);
    auto *regForm = new QWidget(regPages);
    auto *regVerify = new QWidget(regPages);
    regPages->addWidget(regForm);
    regPages->addWidget(regVerify);
    regLayout->addWidget(regPages);

    auto *regFormLayout = new QVBoxLayout(regForm);
    regFormLayout->setContentsMargins(0, 0, 0, 0);
    regFormLayout->setSpacing(10);

    auto *regTitle = new QLabel(tr("Create account"), regForm);
    regTitle->setObjectName(QStringLiteral("AuthTitle"));
    regFormLayout->addWidget(regTitle);
    auto *regHint = new QLabel(tr("Unlock extensions & sync"), regForm);
    regHint->setObjectName(QStringLiteral("AuthHint"));
    regFormLayout->addWidget(regHint);

    auto *regEmailLabel = new QLabel(tr("Email"), regForm);
    regEmailLabel->setObjectName(QStringLiteral("AuthSection"));
    regFormLayout->addWidget(regEmailLabel);
    auto *regUserEdit = new QLineEdit(regForm);
    regUserEdit->setPlaceholderText(tr("user@example.com"));
    regFormLayout->addWidget(regUserEdit);

    auto *regLoginLabel = new QLabel(tr("Username"), regForm);
    regLoginLabel->setObjectName(QStringLiteral("AuthSection"));
    regFormLayout->addWidget(regLoginLabel);
    auto *regLoginEdit = new QLineEdit(regForm);
    regLoginEdit->setPlaceholderText(tr("your_login"));
    regFormLayout->addWidget(regLoginEdit);

    auto *regPassLabel = new QLabel(tr("Password"), regForm);
    regPassLabel->setObjectName(QStringLiteral("AuthSection"));
    regFormLayout->addWidget(regPassLabel);
    auto *regPassEdit = new QLineEdit(regForm);
    regPassEdit->setPlaceholderText(tr("Password"));
    regPassEdit->setEchoMode(QLineEdit::Password);
    addEyeToggle(regPassEdit);
    regFormLayout->addWidget(regPassEdit);

    auto *regPassConfirmLabel = new QLabel(tr("Confirm password"), regForm);
    regPassConfirmLabel->setObjectName(QStringLiteral("AuthSection"));
    regFormLayout->addWidget(regPassConfirmLabel);
    auto *regPassConfirm = new QLineEdit(regForm);
    regPassConfirm->setPlaceholderText(tr("Confirm password"));
    regPassConfirm->setEchoMode(QLineEdit::Password);
    addEyeToggle(regPassConfirm);
    regFormLayout->addWidget(regPassConfirm);

    auto *regStartBtn = new QPushButton(tr("Create account"), regForm);
    regStartBtn->setObjectName(QStringLiteral("AuthPrimary"));
    regFormLayout->addWidget(regStartBtn);
    auto *regGoVerifyBtn = new QToolButton(regForm);
    regGoVerifyBtn->setObjectName(QStringLiteral("AuthLink"));
    regGoVerifyBtn->setText(tr("Enter code"));
    regGoVerifyBtn->setAutoRaise(true);
    regGoVerifyBtn->setVisible(false);
    regFormLayout->addWidget(regGoVerifyBtn, 0, Qt::AlignLeft);

    auto *regVerifyLayout = new QVBoxLayout(regVerify);
    regVerifyLayout->setContentsMargins(0, 0, 0, 0);
    regVerifyLayout->setSpacing(10);
    auto *verifyTitle = new QLabel(tr("Confirm email"), regVerify);
    verifyTitle->setObjectName(QStringLiteral("AuthTitle"));
    regVerifyLayout->addWidget(verifyTitle);
    auto *verifyHint = new QLabel(tr("Enter the code sent to your email."), regVerify);
    verifyHint->setObjectName(QStringLiteral("AuthHint"));
    verifyHint->setWordWrap(true);
    regVerifyLayout->addWidget(verifyHint);
    auto *regCodeEdit = new QLineEdit(regVerify);
    regCodeEdit->setPlaceholderText(tr("Code"));
    regVerifyLayout->addWidget(regCodeEdit);
    auto *verifyRow = new QHBoxLayout();
    auto *resendRegCodeBtn = new QPushButton(tr("Resend code"), regVerify);
    resendRegCodeBtn->setObjectName(QStringLiteral("AuthSecondary"));
    auto *verifyRegBtn = new QPushButton(tr("Verify email"), regVerify);
    verifyRegBtn->setObjectName(QStringLiteral("AuthPrimary"));
    verifyRow->addWidget(resendRegCodeBtn);
    verifyRow->addWidget(verifyRegBtn);
    regVerifyLayout->addLayout(verifyRow);

    auto *backRow = new QHBoxLayout();
    auto *backText = new QLabel(tr("Already have an account?"), registerPage);
    backText->setObjectName(QStringLiteral("AuthHint"));
    auto *backLink = new QToolButton(registerPage);
    backLink->setObjectName(QStringLiteral("AuthLink"));
    backLink->setText(tr("Sign in"));
    backRow->addWidget(backText);
    backRow->addWidget(backLink);
    backRow->addStretch(1);
    regLayout->addLayout(backRow);

    auto *buttons = new QHBoxLayout();
    buttons->addStretch(1);
    auto *close = new QPushButton(tr("Close"), dlg);
    close->setObjectName(QStringLiteral("AuthSecondary"));
    buttons->addWidget(close);
    layout->addLayout(buttons);

    auto *net = new QNetworkAccessManager(dlg);
    auto normalizeError = [](const QString &msg) -> QString {
        const QString lower = msg.trimmed().toLower();
        if (lower == QStringLiteral("email_taken")) return QStringLiteral("Эта почта уже занята.");
        if (lower == QStringLiteral("login_taken")) return QStringLiteral("Это имя уже занято.");
        if (lower == QStringLiteral("user_exists")) return QStringLiteral("Пользователь уже существует.");
        if (lower == QStringLiteral("user exists")) return QStringLiteral("Пользователь уже существует.");
        if (lower == QStringLiteral("registration_not_found")) return QStringLiteral("Регистрация не найдена. Запросите код заново.");
        if (lower == QStringLiteral("invalid credentials")) return QStringLiteral("Неверная почта или пароль.");
        if (lower == QStringLiteral("email not verified")) return QStringLiteral("Почта не подтверждена.");
        if (lower == QStringLiteral("user not found")) return QStringLiteral("Пользователь не найден.");
        if (lower == QStringLiteral("send failed")) return QStringLiteral("Не удалось отправить код.");
        if (lower == QStringLiteral("verify failed")) return QStringLiteral("Проверка кода не удалась.");
        if (lower == QStringLiteral("register failed")) return QStringLiteral("Не удалось создать аккаунт.");
        if (lower == QStringLiteral("login failed.")) return QStringLiteral("Не удалось войти.");
        if (lower == QStringLiteral("invalid code")) return QStringLiteral("Неверный код.");
        if (lower == QStringLiteral("code expired")) return QStringLiteral("Срок действия кода истек.");
        if (lower == QStringLiteral("code used")) return QStringLiteral("Код уже использован.");
        if (lower == QStringLiteral("missing credentials")) return QStringLiteral("Введите логин и пароль.");
        if (lower == QStringLiteral("missing email or code")) return QStringLiteral("Введите почту и код.");
        if (lower == QStringLiteral("invalid email")) return QStringLiteral("Неверный формат почты.");
        if (lower == QStringLiteral("password too short")) return QStringLiteral("Пароль слишком короткий.");
        if (lower == QStringLiteral("login required")) return QStringLiteral("Введите имя пользователя.");
        if (lower == QStringLiteral("email required")) return QStringLiteral("Введите почту.");
        if (lower == QStringLiteral("unsupported channel")) return QStringLiteral("Неподдерживаемый канал.");
        if (lower == QStringLiteral("invalid json")) return QStringLiteral("Некорректные данные запроса.");
        return msg.trimmed();
    };
    auto setStatus = [this, statusLabel, normalizeError](const QString &msg, const QString &kind) {
        const QString text = normalizeError(msg);
        if (statusBar()) {
            statusBar()->showMessage(text, 3500);
        }
        if (statusLabel) {
            statusLabel->setProperty("status", kind);
            statusLabel->style()->unpolish(statusLabel);
            statusLabel->style()->polish(statusLabel);
            statusLabel->setText(text);
        }
    };
    auto resolveBaseUrl = [this]() -> QString {
        QString base = m_authBaseUrl.trimmed();
        if (base.isEmpty()) base = QStringLiteral("https://api.fusionterminal.xyz");
        if (base.endsWith('/')) {
            base.chop(1);
        }
        return base;
    };
    auto postJson = [net, resolveBaseUrl](const QString &path, const QJsonObject &obj) -> QNetworkReply * {
        const QString base = resolveBaseUrl();
        if (base.isEmpty()) {
            return nullptr;
        }
        QNetworkRequest req(QUrl(base + path));
        req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        return net->post(req, QJsonDocument(obj).toJson(QJsonDocument::Compact));
    };
    auto applyToken = [this, resolveBaseUrl](const QString &token,
                                             const QString &role,
                                             const QString &user) {
        m_authBaseUrl = resolveBaseUrl();
        m_authToken = token;
        m_authRole = role;
        m_authUser = user;
        saveUserSettings();
        updateAuthNavUi();
        requestInstalledMods();
    };

    connect(close, &QPushButton::clicked, dlg, &QDialog::close);
    connect(advancedToggle, &QToolButton::toggled, this, [advancedContainer](bool on) {
        advancedContainer->setVisible(on);
    });
    connect(registerLink, &QToolButton::clicked, this, [pages, regPages]() {
        regPages->setCurrentIndex(0);
        pages->setCurrentIndex(1);
    });
    connect(backLink, &QToolButton::clicked, this, [pages, regPages]() {
        regPages->setCurrentIndex(0);
        pages->setCurrentIndex(0);
    });
    connect(regGoVerifyBtn, &QToolButton::clicked, this, [regPages]() {
        regPages->setCurrentIndex(1);
    });

    connect(signInPasswordBtn, &QPushButton::clicked, this, [=]() {
        const QString base = resolveBaseUrl();
        if (base.isEmpty()) {
            setStatus(tr("Сервер недоступен."), QStringLiteral("error"));
            return;
        }
        const QString loginVal = signUserEdit->text().trimmed();
        const QString passVal = signPassEdit->text();
        if (loginVal.isEmpty()) {
            setStatus(tr("Введите почту или логин."), QStringLiteral("error"));
            return;
        }
        if (passVal.isEmpty()) {
            setStatus(tr("Введите пароль."), QStringLiteral("error"));
            return;
        }
        setStatus(tr("Входим..."), QStringLiteral("info"));
        QJsonObject obj;
        obj.insert(QStringLiteral("email_or_login"), loginVal);
        obj.insert(QStringLiteral("password"), passVal);
        QNetworkReply *reply = postJson(QStringLiteral("/auth/login"), obj);
        if (!reply) {
            setStatus(tr("Ошибка запроса авторизации."), QStringLiteral("error"));
            return;
        }
        connect(reply, &QNetworkReply::finished, this, [reply, applyToken, setStatus]() {
            const QByteArray raw = reply->readAll();
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) {
                const QJsonDocument doc = QJsonDocument::fromJson(raw);
                const QString errMsg = doc.isObject()
                    ? doc.object().value(QStringLiteral("error")).toString()
                    : QString::fromUtf8(raw).trimmed();
                setStatus(errMsg.isEmpty() ? tr("Не удалось войти.") : errMsg, QStringLiteral("error"));
                return;
            }
            const QJsonDocument doc = QJsonDocument::fromJson(raw);
            if (!doc.isObject()) {
                applyToken(QString::fromUtf8(raw).trimmed(), QString(), QString());
                setStatus(tr("Вход выполнен."), QStringLiteral("ok"));
                return;
            }
            const QJsonObject obj = doc.object();
            const QString token = obj.value(QStringLiteral("token")).toString();
            const QString role = obj.value(QStringLiteral("role")).toString();
            const QString user = obj.value(QStringLiteral("user")).toString();
            if (token.isEmpty()) {
                setStatus(tr("Не удалось войти."), QStringLiteral("error"));
                return;
            }
            applyToken(token, role, user);
            setStatus(tr("Вход выполнен."), QStringLiteral("ok"));
        });
    });

    connect(sendSignCodeBtn, &QPushButton::clicked, this, [=]() {
        const QString base = resolveBaseUrl();
        if (base.isEmpty()) {
            setStatus(tr("Сервер недоступен."), QStringLiteral("error"));
            return;
        }
        const QString loginVal = signUserEdit->text().trimmed();
        if (loginVal.isEmpty()) {
            setStatus(tr("Введите почту или логин."), QStringLiteral("error"));
            return;
        }
        setStatus(tr("Отправляем код..."), QStringLiteral("info"));
        QJsonObject obj;
        obj.insert(QStringLiteral("email_or_login"), loginVal);
        obj.insert(QStringLiteral("channel"), signChannelBox->currentText().trimmed());
        QNetworkReply *reply = postJson(QStringLiteral("/auth/send_code"), obj);
        if (!reply) {
            setStatus(tr("Не удалось отправить код."), QStringLiteral("error"));
            return;
        }
        connect(reply, &QNetworkReply::finished, this, [reply, setStatus]() {
            const QByteArray raw = reply->readAll();
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) {
                const QJsonDocument doc = QJsonDocument::fromJson(raw);
                const QString errMsg = doc.isObject()
                    ? doc.object().value(QStringLiteral("error")).toString()
                    : QString::fromUtf8(raw).trimmed();
                setStatus(errMsg.isEmpty() ? tr("Не удалось отправить код.") : errMsg, QStringLiteral("error"));
                return;
            }
            setStatus(tr("Код отправлен."), QStringLiteral("ok"));
        });
    });

    connect(verifySignCodeBtn, &QPushButton::clicked, this, [=]() {
        const QString base = resolveBaseUrl();
        if (base.isEmpty()) {
            setStatus(tr("Сервер недоступен."), QStringLiteral("error"));
            return;
        }
        const QString loginVal = signUserEdit->text().trimmed();
        const QString codeVal = signCodeEdit->text().trimmed();
        if (loginVal.isEmpty()) {
            setStatus(tr("Введите почту или логин."), QStringLiteral("error"));
            return;
        }
        if (codeVal.isEmpty()) {
            setStatus(tr("Введите код."), QStringLiteral("error"));
            return;
        }
        setStatus(tr("Проверяем код..."), QStringLiteral("info"));
        QJsonObject obj;
        obj.insert(QStringLiteral("email_or_login"), loginVal);
        obj.insert(QStringLiteral("code"), codeVal);
        obj.insert(QStringLiteral("flow"), QStringLiteral("login"));
        QNetworkReply *reply = postJson(QStringLiteral("/auth/verify_code"), obj);
        if (!reply) {
            setStatus(tr("Не удалось проверить код."), QStringLiteral("error"));
            return;
        }
        connect(reply, &QNetworkReply::finished, this, [reply, applyToken, setStatus]() {
            const QByteArray raw = reply->readAll();
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) {
                const QJsonDocument doc = QJsonDocument::fromJson(raw);
                const QString errMsg = doc.isObject()
                    ? doc.object().value(QStringLiteral("error")).toString()
                    : QString::fromUtf8(raw).trimmed();
                setStatus(errMsg.isEmpty() ? tr("Не удалось проверить код.") : errMsg, QStringLiteral("error"));
                return;
            }
            const QJsonDocument doc = QJsonDocument::fromJson(raw);
            if (!doc.isObject()) {
                applyToken(QString::fromUtf8(raw).trimmed(), QString(), QString());
                setStatus(tr("Вход выполнен."), QStringLiteral("ok"));
                return;
            }
            const QJsonObject obj = doc.object();
            const QString token = obj.value(QStringLiteral("token")).toString();
            const QString role = obj.value(QStringLiteral("role")).toString();
            const QString user = obj.value(QStringLiteral("user")).toString();
            if (token.isEmpty()) {
                setStatus(tr("Не удалось проверить код."), QStringLiteral("error"));
                return;
            }
            applyToken(token, role, user);
            setStatus(tr("Вход выполнен."), QStringLiteral("ok"));
        });
    });

    connect(signGoogleBtn, &QPushButton::clicked, this, [=]() {
        const QString base = resolveBaseUrl();
        if (base.isEmpty()) {
            setStatus(tr("Сервер недоступен."), QStringLiteral("error"));
            return;
        }
        QDesktopServices::openUrl(QUrl(base + QStringLiteral("/auth/google")));
    });

    connect(signUseTokenBtn, &QPushButton::clicked, this, [=]() {
        const QString tok = signTokenEdit->text().trimmed();
        if (tok.isEmpty()) {
            setStatus(tr("Введите токен."), QStringLiteral("error"));
            return;
        }
        applyToken(tok, QString(), signUserEdit->text().trimmed());
        setStatus(tr("Вход выполнен."), QStringLiteral("ok"));
    });

    connect(resendRegCodeBtn, &QPushButton::clicked, this, [=]() {
        const QString base = resolveBaseUrl();
        if (base.isEmpty()) {
            setStatus(tr("Сервер недоступен."), QStringLiteral("error"));
            return;
        }
        const QString regEmail = regUserEdit->text().trimmed();
        if (regEmail.isEmpty()) {
            setStatus(tr("Введите почту."), QStringLiteral("error"));
            return;
        }
        setStatus(tr("Отправляем код..."), QStringLiteral("info"));
        QJsonObject obj;
        obj.insert(QStringLiteral("email_or_login"), regEmail);
        obj.insert(QStringLiteral("channel"), QStringLiteral("email"));
        QNetworkReply *reply = postJson(QStringLiteral("/auth/send_code"), obj);
        if (!reply) {
            setStatus(tr("Не удалось отправить код."), QStringLiteral("error"));
            return;
        }
        connect(reply, &QNetworkReply::finished, this, [reply, setStatus]() {
            const QByteArray raw = reply->readAll();
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) {
                const QJsonDocument doc = QJsonDocument::fromJson(raw);
                const QString errMsg = doc.isObject()
                    ? doc.object().value(QStringLiteral("error")).toString()
                    : QString::fromUtf8(raw).trimmed();
                setStatus(errMsg.isEmpty() ? tr("Не удалось отправить код.") : errMsg, QStringLiteral("error"));
                return;
            }
            setStatus(tr("Код отправлен."), QStringLiteral("ok"));
        });
    });

    connect(regStartBtn, &QPushButton::clicked, this, [=]() {
        const QString base = resolveBaseUrl();
        if (base.isEmpty()) {
            setStatus(tr("Сервер недоступен."), QStringLiteral("error"));
            return;
        }
        const QString regEmail = regUserEdit->text().trimmed();
        const QString regLogin = regLoginEdit->text().trimmed();
        const QString regPass = regPassEdit->text();
        const QString regPass2 = regPassConfirm->text();
        if (regEmail.isEmpty()) {
            setStatus(tr("Введите почту."), QStringLiteral("error"));
            return;
        }
        if (regLogin.isEmpty()) {
            setStatus(tr("Введите имя пользователя."), QStringLiteral("error"));
            return;
        }
        if (regPass.isEmpty()) {
            setStatus(tr("Введите пароль."), QStringLiteral("error"));
            return;
        }
        if (regPass != regPass2) {
            setStatus(tr("Пароли не совпадают."), QStringLiteral("error"));
            return;
        }
        setStatus(tr("Создаем аккаунт..."), QStringLiteral("info"));
        QJsonObject reg;
        reg.insert(QStringLiteral("email"), regEmail);
        reg.insert(QStringLiteral("login"), regLogin);
        reg.insert(QStringLiteral("email_or_login"), regEmail);
        reg.insert(QStringLiteral("password"), regPass);
        QNetworkReply *registerReply = postJson(QStringLiteral("/auth/register"), reg);
        if (!registerReply) {
            setStatus(tr("Не удалось создать аккаунт."), QStringLiteral("error"));
            return;
        }
        connect(registerReply, &QNetworkReply::finished, this, [=]() {
            const QByteArray raw = registerReply->readAll();
            registerReply->deleteLater();
            if (registerReply->error() != QNetworkReply::NoError) {
                const QJsonDocument doc = QJsonDocument::fromJson(raw);
                const QString errMsg = doc.isObject()
                    ? doc.object().value(QStringLiteral("error")).toString()
                    : QString::fromUtf8(raw).trimmed();
                setStatus(errMsg.isEmpty() ? tr("Не удалось создать аккаунт.") : errMsg, QStringLiteral("error"));
                return;
            }
            regGoVerifyBtn->setVisible(true);
            setStatus(tr("Код отправлен. Перейдите к подтверждению."), QStringLiteral("ok"));
        });
    });

    connect(verifyRegBtn, &QPushButton::clicked, this, [=]() {
        const QString base = resolveBaseUrl();
        if (base.isEmpty()) {
            setStatus(tr("Сервер недоступен."), QStringLiteral("error"));
            return;
        }
        const QString regEmail = regUserEdit->text().trimmed();
        const QString regLogin = regLoginEdit->text().trimmed();
        const QString codeVal = regCodeEdit->text().trimmed();
        if (regEmail.isEmpty()) {
            setStatus(tr("Введите почту."), QStringLiteral("error"));
            return;
        }
        if (codeVal.isEmpty()) {
            setStatus(tr("Введите код."), QStringLiteral("error"));
            return;
        }
        setStatus(tr("Проверяем код..."), QStringLiteral("info"));
        QJsonObject obj;
        obj.insert(QStringLiteral("email_or_login"), regEmail);
        obj.insert(QStringLiteral("email"), regEmail);
        obj.insert(QStringLiteral("login"), regLogin);
        obj.insert(QStringLiteral("code"), codeVal);
        obj.insert(QStringLiteral("flow"), QStringLiteral("register"));
        QNetworkReply *reply = postJson(QStringLiteral("/auth/verify_code"), obj);
        if (!reply) {
            setStatus(tr("Не удалось проверить код."), QStringLiteral("error"));
            return;
        }
        connect(reply, &QNetworkReply::finished, this, [reply, applyToken, setStatus, dlg]() {
            const QByteArray raw = reply->readAll();
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) {
                const QJsonDocument doc = QJsonDocument::fromJson(raw);
                const QString errMsg = doc.isObject()
                    ? doc.object().value(QStringLiteral("error")).toString()
                    : QString::fromUtf8(raw).trimmed();
                setStatus(errMsg.isEmpty() ? tr("Не удалось проверить код.") : errMsg, QStringLiteral("error"));
                return;
            }
            const QJsonDocument doc = QJsonDocument::fromJson(raw);
            if (!doc.isObject()) {
                applyToken(QString::fromUtf8(raw).trimmed(), QString(), QString());
                setStatus(tr("Аккаунт создан."), QStringLiteral("ok"));
                dlg->close();
                return;
            }
            const QJsonObject obj = doc.object();
            const QString token = obj.value(QStringLiteral("token")).toString();
            const QString role = obj.value(QStringLiteral("role")).toString();
            const QString user = obj.value(QStringLiteral("user")).toString();
            if (token.isEmpty()) {
                setStatus(tr("Не удалось проверить код."), QStringLiteral("error"));
                return;
            }
            applyToken(token, role, user);
            setStatus(tr("Аккаунт создан."), QStringLiteral("ok"));
            dlg->close();
        });
    });

    connect(dlg, &QDialog::finished, this, [this]() {
        m_authDialog = nullptr;
    });

    dlg->show();
    dlg->raise();
    dlg->activateWindow();
}

bool MainWindow::matchesSltpHotkey(int eventKey, Qt::KeyboardModifiers eventMods) const
{
    // Accept a bit more loosely than matchesHotkey() because Windows/IME/layout switching can
    // inject extra modifiers (e.g. GroupSwitch), and for "hold" actions it is better to
    // show the overlay than miss it.
    Qt::KeyboardModifiers cleaned = eventMods & ~Qt::KeypadModifier;
    cleaned &= ~Qt::GroupSwitchModifier;

    if (m_sltpPlaceKey != 0 && eventKey == m_sltpPlaceKey) {
        if (m_sltpPlaceMods == Qt::NoModifier) {
            // Allow Shift-only (e.g. user typed uppercase) but not Ctrl/Alt/Meta combos.
            const Qt::KeyboardModifiers disallowed =
                Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier;
            if ((cleaned & disallowed) == Qt::NoModifier) {
                return true;
            }
        } else if (cleaned == m_sltpPlaceMods) {
            return true;
        }
    }

    // Fallback: treat Latin 'C' and Cyrillic 'С/с' as equivalent when no modifiers are required.
    if (matchesHotkey(eventKey, eventMods, m_sltpPlaceKey, m_sltpPlaceMods)) {
        return true;
    }
    if (m_sltpPlaceMods == Qt::NoModifier && cleaned == Qt::NoModifier) {
        static constexpr int kCyrillicEsUpper = 0x0421; // 'С'
        static constexpr int kCyrillicEsLower = 0x0441; // 'с'
        const bool eventIsCyrillicEs = (eventKey == kCyrillicEsUpper || eventKey == kCyrillicEsLower);
        const bool configIsCyrillicEs = (m_sltpPlaceKey == kCyrillicEsUpper || m_sltpPlaceKey == kCyrillicEsLower);
        if (m_sltpPlaceKey == Qt::Key_C && eventIsCyrillicEs) {
            return true;
        }
        if (configIsCyrillicEs && eventKey == Qt::Key_C) {
            return true;
        }
    }
    return false;
}

MainWindow::DomColumn *MainWindow::activeColumnForSltpHotkey()
{
    if (auto *focus = QApplication::focusWidget()) {
        if (QWidget *container = columnContainerForObject(focus)) {
            WorkspaceTab *tab = nullptr;
            DomColumn *col = nullptr;
            int idx = -1;
            if (locateColumn(container, tab, col, idx) && col) {
                return col;
            }
        }
    }

    QWidget *active = QApplication::activeWindow();
    if (active) {
        if (active == this) {
            return focusedDomColumn();
        }
        for (auto &tab : m_tabs) {
            for (auto &col : tab.columnsData) {
                if (col.floatingWindow == active) {
                    return &col;
                }
            }
        }
    }

    return focusedDomColumn();
}

bool MainWindow::handleSltpKeyPress(QKeyEvent *event)
{
    if (!event) {
        return false;
    }
    if (m_notionalEditActive) {
        return false;
    }
    bool match = matchesSltpHotkey(event->key(), event->modifiers());
    if (!match) {
        const Qt::KeyboardModifiers cleaned = event->modifiers() & ~Qt::KeypadModifier;
        if (m_sltpPlaceMods == Qt::NoModifier && cleaned == Qt::NoModifier) {
            const QString t = event->text().trimmed().toLower();
            static const QString kLatinC = QStringLiteral("c");
            static const QString kCyrillicEs = QString(QChar(0x0441)); // 'с'
            if (t == kLatinC || t == kCyrillicEs) {
                static constexpr int kCyrillicEsUpper = 0x0421; // 'С'
                static constexpr int kCyrillicEsLower = 0x0441; // 'с'
                const bool configIsCyrillicEs =
                    (m_sltpPlaceKey == kCyrillicEsUpper || m_sltpPlaceKey == kCyrillicEsLower);
                if (m_sltpPlaceKey == Qt::Key_C || configIsCyrillicEs) {
                    match = true;
                }
            }
        }
    }
    if (!match) {
        return false;
    }

    if (event->isAutoRepeat()) {
        event->accept();
        return true;
    }

    setSltpHoldUi(true);
    event->accept();
    return true;

    const QString overlayText = tr("выставить SL/TP заявку");

    // Clear any previous overlay first.
    for (auto &tab : m_tabs) {
        for (auto &col : tab.columnsData) {
            if (col.dom) {
                col.dom->setActionOverlayText(QString());
            }
        }
    }

    m_sltpPlaceHeld = true;
    m_sltpPlaceDom = nullptr;

    QWidget *active = QApplication::activeWindow();
    if (active && active != this) {
        // Floating ladder window: show on that one only.
        for (auto &tab : m_tabs) {
            for (auto &col : tab.columnsData) {
                if (col.floatingWindow == active && col.dom) {
                    col.dom->setActionOverlayText(overlayText);
                    m_sltpPlaceDom = col.dom;
                    event->accept();
                    return true;
                }
            }
        }
    }

    // Main window: show on all visible columns in the current tab so the user definitely sees it.
    if (auto *tab = currentWorkspaceTab()) {
        for (auto &col : tab->columnsData) {
            if (col.dom) {
                col.dom->setActionOverlayText(overlayText);
            }
        }
    }

    // Track a preferred DOM for click-to-place (best-effort).
    if (DomColumn *focused = focusedDomColumn()) {
        m_sltpPlaceDom = focused->dom;
    }
    if (statusBar()) {
        statusBar()->showMessage(tr("SL/TP: режим установки"), 1200);
    }
    event->accept();
    return true;
}

bool MainWindow::handleSltpKeyRelease(QKeyEvent *event)
{
    if (!event) {
        return false;
    }
    bool match = matchesSltpHotkey(event->key(), event->modifiers());
    if (!match) {
        const Qt::KeyboardModifiers cleaned = event->modifiers() & ~Qt::KeypadModifier;
        if (m_sltpPlaceMods == Qt::NoModifier && cleaned == Qt::NoModifier) {
            const QString t = event->text().trimmed().toLower();
            static const QString kLatinC = QStringLiteral("c");
            static const QString kCyrillicEs = QString(QChar(0x0441)); // 'с'
            if (t == kLatinC || t == kCyrillicEs) {
                static constexpr int kCyrillicEsUpper = 0x0421; // 'С'
                static constexpr int kCyrillicEsLower = 0x0441; // 'с'
                const bool configIsCyrillicEs =
                    (m_sltpPlaceKey == kCyrillicEsUpper || m_sltpPlaceKey == kCyrillicEsLower);
                if (m_sltpPlaceKey == Qt::Key_C || configIsCyrillicEs) {
                    match = true;
                }
            }
        }
    }
    if (!match) {
        return false;
    }
    if (event->isAutoRepeat()) {
        return true;
    }

    setSltpHoldUi(false);
    return true;
    m_sltpPlaceHeld = false;
    for (auto &tab : m_tabs) {
        for (auto &col : tab.columnsData) {
            if (col.dom) {
                col.dom->setActionOverlayText(QString());
            }
        }
    }
    m_sltpPlaceDom = nullptr;
    return true;
}

void MainWindow::setSltpHoldUi(bool held)
{
    if (held == m_sltpPlaceHeld) {
        return;
    }

    // Always clear previous overlays first to avoid ghost text.
    for (auto &tab : m_tabs) {
        for (auto &col : tab.columnsData) {
            if (col.dom) {
                col.dom->setActionOverlayText(QString());
            }
        }
    }

    m_sltpPlaceHeld = held;
    m_sltpPlaceDom = nullptr;
    if (!held) {
        return;
    }

    const QString overlayText = tr("выставить SL-TP заявку");

    QWidget *active = QApplication::activeWindow();
    if (active && active != this) {
        for (auto &tab : m_tabs) {
            for (auto &col : tab.columnsData) {
                if (col.floatingWindow == active && col.dom) {
                    col.dom->setActionOverlayText(overlayText);
                    m_sltpPlaceDom = col.dom;
                    return;
                }
            }
        }
    }

    if (auto *tab = currentWorkspaceTab()) {
        for (auto &col : tab->columnsData) {
            if (col.dom) {
                col.dom->setActionOverlayText(overlayText);
            }
        }
    }

    if (DomColumn *focused = focusedDomColumn()) {
        m_sltpPlaceDom = focused->dom;
    }
}

void MainWindow::pollSltpHoldKey()
{
#ifdef Q_OS_WIN
    if (QGuiApplication::applicationState() != Qt::ApplicationActive) {
        if (m_sltpPlaceHeld) {
            setSltpHoldUi(false);
        }
        return;
    }

    auto vkDown = [](int vk) -> bool {
        return (GetAsyncKeyState(vk) & 0x8000) != 0;
    };
    auto qtKeyToVk = [](int qtKey) -> int {
        static constexpr int kCyrillicEsUpper = 0x0421; // 'С'
        static constexpr int kCyrillicEsLower = 0x0441; // 'с'
        if (qtKey == kCyrillicEsUpper || qtKey == kCyrillicEsLower) {
            return 'C';
        }
        if (qtKey >= Qt::Key_A && qtKey <= Qt::Key_Z) {
            return 'A' + (qtKey - Qt::Key_A);
        }
        if (qtKey >= Qt::Key_0 && qtKey <= Qt::Key_9) {
            return '0' + (qtKey - Qt::Key_0);
        }
        switch (qtKey) {
        case Qt::Key_Tab:
        case Qt::Key_Backtab:
            return VK_TAB;
        case Qt::Key_Space:
            return VK_SPACE;
        case Qt::Key_Escape:
            return VK_ESCAPE;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            return VK_RETURN;
        default:
            return 0;
        }
    };

    const int vk = qtKeyToVk(m_sltpPlaceKey);
    if (vk == 0) {
        return;
    }

    const bool keyDown = vkDown(vk);
    const bool ctrlDown = vkDown(VK_CONTROL);
    const bool altDown = vkDown(VK_MENU);
    const bool metaDown = vkDown(VK_LWIN) || vkDown(VK_RWIN);
    const bool shiftDown = vkDown(VK_SHIFT);

    const bool requireCtrl = m_sltpPlaceMods.testFlag(Qt::ControlModifier);
    const bool requireAlt = m_sltpPlaceMods.testFlag(Qt::AltModifier);
    const bool requireMeta = m_sltpPlaceMods.testFlag(Qt::MetaModifier);
    const bool requireShift = m_sltpPlaceMods.testFlag(Qt::ShiftModifier);

    bool modsOk = true;
    if (requireCtrl && !ctrlDown) modsOk = false;
    if (requireAlt && !altDown) modsOk = false;
    if (requireMeta && !metaDown) modsOk = false;
    if (requireShift && !shiftDown) modsOk = false;

    if (!requireCtrl && ctrlDown) modsOk = false;
    if (!requireAlt && altDown) modsOk = false;
    if (!requireMeta && metaDown) modsOk = false;
    // Shift is allowed even when not required (so uppercase doesn't break "hold").

    const bool held = keyDown && modsOk;
    if (held != m_sltpPlaceHeld) {
        setSltpHoldUi(held);
    }
#endif
}

void MainWindow::toggleAlertsPanel()
{
    if (!m_alertsPanel) {
        return;
    }

    if (m_alertsPanelAnim) {
        m_alertsPanelAnim->stop();
        m_alertsPanelAnim->deleteLater();
        m_alertsPanelAnim = nullptr;
    }

    const bool wantShow = !m_alertsPanelExpanded;
    m_alertsPanelExpanded = wantShow;

    repositionAlertsPanel();
    const QRect targetRect = m_alertsPanelTargetRect;
    QRect hiddenRect = targetRect;
    hiddenRect.moveLeft(-targetRect.width() - 12);

    if (wantShow) {
        m_alertsPanel->setGeometry(hiddenRect);
        m_alertsPanel->show();
        m_alertsPanel->raise();
        markAllNotificationsRead();
    }

    const QRect startRect = m_alertsPanel->geometry();
    const QRect endRect = wantShow ? targetRect : hiddenRect;
    m_alertsPanelAnim = new QPropertyAnimation(m_alertsPanel, "geometry", this);
    m_alertsPanelAnim->setDuration(wantShow ? 220 : 170);
    m_alertsPanelAnim->setEasingCurve(wantShow ? QEasingCurve::OutCubic : QEasingCurve::InCubic);
    m_alertsPanelAnim->setStartValue(startRect);
    m_alertsPanelAnim->setEndValue(endRect);
    connect(m_alertsPanelAnim, &QPropertyAnimation::finished, this, [this, wantShow]() {
        if (!m_alertsPanel) {
            return;
        }
        if (!wantShow) {
            m_alertsPanel->hide();
        }
        if (m_alertsPanelAnim) {
            m_alertsPanelAnim->deleteLater();
            m_alertsPanelAnim = nullptr;
        }
    });
    m_alertsPanelAnim->start();
}

bool MainWindow::eventFilter(QObject *obj, QEvent *event)
{
    if (event && (event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease
                  || event->type() == QEvent::ShortcutOverride)) {
        auto *ke = static_cast<QKeyEvent *>(event);
        auto isTextInput = [](QObject *target) -> bool {
            if (target && (qobject_cast<QLineEdit *>(target)
                           || qobject_cast<QPlainTextEdit *>(target)
                           || qobject_cast<QTextEdit *>(target)
                           || qobject_cast<QAbstractSpinBox *>(target))) {
                return true;
            }
            QWidget *fw = QApplication::focusWidget();
            if (fw && (qobject_cast<QLineEdit *>(fw)
                       || qobject_cast<QPlainTextEdit *>(fw)
                       || qobject_cast<QTextEdit *>(fw)
                       || qobject_cast<QAbstractSpinBox *>(fw))) {
                return true;
            }
            return false;
        };
        if (isTextInput(obj)) {
            return false;
        }
        if (event->type() == QEvent::KeyRelease) {
            if (handleSltpKeyRelease(ke)) {
                ke->accept();
                return true;
            }
        } else {
            // Some keys (notably Tab) can be consumed by focus navigation and never reach
            // MainWindow::keyPressEvent; handle them here, including ShortcutOverride.
            if (handleSltpKeyPress(ke)) {
                ke->accept();
                return true;
            }
            if (event->type() == QEvent::KeyPress) {
                bool cancelMatch = matchesHotkey(ke->key(), ke->modifiers(),
                                                 m_cancelLastLimitKey, m_cancelLastLimitMods);
                if (!cancelMatch && m_cancelLastLimitMods == Qt::NoModifier
                    && ke->modifiers() == Qt::NoModifier
                    && m_cancelLastLimitKey == Qt::Key_G) {
                    const QString text = ke->text().toLower();
                    if (text == QStringLiteral("g") || text == QStringLiteral("\u043F")) {
                        cancelMatch = true;
                    }
#if defined(Q_OS_WIN)
                    if (!cancelMatch && ke->nativeVirtualKey() == 0x47) {
                        cancelMatch = true;
                    }
#endif
                }
                if (cancelMatch) {
                    keyPressEvent(ke);
                    ke->accept();
                    return true;
                }
            }
        }
    }

    if (event && (event->type() == QEvent::UngrabMouse || event->type() == QEvent::WindowDeactivate)) {
        if (m_domDragGrabber || m_draggingDomContainer || m_domDragActive || m_pendingTickerLabel) {
            if (QApplication::mouseButtons().testFlag(Qt::LeftButton)) {
                return QMainWindow::eventFilter(obj, event);
            }
            abortDomDrag();
        }
    }

    if (event) {
        const QEvent::Type type = event->type();
        const bool isMouseEvent = (type == QEvent::MouseMove || type == QEvent::MouseButtonPress
                                   || type == QEvent::MouseButtonRelease
                                   || type == QEvent::MouseButtonDblClick
                                   || type == QEvent::Wheel);
        if (isMouseEvent) {
            QWidget *eventWidget = qobject_cast<QWidget *>(obj);
            if (eventWidget) {
                QPoint globalPos;
                if (type == QEvent::Wheel) {
                    auto *we = static_cast<QWheelEvent *>(event);
                    globalPos = we->globalPosition().toPoint();
                } else {
                    auto *me = static_cast<QMouseEvent *>(event);
                    globalPos = me->globalPosition().toPoint();
                }
                const auto insideFloating = [&](QWidget *floating, QWidget *source) {
                    if (!floating || !floating->isVisible()) {
                        return false;
                    }
                    if (source && (source == floating || floating->isAncestorOf(source))) {
                        return false;
                    }
                    const QRect rect = floating->frameGeometry();
                    return rect.contains(globalPos);
                };
                QWidget *hoverTop = QApplication::topLevelAt(globalPos);
                bool hoverIsFloating = false;
                if (hoverTop) {
                    for (auto &tab : m_tabs) {
                        for (auto &col : tab.columnsData) {
                            if (col.floatingWindow && hoverTop == col.floatingWindow) {
                                hoverIsFloating = true;
                            }
                            if (col.chatWindow && !col.chatWindow->isDocked()
                                && hoverTop == col.chatWindow) {
                                hoverIsFloating = true;
                            }
                        }
                    }
                }
                bool sourceIsFloating = false;
                for (auto &tab : m_tabs) {
                    for (auto &col : tab.columnsData) {
                        if (!sourceIsFloating && col.floatingWindow
                            && (eventWidget == col.floatingWindow
                                || col.floatingWindow->isAncestorOf(eventWidget))) {
                            sourceIsFloating = true;
                        }
                        if (!sourceIsFloating && col.chatWindow && !col.chatWindow->isDocked()
                            && (eventWidget == col.chatWindow
                                || col.chatWindow->isAncestorOf(eventWidget))) {
                            sourceIsFloating = true;
                        }
                    }
                }
                if (!sourceIsFloating && hoverIsFloating) {
                    return true;
                }
                if (!sourceIsFloating) {
                    for (auto &tab : m_tabs) {
                        for (auto &col : tab.columnsData) {
                            if (insideFloating(col.floatingWindow, eventWidget)) {
                                return true;
                            }
                            if (col.chatWindow && !col.chatWindow->isDocked()
                                && insideFloating(col.chatWindow, eventWidget)) {
                                return true;
                            }
                        }
                    }
                }
            }
        }
    }

    if (obj == m_settingsSearchEdit && event && event->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(event);
        if (ke->key() == Qt::Key_Escape) {
            toggleSettingsSearchDrawer(false, false);
            ke->accept();
            return true;
        }
        if (matchesHotkey(ke->key(), ke->modifiers(), m_settingsSearchKey, m_settingsSearchMods)) {
            toggleSettingsSearchDrawer(!m_settingsSearchExpanded, true);
            ke->accept();
            return true;
        }
    }
    if (!m_notionalEditActive
        && (event->type() == QEvent::Enter || event->type() == QEvent::MouseMove
            || event->type() == QEvent::MouseButtonPress)) {
        QWidget *column = columnContainerForObject(obj);
        if (column) {
            WorkspaceTab *tab = nullptr;
            DomColumn *col = nullptr;
            int idx = -1;
            if (locateColumn(column, tab, col, idx) && col) {
                setActiveDomContainer(column);
                if (col->dom && !col->dom->hasFocus()) {
                    col->dom->setFocus(Qt::MouseFocusReason);
                }
            }
        }
    }

    if (event->type() == QEvent::Resize) {
        if (obj == m_workspaceStack) {
            repositionAlertsPanel();
        }
        if (auto *w = qobject_cast<QWidget *>(obj)) {
            QVariant ov = w->property("notionalOverlayPtr");
            if (ov.isValid()) {
                QWidget *column = columnContainerForObject(w);
                WorkspaceTab *tab = nullptr;
                DomColumn *col = nullptr;
                int idx = -1;
                if (column && locateColumn(column, tab, col, idx) && col) {
                    repositionNotionalOverlay(*col);
                } else {
                    auto *overlay = reinterpret_cast<QWidget *>(ov.value<quintptr>());
                    if (overlay) {
                        const int x = 0;
                        const int bottomMargin = 36;
                        int y = w->height() - overlay->sizeHint().height() - bottomMargin;
                        const int maxY = std::max(0, w->height() - overlay->sizeHint().height() - 6);
                        if (y < 8) y = 8;
                        if (y > maxY) y = maxY;
                        overlay->move(x, y);
                        overlay->raise();
                    }
                }
            }
            QVariant cov = w->property("clustersFooterOverlayPtr");
            if (cov.isValid()) {
                QWidget *column = columnContainerForObject(w);
                WorkspaceTab *tab = nullptr;
                DomColumn *col = nullptr;
                int idx = -1;
                if (column && locateColumn(column, tab, col, idx) && col) {
                    repositionClustersFooterOverlay(*col);
                } else {
                    auto *overlay = reinterpret_cast<QWidget *>(cov.value<quintptr>());
                    if (overlay) {
                        const int h = overlay->height() > 0 ? overlay->height() : 34;
                        const int y = std::max(0, w->height() - h);
                        overlay->setGeometry(0, y, w->width(), h);
                        overlay->raise();
                    }
                }
            }
            QVariant pov = w->property("positionOverlayPtr");
            if (pov.isValid()) {
                auto *overlay = reinterpret_cast<QWidget *>(pov.value<quintptr>());
                if (overlay) {
                    const int pad = 6;
                    const int bottomMargin = 4;
                    const int width = std::max(80, w->width() - pad * 2);
                    overlay->setFixedWidth(width);
                    overlay->adjustSize();
                    int y = w->height() - overlay->sizeHint().height() - bottomMargin;
                    if (y < 4) y = 4;
                    overlay->move(pad, y);
                    overlay->raise();
                }
            }
        }
    }

    if (auto *btn = qobject_cast<QToolButton *>(obj)) {
        if (btn->property("notionalIndex").isValid()) {
            if (event->type() == QEvent::MouseButtonDblClick) {
                QWidget *column = columnContainerForObject(btn);
                const int presetIndex = btn->property("notionalIndex").toInt();
                startNotionalEdit(column, presetIndex);
                return true;
            }
        }
    }

    if (obj->objectName() == QLatin1String("NotionalEditOverlay") &&
        event->type() == QEvent::MouseButtonPress) {
        QWidget *column = columnContainerForObject(obj);
        commitNotionalEdit(column, false);
        return true;
    }

    if (auto *line = qobject_cast<QLineEdit *>(obj)) {
        if (line->objectName() == QLatin1String("NotionalEditField") &&
            event->type() == QEvent::KeyPress) {
            auto *ke = static_cast<QKeyEvent *>(event);
            if (ke->key() == Qt::Key_Escape) {
                QWidget *column = columnContainerForObject(line);
                commitNotionalEdit(column, false);
                return true;
            }
        }
    }

    if (event->type() == QEvent::Resize) {
        QWidget *column = columnContainerForObject(obj);
        if (column) {
            WorkspaceTab *tab = nullptr;
            DomColumn *col = nullptr;
            int idx = -1;
            if (locateColumn(column, tab, col, idx) && col) {
                updateColumnScrollRange(*col);
                updateColumnViewport(*col, true);
            }
        }
    }

    if (event->type() == QEvent::Wheel && !m_capsAdjustMode) {
        QWidget *column = columnContainerForObject(obj);
        // If the wheel is over the clusters column, use it to zoom cluster bucket widths instead
        // of scrolling the ladder. Do not rely on QWidget::childAt/widgetAt since QQuick can
        // deliver wheel events via QWindow without a QWidget parent chain.
        if (auto *wheel = dynamic_cast<QWheelEvent *>(event)) {
            QPoint globalPos = wheel->globalPosition().toPoint();
            if (globalPos.isNull()) {
                globalPos = QCursor::pos();
            }
            int steps = wheel->angleDelta().y() / 120;
            if (wheel->inverted()) {
                steps = -steps;
            }

            auto tryClustersZoom = [&](DomColumn &colRef) -> bool {
                if (steps == 0 || !colRef.clusters || !colRef.clusters->isVisible()) {
                    return false;
                }
                const QPoint topLeft = colRef.clusters->mapToGlobal(QPoint(0, 0));
                const QRect globalRect(topLeft, colRef.clusters->size());
                if (!globalRect.contains(globalPos)) {
                    return false;
                }
                if (colRef.clusters->adjustColumnWidthByWheelSteps(steps)) {
                    repositionClustersFooterOverlay(colRef);
                    return true;
                }
                return false;
            };

            // Fast path: if we can identify the column container for the event target, check that first.
            if (column) {
                WorkspaceTab *tab = nullptr;
                DomColumn *col = nullptr;
                int idx = -1;
                if (locateColumn(column, tab, col, idx) && col && tryClustersZoom(*col)) {
                    return true;
                }
            }

            // Fallback: scan all columns using global cursor position (covers QQuick/QWindow wheel routing).
            for (WorkspaceTab &tab : m_tabs) {
                for (DomColumn &colRef : tab.columnsData) {
                    if (tryClustersZoom(colRef)) {
                        return true;
                    }
                }
            }
        }
        // If the wheel event originates from another top-level window (e.g. symbol picker),
        // do not hijack it for the DOM ladder scrolling.
        if (!column) {
            if (auto *w = qobject_cast<QWidget *>(obj)) {
                if (QWidget *win = w->window(); win && win != this) {
                    return QMainWindow::eventFilter(obj, event);
                }
            } else if (qobject_cast<QWindow *>(obj)) {
                if (QWidget *activeWin = QApplication::activeWindow(); activeWin && activeWin != this) {
                    return QMainWindow::eventFilter(obj, event);
                }
            }
        }
        // QQuickWidget (clusters) can deliver wheel events from a QQuickWindow that doesn't
        // participate in the QWidget parent chain; fall back to the focused column so the
        // scroll is always routed through the ladder scroll state (hidden scrollbar).
        if (!column && qobject_cast<QWindow *>(obj)) {
            if (QWidget *activeWin = QApplication::activeWindow(); activeWin && activeWin != this) {
                return QMainWindow::eventFilter(obj, event);
            }
            if (DomColumn *focused = focusedDomColumn()) {
                column = focused->container;
            }
        }
        if (column) {
            WorkspaceTab *tab = nullptr;
            DomColumn *col = nullptr;
            int idx = -1;
            if (locateColumn(column, tab, col, idx) && col && col->scrollBar) {
                if (auto *wheel = static_cast<QWheelEvent *>(event)) {
                    int steps = wheel->angleDelta().y() / 120;
                    if (wheel->inverted()) {
                        steps = -steps;
                    }
                    if (steps != 0) {
                        setColumnUserScrolling(column, true);
                        if (col->client && !col->backendManualPinned) {
                            // Prevent backend auto-centering from moving the whole window while the user scrolls.
                            // A zero shift activates manual center at the current backend center tick.
                            col->client->shiftWindowTicks(0);
                            col->backendManualPinned = true;
                        }
                        const int delta = -steps * std::max(1, col->scrollBar->pageStep() / 6);
                        const int next = std::clamp(col->scrollBar->value() + delta,
                                                    col->scrollBar->minimum(),
                                                    col->scrollBar->maximum());
                        if (next != col->scrollBar->value()) {
                            col->scrollBar->setValue(next);
                        } else {
                            const int cur = col->scrollBar->value();
                            const int minValue = col->scrollBar->minimum();
                            const int maxValue = col->scrollBar->maximum();
                            const bool atTop = (cur <= minValue && delta < 0);
                            const bool atBottom = (cur >= maxValue && delta > 0);
                            if (atTop || atBottom) {
                                queueColumnDepthShift(*col, atTop, 1.0);
                            } else {
                                updateColumnViewport(*col, false);
                            }
                        }
                        return true;
                    }
                }
            }
        }
    }

    if (event->type() == QEvent::Wheel && m_capsAdjustMode) {
        if (auto *wheel = static_cast<QWheelEvent *>(event)) {
            const int steps = wheel->angleDelta().y() / 120;
            if (steps != 0) {
                adjustVolumeRulesBySteps(steps);
                return true;
            }
        }
    }

    if (m_domResizeActive) {
        if (event->type() == QEvent::MouseMove) {
            if (auto *me = dynamic_cast<QMouseEvent *>(event)) {
                const int delta = me->globalPos().x() - m_domResizeStartPos.x();
                updateDomColumnResize(delta);
                return true;
            }
        }
    }
    if (event->type() == QEvent::MouseButtonRelease && (m_domResizeActive || m_domResizePending)) {
        endDomColumnResize();
    }
    if (m_domResizePending) {
        if (event->type() == QEvent::MouseMove) {
            if (auto *me = dynamic_cast<QMouseEvent *>(event)) {
                if ((me->buttons() & Qt::LeftButton)
                    && (me->globalPos() - m_domResizeStartPos).manhattanLength() > 2) {
                    m_domResizeActive = true;
                    m_domResizePending = false;
                    const int delta = me->globalPos().x() - m_domResizeStartPos.x();
                    updateDomColumnResize(delta);
                    return true;
                }
            }
        }
    }

    if (obj->objectName() == QLatin1String("DomColumnResizeHandle")) {
        auto *handle = qobject_cast<QWidget *>(obj);
        QWidget *column = columnContainerForObject(handle);
        if (!column) {
            return QMainWindow::eventFilter(obj, event);
        }

        WorkspaceTab *tab = nullptr;
        DomColumn *col = nullptr;
        int columnIndex = -1;
        locateColumn(column, tab, col, columnIndex);

        if (event->type() == QEvent::MouseButtonPress) {
            auto *me = static_cast<QMouseEvent *>(event);
            if (me->button() == Qt::LeftButton && tab && tab->columns) {
                m_domResizePending = true;
                m_domResizeContainer = column;
                m_domResizeTab = tab;
                m_domResizeSplitterIndex = columnIndex;
                m_domResizeFromLeftEdge = false;
                m_domResizeInitialSizes = tab->columns->sizes();
                m_domResizeStartPos = me->globalPos();
                if (m_domResizeHandle != handle) {
                    releaseDomResizeMouseGrab();
                    handle->grabMouse(Qt::SizeHorCursor);
                    m_domResizeHandle = handle;
                }
                return true;
            }
        } else if (event->type() == QEvent::MouseMove) {
            if (m_domResizeActive && m_domResizeContainer == column) {
                auto *me = static_cast<QMouseEvent *>(event);
                const int delta = me->globalPos().x() - m_domResizeStartPos.x();
                updateDomColumnResize(delta);
                return true;
            }
        } else if (event->type() == QEvent::MouseButtonRelease) {
            return true;
        }
        return QMainWindow::eventFilter(obj, event);
    }

    // Глобальные хоткеи, работают из любого виджета.
    if (event->type() == QEvent::KeyPress) {
        if (m_notionalEditActive || QApplication::activeModalWidget() != nullptr) {
            return QMainWindow::eventFilter(obj, event);
        }
        if (QWidget *fw = QApplication::focusWidget()) {
            if (qobject_cast<QLineEdit *>(fw)
                || qobject_cast<QTextEdit *>(fw)
                || qobject_cast<QPlainTextEdit *>(fw)
                || qobject_cast<QAbstractSpinBox *>(fw)) {
                return QMainWindow::eventFilter(obj, event);
            }
            if (auto *cb = qobject_cast<QComboBox *>(fw); cb && cb->isEditable()) {
                return QMainWindow::eventFilter(obj, event);
            }
        }
        auto *ke = static_cast<QKeyEvent *>(event);
        const int key = ke->key();
        const Qt::KeyboardModifiers mods = ke->modifiers();

        if (matchesHotkey(key, mods, Qt::Key_W, Qt::ControlModifier)) {
            if (m_workspaceTabs) {
                handleTabCloseRequested(m_workspaceTabs->currentIndex());
                ke->accept();
                return true;
            }
        }

        if (!ke->isAutoRepeat() && key == Qt::Key_Control) {
            m_ctrlExitArmed = true;
        } else if (m_ctrlExitArmed && (mods & Qt::ControlModifier) && key != Qt::Key_Control) {
            m_ctrlExitArmed = false;
        }

        bool match = false;
        if (key == m_centerKey) {
            if (m_centerMods == Qt::NoModifier) {
                match = true;
            } else {
                Qt::KeyboardModifiers cleaned = mods & ~Qt::KeypadModifier;
                match = (cleaned == m_centerMods);
            }
        }
        if (match) {
            centerActiveLaddersToSpread();
            return true;
        }
        for (int i = 0; i < static_cast<int>(m_notionalPresetKeys.size()); ++i)
        {
            if (matchesHotkey(key, mods, m_notionalPresetKeys[i], m_notionalPresetMods[i]))
            {
                applyNotionalPreset(i);
                return true;
            }
        }
    }

    if (event->type() == QEvent::KeyRelease) {
        if (m_notionalEditActive || QApplication::activeModalWidget() != nullptr) {
            return QMainWindow::eventFilter(obj, event);
        }
        if (QWidget *fw = QApplication::focusWidget()) {
            if (qobject_cast<QLineEdit *>(fw)
                || qobject_cast<QTextEdit *>(fw)
                || qobject_cast<QPlainTextEdit *>(fw)
                || qobject_cast<QAbstractSpinBox *>(fw)) {
                return QMainWindow::eventFilter(obj, event);
            }
            if (auto *cb = qobject_cast<QComboBox *>(fw); cb && cb->isEditable()) {
                return QMainWindow::eventFilter(obj, event);
            }
        }
        auto *ke = static_cast<QKeyEvent *>(event);
        if (!ke->isAutoRepeat() && ke->key() == Qt::Key_Control) {
            const bool shouldExit = m_ctrlExitArmed;
            m_ctrlExitArmed = false;
            if (shouldExit && m_tradeManager) {
                if (DomColumn *col = focusedDomColumn()) {
                    double priceHint = 0.0;
                    if (col->dom) {
                        const TradePosition pos =
                            m_tradeManager->positionForSymbol(col->symbol, col->accountName);
                        priceHint =
                            (pos.side == OrderSide::Buy) ? col->dom->bestBid() : col->dom->bestAsk();
                    }
                    m_tradeManager->closePositionMarket(col->symbol, col->accountName, priceHint);
                    return true;
                }
            }
        }
    }

    if (!m_topBar) {
        return QMainWindow::eventFilter(obj, event);
    }

    // Handle drag/float for DOM title bar
    if (obj->objectName() == QLatin1String("DomTitleBar")) {
        auto *frame = qobject_cast<QWidget *>(obj);
        QWidget *container = columnContainerForObject(frame);

        if (event->type() == QEvent::MouseButtonPress) {
            auto *me = static_cast<QMouseEvent *>(event);
            if (me->button() == Qt::LeftButton && container) {
                WorkspaceTab *tab = nullptr;
                DomColumn *col = nullptr;
                QSplitter *parentSplitter = nullptr;
                int idx = -1;
                locateColumnEx(container, tab, col, parentSplitter, idx);
                if (col && col->floatingWindow) {
                    col->floatingWindow->raise();
                    col->floatingWindow->activateWindow();
                }
                if (frame) {
                    m_domDragGrabber = frame;
                    frame->grabMouse();
                }
                m_draggingDomContainer = container;
                m_domDragStartGlobal = me->globalPos();
                m_domDragStartWindowOffset = me->globalPos() - container->mapToGlobal(QPoint(0, 0));
                locateColumnEx(m_draggingDomContainer, tab, col, parentSplitter, idx);
                m_domDragActive = (col && col->isFloating);
                return true;
            }
        } else if (event->type() == QEvent::MouseMove) {
            if (m_draggingDomContainer) {
                auto *me = static_cast<QMouseEvent *>(event);
                WorkspaceTab *tab = nullptr;
                DomColumn *col = nullptr;
                QSplitter *parentSplitter = nullptr;
                int idx = -1;
                locateColumnEx(m_draggingDomContainer, tab, col, parentSplitter, idx);
                const int dist = (me->globalPos() - m_domDragStartGlobal).manhattanLength();
                if (dist > 6 && tab && col && !col->isFloating && (me->buttons() & Qt::LeftButton)) {
                    floatDomColumn(*tab, *col, idx, me->globalPos());
                    m_domDragActive = true;
                }
                if (m_domDragActive && col && col->floatingWindow) {
                    col->floatingWindow->move(me->globalPos() - m_domDragStartWindowOffset);
                    col->floatingWindow->raise();
                    updateDockOverlay(me->globalPos(), col->container);
                }
                return true;
            }
        } else if (event->type() == QEvent::MouseButtonRelease) {
            if (m_draggingDomContainer) {
                finishDomDrag();
            }
            return true;
        } else if (event->type() == QEvent::MouseButtonDblClick) {
            auto *me = static_cast<QMouseEvent *>(event);
            if (me->button() == Qt::LeftButton && container) {
                toggleDomColumnFloating(container, me->globalPos());
                return true;
            }
        }
        return QMainWindow::eventFilter(obj, event);
    }

    // Quick hover handling for side nav buttons: ????????? ??????????? ??????
    if (event->type() == QEvent::Enter || event->type() == QEvent::Leave) {
        auto *btn = qobject_cast<QToolButton *>(obj);
        if (btn && btn->objectName() == QLatin1String("SideNavButton")) {
            const QString iconName = btn->property("iconName").toString();
            if (!iconName.isEmpty()) {
                const QSize iconSize(28, 28);
                if (event->type() == QEvent::Enter) {
                    btn->setIcon(loadIconTinted(iconName, QColor("#ffffff"), iconSize));
                } else {
                    btn->setIcon(loadIconTinted(iconName, QColor("#c0c0c0"), iconSize));
                }
            }
            // Let other handlers (like tooltips) proceed.
            return QMainWindow::eventFilter(obj, event);
        }
    }


    auto *closeBtn = qobject_cast<QToolButton *>(obj);
    if (closeBtn && closeBtn->objectName() == QLatin1String("WorkspaceTabCloseButton")) {
        if (event->type() == QEvent::Enter) {
            if (!m_tabCloseIconHover.isNull()) {
                closeBtn->setIcon(m_tabCloseIconHover);
            }
        } else if (event->type() == QEvent::Leave) {
            if (!m_tabCloseIconNormal.isNull()) {
                closeBtn->setIcon(m_tabCloseIconNormal);
            }
        }
        return QMainWindow::eventFilter(obj, event);
    }

    // Double-click on workspace tab to rename it.
    if (obj == m_workspaceTabs && event->type() == QEvent::MouseButtonDblClick) {
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() != Qt::LeftButton) {
            return QMainWindow::eventFilter(obj, event);
        }
        const QPoint pos = me->pos();
        const int index = m_workspaceTabs->tabAt(pos);
        if (index >= 0) {
            const QString currentTitle = m_workspaceTabs->tabText(index);
            bool ok = false;
            const QString newTitle = QInputDialog::getText(this,
                                                           tr("Rename tab"),
                                                           tr("Tab name:"),
                                                           QLineEdit::Normal,
                                                           currentTitle,
                                                           &ok).trimmed();
            if (ok && !newTitle.isEmpty()) {
                m_workspaceTabs->setTabText(index, newTitle);
            }
            return true;
        }
    }

    // Handle close of floating DOM window: dock it back.
    for (auto &tab : m_tabs) {
        for (auto &col : tab.columnsData) {
            if (col.floatingWindow == obj) {
                if (event->type() == QEvent::MouseMove) {
                    auto *me = static_cast<QMouseEvent *>(event);
                    if (m_domDragActive && m_draggingDomContainer == col.container
                        && (me->buttons() & Qt::LeftButton)) {
                        col.floatingWindow->move(me->globalPos() - m_domDragStartWindowOffset);
                        col.floatingWindow->raise();
                        updateDockOverlay(me->globalPos(), col.container);
                        return true;
                    }
                    if (!m_domDragActive) {
                        const Qt::Edges edges =
                            resizeEdgesForPos(col.floatingWindow, me->globalPos(), m_resizeBorderWidth);
                        const Qt::CursorShape shape = cursorForEdges(edges);
                        if (col.floatingWindow->cursor().shape() != shape) {
                            col.floatingWindow->setCursor(shape);
                        }
                    }
                } else if (event->type() == QEvent::Leave) {
                    col.floatingWindow->unsetCursor();
                } else if (event->type() == QEvent::MouseButtonPress) {
                    auto *me = static_cast<QMouseEvent *>(event);
                    if (me->button() == Qt::LeftButton && !m_domDragActive) {
                        if (QWindow *w = col.floatingWindow->windowHandle()) {
                            const Qt::Edges edges =
                                resizeEdgesForPos(col.floatingWindow, me->globalPos(), m_resizeBorderWidth);
                            if (edges != Qt::Edges()) {
                                w->startSystemResize(edges);
                                return true;
                            }
                        }
                    }
                } else if (event->type() == QEvent::MouseButtonRelease) {
                    if (m_domDragActive && m_draggingDomContainer == col.container) {
                        finishDomDrag();
                        return true;
                    }
                }
                if (event->type() == QEvent::Enter || event->type() == QEvent::MouseButtonPress) {
                    col.floatingWindow->raise();
                    col.floatingWindow->activateWindow();
                }
                if (event->type() == QEvent::Close) {
                    dockDomColumn(tab, col);
                    return true;
                }
            }
        }
    }

    // While dragging a tab, hide the underline so it does not stay
    // visually attached to the old position. It will be restored when
    // tabMoved/currentChanged fires.
    if (obj == m_workspaceTabs && event->type() == QEvent::MouseButtonPress) {
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() == Qt::LeftButton) {
            const int idx = m_workspaceTabs->tabAt(me->pos());
            if (idx == m_workspaceTabs->currentIndex()) {
                if (m_tabUnderlineAnim) {
                    m_tabUnderlineAnim->stop();
                }
                if (m_tabUnderline) {
                    m_tabUnderline->hide();
                }
                m_tabUnderlineHiddenForDrag = true;
            }
        }
    }

    if (obj == m_workspaceTabs && event->type() == QEvent::MouseButtonRelease) {
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() == Qt::MiddleButton) {
            const int idx = m_workspaceTabs->tabAt(me->pos());
            if (idx >= 0) {
                handleTabCloseRequested(idx);
            }
            return true;
        }
        if (me->button() == Qt::LeftButton && m_tabUnderlineHiddenForDrag) {
            m_tabUnderlineHiddenForDrag = false;
            updateTabUnderline(m_workspaceTabs->currentIndex());
        }
    }

    const bool isTopBarObject =
        obj == m_topBar || obj == m_workspaceTabs || obj == m_addTabButton
        || obj == m_timeLabel;

    // Hover/click on ticker label to open symbol picker or drag the column.
    for (auto &tab : m_tabs) {
        for (auto &col : tab.columnsData) {
            if (obj != col.tickerLabel) {
                continue;
            }
            if (event->type() == QEvent::Enter) {
                if (col.tickerLabel) {
                    applyTickerLabelStyle(col.tickerLabel, col.accountColor, true);
                }
                return true;
            }
            if (event->type() == QEvent::Leave) {
                if (col.tickerLabel) {
                    applyTickerLabelStyle(col.tickerLabel, col.accountColor, false);
                }
                return true;
            }
            if (event->type() == QEvent::MouseButtonPress) {
                auto *me = static_cast<QMouseEvent *>(event);
                if (me->button() == Qt::LeftButton && col.container) {
                    if (col.floatingWindow) {
                        col.floatingWindow->raise();
                        col.floatingWindow->activateWindow();
                    }
                    if (col.tickerLabel) {
                        m_domDragGrabber = col.tickerLabel;
                        col.tickerLabel->grabMouse();
                    }
                    m_pendingTickerLabel = col.tickerLabel;
                    m_pendingTickerContainer = col.container;
                    m_pendingTickerPressGlobal = me->globalPos();
                    m_pendingTickerDrag = false;
                    m_draggingDomContainer = col.container;
                    m_domDragStartGlobal = me->globalPos();
                    m_domDragStartWindowOffset =
                        me->globalPos() - col.container->mapToGlobal(QPoint(0, 0));
                    m_domDragActive = false;
                    return true;
                }
            }
            if (event->type() == QEvent::MouseMove) {
                if (m_pendingTickerLabel == col.tickerLabel && m_draggingDomContainer) {
                    auto *me = static_cast<QMouseEvent *>(event);
                    WorkspaceTab *dragTab = nullptr;
                    DomColumn *dragCol = nullptr;
                    QSplitter *dragSplitter = nullptr;
                    int dragIndex = -1;
                    locateColumnEx(m_draggingDomContainer, dragTab, dragCol, dragSplitter, dragIndex);
                    const int dist =
                        (me->globalPos() - m_pendingTickerPressGlobal).manhattanLength();
                    if (!m_pendingTickerDrag && (me->buttons() & Qt::LeftButton) && dist > 6) {
                        m_pendingTickerDrag = true;
                        if (dragTab && dragCol && !dragCol->isFloating) {
                            floatDomColumn(*dragTab, *dragCol, dragIndex, me->globalPos());
                        }
                        m_domDragActive = true;
                    }
                    if (m_domDragActive && dragCol && dragCol->floatingWindow) {
                        dragCol->floatingWindow->move(me->globalPos() - m_domDragStartWindowOffset);
                        dragCol->floatingWindow->raise();
                        updateDockOverlay(me->globalPos(), dragCol->container);
                    }
                    return true;
                }
            }
            if (event->type() == QEvent::MouseButtonRelease) {
                if (m_pendingTickerLabel == col.tickerLabel) {
                    if (m_pendingTickerDrag) {
                        finishDomDrag();
                    } else {
                        retargetDomColumn(col, QString());
                        abortDomDrag();
                    }
                    return true;
                }
            }
        }
    }

    // Allow resizing floating DOMs by dragging their edges even when the DOM container
    // receives the mouse event.
    if (event->type() == QEvent::MouseMove || event->type() == QEvent::MouseButtonPress
        || event->type() == QEvent::Leave) {
        QWidget *column = columnContainerForObject(obj);
        if (column) {
            WorkspaceTab *tab = nullptr;
            DomColumn *col = nullptr;
            int idx = -1;
            if (locateColumn(column, tab, col, idx) && col && col->floatingWindow) {
                if (event->type() == QEvent::MouseMove) {
                    auto *me = static_cast<QMouseEvent *>(event);
                    if (!m_domDragActive) {
                        const Qt::Edges edges =
                            resizeEdgesForPos(col->floatingWindow,
                                              me->globalPosition().toPoint(),
                                              m_resizeBorderWidth);
                        const Qt::CursorShape shape = cursorForEdges(edges);
                        if (column->cursor().shape() != shape) {
                            column->setCursor(shape);
                        }
                    }
                } else if (event->type() == QEvent::Leave) {
                    column->unsetCursor();
                } else if (event->type() == QEvent::MouseButtonPress) {
                    auto *me = static_cast<QMouseEvent *>(event);
                    if (me->button() == Qt::LeftButton) {
                        const Qt::Edges edges =
                            resizeEdgesForPos(col->floatingWindow,
                                              me->globalPosition().toPoint(),
                                              m_resizeBorderWidth);
                        if (edges != Qt::Edges()) {
                            if (QWindow *w = col->floatingWindow->windowHandle()) {
                                w->startSystemResize(edges);
                                return true;
                            }
                        }
                    }
                }
            }
        }
    }

    if (!isTopBarObject) {
        return QMainWindow::eventFilter(obj, event);
    }

    // Click on time label opens offset menu instead of dragging.
    if (obj == m_timeLabel && event->type() == QEvent::MouseButtonPress) {
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() == Qt::LeftButton) {
            QMenu menu;
            struct OffsetEntry {
                QString label;
                int minutes;
            };
            QList<OffsetEntry> entries;
            for (int hours = -10; hours <= 10; ++hours) {
                OffsetEntry entry;
                entry.minutes = hours * 60;
                if (hours == 0) {
                    entry.label = QStringLiteral("UTC");
                } else {
                    entry.label =
                        QStringLiteral("UTC%1%2")
                            .arg(hours > 0 ? QStringLiteral("+") : QString()) //
                            .arg(hours);
                }
                entries.push_back(entry);
            }
            QAction *currentAction = nullptr;
            for (const auto &entry : entries) {
                QAction *act = menu.addAction(entry.label);
                act->setData(entry.minutes);
                if (entry.minutes == m_timeOffsetMinutes) {
                    act->setCheckable(true);
                    act->setChecked(true);
                    currentAction = act;
                }
            }
            if (!currentAction && m_timeOffsetMinutes == 0 && !menu.actions().isEmpty()) {
                menu.actions().first()->setCheckable(true);
                menu.actions().first()->setChecked(true);
            }

            const QPoint globalPos = m_timeLabel->mapToGlobal(
                QPoint(m_timeLabel->width() / 2, m_timeLabel->height()));
            QAction *chosen = menu.exec(globalPos);
            if (chosen) {
                m_timeOffsetMinutes = chosen->data().toInt();
                updateTimeLabel();
                saveUserSettings();
            }
            return true;
        }
    }

    // Double-click on title bar: ignored (keep single-click system behavior)

    if (event->type() == QEvent::MouseButtonPress) {
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() == Qt::LeftButton) {
            // Only start native system move when the click is on an empty
            // area of the title bar (not on interactive child widgets such
            // as minimize/maximize/close buttons, tabs, etc.). This prevents
            // starting a move operation when the user actually clicks a button
            // which can lead to stuck/invalid move state after minimize.
            if (m_topBar) {
                const QPoint globalPt = me->globalPos();
                const QPoint localInTop = m_topBar->mapFromGlobal(globalPt);
                if (m_topBar->rect().contains(localInTop)) {
                    if (QWidget *child = m_topBar->childAt(localInTop)) {
                        // Click is over an interactive child ? let the child handle it.
                        return QMainWindow::eventFilter(obj, event);
                    }
                }
            }

            if (QWindow *w = windowHandle()) {
                w->startSystemMove();
                return false;
            }
        }
    }

    return QMainWindow::eventFilter(obj, event);
}

void MainWindow::centerActiveLaddersToSpread()
{
    WorkspaceTab *tab = currentWorkspaceTab();
    if (!tab || !tab->columns) {
        return;
    }

    auto spreadCenterTickFor = [](const DomColumn &col, bool *okOut) -> qint64 {
        if (okOut) *okOut = false;
        if (!col.dom) {
            return 0;
        }
        const double bestBid = col.dom->bestBid();
        const double bestAsk = col.dom->bestAsk();
        const double tickSize = col.dom->tickSize();
        if (!(tickSize > 0.0)) {
            return 0;
        }
        auto quantizeTick = [](double price, double tick) -> qint64 {
            if (!(tick > 0.0) || !std::isfinite(tick) || !(price > 0.0) || !std::isfinite(price)) {
                return 0;
            }
            auto pow10i = [](int exp) -> qint64 {
                qint64 v = 1;
                for (int i = 0; i < exp; ++i) v *= 10;
                return v;
            };
            for (int decimals = 0; decimals <= 12; ++decimals) {
                const qint64 scale = pow10i(decimals);
                const double tickScaledD = tick * static_cast<double>(scale);
                if (!std::isfinite(tickScaledD)) {
                    continue;
                }
                const qint64 tickScaled = static_cast<qint64>(std::llround(tickScaledD));
                if (tickScaled <= 0) {
                    continue;
                }
                if (std::abs(tickScaledD - static_cast<double>(tickScaled)) > 1e-9) {
                    continue;
                }
                const qint64 priceScaled = static_cast<qint64>(std::llround(price * static_cast<double>(scale)));
                qint64 out = 0;
                if (priceScaled >= 0) {
                    out = (priceScaled + tickScaled / 2) / tickScaled;
                } else {
                    out = -((-priceScaled + tickScaled / 2) / tickScaled);
                }
                return out;
            }
            return static_cast<qint64>(std::llround(price / tick));
        };
        double centerPrice = 0.0;
        if (bestBid > 0.0 && bestAsk > 0.0) {
            centerPrice = (bestBid + bestAsk) * 0.5;
        } else if (bestBid > 0.0) {
            centerPrice = bestBid;
        } else if (bestAsk > 0.0) {
            centerPrice = bestAsk;
        } else {
            return 0;
        }
        const qint64 tick = quantizeTick(centerPrice, tickSize);
        if (okOut) *okOut = true;
        return tick;
    };

    if (m_centerAllLadders) {
        for (auto &col : tab->columnsData) {
            if (col.client && col.hasBuffer) {
                bool ok = false;
                const qint64 spreadTick = spreadCenterTickFor(col, &ok);
                if (ok) {
                    if (spreadTick >= col.bufferMinTick && spreadTick <= col.bufferMaxTick) {
                        applyColumnCenterTick(col, spreadTick);
                        updateColumnViewport(col, true);
                    } else {
                        col.pendingAutoCenter = true;
                        col.pendingAutoCenterTickValid = true;
                        col.pendingAutoCenterTick = spreadTick;
                        col.client->resetManualCenter();
                        col.backendManualPinned = false;
                    }
                } else {
                    col.pendingAutoCenter = true;
                    col.client->resetManualCenter();
                    col.backendManualPinned = false;
                }
            } else if (col.dom) {
                col.dom->centerToSpread();
            }
        }
        return;
    }

    QWidget *focus = QApplication::focusWidget();
    DomColumn *targetCol = nullptr;
    if (focus) {
        for (auto &col : tab->columnsData) {
            if (col.dom && (col.dom == focus || col.dom->isAncestorOf(focus))) {
                targetCol = &col;
                break;
            }
        }
    }
    if (!targetCol && !tab->columnsData.isEmpty()) {
        targetCol = &tab->columnsData.front();
    }
    if (!targetCol) {
        return;
    }
    if (targetCol->client && targetCol->hasBuffer) {
        bool ok = false;
        const qint64 spreadTick = spreadCenterTickFor(*targetCol, &ok);
        if (ok) {
            if (spreadTick >= targetCol->bufferMinTick && spreadTick <= targetCol->bufferMaxTick) {
                applyColumnCenterTick(*targetCol, spreadTick);
                updateColumnViewport(*targetCol, true);
            } else {
                targetCol->pendingAutoCenter = true;
                targetCol->pendingAutoCenterTickValid = true;
                targetCol->pendingAutoCenterTick = spreadTick;
                targetCol->client->resetManualCenter();
                targetCol->backendManualPinned = false;
            }
        } else {
            targetCol->pendingAutoCenter = true;
            targetCol->client->resetManualCenter();
            targetCol->backendManualPinned = false;
        }
    } else if (targetCol->dom) {
        targetCol->dom->centerToSpread();
    }
}

MainWindow::DomColumn *MainWindow::focusedDomColumn()
{
    WorkspaceTab *tab = currentWorkspaceTab();
    if (!tab) {
        return nullptr;
    }
    if (m_activeDomContainer) {
        for (auto &col : tab->columnsData) {
            if (col.container == m_activeDomContainer) {
                return &col;
            }
        }
    }
    QWidget *focus = QApplication::focusWidget();
    if (focus) {
        for (auto &col : tab->columnsData) {
            if (!col.container) continue;
            if (col.container == focus || col.container->isAncestorOf(focus)) {
                return &col;
            }
        }
    }
    if (!tab->columnsData.isEmpty()) {
        return &tab->columnsData.front();
    }
    return nullptr;
}

void MainWindow::syncWatchedSymbols()
{
    if (!m_tradeManager) {
        return;
    }

    QHash<QString, QSet<QString>> byAccount;
    for (const auto &tab : m_tabs) {
        for (const auto &col : tab.columnsData) {
            const QString sym = col.symbol.trimmed().toUpper();
            const QString account = col.accountName.trimmed().isEmpty() ? QStringLiteral("MEXC Spot")
                                                                        : col.accountName.trimmed();
            if (!sym.isEmpty()) {
                byAccount[account].insert(sym);
            }
        }
    }

    QSet<QString> accounts;
    for (auto it = byAccount.constBegin(); it != byAccount.constEnd(); ++it) {
        accounts.insert(it.key());
    }
    for (auto it = m_lastWatchedSymbolsByAccount.constBegin();
         it != m_lastWatchedSymbolsByAccount.constEnd();
         ++it) {
        accounts.insert(it.key());
    }

    for (const auto &acct : accounts) {
        const QSet<QString> next = byAccount.value(acct);
        const QSet<QString> prev = m_lastWatchedSymbolsByAccount.value(acct);
        if (next == prev) {
            continue;
        }
        m_lastWatchedSymbolsByAccount.insert(acct, next);
        m_tradeManager->setWatchedSymbols(acct, next);
    }
}

void MainWindow::setActiveDomContainer(QWidget *container)
{
    if (!container || container == m_activeDomContainer) {
        return;
    }

    auto setProp = [](QWidget *w, bool on) {
        if (!w) return;
        w->setProperty("active", on);
        w->style()->unpolish(w);
        w->style()->polish(w);
        w->update();
    };

    setProp(m_activeDomContainer, false);
    m_activeDomContainer = container;
    setProp(m_activeDomContainer, m_activeDomOutlineEnabled);
}

void MainWindow::applyActiveDomOutline()
{
    auto setProp = [](QWidget *w, bool on) {
        if (!w) return;
        w->setProperty("active", on);
        w->style()->unpolish(w);
        w->style()->polish(w);
        w->update();
    };
    setProp(m_activeDomContainer, m_activeDomOutlineEnabled);
}

void MainWindow::refreshActiveLadder()
{
    WorkspaceTab *tab = currentWorkspaceTab();
    if (!tab) {
        return;
    }
    DomColumn *col = focusedDomColumn();
    if (!col || !col->client) {
        return;
    }
    restartColumnClient(*col);
}

void MainWindow::handleSettingsSearch()
{
    const QString query = m_settingsSearchEdit ? m_settingsSearchEdit->text().trimmed() : QString();
    const SettingEntry *entry = matchSettingEntry(query);
    if (entry) {
        openSettingEntry(entry->id);
        if (m_settingsSearchEdit) {
            m_settingsSearchEdit->selectAll();
        }
        toggleSettingsSearchDrawer(false, false);
    }
}

void MainWindow::handleSettingsSearchFromCompleter(const QString &value)
{
    const SettingEntry *entry = matchSettingEntry(value);
    if (!entry) {
        for (const auto &candidate : m_settingEntries) {
            if (candidate.name.compare(value, Qt::CaseInsensitive) == 0) {
                entry = &candidate;
                break;
            }
        }
    }
    if (entry) {
        openSettingEntry(entry->id);
        if (m_settingsSearchEdit) {
            m_settingsSearchEdit->selectAll();
        }
        toggleSettingsSearchDrawer(false, false);
    }
}

const MainWindow::SettingEntry *MainWindow::matchSettingEntry(const QString &query) const
{
    const QString q = query.trimmed().toLower();
    if (q.isEmpty()) {
        return nullptr;
    }
    for (const auto &entry : m_settingEntries) {
        if (entry.name.toLower().contains(q)) {
            return &entry;
        }
        for (const auto &keyword : entry.keywords) {
            const QString kw = keyword.toLower();
            if (!kw.isEmpty() && (q.contains(kw) || kw.contains(q))) {
                return &entry;
            }
        }
    }
    return nullptr;
}

void MainWindow::openSettingEntry(const QString &id)
{
    openSettingsWindow();
    if (!m_settingsWindow) {
        return;
    }
    if (id == QLatin1String("centerHotkey")) {
        m_settingsWindow->focusCenterHotkey();
        return;
    }
    if (id == QLatin1String("volumeHighlight")) {
        m_settingsWindow->focusVolumeHighlightRules();
        return;
    }
    if (id == QLatin1String("domFrameRate")) {
        m_settingsWindow->focusVolumeHighlightRules();
        return;
    }
    Q_UNUSED(id);
}

void MainWindow::loadUserSettings()
{
    auto normalizeAccountKey = [](const QString &accountName) {
        return accountName.trimmed().toLower();
    };
    auto normalizeSymbolKey = [](const QString &symbol) {
        return symbol.trimmed().toUpper();
    };
    auto makeNotionalPresetKey = [&](const QString &accountName, const QString &symbol) {
        return normalizeAccountKey(accountName) + QLatin1Char('|') + normalizeSymbolKey(symbol);
    };
    auto decodeSettingsKey = [](const QString &encoded) -> QString {
        if (encoded.trimmed().isEmpty()) {
            return QString();
        }
        const QByteArray raw = QByteArray::fromBase64(encoded.toLatin1(), QByteArray::Base64UrlEncoding);
        if (raw.isEmpty()) {
            return QString();
        }
        return QString::fromUtf8(raw);
    };

    QString baseDir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if (baseDir.isEmpty()) {
        baseDir = QDir::homePath() + QLatin1String("/.fusion_terminal");
    }
    QDir().mkpath(baseDir);

    const QString primaryFile = baseDir + QLatin1String("/fusion_terminal.ini");
    const QString legacyFile = baseDir + QLatin1String("/plasma_terminal.ini");
    const auto legacyPlasmaDir = [&]() -> QString {
        const QString current = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
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
    };
    const QString legacyPlasmaFile = legacyPlasmaDir().isEmpty()
        ? QString()
        : (legacyPlasmaDir() + QLatin1String("/plasma_terminal.ini"));

    const auto probeLegacyAppConfigDirs = [&]() -> QStringList {
        QStringList out;
        const QFileInfo fi(baseDir);
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
    };

    QString readFile = primaryFile;
    if (!QFile::exists(primaryFile) && QFile::exists(legacyFile)) {
        QFile::copy(legacyFile, primaryFile);
        readFile = primaryFile;
    } else if (!legacyPlasmaFile.isEmpty()
               && !QFile::exists(primaryFile)
               && QFile::exists(legacyPlasmaFile)) {
        QFile::copy(legacyPlasmaFile, primaryFile);
        readFile = primaryFile;
    } else if (!QFile::exists(primaryFile)) {
        QStringList candidates;
        candidates << (baseDir + QLatin1String("/ghost_terminal.ini"));
        candidates << (baseDir + QLatin1String("/shah_terminal.ini"));
        candidates << (QDir::homePath() + QLatin1String("/.fusion_terminal/fusion_terminal.ini"));
        if (!legacyPlasmaFile.isEmpty()) {
            candidates << legacyPlasmaFile;
        }
        candidates << (QDir::homePath() + QLatin1String("/.plasma_terminal/plasma_terminal.ini"));
        candidates << (QDir::homePath() + QLatin1String("/.ghost_terminal/ghost_terminal.ini"));
        candidates << (QDir::homePath() + QLatin1String("/.ghost_terminal/shah_terminal.ini"));
        candidates << (QDir::homePath() + QLatin1String("/.shah_terminal/ghost_terminal.ini"));
        candidates << (QDir::homePath() + QLatin1String("/.shah_terminal/shah_terminal.ini"));
        const QStringList legacyDirs = probeLegacyAppConfigDirs();
        for (const QString &legacyDir : legacyDirs) {
            candidates << (legacyDir + QLatin1String("/shah_terminal.ini"));
        }

        for (const QString &candidate : candidates) {
            if (QFile::exists(candidate)) {
                readFile = candidate;
                break;
            }
        }
    }

    QSettings s(readFile, QSettings::IniFormat);
    {
        ThemeManager *theme = ThemeManager::instance();
        s.beginGroup(QStringLiteral("theme"));
        theme->setMode(s.value(QStringLiteral("mode"), QStringLiteral("Dark")).toString());
        theme->setAccentColor(QColor(s.value(QStringLiteral("accent"), QStringLiteral("#007acc")).toString()));
        theme->setBidColor(QColor(s.value(QStringLiteral("bid"), QStringLiteral("#4caf50")).toString()));
        theme->setAskColor(QColor(s.value(QStringLiteral("ask"), QStringLiteral("#e53935")).toString()));
        theme->setPrintsSquare(s.value(QStringLiteral("printsSquare"), false).toBool());
        s.endGroup();
    }
    s.beginGroup(QStringLiteral("hotkeys"));
    m_centerKey = s.value(QStringLiteral("centerKey"), int(Qt::Key_Shift)).toInt();
    m_centerMods = Qt::KeyboardModifiers(
        s.value(QStringLiteral("centerMods"), int(Qt::NoModifier)).toInt());
    m_centerAllLadders =
        s.value(QStringLiteral("centerAllLadders"), true).toBool();
    m_newTabKey = s.value(QStringLiteral("newTabKey"), int(Qt::Key_T)).toInt();
    m_newTabMods = Qt::KeyboardModifiers(
        s.value(QStringLiteral("newTabMods"), int(Qt::ControlModifier)).toInt());
    m_addLadderKey = s.value(QStringLiteral("addLadderKey"), int(Qt::Key_E)).toInt();
    m_addLadderMods = Qt::KeyboardModifiers(
        s.value(QStringLiteral("addLadderMods"), int(Qt::ControlModifier)).toInt());
    m_refreshLadderKey = s.value(QStringLiteral("refreshLadderKey"), int(Qt::Key_R)).toInt();
    m_refreshLadderMods = Qt::KeyboardModifiers(
        s.value(QStringLiteral("refreshLadderMods"), int(Qt::ControlModifier)).toInt());
    m_cancelLastLimitKey = s.value(QStringLiteral("cancelLastLimitKey"), int(Qt::Key_G)).toInt();
    m_cancelLastLimitMods = Qt::KeyboardModifiers(
        s.value(QStringLiteral("cancelLastLimitMods"), int(Qt::NoModifier)).toInt());
    m_volumeAdjustKey = s.value(QStringLiteral("volumeAdjustKey"), int(Qt::Key_CapsLock)).toInt();
    m_volumeAdjustMods = Qt::KeyboardModifiers(
        s.value(QStringLiteral("volumeAdjustMods"), int(Qt::NoModifier)).toInt());
    m_sltpPlaceKey = s.value(QStringLiteral("sltpPlaceKey"), int(Qt::Key_C)).toInt();
    m_sltpPlaceMods = Qt::KeyboardModifiers(
        s.value(QStringLiteral("sltpPlaceMods"), int(Qt::NoModifier)).toInt());
    m_settingsSearchKey = s.value(QStringLiteral("settingsSearchKey"), int(Qt::Key_F3)).toInt();
    m_settingsSearchMods = Qt::KeyboardModifiers(
        s.value(QStringLiteral("settingsSearchMods"), int(Qt::NoModifier)).toInt());
    for (int i = 0; i < static_cast<int>(m_notionalPresetKeys.size()); ++i)
    {
        const QString keyName = QStringLiteral("notionalPresetKey%1").arg(i + 1);
        const QString modName = QStringLiteral("notionalPresetMods%1").arg(i + 1);
        m_notionalPresetKeys[i] =
            s.value(keyName, int(Qt::Key_1) + i).toInt();
        m_notionalPresetMods[i] =
            Qt::KeyboardModifiers(s.value(modName, int(Qt::NoModifier)).toInt());
    }
    s.endGroup();

    s.beginGroup(QStringLiteral("clock"));
    m_timeOffsetMinutes = s.value(QStringLiteral("offsetMinutes"), 0).toInt();
    s.endGroup();

    s.beginGroup(QStringLiteral("layout"));
    m_savedDomPrintsSplitterSizes =
        toIntList(s.value(QStringLiteral("domPrintsSplitterSizes"), toStringList(m_savedDomPrintsSplitterSizes)));
    m_savedClustersPrintsSplitterSizes =
        toIntList(s.value(QStringLiteral("clustersPrintsSplitterSizes"), toStringList(m_savedClustersPrintsSplitterSizes)));
    m_clustersPrintsSplitterEverShown =
        s.value(QStringLiteral("clustersPrintsEverShown"), m_clustersPrintsSplitterEverShown).toBool();
    if (!m_clustersPrintsSplitterEverShown && m_savedClustersPrintsSplitterSizes.size() >= 2
        && m_savedClustersPrintsSplitterSizes.at(0) <= 0) {
        m_savedClustersPrintsSplitterSizes = {110, 220};
        m_clustersPrintsSplitterEverShown = true;
    }
    if (m_savedClustersPrintsSplitterSizes.size() >= 2 && m_savedClustersPrintsSplitterSizes.at(0) > 0) {
        m_clustersPrintsSplitterEverShown = true;
    }
    s.endGroup();

    s.beginGroup(QStringLiteral("ladder"));
    m_domTargetFps = s.value(QStringLiteral("domRefreshFps"), m_domTargetFps).toInt();
    m_activeDomOutlineEnabled = s.value(QStringLiteral("activeDomOutline"), true).toBool();
    m_volumeRules.clear();
    int ruleCount = s.beginReadArray(QStringLiteral("volumeRules"));
    for (int i = 0; i < ruleCount; ++i) {
        s.setArrayIndex(i);
        VolumeHighlightRule rule;
        rule.threshold = s.value(QStringLiteral("threshold"), 0.0).toDouble();
        rule.color = QColor(s.value(QStringLiteral("color"), QStringLiteral("#ffd54f")).toString());
        if (rule.color.isValid()) {
            m_volumeRules.append(rule);
        }
    }
    s.endArray();
    s.endGroup();
    std::sort(m_volumeRules.begin(), m_volumeRules.end(), [](const VolumeHighlightRule &a, const VolumeHighlightRule &b) {
        return a.threshold < b.threshold;
    });

    m_futuresLeverageBySymbol.clear();
    s.beginGroup(QStringLiteral("futuresLeverage"));
    const QStringList levKeys = s.childKeys();
    for (const QString &k : levKeys) {
        const int v = s.value(k, 20).toInt();
        if (v > 0) {
            m_futuresLeverageBySymbol.insert(k.trimmed().toUpper(), std::clamp(v, 1, 1000));
        }
    }
    s.endGroup();

    m_notionalPresetsByKey.clear();
    s.beginGroup(QStringLiteral("notionalPresets"));
    const QStringList presetKeys = s.childKeys();
    for (const QString &encodedKey : presetKeys) {
        const QString rawKey = decodeSettingsKey(encodedKey);
        if (rawKey.isEmpty()) {
            continue;
        }
        QStringList parts = s.value(encodedKey).toStringList();
        if (parts.size() == 1 && parts.front().contains(QLatin1Char(','))) {
            parts = parts.front().split(QLatin1Char(','), Qt::SkipEmptyParts);
        }
        if (parts.size() != 5) {
            continue;
        }
        std::array<double, 5> values{};
        bool okAll = true;
        for (int i = 0; i < 5; ++i) {
            bool ok = false;
            const double v = parts[i].trimmed().toDouble(&ok);
            if (!ok || !(v > 0.0)) {
                okAll = false;
                break;
            }
            values[static_cast<std::size_t>(i)] = v;
        }
        if (!okAll) {
            continue;
        }
        // Normalize key components for stable lookups.
        const QStringList keyParts = rawKey.split(QLatin1Char('|'));
        if (keyParts.size() == 2) {
            const QString normalized = makeNotionalPresetKey(keyParts[0], keyParts[1]);
            m_notionalPresetsByKey.insert(normalized, values);
        } else {
            m_notionalPresetsByKey.insert(rawKey, values);
        }
    }
    s.endGroup();

    s.beginGroup(QStringLiteral("symbols"));
    const QStringList savedSymbols = s.value(QStringLiteral("list")).toStringList();
    if (!savedSymbols.isEmpty()) {
        m_symbolLibrary = savedSymbols;
    }
    const QStringList savedFutures = s.value(QStringLiteral("futuresList")).toStringList();
    if (!savedFutures.isEmpty()) {
        m_mexcFuturesSymbols = savedFutures;
    }
    // Не тянем старый список apiOff из настроек, чтобы не красить все тикеры при ошибочных данных.
    s.endGroup();

    s.beginGroup(QStringLiteral("auth"));
    m_authBaseUrl = s.value(QStringLiteral("baseUrl"), QString()).toString();
    m_authToken = s.value(QStringLiteral("token"), QString()).toString();
    m_authRole = s.value(QStringLiteral("role"), QString()).toString();
    m_authUser = s.value(QStringLiteral("user"), QString()).toString();
    s.endGroup();
    if (m_authBaseUrl.trimmed().isEmpty()) {
        const QString envBase = qEnvironmentVariable("FUSION_SERVER_URL");
        if (!envBase.trimmed().isEmpty()) {
            m_authBaseUrl = envBase.trimmed();
        } else {
            m_authBaseUrl = QStringLiteral("https://api.fusionterminal.xyz");
        }
    }

    m_savedLayout.clear();
    m_savedWorkspaceColumnSizes.clear();
    s.beginGroup(QStringLiteral("workspace"));
    int tabCount = s.beginReadArray(QStringLiteral("tabs"));
    for (int t = 0; t < tabCount; ++t) {
        s.setArrayIndex(t);
        QVector<SavedColumn> cols;
        const QList<int> splitterSizes = toIntList(s.value(QStringLiteral("splitterSizes")));
        int colCount = s.beginReadArray(QStringLiteral("columns"));
        for (int c = 0; c < colCount; ++c) {
            s.setArrayIndex(c);
            SavedColumn sc;
            sc.symbol = s.value(QStringLiteral("symbol")).toString();
            sc.compression = s.value(QStringLiteral("compression"), 1).toInt();
            sc.account = s.value(QStringLiteral("account"), QStringLiteral("MEXC Spot")).toString();
            sc.leverage = s.value(QStringLiteral("leverage"), 20).toInt();
            if (!sc.symbol.trimmed().isEmpty()) {
                cols.push_back(sc);
            }
        }
        s.endArray();
        if (!cols.isEmpty()) {
            m_savedLayout.push_back(cols);
            m_savedWorkspaceColumnSizes.push_back(splitterSizes);
        }
    }
    s.endArray();
    s.endGroup();

}

void MainWindow::saveUserSettings() const
{
    auto normalizeAccountKey = [](const QString &accountName) {
        return accountName.trimmed().toLower();
    };
    auto normalizeSymbolKey = [](const QString &symbol) {
        return symbol.trimmed().toUpper();
    };
    auto makeNotionalPresetKey = [&](const QString &accountName, const QString &symbol) {
        return normalizeAccountKey(accountName) + QLatin1Char('|') + normalizeSymbolKey(symbol);
    };
    auto encodeSettingsKey = [](const QString &raw) -> QString {
        const QByteArray b64 = raw.toUtf8().toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
        return QString::fromLatin1(b64);
    };

    QString baseDir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if (baseDir.isEmpty()) {
        baseDir = QDir::homePath() + QLatin1String("/.fusion_terminal");
    }
    QDir().mkpath(baseDir);
    const QString file = baseDir + QLatin1String("/fusion_terminal.ini");
    QSettings s(file, QSettings::IniFormat);
    {
        s.beginGroup(QStringLiteral("layout"));
        s.setValue(QStringLiteral("domPrintsSplitterSizes"), toStringList(m_savedDomPrintsSplitterSizes));
        s.setValue(QStringLiteral("clustersPrintsSplitterSizes"), toStringList(m_savedClustersPrintsSplitterSizes));
        s.setValue(QStringLiteral("clustersPrintsEverShown"), m_clustersPrintsSplitterEverShown);
        s.endGroup();
    }
    {
        ThemeManager *theme = ThemeManager::instance();
        s.beginGroup(QStringLiteral("theme"));
        s.setValue(QStringLiteral("mode"), theme->mode());
        s.setValue(QStringLiteral("accent"), theme->accentColor().name(QColor::HexRgb));
        s.setValue(QStringLiteral("bid"), theme->bidColor().name(QColor::HexRgb));
        s.setValue(QStringLiteral("ask"), theme->askColor().name(QColor::HexRgb));
        s.setValue(QStringLiteral("printsSquare"), theme->printsSquare());
        s.endGroup();
    }
    s.beginGroup(QStringLiteral("hotkeys"));
    s.setValue(QStringLiteral("centerKey"), m_centerKey);
    s.setValue(QStringLiteral("centerMods"), int(m_centerMods));
    s.setValue(QStringLiteral("centerAllLadders"), m_centerAllLadders);
    s.setValue(QStringLiteral("newTabKey"), m_newTabKey);
    s.setValue(QStringLiteral("newTabMods"), int(m_newTabMods));
    s.setValue(QStringLiteral("addLadderKey"), m_addLadderKey);
    s.setValue(QStringLiteral("addLadderMods"), int(m_addLadderMods));
    s.setValue(QStringLiteral("refreshLadderKey"), m_refreshLadderKey);
    s.setValue(QStringLiteral("refreshLadderMods"), int(m_refreshLadderMods));
    s.setValue(QStringLiteral("cancelLastLimitKey"), m_cancelLastLimitKey);
    s.setValue(QStringLiteral("cancelLastLimitMods"), int(m_cancelLastLimitMods));
    s.setValue(QStringLiteral("volumeAdjustKey"), m_volumeAdjustKey);
    s.setValue(QStringLiteral("volumeAdjustMods"), int(m_volumeAdjustMods));
    s.setValue(QStringLiteral("sltpPlaceKey"), m_sltpPlaceKey);
    s.setValue(QStringLiteral("sltpPlaceMods"), int(m_sltpPlaceMods));
    s.setValue(QStringLiteral("settingsSearchKey"), m_settingsSearchKey);
    s.setValue(QStringLiteral("settingsSearchMods"), int(m_settingsSearchMods));
    for (int i = 0; i < static_cast<int>(m_notionalPresetKeys.size()); ++i)
    {
        const QString keyName = QStringLiteral("notionalPresetKey%1").arg(i + 1);
        const QString modName = QStringLiteral("notionalPresetMods%1").arg(i + 1);
        s.setValue(keyName, m_notionalPresetKeys[i]);
        s.setValue(modName, int(m_notionalPresetMods[i]));
    }
    s.endGroup();

    s.beginGroup(QStringLiteral("auth"));
    s.setValue(QStringLiteral("baseUrl"), m_authBaseUrl);
    s.setValue(QStringLiteral("token"), m_authToken);
    s.setValue(QStringLiteral("role"), m_authRole);
    s.setValue(QStringLiteral("user"), m_authUser);
    s.endGroup();

    s.beginGroup(QStringLiteral("clock"));
    s.setValue(QStringLiteral("offsetMinutes"), m_timeOffsetMinutes);
    s.endGroup();

    s.beginGroup(QStringLiteral("ladder"));
    s.setValue(QStringLiteral("domRefreshFps"), m_domTargetFps);
    s.setValue(QStringLiteral("activeDomOutline"), m_activeDomOutlineEnabled);
    s.beginWriteArray(QStringLiteral("volumeRules"));
    for (int i = 0; i < m_volumeRules.size(); ++i) {
        s.setArrayIndex(i);
        s.setValue(QStringLiteral("threshold"), m_volumeRules[i].threshold);
        s.setValue(QStringLiteral("color"), m_volumeRules[i].color.name(QColor::HexRgb));
    }
    s.endArray();
    s.endGroup();

    s.beginGroup(QStringLiteral("futuresLeverage"));
    s.remove(QString());
    for (auto it = m_futuresLeverageBySymbol.constBegin(); it != m_futuresLeverageBySymbol.constEnd(); ++it) {
        const QString key = it.key().trimmed().toUpper();
        if (key.isEmpty()) {
            continue;
        }
        s.setValue(key, std::clamp(it.value(), 1, 1000));
    }
    s.endGroup();

    s.beginGroup(QStringLiteral("notionalPresets"));
    s.remove(QString());
    for (auto it = m_notionalPresetsByKey.constBegin(); it != m_notionalPresetsByKey.constEnd(); ++it) {
        const QString rawKey = makeNotionalPresetKey(it.key().section(QLatin1Char('|'), 0, 0),
                                                     it.key().section(QLatin1Char('|'), 1));
        const QString encodedKey = encodeSettingsKey(rawKey);
        if (encodedKey.isEmpty()) {
            continue;
        }
        QStringList values;
        values.reserve(5);
        for (double v : it.value()) {
            values << QString::number(v, 'g', 12);
        }
        s.setValue(encodedKey, values);
    }
    s.endGroup();

    s.beginGroup(QStringLiteral("symbols"));
    s.setValue(QStringLiteral("list"), m_symbolLibrary);
    s.setValue(QStringLiteral("apiOff"), QStringList(m_apiOffSymbols.begin(), m_apiOffSymbols.end()));
    s.setValue(QStringLiteral("futuresList"), m_mexcFuturesSymbols);
    s.endGroup();

    s.beginGroup(QStringLiteral("workspace"));
    s.remove(QString());
    s.beginWriteArray(QStringLiteral("tabs"));
    for (int t = 0; t < m_tabs.size(); ++t) {
        s.setArrayIndex(t);
        const auto &tab = m_tabs[t];
        if (tab.columns) {
            s.setValue(QStringLiteral("splitterSizes"), toStringList(tab.columns->sizes()));
        }
        s.beginWriteArray(QStringLiteral("columns"));
        for (int c = 0; c < tab.columnsData.size(); ++c) {
            s.setArrayIndex(c);
            const auto &col = tab.columnsData[c];
            s.setValue(QStringLiteral("symbol"), col.symbol);
            s.setValue(QStringLiteral("compression"), col.tickCompression);
            s.setValue(QStringLiteral("account"), col.accountName);
            s.setValue(QStringLiteral("leverage"), col.leverage);
        }
        s.endArray();
    }
    s.endArray();
    s.endGroup();

}

void MainWindow::scheduleSaveUserSettings(int delayMs)
{
    if (!m_saveSettingsDebounce) {
        m_saveSettingsDebounce = new QTimer(this);
        m_saveSettingsDebounce->setSingleShot(true);
        connect(m_saveSettingsDebounce, &QTimer::timeout, this, [this]() {
            saveUserSettings();
        });
    }
    m_saveSettingsDebounce->start(std::clamp(delayMs, 50, 5000));
}


void MainWindow::adjustVolumeRulesBySteps(int steps)
{
    if (steps == 0 || m_volumeRules.isEmpty()) {
        return;
    }
    const double stepFactor = 0.1;
    double factor = 1.0 + stepFactor * steps;
    if (factor <= 0.0) {
        factor = 0.1;
    }
    for (auto &rule : m_volumeRules) {
        rule.threshold = std::max(1.0, rule.threshold * factor);
    }
    std::sort(m_volumeRules.begin(), m_volumeRules.end(), [](const VolumeHighlightRule &a, const VolumeHighlightRule &b) {
        return a.threshold < b.threshold;
    });
    applyVolumeRulesToAllDoms();
    if (m_settingsWindow) {
        m_settingsWindow->setVolumeHighlightRules(m_volumeRules);
    }
    saveUserSettings();
    const int pct = static_cast<int>(std::round(factor * 100.0));
    statusBar()->showMessage(tr("Volume thresholds x%1%").arg(pct), 1200);
}

void MainWindow::initializeDomFrameTimer()
{
    if (!m_domFrameTimer) {
        m_domFrameTimer = new QTimer(this);
        m_domFrameTimer->setTimerType(Qt::PreciseTimer);
        connect(m_domFrameTimer, &QTimer::timeout, this, &MainWindow::handleDomFrameTick);
    }
    applyDomFrameRate(m_domTargetFps);
}

void MainWindow::applyDomFrameRate(int fps)
{
    const int clamped = std::clamp(fps, 0, 480);
    m_domTargetFps = clamped;
    if (!m_domFrameTimer) {
        return;
    }
    if (clamped <= 0) {
        m_domFrameTimer->stop();
        return;
    }
    const int interval = std::max(1, static_cast<int>(std::round(1000.0 / clamped)));
    if (!m_domFrameTimer->isActive() || m_domFrameTimer->interval() != interval) {
        m_domFrameTimer->start(interval);
    }
}

void MainWindow::handleDomFrameTick()
{
    if (m_domTargetFps <= 0) {
        return;
    }
    if (!isVisible()) {
        return;
    }
    if (!m_perfFpsTimer.isValid()) {
        m_perfFpsTimer.start();
        m_perfFpsFrames = 0;
    }
    ++m_perfFpsFrames;
    const qint64 elapsedMs = m_perfFpsTimer.elapsed();
    if (elapsedMs >= 1000) {
        m_lastUiFps = (m_perfFpsFrames * 1000.0) / static_cast<double>(elapsedMs);
        m_perfFpsFrames = 0;
        m_perfFpsTimer.restart();
        updateAllPerfOverlays();
    }
    WorkspaceTab *tab = currentWorkspaceTab();
    if (!tab) {
        return;
    }
    for (auto &col : tab->columnsData) {
        refreshDomColumnFrame(col);
    }
}

void MainWindow::updateColumnStatusLabel(DomColumn &col)
{
    if (!col.statusLabel) {
        return;
    }
    QString text = col.lastStatusMessage;
    text = text.trimmed();
    QString fpsText;
    if (col.lastFpsHz > 0.0) {
        fpsText = QStringLiteral("%1 Hz").arg(col.lastFpsHz, 0, 'f', 1);
    }
    if (!fpsText.isEmpty()) {
        if (!text.isEmpty()) {
            text.append(QStringLiteral(" · "));
        }
        text.append(fpsText);
    }
    if (text.isEmpty()) {
        text = QStringLiteral("-");
    }
    if (col.statusLabel->text() != text) {
        col.statusLabel->setText(text);
    }
}

void MainWindow::refreshDomColumnFrame(DomColumn &col)
{
    if (!col.client || !col.dom || !col.hasBuffer) {
        return;
    }
    if (!col.container || !col.container->isVisible()) {
        return;
    }
    if (col.pendingViewportUpdate) {
        flushPendingColumnViewport(col);
        return;
    }
    if (col.lastViewportTop < col.lastViewportBottom) {
        return;
    }
    // Avoid rebuilding snapshots every frame when nothing changed: snapshots are already pulled
    // on buffer updates (via bookRangeUpdated -> handleColumnBufferRange -> updateColumnViewport).
    if (col.lastViewportRevision == col.bufferRevision) {
        return;
    }
    pullSnapshotForColumn(col, col.lastViewportBottom, col.lastViewportTop);
}

bool MainWindow::pullSnapshotForColumn(DomColumn &col, qint64 bottomTick, qint64 topTick)
{
    if (!col.client || !col.dom) {
        return false;
    }
    DomSnapshot snap = col.client->snapshotForRange(bottomTick, topTick);
    if (snap.tickSize > 0.0) {
        col.bufferTickSize = snap.tickSize;
    }
    // Always push the snapshot to the DOM widget, even if empty.
    // Otherwise the GPU DOM can keep stale rows visible until user scroll triggers a rebuild.
    col.dom->updateSnapshot(snap);
    const bool hasLevels = !snap.levels.isEmpty();
    // We do not use QScrollArea's native pixel scroll (we render a fixed window in ticks).
    // If it ever drifts (e.g. due to geometry changes), it can create a persistent 1-row offset.
    if (col.scrollArea) {
        if (auto *vsb = col.scrollArea->verticalScrollBar()) {
            if (vsb->value() != 0) {
                vsb->setValue(0);
            }
        }
        if (auto *hsb = col.scrollArea->horizontalScrollBar()) {
            if (hsb->value() != 0) {
                hsb->setValue(0);
            }
        }
    }

    // Keep overlay PnL responsive: update from top-of-book at a modest rate.
    if (col.hasCachedPosition) {
        static constexpr qint64 kOverlayMinIntervalMs = 50;
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        if (col.lastOverlayUpdateMs <= 0 || nowMs - col.lastOverlayUpdateMs >= kOverlayMinIntervalMs) {
            col.lastOverlayUpdateMs = nowMs;
            updatePositionOverlay(col, col.cachedPosition);
        }
    }

    // SL/TP orders are sent as native Lighter stop orders (via signer),
    // so we don't locally force-close positions based on price ticks.

    if (col.prints) {
        const double tickForPrints =
            snap.tickSize > 0.0 ? snap.tickSize : col.client->tickSize();
        const int rowHeight = col.dom->rowHeight();
        const int compression = col.tickCompression;
        // IMPORTANT: Prints must follow the actual bucketized ladder bounds used to build `snap.levels`.
        // Comparing against raw viewport ticks can miss a 1-tick bucket shift when crossing compression boundaries.
        const bool mappingChanged =
            snap.minTick != col.lastPrintsBottomTick || snap.maxTick != col.lastPrintsTopTick
            || rowHeight != col.lastPrintsRowHeight || compression != col.lastPrintsCompression
            || std::abs(tickForPrints - col.lastPrintsTick) > 1e-12
            || snap.levels.size() != col.lastPrintsRowCount;
        if (mappingChanged) {
            QVector<double> prices;
            prices.reserve(snap.levels.size());
            QVector<qint64> rowTicks;
            rowTicks.reserve(snap.levels.size());
            for (const auto &lvl : snap.levels) {
                prices.push_back(lvl.price);
                rowTicks.push_back(lvl.tick);
            }
            if (col.clusters) {
                const int infoH = col.dom ? col.dom->infoAreaHeight() : 0;
                col.clusters->setRowLayout(prices.size(), rowHeight, infoH);
            }
            col.prints->setLadderPrices(prices,
                                        rowTicks,
                                        rowHeight,
                                        tickForPrints * compression,
                                        snap.minTick,
                                        snap.maxTick,
                                        compression,
                                        tickForPrints);
            col.lastPrintsBottomTick = snap.minTick;
            col.lastPrintsTopTick = snap.maxTick;
            col.lastPrintsRowHeight = rowHeight;
            col.lastPrintsCompression = compression;
            col.lastPrintsTick = tickForPrints;
            col.lastPrintsRowCount = snap.levels.size();
        }
    }
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (col.lastSnapshotTimestampMs > 0) {
        const qint64 delta = std::max<qint64>(1, nowMs - col.lastSnapshotTimestampMs);
        if (col.avgSnapshotIntervalMs <= 0.0) {
            col.avgSnapshotIntervalMs = static_cast<double>(delta);
        } else {
            const double alpha = 0.2;
            col.avgSnapshotIntervalMs =
                (1.0 - alpha) * col.avgSnapshotIntervalMs + alpha * static_cast<double>(delta);
        }
    }
    col.lastSnapshotTimestampMs = nowMs;
    if (col.avgSnapshotIntervalMs > 0.0) {
        const double fps = 1000.0 / col.avgSnapshotIntervalMs;
        static constexpr qint64 kStatusUpdateIntervalMs = 500;
        if (nowMs - col.lastFpsLabelUpdateMs >= kStatusUpdateIntervalMs || col.lastFpsHz <= 0.0) {
            col.lastFpsHz = fps;
            col.lastFpsLabelUpdateMs = nowMs;
            updateColumnStatusLabel(col);
            updatePerfOverlay(col);
        }
        // Avoid spamming logs with FPS; the status label already shows the value when enabled.
    }
    return hasLevels;
}

void MainWindow::maybeTriggerSltpForColumn(DomColumn &col, const DomSnapshot &snap)
{
    if (!m_tradeManager) {
        return;
    }
    if (!col.sltpHasSl && !col.sltpHasTp) {
        return;
    }
    const QString symbolUpper = col.symbol.trimmed().toUpper();
    if (symbolUpper.isEmpty()) {
        return;
    }

    const SymbolSource src = symbolSourceForAccount(col.accountName);
    const bool lighterPerp = (src == SymbolSource::Lighter) && !symbolUpper.contains(QLatin1Char('/'));
    if (!lighterPerp) {
        return;
    }

    const TradePosition pos = m_tradeManager->positionForSymbol(symbolUpper, col.accountName);
    const bool hasPosition =
        pos.hasPosition && pos.quantity > 0.0 && pos.averagePrice > 0.0;
    if (!hasPosition) {
        return;
    }

    // Use the right side for the right condition:
    // - SL: long triggers on ask falling to SL, short triggers on bid rising to SL.
    // - TP: long triggers on bid rising to TP, short triggers on ask falling to TP.
    double bestBid = snap.bestBid;
    double bestAsk = snap.bestAsk;
    if (col.dom) {
        if (!(bestBid > 0.0)) bestBid = col.dom->bestBid();
        if (!(bestAsk > 0.0)) bestAsk = col.dom->bestAsk();
    }
    if (!(bestBid > 0.0) && !(bestAsk > 0.0)) {
        return;
    }
    const double slMark = (pos.side == OrderSide::Buy) ? bestAsk : bestBid;
    const double tpMark = (pos.side == OrderSide::Buy) ? bestBid : bestAsk;
    const double refForTol = (slMark > 0.0) ? slMark : tpMark;
    if (!(refForTol > 0.0)) {
        return;
    }

    const double tol = std::max(1e-8, snap.tickSize > 0.0 ? snap.tickSize * 0.25 : refForTol * 1e-8);
    bool triggered = false;
    QString tag;
    double triggerPrice = 0.0;

    if (pos.side == OrderSide::Buy) {
        if (col.sltpHasSl && col.sltpSlPrice > 0.0 && slMark > 0.0 && slMark <= col.sltpSlPrice + tol) {
            triggered = true;
            tag = QStringLiteral("SL");
            triggerPrice = col.sltpSlPrice;
        } else if (col.sltpHasTp && col.sltpTpPrice > 0.0 && tpMark > 0.0 && tpMark >= col.sltpTpPrice - tol) {
            triggered = true;
            tag = QStringLiteral("TP");
            triggerPrice = col.sltpTpPrice;
        }
    } else {
        if (col.sltpHasSl && col.sltpSlPrice > 0.0 && slMark > 0.0 && slMark >= col.sltpSlPrice - tol) {
            triggered = true;
            tag = QStringLiteral("SL");
            triggerPrice = col.sltpSlPrice;
        } else if (col.sltpHasTp && col.sltpTpPrice > 0.0 && tpMark > 0.0 && tpMark <= col.sltpTpPrice + tol) {
            triggered = true;
            tag = QStringLiteral("TP");
            triggerPrice = col.sltpTpPrice;
        }
    }

    if (!triggered) {
        return;
    }

    clearSltpMarkers(col);
    statusBar()->showMessage(
        tr("%1 triggered @ %2").arg(tag, QString::number(triggerPrice, 'f', 6)),
        2500);
    markPendingCancelForSymbol(symbolUpper);
    m_tradeManager->cancelAllOrders(symbolUpper, col.accountName);
    const double closeHint = (tag == QStringLiteral("TP")) ? tpMark : slMark;
    m_tradeManager->closePositionMarket(symbolUpper, col.accountName, closeHint);
}

QVector<SettingsWindow::HotkeyEntry> MainWindow::currentCustomHotkeys() const
{
    QVector<SettingsWindow::HotkeyEntry> entries;
    entries.append({QStringLiteral("newTab"),
                    tr("Открыть новую вкладку"),
                    m_newTabKey,
                    m_newTabMods});
    entries.append({QStringLiteral("addLadder"),
                    tr("Добавить стакан в текущую вкладку"),
                    m_addLadderKey,
                    m_addLadderMods});
    entries.append({QStringLiteral("refreshLadder"),
                    tr("Перезапустить активный стакан"),
                    m_refreshLadderKey,
                    m_refreshLadderMods});
    entries.append({QStringLiteral("cancelLastLimit"),
                    tr("Cancel last limit order"),
                    m_cancelLastLimitKey,
                    m_cancelLastLimitMods});
    entries.append({QStringLiteral("settingsSearch"),
                    tr("Search settings"),
                    m_settingsSearchKey,
                    m_settingsSearchMods});
    entries.append({QStringLiteral("volumeAdjust"),
                    tr("Режим изменения порогов колесом"),
                    m_volumeAdjustKey,
                    m_volumeAdjustMods});
    entries.append({QStringLiteral("sltpPlace"),
                    tr("Поставить SL/TP (удерживать)"),
                    m_sltpPlaceKey,
                    m_sltpPlaceMods});
    for (int i = 0; i < static_cast<int>(m_notionalPresetKeys.size()); ++i)
    {
        entries.append({QStringLiteral("notionalPreset%1").arg(i + 1),
                        tr("Горячая клавиша пресета объема %1").arg(i + 1),
                        m_notionalPresetKeys[i],
                        m_notionalPresetMods[i]});
    }
    return entries;
}
void MainWindow::updateCustomHotkey(const QString &id, int key, Qt::KeyboardModifiers mods)
{
    auto assign = [&](int &targetKey, Qt::KeyboardModifiers &targetMods) {
        targetKey = key;
        targetMods = mods;
    };
    if (id == QLatin1String("newTab")) {
        assign(m_newTabKey, m_newTabMods);
    } else if (id == QLatin1String("addLadder")) {
        assign(m_addLadderKey, m_addLadderMods);
    } else if (id == QLatin1String("refreshLadder")) {
        assign(m_refreshLadderKey, m_refreshLadderMods);
    } else if (id == QLatin1String("cancelLastLimit")) {
        assign(m_cancelLastLimitKey, m_cancelLastLimitMods);
    } else if (id == QLatin1String("settingsSearch")) {
        assign(m_settingsSearchKey, m_settingsSearchMods);
    } else if (id == QLatin1String("volumeAdjust")) {
        assign(m_volumeAdjustKey, m_volumeAdjustMods);
        m_capsAdjustMode = false;
    } else if (id == QLatin1String("sltpPlace")) {
        assign(m_sltpPlaceKey, m_sltpPlaceMods);
    } else if (id.startsWith(QLatin1String("notionalPreset"))) {
        bool ok = false;
        const int idx = id.mid(QStringLiteral("notionalPreset").size()).toInt(&ok) - 1;
        if (!ok || idx < 0 || idx >= static_cast<int>(m_notionalPresetKeys.size())) {
            return;
        }
        assign(m_notionalPresetKeys[idx], m_notionalPresetMods[idx]);
    } else {
        return;
    }
    saveUserSettings();
}
bool MainWindow::matchesHotkey(int eventKey,
                               Qt::KeyboardModifiers eventMods,
                               int key,
                               Qt::KeyboardModifiers mods)
{
    if (key == 0) {
        return false;
    }
    Qt::KeyboardModifiers cleaned = eventMods & ~Qt::KeypadModifier;
    return eventKey == key && cleaned == mods;
}

int MainWindow::baseLevelsPerSide() const
{
    return std::clamp(m_levels, kMinBaseLadderLevels, kMaxBaseLadderLevels);
}

int MainWindow::guiLevelsPerSide() const
{
    return std::max(baseLevelsPerSide(), kGuiLevelsPerSide);
}

int MainWindow::backgroundLevelsPerSide() const
{
    const int gui = guiLevelsPerSide();
    const int margin = std::max(200, kBackgroundMarginLevels);
    return std::min(gui + margin, kMaxEffectiveLadderLevels / 2);
}

int MainWindow::effectiveLevelsForColumn(const DomColumn &col) const
{
    const int compression = std::max(1, col.tickCompression);
    const qint64 raw = static_cast<qint64>(backgroundLevelsPerSide()) * compression;
    const qint64 capped = std::min(raw, static_cast<qint64>(kMaxEffectiveLadderLevels));
    return static_cast<int>(capped);
}

qint64 MainWindow::targetDisplaySpanTicks(const DomColumn &col) const
{
    const qint64 compression = std::max<qint64>(1, col.tickCompression);
    const qint64 perSide = static_cast<qint64>(guiLevelsPerSide()) * compression;
    const qint64 totalSpan = std::max<qint64>(compression, perSide * 2);
    const qint64 bufferSpan =
        std::max<qint64>(compression,
                         static_cast<qint64>(effectiveLevelsForColumn(col)) * 2);
    return std::min(totalSpan, bufferSpan);
}

qint64 MainWindow::displaySlideStepTicks(const DomColumn &col) const
{
    const qint64 span =
        std::max<qint64>(1, col.displayMaxTick - col.displayMinTick + 1);
    const qint64 compression = std::max<qint64>(1, col.tickCompression);
    const qint64 minStep = std::max(compression * 200, compression * 2);
    return std::max(span / 4, minStep);
}

qint64 MainWindow::smoothSlideStepTicks(const DomColumn &col) const
{
    const int rows = std::max(1, col.visibleRowCount);
    const qint64 compression = std::max<qint64>(1, col.tickCompression);
    const qint64 rowsPerStep = std::max(2, rows / 15);
    return compression * rowsPerStep;
}

bool MainWindow::slideDisplayWindow(DomColumn &col, int direction, qint64 overrideStep)
{
    if (!col.hasBuffer) {
        return false;
    }
    const qint64 span =
        std::max<qint64>(1, col.displayMaxTick - col.displayMinTick + 1);
    if (span <= 0) {
        return false;
    }
    const qint64 step =
        overrideStep > 0 ? overrideStep : displaySlideStepTicks(col);
    if (direction > 0) {
        if (col.displayMaxTick >= col.bufferMaxTick) {
            return false;
        }
        const qint64 shift = std::min(step, col.bufferMaxTick - col.displayMaxTick);
        col.displayMinTick += shift;
        col.displayMaxTick += shift;
        return true;
    }
    if (direction < 0) {
        if (col.displayMinTick <= col.bufferMinTick) {
            return false;
        }
        const qint64 shift = std::min(step, col.displayMinTick - col.bufferMinTick);
        col.displayMinTick -= shift;
        col.displayMaxTick -= shift;
        return true;
    }
    return false;
}

void MainWindow::maybePrefetchBuffer(DomColumn &col, bool upwards)
{
    if (!col.client) {
        return;
    }
    const qint64 span =
        std::max<qint64>(1, col.displayMaxTick - col.displayMinTick + 1);
    const qint64 guard = std::max<qint64>(span * kDisplayPrefetchGuardRatio,
                                          displaySlideStepTicks(col));
    const qint64 chunk = depthChunkTicks(col.tickCompression,
                                        col.scrollBar,
                                        col.hasBuffer,
                                        col.bufferMinTick,
                                        col.bufferMaxTick);
    if (upwards) {
        const qint64 headroom = col.bufferMaxTick - col.displayMaxTick;
        if (headroom <= guard && !col.pendingExtendUp) {
            col.client->shiftWindowTicks(+chunk);
            col.pendingExtendUp = true;
        }
        return;
    }
    const qint64 headroom = col.displayMinTick - col.bufferMinTick;
    if (headroom <= guard && !col.pendingExtendDown) {
        col.client->shiftWindowTicks(-chunk);
        col.pendingExtendDown = true;
    }
}

void MainWindow::queueColumnDepthShift(DomColumn &col, bool upwards, double multiplier)
{
    if (!col.client) {
        return;
    }
    const double clamped = std::clamp(multiplier, 1.0, 8.0);
    const qint64 blocks =
        std::clamp(static_cast<qint64>(std::ceil(clamped)), qint64(1), qint64(12));
    const qint64 chunk = depthChunkTicks(col.tickCompression,
                                        col.scrollBar,
                                        col.hasBuffer,
                                        col.bufferMinTick,
                                        col.bufferMaxTick);
    const qint64 shift = static_cast<qint64>(blocks) * chunk;
    const qint64 maxQueued = maxQueuedShiftTicks(col.tickCompression,
                                                col.scrollBar,
                                                col.hasBuffer,
                                                col.bufferMinTick,
                                                col.bufferMaxTick);
    if (upwards) {
        if (!col.pendingExtendUp) {
            col.client->shiftWindowTicks(+shift);
            col.pendingExtendUp = true;
        } else {
            col.extendQueuedShiftUp =
                std::clamp(col.extendQueuedShiftUp + shift,
                           qint64(0),
                           maxQueued);
        }
        return;
    }
    if (!col.pendingExtendDown) {
        col.client->shiftWindowTicks(-shift);
        col.pendingExtendDown = true;
    } else {
        col.extendQueuedShiftDown =
            std::clamp(col.extendQueuedShiftDown + shift,
                       qint64(0),
                       maxQueued);
    }
}

void MainWindow::recenterDisplayWindow(DomColumn &col, qint64 centerTick)
{
    if (!col.hasBuffer) {
        return;
    }
    const qint64 bufferSpan =
        std::max<qint64>(1, col.bufferMaxTick - col.bufferMinTick + 1);
    const qint64 targetSpan =
        std::max<qint64>(1, targetDisplaySpanTicks(col));
    const qint64 span = std::min(bufferSpan, targetSpan);
    const qint64 clampedCenter =
        std::clamp(centerTick, col.bufferMinTick, col.bufferMaxTick);
    qint64 minDisplay = clampedCenter - span / 2;
    qint64 maxDisplay = minDisplay + span - 1;
    if (minDisplay < col.bufferMinTick) {
        minDisplay = col.bufferMinTick;
        maxDisplay = std::min(col.bufferMaxTick, minDisplay + span - 1);
    }
    if (maxDisplay > col.bufferMaxTick) {
        maxDisplay = col.bufferMaxTick;
        minDisplay = std::max(col.bufferMinTick, maxDisplay - span + 1);
    }
    col.displayMinTick = minDisplay;
    col.displayMaxTick = maxDisplay;
}

int MainWindow::desiredVisibleRows(const DomColumn &col) const
{
    const int rowHeight = col.dom ? col.dom->rowHeight() : 12;
    if (!col.scrollArea) {
        return std::max(40, rowHeight > 0 ? 600 / rowHeight : 50);
    }
    const int viewportHeight = std::max(200, col.scrollArea->viewport()->height());
    const int rows = rowHeight > 0 ? viewportHeight / rowHeight : 0;
    return std::max(40, rows + 6);
}

void MainWindow::updateColumnScrollRange(DomColumn &col)
{
    if (!col.scrollBar || !col.hasBuffer) {
        return;
    }
    col.visibleRowCount = desiredVisibleRows(col);
    const qint64 requiredTicks =
        std::max<qint64>(1, static_cast<qint64>(col.visibleRowCount) * col.tickCompression);
    qint64 windowMin = std::max(col.bufferMinTick, col.displayMinTick);
    qint64 windowMax = std::min(col.bufferMaxTick, col.displayMaxTick);
    if (windowMax < windowMin) {
        windowMax = windowMin;
    }
    const qint64 totalTicks = (windowMax - windowMin) + 1;
    const qint64 maxOffset = totalTicks > requiredTicks ? (totalTicks - requiredTicks) : 0;
    QSignalBlocker blocker(col.scrollBar);
    col.scrollBar->setRange(0, static_cast<int>(std::max<qint64>(0, maxOffset)));
    const int page =
        static_cast<int>(std::clamp<qint64>(requiredTicks, 1, std::numeric_limits<int>::max()));
    col.scrollBar->setPageStep(page);
    const int clamped =
        std::clamp(col.scrollBar->value(), col.scrollBar->minimum(), col.scrollBar->maximum());
    if (clamped != col.scrollBar->value()) {
        col.scrollBar->setValue(clamped);
        col.scrollValueValid = false;
    }
}

void MainWindow::applyColumnCenterTick(DomColumn &col, qint64 centerTick)
{
    if (!col.scrollBar || !col.hasBuffer) {
        return;
    }
    recenterDisplayWindow(col, centerTick);
    updateColumnScrollRange(col);
    const qint64 requiredTicks =
        std::max<qint64>(1, static_cast<qint64>(col.visibleRowCount) * col.tickCompression);
    qint64 halfSpan = requiredTicks / 2;
    qint64 topTick = centerTick + halfSpan;
    qint64 windowMax = std::min(col.bufferMaxTick, col.displayMaxTick);
    qint64 windowMin = std::max(col.bufferMinTick, col.displayMinTick);
    if (windowMax < windowMin) {
        windowMax = windowMin;
    }
    if (topTick > windowMax) {
        topTick = windowMax;
    }
    qint64 bottomTick = topTick - (requiredTicks - 1);
    if (bottomTick < windowMin) {
        bottomTick = windowMin;
        topTick = std::min(windowMax, bottomTick + requiredTicks - 1);
    }
    const qint64 offset = std::max<qint64>(0, windowMax - topTick);
    QSignalBlocker blocker(col.scrollBar);
    col.scrollBar->setValue(static_cast<int>(std::clamp<qint64>(
        offset, col.scrollBar->minimum(), col.scrollBar->maximum())));
    col.scrollValueValid = false;
    col.visibleCenterTick = centerTick;
    col.userScrolling = false;
}

void MainWindow::updateColumnViewport(DomColumn &col, bool forceCenter)
{
    if (!col.client || !col.dom || !col.hasBuffer || !col.scrollBar) {
        return;
    }
    const int newRows = desiredVisibleRows(col);
    if (newRows != col.visibleRowCount || forceCenter) {
        col.visibleRowCount = newRows;
        updateColumnScrollRange(col);
    }
    const qint64 spanTicks =
        std::max<qint64>(1, static_cast<qint64>(col.visibleRowCount) * col.tickCompression);
    qint64 offset = col.scrollBar ? col.scrollBar->value() : 0;
    offset = std::clamp<qint64>(offset,
                                col.scrollBar ? col.scrollBar->minimum() : 0,
                                col.scrollBar ? col.scrollBar->maximum() : 0);
    qint64 windowMax = std::min(col.bufferMaxTick, col.displayMaxTick);
    qint64 windowMin = std::max(col.bufferMinTick, col.displayMinTick);
    if (windowMax < windowMin) {
        windowMax = windowMin;
    }
    qint64 topTick = windowMax - offset;
    qint64 bottomTick = topTick - (spanTicks - 1);
    if (bottomTick < windowMin) {
        bottomTick = windowMin;
        topTick = bottomTick + spanTicks - 1;
        if (topTick > windowMax) {
            topTick = windowMax;
            bottomTick = std::max(windowMin, topTick - spanTicks + 1);
        }
    }
    if (col.pendingViewportUpdate) {
        col.pendingViewportBottom = bottomTick;
        col.pendingViewportTop = topTick;
        col.pendingViewportRevision = col.bufferRevision;
        return;
    }
    col.pendingViewportUpdate = true;
    col.pendingViewportBottom = bottomTick;
    col.pendingViewportTop = topTick;
    col.pendingViewportRevision = col.bufferRevision;
    QPointer<QWidget> container = col.container;
    QTimer::singleShot(0, this, [this, container]() {
        if (!container) {
            return;
        }
        WorkspaceTab *tab = nullptr;
        DomColumn *colPtr = nullptr;
        int idx = -1;
        if (!locateColumn(container, tab, colPtr, idx) || !colPtr) {
            return;
        }
        flushPendingColumnViewport(*colPtr);
    });
}

void MainWindow::flushPendingColumnViewport(DomColumn &col)
{
    if (!col.pendingViewportUpdate || !col.client || !col.dom) {
        col.pendingViewportUpdate = false;
        col.pendingViewportRevision = 0;
        return;
    }
    col.pendingViewportUpdate = false;
    const qint64 bottomTick = col.pendingViewportBottom;
    const qint64 topTick = col.pendingViewportTop;
    const quint64 revision = col.pendingViewportRevision;
    if (bottomTick == col.lastViewportBottom && topTick == col.lastViewportTop
        && revision == col.lastViewportRevision) {
        return;
    }
    col.lastViewportBottom = bottomTick;
    col.lastViewportTop = topTick;
    col.lastViewportRevision = revision;
    pullSnapshotForColumn(col, bottomTick, topTick);
}

void MainWindow::handleColumnBufferRange(QWidget *container,
                                         qint64 minTick,
                                         qint64 maxTick,
                                         qint64 centerTick,
                                         double tickSize)
{
    WorkspaceTab *tab = nullptr;
    DomColumn *col = nullptr;
    int idx = -1;
    if (!locateColumn(container, tab, col, idx) || !col) {
        return;
    }
    const bool firstBuffer = !col->hasBuffer;
    const qint64 prevBufferMin = col->bufferMinTick;
    const qint64 prevBufferMax = col->bufferMaxTick;
    const qint64 prevDisplayMin = col->displayMinTick;
    const qint64 prevDisplayMax = col->displayMaxTick;
    const int prevScrollValue = col->scrollBar ? col->scrollBar->value() : 0;
    const bool wasPendingUp = col->pendingExtendUp;
    const bool wasPendingDown = col->pendingExtendDown;

    bool preserveViewport = false;
    qint64 shiftDeltaTicks = 0;
    if (!firstBuffer && col->scrollBar && (wasPendingUp || wasPendingDown) && !col->pendingAutoCenter)
    {
        const qint64 deltaMin = minTick - prevBufferMin;
        const qint64 deltaMax = maxTick - prevBufferMax;
        if (deltaMin == deltaMax && deltaMax != 0)
        {
            const bool dirOk = (deltaMax > 0 && wasPendingUp) || (deltaMax < 0 && wasPendingDown);
            if (dirOk)
            {
                preserveViewport = true;
                shiftDeltaTicks = deltaMax;
                col->displayMinTick = prevDisplayMin + shiftDeltaTicks;
                col->displayMaxTick = prevDisplayMax + shiftDeltaTicks;
            }
        }
    }
    col->bufferMinTick = minTick;
    col->bufferMaxTick = maxTick;
    col->visibleCenterTick = centerTick;
    col->bufferTickSize = tickSize;
    const qint64 bufferSpan = (col->bufferMaxTick - col->bufferMinTick) + 1;
    const qint64 targetSpan = std::max<qint64>(1, targetDisplaySpanTicks(*col));
    const qint64 span = std::min(bufferSpan, targetSpan);
    if (span > 0) {
        if (firstBuffer || col->displayMaxTick <= col->displayMinTick) {
            qint64 center = std::clamp(centerTick, col->bufferMinTick, col->bufferMaxTick);
            qint64 half = span / 2;
            qint64 minDisplay = center - half;
            qint64 maxDisplay = minDisplay + span - 1;
            if (minDisplay < col->bufferMinTick) {
                minDisplay = col->bufferMinTick;
                maxDisplay = std::min(col->bufferMaxTick, minDisplay + span - 1);
            }
            if (maxDisplay > col->bufferMaxTick) {
                maxDisplay = col->bufferMaxTick;
                minDisplay = std::max(col->bufferMinTick, maxDisplay - span + 1);
            }
            col->displayMinTick = minDisplay;
            col->displayMaxTick = maxDisplay;
        } else {
            const qint64 currentSpan =
                std::max<qint64>(1, col->displayMaxTick - col->displayMinTick + 1);
            const qint64 desiredSpan = std::min(currentSpan, span);
            qint64 minDisplay = std::clamp(col->displayMinTick,
                                           col->bufferMinTick,
                                           col->bufferMaxTick - desiredSpan + 1);
            qint64 maxDisplay = minDisplay + desiredSpan - 1;
            if (maxDisplay > col->bufferMaxTick) {
                maxDisplay = col->bufferMaxTick;
                minDisplay = std::max(col->bufferMinTick, maxDisplay - desiredSpan + 1);
            }
            col->displayMinTick = minDisplay;
            col->displayMaxTick = maxDisplay;
        }
    } else {
        col->displayMinTick = col->bufferMinTick;
        col->displayMaxTick = col->bufferMaxTick;
    }
    col->hasBuffer = true;
    col->pendingExtendUp = false;
    col->pendingExtendDown = false;
    ++col->bufferRevision;
    updateColumnScrollRange(*col);
    if (preserveViewport && col->scrollBar)
    {
        const qint64 desired = static_cast<qint64>(prevScrollValue) + shiftDeltaTicks;
        const int clamped =
            static_cast<int>(std::clamp<qint64>(desired, col->scrollBar->minimum(), col->scrollBar->maximum()));
        QSignalBlocker blocker(col->scrollBar);
        col->scrollBar->setValue(clamped);
        col->scrollValueValid = false;
        col->lastScrollBarValue = clamped;
    }
    if (firstBuffer || col->pendingAutoCenter) {
        col->pendingAutoCenter = false;
        const qint64 tick =
            col->pendingAutoCenterTickValid ? col->pendingAutoCenterTick : centerTick;
        col->pendingAutoCenterTickValid = false;
        applyColumnCenterTick(*col, tick);
        updateColumnViewport(*col, true);
    } else {
        updateColumnViewport(*col, false);
    }

    if (col->extendQueuedShiftUp > 0) {
        const qint64 queued = col->extendQueuedShiftUp;
        col->extendQueuedShiftUp = 0;
        col->client->shiftWindowTicks(+queued);
        col->pendingExtendUp = true;
    }
    if (col->extendQueuedShiftDown > 0) {
        const qint64 queued = col->extendQueuedShiftDown;
        col->extendQueuedShiftDown = 0;
        col->client->shiftWindowTicks(-queued);
        col->pendingExtendDown = true;
    }
}

void MainWindow::bootstrapColumnFromClient(QWidget *container)
{
    if (!container) {
        return;
    }
    WorkspaceTab *tab = nullptr;
    DomColumn *col = nullptr;
    int idx = -1;
    if (!locateColumn(container, tab, col, idx) || !col || !col->client) {
        return;
    }
    if (col->hasBuffer) {
        return;
    }
    if (!col->client->hasBook()) {
        return;
    }
    const qint64 minTick = col->client->bufferMinTick();
    const qint64 maxTick = col->client->bufferMaxTick();
    const qint64 centerTick = col->client->centerTick();
    const double tickSize = col->client->tickSize();
    if (!(tickSize > 0.0) || maxTick <= minTick) {
        return;
    }
    handleColumnBufferRange(container, minTick, maxTick, centerTick, tickSize);
}

QWidget *MainWindow::columnContainerForObject(QObject *obj) const
{
    QObject *current = obj;
    while (current) {
        const QVariant ptr = current->property("domContainerPtr");
        if (ptr.isValid()) {
            return reinterpret_cast<QWidget *>(ptr.value<quintptr>());
        }
        current = current->parent();
    }
    return nullptr;
}

void MainWindow::setColumnUserScrolling(QWidget *container, bool scrolling)
{
    if (!container) {
        return;
    }
    WorkspaceTab *tab = nullptr;
    DomColumn *col = nullptr;
    int idx = -1;
    if (!locateColumn(container, tab, col, idx) || !col) {
        return;
    }
    col->userScrolling = scrolling;
}

void MainWindow::handleDomScroll(QWidget *columnContainer, int value)
{
    if (!columnContainer) {
        return;
    }
    WorkspaceTab *tab = nullptr;
    DomColumn *col = nullptr;
    int splitIndex = -1;
    if (!locateColumn(columnContainer, tab, col, splitIndex) || !col || !col->scrollBar) {
        return;
    }
    Q_UNUSED(value);
    if (!col->hasBuffer) {
        return;
    }
    updateColumnViewport(*col, false);
    if (!col->client || !col->scrollBar) {
        return;
    }
    const int minValue = col->scrollBar->minimum();
    const int maxValue = col->scrollBar->maximum();
    const int current = col->scrollBar->value();
    const int previous = col->lastScrollBarValue;
    const bool hadPrevious = col->scrollValueValid;
    col->lastScrollBarValue = current;
    col->scrollValueValid = true;
    const int delta = hadPrevious ? (current - previous) : 0;
    const bool movingDown = hadPrevious && delta > 0;
    const bool movingUp = hadPrevious && delta < 0;
    if (maxValue <= minValue) {
        return;
    }
    const double ratio =
        static_cast<double>(current - minValue) / static_cast<double>(maxValue - minValue);
    const bool dragging = col->userScrolling;
    qint64 dragStep = 0;
    if (dragging) {
        dragStep = smoothSlideStepTicks(*col);
    }
    const bool allowTopExtend =
        !col->userScrolling || !hadPrevious || movingUp || current <= minValue;
    const bool allowBottomExtend =
        !col->userScrolling || !hadPrevious || movingDown || current >= maxValue;
    if (ratio <= kDepthExtendThreshold && allowTopExtend) {
        if (slideDisplayWindow(*col, +1, dragStep)) {
            updateColumnScrollRange(*col);
            updateColumnViewport(*col, false);
            maybePrefetchBuffer(*col, true);
        } else {
            const double extendMultiplier = dragging ? 3.0 : 1.0;
            queueColumnDepthShift(*col, true, extendMultiplier);
        }
        return;
    }
    if (ratio >= 1.0 - kDepthExtendThreshold && allowBottomExtend) {
        if (slideDisplayWindow(*col, -1, dragStep)) {
            updateColumnScrollRange(*col);
            updateColumnViewport(*col, false);
            maybePrefetchBuffer(*col, false);
        } else {
            const double extendMultiplier = dragging ? 3.0 : 1.0;
            queueColumnDepthShift(*col, false, extendMultiplier);
        }
    }
}

void MainWindow::restartColumnClient(DomColumn &col)
{
    if (!col.client) {
        return;
    }
    const auto src = symbolSourceForAccount(col.accountName);
    QString exch;
    switch (src) {
    case SymbolSource::Mexc:
        exch = QStringLiteral("mexc");
        break;
    case SymbolSource::MexcFutures:
        exch = QStringLiteral("mexc_futures");
        break;
    case SymbolSource::BinanceSpot:
        exch = QStringLiteral("binance");
        break;
    case SymbolSource::BinanceFutures:
        exch = QStringLiteral("binance_futures");
        break;
    case SymbolSource::UzxSpot:
        exch = QStringLiteral("uzxspot");
        break;
    case SymbolSource::UzxSwap:
        exch = QStringLiteral("uzxswap");
        break;
    case SymbolSource::Lighter:
        exch = QStringLiteral("lighter");
        break;
    }

    // Always refresh backend proxy settings from the current connection profile before restart.
    if (m_connectionStore) {
        ConnectionStore::Profile prof = ConnectionStore::Profile::MexcSpot;
        switch (src) {
        case SymbolSource::MexcFutures:
            prof = ConnectionStore::Profile::MexcFutures;
            break;
        case SymbolSource::UzxSpot:
            prof = ConnectionStore::Profile::UzxSpot;
            break;
        case SymbolSource::UzxSwap:
            prof = ConnectionStore::Profile::UzxSwap;
            break;
        case SymbolSource::BinanceSpot:
            prof = ConnectionStore::Profile::BinanceSpot;
            break;
        case SymbolSource::BinanceFutures:
            prof = ConnectionStore::Profile::BinanceFutures;
            break;
        case SymbolSource::Lighter:
            prof = ConnectionStore::Profile::Lighter;
            break;
        case SymbolSource::Mexc:
        default:
            prof = ConnectionStore::Profile::MexcSpot;
            break;
        }
        const MexcCredentials creds = m_connectionStore->loadMexcCredentials(prof);
        QString proxyType = creds.proxyType.trimmed().toLower();
        if (proxyType == QStringLiteral("https")) {
            proxyType = QStringLiteral("http");
        }
        const QString proxyRaw = creds.proxy.trimmed();
        col.client->setProxy(proxyType, proxyRaw);
    }
    const int effectiveLevels = effectiveLevelsForColumn(col);
    col.hasBuffer = false;
    col.bufferMinTick = 0;
    col.bufferMaxTick = 0;
    col.displayMinTick = 0;
    col.displayMaxTick = 0;
    col.visibleCenterTick = 0;
    col.userScrolling = false;
    col.pendingExtendUp = false;
    col.pendingExtendDown = false;
    col.pendingViewportUpdate = false;
    col.pendingViewportBottom = 0;
    col.pendingViewportTop = 0;
    col.pendingViewportRevision = 0;
    col.lastViewportBottom = 0;
    col.lastViewportTop = 0;
    col.lastViewportRevision = 0;
    col.extendQueuedShiftUp = 0;
    col.extendQueuedShiftDown = 0;
    col.bufferRevision = 0;
    col.scrollValueValid = false;
    col.lastScrollBarValue = 0;
    col.client->restart(col.symbol, effectiveLevels, exch);
}

QVector<VolumeHighlightRule> MainWindow::defaultVolumeHighlightRules() const
{
    return {
        {1000.0, QColor("#ffd54f")},
        {2000.0, QColor("#ffb74d")},
        {10000.0, QColor("#ff8a65")},
        {50000.0, QColor("#ffb74d")},
        {100000.0, QColor("#ffd54f")}
    };
}

void MainWindow::applyVolumeRulesToAllDoms()
{
    for (auto &tab : m_tabs) {
        for (auto &col : tab.columnsData) {
            if (col.dom) {
                col.dom->setVolumeHighlightRules(m_volumeRules);
            }
        }
    }
}

void MainWindow::showEvent(QShowEvent *event)
{
    QMainWindow::showEvent(event);
#ifdef Q_OS_WIN
    // Enable native snapping/resizing, but keep frameless (no system caption).
    if (!m_nativeSnapEnabled) {
        WId id = winId();
        HWND hwnd = reinterpret_cast<HWND>(id);
        if (hwnd) {
            LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
            // Keep WS_CAPTION off so system doesn't draw its title bar; enable thick frame
            // so native snapping and resize work.
            style &= ~WS_CAPTION;
            style |= WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
            SetWindowLongPtr(hwnd, GWL_STYLE, style);
            SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        }
        m_nativeSnapEnabled = true;
    }
#endif
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    saveUserSettings();
    // Гарантированно закрываем приложение даже если остаются невидимые окна или фоновые таймеры.
    QTimer::singleShot(0, qApp, []() { QCoreApplication::exit(0); });
    QMainWindow::closeEvent(event);
}
void MainWindow::addLocalOrderMarker(const QString &accountName,
                                     const QString &symbol,
                                     OrderSide side,
                                     double price,
                                     double quantity,
                                     const QString &orderId,
                                     qint64 createdMs)
{
    if (orderId.isEmpty()) {
        return;
    }
    const qint64 ts = createdMs > 0 ? createdMs : QDateTime::currentMSecsSinceEpoch();
    const QString symUpper = normalizedSymbolKey(symbol);
    if (symUpper.isEmpty()) {
        return;
    }
    if (m_pendingCancelSymbols.contains(symUpper)) {
        logMarkerEvent(QStringLiteral("[Marker skip] pending cancel sym=%1 acc=%2 id=%3")
                           .arg(symUpper)
                           .arg(accountName)
                           .arg(orderId));
        return;
    }
    const QString accountDisplay = normalizedAccountLabel(accountName);
    const QString accountKey = normalizedAccountKey(accountDisplay);
    DomWidget::LocalOrderMarker marker;
    marker.price = price;
    marker.quantity = std::abs(quantity * price);
    marker.side = side;
    marker.createdMs = ts;
    marker.orderId = orderId;
    MarkerBucket &bucket = m_markerBuckets[symUpper][accountKey];
    bucket.accountLabel = accountDisplay;
    bucket.pending.insert(orderId, marker);
    enforcePendingLimit(bucket);
    refreshColumnsForSymbol(symUpper);
    const QString sideName = side == OrderSide::Buy ? QStringLiteral("BUY") : QStringLiteral("SELL");
    const double baseQty = std::abs(quantity);
    const double notional = std::abs(quantity * price);
    logMarkerEvent(QStringLiteral("[Marker add] acc=%1 sym=%2 id=%3 %4 @ %5 qty=%6 notional=%7")
                        .arg(accountDisplay)
                        .arg(symUpper)
                        .arg(orderId)
                        .arg(sideName)
                        .arg(price, 0, 'f', 8)
                        .arg(baseQty, 0, 'f', 8)
                        .arg(notional, 0, 'f', 8));
}

void MainWindow::removeLocalOrderMarker(const QString &accountName,
                                        const QString &symbol,
                                        const QString &orderId)
{
    const QString symUpper = normalizedSymbolKey(symbol);
    if (symUpper.isEmpty() || orderId.isEmpty()) {
        return;
    }
    const QString accountDisplay = normalizedAccountLabel(accountName);
    const QString accountKey = normalizedAccountKey(accountDisplay);
    DomWidget::LocalOrderMarker removed;
    bool touched = false;
    auto symIt = m_markerBuckets.find(symUpper);
    if (symIt != m_markerBuckets.end()) {
        auto &perAccount = symIt.value();
        auto accIt = perAccount.find(accountKey);
        if (accIt != perAccount.end()) {
            auto &bucket = accIt.value();
            if (bucket.pending.contains(orderId)) {
                removed = bucket.pending.value(orderId);
                bucket.pending.remove(orderId);
                touched = true;
            }
            if (bucket.confirmed.contains(orderId)) {
                removed = bucket.confirmed.value(orderId);
                bucket.confirmed.remove(orderId);
                touched = true;
            }
            if (bucket.pending.isEmpty() && bucket.confirmed.isEmpty()) {
                perAccount.remove(accountKey);
            }
        }
        if (perAccount.isEmpty()) {
            m_markerBuckets.remove(symUpper);
        }
    }
    if (touched) {
        refreshColumnsForSymbol(symUpper);
    }
    const QString sideName = removed.side == OrderSide::Buy ? QStringLiteral("BUY") : QStringLiteral("SELL");
    if (!removed.orderId.isEmpty()) {
        logMarkerEvent(QStringLiteral("[Marker remove] acc=%1 sym=%2 id=%3 %4 @ %5")
                           .arg(accountDisplay)
                           .arg(symUpper)
                           .arg(removed.orderId)
                           .arg(sideName)
                           .arg(removed.price, 0, 'f', 8));
    } else {
        // Marker might have already been cleared by a remote order sync after cancel-all, or by order fill.
        // Avoid noisy logs for this benign race.
        return;
    }
}

void MainWindow::handleLocalOrdersUpdated(const QString &accountName,
                                          const QString &symbol,
                                          const QVector<DomWidget::LocalOrderMarker> &markers)
{
    const QString accountLabel = normalizedAccountLabel(accountName);
    const QString accountKey = normalizedAccountKey(accountLabel);
    const QString targetSymbol = normalizedSymbolKey(symbol);
    if (targetSymbol.isEmpty()) {
        return;
    }
    const QString lastKey = lastLimitOrderKey(targetSymbol, accountKey);
    auto lastIt = m_lastLimitOrderStackByKey.find(lastKey);
    if (lastIt != m_lastLimitOrderStackByKey.end()) {
        QSet<QString> liveIds;
        liveIds.reserve(markers.size());
        for (const auto &marker : markers) {
            if (!marker.orderId.isEmpty()) {
                liveIds.insert(marker.orderId);
            }
        }
        auto &stack = lastIt.value();
        stack.erase(std::remove_if(stack.begin(),
                                   stack.end(),
                                   [&liveIds](const QString &id) { return !liveIds.contains(id); }),
                    stack.end());
        if (stack.isEmpty()) {
            m_lastLimitOrderStackByKey.erase(lastIt);
        }
        auto pendingIt = m_pendingLimitCancelByKey.find(lastKey);
        if (pendingIt != m_pendingLimitCancelByKey.end()) {
            auto &pending = pendingIt.value();
            for (auto it = pending.begin(); it != pending.end();) {
                if (!liveIds.contains(it.key())) {
                    it = pending.erase(it);
                } else {
                    ++it;
                }
            }
            if (pending.isEmpty()) {
                m_pendingLimitCancelByKey.erase(pendingIt);
            }
        }
    }
    MarkerBucket &bucket = m_markerBuckets[targetSymbol][accountKey];
    bucket.accountLabel = accountLabel;
    const int prevConfirmed = bucket.confirmed.size();
    const int prevPending = bucket.pending.size();
    logMarkerEvent(QStringLiteral("[orders] localOrdersUpdated account=%1 symbol=%2 incoming=%3 confirmedBefore=%4 pendingBefore=%5")
                       .arg(accountLabel)
                       .arg(targetSymbol)
                       .arg(summarizeMarkers(markers))
                       .arg(prevConfirmed)
                       .arg(prevPending));
    if (markers.isEmpty()) {
        bucket.confirmed.clear();
        if (m_pendingCancelSymbols.contains(targetSymbol)) {
            bucket.pending.clear();
        }
    } else {
        bucket.confirmed.clear();
        for (auto marker : markers) {
            if (marker.orderId.isEmpty()) {
                marker.orderId = QStringLiteral("remote_%1_%2_%3")
                                     .arg(targetSymbol)
                                     .arg(marker.price, 0, 'f', 8)
                                     .arg(marker.createdMs);
            }
            bucket.confirmed.insert(marker.orderId, marker);
            bucket.pending.remove(marker.orderId);
        }
    }
    if (bucket.pending.isEmpty() && bucket.confirmed.isEmpty()) {
        auto symIt = m_markerBuckets.find(targetSymbol);
        if (symIt != m_markerBuckets.end()) {
            symIt.value().remove(accountKey);
            if (symIt.value().isEmpty()) {
                m_markerBuckets.erase(symIt);
            }
        }
    }
    logMarkerEvent(QStringLiteral("[orders] post-update account=%1 symbol=%2 confirmed=%3 pending=%4 suppressRemote=%5")
                       .arg(accountLabel)
                       .arg(targetSymbol)
                       .arg(bucket.confirmed.size())
                       .arg(bucket.pending.size())
                       .arg(m_pendingCancelSymbols.contains(targetSymbol)));
    refreshColumnsForSymbol(targetSymbol);
    if (markers.isEmpty()) {
        clearPendingCancelForSymbol(targetSymbol);
    }
    Q_UNUSED(lastKey);
}

void MainWindow::refreshColumnMarkers(DomColumn &col)
{
    const QString normalizedSymbol = col.symbol.trimmed().toUpper();
    if (normalizedSymbol.isEmpty()) {
        col.localOrders.clear();
        if (col.dom) {
            col.dom->setLocalOrders(col.localOrders);
        }
        if (col.prints) {
            QVector<LocalOrderMarker> empty;
            col.prints->setLocalOrders(empty);
        }
        return;
    }
    const QString accountKey = normalizedAccountKey(col.accountName);
    const bool suppressRemote = m_pendingCancelSymbols.contains(normalizedSymbol);
    QVector<DomWidget::LocalOrderMarker> combined;
    MarkerBucket *bucketPtr = nullptr;
    auto symIt = m_markerBuckets.find(normalizedSymbol);
    int bucketConfirmed = 0;
    int bucketPending = 0;
    if (symIt != m_markerBuckets.end()) {
        auto accountIt = symIt.value().find(accountKey);
        if (accountIt != symIt.value().end()) {
            bucketPtr = &accountIt.value();
        } else if (isPlaceholderAccountLabel(col.accountName) && !symIt.value().isEmpty()) {
            const SymbolSource targetSource = symbolSourceForAccount(col.accountName);
            auto matchIt = symIt.value().end();
            for (auto it = symIt.value().begin(); it != symIt.value().end(); ++it) {
                const QString label = it.value().accountLabel.trimmed();
                if (label.isEmpty()) {
                    continue;
                }
                if (symbolSourceForAccount(label) == targetSource) {
                    matchIt = it;
                    break;
                }
            }
            if (matchIt != symIt.value().end()) {
                bucketPtr = &matchIt.value();
                if (bucketPtr && !bucketPtr->accountLabel.isEmpty()
                    && bucketPtr->accountLabel.compare(col.accountName, Qt::CaseInsensitive) != 0) {
                    col.accountName = bucketPtr->accountLabel;
                    col.accountColor = accountColorFor(col.accountName);
                    if (col.tickerLabel) {
                        col.tickerLabel->setProperty("accountColor", col.accountColor);
                        applyTickerLabelStyle(col.tickerLabel, col.accountColor, false);
                    }
                    applyHeaderAccent(col);
                }
            }
        }
        if (bucketPtr) {
            pruneExpiredPending(*bucketPtr, true);
            combined = aggregateMarkersForDisplay(*bucketPtr, suppressRemote);
            bucketConfirmed = bucketPtr->confirmed.size();
            bucketPending = bucketPtr->pending.size();
        }
    }
    logMarkerEvent(QStringLiteral("[orders] refreshColumn symbol=%1 account=%2 combined=%3 bucketConfirmed=%4 bucketPending=%5 suppressRemote=%6")
                       .arg(normalizedSymbol)
                       .arg(col.accountName)
                       .arg(combined.size())
                       .arg(bucketConfirmed)
                       .arg(bucketPending)
                       .arg(suppressRemote));
    col.localOrders = combined;
    if (col.dom) {
        col.dom->setLocalOrders(col.localOrders);
    }
    if (col.prints) {
        QVector<LocalOrderMarker> printMarkers;
        if (bucketPtr && !suppressRemote) {
            printMarkers.reserve(bucketPtr->confirmed.size() + bucketPtr->pending.size());
            QSet<QString> confirmedIds;
            confirmedIds.reserve(bucketPtr->confirmed.size());
            for (auto it = bucketPtr->confirmed.cbegin(); it != bucketPtr->confirmed.cend(); ++it) {
                if (!it.key().isEmpty()) {
                    confirmedIds.insert(it.key());
                }
            }
            auto pushMarker = [&printMarkers](const DomWidget::LocalOrderMarker &m) {
                if (!(m.price > 0.0) || !(m.quantity > 0.0)) {
                    return;
                }
                LocalOrderMarker pm;
                pm.price = m.price;
                pm.quantity = m.quantity;
                pm.buy = (m.side == OrderSide::Buy);
                pm.createdMs = m.createdMs;
                pm.orderId = m.orderId;
                printMarkers.push_back(pm);
            };
            for (const auto &m : bucketPtr->confirmed) {
                pushMarker(m);
            }
            for (auto it = bucketPtr->pending.cbegin(); it != bucketPtr->pending.cend(); ++it) {
                if (!it.key().isEmpty() && confirmedIds.contains(it.key())) {
                    continue;
                }
                pushMarker(it.value());
            }
        }
        if (col.sltpHasSl && col.sltpSlPrice > 0.0) {
            LocalOrderMarker pm;
            pm.price = col.sltpSlPrice;
            pm.quantity = 0.0;
            pm.buy = false;
            pm.createdMs = col.sltpSlCreatedMs > 0 ? col.sltpSlCreatedMs : QDateTime::currentMSecsSinceEpoch();
            pm.label = QStringLiteral("SL");
            pm.fillColor = QColor("#e53935");
            pm.borderColor = QColor("#992626");
            printMarkers.push_back(pm);
        }
        if (col.sltpHasTp && col.sltpTpPrice > 0.0) {
            LocalOrderMarker pm;
            pm.price = col.sltpTpPrice;
            pm.quantity = 0.0;
            pm.buy = false;
            pm.createdMs = col.sltpTpCreatedMs > 0 ? col.sltpTpCreatedMs : QDateTime::currentMSecsSinceEpoch();
            pm.label = QStringLiteral("TP");
            pm.fillColor = QColor("#4caf50");
            pm.borderColor = QColor("#2f6c37");
            printMarkers.push_back(pm);
        }
        col.prints->setLocalOrders(printMarkers);
    }
}

void MainWindow::refreshSltpMarkers(DomColumn &col)
{
    // SL/TP markers are rendered as "envelopes" in the Prints column.
    // Also highlight the corresponding tick in the DOM price column so the order stays visible
    // even when prints overlap the marker.
    if (col.dom) {
        col.dom->setPriceTextMarkers(QVector<DomWidget::PriceTextMarker>());
        applyDomHighlightPrices(col);
    }
}

void MainWindow::clearSltpMarkers(DomColumn &col)
{
    col.sltpHasSl = false;
    col.sltpSlPrice = 0.0;
    col.sltpSlCreatedMs = 0;
    col.sltpSlMissCount = 0;
    col.sltpHasTp = false;
    col.sltpTpPrice = 0.0;
    col.sltpTpCreatedMs = 0;
    col.sltpTpMissCount = 0;
    if (col.dom) {
        col.dom->setPriceTextMarkers(QVector<DomWidget::PriceTextMarker>());
        col.dragPreviewActive = false;
        col.dragPreviewPrice = 0.0;
        applyDomHighlightPrices(col);
    }
}

void MainWindow::applyDomHighlightPrices(DomColumn &col)
{
    if (!col.dom) {
        return;
    }
    QVector<double> highlight;
    if (col.sltpHasSl && col.sltpSlPrice > 0.0) {
        highlight.push_back(col.sltpSlPrice);
    }
    if (col.sltpHasTp && col.sltpTpPrice > 0.0) {
        highlight.push_back(col.sltpTpPrice);
    }
    if (col.dragPreviewActive && col.dragPreviewPrice > 0.0) {
        highlight.push_back(col.dragPreviewPrice);
    }
    col.dom->setHighlightPrices(highlight);
}

QVector<DomWidget::LocalOrderMarker> MainWindow::aggregateMarkersForDisplay(const MarkerBucket &bucket,
                                                                            bool suppressRemote) const
{
    QVector<DomWidget::LocalOrderMarker> result;
    QHash<QString, DomWidget::LocalOrderMarker> aggregated;
    QSet<QString> confirmedIds;
    auto fold = [&](const QHash<QString, DomWidget::LocalOrderMarker> &source, bool skipConfirmedIds) {
        for (auto it = source.cbegin(); it != source.cend(); ++it) {
            const QString &orderId = it.key();
            const auto &marker = it.value();
            if (marker.price <= 0.0 || marker.quantity <= 0.0) {
                continue;
            }
            if (skipConfirmedIds && !orderId.isEmpty() && confirmedIds.contains(orderId)) {
                continue;
            }
            const QString key =
                QString::number(static_cast<int>(marker.side)) + QLatin1Char('|')
                + QString::number(marker.price, 'f', 8);
            auto it = aggregated.find(key);
            if (it == aggregated.end()) {
                aggregated.insert(key, marker);
            } else {
                auto &existing = it.value();
                existing.quantity += marker.quantity;
                existing.createdMs = std::min(existing.createdMs, marker.createdMs);
            }
        }
    };
    if (!suppressRemote) {
        confirmedIds.reserve(bucket.confirmed.size());
        for (auto it = bucket.confirmed.cbegin(); it != bucket.confirmed.cend(); ++it) {
            if (!it.key().isEmpty()) {
                confirmedIds.insert(it.key());
            }
        }
        fold(bucket.confirmed, false);
        fold(bucket.pending, true);
    }
    result.reserve(aggregated.size());
    for (const auto &marker : aggregated) {
        result.push_back(marker);
    }
    return result;
}

void MainWindow::enforcePendingLimit(MarkerBucket &bucket)
{
    if (bucket.pending.size() <= kMaxPendingMarkersPerBucket) {
        return;
    }
    QVector<QPair<QString, qint64>> entries;
    entries.reserve(bucket.pending.size());
    for (auto it = bucket.pending.cbegin(); it != bucket.pending.cend(); ++it) {
        entries.push_back(qMakePair(it.key(), it.value().createdMs));
    }
    std::sort(entries.begin(), entries.end(), [](const auto &a, const auto &b) {
        return a.second < b.second;
    });
    const int excess = bucket.pending.size() - kMaxPendingMarkersPerBucket;
    for (int i = 0; i < excess && i < entries.size(); ++i) {
        bucket.pending.remove(entries[i].first);
    }
}

void MainWindow::pruneExpiredPending(MarkerBucket &bucket, bool allowRemoval) const
{
    if (!allowRemoval || bucket.pending.isEmpty()) {
        return;
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QList<QString> expired;
    for (auto it = bucket.pending.cbegin(); it != bucket.pending.cend(); ++it) {
        if (now - it.value().createdMs >= kPendingMarkerTimeoutMs) {
            expired.push_back(it.key());
        }
    }
    for (const auto &key : expired) {
        bucket.pending.remove(key);
    }
}


void MainWindow::clearColumnLocalMarkers(DomColumn &col)
{
    clearSymbolLocalMarkersInternal(col.symbol.toUpper(), false);
}


bool MainWindow::clearSymbolLocalMarkersInternal(const QString &symbolUpper, bool markPending)
{
    const QString normalized = normalizedSymbolKey(symbolUpper);
    if (normalized.isEmpty()) {
        return false;
    }
    cancelDelayedMarkers(normalized);
    auto symIt = m_markerBuckets.find(normalized);
    if (symIt != m_markerBuckets.end()) {
        m_markerBuckets.erase(symIt);
    }
    bool touched = false;
    for (auto &tab : m_tabs) {
        for (auto &col : tab.columnsData) {
            if (col.symbol.toUpper() != normalized) {
                continue;
            }
            touched = true;
        }
    }
    if (!touched) {
        return false;
    }
    if (markPending) {
        markPendingCancelForSymbol(normalized);
    } else {
        refreshColumnsForSymbol(normalized);
    }
    return true;
}


void MainWindow::clearSymbolLocalMarkers(const QString &symbolUpper)
{
    clearSymbolLocalMarkersInternal(symbolUpper, true);
}

void MainWindow::cancelDelayedMarkers(const QString &symbolUpper, const QString &accountKey)
{
    const QString normalized = normalizedSymbolKey(symbolUpper);
    if (normalized.isEmpty()) {
        return;
    }
    const QString prefix = markerTimerPrefix(normalized, accountKey);
    QList<QString> removeKeys;
    for (auto it = m_markerDelayTimers.begin(); it != m_markerDelayTimers.end(); ++it) {
        if (!it.key().startsWith(prefix)) {
            continue;
        }
        if (it.value()) {
            it.value()->stop();
            it.value()->deleteLater();
        }
        removeKeys.push_back(it.key());
    }
    for (const auto &key : removeKeys) {
        m_markerDelayTimers.remove(key);
    }
}

void MainWindow::removeMarkerDelayTimer(const QString &timerKey, QTimer *timer)
{
    auto it = m_markerDelayTimers.find(timerKey);
    if (it == m_markerDelayTimers.end()) {
        if (timer) {
            timer->stop();
            timer->deleteLater();
        }
        return;
    }
    QPointer<QTimer> stored = it.value();
    if (stored && (!timer || stored == timer)) {
        stored->stop();
        stored->deleteLater();
    } else if (timer) {
        timer->stop();
        timer->deleteLater();
    }
    m_markerDelayTimers.erase(it);
}


void MainWindow::markPendingCancelForSymbol(const QString &symbol)
{
    const QString normalized = symbol.trimmed().toUpper();
    if (normalized.isEmpty()) {
        return;
    }
    if (!m_pendingCancelSymbols.contains(normalized)) {
        m_pendingCancelSymbols.insert(normalized);
        logMarkerEvent(QStringLiteral("[orders] markPendingCancel symbol=%1").arg(normalized));
    }
    cancelDelayedMarkers(normalized);
    refreshColumnsForSymbol(normalized);
    schedulePendingCancelTimer(normalized);
}


void MainWindow::clearPendingCancelForSymbol(const QString &symbol)
{
    const QString normalized = symbol.trimmed().toUpper();
    if (normalized.isEmpty()) {
        return;
    }
    auto symbolBuckets = m_markerBuckets.find(normalized);
    if (symbolBuckets != m_markerBuckets.end()) {
        bool anyActive = false;
        for (auto it = symbolBuckets.value().begin(); it != symbolBuckets.value().end(); ++it) {
            pruneExpiredPending(it.value(), true);
            if (!it.value().confirmed.isEmpty() || !it.value().pending.isEmpty()) {
                anyActive = true;
                break;
            }
        }
        if (anyActive) {
            return;
        }
    }
    if (!m_pendingCancelSymbols.remove(normalized)) {
        return;
    }
    logMarkerEvent(QStringLiteral("[orders] clearPendingCancel symbol=%1").arg(normalized));
    auto timerIt = m_pendingCancelTimers.find(normalized);
    if (timerIt != m_pendingCancelTimers.end()) {
        timerIt.value()->stop();
        timerIt.value()->deleteLater();
        m_pendingCancelTimers.erase(timerIt);
    }
    refreshColumnsForSymbol(normalized);
}


void MainWindow::schedulePendingCancelTimer(const QString &symbol)
{
    const QString normalized = symbol.trimmed().toUpper();
    if (normalized.isEmpty()) {
        return;
    }
    QTimer *timer = nullptr;
    auto timerIt = m_pendingCancelTimers.find(normalized);
    if (timerIt != m_pendingCancelTimers.end()) {
        timer = timerIt.value();
    } else {
        timer = new QTimer(this);
        timer->setSingleShot(true);
        connect(timer, &QTimer::timeout, this, [this, normalized]() {
            if (!m_pendingCancelSymbols.contains(normalized)) {
                return;
            }
            logMarkerEvent(QStringLiteral("[orders] pendingCancel timeout symbol=%1 clearing markers").arg(normalized));
            clearSymbolLocalMarkersInternal(normalized, false);
        });
        m_pendingCancelTimers.insert(normalized, timer);
    }
    timer->start(kPendingCancelRetryMs);
}



void MainWindow::refreshColumnsForSymbol(const QString &symbolUpper)
{
    for (auto &tab : m_tabs) {
        for (auto &col : tab.columnsData) {
            if (col.symbol.toUpper() == symbolUpper) {
                refreshColumnMarkers(col);
            }
        }
    }
}


void MainWindow::fetchSymbolLibrary()
{
    if (m_symbolRequestInFlight) {
        setPickersRefreshState(SymbolSource::Mexc, true);
        return;
    }
    const QUrl url(QStringLiteral("https://api.mexc.com/api/v3/exchangeInfo"));
    QNetworkRequest req(url);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    m_symbolRequestInFlight = true;
    setPickersRefreshState(SymbolSource::Mexc, true);
    auto *reply = m_symbolFetcher.get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        const auto err = reply->error();
        const QByteArray raw = reply->readAll();
        reply->deleteLater();
        if (err != QNetworkReply::NoError) {
            m_symbolRequestInFlight = false;
            qWarning() << "[symbols] fetch failed:" << err;
            statusBar()->showMessage(tr("Failed to load symbols"), 2500);
            m_symbolRequestInFlight = false;
            setPickersRefreshState(SymbolSource::Mexc, false);
            return;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(raw);
        if (!doc.isObject()) {
            m_symbolRequestInFlight = false;
            qWarning() << "[symbols] invalid payload";
            m_symbolRequestInFlight = false;
            setPickersRefreshState(SymbolSource::Mexc, false);
            return;
        }
        const QJsonArray arr = doc.object().value(QStringLiteral("symbols")).toArray();
        QStringList fetched;
        QSet<QString> apiOff;
        fetched.reserve(arr.size());
        for (const auto &v : arr) {
            const QJsonObject obj = v.toObject();
            QString sym = obj.value(QStringLiteral("symbol")).toString().trimmed().toUpper();
            if (sym.isEmpty()) {
                continue;
            }
            const QString statusRaw = obj.value(QStringLiteral("status")).toString().trimmed();
            const QString statusUpper = statusRaw.toUpper();
            const bool spotAllowed = obj.value(QStringLiteral("isSpotTradingAllowed")).toBool(true);
            // MEXC returns numeric statuses for symbols (1 = enabled, 2 = disabled), while UZX
            // sticks to strings such as "TRADING". Treat any explicit "off" flag as disabled and
            // default to enabled for unknown statuses to avoid painting the whole list red.
            const bool explicitBlock =
                statusUpper == QStringLiteral("BREAK")
                || statusUpper == QStringLiteral("HALT")
                || statusUpper == QStringLiteral("HALTED")
                || statusUpper == QStringLiteral("OFFLINE")
                || statusUpper == QStringLiteral("DISABLED")
                || statusUpper == QStringLiteral("SUSPEND")
                || statusUpper == QStringLiteral("SUSPENDED")
                || statusUpper == QStringLiteral("MAINTENANCE")
                || statusUpper == QStringLiteral("MAINTAIN")
                || statusUpper == QStringLiteral("CLOSE")
                || statusUpper == QStringLiteral("STOP")
                || statusUpper == QStringLiteral("STOPPED")
                || statusUpper == QStringLiteral("PAUSE")
                || statusUpper == QStringLiteral("PAUSED")
                || statusUpper == QStringLiteral("DELISTED")
                || statusRaw == QStringLiteral("0")
                || statusRaw == QStringLiteral("2");
            fetched.push_back(sym);
            if (explicitBlock || !spotAllowed) {
                apiOff.insert(sym);
            }
        }
        if (fetched.isEmpty()) {
            m_symbolRequestInFlight = false;
            qWarning() << "[symbols] empty symbols list";
            statusBar()->showMessage(tr("Failed to load symbols"), 2500);
            m_symbolRequestInFlight = false;
            setPickersRefreshState(SymbolSource::Mexc, false);
            return;
        }
        requestDefaultSymbolAllowList(std::move(fetched), std::move(apiOff), fetched.size());
    });
}

void MainWindow::requestDefaultSymbolAllowList(QStringList symbols,
                                               QSet<QString> apiOff,
                                               int fetchedCount)
{
    const QUrl defaultsUrl(QStringLiteral("https://api.mexc.com/api/v3/defaultSymbols"));
    QNetworkRequest req(defaultsUrl);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    auto *reply = m_symbolFetcher.get(req);
    connect(reply,
            &QNetworkReply::finished,
            this,
            [this, reply, symbols = std::move(symbols), apiOff = std::move(apiOff), fetchedCount]() mutable {
                const auto err = reply->error();
                const QByteArray raw = reply->readAll();
                reply->deleteLater();
                if (err == QNetworkReply::NoError) {
                    const QJsonDocument doc = QJsonDocument::fromJson(raw);
                    if (doc.isObject()) {
                        const QJsonArray arr = doc.object().value(QStringLiteral("data")).toArray();
                        if (!arr.isEmpty()) {
                            QSet<QString> allowed;
                            allowed.reserve(arr.size());
                            for (const auto &val : arr) {
                                const QString sym = val.toString().trimmed().toUpper();
                                if (!sym.isEmpty()) {
                                    allowed.insert(sym);
                                }
                            }
                            if (!allowed.isEmpty()) {
                                for (const QString &sym : symbols) {
                                    if (!allowed.contains(sym)) {
                                        apiOff.insert(sym);
                                    }
                                }
                            }
                        }
                    }
                } else {
                    qWarning() << "[symbols] defaultSymbols fetch failed:" << err;
                }
                finalizeSymbolFetch(std::move(symbols), std::move(apiOff), fetchedCount);
            });
}

void MainWindow::finalizeSymbolFetch(QStringList symbols, QSet<QString> apiOff, int fetchedCount)
{
    mergeSymbolLibrary(symbols, apiOff);
    m_symbolRequestInFlight = false;
    broadcastSymbolsToPickers(SymbolSource::Mexc);
    setPickersRefreshState(SymbolSource::Mexc, false);
    statusBar()->showMessage(tr("Loaded %1 symbols").arg(fetchedCount), 2000);
}

void MainWindow::fetchMexcFuturesSymbols()
{
    if (m_mexcFuturesRequestInFlight) {
        setPickersRefreshState(SymbolSource::MexcFutures, true);
        return;
    }
    const QUrl url(QStringLiteral("https://contract.mexc.com/api/v1/contract/detail"));
    QNetworkRequest req(url);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    m_mexcFuturesRequestInFlight = true;
    setPickersRefreshState(SymbolSource::MexcFutures, true);
    auto *reply = m_symbolFetcher.get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        const auto err = reply->error();
        const QByteArray raw = reply->readAll();
        reply->deleteLater();
        if (err != QNetworkReply::NoError) {
            m_mexcFuturesRequestInFlight = false;
            qWarning() << "[symbols] futures fetch failed:" << err;
            statusBar()->showMessage(tr("Failed to load futures symbols"), 2500);
            setPickersRefreshState(SymbolSource::MexcFutures, false);
            return;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(raw);
        if (!doc.isObject()) {
            m_mexcFuturesRequestInFlight = false;
            qWarning() << "[symbols] invalid futures payload";
            setPickersRefreshState(SymbolSource::MexcFutures, false);
            return;
        }
        const QJsonArray arr = doc.object().value(QStringLiteral("data")).toArray();
        if (arr.isEmpty()) {
            m_mexcFuturesRequestInFlight = false;
            qWarning() << "[symbols] empty futures list";
            statusBar()->showMessage(tr("Failed to load futures symbols"), 2500);
            setPickersRefreshState(SymbolSource::MexcFutures, false);
            return;
        }
        QStringList futures;
        futures.reserve(arr.size());
        QSet<QString> seen;
        QSet<QString> apiOff;
        QHash<QString, int> maxLev;
        QHash<QString, QList<int>> levTags;
        for (const auto &val : arr) {
            const QJsonObject obj = val.toObject();
            QString sym = obj.value(QStringLiteral("symbol")).toString().trimmed().toUpper();
            if (sym.isEmpty() || seen.contains(sym)) {
                continue;
            }
            seen.insert(sym);
            futures.push_back(sym);
            const int parsedMaxLev = obj.value(QStringLiteral("maxLeverage")).toInt(0);
            if (parsedMaxLev > 0) {
                maxLev.insert(sym, parsedMaxLev);
            }
            const QJsonArray tagsArr = obj.value(QStringLiteral("leverageTags")).toArray();
            if (!tagsArr.isEmpty()) {
                QList<int> tags;
                tags.reserve(tagsArr.size());
                for (const auto &tv : tagsArr) {
                    const int v = tv.toInt(0);
                    if (v > 0) {
                        tags.push_back(v);
                    }
                }
                if (!tags.isEmpty()) {
                    std::sort(tags.begin(), tags.end());
                    tags.erase(std::unique(tags.begin(), tags.end()), tags.end());
                    levTags.insert(sym, tags);
                }
            }
            const int state = obj.value(QStringLiteral("state")).toInt(0);
            const bool apiAllowed = obj.value(QStringLiteral("apiAllowed")).toBool(true);
            if (state != 0 || !apiAllowed) {
                apiOff.insert(sym);
            }
        }
        if (futures.isEmpty()) {
            m_mexcFuturesRequestInFlight = false;
            qWarning() << "[symbols] no futures symbols parsed";
            statusBar()->showMessage(tr("Failed to load futures symbols"), 2500);
            setPickersRefreshState(SymbolSource::MexcFutures, false);
            return;
        }
        std::sort(futures.begin(), futures.end(), [](const QString &a, const QString &b) {
            return a.toUpper() < b.toUpper();
        });
        m_mexcFuturesSymbols = futures;
        m_mexcFuturesMaxLeverageBySymbol = std::move(maxLev);
        m_mexcFuturesLeverageTagsBySymbol = std::move(levTags);
        m_mexcFuturesApiOff = apiOff;
        m_mexcFuturesRequestInFlight = false;
        broadcastSymbolsToPickers(SymbolSource::MexcFutures);
        setPickersRefreshState(SymbolSource::MexcFutures, false);
        statusBar()->showMessage(tr("Loaded %1 futures contracts").arg(futures.size()), 2000);
    });
}

void MainWindow::broadcastSymbolsToPickers(SymbolSource source)
{
    for (int i = m_symbolPickers.size() - 1; i >= 0; --i) {
        SymbolPickerDialog *dlg = m_symbolPickers.at(i).data();
        if (!dlg) {
            m_symbolPickers.removeAt(i);
            continue;
        }
        if (symbolSourceForAccount(dlg->selectedAccount()) != source) {
            continue;
        }
        switch (source) {
        case SymbolSource::Mexc:
            dlg->setSymbols(m_symbolLibrary, m_apiOffSymbols);
            break;
        case SymbolSource::MexcFutures:
            dlg->setSymbols(m_mexcFuturesSymbols, m_mexcFuturesApiOff);
            break;
        case SymbolSource::BinanceSpot:
            dlg->setSymbols(m_binanceSpotSymbols, QSet<QString>());
            break;
        case SymbolSource::BinanceFutures:
            dlg->setSymbols(m_binanceFuturesSymbols, QSet<QString>());
            break;
        case SymbolSource::UzxSpot:
            dlg->setSymbols(m_uzxSpotSymbols, m_uzxSpotApiOff);
            break;
        case SymbolSource::UzxSwap:
            dlg->setSymbols(m_uzxSwapSymbols, m_uzxSwapApiOff);
            break;
        case SymbolSource::Lighter:
            dlg->setSymbols(m_lighterSymbols, m_lighterApiOff);
            break;
        }
        dlg->setRefreshInProgress(false);
    }
}

void MainWindow::setPickersRefreshState(SymbolSource source, bool refreshing)
{
    for (int i = m_symbolPickers.size() - 1; i >= 0; --i) {
        SymbolPickerDialog *dlg = m_symbolPickers.at(i).data();
        if (!dlg) {
            m_symbolPickers.removeAt(i);
            continue;
        }
        if (symbolSourceForAccount(dlg->selectedAccount()) == source) {
            dlg->setRefreshInProgress(refreshing);
        }
    }
}

void MainWindow::trackSymbolPicker(SymbolPickerDialog *dlg)
{
    if (!dlg) {
        return;
    }
    m_symbolPickers.append(QPointer<SymbolPickerDialog>(dlg));
    connect(dlg, &QObject::destroyed, this, [this, dlg]() { untrackSymbolPicker(dlg); });
}

void MainWindow::untrackSymbolPicker(SymbolPickerDialog *dlg)
{
    for (int i = m_symbolPickers.size() - 1; i >= 0; --i) {
        SymbolPickerDialog *ptr = m_symbolPickers.at(i).data();
        if (!ptr || ptr == dlg) {
            m_symbolPickers.removeAt(i);
        }
    }
}

MainWindow::SymbolSource MainWindow::symbolSourceForAccount(const QString &accountName) const
{
    const QString normalized = accountName.trimmed().toLower();
    auto srcIt = m_accountSources.constFind(normalized);
    if (srcIt != m_accountSources.constEnd()) {
        return srcIt.value();
    }
    const QString lower = accountName.toLower();
    if (lower.contains(QStringLiteral("lighter"))) {
        return SymbolSource::Lighter;
    }
    if (lower.contains(QStringLiteral("binance"))) {
        if (lower.contains(QStringLiteral("future"))
            || lower.contains(QStringLiteral("futures"))
            || lower.contains(QStringLiteral("perp"))) {
            return SymbolSource::BinanceFutures;
        }
        return SymbolSource::BinanceSpot;
    }
    if (lower.contains(QStringLiteral("uzx"))) {
        if (lower.contains(QStringLiteral("spot"))) {
            return SymbolSource::UzxSpot;
        }
        return SymbolSource::UzxSwap;
    }
    if (lower.contains(QStringLiteral("future"))
        || lower.contains(QStringLiteral("futures"))
        || lower.contains(QStringLiteral("perp"))
        || (lower.contains(QStringLiteral("swap")) && lower.contains(QStringLiteral("mexc")))) {
        return SymbolSource::MexcFutures;
    }
    return SymbolSource::Mexc;
}

void MainWindow::applyVenueAppearance(SymbolPickerDialog *dlg,
                                      SymbolSource source,
                                      const QString &accountName) const
{
    if (!dlg) {
        return;
    }
    QString iconPath;
    switch (source) {
    case SymbolSource::Mexc:
    case SymbolSource::MexcFutures:
        iconPath = resolveAssetPath(QStringLiteral("icons/logos/mexc.png"));
        break;
    case SymbolSource::BinanceSpot:
    case SymbolSource::BinanceFutures:
        iconPath = resolveAssetPath(QStringLiteral("icons/logos/binance.png"));
        break;
    case SymbolSource::UzxSpot:
    case SymbolSource::UzxSwap:
        iconPath = resolveAssetPath(QStringLiteral("icons/logos/UZX.png"));
        break;
    case SymbolSource::Lighter:
        iconPath = resolveAssetPath(QStringLiteral("icons/logos/lighter.png"));
        break;
    }
    bool futures = source == SymbolSource::MexcFutures || source == SymbolSource::BinanceFutures;
    const QString lower = accountName.trimmed().toLower();
    if (source == SymbolSource::UzxSwap) {
        futures = true;
    } else if (!futures
               && (lower.contains(QStringLiteral("future"))
                   || lower.contains(QStringLiteral("futures"))
                   || lower.contains(QStringLiteral("swap"))
                   || lower.contains(QStringLiteral("perp")))) {
        futures = true;
    }
    dlg->setVenueAppearance(iconPath, futures);
}

void MainWindow::fetchSymbolLibrary(SymbolSource source, SymbolPickerDialog *dlg)
{
    if (source == SymbolSource::Mexc) {
        if (dlg) {
            dlg->setSymbols(m_symbolLibrary, m_apiOffSymbols);
            dlg->setRefreshInProgress(m_symbolRequestInFlight);
        }
        fetchSymbolLibrary();
        return;
    }
    if (source == SymbolSource::MexcFutures) {
        if (dlg) {
            dlg->setSymbols(m_mexcFuturesSymbols, m_mexcFuturesApiOff);
            dlg->setRefreshInProgress(m_mexcFuturesRequestInFlight);
        }
        fetchMexcFuturesSymbols();
        return;
    }

    if (source == SymbolSource::BinanceSpot || source == SymbolSource::BinanceFutures) {
        const bool futures = source == SymbolSource::BinanceFutures;
        bool &inFlightRef = futures ? m_binanceFuturesRequestInFlight : m_binanceSpotRequestInFlight;
        QStringList &listRef = futures ? m_binanceFuturesSymbols : m_binanceSpotSymbols;
        if (!listRef.isEmpty()) {
            if (dlg) {
                applyVenueAppearance(dlg, source, dlg->selectedAccount());
                dlg->setSymbols(listRef, QSet<QString>());
                dlg->setRefreshInProgress(inFlightRef);
            }
            if (!inFlightRef) {
                return;
            }
        }
        if (inFlightRef) {
            return;
        }
        inFlightRef = true;
        bool *flagPtr = &inFlightRef;

        const QStringList urls = [&]() -> QStringList {
            // Prefer lighter endpoints for faster picker load.
            // Fall back to exchangeInfo if needed.
            if (futures) {
                return {
                    QStringLiteral("https://fapi.binance.com/fapi/v1/ticker/price"),
                    QStringLiteral("https://fapi.binance.com/fapi/v1/exchangeInfo"),
                };
            }
            return {
                QStringLiteral("https://api.binance.com/api/v3/ticker/price"),
                QStringLiteral("https://api1.binance.com/api/v3/ticker/price"),
                QStringLiteral("https://api2.binance.com/api/v3/ticker/price"),
                QStringLiteral("https://api3.binance.com/api/v3/ticker/price"),
                QStringLiteral("https://api.binance.com/api/v3/exchangeInfo"),
                QStringLiteral("https://api1.binance.com/api/v3/exchangeInfo"),
                QStringLiteral("https://api2.binance.com/api/v3/exchangeInfo"),
                QStringLiteral("https://api3.binance.com/api/v3/exchangeInfo"),
            };
        }();

        auto startRequest = [this, dlg, futures, flagPtr, urls](int idx, auto &&startRequestRef) -> void {
            if (idx >= urls.size()) {
                if (flagPtr) {
                    *flagPtr = false;
                }
                qWarning() << "[symbols] binance fetch failed: all endpoints failed";
                setPickersRefreshState(futures ? SymbolSource::BinanceFutures : SymbolSource::BinanceSpot, false);
                if (dlg) {
                    dlg->setRefreshInProgress(false);
                }
                return;
            }

            const QUrl url(urls.at(idx));
            auto *reply = m_symbolFetcher.get(QNetworkRequest(url));
            connect(reply, &QNetworkReply::finished, this, [this, reply, dlg, futures, flagPtr, idx, urls, startRequestRef]() mutable {
                const auto err = reply->error();
                const QByteArray raw = reply->readAll();
                reply->deleteLater();

                if (err != QNetworkReply::NoError) {
                    qWarning() << "[symbols] binance fetch failed:" << err << "url=" << urls.at(idx);
                    startRequestRef(idx + 1, startRequestRef);
                    return;
                }

                const QJsonDocument doc = QJsonDocument::fromJson(raw);
                QStringList list;

                if (doc.isArray()) {
                    // ticker/price: [{symbol, price}, ...]
                    const QJsonArray arr = doc.array();
                    list.reserve(arr.size());
                    for (const auto &v : arr) {
                        const QJsonObject obj = v.toObject();
                        QString sym = obj.value(QStringLiteral("symbol")).toString().trimmed().toUpper();
                        if (sym.isEmpty()) continue;
                        if (!list.contains(sym, Qt::CaseInsensitive)) {
                            list.push_back(sym);
                        }
                    }
                } else if (doc.isObject()) {
                    // exchangeInfo: {symbols:[...]}
                    const QJsonArray arr = doc.object().value(QStringLiteral("symbols")).toArray();
                    list.reserve(arr.size());
                    for (const auto &v : arr) {
                        const QJsonObject obj = v.toObject();
                        const QString status = obj.value(QStringLiteral("status")).toString();
                        if (!status.isEmpty() && status != QStringLiteral("TRADING")) {
                            continue;
                        }
                        QString sym = obj.value(QStringLiteral("symbol")).toString().trimmed().toUpper();
                        if (sym.isEmpty()) continue;
                        if (!list.contains(sym, Qt::CaseInsensitive)) {
                            list.push_back(sym);
                        }
                    }
                } else {
                    startRequestRef(idx + 1, startRequestRef);
                    return;
                }

                if (list.isEmpty()) {
                    startRequestRef(idx + 1, startRequestRef);
                    return;
                }

                std::sort(list.begin(), list.end(), [](const QString &a, const QString &b) {
                    return a.toUpper() < b.toUpper();
                });

                if (futures) {
                    m_binanceFuturesSymbols = list;
                } else {
                    m_binanceSpotSymbols = list;
                }

                if (flagPtr) {
                    *flagPtr = false;
                }

                const SymbolSource effectiveSource =
                    futures ? SymbolSource::BinanceFutures : SymbolSource::BinanceSpot;
                broadcastSymbolsToPickers(effectiveSource);
                setPickersRefreshState(effectiveSource, false);

                if (dlg) {
                    applyVenueAppearance(dlg, effectiveSource, dlg->selectedAccount());
                    dlg->setSymbols(list, QSet<QString>());
                    dlg->setRefreshInProgress(false);
                }
            });
        };

        setPickersRefreshState(futures ? SymbolSource::BinanceFutures : SymbolSource::BinanceSpot, true);
        startRequest(0, startRequest);
        if (dlg) {
            dlg->setRefreshInProgress(true);
        }
        return;
    }

    if (source == SymbolSource::Lighter) {
        QPointer<SymbolPickerDialog> safeDlg(dlg);
        if (safeDlg) {
            safeDlg->setSymbols(m_lighterSymbols, m_lighterApiOff);
            safeDlg->setRefreshInProgress(m_lighterRequestInFlight);
        }
        if (m_lighterRequestInFlight) {
            setPickersRefreshState(SymbolSource::Lighter, true);
            return;
        }
        if (!m_lighterSymbols.isEmpty()) {
            if (safeDlg) {
                applyVenueAppearance(safeDlg, source, safeDlg->selectedAccount());
                safeDlg->setSymbols(m_lighterSymbols, m_lighterApiOff);
            }
            return;
        }

        auto summarizeProxy = [](const QString &typeRaw, const QString &raw) -> QString {
            const QString proto =
                (typeRaw.trimmed().toLower() == QStringLiteral("socks5")) ? QStringLiteral("SOCKS5") : QStringLiteral("HTTP");
            QString host;
            QString port;
            bool auth = false;

            const QString trimmed = raw.trimmed();
            const QStringList atSplit = trimmed.split('@');
            if (atSplit.size() == 2) {
                const QStringList cp = atSplit.at(0).split(':');
                const QStringList hp = atSplit.at(1).split(':');
                if (cp.size() == 2 && hp.size() == 2) {
                    auth = true;
                    host = hp.at(0);
                    port = hp.at(1);
                }
            } else {
                const QStringList parts = trimmed.split(':');
                if (parts.size() == 2) {
                    host = parts.at(0);
                    port = parts.at(1);
                } else if (parts.size() == 4) {
                    host = parts.at(0);
                    auth = true;
                    port = parts.at(1);
                    if (port.toInt() <= 0) {
                        port = parts.at(3);
                    }
                }
            }

            if (host.isEmpty() || port.isEmpty()) {
                return QStringLiteral("%1 <unparsed>").arg(proto);
            }
            return QStringLiteral("%1 %2:%3%4").arg(proto, host, port, auth ? QStringLiteral(" auth") : QString());
        };

        QString baseUrl = QStringLiteral("https://mainnet.zklighter.elliot.ai");
        if (m_connectionStore) {
            const MexcCredentials creds = m_connectionStore->loadMexcCredentials(ConnectionStore::Profile::Lighter);
            const QString candidate = creds.baseUrl.trimmed();
            if (!candidate.isEmpty()) {
                baseUrl = candidate;
            }
        }
        while (baseUrl.endsWith(QLatin1Char('/'))) {
            baseUrl.chop(1);
        }
        const QString lighterBaseUrl = baseUrl;

        // Use the lightest public endpoints possible for the picker:
        // - `/api/v1/orderBooks` gives the perp markets list with minimal payload
        // - `/api/v1/orderBookDetails?filter=spot` provides spot markets (usually small)
        const QUrl url(baseUrl + QStringLiteral("/api/v1/orderBooks?market_id=255"));
        QNetworkRequest req(url);
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        req.setRawHeader("Accept", "application/json");
        req.setRawHeader("User-Agent", "FusionTerminal/1.0");
        m_lighterRequestInFlight = true;
        setPickersRefreshState(SymbolSource::Lighter, true);
        if (safeDlg) {
            safeDlg->setRefreshInProgress(true);
        }
        statusBar()->showMessage(tr("Loading Lighter markets..."), 1500);

        QNetworkProxy proxy(QNetworkProxy::NoProxy);
        QString proxySummary = QStringLiteral("disabled");
        if (m_connectionStore) {
            const MexcCredentials creds = m_connectionStore->loadMexcCredentials(ConnectionStore::Profile::Lighter);
            const QString proxyRaw = creds.proxy.trimmed();
            QString type = creds.proxyType.trimmed().toLower();
            if (type == QStringLiteral("https")) {
                type = QStringLiteral("http");
            }
            if (proxyRaw.isEmpty()) {
                proxy = QNetworkProxy(QNetworkProxy::DefaultProxy);
                proxySummary = QStringLiteral("system");
            } else {
                proxySummary = summarizeProxy(type, proxyRaw);
                const auto make = [&](const QString &host, int port, const QString &user, const QString &pass) {
                    const auto ptype =
                        (type == QStringLiteral("socks5")) ? QNetworkProxy::Socks5Proxy : QNetworkProxy::HttpProxy;
                    proxy = QNetworkProxy(ptype, host, port, user, pass);
                };

                bool okPort = false;
                const QStringList atSplit = proxyRaw.split('@');
                if (atSplit.size() == 2) {
                    const QString credsPart = atSplit.at(0);
                    const QString hostPart = atSplit.at(1);
                    const QStringList cp = credsPart.split(':');
                    const QStringList hp = hostPart.split(':');
                    if (cp.size() == 2 && hp.size() == 2) {
                        const int port = hp.at(1).toInt(&okPort);
                        if (okPort) {
                            make(hp.at(0), port, cp.at(0), cp.at(1));
                        }
                    }
                } else {
                    const QStringList parts = proxyRaw.split(':');
                    if (parts.size() == 2) {
                        const int port = parts.at(1).toInt(&okPort);
                        if (okPort) {
                            make(parts.at(0), port, QString(), QString());
                        }
                    } else if (parts.size() == 4) {
                        // host:port:login:pass
                        const int port = parts.at(1).toInt(&okPort);
                        if (okPort) {
                            make(parts.at(0), port, parts.at(2), parts.at(3));
                        } else {
                            // host:login:pass:port
                            const int port2 = parts.at(3).toInt(&okPort);
                            if (okPort) {
                                make(parts.at(0), port2, parts.at(1), parts.at(2));
                            }
                        }
                    }
                }

                if (proxy.type() == QNetworkProxy::NoProxy) {
                    qWarning() << "[symbols] lighter proxy parse failed, using direct";
                    proxySummary = QStringLiteral("disabled");
                }
            }
        }

        struct LighterFetchState {
            QStringList symbols;
            QSet<QString> apiOff;
            QHash<QString, int> maxLevBySymbol;
            bool salvagedPartial = false;
        };
        auto salvageSymbols = [](const QByteArray &raw, LighterFetchState &st) {
            if (raw.isEmpty()) {
                return;
            }
            const QString s = QString::fromUtf8(raw);
            static const QRegularExpression symRe(QStringLiteral("\"symbol\"\\s*:\\s*\"([^\"]+)\""));
            auto it = symRe.globalMatch(s);
            while (it.hasNext()) {
                const auto m = it.next();
                QString sym = m.captured(1).trimmed().toUpper();
                if (sym.isEmpty()) {
                    continue;
                }
                if (!st.symbols.contains(sym, Qt::CaseInsensitive)) {
                    st.symbols.push_back(sym);
                }
            }
            if (!st.symbols.isEmpty()) {
                st.salvagedPartial = true;
            }
        };
        auto state = std::make_shared<LighterFetchState>();

        const bool useDedicatedNam = proxy.type() != QNetworkProxy::NoProxy;
        QNetworkAccessManager *nam = useDedicatedNam ? new QNetworkAccessManager(this) : &m_symbolFetcher;
        if (useDedicatedNam) {
            nam->setProxy(proxy);
        }

        // Stream symbols as the response arrives so the picker doesn't stay empty
        // when the endpoint is slow or never fully completes.
        struct LighterStreamState {
            QByteArray tail;
            qint64 lastProgressMs = 0;
            qint64 lastUiUpdateMs = 0;
            int lastCount = 0;
            bool abortedByIdle = false;
        };
        auto stream = std::make_shared<LighterStreamState>();
        stream->lastProgressMs = QDateTime::currentMSecsSinceEpoch();

        auto *reply = nam->get(req);
        auto *idleTimer = new QTimer(reply);
        idleTimer->setObjectName(QStringLiteral("lighter_symbols_idle"));
        idleTimer->setInterval(1000);
        idleTimer->start();
        connect(idleTimer, &QTimer::timeout, reply, [this, reply, stream, state]() {
            if (!reply || reply->isFinished()) {
                return;
            }
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            const qint64 idleMs = now - stream->lastProgressMs;
            // If we already have some symbols and the stream stalls, abort and use the partial list.
            if (stream->lastCount > 0 && idleMs >= 10000) {
                stream->abortedByIdle = true;
                reply->abort();
                return;
            }
            // If we have nothing at all after a while, abort to avoid infinite "in flight".
            if (stream->lastCount == 0 && idleMs >= 20000) {
                stream->abortedByIdle = true;
                reply->abort();
                return;
            }
        });

        connect(reply,
                &QNetworkReply::readyRead,
                this,
                [this, reply, safeDlg, state, salvageSymbols, stream]() {
                    if (!reply) {
                        return;
                    }
                    const QByteArray chunk = reply->readAll();
                    if (chunk.isEmpty()) {
                        return;
                    }
                    stream->lastProgressMs = QDateTime::currentMSecsSinceEpoch();

                    QByteArray scan = stream->tail;
                    scan.append(chunk);
                    // Keep overlap so we can match symbols split across chunks.
                    const int maxTail = 4096;
                    if (scan.size() > maxTail) {
                        stream->tail = scan.right(maxTail);
                    } else {
                        stream->tail = scan;
                    }

                    const int before = state->symbols.size();
                    salvageSymbols(scan, *state);
                    const int after = state->symbols.size();
                    stream->lastCount = after;
                    if (!safeDlg || after <= before) {
                        return;
                    }
                    const qint64 now = QDateTime::currentMSecsSinceEpoch();
                    if (now - stream->lastUiUpdateMs < 250) {
                        return;
                    }
                    stream->lastUiUpdateMs = now;
                    // Update the picker with the partial list.
                    safeDlg->setSymbols(state->symbols, state->apiOff);
                });
        connect(reply,
                &QNetworkReply::finished,
                this,
                [this, reply, safeDlg, nam, proxySummary, state, salvageSymbols, lighterBaseUrl, useDedicatedNam, stream]() {
            const auto err = reply->error();
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QString errStr = reply->errorString();
            const QByteArray raw = reply->readAll();
            reply->deleteLater();
            auto finishError = [this, safeDlg](const QString &msg) {
                qWarning() << msg;
                logLadderStatus(msg);
                addNotification(msg);
                m_lighterRequestInFlight = false;
                setPickersRefreshState(SymbolSource::Lighter, false);
                if (safeDlg) {
                    safeDlg->setRefreshInProgress(false);
                }
            };
            auto cleanupNam = [useDedicatedNam, nam]() {
                if (useDedicatedNam && nam) {
                    nam->deleteLater();
                }
            };

            const auto parseOrderBooks = [&]() -> bool {
                const QJsonDocument doc = QJsonDocument::fromJson(raw);
                if (!doc.isObject()) {
                    return false;
                }
                const QJsonObject root = doc.object();
                if (root.value(QStringLiteral("code")).toInt(0) != 200) {
                    return false;
                }
                const QJsonArray markets = root.value(QStringLiteral("order_books")).toArray();
                state->symbols.reserve(state->symbols.size() + markets.size());
                for (const auto &v : markets) {
                    const QJsonObject obj = v.toObject();
                    QString sym = obj.value(QStringLiteral("symbol")).toString().trimmed().toUpper();
                    if (sym.isEmpty()) {
                        continue;
                    }
                    if (!state->symbols.contains(sym, Qt::CaseInsensitive)) {
                        state->symbols.push_back(sym);
                    }
                    const QString statusStr = obj.value(QStringLiteral("status")).toString().trimmed().toLower();
                    if (!statusStr.isEmpty() && statusStr != QStringLiteral("active")) {
                        state->apiOff.insert(sym);
                    }
                }
                return !state->symbols.isEmpty();
            };

            // Lighter endpoints are occasionally slow/partial; prefer JSON parse but fall back to
            // extracting symbols from a partial response so the picker isn't empty.
            bool parsed = false;
            if (err == QNetworkReply::NoError && status < 400) {
                parsed = parseOrderBooks();
            }
            if (!parsed) {
                salvageSymbols(raw, *state);
            }
            if (err == QNetworkReply::OperationCanceledError && stream && stream->abortedByIdle && !state->symbols.isEmpty()) {
                // Treat idle-abort as a partial success.
                state->salvagedPartial = true;
            }
            if (state->symbols.isEmpty()) {
                const QString body = raw.isEmpty() ? errStr : QString::fromUtf8(raw);
                const QString msg = QStringLiteral("[symbols] Lighter symbols load failed (proxy=%1 status=%2 err=%3): %4")
                                        .arg(proxySummary,
                                             QString::number(status),
                                             QString::number(static_cast<int>(err)),
                                             body.left(220));
                finishError(msg);
                cleanupNam();
                return;
            }
            if (state->salvagedPartial) {
                const QString msg =
                    QStringLiteral("[symbols] Lighter response incomplete; using partial list (%1 symbols)")
                        .arg(state->symbols.size());
                qWarning() << msg;
            }

            auto finalize = [this, safeDlg](QStringList list, QSet<QString> apiOff, QHash<QString, int> maxLevBySymbol) {
                std::sort(list.begin(), list.end(), [](const QString &a, const QString &b) {
                return a.toUpper() < b.toUpper();
            });

                m_lighterSymbols = std::move(list);
                m_lighterApiOff = std::move(apiOff);
                if (!maxLevBySymbol.isEmpty()) {
                    m_lighterMaxLeverageBySymbol = std::move(maxLevBySymbol);
                }

                // Refresh existing Lighter perp columns so max leverage and presets stay consistent.
                for (auto &tab : m_tabs) {
                    for (auto &col : tab.columnsData) {
                        const auto src = symbolSourceForAccount(col.accountName);
                        const bool lighterPerp =
                            (src == SymbolSource::Lighter) && !col.symbol.contains(QLatin1Char('/'));
                        if (!lighterPerp) {
                            continue;
                        }
                        const QString sym = col.symbol.toUpper();
                        const int maxLev = std::max(1, m_lighterMaxLeverageBySymbol.value(sym, 10));
                        const int desired =
                            std::max(1,
                                     m_futuresLeverageBySymbol.value(sym, col.leverage > 0 ? col.leverage : 20));
                        const int clamped = std::clamp(desired, 1, maxLev);
                        if (clamped != col.leverage) {
                            col.leverage = clamped;
                            m_futuresLeverageBySymbol.insert(sym, clamped);
                            if (col.leverageButton) {
                                col.leverageButton->setText(QStringLiteral("%1x").arg(clamped));
                            }
                        }
                    }
                }

                m_lighterRequestInFlight = false;
                broadcastSymbolsToPickers(SymbolSource::Lighter);
                setPickersRefreshState(SymbolSource::Lighter, false);

                if (safeDlg) {
                    applyVenueAppearance(safeDlg, SymbolSource::Lighter, safeDlg->selectedAccount());
                    safeDlg->setSymbols(m_lighterSymbols, m_lighterApiOff);
                    safeDlg->setRefreshInProgress(false);
                }
                statusBar()->showMessage(tr("Loaded %1 Lighter markets").arg(m_lighterSymbols.size()), 2500);
            };

            auto fetchSpot = [this, nam, proxySummary, safeDlg, state, finalize, finishError, salvageSymbols, lighterBaseUrl, useDedicatedNam]() {
                QNetworkRequest req2(
                    QUrl(lighterBaseUrl + QStringLiteral("/api/v1/orderBookDetails?filter=spot")));
                req2.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                                  QNetworkRequest::NoLessSafeRedirectPolicy);
                req2.setRawHeader("Accept", "application/json");
                req2.setRawHeader("User-Agent", "FusionTerminal/1.0");
                auto *spotReply = nam->get(req2);
                connect(spotReply,
                        &QNetworkReply::finished,
                        this,
                        [this, spotReply, nam, proxySummary, safeDlg, state, finalize, finishError, salvageSymbols, useDedicatedNam]() mutable {
                            const auto err2 = spotReply->error();
                            const int status2 =
                                spotReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                            const QString err2Str = spotReply->errorString();
                            const QByteArray raw2 = spotReply->readAll();
                            spotReply->deleteLater();

                            if (err2 != QNetworkReply::NoError || status2 >= 400) {
                                const QString body = raw2.isEmpty() ? err2Str : QString::fromUtf8(raw2);
                                const QString msg =
                                    QStringLiteral("[symbols] Lighter spot fetch failed (proxy=%1 status=%2): %3")
                                        .arg(proxySummary, QString::number(status2), body.left(220));
                                // Spot is optional; keep perp list if we have it.
                                if (!state->symbols.isEmpty()) {
                                    qWarning() << msg;
                                    finalize(state->symbols, state->apiOff, state->maxLevBySymbol);
                                    if (useDedicatedNam) {
                                        nam->deleteLater();
                                    }
                                    return;
                                }
                                finishError(msg);
                                if (useDedicatedNam) {
                                    nam->deleteLater();
                                }
                                return;
                            }

                            const QJsonDocument doc2 = QJsonDocument::fromJson(raw2);
                            if (!doc2.isObject()) {
                                salvageSymbols(raw2, *state);
                                if (!state->symbols.isEmpty()) {
                                    finalize(state->symbols, state->apiOff, state->maxLevBySymbol);
                                    if (useDedicatedNam) {
                                        nam->deleteLater();
                                    }
                                    return;
                                }
                                const QString msg =
                                    QStringLiteral("[symbols] Lighter spot parse failed (proxy=%1): invalid JSON")
                                        .arg(proxySummary);
                                finishError(msg);
                                if (useDedicatedNam) {
                                    nam->deleteLater();
                                }
                                return;
                            }

                            const QJsonObject root2 = doc2.object();
                            if (root2.value(QStringLiteral("code")).toInt(0) != 200) {
                                const QString msg =
                                    QStringLiteral("[symbols] Lighter spot API error (proxy=%1): %2")
                                        .arg(proxySummary,
                                             root2.value(QStringLiteral("message")).toString(QStringLiteral("unknown")));
                                // Spot is optional; keep perp list if we have it.
                                if (!state->symbols.isEmpty()) {
                                    qWarning() << msg;
                                    finalize(state->symbols, state->apiOff, state->maxLevBySymbol);
                                    if (useDedicatedNam) {
                                        nam->deleteLater();
                                    }
                                    return;
                                }
                                finishError(msg);
                                if (useDedicatedNam) {
                                    nam->deleteLater();
                                }
                                return;
                            }

                            const QJsonArray spot = root2.value(QStringLiteral("spot_order_book_details")).toArray();
                            for (const auto &v : spot) {
                                const QJsonObject obj = v.toObject();
                                QString sym = obj.value(QStringLiteral("symbol")).toString().trimmed().toUpper();
                                if (sym.isEmpty()) {
                                    continue;
                                }
                                if (!state->symbols.contains(sym, Qt::CaseInsensitive)) {
                                    state->symbols.push_back(sym);
                                }
                                const QString status =
                                    obj.value(QStringLiteral("status")).toString().trimmed().toLower();
                                if (!status.isEmpty() && status != QStringLiteral("active")) {
                                    state->apiOff.insert(sym);
                                }
                            }

                            finalize(state->symbols, state->apiOff, state->maxLevBySymbol);
                            if (useDedicatedNam) {
                                nam->deleteLater();
                            }
                        });
            };

            const bool hasSpotSymbols = std::any_of(state->symbols.cbegin(),
                                                   state->symbols.cend(),
                                                   [](const QString &s) { return s.contains(QLatin1Char('/')); });
            if (!hasSpotSymbols) {
                fetchSpot();
                return;
            }

            finalize(state->symbols, state->apiOff, state->maxLevBySymbol);
            cleanupNam();
        });
        return;
    }

    const bool isSwap = source == SymbolSource::UzxSwap;
    bool &inFlightRef = isSwap ? m_uzxSwapRequestInFlight : m_uzxSpotRequestInFlight;
    if (inFlightRef) {
        return;
    }
    if (isSwap && !m_uzxSwapSymbols.isEmpty()) {
        if (dlg) {
            applyVenueAppearance(dlg, source, dlg->selectedAccount());
            dlg->setSymbols(m_uzxSwapSymbols, m_uzxSwapApiOff);
        }
        return;
    }
    if (!isSwap && !m_uzxSpotSymbols.isEmpty()) {
        if (dlg) {
            applyVenueAppearance(dlg, source, dlg->selectedAccount());
            dlg->setSymbols(m_uzxSpotSymbols, m_uzxSpotApiOff);
        }
        return;
    }

    QUrl url;
    if (isSwap) {
        url = QUrl(QStringLiteral("https://api-v2.uzx.com/v2/products"));
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("ins_type"), QStringLiteral("SWAP"));
        url.setQuery(q);
    } else {
        url = QUrl(QStringLiteral("https://api-v2.uzx.com/notification/spot/tickers"));
    }
    inFlightRef = true;
    bool *flagPtr = &inFlightRef;
    auto *reply = m_symbolFetcher.get(QNetworkRequest(url));
    connect(reply, &QNetworkReply::finished, this, [this, reply, dlg, isSwap, flagPtr]() {
        if (flagPtr) {
            *flagPtr = false;
        }
        const auto err = reply->error();
        const QByteArray raw = reply->readAll();
        reply->deleteLater();
        if (err != QNetworkReply::NoError) {
            qWarning() << "[symbols] uzx fetch failed:" << err;
            return;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(raw);
        if (!doc.isObject()) {
            return;
        }
        const QJsonArray arr = doc.object().value(QStringLiteral("data")).toArray();
        QStringList list;
        list.reserve(arr.size());
        QHash<QString, int> maxLevBySymbol;
        QHash<QString, double> swapValueBySymbol;
        QHash<QString, double> priceUnitBySymbol;
        QHash<QString, int> numPrecisionBySymbol;
        for (const auto &v : arr) {
            const QJsonObject obj = v.toObject();
            QString sym = obj.value(isSwap ? QStringLiteral("product_name")
                                           : QStringLiteral("symbol"))
                               .toString()
                               .trimmed()
                               .toUpper();
            if (sym.isEmpty()) continue;
            sym = sym.replace(QStringLiteral("-"), QString());
            if (isSwap) {
                const int maxLev = obj.value(QStringLiteral("max_leverage")).toInt(0);
                if (maxLev > 0) {
                    maxLevBySymbol.insert(sym, maxLev);
                }
                auto toDouble = [](const QJsonValue &v) -> double {
                    if (v.isString()) return v.toString().toDouble();
                    if (v.isDouble()) return v.toDouble();
                    return v.toVariant().toDouble();
                };
                const double swapValue = toDouble(obj.value(QStringLiteral("swap_value")));
                if (swapValue > 0.0) {
                    swapValueBySymbol.insert(sym, swapValue);
                }
                const double priceUnit = toDouble(obj.value(QStringLiteral("price_unit")));
                if (priceUnit > 0.0) {
                    priceUnitBySymbol.insert(sym, priceUnit);
                }
                const int numPrecision = obj.value(QStringLiteral("num_precision")).toInt(-1);
                if (numPrecision >= 0) {
                    numPrecisionBySymbol.insert(sym, numPrecision);
                }
            }
            if (!list.contains(sym, Qt::CaseInsensitive)) {
                list.push_back(sym);
            }
        }
        std::sort(list.begin(), list.end(), [](const QString &a, const QString &b) {
            return a.toUpper() < b.toUpper();
        });
        if (isSwap) {
            m_uzxSwapSymbols = list;
            m_uzxSwapApiOff.clear();
            m_uzxSwapMaxLeverageBySymbol = maxLevBySymbol;
            m_uzxSwapContractValueBySymbol = swapValueBySymbol;
            m_uzxSwapPriceUnitBySymbol = priceUnitBySymbol;
            m_uzxSwapNumPrecisionBySymbol = numPrecisionBySymbol;
        } else {
            m_uzxSpotSymbols = list;
            m_uzxSpotApiOff.clear();
        }
        if (dlg) {
            const SymbolSource effectiveSource =
                isSwap ? SymbolSource::UzxSwap : SymbolSource::UzxSpot;
            applyVenueAppearance(dlg, effectiveSource, dlg->selectedAccount());
            dlg->setSymbols(list, QSet<QString>());
        }
    });
}

void MainWindow::mergeSymbolLibrary(const QStringList &symbols, const QSet<QString> &apiOff)
{
    QStringList merged;
    merged.reserve(m_symbolLibrary.size() + symbols.size());
    auto addList = [&merged](const QStringList &src) {
        for (const QString &sym : src) {
            const QString s = sym.trimmed().toUpper();
            if (s.isEmpty() || merged.contains(s, Qt::CaseInsensitive)) {
                continue;
            }
            merged.push_back(s);
        }
    };
    addList(m_symbolLibrary);
    addList(symbols);
    std::sort(merged.begin(), merged.end(), [](const QString &a, const QString &b) {
        return a.toUpper() < b.toUpper();
    });
    m_symbolLibrary = merged;
    m_apiOffSymbols.clear();
    for (const QString &s : apiOff) {
        m_apiOffSymbols.insert(s.trimmed().toUpper());
    }
}

SymbolPickerDialog *MainWindow::createSymbolPicker(const QString &title,
                                                   const QString &currentSymbol,
                                                   const QString &currentAccount)
{
    refreshAccountColors();
    auto *dlg = new SymbolPickerDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose, true);
    dlg->setModal(false);
    dlg->setWindowModality(Qt::NonModal);
    {
        QString t = title;
        const QString ver = QCoreApplication::applicationVersion().trimmed();
        if (!ver.isEmpty()) {
            t = QStringLiteral("%1 (%2)").arg(t, ver);
        }
        dlg->setWindowTitle(t);
    }
    // Если по какой-то причине api-off внезапно содержит почти все тикеры, не красим всё подряд.
    const bool apiOffLooksSuspicious =
        !m_apiOffSymbols.isEmpty() && m_apiOffSymbols.size() >= (m_symbolLibrary.size() * 8) / 10;
    const QSet<QString> apiOff = apiOffLooksSuspicious ? QSet<QString>() : m_apiOffSymbols;
    const QString initialAccount =
        currentAccount.isEmpty() ? QStringLiteral("MEXC Spot") : currentAccount;
    const SymbolSource src = symbolSourceForAccount(initialAccount);
    applyVenueAppearance(dlg, src, initialAccount);
    if (src == SymbolSource::Mexc) {
        dlg->setSymbols(m_symbolLibrary, apiOff);
    } else if (src == SymbolSource::MexcFutures && !m_mexcFuturesSymbols.isEmpty()) {
        dlg->setSymbols(m_mexcFuturesSymbols, m_mexcFuturesApiOff);
        dlg->setRefreshInProgress(m_mexcFuturesRequestInFlight);
    } else if (src == SymbolSource::BinanceSpot && !m_binanceSpotSymbols.isEmpty()) {
        dlg->setSymbols(m_binanceSpotSymbols, QSet<QString>());
        dlg->setRefreshInProgress(m_binanceSpotRequestInFlight);
    } else if (src == SymbolSource::BinanceFutures && !m_binanceFuturesSymbols.isEmpty()) {
        dlg->setSymbols(m_binanceFuturesSymbols, QSet<QString>());
        dlg->setRefreshInProgress(m_binanceFuturesRequestInFlight);
    } else if (src == SymbolSource::Lighter && !m_lighterSymbols.isEmpty()) {
        dlg->setSymbols(m_lighterSymbols, m_lighterApiOff);
        dlg->setRefreshInProgress(m_lighterRequestInFlight);
    } else if (src == SymbolSource::UzxSwap && !m_uzxSwapSymbols.isEmpty()) {
        dlg->setSymbols(m_uzxSwapSymbols, m_uzxSwapApiOff);
    } else if (src == SymbolSource::UzxSpot && !m_uzxSpotSymbols.isEmpty()) {
        dlg->setSymbols(m_uzxSpotSymbols, m_uzxSpotApiOff);
    } else {
        fetchSymbolLibrary(src, dlg);
    }

    QVector<QPair<QString, QColor>> accounts;
    QSet<QString> seen;
    for (auto it = m_accountColors.constBegin(); it != m_accountColors.constEnd(); ++it) {
        accounts.push_back({it.key(), it.value()});
        seen.insert(it.key().toLower());
    }
    auto ensureAccount = [&](const QString &name, const QColor &fallback) {
        if (seen.contains(name.toLower())) return;
        accounts.push_back({name, fallback});
        seen.insert(name.toLower());
    };
    ensureAccount(QStringLiteral("MEXC Spot"), QColor("#4c9fff"));
    ensureAccount(QStringLiteral("MEXC Futures"), QColor("#f5b642"));
    ensureAccount(QStringLiteral("UZX Spot"), QColor("#8bc34a"));
    ensureAccount(QStringLiteral("UZX Swap"), QColor("#ff7f50"));
    ensureAccount(QStringLiteral("Binance Spot"), QColor("#f0b90b"));
    ensureAccount(QStringLiteral("Binance Futures"), QColor("#f5b642"));
    ensureAccount(QStringLiteral("Lighter"), QColor("#38bdf8"));
    dlg->setAccounts(accounts);
    dlg->setCurrentSymbol(currentSymbol);
    dlg->setCurrentAccount(initialAccount);
    trackSymbolPicker(dlg);
    if ((src == SymbolSource::Mexc && m_symbolRequestInFlight)
        || (src == SymbolSource::MexcFutures && m_mexcFuturesRequestInFlight)
        || (src == SymbolSource::BinanceSpot && m_binanceSpotRequestInFlight)
        || (src == SymbolSource::BinanceFutures && m_binanceFuturesRequestInFlight)
        || (src == SymbolSource::UzxSpot && m_uzxSpotRequestInFlight)
        || (src == SymbolSource::UzxSwap && m_uzxSwapRequestInFlight)
        || (src == SymbolSource::Lighter && m_lighterRequestInFlight)) {
        dlg->setRefreshInProgress(true);
    }
    connect(dlg,
            &SymbolPickerDialog::refreshRequested,
            this,
            [this, dlg]() {
                const SymbolSource src = symbolSourceForAccount(dlg->selectedAccount());
                applyVenueAppearance(dlg, src, dlg->selectedAccount());
                fetchSymbolLibrary(src, dlg);
            });
    connect(dlg,
            &SymbolPickerDialog::accountChanged,
            this,
            [this, dlg](const QString &account) {
                const SymbolSource src = symbolSourceForAccount(account);
                applyVenueAppearance(dlg, src, account);
                if (src == SymbolSource::Mexc) {
                    dlg->setSymbols(m_symbolLibrary, m_apiOffSymbols);
                    dlg->setRefreshInProgress(m_symbolRequestInFlight);
                } else if (src == SymbolSource::MexcFutures && !m_mexcFuturesSymbols.isEmpty()) {
                    dlg->setSymbols(m_mexcFuturesSymbols, m_mexcFuturesApiOff);
                    dlg->setRefreshInProgress(m_mexcFuturesRequestInFlight);
                } else if (src == SymbolSource::BinanceSpot && !m_binanceSpotSymbols.isEmpty()) {
                    dlg->setSymbols(m_binanceSpotSymbols, QSet<QString>());
                    dlg->setRefreshInProgress(m_binanceSpotRequestInFlight);
                } else if (src == SymbolSource::BinanceFutures && !m_binanceFuturesSymbols.isEmpty()) {
                    dlg->setSymbols(m_binanceFuturesSymbols, QSet<QString>());
                    dlg->setRefreshInProgress(m_binanceFuturesRequestInFlight);
                } else if (src == SymbolSource::Lighter && !m_lighterSymbols.isEmpty()) {
                    dlg->setSymbols(m_lighterSymbols, m_lighterApiOff);
                    dlg->setRefreshInProgress(m_lighterRequestInFlight);
                } else if (src == SymbolSource::UzxSwap && !m_uzxSwapSymbols.isEmpty()) {
                    dlg->setSymbols(m_uzxSwapSymbols, m_uzxSwapApiOff);
                } else if (src == SymbolSource::UzxSpot && !m_uzxSpotSymbols.isEmpty()) {
                    dlg->setSymbols(m_uzxSpotSymbols, m_uzxSpotApiOff);
                } else {
                    fetchSymbolLibrary(src, dlg);
                }
            });
    return dlg;
}

void MainWindow::repositionNotionalOverlay(DomColumn &col)
{
    if (!col.notionalOverlay) {
        return;
    }
    enforceNotionalPrintsMinWidth(col);
    QWidget *vp = nullptr;
    if (col.scrollArea) {
        vp = col.scrollArea->viewport();
    }
    if (!vp) {
        vp = qobject_cast<QWidget *>(col.notionalOverlay->parentWidget());
    }
    if (!vp) {
        return;
    }

    col.notionalOverlay->adjustSize();

    const int pad = 0;
    int x = pad;
    int maxX = std::max(pad, vp->width() - col.notionalOverlay->width() - pad);

    // Right boundary: the prints/DOM splitter handle (do not overlap the DOM column).
    if (col.printsDomSplitter) {
        if (auto *h = col.printsDomSplitter->handle(1)) {
            const QPoint handlePos = h->mapTo(vp, QPoint(0, 0));
            const int domLeft = handlePos.x(); // first pixel of the handle
            maxX = std::max(pad, domLeft - col.notionalOverlay->width() - pad);
        }
    }

    // Anchor to the clusters/prints splitter handle so the overlay stays "glued" while dragging,
    // even if child widget geometries haven't been laid out yet for this event tick.
    if (col.clustersPrintsSplitter) {
        if (auto *h = col.clustersPrintsSplitter->handle(1)) {
            const QPoint handlePos = h->mapTo(vp, QPoint(0, 0));
            const int printsLeft = handlePos.x() + h->width(); // first pixel of prints side
            x = printsLeft + pad;
        }
    } else if (col.printsWrap) {
        const QPoint printsPos = col.printsWrap->mapTo(vp, QPoint(0, 0));
        x = printsPos.x() + pad;
    }

    x = std::clamp(x, pad, maxX);

    const int bottomMargin = 36;
    int y = vp->height() - col.notionalOverlay->height() - bottomMargin;
    if (y < 8) {
        y = 8;
    }
    const int maxY = std::max(0, vp->height() - col.notionalOverlay->height() - 6);
    if (y > maxY) {
        y = maxY;
    }

    col.notionalOverlay->move(x, y);
    col.notionalOverlay->raise();
    col.notionalOverlay->show();
}

void MainWindow::enforceNotionalPrintsMinWidth(DomColumn &col)
{
    if (!col.clustersPrintsSplitter || !col.printsWrap || !col.notionalOverlay) {
        return;
    }

    col.notionalOverlay->adjustSize();
    const int overlayW = std::max(0, col.notionalOverlay->sizeHint().width());
    // Prevent prints column from collapsing under the notional overlay.
    const int minPrintsW = std::max(60, overlayW + 6);

    col.printsWrap->setMinimumWidth(minPrintsW);
    if (col.prints) {
        col.prints->setMinimumWidth(minPrintsW);
    }
    col.clustersPrintsSplitter->setCollapsible(1, false);
}

void MainWindow::repositionClustersFooterOverlay(DomColumn &col)
{
    if (!col.clustersFooterOverlay) {
        return;
    }
    if (!col.scrollArea || !col.scrollArea->viewport()) {
        return;
    }
    QWidget *vp = col.scrollArea->viewport();

    const int footerH = col.clustersFooterOverlay->height() > 0 ? col.clustersFooterOverlay->height() : 34;
    int y = vp->height() - footerH;
    if (y < 0) {
        y = 0;
    }

    int x = 0;
    int w = col.clusters ? col.clusters->width() : 0;
    if (col.clusters) {
        const QPoint clustersPos = col.clusters->mapTo(vp, QPoint(0, 0));
        x = clustersPos.x();
        w = col.clusters->width();
    }

    const int maxW = std::max(0, vp->width() - x);
    w = std::clamp(w, 0, maxW);
    if (x < 0) {
        x = 0;
    }

    col.clustersFooterOverlay->setGeometry(x, y, w, footerH);
    col.clustersFooterOverlay->raise();
    col.clustersFooterOverlay->show();
}

void MainWindow::applySymbolToColumn(DomColumn &col,
                                     const QString &symbol,
                                     const QString &accountName)
{
    auto refreshNotionalOverlay = [this, &col]() { repositionNotionalOverlay(col); };

    const QString sym = symbol.trimmed().toUpper();
    if (sym.isEmpty()) {
        return;
    }
    QString account = accountName.trimmed();
    if (account.isEmpty()) {
        account = QStringLiteral("MEXC Spot");
    }

    if (!m_symbolLibrary.contains(sym, Qt::CaseInsensitive)) {
        m_symbolLibrary.push_back(sym);
    }

    col.symbol = sym;
    col.accountName = account;
    col.accountColor = accountColorFor(account);

    const auto src = symbolSourceForAccount(account);
    const bool lighterPerp = (src == SymbolSource::Lighter) && !col.symbol.contains(QLatin1Char('/'));
    if (src == SymbolSource::MexcFutures) {
        const int maxLev = std::max(1, m_mexcFuturesMaxLeverageBySymbol.value(sym, 200));
        col.leverage = std::clamp(m_futuresLeverageBySymbol.value(sym, 20), 1, maxLev);
    } else if (src == SymbolSource::UzxSwap) {
        if (!m_uzxSwapMaxLeverageBySymbol.contains(sym) && m_uzxSwapSymbols.isEmpty()) {
            fetchSymbolLibrary(SymbolSource::UzxSwap, nullptr);
        }
        const int maxLev = std::max(1, m_uzxSwapMaxLeverageBySymbol.value(sym, 125));
        col.leverage = std::clamp(m_futuresLeverageBySymbol.value(sym, 20), 1, maxLev);
    } else if (lighterPerp) {
        if (!m_lighterMaxLeverageBySymbol.contains(sym) && m_lighterSymbols.isEmpty()) {
            fetchSymbolLibrary(SymbolSource::Lighter, nullptr);
        }
        const int maxLev = std::max(1, m_lighterMaxLeverageBySymbol.value(sym, 10));
        col.leverage = std::clamp(m_futuresLeverageBySymbol.value(sym, 20), 1, maxLev);
    } else {
        col.leverage = 20;
    }
    if (col.leverageButton) {
        col.leverageButton->setVisible(src == SymbolSource::MexcFutures
                                       || src == SymbolSource::UzxSwap
                                       || lighterPerp);
        col.leverageButton->setText(QStringLiteral("%1x").arg(col.leverage));
    }
    refreshNotionalOverlay();

    {
        const QString notionalPresetKey =
            col.accountName.trimmed().toLower() + QLatin1Char('|') + col.symbol.trimmed().toUpper();
        const auto savedPresetsIt = m_notionalPresetsByKey.constFind(notionalPresetKey);
        if (savedPresetsIt != m_notionalPresetsByKey.constEnd()) {
            col.notionalValues = savedPresetsIt.value();
        } else {
            for (std::size_t i = 0; i < col.notionalValues.size(); ++i) {
                col.notionalValues[i] = m_defaultNotionalPresets[i];
            }
        }

        const int defaultIndex = std::min(3, static_cast<int>(col.notionalValues.size()) - 1);
        int pickIndex = defaultIndex;
        for (int i = 0; i < static_cast<int>(col.notionalValues.size()); ++i) {
            if (std::abs(col.notionalValues[static_cast<std::size_t>(i)] - 10.0) < 1e-9) {
                pickIndex = i;
                break;
            }
        }
        col.orderNotional = col.notionalValues[static_cast<std::size_t>(pickIndex)];
        if (col.notionalGroup) {
            for (int i = 0; i < static_cast<int>(col.notionalValues.size()); ++i) {
                if (auto *btn = qobject_cast<QToolButton *>(col.notionalGroup->button(i))) {
                    btn->setText(QString::number(col.notionalValues[static_cast<std::size_t>(i)], 'g', 6));
                }
            }
            if (auto *btn = col.notionalGroup->button(pickIndex)) {
                btn->setChecked(true);
            }
        }
    }
    refreshNotionalOverlay();

    col.hasBuffer = false;
    col.bufferMinTick = 0;
    col.bufferMaxTick = 0;
    col.visibleCenterTick = 0;
    col.userScrolling = false;
    col.pendingViewportUpdate = false;
    col.pendingViewportBottom = 0;
    col.pendingViewportTop = 0;
    col.pendingViewportRevision = 0;
    col.lastViewportBottom = 0;
    col.lastViewportTop = 0;
    col.lastViewportRevision = 0;
    col.bufferRevision = 0;
    col.pendingAutoCenter = false;
    col.pendingAutoCenterTickValid = false;
    col.pendingAutoCenterTick = 0;
    if (col.tickerLabel) {
        col.tickerLabel->setProperty("accountColor", col.accountColor);
        col.tickerLabel->setText(sym);
        applyTickerLabelStyle(col.tickerLabel, col.accountColor, false);
    }
    applyHeaderAccent(col);

    clearSltpMarkers(col);

    if (!col.localOrders.isEmpty()) {
        col.localOrders.clear();
        if (col.dom) {
            col.dom->setLocalOrders(col.localOrders);
        }
        if (col.prints) {
            QVector<LocalOrderMarker> empty;
            col.prints->setLocalOrders(empty);
        }
    }

    if (col.client) {
        restartColumnClient(col);
    }
    if (col.chatWindow) {
        col.chatWindow->setAuthContext(m_authBaseUrl, m_authToken, m_authUser);
        col.chatWindow->setChatKey(chatKeyForColumn(col), chatTitleForColumn(col));
    }
    syncWatchedSymbols();
}

void MainWindow::refreshAccountColors()
{
    m_accountColors.clear();
    m_accountSources.clear();
    auto normalizeKey = [](const QString &s) {
        return s.trimmed().toLower();
    };
    auto makeVenueLabel = [&](const QString &userLabelRaw, const QString &fallbackName) {
        const QString userLabel = userLabelRaw.trimmed();
        if (userLabel.isEmpty()) {
            return fallbackName;
        }
        // If the stored label is just a default placeholder from another venue,
        // ignore it (e.g. "MEXC Spot" accidentally saved under Binance).
        if (isPlaceholderAccountLabel(userLabel)
            && userLabel.compare(fallbackName, Qt::CaseInsensitive) != 0) {
            return fallbackName;
        }
        // Never let a different venue name become the primary label.
        // Keep venue as the base, and put user label as a suffix.
        if (userLabel.compare(fallbackName, Qt::CaseInsensitive) == 0) {
            return fallbackName;
        }
        if (userLabel.contains(fallbackName, Qt::CaseInsensitive)) {
            return userLabel;
        }
        return QStringLiteral("%1 (%2)").arg(fallbackName, userLabel);
    };
    auto makeUniqueName = [&](const QString &userLabel, SymbolSource source, const QString &fallbackName) {
        QString candidate = makeVenueLabel(userLabel, fallbackName);
        QString key = normalizeKey(candidate);
        const auto existing = m_accountSources.constFind(key);
        if (existing == m_accountSources.constEnd() || existing.value() == source) {
            return candidate;
        }
        int suffix = 2;
        while (m_accountSources.contains(key)) {
            candidate = QStringLiteral("%1 %2").arg(makeVenueLabel(userLabel, fallbackName)).arg(suffix++);
            key = normalizeKey(candidate);
        }
        return candidate;
    };
    auto insertProfile = [&](ConnectionStore::Profile profile,
                             SymbolSource source,
                             const QString &fallbackName,
                             const QString &fallbackColor) {
        MexcCredentials creds =
            m_connectionStore ? m_connectionStore->loadMexcCredentials(profile) : MexcCredentials{};
        const QString name = makeUniqueName(creds.label, source, fallbackName);
        QColor color = QColor(!creds.colorHex.isEmpty() ? creds.colorHex : fallbackColor);
        if (!color.isValid()) {
            color = QColor(fallbackColor);
        }
        m_accountColors.insert(name, color);
        m_accountSources.insert(normalizeKey(name), source);
    };
    insertProfile(ConnectionStore::Profile::MexcSpot,
                  SymbolSource::Mexc,
                  QStringLiteral("MEXC Spot"),
                  QStringLiteral("#4c9fff"));
    insertProfile(ConnectionStore::Profile::MexcFutures,
                  SymbolSource::MexcFutures,
                  QStringLiteral("MEXC Futures"),
                  QStringLiteral("#f5b642"));
    insertProfile(ConnectionStore::Profile::BinanceSpot,
                  SymbolSource::BinanceSpot,
                  QStringLiteral("Binance Spot"),
                  QStringLiteral("#f0b90b"));
    insertProfile(ConnectionStore::Profile::BinanceFutures,
                  SymbolSource::BinanceFutures,
                  QStringLiteral("Binance Futures"),
                  QStringLiteral("#f5b642"));
    insertProfile(ConnectionStore::Profile::UzxSwap,
                  SymbolSource::UzxSwap,
                  QStringLiteral("UZX Swap"),
                  QStringLiteral("#ff7f50"));
    insertProfile(ConnectionStore::Profile::UzxSpot,
                  SymbolSource::UzxSpot,
                  QStringLiteral("UZX Spot"),
                  QStringLiteral("#8bc34a"));
    insertProfile(ConnectionStore::Profile::Lighter,
                  SymbolSource::Lighter,
                  QStringLiteral("Lighter"),
                  QStringLiteral("#38bdf8"));
    applyAccountColorsToColumns();
}

QColor MainWindow::accountColorFor(const QString &accountName) const
{
    const QString nameLower = accountName.trimmed().toLower();
    for (auto it = m_accountColors.constBegin(); it != m_accountColors.constEnd(); ++it) {
        if (it.key().trimmed().toLower() == nameLower) {
            return it.value();
        }
    }

    if (m_connectionStore) {
        const SymbolSource source = symbolSourceForAccount(accountName);
        ConnectionStore::Profile profile = ConnectionStore::Profile::MexcSpot;
        switch (source) {
        case SymbolSource::Mexc:
            profile = ConnectionStore::Profile::MexcSpot;
            break;
        case SymbolSource::MexcFutures:
            profile = ConnectionStore::Profile::MexcFutures;
            break;
        case SymbolSource::BinanceSpot:
            profile = ConnectionStore::Profile::BinanceSpot;
            break;
        case SymbolSource::BinanceFutures:
            profile = ConnectionStore::Profile::BinanceFutures;
            break;
        case SymbolSource::UzxSpot:
            profile = ConnectionStore::Profile::UzxSpot;
            break;
        case SymbolSource::UzxSwap:
            profile = ConnectionStore::Profile::UzxSwap;
            break;
        case SymbolSource::Lighter:
            profile = ConnectionStore::Profile::Lighter;
            break;
        }
        const MexcCredentials creds = m_connectionStore->loadMexcCredentials(profile);
        QColor color(creds.colorHex);
        if (color.isValid()) {
            return color;
        }
    }

    if (nameLower.contains(QStringLiteral("binance"))) {
        if (nameLower.contains(QStringLiteral("future"))) {
            return QColor("#f5b642");
        }
        return QColor("#f0b90b");
    }
    if (nameLower.contains(QStringLiteral("lighter"))) {
        return QColor("#38bdf8");
    }
    if (nameLower.contains(QStringLiteral("uzx"))) {
        if (nameLower.contains(QStringLiteral("spot"))) {
            return QColor("#8bc34a");
        }
        return QColor("#ff7f50");
    }
    if (nameLower.contains(QStringLiteral("future"))) {
        return QColor("#f5b642");
    }
    return QColor("#4c9fff");
}

void MainWindow::applyAccountColorsToColumns()
{
    for (auto &tab : m_tabs) {
        for (auto &col : tab.columnsData) {
            col.accountColor = accountColorFor(col.accountName);
            if (col.tickerLabel) {
                col.tickerLabel->setProperty("accountColor", col.accountColor);
                applyTickerLabelStyle(col.tickerLabel, col.accountColor, false);
            }
            applyHeaderAccent(col);
        }
    }
}

void MainWindow::applyTickerLabelStyle(QLabel *label, const QColor &accent, bool hovered)
{
    if (!label) {
        return;
    }
    const QColor bg = accent.isValid() ? accent : QColor("#3a7bd5");
    QColor textColor = contrastTextColor(bg);
    if (hovered) {
        textColor = (textColor.lightness() < 128) ? textColor.darker(130) : textColor.lighter(118);
    }
    label->setStyleSheet(QStringLiteral(
                             "QLabel { color:%1; font-weight:800; padding:0px; }")
                             .arg(textColor.name(QColor::HexRgb)));
}

void MainWindow::applyHeaderAccent(DomColumn &col)
{
    if (!col.header) {
        return;
    }
    const QColor base = col.accountColor.isValid() ? col.accountColor : QColor("#3a7bd5");
    const QColor border = base.darker(135);
    const QColor fg = contrastTextColor(base);
    const QColor badgeBg = badgeBackgroundFor(base);
    const QColor badgeBorder = fg.lightness() < 128 ? QColor(0, 0, 0, 80) : QColor(255, 255, 255, 80);
    const QString style = QStringLiteral(
        "QFrame#DomTitleBar {"
        "  background:%1;"
        "  border:1px solid %2;"
        "  border-radius:0px;"
        "}"
        "QToolButton#DomHeaderCloseButton {"
        "  background: transparent;"
        "  color: %3;"
        "  border: none;"
        "}"
        "QToolButton#DomHeaderCloseButton:hover {"
        "  background: rgba(255,255,255,0.10);"
        "}")
                            .arg(base.name(QColor::HexRgb))
                            .arg(border.name(QColor::HexRgb))
                            .arg(fg.name(QColor::HexRgb));
    col.header->setStyleSheet(style);

    // Update venue icon + market type label based on account.
    const SymbolSource source = symbolSourceForAccount(col.accountName);
    QString iconPath;
    switch (source) {
    case SymbolSource::Mexc:
    case SymbolSource::MexcFutures:
        iconPath = resolveAssetPath(QStringLiteral("icons/logos/mexc.png"));
        break;
    case SymbolSource::BinanceSpot:
    case SymbolSource::BinanceFutures:
        iconPath = resolveAssetPath(QStringLiteral("icons/logos/binance.png"));
        break;
    case SymbolSource::UzxSpot:
    case SymbolSource::UzxSwap:
        iconPath = resolveAssetPath(QStringLiteral("icons/logos/UZX.png"));
        break;
    case SymbolSource::Lighter:
        iconPath = resolveAssetPath(QStringLiteral("icons/logos/lighter.png"));
        break;
    }
    if (col.venueIconLabel) {
        QPixmap pix(iconPath);
        if (!pix.isNull()) {
            const qreal dpr = col.venueIconLabel->devicePixelRatioF();
            const QSize targetPx(std::max(1, qRound(static_cast<qreal>(col.venueIconLabel->width()) * dpr)),
                                std::max(1, qRound(static_cast<qreal>(col.venueIconLabel->height()) * dpr)));
            QPixmap scaled = pix.scaled(targetPx, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            scaled.setDevicePixelRatio(dpr);
            col.venueIconLabel->setPixmap(scaled);
            col.venueIconLabel->setText(QString());
        } else {
            col.venueIconLabel->setPixmap(QPixmap());
            col.venueIconLabel->setText(source == SymbolSource::Mexc || source == SymbolSource::MexcFutures
                                            ? QStringLiteral("M")
                                        : (source == SymbolSource::BinanceSpot
                                               || source == SymbolSource::BinanceFutures)
                                            ? QStringLiteral("B")
                                        : (source == SymbolSource::Lighter) ? QStringLiteral("L")
                                                                            : QStringLiteral("U"));
            col.venueIconLabel->setAlignment(Qt::AlignCenter);
            col.venueIconLabel->setStyleSheet(
                QStringLiteral("QLabel { color:%1; font-weight:900; }").arg(fg.name(QColor::HexRgb)));
        }
    }
    if (col.marketTypeLabel) {
        const bool lighterPerp = (source == SymbolSource::Lighter) && !col.symbol.contains(QLatin1Char('/'));
        const bool futures = (source == SymbolSource::MexcFutures) || (source == SymbolSource::UzxSwap)
                             || (source == SymbolSource::BinanceFutures) || lighterPerp;
        col.marketTypeLabel->setText(futures ? QStringLiteral("F") : QStringLiteral("S"));
    }
    if (col.readOnlyLabel) {
        const bool isBinance = (source == SymbolSource::BinanceSpot || source == SymbolSource::BinanceFutures);
        col.readOnlyLabel->setVisible(isBinance);
        if (isBinance) {
            col.readOnlyLabel->setToolTip(tr("Binance работает только в режиме read-only (торговля отключена)."));
        }

    }
    if (col.chatButton) {
        QIcon icon = loadIconTinted(QStringLiteral("message-chatbot"), fg, QSize(12, 12));
        if (icon.isNull()) {
            QPixmap pix(12, 12);
            pix.fill(Qt::transparent);
            QPainter p(&pix);
            p.setRenderHint(QPainter::Antialiasing, false);
            QPen pen(fg);
            pen.setCosmetic(true);
            p.setPen(pen);
            p.drawRect(QRect(1, 1, 10, 10));
            icon = QIcon(pix);
        }
        col.chatButton->setIcon(icon);
        col.chatButton->setToolTip(tr("Chat"));
    }
    if (col.closeButton) {
        col.closeButton->setIcon(loadIconTinted(QStringLiteral("x"), fg, QSize(12, 12)));
        col.closeButton->setToolTip(tr("Close"));
    }

    if (auto *pill = dynamic_cast<PillLabel *>(col.marketTypeLabel)) {
        pill->setPillColors(badgeBg, badgeBorder, fg);
    }
    if (auto *pill = dynamic_cast<PillLabel *>(col.readOnlyLabel)) {
        const bool isBinance = (source == SymbolSource::BinanceSpot || source == SymbolSource::BinanceFutures);
        const QColor warnBg = isBinance ? QColor("#d9534f") : badgeBg;
        const QColor warnBorder = isBinance ? QColor("#b84542") : badgeBorder;
        pill->setPillColors(warnBg, warnBorder, QColor("#ffffff"));
    }
    if (auto *pill = dynamic_cast<PillToolButton *>(col.compressionButton)) {
        pill->setPillColors(badgeBg, badgeBorder, fg);
    }
}

#include "MainWindow.moc"
