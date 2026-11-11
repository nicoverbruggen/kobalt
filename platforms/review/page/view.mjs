// Pure rules of the review page: device bezels, zoom stops and baselines.
// The page and the tool checks both import them.

// Kobalt devices, in 300 PPI pixels, with the same geometry as the screenshot
// gallery's viewer frames. The screen opening matches the capture size. Only
// the frame turns in landscape; the capture stays upright.
export const bezels = {
    'six-inch': { width: 1323, height: 1890, screen: { x: 126, y: 150, width: 1072, height: 1448 } },
    'six-inch-landscape': { width: 1323, height: 1890, screen: { x: 126, y: 150, width: 1072, height: 1448 } },
    'seven-inch': { width: 1708, height: 1902, screen: { x: 90, y: 111, width: 1264, height: 1680 } },
    'phone': { width: 800, height: 1680, screen: { x: 40, y: 40, width: 720, height: 1600 } },
    // Android letterboxes the camera side in landscape; the bands are frame.
    'phone-landscape': { width: 800, height: 1680, screen: { x: 40, y: 100, width: 720, height: 1480 } },
};
export const bezelRadius = 48;

// The device outline around a capture, or null without a known device.
export function bezel(image) {
    const model = bezels[image.device];
    if (!model) return null;
    if (image.width <= image.height) return { width: model.width, height: model.height, screen: model.screen };
    const { x, y, width, height } = model.screen;
    return { width: model.height, height: model.width, screen: { x: model.height - y - height, y: x, width: height, height: width } };
}

// The zoom stops: these steps and Fit. A step within 1% of Fit is Fit.
export const zoomStepScales = [.5, .67, .75, .9, 1, 1.25, 1.5, 2];
export function zoomSteps(fit) {
    const stops = zoomStepScales.filter(step => Math.abs(step - fit) > .01).map(step => ({ scale: step, fit: false }));
    stops.push({ scale: fit, fit: true });
    return stops.sort((a, b) => a.scale - b.scale);
}
// The stop to show for a scale; Fit when no zoom is chosen.
export function zoomIndex(stops, scale, fitChosen) {
    if (fitChosen) return stops.findIndex(stop => stop.fit);
    let nearest = 0;
    stops.forEach((stop, index) => {
        if (Math.abs(stop.scale - scale) < Math.abs(stops[nearest].scale - scale)) nearest = index;
    });
    return nearest;
}

// A baseline is the original screen before the feature ('before') or the
// previous round ('previous').
export function hasBaseline(item, target) {
    if (target === 'before') return Boolean(item.before || item.referenceBefore);
    if (target === 'previous') return Boolean(item.previous);
    return false;
}
// Whether the latest images differ from the baseline. Captures of the same
// pixels have the same checksum, so this needs no image data. A device that
// the baseline lacks counts as a difference.
export function differsFromBaseline(item, target) {
    if (!hasBaseline(item, target)) return false;
    const pairs = target === 'before'
        ? [[item.image, item.before], [item.reference, item.referenceBefore]]
        : [[item.image, item.previous], [item.reference, item.previousReference]];
    return pairs.some(([latest, base]) => latest && base?.sha !== latest.sha);
}
