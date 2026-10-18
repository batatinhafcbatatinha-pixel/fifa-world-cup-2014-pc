# Redistribution and provenance

This repository contains only independently written native components, function-boundary metadata, tests, tooling and documentation. It contains no original game assets, no game executable (XEX), no generated translation units, no disassembly, no SDK binaries and no compiled executable. Code paths of the private build that still rely on emulated Xbox 360 hardware (the SDK's Xenos GPU plugin and XMA contexts) are intentionally not published.

The port's own code is offered under the root MIT license, which grants no rights to the game's content. The ReXGlue SDK (BSD-3-Clause, derived from Xenia) is an external dependency; its license text is in `licenses/`.

The publication audit (`tools/audit_publication.py`, run on every push) uses an allowlist and rejects executables, images, audio, archives, game resources, generated code and oversized files. It is an engineering safeguard, not a legal guarantee for every jurisdiction or use. Do not submit game content or private build artifacts in pull requests or issues.
