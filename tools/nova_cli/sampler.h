#pragma once

#include <memory>
#include <string>

// A sampling profiler for `nova_cli --profile` (Windows; elsewhere it records
// nothing). About every millisecond it suspends each other thread of the
// process in turn, copies its registers and stack, resumes it, and then
// walks the copy for its instruction pointer and return addresses (unwinding
// takes loader locks a suspended thread may hold). stop() resolves the addresses with the binaries' PDBs (build
// the RelWithDebInfo configuration for names) and writes the functions
// (self and inclusive) and the source lines where the samples fell.
class NovaSampler {
public:
    NovaSampler();
    ~NovaSampler();
    void start();
    // Stops sampling and writes the report to `path`; false if it cannot.
    bool stop(std::string const &path);

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};
