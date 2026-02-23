#!/usr/bin/env python3
"""
Icosahedron hemispheres (Americas / Eurafrasia) rendered as two separate images.

This version guarantees:
- exactly 10 faces per hemisphere
- each hemisphere is a single connected component on the icosahedron face graph
- the split “makes geographic sense” by growing two contiguous regions from
  reference directions (Americas vs Eurafrasia), rather than trying to force a
  meridian cut on a 20-face mesh.

Additional fix:
- Unfolding now supports an explicit "forbidden edge" seam so the Eurafrasia net
  stays glued except for ONE intended vertical seam (a meridian-like cut) running
  southward from your chosen TARGET_CORNER point.
"""

import os
import math
import time
from io import BytesIO
from typing import Dict, Tuple, List, Optional, Set

import numpy as np
import requests
from PIL import Image
import rasterio
from rasterio.enums import Resampling
from rasterio.errors import RasterioIOError

# ============================================================
# CONFIG
# ============================================================

# Output resolution control (triangle edge in pixels)
SIDE_PX = int(8000)

# Global DEM working grid (Terrarium mosaic resized to this)
HEIGHT_W = int(8196) * 2
HEIGHT_H = int(4096) * 2

# WorldCover per-tile cache resolution (downsample each 3° tile to this)
# Increase for sharper landcover edges; decreases speed/memory.
WC_TILE_RES = 512

# Optional: pin one existing icosahedron vertex to a specific geographic point
# (useful to stabilize where the net corners land)
TARGET_CORNER_LAT = 26.0
TARGET_CORNER_LON = 25.0
TARGET_CORNER_VERTEX_ID = 0
TARGET_CORNER_TWIST_DEG = 0.0  # spin around that pinned axis without moving the pin

# These define what "Americas" and "Eurafrasia" mean for the contiguous split.
# They are NOT a cut line; they are reference directions used to assign faces.
EURAFRASIA_REF_LAT = 25.0
EURAFRASIA_REF_LON = 20.0

AMERICAS_REF_LAT = 15.0
AMERICAS_REF_LON = -90.0

# --- Seam control for Eurafrasia unfolding ---
# Goal: Eurafrasia net has ONE main vertical seam through Africa:
# seam is near a chosen longitude and only applied southward from the chosen point.
SEAM_LON_DEG = TARGET_CORNER_LON          # "vertical seam" longitude
SEAM_START_LAT_DEG = TARGET_CORNER_LAT    # start seam at this latitude and cut southward
SEAM_LON_TOL_DEG = 12.0                   # tolerance in degrees for selecting seam edges (tune 8..18)

# Optional manual overrides (use sparingly; can break contiguity)
# These are applied AFTER the contiguous 10/10 split, and will be validated.
FORCE_EAST_FACE_IDS = []  # eurafrasia
FORCE_WEST_FACE_IDS = []  # americas

# Hemisphere planar transforms (applied in net plane, not as final image flip)
WEST_ROT90_LEFT = True
WEST_MIRROR_X = False
WEST_MIRROR_Y = False

EAST_MIRROR_X = True
EAST_MIRROR_Y = False
EAST_ROT90_LEFT = False

# Terrarium settings
TERRARIUM_URL = "https://s3.amazonaws.com/elevation-tiles-prod/terrarium/{z}/{x}/{y}.png"
TERRARIUM_Z = 7          # 16x16 = 256 requests; raise for more detail
TERRARIUM_TILE_SIZE = 256
TERRARIUM_CACHE_DIR = os.path.join("data_cache", "terrarium_cache")

# WorldCover settings
WORLDCOVER_BASE = (
    "https://esa-worldcover.s3.eu-central-1.amazonaws.com/v100/2020/map/"
    "ESA_WorldCover_10m_2020_v100_{tile}_Map.tif"
)

# WorldCover class colors
WC_COLORS = {
    10: (0, 100, 0),       # Tree cover
    20: (255, 187, 34),    # Shrubland
    30: (255, 255, 76),    # Grassland
    40: (240, 150, 255),   # Cropland
    50: (250, 0, 0),       # Built-up
    60: (180, 180, 180),   # Bare / sparse vegetation
    70: (240, 240, 240),   # Snow and ice
    80: (0, 100, 200),     # Permanent water bodies
    90: (0, 150, 160),     # Herbaceous wetland
    95: (0, 207, 117),     # Mangroves
    100: (250, 230, 160),  # Moss and lichen
}

# ============================================================
# WEB MERCATOR CONSTANTS
# ============================================================

ORIGIN_SHIFT = 20037508.342789244
MAX_LAT_WEBM = 85.05112878

# ============================================================
# HTTP
# ============================================================

def get_with_retries(session: requests.Session, url: str, tries: int = 6, timeout: int = 180) -> bytes:
    last = None
    for i in range(tries):
        try:
            r = session.get(url, timeout=timeout)
            r.raise_for_status()
            return r.content
        except Exception as e:
            last = e
            wait = 1.5 ** i
            print(f"   ! fetch failed ({type(e).__name__}: {e}), retrying in {wait:.1f}s")
            time.sleep(wait)
    raise RuntimeError(f"Failed after {tries} tries: {url}\nLast error: {last}")

# ============================================================
# WORLD / TILE HELPERS
# ============================================================

def clamp(v, lo, hi):
    return lo if v < lo else hi if v > hi else v

def decode_terrarium_rgba(rgba_u8: np.ndarray) -> np.ndarray:
    # Terrarium: (R*256 + G + B/256) - 32768
    rgb = rgba_u8[..., :3].astype(np.float32)
    r = rgb[..., 0]
    g = rgb[..., 1]
    b = rgb[..., 2]
    elev = (r * 256.0 + g + b / 256.0) - 32768.0
    return elev.astype(np.float32)

def wc_tile_id_from_sw(sw_lat, sw_lon):
    lat_hem = "N" if sw_lat >= 0 else "S"
    lon_hem = "E" if sw_lon >= 0 else "W"
    return f"{lat_hem}{abs(int(sw_lat)):02d}{lon_hem}{abs(int(sw_lon)):03d}"

