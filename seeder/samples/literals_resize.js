"use strict";
// Oracle (literal add/remove): array literal compilato come NewArrayWithBuffer;
// a.length cambia visibilmente dopo add/remove (a differenza di
// literals.js, che stampa a[1] - invariato da add/remove in coda).
var a = [10, 20, 30];
print(a.length);
