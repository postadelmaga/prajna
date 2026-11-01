# ChatAI-Plasmoid
A range of chatbots as Plasmoid for your KDE Plasma desktop 

![Screenshot_20250130_204914](https://github.com/user-attachments/assets/0e72709b-3d10-430c-a24e-8a0511c05423)

## Lightweight web engine

This edition does **not** use Qt WebEngine. Loading Chromium inside
`plasmashell` costs memory even while the popup is closed, and a renderer
crash can take the whole shell down with it. Instead, pages are rendered by a
small **out-of-process WPE WebKit engine** (`chatai-webwpe`), based on the
Zurf engine protocol (`engine/engine_proto.h`):

```
plasmashell ─ ChatAI QML ─ WebSurface (QQuickItem, ~1 MB plugin)
                               │  frames: BGRA slot in shared memory (seqlock)
                               │  control: socketpair (navigate, input, events)
                               ▼
                         chatai-webwpe  ──  WPE WebKit (CPU rendering, no GPU)
```

- Nothing web-related is loaded into Plasma; if the engine crashes it is
  restarted and the panel keeps working.
- While the popup is closed the page is paused, and after a configurable
  time (default 15 min) the engine is stopped and **all** its memory is released.
  Cookies and logins stay on disk (`~/.local/share/chatai-plasmoid/<profile>`).
- WebKit runs as a single web process, without bfcache, WebGL or GPU contexts.
- Supported: file uploads, downloads, notifications, microphone/camera
  permissions, clipboard both ways (including pasting images), find in page,
  context menu, login popups (OAuth), HiDPI, IME/dead keys.
- Not supported: printing/saving pages as PDF (WPE has no print backend).

### Build and install

Requirements: Qt ≥ 6.6, KDE Frameworks/ECM 6, Plasma 6, `wpewebkit` ≥ 2.48
(the WPE Platform API), `libxkbcommon`, CMake, a C/C++ compiler.

```sh
# Arch/CachyOS: pacman -S wpewebkit extra-cmake-modules cmake
./install.sh                 # per-user, into ~/.local (no root needed)
# or system-wide:
cmake -B build -DCMAKE_INSTALL_PREFIX=/usr && cmake --build build && sudo cmake --install build
```

The engine and the QML plugin are installed **inside** the plasmoid package
(`contents/lib/chatai/`), so no QML import path setup is required. After an
upgrade restart Plasma (`systemctl --user restart plasma-plasmashell`).

Tests (optional): `cmake -B build -DBUILD_TESTING=ON && cmake --build build && ctest --test-dir build`.
`engine/tools/engine_probe.py` drives the engine without Qt (screenshots, JS, input).

## Contributors
<!-- readme: contributors -start -->
<table>
	<tbody>
		<tr>
            <td align="center">
                <a href="https://github.com/DenysMb">
                    <img src="https://avatars.githubusercontent.com/u/33737137?v=4" width="100;" alt="DenysMb"/>
                    <br />
                    <sub><b>Denys Madureira</b></sub>
                </a>
            </td>
            <td align="center">
                <a href="https://github.com/bigbruno">
                    <img src="https://avatars.githubusercontent.com/u/6098501?v=4" width="100;" alt="bigbruno"/>
                    <br />
                    <sub><b>Bruno Gonçalves</b></sub>
                </a>
            </td>
            <td align="center">
                <a href="https://github.com/JulWas797">
                    <img src="https://avatars.githubusercontent.com/u/51297298?v=4" width="100;" alt="JulWas797"/>
                    <br />
                    <sub><b>J797</b></sub>
                </a>
            </td>
            <td align="center">
                <a href="https://github.com/meowarex">
                    <img src="https://avatars.githubusercontent.com/u/90243579?v=4" width="100;" alt="meowarex"/>
                    <br />
                    <sub><b>Meow Meow</b></sub>
                </a>
            </td>
            <td align="center">
                <a href="https://github.com/vitor-dantas">
                    <img src="https://avatars.githubusercontent.com/u/45504386?v=4" width="100;" alt="vitor-dantas"/>
                    <br />
                    <sub><b>Vitor Dantas</b></sub>
                </a>
            </td>
            <td align="center">
                <a href="https://github.com/postadelmaga">
                    <img src="https://avatars.githubusercontent.com/u/2010800?v=4" width="100;" alt="postadelmaga"/>
                    <br />
                    <sub><b>postadelmaga</b></sub>
                </a>
            </td>
		</tr>
	<tbody>
</table>
<!-- readme: contributors -end -->

Colorful ChatGPT icon by [IconScout](https://iconscout.com/)

All other chat icons by [Icons8](https://icons8.com/)
