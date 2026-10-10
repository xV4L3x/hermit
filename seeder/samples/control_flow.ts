function max(a: number, b: number): number {
  if (a > b) {
    return a;
  }
  return b;
}
function factorial(n: number): number {
  if (n <= 1) {
    return 1;
  }
  return n * factorial(n - 1);
}
function sumTo(n: number): number {
  let total = 0;
  for (let i = 1; i <= n; i++) {
    total = total + i;
  }
  return total;
}
