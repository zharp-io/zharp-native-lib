import QtQuick 2.15
import QtQuick.Controls 2.15

Page {
    id: page
    title: "Cart"

    Label {
        anchors.centerIn: parent
        text: "3 items"
    }

    Timer {
        interval: 300
        running: true
        onTriggered: {
            var request = new XMLHttpRequest();
            request.onreadystatechange = function () {
                if (request.readyState !== XMLHttpRequest.DONE) return;
                console.warn("payment provider is slow (2300 ms)");
                page.StackView.view.push("qrc:/PaymentPage.qml");
            };
            request.open("GET", shopApi + "/prices?session=abc123");
            request.setRequestHeader("Authorization", "Bearer shop-secret");
            request.send();
        }
    }
}
