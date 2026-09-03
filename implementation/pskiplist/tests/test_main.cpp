// Defines the entry point for doctest. Separated into a separate file to speed up incremental
// builds.
#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <cstdio>

// CTest (and CI generally) runs this binary with stdout piped, not attached to a TTY, so libc
// switches from line-buffered to fully-buffered stdio -- doctest's per-TEST_CASE progress lines
// then sit in an internal buffer instead of reaching the CI log until the buffer fills or the
// process exits normally. Forcing line buffering here means every TEST_CASE header reaches the
// log in real time, so a hang is still diagnosable from the log alone.
int main(int argc, char **argv) {
  std::setvbuf(stdout, nullptr, _IOLBF, BUFSIZ);
  doctest::Context context;
  context.applyCommandLine(argc, argv);
  const int result = context.run();
  if (context.shouldExit()) {
    return result;
  }
  return result;
}
