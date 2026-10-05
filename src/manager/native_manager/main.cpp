// Payload-free manager, sharing the package UI, CLI and business entry.
// The parent package keeps its extracted files alive throughout the handoff.
#define INSTALLER_PERSISTENT_MANAGER
#ifdef MANAGER_TEST_DRIVER
#define INSTALLER_TEST_DRIVER
#endif
#include "../native_installer/main.cpp"

