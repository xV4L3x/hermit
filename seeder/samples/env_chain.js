// Sample: closure environments - env_accesses() / environments() /
// captured_environment() (B30 step 2, M2). Compiled with -g so the slot names
// of every scope are in the debug info (scope_variables()), the ground truth
// of the tests.

function outer(a) {
  var x = a + 1;
  function middle(b) {
    var y = b * 2;
    function inner(c) {
      // level 0 = middle's scope (y), level 1 = outer's scope (x)
      x = x + y + c;
      return x;
    }
    return inner;
  }
  function* gen(n) {
    var g = n;
    yield g;
    // after the resume: the closure below captures gen's scope again
    yield function () { return g + x; };
  }
  return [middle(a), gen(a)];
}

var parts = outer(1);
print(parts[0](2));
var it = parts[1];
it.next();
print(it.next().value());
