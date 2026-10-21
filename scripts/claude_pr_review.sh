#!/usr/bin/env bash
# Static PR-review controller. Inputs are GitHub's documented environment and
# event JSON. Phase argument: prepare|review|publish. No user-defined shell code
# is evaluated. Temporary files belong to the runner and die with its job.
set -euo pipefail

# Fixed policy constants; environment cannot extend the authorized identity.
OwnerId=76451541
RepositoryId=1404691070
MaxDiffBytes=4194304
MaxFileBytes=8388608
MaxSnapshotBytes=67108864
MaxCommentBytes=50000
script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
review_dir="${RUNNER_TEMP:?}/waddle_claude_review"
input_dir="$review_dir/input"

# @param[in] message Non-null diagnostic. No ownership transfer; single process.
# @return Never returns; exits with status 1 without printing PR content/secrets.
fail() {
    echo "::error::$1" >&2
    exit 1
}

# @return 0 only for the fixed owner/repository and a new accepted request.
# Reads event JSON and environment; no network, allocation ownership, or writes.
authorize_request() {
    [[ "${GITHUB_REPOSITORY_ID:-}" == "$RepositoryId" &&
       "${GITHUB_ACTOR_ID:-}" == "$OwnerId" &&
       "${GITHUB_TRIGGERING_ACTOR:-}" == "${GITHUB_ACTOR:?}" &&
       "${GITHUB_RUN_ATTEMPT:-}" == 1 ]] || fail 'Request is not owner-authorized.'
    jq -e --argjson owner_id "$OwnerId" --arg event_name "$GITHUB_EVENT_NAME" '
        .sender.id == $owner_id and
        ((.action == "ready_for_review" and $event_name == "pull_request_target"
          and (.pull_request.number | type == "number")) or
         (.action == "created" and $event_name == "issue_comment"
          and .issue.pull_request != null and .comment.user.id == $owner_id
          and .comment.body == "/claude-review"))
    ' "$GITHUB_EVENT_PATH" >/dev/null || fail 'Unsupported review request.'
}

# @return 0 if live PR state, head, base, and target still match request.json;
# 1 on stale state or API error. Files are caller-owned; GH_TOKEN is required.
check_current_pr() {
    local pr_number
    pr_number="$(jq -er '.number' "$input_dir/request.json")"
    gh api "repos/$GITHUB_REPOSITORY/pulls/$pr_number" > "$review_dir/current.json" || return 1
    jq -e --slurpfile request "$input_dir/request.json" \
        --argjson repository_id "$RepositoryId" '
        .state == "open" and .draft == false and
        .base.repo.id == $repository_id and .number == $request[0].number and
        .head.sha == $request[0].head_sha and .base.sha == $request[0].base_sha and
        .base.ref == $request[0].base_ref
    ' "$review_dir/current.json" >/dev/null
}

# @return 0 after immutable input creation, otherwise exits nonzero before spend.
# Requires GH_TOKEN; consumes no Anthropic credential. Writes only runner files
# and fetched refs in the trusted checkout. All paths/blobs remain static data.
prepare_review() {
    authorize_request
    local pr_number head_sha base_sha merge_base entry metadata file_path
    local file_mode object_type object_sha file_bytes total_bytes=0
    [[ ! -e "$review_dir" ]] || fail 'Review directory already exists.'
    mkdir -p "$input_dir/source" "$input_dir/policy"
    pr_number="$(jq -er '.pull_request.number // .issue.number | select(. > 0 and floor == .)' "$GITHUB_EVENT_PATH")"
    gh api "repos/$GITHUB_REPOSITORY/pulls/$pr_number" > "$input_dir/metadata.json"
    jq -e --argjson repository_id "$RepositoryId" '
        .state == "open" and .draft == false and .base.repo.id == $repository_id and
        (.head.sha | test("^[0-9a-f]{40}$")) and (.base.sha | test("^[0-9a-f]{40}$"))
    ' "$input_dir/metadata.json" >/dev/null || fail 'PR must be open and ready with valid commits.'
    jq '{number, head_sha: .head.sha, base_sha: .base.sha, base_ref: .base.ref}' \
        "$input_dir/metadata.json" > "$input_dir/request.json"
    if [[ "$GITHUB_EVENT_NAME" == pull_request_target ]]; then
        jq -e --slurpfile request "$input_dir/request.json" '
            .pull_request.head.sha == $request[0].head_sha and
            .pull_request.base.sha == $request[0].base_sha and
            .pull_request.base.ref == $request[0].base_ref
        ' "$GITHUB_EVENT_PATH" >/dev/null || fail 'PR changed since the ready request.'
    fi
    head_sha="$(jq -er '.head_sha' "$input_dir/request.json")"
    base_sha="$(jq -er '.base_sha' "$input_dir/request.json")"
    git -c core.hooksPath=/dev/null fetch --no-tags --no-recurse-submodules origin \
        "$base_sha" "+refs/pull/$pr_number/head:refs/waddle_review/head"
    [[ "$(git rev-parse refs/waddle_review/head)" == "$head_sha" ]] || fail 'PR changed while fetching.'
    merge_base="$(git merge-base "$base_sha" "$head_sha")"
    git diff --no-ext-diff --no-textconv --find-renames "$merge_base" "$head_sha" -- \
        > "$input_dir/changes.diff"
    [[ "$(wc -c < "$input_dir/changes.diff")" -le "$MaxDiffBytes" ]] || fail 'Diff exceeds 4 MiB; split this PR before review.'
    git ls-tree --full-tree -rlz "$head_sha" > "$review_dir/tree"
    : > "$input_dir/omitted.txt"
    while IFS= read -r -d '' entry; do
        metadata="${entry%%$'\t'*}"
        file_path="${entry#*$'\t'}"
        read -r file_mode object_type object_sha file_bytes <<< "$metadata"
        case "/$file_path/" in
            *'/../'*|*'/./'*|*'/.git/'*|//* ) fail 'Unsafe Git tree path.' ;;
        esac
        if [[ "$object_type" != blob || "$file_mode" == 120000 ||
              "$file_bytes" -gt "$MaxFileBytes" ||
              "$((total_bytes + file_bytes))" -gt "$MaxSnapshotBytes" ]]; then
            printf '%s\t%q\n' "$file_mode" "$file_path" >> "$input_dir/omitted.txt"
            continue
        fi
        mkdir -p -- "$(dirname -- "$input_dir/source/$file_path")"
        git cat-file blob "$object_sha" > "$input_dir/source/$file_path"
        total_bytes=$((total_bytes + file_bytes))
    done < "$review_dir/tree"
    for file_path in AGENTS.md PROJECT.md CONTRIBUTING.md; do
        if [[ -f "$file_path" ]]; then
            cp -- "$file_path" "$input_dir/policy/$file_path"
        fi
    done
    check_current_pr || fail 'PR changed during preparation; request a new review.'
}

