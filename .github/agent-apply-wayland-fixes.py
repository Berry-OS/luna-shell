#!/usr/bin/env python3
from pathlib import Path
import re

ROOT = Path.cwd()


def replace_once(path: Path, old: str, new: str, label: str) -> None:
    text = path.read_text()
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected exactly one match in {path}, found {count}")
    path.write_text(text.replace(old, new, 1))
    print(f"patched {label}: {path}")


server = ROOT / "wayland-server-rs/src/server.rs"
shell_ipc = ROOT / "wayland-server-rs/src/shell_ipc.rs"
shell_c = ROOT / "ui/luna-shell.c"
for path in (server, shell_ipc, shell_c):
    if not path.exists():
        raise SystemExit(f"missing {path}")

# 1) Focus: remember a transient xdg_toplevel parent before wl_surface.destroy.
old = """        let refocus = self.focused_client_fd == client.conn.fd && self.focused_surface_id == id;\n"""
new = old + """        // A focused transient dialog must hand focus back to its declared\n        // xdg_toplevel parent when it disappears.  Falling back to global\n        // stacking here can activate an unrelated application's window.\n        let transient_parent = if refocus {\n          Self::toplevel_parent_surface_in(client, id)\n        } else {\n          None\n        };\n"""
replace_once(server, old, new, "remember transient parent on destroy")

old = """        if refocus {\n          self.focus_topmost_visible((client.conn.fd, id));\n        }\n"""
new = """        if refocus {\n          if let Some(parent) = transient_parent {\n            let fd = client.conn.fd;\n            self.window_stack.retain(|&(f, s)| !(f == fd && s == parent));\n            self.window_stack.push((fd, parent));\n            // Request dispatch temporarily owns `client`; activate after it is\n            // reinserted into self.clients so keyboard enter can be delivered.\n            self.pending_activate = Some((fd, parent));\n            self.shell_state_dirty = true;\n          } else {\n            self.focus_topmost_visible((client.conn.fd, id));\n          }\n        }\n"""
replace_once(server, old, new, "restore transient parent on destroy")

# GTK commonly unmaps a dialog (attach NULL) before destroying the objects.
# Restore the parent at unmap time too, so focus never sits on an invisible
# child if the toolkit keeps the wl_surface alive for reuse.
old = """      } else if was_mapped && !mapped {\n        self.shell_state_dirty = true;\n      }\n    }\n\n    // Modeless shell dialogs (settings/about/…) use layer-shell with a\n"""
new = """      } else if was_mapped && !mapped {\n        if self.focused_client_fd == fd && self.focused_surface_id == id {\n          if let Some(parent) = Self::toplevel_parent_surface_in(client, id) {\n            self.window_stack.retain(|&(f, s)| !(f == fd && s == parent));\n            self.window_stack.push((fd, parent));\n            self.pending_activate = Some((fd, parent));\n          }\n        }\n        self.shell_state_dirty = true;\n      }\n    }\n\n    // Modeless shell dialogs (settings/about/…) use layer-shell with a\n"""
replace_once(server, old, new, "restore transient parent on unmap")

# 2) Window list: a mapped xdg_toplevel is a real window even when a client
# does not send title/app_id (or sends them after the first map).  pcmanfm's F4
# terminal can hit this path; dropping it permanently makes the window absent
# from Luna's window list.  Export a stable fallback and replace it naturally
# when metadata arrives later.
old = """        if title.is_empty() && app_id.is_empty() {\n          continue;\n        }\n        windows.push(WindowInfo {\n          id: window_id(fd, surface_id),\n          title: if title.is_empty() { app_id.clone() } else { title },\n          app_id,\n"""
new = """        let display_title = if !title.is_empty() {\n          title\n        } else if !app_id.is_empty() {\n          app_id.clone()\n        } else {\n          \"Window\".to_string()\n        };\n        windows.push(WindowInfo {\n          id: window_id(fd, surface_id),\n          title: display_title,\n          app_id,\n"""
replace_once(shell_ipc, old, new, "keep unnamed mapped toplevels in window list")

# 3) Menubar status labels: keep the icon, but do not repeat "Wi-Fi" / "AC".
old = """        const char* bat_icon = g_cached_bat >= 0 ? \"\\uf240\" : \"\\uf1e6\";\n        if (g_cached_bat >= 0) snprintf(buf, sizeof(buf), \"%d%%\", g_cached_bat);\n        else snprintf(buf, sizeof(buf), \"AC\");\n"""
new = """        const char* bat_icon = g_cached_bat >= 0 ? \"\\uf240\" : \"\\uf1e6\";\n        if (g_cached_bat >= 0) snprintf(buf, sizeof(buf), \"%d%%\", g_cached_bat);\n        else buf[0] = '\\0';\n"""
replace_once(shell_c, old, new, "hide AC text while keeping power icon")

old = """        if (text_would_change(idx, g_cached_net)) {\n            luna_set_text_paint_only(idx, g_cached_net);\n            dirty_mb |= repaint_matters(idx);\n        }\n"""
new = """        // The network glyph already distinguishes Wi-Fi/Ethernet.  Keep the\n        // compact menubar icon-only instead of repeating the connection name.\n        if (text_would_change(idx, \"\")) {\n            luna_set_text_paint_only(idx, \"\");\n            dirty_mb |= repaint_matters(idx);\n        }\n"""
replace_once(shell_c, old, new, "hide network text while keeping network icon")

# Remove the static Wi-Fi/AC text from every live skin layout as well so there
# is no startup flash before the first async status sample.  Battery percentage
# remains visible when a real battery is present.
wifi_re = re.compile(r'(<div id="mb_wifi"[^>]*>[^\n]*?</span>)\s*Wi-Fi(\s*</div>)')
ac_re = re.compile(r'(<div id="mb_bat"[^>]*>[^\n]*?</span>)\s*AC(\s*</div>)')
layout_changes = 0
for layout in sorted((ROOT / "skins").glob("*/layout.html")):
    text = layout.read_text()
    updated, n_wifi = wifi_re.subn(r"\1\2", text)
    updated, n_ac = ac_re.subn(r"\1\2", updated)
    if n_wifi or n_ac:
        layout.write_text(updated)
        layout_changes += n_wifi + n_ac
        print(f"patched status labels: {layout} (wifi={n_wifi}, ac={n_ac})")

if layout_changes == 0:
    raise SystemExit("status layouts: expected at least one Wi-Fi/AC label to remove")

# Guard the intended final architecture.
bin_server = (ROOT / "wayland-server-rs/src/bin/server.rs").read_text()
if "DriPacedBackend" in bin_server or "wait_for_scanout" in bin_server:
    raise SystemExit("synchronous DriPacedBackend is still present")
server_text = server.read_text()
for needle in ("in_flight_frame_done", "complete_presentation", "finish_or_defer_presentation_side_effects"):
    if needle not in server_text:
        raise SystemExit(f"async presentation state missing: {needle}")

print("all requested source fixes applied")
