// hang_dump — answer "why is this corpus file hanging?" without a rebuild.
//
// Usage:  mbun tools/integration/hang_dump.js <abs-path-to-test.js> [ms]
//         (always through tools/integration/safe-test.sh — the target hangs)
// Exit:   99 after dumping, so a caller can tell a dump from the test's own exit.
//
// WHY THIS EXISTS: the corpus classifies a hang as `timeout`, which says nothing
// about the cause. 46 hangs across tls/http/fs had received zero investigation
// across several rounds precisely because there was no cheap way to look inside
// one. With this, all 46 were triaged in a single session, and the answer
// overturned the standing assumption:
//
//   20 of 41 survivors: Server{listening} + a live socket pair, nothing
//                       destroyed, _wq=0, no ref'd timer -> the exchange is
//                       stalled mid-flight and the test is genuinely WAITING.
//                       node would hold the loop here too, so these are
//                       protocol-semantics gaps, not handle leaks.
//   11 of 41:           Server{listening} alone, socket count 0 -> the client
//                       side died and nothing ever calls server.close().
//    4 of 41:           no dump at all (rc=124) -> the pump is blocked inside
//                       native code and JS never regains control. Most severe.
//    3 of 41:           a ref'd interval whose terminating condition is never
//                       reached.
//    2 of 41:           FSWatcher counts as NET.pending=1 but never progresses,
//                       so the drain's stall clock runs forever with no fd in
//                       the poll set.
//
// That histogram is the deliverable: 31 of 41 are NOT event-loop bugs, which is
// why every attempt to sweep this block failed. See
// compat/README.md "Classify by the failure's layer, not by its log text".
//
// HOW IT WORKS: `require` the target rather than exec it, so this script's own
// timer stays in the same loop. The timer is REF'D, which is what makes the dump
// possible at all — a ref'd timer bounds the pump's otherwise 60-second poll()
// park, so control returns to JS while the hang is still in progress.
const target = process.argv[2];
const ms = +(process.argv[3] || 8000);

if (!target) {
  console.error("usage: mbun hang_dump.js <abs-path-to-test.js> [ms]");
  process.exit(2);
}

// Deliberately ref'd: see the note above.
setTimeout(() => {
  const NET = globalThis.__mbunNet;
  const T = globalThis.__mbunTimers;
  const out = ["--- HANGDUMP ---"];

  if (NET) {
    out.push(
      "NET.pending=" + NET.pending +
      " handles=" + NET.handles +
      " serveActive=" + NET.serveActive +
      " stall=" + NET.stall +
      " items=" + NET.items.size);
    let i = 0;
    for (const it of NET.items) {
      const d = ["#" + i++, "ctor=" + (it.constructor && it.constructor.name)];
      // The fields that distinguish "waiting for an event" from "leaked handle":
      // a held+ref'd server with no sockets is a dead client flow; a held server
      // WITH live sockets and an empty write queue is a stalled exchange.
      for (const k of ["_fd", "_refd", "_held", "_loopOpen", "destroyed", "listening",
                       "connecting", "readable", "writable", "_pendingOp", "_done",
                       "_eofPushed", "_ended", "_readEnded", "_paused", "_reading"]) {
        if (k in it) d.push(k + "=" + String(it[k]));
      }
      if (it._wq) d.push("_wq=" + it._wq.length);
      out.push("  " + d.join(" "));
    }
  } else {
    out.push("no __mbunNet (the reactor never initialised — suspect a load-time hang)");
  }

  if (T) {
    out.push("timers=" + T.q.length + " refd=" + T.q.filter((x) => x.refd).length);
    for (const x of T.q) {
      // The timer's source text is what identifies whose interval is never
      // converging; a ref'd interval with a far-future `at` is the signature.
      out.push("  timer id=" + x.id + " refd=" + x.refd + " iv=" + x.iv +
               " d=" + x.d + " in=" + (x.at - Date.now()) +
               " fn=" + String(x.fn).slice(0, 120).replace(/\n/g, " "));
    }
  } else {
    out.push("no __mbunTimers");
  }

  out.push("--- /HANGDUMP ---");
  console.error(out.join("\n"));
  process.exit(99);
}, ms);

require(target);
