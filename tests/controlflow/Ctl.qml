import QtQuick
QtObject {
    function branches(x) {
        let r = 0
        if (x > 0)
            r = 1
        else
            r = 2
        return r
    }
    function loops(n) {
        let s = 0
        for (let i = 0; i < n; ++i)
            s += i
        let j = n
        while (j > 0)
            j--
        do {
            s++
        } while (s < 0)
        return s
    }
    function sw(k) {
        switch (k) {
        case 1:
            return "one"
        default:
            return "other"
        }
    }
    property int r2: 0
    function guarded(f) {
        try {
            f()
            return 0
        } catch (e) {
            return 1
        } finally {
            r2 = 1
        }
    }
}
