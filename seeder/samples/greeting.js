"use strict";
// Oracle: a single string literal returned and printed, so that
// `hermes greeting_g0.hbc` echoes it to stdout. Editing the "Hello, world"
// entry with `edit string:<idx>` must change what the runtime prints.
function g() { return "Hello, world"; }
print(g());
