# Project Instructions

## Versioning
- After any code change to `RoamBro.ino`, update the header `Notes:` section.
- Use `.x.1` increments for normal small changes unless the user asks for a different versioning scheme.
- Use `.1` increments for largers changes.
- Use full version for major release or architecture, user should specify if it is a release change.
- Keep prior version note lines in place and add the new version note on the next line below.
- Keep each version note short and specific to the change.

## Behavior
- Preserve the cassette-player interaction model unless the user explicitly requests a behavior change.
- Do not introduce NFC auto-play behavior unless explicitly requested.
- Favor simple, mechanical tape-player behavior over smart or automatic behavior.

## Documentation
- Update `PLAYER_LOGIC.md` whenever player behavior changes.
- Keep documentation aligned with the current sketch behavior.

## Code Style
- Prefer focused, minimal changes over broad refactors.
- Keep debug logging behind a top-level flag.
- Avoid changing working behavior outside the scope of the request.

## Validation
- Compile with `arduino-cli compile -b rp2040:rp2040:waveshare_rp2350_zero RoamBro.ino` after code changes when feasible.
