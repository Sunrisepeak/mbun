const express = require("express");
const app = express();

app.get("/", (_request, response) => {
  response.type("html").send(`
    <!doctype html>
    <html lang="en">
      <head>
        <meta charset="utf-8">
        <meta name="viewport" content="width=device-width, initial-scale=1">
        <title>Express on mbun</title>
        <style>
          body { margin: 0; min-height: 100vh; display: grid; place-items: center; background: #101820; color: #f2f7f5; font: 16px system-ui, sans-serif; }
          main { max-width: 600px; padding: 48px; border: 1px solid #2d6975; border-radius: 24px; background: linear-gradient(135deg, #173f4b, #102b36); box-shadow: 0 24px 80px #0008; }
          span { color: #ffb703; }
          p { color: #b9d7d9; line-height: 1.6; }
        </style>
      </head>
      <body><main><span>NODE / EXPRESS</span><h1>Hello from mbun</h1><p>This page was installed and served by mbun using Express.</p></main></body>
    </html>`);
});

app.listen(3000, "127.0.0.1", () => {
  console.log("http://127.0.0.1:3000");
});
