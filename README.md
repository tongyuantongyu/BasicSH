# BasicSH
A basic shell that can run execute commands

BasicSH can use busybox to provide portable coreutils ability. To utilize this, place busybox executable to ./.runtime/busybox, and BasicSH will automatically check available commands and redirect them to busybox, instead of executing the version in PATH.
