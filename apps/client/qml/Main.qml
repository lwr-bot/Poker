import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Poker.Client

ApplicationWindow {
    id: window
    width: 1280
    height: 800
    minimumWidth: 1040
    minimumHeight: 680
    visible: true
    title: "PokerTable"
    color: "#07110d"

    readonly property color ink: "#eef8f2"
    readonly property color muted: "#8ca69a"
    readonly property color panel: "#101f19"
    readonly property color panelRaised: "#172b23"
    readonly property color accent: "#e6b85c"
    readonly property color felt: "#0d5b3c"

    NetworkClient { id: client }

    component PrimaryButton: Button {
        id: control
        implicitHeight: 44
        font.pixelSize: 14
        font.weight: Font.DemiBold
        contentItem: Text {
            text: control.text
            color: control.enabled ? "#172016" : "#718078"
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            font: control.font
        }
        background: Rectangle {
            radius: 10
            color: control.enabled
                   ? (control.down ? "#c99b45" : control.hovered ? "#f1c96f" : window.accent)
                   : "#39473f"
        }
    }

    component DarkField: TextField {
        implicitHeight: 44
        color: window.ink
        placeholderTextColor: "#60786c"
        selectionColor: window.accent
        selectedTextColor: "#172016"
        background: Rectangle {
            radius: 9
            color: "#0a1712"
            border.color: parent.activeFocus ? window.accent : "#284238"
            border.width: 1
        }
        leftPadding: 13
        rightPadding: 13
    }

    header: Rectangle {
        height: 64
        color: "#0b1813"
        border.color: "#193127"

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 28
            anchors.rightMargin: 28
            spacing: 16

            Rectangle {
                width: 34; height: 34; radius: 10; color: window.accent
                Text { anchors.centerIn: parent; text: "♠"; color: "#152018"; font.pixelSize: 22 }
            }
            Text {
                text: "POKER / TABLE"
                color: window.ink
                font.pixelSize: 17
                font.weight: Font.Bold
                font.letterSpacing: 2
            }
            Item { Layout.fillWidth: true }
            Rectangle {
                width: 8; height: 8; radius: 4
                color: client.connected ? "#5fe091" : "#ef6c6c"
            }
            Text { text: client.status; color: window.muted; font.pixelSize: 13 }
            Rectangle { width: 1; height: 24; color: "#294238"; visible: client.authenticated }
            Text {
                visible: client.authenticated
                text: "CHIPS  " + Number(client.walletChips).toLocaleString(Qt.locale(), "f", 0)
                color: window.accent
                font.pixelSize: 13
                font.weight: Font.DemiBold
            }
            Button {
                visible: client.authenticated
                text: "Sign out"
                onClicked: client.logout()
            }
        }
    }

    StackLayout {
        anchors.fill: parent
        currentIndex: !client.authenticated ? 0 : client.tableId === 0 ? 1 : 2

        Item {
            Rectangle {
                anchors.centerIn: parent
                width: 440
                height: 520
                radius: 24
                color: window.panel
                border.color: "#203a30"

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 42
                    spacing: 14

                    Text { text: "Welcome to the table"; color: window.ink; font.pixelSize: 28; font.weight: Font.Bold }
                    Text {
                        text: "Server-authoritative Texas Hold’em"
                        color: window.muted
                        font.pixelSize: 14
                        Layout.bottomMargin: 14
                    }
                    DarkField { id: hostField; Layout.fillWidth: true; text: "127.0.0.1"; placeholderText: "Lobby host" }
                    DarkField { id: portField; Layout.fillWidth: true; text: "6000"; placeholderText: "Port"; inputMethodHints: Qt.ImhDigitsOnly }
                    PrimaryButton {
                        Layout.fillWidth: true
                        text: client.connected ? "Lobby connected" : "Connect"
                        enabled: !client.connected
                        onClicked: client.connectToLobby(hostField.text, Number(portField.text))
                    }
                    Rectangle { Layout.fillWidth: true; height: 1; color: "#294238"; Layout.topMargin: 8; Layout.bottomMargin: 8 }
                    DarkField { id: usernameField; Layout.fillWidth: true; placeholderText: "Username" }
                    DarkField { id: passwordField; Layout.fillWidth: true; placeholderText: "Password (10+ characters)"; echoMode: TextInput.Password }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 10
                        Button {
                            Layout.fillWidth: true; implicitHeight: 44; text: "Create account"
                            enabled: client.connected
                            onClicked: client.registerAccount(usernameField.text, passwordField.text)
                        }
                        PrimaryButton {
                            Layout.fillWidth: true; text: "Sign in"
                            enabled: client.connected
                            onClicked: client.login(usernameField.text, passwordField.text)
                        }
                    }
                    Item { Layout.fillHeight: true }
                }
            }
        }

        Item {
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 34
                spacing: 20

                RowLayout {
                    Layout.fillWidth: true
                    Text { text: "Cash tables"; color: window.ink; font.pixelSize: 30; font.weight: Font.Bold }
                    Item { Layout.fillWidth: true }
                    Button { text: "Refresh"; onClicked: client.refreshTables() }
                    PrimaryButton { text: "New table"; onClicked: createPopup.open() }
                }

                GridView {
                    id: lobbyGrid
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    cellWidth: 300
                    cellHeight: 190
                    clip: true
                    model: client.tables
                    delegate: Rectangle {
                        width: 280; height: 170; radius: 18
                        color: window.panel
                        border.color: mouse.hovered ? window.accent : "#203a30"
                        Behavior on border.color { ColorAnimation { duration: 120 } }
                        HoverHandler { id: mouse }
                        ColumnLayout {
                            anchors.fill: parent; anchors.margins: 20; spacing: 8
                            Text { text: modelData.name; color: window.ink; font.pixelSize: 18; font.weight: Font.DemiBold }
                            Text { text: "BLINDS  " + modelData.blinds; color: window.accent; font.pixelSize: 13 }
                            Text { text: "NODE  " + modelData.node; color: window.muted; font.pixelSize: 12 }
                            Item { Layout.fillHeight: true }
                            RowLayout {
                                Layout.fillWidth: true
                                Text { text: modelData.players + " / " + modelData.maxPlayers + " players"; color: window.muted }
                                Item { Layout.fillWidth: true }
                                PrimaryButton { text: "Join"; onClicked: client.joinTable(modelData.tableId) }
                            }
                        }
                    }
                }
            }

            Popup {
                id: createPopup
                anchors.centerIn: parent
                width: 420; height: 520
                modal: true
                padding: 28
                background: Rectangle { radius: 20; color: window.panelRaised; border.color: "#315044" }
                ColumnLayout {
                    anchors.fill: parent; spacing: 12
                    Text { text: "Create cash table"; color: window.ink; font.pixelSize: 23; font.weight: Font.Bold }
                    DarkField { id: tableName; Layout.fillWidth: true; text: "Evening Table"; placeholderText: "Table name" }
                    Text { text: "Players"; color: window.muted }
                    SpinBox { id: maxPlayers; from: 2; to: 6; value: 6; Layout.fillWidth: true }
                    DarkField { id: smallBlind; Layout.fillWidth: true; text: "50"; placeholderText: "Small blind"; inputMethodHints: Qt.ImhDigitsOnly }
                    DarkField { id: bigBlind; Layout.fillWidth: true; text: "100"; placeholderText: "Big blind"; inputMethodHints: Qt.ImhDigitsOnly }
                    DarkField { id: minBuy; Layout.fillWidth: true; text: "2000"; placeholderText: "Minimum buy-in"; inputMethodHints: Qt.ImhDigitsOnly }
                    DarkField { id: maxBuy; Layout.fillWidth: true; text: "20000"; placeholderText: "Maximum buy-in"; inputMethodHints: Qt.ImhDigitsOnly }
                    Item { Layout.fillHeight: true }
                    RowLayout {
                        Layout.fillWidth: true
                        Button { Layout.fillWidth: true; text: "Cancel"; onClicked: createPopup.close() }
                        PrimaryButton {
                            Layout.fillWidth: true; text: "Create"
                            onClicked: {
                                client.createTable(tableName.text, maxPlayers.value,
                                                   Number(smallBlind.text), Number(bigBlind.text),
                                                   Number(minBuy.text), Number(maxBuy.text))
                                createPopup.close()
                            }
                        }
                    }
                }
            }
        }

        Item {
            id: tablePage
            function isSeated() {
                for (let i = 0; i < client.players.length; ++i)
                    if (client.players[i].userId === client.userId) return true
                return false
            }

            Rectangle {
                id: felt
                anchors.centerIn: parent
                width: Math.min(parent.width - 110, 1040)
                height: Math.min(parent.height - 150, 560)
                radius: height / 2
                color: window.felt
                border.color: "#b58d46"
                border.width: 10

                Rectangle {
                    anchors.fill: parent; anchors.margins: 18; radius: height / 2
                    color: "transparent"; border.color: "#1a7655"; border.width: 2
                }

                Column {
                    anchors.centerIn: parent
                    spacing: 14
                    Text { anchors.horizontalCenter: parent.horizontalCenter; text: client.street.toUpperCase() + "  ·  POT " + client.pot; color: "#d6e8dd"; font.pixelSize: 14; font.weight: Font.DemiBold }
                    Row {
                        anchors.horizontalCenter: parent.horizontalCenter
                        spacing: 10
                        Repeater {
                            model: client.board
                            Rectangle {
                                width: 66; height: 92; radius: 8; color: "#f3f0e7"
                                border.color: "#c6c0b2"
                                Text {
                                    anchors.centerIn: parent; text: modelData
                                    color: (String(modelData).includes("♥") || String(modelData).includes("♦")) ? "#c13b3b" : "#17201b"
                                    font.pixelSize: 23; font.weight: Font.Bold
                                }
                            }
                        }
                    }
                    Text { anchors.horizontalCenter: parent.horizontalCenter; text: "BET " + client.currentBet + "  ·  MIN RAISE " + client.minimumRaise; color: "#9dc4b1"; font.pixelSize: 12 }
                }

                Repeater {
                    model: client.players
                    delegate: Rectangle {
                        readonly property var px: [0.50, 0.82, 0.82, 0.50, 0.18, 0.18]
                        readonly property var py: [0.02, 0.27, 0.68, 0.91, 0.68, 0.27]
                        width: 178; height: 82; radius: 14
                        x: felt.width * px[modelData.seat % 6] - width / 2
                        y: felt.height * py[modelData.seat % 6] - height / 2
                        color: modelData.userId === client.actingUserId ? "#e6b85c" : "#10251c"
                        border.color: modelData.userId === client.userId ? "#f7dc9f" : "#2b4c3e"
                        opacity: modelData.connected ? 1 : 0.58
                        Column {
                            anchors.fill: parent; anchors.margins: 10; spacing: 3
                            Row {
                                spacing: 7
                                Text { text: "SEAT " + (modelData.seat + 1); color: modelData.userId === client.actingUserId ? "#253322" : window.muted; font.pixelSize: 10; font.weight: Font.Bold }
                                Text { text: modelData.status.toUpperCase(); color: modelData.userId === client.actingUserId ? "#253322" : window.accent; font.pixelSize: 10 }
                            }
                            Text { text: "Player " + modelData.userId + "  ·  " + modelData.stack; color: modelData.userId === client.actingUserId ? "#172016" : window.ink; font.pixelSize: 14; font.weight: Font.DemiBold }
                            Text { text: modelData.cards.length > 0 ? modelData.cards : "•  •"; color: modelData.userId === client.actingUserId ? "#384832" : "#d9e8df"; font.pixelSize: 15 }
                        }
                    }
                }
            }

            Rectangle {
                anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
                height: 92; color: "#0c1914"; border.color: "#203a30"
                RowLayout {
                    anchors.fill: parent; anchors.margins: 20; spacing: 10
                    Button { text: "Leave"; onClicked: client.leaveTable(); enabled: tablePage.isSeated() }
                    Item { Layout.fillWidth: true }
                    RowLayout {
                        visible: !tablePage.isSeated()
                        Text { text: "Seat"; color: window.muted }
                        SpinBox { id: seatChoice; from: 1; to: 6; value: 1 }
                        DarkField { id: buyIn; text: "5000"; placeholderText: "Buy-in"; implicitWidth: 130; inputMethodHints: Qt.ImhDigitsOnly }
                        PrimaryButton { text: "Sit down"; onClicked: client.sitDown(seatChoice.value - 1, Number(buyIn.text)) }
                    }
                    RowLayout {
                        visible: tablePage.isSeated()
                        Button { text: "Ready"; onClicked: client.setReady(true) }
                        Button { text: "Fold"; enabled: client.actingUserId === client.userId; onClicked: client.act("fold") }
                        Button { text: "Check"; enabled: client.actingUserId === client.userId; onClicked: client.act("check") }
                        Button { text: "Call"; enabled: client.actingUserId === client.userId; onClicked: client.act("call") }
                        DarkField { id: targetAmount; text: String(Math.max(client.currentBet + client.minimumRaise, 0)); implicitWidth: 130; inputMethodHints: Qt.ImhDigitsOnly }
                        PrimaryButton { text: "Bet / Raise"; enabled: client.actingUserId === client.userId; onClicked: client.act(client.currentBet > 0 ? "raise" : "bet", Number(targetAmount.text)) }
                        PrimaryButton { text: "All in"; enabled: client.actingUserId === client.userId; onClicked: client.act("all_in") }
                    }
                }
            }
        }
    }
}
