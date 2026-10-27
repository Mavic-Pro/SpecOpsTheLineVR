"""Parser and checks for the view diagnostics of the mod (25/10, tester fixes).

Lines (see docs/TECHNICAL.md, "Tester VR fixes (25/10)"):
  VIEW:    one per eye per stereo tick ("viewlog N"), key=value
  BODY:    one per tick ("bodylog N [bones]")
  TURRET:  turret steps (2 Hz and on changes)
  TWOHAND: "twohandinfo"
  JITTER:  fake runtime summary every N frames ("runtime fake", "fake log N")

Library use (suites in the scratchpad import it):
    import viewcheck as vc
    rows = vc.parse_view(text)            # list of dicts, numbers parsed
    vc.rendered_vs_hmd(rows)             # per tick: angle between the drawn view and the headset pose
    vc.world_yaw(rows)                   # per tick: view yaw in the world (degrees)
    vc.pos_residual(rows)                # per tick: eye position not explained by pawn + fp + head offset
    vc.pos_report(rows)                  # residual spread, >2 Hz high-pass rms, head-to-view gain per axis
    vc.log_counts(text)                  # HITCH / DYNRES / FADE lines in the same text
    vc.parse_jitter(text)                # JITTER summaries

Command line:
    python tools/viewcheck.py <specopsvr.log> [--last N]   # summary of the last N VIEW ticks
"""
import math
import re
import sys

U2D = 360.0 / 65536.0
_NUM = re.compile(r'^-?\d+(\.\d+)?([eE][-+]?\d+)?$')


def _value(v):
    if v.startswith('(') and v.endswith(')'):
        parts = v[1:-1].split(',')
        if all(re.match(r'^-?\d+$', x) for x in parts):
            return tuple(int(x) for x in parts)
        return tuple(float(x) for x in parts)
    if _NUM.match(v):
        return float(v) if ('.' in v or 'e' in v or 'E' in v) else int(v)
    return v


def parse_kv(line, tag):
    """key=value pairs after 'TAG:'; values '(a,b,c)' become tuples."""
    i = line.find(tag + ':')
    if i < 0:
        return None
    out = {}
    for tok in line[i + len(tag) + 1:].split():
        if '=' in tok:
            k, v = tok.split('=', 1)
            out[k] = _value(v)
    return out


def parse_view(text):
    rows = []
    for line in text.splitlines():
        if 'VIEW: tick=' in line:
            d = parse_kv(line, 'VIEW')
            if d and 'tick' in d:
                rows.append(d)
    return rows


def eye_rows(rows, eye=0):
    return [r for r in rows if r.get('eye') == eye]


# --- rotations (3x3 lists, columns = axes like OpenVR: right, up, back) ---------

def mat_mul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)] for i in range(3)]


def mat_t(a):
    return [[a[j][i] for j in range(3)] for i in range(3)]


def angle_deg(a, b):
    """Angle of the relative rotation a^T b (robust for small angles)."""
    r = mat_mul(mat_t(a), b)
    c = (r[0][0] + r[1][1] + r[2][2] - 1.0) * 0.5
    sx, sy, sz = r[2][1] - r[1][2], r[0][2] - r[2][0], r[1][0] - r[0][1]
    s = 0.5 * math.sqrt(sx * sx + sy * sy + sz * sz)
    return math.degrees(math.atan2(s, c))


def quat_mat(q):
    w, x, y, z = q
    return [[1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)],
            [2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)],
            [2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)]]


def yaw_mat(yaw):
    """Rotation turning by 'yaw' radians to the right (mod convention) in OpenVR axes."""
    c, s = math.cos(-yaw), math.sin(-yaw)
    return [[c, 0.0, s], [0.0, 1.0, 0.0], [-s, 0.0, c]]


