import QtQml

QtObject {
    function once() { return 1 }
    function tick() { return 2 }
    property Timer timer: Timer { interval: 50; running: true; repeat: true; onTriggered: tick() }
    Component.onCompleted: once()
}
