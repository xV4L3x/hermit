// functions.ts - sample: funzioni nominate, anonime, arrow, callback
// Serve a verificare che:
//   - le funzioni nominate mostrino il loro nome
//   - le funzioni anonime mostrino "anonymous_func_N"
//   - paramCount includa this (js_params = paramCount - 1)

// Funzione nominata con 2 parametri JS -> paramCount = 3 (this + a + b)
function add(a: number, b: number): number {
    return a + b;
}

// Funzione nominata con 0 parametri JS -> paramCount = 1 (solo this)
function getVersion(): string {
    return "1.0";
}

// Arrow function anonima assegnata a variabile -> "anonymous_func_N"
const double = (x: number) => x * 2;

// Callback anonima passata direttamente -> "anonymous_func_N"
const numbers = [1, 2, 3];
const squared = numbers.map(function(n: number) { return n * n; });

// Funzione nominata con callback anonima interna
function applyTwice(f: (x: number) => number, x: number): number {
    return f(f(x));
}

// IIFE anonima
const result = (function() {
    const a = add(1, 2);
    const b = add(3, 4);
    return add(a, b);
})();

console.log(result);
console.log(double(21));
console.log(getVersion());
console.log(applyTwice(double, 3));
