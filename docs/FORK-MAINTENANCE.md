# Personal fork workflow

This repository switched from a single amended local FIX to ordinary feature commits on 2026-11-05. The historical FIX remains intact as the initial customization snapshot; its old commit-message maintenance instructions are historical, not the current policy.

- Personal fork: https://github.com/qiyuey/SDL
- Development and default branch: `main`, tracking `origin/main`.
- `origin`: personal fork; default push destination.
- `upstream`: original project; integration source `upstream/main`.
- Upstream baseline at migration: `133153507fbef3041fbcb394262af62f74da5912`. Migration does not update the game/library code to a newer upstream revision.

Make focused new commits for subsequent changes. Do not automatically amend the historical FIX, squash the entire customization history, or force-push published branches. When an upstream update is requested, fetch and review the original project and personal fork, preserve work in progress, then integrate into `main` with ordinary merge history. Resolve conflicts by behavior, retaining controller frequency support and local improvements. Do not push directly to the original project. Local legacy branches and backup refs are retained for recovery, not uploaded as extra development branches.

Do not run the single-FIX workflow for this repository unless the user explicitly requests switching back. The local singleFixUpstream markers were removed. Fork publication is separate from updating upstream code; never interpret a save-only request as permission to publish.

## Compatible code snapshot

The pre-migration tested pair is:

- SavageWindRecomp: `7c83b795da48716ef2a71147e9209dc8cf37205e`
- SDL3: `75242f7d66148f555ac756fd08ef14d4035e8bbd`

The migration commits change documentation and repository workflow only. Windows uses the custom SDL3 x64 Release build with libusb support, not a generic SDL DLL. Record both repository revisions and relevant build options when publishing future binaries. Hardware frequency/response mapping remains experimental; software checks do not establish physical calibration.

Keep game dumps, firmware, keys, saves, generated game code, extracted resources, derived catalogs, local logs and build artifacts outside version control. Only the independently authored tools and documentation are tracked.
