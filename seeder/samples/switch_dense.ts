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
console.log(classify(7));
