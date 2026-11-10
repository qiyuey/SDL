# Personal fork workflow

- Personal fork: https://github.com/qiyuey/SDL
- Development and default branch: `main`, tracking `origin/main`.
- `origin`: personal fork; default push destination.
- `upstream`: original project; integration source `upstream/main`.

The November 2026 cleanup consolidates the gamepad extensions in one commit on
upstream. Preserve the frequency rumble API, Switch HD rumble output and Windows
XInput fallback when integrating newer upstream changes. Use focused commits
for subsequent changes. Publish to the personal fork with explicit authorization
and preserve local recovery refs.

Build Windows x64 Release with `SDL_HIDAPI_LIBUSB=ON`. The codec and fake USB
transport checks run through CTest with `SDL_TESTS=ON`. The optional
`testlinearhaptics` tool exercises a real controller on manual input.
