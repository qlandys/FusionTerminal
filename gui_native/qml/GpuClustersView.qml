import QtQuick 2.15
import Fusion 1.0

Item {
    id: root
    property var clustersModel: null
    property int rowCount: 0
    property int rowHeight: 20
    property int infoAreaHeight: 26
    property int columnCount: 5
    property int columnWidth: 28
    property var columnLabels: []
    property var columnTotals: []
    property color backgroundColor: "#151515"
    property color gridColor: "#303030"
    property string fontFamily: "JetBrains Mono"
    property int fontPixelSize: 12
    property string label: ""
    property bool useGpuClusters: false

    Rectangle {
        anchors.fill: parent
        color: backgroundColor
    }

    Item {
        anchors.left: parent.left
        anchors.right: parent.right
        height: rowCount * rowHeight
        Canvas {
            id: hGridCanvasFallback
            anchors.fill: parent
            antialiasing: false
            renderTarget: Canvas.FramebufferObject
            visible: !root.useGpuClusters
            onPaint: {
                var ctx = getContext("2d");
                ctx.reset();
                ctx.clearRect(0, 0, width, height);
                ctx.strokeStyle = root.gridColor;
                ctx.globalAlpha = 0.35;
                ctx.lineWidth = 1;
                var step = Math.max(1, root.rowHeight);
                var maxY = Math.max(0, root.rowCount * step);
                for (var y = 0; y <= maxY; y += step) {
                    ctx.beginPath();
                    ctx.moveTo(0, y + 0.5);
                    ctx.lineTo(width, y + 0.5);
                    ctx.stroke();
                }
            }
            onWidthChanged: requestPaint();
            onHeightChanged: requestPaint();
            Connections {
                target: root
                function onRowCountChanged() { hGridCanvasFallback.requestPaint(); }
                function onRowHeightChanged() { hGridCanvasFallback.requestPaint(); }
                function onGridColorChanged() { hGridCanvasFallback.requestPaint(); }
            }
            Component.onCompleted: requestPaint();
        }
        GpuGridItem {
            anchors.fill: parent
            vertical: false
            count: Math.max(0, root.rowCount)
            step: Math.max(1, root.rowHeight)
            color: root.gridColor
            opacity: 0.35
            visible: root.useGpuClusters
        }
    }

    Item {
        anchors.left: parent.left
        anchors.right: parent.right
        height: rowCount * rowHeight + infoAreaHeight
        Canvas {
            id: vGridCanvasFallback
            anchors.fill: parent
            antialiasing: false
            renderTarget: Canvas.FramebufferObject
            visible: !root.useGpuClusters
            onPaint: {
                var ctx = getContext("2d");
                ctx.reset();
                ctx.clearRect(0, 0, width, height);
                ctx.strokeStyle = root.gridColor;
                ctx.globalAlpha = 0.6;
                ctx.lineWidth = 1;
                var step = Math.max(1, root.columnWidth);
                var count = Math.max(0, root.columnCount);
                for (var i = 0; i <= count; ++i) {
                    var x = i * step;
                    ctx.beginPath();
                    ctx.moveTo(x + 0.5, 0);
                    ctx.lineTo(x + 0.5, height);
                    ctx.stroke();
                }
            }
            onWidthChanged: requestPaint();
            onHeightChanged: requestPaint();
            Connections {
                target: root
                function onColumnCountChanged() { vGridCanvasFallback.requestPaint(); }
                function onColumnWidthChanged() { vGridCanvasFallback.requestPaint(); }
                function onGridColorChanged() { vGridCanvasFallback.requestPaint(); }
                function onInfoAreaHeightChanged() { vGridCanvasFallback.requestPaint(); }
            }
            Component.onCompleted: requestPaint();
        }
        GpuGridItem {
            anchors.fill: parent
            vertical: true
            count: Math.max(0, root.columnCount)
            step: Math.max(1, root.columnWidth)
            color: root.gridColor
            opacity: 0.6
            visible: root.useGpuClusters
        }
    }

    Item {
        anchors.fill: parent
        Repeater {
            model: root.clustersModel ? root.clustersModel : 0
            delegate: Item {
                property int row: typeof model.row !== "undefined" ? model.row : -1
                property int col: typeof model.col !== "undefined" ? model.col : 0
                property string textValue: typeof model.text !== "undefined" ? model.text : ""
                property color textColor: typeof model.textColor !== "undefined" ? model.textColor : "#cfd8dc"
                property color bgColor: typeof model.bgColor !== "undefined" ? model.bgColor : "transparent"
                visible: row >= 0 && row < root.rowCount && textValue.length > 0
                x: col * root.columnWidth
                y: row * root.rowHeight
                width: root.columnWidth
                height: root.rowHeight
                Rectangle {
                    anchors.fill: parent
                    color: bgColor
                }
                Text {
                    anchors.centerIn: parent
                    text: textValue
                    color: textColor
                    font.family: fontFamily
                    font.pixelSize: Math.max(9, fontPixelSize - 2)
                    font.bold: true
                }
            }
        }
    }

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: infoAreaHeight
        color: backgroundColor
    }

    Item {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: infoAreaHeight
        Repeater {
            model: root.columnCount
            delegate: Item {
                x: index * root.columnWidth
                width: root.columnWidth
                height: root.infoAreaHeight
                Column {
                    anchors.fill: parent
                    anchors.margins: 1
                    spacing: 0
                    Text {
                        width: parent.width
                        height: parent.height / 2
                        verticalAlignment: Text.AlignVCenter
                        horizontalAlignment: Text.AlignHCenter
                        text: (root.columnTotals && index < root.columnTotals.length) ? root.columnTotals[index] : ""
                        color: "#cfd8dc"
                        font.family: fontFamily
                        font.pixelSize: Math.max(8, fontPixelSize - 3)
                        font.bold: true
                        opacity: 0.95
                    }
                    Text {
                        width: parent.width
                        height: parent.height / 2
                        verticalAlignment: Text.AlignVCenter
                        horizontalAlignment: Text.AlignHCenter
                        text: (root.columnLabels && index < root.columnLabels.length) ? root.columnLabels[index] : ""
                        color: "#9aa7ad"
                        font.family: fontFamily
                        font.pixelSize: Math.max(8, fontPixelSize - 3)
                        opacity: 0.9
                    }
                }
            }
        }
    }

    Text {
        anchors.left: parent.left
        anchors.leftMargin: 4
        anchors.top: parent.top
        anchors.topMargin: 2
        text: label
        color: "#b0bec5"
        font.family: fontFamily
        font.pixelSize: Math.max(8, fontPixelSize - 3)
        visible: label.length > 0
        opacity: 0.85
    }

    Rectangle {
        anchors.right: parent.right
        width: 1
        height: parent.height
        color: "#303030"
        opacity: 0.9
    }
}
