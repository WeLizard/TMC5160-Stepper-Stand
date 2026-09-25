"""Browser regression tests with an in-memory API; NEVER connects to hardware.
Run: pip install playwright==1.58.0 && playwright install chromium
     python bench_v2/tests/ui_smoke.py
CHROMIUM_EXECUTABLE can select an existing browser.
UI_IN_MEMORY=1 uses a fully in-memory transport/storage for restricted runtimes;
CI uses a real HTTP origin (all requests intercepted) and browser localStorage. API and browser persistence
are simulated independently; none of these checks certify motor safety.
"""
import copy
import json
import os
from pathlib import Path
from playwright.sync_api import sync_playwright, expect

ROOT = Path(__file__).resolve().parents[1]
HTML = (ROOT / 'data/index.html').read_text()
KEY = 'stepper-stand.motor-profiles.v1'
CONFIG = dict(gear=1, rsense=.033, hold=.35, speed=10, accel=20, decel=20,
              low=-90, high=90, homeCoordinate=-95, homeSpeed=5, travel=220,
              backoff=3, sgMinSpeed=3, encoderTolerance=2, homeTimeout=120000,
              encoderZero=0, fullSteps=200, microsteps=16, current=300,
              homeCurrent=200, present=1, commissioned=1, hallMask=0,
              hardLimits=0, homeMethod=0, homeHall=0, sgTuned=0, reverse=0,
              encoder=0, frameConfirmed=0, zeroValid=0, encoderReverse=0,
              frameBits=12, stBits=12, mtBits=0, shift=0, grayCode=1, sgt=0)
AXIS = dict(state='OFF', enabled=False, referenced=True, fault='', position=0,
            target=0, speed=0, healthy=True, halls=0, hall_healthy=True,
            sg=37, driver=0, ramp=0, cycles=0, calculated_current_mA=300,
            measured=False, measured_low=0, measured_high=0,
            encoder_raw_hex='0', encoder_valid=False, encoder_count=0,
            encoder_degrees=0, config=CONFIG)


class Bench:
    """Only emulate command contracts needed by the UI; not motion physics."""
    def __init__(self):
        self.state = dict(boot=1, owner=0, lease_ms=1500, emergency=True,
                          physical_stop=False, revision=0, nvs=True,
                          max_loop_us=25000,
                          axes=[copy.deepcopy(AXIS), copy.deepcopy(AXIS)],
                          results=[], solenoid=dict(enabled=False, busy=False,
                          halls=0, switches=0, failures=0))
        self.commands = []
        self.offline = False
        self.delayed = []
        self.defer_config = False

    def finish(self, data, ident):
        op, axis = data['op'], data.get('axis', 0)
        result = 'OK'
        if op == 'config':
            if data['revision'] != self.state['revision']:
                result = 'STALE_CONFIGURATION'
            else:
                self.state['axes'][axis]['config'] = data['config']
                self.state['axes'][axis]['referenced'] = False
                self.state['revision'] += 1
        elif op == 'ack':
            self.state['emergency'] = False
        elif op == 'zero':
            self.state['axes'][axis]['referenced'] = True
        elif op == 'sol_enable':
            self.state['solenoid']['enabled'] = True
        elif op == 'sol_release':
            self.state['solenoid']['enabled'] = False
        elif op == 'move':
            self.state['axes'][axis]['target'] = data['value']
        self.state['results'].append({'id': ident, 'result': result})
        return ident

    def handle(self, url, body=None):
        if self.offline:
            return {'status': 503, 'data': {'error': 'OFFLINE'}}
        if url.endswith('/api/status'):
            return {'status': 200, 'data': self.state}
        data = json.loads(body or '{}')
        self.commands.append(data)
        op = data['op']
        if op == 'claim':
            self.state['owner'] = data['client']
        if op == 'estop':
            self.state['emergency'] = True
        if op in ('claim', 'heartbeat', 'stop', 'estop'):
            return {'status': 200, 'data': {'ok': True}}
        ident = len(self.commands)
        if op == 'config' and self.defer_config:
            self.delayed.append((data, ident))
        else:
            self.finish(data, ident)
        return {'status': 202, 'data': {'ok': True, 'accepted_id': ident}}

    def route(self, route):
        req = route.request
        if '/api/' not in req.url:
            route.fulfill(status=200, body=HTML, content_type='text/html')
        else:
            response = self.handle(req.url, req.post_data)
            route.fulfill(status=response['status'], json=response['data'])

    def last(self, op):
        return next(x for x in reversed(self.commands) if x['op'] == op)

    def count(self, op):
        return sum(c['op'] == op for c in self.commands)


