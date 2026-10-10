  function add(a: number, b: number): number {
    return a + b;
  }

  function greetAll(names: string[]): void {
    for (const name of names) {
      const msg = 'Hello, ' + name + '!';
      console.log(msg);
    }
  }
  
  const result = add(3, 4);
  console.log('Sum: ' + result);

  const team = ['alice', 'bob', 'charlie', 'diana'];
  greetAll(team);
  
  const obj = { x: 1, y: 2, z: 3 };
  console.log('x=' + obj.x);
