// invoke_kinds.js - one function per ProhibitInvoke value emitted by hermes v96
// (flags bits 0-1 of SmallFuncHeader, a 2-bit enum):
//   2 = None      -> plainDecl, shorthandMethod (callable and constructible)
//   1 = Construct -> arrowFn, asyncFn, genFn    (cannot be used with `new`)
// 0 = Call (class constructor) is never emitted without -Xes6-class: React
// Native bundles transpile classes to plain functions.
function plainDecl(a) { this.v = a; return a + 1; }
var arrowFn = (a) => a * 2;
var obj = { shorthandMethod() { return 1; } };
async function asyncFn() { return 3; }
function* genFn() { yield 4; }

var r1, r2, r3;
try { r1 = "call ok: " + plainDecl(1); } catch (e) { r1 = "call ERR: " + e.message; }
try { r2 = "new ok: " + new plainDecl(2).v; } catch (e) { r2 = "new ERR: " + e.message; }
try { r3 = "new arrow ok"; new arrowFn(3); } catch (e) { r3 = "new arrow ERR: " + e.message; }
print(r1); print(r2); print(r3);
print(obj.shorthandMethod(), genFn().next().value, typeof asyncFn);
