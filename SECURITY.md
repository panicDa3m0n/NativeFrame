# Security policy

## What NativeFrame is, from a security point of view

NativeFrame converts a game's executable into a native program and runs it **directly on your CPU, with your user account's privileges**. It is not a sandbox and not an emulator: a converted program can do anything a normal program you start can do. Only convert and run games you own and obtained from a source you trust.

The system libraries also open files, network sockets, controllers and the GPU on behalf of the game.

## Supported versions

NativeFrame has no releases yet. Security fixes go to the `main` branch only.

## What to report

Please report privately anything that could let a crafted game file, network peer or input do more than the game itself should be able to do through NativeFrame's code, for example:

- memory corruption in the relinker, the loader or a system library reachable from game-controlled data (file formats, network packets, command buffers, shader code);
- path traversal or file access outside the directories a game is given;
- the browser converter (`demo/web`) reading or sending files it should not;
- secrets or personal data accidentally committed to the repository.

Problems that only affect a game's own behaviour (a crash, a wrong frame) are ordinary bugs: open a normal issue.

## How to report

Use GitHub's **private vulnerability reporting**: open the repository's **Security** tab and choose **Report a vulnerability**. Please include:

- the commit you tested;
- what an attacker controls and what they gain;
- steps or a minimal input that reproduce it, **built by you**: never attach game files, executables or dumps.

You will get an answer as soon as the maintainer can give one; this is a volunteer project without a guaranteed response time. Please give a reasonable time for a fix before disclosing publicly. Credit is given in the fix unless you ask otherwise.

If the problem is in code inherited unchanged from AnyPS5, it most likely affects AnyPS5 as well; we will coordinate with the upstream maintainers, or you can report it to them directly.
