#ifdef _WIN32
#include "windows.inc"
#else
#include "wayland.inc"
#endif
#include "battle.inc"

void bmain() {
    simulate_battle();
}