import math
import time
import numpy as np
import requests
from io import BytesIO
from PIL import Image

import rasterio
from rasterio.merge import merge
from rasterio.warp import reproject, Resampling
from rasterio.transform import from_origin
from rasterio.errors import RasterioIOError
from rasterio.vrt import WarpedVRT
from rasterio.warp import transform_bounds
from rasterio.windows import from_bounds as window_from_bounds

# ============================================================
# CONFIG
# ============================================================

OUT_W = 2048
OUT_H = 2048
BOX_SIZE_M = 16384000.0 / 4  # 16,384 km square in projected (EPSG:3857) meters

# Center (Attica bbox center from earlier)
CENTER_NORTH = 38 + 20/60 + 37.75/3600
CENTER_SOUTH = 37 + 36/60 + 58.25/3600
CENTER_WEST  = 23 +  8/60 + 12.85/3600
CENTER_EAST  = 24 + 18/60 + 11.15/3600
CENTER_LAT = (CENTER_NORTH + CENTER_SOUTH) * 0.5
CENTER_LON = (CENTER_WEST + CENTER_EAST) * 0.5

# DEM (Terrarium) tile config
TILE_SIZE = 256
DEM_TILE_URL = "https://s3.amazonaws.com/elevation-tiles-prod/terrarium/{z}/{x}/{y}.png"

# Choose a cap so you don't download thousands of tiles
MAX_DEM_TILES = 600  # adjust
DEM_Z_MIN = 0
DEM_Z_MAX = 12
DEM_OVERSAMPLE = 2.0  # source DEM meters/px ~ output/2 (reasonable)

# WorldCover base (COG tiles, 3x3 degrees)
WORLDCOVER_BASE = (
    "https://esa-worldcover.s3.eu-central-1.amazonaws.com/v100/2020/map/"
    "ESA_WorldCover_10m_2020_v100_{tile}_Map.tif"
)

WC_COLORS = {
    10: (0, 100, 0),
    20: (255, 187, 34),
    30: (255, 255, 76),
    40: (240, 150, 255),
    50: (250, 0, 0),
    60: (180, 180, 180),
    70: (240, 240, 240),
    80: (0, 100, 200),
    90: (0, 150, 160),
    95: (0, 207, 117),
    100: (250, 230, 160),
}

# ============================================================
# HTTP
# ============================================================

def get_with_retries(session, url, tries=6, timeout=60):
    last = None
    for i in range(tries):
        try:
            r = session.get(url, timeout=timeout)
            r.raise_for_status()
            return r
        except Exception as e:
            last = e
            wait = 1.4 ** i
            print(f"   ! fetch failed ({type(e).__name__}: {e}), retrying in {wait:.1f}s")
            time.sleep(wait)
    raise RuntimeError(f"Failed after {tries} tries: {url}\nLast error: {last}")

# ============================================================
# WEB MERCATOR (EPSG:3857) HELPERS
# ============================================================

ORIGIN_SHIFT = 20037508.342789244
MAX_LAT_WEBM = 85.05112878

def clamp(v, lo, hi):
    return lo if v < lo else hi if v > hi else v

def latlon_to_mercator_m(lat_deg, lon_deg):
    lat = clamp(lat_deg, -MAX_LAT_WEBM, MAX_LAT_WEBM)
    lon = ((lon_deg + 180.0) % 360.0) - 180.0
    x = lon * ORIGIN_SHIFT / 180.0
    y = math.log(math.tan((90.0 + lat) * math.pi / 360.0)) * (ORIGIN_SHIFT / math.pi)
    return x, y

def mercator_m_to_latlon(x, y):
    lon = (x / ORIGIN_SHIFT) * 180.0
    lat = (y / ORIGIN_SHIFT) * 180.0
    lat = 180.0 / math.pi * (2.0 * math.atan(math.exp(lat * math.pi / 180.0)) - math.pi / 2.0)
    return lat, lon

