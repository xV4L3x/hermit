// Loop shapes for the decompiler's natural-loop structurer (while / do-while /
// for, test on top or at the latch, with break / continue and nesting).
// Globals keep Hermes from optimizing the bodies away.

var SINK = 0;

function countWhile(n) {
  var s = 0;
  while (n > 0) {
    s += n;
    n--;
  }
  return s;
}

function sumDoWhile(n) {
  var s = 0;
  var i = 0;
  do {
    s += i;
    i++;
  } while (i < n);
  return s;
}

function firstEven(a) {
  var found = -1;
  for (var i = 0; i < a.length; i++) {
    if (a[i] % 2 === 0) {
      found = i;
      break;
    }
  }
  return found;
}

function sumOdd(a) {
  var s = 0;
  for (var i = 0; i < a.length; i++) {
    if (a[i] % 2 === 0) {
      continue;
    }
    s += a[i];
  }
  return s;
}

function nestedMul(n, m) {
  var s = 0;
  for (var i = 0; i < n; i++) {
    for (var j = 0; j < m; j++) {
      s += i * j;
    }
  }
  return s;
}

function findIndex(a, x) {
  var i = 0;
  while (i < a.length) {
    if (a[i] === x) {
      return i;
    }
    i++;
  }
  return -1;
}

SINK = countWhile(3) + sumDoWhile(4) + firstEven([1, 2]) + sumOdd([1, 2, 3]) +
       nestedMul(2, 2) + findIndex([5, 6], 6);
