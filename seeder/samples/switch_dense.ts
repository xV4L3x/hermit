// Dense integer switch (>=10 cases, contiguous range) - the only shape
// Hermes lowers to SwitchImm (LowerSwitchIntoJumpTables, needs numCases>=10
// and range/numCases<=5). A sparse/small switch (see switch.ts) compiles to
// a chain of JStrictEqual instead - never exercises the jump table format.
function classify(i: number) {
    switch (i) {
        case 0: return "zero";
        case 1: return "one";
        case 2: return "two";
        case 3: return "three";
        case 4: return "four";
        case 5: return "five";
        case 6: return "six";
        case 7: return "seven";
        case 8: return "eight";
        case 9: return "nine";
        case 10: return "ten";
        case 11: return "eleven";
        default: return "other";
    }
}

// Second dense switch (10 cases 0..9, also lowered to SwitchImm) exercising the
// shapes `classify` lacks: grouped labels (0 and 1 share a body), JS
// fall-through (case 2 has no break and flows into 3), an empty case (5), a
// default with a body, and a join block after the switch (`return r`).
function route(i: number): number {
    let r = 0;
    switch (i) {
        case 0:
        case 1:
            r = 10;
            break;
        case 2:
            r = 20;
            // fall-through
        case 3:
            r = r + 5;
            break;
        case 4:
            r = 40;
            break;
        case 5:
            break;
        case 6:
            r = 60;
            break;
        case 7:
            r = 70;
            break;
        case 8:
            r = 80;
            break;
        case 9:
            r = 90;
            break;
        default:
            r = -1;
            break;
    }
    return r;
}

console.log(classify(7), route(2));
