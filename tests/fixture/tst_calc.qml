import QtQuick
import QtTest
import "first.js" as First
TestCase {
    name: "Calc"
    Calc { id: c }
    function test_add() { compare(c.add(1, 1), 2) }
    function test_sum() { compare(c.sumTo(3), 6) }
    function test_js() { compare(c.viaJs(3), 9) }
    function test_first() { compare(First.later(function() { return First.first(1) }), 2) }
}
