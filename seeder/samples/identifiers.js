// B3 sample: property names, a global and an object literal, all Identifier
// strings. `alpha` is read at two sites so a global rename and a one-site
// rename give different output. "beta value" is a plain String used as the
// wrong-kind target of a GetById in the tests.
"use strict";

var counter = 7;

function read(o) {
    return o.alpha;
}

function main() {
    var o = { alpha: 1, gamma: 3 };
    o.delta = 4;
    print(read(o));
    print(o.alpha);
    print(o.gamma + o.delta);
    print(counter);
    print("beta value");
}

main();
