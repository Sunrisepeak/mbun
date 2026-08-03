// YAML flow payload partition; keep raw bytes aligned with js_builtins.cppm lines 2572-4457.
export module mbun.jsc.js_builtins:yaml_flow;

import std;

export namespace mbun::jsc::builtins::detail {

inline constexpr std::string_view kYamlFlowJS = R"JS(
    // Bun.YAML: YAML 1.2 (core schema) parse + stringify, pure JS. Loader is a
    // single-pass character scanner (js-yaml-style architecture, 1.2 core
    // resolution); stringify matches bun's format (flow by default, block with
    // a positive `space`, anchors/aliases for shared references).
    if (typeof Bun.YAML === "undefined") {
// Bun.YAML — YAML 1.2 (core schema) parser + stringifier, pure JS.
// Loader architecture: single-pass character scanner with contexts, similar in
// spirit to js-yaml's loader but resolving with the YAML 1.2 core schema.
const __YAML = (function () {
  "use strict";

  function YAMLParseError(message, line, col) {
    const e = new SyntaxError("YAML Parse error: " + message + (line !== undefined ? " at line " + (line + 1) + ", column " + (col + 1) : ""));
    e.name = "YAMLParseError";
    return e;
  }

  const CONTEXT_FLOW_IN = 1;
  const CONTEXT_FLOW_OUT = 2;
  const CONTEXT_BLOCK_IN = 3;
  const CONTEXT_BLOCK_OUT = 4;

  const CHOMPING_CLIP = 1;
  const CHOMPING_STRIP = 2;
  const CHOMPING_KEEP = 3;

  function is_EOL(c) { return c === 0x0A || c === 0x0D; }
  function is_WHITE_SPACE(c) { return c === 0x09 || c === 0x20; }
  function is_WS_OR_EOL(c) { return c === 0x09 || c === 0x20 || c === 0x0A || c === 0x0D; }
  function is_FLOW_INDICATOR(c) { return c === 0x2C /* , */ || c === 0x5B /* [ */ || c === 0x5D /* ] */ || c === 0x7B /* { */ || c === 0x7D /* } */; }

  function fromHexCode(c) {
    if (c >= 0x30 && c <= 0x39) return c - 0x30;
    const lc = c | 0x20;
    if (lc >= 0x61 && lc <= 0x66) return lc - 0x61 + 10;
    return -1;
  }

  function simpleEscapeSequence(c) {
    switch (c) {
      case 0x30: return "\x00"; // 0
      case 0x61: return "\x07"; // a
      case 0x62: return "\x08"; // b
      case 0x74: return "\x09"; // t
      case 0x09: return "\x09"; // Tab
      case 0x6E: return "\x0A"; // n
      case 0x76: return "\x0B"; // v
      case 0x66: return "\x0C"; // f
      case 0x72: return "\x0D"; // r
      case 0x65: return "\x1B"; // e
      case 0x20: return " ";
      case 0x22: return "\"";
      case 0x2F: return "/";
      case 0x5C: return "\\";
      case 0x4E: return "\x85"; // N: next line
      case 0x5F: return "\xA0"; // _: nbsp
      case 0x4C: return "\u2028"; // L: line sep
      case 0x50: return "\u2029"; // P: para sep
      default: return null;
    }
  }

  function escapedHexLen(c) {
    if (c === 0x78) return 2;  // x
    if (c === 0x75) return 4;  // u
    if (c === 0x55) return 8;  // U
    return 0;
  }

  // ---- core-schema scalar resolution ----
  const RE_INT_DEC = /^[-+]?[0-9]+$/;
  const RE_INT_OCT = /^0o[0-7]+$/;
  const RE_INT_HEX = /^0x[0-9a-fA-F]+$/;
  const RE_FLOAT = /^[-+]?(\.[0-9]+|[0-9]+(\.[0-9]*)?)([eE][-+]?[0-9]+)?$/;
  const RE_INF = /^[-+]?\.(inf|Inf|INF)$/;
  const RE_NAN = /^\.(nan|NaN|NAN)$/;

  function resolvePlain(str) {
    if (typeof str !== "string") return str;
    if (str === "" || str === "~" || str === "null" || str === "Null" || str === "NULL") return null;
    if (str === "true" || str === "True" || str === "TRUE") return true;
    if (str === "false" || str === "False" || str === "FALSE") return false;
    if (RE_INT_DEC.test(str)) {
      const v = parseInt(str, 10);
      return v;
    }
    if (RE_INT_OCT.test(str)) return parseInt(str.slice(2), 8);
    if (RE_INT_HEX.test(str)) return parseInt(str, 16);
    if (RE_FLOAT.test(str)) return parseFloat(str);
    if (RE_INF.test(str)) return str[0] === "-" ? -Infinity : Infinity;
    if (RE_NAN.test(str)) return NaN;
    return str;
  }

  // true when the line's first tab lies inside its leading indentation run
  function tabIsInLineIndent(state) {
    if (state.firstTabInLine === -1) return false;
    for (let i = state.lineStart; i < state.firstTabInLine; i++) {
      if (state.input.charCodeAt(i) !== 0x20) return false;
    }
    return true;
  }

  function throwTab(state) {
    throw YAMLParseError("Tab characters cannot be used as indentation", state.line, state.position - state.lineStart);
  }

  // expanded node count of a value (memoized by identity; aliases share subtrees)
  function expandedSize(v, cache) {
    if (v === null || typeof v !== "object") return 1;
    const hit = cache.get(v);
    if (hit !== undefined) return hit;
    cache.set(v, 1); // cycle guard
    let total = 1;
    if (Array.isArray(v)) {
      for (let i = 0; i < v.length; i++) total += expandedSize(v[i], cache);
    } else {
      for (const k of Object.keys(v)) total += expandedSize(v[k], cache);
    }
    cache.set(v, total);
    return total;
  }

  function State(input) {
    this.input = input;
    this.length = input.length;
    this.position = 0;
    this.line = 0;
    this.lineStart = 0;
    this.lineIndent = 0;
    this.firstTabInLine = -1;
    this.documents = [];
    this.version = null;
    this.checkLineBreaks = false;
    this.tagMap = Object.create(null);
    this.anchorMap = Object.create(null);
    this.tag = null;
    this.anchor = null;
    this.kind = null;
    this.result = "";
    this.mergedProps = 0;
    this.aliasNodes = 0;
    this.sizeCache = new Map();
  }

  function throwError(state, message) {
    throw YAMLParseError("Unexpected token (" + message + ")", state.line, state.position - state.lineStart);
  }

  function throwNamed(state, message) {
    throw YAMLParseError(message, state.line, state.position - state.lineStart);
  }

  function captureSegment(state, start, end, checkJson) {
    if (start < end) {
      const result = state.input.slice(start, end);
      if (checkJson) {
        for (let pos = 0; pos < result.length; pos++) {
          const ch = result.charCodeAt(pos);
          if (!(ch === 0x09 || (ch >= 0x20 && ch <= 0x10FFFF))) {
            throwError(state, "expected valid JSON character");
          }
        }
      } else if (/[\x00-\x08\x0B\x0C\x0E-\x1F\x7F-\x84\x86-\x9F\uFFFE\uFFFF]/.test(result)) {
        throwError(state, "the stream contains non-printable characters");
      }
      state.result += result;
    }
  }

  function mergeMappings(state, destination, source, overridableKeys) {
    if (source === null || typeof source !== "object" || Array.isArray(source)) {
      throwError(state, "cannot merge mappings; the provided source object is unacceptable");
    }
    const keys = Object.keys(source);
    for (let i = 0, len = keys.length; i < len; i++) {
      const key = keys[i];
      if (!Object.prototype.hasOwnProperty.call(destination, key)) {
        destination[key] = source[key];
        overridableKeys[key] = true;
        state.mergedProps++;
        if (state.mergedProps > 1000000) {
          throwError(state, "merge keys materialize too many properties");
        }
      }
    }
  }

  function keyToString(keyNode) {
    if (keyNode === null || keyNode === undefined) return "null";
    return String(keyNode);
  }

  function storeMappingPair(state, result, overridableKeys, keyTag, keyNode, valueNode, startLine, startLineStart, startPos) {
    if (result === null) result = {};

    if (keyTag === "tag:yaml.org,2002:merge" && keyNode === "<<") {
      if (Array.isArray(valueNode)) {
        for (let index = 0, quantity = valueNode.length; index < quantity; index++) {
          mergeMappings(state, result, valueNode[index], overridableKeys);
        }
      } else {
        mergeMappings(state, result, valueNode, overridableKeys);
      }
    } else {
      const skey = keyToString(keyNode);
      if (Object.prototype.hasOwnProperty.call(result, skey) && !overridableKeys[skey]) {
        // duplicate key: last one wins (bun behavior)
      }
      result[skey] = valueNode;
      delete overridableKeys[skey];
    }
    return result;
  }

  function readLineBreak(state) {
    const ch = state.input.charCodeAt(state.position);
    if (ch === 0x0A) {
      state.position++;
    } else if (ch === 0x0D) {
      state.position++;
      if (state.input.charCodeAt(state.position) === 0x0A) state.position++;
    } else {
      throwError(state, "a line break is expected");
    }
    state.line += 1;
    state.lineStart = state.position;
    state.firstTabInLine = -1;
  }

  function skipSeparationSpace(state, allowComments, checkIndent) {
    let lineBreaks = 0;
    let ch = state.input.charCodeAt(state.position);

    while (ch !== 0 && !isNaN(ch)) {
      while (is_WHITE_SPACE(ch)) {
        if (ch === 0x09 && state.firstTabInLine === -1) {
          state.firstTabInLine = state.position;
        }
        ch = state.input.charCodeAt(++state.position);
      }

      if (allowComments && ch === 0x23 /* # */ &&
          (state.position === state.lineStart ||
           is_WS_OR_EOL(state.input.charCodeAt(state.position - 1)))) {
        do {
          ch = state.input.charCodeAt(++state.position);
        } while (ch !== 0x0A && ch !== 0x0D && !isNaN(ch));
      }

      if (is_EOL(ch)) {
        readLineBreak(state);
        ch = state.input.charCodeAt(state.position);
        lineBreaks++;
        state.lineIndent = 0;
        while (ch === 0x20) {
          state.lineIndent++;
          ch = state.input.charCodeAt(++state.position);
        }
      } else {
        break;
      }
    }

    if (checkIndent !== -1 && lineBreaks !== 0 && state.lineIndent < checkIndent) {
      // deficient indentation tolerated only where caller checks afterwards
    }
    return lineBreaks;
  }

  function testDocumentSeparator(state) {
    let _position = state.position;
    let ch = state.input.charCodeAt(_position);
    if ((ch === 0x2D /* - */ || ch === 0x2E /* . */) &&
        ch === state.input.charCodeAt(_position + 1) &&
        ch === state.input.charCodeAt(_position + 2)) {
      _position += 3;
      ch = state.input.charCodeAt(_position);
      if (isNaN(ch) || ch === 0 || is_WS_OR_EOL(ch)) return true;
    }
    return false;
  }

  function writeFoldedLines(state, count) {
    if (count === 1) {
      state.result += " ";
    } else if (count > 1) {
      state.result += "\n".repeat(count - 1);
    }
  }

  function readPlainScalar(state, nodeIndent, withinFlowCollection) {
    let _line, _lineStart, _lineIndent;
    const kind = state.kind;
    const result = state.result;
    let ch = state.input.charCodeAt(state.position);

    if (is_WS_OR_EOL(ch) || is_FLOW_INDICATOR(ch) ||
        ch === 0x23 /* # */ || ch === 0x26 /* & */ || ch === 0x2A /* * */ ||
        ch === 0x21 /* ! */ || ch === 0x7C /* | */ || ch === 0x3E /* > */ ||
        ch === 0x27 /* ' */ || ch === 0x22 /* " */ || ch === 0x25 /* % */ ||
        ch === 0x40 /* @ */ || ch === 0x60 /* ` */) {
      return false;
    }

    let following;
    if (ch === 0x3F /* ? */ || ch === 0x2D /* - */) {
      following = state.input.charCodeAt(state.position + 1);
      if (is_WS_OR_EOL(following) || following === 0 || isNaN(following) ||
          (withinFlowCollection && is_FLOW_INDICATOR(following))) {
        return false;
      }
    }
    if (ch === 0x3A /* : */) {
      following = state.input.charCodeAt(state.position + 1);
      if (is_WS_OR_EOL(following) || isNaN(following) || (withinFlowCollection && is_FLOW_INDICATOR(following))) {
        return false;
      }
    }

    state.kind = "scalar";
    state.result = "";
    let captureStart = state.position;
    let captureEnd = state.position;
    let hasPendingContent = false;

    while (ch !== 0 && !isNaN(ch)) {
      if (ch === 0x3A /* : */) {
        following = state.input.charCodeAt(state.position + 1);
        if (is_WS_OR_EOL(following) || isNaN(following) ||
            (withinFlowCollection && is_FLOW_INDICATOR(following))) {
          break;
        }
      } else if (ch === 0x23 /* # */) {
        const preceding = state.input.charCodeAt(state.position - 1);
        if (is_WS_OR_EOL(preceding)) break;
      } else if ((state.position === state.lineStart && testDocumentSeparator(state)) ||
                 (withinFlowCollection && is_FLOW_INDICATOR(ch))) {
        break;
      } else if (is_EOL(ch)) {
        _line = state.line;
        _lineStart = state.lineStart;
        _lineIndent = state.lineIndent;
        skipSeparationSpace(state, false, -1);
        if (state.lineIndent >= nodeIndent) {
          hasPendingContent = true;
          ch = state.input.charCodeAt(state.position);
          continue;
        } else {
          state.position = captureEnd;
          state.line = _line;
          state.lineStart = _lineStart;
          state.lineIndent = _lineIndent;
          break;
        }
      }

      if (hasPendingContent) {
        captureSegment(state, captureStart, captureEnd, false);
        writeFoldedLines(state, state.line - _line);
        captureStart = captureEnd = state.position;
        hasPendingContent = false;
      }

      if (!is_WHITE_SPACE(ch)) {
        captureEnd = state.position + 1;
      }

      ch = state.input.charCodeAt(++state.position);
    }

    captureSegment(state, captureStart, captureEnd, false);

    if (state.result) return true;

    state.kind = kind;
    state.result = result;
    return false;
  }

  function readSingleQuotedScalar(state, nodeIndent) {
    let ch = state.input.charCodeAt(state.position);
    if (ch !== 0x27 /* ' */) return false;

    state.kind = "scalar";
    state.result = "";
    state.position++;
    let captureStart = state.position;
    let captureEnd = state.position;

    while ((ch = state.input.charCodeAt(state.position)) !== 0 && !isNaN(ch)) {
      if (ch === 0x27 /* ' */) {
        captureSegment(state, captureStart, state.position, true);
        ch = state.input.charCodeAt(++state.position);
        if (ch === 0x27) {
          captureStart = state.position;
          state.position++;
          captureEnd = state.position;
        } else {
          return true;
        }
      } else if (is_EOL(ch)) {
        while (captureEnd > captureStart && is_WHITE_SPACE(state.input.charCodeAt(captureEnd - 1))) captureEnd--;
        captureSegment(state, captureStart, captureEnd, true);
        writeFoldedLines(state, skipSeparationSpace(state, false, nodeIndent));
        if (state.lineIndent < nodeIndent && nodeIndent !== 0) {
          throwError(state, "document start or end marker inside a single quoted scalar");
        }
        if (state.position === state.lineStart && testDocumentSeparator(state)) {
          throwError(state, "document start or end marker inside a single quoted scalar");
        }
        captureStart = captureEnd = state.position;
      } else if (state.position === state.lineStart && testDocumentSeparator(state)) {
        throwError(state, "document start or end marker inside a single quoted scalar");
      } else {
        state.position++;
        captureEnd = state.position;
      }
    }
    throwError(state, "unexpected end of the stream within a single quoted scalar");
  }

  function readDoubleQuotedScalar(state, nodeIndent) {
    let ch = state.input.charCodeAt(state.position);
    if (ch !== 0x22 /* " */) return false;

    state.kind = "scalar";
    state.result = "";
    state.position++;
    let captureStart = state.position;
    let captureEnd = state.position;
    let tmp;

    while ((ch = state.input.charCodeAt(state.position)) !== 0 && !isNaN(ch)) {
      if (ch === 0x22 /* " */) {
        captureSegment(state, captureStart, state.position, true);
        state.position++;
        return true;
      } else if (ch === 0x5C /* \ */) {
        captureSegment(state, captureStart, state.position, true);
        ch = state.input.charCodeAt(++state.position);

        if (is_EOL(ch)) {
          skipSeparationSpace(state, false, nodeIndent);
        } else if (ch < 256 && (tmp = simpleEscapeSequence(ch)) !== null) {
          state.result += tmp;
          state.position++;
        } else if ((tmp = escapedHexLen(ch)) > 0) {
          let hexLength = tmp;
          const hexLengthWas = tmp;
          let hexResult = 0;
          for (; hexLength > 0; hexLength--) {
            ch = state.input.charCodeAt(++state.position);
            if ((tmp = fromHexCode(ch)) >= 0) {
              hexResult = (hexResult << 4) + tmp;
            } else {
              throwError(state, "expected hexadecimal character");
            }
          }
          if (hexResult > 0x10FFFF) throwError(state, "escape sequence exceeds Unicode range");
          if (hexLengthWas === 8 && hexResult >= 0xD800 && hexResult <= 0xDFFF) {
            throwError(state, "escape sequence names a surrogate code point");
          }
          if (hexLengthWas === 4 && hexResult >= 0xD800 && hexResult <= 0xDFFF) {
            if (hexResult >= 0xDC00) throwError(state, "unexpected low surrogate escape");
            if (state.input.charCodeAt(state.position + 1) !== 0x5C /* \\ */ ||
                state.input.charCodeAt(state.position + 2) !== 0x75 /* u */) {
              throwError(state, "unpaired high surrogate escape");
            }
            let low = 0;
            for (let k = 0; k < 4; k++) {
              const d = fromHexCode(state.input.charCodeAt(state.position + 3 + k));
              if (d < 0) throwError(state, "expected hexadecimal character");
              low = (low << 4) + d;
            }
            if (low < 0xDC00 || low > 0xDFFF) throwError(state, "unpaired high surrogate escape");
            state.position += 6;
            hexResult = 0x10000 + ((hexResult - 0xD800) << 10) + (low - 0xDC00);
          }
          state.result += String.fromCodePoint(hexResult);
          state.position++;
        } else {
          throwError(state, "unknown escape sequence");
        }
        captureStart = captureEnd = state.position;
      } else if (is_EOL(ch)) {
        while (captureEnd > captureStart && is_WHITE_SPACE(state.input.charCodeAt(captureEnd - 1))) captureEnd--;
        captureSegment(state, captureStart, captureEnd, true);
        writeFoldedLines(state, skipSeparationSpace(state, false, nodeIndent));
        if (state.lineIndent < nodeIndent && nodeIndent !== 0) {
          throwError(state, "deficient indentation within a double quoted scalar");
        }
        if (state.position === state.lineStart && testDocumentSeparator(state)) {
          throwError(state, "document start or end marker inside a double quoted scalar");
        }
        captureStart = captureEnd = state.position;
      } else if (state.position === state.lineStart && testDocumentSeparator(state)) {
        throwError(state, "document start or end marker inside a double quoted scalar");
      } else {
        state.position++;
        captureEnd = state.position;
      }
    }
    throwError(state, "unexpected end of the stream within a double quoted scalar");
  }

  function readFlowCollection(state, nodeIndent) {
    let ch = state.input.charCodeAt(state.position);
    let terminator;
    let isMapping;
    let result;

    if (ch === 0x5B /* [ */) {
      terminator = 0x5D; /* ] */
      isMapping = false;
      result = [];
    } else if (ch === 0x7B /* { */) {
      terminator = 0x7D; /* } */
      isMapping = true;
      result = {};
    } else {
      return false;
    }

    if (state.anchor !== null) {
      state.anchorMap[state.anchor] = result;
    }

    ch = state.input.charCodeAt(++state.position);

    const tag = state.tag;
    const anchor = state.anchor;
    let readNext = true;
    let isExplicitPair, isPair, keyNode, keyTag, valueNode;
    const overridableKeys = Object.create(null);

    while (ch !== 0 && !isNaN(ch)) {
      if (skipSeparationSpace(state, true, nodeIndent) !== 0 && state.lineIndent < nodeIndent) {
        throwError(state, "bad indentation within a flow collection");
      }

      ch = state.input.charCodeAt(state.position);

      if (ch === terminator) {
        state.position++;
        state.tag = tag;
        state.anchor = anchor;
        state.kind = isMapping ? "mapping" : "sequence";
        state.result = result;
        return true;
      } else if (!readNext) {
        throwError(state, "missed comma between flow collection entries");
      } else if (ch === 0x2C /* , */) {
        throwError(state, "expected the node content, but found ','");
      }

      keyTag = keyNode = valueNode = null;
      isPair = isExplicitPair = false;

      if (ch === 0x3F /* ? */) {
        const following = state.input.charCodeAt(state.position + 1);
        if (is_WS_OR_EOL(following) || is_FLOW_INDICATOR(following) || following === 0 || isNaN(following)) {
          isPair = isExplicitPair = true;
          state.position++;
          skipSeparationSpace(state, true, nodeIndent);
        }
      }

      const _line = state.line;
      const _lineStart = state.lineStart;
      const _pos = state.position;
      composeNode(state, nodeIndent, CONTEXT_FLOW_IN, false, true);
      keyTag = state.tag;
      keyNode = state.result;
      skipSeparationSpace(state, true, nodeIndent);

      ch = state.input.charCodeAt(state.position);

      if (ch === 0x3A /* : */ && !(isExplicitPair || isMapping || state.line === _line)) {
        throwNamed(state, "Multiline implicit keys are not allowed");
      }
      if (ch === 0x3A /* : */ && (isExplicitPair || isMapping || state.line === _line)) {
        isPair = true;
        ch = state.input.charCodeAt(++state.position);
        skipSeparationSpace(state, true, nodeIndent);
        composeNode(state, nodeIndent, CONTEXT_FLOW_IN, false, true);
        valueNode = state.result;
      }

      if (isMapping) {
        storeMappingPair(state, result, overridableKeys, keyTag, keyNode, valueNode, _line, _lineStart, _pos);
      } else if (isPair) {
        const pair = storeMappingPair(state, null, overridableKeys, keyTag, keyNode, valueNode, _line, _lineStart, _pos);
        result.push(pair);
      } else {
        result.push(keyNode);
      }

      skipSeparationSpace(state, true, nodeIndent);

      ch = state.input.charCodeAt(state.position);

      if (ch === 0x2C /* , */) {
        readNext = true;
        ch = state.input.charCodeAt(++state.position);
      } else {
        readNext = false;
      }
    }

    throwError(state, "unexpected end of the stream within a flow collection");
  }

  function readBlockScalar(state, nodeIndent) {
    let chomping = CHOMPING_CLIP;
    let didReadContent = false;
    let detectedIndent = false;
    let textIndent = nodeIndent;
    let emptyLines = 0;
    let atMoreIndented = false;

    let ch = state.input.charCodeAt(state.position);

    let folding;
    if (ch === 0x7C /* | */) {
      folding = false;
    } else if (ch === 0x3E /* > */) {
      folding = true;
    } else {
      return false;
    }

    state.kind = "scalar";
    state.result = "";

    let tmp = 0;
    while (ch !== 0 && !isNaN(ch)) {
      ch = state.input.charCodeAt(++state.position);

      if (ch === 0x2B /* + */ || ch === 0x2D /* - */) {
        if (CHOMPING_CLIP === chomping) {
          chomping = (ch === 0x2B) ? CHOMPING_KEEP : CHOMPING_STRIP;
        } else {
          throwError(state, "repeat of a chomping mode identifier");
        }
      } else if ((tmp = ch - 0x30) >= 1 && tmp <= 9) {
        if (tmp === 0) {
          throwError(state, "bad explicit indentation width of a block scalar; it cannot be less than one");
        } else if (!detectedIndent) {
          textIndent = Math.max(nodeIndent - 1, 0) + tmp;
          detectedIndent = true;
        } else {
          throwError(state, "repeat of an indentation width identifier");
        }
      } else if (ch === 0x30) {
        throwError(state, "bad explicit indentation width of a block scalar; it cannot be less than one");
      } else {
        break;
      }
    }

    if (is_WHITE_SPACE(ch)) {
      do { ch = state.input.charCodeAt(++state.position); }
      while (is_WHITE_SPACE(ch));

      if (ch === 0x23 /* # */) {
        do { ch = state.input.charCodeAt(++state.position); }
        while (!is_EOL(ch) && (ch !== 0) && !isNaN(ch));
      }
    }

    let maxLeadingEmptyIndent = 0;
    while (ch !== 0 && !isNaN(ch)) {
      readLineBreak(state);
      state.lineIndent = 0;

      ch = state.input.charCodeAt(state.position);

      while ((!detectedIndent || state.lineIndent < textIndent) && (ch === 0x20)) {
        state.lineIndent++;
        ch = state.input.charCodeAt(++state.position);
      }

      if (ch === 0 || isNaN(ch)) {
        // EOF: an unterminated all-space line counts as an empty line only
        // when no content was read and it reaches the content indentation
        // (or the indentation is still undetermined).
        if (state.position > state.lineStart && !didReadContent &&
            (!detectedIndent || state.lineIndent >= textIndent)) {
          emptyLines++;
        }
        break;
      }

      if (is_EOL(ch)) {
        if (!detectedIndent && state.lineIndent > maxLeadingEmptyIndent) {
          maxLeadingEmptyIndent = state.lineIndent;
        }
        emptyLines++;
        continue;
      }

      if (!detectedIndent && state.lineIndent > textIndent) {
        textIndent = state.lineIndent;
      }

      // End of the scalar: deficient indentation or a document boundary marker.
      if (state.lineIndent < textIndent ||
          (state.lineIndent === 0 && testDocumentSeparator(state))) {
        if (ch === 0x09 /* Tab */) {
          throwTab(state);
        }
        break;
      }

      if (ch === 0x09 /* Tab */ && !detectedIndent && state.lineIndent === 0) {
        throwTab(state);
      }

      if (!detectedIndent && maxLeadingEmptyIndent > textIndent) {
        throwError(state, "a leading all-space line must not have too many spaces");
      }

      // Folded style: fold line breaks.
      if (folding) {
        if (is_WHITE_SPACE(ch)) {
          // Line starts with white space => more-indented line: breaks preserved.
          atMoreIndented = true;
          state.result += "\n".repeat(didReadContent ? 1 + emptyLines : emptyLines);
        } else if (atMoreIndented) {
          // End of more-indented block.
          atMoreIndented = false;
          state.result += "\n".repeat(emptyLines + 1);
        } else if (emptyLines === 0) {
          if (didReadContent) state.result += " ";
        } else {
          state.result += "\n".repeat(emptyLines);
        }
      } else {
        // Literal style: keep all line breaks.
        state.result += "\n".repeat(didReadContent ? 1 + emptyLines : emptyLines);
      }

      didReadContent = true;
      detectedIndent = true;
      emptyLines = 0;
      const captureStart = state.position;

      while (!is_EOL(ch) && (ch !== 0) && !isNaN(ch)) {
        ch = state.input.charCodeAt(++state.position);
      }

      captureSegment(state, captureStart, state.position, false);
    }

    // Chomping epilogue (shared by every exit path). A content line ending at
    // EOF is treated as if terminated by a line break.
    if (chomping === CHOMPING_KEEP) {
      state.result += "\n".repeat(emptyLines + (didReadContent ? 1 : 0));
    } else if (chomping === CHOMPING_CLIP) {
      if (didReadContent) state.result += "\n";
    }

    return true;
  }

  function readBlockSequence(state, nodeIndent) {
    let _line;
    const _tag = state.tag;
    const _anchor = state.anchor;
    const result = [];
    let following;
    let detected = false;
    let ch;

    if (state.firstTabInLine !== -1) {
      const c0 = state.input.charCodeAt(state.position);
      const f0 = state.input.charCodeAt(state.position + 1);
      if (c0 === 0x2D /* - */ && (is_WS_OR_EOL(f0) || f0 === 0 || isNaN(f0))) {
        throwTab(state);
      }
      return false;
    }

    if (state.anchor !== null) {
      state.anchorMap[state.anchor] = result;
    }

    ch = state.input.charCodeAt(state.position);

    while (ch !== 0 && !isNaN(ch)) {
      if (state.firstTabInLine !== -1) {
        state.position = state.firstTabInLine;
        throwTab(state);
      }

      if (ch !== 0x2D /* - */) break;

      following = state.input.charCodeAt(state.position + 1);
      if (!is_WS_OR_EOL(following) && following !== 0 && !isNaN(following)) break;

      if (state.firstTabInLine !== -1 && state.firstTabInLine < state.position) {
        throwTab(state);
      }

      detected = true;
      state.position++;

      if (skipSeparationSpace(state, true, -1)) {
        if (state.lineIndent <= nodeIndent) {
          result.push(null);
          ch = state.input.charCodeAt(state.position);
          continue;
        }
      }

      _line = state.line;
      composeNode(state, nodeIndent, CONTEXT_BLOCK_IN, false, true);
      result.push(state.result);
      skipSeparationSpace(state, true, -1);

      ch = state.input.charCodeAt(state.position);

      if ((state.line === _line || state.lineIndent > nodeIndent) && (ch !== 0 && !isNaN(ch))) {
        if (tabIsInLineIndent(state)) throwTab(state);
        throwError(state, "bad indentation of a sequence entry");
      } else if (state.lineIndent < nodeIndent) {
        break;
      }
    }

    if (detected) {
      state.tag = _tag;
      state.anchor = _anchor;
      state.kind = "sequence";
      state.result = result;
      return true;
    }
    return false;
  }

  function readBlockMapping(state, nodeIndent, flowIndent) {
    const _tag = state.tag;
    const _anchor = state.anchor;
    const result = {};
    const overridableKeys = Object.create(null);
    let following;
    let allowCompact = false;
    let _line, _keyLine, _keyLineStart, _keyPos;
    let _pos;
    let atExplicitKey = false;
    let detected = false;
    let ch;
    let keyTag = null;
    let keyNode = null;
    let valueNode = null;

    if (state.firstTabInLine !== -1) {
      const c0 = state.input.charCodeAt(state.position);
      const f0 = state.input.charCodeAt(state.position + 1);
      if (((c0 === 0x3F /* ? */ || c0 === 0x3A /* : */) &&
           (is_WS_OR_EOL(f0) || f0 === 0 || isNaN(f0))) ||
          lineHasImplicitKeyColon(state)) {
        throwTab(state);
      }
      return false;
    }

    if (state.anchor !== null) {
      state.anchorMap[state.anchor] = result;
    }

    ch = state.input.charCodeAt(state.position);

    while (ch !== 0 && !isNaN(ch)) {
      if (!atExplicitKey && state.firstTabInLine !== -1) {
        state.position = state.firstTabInLine;
        throwTab(state);
      }

      following = state.input.charCodeAt(state.position + 1);
      _line = state.line;

      // Explicit notation case. There are two separate blocks:
      if ((ch === 0x3F /* ? */ || ch === 0x3A /* : */) &&
          (is_WS_OR_EOL(following) || following === 0 || isNaN(following))) {
        if (state.firstTabInLine !== -1 && state.firstTabInLine < state.position) {
          throwTab(state);
        }
        if (ch === 0x3F /* ? */) {
          if (atExplicitKey) {
            storeMappingPair(state, result, overridableKeys, keyTag, keyNode, null, _keyLine, _keyLineStart, _keyPos);
            keyTag = keyNode = valueNode = null;
          }
          detected = true;
          atExplicitKey = true;
          allowCompact = true;
        } else if (atExplicitKey) {
          // i.e. 0x3A : === character after the explicit key.
          atExplicitKey = false;
          allowCompact = true;
        } else {
          // ": value" with no key => implicit null key; a compact collection
          // on the same line is not allowed here
          detected = true;
          atExplicitKey = false;
          allowCompact = false;
          keyTag = null;
          keyNode = null;
          valueNode = null;
        }

        state.position += 1;
        ch = following;

      // Implicit notation case.
      } else {
        _keyLine = state.line;
        _keyLineStart = state.lineStart;
        _keyPos = state.position;

        if (!composeNode(state, flowIndent, CONTEXT_FLOW_OUT, false, true)) {
          break; // Reading is done. Go to the epilogue.
        }

        if (state.line === _line) {
          ch = state.input.charCodeAt(state.position);

          while (is_WHITE_SPACE(ch)) {
            ch = state.input.charCodeAt(++state.position);
          }

          if (ch === 0x3A /* : */) {
            if (state.firstTabInLine !== -1 && state.firstTabInLine < _keyPos) {
              state.position = state.firstTabInLine;
              throwTab(state);
            }
            ch = state.input.charCodeAt(++state.position);

            if (!is_WS_OR_EOL(ch) && !isNaN(ch)) {
              throwError(state, "a whitespace character is expected after the key-value separator within a block mapping");
            }

            if (atExplicitKey) {
              storeMappingPair(state, result, overridableKeys, keyTag, keyNode, null, _keyLine, _keyLineStart, _keyPos);
              keyTag = keyNode = valueNode = null;
            }

            detected = true;
            atExplicitKey = false;
            allowCompact = false;
            keyTag = state.tag;
            keyNode = state.result;

          } else if (detected) {
            throwError(state, "can not read an implicit mapping pair; a colon is missed");
          } else {
            state.tag = _tag;
            state.anchor = _anchor;
            return true; // Keep the result of `composeNode`.
          }

        } else if (detected) {
          throwNamed(state, "Multiline implicit keys are not allowed");
        } else {
          state.tag = _tag;
          state.anchor = _anchor;
          return true; // Keep the result of `composeNode`.
        }
      }

      // Common reading code for both explicit and implicit notations.
      if (state.line === _line || state.lineIndent > nodeIndent) {
        if (atExplicitKey) {
          _keyLine = state.line;
          _keyLineStart = state.lineStart;
          _keyPos = state.position;
        }

        if (composeNode(state, nodeIndent, CONTEXT_BLOCK_OUT, true, allowCompact)) {
          if (atExplicitKey) {
            keyNode = state.result;
          } else {
            valueNode = state.result;
          }
        }

        if (!atExplicitKey) {
          storeMappingPair(state, result, overridableKeys, keyTag, keyNode, valueNode, _keyLine, _keyLineStart, _keyPos);
          keyTag = keyNode = valueNode = null;
        }

        skipSeparationSpace(state, true, -1);
        ch = state.input.charCodeAt(state.position);
      }

      if ((state.line === _line || state.lineIndent > nodeIndent) && (ch !== 0 && !isNaN(ch))) {
        if (tabIsInLineIndent(state)) throwTab(state);
        throwError(state, "bad indentation of a mapping entry");
      } else if (state.lineIndent < nodeIndent) {
        break;
      }
    }

    // Epilogue.
    if (atExplicitKey) {
      storeMappingPair(state, result, overridableKeys, keyTag, keyNode, null, _keyLine, _keyLineStart, _keyPos);
    }

    if (detected) {
      state.tag = _tag;
      state.anchor = _anchor;
      state.kind = "mapping";
      state.result = result;
    }

    return detected;
  }

  function readTagProperty(state) {
    let _position;
    let isVerbatim = false;
    let isNamed = false;
    let tagHandle;
    let tagName;
    let ch;

    ch = state.input.charCodeAt(state.position);

    if (ch !== 0x21 /* ! */) return false;

    if (state.tag !== null) {
      throwNamed(state, "Multiple tags on the same node are not allowed");
    }

    ch = state.input.charCodeAt(++state.position);

    if (ch === 0x3C /* < */) {
      isVerbatim = true;
      ch = state.input.charCodeAt(++state.position);
    } else if (ch === 0x21 /* ! */) {
      isNamed = true;
      tagHandle = "!!";
      ch = state.input.charCodeAt(++state.position);
    } else {
      tagHandle = "!";
    }

    _position = state.position;

    if (isVerbatim) {
      do { ch = state.input.charCodeAt(++state.position); }
      while (ch !== 0 && !isNaN(ch) && ch !== 0x3E /* > */);

      if (state.position < state.length) {
        tagName = state.input.slice(_position, state.position);
        ch = state.input.charCodeAt(++state.position);
      } else {
        throwError(state, "unexpected end of the stream within a verbatim tag");
      }
    } else {
      while (ch !== 0 && !isNaN(ch) && !is_WS_OR_EOL(ch) && !is_FLOW_INDICATOR(ch)) {
        if (ch === 0x21 /* ! */) {
          if (!isNamed) {
            tagHandle = state.input.slice(_position - 1, state.position + 1);
            if (!/^(?:!|!!|![a-z\-]+!)$/i.test(tagHandle)) {
              throwError(state, "named tag handle cannot contain such characters");
            }
            isNamed = true;
            _position = state.position + 1;
          } else {
            throwError(state, "tag suffix cannot contain exclamation marks");
          }
        }
        ch = state.input.charCodeAt(++state.position);
      }

      tagName = state.input.slice(_position, state.position);
    }

    if (tagName && !/^[a-z0-9!$&'()*+,\-.\/:;<=>?@\[\]\\\^_`{|}~%À-￿]*$/i.test(tagName)) {
      throwError(state, "tag name cannot contain such characters: " + tagName);
    }

    try {
      tagName = decodeURIComponent(tagName);
    } catch (err) {
      throwError(state, "tag name is malformed: " + tagName);
    }

    if (isVerbatim) {
      state.tag = tagName;
    } else if (Object.prototype.hasOwnProperty.call(state.tagMap, tagHandle)) {
      state.tag = state.tagMap[tagHandle] + tagName;
    } else if (tagHandle === "!") {
      state.tag = "!" + tagName;
    } else if (tagHandle === "!!") {
      state.tag = "tag:yaml.org,2002:" + tagName;
    } else {
      throwError(state, "undeclared tag handle \"" + tagHandle + "\"");
    }

    return true;
  }

  function readAnchorProperty(state) {
    let ch = state.input.charCodeAt(state.position);
    if (ch !== 0x26 /* & */) return false;

    if (state.anchor !== null) {
      throwNamed(state, "Multiple anchors on the same node are not allowed");
    }

    ch = state.input.charCodeAt(++state.position);
    const _position = state.position;

    while (ch !== 0 && !isNaN(ch) && !is_WS_OR_EOL(ch) && !is_FLOW_INDICATOR(ch)) {
      ch = state.input.charCodeAt(++state.position);
    }

    if (state.position === _position) {
      throwError(state, "name of an anchor node must contain at least one character");
    }

    state.anchor = state.input.slice(_position, state.position);
    return true;
  }

  function readAlias(state) {
    let ch = state.input.charCodeAt(state.position);
    if (ch !== 0x2A /* * */) return false;

    ch = state.input.charCodeAt(++state.position);
    const _position = state.position;

    while (ch !== 0 && !isNaN(ch) && !is_WS_OR_EOL(ch) && !is_FLOW_INDICATOR(ch)) {
      ch = state.input.charCodeAt(++state.position);
    }

    if (state.position === _position) {
      throwError(state, "name of an alias node must contain at least one character");
    }

    const alias = state.input.slice(_position, state.position);
    if (!Object.prototype.hasOwnProperty.call(state.anchorMap, alias)) {
      throwError(state, "unidentified alias \"" + alias + "\"");
    }

    state.aliasNodes += expandedSize(state.anchorMap[alias], state.sizeCache);
    if (state.aliasNodes > 10000000) {
      throwError(state, "alias expansion is too large");
    }

    state.result = state.anchorMap[alias];
    state.kind = typeof state.result === "object" && state.result !== null ? (Array.isArray(state.result) ? "sequence" : "mapping") : "scalar";
    skipSeparationSpace(state, true, -1);
    return true;
  }

  // apply an explicit (non-"!") tag to the freshly composed node
  function resolveByTag(state, hasContent) {
    const t = state.tag;
    let v = state.result;
    if (!hasContent || state.kind === null) {
      // tagged empty node
      if (t === "tag:yaml.org,2002:str") { state.result = ""; return; }
      if (t === "tag:yaml.org,2002:null") { state.result = null; return; }
      if (t === "tag:yaml.org,2002:seq") { state.result = []; return; }
      if (t === "tag:yaml.org,2002:map" || t === "tag:yaml.org,2002:set") { state.result = {}; return; }
      // any other tag on an empty node resolves to null
      state.result = null;
      return;
    }
    if (state.kind !== "scalar") {
      // collections keep their value regardless of the tag (lenient, like bun)
      return;
    }
    if (!state.plain) {
      // quoted / block scalars keep their string value under any tag
      return;
    }
    if (state.plain && typeof v !== "string" && typeof state.raw === "string") v = state.raw;
    if (typeof v !== "string") v = String(v);
    if (t === "tag:yaml.org,2002:str") {
      state.result = String(v === null ? "" : v);
    } else if (t === "tag:yaml.org,2002:int") {
      const s = String(v).trim();
      let n;
      if (RE_INT_DEC.test(s)) n = parseInt(s, 10);
      else if (RE_INT_OCT.test(s)) n = parseInt(s.slice(2), 8);
      else if (RE_INT_HEX.test(s)) n = parseInt(s, 16);
      else throwError(state, "cannot resolve value as !!int: " + s);
      state.result = n;
    } else if (t === "tag:yaml.org,2002:float") {
      const s = String(v).trim();
      if (RE_FLOAT.test(s)) state.result = parseFloat(s);
      else if (RE_INF.test(s)) state.result = s[0] === "-" ? -Infinity : Infinity;
      else if (RE_NAN.test(s)) state.result = NaN;
      else if (RE_INT_DEC.test(s)) state.result = parseFloat(s);
      else throwError(state, "cannot resolve value as !!float: " + s);
    } else if (t === "tag:yaml.org,2002:bool") {
      const s = String(v);
      if (/^(true|True|TRUE)$/.test(s)) state.result = true;
      else if (/^(false|False|FALSE)$/.test(s)) state.result = false;
      else throwError(state, "cannot resolve value as !!bool: " + s);
    } else if (t === "tag:yaml.org,2002:null") {
      const s = v === null ? "" : String(v);
      if (/^(~|null|Null|NULL|)$/.test(s)) state.result = null;
      // otherwise keep the value as-is (lenient, matches bun)
    } else if (t === "tag:yaml.org,2002:binary") {
      // keep as string (raw base64)
      state.result = v;
    } else if (t === "tag:yaml.org,2002:seq") {
      if (!Array.isArray(v)) throwError(state, "cannot resolve value as !!seq");
    } else if (t === "tag:yaml.org,2002:map") {
      if (v === null || typeof v !== "object" || Array.isArray(v)) throwError(state, "cannot resolve value as !!map");
    } else if (t === "tag:yaml.org,2002:merge") {
      // leave "<<" string
    } else {
      // unknown/application tag: leave the value as-is (string for scalars)
    }
  }

  function lineHasImplicitKeyColon(state) {
    let pos = state.position;
    let depth = 0;
    let ch;
    while (pos < state.length && !is_EOL(ch = state.input.charCodeAt(pos))) {
      if (ch === 0x27 /* ' */ || ch === 0x22 /* " */) {
        const q = ch;
        pos++;
        while (pos < state.length) {
          const c2 = state.input.charCodeAt(pos);
          if (is_EOL(c2)) return false;
          if (q === 0x22 && c2 === 0x5C) { pos += 2; continue; }
          if (c2 === q) break;
          pos++;
        }
        if (pos >= state.length) return false;
        pos++;
        continue;
      }
      if (ch === 0x5B /* [ */ || ch === 0x7B /* { */) depth++;
      else if (ch === 0x5D /* ] */ || ch === 0x7D /* } */) { if (depth > 0) depth--; }
      else if (ch === 0x23 /* # */ && pos > 0 && is_WHITE_SPACE(state.input.charCodeAt(pos - 1))) return false;
      else if (ch === 0x3A /* : */ && depth === 0) {
        const nx = state.input.charCodeAt(pos + 1);
        if (is_WS_OR_EOL(nx) || isNaN(nx) || nx === 0) return true;
      }
      pos++;
    }
    return false;
  }

  function composeNode(state, parentIndent, nodeContext, allowToSeek, allowCompact) {
    let allowBlockStyles;
    let allowBlockScalars;
    let allowBlockCollections;
    let indentStatus = 1; // 1: this>parent, 0: this=parent, -1: this<parent
    let atNewLine = false;
    let hasContent = false;
    let typeIndex, typeQuantity, typeList;
    let type;
    let flowIndent;
    let blockIndent;

    state.tag = null;
    state.anchor = null;
    state.kind = null;
    state.result = null;
    state.plain = false;
    state.raw = undefined;

    allowBlockStyles = allowBlockScalars = allowBlockCollections =
      CONTEXT_BLOCK_OUT === nodeContext ||
      CONTEXT_BLOCK_IN === nodeContext;

    if (allowToSeek) {
      if (skipSeparationSpace(state, true, -1)) {
        atNewLine = true;

        if (state.lineIndent > parentIndent) {
          indentStatus = 1;
        } else if (state.lineIndent === parentIndent) {
          indentStatus = 0;
        } else if (state.lineIndent < parentIndent) {
          indentStatus = -1;
        }
      }
    }

    if (indentStatus === 1) {
      let propsStartPos = -1, propsStartLine = 0, propsStartLineStart = 0, propsStartLineIndent = 0;
      for (;;) {
        const _pp = state.position, _pl = state.line, _pls = state.lineStart, _pli = state.lineIndent;
        const ch0 = state.input.charCodeAt(state.position);
        if ((ch0 === 0x21 /* ! */ && state.tag !== null) ||
            (ch0 === 0x26 /* & */ && state.anchor !== null)) {
          // A second property batch is only legal when it belongs to the key
          // node of a block mapping that starts here.
          if (allowBlockStyles && lineHasImplicitKeyColon(state)) break;
          if (ch0 === 0x26) throwNamed(state, "Multiple anchors on the same node are not allowed");
          throwNamed(state, "Multiple tags on the same node are not allowed");
        }
        if (!(readTagProperty(state) || readAnchorProperty(state))) break;
        if (propsStartPos === -1) {
          propsStartPos = _pp; propsStartLine = _pl; propsStartLineStart = _pls; propsStartLineIndent = _pli;
        }
        if (skipSeparationSpace(state, true, -1)) {
          atNewLine = true;
          allowBlockCollections = allowBlockStyles;
          propsStartPos = -1; // a break follows: props bind to this node

          if (state.lineIndent > parentIndent) {
            indentStatus = 1;
          } else if (state.lineIndent === parentIndent) {
            indentStatus = 0;
          } else if (state.lineIndent < parentIndent) {
            indentStatus = -1;
          }
          if (indentStatus !== 1) break;
        } else {
          allowBlockCollections = false;
        }
      }

      if (propsStartPos !== -1 && allowBlockStyles && (atNewLine || allowCompact) &&
          (state.tag !== null || state.anchor !== null) &&
          lineHasImplicitKeyColon(state)) {
        // Same-line properties followed by "key: ..." bind to the key node:
        // rewind and let the block mapping re-read them as key properties.
        state.position = propsStartPos;
        state.line = propsStartLine;
        state.lineStart = propsStartLineStart;
        state.lineIndent = propsStartLineIndent;
        state.tag = null;
        state.anchor = null;
        atNewLine = true;
        allowBlockCollections = true;
      }
    }

    if (allowBlockCollections) {
      allowBlockCollections = atNewLine || allowCompact;
    }

    if (indentStatus === 1 || CONTEXT_BLOCK_OUT === nodeContext) {
      if (CONTEXT_FLOW_IN === nodeContext || CONTEXT_FLOW_OUT === nodeContext) {
        flowIndent = parentIndent;
      } else {
        flowIndent = parentIndent + 1;
      }

      blockIndent = state.position - state.lineStart;

      if (indentStatus === 1) {
        if (allowBlockCollections &&
            (readBlockSequence(state, blockIndent) ||
             readBlockMapping(state, blockIndent, flowIndent)) ||
            readFlowCollection(state, flowIndent)) {
          hasContent = true;
        } else {
          if ((allowBlockScalars && readBlockScalar(state, flowIndent)) ||
              readSingleQuotedScalar(state, flowIndent) ||
              readDoubleQuotedScalar(state, flowIndent)) {
            hasContent = true;
            state.plain = false;
          } else if (readAlias(state)) {
            hasContent = true;
            state.plain = false;

            if (state.tag !== null || state.anchor !== null) {
              throwError(state, "alias node should not have any properties");
            }
          } else if (readPlainScalar(state, flowIndent, CONTEXT_FLOW_IN === nodeContext)) {
            hasContent = true;
            state.plain = true;
          }
        }
      } else if (indentStatus === 0) {
        // Special case: block sequences are allowed to have same indentation level as the parent.
        // http://www.yaml.org/spec/1.2/spec.html#id2799784
        hasContent = allowBlockCollections && readBlockSequence(state, blockIndent);
      }
    }

    // Resolution: plain untagged scalars go through core-schema resolution;
    // explicit tags are applied; "!" forces string on scalars.
    if (hasContent && state.kind === "scalar" && state.plain &&
        (state.tag === null || state.tag === "?")) {
      if (state.result === "<<") {
        state.tag = "tag:yaml.org,2002:merge";
      } else {
        if (typeof state.result === "string") state.raw = state.result;
        state.result = resolvePlain(state.result);
      }
    } else if (state.tag !== null && state.tag !== "!" && state.tag !== "?") {
      resolveByTag(state, hasContent);
    } else if (state.tag === "!" && state.result === null && state.kind === null) {
      state.result = "";
    }

    if (state.anchor !== null) {
      state.anchorMap[state.anchor] = state.result;
    }

    return state.tag !== null || state.anchor !== null || hasContent;
  }

  function readDocument(state) {
    const documentStart = state.position;
    let _position;
    let directiveName;
    let directiveArgs;
    let hasDirectives = false;
    let ch;

    state.version = null;
    state.checkLineBreaks = false;
    state.tagMap = Object.create(null);
    state.anchorMap = Object.create(null);

    while ((ch = state.input.charCodeAt(state.position)) !== 0 && !isNaN(ch)) {
      skipSeparationSpace(state, true, -1);

      ch = state.input.charCodeAt(state.position);

      if (state.lineIndent > 0 || ch !== 0x25 /* % */) {
        break;
      }

      hasDirectives = true;
      ch = state.input.charCodeAt(++state.position);
      _position = state.position;

      while (ch !== 0 && !isNaN(ch) && !is_WS_OR_EOL(ch)) {
        ch = state.input.charCodeAt(++state.position);
      }

      directiveName = state.input.slice(_position, state.position);
      directiveArgs = [];

      if (directiveName.length < 1) {
        throwError(state, "directive name must not be less than one character in length");
      }

      while (ch !== 0 && !isNaN(ch)) {
        while (is_WHITE_SPACE(ch)) {
          ch = state.input.charCodeAt(++state.position);
        }

        if (ch === 0x23 /* # */) {
          do { ch = state.input.charCodeAt(++state.position); }
          while (ch !== 0 && !isNaN(ch) && !is_EOL(ch));
          break;
        }

        if (is_EOL(ch)) break;

        _position = state.position;

        while (ch !== 0 && !isNaN(ch) && !is_WS_OR_EOL(ch)) {
          ch = state.input.charCodeAt(++state.position);
        }

        directiveArgs.push(state.input.slice(_position, state.position));
      }

      if (ch !== 0 && !isNaN(ch)) readLineBreak(state);

      if (directiveName === "YAML") {
        if (state.version !== null) {
          throwError(state, "duplication of %YAML directive");
        }
        if (directiveArgs.length !== 1) {
          throwError(state, "YAML directive accepts exactly one argument");
        }
        const match = /^([0-9]+)\.([0-9]+)$/.exec(directiveArgs[0]);
        if (match === null) {
          throwError(state, "ill-formed argument of the YAML directive");
        }
        const major = parseInt(match[1], 10);
        const minor = parseInt(match[2], 10);
        if (major !== 1) {
          throwError(state, "unacceptable YAML version of the document");
        }
        state.version = directiveArgs[0];
        state.checkLineBreaks = (minor < 2);
      } else if (directiveName === "TAG") {
        if (directiveArgs.length !== 2) {
          throwError(state, "TAG directive accepts exactly two arguments");
        }
        const handle = directiveArgs[0];
        const prefix = directiveArgs[1];
        if (!/^!([0-9A-Za-z_-]*!)?$/.test(handle)) {
          throwError(state, "ill-formed tag handle (first argument) of the TAG directive");
        }
        if (Object.prototype.hasOwnProperty.call(state.tagMap, handle)) {
          throwError(state, "there is a previously declared suffix for \"" + handle + "\" tag handle");
        }
        if (!/^(?:[0-9A-Za-z_.!~*'()\[\]\/#;?:@&=+$,%-]|%[0-9a-fA-F]{2})*$/.test(prefix)) {
          throwError(state, "ill-formed tag prefix (second argument) of the TAG directive");
        }
        let decoded;
        try {
          decoded = decodeURIComponent(prefix);
        } catch (err) {
          throwError(state, "tag prefix is malformed: " + prefix);
        }
        state.tagMap[handle] = decoded;
      } else {
        // Unknown directives are ignored with a warning per spec.
      }
    }

    skipSeparationSpace(state, true, -1);

    let hasExplicitStart = false;
    let sameLineContent = false;
    if (state.lineIndent === 0 &&
        state.input.charCodeAt(state.position) === 0x2D /* - */ &&
        state.input.charCodeAt(state.position + 1) === 0x2D /* - */ &&
        state.input.charCodeAt(state.position + 2) === 0x2D /* - */ &&
        (is_WS_OR_EOL(state.input.charCodeAt(state.position + 3)) || isNaN(state.input.charCodeAt(state.position + 3)) || state.input.charCodeAt(state.position + 3) === 0)) {
      state.position += 3;
      hasExplicitStart = true;
      if (skipSeparationSpace(state, true, -1) === 0) {
        sameLineContent = true;
      }
    } else if (hasDirectives) {
      throwError(state, "directives end mark is expected");
    }

    const composed = composeNode(state, state.lineIndent - 1, CONTEXT_BLOCK_OUT, false, !sameLineContent);
    skipSeparationSpace(state, true, -1);

    if (state.checkLineBreaks &&
        /[\x85\u2028\u2029]/.test(state.input.slice(documentStart, state.position))) {
      // non-ASCII line breaks in YAML 1.1: ignore (warning in js-yaml)
    }

    if (hasExplicitStart || composed) {
      state.documents.push(state.result);
    }

    if (state.position === state.lineStart && testDocumentSeparator(state)) {
      if (state.input.charCodeAt(state.position) === 0x2E /* . */) {
        state.position += 3;
        let chAfter = state.input.charCodeAt(state.position);
        while (is_WHITE_SPACE(chAfter)) chAfter = state.input.charCodeAt(++state.position);
        if (chAfter === 0x23 /* # */) {
          while (!is_EOL(chAfter) && chAfter !== 0 && !isNaN(chAfter)) chAfter = state.input.charCodeAt(++state.position);
        }
        if (!is_EOL(chAfter) && chAfter !== 0 && !isNaN(chAfter)) {
          throwError(state, "unexpected content after document end marker");
        }
        skipSeparationSpace(state, true, -1);
      }
      return;
    }

    if (state.position < state.length - 1) {
      throwError(state, "end of the stream or a document separator is expected");
    }
  }

  function loadAll(input) {
    input = String(input);

    if (input.length !== 0) {
      // Strip BOM
      if (input.charCodeAt(0) === 0xFEFF) {
        input = input.slice(1);
      }
    }

    const state = new State(input);
    const nullpos = input.indexOf("\0");
    if (nullpos !== -1) {
      state.position = nullpos;
      throwError(state, "null byte is not allowed in input");
    }

    // Use 0 as string terminator. That significantly simplifies bounds check.
    state.input += "\0";

    while (state.input.charCodeAt(state.position) === 0x20 /* Space */) {
      state.lineIndent += 1;
      state.position += 1;
    }

    while (state.position < state.length) {
      readDocument(state);
    }

    return state.documents;
  }

  function coerceInput(input) {
    if (typeof input === "string") return input;
    if (input && typeof input === "object") {
      let bytes = null;
      if (input instanceof ArrayBuffer) bytes = new Uint8Array(input);
      else if (ArrayBuffer.isView(input)) bytes = new Uint8Array(input.buffer, input.byteOffset, input.byteLength);
      else if (typeof SharedArrayBuffer !== "undefined" && input instanceof SharedArrayBuffer) bytes = new Uint8Array(input);
      else if (typeof Blob !== "undefined" && input instanceof Blob) {
        if (input._u8 && ArrayBuffer.isView(input._u8)) bytes = new Uint8Array(input._u8.buffer, input._u8.byteOffset, input._u8.byteLength);
        else if (typeof input._s === "string") return input._s;
      }
      if (bytes !== null) {
        if (bytes.byteLength >= 2147483648) {
          const err = new RangeError("The value of \"input.byteLength\" is out of range. It must be <= 2147483647. Received " + bytes.byteLength);
          err.code = "ERR_OUT_OF_RANGE";
          throw err;
        }
        // trailing NULs (typed-array padding) end the stream
        let len = bytes.length;
        while (len > 0 && bytes[len - 1] === 0) len--;
        return new globalThis.TextDecoder().decode(bytes.subarray(0, len));
      }
    }
    return String(input);
  }

  function parse(input) {
    const text = coerceInput(input);
    if (text.length >= 2147483648) { const err = new RangeError("The value of \"input.byteLength\" is out of range. It must be <= 2147483647. Received " + text.length); err.code = "ERR_OUT_OF_RANGE"; throw err; }
    const docs = loadAll(text);
    if (docs.length === 0) return null;
    if (docs.length === 1) return docs[0];
    return docs;
  }

  // ---------------- stringify ----------------

  const BOOL_NULL_KEYWORDS = /^(true|True|TRUE|false|False|FALSE|null|Null|NULL|yes|Yes|YES|no|No|NO|on|On|ON|off|Off|OFF|y|Y|n|N|~)$/;
  const SPECIAL_FLOATS = /^[-+]?\.(inf|Inf|INF|nan|NaN|NAN)$/;
  // characters that must be escaped inside a double-quoted scalar
  const NEEDS_ESCAPE_RE = /[\x00-\x1F\x7F\x85\xA0\u2028\u2029"\\]/;

  // length of the longest leading number-like prefix (strtod-flavoured), or 0
  function numberPrefixLen(str) {
    let i = 0;
    const n = str.length;
    if (i < n && (str[i] === "+" || str[i] === "-")) i++;
    const afterSign = i;
    if (i + 1 < n && str[i] === "0" && (str[i + 1] === "x" || str[i + 1] === "X")) {
      let j = i + 2;
      while (j < n && /[0-9a-fA-F]/.test(str[j])) j++;
      if (j > i + 2) return j;
    }
    if (i + 1 < n && str[i] === "0" && (str[i + 1] === "o" || str[i + 1] === "O")) {
      let j = i + 2;
      while (j < n && /[0-7]/.test(str[j])) j++;
      if (j > i + 2) return j;
    }
    let digits = 0;
    while (i < n && str[i] >= "0" && str[i] <= "9") { i++; digits++; }
    if (i < n && str[i] === ".") {
      i++;
      while (i < n && str[i] >= "0" && str[i] <= "9") { i++; digits++; }
    }
    if (digits === 0) return 0;
    if (i < n && (str[i] === "e" || str[i] === "E")) {
      let j = i + 1;
      if (j < n && (str[j] === "+" || str[j] === "-")) j++;
      let ed = 0;
      while (j < n && str[j] >= "0" && str[j] <= "9") { j++; ed++; }
      if (ed > 0) i = j;
    }
    return i > afterSign - 0 && digits > 0 ? i : 0;
  }

  function isNumberLike(s) {
    if (SPECIAL_FLOATS.test(s)) return true;
    const p = numberPrefixLen(s);
    if (p === 0) {
      // `.`/`+`/`-` directly before a flow indicator confuses the scanner
      return /^[.+\-][,\[\]{}]/.test(s);
    }
    if (p === s.length) return true;
    const c = s[p];
    if (c === "+" || c === "-") return true;      // "1+5", "123-456"
    if (c === "," || c === "[" || c === "]" || c === "{" || c === "}") return true;
    return false;
  }

  function needsQuote(s) {
    if (s === "") return true;
    if (BOOL_NULL_KEYWORDS.test(s)) return true;
    if (isNumberLike(s)) return true;
    if (/^[?|<>!%@&*#`"'\-:,\[\]{}\s]/.test(s)) return true;   // leading indicator
    if (/[\s]$/.test(s)) return true;                            // trailing space
    if (/[,\[\]{}]/.test(s)) return true;                        // flow indicators anywhere
    if (/:(\s|$)/.test(s)) return true;                          // ": ", ":\t", trailing ":"
    if (/\s#/.test(s)) return true;                              // comment introducer
    if (/[\x00-\x1F\x7F\x85\xA0\u2028\u2029"]/.test(s)) return true;
    if (s.startsWith("...")) return true;                        // document end marker prefix
    if (s.startsWith("=")) return false;
    return false;
  }

  const ESCAPES = {
    0x00: "\\0", 0x07: "\\a", 0x08: "\\b", 0x09: "\\t", 0x0A: "\\n",
    0x0B: "\\v", 0x0C: "\\f", 0x0D: "\\r", 0x1B: "\\e", 0x22: "\\\"",
    0x5C: "\\\\", 0x85: "\\N", 0xA0: "\\_", 0x2028: "\\L", 0x2029: "\\P",
  };

  function quoteString(s) {
    let out = "\"";
    for (const chStr of s) {
      const c = chStr.codePointAt(0);
      const esc = ESCAPES[c];
      if (esc !== undefined) out += esc;
      else if (c < 0x20 || c === 0x7F) out += "\\x" + c.toString(16).padStart(2, "0");
      else out += chStr;
    }
    return out + "\"";
  }

  function emitString(s) {
    return needsQuote(s) ? quoteString(s) : s;
  }

  function emitNumber(v) {
    if (Number.isNaN(v)) return ".nan";
    if (v === Infinity) return ".inf";
    if (v === -Infinity) return "-.inf";
    if (Object.is(v, -0)) return "-0";
    return String(v);
  }

  function unbox(v) {
    if (v instanceof Number || v instanceof Boolean || v instanceof String) return v.valueOf();
    return v;
  }

  function isSkippable(v) {
    return v === undefined || typeof v === "function" || typeof v === "symbol";
  }

  function emitScalar(v) {
    if (v === null) return "null";
    switch (typeof v) {
      case "boolean": return v ? "true" : "false";
      case "number": return emitNumber(v);
      case "string": return emitString(v);
    }
    return "null";
  }

  // pass 1: find objects referenced more than once (or cyclically)
  function collectShared(value, seenOnce, shared) {
    const v = value;
    if (v === null || typeof v !== "object") return;
    if (v instanceof String || v instanceof Number || v instanceof Boolean) return;
    if (seenOnce.has(v)) { shared.add(v); return; }
    seenOnce.add(v);
    if (Array.isArray(v)) {
      for (let i = 0; i < v.length; i++) {
        if (!(i in v)) continue;
        const item = v[i];
        if (isSkippable(item)) continue;
        collectShared(item, seenOnce, shared);
      }
    } else if (!isMaskedPlatformObject(v)) {
      for (const k of ownKeysOf(v)) {
        const val = v[k];
        if (isSkippable(val)) continue;
        collectShared(val, seenOnce, shared);
      }
    }
  }

  function ownKeysOf(v) {
    const keys = Object.keys(v);
    const syms = Object.getOwnPropertySymbols(v);
    for (const sym of syms) {
      const d = Object.getOwnPropertyDescriptor(v, sym);
      if (d && d.enumerable) keys.push(sym);
    }
    return keys;
  }

  function keyText(k) {
    return typeof k === "symbol" ? (k.description || "") : k;
  }

  // platform wrapper objects serialize as their own enumerable props in bun;
  // mbun's URL shim keeps state in enumerable fields, so mask them
  function isMaskedPlatformObject(v) {
    return (typeof URL !== "undefined" && v instanceof URL) ||
           (typeof URLSearchParams !== "undefined" && v instanceof URLSearchParams);
  }

  const SAFE_ANCHOR = /^[A-Za-z0-9_][A-Za-z0-9_-]*$/;

  function Emitter(space) {
    this.space = space;             // null => flow mode; string => block indent unit
    this.anchorNames = new Map();   // object -> anchor name
    this.emittedAnchors = new Set();// objects whose anchor line was written
    this.usedNames = new Set();
    this.itemCounter = 0;
    this.valueCounter = 0;
    this.shared = new Set();
  }

  Emitter.prototype.pickName = function (base) {
    if (base !== null && !this.usedNames.has(base)) {
      this.usedNames.add(base);
      return base;
    }
    const stem = base === null ? "value" : base;
    let i = base === null ? this.valueCounter : 1;
    for (;;) {
      const cand = stem + i;
      if (!this.usedNames.has(cand)) {
        if (base === null) this.valueCounter = i + 1;
        this.usedNames.add(cand);
        return cand;
      }
      i++;
    }
  };

  Emitter.prototype.nameFor = function (v, hintKey, inArray, isRoot) {
    let name = this.anchorNames.get(v);
    if (name !== undefined) return name;
    if (isRoot) name = this.pickName("root");
    else if (inArray) {
      // item anchors use a dedicated counter
      for (;;) {
        const cand = "item" + this.itemCounter++;
        if (!this.usedNames.has(cand)) { this.usedNames.add(cand); name = cand; break; }
      }
    } else if (typeof hintKey === "string" && SAFE_ANCHOR.test(hintKey)) {
      name = this.pickName(hintKey);
    } else {
      name = this.pickName(null);
    }
    this.anchorNames.set(v, name);
    return name;
  };

  // ---- flow mode ----
  Emitter.prototype.flowNode = function (v, hintKey, inArray, isRoot) {
    v = typeof v === "object" && v !== null ? unboxOrSelf(v) : v;
    if (v === null || typeof v !== "object") return emitScalar(v);
    if (this.shared.has(v)) {
      const known = this.emittedAnchors.has(v);
      const name = this.nameFor(v, hintKey, inArray, isRoot);
      if (known) return "*" + name;
      this.emittedAnchors.add(v);
      return "&" + name + " " + this.flowContent(v);
    }
    return this.flowContent(v);
  };

  Emitter.prototype.flowContent = function (v) {
    if (Array.isArray(v)) {
      const parts = [];
      for (let i = 0; i < v.length; i++) {
        if (!(i in v)) continue;
        const item = v[i];
        if (isSkippable(item)) continue;
        parts.push(this.flowNode(item, null, true, false));
      }
      return "[" + parts.join(",") + "]";
    }
    const parts = [];
    if (!isMaskedPlatformObject(v)) {
      for (const k of ownKeysOf(v)) {
        const val = v[k];
        if (isSkippable(val)) continue;
        parts.push(emitString(keyText(k)) + ": " + this.flowNode(val, keyText(k), false, false));
      }
    }
    return "{" + parts.join(",") + "}";
  };

  function unboxOrSelf(v) {
    if (v instanceof String) return v.valueOf();
    return v;
  }

)JS";

}  // namespace mbun::jsc::builtins::detail
