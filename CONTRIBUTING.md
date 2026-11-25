# Contributing

Reports and changes are welcome.

**Reporting a problem:** use the issue form. Please give your Mac model, macOS version, sample rate, music app and buffer size. Read a saved report before attaching it, and leave out anything private.

**Sending a change:**
1. Read [docs/architecture.md](docs/architecture.md) and [docs/building.md](docs/building.md).
2. Keep one change per pull request, and add a test for it.
3. Run `make test`, and `make app-test` if you touched `app/`. Use no new dependencies.
4. Say what you tried on a real UA-101, with the rate and macOS version, and what you didn't.

**Safety rules**, because a mistake can be loud or erase someone's settings:
- Never send a volume above 0 dB.
- A new kind of control message needs evidence from a real unit, and a test that checks its exact bytes.
- Test programs stay silent by default; tones stay short and very quiet.
- Report measured results as they are. The clock limit in the tests is 100 ppm and stays.
- Don't copy Roland's code or files, or code under an incompatible licence.

By contributing you agree that your work is licensed under GPL-2.0-only.
