import { AsyncLocalStorage, AsyncResource } from "async_hooks";
import { expect, test } from "bun:test";
import { EventEmitter } from "events";

test("explicit promise, timer, nextTick, and EventEmitter callbacks retain context", async () => {
  const storage = new AsyncLocalStorage<string>();
  const emitter = new EventEmitter();
  const seen: string[] = [];
  let resolve!: () => void;
  const pending = new Promise<void>(done => (resolve = done));

  storage.run("focused", () => {
    pending.then(() => seen.push(`then:${storage.getStore()}`));
    queueMicrotask(() => seen.push(`microtask:${storage.getStore()}`));
    process.nextTick(() => seen.push(`nextTick:${storage.getStore()}`));
    setTimeout(() => seen.push(`timeout:${storage.getStore()}`), 1);
    emitter.on("event", () => seen.push(`event:${storage.getStore()}`));
  });

  resolve();
  emitter.emit("event");
  await new Promise(done => setTimeout(done, 2));
  expect(seen).toContain("then:focused");
  expect(seen).toContain("microtask:focused");
  expect(seen).toContain("nextTick:focused");
  expect(seen).toContain("timeout:focused");
  expect(seen).toContain("event:focused");
});

test("AsyncResource invokes callbacks in its construction snapshot", () => {
  const storage = new AsyncLocalStorage<string>();
  let resource!: AsyncResource;
  storage.run("resource", () => (resource = new AsyncResource("focused")));
  expect(resource.runInAsyncScope(() => storage.getStore())).toBe("resource");
});
