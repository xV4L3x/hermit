"use strict";

// risky: try/catch -> la funzione ha una exception handler table.
// Target di test: inserire un'istruzione prima del try-range e
// verificare che start/end/target dell'handler vengano rilocati correttamente.
function risky(a) {
  try {
    if (a < 0) throw new Error("neg");
    return a * 2;
  } catch (e) {
    return -1;
  }
}

print(risky(5));   // 10
print(risky(-3));  // -1
