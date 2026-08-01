# W41 Linux corpus coverage record

## Scope

This checkpoint continues PR #36 against `rewrite_bun_in_mcpp`. It uses exact
failure-file lists, bounded runners, and one coordinator build. It does not
claim a new full-corpus score.

## Baseline and planner

| corpus | full baseline | actionable files |
| --- | ---: | ---: |
| Node | 3134/4433 green (70.7%) | 425 |
| Bun | 1015/1902 green (53.4%) | 170 |

Commit `00b1fcc` makes throughput totals ignore rows whose `minutes` or
`delivered` fields are `PENDING`. The fixture now fails before the fix and the
planner self-test passes after it. The resource guard planned five lanes under
the observed Linux limits; the coordinator owns the only root build.

## Probe ledger

| wave | lane | selected files | result | strategy |
| --- | --- | ---: | --- | --- |
| 1 | Node crypto | 24 | 0 green, 24 fail | park; native/PQC/FIPS/Argon2 walls |
| 1 | Node VM | 19 | 0 green, 19 fail | retain only isolated candidates |
| 1 | Node WebCrypto | 19 | 0 green, 19 fail | park; unsupported algorithms and WebIDL gaps |
| 1 | Bun third-party | 25 | 0 green, 1280 pass / 134 fail assertions | park; optional native/TLS/external runtime |
| 1 | Bun CLI/run | 17 | 0 green, 19 pass / 258 fail assertions | park; package/FUSE/bun:bundle gaps |
| 2 | Node test-runner | 30 | 0 green, 30 fail | keep; snapshot surface is the clearest seam |
| 2 | Node test-util | 12 | 0 green, 12 fail | keep only low-risk isolated APIs |
| 2 | Node test-v8 | 11 | 0 green, 11 fail | park; profiler/queryObjects/startup snapshot ownership |
| W83 | Bun/Node path | 5 Bun + 1 Node guard | pre 4/5 Bun green; post 5/5 Bun green; Node 1/1 | issue #50; dialect-only error-text fix |
| W84 | Bun Node-path continuation | 5 | 5/5 green; 88 passed / 0 failed / 89 ran | retain as green path coverage; no source owner |
| W85 | Bun os/string_decoder | 5 | pre 3 green + 1 skipped; post 4 green + 1 skipped; 149 passed / 0 failed / 150 ran | issue #51; dialect-aware Buffer ceiling |
| W86 | Bun Buffer completion guards | 5 | 4/5 green; 25 passed / 6 failed / 31 ran | park concat multi-owner; preserve four green guards |
| W87 | Bun Node-fs leaves | 5 | 5/5 green; 46 passed / 0 failed / 70 ran / 92 expects | fresh confirmation; no source owner |
| W88 | Bun Node-fs directory/Stats leaves | 5 | 5/5 green; 52 passed / 0 failed / 55 ran / 138 expects | fresh confirmation of narrow fs leaves; no source owner |
| W89 | Bun Node-inspector probe | 5 | 4/5 green; 34 passed / 27 failed / 64 ran / 131 expects | park inspector-profiler behind missing inspector/profiler subsystem |
| W90 | Bun Node-fs/child_process leaves | 5 | corrected probe: 4/5 green; 39 passed / 12 failed / 56 ran / 107 expects | retain four green leaves; park fs/promises multi-owner failures |
| W91 | Bun Node-net leaves | 5 | 3/5 green; 5 passed / 1 failed / 7 ran / 15 expects; one no-tests stress fixture | retain three green net leaves; park autoSelectFamily liveness |
| W92 | Bun/Node fs/promises AbortError owner | 5 Bun + 3 Node guards | Bun target 24 passed / 5 failed / 34 ran; 4 Bun guards green; Node 2/3 pass | issue #52; focused message fix landed, remaining fs/promises owners parked |
| W93 | Node fs/promises FileHandle leaves | 5 | 5/5 files pass; no build | retain FileHandle coverage after W92 fix; no source owner |
| W94 | Bun HTTP/2/Worker staged leaves | 5 | pre-fix 4/5 green; 7 passed / 1 failed / 8 ran / 6 expects | issue #53; isolate reserved-push DATA state |
| W95 | Bun HTTP/2/Worker staged regression | 5 | post-fix 5/5 green; 8 passed / 0 failed / 8 ran / 8 expects | issue #53 landed; retain all five guards |
| W96 | Node HTTP/2 RST lifecycle | 5 Node + 5 Bun guards | pre-fix Node 4/5 pass; post-fix Node 5/5 pass; Bun 5/5 green, 8 passed / 0 failed / 8 ran / 8 expects | issue #54; align readable end and non-zero peer-RST error delivery |
| W97 | Node HTTP/2 connect-abort teardown | 5 Node + 5 Node/Bun regression guards | pre-fix Node 4/5 pass; post-fix 5/5 pass; W96 Node 5/5 pass; W95 Bun 5/5 green, 8 passed / 0 failed / 8 ran / 8 expects | issue #55; preserve session AbortError while canceling streams with `ERR_HTTP2_STREAM_CANCEL` |
| W98 | Bun standard-module/API leaf probe | 5 | 4/5 files green; 161 passed / 1 ahead-of-reference / 165 ran / 100536 expects | retain four green leaves; classify `require`'s passing `test.failing` case as ahead-of-reference, no source owner |
| W99 | Bun fs/streams/spawn/DNS/URL leaves | 5 + DNS isolated rerun | initial 4/5 green; 108 passed / 1 failed / 109 ran / 355 expects; DNS isolated 1/1 green with 69/69 | retain four stable leaves; DNS public-answer variance is external, no source owner |
| W100 | Node fs/promises FileHandle leaves | 5 | 5/5 files pass; no build | retain five green leaves; no source owner |
| W101 | Node module loader / CLI entry leaves | 5 | pre-fix 3/5 pass + 1 skipped + 1 fail; post-fix 4/5 pass + 1 skipped; W100 regression 5/5 pass | issue #56; preserve `Module.runMain()` as the Node preload entry hook |
| W102 | Node module introspection / lookup leaves | 5 | 4/5 files pass; 1 fail; no build | retain four green leaves; park `require.extensions` custom-loader integration |
| W103 | Bun `node:module` / SourceMap leaves | 5 | 3/5 files green; 44 passed / 10 failed / 54 ran / 142 expects; no build | retain three green leaves; park split CJS-loader and malformed-sourcemap diagnostic owners |
| W104 | Bun Node process / stdio leaves | 5 | 5/5 files green; 39 passed / 0 failed / 39 ran / 93 expects; no build | retain all five green leaves; no source owner |
| W105 | Node process identity / timing leaves | 5 | 5/5 files pass; no build | retain all five green leaves; no source owner |
| W106 | Node process exitCode validation leaves | 5 | pre-fix 4/5 pass; post-fix 5/5 pass; issue #57; fresh serialized build | retain all five leaves; keep exitCode owner closed unless a new reproduction reopens it |
| W107 | Node net low-coupling leaves | 5 | 5/5 files pass; no build | retain all five green net leaves; no source owner |
| W108 | Bun Node-net constructor/server leaves | 5 | pre 2/5 green, 147 passed / 20 failed / 175 ran / 300 expects; post 2/5 green, 152 passed / 15 failed / 175 ran / 301 expects | issue #58; retain two green leaves, park node-net multi-owner failures and matcher-only gaps |
| W109 | Node TLS constructor/default-option leaves | 5 | 3/5 pass; 1 fail; 1 timeout; isolated timeout reproduced at one job / 60s | park TLS pauseOnConnect propagation and silent socket-default timeout as separate owners |
| W110 | Bun util low-coupling leaves | 5 | 5/5 green; 57 passed / 0 failed / 57 ran / 966 expects; no build | retain all five green leaves; no source owner |
| W111 | Node child_process basic contract leaves | 5 | 5/5 pass; 0 fail; 0 timeout; no build | retain all five green leaves; keep fork/IPC and timeout/kill owners separate |
| W112 | Node child_process adjacent contract leaves | 5 | 5/5 pass; 0 fail; 0 timeout; no build | retain all five green leaves; park IPC backlog/handle and signal-race owners |
| W113 | Node process environment/runtime leaves | 5 | 4/5 pass; 1 fail; 0 timeout; TZ failure reproduced at 1 job / 60s | issue #59; retain four green leaves, park existing-Date timezone cache invalidation |
| W114 | Node streams writable/readable basic leaves | 5 | 5/5 pass; 0 fail; 0 timeout; no build | retain all five green leaves; no source owner |
| W115 | Node streams event-order/pipe leaves | 5 | 4/5 pass; 1 fail; 0 timeout; TickObject failure reproduced at 1 job / 60s | issue #60; retain four green leaves, park callback-less Writable tick scheduling |
| W116 | Node streams state/encoding leaves | 5 | 5/5 pass; 0 fail; 0 timeout; no build | retain all five green leaves; no source owner |
| W117 | Bun util UUID/cookie/width/error leaves | 5 | 2/5 green; 288 passed / 54 failed / 360 ran / 1297 expects; no build | retain cookie + UUIDv5; park UUIDv7 validation/monotonicity, stringWidth ANSI/unicode, inspect-error source diagnostics as separate owners |
| W118 | Bun util encoding/file/error/path leaves | 5 | 5/5 green; 15 passed / 0 failed / 16 ran / 559 expects; no build | retain all five green leaves; no source owner |
| W119 | Node assert/buffer/diagnostics/encoding/http leaves | 5 | 4/5 pass; 1 fail; 0 timeout; no build | issue #61; retain four green leaves, park missing `assert.Assert` export/constructor as one owner |
| W120 | Bun util encoding/memory/promise/worker leaves | 5 | 5/5 green; 15 passed / 0 failed / 15 ran / 43 expects; no build | retain all five green leaves; no source owner |
| W121 | Node HTTP/FS/UDP/zlib leaves | 5 | 4/5 pass; 1 fail; 0 timeout; no build | retain four green leaves; park zlib weak-handle memory accounting after a bounded zero-delta reproduction |
| W122 | Bun util file/stream leaves | 5 | 4/5 green; 12 passed / 2 failed / 14 ran / 432 expects; no build | issue #62; retain four green leaves, park `readableStreamToArrayBuffer` intrinsic Promise plumbing |
| W123 | Node fs error/HTTP lifecycle leaves | 5 | 5/5 pass; 0 fail; 0 timeout; no build | retain all five green leaves; no source owner |
| W124 | Bun util object/string/file/GC/stdin leaves | 5 | 2/5 green; 11 passed / 6 failed / 17 ran / 250 expects; no build | retain stdin + error-GC; park internal helper exports, Bun.file async-stack/JSON message owners separately |
| W125 | Node HTTP/fs stream lifecycle leaves | 5 | 5/5 pass; 0 fail; 0 timeout; no build | retain all five green leaves; no source owner |
| W126 | Bun util + Node HTTP mixed leaves | 5 | corrected split-run: 4/5 files green; Bun 350 passed / 53 failed / 403 ran / 54616 expects, Node 2/2 pass; no build | retain indexOfLine, Bun.main, and two Node HTTP leaves; park CryptoHasher HMAC/unsupported-algorithm owners separately |
| W127 | Bun console + Node HTTP/net leaves | 5 | 4/5 valid files green; Bun 31 passed / 2 failed / 34 ran / 84 expects, Node 3/3 pass; no build | issue #63; retain console.write and all Node leaves, park console.table alignment policy |
| W128 | Node worker/message-port leaves | 5 | initial 4/5 pass + 1 timeout at 30s; isolated 1 job / 60s confirmation 5/5 pass, slow leaf 57.762s; no build | retain all five, mark MessagePort race as slow stress leaf and exclude it from the default 30s fast lane |
| W129 | Bun console iterator + Node net/domain leaves | 3 | 2/3 valid files green; Bun 0 passed / 17 failed / 17 ran, Node 2/2 pass; no build | issue #64; retain both Node leaves, park missing Bun console async iterator/input contract |
| W130 | Node REPL focused leaves | 3 | 1/3 pass; 1 fail; 1 timeout at 30s; no build; direct probe reproduced shared RegExp owner | issue #65; retain multiline navigation, park REPL/autolibs behind RegExp.$N static getter binding |
| W131 | Node VM/URL/WHATWG leaves | 5 | 5/5 pass; 0 fail; 0 timeout; no build | retain all five green leaves; no source owner |
| W132 | Node VM/WHATWG streams/URL leaves | 5 | 3/5 pass; 2 fail; 0 timeout; no build | issues #66/#67; retain VM ownpropertynames, URLSearchParams entries, WritableStream close; park VM readonly wording and TextDecoderStream invalid receivers |
| W133 | Bun.Terminal native leaves | 3 | 3/3 green; 129 passed / 0 failed / 130 ran / 334 expects; no build | retain all three terminal leaves; no source owner |
| W134 | Node HTTP low-coupling leaves | 5 | 5/5 pass; 0 fail; 0 timeout; no build | retain all five HTTP leaves; no source owner |
| W135 | Bun spawn/io low-coupling leaves | 5 | 3/5 green; 58 passed / 14 failed / 75 ran / 1391 expects; 0 timeout; no build | retain exit-code, empty stdin, kill-signal; park Bun.write and spawnSync multi-owner failures |
| W136 | Bun Web Encoding leaves | 5 | 4/5 green; 82 passed / 34 failed / 116 ran / 10777 expects; 0 timeout; no build | retain four encoding leaves; park CJK decoder behind missing legacy-label support |
| W137 | Node path pure-contract leaves | 5 | 5/5 pass; 0 fail; 0 timeout; no build | retain all five path leaves; no source owner |
| W138 | Node os pure-contract leaves | 5 | 5/5 pass; 0 fail; 0 timeout; no build | retain all five os leaves; no source owner |
| W139 | Bun Web Abort leaves | 3 | 3/3 green; 15 passed / 0 failed / 15 ran / 27 expects; 0 timeout; no build | retain all three abort leaves; no source owner |
| W140 | Bun Web timers basic leaves | 4 | 4/4 green; 12 passed / 0 failed / 12 ran / 58 expects; 0 timeout; no build | retain all four timer leaves; no source owner |
| W141 | Node events basic leaves | 5 | 3/5 pass; 2 fail; 0 timeout; no build | retain CustomEvent/list/listener-count; park AbortSignal max-listener default and events.once error-code owners |
| W142 | Node string_decoder leaves | 3 | 2/3 pass; 1 fail; 0 timeout; no build | retain end/fuzz; park StringDecoder.prototype.write invalid-this brand owner |
| W143 | Node timers basic leaves | 5 | 5/5 pass; 0 fail; 0 timeout; no build | retain all five timer leaves; no source owner |
| W144 | Bun Web console basic leaves | 4 | 2/4 green; 3 passed / 7 failed / 10 ran / 15 expects; 0 timeout; no build | retain UTF-16/recursive; park console.log and console.timeLog multi-owner formatting gaps |
| W145 | Node timers adjacent leaves | 5 | 4/5 pass; 1 fail; 0 timeout; no build | retain four leaves; park non-integer delay callback-order owner |
| W146 | Bun Web Request leaves | 3 | 2/3 green; 14 passed / 6 failed / 20 ran / 24 expects; 0 timeout; no build | retain request-subclass; retain clone-leak only as slow stress; park request-method heapStats NaN owner |
| W147 | Node path adjacent pure leaves | 3 | 3/3 pass; 0 fail; 0 timeout; no build | retain join/normalize/relative; no source owner |
| W148 | Node path namespace/glob leaves | 3 | 3/3 pass; 0 fail; 0 timeout; no build | retain glob and posix/win32 identity guards; no source owner |
| W149 | Bun Fetch/Web basic leaves | 4 | 4/4 green; 27 passed / 0 failed / 27 ran / 40 expects; 0 timeout; no build | retain all four Fetch/Web leaves; no source owner |
| W150 | Bun Blob focused leaves | 4 | 4/4 green; 27 passed / 0 failed / 27 ran / 62 expects; 0 timeout; no build | retain all four Blob leaves; no source owner |
| W151 | Node URL format/property leaves | 5 | 3/5 pass; 2 fail; 0 timeout; no build | retain fileURL/path and format leaves; park URL invalid-this and descriptor-enumerability owners |
| W152 | Bun Fetch body/cyclic leaves | 3 | 3/3 green; 6 passed / 0 failed / 6 ran / 8 expects; 0 timeout; no build | retain all three body/cyclic leaves; no source owner |
| W153 | Node URL utility leaves | 3 | 3/3 pass; 0 fail; 0 timeout; no build | retain pathToFileURL/revokeObjectURL/urlToHttpOptions; no source owner |
| W154 | Node URLSearchParams getter leaves | 5 | 5/5 pass; 0 fail; 0 timeout; no build | retain all five getter/iterator leaves; no source owner |
| W155 | Node querystring/URL query leaves | 5 | 5/5 pass; 0 fail; 0 timeout; no build | retain all five query leaves; no source owner |
| W156 | Node util low-coupling leaves | 5 | 5/5 pass; 0 fail; 0 timeout; no build | retain all five util leaves; no source owner |
| W157 | Node Buffer numeric read/write leaves | 5 | 5/5 pass; 0 fail; 0 timeout; no build | retain all five Buffer numeric leaves; no source owner |
| W158 | Node Buffer compare/copy leaves | 5 | 5/5 pass; 0 fail; 0 timeout; no build | retain all five Buffer compare/copy leaves; no source owner |
| W159 | Node assert deep-comparison leaves | 5 | 2/5 pass; 3 fail; 0 timeout; no build | retain assert-fail/if-error; park deep/partial/typed-array assertion owners separately |
| W160 | Bun Web URL/Response/clone leaves | 4 | 2/3 executable files green; 131 passed / 1 failed / 148 ran / 842 expects; 16 Linux-inapplicable skips; 0 timeout | retain URLSearchParams and structured-clone-fastpath; park Response FileRef snapshot root mismatch; exclude Windows URL skips |
| W161 | Bun WebStreams leak/fast-path leaves | 4 | 4/4 green; 15 passed / 0 failed / 15 ran / 23 expects; 0 timeout; no build | retain all four leaves; mark native-source-onclose as slow but bounded |
| W162 | Bun WebStreams compression/large surface | 3 | 1/3 green; 159 passed / 13 failed / 172 ran / 350 expects; 0 timeout; no build | retain compression 11/11; park streams-leak and streams.test multi-owner failures |
| W163 | Node fs pure-contract leaves | 5 | 4/5 pass; 1 fail; 0 timeout; no build | retain constants/mkdir/mkdtemp/open-flags; park fs.promises.access stack-shape owner |
| W164 | Node fs I/O pure-contract leaves | 5 | 5/5 pass; 0 fail; 0 timeout; no build | retain all five read/write leaves; no source owner |
| W165 | Node fs error/read leaves | 5 | 3/3 executable pass; 2 Linux-inapplicable skips; 0 fail; 0 timeout; no build | retain all three executable leaves; exclude two Windows-only skips |
| W166 | Bun util/file low-coupling leaves | 4 | 3/4 green; 28 passed / 4 failed / 32 ran / 73 expects; 0 timeout; no build | retain fileUrl/bun-file-read/concat; reconfirm Bun.file async-stack and JSON-message owners |
| W167 | Node fs delete/error leaves | 5 | 5/5 pass; 0 fail; 0 timeout; no build | retain all five delete/path-error leaves; no source owner |
| W168 | Bun util error/ANSI leaves | 4 | 2/4 green; 50 passed / 203 failed / 253 ran / 261 expects; 0 timeout; no build | retain error-code-mirror/exotic-global; park reportError printer and wrapAnsi multi-owner failures |
| W169 | Node fs/promises basic leaves | 5 | 4/5 pass; 1 fail; 0 timeout; no build | retain four green leaves; park readfile zero-byte-liar child-fixture callback owner |
| W170 | Node fs vector/copy/truncate leaves | 5 | 5/5 pass; 0 fail; 0 timeout; no build | retain all five vector/copy/truncate leaves; no source owner |
| W171 | Node fs directory/stat leaves | 5 | 4/4 executable pass; 1 MacOS-inapplicable skip; 0 fail; 0 timeout; no build | retain all four executable leaves; exclude MacOS-only readdir buffer skip |
| W172 | Bun FileSink/ArrayBufferSink/file-exists leaves | 3 | 3/3 green; 53 passed / 0 failed / 53 ran / 1728 expects; 0 timeout; no build | retain all three I/O leaves; no source owner |
| W173 | Node stream basic leaves | 5 | 5/5 pass; 0 fail; 0 timeout; no build | retain all five Duplex/Readable/Writable leaves; no source owner |
| W174 | Node stream error/end leaves | 5 | 5/5 pass; 0 fail; 0 timeout; no build | retain all five invalid-chunk/finished/end leaves; no source owner |
| W175 | Node stream state/event leaves | 5 | 5/5 pass; 0 fail; 0 timeout; no build | retain all five Readable/Writable state leaves; no source owner |
| W176 | Node stream/Web strategy leaves | 5 | 5/5 pass; 0 fail; 0 timeout; no build | retain all five Web termination/strategy/HWM leaves; no source owner |
| W177 | Node stream encoding/buffer leaves | 5 | 4/5 pass; 1 fail; 0 timeout; no build | retain four green HWM/encoding/buffer leaves; park stream-wrap callback owner |
| W178 | Node stream iterator leaves | 5 | 4/5 pass; 1 fail; 0 timeout; no build | retain four iterator leaves; park Buffer/Uint8Array readable-interop owner |
| W179 | Node stream pipeline leaves | 5 | 5/5 pass; 0 fail; 0 timeout; no build | retain all five pipeline leaves; no source owner |
| W180 | Node stream advanced leaves | 5 | 4/5 pass; 1 fail; 0 timeout; no build | retain compose/consumers/duplexpair/promises; park finished callback owner |
| W181 | Node Readable/Web BYOB leaves | 4 | 4/4 pass; 0 fail; 0 timeout; no build | retain all four BYOB/Web bridge leaves; no source owner |
| W182 | Node stream pipe leaves | 5 | 5/5 pass; 0 fail; 0 timeout; no build | retain all five pipe cleanup/event/flow leaves; no source owner |
| W183 | Node Transform leaves | 5 | 5/5 pass; 0 fail; 0 timeout; no build | retain all five Transform callback/final/object/HWM leaves; no source owner |
| W184 | Node Writable leaves | 5 | 5/5 pass; 0 fail; 0 timeout; no build | retain all five constructor/final/destroy/write leaves; no source owner |
| W185 | Node Writable adjacent leaves | 5 | 5/5 pass; 0 fail; 0 timeout; no build | retain all five encoding/end/state/callback leaves; no source owner |
| W186 | Node Writable final/error leaves | 5 | 5/5 pass; 0 fail; 0 timeout; no build | retain all five abort/final/error/writev leaves; no source owner |
| W187 | Node Readable basic leaves | 5 | 5/5 pass; 0 fail; 0 timeout; no build | retain all five constructor/data/encoding/readable-event leaves; no source owner |
| W188 | Node Readable event/end leaves | 5 | 5/5 pass; 0 fail; 0 timeout; no build | retain all five end/error/event/flow leaves; no source owner |
| W189 | Node Readable readiness leaves | 5 | 5/5 pass; 4 fresh green + 1 prior guard reconfirmed; 0 fail; 0 timeout; no build | retain four newly measured leaves; keep no-unneeded-readable as a reconfirmed W115 guard |
| W190 | Bun parser/API leaves | 5 | 5/5 green; 719/719 tests; 0 failed; 0 timeout; 5324 expects; no build | retain all five cron/INI/JSON5/JSONC/JSONL leaves; no source owner |
| W191 | Bun util low-coupling leaves | 5 | 5/5 green; 105 passed / 0 failed / 106 ran / 1 skipped / 409 expects; 0 timeout; no build | retain all five password/hash/error/sleep/path leaves; no source owner |
| W192 | Bun parser/cron adjacent leaves | 5 | 3/5 green; 464 passed / 78 failed / 578 ran / 755 expects; 0 timeout; no build | retain TLS-segment-size and JSON5/JSONC suites; park cron scheduling plus cron alias/validation owners separately |
| W193 | Bun low-coupling stream/source-map/system leaves | 5 | 3/5 green; 323 passed / 12 failed / 604 ran / 2302 expects; 0 timeout; no build | retain direct-readable, libuv error-name, histogram; park source-map path/UTF-8 and internal-source-map owners separately |
| W194 | Bun JSC/resolve/transpiler leaves | 5 | 3/5 green; 39 passed / 73 failed / 130 ran / 286 expects; 0 timeout; no build | retain native-constructor, string-noAtomize, bun-lock; park REPL transform and bytecode/type-export owners separately |
| W195 | Bun resolver import/meta leaves | 5 | 1/5 green; 42 passed / 28 failed / 70 ran / 82 expects; 0 timeout; no build | retain import-meta-resolve; park import.meta path, empty-module shape, and CJS __esModule owners separately |
| W196 | Node Readable readiness/encoding leaves | 5 | 5/5 pass; 0 fail; 0 timeout; no build | retain all five readable/resume/encoding leaves; no source owner |
| W197 | Node Readable boundary leaves | 5 | 5/5 pass; 0 fail; 0 timeout; no build | retain all five unshift/read/object/destroy leaves; no source owner |
| W198 | Node Duplex leaves | 5 | 5/5 pass; 0 fail; 0 timeout; no build | retain all five Duplex from/props/readable-writable/end leaves; no source owner |
| W199 | Node Duplex/destroy/finalization leaves | 5 | 5/5 pass; 0 fail; 0 timeout; no build | retain all five Duplex/destroy/finished leaves; no source owner |
| W200 | Node pipe/backpressure leaves | 5 | 5/5 pass; 0 fail; 0 timeout; no build | retain all five after-end/drain/cleanup leaves; no source owner |
| W201 | Node pipe continuation leaves | 5 | 5/5 pass; 0 fail; 0 timeout; no build | retain all five deadlock/resume/drain/object/listener leaves; no source owner |
| W202 | Node pipeline/finished leaves | 5 | 3/5 pass; 2 fail; 0 timeout; no build | retain queued-end-destroy and uncaught; park child-command pipeline and AsyncContextFrame owners |
| W203 | Node pipe error/flow leaves | 5 | 5/5 pass; 0 fail; 0 timeout; no build | retain error-handling/error-unhandled/flow-after-unpipe; treat flow/multiple-pipes as W182-adjacent reconfirmation until historical file mapping is rechecked |
| W204 | Node pipeline/pipe cleanup leaves | 5 | 5/5 pass; 0 fail; 0 timeout; no build | retain async-iterator/duplex/listeners/empty-string pipeline and pipe-cleanup leaves; no source owner |
| W205 | Node stream state/lifecycle leaves | 5 | 5/5 pass; 0 fail; 0 timeout; no build | retain asyncDispose, readableListening, setEncoding(null), unpipe-resume, and Writable ending-state leaves; no source owner |
| W206 | Bun Web/Atomics/URLPattern leaves | 5 | 4/5 green; 447 passed / 12 failed / 459 ran / 6375 expects; 0 timeout; no build | retain explicit-resource-management, nationalized, SHA-3, and Atomics; park URLPattern parser/URL-base/Unicode owners |
| W207 | Node diagnostics_channel leaves | 5 | 5/5 pass; 0 fail; 0 timeout; no build | retain has-subscribers, object/channel pub-sub, symbol channel, and sync-unsubscribe leaves; no source owner |
| W208 | Node events lifecycle leaves | 4 | 2/4 pass; 2 fail; 0 timeout; no build | retain addAbortListener and static getEventListeners; park async-iterator invalid-argument code and uncaught-exception stack-shape owners |
| W209 | Bun Node Buffer/DOM/crypto leaves | 5 | 5/5 green; 17 passed / 0 failed / 17 ran / 74 expects; 0 timeout; no build | retain Buffer Symbol.toPrimitive/resolveObjectURL, DOMException, crypto invalid-this, and HKDF leaves; no source owner |
| W210 | Node dgram UDP lifecycle leaves | 5 | 5/5 pass; 0 fail; 0 timeout; no build | retain address, asyncDispose, default bind address, AbortSignal close, and bytes-length leaves; no source owner |
| W211 | Bun Node crypto leaves | 5 | 5/5 green; 72 passed / 0 failed / 72 ran / 383 expects; 0 timeout; no build | retain LazyHash, one-shot hash/verify, RSA sign variants, X509 subclass, and random API leaves; no source owner |
| W212 | Node DNS contract leaves | 5 | 3/5 pass; 2 fail; 0 timeout; no build | retain getServer, lookup option validation, setServers type checks; park dns/promises ENODATA constant and maxTimeout error-shape owners |
| W213 | Bun VM/TLS/zlib leaves | 3 | 2/3 green; 5 passed / 3 failed / 8 ran / 262 expects; 0 timeout; no build | retain vm-sourceURL and Node TLS internals; park zlib native handle `write` exposure owner; no source/fixture change |
| W214 | Bun process/TLS/HTTP leaves | 3 | 1/3 green; 4 passed / 2 failed / 6 ran / 6 expects; 0 timeout; no build | retain process stdio stack-limit guard; park TLS `allowHalfOpen` propagation and HTTP internal-handle bootstrap owners |
| W215 | Bun TLS leaves | 3 | 3/3 green; 7 passed / 0 failed / 7 ran / 33 expects; 0 timeout; no build | retain rootCertificates immutability, no-cipher-match error shape, and createSecureContext argument validation; no source owner |
| W216 | Bun VM leak/integration leaves | 3 | 2/3 green; 5 passed / 1 failed / 6 ran / 1 expect; 0 timeout; no build | retain vm-script-fetcher and vm.Script leak guards; park happy-dom DOM integration owner |
| W217 | Node assert owner-split leaves | 3 | 0/3 pass; 3 fail; 0 timeout; no build | park assert.Assert constructor, Error cause deep-equality message/stack, and TypedArray/ArrayBuffer deepEqual semantics as separate owners |
| W218 | Node console plain-script leaves | 3 | 3/3 pass; 0 fail; 0 timeout; no build | retain console replacement/recovery, primitive throw output, and inspect-toString guards; no source owner |
| W219 | Node Buffer plain-script leaves | 3 | 3/3 pass; 0 fail; 0 timeout; no build | retain isUtf8 validation, byteLength encoding/type boundaries, and compare offset/range guards; no source owner |
| W220 | Bun stack/stdio/HTTP leaves | 3 | 21 passed / 29 failed / 50 ran / 114 expects; 0 runner timeout; no build | retain HTTP proxy-style normal paths; park CR/LF host validation, stdio write-after-end pipe/file state, and stack/frame/internal-hook/lazy-error owners |
| W221 | Node Buffer continuation leaves | 3 | 3/3 pass; 0 fail; 0 timeout; no build | retain bad-hex handling, BigInt64/BigUInt64 endian/range, and ArrayBuffer sharing/offset/length guards; no source owner |
| W222 | Node Buffer numeric leaves | 3 | 3/3 pass; 0 fail; 0 timeout; no build | retain signed/unsigned reads and signed writes across OOB/type/range/endianness guards; no source owner |
| W223 | Node Buffer float leaves | 3 | 3/3 pass; 0 fail; 0 timeout; no build | retain Float32/Float64 BE/LE read/write and OOB/range guards; no source owner |
| W224 | Node Buffer write leaves | 3 | 3/3 pass; 0 fail; 0 timeout; no build | retain generic encoding/range, Double BE/LE, and UInt BE/LE write guards; no source owner |
| W225 | Node Buffer read/string leaves | 3 | 3/3 pass; 0 fail; 0 timeout; no build | retain basic reads, toString range/coercion, and JSON serialization guards; no source owner |
| W226 | Node crypto leaves | 3 | 3/3 pass; 0 fail; 0 timeout; no build | retain cipher encoding validation, getCipherInfo lookup/type/range, and RSA-OAEP empty-payload guards; no source owner |
| W227 | Node crypto leaves | 3 | 2/3 pass; 1 fail; 0 timeout; no build | retain HKDF and KeyObject brand-check leaves; park randomFill offset/size type validation owner |
| W228 | Node crypto/WebCrypto leaves | 3 | 3/3 pass; 0 fail; 0 timeout; no build | retain KeyObject own-key, AES-GCM empty-payload, and short-tag rejection guards; no source owner |
| W229 | Bun Node URL/path leaves | 3 | 3/3 green; 4 passed / 0 failed / 5 ran / 2 expects; 0 timeout; no build | retain path parse/format and zero-length guards plus legacy URL query-object prototype guard; one URL TODO remains; no source owner |
| W230 | Bun Node path/URL leaves | 3 | 3/3 green; 8 passed / 0 failed / 8 ran / 0 expects; 0 timeout; no build | retain basename/extname platform cases and WHATWG URL format; no source owner |
| W231 | Bun Node util/events/string_decoder leaves | 3 | 3/3 green; 214 passed / 0 failed / 214 ran / 6538 expects; 0 timeout; no build | retain EventEmitter, StringDecoder, and util.types green cluster; no source owner |
| W232 | Node string/events/URL leaves | 3 | 1/3 pass; 2 fail; 0 timeout; no build | retain URL query parsing; park events.once invalid-option error code and StringDecoder forged-receiver ERR_INVALID_THIS owners |
| W233 | Node path/querystring/URL leaves | 3 | 3/3 pass; 0 fail; 0 timeout; no build | retain path parse/format, querystring, and legacy URL parse/format leaves; no source owner |
| W234 | Bun Node util leaves | 3 | 2/3 green; 297 passed / 2 failed / 300 ran / 559 expects; 0 timeout; no build | retain promisify/callbackify; park util.styleText ANSI colorization under runner color policy |
| W235 | Node timers leaves | 3 | 3/3 pass; 0 fail; 0 timeout; no build | retain zero-timeout arguments, clearImmediate cancellation, and timer callback receiver/argument guards; no source owner |
| W236 | Bun timers leaves | 3 | 1 counted file; 18 passed / 2 failed / 20 ran / 31 expects; 0 runner timeout; 2 no-tests excluded; no build | park UTF-16 timer-id classification and immediate-exception fixture subprocess owners |
| W237 | Bun Node stream leaves | 2 | 1/2 green; 92 passed / 6 failed / 104 ran / 165 expects; 0 runner timeout; no build | retain Uint8Array stream guards; park stdin subprocess, Web/Node cancellation reasons, Bun.serve direct sink, and gated resolve.paths owners |
| W238 | Node stream lifecycle leaves | 3 | 3/3 pass; 0 fail; 0 timeout; no build | retain Readable Web termination, Writable cork-buffer accounting, and Duplex end/half-open guards; no source owner |
| W239 | Node stream pipeline/state leaves | 3 | 3/3 pass; 0 fail; 0 timeout; no build | retain pipeline listener cleanup/uncaught delivery, Writable needDrain, and writableCorked transitions; no source owner |
| W240 | Node stream state/destroy leaves | 2 | 2/2 pass; 0 fail; 0 timeout; no build | retain Readable pause/resume/backpressure and Writable destroy/error/custom-destroy lifecycle guards; no source owner |
| W241 | Bun Node HTTP leaf probe | 3 | 2/3 green; 7 passed / 1 failed / 8 ran / 13 expects; 0 timeout; no build | retain maxHeaderSize and HTTP primordials; park proxy-agent CR/LF host validation owner |
| W242 | Bun spawn/mock leaves | 3 | 3/3 green; 11 passed / 0 failed / 11 ran / 45 expects; 0 timeout; no build | retain spoofed spawn-array length, disposable mock restore, and mock.module validation/resolver short-circuit guards; no source owner |
| W243 | Bun Web Fetch/Response leaves | 3 | 3/3 green; 86 passed / 0 failed / 86 ran / 192 expects; 0 timeout; no build | retain Response constructor/redirect/clone, body-used errors, and fetch option-conversion/no-send guards; no source owner |
| W244 | Bun test matcher leaves | 3 | 3/3 green; 45 passed / 0 failed / 45 ran / 92 expects; 0 timeout; no build | retain expect labels, expect.assertions failure accounting, and toHaveReturnedWith/toHaveLastReturnedWith guards; no source owner |
| W245 | Node console/process plain-script leaves | 3 | 2/3 pass; 1 fail; 0 timeout; no build | retain console.count and process.uptime; park Console group multiline-object pretty-print/indentation owner |
| W246 | Node console/util plain-script leaves | 3 | 3/3 pass; 0 fail; 0 timeout; no build | retain Console method-constructor guards, stdio setter routing, and util.inherits chains; no source owner |
| W247 | Node util.deprecate/inspect plain-script leaves | 3 | 3/3 pass; 0 fail; 0 timeout; no build | retain deprecate code validation/one-time warning behavior and inspect primordial isolation; no source owner |
| W248 | Node console assignment/error plain-script leaves | 3 | 3/3 pass; 0 fail; 0 timeout; no build | retain primitive console replacement/self-assignment and Console primitive-write failure guards; no source owner |
| W249 | Node process identity/memory plain-script leaves | 3 | 3/3 pass; 0 fail; 0 timeout; no build | retain process.ppid child relation, process.release LTS/version contract, and availableMemory numeric guard; no source owner |
| W250 | Node process queue/mask/CPU plain-script leaves | 3 | 3/3 pass; 0 fail; 0 timeout; no build | retain nextTick uncaught propagation, umask mask coercion, and cpuUsage result/argument guards; no source owner |
| W251 | Node process exec/argv/umask plain-script leaves | 3 | 3/3 pass; 0 fail; 0 timeout; no build | retain symlink execPath, child argv[0], and full umask read/write/error guards; no source owner |
| W252 | Bun base64/highlighter/UUID leaves | 3 | 2/3 green; 28 passed / 11 failed / 39 ran / 575 expects; 0 timeout; no build | retain base64url 5/5 and highlighter 16/16; park randomUUIDv7 timestamp validation, rollover/order, and counter-seeding owners |
| W253 | Node events/listener plain-script leaves | 3 | 3/3 pass; 0 fail; 0 timeout; no build | retain EventEmitter eventNames/listenerCount semantics and EventSource-disabled global guard; no source owner |
| W254 | Bun test-runner hook/scope leaves | 3 | 2/3 green; 15 passed / 11 failed / 26 ran / 21 expects; 0 timeout; no build | retain nested-describes 3/3 and onTestFinished 12/12; park failure-skip nested child-runner empty-output owner |
| W255 | Bun retry/jest-each/fake-timers leaves | 3 | 2/3 green; 40 passed / 4 failed / 44 ran / 61 expects; 0 timeout; no build | retain jest-each 25/25 and retry/repeats 12/12; park fake-timers Intl clock-format and child-eval `jest.useFakeTimers` owners |
| W256 | Node fs/http/url plain-script leaves | 3 | 3/3 pass; 0 fail; 0 timeout; no build | retain TypedArray `fs.promises.writeFile`, HTTP header name/value validation, and invalid `file:` URL path guards; no source owner |
| W257 | Bun mock.module/re-export leaves | 3 | 2/3 green; 6 passed / 6 failed / 13 ran / 32 expects; 0 timeout; no build | retain re-export mocks 2/2 and non-existent-specifier 1/1; split mock-module async, restore identity, relative-file, and cache/update owners |
| W258 | Node child-process stdio/destroy leaves | 3 | 3/3 pass; 0 fail; 0 timeout; no build | retain stdio inherit/flush and child destroy state guards; no source owner |
| W259 | Node child-process IPC/exec leaves | 3 | 2/3 pass; 1 fail; 0 timeout; no build | retain disconnect async/self-termination and exec encoding; park IPC server-handle transfer owner |
| W260 | Bun hooks/custom matcher/mock-fn leaves | 3 | 2/3 green; 78 passed / 34 failed / 113 ran / 20472 expects; 0 timeout; no build | retain expect-extend 28/28 and jest-hooks 17 pass + 1 todo; park mock-fn metadata/this, call bookkeeping, missing APIs, reset/restore, and spyOn owners |
| W261 | Node Buffer iterator/read/allocation leaves | 3 | 3/3 pass; 0 fail; 0 timeout; no build | retain Buffer iterator variants, read boundary/error guards, and negative allocation validation; no source owner |
| W262 | Node crypto Certificate/DH/keygen leaves | 3 | 3/3 pass; 0 fail; 0 timeout; no build | retain Certificate fixture parsing/API, `modp2` Diffie-Hellman group, and empty-passphrase keygen no-prompt guards; no source owner |
| W263 | Node stream append/backpressure/order leaves | 3 | 3/3 pass; 0 fail; 0 timeout; no build | retain Readable data-time append, backpressure completion, and push ordering; no source owner |
| W264 | Bun spyMatchers/pretty-format/test.failing leaves | 3 | 1/3 green; 130 passed / 24 failed / 159 ran / 494 expects; 0 timeout; no build | retain pretty-format 1/1; retain spyMatchers 124 pass + 5 todo; park matcher error/argument semantics and test.failing message/timeout owners |
| W265 | Node fs append/rename/stream-type leaves | 3 | 3/3 pass; 0 fail; 0 timeout; no build | retain appendFileSync data/mode/FD behavior, rename type guards, and WriteStream option TypeErrors; no source owner |
| W266 | Node HTTP framing/status/listening leaves | 3 | 3/3 pass; 0 fail; 0 timeout; no build | retain no-content-length framing, statusMessage behavior, and server listening transitions; no source owner |
| W267 | Node DNS promises/error-shape leaves | 3 | 1/3 pass; 2 fail; 0 timeout; no build | retain resolve-promises; park dns/promises `NODATA` export and memory-error stack-shape owners |
| W268 | Node DNS lookup/order/type leaves | 3 | 3/3 pass; 0 fail; 0 timeout; no build | retain lookup promise stub, result-order controls, and resolveNs type guards; no source owner |
| W269 | Node DNS lookup/resolver leaves | 3 | 2/3 pass; 1 fail; 0 timeout; no build | retain lookupService and malformed resolveAny guards; park invalid-hostname/all-mode sync validation owner |
| W270 | Bun module/Buffer/DOMException leaves | 3 | 2/3 green; 8 passed / 1 failed / 9 ran / 44 expects; 0 timeout; no build | retain Buffer and DOMException leaves; park `node:missing` built-in error contract |
| W271 | Bun Buffer/process/module leaves | 3 | 2/3 green; 9 passed / 5 failed / 14 ran / 21 expects; 0 timeout; no build | retain UTF-16 Buffer and Module options.paths; park process.nextTick input/args/order/repeat owners |
| W272 | Node querystring pure-contract leaves | 3 | 3/3 pass; 0 fail; 0 timeout; no build | retain escape coercion/URI errors, multi-character separators, and non-finite maxKeys behavior; no source owner |
| W273 | Bun Buffer safety/bounds leaves | 3 | 2/3 green; 47 passed / 2 failed / 49 ran / 82 expects; 0 timeout; no build | retain indexOf detach and compare bounds; park Buffer.fill string-branch encoding coercion owner |
| W274 | Node module/constants plain-script leaves | 3 | 2/3 pass; 1 fail; 0 timeout; no build | retain `module.isBuiltin` and `builtinModules`; park internal/public constants mapping owner |
| W275 | Node module/punycode plain-script leaves | 3 | 2/3 pass; 1 fail; 0 timeout; no build | retain createRequire and invalid-module loading guards; park punycode invalid-input message owner |
| W276 | Bun crypto HMAC/PBKDF2/ECDH leaves | 3 | 3/3 green; 126 passed / 0 failed / 126 ran / 226 expects; 0 timeout; no build | retain RFC/vector HMAC, PBKDF2 validation/derivation, and ECDH conversion/secret guards; no source owner |
| W277 | Node module cache/lookup-path leaves | 3 | 3/3 pass; 0 fail; 0 timeout; no build | retain module cache, relative lookup, and node_modules path contract leaves; no source owner |
| W278 | Node module createRequire/cache/prototype leaves | 3 | 3/3 pass; 0 fail; 0 timeout; no build | retain multibyte createRequire, cache injection, and prototype-safety leaves; no source owner |
| W279 | Bun crypto invalid-this/lazyhash/HKDF leaves | 3 | 3/3 green; 8 passed / 0 failed / 8 ran / 24 expects; 0 timeout; no build | retain invalid-this safety, lazy hash inheritance, and HKDF callback/KeyObject guards; no source owner |
| W280 | Node module warning/source-map API leaves | 3 | 3/3 pass; 0 fail; 0 timeout; no build | retain deprecation-warning and `Module.setSourceMapsSupport` argument-contract leaves; no source owner |
| W281 | Bun crypto KeyObject/RSA/X509 leaves | 3 | 2/3 green; 118 passed / 1 failed / 142 ran / 1016 expects; 0 timeout; no build | retain KeyObject and X509 green clusters; park RSA PKCS#1 private-decrypt rejection owner |
| W282 | Node module wrap/wrapper/deprecation leaves | 3 | 3/3 pass; 0 fail; 0 timeout; no build | retain CJS wrapper child-process probes and `module.parent` setter deprecation guard; no source owner |
| W283 | Node module entry/global-path leaves | 3 | 2/3 pass; 1 fail; 0 timeout; no build | retain NODE_PATH and main-extension leaves; park copied-child HOME/global-path resolution owner |
| W284 | Bun HTTP timeout/cork/TLS leaves | 3 | 3/3 green; 16 passed / 0 failed / 16 ran / 46 expects; 0 timeout; no build | retain timeout lifecycle, nested-cork isolation, and TLS identity guards; no source owner |
| W285 | Node circular-loader/require-error leaves | 5 | 4/5 pass; 1 fail; 0 timeout; no build | retain circular warning/symlink, invalid-package, and Unicode-path leaves; park JSON parse filename diagnostic owner |
| W286 | Node require boundary leaves | 5 | 5/5 pass; 0 fail; 0 timeout; no build | retain node-prefix/cache, NUL, exception-reload, empty-main, and deleted-directory resolution guards; no source owner |
| W287 | Bun util inspect/fs metadata leaves | 5 | 4/5 green; 61 passed / 1 failed / 62 ran / 130 expects; 0 timeout; no build | retain Bun/custom inspect, birthtime, and cp symlink guards; park proxy inspect trap owner |
| W288 | Node resolver/require flag leaves | 5 | 5/5 pass; 0 fail; 0 timeout; no build | retain import resilience, dot resolution, guarded ESM require, process identity, and invalid resolve-path validation; no source owner |
| W289 | Node resolver/extension/symlink leaves | 5 | 3/5 pass; 2 fail; 0 timeout; no build | retain symlinked-peer, invalid-main, and relative-path guards; park extension-over-directory and require.resolve fixture lookup owners |
| W290 | Node module metadata/extension leaves | 5 | 4/5 pass; 1 fail; 0 timeout; no build | retain module children/stat/version and extension-main guards; merge same-filename-as-dir into the W289 extension-over-directory precedence owner |
| W291 | Bun Base64/Buffer/console/encoding leaves | 5 | 5/5 green; 38 passed / 0 failed / 40 ran / 118 expects; 0 timeout; no build | retain all five Bun guards; no source owner |
| W292 | Bun console/performance/HTTP leaf probe | 5 | 4/5 green; 30 passed / 4 failed / 34 ran / 66 expects; 0 runner timeout; no build | retain four green guards; park malformed HTTP trailer validation/liveness owner |
| W293 | Node loader/symlink continuation leaves | 5 | 3/5 pass; 2 fail; 0 timeout; no build | retain entry-point and trailing-slash guards; split custom multi-extension selection from preserve-symlinks cache identity |
| W294 | Node path/os/url pure-contract leaves | 5 | 5/5 pass; 0 fail; 0 timeout; no build | retain all five path/os/url guards; no source owner |
| W295 | Node path/query/url/events leaves | 5 | 4/5 pass; 1 fail; 0 timeout; no build | retain path identity, querystring, URL invalid-input, and CustomEvent guards; park events.on invalid-argument error-code shape |
| W296 | Bun fetch/encoding/timer leaf probe | 5 | 5/5 green; 70 passed / 0 failed / 70 ran / 85 expects; 0 timeout; no build | retain all five Bun guards; no source owner |
| W297 | Bun fetch/blob/timer ownership probe | 5 | 3/5 green; 24 passed / 1 failed / 25 ran / 54 expects; 1 runner timeout; no build | retain three Blob guards; park fetch-gzip timeout and setInterval cancellation owners |
| W298 | Node path/os continuation leaves | 5 | 5/5 pass; 0 fail; 0 timeout; no build | retain all five path/os guards; no source owner |
| W299 | Bun Blob/stream/event/body leaf probe | 5 | 5/5 green; 47 passed / 0 failed / 50 ran / 119 expects; 0 timeout; no build | retain all five Bun/Deno guards; no source owner |
| W300 | Node util/styleText/os/url leaves | 5 | 4/5 pass; 1 skip; 0 fail; 0 timeout; no build | retain util sleep, hex styleText, URL deprecation, and userinfo guards; record regular styleText as a TTY harness skip |
| W301 | Node util/VM/encoding continuation leaves | 5 | 2/5 pass; 3 fail; 0 timeout; no build | retain signal exit-code and TextDecoder guards; split VM namespace inspect, internal symbol enumerability, and promisify custom-name owners |
| W302 | Node URL continuation leaves | 5 | 5/5 pass; 0 fail; 0 timeout; no build | retain all five URL parse/format/brand guards; no source owner |
| W303 | Bun/Deno Fetch/URL API leaves | 5 | 5/5 green; 80 passed / 0 failed / 85 ran / 245 expects; 0 timeout; no build | retain all five Fetch/URL guards; no source owner |
| W304 | Node events/path continuation leaves | 5 | 4/5 pass; 1 fail; 0 timeout; no build | retain AbortListener, getEventListeners, relative, and zero-length path guards; park uncaughtException stack rendering owner |
| W305 | Bun/Deno Event/Performance/URL/crypto leaves | 5 | 4/5 green; 63 passed / 0 failed / 68 ran / 287 expects; 1 all-skipped; 0 timeout; no build | retain four green guards; record Deno V8 error file as an all-skipped corpus entry |
| W306 | Bun/Deno abort/encoding/Event/Fetch body leaves | 5 | 5/5 green; 46 passed / 0 failed / 51 ran / 118 expects; 5 skipped; 0 timeout; no build | retain all five files; keep encoding and Fetch body skips as bounded capability gaps |
| W307 | Bun globals/archive/console/crypto leaf probe | 5 | 2/5 green; 72 passed / 118 failed / 191 ran / 212 expects; 1 skipped; 0 runner timeout; no build | retain inspect-table and cipheriv; park Archive API, console iterator, and split globals owners |
| W308 | Node console/path leaf probe | 5 | 4/5 pass; 1 fail; 0 timeout; no build | retain console-clear, console-instance, path-isabsolute, and path-join; park diagnostics-channel callback delivery |
| W309 | Node console/process contract leaves | 5 | 4/5 pass; 1 skipped; 0 fail; 0 timeout; no build | retain four console/process guards; record process-config as a Linux environment skip |
| W310 | Node process/console builtin leaves | 5 | 4/5 pass; 1 fail; 0 timeout; no build | retain four guards; park process.getBuiltinModule node:test identity owner |
| W311 | Bun util low-coupling green cluster | 5 | 5/5 green; 12 passed / 0 failed / 12 ran / 438 expects; 0 timeout; no build | retain all five Bun util guards; no source owner |
| W312 | Bun util error/file/unsafe/report/fuzzy leaves | 5 | 3/5 green; 9 passed / 155 failed / 164 ran / 30 expects; 0 timeout; no build | retain error-name, file-type, unsafe; park Promise.resolve intrinsic and split reportError owners |
| W313 | Node console/process/path guard leaves | 5 | 4/5 pass; 1 fail; 0 timeout; no build | retain four process/path guards; park console.dir revoked-Proxy inspection owner |
| W314 | Bun util mmap/hash/CSRF/file/concat leaves | 5 | 4/5 green; 50 passed / 8 failed / 58 ran / 200 expects; 0 timeout; no build | retain four green util guards; park missing Bun.mmap API owner |
| W315 | Node process/console lifecycle green cluster | 5 | 5/5 pass; 0 fail; 0 timeout; no build | retain all five process/console lifecycle guards; no source owner |
| W316 | Node process.env contract leaves | 5 | 3/5 pass; 1 fail; 1 skipped; 0 timeout; no build | retain three env guards; record inspector skip and split load-env-file cwd/diagnostic owners |
| W317 | Node console/stdio/finalization green cluster | 5 | 5/5 pass; 0 fail; 0 timeout; no build | retain all five console/stdio/finalization guards; no source owner |
| W318 | Node process identity/active-handle and console color leaves | 5 | 5/5 pass; 0 fail; 0 timeout; no build; 4 fresh + 1 revalidation | retain four fresh guards; revalidate `test-process-execve-validation.js` already recorded in W113 |
| W319 | Node active-resources/priority/console leaves | 5 | 5/5 pass; 0 fail; 0 timeout; no build | retain all five resource/priority/console guards; no source owner |
| W320 | Bun util object/encoding/timer/path leaves | 5 | 4/5 green; 20 passed / 1 failed / 21 ran / 236 expects; 0 timeout; no build; 1 fresh + 4 revalidations | retain fresh `sleepSync`; revalidate four historical Bun guards and retain the known helper owner |
| W321 | Node warning/identity contract leaves | 5 | 5/5 pass; 0 fail; 0 timeout; no build | retain all five warning/identity guards; no source owner |
| W322 | Node active-resource lifetime/signal/title leaves | 5 | 5/5 pass; 0 fail; 0 timeout; no build; 5 fresh | retain all five active-resource/signal/title guards; no source owner |
| W323 | Bun FileSink/loader/path/ANSI leaves | 5 | 4/5 green; 60 passed / 20 failed / 81 ran / 1351 expects; 0 timeout; 2 fresh + 3 revalidations | retain fresh `bun-file-windows` and `text-loader`; revalidate W168/W172/W191 owners |
| W324 | Node process metadata/warning/SourceMap leaves | 5 | 4/5 pass; 1 fail; 0 timeout; no build; 5 fresh | retain four warning/SourceMap/resource guards; park process.config metadata-shape owner |
| W325 | Node eval/global/instanceof/WHATWG URL leaves | 5 | 5/5 pass; 0 fail; 0 timeout; no build; 5 fresh | retain all five pure-contract guards; no source owner |
| W326 | Node fs/fs.promises contract leaves | 5 | 5/5 pass; 0 fail; 0 timeout; no build; 5 fresh | retain all five fs/fs.promises guards; no source owner |
| W327 | Node timer clear/refresh/tampering leaves | 5 | 5/5 pass; 0 fail; 0 timeout; no build; 5 fresh | retain all five timer guards; no source owner |
| W328 | Node Buffer deprecation/encoding/zero-fill leaves | 5 | 5/5 pass; 0 fail; 0 timeout; no build; 5 fresh | retain all five Buffer guards; no source owner |
| W329 | Node HTTP/HTTPS/MessageEvent/WebCrypto/console leaves | 5 | 3/5 pass; 2 fail; 0 timeout; no build; 5 fresh | retain HTTP/HTTPS/MessageEvent; park WebCrypto class identity and global-console warning-order owners |
| W330 | Node perf_hooks/performance contract leaves | 5 | 4/5 pass; 1 fail; 0 timeout; no build; 5 fresh | retain timerify/global/measure guards; park resource-timing BigInt validation owner |
| W331 | Node perf_hooks/performance continuation leaves | 5 | 4/5 pass; 1 fail; 0 timeout; no build; 5 fresh | retain timerify and async-function guards; park nodeTiming milestone-order owner |
| W332 | Bun low-coupling Linux leaves | 5 | 2/5 green; 4 passed / 4 failed / 8 ran / 17 expects; 0 timeout; no build; 5 fresh | retain RuntimeError/data-URL module; park namespace pollution, no-addons diagnostic, and glibc symbol owners |
| W333 | Node error/internal-contract leaves | 5 | 1/5 pass; 4 fail; 0 timeout; no build; 5 fresh | retain bad-Unicode parser; park accessor brands, constants shape, stack limit, and SystemError dialect owners |
| W334 | Node process lifecycle/propagation leaves | 5 | 4/5 pass; 1 fail; 0 timeout; no build; 5 fresh | retain beforeExit throw, binding allowlist, execArgv, and uncaught-monitor guards; park beforeExit reentry owner |
| W335 | Node timer API leaves | 5 | 5/5 pass; 0 fail; 0 timeout; no build; 5 fresh | retain all five timer API guards; no source owner |
| W336 | Node assert/DNS validation leaves | 5 | 3/5 pass; 2 fail; 0 timeout; no build; 5 fresh | retain ESM/CJS, Myers, and DNS guards; park async-thenable and first-line assertion-message owners |
| W337 | Node HTTP contract leaves | 5 | 5/5 pass; 0 fail; 0 timeout; no build; 5 fresh | retain all five HTTP method/port/validation/header guards; no source owner |
| W338 | Node dgram local UDP contract leaves | 5 | 5/5 pass; 0 fail; 0 timeout; no build; 5 fresh | retain all five dgram close/type/send/address/empty-packet guards; no source owner |
| W339 | Bun small Linux regression leaves | 5 | 5/5 green; 8 passed / 0 failed / 8 ran / 32 expects; 0 timeout; no build; 5 fresh | retain all five regression guards; no source owner |
| W340 | Node dgram continuation leaves | 5 | 4/5 pass; 1 fail; 0 timeout; no build; 5 fresh | retain bind-error-repeat, connected-send, ref, and unref guards; park connected-port validation message owner |
| W341 | Node dgram error/options leaves | 5 | 4/5 pass; 1 fail; 0 timeout; no build; 5 fresh | retain send-error, callback-recursion, broadcast, and TTL guards; park socket-buffer-size error-rendering owner |
| W342 | Bun Linux regression/parser/filesystem leaves | 5 | 4/5 green; 11 passed / 3 failed / 14 ran / 48 expects; 0 timeout; no build; 5 fresh | retain module-extensions, WebSocket-cookie, Dirent, and console-format guards; park HTML-entrypoint parser/build owner |
| W343 | Node readline/TTY leaves | 5 | 4/5 pass; 1 fail; 0 timeout; no build; 5 fresh | retain CSI, keypress, stdin-end, and stdin-pipe guards; park TTY backwards-API forwarding owner |
| W344 | Bun Linux loader/build/network/REPL leaves | 5 | 5/5 green; 8 passed / 0 failed / 8 ran / 21 expects; 0 timeout; no build; 5 fresh | retain DCE syntax, tsconfig paths, deferred node import, CONNECT pipelining, and REPL startup guards; no source owner |
| W345 | Node readline continuation leaves | 5 | 4/5 pass; 1 fail; 0 timeout; no build; 5 fresh; TERM=xterm rerun | retain no-trailing-newline, recursive-write, raw-mode, and cursor-position guards; park Unicode line-separator owner |
| W346 | Node readline Unicode line-separator source fix | 5 | pre 4/5 pass + 1 fail; post target 1/1 pass and bounded regression 5/5 pass; 0 timeout; one release rebuild | add U+2028/U+2029 to `lineEnding`; retain all five readline guards; no upstream-fixture change |
| W347 | Node/Bun dgram port-message source fix | 5 Node + 1 Bun | pre Node 4/5 pass + 1 fail; post Node 5/5 pass; Bun 3/3 passed / 0 failed / 3 ran / 4 expects; 0 timeout; one release rebuild | change only `allowZero=false` wording from `>= 1` to `> 0`; retain dgram guards; no upstream-fixture change |

## W305 Bun/Deno Event/Performance/URL/crypto leaf probe

The bounded five-job Bun selector covered Deno Event, Performance, URL,
crypto-random, and V8 error leaves. It measured **4/5 green files**, **63
passed / 0 failed / 68 ran / 287 expects**, **1 all-skipped file**, and **0
runner timeouts**. Per-file durations were **250–400 ms**.

`random.test.ts` measured **10 passed / 0 failed / 10 ran / 11 expects**;
`event.test.ts` measured **8 / 0 / 10 / 32**;
`performance.test.ts` measured **13 / 0 / 13 / 44**; and
`url.test.ts` measured **32 / 0 / 33 / 200**. These four guards are retained
as green coverage. `v8/error.test.ts` ran two tests and both were skipped by
the corpus, with no failures; it is recorded as an environment/ability skip,
not a source owner. There were no source or upstream-fixture changes.

The selector used the bounded five-job Bun runner with a 30-second per-file
timeout, the existing coordinator binary, and missing Node modules allowed.
No full corpus, build, or workspace-wide test was run.

## W306 Bun/Deno abort/encoding/Event/Fetch body leaf probe

The bounded five-job Bun selector covered five previously unrecorded Deno
leaves: `abort-controller`, `encoding`, `custom-event`, `event-target`, and
Fetch `body`. It measured **5/5 green files**, **46 passed / 0 failed / 51
ran / 118 expects**, and **0 runner timeouts**. Five individual tests were
skipped: two in `encoding`, one in `event-target`, and two multipart cases in
Fetch `body`; no test failed.

Per-file results were: `abort-controller` **6 / 0 / 6 / 15** with no skips;
`encoding` **21 / 0 / 23 / 41** with 2 skips; `custom-event` **2 / 0 / 2 /
8** with no skips; `event-target` **14 / 0 / 15 / 43** with 1 skip; and Fetch
`body` **3 / 0 / 5 / 11** with 2 skips. Durations were **200–250 ms** per
file. The five files remain useful green coverage; the skipped cases are
recorded as capability/test-environment gaps rather than source failures.

This was a measurement-only wave: no source or upstream-fixture changes, no
full corpus, and no workspace-wide build. The existing coordinator binary
was used with a 30-second per-file timeout and missing Node modules allowed.
Resource recheck after the run showed about **44 GiB available memory**, only
about **32 MiB free swap**, and about **17 GiB free disk at 99% usage**; the
temporary runner output was therefore scheduled for immediate cleanup and no
build was started.

## W307 Bun globals/archive/console/crypto leaf probe

The bounded five-job Bun selector covered five previously unrecorded Bun
files: `globals`, `archive`, console iterator, `bun-inspect-table`, and
`cipheriv-decipheriv`. It measured **2/5 green files**, **72 passed / 118
failed / 191 ran / 212 expects**, **1 skipped**, and **0 runner timeouts**.
Durations were **199–2002 ms** per file.

The two retained green guards were `console/bun-inspect-table.test.ts` at
**35 passed / 0 failed / 35 ran / 35 expects** and
`crypto/cipheriv-decipheriv.test.ts` at **13 / 0 / 13 / 38**. The three red
files were separated by owner: `archive.test.ts` reached **8 passed / 97
failed / 106 ran / 12 expects** with one skipped case because
`Bun.Archive` is not available; `console-iterator.test.ts` reached **0 / 17 /
17 / 17**, with subprocess/stream output remaining empty; and
`globals.test.js` reached **16 / 4 / 20 / 110**, split between File invalid
argument/native TypeError behavior and `globalThis.gc` exposure/property-slot
semantics. No mixed fix was attempted.

This was a measurement-only wave: no source or upstream-fixture changes, no
full corpus, and no workspace-wide build. The existing coordinator binary was
used with five jobs, a 30-second per-file timeout, and missing Node modules
allowed. After the run, resources showed about **46 GiB available memory**,
about **332 KiB free swap**, and about **18 GiB free disk at 99% usage**;
temporary runner output is cleaned immediately and the next wave remains
deferred until the swap pressure improves.

## W308 Node console/path leaf probe

The bounded five-job Node selector covered five previously unrecorded files:
`test-console-clear.js`, `test-console-instance.js`,
`test-console-diagnostics-channels.js`, `test-path-isabsolute.js`, and
`test-path-join.js`. The final run measured **4/5 file-level passes**, **1
failure**, **0 runner timeouts**, and **200–201 ms** per file. The Node runner
reports file status only; no assertion-level pass total is inferred.

`test-console-clear.js`, `test-console-instance.js`,
`test-path-isabsolute.js`, and `test-path-join.js` passed. The only failure,
`test-console-diagnostics-channels.js`, repeatedly observed **0** subscriber
callbacks where the fixture expects one callback for each console diagnostic
channel publication. This is one diagnostics-channel callback-delivery owner;
no mixed console patch was attempted.

The first invocation was rejected before dispatch because the temporary
selector had not been created in the coordinator worktree; it produced no test
result and is excluded from the metrics above. After correcting the selector
location, the same five files ran successfully under the existing binary.
There were no source or upstream-fixture changes, no full corpus, and no
workspace-wide build. After the run, resources showed about **46 GiB
available memory**, about **768 KiB free swap**, and about **18 GiB free disk at
99% usage**; the temporary runner output is cleaned and the next wave remains
resource-gated.

## W309 Node console/process contract leaf probe

The bounded five-job Node selector covered five previously unrecorded files:
`test-console-not-call-toString.js`, `test-console-with-frozen-intrinsics.js`,
`test-process-prototype.js`, `test-process-config.js`, and
`test-process-features.js`. The runner measured **4/5 file-level passes**, **1
skip**, **0 failures**, **0 runner timeouts**, and **197–199 ms** per file.
The Node runner reports file-level status only; no assertion-level pass total is
inferred.

`test-console-not-call-toString.js`, `test-console-with-frozen-intrinsics.js`,
`test-process-features.js`, and `test-process-prototype.js` passed. The
`test-process-config.js` entry was explicitly skipped because the Linux corpus
environment does not provide `config.gypi`; it is recorded as an environment
coverage boundary rather than a runtime failure. No source or upstream-fixture
changes were made, and no full corpus or workspace-wide build was run.

After the run, resources showed about **45 GiB available memory**, about **1.1
MiB free swap**, and about **17 GiB free disk at 99% usage**. The temporary
runner output is cleaned immediately and the next wave remains resource-gated.

## W310 Node process/console builtin leaf probe

The bounded five-job Node selector covered five previously unrecorded files:
`test-process-get-builtin.mjs`, `test-process-default.js`,
`test-process-chdir-errormessage.js`, `test-process-env-symbols.js`, and
`test-console-formatTime.js`. It measured **4/5 file-level passes**, **1
failure**, **0 runner timeouts**, and **198–349 ms** per file. The Node runner
reports file-level status only; no assertion-level pass total is inferred.

`test-process-default.js`, `test-process-chdir-errormessage.js`,
`test-process-env-symbols.js`, and `test-console-formatTime.js` passed. The
only failure, `test-process-get-builtin.mjs`, found that the returned
`node:test` namespace had the expected structure but was not reference-equal
to the imported namespace. This is one builtin-module identity/cache owner;
no mixed process or console patch was attempted.

There were no source or upstream-fixture changes, no full corpus, and no
workspace-wide build. After the run, resources showed about **45 GiB
available memory**, about **1.1 MiB free swap**, and about **17 GiB free disk at
99% usage**; temporary runner output is cleaned immediately and the next wave
remains resource-gated.

## W311 Bun util low-coupling green cluster

The bounded five-job Bun selector covered five previously unrecorded util
files: `bun-isMainThread`, `arraybuffersink`, `error-code-mirror`,
`exotic-global-mutable-prototype`, and `peek`. It measured **5/5 green files**,
**12 passed / 0 failed / 12 ran / 438 expects**, **0 skips**, and **0 runner
timeouts**. Per-file durations were **199–804 ms**.

Per-file results were: `arraybuffersink.test.ts` **6 / 0 / 6 / 418**;
`bun-isMainThread.test.js` **1 / 0 / 1 / 3**;
`error-code-mirror.test.ts` **2 / 0 / 2 / 3**;
`exotic-global-mutable-prototype.test.ts` **1 / 0 / 1 / 2**; and
`peek.test.ts` **2 / 0 / 2 / 12**. All five remain green coverage with no
source or upstream-fixture changes and no inferred owner. No full corpus or
workspace-wide build was run.

After the run, resources showed about **45 GiB available memory**, about **1.2
MiB free swap**, and about **17 GiB free disk at 99% usage**. Temporary runner
output is cleaned immediately and the next wave remains resource-gated.

## W312 Bun util error/file/unsafe/report/fuzzy leaf probe

The bounded five-job Bun selector covered five previously unrecorded util
files: `error-name-preservation`, `file-type`, `unsafe`, `reportError`, and
`fuzzy-wuzzy`. It measured **3/5 green files**, **9 passed / 155 failed / 164
ran / 30 expects**, **0 skips**, and **0 runner timeouts**. Per-file durations
were **236–486 ms**.

The retained green files were `error-name-preservation.test.ts` at **3 / 0 /
3 / 6**, `file-type.test.ts` at **2 / 0 / 2 / 3**, and `unsafe.test.js` at **4 /
0 / 4 / 18**. `fuzzy-wuzzy.test.ts` reached **0 / 153 / 153 / 0** because
every case encountered the same missing `Promise.resolve` intrinsic; this is
parked as one runtime/intrinsic owner rather than 153 independent failures.
`reportError.test.ts` reached **0 / 2 / 2 / 3**; its failures split into an
error-printer snapshot-format mismatch and a lone-surrogate printer position
contract, so no mixed fix was attempted.

There were no source or upstream-fixture changes, no full corpus, and no
workspace-wide build. After the run, resources showed about **45 GiB
available memory**, about **1.3 MiB free swap**, and about **17 GiB free disk at
99% usage**. Temporary runner output is cleaned immediately and the next wave
remains resource-gated.

## W313 Node console/process/path guard leaf probe

The bounded five-job Node selector covered five previously unrecorded files:
`test-console-issue-43095.js`, `test-process-binding-util.js`,
`test-process-chdir.js`, `test-process-constants-noatime.js`, and
`test-path-posix-relative-on-windows.js`. It measured **4/5 file-level passes**,
**1 failure**, **0 runner timeouts**, and **280–282 ms** per file. The Node
runner reports file-level status only; no assertion-level pass total is
inferred.

`test-process-binding-util.js`, `test-process-chdir.js`,
`test-process-constants-noatime.js`, and `test-path-posix-relative-on-windows.js`
passed. The only failure, `test-console-issue-43095.js`, reached the
`console.dir` path with an already revoked Proxy and threw the revoked-Proxy
TypeError instead of completing the diagnostic print. This is one console
inspection/error-handling owner; no mixed process or path patch was attempted.

There were no source or upstream-fixture changes, no full corpus, and no
workspace-wide build. After the run, resources showed about **44 GiB
available memory**, about **1.3 MiB free swap**, and about **17 GiB free disk at
99% usage**. Temporary runner output is cleaned immediately and the next wave
remains resource-gated.

## W314 Bun util mmap/hash/CSRF/file/concat leaf probe

The bounded five-job Bun selector covered five previously unrecorded util
files: `mmap`, `hash`, `csrf`, `bun-file-exists`, and `concat`. It measured
**4/5 green files**, **50 passed / 8 failed / 58 ran / 200 expects**, **0
skips**, and **0 runner timeouts**. Per-file durations were **200–401 ms**.

The retained green files were `bun-file-exists.test.js` at **1 / 0 / 1 / 7**;
`concat.test.js` at **5 / 0 / 5 / 5**; `csrf.test.ts` at **24 / 0 / 24 / 48**;
and `hash.test.js` at **20 / 0 / 20 / 136**. `mmap.test.js` reached **0 / 8 /
8 / 4** because `Bun.mmap` is not available; all eight failures are the same
missing-API owner, including its option-validation cases.

There were no source or upstream-fixture changes, no full corpus, and no
workspace-wide build. After the run, resources showed about **44 GiB
available memory**, about **1.4 MiB free swap**, and about **17 GiB free disk at
99% usage**. Temporary runner output is cleaned immediately and the next wave
remains resource-gated.

## W315 Node process/console lifecycle green cluster

The bounded five-job Node selector covered five previously unrecorded files:
`test-process-kill-null.js`, `test-process-kill-pid.js`,
`test-process-ref-unref.js`, `test-process-raw-debug.js`, and
`test-console-async-write-error.js`. The runner measured **5/5 file-level
passes**, **0 failures**, **0 runner timeouts**, and **233–386 ms** per file.
The Node runner reports file-level status only; no assertion-level pass total is
inferred.

All five guards passed and are retained: process kill with a null signal,
process kill by PID, ref/unref lifecycle, raw debug child-process behavior, and
console async-write error handling. There were no source or upstream-fixture
changes, no full corpus, and no workspace-wide build. After the run, resources
showed about **44 GiB available memory**, about **1.4 MiB free swap**, and about
**17 GiB free disk at 99% usage**. Temporary runner output is cleaned
immediately and the next wave remains resource-gated.

## W316 Node process.env contract leaf probe

The bounded five-job Node selector covered five previously unrecorded files:
`test-process-load-env-file.js`, `test-process-env-delete.js`,
`test-process-env-deprecation.js`, `test-process-env-ignore-getter-setter.js`,
and `test-process-env-sideeffects.js`. It measured **3/5 file-level passes**,
**1 failure**, **1 skip**, **0 runner timeouts**, and **248–801 ms** per file.
The Node runner reports file-level status only; no assertion-level pass total is
inferred.

`test-process-env-delete.js`, `test-process-env-deprecation.js`, and
`test-process-env-ignore-getter-setter.js` passed. The
`test-process-env-sideeffects.js` entry was skipped because V8 inspector is
disabled in this Linux runtime. `test-process-load-env-file.js` failed two
subtests: the missing-`.env` case is coupled to the runner working directory
and expected fixture layout, while the permission case has a separate error
diagnostic/regex mismatch. These remain two owners; no mixed process.env patch
was attempted.

There were no source or upstream-fixture changes, no full corpus, and no
workspace-wide build. After the run, resources showed about **44 GiB
available memory**, about **1.4 MiB free swap**, and about **17 GiB free disk at
99% usage**. Temporary runner output is cleaned immediately and the next wave
remains resource-gated.

## W317 Node console/stdio/finalization green cluster

The bounded five-job Node selector covered five previously unrecorded files:
`test-console-log-stdio-broken-dest.js`, `test-console-sync-write-error.js`,
`test-process-external-stdio-close.js`,
`test-process-external-stdio-close-spawn.js`, and
`test-process-finalization.mjs`. The runner measured **5/5 file-level passes**,
**0 failures**, **0 runner timeouts**, and **287–1992 ms** per file. The Node
runner reports file-level status only; no assertion-level pass total is
inferred.

All five console/stdio/finalization guards passed and are retained. There were
no source or upstream-fixture changes, no full corpus, and no workspace-wide
build. An unrelated external package installation remained active during the
probe but was not touched. After the run, resources showed about **44 GiB
available memory**, about **1.5 MiB free swap**, and about **17 GiB free disk at
99% usage**. Temporary runner output is cleaned immediately and the next wave
remains resource-gated.

## W318 Node process identity/active-handle and console color leaves

The bounded five-job Node selector covered five selected files: four were fresh
at the time of the run, while `test-process-execve-validation.js` was already
recorded by W113 and is a revalidation. The other files were
`test-process-euid-egid.js`, `test-process-getactivehandles.js`,
`test-process-getactiverequests.js`, and `test-console-tty-colors.js`. The
runner measured **5/5 file-level passes**, **0 failures**, **0 runner
timeouts**, and **198–249 ms** per file. The Node runner reports file-level
status only; no assertion-level pass total is inferred.

Four fresh process/console guards passed and are retained. The repeated
`test-process-execve-validation.js` pass is retained as a W113 revalidation.
There were no source or upstream-fixture changes, no full corpus, and no
workspace-wide build. An unrelated external package installation remained
active during the probe but was not touched. After the run, resources remained
at about **44 GiB available memory**, about **1.5 MiB free swap**, and about
**17 GiB free disk at 99% usage**. Temporary runner output is cleaned
immediately and the next wave remains resource-gated.

## W319 Node active-resources/priority/console leaves

The bounded five-job Node selector covered five previously unrecorded files:
`test-console-tty-colors-per-stream.js`, `test-os-process-priority.js`,
`test-process-getactiveresources.js`,
`test-process-getactiveresources-track-active-handles.js`, and
`test-process-getactiveresources-track-active-requests.js`. The runner measured
**5/5 file-level passes**, **0 failures**, **0 runner timeouts**, and
**199–248 ms** per file. The Node runner reports file-level status only; no
assertion-level pass total is inferred.

All five active-resource, priority, and console guards passed and are retained.
There were no source or upstream-fixture changes, no full corpus, and no
workspace-wide build. After the run, resources remained at about **44 GiB
available memory**, about **1.5 MiB free swap**, and about **16 GiB free disk at
99% usage**. Temporary runner output is cleaned immediately and the next wave
remains resource-gated.

## W320 Bun util object/encoding/timer/path leaves

The bounded five-job Bun selector covered five selected files. A basename audit
of the historical ledger shows four revalidations: `BunObject.test.ts` was
already recorded in W78, `escapeRegExp.test.ts` and `which.test.ts` in W57,
and `toUTF16Alloc.test.ts` in W56. Only `sleepSync.test.ts` is a new file
entry from this wave. The run measured **4/5 green files**, **20 passed / 1
failed / 21 ran / 236 expects**, **0 runner timeouts**, and **200–306 ms** per
file. The four green files include the fresh `sleepSync` guard and three
historical green revalidations.

`BunObject.test.ts` passed its `require("bun")` and dynamic-import checks but
its `hasNonReifiedStatic` check could not run because the internal test helper
was unavailable. This revalidates the W78 internal-helper/API owner; no source
or upstream-fixture changes were made. No full corpus or workspace-wide build
was run. After the run, resources remained at about **44 GiB available
memory**, **1.6 MiB free swap**, and **17 GiB free disk at 99% usage**.
Temporary runner output is cleaned immediately and the next wave remains
resource-gated.

## W321 Node warning/identity contract leaves

The bounded five-job Node selector covered five previously unrecorded files:
`test-process-emit.js`, `test-process-emitwarning.js`,
`test-process-warning.js`, `test-process-warnings.mjs`, and
`test-process-uid-gid.js`. The runner measured **5/5 file-level passes**,
**0 failures**, **0 runner timeouts**, and **198–2306 ms** per file. The Node
runner reports file-level status only; no assertion-level pass total is
inferred.

All five warning emission, warning filtering, and POSIX identity guards passed
and are retained. Warning text in the bounded logs was expected test output;
no source or upstream-fixture changes were made. No full corpus or
workspace-wide build was run. After the run, resources remained at about
**43 GiB available memory**, **1.6 MiB free swap**, and **17 GiB free disk at
99% usage**. Temporary runner output is cleaned immediately and the next wave
remains resource-gated.

## W322 Node active-resource lifetime/signal/title leaves

The bounded five-job Node selector covered five genuinely fresh files after a
basename audit of the entire ledger: `test-process-getactiveresources-track-interval-lifetime.js`,
`test-process-getactiveresources-track-multiple-timers.js`,
`test-process-getactiveresources-track-timer-lifetime.js`,
`test-process-remove-all-signal-listeners.js`, and
`test-process-title-cli.js`. The runner measured **5/5 file-level passes**,
**0 failures**, **0 runner timeouts**, and **281–388 ms** per file. The Node
runner reports file-level status only; no assertion-level pass total is
inferred.

All five active-resource lifetime, signal cleanup, and CLI title guards passed
and are retained. The bounded title test emitted only its expected flag-check
note. There were no source or upstream-fixture changes, no full corpus, and no
workspace-wide build. After the run, resources showed about **44 GiB available
memory**, **1.7 MiB free swap**, and **17 GiB free disk at 99% usage**. Temporary
runner output is cleaned immediately and the next wave remains resource-gated.

## W323 Bun FileSink/loader/path/ANSI leaves

The bounded five-job Bun selector covered five selected files. A second audit
found semantic historical aliases beyond filename matching: `pathToFileURL-invalid.test.ts`
revalidates the W191 invalid-input `pathToFileURL` guard, `filesink.test.ts`
revalidates the W172 FileSink cluster, and `wrapAnsi.npm.test.ts` revalidates
the W168 wrapAnsi owner split. `bun-file-windows.test.ts` and
`text-loader.test.ts` are the two genuinely fresh file entries. The run
measured **4/5 green files**, **60 passed / 20 failed / 81 ran / 1351 expects**,
**0 runner timeouts**, and **199–1855 ms** per file.

The fresh Windows `/dev/null` file-handle guard passed **3/3**, and the fresh
text-loader guard passed **7/7**. FileSink revalidated **46/46**; the
pathToFileURL revalidation passed **1/2**; and wrapAnsi revalidated **3/23**,
with its 20 failures spanning ANSI escape-token segmentation, width/Unicode,
and whitespace/newline wrapping owners already parked by W168. No source or
upstream-fixture changes, full corpus, or workspace-wide build were made.
After the run, resources showed about **44 GiB available memory**, **1.7 MiB
free swap**, and **17 GiB free disk at 99% usage**. Temporary runner output is
cleaned immediately and the next wave remains resource-gated.

## W324 Node process metadata/warning/SourceMap leaves

The bounded five-job Node selector covered five fresh files after the filename
and semantic-owner audit: `test-process-constrained-memory.js`,
`test-process-setsourcemapsenabled.js`, `test-process-redirect-warnings.js`,
`test-process-redirect-warnings-env.js`, and `test-process-versions.js`. It
measured **4/5 file-level passes**, **1 failure**, **0 runner timeouts**, and
**199–654 ms** per file. The Node runner reports file-level status only; no
assertion-level pass total is inferred.

The constrained-memory, SourceMap argument validation, and both warning
redirection guards passed and are retained. `test-process-versions.js` failed
before its version assertions because `process.config.variables` lacks the
Node metadata field `node_builtin_shareable_builtins`; this is one process.config
metadata-shape owner, not a version-vector assertion result. No source or
upstream-fixture changes, full corpus, or workspace-wide build were made. After
the run, resources showed about **44 GiB available memory**, **1.8 MiB free
swap**, and **17 GiB free disk at 99% usage**. Temporary runner output is
cleaned immediately and the next wave remains resource-gated.

## W325 Node eval/global/instanceof/WHATWG URL leaves

The bounded five-job Node selector covered five fresh pure-contract files after
filename/stem and semantic-owner review: `test-eval.js`,
`test-global-domexception.js`, `test-global-encoder.js`, `test-instanceof.js`,
and `test-whatwg-url-custom-href-side-effect.js`. The runner measured **5/5
file-level passes**, **0 failures**, **0 runner timeouts**, and **235–288 ms**
per file. The Node runner reports file-level status only; no assertion-level
pass total is inferred.

All five eval policy, global constructor/encoder identity, instanceof, and
WHATWG URL state-preservation guards passed and are retained. There were no
source or upstream-fixture changes, no full corpus, and no workspace-wide
build. After the run, resources showed about **44 GiB available memory**,
**1.8 MiB free swap**, and **17 GiB free disk at 99% usage**. Temporary runner
output is cleaned immediately and the next wave remains resource-gated.

## W326 Node fs/fs.promises contract leaves

The bounded five-job Node selector covered five fresh low-coupling files after
filename/stem and semantic-owner review: `test-fs-promises-exists.js`,
`test-fs-close.js`, `test-fs-promises-statfs-validate-path.js`,
`test-fs-read-file-assert-encoding.js`, and
`test-fs-write-stream-close-without-callback.js`. The runner measured **5/5
file-level passes**, **0 failures**, **0 runner timeouts**, and **285–288 ms**
per file. The Node runner reports file-level status only; no assertion-level
pass total is inferred.

All five fs/fs.promises identity, close callback, statfs validation, encoding
validation, and WriteStream close guards passed and are retained. There were
no source or upstream-fixture changes, no full corpus, and no workspace-wide
build. After the run, resources showed about **46 GiB available memory**,
**2.2 MiB free swap**, and **17 GiB free disk at 99% usage**. Temporary runner
output is cleaned immediately and the next wave remains resource-gated.

## W327 Node timer clear/refresh/tampering leaves

The bounded five-job Node selector covered five fresh timer files after
filename/stem and semantic-owner review: `test-timers-clearImmediate.js`,
`test-timers-clear-object-does-not-throw-error.js`,
`test-timers-clear-null-does-not-throw-error.js`,
`test-timers-process-tampering.js`, and
`test-timers-refresh-in-callback.js`. The runner measured **5/5 file-level
passes**, **0 failures**, **0 runner timeouts**, and **253–257 ms** per file.
The Node runner reports file-level status only; no assertion-level pass total is
inferred.

All five timer clear, refresh, and process-tampering guards passed and are
retained. There were no source or upstream-fixture changes, no full corpus, and
no workspace-wide build. After the run, resources showed about **46 GiB
available memory**, **2.2 MiB free swap**, and **17 GiB free disk at 99% usage**.
Temporary runner output is cleaned immediately and the next wave remains
resource-gated.

## W328 Node Buffer deprecation/encoding/zero-fill leaves

The bounded five-job Node selector covered five fresh Buffer files after
filename/stem and semantic-owner review: `test-buffer-of-no-deprecation.js`,
`test-buffer-new.js`, `test-buffer-zero-fill.js`,
`test-buffer-zero-fill-reset.js`, and `test-buffer-isencoding.js`. The runner
measured **5/5 file-level passes**, **0 failures**, **0 runner timeouts**, and
**235–288 ms** per file. The Node runner reports file-level status only; no
assertion-level pass total is inferred.

All five Buffer deprecation, constructor validation, zero-fill, allocator-reset,
and encoding-recognition guards passed and are retained. The deprecated Buffer
cases emitted expected DEP0005 warnings only. There were no source or
upstream-fixture changes, no full corpus, and no workspace-wide build. After the
run, resources showed about **46 GiB available memory**, **2.3 MiB free swap**,
and **17 GiB free disk at 99% usage**. Temporary runner output is cleaned
immediately and the next wave remains resource-gated.

## W329 Node HTTP/HTTPS/MessageEvent/WebCrypto/console leaves

The bounded five-job Node selector covered five fresh files after
filename/stem and semantic-owner review: `test-http-client-invalid-path.js`,
`test-https-agent-constructor.js`, `test-messageevent-brandcheck.js`,
`test-global-webcrypto-classes.js`, and `test-global-console-exists.js`. The
runner measured **3/5 file-level passes**, **2 failures**, **0 runner timeouts**,
and **231–431 ms** per file. The Node runner reports file-level status only;
no assertion-level pass total is inferred.

The HTTP invalid-path, HTTPS Agent constructor, and MessageEvent receiver-brand
guards passed and are retained. `test-global-webcrypto-classes.js` failed on
global-versus-internal `Crypto` constructor identity, parked as one WebCrypto
class-export owner. `test-global-console-exists.js` failed because the warning
handler observed the monkeypatched stderr write count before the expected
default warning write, parked as one global-console warning-order owner. No
source or upstream-fixture changes, full corpus, or workspace-wide build were
made. After the run, resources showed about **46 GiB available memory**, **2.3
MiB free swap**, and **17 GiB free disk at 99% usage**. Temporary runner output
is cleaned immediately and the next wave remains resource-gated.

## W330 Node perf_hooks/performance contract leaves

The bounded five-job Node selector covered five fresh `perf_hooks` and
Performance files after filename/stem and semantic-owner review:
`test-perf-hooks-timerify-invalid-args.js`,
`test-perf-hooks-timerify-return-value.js`, `test-performance-global.js`,
`test-performance-measure-detail.js`, and
`test-performance-resourcetimingbuffersize.js`. The runner measured **4/5
file-level passes**, **1 failure**, **0 runner timeouts**, and **199–201 ms** per
file. The Node runner reports file-level status only; no assertion-level pass
total is inferred.

The two `timerify` guards, global `performance` binding, and `measure` detail
observer guard passed and are retained. The resource-timing file stopped at
the first invalid-input assertion because
`performance.setResourceTimingBufferSize(1n)` did not throw the expected
BigInt `TypeError` with `ERR_INVALID_ARG_TYPE`; this is parked as one
resource-timing argument-validation owner. No source or upstream-fixture
changes, full corpus, or workspace-wide build were made. After the run,
resources showed about **46 GiB available memory**, **0 B free swap**, and
**16 GiB free disk at 99% usage**. Temporary runner output is cleaned
immediately and the next wave remains resource-gated.

## W331 Node perf_hooks/performance continuation leaves

The bounded five-job Node selector covered five fresh continuation files after
filename/stem and semantic-owner review: `test-perf-hooks-timerify-basic.js`,
`test-perf-hooks-timerify-constructor.js`,
`test-perf-hooks-timerify-error.js`, `test-performance-nodetiming.js`, and
`test-performance-function-async.js`. The runner measured **4/5 file-level
passes**, **1 failure**, **0 runner timeouts**, and **197–348 ms** per file. The
Node runner reports file-level status only; no assertion-level pass total is
inferred.

The three timerify guards and the async-function timerify guard passed and are
retained. `test-performance-nodetiming.js` failed on the strict milestone
ordering assertion `nodeTiming.v8Start > nodeTiming.nodeStart`; this is parked
as one nodeTiming initialization/order owner. No source or upstream-fixture
changes, full corpus, or workspace-wide build were made. After the run,
resources showed about **46 GiB available memory**, **24 KiB free swap**, and
**15 GiB free disk at 99% usage**. Temporary runner output is cleaned
immediately and the next wave remains resource-gated.

## W332 Bun low-coupling Linux leaves

The bounded five-job Bun selector covered five fresh low-coupling files after
filename/stem and narrow semantic-owner review:
`test/js/bun/namespace-prototype-pollution.test.ts`,
`test/js/bun/runtime-error.test.ts`, `test/js/node/string-module.test.js`,
`test/js/node/no-addons.test.ts`, and `test/js/bun/symbols.test.ts`. Using the
existing binary and the real Bun harness, the runner measured **2/5 green
files**, **4 passed / 4 failed / 8 ran / 17 expects**, **0 runner timeouts**, and
**231–431 ms** per file.

`runtime-error` (**1/1**) and `string-module` (**3/3**) passed and are retained.
The namespace test failed because the imported namespace inherited the
`Object.prototype` function; `no-addons` failed before the expected disabled-
addon diagnostic because `process.dlopen()` reported its two-argument
validation error; and `symbols` failed its two Linux ELF checks because the
current binary exposes glibc symbols newer than the test's compatibility floor
and exits non-zero. These are three separate owners: module namespace
prototype isolation, `process.dlopen` validation/diagnostic ordering, and
Linux binary compatibility metadata. No source or upstream-fixture changes,
full corpus, or workspace-wide build were made. After the run, resources
showed about **46 GiB available memory**, **40 KiB free swap**, and **15 GiB
free disk at 99% usage**. Temporary runner output is cleaned immediately and
the next wave remains resource-gated.

## W333 Node error/internal-contract leaves

The bounded five-job Node selector covered five fresh files after
filename/stem and narrow semantic-owner review: `test-errors-systemerror.js`,
`test-errors-hide-stack-frames.js`, `test-accessor-properties.js`,
`test-binding-constants.js`, and `test-bad-unicode.js`. The runner measured
**1/5 file-level pass**, **4 failures**, **0 runner timeouts**, and
**198–350 ms** per file. The Node runner reports file-level status only; no
assertion-level pass total is inferred.

`test-bad-unicode.js` passed and is retained. The four failures are separated
as follows: accessor getters did not reject incompatible receivers; internal
constants exposed an extra `os.signals` key; `Error.stackTraceLimit` was 100
instead of the expected 10 while checking hidden stack frames; and the empty
`SystemError` context used a JSC-style error message instead of Node's
`Cannot read properties of undefined (reading 'syscall')` wording. These are
four independent owners: accessor brand checks, constants shape, stack-limit
initialization, and SystemError error-text dialect. No source or
upstream-fixture changes, full corpus, or workspace-wide build were made.
After the run, resources showed about **46 GiB available memory**, **48 KiB
free swap**, and **15 GiB free disk at 99% usage**. Temporary runner output is
cleaned immediately and the next wave remains resource-gated.

## W334 Node process lifecycle/propagation leaves

The bounded five-job Node selector covered five fresh process files after
filename/stem and narrow semantic-owner review:
`test-process-beforeexit-throw-exit.js`, `test-process-beforeexit.js`,
`test-process-binding-internalbinding-allowlist.js`,
`test-process-exec-argv.js`, and `test-process-uncaught-exception-monitor.js`.
The runner measured **4/5 file-level passes**, **1 failure**, **0 runner
timeouts**, and **230–849 ms** per file. The Node runner reports file-level
status only; no assertion-level pass total is inferred.

The beforeExit-throw/exit, internalBinding allowlist, execArgv propagation,
and uncaughtExceptionMonitor guards passed and are retained. The ordinary
beforeExit lifecycle file failed because the `tryRepeatedTimer` callback was
expected once but was observed zero times after the timer/listen re-entry
sequence; this is parked as one beforeExit event-loop re-entry owner. No
source or upstream-fixture changes, full corpus, or workspace-wide build were
made. After the run, resources showed about **46 GiB available memory**,
**52 KiB free swap**, and **14 GiB free disk at 100% usage**. Temporary runner
output is cleaned immediately and the next wave remains resource-gated.

## W335 Node timer API leaves

The bounded five-job Node selector covered five fresh timer files after
filename/stem and narrow semantic-owner review: `test-timers-api-refs.js`,
`test-timers-args.js`, `test-timers-clear-timeout-interval-equivalent.js`,
`test-timers-invalid-clear.js`, and `test-timers-to-primitive.js`. The runner
measured **5/5 file-level passes**, **0 failures**, **0 runner timeouts**, and
**232–484 ms** per file. The Node runner reports file-level status only; no
assertion-level pass total is inferred.

All five timer API guards passed: internal timer APIs remain usable after
global timer deletion, callback arguments propagate through the bounded 128-
argument sequence, timeout/interval clearing is interchangeable,
`clearImmediate` handles a non-Immediate input, and timer primitive/string IDs
clear correctly. No source or upstream-fixture changes, full corpus, or
workspace-wide build were made. After the run, resources showed about **46 GiB
available memory**, **60 KiB free swap**, and **16 GiB free disk at 99% usage**.
Temporary runner output is cleaned immediately and the next wave remains
resource-gated.

## W336 Node assert/DNS validation leaves

The bounded five-job Node selector covered five fresh files after
filename/stem and narrow semantic-owner review: `test-assert-async.js`,
`test-assert-first-line.js`, `test-assert-esm-cjs-message-verify.js`,
`test-assert-myers-diff.js`, and `test-dns-setlocaladdress.js`. The runner
measured **3/5 file-level passes**, **2 failures**, **0 runner timeouts**, and
**248–349 ms** per file. The Node runner reports file-level status only; no
assertion-level pass total is inferred.

The ESM/CJS assertion-message comparison, Myers diff input-size boundary, and
DNS `setLocalAddress` validation guards passed and are retained. The async
assertion file failed in a thenable/validation expectation, while the
first-line assertion file produced only the generic falsy-expression message
instead of including the source line; these are parked as two separate
assert async-contract and assertion-source-rendering owners. No source or
upstream-fixture changes, full corpus, or workspace-wide build were made.
After the run, resources showed about **46 GiB available memory**, **244 KiB
free swap**, and **16 GiB free disk at 99% usage**. Temporary runner output is
cleaned immediately and the next wave remains resource-gated.

## W337 Node HTTP contract leaves

The bounded five-job Node selector covered five fresh HTTP files after
filename/stem and narrow semantic-owner review: `test-http-methods.js`,
`test-http-default-port.js`, `test-http-hostname-typechecking.js`,
`test-http-invalid-urls.js`, and `test-http-header-value-relaxed.js`. The
runner measured **5/5 file-level passes**, **0 failures**, **0 runner
timeouts**, and **198 ms–4.263 s** per file. The Node runner reports file-level
status only; no assertion-level pass total is inferred.

All five HTTP guards passed: method table exposure, local HTTP/HTTPS default
port routing, host/hostname type validation, invalid URL rejection, and strict
versus relaxed header-value handling. The default-port HTTPS fixture was the
slowest at **4.263 s**, still below the 30-second per-file bound. No source or
upstream-fixture changes, full corpus, or workspace-wide build were made. After
the run, resources showed about **46 GiB available memory**, **272 KiB free
swap**, and **16 GiB free disk at 99% usage**. Temporary runner output is
cleaned immediately and the next wave remains resource-gated.

## W338 Node dgram local UDP contract leaves

The bounded five-job Node selector covered five fresh dgram files after
filename/stem and narrow semantic-owner review:
`test-dgram-close-is-not-callback.js`, `test-dgram-createSocket-type.js`,
`test-dgram-send-bad-arguments.js`, `test-dgram-send-address-types.js`, and
`test-dgram-send-empty-array.js`. The runner measured **5/5 file-level passes**,
**0 failures**, **0 runner timeouts**, and **285–337 ms** per file. The Node
runner reports file-level status only; no assertion-level pass total is
inferred.

All five local UDP guards passed: non-function close callback handling, socket
type and buffer-option validation, send argument/range validation, address
type validation, and empty datagram delivery. No source or upstream-fixture
changes, full corpus, or workspace-wide build were made. After the run,
resources showed about **46 GiB available memory**, **296 KiB free swap**, and
**16 GiB free disk at 99% usage**. Temporary runner output is cleaned
immediately and the next wave remains resource-gated.

## W339 Bun small Linux regression leaves

The bounded five-job Bun selector covered five fresh small regression files
after filename/stem and narrow semantic-owner review:
`test/regression/issue/hashbang-still-works.test.ts`,
`test/regression/issue/utf16-encoding-crash.test.ts`,
`test/regression/issue/comma-operator-this-binding.test.ts`,
`test/regression/issue/yaml-parse-syntax-error.test.ts`, and
`test/regression/issue/19107.test.ts`. Using the real Bun harness and existing
binary, the runner measured **5/5 green files**, **8 passed / 0 failed / 8 ran /
32 expects**, **0 runner timeouts**, and **198–450 ms** per file.

All five guards passed: hashbang/lexer bounds, UTF-16/ucs2 file decoding,
comma-operator `this` binding, YAML `SyntaxError` shape, and the no-crash
regression. No source or upstream-fixture changes, full corpus, or
workspace-wide build were made. After the run, resources showed about **46 GiB
available memory**, **356 KiB free swap**, and **16 GiB free disk at 99% usage**.
Temporary runner output is cleaned immediately and the next wave remains
resource-gated.

## W340 Node dgram continuation leaves

The bounded five-job Node selector covered five fresh dgram files after
filename/stem and narrow semantic-owner review:
`test-dgram-connect.js`, `test-dgram-connect-send-default-host.js`,
`test-dgram-bind-error-repeat.js`, `test-dgram-ref.js`, and
`test-dgram-unref.js`. The runner measured **4/5 file-level passes**, **1
failure**, **0 runner timeouts**, and **199–250 ms** per file. The Node runner
reports file-level status only; no assertion-level pass total is inferred.

The repeated-bind warning guard, connected-send payload/default-target guard,
and ref/unref handle-lifecycle guards passed and are retained. The connected
socket lifecycle file failed only in its invalid-port message comparison: the
runtime reported the `>= 1` wording with the received port value, while the
Node fixture expects the `> 0` wording. This is parked as one connected-port
validation-message owner; no source or upstream-fixture change was made. No
full corpus or workspace-wide build was run. After the run, resources showed
about **45 GiB available memory**, **372 KiB free swap**, and **16 GiB free
disk at 99% usage**. Temporary runner output is cleaned immediately and the
next wave remains resource-gated.

## W341 Node dgram error/options leaves

The bounded five-job Node selector covered five fresh dgram files after
filename/stem and narrow semantic-owner review:
`test-dgram-send-error.js`, `test-dgram-send-callback-recursive.js`,
`test-dgram-setBroadcast.js`, `test-dgram-setTTL.js`, and
`test-dgram-socket-buffer-size.js`. The runner measured **4/5 file-level
passes**, **1 failure**, **0 runner timeouts**, and **250–353 ms** per file.
The Node runner reports file-level status only; no assertion-level pass total
is inferred.

The send-error propagation, recursive callback scheduling, broadcast option,
and TTL validation/setting guards passed and are retained. The socket buffer
size file failed in its exact `ERR_SOCKET_BUFFER_SIZE` `inspect()` comparison:
the runtime error includes the core message and stack-shaped output but does
not match Node's `SystemError` detail rendering and fields. This is parked as
one socket-buffer-size error-rendering owner; no source or upstream-fixture
change was made. No full corpus or workspace-wide build was run. After the
run, resources showed about **46 GiB available memory**, **396 KiB free swap**,
and **16 GiB free disk at 99% usage**. Temporary runner output is cleaned
immediately and the next wave remains resource-gated.

## W342 Bun Linux regression/parser/filesystem leaves

The bounded five-job Bun selector covered five fresh small regression files
after filename/stem and narrow semantic-owner review:
`test/regression/issue/22929-module-extensions-asi.test.ts`,
`test/regression/issue/23474.test.ts`,
`test/regression/issue/23569.test.ts`,
`test/regression/issue/24129.test.ts`, and
`test/regression/issue/24234.test.ts`. Using the real Bun harness and existing
Linux binary, the runner measured **4/5 green files**, **11 passed / 3 failed /
14 ran / 48 expects**, **0 runner timeouts**, and **236–789 ms** per file.

The Module `_extensions` ASI regression, WebSocket upgrade-cookie behavior,
unknown/FIFO `fs.Dirent` checks, and `console.log("%j")` formatting passed and
are retained. The HTML entrypoint file failed all three executed cases: both
`--no-bundle` diagnostics and the bundled build returned an `Unterminated
regular expression` error instead of the fixture's expected HTML build
behavior. This is parked as one HTML-entrypoint parser/build owner. No source
or upstream-fixture change was made. No full corpus or workspace-wide build
was run. After the run, resources showed about **46 GiB available memory**,
**472 KiB free swap**, and **16 GiB free disk at 99% usage**. Temporary runner
output is cleaned immediately and the next wave remains resource-gated.

## W343 Node readline/TTY leaves

The bounded five-job Node selector covered five fresh readline/TTY files after
filename/stem and narrow semantic-owner review:
`test-tty-backwards-api.js`, `test-tty-stdin-end.js`,
`test-tty-stdin-pipe.js`, `test-readline-csi.js`, and
`test-readline-emit-keypress-events.js`. The runner measured **4/5 file-level
passes**, **1 failure**, **0 runner timeouts**, and **249–350 ms** per file.
The Node runner reports file-level status only; no assertion-level pass total
is inferred.

The readline CSI escape/argument contract, explicit keypress event parsing,
stdin end safety, and stdin pipe lifecycle guards passed and are retained. The
TTY backwards-API file failed because the `WriteStream` methods did not invoke
the mocked readline forwarding functions or callbacks (the fixture expected
two calls per method but observed zero). This is parked as one TTY
backwards-API forwarding owner; no source or upstream-fixture change was made.
No full corpus or workspace-wide build was run. After the run, resources
showed about **47 GiB available memory**, **528 KiB free swap**, and **16 GiB
free disk at 99% usage**. Temporary runner output is cleaned immediately and
the next wave remains resource-gated.

## W344 Bun Linux loader/build/network/REPL leaves

The bounded five-job Bun selector covered five fresh small regression files
after filename/stem and narrow semantic-owner review:
`test/regression/issue/25609.test.ts`,
`test/regression/issue/25622.test.ts`,
`test/regression/issue/25707.test.ts`,
`test/regression/issue/25862.test.ts`, and
`test/regression/issue/26058.test.ts`. Using the real Bun harness and existing
Linux binary, the runner measured **5/5 green files**, **8 passed / 0 failed /
8 ran / 21 expects**, **0 runner timeouts**, and **287–589 ms** per file.

All five guards passed: dead-code-elimination syntax validity, child tsconfig
path replacement, deferred CJS dynamic `node:` import resolution, pipelined
HTTP CONNECT head delivery, and REPL startup without package-resolution
output. No source or upstream-fixture changes, full corpus, or workspace-wide
build were made. After the run, resources showed about **47 GiB available
memory**, **600 KiB free swap**, and **16 GiB free disk at 99% usage**.
Temporary runner output is cleaned immediately and the next wave remains
resource-gated.

## W345 Node readline continuation leaves

The bounded five-job Node selector covered five fresh readline files after
filename/stem and narrow semantic-owner review:
`test-readline-interface-no-trailing-newline.js`,
`test-readline-interface-recursive-writes.js`,
`test-readline-line-separators.js`, `test-readline-set-raw-mode.js`, and
`test-readline-position.js`. The default environment marked three terminal
files skipped because `TERM=dumb`; the same selector was rerun with
`TERM=xterm` and is the authoritative result. That bounded rerun measured
**4/5 file-level passes**, **1 failure**, **0 runner timeouts**, and
**198–349 ms** per file. The Node runner reports file-level status only; no
assertion-level pass total is inferred.

The no-trailing-newline, recursive-write, raw-mode lifecycle, and Unicode
cursor-position guards passed and are retained. The line-separator file failed
because U+2028 and U+2029 remained embedded in the final line instead of being
split into separate lines. This is parked as one Unicode line-separator owner;
no source or upstream-fixture change was made. No full corpus or
workspace-wide build was run. After the rerun, resources showed about **47 GiB
available memory**, **1 MiB free swap**, and **16 GiB free disk at 99% usage**.
Temporary runner output is cleaned immediately and the next wave remains
resource-gated.

## W346 Node readline Unicode line-separator source fix

The W345 failure was reproduced first with the existing coordinator binary:
`test-readline-line-separators.js` measured **1/1 file-level failure** and
reported `89<U+2028>ABC<U+2029>DEF` as one line instead of six expected lines.
The source owner was localized to the `lineEnding` regular expression in
`modules/jsc/src/builtins/node_readline.cppm`; it matched CR/LF but not U+2028
or U+2029. The minimal source fix adds both Unicode line terminators to that
regular expression. No `compat/` test or assertion was changed.

After one release rebuild, the focused acceptance run measured **1/1 pass**.
The bounded five-job adjacent regression measured **5/5 file-level passes** and
**0 runner timeouts** for the target plus no-trailing-newline,
recursive-writes, set-raw-mode, and cursor-position readline guards. The
targeted selector used `TERM=xterm`, a 30-second per-file bound, and jobs=5;
no full corpus or workspace-wide test was run. This is the first W41
source-fix checkpoint; later PR updates should summarize substantive
checkpoints rather than each measurement wave.

## W347 Node/Bun dgram port-message source fix

The W340 connected-dgram failure was reproduced first with the current
coordinator binary: the bounded five-job Node selector measured **4/5
file-level passes**, **1 failure**, and **0 runner timeouts**. The only failure
was `test-dgram-connect.js` comparing `Port should be >= 1 and < 65536` with
Node's `Port should be > 0 and < 65536` contract. The owner was localized to
`modules/jsc/src/js_dgram.cppm` `validatePort`; the fix changes only the
`allowZero=false` lower-bound wording and leaves the accepted range unchanged.

After one release rebuild, the same Node selector measured **5/5 passes** and
**0 runner timeouts**. A correct Bun-native `node:dgram` entry
(`compat/bun/test/js/node/dgram/node-dgram.test.js`) measured **3 passed / 0
failed / 3 ran / 4 expects**. An exploratory mixed Bun selector was not used
as evidence: three Node-style files were correctly classified as `no-tests`,
and a separate port-occupation/membership fixture had two unrelated failures;
the selector was replaced with the Bun-native guard. No `compat/` test or
assertion was changed, and no full corpus or workspace-wide test was run.

## Coverage novelty audit correction after W323

A post-wave audit found that older ledger sections record many files by
basename rather than full relative path. The prior candidate check searched
only for the full path, so W318 repeated one W113 file and W320 repeated four
historical Bun files. Their measured runner results remain valid, but the
incremental-new-file counts are corrected above: W318 is **4 fresh + 1
revalidation** and W320 is **1 fresh + 4 revalidations**. No source, fixture,
build, or test behavior changed. Future selection gates use basename matches
against the entire ledger, including historical sections, before a file is
called fresh. W323 also showed that semantic aliases must be reviewed: basename
and stem matching alone did not connect `pathToFileURL-invalid` to W191,
`FileSink` to W172, or `wrapAnsi` to W168. Future selection gates therefore
combine filename/stem matching with a narrow owner/category alias review before
calling a file fresh.

## W304 Node events/path continuation leaf probe

The bounded five-job Node selector covered AbortListener registration,
`getEventListeners`, uncaught-exception stack formatting, path relative
resolution, and zero-length path semantics. It measured **4/5 file-level
passes**, **1 failure**, and **0 runner timeouts**. Per-file durations were
**283–387 ms**.

`test-events-add-abort-listener.mjs`,
`test-events-static-geteventlisteners.js`, `test-path-relative.js`, and
`test-path-zero-length-strings.js` passed. The
`test-events-uncaught-exception-stack.js` failure was limited to stack
rendering: the first stack line included a call-site prefix where the test
expects the plain `Error` line. This is a focused stack-format owner. There
were no source or upstream-fixture changes.

The selector used the bounded five-job Node runner with a 30-second per-file
timeout. No full corpus, build, or workspace-wide test was run; the Node
runner reports file-level status only.

## W303 Bun/Deno Fetch/URL API leaf probe

The bounded five-job Bun selector covered Deno Fetch headers, Blob, Request,
Response, and URLSearchParams APIs. It measured **5/5 green files**, **80
passed / 0 failed / 85 ran / 245 expects**, and **0 runner timeouts**.
Per-file durations were **286–293 ms**.

`headers.test.ts` measured **26 passed / 0 failed / 27 ran / 122 expects**;
`blob.test.ts` measured **9 / 0 / 10 / 16**;
`response.test.ts` measured **8 / 0 / 9 / 24**;
`request.test.ts` measured **5 / 0 / 7 / 7**; and
`urlsearchparams.test.ts` measured **32 / 0 / 32 / 76**. All five guards are
retained as green coverage with no source or upstream-fixture changes.

The selector used the bounded five-job Bun runner with a 30-second per-file
timeout, the existing coordinator binary, and missing Node modules allowed.
No full corpus, build, or workspace-wide test was run.

## W302 Node URL continuation leaf probe

The bounded five-job Node selector covered URL parse/query and parse/format
compatibility, `urlToHttpOptions`, relative resolution, and the internal URL
brand check. It measured **5/5 file-level passes**, **0 failures**, and **0
runner timeouts**. Per-file durations were **200–399 ms**.

`test-url-parse-query.js`, `test-url-parse-format.js`,
`test-url-urltooptions.js`, `test-url-relative.js`, and
`test-url-is-url-internal.js` all passed. There were no source or
upstream-fixture changes.

The selector used the bounded five-job Node runner with a 30-second per-file
timeout. No full corpus, build, or workspace-wide test was run; the Node
runner reports file-level status only.

## W301 Node util/VM/encoding continuation leaf probe

The bounded five-job Node selector covered signal-to-exit-code conversion,
internal util symbols, VM namespace inspection, TextDecoder, and custom
promisify names. It measured **2/5 file-level passes**, **3 failures**, and
**0 runner timeouts**. Per-file durations were **231–382 ms**.

`test-util-convert-signal-to-exit-code.mjs` and
`test-util-text-decoder.js` passed.

`test-util-inspect-namespace.js` failed on the VM namespace inspection shape:
the runtime returned a generic null-prototype object rather than the expected
Module namespace representation. `test-util-internal.js` failed because the
private arrow-message symbol appeared in `Reflect.ownKeys()` where the test
expects it to remain hidden. `test-util-promisify-custom-names.mjs` failed
because the custom-promisified `fs.exists` function name was empty instead of
`exists`. These are three separate focused owners. There were no source or
upstream-fixture changes.

The selector used the bounded five-job Node runner with a 30-second per-file
timeout. No full corpus, build, or workspace-wide test was run; the Node
runner reports file-level status only.

## W300 Node util/styleText/os/url leaf probe

The bounded five-job Node selector covered internal sleep argument validation,
regular and hex `util.styleText`, URL deprecation behavior, and an `os.userInfo`
getter-error guard. It measured **4/5 file-level passes**, **1 skip**, **0
failures**, and **0 runner timeouts**. Per-file durations were **406–959 ms**.

`test-util-sleep.js`, `test-util-styletext-hex.js`,
`test-url-parse-deprecation.js`, and
`test-os-userinfo-handles-getter-errors.js` passed. The regular
`test-util-styletext.js` file was skipped because the Linux runner could not
create a TTY file descriptor; this is a harness/environment skip, not a source
failure. There were no source or upstream-fixture changes.

The selector used the bounded five-job Node runner with a 30-second per-file
timeout. No full corpus, build, or workspace-wide test was run; the Node
runner reports file-level status only.

## W299 Bun Blob/stream/event/body leaf probe

The bounded five-job Bun selector covered Blob write behavior, stream fast
paths, EventTarget, CustomEvent, and Fetch body leaves. It measured **5/5
green files**, **47 passed / 0 failed / 50 ran / 119 expects**, and **0 runner
timeouts**. Per-file durations were **200–251 ms**.

`blob-write.test.ts` measured **10 passed / 0 failed / 10 ran / 25 expects**;
`stream-fast-path.test.ts` measured **18 / 0 / 18 / 32**;
`event-target.test.ts` measured **14 / 0 / 15 / 43**;
`custom-event.test.ts` measured **2 / 0 / 2 / 8**; and
`body.test.ts` measured **3 / 0 / 5 / 11**. All five guards are retained as
green coverage with no source or upstream-fixture changes.

The selector used the bounded five-job Bun runner with a 30-second per-file
timeout, the existing coordinator binary, and missing Node modules allowed.
No full corpus, build, or workspace-wide test was run.

## W298 Node path/os continuation leaf probe

The bounded five-job Node selector continued the path/os slice with basename,
dirname, extname, home-directory fallback, and checked syscall-error leaves.
It measured **5/5 file-level passes**, **0 failures**, and **0 runner
timeouts**. Per-file durations were **201–350 ms**.

`test-path-basename.js`, `test-path-dirname.js`, `test-path-extname.js`,
`test-os-homedir-no-envvar.js`, and `test-os-checked-function.js` all passed.
There were no source or upstream-fixture changes.

The selector used the bounded five-job Node runner with a 30-second per-file
timeout. No full corpus, build, or workspace-wide test was run; the Node
runner reports file-level status only.

## W297 Bun fetch/blob/timer ownership probe

The bounded five-job Bun selector covered gzip fetch decoding, Blob ownership
and copy-on-write, Blob array fast paths, and setInterval cancellation. It
measured **3/5 green files**, **24 passed / 1 failed / 25 ran / 54 expects**,
and **1 runner timeout**. Per-file durations were **250–33170 ms**.

`blob-array-fast-path.test.ts` measured **11 passed / 0 failed / 11 ran / 13
expects**; `blob-cow.test.ts` measured **5 / 0 / 5 / 21**; and
`blob-file-name-ownership.test.ts` measured **1 / 0 / 1 / 3**. These three
ownership/fast-path guards are retained as green coverage.

`fetch-gzip.test.ts` did not complete within the 30-second per-file runner
limit and is parked as a gzip fetch liveness/timeout owner. The
`setInterval.test.js` file measured **7 passed / 1 failed / 8 ran / 17
expects**; the failure was the cancellation-after-scheduling case. This is a
separate timer cancellation owner. There were no source or upstream-fixture
changes.

The selector used the bounded five-job Bun runner with a 30-second per-file
timeout, the existing coordinator binary, and missing Node modules allowed.
No full corpus, build, or workspace-wide test was run.

## W296 Bun fetch/encoding/timer leaf probe

The bounded five-job Bun selector covered fetch header casing, UTF-8 BOM
handling, Performance entries, setImmediate event-loop progress, and
TextDecoderStream. It measured **5/5 green files**, **70 passed / 0 failed /
70 ran / 85 expects**, and **0 runner timeouts**. Per-file durations were
**200–1702 ms**.

`text-decoder-stream.test.ts` measured **44 passed / 0 failed / 44 ran / 50
expects**; `headers-case.test.ts` measured **3 / 0 / 3 / 9**;
`utf8-bom.test.ts` measured **21 / 0 / 21 / 23**;
`performance-entries.test.ts` measured **1 / 0 / 1 / 2**; and
`setImmediate2.test.ts` measured **1 / 0 / 1 / 1**. All five guards are
retained as green coverage with no source or upstream-fixture changes.

The selector used the bounded five-job Bun runner with a 30-second per-file
timeout, the existing coordinator binary, and missing Node modules allowed.
No full corpus, build, or workspace-wide test was run.

## W295 Node path/query/url/events leaf probe

The bounded five-job Node selector covered path identity, querystring handling,
URL invalid-input validation, CustomEvent, and `events.on` async iteration. It
measured **4/5 file-level passes**, **1 failure**, and **0 runner timeouts**.
Per-file durations were **199–700 ms**.

`test-path-posix-exists.js`, `test-querystring.js`,
`test-url-parse-invalid-input.js`, and `test-events-customevent.js` passed.
`test-events-on-async-iterator.js` failed in its invalid-argument validation:
the thrown error lacked the expected `ERR_INVALID_ARG_TYPE` `code` property.
This is a focused events error-code shape owner. There were no source or
upstream-fixture changes.

The selector used the bounded five-job Node runner with a 30-second per-file
timeout. No full corpus, build, or workspace-wide test was run; the Node
runner reports file-level status only.

## W294 Node path/os/url pure-contract leaf probe

The bounded five-job Node selector covered path normalization, OS property
immutability, URL international-domain conversion, and URL argument
validation. It measured **5/5 file-level passes**, **0 failures**, and **0
runner timeouts**. Per-file durations were **198–248 ms**.

`test-path-normalize.js`, `test-os-eol.js`,
`test-os-constants-signals.js`, `test-url-domain-ascii-unicode.js`, and
`test-url-revokeobjecturl.js` all passed. There were no source or
upstream-fixture changes.

The selector used the bounded five-job Node runner with a 30-second per-file
timeout. No full corpus, build, or workspace-wide test was run; the Node
runner reports file-level status only.

## W293 Node loader/symlink continuation leaf probe

The bounded five-job Node selector continued the W289/W290 loader probe with
entry-point, multi-extension, trailing-slash, and preserve-symlinks leaves. It
measured **3/5 file-level passes**, **2 failures**, and **0 runner timeouts**.
Per-file durations were **199–450 ms**.

`test-module-main-fail.js`,
`test-module-main-preserve-symlinks-fail.js`, and
`test-require-extensions-same-filename-as-dir-trailing-slash.js` passed. The
trailing-slash variant passing while W290's non-trailing variant failed narrows
that existing owner to a specific extensionless directory/file precedence
path, not all trailing-slash resolution.

`test-module-multi-extensions.js` failed because the custom multi-part
extension was not selected for the extensionless require path. This is a
separate custom-extension selection owner. `test-require-symlink.js` failed in
the preserve-symlinks child/worker path because the symlinked entry's
`__filename` was absent from `require.cache`; this is a preserve-symlinks
cache-identity owner. There were no source or upstream-fixture changes.

The selector used the bounded five-job Node runner with a 30-second per-file
timeout. No full corpus, build, or workspace-wide test was run; the Node
runner reports file-level status only.

## W292 Bun console/performance/HTTP leaf probe

The bounded five-job Bun selector covered Console, Performance, Buffer URL,
clearImmediate GC, and HTTP transfer-encoding leaves. It measured **4/5 green
files**, **30 passed / 4 failed / 34 ran / 66 expects**, and **0 runner-level
timeouts**. Per-file durations were **200–6727 ms**.

`buffer-resolveObjectURL.test.ts` measured **3 passed / 0 failed / 3 ran / 12
expects**; `console.test.ts` measured **7 / 0 / 7 / 10**;
`clearImmediate-gc.test.ts` measured **1 / 0 / 1 / 3**; and
`performance.test.js` measured **7 / 0 / 7 / 12**. The HTTP
`node-http-transfer-encoding.test.ts` file measured **12 passed / 4 failed /
16 ran / 29 expects** in 6727 ms.

The four HTTP failures were concentrated in malformed trailer validation and
liveness: bare-LF/CTL trailer handling, client-error delivery, and the
configured trailer-size boundary. Two cases reported an internal test timeout,
but the file stayed within the runner's 30-second limit. This is one focused
HTTP trailer owner; there were no source or upstream-fixture changes.

The selector used the bounded five-job Bun runner with a 30-second per-file
timeout, the existing coordinator binary, and missing Node modules allowed.
No full corpus, build, or workspace-wide test was run.

## W291 Bun Base64/Buffer/console/encoding leaf probe

The bounded five-job Bun selector covered Base64, Buffer bounds and metadata,
console constructor stack recovery, and Deno encoding leaves. It measured
**5/5 green files**, **38 passed / 0 failed / 40 ran / 118 expects**, and **0
runner timeouts**. Per-file durations were **200–300 ms**.

`encoding.test.ts` measured **21 passed / 0 failed / 23 ran / 41 expects**;
`buffer-compare-bounds.test.ts` measured **13 / 0 / 13 / 17**;
`buffer-inspectmaxbytes.test.ts` measured **1 / 0 / 1 / 5**;
`console-constructor-exception.test.ts` measured **1 / 0 / 1 / 4**; and
`atob.test.js` measured **2 / 0 / 2 / 51**. All five guards are retained as
green coverage with no source or upstream-fixture changes.

The selector used the bounded five-job Bun runner with a 30-second per-file
timeout, the existing coordinator binary, and missing Node modules allowed.
No full corpus, build, or workspace-wide test was run.

## W290 Node module metadata/extension leaf probe

The bounded five-job Node selector covered module metadata and extension
resolution leaves. It measured **4/5 file-level passes**, **1 failure**, and
**0 runner timeouts**. Per-file durations were **198–299 ms**.

`test-module-children.js`, `test-module-stat.js`,
`test-module-version.js`, and `test-require-extensions-main.js` passed.
`test-require-extensions-same-filename-as-dir.js` failed because resolution
selected the directory-backed fixture content where the test expected the
explicit module file. This confirms the W289 extension-over-directory
precedence owner rather than introducing a new source owner. There were no
source or upstream-fixture changes.

The selector used the bounded five-job Node runner with a 30-second per-file
timeout. No full corpus, build, or workspace-wide test was run; the Node
runner reports file-level status only.

## W289 Node resolver/extension/symlink leaf probe

The bounded five-job Node selector covered resolver, extension precedence, and
symlinked-module leaves. It measured **3/5 file-level passes**, **2
failures**, and **0 runner timeouts**. Per-file durations were **197–349 ms**.

`test-module-symlinked-peer-modules.js`,
`test-require-invalid-main-no-exports.js`, and
`test-require-resolve-opts-paths-relative.js` passed.
`test-require-extension-over-directory.js` failed because directory-versus-
extension resolution did not select the same expected module. This is a
narrow extension-precedence owner. `test-require-resolve.js` failed when its
fixture's default `bar` module could not be resolved from the expected fixture
lookup location. This is a separate `require.resolve` fixture-lookup owner.
There were no source or upstream-fixture changes.

The selector used the bounded five-job Node runner with a 30-second per-file
timeout. No full corpus, build, or workspace-wide test was run; the Node
runner reports file-level status only.

## W288 Node resolver/require flag leaf probe

The bounded **five-job** Node selector covered
`test-require-delete-array-iterator.js`, `test-require-dot.js`,
`test-require-mjs.js`, `test-require-process.js`, and
`test-require-resolve-invalid-paths.js`. It measured **5/5 file-level passes**,
**0 failures**, and **0 runner timeouts**. Per-file durations were 284–384ms.

The selector passed dynamic import after deleting the Array iterator prototype,
dot-module resolution and `NODE_PATH` behavior, the
`--no-experimental-require-module` ESM rejection contract, `require('process')`
identity, and non-string `require.resolve()` path validation. These are
isolated resolver/require leaves with no new source owner.

No source or upstream fixture change was made. The selector reused the
coordinator binary through `tools/integration/node_corpus_runner.py` with
**five bounded jobs** and a 30-second per-file timeout. No full corpus, build,
or workspace-wide test was run. The Node corpus runner reports only file-level
status for these plain scripts, so no synthetic subtest count was added.

## W287 Bun util inspect/fs metadata leaf probe

The bounded **five-job** Bun selector covered `bun-inspect.test.ts`,
`custom-inspect.test.js`, `util-inspect-proxy.test.js`,
`fs-birthtime-linux.test.ts`, and `cp-symlink-target.test.ts`. It measured
**4/5 files green**, **61 passed / 1 failed / 62 ran / 130 expects**, and **0
runner timeouts**. Per-file durations were 234–287ms under the bounded
4G/512-task resource profile.

`bun-inspect.test.ts` passed **12/12 tests / 19 expects**;
`custom-inspect.test.js` passed **42/42 / 91 expects**;
`fs-birthtime-linux.test.ts` passed **5/5 / 20 expects**; and
`cp-symlink-target.test.ts` passed **2/2** with no additional `expect()` calls
reported. The single `util-inspect-proxy.test.js` test failed immediately when
inspection triggered the proxy's `getPrototypeOf` trap. This is a narrow
proxy-inspection owner; no source or upstream fixture change was made.

The selector reused the coordinator binary through
`tools/integration/bun_corpus_runner.py` with **five bounded jobs**, a
30-second per-file timeout, and missing Node modules allowed. No full corpus,
build, or workspace-wide test was run.

## W286 Node require boundary leaf probe

The bounded **five-job** Node selector covered `test-require-empty-main.js`,
`test-require-enoent-dir.js`, `test-require-exceptions.js`,
`test-require-node-prefix.js`, and `test-require-nul.js`. It measured **5/5
file-level passes**, **0 failures**, and **0 runner timeouts**. Per-file
durations were 249–255ms.

The selector passed empty-package-main fallback, resolution after a dependency
directory is deleted, repeated throwing-module loads, `node:` prefix/cache
bypass behavior, and NUL-byte rejection. These are isolated require-path and
error-contract leaves with no new source owner.

No source or upstream fixture change was made. The selector reused the
coordinator binary through `tools/integration/node_corpus_runner.py` with
**five bounded jobs** and a 30-second per-file timeout. No full corpus, build,
or workspace-wide test was run. The Node corpus runner reports only file-level
status for these plain scripts, so no synthetic subtest count was added.

## W285 Node circular-loader/require-error leaf probe

The bounded **five-job** Node selector covered
`test-module-circular-dependency-warning.js`,
`test-module-circular-symlinks.js`, `test-require-invalid-package.js`,
`test-require-json.js`, and `test-require-unicode.js`. It measured **4/5
file-level passes**, **1 failure**, and **0 runner timeouts**. Per-file
durations were 200–251ms.

`test-module-circular-dependency-warning.js`,
`test-module-circular-symlinks.js`, `test-require-invalid-package.js`, and
`test-require-unicode.js` passed. `test-require-json.js` failed at its first
invalid-JSON assertion: the reference expects the error message to include the
fixture path, while mbun returned only a generic JSON parse message. This is a
narrow JSON diagnostic owner; no source or upstream fixture change was made.

The selector reused the coordinator binary through
`tools/integration/node_corpus_runner.py` with **five bounded jobs** and a
30-second per-file timeout. No full corpus, build, or workspace-wide test was
run. The Node corpus runner reports only file-level status for these plain
scripts, so no synthetic subtest count was added.

## W284 Bun HTTP timeout/cork/TLS leaf probe

The bounded three-job Bun selector covered `client-timeout-error.test.ts`,
`node-http-nested-cork.test.ts`, and `node-https-checkServerIdentity.test.ts`.
It measured **3/3 files green**, **16 passed / 0 failed / 16 ran / 46 expects**,
and **0 runner timeouts**. Per-file durations were 333–1486ms under the
bounded 4G/512-task resource profile.

`client-timeout-error.test.ts` passed **2/2 tests / 4 expects** for timeout
emission, explicit timeout clearing, and request lifecycle; the nested-cork
security matrix passed **10/10 tests / 30 expects** with no cross-socket data
bleed across Node HTTP and Bun.serve write/yield patterns; and
`node-https-checkServerIdentity.test.ts` passed **4/4 tests / 12 expects** for
hostname mismatch errors, Subject-CN fallback, custom identity checks, and
client-certificate request behavior.

No source or upstream fixture change was made. The selector reused the
coordinator binary through `tools/integration/bun_corpus_runner.py` with
three bounded jobs, a 30-second per-file timeout, and missing Node modules
allowed. No full corpus, build, or workspace-wide test was run.

## W283 Node module entry/global-path leaf probe

The bounded three-job Node selector covered
`test-module-globalpaths-nodepath.js`, `test-module-loading-globalpaths.js`,
and `test-module-main-extension-lookup.js`. It measured **2/3 file-level
passes**, **1 failure**, and **0 runner timeouts**. Per-file durations were
200–550ms.

`test-module-globalpaths-nodepath.js` passed `NODE_PATH` initialization and
global-path filtering, and `test-module-main-extension-lookup.js` passed its
child-process ESM/extension lookup probes. `test-module-loading-globalpaths.js`
failed when its copied child runtime could not resolve the expected `foo`
package from the HOME/global-path setup. This is a focused copied-runtime
global-path loader owner; no source or upstream fixture change was made.

The selector reused the coordinator binary through
`tools/integration/node_corpus_runner.py` with three bounded jobs and a
30-second per-file timeout. No full corpus, build, or workspace-wide test was
run. The Node corpus runner reports only file-level status for these plain
scripts, so no synthetic subtest count was added.

## W282 Node module wrap/wrapper/deprecation leaf probe

The bounded three-job Node selector covered
`test-module-parent-setter-deprecation.js`, `test-module-wrap.js`, and
`test-module-wrapper.js`. It measured **3/3 file-level passes**, **0 failures**,
and **0 runner timeouts**. Per-file durations were 299–300ms.

`test-module-parent-setter-deprecation.js` passed the pending-deprecation
`DEP0144` warning and setter path; both `test-module-wrap.js` and
`test-module-wrapper.js` passed their child-process CommonJS wrapper fixture
probes. These are isolated loader/deprecation leaves with no new source owner.

No source or upstream fixture change was made. The selector reused the
coordinator binary through `tools/integration/node_corpus_runner.py` with
three bounded jobs and a 30-second per-file timeout. No full corpus, build,
or workspace-wide test was run. The Node corpus runner reports only file-level
status for these plain scripts, so no synthetic subtest count was added.

## W281 Bun crypto KeyObject/RSA/X509 leaf probe

The bounded three-job Bun selector covered `crypto.key-objects.test.ts`,
`crypto-rsa.test.js`, and `x509-subclass.test.ts`. It measured **2/3 files
green**, **118 passed / 1 failed / 142 ran / 1016 expects**, and **0 runner
timeouts**. Per-file durations were 198–903ms under the bounded 4G/512-task
resource profile.

`crypto.key-objects.test.ts` was green with **85 passed, 0 failed, 108 ran,
917 expects**, plus **22 skipped** and **1 todo**; it covers secret/public/
private KeyObject creation, JWK/PEM conversion, encryption/signing, and
validation. `x509-subclass.test.ts` was green with **12 passed / 0 failed /
12 ran / 34 expects**, covering prototype/subclass behavior and `checkIssued`.

`crypto-rsa.test.js` reached **21 passed / 1 failed / 22 ran / 65 expects**.
The single failure is the `RSA_PKCS1_PADDING` private-decrypt guard: the
reference expects `ERR_INVALID_ARG_VALUE`, while mbun did not throw. This is a
narrow RSA padding-policy owner; no source or upstream fixture change was
made.

The selector reused the coordinator binary through
`tools/integration/bun_corpus_runner.py` with three bounded jobs, a 30-second
per-file timeout, and missing Node modules allowed. No full corpus, build, or
workspace-wide test was run.

## W280 Node module warning/source-map API leaf probe

The bounded three-job Node selector covered `test-module-loading-deprecated.js`,
`test-module-parent-deprecation.js`, and `test-module-setsourcemapssupport.js`.
It measured **3/3 file-level passes**, **0 failures**, and **0 runner
timeouts**. Per-file durations were 197–300ms.

`test-module-loading-deprecated.js` passed the `DEP0128` invalid-main warning
contract; `test-module-parent-deprecation.js` passed the pending-deprecation
`DEP0144` warning and `module.parent` value contract; and
`test-module-setsourcemapssupport.js` passed invalid top-level and option-value
argument validation. These are isolated warning/API leaves with no new source
owner.

No source or upstream fixture change was made. The selector reused the
coordinator binary through `tools/integration/node_corpus_runner.py` with
three bounded jobs and a 30-second per-file timeout. No full corpus, build,
or workspace-wide test was run. The Node corpus runner reports only file-level
status for these plain scripts, so no synthetic subtest count was added.

## W279 Bun crypto invalid-this/lazyhash/HKDF leaf probe

The bounded three-job Bun selector covered
`crypto-invalid-this.test.ts`, `crypto-lazyhash.test.ts`, and
`hkdf-callback-null.test.ts`. It measured **3/3 files green**, **8 passed / 0
failed / 8 ran / 24 expects**, and **0 runner timeouts**. Per-file durations
were 198–203ms under the bounded 4G/512-task resource profile.

`crypto-invalid-this.test.ts` passed **3/3** tests and **9/9 expects** for
invalid receivers on native HMAC and DiffieHellmanGroup accessors;
`crypto-lazyhash.test.ts` passed **2/2** tests and **2/2 expects** for the
Transform inheritance contract; and `hkdf-callback-null.test.ts` passed
**3/3** tests and **13/13 expects** for callback-null and secret-KeyObject
validation. This is a compact green crypto guard cluster with no new source
owner.

No source or upstream fixture change was made. The selector reused the
coordinator binary through `tools/integration/bun_corpus_runner.py` with
three bounded jobs, a 30-second per-file timeout, and missing Node modules
allowed. No full corpus, build, or workspace-wide test was run.

## W278 Node module createRequire/cache/prototype leaf probe

The bounded three-job Node selector covered
`test-module-create-require-multibyte.js`, `test-module-prototype-mutation.js`,
and `test-require-cache.js`. It measured **3/3 file-level passes**, **0
failures**, and **0 runner timeouts**. Per-file durations were 231–233ms.

`test-module-create-require-multibyte.js` passed createRequire resolution for
multibyte fixture paths; `test-module-prototype-mutation.js` passed module
loading with guarded `Object.prototype` accessors; and `test-require-cache.js`
passed injected cache entries for relative and builtin module requests. These
are isolated module-loader contract leaves with no new source owner.

No source or upstream fixture change was made. The selector reused the
coordinator binary through `tools/integration/node_corpus_runner.py` with
three bounded jobs and a 30-second per-file timeout. No full corpus, build,
or workspace-wide test was run. The Node corpus runner reports only file-level
status for these plain scripts, so no synthetic subtest count was added.

## W277 Node module cache/lookup-path leaf probe

The bounded three-job Node selector covered `test-module-cache.js`,
`test-module-relative-lookup.js`, and `test-module-nodemodulepaths.js`. It
measured **3/3 file-level passes**, **0 failures**, and **0 runner timeouts**.
Per-file durations were 199–200ms.

`test-module-cache.js` passed its temporary JSON-module cache contract;
`test-module-relative-lookup.js` passed relative lookup-path resolution; and
`test-module-nodemodulepaths.js` passed the POSIX/Windows node_modules path
contract checks. The three files are isolated module-loader leaves with no new
source owner.

No source or upstream fixture change was made. The selector reused the
coordinator binary through `tools/integration/node_corpus_runner.py` with
three bounded jobs and a 30-second per-file timeout. No full corpus, build,
or workspace-wide test was run. The Node corpus runner reports only file-level
status for these plain scripts, so no synthetic subtest count was added.

## W276 Bun crypto HMAC/PBKDF2/ECDH leaf probe

The bounded three-job Bun selector covered `crypto.hmac.test.ts`,
`pbkdf2.test.ts`, and `ecdh.test.ts`. It measured **3/3 files green**,
**126 passed / 0 failed / 126 ran / 226 expects**, and **0 runner timeouts**.
The runner used the default bounded profile of **4G memory / 512 tasks**; per-
file durations were 200–202ms.

`crypto.hmac.test.ts` passed **74/74**, including RFC 2202/4231 vectors,
KeyObject keys, streaming/finalization, and invalid digest/option guards.
`pbkdf2.test.ts` passed **38/38**, covering known derivations and invalid
keylen/iteration/digest inputs. `ecdh.test.ts` passed **14/14**, covering
supported curves, key formats, shared-secret agreement, conversion, and
invalid-key/private-key states. This is a high-value green crypto cluster with
no newly identified source owner.

No source or upstream fixture change was made. The selector reused the
coordinator binary through `tools/integration/bun_corpus_runner.py` with
three bounded jobs, a 30-second per-file timeout, and missing Node modules
allowed. No full corpus, build, or workspace-wide test was run.

## W275 Node module/punycode plain-script leaf probe

The bounded three-job Node selector covered `test-punycode.js`,
`test-module-create-require.js`, and `test-module-loading-error.js`. It
measured **2/3 file-level passes**, **1 failure**, and **0 runner timeouts**.
Per-file durations were 199–300ms.

`test-module-create-require.js` and `test-module-loading-error.js` passed.
`test-punycode.js` stopped at its first invalid-input assertion: the reference
expects `RangeError: Invalid input`, while mbun produced `RangeError: invalid`.
This is a narrow punycode error-message owner; later invalid-input cases in the
file were not counted after the first assertion failure.

No source or upstream fixture change was made. The selector reused the
coordinator binary through `tools/integration/node_corpus_runner.py` with
three bounded jobs and a 30-second per-file timeout. No full corpus, build,
or workspace-wide test was run. The Node corpus runner reports only file-level
status for these plain scripts, so no synthetic subtest count was added.

## W274 Node module/constants plain-script leaf probe

The bounded three-job Node selector covered `test-module-isBuiltin.js`,
`test-module-builtin.js`, and `test-constants.js`. It measured **2/3
file-level passes**, **1 failure**, and **0 runner timeouts**. Per-file
durations were 200–298ms.

`test-module-isBuiltin.js` and `test-module-builtin.js` passed. The constants
fixture failed at its first internal/public mapping comparison: an internal
constant had value `1` while the corresponding public `constants` entry was
`undefined`. This is one constants-export mapping owner; no module-loader or
mixed compatibility change was attempted.

No source or upstream fixture change was made. The selector reused the
coordinator binary through `tools/integration/node_corpus_runner.py` with
three bounded jobs and a 30-second per-file timeout. No full corpus, build,
or workspace-wide test was run. The Node corpus runner reports only file-level
status for these plain scripts, so no synthetic subtest count was added.

## W273 Bun Buffer safety/bounds leaf probe

The bounded three-job Bun selector covered `buffer-copy-fill-detach.test.ts`,
`buffer-indexOf-detach.test.ts`, and `buffer-compare-bounds.test.ts`. It
measured **2/3 files green**, **47 passed / 2 failed / 49 ran / 82 expects**,
and **0 runner timeouts**. The runner used the default bounded profile of **4G
memory / 512 tasks**; per-file durations were 198–2708ms.

`buffer-indexOf-detach.test.ts` and `buffer-compare-bounds.test.ts` were fully
green at **13/13** each. `buffer-copy-fill-detach.test.ts` passed **21/23**;
its two failures are the same Buffer.fill string-branch owner: when the
encoding argument is an object with a detaching/resizing `toString`, mbun
rejects it as a non-string instead of coercing it and preserving the expected
post-detach/post-resize behavior. All other copy/fill crash, resize, ordering,
and ordinary-argument guards in the file passed.

No source or upstream fixture change was made. The selector reused the
coordinator binary through `tools/integration/bun_corpus_runner.py` with
three bounded jobs, a 30-second per-file timeout, and missing Node modules
allowed. No full corpus, build, or workspace-wide test was run.

## W272 Node querystring pure-contract plain-script leaf probe

The bounded three-job Node selector covered `test-querystring-escape.js`,
`test-querystring-multichar-separator.js`, and
`test-querystring-maxKeys-non-finite.js`. It measured **3/3 file-level passes**,
**0 failures**, and **0 runner timeouts**. Per-file durations were 181–197ms.

The probe retained querystring value coercion and malformed-URI behavior,
multi-character key/value separators for parse/stringify, and the 10,000-key
non-finite `maxKeys` boundary. The Node corpus runner reports only file-level
status for these plain scripts, so no synthetic subtest count was added.

No source or upstream fixture change was made. The selector reused the
coordinator binary through `tools/integration/node_corpus_runner.py` with
three bounded jobs and a 30-second per-file timeout. No full corpus, build,
or workspace-wide test was run.

## W271 Bun Buffer/process/module leaf probe

The bounded three-job Bun selector covered `buffer-utf16.test.ts`,
`process-nexttick.test.js`, and `module-resolve-filename-paths.test.js`. It
measured **2/3 files green**, **9 passed / 5 failed / 14 ran / 21 expects**, and
**0 runner timeouts**. The runner used the default bounded profile of **4G
memory / 512 tasks**; per-file durations were 199–248ms.

`buffer-utf16.test.ts` passed **1/1**, and
`module-resolve-filename-paths.test.js` passed **6/6**, including package and
relative resolution through `options.paths` plus invalid-paths validation.
`process-nexttick.test.js` passed **2/7** and failed 5 tests: non-function
input validation, callback argument forwarding, `process.nextTick` versus
`queueMicrotask` ordering, and high-volume/repeated scheduling behavior. The
AsyncLocalStorage interaction in that file passed. These are separate
process-scheduler owners; no mixed fix was attempted.

No source or upstream fixture change was made. The selector reused the
coordinator binary through `tools/integration/bun_corpus_runner.py` with
three bounded jobs, a 30-second per-file timeout, and missing Node modules
allowed. No full corpus, build, or workspace-wide test was run.

## W270 Bun module/Buffer/DOMException leaf probe

The bounded three-job Bun selector covered `missing-module.test.js`,
`buffer-inspectmaxbytes.test.ts`, and `domexception-node.test.js`. It measured
**2/3 files green**, **8 passed / 1 failed / 9 ran / 44 expects**, and **0 runner
timeouts**. The runner used the default bounded profile of **4G memory / 512
tasks**; per-file durations were 200ms.

`buffer-inspectmaxbytes.test.ts` passed **1/1** and
`domexception-node.test.js` passed **7/7** (including its declared failing
case). `missing-module.test.js` stopped at its first assertion: for the
`node:missing` built-in, the reference requires `ERR_UNKNOWN_BUILTIN_MODULE`
and the `No such built-in module` message, while mbun returned a generic
missing-module error message. The remaining missing-module cases were not
counted after that first failure; this is one module-loader error-contract
owner, not a reason to mix in the passing Buffer/DOMException surfaces.

No source or upstream fixture change was made. The selector reused the
coordinator binary through `tools/integration/bun_corpus_runner.py` with
three bounded jobs, a 30-second per-file timeout, and missing Node modules
allowed. No full corpus, build, or workspace-wide test was run.

## W269 Node DNS lookup/resolver plain-script leaf probe

The bounded three-job Node selector covered `test-dns-lookup.js`,
`test-dns-lookupService.js`, and `test-dns-resolveany-bad-ancount.js`. It
measured **2/3 file-level passes**, **1 failure**, and **0 runner timeouts**.
Per-file durations were 300–351ms.

`test-dns-lookupService.js` passed the stubbed `getnameinfo` error contract for
callback and promise APIs. `test-dns-resolveany-bad-ancount.js` passed the
local malformed-DNS-answer handling for callback and promise resolvers.
`test-dns-lookup.js` failed at the `dns.lookup(false, { all: true })` guard:
the reference expects a synchronous `ERR_INVALID_ARG_VALUE`, but no exception
was raised. This is a separate invalid-hostname/all-mode validation owner; no
mixed DNS fix was attempted.

No source or upstream fixture change was made. The selector reused the
coordinator binary through `tools/integration/node_corpus_runner.py` with
three bounded jobs and a 30-second per-file timeout. No full corpus, build,
or workspace-wide test was run. The Node corpus runner reports only file-level
status for these plain scripts, so no synthetic subtest count was added.

## W268 Node DNS lookup/order/type plain-script leaf probe

The bounded three-job Node selector covered `test-dns-lookup-promises.js`,
`test-dns-resolvens-typeerror.js`, and `test-dns-set-default-order.js`. It
measured **3/3 file-level passes**, **0 failures**, and **0 runner timeouts**.
Per-file durations were 198–450ms.

`test-dns-lookup-promises.js` passed its c-ares stubbed positive and `ENOMEM`
rejection paths for promise lookup and lookup-all. The `resolveNs` invalid-name
and invalid-callback checks passed, as did default-result-order validation and
the `verbatim`/`ipv4first`/`ipv6first` propagation checks across callback and
promise lookup APIs. These are three isolated green DNS leaves with no newly
identified runtime owner.

No source or upstream fixture change was made. The selector reused the
coordinator binary through `tools/integration/node_corpus_runner.py` with
three bounded jobs and a 30-second per-file timeout. No full corpus, build,
or workspace-wide test was run. The Node corpus runner reports only file-level
status for these plain scripts, so no synthetic subtest count was added.

## W267 Node DNS promises/error-shape plain-script leaf probe

The bounded three-job Node selector covered `test-dns-promises-exists.js`,
`test-dns-resolve-promises.js`, and `test-dns-memory-error.js`. It measured
**1/3 file-level pass**, **2 failures**, and **0 runner timeouts**. Per-file
durations were 199–299ms.

`test-dns-resolve-promises.js` passed. `test-dns-promises-exists.js` failed
because `dnsPromises.NODATA` was `undefined` while the reference value was
`dns.NODATA === 'ENODATA'`. `test-dns-memory-error.js` failed on the expected
second stack-frame shape (`/^ {4}at Object/`). These remain two independent
runtime owners: the `dns/promises` constant surface and Node-shaped error-stack
formatting; no mixed fix was attempted.

No source or upstream fixture change was made. The selector reused the
coordinator binary through `tools/integration/node_corpus_runner.py` with
three bounded jobs and a 30-second per-file timeout. No full corpus, build,
or workspace-wide test was run. The two failing owners are parked for a
separate minimal reproduction; the passing resolver-promises leaf is retained.

## W266 Node HTTP framing/status/listening plain-script leaf probe

The bounded three-job Node selector covered `test-http-no-content-length.js`,
`test-http-status-message.js`, and `test-http-listening.js`. It measured **3/3
file-level passes**, **0 failures**, and **0 runner timeouts**. Per-file
durations were 200–250ms.

The probe retained HTTP response framing without an explicit Content-Length,
status-message behavior over a local connection, and server `listening` state
transitions. The Node corpus runner reports only file-level status for these
plain scripts, so no synthetic subtest count was added.

No source or upstream fixture change was made. The selector reused the
coordinator binary through `tools/integration/node_corpus_runner.py` with
three bounded jobs and a 30-second per-file timeout. No full corpus, build,
or workspace-wide test was run.

## W265 Node fs append/rename/stream-type plain-script leaf probe

The bounded three-job Node selector covered `test-fs-append-file-sync.js`,
`test-fs-rename-type-check.js`, and
`test-fs-write-stream-throw-type-error.js`. It measured **3/3 file-level
passes**, **0 failures**, and **0 runner timeouts**. Per-file durations were
199–201ms.

The probe retained synchronous append behavior for text, buffers, modes, and
file descriptors; rename argument type validation; and createWriteStream
invalid-options TypeErrors. The Node corpus runner reports only file-level
status for these plain scripts, so no synthetic subtest count was added.

No source or upstream fixture change was made. The selector reused the
coordinator binary through `tools/integration/node_corpus_runner.py` with
three bounded jobs and a 30-second per-file timeout. No full corpus, build,
or workspace-wide test was run.

## W264 Bun spyMatchers/pretty-format/test.failing leaf probe

The bounded three-job Bun selector covered `spyMatchers.test.ts`,
`pretty-format-overflow.test.ts`, and `test-failing.test.ts`. It measured
**1/3 files green**, with **130 passed / 24 failed / 159 ran / 494 expects / 0
runner timeouts**. Per-file durations were 197–750ms.

`pretty-format-overflow.test.ts` passed its single test for deeply nested diff
formatting without a crash. `spyMatchers.test.ts` passed 124 tests, had 21
failures and 5 todos; failures clustered around expected matcher-error throws,
optional/trailing-undefined argument semantics, returned-call bookkeeping,
negative nth validation, and incomplete recursive calls.

`test-failing.test.ts` passed 5/8 tests. Its three failures split into the
`test.failing` non-function error message, expected-failure output formatting,
and a timeout fixture using unavailable `jest.setTimeout`. No mixed fix was
attempted.

No source or upstream fixture change was made. The selector reused the
coordinator binary through `tools/integration/bun_corpus_runner.py` with
three bounded jobs, a 30-second per-file timeout, and missing Node modules
allowed. No full corpus, build, or workspace-wide test was run.

## W263 Node stream append/backpressure/order plain-script leaf probe

The bounded three-job Node selector covered
`test-stream-readable-add-chunk-during-data.js`, `test-stream-backpressure.js`,
and `test-stream-push-order.js`. It measured **3/3 file-level passes**, **0
failures**, and **0 runner timeouts**. Per-file durations were 182–183ms.

The probe retained adding data during a Readable `data` event, Writable
backpressure completion, and deterministic Readable push ordering. The Node
corpus runner reports only file-level status for these plain scripts, so no
synthetic subtest count was added.

No source or upstream fixture change was made. The selector reused the
coordinator binary through `tools/integration/node_corpus_runner.py` with
three bounded jobs and a 30-second per-file timeout. No full corpus, build,
or workspace-wide test was run.

## W262 Node crypto Certificate/DH/keygen plain-script leaf probe

The bounded three-job Node selector covered `test-crypto-certificate.js`,
`test-crypto-dh-modp2.js`, and
`test-crypto-keygen-empty-passphrase-no-prompt.js`. It measured **3/3
file-level passes**, **0 failures**, and **0 runner timeouts**. Per-file
durations were 202–252ms.

The probe retained Certificate fixture parsing and API checks, the `modp2`
Diffie-Hellman group contract, and key generation with an empty passphrase
without an interactive prompt. The Node corpus runner reports only file-level
status for these plain scripts, so no synthetic subtest count was added.

No source or upstream fixture change was made. The selector reused the
coordinator binary through `tools/integration/node_corpus_runner.py` with
three bounded jobs and a 30-second per-file timeout. No full corpus, build,
or workspace-wide test was run.

## W261 Node Buffer iterator/read/allocation plain-script leaf probe

The bounded three-job Node selector covered `test-buffer-iterator.js`,
`test-buffer-read.js`, and `test-buffer-no-negative-allocation.js`. It measured
**3/3 file-level passes**, **0 failures**, and **0 runner timeouts**. Per-file
durations were 200–201ms.

The probe retained Buffer iterator behavior across values and offsets, read
boundary/error handling, and negative/NaN allocation validation across the
safe and unsafe allocation APIs. The Node corpus runner reports only
file-level status for these plain scripts, so no synthetic subtest count was
added.

No source or upstream fixture change was made. The selector reused the
coordinator binary through `tools/integration/node_corpus_runner.py` with
three bounded jobs and a 30-second per-file timeout. No full corpus, build,
or workspace-wide test was run.

## W260 Bun hooks/custom matcher/mock-fn leaf probe

The bounded three-job Bun selector covered `jest-hooks.test.ts`,
`expect-extend.test.js`, and `mock-fn.test.js`. It measured **2/3 files
green**, with **78 passed / 34 failed / 113 ran / 20,472 expects / 0 runner
timeouts**. Per-file durations were 182–232ms.

`expect-extend.test.js` passed all 28 tests with 20,088 expects, covering
custom matcher context, asymmetric matchers, async results, invalid matcher
errors, prototypes/classes, and intensive use. `jest-hooks.test.ts` passed 17
tests with 1 todo and no failures, covering nested, async, and done-callback
hook ordering.

`mock-fn.test.js` passed 33/67 tests and failed 34. The failures cluster around
mock metadata/invalid-this behavior, return and call bookkeeping, missing
`withImplementation`/`getMockImplementation`/`invocationCallOrder` APIs,
reset/restore behavior, and `spyOn` identity/indexed-property semantics. No
mixed fix was attempted.

No source or upstream fixture change was made. The selector reused the
coordinator binary through `tools/integration/bun_corpus_runner.py` with
three bounded jobs, a 30-second per-file timeout, and missing Node modules
allowed. No full corpus, build, or workspace-wide test was run.

## W259 Node child-process IPC/exec plain-script leaf probe

The bounded three-job Node selector covered
`test-child-process-disconnect.js`, `test-child-process-send-returns-boolean.js`,
and `test-child-process-exec-encoding.js`. It measured **2/3 file-level
passes**, **1 failure**, and **0 runner timeouts**. Per-file durations were
201–602ms.

`test-child-process-disconnect.js` and `test-child-process-exec-encoding.js`
both clean-exited, retaining deferred disconnect/self-termination behavior
and string/Buffer encoding behavior for `exec()` output.

`test-child-process-send-returns-boolean.js` failed when the first IPC send
with a server handle threw an unsupported-handle `TypeError` instead of
entering the expected boolean/backlog sequence. This is tracked as one IPC
server-handle transfer owner; no mixed fix was attempted.

The Node corpus runner reports only file-level status for these plain scripts,
so no synthetic subtest count was added. No source or upstream fixture change
was made. No full corpus, build, or workspace-wide test was run.

## W258 Node child-process stdio/destroy plain-script leaf probe

The bounded three-job Node selector covered
`test-child-process-stdio-inherit.js`, `test-child-process-flush-stdio.js`,
and `test-child-process-destroy.js`. It measured **3/3 file-level passes**,
**0 failures**, and **0 runner timeouts**. Per-file durations were 199–352ms.

The probe retained child stdio inheritance, flush behavior for readable and
writable stdio, and destroy/kill state transitions. The Node corpus runner
reports only file-level status for these plain scripts, so no synthetic
subtest count was added.

No source or upstream fixture change was made. The selector reused the
coordinator binary through `tools/integration/node_corpus_runner.py` with
three bounded jobs and a 30-second per-file timeout. No full corpus, build,
or workspace-wide test was run.

## W257 Bun mock.module/re-export leaf probe

The bounded three-job Bun selector covered `mock-module.test.ts`,
`mock/6879/6879.test.ts`, and `mock-module-resolve-log.test.ts`. It measured
**2/3 files green**, with **6 passed / 6 failed / 13 ran / 32 expects / 0
runner timeouts**. Per-file durations were 184–187ms.

`mock/6879/6879.test.ts` passed both tests for export-list and named re-export
mocking. `mock-module-resolve-log.test.ts` passed its single regression test
for a non-existent specifier being mocked without a resolver crash.

`mock-module.test.ts` passed 3/10 executable tests, with 6 failures and 1
todo. The failures split into four owners: async mock result was `undefined`
instead of 123; `mock.restore` did not restore the original spy identity;
relative file and file-URL mocks failed for two non-existent paths; and later
local/package mock updates retained 42 instead of the expected 43. No mixed
fix was attempted.

No source or upstream fixture change was made. The selector reused the
coordinator binary through `tools/integration/bun_corpus_runner.py` with
three bounded jobs, a 30-second per-file timeout, and missing Node modules
allowed. No full corpus, build, or workspace-wide test was run.

## W256 Node fs/http/url plain-script leaf probe

The bounded three-job Node selector covered
`test-fs-promises-writefile-typedarray.js`, `test-http-header-validators.js`,
and `test-url-invalid-file-url-path-input.js`. It measured **3/3 file-level
passes**, **0 failures**, and **0 runner timeouts**. Per-file durations were
199–250ms.

The probe retained TypedArray write support through `fs.promises.writeFile`,
HTTP header-name/value validation including invalid-character cases, and
invalid `file:` URL path rejection. The Node corpus runner reports only
file-level status for these plain scripts, so no synthetic subtest count was
added.

No source or upstream fixture change was made. The selector reused the
coordinator binary through `tools/integration/node_corpus_runner.py` with
three bounded jobs and a 30-second per-file timeout. No full corpus, build,
or workspace-wide test was run.

## W255 Bun retry/jest-each/fake-timers leaf probe

The bounded three-job Bun selector covered `test-retry-repeats-basic.test.ts`,
`jest-each.test.ts`, and `test-timers.test.ts`. It measured **2/3 files
green**, with **40 passed / 4 failed / 44 ran / 61 expects / 0 runner
timeouts**. Per-file durations were 200–652ms.

`jest-each.test.ts` passed all 25 tests, covering parameter formatting,
callback parameters, object cases, `describe.each`, and generated test names.
`test-retry-repeats-basic.test.ts` passed all 12 tests, covering retry and
repeat counts, hook order, `onTestFinished`, and inner `afterAll` behavior.

`test-timers.test.ts` passed 3/7 tests. One failure showed
`Intl.DateTimeFormat().format()` remaining on the real current date after the
fake clock was set. Three failures came from child evaluations expecting
`jest.useFakeTimers()` to complete and print `ok`, but the API was unavailable
there. The two groups were kept as separate owners; no mixed fix was attempted.

No source or upstream fixture change was made. The selector reused the
coordinator binary through `tools/integration/bun_corpus_runner.py` with three
bounded jobs, a 30-second per-file timeout, and missing Node modules allowed.
No full corpus, build, or workspace-wide test was run.

## W254 Bun test-runner hook/scope leaf probe

The bounded three-job Bun selector covered `nested-describes.test.ts`,
`failure-skip.test.ts`, and `test-on-test-finished.test.ts`. It measured
**2/3 files green**, with **15 passed / 11 failed / 26 ran / 21 expects / 0
runner timeouts**. Per-file durations were 199–1402ms.

`nested-describes.test.ts` passed all 3 tests for nested scope execution and
`test-on-test-finished.test.ts` passed all 12 tests for ordering, async
callbacks, concurrent-test rejection, and failing-test cleanup.

`failure-skip.test.ts` had 0/11 outer tests pass: every snapshot received an
empty child-runner stdout instead of its expected hook trace. The failures
share one nested child-runner/fixture-output integration owner; hook ordering
was not treated as independently disproven and no mixed fix was attempted.

No source or upstream fixture change was made. The selector reused the
coordinator binary through `tools/integration/bun_corpus_runner.py` with
three bounded jobs, a 30-second per-file timeout, and missing Node modules
allowed. No full corpus, build, or workspace-wide test was run.

## W253 Node events/listener plain-script probe

The bounded three-job Node selector covered `test-events-list.js`,
`test-events-listener-count-with-listener.js`, and
`test-eventsource-disabled.js`. All **3/3 files passed**, with **0 failures
and 0 timeouts**. The Node runner reports these plain scripts as file-level
pass/fail units, so no synthetic subtest or expect totals are reported.
Per-file durations were 200–201ms.

The passing guards cover EventEmitter `eventNames()` ordering and Symbol
names, listener-count filtering by function, and the disabled global
`EventSource` contract.

No source or upstream fixture change was made. The selector reused the
coordinator binary through `tools/integration/node_corpus_runner.py` with
three bounded jobs and a 30-second per-file timeout. No full corpus, build,
or workspace-wide test was run.

## W252 Bun base64/highlighter/UUID leaf probe

The bounded three-job Bun selector covered `base64-url-safe-encode.test.ts`,
`highlighter.test.ts`, and `randomUUIDv7.test.ts`. It measured **2/3 files
green**, with **28 passed / 11 failed / 39 ran / 575 expects / 0 runner
timeouts**. Per-file durations were 252–855ms.

`base64-url-safe-encode.test.ts` passed all 5 tests, including scalar RFC
4648 reference vectors through length 513, large byte-exact output, Node
crypto, and Bun CryptoHasher. `highlighter.test.ts` passed all 16 tests,
including end-of-input safety, redacting highlighter behavior, and bunfig
error handling.

`randomUUIDv7.test.ts` passed 7/18 tests. Its 11 failures split into four
semantic owners: 12-bit counter rollover ordering, invalid timestamp/range
validation and error shape, older explicit timestamp ordering, and
per-millisecond counter seeding. No mixed UUID patch was attempted.

No source or upstream fixture change was made. The selector reused the
coordinator binary through `tools/integration/bun_corpus_runner.py` with
three bounded jobs, a 30-second per-file timeout, and missing Node modules
allowed. No full corpus, build, or workspace-wide test was run.

## W251 Node process exec/argv/umask plain-script probe

The bounded three-job Node selector covered `test-process-execpath.js`,
`test-process-argv-0.js`, and `test-process-umask.js`. All **3/3 files passed**,
with **0 failures and 0 timeouts**. The Node runner reports these plain
scripts as file-level pass/fail units, so no synthetic subtest or expect totals
are reported. Per-file durations were 198–298ms.

The passing guards cover resolving `process.execPath` through a symlink,
child-process `argv[0]` behavior, and umask read/write restoration plus
invalid object/string validation. This complements W250's mask-coercion leaf
without duplicating it.

No source or upstream fixture change was made. The selector reused the
coordinator binary through `tools/integration/node_corpus_runner.py` with
three bounded jobs and a 30-second per-file timeout. No full corpus, build,
or workspace-wide test was run.

## W250 Node process queue/mask/CPU plain-script probe

The bounded three-job Node selector covered `test-process-next-tick.js`,
`test-process-umask-mask.js`, and `test-process-cpuUsage.js`. All **3/3 files
passed**, with **0 failures and 0 timeouts**. The Node runner reports these
plain scripts as file-level pass/fail units, so no synthetic subtest or expect
totals are reported. Per-file durations were 200–251ms.

The passing guards cover `process.nextTick` callback/error propagation,
numeric and octal-string `process.umask` mask handling, and `process.cpuUsage`
result shape plus invalid argument validation.

No source or upstream fixture change was made. The selector reused the
coordinator binary through `tools/integration/node_corpus_runner.py` with
three bounded jobs and a 30-second per-file timeout. No full corpus, build,
or workspace-wide test was run.

## W249 Node process identity/memory plain-script probe

The bounded three-job Node selector covered `test-process-ppid.js`,
`test-process-release.js`, and `test-process-available-memory.js`. All **3/3
files passed**, with **0 failures and 0 timeouts**. The Node runner reports
these plain scripts as file-level pass/fail units, so no synthetic subtest or
expect totals are reported. Per-file durations were 200–351ms.

The passing guards cover parent-process identity across a child invocation,
the Node release name/LTS-version contract, and the numeric shape of
`process.availableMemory()`.

No source or upstream fixture change was made. The selector reused the
coordinator binary through `tools/integration/node_corpus_runner.py` with
three bounded jobs and a 30-second per-file timeout. No full corpus, build,
or workspace-wide test was run.

## W248 Node console assignment/error plain-script probe

The bounded three-job Node selector covered `test-console-assign-undefined.js`,
`test-console-self-assign.js`, and `test-console-log-throw-primitive.js`. All
**3/3 files passed**, with **0 failures and 0 timeouts**. The Node runner
reports these plain scripts as file-level pass/fail units, so no synthetic
subtest or expect totals are reported. Per-file durations were 199–201ms.

The passing guards cover replacing the global Console binding with primitive
values and restoring it, self-assignment of the global Console binding, and
Console logging through a Writable whose write path throws a primitive.

No source or upstream fixture change was made. The selector reused the
coordinator binary through `tools/integration/node_corpus_runner.py` with
three bounded jobs and a 30-second per-file timeout. No full corpus, build,
or workspace-wide test was run.

## W247 Node util.deprecate/inspect plain-script probe

The bounded three-job Node selector covered
`test-util-deprecate-invalid-code.js`, `test-util-deprecate.js`, and
`test-util-primordial-monkeypatching.js`. All **3/3 files passed**, with
**0 failures and 0 timeouts**. The Node runner reports these plain scripts as
file-level pass/fail units, so no synthetic subtest or expect totals are
reported. Per-file durations were 199–349ms.

The passing guards cover invalid `util.deprecate` code validation, one-time
deprecation warning and prototype behavior, and `util.inspect` remaining
stable while `Object.keys` is monkeypatched. No source owner was exposed.

No source or upstream fixture change was made. The selector reused the
coordinator binary through `tools/integration/node_corpus_runner.py` with
three bounded jobs and a 30-second per-file timeout. No full corpus, build,
or workspace-wide test was run.

## W246 Node console/util plain-script probe

The bounded three-job Node selector covered `test-console-methods.js`,
`test-console-stdio-setters.js`, and `test-util-inherits.js`. All **3/3 files
passed**, with **0 failures and 0 timeouts**. The Node runner reports these
plain scripts as file-level pass/fail units, so no synthetic subtest or expect
totals are reported. Per-file durations were 198–199ms.

The passing guards cover non-constructible Console methods and method names,
global Console stdio setter routing, and multi-level `util.inherits` prototype
and constructor relationships. These are independent of the W245 multiline
object pretty-print owner.

No source or upstream fixture change was made. The selector reused the
coordinator binary through `tools/integration/node_corpus_runner.py` with
three bounded jobs and a 30-second per-file timeout. No full corpus, build,
or workspace-wide test was run.

## W245 Node console/process plain-script probe

The bounded three-job Node selector covered `test-console-count.js`,
`test-console-group.js`, and `test-process-uptime.js`. It measured **2/3 files
pass, 1/3 fail, and 0 timeouts**; the Node runner reports these plain scripts
as file-level pass/fail units, so no synthetic subtest or expect totals are
reported. Per-file durations were 198–250ms.

`test-console-count.js` and `test-process-uptime.js` exited cleanly.
`test-console-group.js` reached its multiline object indentation assertion:
the observed output kept the object on one line, while Node expects a
property-per-line indented rendering. Keep this as one Console formatting
owner; no mixed console patch was attempted.

No source or upstream fixture change was made. The selector reused the
coordinator binary through `tools/integration/node_corpus_runner.py` with
three bounded jobs and a 30-second per-file timeout. No full corpus, build,
or workspace-wide test was run.

## W244 Bun test matcher leaf probe

The bounded three-job Bun selector covered `expect-label.test.ts`,
`expect-assertions.test.ts`, and `expect/toHaveReturnedWith.test.ts`. All
**3/3 files were green**, with **45 passed / 0 failed / 45 ran / 92 expects /
0 runner timeouts**. Per-file durations were 199–350ms.

`expect-label.test.ts` passed all 3 tests for labeled `toBe`/`toEqual`
diagnostics and non-string labels. `expect-assertions.test.ts` passed its
outer 1-test guard; its child runner intentionally produced **0 pass / 5
fail** for the under-asserted sync, async, callback, `setImmediate`, and
`queueMicrotask` cases, confirming the expected failure accounting. The
41-test return matcher file passed all `toHaveReturnedWith` and
`toHaveLastReturnedWith` success, failure, edge, and comparison cases.

No source or upstream fixture change was made. The selector reused the
coordinator binary through `tools/integration/bun_corpus_runner.py` with
three bounded jobs, a 30-second per-file timeout, and missing Node modules
allowed. No full corpus, build, or workspace-wide test was run.

## W243 Bun Web Fetch/Response leaf probe

The bounded three-job Bun selector covered `response.test.ts`,
`body-mixin-errors.test.ts`, and `fetch-args.test.ts`. All **3/3 files were
green**, with **86 passed / 0 failed / 86 ran / 192 expects / 0 runner
timeouts**. Per-file durations were 182–283ms.

`response.test.ts` passed all 23 tests covering empty and initialized
responses, redirect status/URL/header behavior, stack-overflow handling, and
clone body locking/readability. `body-mixin-errors.test.ts` passed both
Response and Request body-used TypeError guards. `fetch-args.test.ts` passed
all 61 tests covering Request subclasses, deferred option-conversion
rejections, invalid-request no-send guards, and getter/error propagation.

No source or upstream fixture change was made. The selector reused the
coordinator binary through `tools/integration/bun_corpus_runner.py` with
three bounded jobs, a 30-second per-file timeout, and missing Node modules
allowed. No full corpus, build, or workspace-wide test was run.

## W242 Bun spawn/mock leaf probe

The bounded three-job Bun selector covered `spawn-large-array-length`,
`mock-disposable`, and `mock-module-non-string`. All **3/3 files were green**,
with **11 passed / 0 failed / 11 ran / 45 expects / 0 runner timeouts**.
Per-file durations were 199–350ms.

`spawn-large-array-length` passed all 3 tests for spoofed near-u32-max command
array lengths and the normal-array control. `mock-disposable` passed all 3
tests for `spyOn`/`mock` disposal and automatic prototype restoration.
`mock-module-non-string` passed all 5 tests covering non-string argument
validation, valid string mocking, malformed specifiers, missing callbacks,
and resolver short-circuit behavior.

No source or upstream fixture change was made. The selector reused the
coordinator binary through `tools/integration/bun_corpus_runner.py` with
three bounded jobs, a 30-second per-file timeout, and missing Node modules
allowed. No full corpus, build, or workspace-wide test was run.

## W241 Bun Node HTTP leaf probe

The bounded three-job Bun selector covered `node-http-maxHeaderSize`,
`node-http-primoridals`, and `node-http-proxy-url`. It measured **2/3 files
green**, with **7 passed / 1 failed / 8 ran / 13 expects / 0 runner
timeouts**. Per-file durations were 251–652ms.

`node-http-maxHeaderSize` passed all 4 tests, including runtime header limits
and `--max-http-header-size` child-process validation. The HTTP primordials
guard passed its 1 test while replacing global `Request`, `Response`,
`Headers`, and `Blob`. The proxy URL file passed its 2 nested subprocess
checks, but its CR/LF host validation check received `no-error` instead of
`ERR_INVALID_CHAR`; keep that as a focused proxy-agent host-validation owner.

No source or upstream fixture change was made. The selector reused the
coordinator binary through `tools/integration/bun_corpus_runner.py` with
three bounded jobs, a 30-second per-file timeout, and missing Node modules
allowed. No full corpus, build, or workspace-wide test was run.

## W133 Bun.Terminal green cluster

The bounded three-job selector covered the core terminal contract, explicit
POSIX/Windows platform gaps, and terminal subprocess integration. All **3/3
files were green**, reaching **129 passed / 0 failed / 130 ran / 334 expects**.
Per-file durations were 1.233–3.694s, with no timeout.

No source or upstream fixture change was made. The selector used the existing
coordinator binary through `tools/integration/bun_corpus_runner.py` with three
bounded jobs, a 30-second per-file timeout, and missing Node modules allowed.
No full corpus or workspace-wide test was run; the selector and raw runner
output were removed after recording the result.

## W217 Node assert owner split

The bounded three-job Node selector covered Assert class destructuring,
Error `cause` deep equality, and TypedArray/ArrayBuffer deep equality. The
file-level runner result was **0/3 pass: 3 failures / 0 timeouts**; per-file
durations were 199–1604ms. This runner reports Node corpus files as pass/fail
units because the upstream files are plain scripts, so no subtest pass count is
invented here.

The Assert class file stops because `assert.Assert` is not a constructor. The
Error-cause file reaches comparison but differs in Node's expected diagnostic
message/stack formatting. The TypedArray file reaches its loose/not-equal
cases but several expected `AssertionError` throws do not occur. These are
separate owners; no mixed assertion patch was attempted.

The first cwd-relative selector was rejected before dispatch and ran zero
tests; it was corrected to repository-root-relative paths before the measured
probe. No source or upstream fixture change was made. The final selector used
the existing coordinator binary through `tools/integration/node_corpus_runner.py`
with three bounded jobs and a 30-second per-file timeout. No full corpus or
workspace-wide test was run; the selector and raw runner output were removed
after recording the result.

## W223 Node Buffer float green cluster

The bounded three-job Node selector covered Float32 and Float64 big/little
endian reads and writes, including out-of-bounds and range guards. All **3/3
files passed**, with **0 failures and 0 timeouts**; each file took 199ms.

These upstream files are plain scripts, so the Node runner's file-level
clean-exit classification is the authoritative result. No source or upstream
fixture change was made. The selector used the existing coordinator binary
through `tools/integration/node_corpus_runner.py` with three bounded jobs and a
30-second per-file timeout. No full corpus or workspace-wide test was run; the
selector and raw runner output were removed after recording the result.

## W224 Node Buffer write green cluster

The bounded three-job Node selector covered generic encoding/range writes,
Double big/little endian writes, and UInt big/little endian offset/OOB writes.
All **3/3 files passed**, with **0 failures and 0 timeouts**; per-file
durations were 200–201ms.

These upstream files are plain scripts, so the Node runner's file-level
clean-exit classification is the authoritative result. No source or upstream
fixture change was made. The selector used the existing coordinator binary
through `tools/integration/node_corpus_runner.py` with three bounded jobs and a
30-second per-file timeout. No full corpus or workspace-wide test was run; the
selector and raw runner output were removed after recording the result.

## W225 Node Buffer read/string green cluster

The bounded three-job Node selector covered basic numeric reads, `toString`
range/coercion and empty boundaries, and JSON Buffer serialization. All **3/3
files passed**, with **0 failures and 0 timeouts**; per-file durations were
199–200ms.

These upstream files are plain scripts, so the Node runner's file-level
clean-exit classification is the authoritative result. No source or upstream
fixture change was made. The selector used the existing coordinator binary
through `tools/integration/node_corpus_runner.py` with three bounded jobs and a
30-second per-file timeout. No full corpus or workspace-wide test was run; the
selector and raw runner output were removed after recording the result.

## W226 Node crypto green cluster

The bounded three-job Node selector covered cipher encoding validation,
`getCipherInfo` lookup/type/range behavior, and RSA-OAEP empty-payload
round-trips. All **3/3 files passed**, with **0 failures and 0 timeouts**;
per-file durations were 231–233ms.

These upstream files are plain scripts, so the Node runner's file-level
clean-exit classification is the authoritative result. No source or upstream
fixture change was made. The selector used the existing coordinator binary
through `tools/integration/node_corpus_runner.py` with three bounded jobs and a
30-second per-file timeout. No full corpus or workspace-wide test was run; the
selector and raw runner output were removed after recording the result.

## W227 Node crypto partial cluster

The bounded three-job Node selector covered HKDF sync/async and input-boundary
behavior, KeyObject native-brand and forged-prototype guards, and random-fill
argument validation. **2/3 files passed**, with **1 failure and 0 timeouts**;
per-file durations were 198–300ms.

HKDF and KeyObject brand-check passed cleanly. The random leaf stopped at the
upstream assertion that `randomFillSync` rejects a string `offset`, identifying
a focused `crypto.randomFill*` offset/size type-validation owner. These
upstream files are plain scripts, so file-level clean exit is authoritative.
No source or fixture change was made, and no full corpus or workspace-wide test
was run; the selector and raw runner output were removed after recording the
result.

## W240 Node stream state/destroy green cluster

The bounded three-job Node selector covered repeated Readable pause/resume with
drain/backpressure behavior and Writable normal/error/custom-destroy lifecycle
ordering. Both fresh files passed (**2/2**, **0 failures**, **0 timeouts**) with
per-file durations of 198–199ms; two fresh files were sufficient after
excluding already-recorded stream leaves.

These plain upstream scripts were classified at file-level clean exit. No
source or upstream fixture change was made, and no full corpus or workspace-wide
test was run; the selector and raw runner output were removed after recording
the result.

## W239 Node stream pipeline/state green cluster

The bounded three-job Node selector covered pipeline listener cleanup and
uncaught error delivery, Writable `needDrain` state across buffered writes, and
`writableCorked` nested cork/uncork transitions. All **3/3 files passed**, with
**0 failures and 0 timeouts**; per-file durations were 198–299ms.

These plain upstream scripts were classified at file-level clean exit. No
source or upstream fixture change was made, and no full corpus or workspace-wide
test was run; the selector and raw runner output were removed after recording
the result.

## W238 Node stream lifecycle green cluster

The bounded three-job Node selector covered destruction during a
`Readable.fromWeb`/`Readable.toWeb` data delivery, Writable cork-buffer
accounting, and Duplex default/`allowHalfOpen: false` end behavior. All **3/3
files passed**, with **0 failures and 0 timeouts**; per-file durations were
198–200ms.

These plain upstream scripts were classified at file-level clean exit. No
source or upstream fixture change was made, and no full corpus or workspace-wide
test was run; the selector and raw runner output were removed after recording
the result.

## W237 Bun Node stream partial cluster

The bounded three-job Bun selector covered the broad Node stream suite and a
Uint8Array-focused stream suite. **1/2 files was green**, reaching **92 passed /
6 failed / 104 ran / 165 expects** with **0 runner timeouts**; the Uint8Array
file passed 5/5 and the broad file passed 87/99 in about 8.2s.

The six failures split into stdin piping subprocess behavior, three Web/Node
stream cancellation or abort-reason cases, direct-byte ReadableStream
consumption by the Bun.serve sink, and `require.resolve.paths` agreement for
gated stream/iterator specifiers. No source or upstream fixture change was
made, and no full corpus or workspace-wide test was run; the selector and raw
runner output were removed after recording the result.

## W236 Bun timers partial cluster

The initial bounded three-job Bun selector included one Bun timers suite and
two Node-style plain scripts. The latter were correctly classified as
`no-tests` and excluded. The authoritative counted result was **1/1 file, 18
passed / 2 failed / 20 ran / 31 expects**, with **0 runner timeouts**; the
counted file completed in 784ms.

The failures were a UTF-16 timer-id string-classification mismatch and an
immediate-exception fixture subprocess contract mismatch. No source or
upstream fixture change was made, and no full corpus or workspace-wide test was
run; selector and raw runner output were removed after recording the result.

## W235 Node timers green cluster

The bounded three-job Node selector covered zero-timeout and interval argument
delivery/cancellation, repeated `clearImmediate`, and timeout/immediate/
interval callback receiver semantics. All **3/3 files passed**, with **0
failures and 0 timeouts**; per-file durations were 198–199ms.

These plain upstream scripts were classified at file-level clean exit. No
source or upstream fixture change was made, and no full corpus or workspace-wide
test was run; the selector and raw runner output were removed after recording
the result.

## W234 Bun Node util partial cluster

The bounded three-job Bun selector covered `util.promisify`, `util.callbackify`,
and the broader Node util contract. **2/3 files were green**, reaching **297
passed / 2 failed / 300 ran / 559 expects** with **0 timeouts**; per-file
durations were 199–249ms.

`util.promisify` recorded 16 passes across 17 ran tests (one upstream skip), and
`util.callbackify` recorded 90/90 passes. The broader util file recorded 191
passes and two failures, both ANSI `styleText` colorization cases affected by
the runner's color-disabled policy. No source or upstream fixture change was
made, and no full corpus or workspace-wide test was run; the selector and raw
runner output were removed after recording the result.

## W233 Node path/querystring/URL green cluster

The bounded three-job Node selector covered POSIX/Win32 `path.parse` and
`path.format`, prototype-safe querystring parsing/stringifying and limits, and
legacy URL parse/format behavior. All **3/3 files passed**, with **0 failures
and 0 timeouts**; per-file durations were 200–250ms.

These plain upstream scripts were classified at file-level clean exit. No
source or upstream fixture change was made, and no full corpus or workspace-wide
test was run; the selector and raw runner output were removed after recording
the result.

## W232 Node string/events/URL owner split

The bounded three-job Node selector covered StringDecoder receiver validation,
`events.once` option errors, and legacy `url.parse` query handling. **1/3 files
passed**, with **2 failures and 0 timeouts**; per-file durations were 198–401ms.

The URL query leaf passed. `events.once` invalid options produced an error
without the expected `ERR_INVALID_ARG_TYPE` code, while a forged
`StringDecoder.prototype.write` receiver did not throw the expected
`ERR_INVALID_THIS`. Both are focused API semantic owners; no source or
upstream fixture change was made, and no full corpus or workspace-wide test was
run. The selector and raw runner output were removed after recording the
result.

## W231 Bun Node util/events/string_decoder green cluster

The bounded three-job Bun selector covered EventEmitter lifecycle/async/error
handling, StringDecoder encoding and partial-sequence state, and `util.types`
brand/cross-import checks. All **3/3 files were green**, reaching **214 passed /
0 failed / 214 ran / 6538 expects** with **0 timeouts**; per-file durations were
200–451ms.

No source or upstream fixture change was made, and no full corpus or
workspace-wide test was run; the selector and raw runner output were removed
after recording the result.

## W230 Bun Node path/URL green cluster

The bounded three-job Bun selector covered `path.basename` platform/Win32/POSIX
cases, `path.extname` general/Win32/POSIX cases, and WHATWG `url.format`. All
**3/3 files were green**, reaching **8 passed / 0 failed / 8 ran / 0 expects**;
there were no timeouts and each file completed in about 200ms.

No source or upstream fixture change was made, and no full corpus or
workspace-wide test was run; the selector and raw runner output were removed
after recording the result.

## W229 Bun Node URL/path green cluster

The final bounded three-job Bun selector covered `path.parse`/`path.format`,
zero-length path strings, and the legacy `url.parse` query-object prototype
guard. All **3/3 files were green**, reaching **4 passed / 0 failed / 5 ran / 2
expects** with no timeout; per-file durations were 202–302ms. The URL file
retained one upstream TODO and no failure.

An initial querystring candidate was excluded because the Bun runner correctly
classified the plain script as `no-tests`; it is not counted as coverage. No
source or upstream fixture change was made, and no full corpus or workspace-wide
test was run; the selector and raw runner output were removed after recording
the result.

## W228 Node crypto/WebCrypto green cluster

The bounded three-job Node selector covered KeyObject own-string/Symbol key
guards after metadata access, AES-GCM WebCrypto empty-payload round-trip, and
short-tag decrypt rejection. All **3/3 files passed**, with **0 failures and 0
timeouts**; per-file durations were 199–200ms.

These upstream files are plain scripts, so the Node runner's file-level
clean-exit classification is authoritative. No source or upstream fixture
change was made, and no full corpus or workspace-wide test was run; the
selector and raw runner output were removed after recording the result.

## W222 Node Buffer numeric green cluster

The bounded three-job Node selector covered signed and unsigned integer reads,
plus signed integer writes, including type/OOB/range errors and endianness.
All **3/3 files passed**, with **0 failures and 0 timeouts**; per-file
durations were 201–202ms.

These upstream files are plain scripts, so the Node runner's file-level
clean-exit classification is the authoritative result. No source or upstream
fixture change was made. The selector used the existing coordinator binary
through `tools/integration/node_corpus_runner.py` with three bounded jobs and a
30-second per-file timeout. No full corpus or workspace-wide test was run; the
selector and raw runner output were removed after recording the result.

## W221 Node Buffer continuation green cluster

The bounded three-job Node selector covered malformed-hex writes, signed and
unsigned 64-bit BigInt read/write boundaries, and ArrayBuffer sharing with
offset/length validation. All **3/3 files passed**, with **0 failures and 0
timeouts**; each file took about 200ms.

These upstream files are plain scripts, so the Node runner's file-level
clean-exit classification is the authoritative result. No source or upstream
fixture change was made. The selector used the existing coordinator binary
through `tools/integration/node_corpus_runner.py` with three bounded jobs and a
30-second per-file timeout. No full corpus or workspace-wide test was run; the
selector and raw runner output were removed after recording the result.

## W220 Bun stack/stdio/HTTP owner split

The bounded three-job selector covered V8-style stack/call-frame behavior,
stdio write-after-end lifecycle, and HTTP proxy-style absolute URLs plus
invalid-host validation. It reached **21 passed / 29 failed / 50 ran / 114
expects / 0 runner timeouts**; per-file durations were 582–5645ms. One stack
subtest reported its own 5-second test timeout, but the bounded runner itself
did not time out.

The HTTP file was **2/3**: normal proxy-style request paths passed, while the
CR/LF host case returned no `ERR_INVALID_CHAR`. The stdio file was **0/4**:
pipe-backed writes did not transition to Node's post-end error state, and
file-backed runs ended before producing the expected report. The stack file was
**19/43**; failures split across stack/frame formatting, unavailable internal
testing hooks, async/sourceURL/stack-limit metadata, and lazy error-info/error
handling. These owners remain separate; no source or fixture change was made.

The selector used the existing coordinator binary through
`tools/integration/bun_corpus_runner.py` with three bounded jobs, a 30-second
per-file timeout, and missing Node modules allowed. No full corpus or
workspace-wide test was run; the selector and raw runner output were removed
after recording the result.

## W219 Node Buffer green cluster

The bounded three-job Node selector covered UTF-8 validity, Buffer byte-length
encoding/type boundaries, and Buffer comparison offset/range validation. All
**3/3 files passed**, with **0 failures and 0 timeouts**; per-file durations
were 232–285ms.

These upstream files are plain scripts, so the Node runner's file-level
clean-exit classification is the authoritative result. No source or upstream
fixture change was made. The selector used the existing coordinator binary
through `tools/integration/node_corpus_runner.py` with three bounded jobs and a
30-second per-file timeout. No full corpus or workspace-wide test was run; the
selector and raw runner output were removed after recording the result.

## W218 Node console green cluster

The bounded three-job Node selector covered console replacement and recovery,
primitive-throw output handling, and the `util.inspect` no-`toString` guard.
All **3/3 files passed**, with **0 failures and 0 timeouts**; per-file durations
were 199–200ms.

These are upstream plain scripts, so the Node runner's file-level clean-exit
classification is the authoritative result rather than a synthetic subtest
count. No source or upstream fixture change was made. The selector used the
existing coordinator binary through `tools/integration/node_corpus_runner.py`
with three bounded jobs and a 30-second per-file timeout. No full corpus or
workspace-wide test was run; the selector and raw runner output were removed
after recording the result.

## W216 Bun VM leak/integration owner split

The bounded three-job selector covered NodeVMScriptFetcher object-count guards,
the vm.Script RSS leak guard, and a happy-dom VM reproduction. It reached
**2/3 files green: 5 passed / 1 failed / 6 ran / 1 expect / 0 timeouts**;
per-file durations were 182–887ms.

`vm-script-fetcher-leak.test.ts` was **4/4** and `script-leak.test.ts` was
**1/1**. The happy-dom reproduction stopped in DOM integration because
`ParentNodeUtility.getElementByTagName` was unavailable; no VM leak conclusion
was inferred from that failure.

No source or upstream fixture change was made. The selector used the existing
coordinator binary through `tools/integration/bun_corpus_runner.py` with three
bounded jobs, a 30-second per-file timeout, and missing Node modules allowed.
No full corpus or workspace-wide test was run; the selector and raw runner
output were removed after recording the result.

## W215 Bun TLS green cluster

The bounded three-job selector covered immutable `rootCertificates`, TLS
no-cipher-match error properties, and `createSecureContext` extra-argument
validation. It reached **3/3 files green: 7 passed / 0 failed / 7 ran / 33
expects / 0 timeouts**; per-file durations were 202–206ms.

All three selected TLS leaves stayed green and no source owner surfaced. An
initial HTTP top-level script was intentionally excluded after the runner
reported **0 tests ran**; it was replaced before the final W215 measurement and
does not contribute to the coverage count.

No source or upstream fixture change was made. The final selector used the
existing coordinator binary through `tools/integration/bun_corpus_runner.py`
with three bounded jobs, a 30-second per-file timeout, and missing Node modules
allowed. No full corpus or workspace-wide test was run; selectors and raw
runner output were removed after recording the result.

## W214 Bun process/TLS/HTTP owner split

The bounded three-job selector covered process stdio lazy initialization near
the stack limit, TLSSocket `allowHalfOpen` handling over a Duplex, and the
NodeHTTPResponse ondata re-registration leak guard. It reached **1/3 files
green: 4 passed / 2 failed / 6 ran / 6 expects / 0 timeouts**; per-file
durations were 235–686ms.

`process-stdio-stack-overflow.test.ts` was **4/4**. The TLS leaf received
`allowHalfOpen: true` where Node expects `false`; the HTTP leak fixture stopped
at bootstrap because its internal handle was unavailable, before the leak
assertion. These are separate option-propagation and internal-handle bootstrap
owners.

No source or upstream fixture change was made. The selector used the existing
coordinator binary through `tools/integration/bun_corpus_runner.py` with three
bounded jobs, a 30-second per-file timeout, and missing Node modules allowed.
No full corpus or workspace-wide test was run; the selector and raw runner
output were removed after recording the result.

## W134 Node HTTP green cluster

The bounded three-job selector covered five previously unrecorded, low-coupling
HTTP leaves: agent close, destroyed-socket handling, default headers, input
function handling, and null-prototype client options. All **5/5 files passed**;
there were **0 failures and 0 timeouts**. Per-file durations were 205–299ms.

No source or upstream fixture change was made. The corrected selector used the
existing coordinator binary through `tools/integration/node_corpus_runner.py`
with three bounded jobs and a 30-second per-file timeout. An initial path-only
selector validation was rejected before dispatch and is excluded from the
result. No full corpus or workspace-wide test was run; the selector and raw
runner output were removed after recording the result.

## W135 Bun spawn/io owner split

The bounded three-job selector covered five low-coupling Bun spawn/io leaves.
The result was **3/5 files green**, with **58 passed / 14 failed / 75 ran / 1391
expects / 0 timeouts**. Green coverage came from `exit-code.test.ts`
(**5/5**), `spawn-empty-arrayBufferOrBlob.test.ts` (**3/3**), and
`spawn-kill-signal.test.ts` (**16/16**). Their durations were 215–683ms.

`bun-write.test.js` took 2793ms and had **28 passed / 7 failed / 35 ran / 1316
expects**. Its failures span file-to-file/content behavior, last-modified
updates, Blob/GC retention, copyFileRange fallback, fd/createPath handling, and
timed output, so it is not one safe owner. `spawnSync.test.ts` took 7029ms and
had **6 passed / 7 failed / 16 ran / 16 expects**; its failures split across
timeout-zero behavior, memfd/counter optimizations, and uid/gid validation.
Both files are parked without a speculative source change or issue.

No source or upstream fixture change was made. The selector used the existing
coordinator binary through `tools/integration/bun_corpus_runner.py` with three
bounded jobs, a 30-second per-file timeout, and missing Node modules allowed.
No full corpus or workspace-wide test was run; the selector and raw runner
output were removed after recording the result.

## W136 Bun Web Encoding green cluster

The bounded three-job selector covered bad stream chunks, CJK decoding,
single-byte decoding, TextEncoder, and TextEncoderStream. It reached **4/5
files green**, with **82 passed / 34 failed / 116 ran / 10777 expects / 0
timeouts**. The green files were `encode-bad-chunks.test.ts` (**6/6**),
`text-decoder-single-byte.test.ts` (**14/14**), `text-encoder-stream.test.ts`
(**20/20**), and `text-encoder.test.js` (**42/42**); their durations were
165–565ms.

`text-decoder-cjk.test.ts` took 202ms and failed **34/34** cases before any
assertion expectations were counted. Every case reported an unsupported
legacy encoding label across Shift_JIS, EUC-JP, Big5, EUC-KR, GBK, GB18030, or
ISO-2022-JP. This is one broad missing-encoding subsystem owner, not a safe
single-file patch, so it is parked without a speculative issue or source
change.

No source or upstream fixture change was made. The selector used the existing
coordinator binary through `tools/integration/bun_corpus_runner.py` with three
bounded jobs, a 30-second per-file timeout, and missing Node modules allowed.
No full corpus or workspace-wide test was run; the selector and raw runner
output were removed after recording the result.

## W137 Node path green cluster

The bounded three-job selector covered basename, dirname, extname, absolute
path detection, and zero-length string behavior. All **5/5 files passed**, with
**0 failures and 0 timeouts**. Per-file durations were 164–200ms.

No source or upstream fixture change was made. The selector used the existing
coordinator binary through `tools/integration/node_corpus_runner.py` with
three bounded jobs and a 30-second per-file timeout. No full corpus or
workspace-wide test was run; the selector and raw runner output were removed
after recording the result.

## W138 Node os green cluster

The bounded three-job selector covered checked-function validation, signal
constants, EOL, homedir fallback without an environment override, and userinfo
getter errors. All **5/5 files passed**, with **0 failures and 0 timeouts**.
Per-file durations were 199–349ms.

No source or upstream fixture change was made. The selector used the existing
coordinator binary through `tools/integration/node_corpus_runner.py` with
three bounded jobs and a 30-second per-file timeout. No full corpus or
workspace-wide test was run; the selector and raw runner output were removed
after recording the result.

## W139 Bun Web Abort green cluster

The bounded three-job selector covered the base Abort contract, AbortController
GC reason handling, and AbortSignal event-listener leak behavior. All **3/3
files were green**, reaching **15 passed / 0 failed / 15 ran / 27 expects** with
no timeout. Per-file durations were 298–800ms.

No source or upstream fixture change was made. The selector used the existing
coordinator binary through `tools/integration/bun_corpus_runner.py` with three
bounded jobs, a 30-second per-file timeout, and missing Node modules allowed.
No full corpus or workspace-wide test was run; the selector and raw runner
output were removed after recording the result.

## W140 Bun Web timers green cluster

The bounded three-job selector covered `setImmediate`, the adjacent
`setImmediate2` contract, performance timing, and performance entries. All
**4/4 files were green**, reaching **12 passed / 0 failed / 12 ran / 58
expects** with no timeout. Per-file durations were 166ms–1.709s.

No source or upstream fixture change was made. The selector used the existing
coordinator binary through `tools/integration/bun_corpus_runner.py` with three
bounded jobs, a 30-second per-file timeout, and missing Node modules allowed.
No full corpus or workspace-wide test was run; the selector and raw runner
output were removed after recording the result.

## W141 Node events owner split

The bounded three-job selector covered CustomEvent, getMaxListeners, the
events list surface, listener-count behavior, and `events.once`. It reached
**3/5 files passed**, with **2 failures and 0 timeouts**; per-file durations
were 164–349ms. The green leaves were CustomEvent, events list, and
listener-count-with-listener.

The two failures are separate owners. `test-events-getmaxlisteners.js` reports
the default maximum for an AbortSignal as **10** where Node expects **0**.
`test-events-once.js` reaches the invalid-argument error path but the thrown
error has no `ERR_INVALID_ARG_TYPE` code. Neither is folded into the other or
patched speculatively.

No source or upstream fixture change was made. The selector used the existing
coordinator binary through `tools/integration/node_corpus_runner.py` with
three bounded jobs and a 30-second per-file timeout. No full corpus or
workspace-wide test was run; the selector and raw runner output were removed
after recording the result.

## W142 Node string_decoder owner split

The bounded three-job selector covered StringDecoder end behavior, randomized
byte fuzzing, and the main contract file. It reached **2/3 files passed**, with
**1 failure and 0 timeouts**; per-file durations were 200–349ms.

`test-string-decoder-end.js` and `test-string-decoder-fuzz.js` passed. The main
file failed only when `StringDecoder.prototype.write` was invoked without a
decoder instance: Node requires an `ERR_INVALID_THIS` error, while the current
runtime did not throw. This is a single private-brand/invalid-this owner and is
parked without a speculative source change or issue.

No source or upstream fixture change was made. The selector used the existing
coordinator binary through `tools/integration/node_corpus_runner.py` with
three bounded jobs and a 30-second per-file timeout. No full corpus or
workspace-wide test was run; the selector and raw runner output were removed
after recording the result.

## W143 Node timers green cluster

The bounded three-job selector covered timer argument forwarding, clearing
null/object handles, invalid clear inputs, and zero-timeout behavior. All
**5/5 files passed**, with **0 failures and 0 timeouts**. Per-file durations
were 165–350ms.

No source or upstream fixture change was made. The selector used the existing
coordinator binary through `tools/integration/node_corpus_runner.py` with
three bounded jobs and a 30-second per-file timeout. No full corpus or
workspace-wide test was run; the selector and raw runner output were removed
after recording the result.

## W144 Bun Web console owner split

The bounded three-job selector covered UTF-16 logging, ordinary console.log,
recursive formatting, and console.timeLog. It reached **2/4 files green**, with
**3 passed / 7 failed / 10 ran / 15 expects / 0 timeouts**. The green files were
the UTF-16 and recursive-formatting leaves; their durations were 299–401ms.

`console-log.test.ts` had four failures spanning snapshot/formatting output,
long-array cutoff, console.group stack formatting, and SharedArrayBuffer
rendering. `console-timeLog.test.ts` had three failures spanning elapsed-time
output and logging format. These are multiple owners, so both files are parked
without a speculative source change or issue.

No source or upstream fixture change was made. The selector used the existing
coordinator binary through `tools/integration/bun_corpus_runner.py` with three
bounded jobs, a 30-second per-file timeout, and missing Node modules allowed.
No full corpus or workspace-wide test was run; the selector and raw runner
output were removed after recording the result.

## W145 Node timers adjacent owner split

The bounded three-job selector covered timer API refs, clearTimeout/interval
equivalence, setImmediate, non-integer delays, and callback `this` behavior.
It reached **4/5 files passed**, with **1 failure and 0 timeouts**; per-file
durations were 167–251ms.

The four non-failing leaves are retained. `test-timers-non-integer-delay.js`
reported callback order **1,4,3,2** where Node expects **1,2,3,4**. This is a
single fractional-delay ordering owner and is parked without a speculative
source change or issue.

No source or upstream fixture change was made. The selector used the existing
coordinator binary through `tools/integration/node_corpus_runner.py` with
three bounded jobs and a 30-second per-file timeout. No full corpus or
workspace-wide test was run; the selector and raw runner output were removed
after recording the result.

## W146 Bun Web Request owner split

The bounded three-job selector covered Request method memory behavior, Request
subclass getter dispatch, and Request clone leak behavior. It reached **2/3
files green**, with **14 passed / 6 failed / 20 ran / 24 expects / 0 timeouts**.
`request-subclass.test.ts` passed in 199ms and `request-clone-leak.test.ts`
passed all **12/12** checks but took **21.240s**, so the latter is retained as
a slow stress guard and excluded from the default fast lane.

`request-method-getter.test.ts` failed all **6/6** checks because the observed
heap metric was `NaN`. This is a memory-accounting/heapStats owner, not a safe
Request method wrapper fix, so it is parked without a speculative source
change or issue.

No source or upstream fixture change was made. The selector used the existing
coordinator binary through `tools/integration/bun_corpus_runner.py` with three
bounded jobs, a 30-second per-file timeout, and missing Node modules allowed.
No full corpus or workspace-wide test was run; the selector and raw runner
output were removed after recording the result.

## W147 Node path adjacent green cluster

The bounded three-job selector covered path join, normalize, and relative
resolution semantics. All **3/3 files passed**, with **0 failures and 0
timeouts**. Per-file durations were 198–199ms.

No source or upstream fixture change was made. The selector used the existing
coordinator binary through `tools/integration/node_corpus_runner.py` with
three bounded jobs and a 30-second per-file timeout. No full corpus or
workspace-wide test was run; the selector and raw runner output were removed
after recording the result.

## W148 Node path namespace/glob green cluster

The bounded three-job selector covered path glob matching plus the `path.posix`
and `path.win32` namespace identity guards. All **3/3 files passed**, with
**0 failures and 0 timeouts**. Per-file durations were 198–200ms.

No source or upstream fixture change was made. The selector used the existing
coordinator binary through `tools/integration/node_corpus_runner.py` with
three bounded jobs and a 30-second per-file timeout. No full corpus or
workspace-wide test was run; the selector and raw runner output were removed
after recording the result.

## W149 Bun Fetch/Web green cluster

The bounded three-job selector covered Body mixin errors, FormData
Content-Length, wire header casing, and UTF-8 BOM handling. All **4/4 files
were green**, reaching **27 passed / 0 failed / 27 ran / 40 expects** with no
timeout. Per-file durations were 200–216ms.

No source or upstream fixture change was made. The selector used the existing
coordinator binary through `tools/integration/bun_corpus_runner.py` with three
bounded jobs, a 30-second per-file timeout, and missing Node modules allowed.
No full corpus or workspace-wide test was run; the selector and raw runner
output were removed after recording the result.

## W150 Bun Blob green cluster

The bounded three-job selector covered the Blob array fast path, copy-on-write,
file-name ownership, and blob.write validation. All **4/4 files were green**,
reaching **27 passed / 0 failed / 27 ran / 62 expects** with no timeout.
Per-file durations were 165–350ms.

The first selector version used an incorrect directory prefix and was rejected
by path validation before dispatch; it is excluded from coverage data. The
corrected selector used the existing coordinator binary through
`tools/integration/bun_corpus_runner.py` with three bounded jobs, a 30-second
per-file timeout, and missing Node modules allowed.

No source or upstream fixture change was made. No full corpus or
workspace-wide test was run; the selector and raw runner output were removed
after recording the result.

## W151 Node URL owner split

The bounded three-job selector covered fileURL-to-path conversion, invalid URL
format input, WHATWG formatting, invalid receivers, and URL property
descriptors. It reached **3/5 files passed**, with **2 failures and 0
timeouts**; per-file durations were 167–253ms. The green leaves were
`test-url-fileurltopath.js`, `test-url-format-invalid-input.js`, and
`test-url-format-whatwg.js`.

The two failures are independent. `test-whatwg-url-invalidthis.js` found that
URL prototype methods invoked with an invalid receiver did not throw the
required TypeError. `test-whatwg-url-properties.js` found a URL method
descriptor with `enumerable: false` where Node expects `true`. Both are parked
without speculative source changes or issues.

No source or upstream fixture change was made. The selector used the existing
coordinator binary through `tools/integration/node_corpus_runner.py` with
three bounded jobs and a 30-second per-file timeout. No full corpus or
workspace-wide test was run; the selector and raw runner output were removed
after recording the result.

## W152 Bun Fetch body/cyclic green cluster

The bounded three-job selector covered async-iterator body safety and cyclic
Request/Response stream references. All **3/3 files were green**, reaching
**6 passed / 0 failed / 6 ran / 8 expects** with no timeout. Per-file durations
were 253–404ms.

Both cyclic-reference files' heapStats checks passed, so this probe did not
reproduce the W146 Request method `NaN` memory-accounting owner. No source or
upstream fixture change was made. The selector used the existing coordinator
binary through `tools/integration/bun_corpus_runner.py` with three bounded
jobs, a 30-second per-file timeout, and missing Node modules allowed.
No full corpus or workspace-wide test was run; the selector and raw runner
output were removed after recording the result.

## W153 Node URL utility green cluster

The bounded three-job selector covered `pathToFileURL`, `URL.revokeObjectURL`
argument validation, and `urlToHttpOptions`. All **3/3 files passed**, with
**0 failures and 0 timeouts**. Per-file durations were 199–252ms.

No source or upstream fixture change was made. The selector used the existing
coordinator binary through `tools/integration/node_corpus_runner.py` with
three bounded jobs and a 30-second per-file timeout. No full corpus or
workspace-wide test was run; the selector and raw runner output were removed
after recording the result.

## W154 Node URLSearchParams green cluster

The bounded three-job selector covered URLSearchParams `get`, `getAll`, `has`,
`keys`, and `values` semantics. All **5/5 files passed**, with **0 failures and
0 timeouts**. Per-file durations were 165–201ms.

No source or upstream fixture change was made. The selector used the existing
coordinator binary through `tools/integration/node_corpus_runner.py` with
three bounded jobs and a 30-second per-file timeout. No full corpus or
workspace-wide test was run; the selector and raw runner output were removed
after recording the result.

## W155 Node querystring green cluster

The bounded three-job selector covered querystring encode/escape behavior,
non-finite `maxKeys`, multicharacter separators, and legacy URL query parsing.
All **5/5 files passed**, with **0 failures and 0 timeouts**. Per-file
durations were 164–200ms.

No source or upstream fixture change was made. The selector used the existing
coordinator binary through `tools/integration/node_corpus_runner.py` with
three bounded jobs and a 30-second per-file timeout. No full corpus or
workspace-wide test was run; the selector and raw runner output were removed
after recording the result.

## W156 Node util green cluster

The bounded three-job selector covered `util.deprecate`, `util.inherits`,
`util.types`, type-existence helpers, and VT control-character stripping. All
**5/5 files passed**, with **0 failures and 0 timeouts**. Per-file durations
were 165–350ms.

No source or upstream fixture change was made. The selector used the existing
coordinator binary through `tools/integration/node_corpus_runner.py` with
three bounded jobs and a 30-second per-file timeout. No full corpus or
workspace-wide test was run; the selector and raw runner output were removed
after recording the result.

## W157 Node Buffer numeric green cluster

The bounded three-job selector covered signed and unsigned integer reads,
floating-point reads, and signed and unsigned integer writes. All **5/5 files
passed**, with **0 failures and 0 timeouts**. Per-file durations were 164–201ms.

No source or upstream fixture change was made. The selector used the existing
coordinator binary through `tools/integration/node_corpus_runner.py` with
three bounded jobs and a 30-second per-file timeout. No full corpus or
workspace-wide test was run; the selector and raw runner output were removed
after recording the result.

## W158 Node Buffer compare/copy green cluster

The bounded three-job selector covered Buffer compare, copy, equals, indexOf,
and double-precision read behavior. All **5/5 files passed**, with **0
failures and 0 timeouts**. Per-file durations were 165–300ms.

No source or upstream fixture change was made. The selector used the existing
coordinator binary through `tools/integration/node_corpus_runner.py` with
three bounded jobs and a 30-second per-file timeout. No full corpus or
workspace-wide test was run; the selector and raw runner output were removed
after recording the result.

## W159 Node assert owner split

The bounded three-job selector covered five assertion leaves. It measured **2/5
files passed, 3/5 failed, and 0 timeouts**. `test-assert-fail.js` and
`test-assert-if-error.js` stayed green. The failures in `test-assert-deep.js`,
`test-assert-partial-deep-equal.js`, and `test-assert-typedarray-deepequal.js`
span generic deep-comparison, partial-matching, and typed-array assertion
contracts; they are parked as separate owners rather than mixed into one fix.

No source or upstream fixture change was made. The selector used the existing
coordinator binary through `tools/integration/node_corpus_runner.py` with
three bounded jobs and a 30-second per-file timeout. No full corpus or
workspace-wide test was run; the selector and raw runner output were removed
after recording the result.

## W160 Bun Web owner split

The bounded three-job selector covered Bun URLSearchParams, structured-clone
fast paths, Response behavior, and the platform-specific Windows URL file.
Among the **3 executable files**, **2/3 were green**; the aggregate was **131
passed / 1 failed / 148 ran / 842 expects**, with **0 timeouts**. The green
files were URLSearchParams (**17/17**) and structured-clone-fastpath
(**92/92**). Response reached **22/23**, with one `FileRef` print-size
snapshot mismatch caused by a different relative path root. The Windows URL
file had **16/16 skipped** on Linux and is excluded from the green denominator.

No source or upstream fixture change was made. The selector used the existing
coordinator binary through `tools/integration/bun_corpus_runner.py` with
three bounded jobs, a 30-second per-file timeout, and missing Node modules
allowed. No full corpus or workspace-wide test was run; the selector and raw
runner output were removed after recording the result.

## W163 Node fs owner split

The bounded three-job selector covered access, constants, mkdir, mkdtemp, and
open-flags behavior. It reached **4/5 files passed**, with **1 failure and 0
timeouts**. Constants, mkdir, mkdtemp, and open-flags stayed green. The access
failure was limited to the expected async stack shape for
`fs.promises.access()` and is parked as an error-stack owner.

No source or upstream fixture change was made. The selector used the existing
coordinator binary through `tools/integration/node_corpus_runner.py` with
three bounded jobs and a 30-second per-file timeout. No full corpus or
workspace-wide test was run; the selector and raw runner output were removed
after recording the result.

## W164 Node fs I/O green cluster

The bounded three-job selector covered read, zero-length read, optional-argument
writeSync, appendFileSync, and the readFile UTF-8 fast path. All **5/5 files
passed**, with **0 failures and 0 timeouts**. Per-file durations were 165–265ms.

No source or upstream fixture change was made. The selector used the existing
coordinator binary through `tools/integration/node_corpus_runner.py` with
three bounded jobs and a 30-second per-file timeout. No full corpus or
workspace-wide test was run; the selector and raw runner output were removed
after recording the result.

## W165 Node fs error/read green cluster

The bounded three-job selector covered empty-file reads, readfile errors,
readlink type validation, and two invalid-path cases. The **3 executable files
all passed** with **0 failures and 0 timeouts**. The remaining two files were
explicitly Windows-only and skipped on Linux; they are excluded from the green
denominator. Executable per-file durations were 165–350ms.

No source or upstream fixture change was made. The selector used the existing
coordinator binary through `tools/integration/node_corpus_runner.py` with
three bounded jobs and a 30-second per-file timeout. No full corpus or
workspace-wide test was run; the selector and raw runner output were removed
after recording the result.

## W166 Bun util/file owner split

The bounded three-job selector covered `fileUrl`, Bun.file read behavior,
`Bun.file()` broad error/JSON behavior, and concat. It reached **3/4 files
green**, with **28 passed / 4 failed / 32 ran / 73 expects** and **0 timeouts**.
The green leaves were `fileUrl` (**20/20**), Bun.file read (**1/1**), and
concat (**5/5**). The four failures in `bun-file.test.ts` reconfirm the
already parked async-stack and empty-JSON-message owners; no new issue or
mixed fix was opened.

No source or upstream fixture change was made. The selector used the existing
coordinator binary through `tools/integration/bun_corpus_runner.py` with
three bounded jobs, a 30-second per-file timeout, and missing Node modules
allowed. No full corpus or workspace-wide test was run; the selector and raw
runner output were removed after recording the result.

## W167 Node fs delete/error green cluster

The bounded three-job selector covered rename and unlink type validation,
rmdir not-found and file-target errors, and mkdir/rmdir lifecycle behavior.
All **5/5 files passed**, with **0 failures and 0 timeouts**. Per-file
durations were 164–249ms.

No source or upstream fixture change was made. The selector used the existing
coordinator binary through `tools/integration/node_corpus_runner.py` with
three bounded jobs and a 30-second per-file timeout. No full corpus or
workspace-wide test was run; the selector and raw runner output were removed
after recording the result.

## W168 Bun util owner split

The bounded three-job selector covered error-code mirroring, mutable global
prototype behavior, reportError output, and ANSI wrapping. It reached **2/4
files green**, with **50 passed / 203 failed / 253 ran / 261 expects** and
**0 timeouts**. The green leaves were error-code-mirror (**2/2**) and
exotic-global-mutable-prototype (**1/1**). ReportError failures split across
native error-printer output/stack and lone-surrogate handling; wrapAnsi failures
span word wrapping, width accounting, and ANSI escape composition. These are
parked as separate broad owners.

No source or upstream fixture change was made. The selector used the existing
coordinator binary through `tools/integration/bun_corpus_runner.py` with
three bounded jobs, a 30-second per-file timeout, and missing Node modules
allowed. No full corpus or workspace-wide test was run; the selector and raw
runner output were removed after recording the result.

## W169 Node fs/promises owner split

The bounded three-job selector covered `fs.promises.exists`, readfile with an
fd, basic readfile, statfs path validation, and writefile. It reached **4/5
files passed**, with **1 failure and 0 timeouts**. The four green leaves were
the exists, fd-backed readfile, statfs validation, and writefile contracts.
The basic readfile failure was the known zero-byte-liar child-fixture callback
count mismatch, not a newly isolated fs/promises source owner.

No source or upstream fixture change was made. The selector used the existing
coordinator binary through `tools/integration/node_corpus_runner.py` with
three bounded jobs and a 30-second per-file timeout. No full corpus or
workspace-wide test was run; the selector and raw runner output were removed
after recording the result.

## W170 Node fs vector/copy green cluster

The bounded three-job selector covered readv, writev, writevSync, copyfile,
and truncateSync behavior. All **5/5 files passed**, with **0 failures and 0
timeouts**. Per-file durations were 165–265ms.

No source or upstream fixture change was made. The selector used the existing
coordinator binary through `tools/integration/node_corpus_runner.py` with
three bounded jobs and a 30-second per-file timeout. No full corpus or
workspace-wide test was run; the selector and raw runner output were removed
after recording the result.

## W171 Node fs directory/stat green cluster

The bounded three-job selector covered opendir, readdir entry types, symlink
entry types, and stat behavior. All **4/4 Linux-executable files passed**, with
**0 failures and 0 timeouts**. The remaining readdir-buffer file is explicitly
MacOS-only and was skipped, so it is excluded from the green denominator.
Executable per-file durations were 165–352ms.

No source or upstream fixture change was made. The selector used the existing
coordinator binary through `tools/integration/node_corpus_runner.py` with
three bounded jobs and a 30-second per-file timeout. No full corpus or
workspace-wide test was run; the selector and raw runner output were removed
after recording the result.

## W172 Bun I/O green cluster

The bounded three-job selector covered Bun FileSink, ArrayBufferSink, and
Bun.file.exists. All **3/3 files were green**, reaching **53 passed / 0 failed /
53 ran / 1728 expects**, with **0 timeouts**. FileSink passed **46/46** in
1.160s, ArrayBufferSink passed **6/6**, and Bun.file.exists passed **1/1**;
the other two files took about 200ms each.

No source or upstream fixture change was made. The selector used the existing
coordinator binary through `tools/integration/bun_corpus_runner.py` with
three bounded jobs, a 30-second per-file timeout, and missing Node modules
allowed. No full corpus or workspace-wide test was run; the selector and raw
runner output were removed after recording the result.

## W173 Node stream green cluster

The bounded three-job selector covered Duplex behavior, Readable state,
Writable properties, string pushes, and TypedArray chunks. All **5/5 files
passed**, with **0 failures and 0 timeouts**. Per-file durations were 199–316ms.

No source or upstream fixture change was made. The selector used the existing
coordinator binary through `tools/integration/node_corpus_runner.py` with
three bounded jobs and a 30-second per-file timeout. No full corpus or
workspace-wide test was run; the selector and raw runner output were removed
after recording the result.

## W174 Node stream error/end green cluster

The bounded three-job selector covered readable and writable invalid chunks,
writable finished state, end-of-stream handling, and Duplex end behavior. All
**5/5 files passed**, with **0 failures and 0 timeouts**. Per-file durations
were 165–230ms.

No source or upstream fixture change was made. The selector used the existing
coordinator binary through `tools/integration/node_corpus_runner.py` with
three bounded jobs and a 30-second per-file timeout. No full corpus or
workspace-wide test was run; the selector and raw runner output were removed
after recording the result.

## W175 Node stream state/event green cluster

The bounded three-job selector covered Writable ended and needDrain state,
Readable data and readable events, and isPaused behavior. All **5/5 files
passed**, with **0 failures and 0 timeouts**. Per-file durations were 165–200ms.

No source or upstream fixture change was made. The selector used the existing
coordinator binary through `tools/integration/node_corpus_runner.py` with
three bounded jobs and a 30-second per-file timeout. No full corpus or
workspace-wide test was run; the selector and raw runner output were removed
after recording the result.

## W176 Node stream/Web strategy green cluster

The bounded three-job selector covered ReadableStream termination through Web
bridges, Readable strategy options, Writable default encoding, and the global
stream high-water-mark setting. All **5/5 files passed**, with **0 failures and
0 timeouts**. Per-file durations were 165–333ms.

No source or upstream fixture change was made. The selector used the existing
coordinator binary through `tools/integration/node_corpus_runner.py` with
three bounded jobs and a 30-second per-file timeout. No full corpus or
workspace-wide test was run; the selector and raw runner output were removed
after recording the result.

## W177 Node stream encoding/buffer owner split

The bounded three-job selector covered Readable HWM zero, default encoding,
wrapped encoding, Writable buffer clearing, and null writes. It reached **4/5
files passed**, with **1 failure and 0 timeouts**. The four HWM/default-encoding/
buffer leaves stayed green. `stream-wrap-encoding` failed only on expected
callback counts and is parked as a separate stream-wrap callback owner.

No source or upstream fixture change was made. The selector used the existing
coordinator binary through `tools/integration/node_corpus_runner.py` with
three bounded jobs and a 30-second per-file timeout. No full corpus or
workspace-wide test was run; the selector and raw runner output were removed
after recording the result.

## W178 Node stream iterator owner split

The bounded three-job selector covered iterator push, sync and async sources,
Readable interop, and iterator validation. It reached **4/5 files passed**,
with **1 failure and 0 timeouts**. The push, sync, async, and validation leaves
stayed green. The interop failure compared a Buffer result against a
Uint8Array expectation and is parked as a separate typed-array interop owner.

No source or upstream fixture change was made. The selector used the existing
coordinator binary through `tools/integration/node_corpus_runner.py` with
three bounded jobs and a 30-second per-file timeout. No full corpus or
workspace-wide test was run; the selector and raw runner output were removed
after recording the result.

## W179 Node stream pipeline green cluster

The bounded three-job selector covered basic pipeline behavior, listener
cleanup, empty-string input, Duplex pipelines, and async-iterator pipelines.
All **5/5 files passed**, with **0 failures and 0 timeouts**. Per-file
durations were 165ms–1.183s.

No source or upstream fixture change was made. The selector used the existing
coordinator binary through `tools/integration/node_corpus_runner.py` with
three bounded jobs and a 30-second per-file timeout. No full corpus or
workspace-wide test was run; the selector and raw runner output were removed
after recording the result.

## W180 Node stream advanced owner split

The bounded three-job selector covered stream compose, consumers, promises,
DuplexPair, and finished behavior. It reached **4/5 files passed**, with
**1 failure and 0 timeouts**. Compose, consumers, DuplexPair, and promises
stayed green. The finished leaf failed only on an expected callback count and
is parked as a separate finished-callback owner.

No source or upstream fixture change was made. The selector used the existing
coordinator binary through `tools/integration/node_corpus_runner.py` with
three bounded jobs and a 30-second per-file timeout. No full corpus or
workspace-wide test was run; the selector and raw runner output were removed
after recording the result.

## W181 Node Readable/Web BYOB green cluster

The bounded three-job selector covered Readable-to-Web BYOB behavior, BYOB
termination, the Readable-to-Web module path, and the server-response bridge.
All **4/4 files passed**, with **0 failures and 0 timeouts**. Per-file
durations were 200ms–2.259s.

No source or upstream fixture change was made. The selector used the existing
coordinator binary through `tools/integration/node_corpus_runner.py` with
three bounded jobs and a 30-second per-file timeout. No full corpus or
workspace-wide test was run; the selector and raw runner output were removed
after recording the result.

## W182 Node stream pipe green cluster

The bounded three-job selector covered pipe cleanup, pipe events and flow,
multiple destinations, and piping the same destination twice. All **5/5 files
passed**, with **0 failures and 0 timeouts**. Per-file durations were 164–250ms.

No source or upstream fixture change was made. The selector used the existing
coordinator binary through `tools/integration/node_corpus_runner.py` with
three bounded jobs and a 30-second per-file timeout. No full corpus or
workspace-wide test was run; the selector and raw runner output were removed
after recording the result.

## W183 Node Transform green cluster

The bounded three-job selector covered callback-twice handling, synchronous
finalization, falsey object-mode values, zero HWM, and destroy behavior. All
**5/5 files passed**, with **0 failures and 0 timeouts**. Per-file durations
were 166–201ms.

No source or upstream fixture change was made. The selector used the existing
coordinator binary through `tools/integration/node_corpus_runner.py` with
three bounded jobs and a 30-second per-file timeout. No full corpus or
workspace-wide test was run; the selector and raw runner output were removed
after recording the result.

## W184 Node Writable green cluster

The bounded three-job selector covered Writable constructor method settings,
asynchronous finalization, final/destroy ordering, destroy lifecycle, and write
callback errors. All **5/5 files passed**, with **0 failures and 0 timeouts**.
Per-file durations were 165–201ms.

No source or upstream fixture change was made. The selector used the existing
coordinator binary through `tools/integration/node_corpus_runner.py` with
three bounded jobs and a 30-second per-file timeout. No full corpus or
workspace-wide test was run; the selector and raw runner output were removed
after recording the result.

## W185 Node Writable adjacent green cluster

The bounded three-job selector covered default-encoding changes, end callback
errors, repeated end calls, finished-state reporting, and duplicate write
callbacks. All **5/5 files passed**, with **0 failures and 0 timeouts**.
Per-file durations were 164–249ms.

No source or upstream fixture change was made. The selector used the existing
coordinator binary through `tools/integration/node_corpus_runner.py` with
three bounded jobs and a 30-second per-file timeout. No full corpus or
workspace-wide test was run; the selector and raw runner output were removed
after recording the result.

## W186 Node Writable final/error green cluster

The bounded three-job selector covered abort handling, thrown finalizers,
finish-after-destroy behavior, write errors, and writev finish behavior. All
**5/5 files passed**, with **0 failures and 0 timeouts**. Per-file durations
were 164–200ms.

No source or upstream fixture change was made. The selector used the existing
coordinator binary through `tools/integration/node_corpus_runner.py` with
three bounded jobs and a 30-second per-file timeout. No full corpus or
workspace-wide test was run; the selector and raw runner output were removed
after recording the result.

## W187 Node Readable basic green cluster

The bounded three-job selector covered Readable constructor method settings,
adding a chunk during data delivery, default encoding, internal didRead
behavior, and short-stream readable emission. All **5/5 files passed**, with
**0 failures and 0 timeouts**. Per-file durations were 164–199ms.

No source or upstream fixture change was made. The selector used the existing
coordinator binary through `tools/integration/node_corpus_runner.py` with
three bounded jobs and a 30-second per-file timeout. No full corpus or
workspace-wide test was run; the selector and raw runner output were removed
after recording the result.

## W188 Node Readable event/end green cluster

The bounded three-job selector covered end-after-destroy, ended state,
error-end ordering, readable event behavior, and flow recursion. All **5/5
files passed**, with **0 failures and 0 timeouts**. Per-file durations were
164–248ms.

No source or upstream fixture change was made. The selector used the existing
coordinator binary through `tools/integration/node_corpus_runner.py` with
three bounded jobs and a 30-second per-file timeout. No full corpus or
workspace-wide test was run; the selector and raw runner output were removed
after recording the result.

## W189 Node Readable readiness green cluster

The bounded three-job selector covered emitted-readable timing, needReadable
behavior, suppression of unnecessary readable events, pause/resume, and a
single readable event. All **5/5 files passed**, with **0 failures and 0
timeouts**. Per-file durations were 165–250ms. Four files were fresh new
coverage; `test-stream-readable-no-unneeded-readable.js` was already green in
W115 and is recorded here as a reconfirmed guard, not a new increment.

No source or upstream fixture change was made. The selector used the existing
coordinator binary through `tools/integration/node_corpus_runner.py` with
three bounded jobs and a 30-second per-file timeout. No full corpus or
workspace-wide test was run; the selector and raw runner output were removed
after recording the result.

## W190 Bun parser/API green cluster

The bounded three-job selector covered `Bun.cron.parse`, INI parsing, JSON5
parsing, JSONC parsing, and JSONL parsing. All **5/5 files were green**, with
**719/719 tests passed**, **0 failed**, **0 timed out**, and **5324 expects**.
Per-file durations were 465ms–4.166s; the INI suite was the slowest at 4.166s
but remained well inside the 30-second bound.

No source or upstream fixture change was made. The selector used the existing
coordinator binary through `tools/integration/bun_corpus_runner.py` with three
bounded jobs, a 30-second per-file timeout, and missing Node modules allowed.
No full corpus or workspace-wide test was run; the selector and raw runner
output were removed after recording the result.

## W191 Bun util green cluster

The bounded three-job selector covered password hashing, xxHash vectors, native
error name/code preservation, `sleepSync`, and invalid-input `pathToFileURL`.
All **5/5 files were green**, with **105 passed**, **0 failed**, **106 ran**,
**1 skipped**, and **409 expects**. `password.test.ts` was the slowest file at
8.789s; the other files took 198–264ms, with no timeout.

No source or upstream fixture change was made. The selector used the existing
coordinator binary through `tools/integration/bun_corpus_runner.py` with three
bounded jobs, a 30-second per-file timeout, and missing Node modules allowed.
No full corpus or workspace-wide test was run; the selector and raw runner
output were removed after recording the result.

## W192 Bun parser/cron owner split

The bounded three-job selector covered the JSON5 conformance suite, JSONC
JSONTestSuite, cron API/parse behavior, in-process cron hot reload, and the
static TLS segment-size guard. It reached **3/5 files green**, with **464
passed**, **78 failed**, **578 ran**, **755 expects**, and **0 timeouts**.
The green files were the TLS segment-size guard (**1 pass, 1 skip**), JSON5
conformance (**113/113**), and JSONC JSONTestSuite (**319/319**).

The two cron files remain split rather than mixed: `in-process-cron.test.ts`
hits the explicit missing Bun.cron scheduling surface (**0 pass / 27 fail**),
while `cron.test.ts` combines registration/removal/execution gaps with a
separate parse nickname/validation owner (**31 pass / 51 fail / 35 skip**).
No source or upstream fixture change was made and no new issue was opened from
this mixed measurement.

The selector used the existing coordinator binary through
`tools/integration/bun_corpus_runner.py` with three bounded jobs, a 30-second
per-file timeout, and missing Node modules allowed. No full corpus or
workspace-wide test was run; the selector and raw runner output were removed
after recording the result.

## W193 Bun low-coupling owner split

The bounded three-job selector covered direct Readable stream behavior,
internal source-map roundtrips, internal source-map stack mapping, libuv error
name conversion, and perf-hooks histograms. It reached **3/5 files green**,
with **323 passed**, **12 failed**, **604 ran**, **2302 expects**, and **0
timeouts**.

The retained green files were direct-readable (**269 pass / 268 skip / 0 fail
/ 537 ran / 440 expects**), libuv error-name (**1 pass / 1 skip / 0 fail / 2
ran / 1 expect**), and histogram (**38 pass / 0 fail / 38 ran**). The two
source-map files remain separate owners: roundtrip failures combine relative
path/source-root normalization with truncated-UTF-8 mapping, while the
internal-source-map failures combine astral inline-snapshot positions,
long-line mapping, cache eviction, and stack-path shape. No source or upstream
fixture change was made and no mixed fix was attempted.

The selector used the existing coordinator binary through
`tools/integration/bun_corpus_runner.py` with three bounded jobs, a 30-second
per-file timeout, and missing Node modules allowed. No full corpus or
workspace-wide test was run; the selector and raw runner output were removed
after recording the result.

## W194 Bun JSC/resolve/transpiler owner split

The bounded three-job selector covered native constructor identity, string
no-atomize behavior, bun.lock import, REPL-mode transpilation, and TypeScript
type-export cases. It reached **3/5 files green**, with **39 passed**, **73
failed**, **130 ran**, **286 expects**, and **0 timeouts**.

The retained green files were native constructor identity (**4/4**),
string-noAtomize (**1/1**), and bun.lock import (**1/1**). The REPL transform
file splits between destructuring/parser failures and REPL-output behavior.
The type-export file is dominated by the explicit `--bytecode` unavailable
compile path plus related compile cases. These are separate owners; no source
or upstream fixture change was made and no mixed fix was attempted.

The selector used the existing coordinator binary through
`tools/integration/bun_corpus_runner.py` with three bounded jobs, a 30-second
per-file timeout, and missing Node modules allowed. No full corpus or
workspace-wide test was run; the selector and raw runner output were removed
after recording the result.

## W195 Bun resolver import/meta owner split

The bounded three-job selector covered `import.meta`, `import.meta.resolve`,
empty-file/empty-sqlite imports, CJS `__esModule` annotations, and ESM/CJS
module shape behavior. It reached **1/5 files green**, with **42 passed**, **28
failed**, **70 ran**, **82 expects**, and **0 timeouts**.

`import-meta-resolve.test.mjs` was fully green (**15/15**). The remaining
failures split into three owners: import.meta relative-vs-absolute filename
semantics, empty-file/empty-sqlite module shape, and CJS `__esModule` export
annotation/setter behavior. No source or upstream fixture change was made and
no mixed fix was attempted.

The selector used the existing coordinator binary through
`tools/integration/bun_corpus_runner.py` with three bounded jobs, a 30-second
per-file timeout, and missing Node modules allowed. No full corpus or
workspace-wide test was run; the selector and raw runner output were removed
after recording the result.

## W196 Node Readable readiness/encoding green cluster

The bounded three-job selector covered readable-then-resume, reading-more
state, resume high-water-mark behavior, scheduled resume, and setting encoding
over existing buffers. All **5/5 files passed**, with **0 failures and 0
timeouts**. Per-file durations were 165–200ms.

No source or upstream fixture change was made. The selector used the existing
coordinator binary through `tools/integration/node_corpus_runner.py` with
three bounded jobs and a 30-second per-file timeout. No full corpus or
workspace-wide test was run; the selector and raw runner output were removed
after recording the result.

## W197 Node Readable boundary green cluster

The bounded three-job selector covered unshift behavior, an unimplemented
`_read` guard, next-without-null behavior, asynchronous object-mode multi-push,
and destroy handling. All **5/5 files passed**, with **0 failures and 0
timeouts**. Per-file durations were 165–265ms.

No source or upstream fixture change was made. The selector used the existing
coordinator binary through `tools/integration/node_corpus_runner.py` with
three bounded jobs and a 30-second per-file timeout. No full corpus or
workspace-wide test was run; the selector and raw runner output were removed
after recording the result.

## W198 Node Duplex green cluster

The bounded three-job selector covered Duplex-from construction, Duplex
properties, readable/writable state coupling, writable-finished behavior, and
Duplex end handling. All **5/5 files passed**, with **0 failures and 0
timeouts**. Per-file durations were 165–283ms.

No source or upstream fixture change was made. The selector used the existing
coordinator binary through `tools/integration/node_corpus_runner.py` with
three bounded jobs and a 30-second per-file timeout. No full corpus or
workspace-wide test was run; the selector and raw runner output were removed
after recording the result.

## W199 Node Duplex/destroy/finalization green cluster

The bounded three-job selector covered Duplex destroy, Duplex readable end,
base Duplex behavior, stream destroy, and the finished default path. All **5/5
files passed**, with **0 failures and 0 timeouts**. Per-file durations were
198–265ms.

No source or upstream fixture change was made. The selector used the existing
coordinator binary through `tools/integration/node_corpus_runner.py` with
three bounded jobs and a 30-second per-file timeout. No full corpus or
workspace-wide test was run; the selector and raw runner output were removed
after recording the result.

## W200 Node pipe/backpressure green cluster

The bounded three-job selector covered piping after end, await-drain,
manual-resume drain handling, pushing while writing, and cleanup pause
behavior. All **5/5 files passed**, with **0 failures and 0 timeouts**.
Per-file durations were 166–185ms.

No source or upstream fixture change was made. The selector used the existing
coordinator binary through `tools/integration/node_corpus_runner.py` with
three bounded jobs and a 30-second per-file timeout. No full corpus or
workspace-wide test was run; the selector and raw runner output were removed
after recording the result.

## W201 Node pipe continuation green cluster

The bounded three-job selector covered pipe deadlock prevention, manual resume,
needDrain state, object-mode to non-object-mode piping, and operation without a
`listenerCount` helper. All **5/5 files passed**, with **0 failures and 0
timeouts**. Per-file durations were 165–281ms.

No source or upstream fixture change was made. The selector used the existing
coordinator binary through `tools/integration/node_corpus_runner.py` with
three bounded jobs and a 30-second per-file timeout. No full corpus or
workspace-wide test was run; the selector and raw runner output were removed
after recording the result.

## W202 Node pipeline/finished owner split

The bounded three-job selector covered pipeline process execution, queued end
while destroying, uncaught pipeline errors, finished async-local-storage
behavior, and the finished `bindAsyncResource` path. It reached **3/5 files
passed**, with **2 failures and 0 timeouts**.

The queued-end-destroy and uncaught pipeline leaves passed. The
`pipeline-process` failure is a child-command invocation/exit owner. The
`finished-async-local-storage` failure occurs at the AsyncContextFrame or
enabled-hooks prerequisite before the finished assertion, so it remains a
separate async-context owner. No source or upstream fixture change was made
and no mixed fix was attempted.

The selector used the existing coordinator binary through
`tools/integration/node_corpus_runner.py` with three bounded jobs and a
30-second per-file timeout. No full corpus or workspace-wide test was run; the
selector and raw runner output were removed after recording the result.

## W203 Node pipe error/flow green cluster

The bounded three-job selector covered pipe error handling, unhandled pipe
errors, flow after unpipe, flow, and multiple pipes. All **5/5 files passed**,
with **0 failures and 0 timeouts**. Per-file durations were 165–282ms.

The error-handling, error-unhandled, and flow-after-unpipe leaves are retained
as the clearest fresh additions. W182 already described the neighboring pipe
events/flow and multiple-destination cluster, so `flow.js` and
`multiple-pipes.js` are recorded as reconfirmations until the historical W182
selector mapping is recovered; this wave does not claim five unique new files.

No source or upstream fixture change was made. The selector used the existing
coordinator binary through `tools/integration/node_corpus_runner.py` with
three bounded jobs and a 30-second per-file timeout. No full corpus or
workspace-wide test was run; the selector and raw runner output were removed
after recording the result.

## W204 Node pipeline/pipe cleanup green cluster

The bounded three-job selector covered pipeline async-iterator, Duplex,
listener cleanup, empty-string input, and pipe cleanup leaves. All **5/5 files
passed**, with **0 failures and 0 timeouts**. Per-file durations were 166–299ms.

No source or upstream fixture change was made and no single failing owner was
found. The selector used the existing coordinator binary through
`tools/integration/node_corpus_runner.py` with three bounded jobs and a
30-second per-file timeout. No full corpus or workspace-wide test was run; the
selector and raw runner output were removed after recording the result.

## W205 Node stream state/lifecycle green cluster

The bounded three-job selector covered Readable async disposal,
`readableListening` state, `setEncoding(null)`, unpipe/resume behavior, and
Writable ending-state transitions. All **5/5 files passed**, with **0 failures
and 0 timeouts**. Per-file durations were 166–316ms.

No source or upstream fixture change was made and no single failing owner was
found. The selector used the existing coordinator binary through
`tools/integration/node_corpus_runner.py` with three bounded jobs and a
30-second per-file timeout. No full corpus or workspace-wide test was run; the
selector and raw runner output were removed after recording the result.

## W206 Bun Web/Atomics/URLPattern owner split

The first W206 selector used paths relative to the Bun test working directory,
which produced **5 harness load-errors / 0 tests ran**; that dispatch is
excluded from coverage. The corrected selector used repository-root-relative
paths and reached **4/5 files green**, with **447 passed / 12 failed / 459 ran /
6375 expects / 0 timeouts**.

The green files were explicit resource management (**4/4**), nationalized
AbortController (**2/2**), WebCrypto SHA-3 (**17/17**), and Atomics
(**28/28**). URLPattern reached **396 passed / 12 failed / 408 ran / 6227
expects**. Its failures span non-ASCII protocol/path URL parsing, invalid
pattern/port validation, base-URL wildcard serialization, and Unicode regexp
set matching; they remain parked as separate URLPattern owners.

No source or upstream fixture change was made. The corrected selector used the
existing coordinator binary through `tools/integration/bun_corpus_runner.py`
with three bounded jobs, a 30-second per-file timeout, and missing Node modules
allowed. No full corpus or workspace-wide test was run; both selectors and raw
runner output were removed after recording the result.

## W207 Node diagnostics_channel green cluster

The bounded three-job selector covered `diagnostics_channel` subscriber
presence, object-channel pub/sub, named pub/sub, symbol-named channels, and
synchronous unsubscribe. All **5/5 files passed**, with **0 failures and 0
timeouts**. Per-file durations were 168–252ms.

No source or upstream fixture change was made and no single failing owner was
found. The selector used the existing coordinator binary through
`tools/integration/node_corpus_runner.py` with three bounded jobs and a
30-second per-file timeout. No full corpus or workspace-wide test was run; the
selector and raw runner output were removed after recording the result.

## W208 Node events lifecycle owner split

The bounded three-job selector covered `events.addAbortListener`, event
async-iterator behavior, static `getEventListeners`, and uncaught-exception
stack shape. It reached **2/4 files passed**, with **2 failures and 0
timeouts**; per-file durations were 233–332ms.

`addAbortListener` and static `getEventListeners` stayed green. The async
iterator file fails on the invalid-argument path because the thrown error lacks
Node's `ERR_INVALID_ARG_TYPE` code. The uncaught-exception stack file reports
the throw-site location in the first stack line instead of Node's `Error`
header. These remain separate owners; no mixed fix was attempted.

No source or upstream fixture change was made. The selector used the existing
coordinator binary through `tools/integration/node_corpus_runner.py` with
three bounded jobs and a 30-second per-file timeout. No full corpus or
workspace-wide test was run; the selector and raw runner output were removed
after recording the result.

## W209 Bun Node Buffer/DOM/crypto green cluster

The bounded three-job selector covered Buffer `Symbol.toPrimitive`,
`buffer.resolveObjectURL`, Node DOMException behavior, native crypto invalid
receivers, and HKDF callback/key-object contracts. All **5/5 files were green**:
**17 passed / 0 failed / 17 ran / 74 expects / 0 timeouts**. Per-file durations
were 165–300ms.

No source or upstream fixture change was made and no single failing owner was
found. The selector used the existing coordinator binary through
`tools/integration/bun_corpus_runner.py` with three bounded jobs, a 30-second
per-file timeout, and missing Node modules allowed. No full corpus or
workspace-wide test was run; the selector and raw runner output were removed
after recording the result.

## W210 Node dgram UDP lifecycle green cluster

The bounded three-job selector covered UDP address reporting, async disposal,
default bind addresses, AbortSignal-driven close, and send callback byte
lengths. All **5/5 files passed**, with **0 failures and 0 timeouts**. Per-file
durations were 165–247ms.

No source or upstream fixture change was made and no single failing owner was
found. The selector used the existing coordinator binary through
`tools/integration/node_corpus_runner.py` with three bounded jobs and a
30-second per-file timeout. No full corpus or workspace-wide test was run; the
selector and raw runner output were removed after recording the result.

## W211 Bun Node crypto green cluster

The bounded three-job selector covered LazyHash prototype behavior, one-shot
hash/verify contracts, RSA digest variants, X509 subclassing, and random API
argument/bounds behavior. All **5/5 files were green**: **72 passed / 0 failed /
72 ran / 383 expects / 0 timeouts**. Per-file durations were 170–1423ms; the
random API stress leaf was the slowest but remained well inside the bound.

No source or upstream fixture change was made and no single failing owner was
found. The selector used the existing coordinator binary through
`tools/integration/bun_corpus_runner.py` with three bounded jobs, a 30-second
per-file timeout, and missing Node modules allowed. No full corpus or
workspace-wide test was run; the selector and raw runner output were removed
after recording the result.

## W212 Node DNS contract owner split

The bounded three-job selector covered Resolver server listing, dns/promises
constant exports, resolver server-type validation, lookup-promises option
validation, and Resolver `maxTimeout` behavior. It reached **3/5 files passed**,
with **2 failures and 0 timeouts**; per-file durations were 166–317ms.

`test-dns-get-server.js`,
`test-dns-lookup-promises-options-deprecated.js`, and
`test-dns-setservers-type-check.js` stayed green. The promises-constant file
is missing the `ENODATA` export. The max-timeout file reaches the intended
range checks but the thrown error does not match Node's expected code/shape.
These are separate DNS owners; no mixed fix was attempted.

No source or upstream fixture change was made. The selector used the existing
coordinator binary through `tools/integration/node_corpus_runner.py` with
three bounded jobs and a 30-second per-file timeout. No full corpus or
workspace-wide test was run; the selector and raw runner output were removed
after recording the result.

## W213 Bun VM/TLS/zlib owner split

The bounded three-job selector covered VM `sourceURL` stack/output behavior,
Node TLS internal helpers, and a zlib native-handle re-entrancy guard. It reached
**2/3 files green: 5 passed / 3 failed / 8 ran / 262 expects / 0 timeouts**;
per-file durations were 201–502ms.

`vm-sourceUrl.test.ts` was **3/3** and `node-tls-internals.test.ts` was **2/2**.
The zlib file ran all three bounded iterations, but each child stopped before
the intended re-entrancy assertion because the native handle did not expose the
expected `write` method. This is recorded as a zlib native-handle exposure
owner, separate from the intended write/close lifetime behavior.

No source or upstream fixture change was made. The selector used the existing
coordinator binary through `tools/integration/bun_corpus_runner.py` with three
bounded jobs, a 30-second per-file timeout, and missing Node modules allowed.
No full corpus or workspace-wide test was run; the selector and raw runner
output were removed after recording the result.

## W132 Node VM and WebStreams owner split

The bounded three-job selector covered two VM proxy/property leaves, two
WebStreams contracts, and a URLSearchParams iterator contract. Three files
passed (**3/5**): VM own-property names, URLSearchParams entries, and
WritableStream close, taking 164–251ms.

The two failures are separate owners:

- `test-vm-global-setter.js` rejects the readonly assignment correctly but
  reports `Attempted to assign to readonly property.` instead of Node's
  `Cannot redefine property: nonWritableProp`; issue
  [#66](https://github.com/Sunrisepeak/mbun/issues/66) records the message-only
  contract mismatch.
- `test-whatwg-webstreams-encoding.js` expects invalid receivers for five
  TextDecoderStream accessors to throw private-brand TypeErrors. mbun returns
  `undefined` for all five; a direct bounded probe reproduces it. Issue
  [#67](https://github.com/Sunrisepeak/mbun/issues/67) records that accessor
  owner.

No source or upstream fixture change was made. No full corpus or workspace-wide
test was run; the selector and raw runner output were removed after recording
the result.

## W131 Node VM and URL green leaves

The bounded three-job selector covered VM indexed properties, VM global
assignment, legacy URL query parsing, IDN ASCII/Unicode conversion, and
`URLSearchParams.prototype.forEach` invalid-this validation. All **5/5 files
passed**, with **0 failures** and **0 timeouts**; per-file durations were
164–215ms.

No source or upstream fixture change was made. The selector used the existing
coordinator binary through `tools/integration/node_corpus_runner.py` with
three bounded jobs and a 30-second per-file timeout. No full corpus or
workspace-wide test was run; the selector and raw runner output were removed
after recording the result.

## W130 Node REPL owner triage

The bounded three-job selector covered the main REPL, autoloaded libraries, and
multiline navigation. `test-repl-multiline-navigation.js` passed in 281ms;
`test-repl-autolibs.js` failed in 182ms; and `test-repl.js` timed out at the
30-second bound after its first `message` evaluation.

Both non-green paths first report
`RegExp.$N getters require RegExp constructor as |this|`. A direct bounded
probe reading `RegExp.$1` reproduces the same TypeError without REPL, so this
is parked as one RegExp static getter receiver owner rather than two REPL
owners. Issue [#65](https://github.com/Sunrisepeak/mbun/issues/65) records the
sanitized reproduction. No source or upstream fixture change was made, and no
full corpus or workspace-wide test was run.

## W129 console iterator owner triage

The corpus-specific selectors covered one Bun console iterator file and two
Node lifecycle leaves. The valid result was **2/3 files green**: the Node
closed-socket and domain uncaught-exception files both passed in 248–250ms.
The Bun file reached **0 passed / 17 failed / 17 ran**, with no timeout.

All Bun failures share one owner. Its child fixture uses
`for await (const line of console)`, but mbun reports an undefined-function
TypeError at that input loop and returns empty output for static, streaming,
and repeated-iterator cases. Issue
[#64](https://github.com/Sunrisepeak/mbun/issues/64) records the sanitized
console async-iterator/input reproduction.

No source or upstream fixture change was made. No full corpus or workspace-wide
test was run; selectors and raw runner output were removed after recording the
result.

## W128 Node worker stress confirmation

The bounded three-job selector covered MessagePort close/race delivery,
MessagePort close behavior, worker thread names, worker async-module exit, and
clean worker exit. The initial result was **4/5 pass** with one 30-second
timeout and no failure. The timeout had no error output or residual process.

An isolated one-job rerun with a 60-second per-file timeout passed the MessagePort
race file in **57.762s**, confirming a slow stress path rather than a stable
hang. The other four files passed in 165–651ms. The five leaves are retained,
but the 10,000-message race file is not used in the default 30-second fast
lane.

No source or upstream fixture change was made. No full corpus or workspace-wide
test was run; selectors and raw runner output were removed after recording both
the initial bounded result and the isolated confirmation.

## W127 console TablePrinter triage

The selectors were kept corpus-specific: Bun covered `console-write.test.ts`
and `console-table.test.ts`; Node covered HTTP upgrade, URL auth-header, and
net capture-rejection leaves. The corrected result is **4/5 valid files green**.
Bun reached **31 passed / 2 failed / 34 ran / 84 expects**; `console.write` was
**1/1** and `console.table` was **30 passed / 2 failed / 33 ran**. All three
Node files passed, taking 251ms, 4.265s, and 249ms.

The two `console.table` failures are exact padding differences in the same
TablePrinter alignment policy: a primitive Values cell has internal padding,
and a header is centered where the reference is left-aligned. Getter access,
custom inspection, GC, and iteration cases remain green. Issue
[#63](https://github.com/Sunrisepeak/mbun/issues/63) records the sanitized
single-owner reproduction.

No source or upstream fixture change was made. No full corpus or workspace-wide
test was run; selectors and raw runner output were removed after recording the
result.

## W126 corrected mixed-lane triage

The first attempt accidentally sent a mixed Bun/Node selector to the Bun
runner; its two Node rows were `no-tests` and are excluded from all coverage
counts. The selector was corrected into separate bounded runners without
rerunning the Bun files.

The valid Bun result was **2/3 files green**, **350 passed**, **53 failed**,
**403 ran**, and **54,616 expects**: `index-of-line.test.ts` passed **4/4**,
`bun-main.test.ts` passed **2/2**, and `bun-cryptohasher.test.ts` reached
**344 passed / 53 failed / 397 ran**. The valid Node runner then passed both
HTTP files (**2/2**), taking 232ms and 6.242s. Thus the corrected five-file
lane is **4/5 green** with no timeout.

CryptoHasher failures split into unsupported HMAC keying and a separate
unsupported-algorithm matrix expectation; they are parked separately rather
than treated as one source fix. No source or upstream fixture change was
made. No full corpus or workspace-wide test was run.

## W125 Node HTTP and fs green leaves

The bounded three-job selector covered HTTP agent error/close handling, empty
HTTP writes, a recursive `cpSync` symlink error, file WriteStream uncorking,
and an uncaught request-callback path. All **5/5 files passed**, with **0
failures** and **0 timeouts**. Four files took 215–234ms; the empty-write HTTP
file took 4.289s and remained below the 30-second bound.

No source or upstream fixture change was made. The selector used the existing
coordinator binary through `tools/integration/node_corpus_runner.py` with three
bounded jobs and a 30-second per-file timeout. No full corpus or workspace-wide
test was run; the selector and raw runner output were removed after recording
the result.

## W124 Bun util owner triage

The bounded three-job selector covered Bun object initialization, BunString
thread-safe refcounts, Error GC, Bun.file behavior, and stdin slicing. The
result was **2/5 files green**, **11 passed**, **6 failed**, **17 ran**, and
**250 expects**, with no timeout.

The retained green leaves are stdin slicing (**2/2**) and Error GC (**4/4**).
The non-green files are intentionally split:

- `BunObject.test.ts`: **2 passed / 1 failed / 3 ran**; the one failure is the
  missing `hasNonReifiedStatic` internal test helper, while module import and
  Bun object identity checks pass.
- `bunstring-tothreadsafe.test.ts`: **1 passed / 1 failed / 2 ran**; the real
  Bun.file/fs.write caller balance passes, while the optional internal
  refcount-delta helper is not exported.
- `bun-file.test.ts`: **2 passed / 4 failed / 6 ran**; three failures are
  async-stack frame formatting and one is the empty-JSON parse message. These
  are separate from the two internal-helper gaps.

No source or upstream fixture change was made. The selector used the existing
coordinator binary through `tools/integration/bun_corpus_runner.py` with three
bounded jobs, a 30-second per-file timeout, and missing Node modules allowed.
Raw output was discarded without copying its local environment expansion into
this record. No full corpus or workspace-wide test was run.

## W123 Node fs and HTTP green leaves

The bounded three-job selector covered WriteStream option validation, two
`cpSync` error contracts, HTTP responses without Content-Length, and HTTP agent
timeout handling. All **5/5 files passed**, with **0 failures** and **0
timeouts**; per-file durations were 215–265ms.

No source or upstream fixture change was made. The selector used the existing
coordinator binary through `tools/integration/node_corpus_runner.py` with three
bounded jobs and a 30-second per-file timeout. No full corpus or workspace-wide
test was run; the selector and raw runner output were removed after recording
the result.

## W122 Bun readableStreamToArrayBuffer owner triage

The bounded three-job selector covered Bun.file offset reads, fd-backed reads,
ArrayBufferSink, file MIME type, and `readableStreamToArrayBuffer`. The result
was **4/5 files green**, **12 passed**, **2 failed**, **14 ran**, and **432
expects**. The four file/FD/MIME/sink leaves are retained.

The only failure was `readablestreamtoarraybuffer.test.ts`: both tests patch
`Promise.prototype.then` and observe **6 calls** from mbun. Node/Bun expect zero
calls for a synchronous stream start and one observable adoption call for an
async start. This is one intrinsic Promise-plumbing owner, not two unrelated
stream failures. Issue
[#62](https://github.com/Sunrisepeak/mbun/issues/62) records the sanitized
reproduction and keeps the fix scoped to that API.

No source or upstream fixture change was made. The selector used the existing
coordinator binary through `tools/integration/bun_corpus_runner.py` with three
bounded jobs, a 30-second per-file timeout, and missing Node modules allowed.
No full corpus or workspace-wide test was run; the selector and raw runner
output were removed after recording the result.

## W161 Bun WebStreams green cluster

The bounded three-job selector covered readable-stream Blob consumption,
synchronous pull fast paths, TransformStream leak handling, and native-source
close handling. All **4/4 files were green**, reaching **15 passed / 0 failed /
15 ran / 23 expects**, with **0 timeouts**. Per-file durations were 200ms–4.121s;
the native-source-onclose leaf is retained as a slow but bounded guard.

No source or upstream fixture change was made. The selector used the existing
coordinator binary through `tools/integration/bun_corpus_runner.py` with
three bounded jobs, a 30-second per-file timeout, and missing Node modules
allowed. No full corpus or workspace-wide test was run; the selector and raw
runner output were removed after recording the result.

## W121 Node zlib metric triage

The bounded three-job selector covered `test-http-agent-false.js`,
`test-http-listening.js`, `test-dgram-blocklist.js`, `test-fs-buffer.js`, and
`test-zlib-unused-weak.js`. Four files passed; the zlib file was the only
failure, with **4/5 pass**, **1 fail**, and **0 timeouts**. The four passing
leaves are retained.

The zlib assertion measures the external-memory delta before and after creating
100 gzip handles and after GC. A minimal bounded probe reproduces
`before=0`, `afterCreation=0`, and `afterGC=0`, so the failure is a zero-denominator
`process.memoryUsage().external` accounting/GC metric owner rather than a
single zlib operation error. It is parked without a speculative issue or
source change until the memory accounting contract is isolated from zlib handle
lifetime.

No source or upstream fixture change was made. The selector used the existing
coordinator binary through `tools/integration/node_corpus_runner.py` with three
bounded jobs and a 30-second per-file timeout. No full corpus or workspace-wide
test was run; the selector and raw runner output were removed after recording
the result.

## W120 Bun util green leaves

The bounded three-job selector covered UTF-16 allocation fallback, file MIME
types, unsafe buffer/string conversion, promise peeking, and main-thread worker
identity. All **5/5 files were green**, with **15 passed / 0 failed / 15 ran /
43 expects**. The worker identity guard took 718ms; the other files took
166–249ms.

No source or upstream fixture change was made. The selector used the existing
coordinator binary through `tools/integration/bun_corpus_runner.py` with three
bounded jobs, a 30-second per-file timeout, and missing Node modules allowed.
No full corpus or workspace-wide test was run; the selector and raw runner
output were removed after recording the result.

## W162 Bun WebStreams owner split

The bounded three-job selector covered compression, stream leak guards, and the
larger WebStreams contract file. It reached **1/3 files green**, with **159
passed / 13 failed / 172 ran / 350 expects** and **0 timeouts**. Compression
was fully green (**11/11**). The two `streams-leak` failures split between
native pull-buffer behavior and memory accounting. The 11 failures in
`streams.test.js` span error shape/stack, string-allocation-limit wording,
read batching, controller state, foreign-realm construction, and async
iterator reentrancy. These remain parked as separate owners.

No source or upstream fixture change was made. The selector used the existing
coordinator binary through `tools/integration/bun_corpus_runner.py` with
three bounded jobs, a 30-second per-file timeout, and missing Node modules
allowed. No full corpus or workspace-wide test was run; the selector and raw
runner output were removed after recording the result.

## W119 Node assert owner triage

The bounded three-job selector covered Node `assert`, Buffer negative-allocation
validation, diagnostics-channel tracing, TextDecoder `ignoreBOM`, and HTTP
header validators. The result was **4/5 files pass**, **1 fail**, and **0
timeouts**, with per-file durations of 165–251ms. The four passing leaves are
retained.

The only failure was `test-assert-class.js`: all 12 subtests fail from the same
missing surface. A minimal bounded probe reports
`typeof require("assert").Assert === "undefined"`; calling `assert.Assert()`
produces a `TypeError` without a code, while Node exposes the constructor and
reports `ERR_CONSTRUCT_CALL_REQUIRED` when called without `new`. Issue
[#61](https://github.com/Sunrisepeak/mbun/issues/61) records the sanitized
reproduction and suspected Node assert export/implementation owner.

No source or upstream fixture change was made. The selector used the existing
coordinator binary through `tools/integration/node_corpus_runner.py` with three
bounded jobs and a 30-second per-file timeout. No full corpus or workspace-wide
test was run; the selector and raw runner output were removed after recording
the result.

## W117 Bun util owner triage

The bounded three-job selector covered `cookie.test.js`, UUIDv5 and UUIDv7
generation, `stringWidth.test.ts`, and `inspect-error.test.js`. The result was
**2/5 files green**, **288 passed**, **54 failed**, **360 ran**, and **1297
expects**. Cookie (**101/101**) and UUIDv5 (**40/40**) are retained as green
leaves.

The remaining failures are intentionally parked as separate owners:

- UUIDv7 reached **7 passed / 11 failed / 18 ran**. Failures cover 12-bit
  counter rollover, timestamp validation, explicit older timestamps, and
  pseudo-random counter seeding; this is broader than one monotonicity branch.
- `stringWidth.test.ts` reached **139 passed / 34 failed / 173 ran**. Failures
  span C1/ST control-sequence stripping, ANSI consistency, fuzzer-like input,
  UTF-16 bulk width, combining marks, Jamo, and Unicode 16/17 width behavior.
- `inspect-error.test.js` reached **1 passed / 9 failed / 10 ran**. Failures
  span Bun rich source-context formatting, cause/error snapshots,
  BuildMessage, source-map location properties, and long-file diagnostics.

No source or upstream fixture change was made. The selector used the existing
coordinator binary through `tools/integration/bun_corpus_runner.py` with three
bounded jobs, a 30-second per-file timeout, and missing Node modules allowed.
No full corpus or workspace-wide test was run. The raw runner output was
discarded after extracting sanitized counts; local absolute paths and machine
details were not copied into this record.

## W115 streams owner triage

The bounded three-job selector covered `test-stream-readable-no-unneeded-readable.js`,
`test-stream-pipe-unpipe-streams.js`, `test-stream-destroy-event-order.js`,
`test-stream-writable-samecb-singletick.js`, and `test-stream-readable-aborted.js`.
Four files passed; the only failure was `test-stream-writable-samecb-singletick.js`.
Per-file durations were 165–235 ms, with no timeout. A one-job/60-second
isolated rerun reproduced the same failure.

The failure is `Mismatched noop function calls. Expected exactly 1, actual 0`.
Node creates one `TickObject` for the callback-less `Console`/`Writable` write
sequence. mbun's direct `process.nextTick()` probe and a `Writable.write()` with
an explicit user callback both report `TickObject`; only the callback-less
Console path omits it. The owner is therefore the synchronous
`node_stream_writable.cppm` after-write path, whose `afterWriteTick` condition
does not cover a no-op user callback. This is separate from async_hooks
registration.

Issue [#60](https://github.com/Sunrisepeak/mbun/issues/60) records the sanitized
reproduction and owner. No source or upstream fixture change was made; four
green streams leaves are retained, and no full corpus or workspace-wide test
was run.

## W113 process feature triage

The bounded three-job selector covered `test-process-env-tz.js`,
`test-process-env-allowed-flags.js`, `test-process-execve-validation.js`,
`test-process-threadCpuUsage-main-thread.js`, and
`test-process-no-deprecation.js`. Four files passed; the only failure was
`test-process-env-tz.js`. Per-file durations were 165–265 ms, with no timeout.

An isolated one-job/60-second rerun reproduced the same failure. Node changes
one existing `Date` from `Europe/Amsterdam` (`+0200`) to `Europe/London`
(`+0100`) and then `Etc/UTC` (`+0000`). mbun changes the display names but keeps
the first `+0200` offset. The setter reaches `__mbunProcNative.setTimeZone` and
the VM DateCache reset, but the existing JSC Date instance reuses its cached
local Gregorian value keyed only by milliseconds after the first
`toString()`. This identifies cache invalidation as the owner, not host TZ
variance or missing environment propagation.

Issue [#59](https://github.com/Sunrisepeak/mbun/issues/59) records the sanitized
reproduction and source owner. No source or upstream fixture change was made;
the four green process leaves are retained, and the TZ owner is parked until a
minimal existing-Date invalidation design is verified. No full corpus or
workspace-wide test was run.

## W96 delivered slice

W96 started with a fresh five-file Node HTTP/2 probe. Four files passed; the
only failure was `test-http2-client-rststream-before-connect.js`, where the
client readable `end` event and server-side `ERR_HTTP2_STREAM_ERROR` were both
missing from the RST lifecycle. A tiny standalone reproduction confirmed that
the client `_destroy()` hook itself was called once, so the owner was narrowed
to RST event ordering rather than stream destruction dispatch.

Issue [#54](https://github.com/Sunrisepeak/mbun/issues/54) landed in commit
`fa2373e`:

- `modules/jsc/src/js_http2.cppm` now ends the readable side before delivering
  the client reset error, with destruction deferred until the readable
  next-tick completion.
- `modules/jsc/src/js_http2_part2.cppm` routes non-zero server-side peer resets
  through `_destroy`, preserving `ERR_HTTP2_STREAM_ERROR`; CANCEL remains a
  non-error destroy.

Evidence from the fresh coordinator build:

- `bash tools/integration/build_or_die.sh` — pass.
- W96 Node selector — **5/5 files pass** after the fix; the four pre-existing
  green files stayed green.
- Adjacent Bun HTTP/2/Worker selector — **5/5 files green**, **8 passed / 0
  failed / 8 ran / 8 expects**.
- The standalone lifecycle smoke observed client `end`, client/server stream
  errors, close callback, and `_destroy()` exactly once. It is diagnostic only
  and is not counted as corpus coverage.

No upstream fixture changed and no full corpus/workspace-wide test was run.

## W97 delivered slice

W97 selected five adjacent Node HTTP/2 teardown paths: stream destroy before
connect, session destroy, session close before stream close, shutdown before
connect, and upload rejection. The fresh pre-fix result was **4/5 files pass**;
`test-http2-client-destroy.js` failed because a connect-level AbortSignal made
the pending request receive `ABORT_ERR` instead of Node's
`ERR_HTTP2_STREAM_CANCEL`.

Issue [#55](https://github.com/Sunrisepeak/mbun/issues/55) landed in commit
`ed9c854`:

- the client session consumes the connect AbortSignal exactly once and removes
  it from the underlying net/tls option copy;
- an aborted session still emits `AbortError` / `ABORT_ERR`, while all streams
  in that session are torn down with the pending-stream cancellation error;
- ordinary session teardown keeps its previous open-stream behavior.

Evidence from the fresh coordinator build:

- `bash tools/integration/build_or_die.sh` — pass.
- W97 Node selector — **5/5 files pass** after the fix.
- W96 Node HTTP/2 regression selector — **5/5 files pass**.
- W95 Bun HTTP/2/Worker regression selector — **5/5 files green**, **8
  passed / 0 failed / 8 ran / 8 expects**.
- Plain and secure connect-abort smoke both observed session `ABORT_ERR` and
  request `ERR_HTTP2_STREAM_CANCEL`; the bounded corpus selector is the
  authoritative result.

No upstream fixture changed and no full corpus/workspace-wide test was run.

## W98 Bun leaf coverage

W98 used five bounded jobs and the W97 coordinator binary without a build. The
selected files were Bun sleep, Deno URLSearchParams, Node X509, TextDecoder,
and Bun require resolution. The result was **4/5 files green**, **161 passed**,
**1 ahead-of-reference**, **165 ran**, and **100,536 expects**.

The four green files were `sleep.test.ts` (**2/2**),
`urlsearchparams.test.ts` (**32/32**), `x509.test.ts` (**14/14**), and
`text-decoder.test.js` (**104/104**). `resolve/require.test.ts` reached **9
passed / 1 ahead-of-reference / 3 todo / 13 ran**: its only failure is a Bun
`test.failing` case that now passes in mbun, so it is recorded as more correct
than the reference rather than treated as an implementation regression.

No source owner was opened from W98, no upstream fixture changed, and no full
corpus/workspace-wide test was run.

## W99 Bun cross-subsystem leaf coverage

W99 used five bounded jobs and the same binary without a build. The initial
five-file result was **4/5 files green**, **108 passed**, **1 failed**, **109
ran**, and **355 expects**. The stable green leaves were:

- `spawn/null-byte-injection.test.ts`: **20/20**;
- `fs/fs-leak.test.js`: **4/4**;
- `streams/pipeTo-signal-leak.test.ts`: **2/2**;
- `web/url/url.test.ts`: **14/14**.

`node-dns.test.js` reached **68/69** in the 5-job probe. Its only failure was
the expected IPv6 address for a public DNS name differing from the answer
returned by the resolver. An isolated 1-job rerun reached **69/69**; a second
five-job rerun reproduced the external answer variance. This is not a stable
runtime owner, so DNS stays parked without a source change.

No upstream fixture changed, no issue was opened, and no full
corpus/workspace-wide test was run.

## W100 Node FileHandle leaf coverage

W100 used five bounded jobs and the existing coordinator binary without a
build. The selected `fs/promises` FileHandle leaves were close-errors,
aggregate-errors, pull, readFile, and writer. All **5/5 files passed**; no
upstream fixture changed and no source owner was opened. This extends W93's
green chmod/stat/truncate/write/sync set without rerunning the full fs subtree.

## W101 Node Module.runMain preload hook

W101 selected five module-loader leaves with five bounded jobs: builtin
identity, createRequire, readonly/wrapper behavior, and the CLI
`runMain` monkey-patch fixture. The pre-fix result was **3/5 files pass**,
**1 skipped** (Windows-only), and **1 failed**. The failure was isolated to
the native entry dispatch: `--require` loaded the preload, but the main file
was evaluated directly, so a preload replacing `Module.runMain` never ran.

Issue [#56](https://github.com/Sunrisepeak/mbun/issues/56) landed in commit
`f296040`. For a Node-dialect entry with preloads, the runtime now calls
`Module.runMain()` after preload execution; Bun continues through its existing
entry wrapper. This restores the observable Node hook without changing the
upstream fixture.

Evidence from the fresh coordinator build:

- `bash tools/integration/build_or_die.sh` — pass.
- W101 rerun — **4/5 files pass**, **1 skipped**, **0 failed**; the skipped
  fixture is Windows-only.
- W100 FileHandle regression selector — **5/5 files pass**.

No upstream fixture changed, no full corpus/workspace-wide test was run, and
the temporary selectors/output were cleaned after verification.

## W102 Node module introspection and lookup leaves

W102 used five bounded jobs and the W101 coordinator binary without a build.
The selected leaves covered `process.config` module-version metadata,
`Module._stat`, builtin-module listing, relative lookup priority, and multi-part
extension resolution. The result was **4/5 files pass** and **1 failed**.

The four green leaves are retained. The only failure is the multi-extension
fixture, which installs mutable `require.extensions` handlers. The current
`node:module` implementation documents custom `require.extensions` loader
integration as deferred native CJS-loader work, so this remains parked without
an issue or speculative source change.

No upstream fixture changed, no build or full corpus/workspace-wide test was
run, and the temporary selector/output were cleaned after verification.

## W103 Bun node:module and SourceMap leaves

W103 used five bounded jobs and the W101 coordinator binary without a build.
The selected files covered the public `node:module` surface, `options.paths`
resolution, SourceMap construction, malformed-map handling, and the concurrent
GC children guard. The aggregate result was **3/5 files green**, **44 passed**,
**10 failed**, **54 ran**, and **142 expects**.

The stable green files were:

- `module-resolve-filename-paths.test.js`: **6/6**, 10 expects;
- `module-sourcemap.test.js`: **3/3**, 6 expects;
- `module-children-concurrent-gc.test.ts`: **1/1**, 2 expects.

The two non-green files have separate owners. `node-module-module.test.js`
reported **21 passed / 9 failed / 30 ran / 102 expects**, spanning builtin
inventory, overridden `_resolveFilename`/`Module.prototype.require`, builtin
cache export shape, `Module.runMain`, and children-tree semantics. The
standalone `sourcemap.test.js` reported **13 passed / 1 failed / 14 ran / 22
expects**: the malformed inline map did not emit the expected decode warning
while preserving the unmapped stack. Current native CJS hook and source-map
stack/diagnostic integration are separate deferred surfaces, so no speculative
issue or mixed fix was opened from W103.

No upstream fixture changed, no build or full corpus/workspace-wide test was
run, and the temporary selector/output were cleaned after verification.

## W104 Bun Node process and stdio leaves

W104 used five bounded jobs and the W101 coordinator binary without a build.
The selected Linux-focused leaves covered synthetic `memoryPressure`, signal
listener install/remove/reinstall, callable process construction, accessor
guards for `setgroups`/`hrtime`, and invalid UTF-16 writes to stdout/stderr.
All **5/5 files passed**, with **39/39 tests**, **0 failures**, and **93
expects**.

Per-file results:

- `process-memory-pressure.test.ts`: **5/5**, 10 expects;
- `process-signal-listener-count.test.ts`: **3/3**, 10 expects;
- `call-constructor.test.js`: **2/2**, 1 expect;
- `process-array-accessor-crash.test.ts`: **5/5**, 6 expects;
- `process-stdio-invalid-utf16.test.ts`: **24/24**, 66 expects.

No source owner was opened, no upstream fixture changed, no build or full
corpus/workspace-wide test was run, and the temporary selector/output were
cleaned after verification.

## W105 Node process identity and timing leaves

W105 used five bounded jobs and the W101 coordinator binary without a build.
The selected leaves covered `process.argv[0]`, `process.uptime()`, symlinked
`process.execPath`, parent-process identity, and Linux `fs.constants.O_NOATIME`.
All **5/5 files passed** with no source owner or upstream fixture change.

No build or full corpus/workspace-wide test was run, and the temporary
selector/output were cleaned after verification.

## W106 Node process exitCode validation leaves

W106 selected five Linux-focused process leaves: high-resolution time,
'process.exitCode' validation and deletion, environment-key deletion, and
timer lifetime tracking. The pre-fix bounded probe was **4/5 files pass**;
the only visible failure was the strict deletion assertion for
'process.exitCode'.

Issue [#57](https://github.com/Sunrisepeak/mbun/issues/57) isolated the owner.
The process slot was wrapped with Node's validation setter but remained
configurable, and JSC's generic non-configurable deletion diagnostic omitted
the property and receiver. The fix in commit 92dd963 makes the slot
non-configurable, formats only the Node process deletion through a narrow
proxy, and preserves invalid 'process.exit(code)' as status 1 instead of
falling through to status 0.

Evidence from the fresh coordinator build:

- bash tools/integration/build_or_die.sh — pass.
- W106 rerun with three bounded jobs — **5/5 files pass**.
- W105 adjacent Node process identity/timing guard — **5/5 files pass**.

No upstream fixture changed, no full corpus/workspace-wide test was run, and
temporary selectors/output were cleaned after verification.

## W107 Node net low-coupling leaves

W107 reused the fresh W106 coordinator binary and ran five bounded jobs without
a build. The selected leaves covered IPv4 classification, argument
normalization, Socket construction, listening state, and local address/port
reporting. All **5/5 files passed**; no source owner or issue was opened.

No upstream fixture changed, no full corpus/workspace-wide test was run, and
the temporary selectors/output were cleaned after verification.

## W108 Bun Node-net constructor and server leaves

W108 used three bounded jobs and the W106 binary. The fresh pre-fix selector
measured **2/5 files green**, **147 passed**, **20 failed**, **175 ran**, and
**300 expects**:

- 'blocklist-gc.test.ts' and 'node-net-server.test.ts' were green;
- 'socketaddress.spec.ts' had five failures from the Bun matcher
  toThrowWithCode being unavailable in the mbun test harness;
- 'node-net.test.ts' had nine failures across unref/liveness, flowing state,
  AbortError, heap statistics, fd adoption, and reset behavior;
- 'server.spec.ts' had six failures concentrated in constructor/prototype
  shape and default fields.

Issue [#58](https://github.com/Sunrisepeak/mbun/issues/58) isolated the
server-shape owner. Commit 1d755dc aligns the callable constructor parent,
publishes _connections, _unref, _usingWorkers, and highWaterMark, keeps
connection counts synchronized, and matches the Bun EventEmitter prototype
descriptor shape.

Post-fix evidence from a fresh serialized build:

- W108 full selector — **2/5 files green**, **152 passed**, **15 failed**,
  **175 ran**, **301 expects**; five server runtime shape failures were
  removed.
- 'server.spec.ts' focused result — **37 passed / 1 failed / 41 ran / 58
  expects**. The remaining failure is the mbun harness toMatchObject
  own-property treatment of inherited EventEmitter methods; changing the
  real constructor identity to satisfy it would diverge from Bun/Node and is
  parked.
- W107 Node net regression selector — **5/5 files pass**.

No upstream fixture changed, no full corpus/workspace-wide test was run, and
temporary selectors/output were cleaned after verification.

## W109 Node TLS constructor and default-option leaves

W109 reused the W108 fresh binary and ran five bounded jobs without a build.
The selector measured **3/5 files pass**, **1 fail**, and **1 timeout**:

- TLS server identity checking, no-host connect, and boolean option validation
  passed;
- 'test-tls-server-parent-constructor-options.js' failed only when an accepted
  TLS socket expected pauseOnConnect to remain true;
- 'test-tls-socket-default-options.js' produced no test output and timed out.
  A one-job, 60-second isolated rerun reproduced the same silent timeout, so
  it is not counted as a parallel-load flake.

The two non-green results have different owners and no source change was made:
the pauseOnConnect path needs TLS accepted-socket propagation analysis, while
the silent timeout needs a bounded fixture/liveness investigation. No
speculative issue was opened.

No upstream fixture changed, no full corpus/workspace-wide test was run, and
temporary selectors/output were cleaned after verification.

## Delivered slice

The frozen binary reported `test-runner-string-to-regexp.js` as `fail`; the
coordinator binary built with GCC16 reports `pass`. The fix wraps the native
RegExp constructor only to restore Node's invalid-flags diagnostic and repairs
the observable prototype constructor identity. It does not alter successful
RegExp matching or construction.

Evidence:

- `bash tools/integration/build_or_die.sh` — coordinator build passed.
- `python3 tools/integration/node_corpus_runner.py --bin <coordinator-bin> --files <file-list> ...`
  — target file **1/1 pass**.
- Direct smoke — function call, `new`, `instanceof`, constructor identity, and
  invalid-flags error text all passed.
- Regression comparison — `test-runner-option-validation.js` frozen/new both
  **1/1**; `test-util-inspect-regexp.js` and `test-util-inspect.js` frozen/new
  both **0/2**, so those pre-existing inspect failures were not counted as
  regressions.

The next coordinator build also repaired one isolated `util.promisify` contract:

- `internal/util`'s private `customPromisifyArgs` identity is normalized at the
  loader boundary; the public bootstrap implementation now maps multiple
  callback values to the named object.
- Direct smoke with a function decorated by the internal symbol produced
  `{"first":5,"second":17}`.
- `test-fs-readv-promisify.js` is **1/1 pass** after the build.
- `test-util-promisify.js` remains **0/1 file green** because two independent
  warning-contract checks still fail; its custom-args assertion no longer fails.
- The fresh build kept `test-runner-option-validation.js` and
  `test-runner-string-to-regexp.js` at **1/1** each.

The following runner slice wires the existing vendored `SnapshotManager` into
the standalone TestContext:

- `t.assert.snapshot()` and `t.assert.fileSnapshot()` now use one per-process
  manager, including update/read mode and exit-time writes.
- `node:test.snapshot.setResolveSnapshotPath()` and
  `setDefaultSnapshotSerializers()` are exposed.
- `test-runner-snapshot-file-tests.js` is **1/1 pass**; its validation and
  update/read flows pass.
- The multi-file `--test --test-isolation=none` path now works from an arbitrary
  temporary cwd: an update/read round reaches **6/6 pass** for both fixture
  files. The fix discovers the vendored Node `lib` root from the entry file for
  `internal/*` requests and temporarily anchors the snapshot loader during its
  one-time builtin-map initialization.
- Serial `test-runner-snapshot-tests.js` now reaches **33/33 subtests pass**.
  The TAP reporter includes the failed assertion's structured error message, so
  the child negative case observes `Missing snapshots` as Node does. A five-lane
  concurrent probe also hit a shared child-shim `ENOENT`; the serial rerun is
  the authoritative measurement and the shim race is not counted as a runtime
  regression.
- `test-runner-assert.js` now passes the `t.assert` method enumeration and fails
  only on source-expression stack enrichment. No no-op snapshot methods were
  added.

The remaining `util.promisify` warning contract is now a separate green slice:

- The public bootstrap wrapper now retains the original callback-style
  function's return value and emits Node's `DEP0174` deprecation warning when
  that value is a Promise. The existing custom-promisify path is unchanged.
- The original corpus file `test-util-promisify.js` is **1/1 file pass**; both
  warning expectations fire, and the earlier custom-argument assertions remain
  covered by the separate readv test.
- The regression `test-fs-readv-promisify.js` remains **1/1 pass**.
- A three-lane bounded regression probe kept
  `test-runner-snapshot-file-tests.js` at **7/7 assertions** and
  `test-runner-option-validation.js` at **12/12 assertions** while the
  promisify wrapper changed. No full corpus run was started.

A follow-up low-risk `node:util` probe used five parallel bounded lanes. Five
files exited successfully: `test-util-getcallsites.js`,
`test-util-getcallsites-preparestacktrace.js`, `test-util-stripvtcontrolcharacters.js`,
`test-util-types-exists.js`, and `test-util-parse-env.js`. The nearby
`test-util-callbackify.js` lane stayed red on one stack-shape expectation
(`processTicksAndRejections`), while `test-util-format.js`,
`test-util-types.js`, and `test-util-inspect-getters-accessing-this.js` each
hit one distinct semantic blocker. Those three were not changed speculatively.

The next targeted slice added the missing external-value identity for the
internal `JSStream` test handle:

- `util.types.isExternal()` now uses a private WeakSet identity registry, and
  `JSStream` exposes its non-enumerable `_externalStream` test value through
  that registry. Plain objects are not classified as external.
- `test-util-types.js` moved past its first blocker (`undefined` did not match
  `isExternal`) and now reaches the later V8-native-syntax probe for
  `%PrepareFunctionForOptimization`; it remains **1 file fail**, so no new
  green file is claimed from this slice.
- Fresh five-lane regression evidence is all exit 0:
  `test-util-types-exists.js`, `test-util-getcallsites.js`,
  `test-util-parse-env.js`, `test-util-promisify.js`, and
  `test-runner-snapshot-file-tests.js`.

The V8-native-syntax blocker was isolated to optimization-only controls rather
than type semantics:

- Under the explicit `--allow-natives-syntax` flag only, the runtime accepts
  `%PrepareFunctionForOptimization(...)` and `%OptimizeFunctionOnNextCall(...)`
  as no-ops. JSC has no equivalent V8 optimizer controls; ordinary processes
  retain the native `eval` path.
- `test-util-types.js` now exits 0 and exercises the complete type-predicate
  matrix, including the JSStream external value.
- Three parallel fast-call regressions also exit 0:
  `test-buffer-swap-fast.js`, `test-timers-fast-calls.js`, and
  `test-os-fast.js`.

The next measured slice closed the `process.hrtime` argument contract:

- `3427591` validates a supplied previous-time value as an Array of exactly two
  entries, preserving the existing nanosecond subtraction and reporting the
  corresponding Node error codes.
- `test-process-hrtime.js` moved from the observed missing TypeError to exit 0
  after the RED-to-GREEN change. Fresh bounded regressions also exit 0 for
  `test-process-hrtime-bigint.js`, `test-util-types.js`,
  `test-buffer-swap-fast.js`, and `test-runner-snapshot-file-tests.js`.
- This is a targeted runtime result, not a new full-corpus score. The remaining
  `test-whatwg-url-canparse.js` TypeError mismatch stays an independent blocker.

The URL.canParse candidate then closed with a narrow bootstrap fix:

- The failure was in the public `G.URL.canParse` wrapper: its catch-all conversion
  to `false` swallowed the required zero-argument `ERR_MISSING_ARGS` TypeError.
  The vendored internal URL implementation was not the live public path for this
  module shape.
- `800d2a9` adds only the argument-count guard before the existing parse wrapper.
  A fresh coordinator build made `test-whatwg-url-canparse.js` exit 0; a direct
  smoke retained true results for absolute and base-relative URLs.
- Four bounded regressions also exit 0: `test-process-hrtime.js`,
  `test-process-hrtime-bigint.js`, `test-util-types.js`, and
  `test-runner-snapshot-file-tests.js`. No full corpus was rerun.

The assertion source-position probe was also closed as parked for this
checkpoint. `internalBinding('errors').getErrorSourcePositions()` is not the
live path for `t.assert.ok`: the public test assertion uses bootstrap-owned
`AErr`. A narrow internal-binding bridge was built and measured twice, but the
target stayed at **1/2**; the bridge was removed and no unverified behavior was
committed. A future attempt must own the bootstrap assertion/source extraction
boundary as one slice.

The measured `test-util-callbackify.js` candidate was evaluated and parked
without a source commit:

- The existing RED was reproduced. A temporary helper probe confirmed that
  Node's falsy-rejection stack contract needs a `process.processTicksAndRejections`
  frame, while the C++-owned tick queue exposes host `run`/`runTicks` frames.
- A narrow temporary stack-frame compatibility experiment moved past that
  assertion, but the same file then stopped at an independent uncaught-callback
  fixture count (**9** observed error lines versus **7** expected). That generic
  nextTick/uncaught stack boundary is broader than callbackify and was not mixed
  into this lane.
- The experiment was reverted; the working tree has no callbackify change and no
  green file is claimed from this evaluation.

The measured `test-util-format.js` candidate was also parked after root-cause
analysis:

- The RED is the null-prototype class label: mbun reports
  `[Object: null prototype] {}`, while Node reports `[Foo: null prototype] {}`.
- Node's vendored inspect path delegates this case to the V8 internal
  `getConstructorName`, which can recover the original instance constructor after
  `Object.setPrototypeOf(value, null)`. The current JS-only `inspectCtorName` has
  no equivalent information and correctly sees only the null prototype.
- Recovering that name would require global `Object.setPrototypeOf` tracking or an
  engine-level seam, so no speculative global wrapper was added and no green file
  is claimed from this evaluation.

The measured getter-display candidate is now a bounded green slice:

- `a93a26d` makes the public Node inspector collect up to three user-defined
  prototype accessor layers under `showHidden`, invoke getters only when the
  requested `getters` mode allows it, preserve the receiver as `this`, and
  render returned objects with Node's getter label. It also restores the root
  circular-reference marker and applies the default `breakLength` boundary to
  the Node object layout.
- A fresh coordinator build moved
  `test-util-inspect-getters-accessing-this.js` from the measured RED to exit
  0. The first getter assertion, receiver-sensitive value, root `<ref *1>`
  marker, and multiline layout all match the corpus contract.
- Four bounded regressions ran concurrently and all exited 0:
  `test-util-types.js`, `test-runner-snapshot-file-tests.js`,
  `test-process-hrtime.js`, and the getter target itself. No full corpus run
  was started; callbackify, util.format, assertion source-position, and test-v8
  remain separately parked.

The next Node VM probe produced one additional narrow green slice:

- A five-file bounded probe measured `test-vm-create-context-arg.js`,
  `test-vm-is-context.js`, and `test-vm-options-validation.js` at exit 0;
  `test-vm-basic.js` and `test-vm-context.js` were the only two RED files in
  that probe. The latter stopped at one display-error position assertion, not
  at context isolation or argument validation.
- `f3a7393` carries `vm.Script`'s validated `lineOffset`/`columnOffset` into
  Node's decorated error header and first stack frame. The source excerpt and
  caret remain relative to the original source, matching Node's display-errors
  contract.
- A fresh build moved `test-vm-context.js` from exit 1 to exit 0. Five bounded
  lanes then all exited 0: `test-vm-context.js`,
  `test-vm-create-context-arg.js`, `test-vm-is-context.js`,
  `test-vm-options-validation.js`, and the already-green getter target.
  `test-vm-basic.js` remains RED only on JSC's generic `Parser error` versus
  Node's token-specific `Unexpected token '}'` message; that parser-boundary
  mismatch remains isolated rather than receiving a test-specific rewrite.

The adjacent VM property/context probe then separated a larger green surface
from engine-boundary failures without another build:

- Five files exited 0: `test-vm-global-get-own.js`, `test-vm-ownkeys.js`,
  `test-vm-ownpropertynames.js`, `test-vm-ownpropertysymbols.js`, and
  `test-vm-getters.js`. A second five-file probe added green
  `test-vm-cross-context.js`, `test-vm-create-and-run-in-context.js`,
  `test-vm-run-in-new-context.js`, `test-vm-new-script-new-context.js`, and
  `test-vm-new-script-this-context.js`.
- The remaining measured failures were kept separate: global setter error
  wording, global-property enumeration/prototype ownership, and the
  self-referential accessor identity in `test-vm-property-not-on-sandbox.js`.
  These require JSC global-proxy/interceptor semantics or engine-specific error
  information; no broad context mirror rewrite was attempted.
- Across the VM probes in this checkpoint, 15 named files exited 0 and six
  distinct RED boundaries were recorded. This is probe data, not a replacement
  for the repository's full-corpus score; no full corpus run was started.

The first bounded Bun-native probe also hardened the measurement path:

- The initial `bun_corpus_runner.py` invocation exposed a harness-only failure:
  resolving a symlinked `compat/bun` checkout made its discovered files fall
  outside the lexical `--root`. `aafba81` keeps the discovery path lexical and
  adds a symlink regression to the runner self-test; the regression is RED
  before the change and the full self-test is GREEN after it.
- The repaired runner measured five real `compat/bun/test/js/node` files with
  four bounded jobs. The repeated result is **2 green files**, **3 test-failure
  files**, **198/206 passed tests**, and **8 failed tests**. The green files were
  `test/js/node/net/blocklist-gc.test.ts` and
  `test/js/node/tls/node-tls-upgrade.test.ts`; the crypto file reached
  **196/202** with six failures, readline had one timing/stream expectation
  failure, and trace-events had one proxy-network error expectation failure.
- No build was needed for the runner fix and no full Bun corpus run was
  started. The Bun result remains a measured five-file slice, not a new full
  corpus score.

A second Bun-native wave used four parallel one-job runners over adjacent
`test/js/node` subtrees:

- `events/event-emitter.test.ts` is green at **67/67 tests**.
- `console/console-table-iterators.test.ts` is a one-test snapshot formatting
  failure; `url/url-parse-format.test.js` is a one-test invalid-port diagnostic
  failure.
- `process/process-stdin.test.ts` reached **11/14 tests** with three stdin
  behavior failures; one adjacent stale-HUP stdin file timed out at the bounded
  limit. These are kept as process/stream ownership boundaries, not counted as
  green.
- `assert/deep-equal.test.ts` was classified **ahead-of-reference** at
  **229/251 tests** with 22 Bun `test.failing` cases passing in mbun; this is
  not a runtime green file and is retained as a separate classifier result.

A low-cost Bun micro-wave then measured four small subtrees in parallel:

- `os/os.test.js` is green at **52/52 tests** and
  `stream/node-stream-uint8array.test.ts` is green at **5/5 tests**.
- `async_hooks/AsyncLocalStorage.test.ts` reached **32/45 tests** with 11
  failures across async-context propagation, HTTP/HTTP2 cleanup, and plugin
  loading. `string_decoder/string-decoder.test.js` reached **93/95 tests**;
  its two failures are large-buffer allocation/range and output-shape edges.
- This wave used no build and produced only small temporary logs; no cleanup was
  necessary despite the disk gate. The broad async-context and large-buffer
  boundaries remain parked behind their own owners.

The next standard-module Bun wave stayed build-free and produced a high-yield
green sample:

- Four one-file subtrees all exited green: `path/dirname.test.js` (3/3),
  `zlib/deflate-streaming.test.ts` (1/1),
  `dns/dns-lookup-keepalive.test.ts` (1/1), and
  `diagnostics_channel/diagnostics_channel.test.ts` (6/6; 9 tests ran).
- Expanded four-file samples gave `path` **3/4 files green, 9/10 tests** and
  `zlib` **2/4 files green, 9/39 tests**. The path failure is the
  `toNamespacedPath` Windows trailing-slash shape; zlib failures are native
  handle `write`/`writeSync` and lifecycle API ownership gaps.
- No new build was started while swap headroom remained critically low. These
  are measured native slices, not full-corpus score changes.

One more no-build Bun standard-module wave kept the same fast lane:

- `timers.promises/timers.promises.test.ts` is green at **4/4 tests**,
  `perf_hooks/perf_hooks.test.ts` at **8/8**, and
  `promise/reject-tostring.test.ts` at **1/1**.
- `timers/node-timers.test.ts` reached **18/20 tests**; its two failures are
  UTF-16 timer-label formatting and immediate-exception/microtask ordering.
  No runtime-wide timer change was attempted from those two boundaries.

The following four-lane Bun native probe kept only two narrow green slices:

- `dgram/node-dgram.test.js` is green at **3/3 tests** and
  `module/module-children-concurrent-gc.test.ts` at **1/1**.
- `v8/v8-date-parser.test.js` reached **2/5 tests**; the failures are JSC date
  parser semantics, not a broad v8 API absence.
- `test_runner/node-test.test.ts` reached **1/18 tests**; failures span the
  already measured `t.assert` surface, test hooks/async scheduling, nested
  test errors, and mock APIs. It remains parked as a multi-owner test-runner
  boundary rather than a speculative bundle of fixes.

The next single-owner path lane was repaired and rechecked:

- `0ddb3f3` preserves Node's trailing separator for a bare Windows namespace
  root such as `\\\\?\\foo` in `path.win32.toNamespacedPath`. The narrow fix
  is limited to namespace-root shapes and leaves UNC, device, drive-root, and
  ordinary path handling on the existing branches.
- A fresh Linux build completed successfully. The target Bun file is now
  green at **4/4 tests**; three unrelated regression lanes stayed green:
  `events/event-emitter` **67/67**, `os/os` **52/52**, and
  `timers.promises` **4/4**. The four-file run therefore measured
  **127/127 tests**, **0 failed**, with **542 expects** across the selected
  files.
- The resource gate remained open for this single build: about **43 GiB
  available memory**, only **41 MiB swap headroom**, and about **24 GiB free
  disk**. The stale-cache dry-run found approximately **0 MiB** safely
  reclaimable, so no cleanup was performed and no full corpus run was started.
- Reusing the fresh binary, the previously expanded four-file path sample
  moved from **3/4 files and 9/10 tests** to **4/4 files and 10/10 tests**:
  `dirname` **3/3**, `is-absolute` **2/2**, `to-namespaced-path` **4/4**, and
  `win32-exists` **1/1**. This was a no-build confirmation of the adjacent
  path slice, not a full path-corpus claim.

The next measured console owner also closed with one narrow runtime change:

- `5f36ae9` aligns `Console#table` with the vendored Bun `ConsoleObject` model:
  cells are centered within their display-width column and an odd spare space
  is placed on the right. The prior implementation incorrectly used Node's
  left-aligned CLI-table rule.
- The target `console/console-table-iterators.test.ts` moved from **0/1** to
  **1/1**. Fresh-build regression lanes stayed green for
  `path/to-namespaced-path` **4/4**, `events/event-emitter` **67/67**, and
  `os/os` **52/52**; the four-file run measured **124/124 tests**, **0 failed**,
  and **537 expects**.

A four-file triage probe then separated the next candidates without another
build:

- `url/url-parse-format.test.js` reached **4/6 tests** with one failure and
  one TODO. Its invalid-port expectation conflicts with the already-green
  Node invalid-input contract, so the existing cross-corpus conflict remains
  parked rather than routing on test identity.
- `process/process-stdin.test.ts` reached **11/14 tests**; the three failures
  are distinct stdin stream behaviors (file helper output, paused data
  delivery, and explicit read pull semantics), not one formatting owner.
- `v8/v8-date-parser.test.js` reached **2/5 tests**; all three failures are
  JSC date-parser semantic differences. The four-file probe measured
  **17/26 tests passed**, **8 failed**, with the console target already closed
  in the preceding fresh-build wave.

The cross-corpus guard then caught and isolated two dialect conflicts:

- The initial Node guard exposed the expected opposite contracts: Node's table
  cells are left-aligned and Node's bare namespace root has no trailing slash.
  `cea1bc4` routes these two choices through the existing process-level
  `__mbunDialect`; it does not inspect test paths or weaken either corpus.
- After a fresh build, Bun's four-file guard remained **124/124 tests green**.
  Node `test-path-makelong.js` and `test-path-resolve.js` both passed, and a
  bounded Node custom smoke confirmed left-aligned table rows plus the
  no-trailing-slash namespace result.
- The broader Node `test-console-table.js` guard still stops at its separate
  Map-iterator Key/Values shape gap; `test-console.js` still stops at the
  separate `_times` private-field gap. Neither is counted as a new regression
  from the dialect patch.

A bounded Node test-runner probe was used to choose the next strategy:

- `test-runner-get-test-context.js` passed **1/1**.
- `test-runner-cli.js` failed at fixture discovery/cwd resolution;
  `test-runner-diagnostics-channel.js` failed on bindStore/event payload
  propagation; and `test-runner-error-reporter.js` failed on reporter failure
  counts. These are three different ownership boundaries, not one cheap
  formatting fix.
- The probe therefore measured **1/4 files green** and keeps the remaining
  test-runner work in the port/graft track. No speculative patch or full
  test-runner sweep was started.

A build-free Bun.Terminal coverage checkpoint then confirmed the newer PTY
surface on Linux:

- Four files were green under four bounded jobs: `terminal.test.ts` **94/94**,
  `terminal-spawn.test.ts` **16/17** with one declared skip,
  `terminal-platform-gaps.test.ts` **19/19**, and
  `spawn/spawn-path.test.ts` **1/1**.
- The aggregate was **130 passed, 1 declared skip, 0 failed, 336 expects**.
  This is a real runtime result for the selected files, not a ported-symbol
  claim; no build or full Bun corpus run was started.

### W41 stream-like spawn stdin：async pipe owner 实测推进

- Wave 46 的四文件基线为 `spawn-stdin-readable-stream` **7/30**、
  `spawn-streaming-stdout` **1/1**、`spawnSync` **6/16**，另有 broad
  `spawn.test` 在 30 秒边界超时；四文件合计 **32 passed、10 failed**。
- `90d895d` 将真实 `ReadableStream` 与 async iterable stdin 接入已有
  `spawnEx`/`__mbun_io_tick` 路径；reader/iterator 在 child exit 时分别走
  `cancel()`/`return()`，普通 EOF 仍通过 `stdin.end()` 收口。为绕开
  `process_web.cppm` 已接近 GCC constexpr 字符串上限，适配器保持在相邻
  `bun_spawn_stream.cppm` payload 分区。
- fresh Linux build 成功（约 **61 秒**）。同一四文件 bounded probe（4 jobs、
  30 秒/文件）后为：stream stdin **27/30 passed、1 failed、2 TODO**，其中
  两项 async-iterable child-exit 用例已通过；streaming-stdout 仍为 **1/1**、
  **211 expects**；四文件合计 **34 passed、8 failed、47 ran、288 expects**。
- stream 文件唯一剩余失败是其 upstream `ReadableStream object type count`
  的 50-child burst，在 `spawnEx: fork() failed` 处触发 native spawn-burst
  资源边界；单文件复测仍复现，因此不归因于四 lane 并发，也不继续扩大本
  owner 的改动。随后以单文件 `MemoryMax=34G、TasksMax=1024` bounded scope
  复跑完整 stream 文件，结果为 **28 pass、2 TODO、0 fail**，说明源适配本身
  已闭合，默认 runner 的 **4G/512** 安全 profile 才是该异常 burst 的测量边界。
  `spawnSync` 与 broad `spawn.test` 维持原有独立 owner。
- 构建后资源记录约 **44 GiB available memory**、swap 可用约 **163 MiB**、
  根分区可用约 **23 GiB**；临时构建产物 dry-run 未发现安全可回收量，未做
  清理，未启动全量 corpus。

### W48 Linux 无构建近绿筛选：Bun/Node owner 再分流

- Bun 四文件 bounded probe（4 jobs、30 秒/文件）合计 **35/45 tests passed**、
  **9 failed、355 expects**：`process-stdin` **11/14**（paused data、file
  helper、explicit read 三个 stdin 语义缺口），`node-timers` **18/20**（UTF-16
  timer label 与 exception/microtask ordering），`url-parse-format` **4/6**
  （invalid-port 方言冲突），`v8-date-parser` **2/5**（JSC date parser semantics）。
- Node 四文件 bounded probe 为 **1/4 files pass**：`test-vm-context.js` 已绿；
  `test-util-callbackify.js` 剩余 `processTicksAndRejections` stack shape，
  `test-util-format.js` 剩余 null-prototype object 的 constructor label，
  `test-vm-basic.js` 剩余 JSC `Parser error` 与 Node token-specific message
  差异。三条红测 ownership 不同，本轮不做跨 owner 混修或新构建。
- 策略调整：保留 stream stdin 的 source checkpoint；资源 profile 作为独立
  runner/tooling owner，W48 的 Node/Bun 红测分别停车到 stack/inspect/parser/
  dialect 专项；下一轮只从可由单一运行时边界闭合的近绿项选任务，继续维持
  3–5 个 bounded lanes，暂不跑全量 corpus。

### W49 fresh-binary 回归闸门

- 复用当前最新二进制并行复跑四条既有绿线：Bun
  `events/event-emitter`、`os/os`、`timers.promises`、
  `stream/node-stream-uint8array`。
- 结果为 **4/4 files green、128/128 tests、0 failed、565 expects**；这是
  stream-like stdin source checkpoint 的邻接回归证据，不替代 Bun/Node 全量分数。
- 资源保持在约 **44 GiB available memory / 164 MiB swap / 23 GiB free disk**，
  无新构建、无全量 corpus、无临时产物清理。

### W50 spawn-heavy resource profile tooling

- Issue [#37](https://github.com/Sunrisepeak/mbun/issues/37) 记录并脱敏了
  `spawn-stdin-readable-stream` 的 scope-induced `fork()` 边界；设计记录见
  `20260801-bounded-resource-profile-design.md`。
- `1449975` 为 `bun_corpus_runner.py` 增加显式 `--memory-max` /
  `--tasks-max`，默认仍为 **4G/512**；非默认 profile 强制 `--jobs 1`，summary
  写入实际 profile，未按文件名自动放宽，也未改 upstream corpus。
- runner self-test、journal self-test、Python syntax check 均通过。真实
  `spawn-stdin-readable-stream` 单 lane 使用 `34G/1024` 后为 **1 file green、
  28/30 pass、0 fail、2 TODO、61 expects**；这是此前默认 profile `27/30`
  加资源对照后的可复现闭合结果。
- 本节点无构建、无全量 corpus；高资源 scope 只运行 1 lane，未改变 3–5 lane
  常规并行策略，资源水位约 **44 GiB available / 164 MiB swap / 23 GiB free disk**。

### W51 process.stdin final read/end ordering

- Issue [#38](https://github.com/Sunrisepeak/mbun/issues/38) 记录了一个单一
  `process.stdin` owner：无 `readable` listener 的 `read(3)` 在返回最后的 `gh`
  之前同步触发 `end`，使 `end` listener 只能看到 `['abc', 'def']`，而调用方
  最终实际收到 `['abc', 'def', 'gh']`。
- `bootstrap.cppm` 只将最后一次 `emitEnd()` 放到当前 `read()` 返回后的
  `process.nextTick`，不改变 EOF、readable buffer 或 close 语义。root `mcpp build`
  成功，release 构建耗时约 **60 秒**。
- focused `process-stdin.test.ts` 从既有 **11/14** 提升到 **12/14**，**24 expects**；
  `read(n)` 目标通过，剩余两项明确为不同 owner：`Bun.file()` child stdin 的
  ref 形态，以及 stdout WebStream 的 disturbed/reject 语义。
- W48 四文件 triage 在新 binary 上为 **36 passed、8 failed、45 ran、355 expects**，
  相比 **35/45、9 failed** 净增一条通过；四条既有 green guards 保持
  **4/4 files、128/128 tests、0 failed、565 expects**，使用默认 **4G/512、4 jobs**。
- 资源保护：一次 workspace-wide 构建在 swap 降至约 **43 MiB**、磁盘约 **20 GiB**
  时停止，随后只完成 root 窄构建；无全量 corpus，未删除源码或必要 fresh binary，
  只保留可复核的 bounded 结果。

### W52 Bun.spawn file-backed stdin owner

- Issue [#39](https://github.com/Sunrisepeak/mbun/issues/39) 记录了第二个
  `process-stdin` owner：`Bun.file()` 被错误降级为匿名 pipe，child 的
  `typeof process.stdin.ref` 为 `function`，而 regular-file stdin 的 Node
  对照为 `undefined`；相邻 `stdin: "pipe"` 仍为 `function`。
- 根因修复分两点：regular-file `Bun.file()` 在 `bun_spawn_stream.cppm` 中先以
  fd stdio 传给 child，并在 spawn 边界后关闭 parent fd；bootstrap 只对非
  regular-file fd 0 安装 `ref/unref`。generic Blob、byte、ReadableStream、
  async iterable 和 keyword stdin 路径没有改动。
- 首次 root 构建因 `process_web` raw payload 超过 GCC constexpr 字符串长度上限
  失败；随后按现有 payload 分区规则移到 `bun_spawn_stream.cppm`，root release
  build 成功，耗时约 **60.26 秒**。这是构建边界处理，不是运行时失败。
- focused `process-stdin.test.ts` 从 **12/14** 提升到 **13/14、26 expects**；
  `file does the right thing`、pipe 对照和真实 file-byte smoke 均通过。唯一
  剩余失败是 stdout WebStream 被重复消费后没有 reject，归入独立 WebStream
  disturbed/reject owner。
- W52 四文件 post-fix bounded probe：**126 passed、8 failed、134 ran、3015
  expects**；修复前为 **125/134、9 failed、3013 expects**。四条既有 green guards
  保持 **4/4 files、128/128 tests、0 failed、565 expects**。
- 资源策略保持 root 窄构建、默认 **4G/512** runner 和 bounded lanes；未启动
  workspace-wide 或全量 corpus。构建与测试期间 swap 仍约 **43 MiB free**、磁盘
  约 **20 GiB free**，后续新 owner 需继续避免 broad build。

### W53 Bun.spawn stdout disturbed/reject owner

- Issue [#40](https://github.com/Sunrisepeak/mbun/issues/40) 记录了 W52 留下的
  最后一个 `process-stdin` 红测：`new Response(proc.stdout).text()` 消费 custom
  stdout adapter 后，第二次 `proc.stdout.text()` 错误地再次 resolve。最小 smoke
  同时确认 direct helper 二次消费、async iterator 后 helper、`pipeTo` 后 helper
  都缺少共享的 used-state；native `ReadableStream` 的 `consumerUsableError`
  已有对应语义。
- `04214b1` 在 `bun_spawn_stream.cppm` 的 payload 分区中只包装 live subprocess
  readable adapter，为 `text/bytes/arrayBuffer/blob/json`、async iterator 与
  `pipeTo` 共用一次 claim；generic Blob stdin 与已有 pipe/stream 路径不变，避免
  再次扩大接近 constexpr 上限的 `process_web` payload。
- focused `process-stdin.test.ts` 从 **13/14、26 expects** 提升到 **14/14、27
  expects**，成为本轮第一个 focused green；4-file adapter lane 使用默认
  **4G/512、4 jobs**，结果为 **28/46 passed、18 failed、410 expects**。其中
  `readablestream-helpers` 的 10 个 Bun.spawn conversion checks 全通过，18 个
  wrong-this failures 是该文件既有的 `ReadableStream.prototype.*` contract，
  未混入本 owner；`spawn-streaming-stdout` 与 `spawn-streaming-stdin` 均 green。
- 四条既有 guards 复测仍为 **4/4 files、128/128 tests、0 failed、565 expects**。
  root release build 成功，耗时约 **60.21 秒**；未启动 workspace-wide build，未跑
  全量 corpus。资源策略继续保持 3–5 个 bounded lanes；本轮只在一个 adapter lane
  中使用 4 jobs，并继续回避 swap/disk 低水位下的并发构建。

### W54 ReadableStream conversion-helper brand owner

- Issue [#41](https://github.com/Sunrisepeak/mbun/issues/41) 记录了 adapter lane
  暴露的独立 contract：`ReadableStream.prototype.text/json/bytes` 对非法 receiver
  返回 rejected Promise，`blob` 还在 brand check 前读取 receiver 属性；Node/Bun
  原生 contract 要求同步 `ERR_INVALID_THIS`。
- `a8a6c05` 在 `js_streams.cppm` 为五个 conversion helper 复用已有
  `isReadableStream()` brand predicate，非法 receiver 立即抛出
  `Value of "this" must be of type ReadableStream`，valid stream 的消费与
  locked/used Promise 错误路径不变。
- `readablestream-helpers.test.ts` 从 **12/30、18 failed** 提升到 **30/30、0
  failed、43 expects**。W54 六文件 bounded lane 使用默认 **4G/512、4 jobs**，合计
  **6/6 files green、172/172 tests、0 failed、635 expects**；额外最小 smoke 验证
  五个方法对非法 receiver 都同步给出 `ERR_INVALID_THIS`，valid `text` 与
  `arrayBuffer` 仍正常转换。
- root release build 成功，耗时约 **60.50 秒**；未启动 workspace-wide build，未跑
  全量 corpus。当前资源约 **44 GiB available memory、43 MiB swap free、20 GiB
  free disk**，继续暂停 broad build，仅保留 bounded lane。

### W61 Node buffer.transcode owner

- Fresh Linux bounded probe before implementation used five jobs over four Node
  candidates plus the existing Buffer.fill guard: **1/5 files pass**. The
  target `test-icu-transcode.js` failed immediately because `buffer.transcode`
  was not exposed; the three URL files failed independent formatting/error
  contracts and stayed outside this owner.
- Issue [#44](https://github.com/Sunrisepeak/mbun/issues/44) adds the Node-only
  `buffer.transcode` module export in
  `modules/jsc/src/builtins/node_buffer_extra.cppm`. It accepts Buffer and
  Uint8Array input, covers the corpus's utf8/latin1/ascii/utf16le/ucs2 aliases,
  maps unrepresentable latin1/ascii code points to `?`, and preserves Bun's
  existing undefined exports.
- The focused acceptance file passed **1/1**. The target plus ten Buffer/Node
  guards passed **11/11 files** with three jobs. The original five-file probe
  moved to **2/5 files pass**; the three URL failures remain independent.
  Bun-dialect smoke kept both transcode exports `undefined`. Root release build
  passed in **60.12 seconds**. No full corpus or workspace-wide build was run.

### W62 Node URL custom-inspect owner

- The W61 five-file candidate set had three URL/Buffer owners after transcode:
  URL custom inspect, URL custom parsing, and URL custom setters. The inspect
  failure was isolated to the existing Node URL custom-inspect hook: JSON
  double quotes, visible `toJSON`/`toString`, missing `showHidden` context, and
  a fixed `URL` header.
- Issue [#45](https://github.com/Sunrisepeak/mbun/issues/45) changes only the
  Node-dialect custom inspector in `node_util_extra.cppm`: Node quote/field
  order, hidden URLContext, dynamic subclass name, and depth-zero output. Bun
  output and generic URL parser/setter paths are unchanged.
- Focused `test-whatwg-url-custom-inspect.js`: **1/1 pass**. The complete
  five-file candidate set moved to **3/5 pass**: transcode, URL inspect, and
  Buffer.fill green; custom parsing and custom setters remain parked as
  independent owners. Root release build passed in **60.24 seconds**. No full
  corpus or workspace-wide build was run.

### W63 Node URL custom-parsing triage parked

- The remaining W61/W62 URL candidate was measured as a message mismatch:
  native URL errors exposed input/base details instead of Node's `Invalid URL`.
  A diagnostic-only Node wrapper normalized that message, but the focused file
  then revealed **9 invalid inputs accepted by the native parser**, so the
  candidate is a parser-algorithm cluster rather than a safe message-only owner.
- The experiment was reverted with **no source commit** and no green claim.
  URL custom setters remain a separate lone-surrogate Unicode owner. The
  transcode, URL inspect, and Buffer.fill green slices stay closed.
- The diagnostic root build passed in **60.76 seconds**; the bounded focused
  run remained red. No full corpus or workspace-wide build was performed, and
  no new issue was mixed into the parser cluster.

### W64 Node URL setter USVString owner

- Issue [#47](https://github.com/Sunrisepeak/mbun/issues/47) isolates the
  remaining URL setter contract: Node applies WebIDL `USVString` conversion
  to URL setters, while the native path rejected lone surrogates and accepted
  Symbol/object values with the wrong observable behavior.
- The Node-only wrapper in `modules/jsc/src/builtins/node_util_extra.cppm`
  now applies JavaScript `ToString`, rejects Symbols with the Node TypeError,
  and normalizes lone surrogates through `util.toUSVString` for `href`,
  `protocol`, `username`, `password`, `host`, `hostname`, `port`, `pathname`,
  `search`, and `hash`. Bun behavior and the parked parser path are unchanged.
- Focused `test-whatwg-url-custom-setters.js`: **1/1 pass**. The five-file
  bounded regression used **5 jobs** and measured **4/5 files pass**: setter,
  transcode, URL inspect, and Buffer.fill are green; URL custom parsing remains
  the W63 parked failure with the same native message mismatch.
- The final root release build passed in **60.86 seconds**. No full corpus or
  workspace-wide build was started; resource policy remains serialized builds
  plus 3–5 bounded test lanes.

### W65 Bun URL API leaf coverage

- A fresh five-file Bun-native probe used **5 bounded jobs** and reused the
  current Linux binary; no source change or build was needed. The selected
  leaves were `url-parse-query`, `url-format`, `url-format-whatwg`,
  `url-domain-ascii-unicode`, and `url-canParse-whatwg`.
- All **5/5 files** were green: **135 passed, 0 failed, 137 ran, 130
  expects**. Two framework-level skips are included in the ran/pass total as
  reported by the Bun runner; no test failure or timeout occurred.
- This is additive Bun coverage data, not a claim about the parked URL parser
  cluster or the complete URL subtree. No full corpus or workspace-wide build
  was started; the next route remains a fresh one-owner Node/Bun near-green
  measurement under the 3–5 lane resource policy.

### W66 Bun util and parse_args leaf coverage

- A fresh five-file Bun-native probe used **5 bounded jobs** and reused the
  current Linux binary; no source change or build was needed. The selected
  leaves covered MIME APIs, both `parse_args` files, AbortSignal resource
  handling, and the exact MIME fixture.
- Four files were green: **128 passed, 0 failed, 128 ran, 273 expects**.
  `mime-api`, `parse_args/default-args`, `parse_args/parse-args`, and
  `test-aborted` all passed. `util/exact/mime-test.js` exited successfully but
  registered **0 tests** and is classified `no-tests`, not green coverage.
- This checkpoint adds module-level coverage without changing the runtime or
  conflating no-test fixtures with passing tests. No full corpus or
  workspace-wide build was started; continue with a fresh one-owner row and
  3–5 bounded lanes.

### W67 Bun worker_threads triage parked

- A fresh five-file Bun-native probe used **5 bounded jobs** and reused the
  current Linux binary; no source change or build was needed. The sample was
  `worker-thread-id`, `worker-top-level-await`, `worker-shutdown-post-leak`,
  `worker-async-dispose`, and `worker-transfer-list`.
- The runner measured **2 green files, 8 passed, 3 failed, 12 ran, 13
  expects**; one additional file was `all-skipped`. `worker-async-dispose`
  and `worker-transfer-list` were fully green. The direct worker-thread-id
  entry is a fixture-style file and fails without its parent worker context,
  so it is not an independent runtime owner.
- `worker-top-level-await` reproduced serially at **4/6 tests pass, 2 fail**.
  Both failures are the same unsettled-TLA exit-code 13 contract. The existing
  engine evidence and prior rollback show that applying Node's status 13 in the
  shared path removes two Bun green files and exposes a separate worker-open
  liveness gap; this remains parked as a cross-corpus/liveness boundary, with
  no speculative source patch or mixed issue.
- No full corpus or workspace-wide build was started. Continue with a fresh
  one-owner row under the 3–5 lane policy.

### W68 Bun process leaf probe and fixture triage

- A fresh five-file Bun-native probe used **5 bounded jobs** and reused the
  current Linux binary; no source change or build was needed. The sample was
  `process-nexttick`, `process-args`, `process-exitCode-with-exit`,
  `process-on`, and `process-stdio-invalid-utf16`.
- The runner measured **3 green files, 30 passed, 6 failed, 36 ran, 131
  expects**. The green files were `process-args` (**1/1, 50 expects**),
  `process-on` (**3/3, 5 expects**), and invalid-UTF-16 stdio
  (**24/24, 66 expects**).
- `process-exitCode-with-exit.js` is a fixture-style entry: direct `bun test`
  invocation leaves a non-numeric final argv and produces NaN, while a direct
  smoke with an explicit numeric argument prints `PASS`; it is not an
  exitCode setter owner. `process-nexttick` reached **2/7 tests pass** with
  callback validation, queue ordering, and repeated scheduling failures, so
  it remains a multi-owner process boundary.
- No full corpus or workspace-wide build was started. Keep the three green
  process leaves as coverage guards and continue from a fresh one-owner row.

### W69 Bun module loader probe parked

- A fresh five-file Bun-native probe used **5 bounded jobs** and reused the
  current Linux binary; no source change or build was needed. The sample was
  module resolve paths, module metadata, custom require extensions, and two
  sourcemap files.
- The runner measured **2 green files, 46 passed, 17 failed, 63 ran, 158
  expects**. `module-resolve-filename-paths` was **6/6** and
  `module-sourcemap` was **3/3** green.
- `node-module-module` reached **21/30** with failures across builtin list
  size, overridden resolve/require hooks, builtin cache/export shape, and
  Module.runMain/children. `require-extensions` reached **3/10** with custom
  loader and extension mutation failures. These are separate loader owners.
- `sourcemap` reached **13/14**; the sole failure is the missing
  `Could not decode sourcemap` warning in an entry runtime error stack. The
  `node:module SourceMap` class itself passed its API/VLQ cases, while the
  runtime warning/stack integration has no narrow existing owner. Park this
  with the loader cluster rather than changing the class or global error path.
- No full corpus or workspace-wide build was started. Keep the two green
  module guards and choose the next fresh one-owner row.

### W70 Bun filesystem leaf coverage

- A fresh five-file Bun-native probe used **5 bounded jobs** and reused the
  current Linux binary; no source change or build was needed. The sample was
  `fs/dir`, `fs-mkdir`, `fs-promises-writeFile-async-iterator`,
  `fs-stats-constructor`, and `fs-stats-truncate`.
- All **5/5 files** were green: **52 passed, 0 failed, 55 ran, 138 expects**.
  This adds filesystem leaves across directory operations, mkdir validation,
  async-iterator writes, and Stats construction/truncation without touching
  the broader fs error or platform-specific clusters.
- No full corpus or workspace-wide build was started. Keep these five files as
  bounded filesystem guards and continue with a fresh one-owner row under the
  3–5 lane policy.

### W71 Bun HTTP leaf probe parked

- A fresh five-file Bun-native probe used **5 bounded jobs** and reused the
  current Linux binary; no source change or build was needed. The sample was
  numeric headers, response timeout/unref, early hints, HTTPParser, and
  transfer-encoding/trailer handling.
- The runner measured **3 green files, 22 passed, 13 failed, 35 ran, 68
  expects**. `numeric-header`, `node-http-res-settimeout-unref`, and
  `early-hints-crlf-injection` were green.
- `node-http-parser` and `node-http-transfer-encoding` remain parked: their
  failures span parser state transitions, buffer ownership, header/trailer
  validation, connection ordering, and timeout/liveness behavior. No single
  safe HTTP owner was inferred and no full HTTP subtree was scanned.
- No full corpus or workspace-wide build was started. Keep the three green
  HTTP guards and select the next fresh one-owner row.

### W72 Bun child-process leaf probe and IPC owner

- A fresh five-file Bun-native probe used **5 bounded jobs** and reused the
  current Linux binary; no source change or build was needed. The sample was
  `child_process_send_cb`, child-process stdio, child-process exec,
  `child-process-rlimit-nofile`, and `child_process_ipc`.
- The runner measured **4 green files, 20 passed, 1 failed, 21 ran, 44
  expects**. `child_process_send_cb` (**1/1**), `child-process-stdio`
  (**7/7**), `child-process-exec` (**11/11**), and
  `child-process-rlimit-nofile` (**1/1**) were green.
- `child_process_ipc` is a narrow compatibility failure: its fixture passes
  an unlistened `net.Server` (`_fd === -1`) to `child.send()`. Node returns
  `true`, invokes the callback with `null`, and does not report a handle error;
  mbun emits an extra `uncaughtException ERR_INVALID_HANDLE_TYPE` before the
  expected five output lines. A focused Node probe confirmed the reference
  behavior. Issue [#48](https://github.com/Sunrisepeak/mbun/issues/48) tracks
  the smallest branch: treat this no-descriptor server as a plain message,
  while retaining rejection for genuinely unsupported handles.
- Issue #48 was fixed in `d390ad7`: `child_process.send()` now recognizes an
  actual unlistened `net.Server` and sends the message without a
  `NODE_HANDLE` frame, while fake or unsupported handles still return
  `ERR_INVALID_HANDLE_TYPE`. The root release build completed in **60.59s**.
- The focused guard moved from **1/1 failed** to **1/1 green**. The complete
  W72 five-file regression with **5 bounded jobs** is now **5/5 files green,
  21 passed, 0 failed, 21 ran, 44 expects**. The upstream fixture remains
  read-only; no full corpus or workspace-wide build was started.

### W73 Node crypto leaf coverage and checkPrime snapshot owner

- A fresh five-file Node crypto probe used **5 bounded jobs** and reused the
  current Linux binary before the fix. It measured **4 green files, 33
  passed, 2 failed, 35 ran, 85 expects**; HMAC algorithm validation,
  invalid-this handling, lazy hash, and HKDF callback-null behavior were
  already green.
- Both failures were in `crypto-random.test.ts`: sync and async `checkPrime`
  must snapshot candidate bytes before evaluating the `options.checks` getter,
  and must read that getter exactly once. Issue [#49](https://github.com/Sunrisepeak/mbun/issues/49)
  owns this single `crypto_asym.cppm` argument-order owner.
- `b2b5bae` copies normalized candidate bytes before option evaluation and
  stores `options.checks` in one local read. Root release build completed in
  **60.40s**. The same five-file bounded regression is now **5/5 files green,
  35 passed, 0 failed, 35 ran, 87 expects**.
- No upstream fixture changes, no full corpus, and no workspace-wide build
  were performed. Keep the five green crypto leaves as guards and choose the
  next fresh one-owner row.

### W74 Node events/stream/assert/console leaf probe parked

- A fresh five-file Node leaf probe used **5 bounded jobs** and reused the
  current Linux binary; no source change or build was needed. It measured
  **4 green files, 136 passed, 2 failed, 138 ran, 279 expects**.
- `event-emitter` (**67/67**), `node-stream-uint8array` (**5/5**),
  `assert-typedarray-deepequal` (**45/45**), and
  `console-table-iterators` (**1/1**) were green and are retained as guards.
- `node-timers` reached **18/20**. Its two failures are separate owners: a
  JSC UTF-16 string representation assertion in `clearTimeout`, and
  immediate-exception/microtask ordering in a spawned fixture. No mixed fix
  was attempted and no issue was opened for the multi-owner file.
- No upstream fixture changes, no full corpus, and no workspace-wide build
  were performed. Continue with a fresh one-owner row under the 3–5 lane
  policy.

### W75 Bun util leaf coverage

- A fresh five-file Bun-native util probe used **5 bounded jobs** and reused
  the current Linux binary; no source change or build was needed. All **5/5
  files** were green: **13 passed, 0 failed, 13 ran, 24 expects**.
- The guards cover Bun file existence, `Bun.concat`, error-code mirroring,
  error-name preservation, and file-type detection. Keep them as cheap Bun
  leaves while selecting the next fresh candidate.
- No upstream fixture changes, no full corpus, and no workspace-wide build
  were performed.

### W76 Node assert leaf probe and ahead-of-reference matrix

- A fresh five-file Node assert probe used **5 bounded jobs** and reused the
  current Linux binary; no source change or build was needed. Four files were
  green and the fifth was `ahead-of-reference`: **275 passed, 22 failed, 297
  ran, 451 expects**.
- `assert-doesNotMatch` (**3/3**), `assert-match` (**3/3**), `assert-promise`
  (**12/12**), and `assert.spec` (**28/28**) were green. `deep-equal` reached
  **229/251**; all 22 failures are upstream `test.failing` cases where mbun
  passes a case Bun currently expects to fail, spanning prototypes, own
  properties, RegExp state, and collection/typed-array semantics. The runner
  therefore classified it `ahead-of-reference`, not green.
- No upstream fixture changes, no full corpus, and no workspace-wide build
  were performed. Keep the four assertion guards and leave the multi-semantic
  deep-equality matrix split for a later measured owner.

### W77 Node crypto leaf probe parked on external-memory owner

- A fresh five-file Node crypto probe used **5 bounded jobs** and reused the
  current Linux binary; no source change or build was needed. It measured
  **4 green files, 51 passed, 8 failed, 59 ran, 344 expects**.
- `crypto-sign-regression` (**1/1**), `x509` (**14/14**), `scrypt` (**1/1**),
  and `crypto-oneshot` (**35/35**) were green. `crypto-extra-memory` had
  **0/8** green tests: all failures are `heapStats().extraMemorySize` deltas
  for SecretKey, asymmetric keys, Hash/Hmac/Cipher, ECDH, Sign, and Verify.
- The failures share the JSC GC/native-wrapper external-memory accounting
  boundary, not one crypto algorithm. No mixed crypto patch was attempted;
  keep the four green leaves and park this architectural owner.
- No upstream fixture changes, no full corpus, and no workspace-wide build
  were performed.

### W78 Bun file/util leaf probe parked on Bun object internals

- A fresh five-file Bun-native util/file probe used **5 bounded jobs** and
  reused the current Linux binary; no source change or build was needed. It
  measured **4 green files, 27 passed, 1 failed, 28 ran, 227 expects**.
- `bun-file-fd-read` (**3/3**), `bun-file-read` (**1/1**),
  `bun-isMainThread` (**1/1**), and `fileUrl` (**20/20**) were green.
- `BunObject` reached **2/3**: its only failure is the missing
  `bun:internal-for-testing.hasNonReifiedStatic` helper used to check Bun's
  lazy static-property reification before/after spreading the Bun object.
  This is a Bun object/bootstrap-internals owner, not a file API regression;
  it is parked without a test-specific shim.
- The probe emitted a local environment dump while printing the Bun object;
  that temporary output was deleted and no environment values, paths, or
  identifiers were copied into project docs, commits, or PR text.
- No upstream fixture changes, no full corpus, and no workspace-wide build
  were performed.

### W79 Bun crypto probe parked on WebCrypto validation matrix

- A fresh three-file Bun-native crypto probe used **3 bounded jobs** and
  reused the current Linux binary; no source change or build was needed. It
  measured **2 green files, 9,515 passed, 736 failed, 10,251 ran, 29,406
  expects**.
- `cipheriv-decipheriv` (**13/13**) and `x25519-derive-bits` (**12/12**) were
  green. The WPT `generateKey` file reached **9,490/10,226**; its failures
  repeat across empty-algorithm and RSA algorithm-property validation cases
  with exception-type/validation mismatches. This is a broad WebCrypto
  validation matrix, not a single safe crypto primitive owner, so it is
  parked without a speculative patch.
- No upstream fixture changes, no full corpus, and no workspace-wide build
  were performed.

### W80 Node zlib leaf probe parked on native-handle boundary

- A fresh five-file Node zlib probe used **5 bounded jobs** and reused the
  current Linux binary; no source change or build was needed. It measured
  **3 green files, 17 passed, 30 failed, 47 ran, 64 expects**.
- `bytesWritten` (**5/5**), `deflate-streaming` (**1/1**), and
  `zlib.kMaxLength.global` (**8/8**) were green. The two remaining files
  failed across native handle bounds/writeState, missing handle methods,
  init-after-close validation, and onerror re-entrancy. These are a shared
  zlib binding lifecycle/handle surface rather than one safe leaf owner, so
  no mixed fix was attempted.
- No upstream fixture changes, no full corpus, and no workspace-wide build
  were performed.

### W81 Node console/zlib leaf probe parked on mixed zlib failures

- A fresh five-file Node console/zlib probe used **5 bounded jobs** and reused
  the current Linux binary; no source change or build was needed. It measured
  **4 green files, 390 passed, 12 failed, 404 ran, 480 expects**.
- `console-constructor-exception` (**1/1**), `console` (**7/7**), zlib
  `leak` (**8/8**), and `zlib-reset-race` (**3/3**) were green. `zlib.test`
  reached **371/385**; its 12 failures span invalid raw data, libdeflate
  level validation, chunk/output bounds, and async buffer lifetime. No single
  safe owner was inferred, so no mixed zlib patch was attempted.
- No upstream fixture changes, no full corpus, and no workspace-wide build
  were performed.

### W82 Node path leaf coverage

- A fresh five-file Node path probe used **5 bounded jobs** and reused the
  current Linux binary; no source change or build was needed. All **5/5 files**
  were green: **13 passed, 0 failed, 13 ran, 0 `expect()` calls**. These
  assert-style files cover basename, dirname, extname, isAbsolute, and join.
- The zero `expect()` count is a runner metric, not missing assertions: the
  upstream files use Node's assert APIs. No upstream fixture changes, no full
  corpus, and no workspace-wide build were performed.

### W83 Bun path format dialect fix

- The fresh five-file Bun path probe used **5 bounded jobs**. Before the source
  change it measured **4/5 files green, 15 passed, 1 failed, 16 ran, 9
  expects**. The sole failure was `path.format(null)`: the Bun corpus expects
  `The "pathObject" property must be of type object, got object`, while the
  shared validator emitted Node's `The "pathObject" argument must be of type
  object. Received null` form.
- Issue [#50](https://github.com/Sunrisepeak/mbun/issues/50) isolated the
  owner. Commit `7f6af93` makes only this invalid-object message dispatch on
  `globalThis.__mbunDialect`; Node keeps the existing `nodeArgTypeError()`
  path, and Bun restores its compatibility wording. The release build passed
  in **60.34 seconds**.
- After the change, W83 measured **5/5 files green, 16 passed, 0 failed, 16
  ran, 9 expects**. The combined W82/W83 ten-file guard measured **10/10
  green, 29 passed, 0 failed, 29 ran**; Node's
  `test-path-parse-format.js` was independently **1/1 pass**. No upstream
  fixture changes and no full corpus were performed.
- Resource checkpoint around the build: approximately **43 GiB available
  memory, 48 MiB free swap, and 20 GiB free disk**. The run stayed within the
  serialized build lock and five-job bounded runner; no workspace-wide build
  was started.

### W84 Bun Node-path continuation

- A fresh five-file Bun path probe used **5 bounded jobs**, reused the existing
  Linux binary, and did not build. It measured **5/5 files green, 88 passed,
  0 failed, 89 ran, 382 expects**.
- The high-value `browserify` compatibility file was **52/52** and
  `matches-glob` was **31/31**. The same probe also covered path base
  properties, the long-path join guard, and zero-length strings; one skipped
  case in the path-property file is reflected by **88 passed / 89 ran**, not
  silently counted as a pass.
- This wave found no single runtime owner and required no source change or
  issue. No upstream fixture changes and no full corpus were performed; the
  next route remains a fresh near-green Node/Bun leaf with 3–5 bounded workers.

### W85 Bun Node API probe and Buffer ceiling fix

- The fresh five-file probe used **5 bounded jobs**. Before the fix it measured
  **3 green files, 1 all-skipped file, 147 passed, 2 failed, 150 ran, 3035
  expects**: `node:os` was **52/52**, `path.posix` and `path.win32` existence
  guards were **1/1** each, and the platform-specific relative-path file was
  all-skipped. `string_decoder` reached **93 passed / 2 failed / 95 ran**.
- Both failures were the same owner, not decoder logic: the child smoke tried
  `Buffer.allocUnsafe(2**31)` and `Buffer.allocUnsafe(2**31 + 16)`, but the Bun
  dialect still applied `0x7fffffff`, so the child exited before
  `StringDecoder.write()` or `.text()` could exercise their large-buffer
  guards. Direct reproduction returned `ERR_OUT_OF_RANGE` at the allocator
  boundary.
- Issue [#51](https://github.com/Sunrisepeak/mbun/issues/51) led to commit
  `03b22da`. The completion partition now selects Bun's 64-bit
  `K_MAX_LENGTH = 0x100000000` and `MAX_STRING_LENGTH = 0x7fffffff`, updates
  `Buffer.alloc*`, `Buffer.concat`, and the public module constants, and keeps
  the Node dialect's previous values unchanged. The retry build passed in
  **60.13 seconds**. A first attempt that edited the oversized `process_web`
  raw payload hit GCC16's constexpr string-length limit; that change was
  removed, keeping the final patch in the existing buffer completion owner.
- After the fix W85 measured **4 green files, 1 all-skipped file, 149 passed,
  0 failed, 150 ran, 3038 expects**. `string_decoder` is now **95/95** and
  `node:os` remains **52/52**. A separate Node direct guard still reports
  `MAX_LENGTH=2147483647`, `MAX_STRING_LENGTH=536870888`, and rejects
  `allocUnsafe(2**31)`, so the dialect boundary was verified without a large
  Node allocation. No upstream fixture changes and no full corpus were
  performed.
- Resource checkpoint after the build and bounded child runs: approximately
  **42 GiB available memory, 54 MiB free swap, and 20 GiB free disk**. No
  workspace-wide build or parallel build storm was started.

### W86 Buffer completion regression guard

- A focused five-file Bun guard reused the W85 binary with **5 bounded jobs**
  and no build. It measured **4 green files, 25 passed, 6 failed, 31 ran, 46
  expects**. `buffer-compare-bounds` (**13/13**),
  `buffer-from-encoding-leak` (**2/2**), `buffer-inspectmaxbytes` (**1/1**),
  and `buffer-utf16` (**1/1**) stayed green.
- `buffer-concat` reached **8/14**. Its six failures are not one #51 ceiling
  contract: they span the large-concat OOM error shape, resizable-buffer
  post-getter sizing, and detached TypedArray/ArrayBuffer propagation in
  `Bun.concatArrayBuffers`. The row is parked as a native concat/lifetime
  cluster; no source patch or new issue was opened.
- This is a guard result, not a new full Buffer-suite score. No upstream
  fixture changes, no full corpus, and no workspace-wide build were performed.

### W87 Bun Node-fs leaf confirmation

- A fresh five-file Bun filesystem probe reused the W85 binary with **5 bounded
  jobs** and no build. It measured **5/5 files green, 46 passed, 0 failed, 70
  ran, 92 expects**.
- `fs.glob` was **27/27**, `fs-path-length` **11/11**, Linux birthtime **5/5**,
  cp symlink target **2/2**, and recursive readdir error-leak **1/1**. The
  path-length file ran 35 cases and the readdir leak guard completed in the
  bounded runner without a timeout.
- This wave intentionally refreshed older inventory claims rather than treating
  them as current facts. All five remain green, no single source owner surfaced,
  and no issue or patch was needed. No upstream fixture changes, no full corpus,
  and no workspace-wide build were performed.

### W88 Bun Node-fs directory/Stats leaf confirmation

- A fresh five-file Bun filesystem probe reused the W85 binary with **5 bounded
  jobs** and no build. The dependency gate required the already-authorized
  missing-dependency measurement mode; this was a runner precondition, not a
  test failure. The probe measured **5/5 files green, 52 passed, 0 failed, 55
  ran, 138 expects**.
- `dir` was **23/23**, `fs-mkdir` **21/24**, async-iterator `writeFile` **2/2**,
  Stats constructor **3/3**, and Stats truncate **3/3**. This refreshes the
  older W70 filesystem result with current bounded evidence; no single source
  owner surfaced, no issue or patch was needed, and no upstream fixture was
  changed.
- No full corpus or workspace-wide build was performed. The next route remains
  a fresh one-owner Bun/Node leaf, with 3–5 bounded lanes while swap and disk
  headroom remain low.

### W89 Bun Node-inspector probe parked

- A fresh five-file Bun standard-module probe reused the W85 binary with **5
  bounded jobs** and no build. It measured **4/5 files green, 34 passed, 27
  failed, 64 ran, 131 expects**.
- `inspector.test` was **5/5**, diagnostics channel **6/9**, perf hooks **8/8**,
  and timers promises **4/4**. `inspector-profiler` reached **11 passed / 27
  failed / 38 ran**; the failures cover Session connected-state checks,
  profiler enable/start/stop return and state contracts, and unsupported-method
  errors.
- A narrow source check confirmed the current runtime advertises
  `process.features.inspector` as false and has no inspector/profiler
  implementation owner in the JSC builtins. This is a missing subsystem
  boundary, not a safe error-text or one-method patch; it is parked without a
  speculative issue or source change. No upstream fixture changes, full corpus,
  or workspace-wide build were performed.

### W90 Bun Node-fs/child_process leaf probe

- The first dispatch had one selector typo (`fs/fs-promises.test.js`); the
  runner classified it as a harness load error with no tests. It was not counted
  as runtime evidence. The corrected selector used the real
  `fs/promises.test.js` path and reran the same five-file wave with **5 bounded
  jobs** and no build.
- The authoritative corrected result was **4/5 files green, 39 passed, 12
  failed, 56 ran, 107 expects**. `child-process-exec` was **11/11**,
  `child-process-rlimit-nofile` **1/1**, `child-process-stdio` **7/7**, and
  Linux `fs-stat-seccomp` **3/3**.
- `fs/promises` reached **17 passed / 12 failed / 34 ran**. Its failures split
  across async stack frames, the internal stream loader, FileHandle close/
  in-flight lifetime, and AbortError message/shape. The current source layout
  does not expose one safe live owner for this mixed row, so no issue or patch
  was opened. No upstream fixture changes, full corpus, or workspace-wide
  build were performed; temporary selector and output data were removed.

### W91 Bun Node-net leaf probe

- A fresh five-file Bun net probe reused the W85 binary with **5 bounded jobs**
  and no build. It measured **3 green files, 5 passed, 1 failed, 7 ran, 15
  expects**; one additional file was classified `no-tests`.
- `double-connect` was **1/1**, `node-net-allowHalfOpen` **2/2**, and
  `socket-reconnect-live` **2/2**. The `handle-leak` fixture is a stress
  entrypoint rather than a Bun test file; it completed its 100,000-connection
  leak exercise but is not counted as green coverage.
- `connect-autoselectfamily-stale-timer` skipped its macOS-only case and timed
  out the Linux destroy-while-pending case after **5 seconds**. The fixture
  never reached its `OK` marker; the source path has no bounded Happy-Eyeballs
  attempt-timer lifecycle to patch narrowly, so this remains a net liveness
  boundary with no speculative issue or source change. No upstream fixture
  changes, full corpus, or workspace-wide build were performed.

### W92 fs/promises AbortError message fix

- W90's red `fs/promises` row was reduced to one owner: the live
  `fsAbortErr()` helper emitted `The operation was aborted` without Node's
  terminal period. Issue [#52](https://github.com/Sunrisepeak/mbun/issues/52)
  captured the red contract; commit `84f7d59` changes only that message and
  preserves `name`, `code`, and `cause`.
- A fresh release build completed under the serialized build lock. The focused
  Bun rerun used **5 bounded jobs**: `fs/promises` moved from **17 passed / 12
  failed / 34 ran** to **24 passed / 5 failed / 34 ran**, and the four W90
  child_process/fs guards remained green. Aggregate focused result: **4/5
  files green, 46 passed, 5 failed, 56 ran, 114 expects**.
- The seven AbortError message/shape cases all turned green. The five remaining
  failures are independent async-stack, internal-stream-loader,
  FileHandle-lifetime, and async-iterator abort-assertion owners; no mixed
  follow-up was attempted. Direct smoke reports the corrected message in both
  Node and Bun dialects with `AbortError` and `ABORT_ERR`.
- Node's bounded guard passed the two FileHandle abort files; the third
  readFile file remains red only at its unrelated zero-byte-liar child fixture
  assertion. No upstream fixture changes, full corpus, or workspace-wide build
  were performed.

### W93 Node fs/promises FileHandle leaf confirmation

- A fresh Node corpus probe reused the W92 release binary with **5 bounded
  jobs** and no build. All **5/5 files passed**: FileHandle `chmod`, `stat`,
  `truncate`, `write`, and `sync`.
- This is a post-fix Node-side guard for the adjacent fs/promises surface. It
  found no new source owner and required no fixture change or issue. No full
  corpus or workspace-wide build was performed; temporary selector and output
  data were removed.

### W94 Bun HTTP/2/Worker staged probe before fix

- A fresh five-file Bun probe used **5 bounded jobs** and the existing binary;
  it measured **4/5 files green, 7 passed, 1 failed, 8 ran, 6 expects**.
- HTTP/2 late-RST (**2/2**) and streams-rehash (**3/3**) were green, as were
  Worker SharedArrayBuffer (**1/1**) and transfer-terminate (**1/1**). The
  single red file was the reserved-push DATA refusal case, which timed out
  before observing `RST_STREAM(STREAM_CLOSED)`.
- Narrow source inspection showed the client parser created the pushed stream
  but forwarded DATA before response HEADERS. Issue [#53](https://github.com/Sunrisepeak/mbun/issues/53)
  captured that one state-machine owner; no mixed HTTP/2 patch was attempted.

### W95 Bun HTTP/2/Worker staged regression after #53

- Commit `fa7b12b` adds only the reserved-push guard in the client DATA path:
  before response HEADERS, it sends `STREAM_CLOSED`, closes the stream, and
  keeps the session available for later frames.
- The serialized fresh build completed. The same five-file probe with **5
  bounded jobs** measured **5/5 files green, 8 passed, 0 failed, 8 ran, 8
  expects**. The former push-refusal timeout is now **1/1**, while late-RST,
  streams-rehash, SharedArrayBuffer, and transfer-terminate remain green.
- No upstream fixture changes, full corpus, or workspace-wide build were
  performed; temporary selectors and outputs were removed.

### W59 Node buffer leaf sample

- W59 使用 Node corpus runner 的默认 bounded profile、**3 jobs**。首批五个文件为
  **4/5 pass**，第二批五个文件为 **5/5 pass**，第三批五个文件在修复前为
  **4/5 pass**；因此完整样本修复前为 **13/15 pass、2 failed**。
- Issue [#43](https://github.com/Sunrisepeak/mbun/issues/43) 负责同一
  `Buffer.prototype.fill` 入口的三个 Node contract：hex 奇数/非法字符的
  `ERR_INVALID_ARG_VALUE`、非字符串 encoding 的 `ERR_INVALID_ARG_TYPE`，以及
  伪造 `length` 时的 `ERR_BUFFER_OUT_OF_BOUNDS`。修复仅位于
  `modules/jsc/src/builtins/node_buffer_extra.cppm`，上游测试保持只读。
- root release build 成功，耗时 **60.70 秒**；focused fill + 9 guards 为
  **10/10 files pass**，完整 W59 复测为 **14/15 files pass**。唯一失败仍是
  `test-buffer-constants.js` 的通用 JSC String capacity 边界，不归入 Buffer.fill
  owner。未跑全量 corpus，未启动 workspace-wide build。

### W58 JSON5/YAML parser sample

- W58 复用已有 fresh binary，默认 **4G/512、3 jobs**，无构建、无全量 corpus。三个
  parser 文件全绿：JSON5 扩展 **321/321**、JSON5 官方 suite **113/113**、YAML
  block-scalar matrix **1084/1084**。`import-attributes` 为 **3/12**，9 个失败
  跨无扩展 JS/TS loader、JSON/JSONC/TOML/YAML loader、tsconfig JSONC 识别以及
  wasm/不存在模块处理，不能归并为一个安全 owner。
- W58 合计 **4 files、1521/1530 tests passed、9 failed、1809 expects、3/4 files
  green**。JSON5/YAML 结果作为新的高收益绿色覆盖记录；import-attributes 停车，
  下一任务仍需 fresh bounded measurement 证明单 owner 后再建 issue。

### W57 fresh Bun built-in probes

- W57 使用默认 **4G/512、3 jobs**，无构建、无全量 corpus。第一批五文件为
  `ini/ini`、`jsonl/jsonl-parse`、`jsonc/jsonc`、Markdown heading IDs 和
  `md/gfm-compat`：前四个分别为 **62/62、269/269、43/43、17/17**，GFM 为
  **47/62**，15 个失败。失败证据分成 table interruption/column-count/escaping、
  autolink 特殊字符、单波浪线删除线和 entity-like suffix，至少四个 owner，未建
  混合修复 issue。
- 第二批 cookie 四文件与 `cron/cron-parse` 全绿：**5/5 files、138/138 tests、0
  failed、667 expects**。两批合计 **10 files、576/591 tests passed、15 failed、5479
  expects**，其中 **9/10 files green**；失败全部来自 GFM，cookie/cron/ini/JSON/heading
  结果可作为下一轮 guard 或已交付覆盖数据。
- 第三批 util probe 使用同一 profile 测得 base64url **5/5**、escapeHTML **10/10**、
  escapeRegExp **2/2**、which **5/5**；`stripANSI` 为 **284/296**，12 failed。失败
  分成 malformed CSI/OSC/C1、Unicode/C1 边界和 large-input 性能断言多个 owner。
- 三批独立选集合计 **15 files、882/909 tests passed、27 failed、7451 expects**，其中
  **13/15 files green**。结论：GFM 与 stripANSI 都停车；下一任务继续从 fresh
  bounded measurement 选单一 Bun/Node near-green row，不因绿色叶子文件而
  speculative 改动 parser 或 ANSI scanner。

### W56 Bun.Glob path-boundary owner

- W56 先纠正了一次候选路径选择错误：错误的 `compat/bun/...` 前缀只触发了
  harness 的 no-test/load-error 分类，未计入覆盖数据。修正为真实 vendored 路径后，
  `cli/install/semver`、`glob/match`、`glob/proto`、`util/toUTF16Alloc` 四个文件先得
  到 **4/4 files、58/58 tests、0 failed、3261 expects**；随后加入
  `glob/path-length`，初始为 **1/6、5 failed**，失败集中在同一条路径边界 owner。
- Issue [#42](https://github.com/Sunrisepeak/mbun/issues/42) 和设计记录
  `.agents/docs/20260801-bun-glob-path-boundary-design.md` 先固定了契约：复用
  `mbun.platform.path` 的 host policy，在 pattern、目录下降以及
  `directory_entry` status/iterator 返回 `ENAMETOOLONG` 时传递错误；matched file 的
  逻辑路径仍按原生测试要求可返回，并保留 matcher、symlink-cycle 和 only-files 语义。
- `d8d8082` 在 `glob.cppm` 增加可选 scan error-code 输出和目录边界传播，在 JSC
  callback 使用已有 Node-shaped fs error helper，并补齐 only-files fast path 的
  `absolute` 结果。glob 单元目标为 **1497 checks、0 failures**；root release build
  约 **59.3 秒**。
- 最终 bounded lane 使用默认 **4G/512、3 jobs**，9 个真实文件为 **9/9 files
  green、193/193 tests、0 failed、3856 expects**：`glob/path-length` 已从 **1/6**
  提升到 **6/6**，四条既有 guards 仍为 **4/4 files、128/128 tests、0 failed、565
  expects**。未跑全量 corpus，未做 workspace-wide build。

### W55 fresh inventory refresh and Bun CSS triage

- W55 首先复测了 inventory 指向的 Node fs owner：FileHandle
  `pull/pullSync/writer/aggregate-errors/close-errors/op-errors` **6/6 pass**；随后
  flush、AbortSignal 与 WHATWG URL 组合的 `append-file-flush`、`write-file-flush`、
  `readfile`、`write-file`、`whatwg-url` **5/5 pass**。两组共 **11/11 pass**，说明旧
  inventory 条目已 stale，本轮没有为它们创建 issue 或 source patch。
- 为验证 Bun 侧最高价值的可单层入口，测量了 5 个真实引用 `cssInternals` 的文件，
  使用默认 **4G/512、5 jobs**：**1/5 files green、6/15 tests passed、9 failed、
  30 expects**。`custom-pseudo-ident-escape` **4/4** 已绿；其余失败分别落在
  缺失 `cssInternals._test`、angle 非有限数值序列化、attribute-selector parser、
  nested-selector expansion，不能由一个安全 wrapper 闭合。
- 结论：不把旧 inventory 当作当前事实，不对 CSS `~12-file` cluster 做跨 owner
  混修；下一轮必须从 fresh bounded measurement 选单一 owner。W55 无构建、无全量
  corpus，资源策略继续保持 3–5 jobs，并在 swap/disk 低水位时只做小型 probe。

## Next route

1. Keep the native-syntax compatibility gate limited to the two measured
   optimization controls; evaluate additional V8 intrinsics only from their
   own failing corpus evidence.
2. Keep callbackify stack shape, util.format's constructor-name mismatch, and
   assertion source-position parked behind their generic runtime boundaries.
   Treat the VM parser-message mismatch the same way; use the next measured
   Node or Bun actionable row only after its owner and expected contract are
   identified.
3. Keep test-v8 profiler/queryObjects and broad VM wording changes parked until
   ownership is clear.
4. Keep the W56 Bun.Glob, W57 green builtin/util slices, W58 JSON5/YAML parser
   sample, and W59 Node buffer leaves closed unless a new minimal reproduction
   reopens them. Keep GFM, stripANSI, import-attributes, and buffer constants
   parked behind their generic or multiple owners; prioritize the next one-owner
   Bun row over zlib's native-handle cluster and test-runner's multi-owner boundary.
5. Keep the fixed W51 final-read ordering, W52 file-backed stdin, W53 stdout
   disturbed/reject, and W54 conversion-helper brand owners closed; reopen only
   with a new minimal reproduction.
6. Treat old inventory entries as hypotheses only: refresh the named files before
   dispatching a fix. Select the next task only from a fresh one-owner Bun/Node
   near-green row; keep 3–5 bounded lanes, record per-file pass/fail/expect counts,
   and defer full-corpus runs and broad builds while swap or disk headroom remains
   low.

No local absolute paths, user names, host names, credentials, private URLs, or
machine-specific identifiers belong in future comments, commits, PR text, or
copied logs; use placeholders when examples need them.
