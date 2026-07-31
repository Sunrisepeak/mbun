// mbun.platform aggregate interface.
export module mbun.platform;

export import mbun.platform.capability;
export import mbun.platform.error;
export import mbun.platform.path;
export import mbun.platform.syscall;
// Implementations behind the seams above. These are the modules that are
// allowed to include OS headers; everything else in mbun consumes them.
export import mbun.platform.posix_backend;
export import mbun.platform.pty;
