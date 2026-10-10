// app.ts - entry point dell'applicazione
// Esportato come modulo Metro (module ID 2)
// Dipende da math_utils (0) e string_utils (1)

import { add, mul, clamp } from "./math_utils";
import { greet, repeat, pad } from "./string_utils";

export function run(): void {
    const x = add(3, 4);
    const y = mul(x, 2);
    const z = clamp(y, 0, 10);

    const msg = greet("hermit");
    const line = repeat("-", 20);
    const numStr = pad(String(z), 6);

    console.log(line);
    console.log(msg);
    console.log("result:" + numStr);
    console.log(line);
}

run();
