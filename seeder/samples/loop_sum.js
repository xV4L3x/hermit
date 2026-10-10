"use strict";

// loopSum: somma 1..n con un for. Il corpo contiene un salto condizionale
// all'indietro (il loop) - target di test (inserire un'istruzione
// prima del backward jump e verificare che il target resti corretto).
function loopSum(n) {
  var s = 0;
  for (var i = 1; i <= n; i++) {
    s = s + i;
  }
  return s;
}

print(loopSum(5));   // 15
print(loopSum(10));  // 55
