"""Raster strategies behind the runtime builder; the reader renders its own reference."""

from PIL import Image, ImageDraw


def bake_baseline_cell(ch, font, px, cell_size, baseline, density):
    from PIL import Image, ImageDraw

    # Center the font's em frame once. Centering each cropped ink box would
    # discard the relative baseline bearings of shorter glyphs such as 用/同.
    canvas = Image.new("L", (px * 3, cell_size))
    ImageDraw.Draw(canvas).text((px, baseline), ch, font=font, fill=255, anchor="ls")
    bbox = canvas.getbbox()
    if bbox is None:
        raise ValueError(f"Source font lacks {ch}")
    ink_width = bbox[2] - bbox[0]
    fits_cell = (
        ink_width <= cell_size - 2 * density
        and bbox[1] >= density
        and bbox[3] <= cell_size - density
    )
    if not fits_cell:
        raise ValueError(f"Shared font baseline does not fit the cell for {ch}")
    cell = Image.new("L", (cell_size, cell_size))
    cell.paste(canvas.crop((bbox[0], 0, bbox[2], cell_size)), ((cell_size - ink_width) // 2, 0))
    # Bounds must describe the actual BC2 alpha, including erased edge pixels.
    cell = cell.point(lambda alpha: ((alpha * 15 + 127) // 255) * 17)
    cell = cell.transpose(Image.Transpose.FLIP_TOP_BOTTOM)
    return cell, cell.getbbox(), baseline


def padded_ink(ch, font, cell, font_px, baseline, native_baseline):
    density, margin = 2, font_px * 4
    canvas = Image.new("L", (margin * 2 + cell * 2, margin * 2 + cell * 2))
    ImageDraw.Draw(canvas).text(
        (margin, margin + baseline * 2), ch, font=font, fill=255, anchor="ls"
    )
    canvas = canvas.point(lambda alpha: ((alpha * 15 + 127) // 255) * 17)
    bbox = canvas.getbbox()
    if bbox is None:
        return None
    if bbox[0] <= 0 or bbox[1] <= 0 or bbox[2] >= canvas.width or bbox[3] >= canvas.height:
        raise ValueError("The measurement canvas clipped real ink")
    ink = canvas.crop(bbox)
    ink_left = (cell * 2 - ink.width) // 2
    ink_top = bbox[1] - margin
    left, top = ink_left // 2 * 2, ink_top // 2 * 2
    right, bottom = (ink_left + ink.width + 1) // 2 * 2, (ink_top + ink.height + 1) // 2 * 2
    sample = Image.new("L", (right - left, bottom - top))
    sample.paste(ink, (ink_left - left, ink_top - top))
    quad = [
        sample.width // density,
        sample.height // density,
        left // density,
        native_baseline - top // density,
        cell,
    ]
    return quad, sample
