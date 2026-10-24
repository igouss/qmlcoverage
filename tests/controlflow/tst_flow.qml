import QtQuick
import QtTest
TestCase {
    name: "Flow"
    Ctl { id: f }
    function test_all() {
        f.branches(1); f.branches(-1); f.branches(-2)
        f.loops(3)
        f.sw(1); f.sw(2); f.sw(3)
        f.guarded(function() {}); f.guarded(function() { throw 1 })
    }
}
