# Nova viewport check, run in a separate Houdini instance (the user's own
# session is left alone). Loaded through HOUDINI_PATH only for this instance.
import os

if os.environ.get("NOVA_VIEWPORT_CHECK"):
    import nova_viewport_check

    nova_viewport_check.start()