# @return CLI exit status; absence/invalid completion is handled by publication.
# Requires ANTHROPIC_API_KEY. Only the vendor process receives that credential;
# its minimal environment excludes GH_TOKEN and Actions command/event file paths.
run_review() {
    [[ -s "$input_dir/request.json" ]] || fail 'Review input is missing.'
    [[ -n "${ANTHROPIC_API_KEY:-}" ]] || fail 'ANTHROPIC_API_KEY secret is missing.'
    cd -- "$input_dir"
    env -i PATH="$PATH" HOME="$HOME" LANG=C.UTF-8 \
        ANTHROPIC_API_KEY="$ANTHROPIC_API_KEY" \
        CLAUDE_CONFIG_DIR="$review_dir/claude_config" \
        DISABLE_AUTOUPDATER=1 CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC=1 \
        claude --bare --restricted --disable-slash-commands \
        --strict-mcp-config --mcp-config '{"mcpServers":{}}' \
        --tools 'Read,Glob,Grep' --allowedTools 'Read,Glob,Grep' \
        --permission-prompts none --no-session-persistence \
        --model sonnet --max-turns 25 --max-budget-usd 3 --output-format json \
        --append-system-prompt "$(cat "$script_dir/claude_pr_review_prompt.txt")" \
        -p 'Review this request. Read request.json, metadata.json, changes.diff, omitted.txt, and policy/. Inspect source/ as needed. Return the specified Markdown review.' \
        > "$review_dir/result.json"
    # Keep the Actions run failed for soft errors reported with a zero exit code.
    validate_result || fail 'Claude did not produce a complete, valid review.'
    touch "$review_dir/complete"
}

# @return 0 only for completed, bounded Markdown with exactly one known verdict.
# Reads vendor JSON using jq; never evaluates model output as commands.
validate_result() {
    jq -e --argjson max_bytes "$MaxCommentBytes" '
        .subtype == "success" and .is_error == false and
        (.result | type == "string" and utf8bytelength > 0 and utf8bytelength <= $max_bytes)
    ' "$review_dir/result.json" >/dev/null 2>&1 || return 1
    jq -er '.result' "$review_dir/result.json" > "$review_dir/body.md"
    [[ "$(grep -Ec '^Verdict:' "$review_dir/body.md")" == 1 ]] || return 1
    grep -Eq '^Verdict: (NEEDS CHANGES|QUESTIONS|NO BLOCKERS FOUND|INCOMPLETE)$' "$review_dir/body.md"
}

# @return 0 on published comment or stale skip; API failures return nonzero.
# Requires GH_TOKEN only. Rechecks owner policy and live PR; posts exactly once.
publish_review() {
    authorize_request
    if ! check_current_pr; then
        echo 'Review withheld: PR changed, is no longer ready, or could not be verified. Request again.'
        return 0
    fi
    if [[ ! -f "$review_dir/complete" ]] || ! validate_result; then
        cat > "$review_dir/body.md" <<'EOF'
Verdict: INCOMPLETE

### Findings
No completed review is available. Do not interpret this as approval.

### Questions
None.

### Validation and limits
Claude failed, reached a limit, or returned invalid output. Check the Actions run
before posting a new `/claude-review` request. Tests were not run by this reviewer.
EOF
    fi
    {
        echo '## Claude PR review'
        # shellcheck disable=SC2016 # Markdown backticks are literal data.
        printf 'Reviewed head: `%s`\n\n' "$(jq -er '.head_sha' "$input_dir/request.json")"
        # shellcheck disable=SC2016 # Markdown backticks are literal data.
        printf 'Reviewed base: `%s`\n\n' "$(jq -er '.base_sha' "$input_dir/request.json")"
        printf '[Actions run](%s/%s/actions/runs/%s) · Static, advisory review\n\n' \
            "$GITHUB_SERVER_URL" "$GITHUB_REPOSITORY" "$GITHUB_RUN_ID"
        cat "$review_dir/body.md"
    } > "$review_dir/comment.md"
    jq -n --rawfile body "$review_dir/comment.md" '{body: $body}' > "$review_dir/comment.json"
    gh api --method POST "repos/$GITHUB_REPOSITORY/issues/$(jq -er '.number' "$input_dir/request.json")/comments" \
        --input "$review_dir/comment.json" --silent
}

case "${1:-}" in
    prepare) prepare_review ;;
    review) run_review ;;
    publish) publish_review ;;
    *) fail 'Expected phase: prepare, review, or publish.' ;;
esac
