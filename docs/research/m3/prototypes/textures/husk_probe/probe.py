import OpenImageIO as oiio, os, sys
print("PROBE OIIO", oiio.VERSION_STRING if hasattr(oiio,"VERSION_STRING") else oiio.__version__)
print("PROBE OCIO env", os.environ.get("OCIO"))
print("PROBE OPENIMAGEIO_IMAGECACHE_OPTIONS", os.environ.get("OPENIMAGEIO_IMAGECACHE_OPTIONS"))
print("PROBE OPENIMAGEIO_TEXTURE_OPTIONS", os.environ.get("OPENIMAGEIO_TEXTURE_OPTIONS"))
ic = oiio.ImageCache(True)
for n in ["max_memory_MB","max_open_files","autotile","autoscanline","automip","forcefloat","accept_untiled","accept_unmipped","failure_retries","deduplicate","colorspace","colorconfig","trust_file_extensions","max_errors_per_file","max_mip_res","unassociatedalpha","searchpath"]:
    try:
        print("PROBE ic", n, "=", ic.getattribute(n))
    except Exception as e:
        print("PROBE ic", n, "ERR", e)
ts = oiio.TextureSystem(True)
for n in ["gray_to_rgb","flip_t","max_tile_channels","latlong_up","stochastic","max_mip_res"]:
    try:
        print("PROBE ts", n, "=", ts.getattribute(n))
    except Exception as e:
        print("PROBE ts", n, "ERR", e)
