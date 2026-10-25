//! Public bound and ABI smoke tests; transaction tests live with the source
//! module because they exercise private recovery checkpoints directly.
const std = @import("std");
const manifest_store = @import("manifest_store");
test "manifest store exposes bounded journal and root limits" {
    try std.testing.expectEqual(@as(usize, 3), manifest_store.MaxRoots);
    try std.testing.expectEqual(@as(usize, 32), manifest_store.MaxJournals);
    try std.testing.expectEqual(@as(usize, 4 * 1024 * 1024), manifest_store.MaxJournalBytes);
    try std.testing.expectEqual(@as(usize, 32 * 1024 * 1024), manifest_store.MaxArtifactBytes);
    try std.testing.expect(@hasDecl(manifest_store.store_t, "init"));
    try std.testing.expect(@hasDecl(manifest_store.store_t, "commit"));
    try std.testing.expect(@hasDecl(manifest_store.store_t, "replace"));
    try std.testing.expect(@hasDecl(manifest_store.store_t, "restore"));
    try std.testing.expect(@hasDecl(manifest_store.store_t, "recover"));
    try std.testing.expect(@hasDecl(manifest_store.store_t, "deinit"));
    try std.testing.expect(@hasDecl(manifest_store, "artifact_identity_t"));
    try std.testing.expect(@hasDecl(manifest_store, "backup_ref_t"));
}