def poll(page):
    page.wait_for_timeout(650)


def open_details(page, selector):
    page.locator(selector).evaluate('(e) => e.open = true')


def run():
    with sync_playwright() as p:
        launch = {'headless': True}
        if os.environ.get('CHROMIUM_EXECUTABLE'):
            launch['executable_path'] = os.environ['CHROMIUM_EXECUTABLE']
        browser = p.chromium.launch(**launch)
        context = browser.new_context(viewport={'width': 1440, 'height': 1080})
        bench = Bench()
        context.route('**/*', bench.route)
        errors = []
        in_memory = os.environ.get('UI_IN_MEMORY') == '1'

        def load_page(saved=None):
            page = context.new_page()
            page.on('pageerror', lambda e: errors.append(str(e)))
            page.on('dialog', lambda d: d.accept())
            if in_memory:
                page.expose_function('apiForTest', bench.handle)
                shim = """<script>
                const memory = new Map(Object.entries(SAVED_VALUES));
                Storage.prototype.getItem = key => memory.has(key)?memory.get(key):null;
                Storage.prototype.setItem = (key,value) => memory.set(key,String(value));
                Object.defineProperty(window,'localStorage',{value:Object.create(Storage.prototype)});
                window.fetch=async (url,options={})=>{
                    const r=await window.apiForTest(url,options.body||null);
                    return {ok:r.status<400,status:r.status,json:async()=>r.data};
                };
                window.__downloads=[];
                URL.createObjectURL=blob=>{blob.text().then(text=>__downloads.push(text));return 'blob:test';};
                HTMLAnchorElement.prototype.click=function(){};
                navigator.sendBeacon=()=>true;
                </script>""".replace('SAVED_VALUES', json.dumps(saved or {}))
                page.set_content(shim + HTML)
            else:
                page.goto('http://bench.test/')
            return page

        page = load_page()
        expect(page.locator('#c_gear')).to_have_value('1')
        # A spectator cannot move; stops remain reachable even without ownership.
        expect(page.locator('#move')).to_be_disabled()
        expect(page.locator('#estop')).to_be_enabled()
        page.click('#claim')
        page.click('#ack')
        expect(page.locator('#move')).to_be_enabled()
        count = bench.count('move')
        page.click('[data-angle="-45"]')
        assert bench.count('move') == count, 'Angle shortcut must not move a motor'
        page.click('#move')
        poll(page)
        assert bench.last('move')['value'] == -45
        page.select_option('#jogStep', '0.1')
        page.click('[data-jog="1"]')
        poll(page)
        assert bench.last('jog')['value'] == .1

        # Drafts and movement inputs stay with the selected axis, including fractions.
        page.fill('#c_gear', '0.333')
        page.fill('#c_current', '450')
        expect(page.locator('#move')).to_be_disabled()
        page.click('[data-axis="1"]')
        expect(page.locator('#c_current')).to_have_value('300')
        page.fill('#c_current', '550')
        page.fill('#angle', '30')
        page.click('[data-axis="0"]')
        expect(page.locator('#c_current')).to_have_value('450')
        expect(page.locator('#c_gear')).to_have_value('0.333')
        expect(page.locator('#angle')).to_have_value('-45')
        page.click('#save')
        poll(page)
        assert bench.last('config')['axis'] == 0
        assert bench.last('config')['config']['gear'] == .333
        page.click('[data-axis="1"]')
        expect(page.locator('#c_current')).to_have_value('550')
        # Revision caused by the OTHER axis is rebased without overwriting its draft.
        page.click('#save')
        poll(page)
        assert bench.last('config')['revision'] == 1
        assert bench.state['axes'][1]['config']['current'] == 550

        # Native numeric selectors and three Hall checkboxes replace magic numbers.
        open_details(page, '#config details[data-group*="Холлы"]')
        page.select_option('#c_homeMethod', '1')
        page.check('#hall_1')
        page.check('#hall_4')
        page.click('#save')
        poll(page)
        assert bench.last('config')['config']['hallMask'] == 5

        # Every NEMA template only prepares a draft; never changes gear/limits/sensors.
        base = copy.deepcopy(bench.state['axes'][1]['config'])
        before_config, before_move = bench.count('config'), bench.count('move')
        for index, current in enumerate([600, 700, 800, 2500]):
            page.select_option('#profileSelect', 'b:' + str(index))
            page.click('#profileLoad')
            expect(page.locator('#c_current')).to_have_value(str(current))
            expect(page.locator('#c_gear')).to_have_value(str(base['gear']))
            expect(page.locator('#c_commissioned')).not_to_be_checked()
            assert bench.count('config') == before_config
            assert bench.count('move') == before_move
        open_details(page, '#profileManager')
        page.fill('#profileName', 'Мой NEMA23')
        page.click('#profileSave')
        stored = json.loads(page.evaluate('(key) => localStorage.getItem(key)', KEY))
        assert stored[0]['name'] == 'Мой NEMA23'
        assert set(stored[0]['params']) == {'fullSteps', 'microsteps', 'current', 'hold', 'homeCurrent'}
        assert 'gear' not in stored[0]['params'] and 'commissioned' not in stored[0]['params']
        if in_memory:
            page.click('#profileExport')
            page.wait_for_function('window.__downloads.length > 0')
            exported = json.loads(page.evaluate('__downloads.at(-1)'))
        else:
            with page.expect_download() as event:
                page.click('#profileExport')
            exported = json.loads(Path(event.value.path()).read_text())
        assert exported['format'] == 'stepper-motor-profiles' and exported['version'] == 1
        # Import whitelists motor fields and ignores attempted calibration injection.
        imported = copy.deepcopy(exported)
        imported['profiles'][0]['name'] = '<img src=x onerror=alert(1)>'
        imported['profiles'][0]['params']['commissioned'] = 1
        page.set_input_files('#profileFile', {'name': 'motors.json', 'mimeType': 'application/json', 'buffer': json.dumps(imported).encode()})
        expect(page.locator('#message')).to_contain_text('Импортировано профилей: 1')
        page.select_option('#profileSelect', 'u:1')
        page.click('#profileLoad')
        expect(page.locator('#c_commissioned')).not_to_be_checked()
        assert page.locator('#profileSelect img').count() == 0
        bad = copy.deepcopy(exported)
        bad['profiles'][0]['params']['current'] = -30
        old_storage = page.evaluate('(key) => localStorage.getItem(key)', KEY)
        page.set_input_files('#profileFile', {'name': 'bad.json', 'mimeType': 'application/json', 'buffer': json.dumps(bad).encode()})
        expect(page.locator('#message')).to_contain_text('неверное поле')
        assert page.evaluate('(key) => localStorage.getItem(key)', KEY) == old_storage
        page.click('#profileDelete')
        assert len(json.loads(page.evaluate('(key) => localStorage.getItem(key)', KEY))) == 1
        if in_memory:
            saved = {KEY: page.evaluate('(key) => localStorage.getItem(key)', KEY)}
            page.close()
            page = load_page(saved)
        else:
            page.reload()
        expect(page.locator('#profileSelect option[value="u:0"]')).to_have_text('Мой NEMA23')
        page.click('#claim')
        page.click('#ack')
        poll(page)

        # Config acknowledgement binds to the originating axis, not the current tab.
        page.fill('#c_current', '460')
        bench.defer_config = True
        page.click('#save')
        poll(page)
        page.fill('#c_current', '461')
        page.click('[data-axis="1"]')
        page.fill('#c_current', '560')
        assert bench.delayed
        data, ident = bench.delayed.pop()
        bench.finish(data, ident)
        bench.defer_config = False
        poll(page)
        expect(page.locator('#c_current')).to_have_value('560')
        page.click('[data-axis="0"]')
        expect(page.locator('#c_current')).to_have_value('461')

        # External edits to the SAME axis block lost-update overwrites.
        page.fill('#c_current', '470')
        bench.state['axes'][0]['config']['current'] = 480
        bench.state['revision'] += 1
        expect(page.locator('#draftBadge')).to_have_text('Конфликт')
        expect(page.locator('#save')).to_be_disabled()
        page.click('#reload')
        expect(page.locator('#c_current')).to_have_value('480')

        # Manual zero, bounded autotest, and shortcuts preserve the existing API.
        page.click('#tab-home')
        page.click('#zero')
        poll(page)
        page.click('#tab-auto')
        page.click('#range90')
        page.click('#test')
        poll(page)
        assert bench.last('test')['low'] == -90
        assert bench.last('test')['high'] == 90
        count = bench.count('test')
        page.fill('#testHigh', '100')
        page.click('#test')
        expect(page.locator('#message')).to_contain_text('внутри рабочих границ')
        assert bench.count('test') == count
        page.click('#range45')
        # Solenoid commands retain their original route and bounded arguments.
        open_details(page, '#solPanel')
        page.click('#solEnable')
        expect(page.locator('[data-sol="0"]')).to_be_enabled()
        page.click('[data-sol="0"]')
        poll(page)
        assert bench.last('sol_pulse')['axis'] == 0
        assert bench.last('sol_pulse')['duration'] == 100
        page.click('#solTest')
        poll(page)
        assert bench.last('sol_test')['cycles'] == 10
        page.click('#solRelease')
        expect(page.locator('[data-sol="0"]')).to_be_disabled()
        page.locator('#solPanel').evaluate('(e) => e.open = false')
        # Local storage failures don't take down motion UI or emergency controls.
        page.evaluate('() => { Storage.prototype.setItem = () => {throw new Error("quota")}; }')
        open_details(page, '#profileManager')
        page.fill('#profileName', 'Not saved')
        page.click('#profileSave')
        expect(page.locator('#message')).to_contain_text('не сохранил библиотеку')
        expect(page.locator('#estop')).to_be_enabled()

        # Leaving stops once and requires explicit ownership reacquisition.
        beacons = []
        page.expose_function('captureBeacon', lambda data: beacons.append(data))
        page.evaluate('() => { navigator.sendBeacon = (url,blob) => {blob.text().then(window.captureBeacon);return true;}; }')
        page.evaluate('window.dispatchEvent(new Event("pagehide"))')
        poll(page)
        assert any(json.loads(body)['op'] == 'stop' for body in beacons)
        heartbeat_count = bench.count('heartbeat')
        poll(page)
        assert bench.count('heartbeat') == heartbeat_count
        expect(page.locator('#test')).to_be_disabled()
        page.click('#claim')
        expect(page.locator('#test')).to_be_enabled()

        # Offline stale telemetry is not announced as a confirmed physical stop.
        bench.offline = True
        expect(page.locator('#connection')).to_contain_text('Нет связи')
        expect(page.locator('#test')).to_be_disabled()
        expect(page.locator('#stop')).to_be_enabled()
        expect(page.locator('#estop')).to_be_enabled()
        bench.offline = False
        poll(page)
        expect(page.locator('#test')).to_be_disabled()
        page.click('#claim')
        expect(page.locator('#test')).to_be_enabled()
        page.click('#stop')
        page.click('#estop')
        assert bench.count('stop') and bench.count('estop')
        expect(page.locator('#test')).to_be_disabled()

        # Reviewable screenshots with a real DOM, desktop and phone layout.
        page.click('#ack')
        poll(page)
        page.click('#tab-manual')
        page.locator('#profileManager').evaluate('(e) => e.open = false')
        page.locator('#config details').evaluate_all('(els) => els.forEach(e=>e.open=false)')
        page.evaluate('scrollTo(0,0)')
        (ROOT / '.native').mkdir(exist_ok=True)
        for label, width, height in [('desktop', 1440, 1080), ('mobile', 390, 844), ('small', 320, 740)]:
            page.set_viewport_size({'width': width, 'height': height})
            assert page.evaluate('document.documentElement.scrollWidth <= innerWidth'), label
            box = page.locator('#estop').bounding_box()
            assert box and box['y'] >= 0 and box['y'] + box['height'] <= height
            page.screenshot(path=str(ROOT / f'.native/ui-{label}.png'), full_page=True)
        assert not errors, errors
        context.close()
        browser.close()
    print('UI regression passed: two-axis drafts, profiles/import/export, selectors, '
          'configuration revisions, command results, lease/offline/stop, 3 viewports; no JS errors')


if __name__ == '__main__':
    run()
