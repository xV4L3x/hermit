// math_utils.ts - modulo di utilità matematica
// Esportato come modulo Metro (module ID 0)

export function add(a: number, b: number): number {
    return a + b;
}

export function sub(a: number, b: number): number {
    return a - b;
}

export function mul(a: number, b: number): number {
    return a * b;
}

export function clamp(value: number, min: number, max: number): number {
    if (value < min) return min;
    if (value > max) return max;
    return value;
}
