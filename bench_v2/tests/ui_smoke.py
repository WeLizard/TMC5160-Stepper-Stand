"""Browser-only smoke tests with an in-memory API; no hardware claims.
Run: pip install playwright==1.58.0 && playwright install chromium
     python bench_v2/tests/ui_smoke.py
Optional CHROMIUM_EXECUTABLE path for an existing browser.
"""
import json
import os
from pathlib import Path
from playwright.sync_api import sync_playwright

root = Path(__file__).resolve().parents[1]
html = (root / 'data/index.html').read_text()
config = dict(gear=1, rsense=.033, hold=.35, speed=10, accel=20, decel=20,
              low=-90, high=90, homeCoordinate=-95, homeSpeed=5, travel=220,
              backoff=3, sgMinSpeed=3, encoderTolerance=2, homeTimeout=120000,
              encoderZero=0, fullSteps=200, microsteps=16, current=300,
              homeCurrent=200, present=0, commissioned=0, hallMask=0,
              hardLimits=0, homeMethod=0, homeHall=0, sgTuned=0, reverse=0,
              encoder=0, frameConfirmed=0, zeroValid=0, encoderReverse=0,
              frameBits=12, stBits=12, mtBits=0, shift=0, grayCode=1, sgt=0)
axis = dict(state='OFF', enabled=False, referenced=False, fault='', position=0,
            target=0, speed=0, healthy=False, halls=0, hall_healthy=False,
            sg=0, driver=0, ramp=0, cycles=0, calculated_current_mA=300,
            measured=False, measured_low=0, measured_high=0,
            encoder_raw_hex='0', encoder_valid=False, encoder_count=0,
            encoder_degrees=0, config=config)
state = dict(boot=1, owner=0, lease_ms=1500, emergency=True, physical_stop=False,
             revision=0, nvs=True, max_loop_us=25000,
             axes=[json.loads(json.dumps(axis)), json.loads(json.dumps(axis))],
             results=[], solenoid=dict(enabled=False, busy=False, halls=0, switches=0, failures=0))
commands = []
def mock_api(url, body=None):
    if url.endswith('/api/status'):
        return {'status': 200, 'data': state}
    data = json.loads(body)
    commands.append(data)
    if data['op'] == 'claim':
        state['owner'] = data['client']
    if data['op'] in ('claim', 'heartbeat', 'stop', 'estop'):
        return {'status': 200, 'data': {'ok': True}}
    i = len(commands)
    state['results'].append({'id': i, 'result': 'OK'})
    if data['op'] == 'config':
        state['axes'][data['axis']]['config'] = data['config']
        state['revision'] += 1
    return {'status': 202, 'data': {'ok': True, 'accepted_id': i}}

with sync_playwright() as p:
    kwargs = {'headless': True}
    if os.environ.get('CHROMIUM_EXECUTABLE'):
        kwargs['executable_path'] = os.environ['CHROMIUM_EXECUTABLE']
    browser = p.chromium.launch(**kwargs)
    page = browser.new_page(viewport={'width': 1440, 'height': 1080})
    errors = []
    page.on('pageerror', lambda e: errors.append(str(e)))
    page.expose_function('apiForTest', mock_api)
    # In-memory transport: this test never accesses a network or physical hardware.
    transport = """<script>
    window.fetch = async (url, options={}) => {
      const response = await window.apiForTest(url, options.body || null);
      return {ok: response.status < 400, status: response.status,
              json: async () => response.data};
    };
    </script>"""
    page.set_content(transport + html)
    page.wait_for_function("document.querySelector('#card0').textContent.includes('Мотор 1')")
    page.click('#claim')
    page.click('#ack')
    page.fill('#angle', '45')
    page.click('#move')
    assert next(x for x in reversed(commands) if x['op'] == 'move')['value'] == 45
    page.click('[data-axis="1"]')
    page.click('summary')
    page.check('#c_present')
    page.fill('#c_current', '450')
    page.click('#save')
    page.wait_for_timeout(700)
    saved = next(x for x in reversed(commands) if x['op'] == 'config')
    assert saved['axis'] == 1 and saved['config']['current'] == 450
    page.click('[data-axis="0"]')
    assert page.input_value('#c_current') == '300'
    page.click('#range90')
    page.click('#test')
    test = next(x for x in reversed(commands) if x['op'] == 'test')
    assert test['low'] == -90 and test['high'] == 90 and test['cycles'] == 10
    page.click('#stop')
    page.click('#estop')
    assert any(x['op'] == 'stop' for x in commands)
    assert any(x['op'] == 'estop' for x in commands)
    assert not errors, errors
    assert page.evaluate('document.documentElement.scrollWidth <= innerWidth')
    (root / '.native').mkdir(exist_ok=True)
    page.evaluate('scrollTo(0, 0)')
    page.screenshot(path=str(root / '.native/ui-desktop.png'), full_page=True)
    page.set_viewport_size({'width': 390, 'height': 844})
    assert page.evaluate('document.documentElement.scrollWidth <= innerWidth')
    page.screenshot(path=str(root / '.native/ui-mobile.png'), full_page=True)
    browser.close()
print('UI smoke passed: desktop/mobile, two axes, configuration, commands, stop; no JS errors')
