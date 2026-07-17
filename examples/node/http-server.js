const http = require("node:http");

http.createServer((_request, response) => {
  response.writeHead(200, { "content-type": "text/plain" });
  response.end("hello from Node APIs on mbun\n");
}).listen(3000, "127.0.0.1", () => {
  console.log("http://127.0.0.1:3000");
});