# ============================================================
# TERRARIUM ON-DEMAND TILE CACHE (NO GLOBAL MOSAIC)
# ============================================================

class TerrariumTileCache:
    """
    Fetch/decode Terrarium PNG tiles on demand and sample elevations directly
    at native tile resolution (256x256). Avoids global mosaic + downsampling.

    Cache layers:
      - in-memory dict (optionally bounded LRU-ish)
      - on-disk .npy (float32 or float16)
    """
    def __init__(
        self,
        session: requests.Session,
        z: int,
        tile_size: int = 256,
        cache_dir: str = "data_cache/terrarium_tiles",
        store_float16: bool = True,
        max_mem_tiles: int = 256,
    ):
        self.session = session
        self.z = int(z)
        self.tile_size = int(tile_size)
        self.cache_dir = os.path.join(cache_dir, f"z{self.z}")
        self.store_float16 = bool(store_float16)
        self.max_mem_tiles = int(max_mem_tiles)

        self.cache: Dict[Tuple[int, int], np.ndarray] = {}
        self._use_order: List[Tuple[int, int]] = []  # poor-man LRU
        os.makedirs(self.cache_dir, exist_ok=True)

    def _disk_path(self, x: int, y: int) -> str:
        # file name keeps x/y explicit for easy inspection
        suffix = "f16" if self.store_float16 else "f32"
        return os.path.join(self.cache_dir, f"x{x}_y{y}_{suffix}.npy")

    def _touch(self, key: Tuple[int, int]) -> None:
        # move key to end
        try:
            self._use_order.remove(key)
        except ValueError:
            pass
        self._use_order.append(key)

        # evict if too many tiles in memory
        while len(self._use_order) > self.max_mem_tiles:
            old = self._use_order.pop(0)
            self.cache.pop(old, None)

    def get_tile(self, x: int, y: int) -> np.ndarray:
        key = (int(x), int(y))
        if key in self.cache:
            self._touch(key)
            return self.cache[key]

        p = self._disk_path(key[0], key[1])
        if os.path.exists(p):
            arr = np.load(p)
            # accept float16 or float32 depending on storage; convert to float32 for sampling
            if arr.shape == (self.tile_size, self.tile_size) and arr.dtype in (np.float16, np.float32):
                arr32 = arr.astype(np.float32, copy=False)
                self.cache[key] = arr32
                self._touch(key)
                return arr32
            try:
                os.remove(p)
            except OSError:
                pass

        url = TERRARIUM_URL.format(z=self.z, x=key[0], y=key[1])
        data = get_with_retries(self.session, url, tries=6, timeout=180)
        img = Image.open(BytesIO(data)).convert("RGBA")
        elev = decode_terrarium_rgba(np.array(img, dtype=np.uint8))  # float32, shape (256,256)

        # store to disk
        if self.store_float16:
            np.save(p, elev.astype(np.float16))
        else:
            np.save(p, elev.astype(np.float32))

        self.cache[key] = elev
        self._touch(key)
        return elev

    def sample(self, lat: np.ndarray, lon: np.ndarray) -> np.ndarray:
        """
        Vectorized sampling at (lat, lon) arrays (same length).
        Returns float32 elevations in meters.

        Uses standard WebMercator tile math:
          x = floor((lon+180)/360 * 2^z)
          y = floor((1 - ln(tan(lat)+sec(lat))/pi)/2 * 2^z)
        Pixel coords within tile from the fractional position.
        """
        lat = lat.astype(np.float64, copy=False)
        lon = lon.astype(np.float64, copy=False)
        npts = lat.shape[0]
        out = np.zeros(npts, dtype=np.float32)

        # Clamp to WebMercator valid latitude
        lat_c = np.clip(lat, -MAX_LAT_WEBM, MAX_LAT_WEBM)
        lon_n = ((lon + 180.0) % 360.0) - 180.0

        z = self.z
        n = 2 ** z
        ts = float(self.tile_size)

        # Normalize lon to [0,1)
        u = (lon_n + 180.0) / 360.0  # [0,1)
        # Normalize mercator y to [0,1)
        lat_rad = np.radians(lat_c)
        merc = np.log(np.tan(np.pi / 4.0 + lat_rad / 2.0))
        v = (1.0 - merc / np.pi) / 2.0  # [0,1)

        # Convert to tile coords in [0,n-1]
        xf = u * n
        yf = v * n

        tx = np.floor(xf).astype(np.int32)
        ty = np.floor(yf).astype(np.int32)

        # Clamp tile indices
        tx = np.clip(tx, 0, n - 1)
        ty = np.clip(ty, 0, n - 1)

        # Fraction inside tile -> pixel coords
        # Use (ts-1) so coords align with pixel centers similar to your other samplers
        fx = (xf - tx) * (ts - 1.0)
        fy = (yf - ty) * (ts - 1.0)

        # Group points by tile to minimize fetch/decode calls
        key = (tx.astype(np.int64) << 32) | (ty.astype(np.int64) & 0xFFFFFFFF)

        for kk in np.unique(key):
            sel = (key == kk)
            if not sel.any():
                continue

            tx0 = int(tx[sel][0])
            ty0 = int(ty[sel][0])

            tile = self.get_tile(tx0, ty0)  # float32 (256,256)

            # Bilinear sample inside this tile
            out[sel] = sample_bilinear(tile, fy[sel], fx[sel]).astype(np.float32)

        return out
# ============================================================
# ICOSAHEDRON GEOMETRY
# ============================================================

def normalize3(v):
    x, y, z = v
    n = math.sqrt(x*x + y*y + z*z)
    return (x/n, y/n, z/n)

def dot3(a, b):
    return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]

