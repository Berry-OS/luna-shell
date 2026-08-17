#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[1]
readme = root / "README.md"
text = readme.read_text(encoding="utf-8")

start = "<!-- readme-screenshot-gallery:start -->"
end = "<!-- readme-screenshot-gallery:end -->"

# The skin gallery supersedes the old sample/demo image strip at the top.
for legacy in (
    "![Screenshot](ui/sample_01.png)\n",
    "![Screenshot](ui/sample_02.png)\n",
    "![Screenshot](screenshot.png)\n",
    "![Screenshot](luna-shell.png)\n",
):
    text = text.replace(legacy, "")

gallery = r'''<!-- readme-screenshot-gallery:start -->
## ✨ Luna Desktop skins

Every image below is a **real `luna-shell` framebuffer capture**. The shell is
built with its X11/EGL backend and run under Xvfb with the named skin; these are
not browser previews or recreated mockups. The current shell uses its own
KMS/Wayland/X11 hosts, so the capture path does **not** require GLFW.

### Luna — Default

<p align="center">
  <img src="docs/screenshots/luna-shell-skin-default.png" alt="Luna Desktop default skin" width="96%">
</p>

| Nocturne Atelier | Windows XP |
| --- | --- |
| ![Nocturne Atelier skin](docs/screenshots/luna-shell-skin-nocturne-atelier.png) | ![Windows XP skin](docs/screenshots/luna-shell-skin-windows-xp.png) |

| Windows 95 | Classic Mac OS |
| --- | --- |
| ![Windows 95 skin](docs/screenshots/luna-shell-skin-windows-95.png) | ![Classic Mac OS skin](docs/screenshots/luna-shell-skin-classic-mac.png) |

| BeOS | Amiga Workbench |
| --- | --- |
| ![BeOS skin](docs/screenshots/luna-shell-skin-beos.png) | ![Amiga Workbench skin](docs/screenshots/luna-shell-skin-amiga-workbench.png) |

Regenerate the full gallery with:

```bash
./tools/capture-shell-skins.sh
```
<!-- readme-screenshot-gallery:end -->'''

if start in text and end in text:
    before, tail = text.split(start, 1)
    _, after = tail.split(end, 1)
    text = before + gallery + after
else:
    sponsor = "[![Sponsor"
    idx = text.find(sponsor)
    if idx < 0:
        text = gallery + "\n\n" + text
    else:
        line_end = text.find("\n", idx)
        line_end = len(text) if line_end < 0 else line_end + 1
        text = text[:line_end] + "\n" + gallery + "\n\n" + text[line_end:]

# Avoid a large vertical gap between the gallery and the following rule.
text = text.replace(end + "\n\n\n---", end + "\n\n---")

readme.write_text(text, encoding="utf-8")
print(f"updated {readme}")
