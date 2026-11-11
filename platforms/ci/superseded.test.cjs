const assert = require('node:assert/strict');
const { decisions } = require('./superseded.cjs');
const branch = { id: 1, workflow_id: 10, event: 'push', head_branch: 'main', head_sha: 'abc', status: 'in_progress' };
const tag = { ...branch, id: 2, head_branch: 'v1.0' };
assert.deepEqual(decisions(tag, [branch, tag]), { skip: false, cancel: [1] });
assert.deepEqual(decisions(branch, [branch, tag]), { skip: true, cancel: [] });
for (const peer of [
    { ...branch, event: 'workflow_dispatch' },
    { ...branch, head_sha: 'newer' },
    { ...branch, workflow_id: 11 },
    { ...branch, head_branch: 'v0.9' },
    { ...branch, head_branch: null },
    { ...branch, status: 'completed', conclusion: 'success' }
]) assert.deepEqual(decisions(tag, [peer, tag]), { skip: false, cancel: [] });
for (const conclusion of ['failure', 'cancelled', 'timed_out', 'skipped']) {
    assert.equal(decisions(branch, [{ ...tag, status: 'completed', conclusion }]).skip, false);
}
assert.equal(decisions(branch, [{ ...tag, status: 'completed', conclusion: 'success' }]).skip, true);
assert.equal(decisions({ ...branch, event: 'workflow_dispatch' }, [tag]).skip, false);
assert.deepEqual(decisions(tag, [{ ...branch, head_branch: 'develop' }]).cancel, [1]);
assert.equal(decisions(branch, [branch]).skip, false);
console.log('PASS: release/branch cancellation decisions');
