#pragma once

// Crash reports for nova_cli: an access violation, a stack overflow, an
// uncaught C++ exception or abort() prints the exception and the faulting
// thread's call stack to stderr before the process ends with the exception
// code, instead of ending silently. Function names need debug information
// (the RelWithDebInfo configuration); without it frames read module+offset,
// which a .map file or dumpbin resolves.

namespace nova_cli {

void installCrashHandler();

// nova_cli is a UTF-8 process (nova_cli.manifest): the console prints its
// messages as UTF-8 too, so paths outside ASCII read as they are, and gets
// its code page back when the process ends.
void useUtf8Console();

// The calling thread's call stack on stderr: where a diagnostic came from
// (NOVA_CLI_ERROR_STACKS=1 prints it with every USD error).
void printCallStack();

} // namespace nova_cli
