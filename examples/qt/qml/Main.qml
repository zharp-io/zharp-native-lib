import QtQuick 2.15
import QtQuick.Controls 2.15

ApplicationWindow {
    width: 360
    height: 480
    visible: true
    title: "Zharp shop"

    StackView {
        id: stack
        anchors.fill: parent
        initialItem: "qrc:/CartPage.qml"
    }
}
