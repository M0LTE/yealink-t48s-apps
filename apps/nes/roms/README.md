# The games that come with it

Homebrew whose licence lets anyone pass it on. Each file is the author's own release, byte for byte; `LICENSES.txt` and `GPL-3.0.txt` travel with them in the package.

| File | Game | Author | Licence | Release | SHA-256 |
|---|---|---|---|---|---|
| `thwaite.nes` | Thwaite 0.04 | Damian Yerrick | GPL-3.0-or-later | [pinobatch/thwaite-nes v0.04](https://github.com/pinobatch/thwaite-nes/releases/tag/v0.04), commit `ccd27ccb` | `a2df24d9c9f72e56c2fdc4c703becc47a5700ad0158da8208247635ebeb3779c` |
| `croom.nes` | Concentration Room 0.02a | Damian Yerrick | GPL-3.0-or-later, and exact copies may be passed on without source | [pinobatch/croom-nes v0.02a](https://github.com/pinobatch/croom-nes/releases/tag/v0.02a), commit `209e6285` | `2ce17df1ad66a8a0533c0a8739f5b5ebe275c264924bbe350c42c5ac0394f20e` |

Both are NROM boards (mapper 0).

## Their source

The source each ROM was built from is at the tag named above, and a copy is kept here in `src/`, so that it stays available with the ROMs whatever happens upstream. Each is GitHub's archive of the tag, unchanged:

| File | Tag, commit | SHA-256 |
|---|---|---|
| [`src/thwaite-nes-0.04.tar.gz`](src/thwaite-nes-0.04.tar.gz) | v0.04, `ccd27ccbe3e201755b86c1b5f932b2e11ba74110` | `623abf3ede7a40b048a0e9ac9ec09b8dea035b63e4cbc9e2b2c70f35e3843bac` |
| [`src/croom-nes-0.02a.tar.gz`](src/croom-nes-0.02a.tar.gz) | v0.02a, `209e628564ab890d05c188e7c84f80217088aa5a` | `2de1bdefaa7f3174ca5d9ac1c87327714e04f816fd5939338d54df2a66c85ffa` |

They are not in the app's package; `LICENSES.txt`, which is, says where they are.

## Adding one

Check the licence text itself, not a summary of it: it must allow redistribution of the ROM. Add the file as released, a row here with where it came from and its checksum, its notice to `LICENSES.txt`, and for a copyleft licence its source archive to `src/`. No commercial ROMs, and nothing whose author has asked for it not to be used this way.
