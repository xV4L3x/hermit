// Sample: cattura di variabili in closure - verifica scope_variables()

function makeCounter(start: number) {
  let count = start;
  let step = 1;
  return function increment(): number {
    count = count + step;
    return count;
  };
}

const counter = makeCounter(10);
console.log(counter());
