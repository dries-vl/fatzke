import math
import time
import numpy as np
import requests
from io import BytesIO
from PIL import Image

import rasterio
from rasterio.merge import merge
from rasterio.warp import reproject, Resampling
from rasterio.transform import from_bounds

# ============================================================
# CONFIG
# ============================================================

# Your bbox (north, west, south, east)
north = 38 + 17/60 + 30.9/3600
west  = 23 +  0/60 + 45.3/3600
south = 37 + 22/60 + 56.7/3600
east  = 24 + 18/60 + 26.9/3600

# DEM tile zoom (small downloads)
DEM_ZOOM = 9
TILE_SIZE = 256
DEM_TILE_URL = "https://s3.amazonaws.com/elevation-tiles-prod/terrarium/{z}/{x}/{y}.png"

# Target ground resolution
KM_PER_PIXEL = 1.0  # "1km per pixel more or less"

# WorldCover base (COG tiles, 3x3 degrees)
WORLDCOVER_BASE = (
    "https://esa-worldcover.s3.eu-central-1.amazonaws.com/v100/2020/map/"
    "ESA_WorldCover_10m_2020_v100_{tile}_Map.tif"
)

# WorldCover legend palette for preview (optional)
WC_COLORS = {
    10: (0,100,0),     # Tree cover
    20: (255,187,34),  # Shrubland
    30: (255,255,76),  # Grassland
    40: (240,150,255), # Cropland
    50: (250,0,0),     # Built-up
    60: (180,180,180), # Bare/sparse
    70: (240,240,240), # Snow/ice
    80: (0,100,200),   # Water
    90: (0,150,160),   # Wetlands
    95: (0,207,117),   # Mangroves
    100:(250,230,160), # Moss/lichen
}

# ============================================================
# SMALL HELPERS
# ============================================================

def haversine_km(lat1, lon1, lat2, lon2):
    R = 6371.0088
    p = math.pi / 180.0
    a = 0.5 - math.cos((lat2-lat1)*p)/2 + math.cos(lat1*p)*math.cos(lat2*p)*(1-math.cos((lon2-lon1)*p))/2
    return 2 * R * math.asin(math.sqrt(a))

def compute_output_size_km_per_pixel():
    mid_lat = (north + south) / 2.0
    width_km  = haversine_km(mid_lat, west, mid_lat, east)
    height_km = haversine_km(north, west, south, west)
    out_w = max(16, int(round(width_km / KM_PER_PIXEL)))
    out_h = max(16, int(round(height_km / KM_PER_PIXEL)))
    return out_w, out_h, width_km, height_km

def get_with_retries(session, url, tries=5, timeout=60):
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
# DEM (Terrarium) helpers
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
    """
    tile_rgba: HxWx4 uint8
    alpha==0 -> nodata -> NaN
    """
    rgb = tile_rgba[..., :3].astype(np.float32)
    a   = tile_rgba[..., 3]
    r = rgb[..., 0]
    g = rgb[..., 1]
    b = rgb[..., 2]
    elev = (r * 256.0 + g + b / 256.0) - 32768.0
    elev = elev.astype(np.float32)
    elev[a == 0] = np.nan
    return elev

def fill_nans_iterative(arr, iters=6):
    """
    Fill NaNs by neighbor averaging. This is nodata repair, not smoothing “noise”.
    """
    out = arr.copy()
    for _ in range(iters):
        nan_mask = ~np.isfinite(out)
        if not nan_mask.any():
            break
        padded = np.pad(out, ((1,1),(1,1)), mode="edge")
        # 4-neighbor average where finite
        n = padded[0:-2,1:-1]
        s = padded[2:  ,1:-1]
        w = padded[1:-1,0:-2]
        e = padded[1:-1,2:  ]
        stack = np.stack([n,s,w,e], axis=0)
        finite = np.isfinite(stack)
        count = finite.sum(axis=0).astype(np.float32)
        sumv = np.where(finite, stack, 0.0).sum(axis=0)
        fill = np.where(count > 0, sumv / count, np.nan)
        out[nan_mask] = fill[nan_mask]
    # any remaining NaNs -> 0 (rare)
    out[~np.isfinite(out)] = 0.0
    return out

# ============================================================
# WorldCover tile selection (3x3 degree tiles)
# ============================================================

def wc_tile_id_for_latlon(lat, lon):
    sw_lat = math.floor(lat / 3.0) * 3
    sw_lon = math.floor(lon / 3.0) * 3
    lat_hem = "N" if sw_lat >= 0 else "S"
    lon_hem = "E" if sw_lon >= 0 else "W"
    return f"{lat_hem}{abs(int(sw_lat)):02d}{lon_hem}{abs(int(sw_lon)):03d}"

def needed_wc_tiles_for_bbox():
    # sample bbox corners just inside edges
    eps = 1e-9
    pts = [
        (south+eps, west+eps),
        (south+eps, east-eps),
        (north-eps, west+eps),
        (north-eps, east-eps),
    ]
    return sorted(set(wc_tile_id_for_latlon(lat, lon) for lat, lon in pts))

# ============================================================
# MAIN
# ============================================================

