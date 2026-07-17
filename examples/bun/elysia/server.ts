import { Elysia } from "elysia";

new Elysia()
  .get("/", () => `<!doctype html>
<html lang="en">
  <head>
    <meta charset="utf-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <title>Elysia on mbun</title>
    <style>
      body { margin: 0; min-height: 100vh; display: grid; place-items: center; background: #071a14; color: #effff8; font: 16px system-ui, sans-serif; }
      main { max-width: 600px; padding: 48px; border: 1px solid #2dd4a7; border-radius: 24px; background: radial-gradient(circle at top right, #185c4d, #0b2a22 60%); box-shadow: 0 24px 80px #0009; }
      span { color: #5eead4; }
      p { color: #b5dfd3; line-height: 1.6; }
    </style>
  </head>
  <body><main><span>BUN / ELYSIA</span><h1>Hello from mbun</h1><p>This page was installed and served by mbun using Elysia.</p></main></body>
</html>`)
  .listen(3000);

console.log("http://127.0.0.1:3000");
