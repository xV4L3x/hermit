// string_utils.ts - modulo di utilità su stringhe
// Esportato come modulo Metro (module ID 1)

export function greet(name: string): string {
    return "Hello, " + name + "!";
}

export function repeat(s: string, n: number): string {
    let result = "";
    for (let i = 0; i < n; i++) {
        result = result + s;
    }
    return result;
}

export function pad(s: string, width: number): string {
    while (s.length < width) {
        s = " " + s;
    }
    return s;
}
