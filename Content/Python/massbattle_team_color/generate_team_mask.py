"""Offline BaseColor -> dominant-colour TeamMask. Requires numpy and Pillow."""
import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image


def oklab(rgb):
    linear = np.where(rgb <= .04045, rgb / 12.92, ((rgb + .055) / 1.055) ** 2.4)
    lms = linear @ np.array([[.4122214708, .2119034982, .0883024619],
                             [.5363325363, .6806995451, .2817188376],
                             [.0514459929, .1073969566, .6299787005]])
    return np.cbrt(lms) @ np.array([[.2104542553, 1.9779984951, .0259040371],
                                    [.7936177850, -2.4285922050, .7827717662],
                                    [-.0040720468, .4505937099, -.8086757660]])


def generate(source, destination, tolerance=.065):
    """Find the largest colour cluster, ignoring transparent pixels and extreme light/dark.

    Chroma determines similarity; reduced lightness weight retains painted shadows.
    The result is a linear grayscale data texture, not a replacement BaseColor.
    """
    source, destination = Path(source).resolve(), Path(destination).resolve()
    rgba = np.asarray(Image.open(source).convert('RGBA'), dtype=np.float32) / 255
    rgb, alpha = rgba[..., :3], rgba[..., 3]
    lab = oklab(rgb)
    small = np.asarray(Image.fromarray((rgba * 255).astype('uint8')).resize((256, 256)), dtype=np.float32) / 255
    samples = oklab(small[..., :3]).reshape(-1, 3)
    eligible = (small[..., 3].ravel() > .5) & (samples[:, 0] > .40) & (samples[:, 0] < .94)
    samples = samples[eligible]
    if not len(samples):
        destination = Path(destination)
        destination.parent.mkdir(parents=True, exist_ok=True)
        Image.new('L', (rgba.shape[1], rgba.shape[0])).save(destination)
        return {'source': str(source), 'mask': str(destination), 'dominant_rgb': None,
                'coverage': 0, 'reason': 'No opaque paint midtones; preserve this texture',
                'tolerance': tolerance, 'size': [rgba.shape[1], rgba.shape[0]]}
    # Coarse perceptual histogram supplies a deterministic, population-based seed.
    coords = np.floor((samples + [0, .4, .4]) / [.12, .035, .035]).astype(int)
    bins, counts = np.unique(coords, axis=0, return_counts=True)
    winner = bins[np.argmax(counts)]
    center = np.median(samples[np.all(coords == winner, axis=1)], axis=0)
    distance = np.linalg.norm((lab - center) * [.18, 1, 1], axis=-1)
    weight = np.clip((tolerance - distance) / (tolerance * .5), 0, 1)
    weight = weight * weight * (3 - 2 * weight)
    # Deep rubber/metal crevices must not be lifted into bright faction paint.
    weight *= np.clip((lab[..., 0] - .16) / .16, 0, 1) * alpha
    mask = np.round(weight * 255).astype('uint8')
    destination = Path(destination)
    destination.parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray(mask, 'L').save(destination)
    representative = rgb[np.unravel_index(np.argmin(np.linalg.norm(lab - center, axis=-1) + (1-alpha)*10), alpha.shape)]
    return {'source': str(source), 'mask': str(destination), 'dominant_rgb': np.round(representative*255).astype(int).tolist(),
            'dominant_oklab': center.tolist(), 'coverage': float(weight.mean()), 'tolerance': tolerance,
            'size': list(mask.shape[::-1])}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path, help='Image, or exported materials JSON')
    parser.add_argument('output', type=Path)
    parser.add_argument('--tolerance', type=float, default=.065)
    args = parser.parse_args()
    if not 0 < args.tolerance < .5:
        parser.error('tolerance must be between 0 and 0.5')
    if args.source.suffix.lower() == '.json':
        data = json.loads(args.source.read_text(encoding='utf-8-sig'))
        for row in data['textures']:
            row.update(generate(row['source'], args.output / (row['id'] + '_TeamMask.png'), args.tolerance))
        args.output.mkdir(parents=True, exist_ok=True)
        (args.output / 'masks.json').write_text(json.dumps(data, indent=2), encoding='utf-8')
    else:
        print(json.dumps(generate(args.source, args.output, args.tolerance), indent=2))


if __name__ == '__main__':
    main()
