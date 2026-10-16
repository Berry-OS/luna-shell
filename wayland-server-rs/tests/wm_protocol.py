#!/usr/bin/env python3
"""WM regression test on a private software display; leaves the host VT alone.
Usage: python3 tests/wm_protocol.py ../target/debug/luna-compositor
"""
import array
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time


def ints(*values):
    return struct.pack('=' + 'i' * len(values), *values)


def string(value):
    if value is None:
        return ints(0)
    data = value.encode() + b'\0'
    return ints(len(data)) + data + b'\0' * (-len(data) % 4)


class Client:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX)
        self.sock.connect(str(path))
        self.sock.settimeout(3)
        self.next_id = 2
        self.events = []
        self.registry = self.new()
        self.send(1, 1, ints(self.registry))
        self.sync()
        self.globals = {}
        for obj, opcode, payload in self.events:
            if obj == self.registry and opcode == 0:
                name, size = struct.unpack_from('=II', payload)
                iface = payload[8:8 + size - 1].decode()
                version, = struct.unpack_from('=I', payload, 8 + (size + 3) // 4 * 4)
                self.globals[iface] = (name, version)
        self.events.clear()

    def new(self):
        result = self.next_id
        self.next_id += 1
        return result

    def send(self, obj, opcode, payload=b'', fd=None):
        message = struct.pack('=IHH', obj, opcode, 8 + len(payload)) + payload
        if fd is None:
            self.sock.sendall(message)
        else:
            sent = self.sock.sendmsg([message], [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array('i', [fd]))])
            assert sent == len(message)

    def read(self, count):
        result = b''
        while len(result) < count:
            chunk = self.sock.recv(count - len(result))
            assert chunk, 'compositor closed the connection'
            result += chunk
        return result

    def sync(self):
        callback = self.new()
        self.send(1, 0, ints(callback))
        while True:
            obj, opcode, size = struct.unpack('=IHH', self.read(8))
            payload = self.read(size - 8)
            assert not (obj == 1 and opcode == 0), ('protocol error', payload)
            if obj == callback and opcode == 0:
                return
            self.events.append((obj, opcode, payload))

    def bind(self, iface):
        name, version = self.globals[iface]
        obj = self.new()
        self.send(self.registry, 0, ints(name) + string(iface) + ints(version, obj))
        return obj

    def window(self):
        comp = self.bind('wl_compositor')
        wm = self.bind('xdg_wm_base')
        surf, xdg, top = self.new(), self.new(), self.new()
        self.send(comp, 0, ints(surf))
        self.send(wm, 2, ints(xdg, surf))
        self.send(xdg, 1, ints(top))
        self.send(top, 3, string('wm-regression'))
        self.sync()
        assert not any(obj == top and op == 0 for obj, op, _ in self.events), 'configure before initial commit'
        self.events.clear()
        return surf, xdg, top

    def initial(self, surf, xdg, top):
        self.send(surf, 6)
        self.sync()
        # sync may precede deferred WM actions: a second roundtrip drains them.
        self.sync()
        configs = [payload for obj, op, payload in self.events if obj == top and op == 0]
        assert configs, 'no initial configure'
        serials = [payload for obj, op, payload in self.events if obj == xdg and op == 0]
        assert serials
        self.send(xdg, 4, serials[-1])
        return struct.unpack_from('=ii', configs[0])

    def map(self, surf, width, height, color=0xff2030c0):
        shm = self.bind('wl_shm')
        fd = os.memfd_create('wm-test')
        try:
            os.ftruncate(fd, width * height * 4)
            os.write(fd, struct.pack('=I', color) * width * height)
            pool, buf = self.new(), self.new()
            self.send(shm, 0, ints(pool, width * height * 4), fd)
            self.send(pool, 0, ints(buf, 0, width, height, width * 4, 1))
            self.send(surf, 1, ints(buf, 0, 0))
            self.send(surf, 2, ints(0, 0, width, height))
            self.send(surf, 6)
            self.sync()
        finally:
            os.close(fd)


