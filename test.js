const { Example } = require('ih-lib60870-node');

function getNativeBinding() {
  const binding = require(bindingPath);
  console.log('Exported by binding:', Object.keys(binding));
  return binding;
}

try {
  const example = new Example(42);
  console.log('Platform:', example.getPlatform());
  console.log('Result:', example.add(10));
} catch (error) {
  console.error('Error:', error.message);
}