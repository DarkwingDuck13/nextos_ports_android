/*
 * ci/testport/src/main.c — CI integration test fixture
 *
 * This is NOT a real game port. It is a minimal C program used to verify the
 * GitHub Actions build pipeline end-to-end:
 *   - Compiles to a valid aarch64 ELF
 *   - Links against glibc <= 2.17
 *   - Has no forbidden DT_NEEDED entries (libSDL2, libopenal, libmali, libmpg123)
 *   - Contains no forbidden debug symbols (watchdog_thread, etc.)
 *   - Contains no unguarded GLSTATE/GLDRAW strings
 */

#include <stdlib.h>

int main(void)
{
    return EXIT_SUCCESS;
}
