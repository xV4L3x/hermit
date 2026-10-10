// Sample: debug string references are BYTE OFFSETS, not positional indices.
//
// Scope descriptors and textified callees name things by the byte offset of
// the entry in the debug string table. With a single string the offset and
// the position are both 0, so the older corpus could not tell them apart.
// Here the table holds several names of different lengths:
//   - long names push most offsets past the end of the table, where an
//     index-based reader would print <debug_str_N> placeholders;
//   - one-letter names (a, b) give small offsets that fall inside the table,
//     where an index-based reader would silently return the WRONG name.
//
// Compiled with `hermes -emit-binary -g` -> seeder/corpus/debug_names_g.hbc.

function makeState() {
  let a = 1;
  let b = 2;
  let counter = 3;
  let totalValue = 4;
  return function bump() {
    a = a + b;
    counter = counter + totalValue;
    return a + counter;
  };
}

function runAll() {
  var step = makeState();
  print(step());
  console.log(step());
  return step;
}

runAll();
