// Only an automatic branch build of the same commit is redundant with a tag.
// Manual runs are explicit requests, and release runs must never cancel each other.
function decisions(current, runs) {
    const branchPush = run => run.event === 'push' && ['main', 'develop'].includes(run.head_branch);
    const tagPush = run => run.event === 'push' && typeof run.head_branch === 'string' && run.head_branch.startsWith('v');
    const active = run => ['queued', 'in_progress', 'waiting', 'pending', 'requested'].includes(run.status);
    const peers = runs.filter(run => run.id !== current.id && run.head_sha === current.head_sha
        && run.workflow_id === current.workflow_id);
    if (tagPush(current)) {
        return { skip: false, cancel: peers.filter(run => branchPush(run) && active(run)).map(run => run.id) };
    }
    return {
        skip: branchPush(current) && peers.some(run => tagPush(run)
            && (active(run) || run.conclusion === 'success')),
        cancel: []
    };
}

async function run({ github, context, core }) {
    const repo = context.repo;
    const { data: current } = await github.rest.actions.getWorkflowRun({ ...repo, run_id: context.runId });
    // workflow_dispatch is never a release trigger, even when it targets a tag.
    if (context.eventName !== 'push') {
        core.setOutput('proceed', 'true');
        return;
    }
    const runs = await github.paginate(github.rest.actions.listWorkflowRuns, {
        ...repo, workflow_id: current.workflow_id, head_sha: current.head_sha, per_page: 100
    });
    const result = decisions(current, runs);
    for (const run_id of result.cancel) {
        try {
            await github.rest.actions.cancelWorkflowRun({ ...repo, run_id });
            core.info(`Cancelled redundant branch run ${run_id} for ${current.head_sha}`);
        } catch (error) {
            // A run can finish between listing it and requesting cancellation.
            if (error.status !== 409) throw error;
        }
    }
    if (result.skip) core.info('A release run already covers this commit; skipping the branch build.');
    core.setOutput('proceed', String(!result.skip));
}

module.exports = { decisions, run };
