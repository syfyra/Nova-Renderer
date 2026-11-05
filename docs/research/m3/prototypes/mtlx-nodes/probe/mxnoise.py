# Python port of MaterialX mx_noise.glsl (subset) for expected values.
import math
M = 0xFFFFFFFF


def rotl(x, k):
    return ((x << k) | (x >> (32 - k))) & M


def bjmix(a, b, c):
    a = (a - c) & M; a ^= rotl(c, 4); c = (c + b) & M
    b = (b - a) & M; b ^= rotl(a, 6); a = (a + c) & M
    c = (c - b) & M; c ^= rotl(b, 8); b = (b + a) & M
    a = (a - c) & M; a ^= rotl(c, 16); c = (c + b) & M
    b = (b - a) & M; b ^= rotl(a, 19); a = (a + c) & M
    c = (c - b) & M; c ^= rotl(b, 4); b = (b + a) & M
    return a, b, c


def bjfinal(a, b, c):
    c ^= b; c = (c - rotl(b, 14)) & M
    a ^= c; a = (a - rotl(c, 11)) & M
    b ^= a; b = (b - rotl(a, 25)) & M
    c ^= b; c = (c - rotl(b, 16)) & M
    a ^= c; a = (a - rotl(c, 4)) & M
    b ^= a; b = (b - rotl(a, 14)) & M
    c ^= b; c = (c - rotl(b, 24)) & M
    return c


def seed(n):
    return (0xdeadbeef + (n << 2) + 13) & M


def hash2(x, y):
    a = b = c = seed(2)
    return bjfinal((a + x) & M, (b + y) & M, c)


def hash3(x, y, z):
    a = b = c = seed(3)
    return bjfinal((a + x) & M, (b + y) & M, (c + z) & M)


def to01(bits):
    return bits / 4294967296.0


def fade(t):
    return t * t * t * (t * (t * 6.0 - 15.0) + 10.0)


def grad2(h, x, y):
    h &= 7
    u = x if h < 4 else y
    v = 2.0 * (y if h < 4 else x)
    return (-u if h & 1 else u) + (-v if h & 2 else v)


def grad3(h, x, y, z):
    h &= 15
    u = x if h < 8 else y
    v = y if h < 4 else (x if h in (12, 14) else z)
    return (-u if h & 1 else u) + (-v if h & 2 else v)


def perlin_v3_2d(x, y):
    X, Y = math.floor(x), math.floor(y)
    fx, fy = x - X, y - Y
    u, v = fade(fx), fade(fy)
    hs = [hash2(X, Y), hash2(X + 1, Y), hash2(X, Y + 1), hash2(X + 1, Y + 1)]
    out = []
    for c in range(3):
        g = [grad2((hs[0] >> 8 * c) & 255, fx, fy), grad2((hs[1] >> 8 * c) & 255, fx - 1, fy),
             grad2((hs[2] >> 8 * c) & 255, fx, fy - 1), grad2((hs[3] >> 8 * c) & 255, fx - 1, fy - 1)]
        s1 = 1 - u
        out.append(0.6616 * ((1 - v) * (g[0] * s1 + g[1] * u) + v * (g[2] * s1 + g[3] * u)))
    return out


def perlin_v3_3d(x, y, z):
    X, Y, Z = math.floor(x), math.floor(y), math.floor(z)
    fx, fy, fz = x - X, y - Y, z - Z
    u, v, w = fade(fx), fade(fy), fade(fz)
    hs = [hash3(X + (i & 1), Y + ((i >> 1) & 1), Z + (i >> 2)) for i in range(8)]
    out = []
    for c in range(3):
        g = [grad3((hs[i] >> 8 * c) & 255, fx - (i & 1), fy - ((i >> 1) & 1), fz - (i >> 2)) for i in range(8)]
        s1, t1, r1 = 1 - u, 1 - v, 1 - w
        out.append(0.9820 * (r1 * (t1 * (g[0] * s1 + g[1] * u) + v * (g[2] * s1 + g[3] * u)) +
                             w * (t1 * (g[4] * s1 + g[5] * u) + v * (g[6] * s1 + g[7] * u))))
    return out


def fractal_v3(x, y, z, octaves, lac, dim):
    r = [0.0, 0.0, 0.0]
    amp = 1.0
    for _ in range(octaves):
        n = perlin_v3_3d(x, y, z)
        r = [r[i] + amp * n[i] for i in range(3)]
        amp *= dim
        x, y, z = x * lac, y * lac, z * lac
    return r


def cell2(x, y):
    return to01(hash2(math.floor(x), math.floor(y)))


def cell_v3_2d(x, y):
    ix, iy = math.floor(x), math.floor(y)
    return [to01(hash3(ix, iy, k)) for k in range(3)]


def worley2(x, y, jitter, style):
    X, Y = math.floor(x), math.floor(y)
    lx, ly = x - X, y - Y
    d = [1e6, 1e6, 1e6]
    minpos = (0.0, 0.0)
    for i in (-1, 0, 1):
        for j in (-1, 0, 1):
            t = cell_v3_2d(i + X, j + Y)
            cx = i + ((t[0] - 0.5) * jitter + 0.5) - lx
            cy = j + ((t[1] - 0.5) * jitter + 0.5) - ly
            dist = cx * cx + cy * cy
            if dist < d[0]:
                d[2] = d[1]; d[1] = d[0]; d[0] = dist; minpos = (cx, cy)
            elif dist < d[1]:
                d[2] = d[1]; d[1] = dist
            elif dist < d[2]:
                d[2] = dist
    if style == 1:
        return cell_v3_2d(minpos[0] + x, minpos[1] + y)
    return [math.sqrt(v) for v in d]
