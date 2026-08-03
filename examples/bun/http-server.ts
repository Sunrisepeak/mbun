Bun.serve({
  port: 3000,
  fetch: () => new Response("hello from Bun APIs on mbun\n"),
});

console.log("http://127.0.0.1:3000");
