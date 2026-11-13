# Security

Mullion runs inside the game's own process and gives it no rights it did not have: a game can already run any code
as the user who starts it. A crash or a memory error that a game's Direct3D calls or shaders cause is therefore a
bug, and is reported as an [issue](https://github.com/dttdrv/mullion/issues/new/choose).

A security problem is something that harms a user who only installed and ran Mullion. For example:

- `install.sh` writing outside the Wine or prefix it was given, or running something it should not
- a release archive whose contents differ from what its source builds
- one process reaching another's memory or files through what Mullion shares between processes or keeps on disk

These are reported privately, through **Report a vulnerability** on the repository's
[Security](https://github.com/dttdrv/mullion/security) page. A report gets an answer within a week. Fixes go into
the main branch and the next release; older releases are not updated.
