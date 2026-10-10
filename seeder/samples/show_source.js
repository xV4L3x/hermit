"use strict";
// Probes Hermes' SourceVisibility directives ("show
// source" / "hide source" / "sensitive") and what exactly ends up in the
// Function Source Table (header 0x64 functionSourceCount) for each case.
function withComment() {
  "show source";
  // a comment inside the function
  var x = 1;
  return x + 1;
}

var arrowShow = () => {
  "show source";
  return 42;
};

function hidden() {
  "hide source";
  return "secret";
}

function sensitiveFn() {
  "sensitive";
  return "topsecret";
}

function normal() {
  return "nothing special here";
}

print(withComment(), arrowShow(), hidden(), sensitiveFn(), normal());
