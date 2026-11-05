# The falloff of kma_roundededge's normal blend, fitted to Karma's measured
# profile (docs/research/m4/karma-nodes.md 4.6): the angle towards the side
# face of a 90-degree edge at distance a = t / radius, for weight families
# w(d). Nova uses w = 1 - (d/r)^2 (docs/design/M4.md, M4.4 notes).

import math

# Karma kma_roundededge profile (karma-nodes.md 4.6): a = t / radius -> angle (deg)
karma = [(0.97, 0.3), (0.72, 7.9), (0.47, 19.9), (0.22, 33.8), (0.94, 0.8), (0.44, 21.8)]


def angle(a, w, n=400):
    # r = 1, P at the origin on the top face z = 0 (x < a), side face x = a (z < 0)
    h = 2.0 / n
    top = side = 0.0
    for i in range(n):
        x = -1 + (i + 0.5) * h
        for j in range(n):
            y = -1 + (j + 0.5) * h
            d = math.hypot(x, y)
            if d < 1 and x < a:
                top += w(d)
            z = -(i + 0.5) * h / 2  # z in (-1, 0)
            d2 = math.sqrt(a * a + y * y + z * z)
            if d2 < 1:
                side += w(d2) * 0.5
    return math.degrees(math.atan2(side, top))


def err(w):
    return sum((angle(a, w, 160) - k) ** 2 for a, k in karma)


for name, w in [('const', lambda d: 1.0)] + \
        [('poly k=%g' % k, (lambda k: lambda d: (1 - d * d) ** k)(k)) for k in (1, 2, 3, 4)] + \
        [('gauss s=%g' % s, (lambda s: lambda d: math.exp(-d * d / (2 * s * s))) (s)) for s in (0.2, 0.25, 0.3, 0.35, 0.4, 0.5)] + \
        [('lin', lambda d: 1 - d), ('lin2', lambda d: (1 - d) ** 2)]:
    prof = [round(angle(a, w, 160), 1) for a, _ in karma]
    print(name, prof, round(err(w), 1))
print('karma', [k for _, k in karma])
