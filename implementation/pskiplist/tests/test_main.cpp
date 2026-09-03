#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <cstdio>

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