def main():
    out_w, out_h, width_km, height_km = compute_output_size_km_per_pixel()
    print("=== Attica DEM + WorldCover ===")
    print(f"bbox: N{north:.6f} W{west:.6f} S{south:.6f} E{east:.6f}")
    print(f"region approx: {width_km:.1f} km wide x {height_km:.1f} km tall")
    print(f"target: ~{KM_PER_PIXEL:.2f} km/px -> output {out_w}x{out_h}")
    print(f"DEM zoom: {DEM_ZOOM}")

    # ---------------- DEM: stitch tiles ----------------
    px0, py0 = latlon_to_pixel_xy(north, west, DEM_ZOOM)
    px1, py1 = latlon_to_pixel_xy(south, east, DEM_ZOOM)
    tx0, ty0 = pixel_xy_to_tile_xy(px0, py0)
    tx1, ty1 = pixel_xy_to_tile_xy(px1, py1)

    tiles_w = tx1 - tx0 + 1
    tiles_h = ty1 - ty0 + 1
    total = tiles_w * tiles_h
    print(f"DEM tiles: {tiles_w} x {tiles_h} = {total}")

    stitched = np.full((tiles_h * TILE_SIZE, tiles_w * TILE_SIZE), np.nan, dtype=np.float32)

    session = requests.Session()
    idx = 0
    for iy, ty in enumerate(range(ty0, ty1 + 1)):
        for ix, tx in enumerate(range(tx0, tx1 + 1)):
            idx += 1
            url = DEM_TILE_URL.format(z=DEM_ZOOM, x=tx, y=ty)
            print(f"[{idx}/{total}] DEM z{DEM_ZOOM}/{tx}/{ty}")
            r = get_with_retries(session, url, tries=5, timeout=60)

            # IMPORTANT: read RGBA, preserve alpha nodata
            tile = Image.open(BytesIO(r.content)).convert("RGBA")
            elev = decode_terrarium_rgba(np.array(tile, dtype=np.uint8))

            # clamp wild values (rare)
            elev = np.clip(elev, -12000.0, 9000.0)

            y0 = iy * TILE_SIZE
            x0 = ix * TILE_SIZE
            stitched[y0:y0+TILE_SIZE, x0:x0+TILE_SIZE] = elev

    # Crop with floor/ceil (avoid seam rounding artifacts)
    crop_x0 = int(math.floor(px0 - tx0 * TILE_SIZE))
    crop_y0 = int(math.floor(py0 - ty0 * TILE_SIZE))
    crop_x1 = int(math.ceil (px1 - tx0 * TILE_SIZE))
    crop_y1 = int(math.ceil (py1 - ty0 * TILE_SIZE))

    dem_crop = stitched[crop_y0:crop_y1, crop_x0:crop_x1]
    print(f"Cropped DEM px: {dem_crop.shape[1]} x {dem_crop.shape[0]}")

    # Fill nodata (alpha==0) BEFORE resampling to avoid specks
    nan_count = int((~np.isfinite(dem_crop)).sum())
    if nan_count:
        print(f"Filling DEM nodata pixels: {nan_count}")
        dem_crop = fill_nans_iterative(dem_crop, iters=8)

    # Resample DEM to output
    dem_img = Image.fromarray(dem_crop.astype(np.float32), mode="F").resize(
        (out_w, out_h), resample=Image.BILINEAR
    )
    dem = np.array(dem_img, dtype=np.float32)

    # Normalize using land-only percentiles to avoid sea flattening
    land = dem[dem > 0.0]
    lo = float(np.percentile(land, 0.5)) if land.size else float(np.min(dem))
    hi = float(np.percentile(land, 99.5)) if land.size else float(np.max(dem))
    print(f"Normalize (land p0.5..p99.5): lo={lo:.2f}m hi={hi:.2f}m")

    dem_clamped = np.clip(dem, lo, hi)
    norm = (dem_clamped - lo) / (hi - lo + 1e-9)

    h16 = np.clip(norm * 65535.0, 0, 65535).astype(np.uint16)
    Image.fromarray(h16, mode="I;16").save("attica_height_16bit.png")
    Image.fromarray((norm * 255.0).astype(np.uint8), mode="L").save("attica_height_preview_8bit.png")
    print("Saved: attica_height_16bit.png")
    print("Saved: attica_height_preview_8bit.png")

    # ---------------- WorldCover: mosaic needed tiles ----------------
    tiles = needed_wc_tiles_for_bbox()
    print(f"WorldCover tiles needed: {tiles}")

    srcs = []
    for t in tiles:
        url = WORLDCOVER_BASE.format(tile=t)
        print(f"Open WorldCover COG: {url}")
        srcs.append(rasterio.open(url))

    mosaic, mosaic_transform = merge(srcs)  # (bands, H, W)
    mosaic_arr = mosaic[0].astype(np.uint8)
    for s in srcs:
        s.close()

    # Reproject/crop mosaic to bbox at our output size (EPSG:4326)
    dst = np.zeros((out_h, out_w), dtype=np.uint8)
    dst_transform = from_bounds(west, south, east, north, out_w, out_h)

    reproject(
        source=mosaic_arr,
        destination=dst,
        src_transform=mosaic_transform,
        src_crs="EPSG:4326",
        dst_transform=dst_transform,
        dst_crs="EPSG:4326",
        resampling=Resampling.nearest
    )

    Image.fromarray(dst, mode="L").save("attica_landcover_worldcover_codes.png")
    print("Saved: attica_landcover_worldcover_codes.png")

    # Preview
    palette = np.zeros((256, 3), dtype=np.uint8)
    for code, rgb in WC_COLORS.items():
        palette[int(code)] = rgb
    preview = palette[dst]
    Image.fromarray(preview, mode="RGB").save("attica_landcover_preview.png")
    print("Saved: attica_landcover_preview.png")

    print("\nDone. Outputs:")
    print(" - attica_height_16bit.png")
    print(" - attica_height_preview_8bit.png")
    print(" - attica_landcover_worldcover_codes.png")
    print(" - attica_landcover_preview.png")

if __name__ == "__main__":
    main()
