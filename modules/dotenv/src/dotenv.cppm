// dotenv.cppm — mbun.dotenv: .env file parser subsystem aggregator.
//
// Thin aggregator that re-exports the dotenv subsystem's modules. Pure logic
// only — file I/O, process-env loading, S3/proxy/NODE_OPTIONS helpers and the
// JSC binding layer from bun's env_loader remain outside this pure-logic
// translation checkpoint. ref: bun src/dotenv/env_loader.rs.
export module mbun.dotenv;

export import mbun.dotenv.map;
export import mbun.dotenv.parser;