def rot_from_to(a, b):
    # rotation matrix mapping unit vector a -> unit vector b
    a = np.array(a, dtype=np.float64)
    b = np.array(b, dtype=np.float64)
    a /= np.linalg.norm(a)
    b /= np.linalg.norm(b)
    v = np.cross(a, b)
    c = float(np.dot(a, b))
    s = float(np.linalg.norm(v))
    if s < 1e-12:
        if c > 0:
            return np.eye(3, dtype=np.float64)
        axis = np.cross(a, np.array([1.0, 0.0, 0.0]))
        if np.linalg.norm(axis) < 1e-6:
            axis = np.cross(a, np.array([0.0, 1.0, 0.0]))
        axis /= np.linalg.norm(axis)
        K = np.array([[0, -axis[2], axis[1]],
                      [axis[2], 0, -axis[0]],
                      [-axis[1], axis[0], 0]], dtype=np.float64)
        return np.eye(3, dtype=np.float64) + 2.0 * (K @ K)
    v /= s
    K = np.array([[0, -v[2], v[1]],
                  [v[2], 0, -v[0]],
                  [-v[1], v[0], 0]], dtype=np.float64)
    return np.eye(3, dtype=np.float64) + K * s + (K @ K) * (1.0 - c)

def apply_R(R, v):
    x = R[0,0]*v[0] + R[0,1]*v[1] + R[0,2]*v[2]
    y = R[1,0]*v[0] + R[1,1]*v[1] + R[1,2]*v[2]
    z = R[2,0]*v[0] + R[2,1]*v[1] + R[2,2]*v[2]
    return (x, y, z)

def latlon_to_unit(lat_deg: float, lon_deg: float):
    lat = math.radians(lat_deg)
    lon = math.radians(lon_deg)
    cl = math.cos(lat)
    return (cl * math.cos(lon), cl * math.sin(lon), math.sin(lat))

def rotate_axis_angle(v, axis, angle_deg: float):
    # Rodrigues
    ax, ay, az = axis
    vx, vy, vz = v
    ang = math.radians(angle_deg)
    c = math.cos(ang)
    s = math.sin(ang)
    cx = ay*vz - az*vy
    cy = az*vx - ax*vz
    cz = ax*vy - ay*vx
    d = ax*vx + ay*vy + az*vz
    rx = vx*c + cx*s + ax*d*(1.0 - c)
    ry = vy*c + cy*s + ay*d*(1.0 - c)
    rz = vz*c + cz*s + az*d*(1.0 - c)
    return (rx, ry, rz)

def build_icosahedron_vertices_pole_oriented():
    phi = (1.0 + math.sqrt(5.0)) * 0.5
    raw = [
        (0,  1,  phi), (0, -1,  phi), (0,  1, -phi), (0, -1, -phi),
        ( 1,  phi, 0), (-1,  phi, 0), ( 1, -phi, 0), (-1, -phi, 0),
        ( phi, 0,  1), ( phi, 0, -1), (-phi, 0,  1), (-phi, 0, -1),
    ]
    V = [normalize3(v) for v in raw]

    # find most-opposite pair
    best_i, best_j, best_d = 0, 1, 1.0
    for i in range(len(V)):
        for j in range(i+1, len(V)):
            d = dot3(V[i], V[j])
            if d < best_d:
                best_i, best_j, best_d = i, j, d

    R = rot_from_to(V[best_i], (0.0, 0.0, 1.0))
    V2 = [normalize3(apply_R(R, v)) for v in V]
    # ensure opposite is -Z
    if V2[best_j][2] > 0.0:
        Rx = np.array([[1,0,0],[0,-1,0],[0,0,-1]], dtype=np.float64)
        V2 = [normalize3(apply_R(Rx, v)) for v in V2]
    return V2

def vertices_to_faces(V):
    faces = []
    nV = len(V)
    for a in range(nV):
        for b in range(a+1, nV):
            for c in range(b+1, nV):
                va, vb, vc = V[a], V[b], V[c]
                n0 = (
                    (vb[1]-va[1])*(vc[2]-va[2]) - (vb[2]-va[2])*(vc[1]-va[1]),
                    (vb[2]-va[2])*(vc[0]-va[0]) - (vb[0]-va[0])*(vc[2]-va[2]),
                    (vb[0]-va[0])*(vc[1]-va[1]) - (vb[1]-va[1])*(vc[0]-va[0]),
                )
                nn = math.sqrt(n0[0]*n0[0] + n0[1]*n0[1] + n0[2]*n0[2])
                if nn < 1e-12:
                    continue
                n = (n0[0]/nn, n0[1]/nn, n0[2]/nn)
                pos = neg = 0
                for k in range(nV):
                    if k in (a, b, c):
                        continue
                    s = n[0]*(V[k][0]-va[0]) + n[1]*(V[k][1]-va[1]) + n[2]*(V[k][2]-va[2])
                    if s > 1e-9:
                        pos += 1
                    elif s < -1e-9:
                        neg += 1
                    if pos and neg:
                        break
                if pos and neg:
                    continue
                faces.append((a, b, c))

    uniq = []
    seen = set()
    for f in faces:
        key = tuple(sorted(f))
        if key in seen:
            continue
        seen.add(key)
        uniq.append(f)

    if len(uniq) != 20:
        raise RuntimeError(f"Expected 20 faces, got {len(uniq)}")
    return uniq

def rotate_vertices_to_pin_vertex(V, vertex_id, target_lat, target_lon, twist_deg=0.0):
    tgt = latlon_to_unit(target_lat, target_lon)
    R = rot_from_to(V[vertex_id], tgt)
    V2 = [normalize3(apply_R(R, v)) for v in V]
    if abs(twist_deg) > 1e-9:
        axis = tgt
        V2 = [normalize3(rotate_axis_angle(v, axis, twist_deg)) for v in V2]
    return V2

def face_centroid_unit(V, face):
    a, b, c = face
    x = V[a][0] + V[b][0] + V[c][0]
    y = V[a][1] + V[b][1] + V[c][1]
    z = V[a][2] + V[b][2] + V[c][2]
    return normalize3((x, y, z))

# ============================================================
# UNFOLD NET (subset)
# ============================================================

SQRT3 = math.sqrt(3.0)

def build_face_adjacency(faces):
    edge_to_faces = {}
    for fi, (a,b,c) in enumerate(faces):
        for u,v in [(a,b),(b,c),(c,a)]:
            key = (u,v) if u < v else (v,u)
            edge_to_faces.setdefault(key, []).append(fi)
    return edge_to_faces

