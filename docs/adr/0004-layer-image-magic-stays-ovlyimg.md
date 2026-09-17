# The layer-image magic stays `OVLYIMG\0`

The project was renamed from OverlayFS to LayerMount, but the `.lmnt` layer image is a public on-disk format and images already exist. The first eight bytes stay `OVLYIMG\0`. A new magic would break every existing image or need a dual-magic reader for no gain in function.