def make_square_bbox_mercator(center_lat, center_lon, box_size_m):
    cx, cy = latlon_to_mercator_m(center_lat, center_lon)
    half = box_size_m * 0.5
    mx0 = cx - half
    mx1 = cx + half
    my0 = cy - half
    my1 = cy + half

    # clamp to WebMercator valid extent in Y
    my0 = clamp(my0, -ORIGIN_SHIFT, ORIGIN_SHIFT)
    my1 = clamp(my1, -ORIGIN_SHIFT, ORIGIN_SHIFT)

    # X can wrap; keep within a single world by clamping to extent.
    # (If you need wrap, you must split. This script does not.)
    mx0 = clamp(mx0, -ORIGIN_SHIFT, ORIGIN_SHIFT)
    mx1 = clamp(mx1, -ORIGIN_SHIFT, ORIGIN_SHIFT)

    return mx0, my0, mx1, my1

# ============================================================
# TERRARIUM (DEM) HELPERS
# ============================================================

def latlon_to_pixel_xy(lat, lon, z):
    siny = math.sin(math.radians(lat))
    siny = min(max(siny, -0.9999), 0.9999)
    n = 2 ** z
    x = (lon + 180.0) / 360.0 * n * TILE_SIZE
    y = (0.5 - math.log((1 + siny) / (1 - siny)) / (4 * math.pi)) * n * TILE_SIZE
    return x, y

