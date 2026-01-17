import QtQuick 2.15
import Fusion 1.0

Item {
    id: root
    property var circlesModel: null
    property var ordersModel: null
    property var printsBridge: null
    property bool useGpuPrints: false
    property int rowCount: 0
    property int rowHeight: 20
    property color backgroundColor: "#151515"
    property bool printsSquare: false
    property color gridColor: "#303030"
    property color connectionColor: "#444444"
    // Thin trail between consecutive prints (helps see sequence).
    property color trailColor: "#ffffff"
    property real trailOpacity: 0.35
    property real trailWidth: 1.25
    property color hoverColor: "#2860ff"
    property color hoverTextColor: "#f4f6f8"
    // Hover "ray" for DOM hover info text (percent | notional).
    // Use the same blue as DOM hover highlight so it reads as one element.
    property color hoverBeamColor: "#2860ff"
    property color hoverBeamBorderColor: "#2b3a55"
    // Beam needs to stay readable but not hide limit marker volumes beneath it.
    property real hoverBeamOpacity: 0.22
    property string fontFamily: Qt.application.font.family
    property int fontPixelSize: 12
    property int hoverRow: -1
    property string hoverText: ""
    property int sidePadding: 6
    property int infoAreaHeight: 26

    // Drag-to-move marker (single order id only).
    property bool dragActive: false
    // `order` = regular limit order marker, `sltp` = SL/TP marker.
    property string dragMode: ""
    property string dragOrderId: ""
    property string dragSltpKind: "" // "SL" or "TP"
    property bool dragBuy: true
    property string dragText: ""
    property color dragFillColor: "#4caf50"
    property color dragBorderColor: "#2f6c37"
    property int dragRow: -1
    property int dragStartRow: -1
    property int dragPreviewRow: -1
    property bool dragMoved: false
    property real dragOriginPrice: 0.0

    function updateDragFromY(yAbs) {
        if (!root.dragActive || !root.printsBridge) {
            return;
        }
        // Clamp to ladder area so a cursor in the footer/info area doesn't pin the drag to a single row.
        var maxY = Math.max(0, root.height - root.infoAreaHeight - 1);
        var yClamped = Math.max(0, Math.min(yAbs, maxY));
        var r = root.printsBridge.snapRowFromY(yClamped);
        root.dragRow = r;
        if (r !== root.dragPreviewRow) {
            root.dragPreviewRow = r;
            if (r >= 0) {
                var p = root.printsBridge.priceForRow(r);
                root.printsBridge.requestDragPreview(p);
            }
        }
    }

    function finishDrag() {
        if (!root.dragActive) {
            return;
        }
        // Only commit when the user actually moved to a different tick row.
        // A simple click should behave like "cancel", not cancel+re-place at the same price.
        var moved = (root.dragStartRow >= 0 && root.dragRow !== root.dragStartRow);
        if (root.printsBridge && moved && root.dragRow >= 0) {
            var newPrice = root.printsBridge.priceForRow(root.dragRow);
            if (root.dragMode === "order" && root.dragOrderId.length > 0) {
                root.printsBridge.requestCommitMove(root.dragOrderId, newPrice, root.dragOriginPrice);
            } else if (root.dragMode === "sltp" && root.dragSltpKind.length > 0) {
                root.printsBridge.requestCommitMoveSltp(root.dragSltpKind, newPrice, root.dragOriginPrice);
            }
        }
        root.dragActive = false;
        root.dragMode = "";
        root.dragOrderId = "";
        root.dragSltpKind = "";
        root.dragRow = -1;
        root.dragStartRow = -1;
        root.dragPreviewRow = -1;
        root.dragMoved = false;
        root.hoverRow = -1;
        if (root.printsBridge) root.printsBridge.requestClearDragPreview();
    }

    function cancelDrag() {
        root.dragActive = false;
        root.dragMode = "";
        root.dragOrderId = "";
        root.dragSltpKind = "";
        root.dragRow = -1;
        root.dragStartRow = -1;
        root.dragPreviewRow = -1;
        root.dragMoved = false;
        root.hoverRow = -1;
        if (root.printsBridge) root.printsBridge.requestClearDragPreview();
    }

    // Global overlay: owns the mouse grab from the start of the interaction so the drag keeps working
    // even if the marker delegate disappears (it can be removed from the model right after we send cancel).
    MouseArea {
        id: dragOverlay
        anchors.fill: parent
        z: 100
        acceptedButtons: Qt.LeftButton
        preventStealing: true
        hoverEnabled: true
        cursorShape: Qt.ArrowCursor
        property double _lastDragLogMs: 0

        function hitTestMarkerAt(x, y) {
            if (!root.ordersModel) return null;
            if (!ordersLayer || !ordersLayer.children) return null;
            for (var i = ordersLayer.children.length - 1; i >= 0; --i) {
                var c = ordersLayer.children[i];
                if (!c || c.objectName !== "orderMarker" || !c.visible) continue;
                if (x < c.x || x > (c.x + c.width) || y < c.y || y > (c.y + c.height)) continue;
                return c;
            }
            return null;
        }

        function tryBeginDragAt(x, y) {
            if (root.dragActive || !root.printsBridge) return false;
            var marker = hitTestMarkerAt(x, y);

            if (!marker) return false;

            var canMoveOrder = marker.orderIds && marker.orderIds.length === 1;
            var labelUpper = (marker.textValue || "").toString().trim().toUpperCase();
            var canMoveSltp = (labelUpper === "SL" || labelUpper === "TP");
            if (!canMoveOrder && !canMoveSltp) return false;

            root.dragActive = true;
            if (canMoveOrder) {
                root.dragMode = "order";
                root.dragOrderId = marker.orderIds[0];
                root.dragSltpKind = "";
            } else {
                root.dragMode = "sltp";
                root.dragOrderId = "";
                root.dragSltpKind = labelUpper;
            }
            root.dragBuy = marker.buy;
            root.dragText = marker.textValue;
            root.dragFillColor = marker.fillColor;
            root.dragBorderColor = marker.borderColor;
            root.dragOriginPrice = marker.price;
            root.hoverRow = -1;
            root.updateDragFromY(y);
            root.dragStartRow = root.dragRow;
            root.dragMoved = false;
            root.dragPreviewRow = root.dragRow;
            if (root.dragRow >= 0) {
                root.printsBridge.requestDragPreview(root.printsBridge.priceForRow(root.dragRow));
            }
            if (root.dragMode === "order") {
                root.printsBridge.requestBeginMove(root.dragOrderId);
                console.log("[drag] begin id=" + root.dragOrderId + " y=" + y + " row=" + root.dragRow);
            } else {
                root.printsBridge.requestBeginMoveSltp(root.dragSltpKind, root.dragOriginPrice);
                console.log("[drag] begin sltp=" + root.dragSltpKind + " y=" + y + " row=" + root.dragRow);
            }
            return true;
        }

        onPressed: function(mouse) {
            if (!tryBeginDragAt(mouse.x, mouse.y)) {
                mouse.accepted = false;
                return;
            }
        }
        onPositionChanged: function(mouse) {
            if (root.dragActive) root.updateDragFromY(mouse.y);
        }
        onReleased: function(mouse) {
            if (!root.dragActive) {
                mouse.accepted = false;
                return;
            }
            root.finishDrag();
        }
        onCanceled: {
            root.cancelDrag();
        }
    }

    function clamp01(v) {
        return Math.max(0, Math.min(1, v));
    }

    function usableWidth() {
        return Math.max(1, width - sidePadding * 2);
    }

    function xForRatio(ratio) {
        return sidePadding + clamp01(ratio !== undefined ? ratio : 0.5) * usableWidth();
    }

    function yForValue(v) {
        return v !== undefined ? v : (rowHeight / 2);
    }

    Rectangle {
        anchors.fill: parent
        color: backgroundColor
        z: 0
    }

    Rectangle {
        // Row hover highlight.
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.topMargin: hoverRow >= 0 ? hoverRow * rowHeight : -rowHeight
        height: rowHeight
        color: hoverColor
        opacity: hoverRow >= 0 ? 0.25 : 0.0
        visible: hoverRow >= 0 && !root.dragActive && hoverText.length === 0
        z: 5
    }

    Rectangle {
        // Compact "ray" background, clipped on the left by matching the hover label width.
        // This keeps underlying prints/limit markers readable.
        x: hoverBeam.visible ? hoverBeam.x : 0
        y: hoverRow >= 0 ? hoverRow * rowHeight : -rowHeight
        width: hoverBeam.visible ? hoverBeam.width : 0
        height: rowHeight
        color: root.hoverBeamColor
        opacity: hoverBeam.visible ? root.hoverBeamOpacity : 0.0
        visible: hoverBeam.visible
        z: 6.2
    }

    Item {
        id: gridLayer
        anchors.left: parent.left
        anchors.right: parent.right
        height: rowCount * rowHeight
        z: 1
        Canvas {
            id: gridCanvasFallback
            anchors.fill: parent
            antialiasing: false
            renderTarget: Canvas.Image
            visible: !root.useGpuPrints
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
            onWidthChanged: requestPaint()
            onHeightChanged: requestPaint()
            Connections {
                target: root
                function onRowCountChanged() { gridCanvasFallback.requestPaint(); }
                function onRowHeightChanged() { gridCanvasFallback.requestPaint(); }
                function onGridColorChanged() { gridCanvasFallback.requestPaint(); }
            }
            Component.onCompleted: requestPaint()
        }
        GpuGridItem {
            anchors.fill: parent
            vertical: false
            count: Math.max(0, root.rowCount)
            step: Math.max(1, root.rowHeight)
            color: root.gridColor
            opacity: 0.35
            visible: root.useGpuPrints
        }
    }

    // Trail under circles: draw as a simple polyline so it always connects centers correctly.
    GpuTrailItem {
        id: trailGpu
        anchors.fill: parent
        z: 2.6
        circlesModel: root.circlesModel
        sidePadding: root.sidePadding
        maxSegments: 256
        color: root.trailColor
        opacity: root.trailOpacity
        lineWidth: root.trailWidth
        visible: root.useGpuPrints
    }

    Canvas {
        id: trailCanvasFallback
        anchors.fill: parent
        z: 2.6
        antialiasing: true
        renderTarget: Canvas.Image
        visible: !root.useGpuPrints
        onPaint: {
            var ctx = getContext("2d");
            ctx.reset();
            ctx.clearRect(0, 0, width, height);
            if (!root.circlesModel) return;

            ctx.strokeStyle = root.trailColor;
            ctx.globalAlpha = root.trailOpacity;
            ctx.lineWidth = root.trailWidth;
            ctx.lineCap = "round";
            ctx.lineJoin = "round";

            for (var i = 0; i < 256; ++i) {
                var a = root.circlesModel.get(i);
                if (!a || a.xRatio === undefined || a.y === undefined) break;
                var b = root.circlesModel.get(i + 1);
                if (!b || b.xRatio === undefined || b.y === undefined) break;

                var x1 = xForRatio(a.xRatio);
                var y1 = yForValue(a.y);
                var x2 = xForRatio(b.xRatio);
                var y2 = yForValue(b.y);

                var dx = x2 - x1;
                var dy = y2 - y1;
                if ((dx * dx + dy * dy) < 0.25) continue;

                ctx.beginPath();
                ctx.moveTo(x1, y1);
                ctx.lineTo(x2, y2);
                ctx.stroke();
            }
        }
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
        Connections {
            target: root.circlesModel
            function onModelReset() { trailCanvasFallback.requestPaint(); }
            function onDataChanged() { trailCanvasFallback.requestPaint(); }
            function onRowsInserted() { trailCanvasFallback.requestPaint(); }
            function onRowsRemoved() { trailCanvasFallback.requestPaint(); }
            function onLayoutChanged() { trailCanvasFallback.requestPaint(); }
        }
        Connections {
            target: root
            function onTrailColorChanged() { trailCanvasFallback.requestPaint(); }
            function onTrailOpacityChanged() { trailCanvasFallback.requestPaint(); }
            function onTrailWidthChanged() { trailCanvasFallback.requestPaint(); }
        }
        Component.onCompleted: requestPaint()
    }

    Item {
        anchors.fill: parent
        z: 3

        // GPU-rendered circles (geometry only).
        GpuCirclesItem {
            anchors.fill: parent
            circlesModel: root.circlesModel
            sidePadding: root.sidePadding
            printsSquare: root.printsSquare
            maxCircles: 96
            circleSegments: 72
            visible: root.useGpuPrints
        }

        // Text overlay remains in QML for now (small count).
        Repeater {
            model: root.circlesModel ? root.circlesModel : 0
            visible: true
            delegate: Item {
                property real circleRadius: typeof model.radius !== "undefined" ? model.radius : 0
                width: circleRadius * 2
                height: width
                x: xForRatio(model.xRatio) - width / 2
                y: yForValue(model.y) - height / 2
                visible: root.useGpuPrints && circleRadius > 0.5
                Text {
                    anchors.centerIn: parent
                    text: typeof model.text !== "undefined" ? model.text : ""
                    color: "#f4f7fb"
                    font.family: fontFamily
                    font.bold: true
                    font.pixelSize: Math.max(6, Math.min(parent.width * 0.55, fontPixelSize))
                }
            }
        }

        // Full fallback (old path) when GPU prints is disabled.
        Repeater {
            model: root.circlesModel ? root.circlesModel : 0
            visible: !root.useGpuPrints
            delegate: Item {
                property real circleRadius: typeof model.radius !== "undefined" ? model.radius : 0
                width: circleRadius * 2
                height: width
                x: xForRatio(model.xRatio) - width / 2
                y: yForValue(model.y) - height / 2
                Rectangle {
                    anchors.fill: parent
                    radius: root.printsSquare ? Math.min(width * 0.25, 6) : width / 2
                    color: typeof model.fillColor !== "undefined" ? model.fillColor : "#4caf50"
                    border.color: typeof model.borderColor !== "undefined" ? model.borderColor : "#2f6c37"
                    border.width: 2
                    antialiasing: true
                }
                Text {
                    anchors.centerIn: parent
                    text: typeof model.text !== "undefined" ? model.text : ""
                    color: "#f4f7fb"
                    font.family: fontFamily
                    font.bold: true
                    font.pixelSize: Math.max(6, Math.min(parent.width * 0.55, fontPixelSize))
                }
            }
        }
    }

    Item {
        id: ordersLayer
        anchors.fill: parent
        z: 6
        Repeater {
            model: root.ordersModel ? root.ordersModel : 0
            delegate: Item {
                objectName: "orderMarker"
                id: markerRoot
                property string textValue: typeof model.text !== "undefined" ? model.text : ""
                property bool buy: typeof model.buy !== "undefined" ? model.buy : true
                property color fillColor: typeof model.fillColor !== "undefined" ? model.fillColor : (buy ? "#4caf50" : "#ef5350")
                property color borderColor: typeof model.borderColor !== "undefined" ? model.borderColor : (buy ? "#2f6c37" : "#992626")
                property int row: typeof model.row !== "undefined" ? model.row : -1
                property real price: typeof model.price !== "undefined" ? model.price : 0.0
                property var orderIds: typeof model.orderIds !== "undefined" ? model.orderIds : []
                property bool baseVisible: row >= 0 && row < root.rowCount && textValue.length > 0
                visible: baseVisible
                         && !(root.dragActive
                              && ((root.dragMode === "order"
                                   && root.dragOrderId.length > 0
                                   && orderIds
                                   && orderIds.length === 1
                                   && root.dragOrderId === orderIds[0])
                                  || (root.dragMode === "sltp"
                                      && root.dragSltpKind.length > 0
                                      && (textValue || "").toString().trim().toUpperCase() === root.dragSltpKind)))
                height: Math.min(Math.max(root.rowHeight - 2, 14), 26)
                property real tipSize: Math.min(height / 2, 12)
                width: Math.max(textItem.contentWidth + 22 + tipSize, 70)
                x: root.width - width - 4
                y: row * root.rowHeight + root.rowHeight / 2 - height / 2
                property real animPhase: 0.0
                property bool hovered: false
                property real hoverT: hovered ? 1.0 : 0.0
                Behavior on hoverT { NumberAnimation { duration: 120; easing.type: Easing.OutCubic } }
                clip: true

                GpuMarkerItem {
                    anchors.fill: parent
                    fillColor: markerRoot.fillColor
                    borderColor: markerRoot.borderColor
                    phase: markerRoot.animPhase
                }

                Text {
                    id: textItem
                    anchors.fill: parent
                    anchors.leftMargin: 8
                    anchors.rightMargin: tipSize + 6
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    text: textValue
                    color: "#f7f9fb"
                    font.family: fontFamily
                    font.weight: Font.DemiBold
                    font.bold: false
                    font.kerning: false
                    font.preferShaping: true
                    font.features: { "tnum": 1, "lnum": 1 }
                    font.pixelSize: Math.max(10, Math.min(fontPixelSize, height - 4))
                }

                NumberAnimation on animPhase {
                    loops: Animation.Infinite
                    running: visible
                    from: 0.0
                    to: 1.0
                    duration: 1100
                    easing.type: Easing.InOutSine
                }

                // Drag is handled by `dragOverlay` so it keeps working even if this delegate is destroyed.

                Item {
                    id: cancelBtn
                    objectName: "orderCancel"
                    z: 3
                    width: 14
                    height: 14
                    x: 0
                    y: parent.height / 2 - height / 2
                    opacity: 0.78 + 0.22 * hoverT
                    visible: true

                    Rectangle {
                        id: cancelShadow
                        anchors.fill: parent
                        x: 1
                        y: 1
                        radius: 2
                        color: "#000000"
                        opacity: 0.22
                    }

                    Rectangle {
                        anchors.fill: parent
                        radius: 2
                        color: Qt.darker(fillColor, 1.55)
                        opacity: 1.0
                        border.width: 1
                        border.color: Qt.darker(borderColor, 1.25)
                    }

                    Image {
                        anchors.centerIn: parent
                        width: 12
                        height: 12
                        source: "qrc:/icons/outline/x-white.svg"
                        fillMode: Image.PreserveAspectFit
                        smooth: true
                        opacity: 0.98
                    }

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.ArrowCursor
                        onClicked: {
                            if (root.printsBridge) {
                                root.printsBridge.requestCancel(orderIds, textValue, price, buy);
                            }
                        }
                    }
                }

                MouseArea {
                    anchors.fill: parent
                    hoverEnabled: true
                    acceptedButtons: Qt.NoButton
                    cursorShape: Qt.ArrowCursor
                    onEntered: hovered = true
                    onExited: hovered = false
                }
            }
        }
    }

    // Drag ghost overlay: keeps the marker visible while the original order is canceled.
    Item {
        id: dragGhost
        visible: root.dragActive
                 && ((root.dragMode === "order" && root.dragOrderId.length > 0)
                     || (root.dragMode === "sltp" && root.dragSltpKind.length > 0))
                 && root.dragRow >= 0
                 && root.dragRow < root.rowCount
        z: 6.8
        height: Math.min(Math.max(root.rowHeight - 2, 14), 26)
        property real tipSize: Math.min(height / 2, 12)
        width: Math.max(70, ghostText.contentWidth + 22 + tipSize)
        x: root.width - width - 4
        y: root.dragRow * root.rowHeight + root.rowHeight / 2 - height / 2
        clip: true

        GpuMarkerItem {
            anchors.fill: parent
            fillColor: root.dragFillColor
            borderColor: root.dragBorderColor
            phase: 0.0
        }

        Text {
            id: ghostText
            anchors.fill: parent
            anchors.leftMargin: 8
            anchors.rightMargin: dragGhost.tipSize + 6
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            text: root.dragText
            color: "#f7f9fb"
            font.family: root.fontFamily
            font.weight: Font.DemiBold
            font.bold: false
            font.kerning: false
            font.preferShaping: true
            font.features: { "tnum": 1, "lnum": 1 }
            font.pixelSize: Math.max(10, Math.min(root.fontPixelSize, dragGhost.height - 4))
        }
    }

    Item {
        id: hoverBeam
        z: 7
        visible: hoverRow >= 0 && hoverText.length > 0 && !root.dragActive
        height: rowHeight
        y: hoverRow >= 0 ? hoverRow * rowHeight : 0
        property int padX: 10
        property int padY: 2
        width: Math.min(parent.width, Math.max(1, hoverBeamText.contentWidth + padX * 2))
        x: parent.width - width

        Text {
            id: hoverBeamText
            anchors.fill: parent
            anchors.leftMargin: hoverBeam.padX
            anchors.rightMargin: hoverBeam.padX
            anchors.topMargin: hoverBeam.padY
            anchors.bottomMargin: hoverBeam.padY
            verticalAlignment: Text.AlignVCenter
            horizontalAlignment: Text.AlignHCenter
            text: hoverText
            color: hoverTextColor
            style: Text.Outline
            styleColor: "#000000"
            font.family: fontFamily
            font.weight: Font.DemiBold
            font.bold: false
            font.kerning: false
            font.preferShaping: true
            font.features: { "tnum": 1, "lnum": 1 }
            font.pixelSize: Math.max(8, fontPixelSize - 1)
            elide: Text.ElideNone
            clip: true
        }
    }

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: infoAreaHeight
        color: backgroundColor
        z: 10
    }
}
