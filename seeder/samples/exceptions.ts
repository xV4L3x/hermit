// Sample: exception handling - verifica hasExceptionHandler + CFG edges

function safeDivide(a: number, b: number): number {
  try {
    if (b === 0) throw new Error("division by zero");
    return a / b;
  } catch (e) {
    return -1;
  }
}

function parseNumber(s: string): number {
  try {
    const n = parseInt(s, 10);
    if (isNaN(n)) throw new Error("not a number");
    return n;
  } catch (e) {
    return 0;
  } finally {
    // cleanup
    s = "";
  }
}

console.log(safeDivide(10, 2));
console.log(safeDivide(10, 0));
console.log(parseNumber("42"));
console.log(parseNumber("abc"));
