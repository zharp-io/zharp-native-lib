import QtQuick 2.15
import QtQuick.Controls 2.15

Page {
    title: "Payment"

    Label {
        anchors.centerIn: parent
        text: "Pay 25.00"
    }

    // Once the page is on top (and so is the screen), go on.
    Timer {
        interval: 500
        running: true
        onTriggered: {
            if (mode === "crash") payments.charge(2500);
            else if (mode === "fatal") payments.openDatabase();
            else Qt.quit();
        }
    }
}
