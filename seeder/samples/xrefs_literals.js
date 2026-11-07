"use strict";
// String XREF oracle for literal buffers: every string below is referenced
// only from an array/object literal buffer (NewArrayWithBuffer /
// NewObjectWithBuffer), except "beta", which is also a LoadConstString operand.
//   fragments : 3-string array (the supply-chain PoC shape)
//   dup       : "dup" twice in one array -> one site, two elements
//   mixed     : strings and non-strings in one array
//   obj       : string keys and string values
//   first/second : the same array literal in two functions (shared buffer)
var fragments = ["loca", "lho", "st"];
var dup = ["dup", "dup", "other"];
var mixed = ["mx1", 7, "mx2", true, null, "mx3"];
var obj = { kx: "vx", ky: "vy", kz: "vz" };
function first()  { return ["alpha", "beta", "gamma", "delta", "eps", "zeta"]; }
function second() { return ["alpha", "beta", "gamma", "delta", "eps", "zeta"]; }
print(fragments, dup, mixed, obj, first(), second(), "beta");
