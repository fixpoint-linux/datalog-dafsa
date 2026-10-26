//! dl_mod.zig — facade entry point so consumers can import the datalog engine
//! as a SINGLE Zig module, the way ../dhall-c/zig/src/dhall_mod.zig does for
//! the Dhall core.  The ported modules import each other by bare filename
//! (`@import("parser.zig")`), which resolves relative to the importing file, so
//! a consumer only has to root its module HERE.
//!
//! hybrid.zig is referenced for its side effect: it pulls in every ported
//! module so their `export fn`s are present in the consumer's binary, which is
//! what the C-ABI declarations in src/*.h resolve against.
const std = @import("std");

comptime {
    _ = @import("hybrid.zig");
}

pub const dl = @import("dl.zig");
pub const wfs = @import("wfs.zig");
pub const parser = @import("parser.zig");
pub const compiler = @import("compiler.zig");
pub const relation = @import("relation.zig");
pub const intern = @import("intern.zig");
