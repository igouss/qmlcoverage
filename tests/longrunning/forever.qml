import QtQuick
Item {
    property int ticks: 0
    function tick() { ticks += 1 }
    function never() { return 0 }
    Timer { interval: 100; running: true; repeat: true; onTriggered: tick() }
}
