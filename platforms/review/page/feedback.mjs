// Older verdicts also tell us that a reviewer has seen the screen.
export function wasSeen(entry) {
    return entry?.seen ?? Boolean(entry?.verdict || hasAnnotations(entry));
}
export function hasAnnotations(entry) {
    return Boolean(entry?.note?.trim() || entry?.pins?.length);
}
export function reviewVerdict(entry) {
    if (hasAnnotations(entry) || (entry?.seen === undefined &&
        ['rejected', 'needs-changes'].includes(entry?.verdict))) return 'needs-changes';
    return wasSeen(entry) ? 'approved' : null;
}
export function completeFeedback(entry) {
    const result = { ...entry, seen: true };
    return { ...result, verdict: reviewVerdict(result) };
}
