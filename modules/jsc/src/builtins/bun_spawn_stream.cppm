// Bun.spawn ReadableStream stdin adapter.
//
// This is a separate payload partition because process_web.cppm is already
// close to GCC's constant-evaluated string-length ceiling. js_builtins.cppm
// appends it immediately after process_web, so it remains inside the same
// Bun block and sees spawnAsyncBun, anyToU8, and the process object it returns.
export module mbun.jsc.js_builtins:bun_spawn_stream;

import std;

export namespace mbun::jsc::builtins::detail {

inline constexpr std::string_view kBunSpawnStreamJS = R"JS(
  // Web Streams protocol glue for Bun.spawn stdin. Native fd creation and
  // non-blocking writes remain in spawnAsyncBun/io_tick.
  const validateBunReadableStdin = (stream) => {
    if (stream.locked) throw new TypeError("'stdin' ReadableStream is locked");
    if (G.__mbunStreams && G.__mbunStreams.isDisturbed(stream)) {
      throw new TypeError("'stdin' ReadableStream has already been used");
    }
  };
  const pumpBunReadableStdin = (proc, source, isStream) => {
    let reader = null;
    let iterator = null;
    let childClosed = false;
    let cancelIssued = false;
    const childClosedResult = {};
    const cancelSource = () => {
      if (cancelIssued) return;
      cancelIssued = true;
      if (reader) {
        try {
          const result = reader.cancel();
          if (result && typeof result.catch === "function") result.catch(() => {});
        } catch (e) {}
      } else if (iterator && typeof iterator.return === "function") {
        try {
          const result = iterator.return();
          if (result && typeof result.catch === "function") result.catch(() => {});
        } catch (e) {}
      }
    };
    // A pending source pull must be cancelled after the child closes, or the
    // source can keep producing work after its pipe has gone away.
    const childExit = proc.exited.then(() => {
      childClosed = true;
      cancelSource();
      return childClosedResult;
    }, () => {
      childClosed = true;
      cancelSource();
      return childClosedResult;
    });
    const pump = (async () => {
      try {
        if (isStream) reader = source.getReader();
        else iterator = source[Symbol.asyncIterator]();
        if (childClosed) {
          cancelSource();
          return;
        }
        for (;;) {
          const next = isStream ? reader.read() : iterator.next();
          const result = await Promise.race([next, childExit]);
          if (result === childClosedResult) break;
          if (result.done) break;
          if (childClosed || proc.killed || proc.exitCode !== null || proc.signalCode !== null) {
            cancelSource();
            break;
          }
          proc.stdin.write(result.value);
        }
      } catch (e) {
        // Preserve bytes already queued and keep source errors from becoming
        // unhandled rejections after the child has closed.
        if (!childClosed) { try { proc.stdin.end(); } catch (e2) {} }
      } finally {
        try { if (reader) reader.releaseLock(); } catch (e) {}
        try { proc.stdin.end(); } catch (e) {}
      }
    })();
    pump.catch(() => {});
  };
)JS";

}  // namespace mbun::jsc::builtins::detail
