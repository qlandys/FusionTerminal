import QtQuick 2.15

Item {
    id: root
    anchors.fill: parent

    Rectangle {
        anchors.fill: parent
        color: (typeof theme !== "undefined" && theme) ? theme.panelBackground : "#1e1e1e"
    }

    Column {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 14

        Text {
            text: "Appearance"
            color: theme ? theme.textPrimary : "#e4e4e4"
            font.pixelSize: 18
            font.bold: true
        }

        Text {
            text: "Тема и цвета. Сохраняется автоматически."
            color: theme ? theme.textSecondary : "#9aa7ad"
            font.pixelSize: 12
            wrapMode: Text.WordWrap
        }

        Column {
            spacing: 8

            Text {
                text: "Theme"
                color: theme ? theme.textSecondary : "#9aa7ad"
                font.pixelSize: 12
                font.bold: true
            }

            Row {
                spacing: 8

                Repeater {
                    model: ["System", "Dark", "Light", "OLED"]
                    delegate: Rectangle {
                        property string modeName: modelData
                        radius: 10
                        height: 30
                        width: Math.max(76, label.implicitWidth + 22)
                        color: (theme && theme.mode === modeName) ? (theme.selectionColor) : "transparent"
                        border.width: 1
                        border.color: theme ? theme.borderColor : "#2b2b2b"

                        Text {
                            id: label
                            anchors.centerIn: parent
                            text: modeName
                            color: theme ? theme.textPrimary : "#e4e4e4"
                            font.pixelSize: 12
                            font.bold: theme && theme.mode === modeName
                        }

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: if (theme) theme.mode = modeName
                        }
                    }
                }
            }
        }

        Column {
            spacing: 8

            Text {
                text: "Prints shape"
                color: theme ? theme.textSecondary : "#9aa7ad"
                font.pixelSize: 12
                font.bold: true
            }

            Row {
                spacing: 8

                Repeater {
                    model: [
                        { label: "Round", square: false },
                        { label: "Rounded square", square: true }
                    ]
                    delegate: Rectangle {
                        property bool square: modelData.square
                        radius: 10
                        height: 30
                        width: Math.max(110, label.implicitWidth + 22)
                        color: (theme && theme.printsSquare === square) ? theme.selectionColor : "transparent"
                        border.width: 1
                        border.color: theme ? theme.borderColor : "#2b2b2b"

                        Text {
                            id: label
                            anchors.centerIn: parent
                            text: modelData.label
                            color: theme ? theme.textPrimary : "#e4e4e4"
                            font.pixelSize: 12
                            font.bold: theme && theme.printsSquare === square
                        }

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: if (theme) theme.printsSquare = square
                        }
                    }
                }
            }
        }

        Column {
            spacing: 10

            Text {
                text: "Accent"
                color: theme ? theme.textSecondary : "#9aa7ad"
                font.pixelSize: 12
                font.bold: true
            }

            Row {
                spacing: 10
                Repeater {
                    model: ["#007acc", "#00b0ff", "#7c4dff", "#00c853", "#ffab00", "#ff5252"]
                    delegate: Rectangle {
                        width: 26
                        height: 26
                        radius: 8
                        color: modelData
                        border.width: theme && (theme.accentColor == modelData) ? 2 : 1
                        border.color: theme ? theme.borderColor : "#2b2b2b"

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: if (theme) theme.accentColor = modelData
                        }
                    }
                }
            }
        }

        Row {
            spacing: 28

            Column {
                spacing: 10
                Text {
                    text: "Bid"
                    color: theme ? theme.textSecondary : "#9aa7ad"
                    font.pixelSize: 12
                    font.bold: true
                }
                Row {
                    spacing: 10
                    Repeater {
                        model: ["#00c853", "#4caf50", "#2ecc71", "#00e676"]
                        delegate: Rectangle {
                            width: 26
                            height: 26
                            radius: 8
                            color: modelData
                            border.width: theme && (theme.bidColor == modelData) ? 2 : 1
                            border.color: theme ? theme.borderColor : "#2b2b2b"
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: if (theme) theme.bidColor = modelData
                            }
                        }
                    }
                }
            }

            Column {
                spacing: 10
                Text {
                    text: "Ask"
                    color: theme ? theme.textSecondary : "#9aa7ad"
                    font.pixelSize: 12
                    font.bold: true
                }
                Row {
                    spacing: 10
                    Repeater {
                        model: ["#e53935", "#ff5252", "#ef5350", "#ff6b81"]
                        delegate: Rectangle {
                            width: 26
                            height: 26
                            radius: 8
                            color: modelData
                            border.width: theme && (theme.askColor == modelData) ? 2 : 1
                            border.color: theme ? theme.borderColor : "#2b2b2b"
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: if (theme) theme.askColor = modelData
                            }
                        }
                    }
                }
            }
        }

        Rectangle {
            width: parent.width
            height: 140
            radius: 12
            color: theme ? theme.windowBackground : "#141414"
            border.width: 1
            border.color: theme ? theme.borderColor : "#2b2b2b"

            Rectangle {
                anchors.fill: parent
                anchors.margins: 12
                radius: 10
                color: theme ? theme.panelBackground : "#1e1e1e"
                border.width: 1
                border.color: theme ? theme.borderColor : "#2b2b2b"

                Column {
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 8

                    Text {
                        text: "Preview"
                        color: theme ? theme.textPrimary : "#e4e4e4"
                        font.pixelSize: 13
                        font.bold: true
                    }

                    Row {
                        spacing: 10

                        Rectangle {
                            width: 70
                            height: 28
                            radius: 8
                            color: theme ? theme.selectionColor : "#2860ff40"
                            border.width: 1
                            border.color: theme ? theme.borderColor : "#2b2b2b"
                            Text {
                                anchors.centerIn: parent
                                text: "Accent"
                                color: theme ? theme.textPrimary : "#e4e4e4"
                                font.pixelSize: 12
                            }
                        }

                        Rectangle {
                            width: 70
                            height: 28
                            radius: 8
                            color: theme ? theme.bidColor : "#4caf50"
                            border.width: 1
                            border.color: theme ? theme.borderColor : "#2b2b2b"
                            Text {
                                anchors.centerIn: parent
                                text: "BID"
                                color: "#0b0b0b"
                                font.pixelSize: 12
                                font.bold: true
                            }
                        }

                        Rectangle {
                            width: 70
                            height: 28
                            radius: 8
                            color: theme ? theme.askColor : "#e53935"
                            border.width: 1
                            border.color: theme ? theme.borderColor : "#2b2b2b"
                            Text {
                                anchors.centerIn: parent
                                text: "ASK"
                                color: "#0b0b0b"
                                font.pixelSize: 12
                                font.bold: true
                            }
                        }
                    }

                    Rectangle {
                        width: parent.width
                        height: 1
                        color: theme ? theme.gridColor : "#303030"
                        opacity: 0.6
                    }

                    Text {
                        text: "Text: 123.45  |  Secondary text"
                        color: theme ? theme.textPrimary : "#e4e4e4"
                        font.pixelSize: 12
                    }

                    Text {
                        text: "Grid/border sample"
                        color: theme ? theme.textSecondary : "#9aa7ad"
                        font.pixelSize: 11
                    }
                }
            }
        }

        Item { height: 1; width: 1 }
    }
}
