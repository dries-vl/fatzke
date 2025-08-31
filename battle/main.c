#ifdef _WIN32
#include "windows.inc"
#else
#include "wayland.inc"
#endif
#include "battle.inc"

void main() {
    simulate_battle();
}