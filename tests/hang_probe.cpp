// hang_probe: a test that runs past its budget. meson.build registers it
// with every other test and expects it to fail, so it fails the suite if
// it ever returns: if the tests stop being run under tests/watchdog.cpp,
// it exits 0 after a minute instead of being killed after a second.
#include <chrono>
#include <thread>

int main() {
  std::this_thread::sleep_for(std::chrono::minutes(1));
  return 0;
}