def build_face_neighbors(faces):
    edge_to_faces = build_face_adjacency(faces)
    nbrs = {fi: set() for fi in range(len(faces))}
    for _, flist in edge_to_faces.items():
        if len(flist) == 2:
            a, b = flist
            nbrs[a].add(b)
            nbrs[b].add(a)
    return nbrs

def rot60(dx, dy, sign):
    c = 0.5
    s = (SQRT3 * 0.5) * (1.0 if sign > 0 else -1.0)
    return (c*dx - s*dy, s*dx + c*dy)

def place_net_subset(
    faces,
    root_fi,
    allowed_set,
    forbidden_edges: Optional[Set[Tuple[int, int]]] = None
):
    """
    Unfold a connected set of faces into the plane.
    If forbidden_edges is provided, we will NOT traverse adjacencies across those edges.
    Those edges therefore become cuts in the net.
    """
    if forbidden_edges is None:
        forbidden_edges = set()

    side = float(SIDE_PX)
    h = side * (SQRT3 * 0.5)
    edge_to_faces = build_face_adjacency(faces)

    a, b, c = faces[root_fi]
    placed = {root_fi: {"verts_order": [a, b, c], "pts": [(0.0, 0.0), (side, 0.0), (side*0.5, h)]}}
    q = [root_fi]

    def face_point(face_id, vid):
        vo = placed[face_id]["verts_order"]
        pts = placed[face_id]["pts"]
        for i in range(3):
            if vo[i] == vid:
                return pts[i]
        return None

    while q:
        fcur = q.pop(0)
        vo = placed[fcur]["verts_order"]
        edges = [(vo[0], vo[1], vo[2]), (vo[1], vo[2], vo[0]), (vo[2], vo[0], vo[1])]
        for u, v, third_cur in edges:
            ekey = (u, v) if u < v else (v, u)
            if ekey in forbidden_edges:
                continue

            fs = edge_to_faces.get(ekey, [])
            if len(fs) != 2:
                continue
            fnei = fs[0] if fs[1] == fcur else fs[1]
            if fnei not in allowed_set or fnei in placed:
                continue

            a2, b2, c2 = faces[fnei]
            if a2 != u and a2 != v:
                third_nei = a2
            elif b2 != u and b2 != v:
                third_nei = b2
            else:
                third_nei = c2

            Pu = face_point(fcur, u)
            Pv = face_point(fcur, v)
            Ptc = face_point(fcur, third_cur)

            dx = Pv[0] - Pu[0]
            dy = Pv[1] - Pu[1]
            c1 = (Pu[0] + rot60(dx, dy, +1)[0], Pu[1] + rot60(dx, dy, +1)[1])
            c2 = (Pu[0] + rot60(dx, dy, -1)[0], Pu[1] + rot60(dx, dy, -1)[1])

            d1 = (c1[0]-Ptc[0])**2 + (c1[1]-Ptc[1])**2
            d2 = (c2[0]-Ptc[0])**2 + (c2[1]-Ptc[1])**2
            Ptnei = c2 if d1 < d2 else c1

            placed[fnei] = {"verts_order": [u, v, third_nei], "pts": [Pu, Pv, Ptnei]}
            q.append(fnei)

    return placed

def bbox_of_placed(placed):
    xs = []
    ys = []
    for rec in placed.values():
        for (x, y) in rec["pts"]:
            xs.append(x)
            ys.append(y)
    return min(xs), min(ys), max(xs), max(ys)

def translate_placed(placed, dx, dy):
    out = {}
    for fi, rec in placed.items():
        out[fi] = {"verts_order": rec["verts_order"], "pts": [(p[0] + dx, p[1] + dy) for p in rec["pts"]]}
    return out

def transform_placed(placed, mirror_x=False, mirror_y=False, rot90_left=False):
    minx, miny, maxx, maxy = bbox_of_placed(placed)
    cx = 0.5 * (minx + maxx)
    cy = 0.5 * (miny + maxy)
    sx = -1.0 if mirror_x else 1.0
    sy = -1.0 if mirror_y else 1.0

    out = {}
    for fi, rec in placed.items():
        pts2 = []
        for (x, y) in rec["pts"]:
            x0 = (x - cx) * sx
            y0 = (y - cy) * sy
            if rot90_left:
                x0, y0 = (-y0, x0)
            pts2.append((cx + x0, cy + y0))
        out[fi] = {"verts_order": rec["verts_order"], "pts": pts2}
    return out

# ============================================================
# SEAM HELPERS (for "one big vertical cut" on Eurafrasia net)
# ============================================================

def wrap_lon(lon_deg: float) -> float:
    return ((lon_deg + 180.0) % 360.0) - 180.0

def unit_to_latlon(p):
    x, y, z = p
    lat = math.degrees(math.asin(max(-1.0, min(1.0, z))))
    lon = math.degrees(math.atan2(y, x))
    return lat, lon

def edge_midpoint_latlon(V, u, v):
    mx = V[u][0] + V[v][0]
    my = V[u][1] + V[v][1]
    mz = V[u][2] + V[v][2]
    m = normalize3((mx, my, mz))
    return unit_to_latlon(m)

def build_seam_forbidden_edges(
    V,
    faces,
    face_set: Set[int],
    seam_lon_deg: float,
    seam_start_lat_deg: float,
    lon_tol_deg: float
) -> Set[Tuple[int, int]]:
    """
    Mark internal adjacency edges (shared by 2 faces in face_set) as forbidden if their
    edge midpoint lies near seam_lon_deg and at/below seam_start_lat_deg.
    """
    edge_to_faces = build_face_adjacency(faces)
    forbidden: Set[Tuple[int, int]] = set()

    for (u, v), flist in edge_to_faces.items():
        if len(flist) != 2:
            continue
        f0, f1 = flist
        if f0 not in face_set or f1 not in face_set:
            continue

        latm, lonm = edge_midpoint_latlon(V, u, v)
        dlon = abs(wrap_lon(lonm - seam_lon_deg))

        if dlon <= lon_tol_deg and latm <= seam_start_lat_deg + 1e-9:
            forbidden.add((u, v) if u < v else (v, u))

    return forbidden

