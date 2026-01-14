import QtQuick 2.15
import Fusion 1.0

Item {
    id: root
    property var levelsModel: null
    property bool useGpuDom: false
    property int rowHeight: 14
    property color backgroundColor: "#121212"
    property color gridColor: "#1f1f1f"
    property color textColor: "#e4e4e4"
    property color bidColor: "#3fbf7f"
    property color askColor: "#ff6b81"
    property int priceColumnWidth: 80
    property color priceBorderColor: "#2b2b2b"
    property var domBridge
    property string priceFontFamily: "JetBrains Mono"
    property int priceFontPixelSize: 12
    property int hoverRow: -1
    property real tickSize: 0.0
    property bool positionActive: false
    property real positionEntryPrice: 0.0
    property real positionMarkPrice: 0.0
    property bool actionOverlayVisible: false
    property string actionOverlayText: ""
    // Shared phase. Keep it synced with the marker "shine" animation in `GpuPrintsView.qml`.
    property real orderHighlightPhase: 0.0
    // Set by C++ (`DomWidget`) so we don't animate every DOM 60fps when there's nothing to highlight.
    property bool hasOrderHighlights: false

    NumberAnimation on orderHighlightPhase {
        loops: Animation.Infinite
        running: root.useGpuDom && root.hasOrderHighlights
        from: 0.0
        to: 1.0
        duration: 1100
        easing.type: Easing.InOutSine
    }

    property int infoAreaHeight: 0
    property bool positionVisible: false
    property string positionAvgText: ""
    property string positionValueText: ""
    property string positionPctText: ""
    property string positionPnlText: ""
    property color positionPnlColor: "#e4e4e4"
    property bool positionIsLong: true
    property bool positionPnlPositive: true

    Rectangle {
        anchors.fill: parent
        color: backgroundColor
    }

    Item {
        id: gpuLayer
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: actionOverlay.visible
                        ? actionOverlay.top
                        : (positionBar.visible ? positionBar.top : parent.bottom)
        clip: true
        visible: root.useGpuDom

        GpuDomItem {
            id: gpuDom
            objectName: "gpuDom"
            anchors.fill: parent
            rowHeight: root.rowHeight
            priceColumnWidth: root.priceColumnWidth
            backgroundColor: root.backgroundColor
            gridColor: root.gridColor
            hoverColor: "#2860ff"
            priceBorderColor: root.priceBorderColor
            hoverRow: root.hoverRow
            orderHighlightPhase: root.orderHighlightPhase
            positionActive: root.positionActive
            positionEntryPrice: root.positionEntryPrice
            positionMarkPrice: root.positionMarkPrice
            tickSize: root.tickSize
            positionPnlColor: root.positionPnlColor
        }

        // Text overlay (virtualized): keep QML node count low in GPU mode.
        // Repeater would instantiate thousands of delegates and cause periodic freezes when volatility spikes.
        ListView {
            id: gpuTextView
            anchors.fill: parent
            clip: true
            interactive: false
            spacing: 0
            model: root.levelsModel
            reuseItems: true
            cacheBuffer: root.rowHeight * 40
            visible: true

            delegate: Item {
                width: ListView.view ? ListView.view.width : root.width
                height: root.rowHeight
                property double levelPrice: typeof price !== "undefined" ? price : 0
                property string levelPriceText: typeof priceText !== "undefined" ? priceText : ""
                property string levelVolumeText: typeof volumeText !== "undefined" ? volumeText : ""
                property color levelVolumeTextColor: typeof volumeTextColor !== "undefined" ? volumeTextColor : root.textColor

                property real positionTol: Math.max(1e-8, root.tickSize > 0 ? root.tickSize * 0.25 : 1e-8)
                property real positionRangeMin: Math.min(root.positionEntryPrice, root.positionMarkPrice)
                property real positionRangeMax: Math.max(root.positionEntryPrice, root.positionMarkPrice)
                property bool inPositionRange: root.positionActive
                                    && root.positionEntryPrice > 0 && root.positionMarkPrice > 0
                                    && (levelPrice + positionTol >= positionRangeMin)
                                    && (levelPrice - positionTol <= positionRangeMax)
                property bool isMarkRow: root.positionActive
                               && root.positionMarkPrice > 0
                               && Math.abs(levelPrice - root.positionMarkPrice) <= positionTol

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: parent.left
                    anchors.leftMargin: 4
                    width: Math.max(0, parent.width - root.priceColumnWidth - 8)
                    text: levelVolumeText
                    color: levelVolumeTextColor
                    visible: levelVolumeText.length > 0
                    font.pixelSize: Math.max(10, root.rowHeight - 4)
                    font.bold: true
                    elide: Text.ElideRight
                }

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.right: parent.right
                    anchors.rightMargin: 6
                    width: root.priceColumnWidth - 12
                    horizontalAlignment: Text.AlignRight
                    text: levelPriceText
                    color: inPositionRange ? root.positionPnlColor : root.textColor
                    font.pixelSize: Math.max(10, root.priceFontPixelSize)
                    font.family: root.priceFontFamily
                    font.bold: isMarkRow
                    elide: Text.ElideLeft
                }
            }
        }
    }

    ListView {
        id: levelsView
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: actionOverlay.visible
                        ? actionOverlay.top
                        : (positionBar.visible ? positionBar.top : parent.bottom)
        spacing: 0
        clip: true
        model: root.levelsModel
        interactive: false
        visible: !root.useGpuDom

        delegate: Item {
            id: rowItem
            width: ListView.view ? ListView.view.width : root.width
            height: root.rowHeight
            property double levelPrice: typeof price !== "undefined" ? price : 0
            property double levelBid: typeof bidQty !== "undefined" ? bidQty : 0
            property double levelAsk: typeof askQty !== "undefined" ? askQty : 0
            property string levelPriceText: typeof priceText !== "undefined" ? priceText : ""
            property color levelBookColor: typeof bookColor !== "undefined" ? bookColor : "transparent"
            property color levelPriceBgColor: typeof priceBgColor !== "undefined" ? priceBgColor : "transparent"
            property string levelVolumeText: typeof volumeText !== "undefined" ? volumeText : ""
            property color levelVolumeTextColor: typeof volumeTextColor !== "undefined" ? volumeTextColor : root.textColor
            property color levelVolumeFillColor: typeof volumeFillColor !== "undefined" ? volumeFillColor : "#00000000"
            property real levelVolumeFillRatio: typeof volumeFillRatio !== "undefined" ? volumeFillRatio : 0.0
            property string levelMarkerText: typeof markerText !== "undefined" ? markerText : ""
            property color levelMarkerFillColor: typeof markerFillColor !== "undefined" ? markerFillColor : "transparent"
            property color levelMarkerBorderColor: typeof markerBorderColor !== "undefined" ? markerBorderColor : "transparent"
            property bool levelMarkerBuy: typeof markerBuy !== "undefined" ? markerBuy : true
            // NOTE: model role name is `orderHighlight`; avoid id name collisions in this delegate.
            property bool levelOrderHighlight: typeof orderHighlight !== "undefined" ? orderHighlight : false

            property bool hasBid: levelBid > 0.0
            property bool hasAsk: levelAsk > 0.0
            property bool isAskDominant: levelAsk >= levelBid
            property real bookWidth: Math.max(0, width - root.priceColumnWidth)
            property real positionTol: Math.max(1e-8, root.tickSize > 0 ? root.tickSize * 0.25 : 1e-8)
            property real positionRangeMin: Math.min(root.positionEntryPrice, root.positionMarkPrice)
            property real positionRangeMax: Math.max(root.positionEntryPrice, root.positionMarkPrice)
            property bool inPositionRange: root.positionActive
                                        && root.positionEntryPrice > 0 && root.positionMarkPrice > 0
                                        && (levelPrice + positionTol >= positionRangeMin)
                                        && (levelPrice - positionTol <= positionRangeMax)
            property bool isMarkRow: root.positionActive
                                   && root.positionMarkPrice > 0
                                   && Math.abs(levelPrice - root.positionMarkPrice) <= positionTol

            Rectangle {
                id: bookArea
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                width: bookWidth
                color: levelBookColor
            }

            Rectangle {
                anchors.left: bookArea.left
                anchors.top: bookArea.top
                anchors.bottom: bookArea.bottom
                width: Math.max(0, bookArea.width * levelVolumeFillRatio)
                color: levelVolumeFillColor
                visible: levelVolumeFillRatio > 0
            }
            Rectangle {
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                anchors.left: bookArea.right
                width: 1
                color: root.priceBorderColor
                opacity: 0.7
            }

            Text {
                anchors.verticalCenter: bookArea.verticalCenter
                anchors.left: bookArea.left
                anchors.leftMargin: 4
                text: levelVolumeText
                color: levelVolumeTextColor
                visible: levelVolumeText.length > 0
                font.pixelSize: Math.max(10, root.rowHeight - 4)
                font.bold: true
            }

            // Tick highlight: makes our orders visible even when prints overlap the marker.
            // Simple row glow pulse (no outlines/lines).
            Item {
                id: orderFx
                anchors.fill: parent
                visible: rowItem.levelOrderHighlight
                z: 9

                property real phase: root.orderHighlightPhase
                // Smooth pulse (0..1), aligned to the same phase timing as marker shine.
                property real pulse: 0.5 - 0.5 * Math.cos(phase * 6.283185307179586)

                // Uniform full-row glow (no bright bands/strips).
                Rectangle {
                    anchors.fill: parent
                    color: "#ffffff"
                    opacity: 0.020 + orderFx.pulse * 0.090
                }
            }

            Rectangle {
                id: priceArea
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                anchors.right: parent.right
                width: root.priceColumnWidth
                color: levelPriceBgColor
            }

            Rectangle {
                anchors.fill: priceArea
                color: root.positionPnlColor
                visible: rowItem.inPositionRange
                opacity: 0.12
            }
            Rectangle {
                anchors.left: priceArea.left
                anchors.top: priceArea.top
                anchors.bottom: priceArea.bottom
                width: 2
                color: root.positionPnlColor
                visible: rowItem.isMarkRow
                opacity: 0.9
            }
            Rectangle {
                anchors.fill: priceArea
                color: "transparent"
                border.width: rowItem.isMarkRow ? 1 : 0
                border.color: root.positionPnlColor
            }
            Rectangle {
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                anchors.right: priceArea.right
                width: 1
                color: root.priceBorderColor
            }

            Text {
                anchors.verticalCenter: parent.verticalCenter
                anchors.right: priceArea.right
                anchors.rightMargin: 6
                width: priceArea.width - 12
                horizontalAlignment: Text.AlignRight
                text: levelPriceText
                color: rowItem.inPositionRange ? root.positionPnlColor : root.textColor
                font.pixelSize: Math.max(10, root.priceFontPixelSize)
                font.family: root.priceFontFamily
                font.bold: rowItem.isMarkRow
            }

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: 1
                color: root.gridColor
                opacity: 0.4
            }
            Rectangle {
                anchors.fill: parent
                color: "#2860ff"
                opacity: root.hoverRow === index ? 0.25 : 0.0
            }
        }
    }

    Rectangle {
        id: actionOverlay
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: positionBar.visible ? positionBar.top : parent.bottom
        height: visible ? Math.max(22, root.rowHeight + 8) : 0
        visible: root.actionOverlayVisible && root.actionOverlayText.length > 0
        color: "#000000"
        opacity: 0.65

        Text {
            anchors.centerIn: parent
            text: root.actionOverlayText
            color: "#ffffff"
            font.bold: true
            font.pixelSize: Math.max(12, root.priceFontPixelSize)
        }
    }

    Rectangle {
        id: positionBar
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: root.infoAreaHeight
        visible: root.positionVisible && root.infoAreaHeight > 0
        color: "transparent"

        Row {
            id: posRow
            anchors.fill: parent
            anchors.margins: 6
            spacing: 6

            function cellWidth() {
                return Math.max(1, (width - spacing * 2) / 3);
            }

            Rectangle {
                width: posRow.cellWidth()
                height: posRow.height
                radius: 8
                color: "#0b0b0b"
                opacity: 0.92
                border.width: 1
                border.color: "#1a1a1a"

                Text {
                    anchors.centerIn: parent
                    text: root.positionAvgText
                    color: "#ffffff"
                    font.bold: true
                    font.family: root.priceFontFamily
                    font.pixelSize: Math.max(12, root.priceFontPixelSize + 3)
                }
            }

            Rectangle {
                width: posRow.cellWidth()
                height: posRow.height
                radius: 8
                color: root.positionIsLong ? "#1e5b38" : "#6a1b1b"
                opacity: 0.88
                border.width: 1
                border.color: root.positionIsLong ? "#2f6c37" : "#992626"

                Text {
                    anchors.centerIn: parent
                    text: root.positionValueText
                    color: "#ffffff"
                    font.bold: true
                    font.family: root.priceFontFamily
                    font.pixelSize: Math.max(12, root.priceFontPixelSize + 3)
                }
            }

            Rectangle {
                width: posRow.cellWidth()
                height: posRow.height
                radius: 8
                color: root.positionPnlPositive ? "#1e5b38" : "#6a1b1b"
                opacity: 0.88
                border.width: 1
                border.color: root.positionPnlPositive ? "#2f6c37" : "#992626"

                Text {
                    anchors.centerIn: parent
                    text: root.positionPnlText
                    color: "#ffffff"
                    font.bold: true
                    font.family: root.priceFontFamily
                    font.pixelSize: Math.max(12, root.priceFontPixelSize + 3)
                }
            }
        }

        Rectangle {
            id: exitButton
            anchors.right: parent.right
            anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            width: 56
            height: Math.max(14, parent.height - 6)
            radius: 4
            color: "#b71c1c"
            visible: false

            Text {
                anchors.centerIn: parent
                text: "EXIT"
                color: "#ffffff"
                font.bold: true
                font.pixelSize: Math.max(11, root.priceFontPixelSize)
            }

            MouseArea {
                anchors.fill: parent
                enabled: root.positionVisible
                onClicked: {
                    if (root.domBridge) {
                        root.domBridge.handleExitClick();
                    }
                }
            }
        }
    }
}
