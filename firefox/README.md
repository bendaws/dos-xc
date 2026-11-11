# DOSfox
DOSfox is a build of [Firefox](https://www.firefox.com/en-US/) for DOS-XC. It is packaged as `dosfox` and preincluded in your DOS-XC iso.

It's not named "Firefox" because Firefox is trademarked, but they are the same source, just with some different branding.
## Processes and the sandbox

Web pages run in a separate content process: `DOSFOX.EXE` starts itself
again with `-contentproc`, using `dosxc_spawn_fds`, so the child gets only its
IPC socket and standard output. Once it is set up, the browser tells it to
sandbox itself (`dosxc_sandbox`). From then on it cannot write files, start
programs, open network sockets, read the keyboard or mouse, or use system
calls such as disks, drivers and accounts. Sound stays allowed, because
audio plays in the content process. There is one content process (no
Fission), and none is started ahead of time.

The preferences are in `files/dosxc-prefs.js`.
`browser.tabs.remote.autostart` set to `false` in a profile goes back to a
single process, and `security.sandbox.content.level` set to `0` leaves the
content process unsandboxed.

Fonts come from the package's `fonts` folder and from `\SYSTEM\FONTS` (the
Fonts program on the desktop installs them there).