def ue_view_mat(rot):
    """FRotator (UE units) -> camera rotation in OpenVR axes (x right = UE Y, y up = UE Z, z back = -UE X)."""
    p, y, r = (v * U2D * math.pi / 180.0 for v in rot)
    cp, sp, cy, sy, cr, sr = math.cos(p), math.sin(p), math.cos(y), math.sin(y), math.cos(r), math.sin(r)
    f = (cp * cy, cp * sy, sp)
    rt = (sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, -sr * cp)
    u = (-(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp)
    conv = lambda v: (v[1], v[2], -v[0])  # noqa: E731
    cr_, cu_, cf_ = conv(rt), conv(u), conv(f)
    return [[cr_[i], cu_[i], -cf_[i]] for i in range(3)]


def rendered_vs_hmd(rows, eye=0):
    """Per tick (headset rows with hmd=1): angle in degrees between the view the mod drew, brought back to
    tracking space (yaw ref - body), and the headset pose it sampled. 0 = the view is the headset pose."""
    out = []
    for r in eye_rows(rows, eye):
        if r.get('hmd') != 1:
            continue
        drawn = mat_mul(yaw_mat(-(r['body'] * U2D * math.pi / 180.0 - r['ref'])), ue_view_mat(r['rot']))
        out.append((r['tick'], angle_deg(drawn, quat_mat(r['hmdq']))))
    return out


def world_yaw(rows, eye=0):
    """Per tick: view yaw in the world (degrees, unwrapped)."""
    out, last, acc = [], None, 0.0
    for r in eye_rows(rows, eye):
        y = (r['rot'][1] & 0xFFFF) * U2D
        if last is not None:
            d = (y - last + 180.0) % 360.0 - 180.0
            acc += d
        else:
            acc = y
        last = y
        out.append((r['tick'], acc))
    return out


# --- view position (b3 world scale, WI-2 criteria) ------------------------------

def _rot2(yaw_units, x, y):
    a = yaw_units * U2D * math.pi / 180.0
    c, s = math.cos(a), math.sin(a)
    return x * c - y * s, x * s + y * c


def pos_residual(rows, eye=0):
    """Per tick: (tick, t_ms, residual (forward, right, up), head (forward, right, up)) in units, body frame.

    residual = loc - eyeoff - (pawn + fpfwd * dir(fpyaw) + R(bodyofs) * ho), rotated into the body frame
    (bodyofs). With the model view (eye = Walker centre + first-person forward + head offset) the residual is
    constant: forward/right ~ 0, up = the eye height above the Pawn centre. Its changes are the view moving
    without the head or the Pawn moving it (wall pull, head collision t, eye-height clamps, crouch, eyesmooth,
    base snaps): bug 3 (world scale changes) and the WI-2 forward error / drift. 'head' = the head offset
    used (ho), for the gain fit."""
    out = []
    for r in eye_rows(rows, eye):
        if not all(k in r for k in ('loc', 'pawn', 'ho', 'bodyofs')):
            continue
        loc, pawn, ho = r['loc'], r['pawn'], r['ho']
        eo = r.get('eyeoff', (0.0, 0.0, 0.0))
        fwd = r.get('fpfwd', r.get('fp', 0.0))
        fx, fy = _rot2(r.get('fpyaw', r['bodyofs']), fwd, 0.0)
        hx, hy = _rot2(r['bodyofs'], ho[0], ho[1])
        d = [loc[0] - eo[0] - pawn[0] - fx - hx, loc[1] - eo[1] - pawn[1] - fy - hy, loc[2] - eo[2] - pawn[2] - ho[2]]
        bf, br = _rot2(-r['bodyofs'], d[0], d[1])
        out.append((r['tick'], r.get('t', 0.0), (bf, br, d[2]), tuple(ho)))
    return out


def highpass_rms(ts_ms, xs, fc=2.0, settle_s=0.5):
    """RMS of a first-order high-pass (fc Hz) of xs sampled at ts_ms, after 'settle_s' seconds."""
    if len(xs) < 3:
        return None
    rc = 1.0 / (2.0 * math.pi * fc)
    y, out = 0.0, []
    for i in range(1, len(xs)):
        dt = max(1e-4, (ts_ms[i] - ts_ms[i - 1]) * 0.001)
        a = rc / (rc + dt)
        y = a * (y + xs[i] - xs[i - 1])
        if ts_ms[i] - ts_ms[0] >= settle_s * 1000.0:
            out.append(y)
    return math.sqrt(sum(v * v for v in out) / len(out)) if out else None


def _gain(h, v):
    """Least-squares slope of v against h (None if the head did not move: spread under 0.5 units)."""
    n = len(h)
    if n < 3:
        return None
    mh, mv = sum(h) / n, sum(v) / n
    var = sum((x - mh) ** 2 for x in h)
    if var / n < 0.25:
        return None
    return sum((h[i] - mh) * (v[i] - mv) for i in range(n)) / var


def pos_report(rows, eye=0, fc=2.0):
    """Summary of pos_residual: per axis (forward, right, up) the median, spread (max - min), largest tick step,
    high-pass (fc Hz) rms, and the gain of the view against the head offset (1 = the view follows the head
    1:1: residual + head fitted against head). Unreal units (~2 cm)."""
    res = pos_residual(rows, eye)
    if not res:
        return {'n': 0}
    ts = [x[1] for x in res]
    out = {'n': len(res), 'seconds': round((ts[-1] - ts[0]) * 0.001, 2)}
    for k, name in enumerate(('forward', 'right', 'up')):
        v = [x[2][k] for x in res]
        h = [x[3][k] for x in res]
        sv = sorted(v)
        steps = [abs(v[i] - v[i - 1]) for i in range(1, len(v))]
        g = _gain(h, [v[i] + h[i] for i in range(len(v))])
        hp = highpass_rms(ts, v, fc)
        out[name] = {'median': round(sv[len(sv) // 2], 3), 'spread': round(sv[-1] - sv[0], 3),
                     'step_max': round(max(steps), 3) if steps else 0.0,
                     'hp_rms': round(hp, 4) if hp is not None else None,
                     'gain': round(g, 4) if g is not None else None}
    return out


def log_counts(text):
    """Stutter and resolution events in a log excerpt: HITCH lines (ticks over 20 ms) and the longest one,
    DYNRES scale changes, FADE (view jumps that blink)."""
    hitch = re.findall(r'HITCH: tick ([\d.]+) ms', text)
    return {'hitch': len(hitch), 'hitch_max_ms': max((float(x) for x in hitch), default=0.0),
            'dynres': len(re.findall(r'DYNRES: scene \d+% -> \d+%', text)), 'fade': text.count('FADE: view jumped')}


def stats(values):
    v = sorted(values)
    if not v:
        return {'n': 0}
    rms = math.sqrt(sum(x * x for x in v) / len(v))
    p99 = v[min(len(v) - 1, int(round(0.99 * (len(v) - 1))))]
    return {'n': len(v), 'rms': rms, 'p99': p99, 'max': v[-1], 'min': v[0]}


JITTER = re.compile(r'JITTER: (\d+) frames at (\d+) Hz \((\w+), display (\d+) ms, posed (\d+)\) \| mismatch declared vs drawn rms '
                    r'([\d.]+) p99 ([\d.]+) max ([\d.]+) deg \| world step rms ([\d.]+) p99 ([\d.]+) max ([\d.]+) deg, over 0.1 deg '
                    r'([\d.]+)% \| edge rms ([\d.]+) p99 ([\d.]+) max ([\d.]+) deg \| repeats (\d+) late (\d+) dropped (\d+) hitches '
                    r'(\d+) \| fades to black (\d+) \| ([\d.]+) frames/s \| no view (\d+) cinema (\d+)'
                    r'(?: \| render pose vs declared rot rms ([\d.]+) max ([\d.]+) deg pos rms ([\d.]+) max ([\d.]+) mm \((\d+)\))?')
_JKEYS = ['frames', 'hz', 'semantics', 'display_ms', 'posed', 'mismatch_rms', 'mismatch_p99', 'mismatch_max', 'step_rms',
          'step_p99', 'step_max', 'step_over_pct', 'edge_rms', 'edge_p99', 'edge_max', 'repeats', 'late', 'dropped', 'hitches',
          'fades', 'fps', 'noview', 'cinema', 'rp_rot_rms', 'rp_rot_max', 'rp_pos_rms', 'rp_pos_max', 'rp_n']


def parse_jitter(text):
    out = []
    for m in JITTER.finditer(text):
        d = {}
        for k, v in zip(_JKEYS, m.groups()):
            if v is None:
                continue  # older lines, without the render pose part
            d[k] = v if k == 'semantics' else (float(v) if '.' in v else int(v))
        out.append(d)
    return out


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return
    text = open(sys.argv[1], 'rb').read().decode('utf-8', 'replace')
    last = 600
    if '--last' in sys.argv:
        last = int(sys.argv[sys.argv.index('--last') + 1])
    rows = parse_view(text)
    ticks = sorted({r['tick'] for r in rows})[-last:]
    keep = set(ticks)
    rows = [r for r in rows if r['tick'] in keep]
    print('VIEW ticks:', len(ticks))
    rv = rendered_vs_hmd(rows)
    if rv:
        print('drawn view vs headset pose (deg):', stats([a for _, a in rv]))
    wy = world_yaw(rows)
    if len(wy) > 1:
        steps = [abs(wy[i][1] - wy[i - 1][1]) for i in range(1, len(wy))]
        print('view yaw step per tick (deg):', stats(steps))
    pr = pos_report(rows)
    if pr.get('n'):
        print('view position residual (units, body frame):', pr)
    print('log events:', log_counts(text))
    for j in parse_jitter(text)[-5:]:
        print('JITTER', j)


if __name__ == '__main__':
    main()
