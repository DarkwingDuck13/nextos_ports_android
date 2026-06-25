# Default Terraria Players

This directory is copied by `run.sh` into `/storage/roms/terraria/Players`.

Rules:
- Keep every bundled player as a `.plr` file in this directory.
- `run.sh` copies missing `.plr` files on launch, but never overwrites an existing player save.
- To ship two or three ready players, add more validated `.plr` files here and commit them.
- Do not depend on the virtual keyboard path for first boot.

Current state:
- No bundled player is validated right now.
- First boot must create the character through the original Terraria menu flow.
- `run.sh` enables `TER_AUTONAME=1`, which fills the native name fields without opening a keyboard.
