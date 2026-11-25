# Contributing

New apps and fixes are welcome, written by hand or with an AI tool. The shared rules (licence, wording, writing, commit messages, AI-assisted work) are in the firmware's [CONTRIBUTING.md](https://github.com/M0LTE/yealink-t48s/blob/main/CONTRIBUTING.md); this page is what differs here. How to write an app is in the [README](README.md#writing-an-app).

## Before you start

- **A new app or a small fix:** open a pull request.
- **Something the firmware would have to change for** (a new permission, a protocol message, a device): open an issue in [M0LTE/yealink-t48s](https://github.com/M0LTE/yealink-t48s/issues) first.
- One app or one change per pull request.

## What a pull request needs

1. **`make all` and `make check` pass** with the published SDK (`sdk-latest`), as in CI. CI also runs them on your pull request.
2. **A simulator test** for the app: `tests/*.script`, with settings in `tests/APP.conf`. The script commands are in the firmware's [docs/shell.md](https://github.com/M0LTE/yealink-t48s/blob/main/docs/shell.md#the-simulator).
3. **Pictures** in `screenshots/`, from the simulator (`make shot`, or `shot` in a script). For a change to how an app looks, before and after in the pull request.
4. **The app's README** says what it does, its settings, and anything it fetches from the network.
5. **Tried on a phone** if you have one: say so, and on which model. A 5 V supply of 1.5 A or more; [start the firmware from a USB stick](https://github.com/M0LTE/yealink-t48s#start) and nothing is written to the phone.

## Apps

- One static ARM program, a `manifest.json` and a 96 x 96 `icon.ppm`. Start from [apps/hello](apps/hello).
- Colours from `t48_theme_now()`, drawn again on `T48_EV_THEME`; `t48_ui_*` for headers, buttons and rows. The look: [docs/design](https://github.com/M0LTE/yealink-t48s/blob/main/docs/design/README.md).
- A short press of X is back. Close network connections on PAUSE, draw everything again on RESUME.
- A manifest asks only for the permissions the app uses. Say `"screen": "any"` only if the app lays itself out for the T46S's 480x272 too, with a test for it.
- C as in `apps/hello`: four spaces, `-Wall -Wextra` clean, a comment at the top of each file saying what it is for.
- Text on the screen follows the firmware's [docs/style.md](https://github.com/M0LTE/yealink-t48s/blob/main/docs/style.md): terse, no exclamation marks, say it once, ASCII only.

## Licence

AGPL-3.0-or-later, as libt48 is, with an `SPDX-License-Identifier` line in every source file. Third-party code or data (games, fonts, sounds) only under a licence that lets it ship in the package, with its licence text beside it and a line in the README's Licence section; see [apps/nes/roms](apps/nes/roms/README.md) for how. No Yealink files, and nothing you may not pass on.

## Commit messages

`app: what changed`, as in `git log` (`nes: refuse a screen smaller than the NES picture`). Body and the AI trailer as in the firmware's [CONTRIBUTING.md](https://github.com/M0LTE/yealink-t48s/blob/main/CONTRIBUTING.md#commit-messages).
