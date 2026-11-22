"""Simplifies a terrain stamp's point grid into triangle lists (tools/bake_terrain_stamp.py).

Quadric error edge collapses (Garland and Heckbert 1997), restricted to
half-edge collapses: a point is merged into one of its neighbours, so every
kept point is an original stamp point and keeps its normal, texcoord and
weight exactly. The terrain draws a stamp by blending each point from the
ground under it to its place on the rock by its weight, so:
  - points on the mesh boundary (the weight-0 ring that lies on the ground)
    only slide along it, into a boundary neighbour, held to the outline by
    planes perpendicular to it (the ground under them is a 4 m grid anyway);
  - a point only merges into a neighbour of similar weight (about one row of
    the blend band), so the band to the ground keeps its shape;
  - a collapse that would turn a triangle over (or nearly edge-on) is refused.
Levels are nested: each continues from the previous one with a larger error.
"""

import heapq

import numpy as np

WEIGHT_MATCH = 0.35   # largest weight difference between merged points
BOUNDARY_WEIGHT = 10.0  # how firmly boundary planes hold the outline
FLIP_LIMIT = 0.2      # smallest cosine between a triangle's old and new normal


def grid_triangles(weight):
    """The full-resolution triangulation the terrain drew before: every grid
    quad touching the shape (any corner with weight > 0), two triangles each,
    wound so u x v (the stamp's outward direction) faces out. Indices are
    points (u + v * width)."""
    height, width = weight.shape
    index = np.arange(height * width).reshape(height, width)
    touched = (weight[:-1, :-1] > 0) | (weight[:-1, 1:] > 0) | (weight[1:, :-1] > 0) | (weight[1:, 1:] > 0)
    a, b = index[:-1, :-1][touched], index[:-1, 1:][touched]
    c, d = index[1:, :-1][touched], index[1:, 1:][touched]
    return np.concatenate([np.stack([a, b, c], 1), np.stack([b, d, c], 1)])


def _plane_quadric(p0, p1, p2):
    n = np.cross(p1 - p0, p2 - p0)
    area2 = np.linalg.norm(n)
    if area2 < 1e-12:
        return np.zeros((4, 4))
    n = n / area2
    plane = np.append(n, -np.dot(n, p0))
    # Area-weighted, so large flat triangles hold their plane firmly.
    return np.outer(plane, plane) * (area2 * 0.5)


def simplify_levels(positions, weight, triangles, tolerances, locked=None):
    """positions (n, 3), weight (n,), triangles (m, 3) point indices.
    tolerances: largest allowed error per level (metres, stamp space), rising.
    locked (n,) bool, optional: points that are never merged away (a sculpted
    cell's 4 m border points, which its neighbours' grids share).
    Returns one (k, 3) triangle array per tolerance."""
    count = len(positions)
    triangles = [list(t) for t in triangles]
    alive = [True] * len(triangles)
    around = [set() for _ in range(count)]
    for t, tri in enumerate(triangles):
        for v in tri:
            around[v].add(t)
    # Boundary points: on an edge used by one triangle only.
    edge_uses = {}
    for tri in triangles:
        for i in range(3):
            e = tuple(sorted((tri[i], tri[(i + 1) % 3])))
            edge_uses[e] = edge_uses.get(e, 0) + 1
    fixed = np.zeros(count, bool)
    boundary_edges = set()
    for (a, b), uses in edge_uses.items():
        if uses == 1:
            fixed[a] = fixed[b] = True
            boundary_edges.add((a, b))
    quadric = np.zeros((count, 4, 4))
    for tri in triangles:
        q = _plane_quadric(positions[tri[0]], positions[tri[1]], positions[tri[2]])
        for v in tri:
            quadric[v] += q
    # Boundary: a plane through each boundary edge, perpendicular to its
    # triangle, so sliding along the outline costs only its bends.
    for tri in triangles:
        for i in range(3):
            a, b = tri[i], tri[(i + 1) % 3]
            if (min(a, b), max(a, b)) not in boundary_edges:
                continue
            face = np.cross(positions[tri[1]] - positions[tri[0]], positions[tri[2]] - positions[tri[0]])
            edge = positions[b] - positions[a]
            normal = np.cross(edge, face)
            length = np.linalg.norm(normal)
            if length < 1e-12:
                continue
            normal /= length
            plane = np.append(normal, -np.dot(normal, positions[a]))
            q = np.outer(plane, plane) * BOUNDARY_WEIGHT * np.dot(edge, edge)
            quadric[a] += q
            quadric[b] += q
    version = [0] * count
    removed = np.zeros(count, bool)
    used = np.zeros(count, bool)
    for tri in triangles:
        used[tri] = True

    def neighbours(v):
        result = set()
        for t in around[v]:
            result.update(triangles[t])
        result.discard(v)
        return result

    def area_around(u):
        return sum(np.linalg.norm(np.cross(
            positions[triangles[t][1]] - positions[triangles[t][0]],
            positions[triangles[t][2]] - positions[triangles[t][0]])) * 0.5 for t in around[u])

    def cost(u, v, area):
        # Squared plane distances, area-weighted, per unit of the area around
        # u: about the squared distance the surface moves.
        p = np.append(positions[v], 1.0)
        return float(p @ (quadric[u] + quadric[v]) @ p) / max(area, 1e-6)

    def allowed(u, v):
        if locked is not None and locked[u]:
            return False
        if abs(weight[u] - weight[v]) > WEIGHT_MATCH:
            return False
        # Boundary points slide along the boundary only.
        if fixed[u] and (min(u, v), max(u, v)) not in boundary_edges:
            return False
        for t in around[u]:
            tri = triangles[t]
            if v in tri:
                continue
            old = np.cross(positions[tri[1]] - positions[tri[0]], positions[tri[2]] - positions[tri[0]])
            moved = [v if x == u else x for x in tri]
            new = np.cross(positions[moved[1]] - positions[moved[0]], positions[moved[2]] - positions[moved[0]])
            lo, ln = np.linalg.norm(old), np.linalg.norm(new)
            if ln < 1e-12 or (lo > 1e-12 and np.dot(old, new) / (lo * ln) < FLIP_LIMIT):
                return False
        return True

    heap = []

    def push_from(u):
        area = area_around(u)
        for v in neighbours(u):
            heapq.heappush(heap, (cost(u, v, area), u, v, version[u], version[v]))

    for u in range(count):
        if used[u]:
            push_from(u)
    levels = []
    for tolerance in tolerances:
        # Collapses are taken cheapest first while the surface moves less
        # than the tolerance.
        while heap:
            c, u, v, vu, vv = heap[0]
            if removed[u] or removed[v] or vu != version[u] or vv != version[v]:
                heapq.heappop(heap)
                continue
            if c > tolerance * tolerance:
                break
            heapq.heappop(heap)
            if not allowed(u, v):
                continue
            for t in list(around[u]):
                tri = triangles[t]
                if v in tri:
                    alive[t] = False
                    for x in tri:
                        around[x].discard(t)
                else:
                    triangles[t] = [v if x == u else x for x in tri]
                    around[v].add(t)
            around[u].clear()
            removed[u] = True
            quadric[v] += quadric[u]
            version[v] += 1
            for w in neighbours(v):
                version[w] += 1
            push_from(v)
            for w in neighbours(v):
                if not removed[w]:
                    push_from(w)
        levels.append(np.array([triangles[t] for t in range(len(triangles)) if alive[t]], dtype=np.int64))
    return levels
