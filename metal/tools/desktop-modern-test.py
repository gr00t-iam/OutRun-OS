#!/usr/bin/env python3
"""Exercise XP window controls through the real PS/2/QMP input path.

Run from metal/. Geometry constants are read from the same kernel source that
is built into the subject. An old ISO is a negative control: it must fail the
maximize assertion, not be classified as a successful legacy run.
"""
import argparse
import hashlib
import importlib.util
import json
import pathlib
import re
import subprocess
import tempfile
import time


def load_driver():
    path = pathlib.Path(__file__).with_name('desktop-ui-test.py')
    spec = importlib.util.spec_from_file_location('desktop_driver', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('iso')
    parser.add_argument('--log', default='/tmp/outrun-modern.log')
    args = parser.parse_args()
    driver = load_driver()
    driver.LOG = str(pathlib.Path(args.log).resolve())
    source = pathlib.Path('kernel/kernel64.c').read_text()

    def constant(name):
        match = re.search(r'^#define\s+' + re.escape(name) + r'\s+(\d+)',
                          source, re.MULTILINE)
        if not match:
            raise ValueError('missing kernel geometry constant: ' + name)
        return int(match.group(1))

    title = constant('WIN_TITLE_H')
    button_width = constant('WIMP_CLOSE_W')
    rail = constant('DESK_RAIL_W')
    taskbar = constant('WIN_TASKBAR_H')
    width, height = constant('WIN_MAX_W'), constant('WIN_MAX_H')
    iso = pathlib.Path(args.iso).resolve()
    digest = hashlib.md5(iso.read_bytes()).hexdigest()
    pathlib.Path(driver.LOG).parent.mkdir(parents=True, exist_ok=True)
    pathlib.Path(driver.LOG + '.image').write_text(str(iso) + ' md5=' + digest + '\n')
    print('image md5:', digest, flush=True)
    results = []

    def check(name, ok):
        results.append({'name': name, 'pass': bool(ok)})
        print(('PASS ' if ok else 'FAIL ') + name, flush=True)
        if not ok:
            raise AssertionError(name)

    def mark():
        return driver.size_of(driver.LOG)

    def action(name, since, expected=None):
        check('real input emitted ' + name,
              driver.wait_for('[wm] ' + name + ' id=0 ', 30, since))
        text = driver.read_log(since).decode('utf-8', 'replace')
        found = re.search(r'\[wm\] ' + name + r' id=0 x=(\d+) y=(\d+) w=(\d+) h=(\d+)', text)
        check(name + ' reports geometry', found is not None)
        geometry = tuple(map(int, found.groups()))
        if expected is not None:
            check(name + ' applies expected geometry', geometry == expected)
        return geometry

    with tempfile.TemporaryDirectory(prefix='outrun-modern-') as tmp:
        qmp_path = str(pathlib.Path(tmp) / 'qmp')
        proc = subprocess.Popen([
            'qemu-system-x86_64', '-accel', 'tcg', '-cdrom', str(iso),
            '-m', '512M', '-no-reboot', '-vga', 'none', '-device', 'virtio-vga',
            '-display', 'none', '-serial', 'file:' + driver.LOG,
            '-qmp', 'unix:' + qmp_path + ',server,nowait'])
        try:
            qmp = driver.Qmp(qmp_path)
            check('desktop loop ready', driver.wait_for('[desktop] logical desktop', 420))
            check('initial editor running', driver.wait_for("launched 'VAULT PAD'", 90))
            check('initial window published', driver.wait_for('1 window(s)', 90))
            geometry = (60, 40, width, height)

            def position(x, y):
                qmp.move_rel(-2000, -2000)
                qmp.move_rel(x, y)
                time.sleep(0.4)

            def button(down):
                qmp.cmd('input-send-event', events=[
                    {'type': 'btn', 'data': {'down': down, 'button': 'left'}}])
                time.sleep(0.18)

            def control_point(control):
                x, y, w, h = geometry
                return (x + w - 3 - button_width - (control - 1) * (button_width + 3)
                        + button_width // 2, y + title // 2)

            qmp.screenshot(driver.LOG + '.initial.ppm')
            start = mark()
            qmp.click(*control_point(2))
            geometry = action('maximize', start, (rail, 0, 1024 - rail, 768 - taskbar))
            qmp.screenshot(driver.LOG + '.maximized.ppm')
            start = mark()
            qmp.click(*control_point(2))
            geometry = action('restore', start, (60, 40, width, height))

            # A press that leaves the close button must not close the window.
            position(*control_point(1))
            button(True)
            qmp.move_rel(-100, 50)
            button(False)
            start = mark()
            check('cancelled close keeps editor alive', driver.wait_for('1 window(s)', 30, start))

            # Move, then resize through the edges, not internal helper calls.
            position(geometry[0] + 80, geometry[1] + title // 2)
            start = mark()
            button(True)
            qmp.move_rel(50, 45)
            button(False)
            geometry = action('drag', start, (110, 85, width, height))
            position(geometry[0] + geometry[2] - 1, geometry[1] + geometry[3] - 1)
            start = mark()
            button(True)
            qmp.move_rel(40, 30)
            button(False)
            geometry = action('resize', start, (110, 85, width + 40, height + 30))
            qmp.screenshot(driver.LOG + '.resized.ppm')

            # Real double-click: pointer stays put; only buttons are cycled.
            position(geometry[0] + 80, geometry[1] + title // 2)
            start = mark()
            button(True); button(False); button(True); button(False)
            restored = geometry
            geometry = action('maximize', start, (rail, 0, 1024 - rail, 768 - taskbar))
            start = mark()
            qmp.click(*control_point(2))
            geometry = action('restore', start, restored)

            start = mark()
            qmp.click(*control_point(3))
            action('minimize', start, geometry)
            # The first taskbar chip restores slot zero.
            qmp.click(20, 768 - taskbar // 2)
            start = mark()
            qmp.click(*control_point(2))
            geometry = action('maximize', start, (rail, 0, 1024 - rail, 768 - taskbar))
            start = mark()
            qmp.click(*control_point(1))
            action('close', start)
            check('closing editor releases window', driver.wait_for('0 window(s)', 45, start))
            text = driver.read_log().decode('utf-8', 'replace')
            check('no panic or lock rank fault', not any(s in text for s in
                  ('PANIC', 'rank violation', 'UNDERFLOW', 'page fault')))
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=15)
            except subprocess.TimeoutExpired:
                proc.kill(); proc.wait()
            pathlib.Path(driver.LOG + '.checks.json').write_text(json.dumps({
                'image_md5': digest, 'checks': results}, indent=2) + '\n')
    print('Modern window input checks passed:', len(results), flush=True)


if __name__ == '__main__':
    main()
