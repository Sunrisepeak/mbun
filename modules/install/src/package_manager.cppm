// package_manager.cppm — mbun.install.package_manager
//
// Thin aggregator over the PackageManager wave-2 pure-logic port (bun
// src/install/PackageManager.rs is >2000 lines, so per project rule it is
// split into src/package_manager/ submodules):
//   * options        — Subcommand, WorkspaceFilter, LogLevel, Do/Enable flags,
//                      CommandLineArguments subset, Options::load
//   * update_request — `bun add/remove/... <pkg[@version]>` positional parsing
//   * enqueue        — the resolution queue / dependency-walk state machine
//
// The execution layers (network scheduling, thread pool, lifecycle scripts,
// directories/cache I/O) are documented seams in the submodules.
export module mbun.install.package_manager;

export import mbun.install.package_manager.options;
export import mbun.install.package_manager.update_request;
export import mbun.install.package_manager.enqueue;