def pixel_xy_to_tile_xy(px, py):
    return int(px // TILE_SIZE), int(py // TILE_SIZE)

def decode_terrarium_rgba(tile_rgba):
    rgb = tile_rgba[..., :3].astype(np.float32)
    a = tile_rgba[..., 3]
    r = rgb[..., 0]
    g = rgb[..., 1]
    b = rgb[..., 2]
    elev = (r * 256.0 + g + b / 256.0) - 32768.0
    elev = elev.astype(np.float32)
    elev[a == 0] = np.nan
    return elev

def fill_nans_iterative(arr, iters=8):
    out = arr.copy()
    for _ in range(iters):
        nan_mask = ~np.isfinite(out)
        if not nan_mask.any():
            break
        padded = np.pad(out, ((1, 1), (1, 1)), mode="edge")
        n = padded[0:-2, 1:-1]
        s = padded[2:, 1:-1]
        w = padded[1:-1, 0:-2]
        e = padded[1:-1, 2:]
        stack = np.stack([n, s, w, e], axis=0)
        finite = np.isfinite(stack)
        count = finite.sum(axis=0).astype(np.float32)
        sumv = np.where(finite, stack, 0.0).sum(axis=0)
        fill = np.where(count > 0, sumv / count, np.nan)
        out[nan_mask] = fill[nan_mask]
    out[~np.isfinite(out)] = 0.0
    return out

def webmercator_pixel_to_meters(px, py, z):
    n = 2 ** z
    res = (2.0 * ORIGIN_SHIFT) / (n * TILE_SIZE)
    mx = px * res - ORIGIN_SHIFT
    my = ORIGIN_SHIFT - py * res
    return mx, my, res

def webmercator_mpp_at_lat(z, lat_deg):
    return 156543.03392804097 * math.cos(math.radians(lat_deg)) / (2 ** z)

def choose_dem_zoom_for_output(center_lat, box_size_m, out_w, z_min, z_max, oversample):
    desired_mpp = box_size_m / out_w
    target_mpp = desired_mpp / oversample
    mpp0 = webmercator_mpp_at_lat(0, center_lat)
    z = int(round(math.log(mpp0 / target_mpp, 2)))
    return max(z_min, min(z_max, z))

def count_dem_tiles_for_latlon_bbox(north, west, south, east, z):
    px0, py0 = latlon_to_pixel_xy(north, west, z)
    px1, py1 = latlon_to_pixel_xy(south, east, z)
    tx0, ty0 = pixel_xy_to_tile_xy(px0, py0)
    tx1, ty1 = pixel_xy_to_tile_xy(px1, py1)
    tiles_w = tx1 - tx0 + 1
    tiles_h = ty1 - ty0 + 1
    return tx0, ty0, tx1, ty1, tiles_w, tiles_h, tiles_w * tiles_h, px0, py0, px1, py1

# ============================================================
# WORLDCOVER (3x3 degree tiles) HELPERS
# ============================================================

def wc_tile_sw_corner(lat, lon):
    sw_lat = math.floor(lat / 3.0) * 3
    sw_lon = math.floor(lon / 3.0) * 3
    return sw_lat, sw_lon

def wc_tile_id_from_sw(sw_lat, sw_lon):
    lat_hem = "N" if sw_lat >= 0 else "S"
    lon_hem = "E" if sw_lon >= 0 else "W"
    return f"{lat_hem}{abs(int(sw_lat)):02d}{lon_hem}{abs(int(sw_lon)):03d}"

def needed_wc_tiles_for_bbox(north, west, south, east):
    sw_lat0, sw_lon0 = wc_tile_sw_corner(south + 1e-9, west + 1e-9)
    sw_lat1, sw_lon1 = wc_tile_sw_corner(north - 1e-9, east - 1e-9)
    tiles = []
    lat = sw_lat0
    while lat <= sw_lat1:
        lon = sw_lon0
        while lon <= sw_lon1:
            tiles.append(wc_tile_id_from_sw(lat, lon))
            lon += 3
        lat += 3
    return sorted(set(tiles))

# ============================================================
# MAIN
# ============================================================

def main():
    meters_per_px = BOX_SIZE_M / OUT_W

    # Define target square in EPSG:3857 meters (stable “square meters” notion)
    mx0, my0, mx1, my1 = make_square_bbox_mercator(CENTER_LAT, CENTER_LON, BOX_SIZE_M)

    # Convert that to a lat/lon bbox for selecting DEM tiles and WorldCover tiles
    # (DEM tiles are indexed by lat/lon; WorldCover is EPSG:4326)
    north, west = mercator_m_to_latlon(mx0, my1)
    south, east = mercator_m_to_latlon(mx1, my0)

    # Normalize lon ordering (no dateline wrapping supported here)
    if east < west:
        # This means you crossed the dateline in lon. This script doesn't split across it.
        raise RuntimeError(f"BBox crosses dateline: west={west:.6f} east={east:.6f}. Split into two bboxes to proceed.")

    # Choose DEM zoom to avoid absurd tile counts
    z = choose_dem_zoom_for_output(CENTER_LAT, BOX_SIZE_M, OUT_W, DEM_Z_MIN, DEM_Z_MAX, DEM_OVERSAMPLE)
    while z > DEM_Z_MIN:
        _, _, _, _, tw, th, total, _, _, _, _ = count_dem_tiles_for_latlon_bbox(north, west, south, east, z)
        if total <= MAX_DEM_TILES:
            break
        z -= 1
    DEM_ZOOM = z

    print("=== 16,384 km square map (EPSG:3857) ===")
    print(f"center lat/lon: {CENTER_LAT:.6f}, {CENTER_LON:.6f}")
    print(f"target: {BOX_SIZE_M/1000.0:.1f} km square, output {OUT_W}x{OUT_H}, {meters_per_px:.1f} m/px")
    print(f"mercator bbox meters: x[{mx0:.1f}, {mx1:.1f}] y[{my0:.1f}, {my1:.1f}]")
    print(f"lat/lon bbox used for sources: N{north:.6f} W{west:.6f} S{south:.6f} E{east:.6f}")
    if my0 == -ORIGIN_SHIFT or my1 == ORIGIN_SHIFT:
        print("NOTE: Y bbox hit WebMercator limit (±85.0511°). North/south coverage is clamped by projection limits.")

    print(f"DEM zoom auto: {DEM_ZOOM}")

    tx0, ty0, tx1, ty1, tiles_w, tiles_h, total, px0, py0, px1, py1 = count_dem_tiles_for_latlon_bbox(north, west, south, east, DEM_ZOOM)
    print(f"DEM tiles: {tiles_w} x {tiles_h} = {total}")

    # Destination transform in EPSG:3857
    dst_transform_3857 = from_origin(mx0, my1, meters_per_px, meters_per_px)

    # ---------------- DEM: stitch tiles ----------------
    stitched = np.full((tiles_h * TILE_SIZE, tiles_w * TILE_SIZE), np.nan, dtype=np.float32)
    session = requests.Session()

    idx = 0
    for iy, ty in enumerate(range(ty0, ty1 + 1)):
        for ix, tx in enumerate(range(tx0, tx1 + 1)):
            idx += 1
            url = DEM_TILE_URL.format(z=DEM_ZOOM, x=tx, y=ty)
            print(f"[{idx}/{total}] DEM z{DEM_ZOOM}/{tx}/{ty}")
            r = get_with_retries(session, url, tries=6, timeout=60)
            tile = Image.open(BytesIO(r.content)).convert("RGBA")
            elev = decode_terrarium_rgba(np.array(tile, dtype=np.uint8))
            elev = np.clip(elev, -12000.0, 9000.0)
            y0 = iy * TILE_SIZE
            x0 = ix * TILE_SIZE
            stitched[y0:y0 + TILE_SIZE, x0:x0 + TILE_SIZE] = elev

    # Crop stitched DEM to the bbox pixel window (avoid seam rounding artifacts)
    crop_x0 = int(math.floor(px0 - tx0 * TILE_SIZE))
    crop_y0 = int(math.floor(py0 - ty0 * TILE_SIZE))
    crop_x1 = int(math.ceil(px1 - tx0 * TILE_SIZE))
    crop_y1 = int(math.ceil(py1 - ty0 * TILE_SIZE))
    dem_crop = stitched[crop_y0:crop_y1, crop_x0:crop_x1]
    print(f"Cropped DEM px: {dem_crop.shape[1]} x {dem_crop.shape[0]}")

    nan_count = int((~np.isfinite(dem_crop)).sum())
    if nan_count:
        print(f"Filling DEM nodata pixels: {nan_count}")
        dem_crop = fill_nans_iterative(dem_crop, iters=8)

    # Source transform for cropped DEM in EPSG:3857
    gpx0 = tx0 * TILE_SIZE + crop_x0
    gpy0 = ty0 * TILE_SIZE + crop_y0
    mx_src0, my_src0, res_src_m = webmercator_pixel_to_meters(gpx0, gpy0, DEM_ZOOM)
    src_transform_3857 = from_origin(mx_src0, my_src0, res_src_m, res_src_m)

    # Reproject DEM (EPSG:3857 -> EPSG:3857) onto target grid
    dem = np.zeros((OUT_H, OUT_W), dtype=np.float32)
    reproject(
        source=dem_crop.astype(np.float32),
        destination=dem,
        src_transform=src_transform_3857,
        src_crs="EPSG:3857",
        dst_transform=dst_transform_3857,
        dst_crs="EPSG:3857",
        resampling=Resampling.bilinear,
    )

    # Normalize by land-only percentiles (avoid sea flattening)
    land = dem[dem > 0.0]
    lo = float(np.percentile(land, 0.5)) if land.size else float(np.min(dem))
    hi = float(np.percentile(land, 99.5)) if land.size else float(np.max(dem))
    print(f"Normalize (land p0.5..p99.5): lo={lo:.2f}m hi={hi:.2f}m")

    dem_clamped = np.clip(dem, lo, hi)
    norm = (dem_clamped - lo) / (hi - lo + 1e-9)

    h16 = np.clip(norm * 65535.0, 0, 65535).astype(np.uint16)
    Image.fromarray(h16, mode="I;16").save("map_height_16bit_2048.png")
    Image.fromarray((norm * 255.0).astype(np.uint8), mode="L").save("map_height_preview_8bit_2048.png")
    print("Saved: map_height_16bit_2048.png")
    print("Saved: map_height_preview_8bit_2048.png")

    # ---------------- WorldCover: mosaic needed tiles ----------------
    # ---------------- WorldCover: stream tiles into final 2048x2048 ----------------
    tiles = needed_wc_tiles_for_bbox(north, west, south, east)
    print(f"WorldCover tiles needed (candidates) ({len(tiles)}): {tiles}")

    dst_wc = np.zeros((OUT_H, OUT_W), dtype=np.uint8)

    for t in tiles:
        url = WORLDCOVER_BASE.format(tile=t)
        print(f"Open WorldCover COG: {url}")
        try:
            src = rasterio.open(url)
        except RasterioIOError as e:
            print(f"   ! skip WorldCover tile {t}: {e}")
            continue

        try:
            # Tile bounds in EPSG:4326 -> EPSG:3857
            b = transform_bounds(src.crs, "EPSG:3857", *src.bounds, densify_pts=21)
            tile_left, tile_bottom, tile_right, tile_top = b

            # Overlap with our destination bbox in EPSG:3857
            ov_left = max(tile_left, mx0)
            ov_right = min(tile_right, mx1)
            ov_bottom = max(tile_bottom, my0)
            ov_top = min(tile_top, my1)
            if ov_left >= ov_right or ov_bottom >= ov_top:
                src.close()
                continue

            # Destination window (pixel coordinates) for just the overlap
            win = window_from_bounds(ov_left, ov_bottom, ov_right, ov_top, transform=dst_transform_3857)
            win = win.round_offsets().round_lengths()

            # Clip to raster bounds (safety)
            row0 = int(max(0, win.row_off))
            col0 = int(max(0, win.col_off))
            row1 = int(min(OUT_H, win.row_off + win.height))
            col1 = int(min(OUT_W, win.col_off + win.width))
            if row0 >= row1 or col0 >= col1:
                src.close()
                continue

            h = row1 - row0
            w = col1 - col0

            # Create a warped view of this tile into our destination grid
            with WarpedVRT(
                src,
                crs="EPSG:3857",
                transform=dst_transform_3857,
                width=OUT_W,
                height=OUT_H,
                resampling=Resampling.nearest,
                nodata=0
            ) as vrt:
                data = vrt.read(1, window=((row0, row1), (col0, col1)), out_shape=(h, w))

            # Composite: WorldCover codes are {10..100}; 0 is nodata. Overwrite only where nonzero.
            mask = (data != 0)
            dst_wc[row0:row1, col0:col1][mask] = data[mask]

        finally:
            src.close()

    Image.fromarray(dst_wc, mode="L").save("map_landcover_worldcover_codes_2048.png")
    print("Saved: map_landcover_worldcover_codes_2048.png")

    palette = np.zeros((256, 3), dtype=np.uint8)
    for code, rgb in WC_COLORS.items():
        palette[int(code)] = rgb
    preview = palette[dst_wc]
    Image.fromarray(preview, mode="RGB").save("map_landcover_preview_2048.png")
    print("Saved: map_landcover_preview_2048.png")

    palette = np.zeros((256, 3), dtype=np.uint8)
    for code, rgb in WC_COLORS.items():
        palette[int(code)] = rgb
    preview = palette[dst_wc]
    Image.fromarray(preview, mode="RGB").save("map_landcover_preview_2048.png")
    print("Saved: map_landcover_preview_2048.png")

    print("\nDone. Outputs:")
    print(" - map_height_16bit_2048.png")
    print(" - map_height_preview_8bit_2048.png")
    print(" - map_landcover_worldcover_codes_2048.png")
    print(" - map_landcover_preview_2048.png")

if __name__ == "__main__":
    main()
