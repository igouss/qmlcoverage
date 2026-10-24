import QtQuick
import QtTest
TestCase {
    name: "Second"
    Calc { id: c }
    function test_neg() { compare(c.neverCalled(-2), 2) }
}
