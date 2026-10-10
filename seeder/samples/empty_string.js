"use strict";
// Oracle: a String-kind entry with byte_length==0, so that
// growing an empty entry into a real string ("append into nothing", zero
// dead bytes to leave behind) can be tested end-to-end with hermes.
function g() { return ""; }
print(g());
