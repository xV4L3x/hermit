"use strict";

// Massimo tra due numeri. Target di patch: invertendo il salto
// condizionale nel corpo di `max` la funzione calcola il MINIMO.
function max(a, b) {
  if (a > b) {
    return a;
  }
  return b;
}

print(max(3, 7));
print(max(42, 17));
print(max(-5, -9));
