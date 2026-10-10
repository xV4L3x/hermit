"use strict";
function max(a, b) {
    if (a > b) {
        return a;
    }
    return b;
}
function factorial(n) {
    if (n <= 1) {
        return 1;
    }
    return n * factorial(n - 1);
}
function sumTo(n) {
    let total = 0;
    for (let i = 1; i <= n; i++) {
        total = total + i;
    }
    return total;
}
