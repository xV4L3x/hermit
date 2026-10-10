"use strict";
// Literal buffer decompilation oracle - one array, one object,
// covering every SLP tag reachable from source syntax: Integer, Number
// (non-integral), string, bool, null.
var arr = [1, 2.5, "three", true, false, null];
var obj = { a: 1, b: "two", c: false };
print(arr.length, obj.a);
