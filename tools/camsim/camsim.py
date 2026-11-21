"""Offline camera tester for Blades of Steel (scroll.log -> 240 pictures/s).

Feeds the game's camera (per-frame dx from scroll.log) to a smoother and
samples the picture's camera 4 times a frame (240 Hz). Reports, per design:
  acc_max / acc_rms : largest / typical change of speed per frame (px/frame^2)
                      at picture resolution: how hard the view lurches
  flips/s           : speed reversals per second while moving (wobble)
  steps/s           : picture-to-picture speed jumps > 0.25 px/frame (visible jolts)
  lag_mean/lag_max  : how far the view trails the game's camera (px)
Usage: python3 camsim.py scroll.log
"""
import sys, math

def segments(path):
    """Runs of gameplay frames (dx), split at 'off' and cuts (|dx| > 16)."""
    segs, cur = [], []
    for line in open(path):
        if line.startswith('#'): continue
        f = line.split()
        if len(f) < 3 or f[1] == 'off':
            if cur: segs.append(cur); cur = []
            continue
        dx = int(f[2])
        if abs(dx) > 16:
            if cur: segs.append(cur); cur = []
            continue
        cur.append(dx)
    if cur: segs.append(cur)
    return segs

# ---- designs: each returns per-frame state list [(x, v)] at frame ends and a
# sampler(i, p) giving the picture camera at fraction p of frame i ----

def current(dxs, a):
    """bos_extras.c cam_frame: distance-braking chaser, linear between frames."""
    cam = x = v = 0.0; xs = [0.0]
    for dx in dxs:
        cam += dx
        e = cam - x; d = abs(e)
        vd = math.sqrt(a * a / 4 + 2 * a * d) - a / 2
        if e < 0: vd = -vd
        dv = vd - v
        v += max(-a, min(a, dv))
        if (e > 0 and v > e) or (e < 0 and v < e): v = e
        x += v
        e = cam - x
        if e > 96: x = cam - 96
        if e < -96: x = cam + 96
        xs.append(x)
    return lambda i, p: xs[i] + (xs[i + 1] - xs[i]) * p

def spring(dxs, T, N, sub=8):
    """Critically damped follower with feed-forward of the game's average
    speed over N frames; integrated in sub-steps, so the picture camera is
    a continuous curve (sampled exactly at each picture)."""
    cam = 0.0; hist = []; x = v = 0.0
    w = 1.0 / T
    pts = [[0.0]]
    for dx in dxs:
        cam += dx
        hist.append(dx); hist = hist[-N:]
        vg = sum(hist) / len(hist)
        row = []
        for s in range(sub):
            h = 1.0 / sub
            acc = w * w * (cam - x) + 2 * w * (vg - v)
            v += acc * h; x += v * h
            row.append(x)
        pts[-1] += []  # keep
        pts.append(row)
    flat = [0.0]
    for r in pts[1:]: flat += r
    def at(i, p):
        k = i * sub + p * sub
        k0 = int(k); f = k - k0
        if k0 + 1 >= len(flat): return flat[-1]
        return flat[k0] + (flat[k0 + 1] - flat[k0]) * f
    return at

def avg_spring(dxs, N, T, sub=8):
    """Moving average of the game's camera over N frames (removes its steps
    and stutters), then a critically damped follower (no feed-forward)."""
    cam = 0.0; hist = []; x = v = 0.0; w = 1.0 / T
    flat = [0.0]
    for dx in dxs:
        cam += dx
        hist.append(cam); hist = hist[-N:]
        tgt = sum(hist) / len(hist)
        for s in range(sub):
            h = 1.0 / sub
            acc = w * w * (tgt - x) - 2 * w * v
            v += acc * h; x += v * h
            flat.append(x)
    def at(i, p):
        k = i * sub + p * sub; k0 = int(k); f = k - k0
        if k0 + 1 >= len(flat): return flat[-1]
        return flat[k0] + (flat[k0 + 1] - flat[k0]) * f
    return at

def measure(segs, make, pics=4):
    accs = []; flips = steps = 0; lags = []; moving = 0; frames = 0
    for dxs in segs:
        if len(dxs) < 30: continue
        at = make(dxs)
        cam = [0]
        for d in dxs: cam.append(cam[-1] + d)
        xs = [at(i, k / pics) for i in range(len(dxs)) for k in range(pics)]
        vs = [(xs[j + 1] - xs[j]) * pics for j in range(len(xs) - 1)]       # px/frame
        for j in range(1, len(vs)):
            dv = vs[j] - vs[j - 1]
            accs.append(abs(dv) * pics)                                    # px/frame^2
            if abs(dv) > 0.25: steps += 1
            if abs(vs[j]) > 0.3 and abs(vs[j - 1]) > 0.3 and (vs[j] > 0) != (vs[j - 1] > 0): flips += 1
        for i in range(len(dxs)):
            lags.append(abs(cam[i] - at(i, 0)))
        frames += len(dxs)
    secs = frames / 60.0
    accs.sort()
    return dict(acc_max=accs[int(len(accs) * 0.999)], acc_rms=math.sqrt(sum(a * a for a in accs) / len(accs)),
                flips=flips / secs, steps=steps / secs,
                lag_mean=sum(lags) / len(lags), lag_max=max(lags))

if __name__ == '__main__':
    segs = segments(sys.argv[1] if len(sys.argv) > 1 else 'scroll.log')
    designs = [
        ('game camera (no smoothing)', lambda d: (lambda c: (lambda i, p: c[i] + (c[i + 1] - c[i]) * p))([0] + [sum(d[:k + 1]) for k in range(len(d))])),
        ('current Quick   (a=1.6)', lambda d: current(d, 1.6)),
        ('current Gentle  (a=1.0)', lambda d: current(d, 1.0)),
        ('current Softest (a=0.8)', lambda d: current(d, 0.8)),
    ]
    for T in (4, 6, 8, 10):
        for N in (8, 16):
            designs.append(('spring T=%d ff N=%d' % (T, N), lambda d, T=T, N=N: spring(d, T, N)))
    for N in (8, 12, 16):
        for T in (3, 5, 8):
            designs.append(('avg N=%d + spring T=%d' % (N, T), lambda d, N=N, T=T: avg_spring(d, N, T)))
    print('%-28s %8s %8s %8s %8s %8s %8s' % ('design', 'acc99.9', 'acc_rms', 'flips/s', 'jolts/s', 'lag_avg', 'lag_max'))
    for name, mk in designs:
        r = measure(segs, mk)
        print('%-28s %8.3f %8.3f %8.2f %8.2f %8.1f %8.1f' % (name, r['acc_max'], r['acc_rms'], r['flips'], r['steps'], r['lag_mean'], r['lag_max']))
