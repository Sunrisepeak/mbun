// patch.cppm — mbun.patch: thin aggregator over the patch subsystem parts.
//
// mbun.patch ports bun's `bun patch` file format handling from
// src/patch/lib.rs: parse a git-style unified-diff patch into a structured
// PatchFile (mbun.patch.parser + mbun.patch.types) and apply it to a tree
// through an injected FileSystem (mbun.patch.apply). All pure logic.
export module mbun.patch;

export import mbun.patch.types;
export import mbun.patch.parser;
export import mbun.patch.apply;
