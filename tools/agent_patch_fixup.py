from pathlib import Path

p = Path('tools/agent_patch_ui.py')
text = p.read_text()
replacements = {
    'static int g_stab_sound_idx = -1;': 'static int g_stab_sound_idx        = -1;',
    '    g_stab_sound_idx = luna_get_element_by_id("stab_sound");': '    g_stab_sound_idx    = luna_get_element_by_id("stab_sound");',
}
for old, new in replacements.items():
    if old not in text:
        raise SystemExit(f'fixup target missing: {old}')
    text = text.replace(old, new, 1)
p.write_text(text)
print('patched helper whitespace anchors')
