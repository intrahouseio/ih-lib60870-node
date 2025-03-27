const { Example } = require('ih-lib60870-node');

try {
  const example = new Example(42);
  console.log('Platform:', example.getPlatform());
  console.log('Result:', example.add(10));
} catch (error) {
  console.error('Error:', error.message);
}