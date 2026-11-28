# Licence texts added to the source-built package

`tools/source-1.0.2/package.py` puts these files into `PPSA83600/licenses/` of a release built
from source. The official v1.0.2 package is missing them, or carries a wrong one. Each file is
verbatim, and `package.py` checks it against the SHA-256 below before using it, so a changed
copy, or one converted to CRLF, stops the packaging.

| File | Package path | Why | Where it came from | SHA-256 |
|---|---|---|---|---|
| `GPL-3.0.txt` | `licenses/GPL-3.0.txt` | `eboot.bin` as a whole is GPL-3.0 (it links wolfSSL and ps5-framegen) | <https://www.gnu.org/licenses/gpl-3.0.txt> | `3972dc9744f6499f0f9b2dbf76696f2ae7ad8af9b23dde66d6af86c9dfb36986` |
| `MINIUPNPC-BSD-3.txt` | `licenses/MINIUPNPC.txt`, replacing the official file | the official `licenses/MINIUPNPC.txt` (and the 1.0.0 drop's copy) is the 10-byte text `../LICENSE`. That is the target of the `miniupnpc/LICENSE` symlink, copied as a file. | `LICENSE` at the root of <https://github.com/miniupnp/miniupnp> commit `b91c025446d3ec3d9161cd56c91e200484af7906` (the netplay submodule the build uses; `miniupnpc/LICENSE` there is a symlink to it), read with `git show <commit>:LICENSE` | `ae63874b77b3d9c58ab99b2d116d83dca5f8c79d3f7d16d6bf7c23068f640fb5` |
| `MBEDTLS-3.6.7-LICENSE.txt` | `licenses/MBEDTLS-3.6.7-LICENSE.txt` | Mbed TLS 3.6.7 is linked (phone-camera TLS server). Its licence is Apache-2.0 OR GPL-2.0-or-later, and this build uses it under Apache-2.0. | `mbedtls-3.6.7/LICENSE` inside <https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/mbedtls-3.6.7.tar.bz2>. This is the archive `assemble.py` pins, SHA-256 `a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6`. | `9b405ef4c89342f5eae1dd828882f931747f71001cfba7d114801039b52ad09b` |
| `MPL-2.0.txt` | `licenses/CACERT-MPL-2.0.txt` | the Mozilla CA bundle (`third_party/cacert/cacert.pem`, from curl.se) is embedded in `eboot.bin` as data, and it is MPL-2.0 | <https://www.mozilla.org/media/MPL/2.0/index.txt> | `3f3d9e0024b1921b067d6f7f88deb4a60cbe7a78e76c64e3f1d7fc3b779b9d04` |
| `LAPY-LGPL-2.1.txt` | `licenses/LAPY-LGPL-2.1.txt` | fix 186: the elevation client in `eboot.bin` (`port/vendor/lapy`, adapted from prospero-win) and the separate helper `lapy.elf` that the package ships are LGPL-2.1-or-later (see `tools/source-1.0.2/lapy-daemon/NOTICE.md` and `manifest.json`) | this repository's `licenses/LAPY-LGPL-2.1.txt`, the GNU LGPL version 2.1 text as fix 186 added it (the same file, byte for byte) | `5749785c8bdefafcb5d798270ed0a967036fe2ca63dcedade1627565dfef81d2` |

`package.py` also writes the package's `LICENSE` from `GPL-3.0.txt`, as this repository's root
`LICENSE` is the GPL-3.0 text, and adds this repository's `licenses/PS5X360-MIT.txt`: a short
header, then the official package's `LICENSE` (the MIT License of the PS5X360 code), which
`package.py` checks word for word, so that notice stays with the code it covers.

The official package's own GPL-3.0 copies stay in the package unchanged.
`PS5-FRAMEGEN-GPL-3.txt`, `NATIVE-RUNTIME-GPL-3.txt` and `PS5-VULKAN-GPL-3.txt` are this text
with CRLF line ends. `WOLFSSL-GPL-3.txt` is wolfSSL's own copy, and its bytes differ from this
text.