def run(binary):
    with tempfile.TemporaryDirectory(prefix='luna-wm-') as directory:
        root = Path(directory)
        env = dict(os.environ, XDG_RUNTIME_DIR=directory, XDG_STATE_HOME=str(root / 'state'), LUNA_TEST_INPUT='1', LUNA_NO_XWAYLAND='1')
        env.pop('LUNA_HEADLESS_GPU', None)
        env.pop('LUNA_WINDOW_RULES', None)
        log = open(root / 'compositor.log', 'w+')
        process = None

        def start():
            child = subprocess.Popen([binary, '--socket', 'test', '--backend', 'software', '--no-input', '--size', '1000x800', '--screenshot', str(root / 'frame.ppm')], env=env, stdout=log, stderr=log)
            for _ in range(300):
                if (root / 'test').is_socket():
                    return child
                assert child.poll() is None, 'compositor failed to start'
                time.sleep(.01)
            raise AssertionError('startup timeout')

        try:
            process = start()
            a = Client(root / 'test')
            surf, xdg, top = a.window()
            manager = a.bind('xdg_session_manager_v1')
            session = a.new()
            a.send(manager, 1, ints(session, 1) + string(None))
            a.sync()
            created = next(p for o, op, p in a.events if o == session and op == 0)
            size, = struct.unpack_from('=I', created)
            session_id = created[4:4 + size - 1].decode()
            handle = a.new()
            a.send(session, 2, ints(handle, top) + string('main'))
            assert a.initial(surf, xdg, top) == (0, 0)
            a.map(surf, 240, 160)
            placement = a.bind('luna_wm_v1')
            a.send(placement, 1, ints(surf, 120, 90))
            a.sync()
            # Switch a client cursor over unchanged window contents, without
            # moving the pointer or committing a new application frame.
            seat = a.bind('wl_seat')
            pointer = a.new()
            a.send(seat, 0, ints(pointer))
            a.sync()
            with socket.socket(socket.AF_UNIX) as control:
                control.connect(str(root / 'luna-shell/luna-shell.sock'))
                control.sendall(b'inject_motion 180 150\n')
            a.sync()
            a.sync()
            enters = [p for o, op, p in a.events if o == pointer and op == 0]
            assert enters, 'no pointer enter'
            serial, = struct.unpack_from('=I', enters[-1])
            comp = a.bind('wl_compositor')
            cursor = a.new()
            a.send(comp, 0, ints(cursor))

            def cursor_pixel(expected):
                for _ in range(100):
                    try:
                        data = (root / 'frame.ppm').read_bytes().split(b'\n', 3)[3]
                        offset = (150 * 1000 + 180) * 3
                        if data[offset:offset + 3] == expected:
                            return
                    except (FileNotFoundError, IndexError):
                        pass
                    time.sleep(.01)
                raise AssertionError('cursor-only change was not presented')

            a.send(pointer, 0, ints(serial, cursor, 0, 0))  # buffer arrives later
            a.map(cursor, 8, 8, 0xffe02020)
            cursor_pixel(bytes((0xe0, 0x20, 0x20)))
            a.map(cursor, 8, 8, 0xff20e020)  # same surface, different glyph
            cursor_pixel(bytes((0x20, 0xe0, 0x20)))
            other_cursor = a.new()
            a.send(comp, 0, ints(other_cursor))
            a.map(other_cursor, 8, 8, 0xff2020e0)
            a.send(pointer, 0, ints(serial, other_cursor, 0, 0))
            a.sync()
            cursor_pixel(bytes((0x20, 0x20, 0xe0)))
            duplicate = subprocess.run([binary, '--socket', 'test', '--backend', 'software', '--no-input'],
                                       env=env, stdout=log, stderr=log, timeout=5)
            assert duplicate.returncode != 0, 'second compositor stole the live socket'
            probe = Client(root / 'test')
            probe.sock.close()
            # A browser unmaps first, then destroys its session handle.
            a.send(surf, 1, ints(0, 0, 0))
            a.send(surf, 6)
            a.send(handle, 0)
            a.sync()
            a.sock.close()
            process.terminate()
            process.wait(timeout=5)
            process = start()

            b = Client(root / 'test')
            surf, xdg, top = b.window()
            manager = b.bind('xdg_session_manager_v1')
            session, handle = b.new(), b.new()
            b.send(manager, 1, ints(session, 1) + string(session_id))
            b.send(session, 3, ints(handle, top) + string('main'))
            b.sync()
            assert not any(o == handle and op == 0 for o, op, _ in b.events), 'restored before empty commit'
            assert b.initial(surf, xdg, top) == (240, 160), 'size not restored'
            order = [(o, op) for o, op, _ in b.events]
            assert order.index((handle, 0)) < order.index((top, 0)), 'restored must precede configure'
            b.map(surf, 240, 160)
            placement = b.bind('luna_wm_v1')
            b.send(placement, 2, ints(surf))
            b.sync()
            position = next(p for o, op, p in b.events if o == placement and op == 0)
            assert struct.unpack('=iii', position) == (surf, 120, 90), 'origin not restored'

            # Popup placement follows the parent's visible geometry, not its
            # buffer origin, and reposition must acknowledge the client's token.
            b.send(xdg, 3, ints(10, 12, 220, 136))
            comp, wm = b.bind('wl_compositor'), b.bind('xdg_wm_base')
            popup_surface, popup_xdg, popup, pos = b.new(), b.new(), b.new(), b.new()
            b.send(comp, 0, ints(popup_surface))
            b.send(wm, 2, ints(popup_xdg, popup_surface))
            b.send(wm, 1, ints(pos))
            b.send(pos, 1, ints(100, 60))
            b.send(pos, 2, ints(30, 20, 5, 5))
            b.send(pos, 3, ints(6))  # bottom-left anchor
            b.send(pos, 4, ints(8))  # bottom-right gravity
            b.send(popup_xdg, 2, ints(popup, xdg, pos))
            b.sync()
            cfg = next(p for o, op, p in b.events if o == popup and op == 0)
            assert struct.unpack('=iiii', cfg) == (30, 25, 100, 60)
            b.events.clear()
            b.send(pos, 6, ints(40, 35))
            b.send(popup, 2, ints(pos, 123))
            b.send(placement, 2, ints(popup_surface))
            b.sync()
            ordered = [(o, op) for o, op, _ in b.events]
            assert ordered.index((popup, 2)) < ordered.index((popup, 0)) < ordered.index((popup_xdg, 0))
            assert next(p for o, op, p in b.events if o == popup and op == 2) == ints(123)
            position = next(p for o, op, p in b.events if o == placement and op == 0)
            assert struct.unpack('=iii', position) == (popup_surface, 200, 162)
            b.send(popup, 0)
            b.send(popup_xdg, 0)
            b.send(popup_surface, 0)
            b.sync()

            seat = b.bind('wl_seat')
            keyboard = b.new()
            b.send(seat, 1, ints(keyboard))
            second, second_xdg, second_top = b.window()
            b.initial(second, second_xdg, second_top)
            b.map(second, 180, 100)
            b.sync()
            b.events.clear()
            b.send(second_top, 13)  # client-requested minimize
            b.sync()
            b.sync()
            leaves = [struct.unpack_from('=II', p)[1] for o, op, p in b.events if o == keyboard and op == 2]
            enters = [struct.unpack_from('=II', p)[1] for o, op, p in b.events if o == keyboard and op == 1]
            assert second in leaves and surf in enters, 'minimize did not hand keyboard focus to previous window'

            # A takeover must retire the old handles without deleting object IDs.
            c = Client(root / 'test')
            manager = c.bind('xdg_session_manager_v1')
            new_session = c.new()
            c.send(manager, 1, ints(new_session, 2) + string(session_id))
            c.sync()
            b.send(handle, 1, string('must-not-rename'))
            b.send(session, 1)  # remove on an inert session must do nothing
            b.sync()
            assert any(o == session and op == 2 for o, op, _ in b.events)
            assert 'main' in (root / 'state/luna-shell/xdg-sessions.tsv').read_text()
            b.sock.close()
            c.sock.close()
            print('PASS: socket ownership, cursor-only glyph changes, initial configure, restore order/size/origin, unmap persistence, popup reposition, minimize focus, session takeover')
        finally:
            if process and process.poll() is None:
                process.terminate()
                process.wait(timeout=5)
            log.seek(0)
            if sys.exc_info()[0]:
                print(log.read(), file=sys.stderr)
            log.close()


if __name__ == '__main__':
    run(str(Path(sys.argv[1] if len(sys.argv) > 1 else '../target/debug/luna-compositor').resolve()))
