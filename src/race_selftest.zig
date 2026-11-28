//! JOG-233: host unit test for runtime/jogrt_race.c vector-clock logic.
const std = @import("std");

export fn jogrt_panic(ptr: ?*const anyopaque, len: i64) void {
    _ = ptr;
    _ = len;
    @panic("jogrt_race unexpected panic during selftest");
}

export fn jogrt_coro_id() i64 {
    return 0;
}

extern fn jogrt_race_selftest() i64;

test "race detector selftest" {
    try std.testing.expectEqual(@as(i64, 0), jogrt_race_selftest());
}