# ============================================================
# FAST SAMPLING
# ============================================================

def sample_bilinear(arr, row, col):
    r0 = np.floor(row).astype(np.int32)
    c0 = np.floor(col).astype(np.int32)
    r1 = r0 + 1
    c1 = c0 + 1

    r0 = np.clip(r0, 0, arr.shape[0]-1)
    r1 = np.clip(r1, 0, arr.shape[0]-1)
    c0 = np.clip(c0, 0, arr.shape[1]-1)
    c1 = np.clip(c1, 0, arr.shape[1]-1)

    dr = (row - r0).astype(np.float32)
    dc = (col - c0).astype(np.float32)

    v00 = arr[r0, c0].astype(np.float32)
    v01 = arr[r0, c1].astype(np.float32)
    v10 = arr[r1, c0].astype(np.float32)
    v11 = arr[r1, c1].astype(np.float32)

    v0 = v00*(1.0-dc) + v01*dc
    v1 = v10*(1.0-dc) + v11*dc
    return v0*(1.0-dr) + v1*dr

def sample_nearest(arr, row, col):
    r = np.rint(row).astype(np.int32)
    c = np.rint(col).astype(np.int32)
    r = np.clip(r, 0, arr.shape[0]-1)
    c = np.clip(c, 0, arr.shape[1]-1)
    return arr[r, c]

# ============================================================
# VECTORIZED TRIANGLE RASTERIZATION
# ============================================================

def barycentric_grid(A, B, C, X, Y):
    Ax, Ay = A; Bx, By = B; Cx, Cy = C
    denom = (By - Cy)*(Ax - Cx) + (Cx - Bx)*(Ay - Cy)
    u = ((By - Cy)*(X - Cx) + (Cx - Bx)*(Y - Cy)) / denom
    v = ((Cy - Ay)*(X - Cx) + (Ax - Cx)*(Y - Cy)) / denom
    w = 1.0 - u - v
    return u, v, w

def xyz_to_latlon_vec(x, y, z):
    lat = np.degrees(np.arcsin(np.clip(z, -1.0, 1.0)))
    lon = np.degrees(np.arctan2(y, x))
    return lat, lon

# ============================================================
# BUILD GLOBAL TERRARIUM GRID
# ============================================================

def build_global_terrarium(session: requests.Session) -> Tuple[np.ndarray, Dict]:
    """
    Terrarium tiles -> mosaic -> resize to (HEIGHT_W, HEIGHT_H).
    Adds persistent disk cache so subsequent runs do 0 HTTP.
    """
    os.makedirs(TERRARIUM_CACHE_DIR, exist_ok=True)
    cache_path = os.path.join(
        TERRARIUM_CACHE_DIR,
        f"terrarium_z{TERRARIUM_Z}_{HEIGHT_W}x{HEIGHT_H}.npy"
    )

    # Disk cache hit
    if os.path.exists(cache_path):
        arr = np.load(cache_path)
        if arr.shape == (HEIGHT_H, HEIGHT_W) and arr.dtype == np.float32:
            print(f"Loaded Terrarium cache: {cache_path}")
            height_meta = {"W": HEIGHT_W, "H": HEIGHT_H, "origin_shift": ORIGIN_SHIFT}
            return arr, height_meta
        try:
            os.remove(cache_path)
        except OSError:
            pass

    n = 2 ** TERRARIUM_Z
    ts = TERRARIUM_TILE_SIZE
    full_h = n * ts
    full_w = n * ts

    print(f"Building Terrarium mosaic z={TERRARIUM_Z}: {n}x{n} tiles = {n*n} requests")
    mosaic = np.zeros((full_h, full_w), dtype=np.float32)

    idx = 0
    total = n * n
    for ty in range(n):
        for tx in range(n):
            idx += 1
            url = TERRARIUM_URL.format(z=TERRARIUM_Z, x=tx, y=ty)
            if idx % 16 == 0 or idx == 1 or idx == total:
                print(f"  Terrarium [{idx}/{total}] z{TERRARIUM_Z}/{tx}/{ty}")
            data = get_with_retries(session, url, tries=6, timeout=180)
            img = Image.open(BytesIO(data)).convert("RGBA")
            elev = decode_terrarium_rgba(np.array(img, dtype=np.uint8))
            y0 = ty * ts
            x0 = tx * ts
            mosaic[y0:y0+ts, x0:x0+ts] = elev

    mosaic_img = Image.fromarray(mosaic)
    resized = mosaic_img.resize((HEIGHT_W, HEIGHT_H), resample=Image.BILINEAR)
    height_arr = np.array(resized, dtype=np.float32)

    np.save(cache_path, height_arr)
    print(f"Saved Terrarium cache: {cache_path}")

    height_meta = {"W": HEIGHT_W, "H": HEIGHT_H, "origin_shift": ORIGIN_SHIFT}
    return height_arr, height_meta

# ============================================================
# WORLDCOVER ON-DEMAND TILE CACHE
# ============================================================

