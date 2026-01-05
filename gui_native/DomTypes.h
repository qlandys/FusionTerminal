#pragma once

#include <QColor>
#include <QMetaType>

struct VolumeHighlightRule {
    double threshold = 0.0; // notional in USDT
    QColor color = QColor();
};

Q_DECLARE_METATYPE(VolumeHighlightRule)
Q_DECLARE_METATYPE(QVector<VolumeHighlightRule>)
