import QtQuick
import "util.js" as Util
QtObject {
    property int base: 10
    property int doubled: base * 2
    function add(a, b) { return a + b }
    function neverCalled(a) { return a > 0 ? a : -a }
    function sumTo(n) {
        let total = 0
        for (let i = 1; i <= n; ++i)
            total = add(total, i)
        return total
    }
    function viaJs(x) { return Util.square(x) }
}
