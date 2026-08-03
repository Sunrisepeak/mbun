const code = await Bun.file(process.argv[2]).text();
globalThis.__bundlerChecksum = 0;
(0, eval)(code);
console.log(globalThis.__bundlerChecksum);
