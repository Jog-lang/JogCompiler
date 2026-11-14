const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const exe_mod = b.createModule(.{
        .root_source_file = b.path("src/main.zig"),
        .target = target,
        .optimize = optimize,
    });

    const exe = b.addExecutable(.{
        .name = "jog",
        .root_module = exe_mod,
    });
    b.installArtifact(exe);

    // JOG-97: tiny C ABI façade (one export) for Jog `@link` / future in-process tools.
    // Zig tools (JogLSP, JogBuild) should prefer Zig modules over this; see docs note.
    const abi_mod = b.createModule(.{
        .root_source_file = b.path("src/capi/capi.zig"),
        .target = target,
        .optimize = optimize,
    });
    const abi_lib = b.addLibrary(.{
        .name = "jogc_abi",
        .linkage = .dynamic,
        .root_module = abi_mod,
    });
    b.installArtifact(abi_lib);

    const run_cmd = b.addRunArtifact(exe);
    run_cmd.step.dependOn(b.getInstallStep());
    if (b.args) |args| {
        run_cmd.addArgs(args);
    }
    const run_step = b.step("run", "Run jog");
    run_step.dependOn(&run_cmd.step);

    const unit_tests = b.addTest(.{
        .root_module = exe_mod,
    });
    const run_unit_tests = b.addRunArtifact(unit_tests);
    const test_step = b.step("test", "Run host compiler tests");
    test_step.dependOn(&run_unit_tests.step);
}