class WorldCoverCache:
    """
    Caches each 3° WorldCover tile as a small uint8 array.
    Cache layers:
      - in-memory dict
      - on-disk .npy
    """
    def __init__(self, session: requests.Session, tile_res: int = 512, cache_dir: str = "data_cache/worldcover_tiles"):
        self.session = session
        self.tile_res = tile_res
        self.cache_dir = cache_dir
        self.cache: Dict[str, np.ndarray] = {}
        os.makedirs(self.cache_dir, exist_ok=True)

    def _disk_path(self, tile_id: str) -> str:
        return os.path.join(self.cache_dir, f"{tile_id}_r{self.tile_res}.npy")

    def get_tile(self, tile_id: str) -> np.ndarray:
        if tile_id in self.cache:
            return self.cache[tile_id]

        p = self._disk_path(tile_id)
        if os.path.exists(p):
            arr = np.load(p)
            if arr.shape == (self.tile_res, self.tile_res) and arr.dtype == np.uint8:
                self.cache[tile_id] = arr
                return arr
            try:
                os.remove(p)
            except OSError:
                pass

        url = WORLDCOVER_BASE.format(tile=tile_id)
        try:
            with rasterio.open(url) as ds:
                data = ds.read(
                    1,
                    out_shape=(self.tile_res, self.tile_res),
                    resampling=Resampling.nearest
                ).astype(np.uint8)
        except RasterioIOError as e:
            print(f"   ! WorldCover missing tile {tile_id} (treated as 0): {e}")
            data = np.zeros((self.tile_res, self.tile_res), dtype=np.uint8)

        np.save(p, data)
        self.cache[tile_id] = data
        return data

    def sample(self, lat: np.ndarray, lon: np.ndarray) -> np.ndarray:
        lat = lat.astype(np.float64, copy=False)
        lon = lon.astype(np.float64, copy=False)

        eps = 1e-9
        lat = np.clip(lat, -90.0 + eps, 90.0 - eps)
        lon = ((lon + 180.0) % 360.0) - 180.0
        lon = np.clip(lon, -180.0 + eps, 180.0 - eps)

        sw_lat = np.floor(lat / 3.0) * 3.0
        sw_lon = np.floor(lon / 3.0) * 3.0
        sw_lat = np.clip(sw_lat, -90.0, 87.0)
        sw_lon = np.clip(sw_lon, -180.0, 177.0)

        sw_lat_i = sw_lat.astype(np.int32)
        sw_lon_i = sw_lon.astype(np.int32)

        k_lat = ((sw_lat_i + 90) // 3).astype(np.int32)
        k_lon = ((sw_lon_i + 180) // 3).astype(np.int32)
        key = k_lat * 1000 + k_lon

        out = np.zeros(lat.shape[0], dtype=np.uint8)

        for kk in np.unique(key):
            sel = (key == kk)
            if not sel.any():
                continue

            lat0 = int(sw_lat_i[sel][0])
            lon0 = int(sw_lon_i[sel][0])
            tile_id = wc_tile_id_from_sw(lat0, lon0)

            tile = self.get_tile(tile_id)
            N = tile.shape[0]

            lat_sel = lat[sel]
            lon_sel = lon[sel]

            row = ((lat0 + 3.0) - lat_sel) / 3.0 * (N - 1)
            col = (lon_sel - lon0) / 3.0 * (N - 1)

            out[sel] = sample_nearest(tile, row, col)

        return out

# ============================================================
# RENDER HEMISPHERE
# ============================================================

def normalize_height_to_16(height: np.ndarray, mask: np.ndarray) -> Tuple[np.ndarray, Tuple[float, float]]:
    valid = (mask > 0) & np.isfinite(height)
    if valid.sum() < 1024:
        lo = float(np.nanmin(height)) if np.isfinite(np.nanmin(height)) else 0.0
        hi = float(np.nanmax(height)) if np.isfinite(np.nanmax(height)) else lo + 1.0
        if hi <= lo + 1e-6:
            hi = lo + 1.0
    else:
        v = height[valid]
        lo = float(np.percentile(v, 0.5))
        hi = float(np.percentile(v, 99.5))
        if hi <= lo + 1e-6:
            hi = lo + 1.0

    h = np.clip(height, lo, hi)
    n = (h - lo) / (hi - lo + 1e-9)
    n[~valid] = 0.0
    h16 = np.clip(n * 65535.0, 0, 65535).astype(np.uint16)
    return h16, (lo, hi)

def height_to_fixed_range_8bit(
    height_m: np.ndarray,
    mask: np.ndarray,
    lo_m: float = 0.0,
    hi_m: float = 8000.0,
    force_sea_black: bool = True,
) -> np.ndarray:
    """
    Map elevation in meters to uint8 using a fixed physical range.
      lo_m..hi_m -> 0..255 (linear), clipped.
    - mask==0 stays 0
    - if force_sea_black: heights <= 0 become 0
    """
    out = np.zeros(height_m.shape, dtype=np.uint8)

    valid = (mask > 0) & np.isfinite(height_m)
    if not np.any(valid):
        return out

    h = height_m.copy()
    if force_sea_black:
        h = np.where(h <= 0.0, 0.0, h)

    h = np.clip(h, lo_m, hi_m)
    scale = 255.0 / (hi_m - lo_m)
    out_valid = np.rint((h[valid] - lo_m) * scale).astype(np.uint8)

    out[valid] = out_valid
    return out

def render_hemisphere(
    out_prefix: str,
    placed: dict,
    V: List[Tuple[float, float, float]],
    terr_cache: "TerrariumTileCache",
    wc_cache: "WorldCoverCache",
    wc_palette: np.ndarray,
):
    # Canvas bounds in image coords (y-down)
    all_pts = []
    for rec in placed.values():
        for (x, y) in rec["pts"]:  # y-up
            all_pts.append((x, -y))
    minx = min(p[0] for p in all_pts)
    miny = min(p[1] for p in all_pts)
    maxx = max(p[0] for p in all_pts)
    maxy = max(p[1] for p in all_pts)

    margin = 32
    W = int(math.ceil(maxx - minx)) + margin*2 + 2
    H = int(math.ceil(maxy - miny)) + margin*2 + 2

    land = np.zeros((H, W, 3), dtype=np.uint8)
    height = np.full((H, W), np.nan, dtype=np.float32)
    mask = np.zeros((H, W), dtype=np.uint8)

    for fi, rec in placed.items():
        vo = rec["verts_order"]
        pts = rec["pts"]  # y-up

        A = (pts[0][0] - minx + margin, -pts[0][1] - miny + margin)
        B = (pts[1][0] - minx + margin, -pts[1][1] - miny + margin)
        C = (pts[2][0] - minx + margin, -pts[2][1] - miny + margin)

        A3 = V[vo[0]]
        B3 = V[vo[1]]
        C3 = V[vo[2]]

        x0 = max(0, int(math.floor(min(A[0], B[0], C[0]) - 1)))
        x1 = min(W-1, int(math.ceil(max(A[0], B[0], C[0]) + 1)))
        y0 = max(0, int(math.floor(min(A[1], B[1], C[1]) - 1)))
        y1 = min(H-1, int(math.ceil(max(A[1], B[1], C[1]) + 1)))

        ww = x1 - x0 + 1
        hh = y1 - y0 + 1
        if ww <= 1 or hh <= 1:
            continue

        xs = np.arange(x0, x1 + 1, dtype=np.float32) + 0.5
        ys = np.arange(y0, y1 + 1, dtype=np.float32) + 0.5
        X, Y = np.meshgrid(xs, ys)

        u, v, w = barycentric_grid(A, B, C, X, Y)
        inside = (u >= 0.0) & (v >= 0.0) & (w >= 0.0)
        if not inside.any():
            continue

        u1 = u[inside].astype(np.float32)
        v1 = v[inside].astype(np.float32)
        w1 = w[inside].astype(np.float32)

        # sphere normalize
        x = u1*A3[0] + v1*B3[0] + w1*C3[0]
        y = u1*A3[1] + v1*B3[1] + w1*C3[1]
        z = u1*A3[2] + v1*B3[2] + w1*C3[2]
        inv = 1.0 / np.sqrt(x*x + y*y + z*z)
        x *= inv; y *= inv; z *= inv

        lat, lon = xyz_to_latlon_vec(x, y, z)

        # DEM: lat/lon -> WebMercator meters -> terrarium array indices
        # DEM sampled directly from native Terrarium tiles (no global downsample)
        hv = terr_cache.sample(lat.astype(np.float64, copy=False), lon.astype(np.float64, copy=False))

        # WorldCover
        cls = wc_cache.sample(lat, lon)

        # Write back to canvas
        iy, ix = np.nonzero(inside)
        px = (ix + x0).astype(np.int32)
        py = (iy + y0).astype(np.int32)

        mask[py, px] = 255
        height[py, px] = hv
        land[py, px] = wc_palette[cls]

        print(f"{out_prefix}: face {fi:02d} -> {px.size} px")

    # New: fixed physical range 0..8000m -> 0..255, sea level black
    h8_fixed = height_to_fixed_range_8bit(height, mask, lo_m=-10.0, hi_m=8000.0, force_sea_black=True)

    Image.fromarray(h8_fixed, mode="L").save(f"{out_prefix}_height_0_8000_8bit.png")  # new fixed-range output
    Image.fromarray(land, mode="RGB").save(f"{out_prefix}_landcover.png")
    Image.fromarray(mask, mode="L").save(f"{out_prefix}_mask.png")

# ============================================================
# CONTIGUOUS 10/10 SPLIT
# ============================================================

def is_connected(nbrs, S):
    if not S:
        return False
    start = next(iter(S))
    seen = {start}
    stack = [start]
    while stack:
        u = stack.pop()
        for v in nbrs[u]:
            if v in S and v not in seen:
                seen.add(v)
                stack.append(v)
    return len(seen) == len(S)

def split_faces_contiguous_10_10(V, faces, ref_e_lat, ref_e_lon, ref_w_lat, ref_w_lon):
    """
    Returns (west, east), each size 10, each connected, chosen to match geography.
    Uses greedy region-growing on the face adjacency graph from two seeds.
    """
    nbrs = build_face_neighbors(faces)

    ve = latlon_to_unit(ref_e_lat, ref_e_lon)
    vw = latlon_to_unit(ref_w_lat, ref_w_lon)

    sE = np.zeros(len(faces), dtype=np.float64)
    sW = np.zeros(len(faces), dtype=np.float64)
    for fi, f in enumerate(faces):
        c = face_centroid_unit(V, f)
        sE[fi] = dot3(c, ve)
        sW[fi] = dot3(c, vw)

    seed_e = int(np.argmax(sE))
    seed_w = int(np.argmax(sW))
    if seed_e == seed_w:
        order = list(np.argsort(-sW))
        for cand in order:
            if int(cand) != seed_e:
                seed_w = int(cand)
                break

    east = {seed_e}
    west = {seed_w}

    def frontier(region, other_region):
        fr = set()
        for u in region:
            for v in nbrs[u]:
                if v not in region and v not in other_region:
                    fr.add(v)
        return fr

    TARGET = 10
    ALL = set(range(len(faces)))

    while len(east) < TARGET or len(west) < TARGET:
        fe = frontier(east, west) if len(east) < TARGET else set()
        fw = frontier(west, east) if len(west) < TARGET else set()

        if len(east) < TARGET and not fe:
            fe = {v for u in east for v in nbrs[u] if v not in east and v not in west}
        if len(west) < TARGET and not fw:
            fw = {v for u in west for v in nbrs[u] if v not in west and v not in east}

        if not fe and not fw:
            break

        # choose which side to grow
        if len(east) < len(west):
            grow_e = True
        elif len(west) < len(east):
            grow_e = False
        else:
            best_e = max((sE[v] - sW[v]) for v in fe) if fe else -1e9
            best_w = max((sW[v] - sE[v]) for v in fw) if fw else -1e9
            grow_e = best_e >= best_w

        if grow_e:
            cand = max(fe, key=lambda v: (sE[v] - sW[v], sE[v]))
            east.add(int(cand))
        else:
            cand = max(fw, key=lambda v: (sW[v] - sE[v], sW[v]))
            west.add(int(cand))

        if len(east) > TARGET or len(west) > TARGET:
            break

    # Fill remaining deterministically by preference
    unassigned = list(ALL - east - west)
    unassigned.sort(key=lambda v: (sE[v] - sW[v]), reverse=True)
    for v in unassigned:
        if len(east) < TARGET:
            east.add(int(v))
        else:
            west.add(int(v))

    if len(east) != TARGET or len(west) != TARGET:
        raise RuntimeError(f"Split sizes wrong: east={len(east)} west={len(west)}")

    # Connectivity repair (should usually already be connected)
    if not is_connected(nbrs, east) or not is_connected(nbrs, west):
        def boundary_faces(A, B):
            bd = set()
            for u in A:
                for v in nbrs[u]:
                    if v in B:
                        bd.add(u)
            return bd

        for _ in range(200):
            if is_connected(nbrs, east) and is_connected(nbrs, west):
                break

            be = boundary_faces(east, west)
            bw = boundary_faces(west, east)

            improved = False
            candidates_e = sorted(be, key=lambda v: (sE[v] - sW[v]))
            candidates_w = sorted(bw, key=lambda v: (sW[v] - sE[v]))
            for e_out in candidates_e[:8]:
                for w_out in candidates_w[:8]:
                    e2 = set(east); w2 = set(west)
                    e2.remove(e_out); w2.remove(w_out)
                    e2.add(w_out); w2.add(e_out)
                    if is_connected(nbrs, e2) and is_connected(nbrs, w2):
                        east, west = e2, w2
                        improved = True
                        break
                if improved:
                    break

            if not improved:
                break

        if not is_connected(nbrs, east) or not is_connected(nbrs, west):
            raise RuntimeError("Could not find a connected 10/10 split. Adjust reference lat/lon or pin/twist.")

    return west, east

# ============================================================
# MAIN
# ============================================================

def main():
    session = requests.Session()

    wc_palette = np.zeros((256, 3), dtype=np.uint8)
    for code, rgb in WC_COLORS.items():
        wc_palette[int(code)] = rgb

    # Icosahedron
    V = build_icosahedron_vertices_pole_oriented()

    # Optional pin
    V = rotate_vertices_to_pin_vertex(
        V,
        TARGET_CORNER_VERTEX_ID,
        TARGET_CORNER_LAT,
        TARGET_CORNER_LON,
        twist_deg=TARGET_CORNER_TWIST_DEG
    )

    faces = vertices_to_faces(V)

    # Contiguous 10/10 split: west=Americas, east=Eurafrasia
    west, east = split_faces_contiguous_10_10(
        V, faces,
        ref_e_lat=EURAFRASIA_REF_LAT, ref_e_lon=EURAFRASIA_REF_LON,
        ref_w_lat=AMERICAS_REF_LAT,   ref_w_lon=AMERICAS_REF_LON
    )

    # Optional manual overrides (validated)
    if FORCE_EAST_FACE_IDS or FORCE_WEST_FACE_IDS:
        nbrs = build_face_neighbors(faces)

        for fi in FORCE_EAST_FACE_IDS:
            west.discard(fi)
            east.add(fi)
        for fi in FORCE_WEST_FACE_IDS:
            east.discard(fi)
            west.add(fi)

        if len(east) != 10 or len(west) != 10:
            raise RuntimeError(f"Overrides broke 10/10 sizing: east={len(east)} west={len(west)}")

        if not is_connected(nbrs, east) or not is_connected(nbrs, west):
            raise RuntimeError("Overrides broke contiguity; remove FORCE_* or change which faces you force.")

    if not west or not east:
        raise RuntimeError("Hemisphere split produced empty set (should not happen).")

    print("west (americas) faces:", sorted(west))
    print("east (eurafrasia) faces:", sorted(east))

    # Root selection: choose Eurafrasia root near your pinned point so seam starts "there"
    tgt = latlon_to_unit(TARGET_CORNER_LAT, TARGET_CORNER_LON)
    root_e = max(east, key=lambda fi: dot3(face_centroid_unit(V, faces[fi]), tgt))
    root_w = min(west)

    # Build seam forbidden edges for EAST only (one intended vertical cut)
    forbidden_e = build_seam_forbidden_edges(
        V, faces, east,
        seam_lon_deg=SEAM_LON_DEG,
        seam_start_lat_deg=SEAM_START_LAT_DEG,
        lon_tol_deg=SEAM_LON_TOL_DEG
    )
    print("eurafrasia forbidden seam edges:", len(forbidden_e))

    placed_w = place_net_subset(faces, root_w, west, forbidden_edges=set())
    placed_e = place_net_subset(faces, root_e, east, forbidden_edges=forbidden_e)

    # Sanity: ensure unfolding reached all faces
    missing_e = sorted(list(east - set(placed_e.keys())))
    missing_w = sorted(list(west - set(placed_w.keys())))
    if missing_e:
        raise RuntimeError(f"Eurafrasia unfolding missed faces {missing_e}. Reduce SEAM_LON_TOL_DEG.")
    if missing_w:
        raise RuntimeError(f"Americas unfolding missed faces {missing_w} (unexpected).")

    # Tighten, transform, tighten
    w_minx, w_miny, _, _ = bbox_of_placed(placed_w)
    e_minx, e_miny, _, _ = bbox_of_placed(placed_e)
    placed_w = translate_placed(placed_w, -w_minx, -w_miny)
    placed_e = translate_placed(placed_e, -e_minx, -e_miny)

    placed_w = transform_placed(placed_w, mirror_x=WEST_MIRROR_X, mirror_y=WEST_MIRROR_Y, rot90_left=WEST_ROT90_LEFT)
    placed_e = transform_placed(placed_e, mirror_x=EAST_MIRROR_X, mirror_y=EAST_MIRROR_Y, rot90_left=EAST_ROT90_LEFT)

    w_minx, w_miny, _, _ = bbox_of_placed(placed_w)
    e_minx, e_miny, _, _ = bbox_of_placed(placed_e)
    placed_w = translate_placed(placed_w, -w_minx, -w_miny)
    placed_e = translate_placed(placed_e, -e_minx, -e_miny)

    # Build global DEM once (Terrarium)
    height_arr, height_meta = build_global_terrarium(session)

    # WorldCover cache
    wc_cache = WorldCoverCache(session=session, tile_res=WC_TILE_RES)

    # Terrarium cache (on-demand tile sampling, no global mosaic)
    terr_cache = TerrariumTileCache(
        session=session,
        z=TERRARIUM_Z,
        tile_size=TERRARIUM_TILE_SIZE,
        cache_dir=os.path.join("data_cache", "terrarium_tiles"),
        store_float16=True,
        max_mem_tiles=512,
    )
    # Render separately
    render_hemisphere("americas", placed_w, V, terr_cache, wc_cache, wc_palette)
    render_hemisphere("eurafrasia", placed_e, V, terr_cache, wc_cache, wc_palette)

    print("Done.")

if __name__ == "__main__":
    main()
