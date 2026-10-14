const std = @import("std");
const c = std.c;
const protocol = @import("protocol.zig");
const Health = @import("Health.zig");

const Connection = @This();

/// Connection configuration for opening a UDP socket to the FPGA.
pub const Config = struct {
    host: []const u8,
    port: u16 = 32100,
    mtu: u16 = 1500,
    rgb_mode: protocol.RgbMode = .bgr888,
    sound_rate: protocol.SoundRate = .off,
    sound_channels: protocol.SoundChannels = .off,
    compressor: ?Compressor = null,
    lz4_mode: protocol.Lz4Mode = .off,
    /// Only used when `lz4_mode == .nlc`.
    nlc: protocol.NlcParams = .{},
};

/// Result of a compression operation, including whether delta encoding was used.
pub const CompressResult = struct {
    data: []const u8,
    is_delta: bool,
};

/// Optional LZ4 compressor passed as a function pointer + context.
pub const Compressor = struct {
    ctx: ?*anyopaque,
    buf: []u8,
    compressFn: *const fn (ctx: ?*anyopaque, src: []const u8, dst: []u8, field: u8) ?CompressResult,

    pub fn compress(self: Compressor, src: []const u8, field: u8) ?CompressResult {
        return self.compressFn(self.ctx, src, self.buf, field);
    }
};

/// Per-frame metadata sent with CMD_BLIT.
pub const FrameOpts = struct {
    frame_num: u32,
    field: u8 = 0,
    vsync_line: u16 = 0,
};

/// Errors that can occur during socket operations.
pub const Error = error{
    SocketCreateFailed,
    ResolveFailed,
    SendFailed,
    SetSendBufFailed,
    AudioTooLarge,
    CompressFailed,
};

// --- State ---
sock: c.fd_t,
dest_addr: std.posix.sockaddr.in,
config: Config,
status: protocol.FpgaStatus = .{},
health: Health = .{},
recv_buf: [64]u8 = undefined,
mtu: u16,

/// Parse a dotted-decimal IPv4 string into a network-order sockaddr.in.
fn parseIp4(host: []const u8, port: u16) !std.posix.sockaddr.in {
    var octets: [4]u8 = .{0} ** 4;
    var idx: usize = 0;
    var cur: u32 = 0;
    var has_digit: bool = false;
    for (host) |ch| {
        if (ch == '.') {
            if (idx >= 3 or !has_digit) return error.InvalidAddress;
            octets[idx] = @intCast(cur);
            idx += 1;
            cur = 0;
            has_digit = false;
        } else if (ch >= '0' and ch <= '9') {
            cur = cur * 10 + (ch - '0');
            if (cur > 255) return error.InvalidAddress;
            has_digit = true;
        } else return error.InvalidAddress;
    }
    if (idx != 3 or !has_digit) return error.InvalidAddress;
    octets[idx] = @intCast(cur);
    // Store octets as little-endian u32; on LE host this puts bytes in network order.
    const addr: u32 = std.mem.readInt(u32, &octets, .little);
    const port_ne = std.mem.nativeToBig(u16, port);
    return std.posix.sockaddr.in{
        .family = c.AF.INET,
        .port = port_ne,
        .addr = addr,
        .zero = .{0} ** 8,
    };
}

/// Create a non-blocking UDP socket and resolve the destination address.
pub fn open(config: Config) Error!Connection {
    const dest_addr = parseIp4(config.host, config.port) catch return Error.ResolveFailed;

    const sock = c.socket(c.AF.INET, c.SOCK.DGRAM, c.IPPROTO.UDP);
    if (sock < 0) return Error.SocketCreateFailed;
    errdefer _ = c.close(sock);
    if (c.fcntl(sock, c.F.SETFL, @as(u32, @bitCast(c.O{ .NONBLOCK = true }))) < 0)
        return Error.SocketCreateFailed;

    const send_buf_size: u32 = 2 * 1024 * 1024;
    if (c.setsockopt(sock, c.SOL.SOCKET, c.SO.SNDBUF,
            &send_buf_size, @sizeOf(u32)) < 0)
        return Error.SetSendBufFailed;

    return .{
        .sock = sock,
        .dest_addr = dest_addr,
        .config = config,
        .mtu = config.mtu - 28,
    };
}

/// Send CMD_CLOSE and close the socket. Always safe to call.
pub fn close(self: *Connection) void {
    var buf: [1]u8 = undefined;
    protocol.buildClosePacket(&buf);
    const dest: *const c.sockaddr = @ptrCast(&self.dest_addr);
    _ = c.sendto(self.sock, &buf, buf.len, 0, dest, @sizeOf(std.posix.sockaddr.in));
    _ = c.close(self.sock);
    self.* = undefined;
}

/// Drain all pending ACK packets from the socket. Non-blocking.
pub fn poll(self: *Connection) void {
    while (true) {
        const n = c.recvfrom(self.sock, &self.recv_buf, self.recv_buf.len, 0, null, null);
        if (n < 0) return;
        if (@as(usize, @intCast(n)) >= protocol.ack_size) {
            self.status = protocol.parseAck(self.recv_buf[0..protocol.ack_size]);
        }
    }
}

/// Send CMD_INIT to start the streaming session.
pub fn sendInit(self: *Connection) Error!void {
    var buf: [5]u8 = undefined;
    if (self.config.lz4_mode == .nlc) {
        protocol.buildInitPacketNlc(&buf, self.config.nlc, self.config.sound_rate,
                                    self.config.sound_channels, self.config.rgb_mode);
    } else {
        protocol.buildInitPacket(&buf, self.config.lz4_mode, self.config.sound_rate,
                                 self.config.sound_channels, self.config.rgb_mode);
    }
    try self.sendRaw(&buf);
}

/// Send an already-compressed frame (NLC): 12-byte blit header carrying the
/// compressed size, then the payload in MTU-sized chunks.
pub fn sendPrecompressed(self: *Connection, data: []const u8, opts: FrameOpts) Error!void {
    var header: [12]u8 = undefined;
    protocol.buildBlitHeaderLz4(&header, opts.frame_num, opts.field,
                                opts.vsync_line, @intCast(data.len));
    try self.sendRaw(&header);
    var offset: usize = 0;
    while (offset < data.len) {
        const end = @min(offset + self.mtu, data.len);
        try self.sendRaw(data[offset..end]);
        offset = end;
    }
}

/// Send CMD_SWITCHRES to change the display modeline.
pub fn switchRes(self: *Connection, modeline: protocol.Modeline) Error!void {
    var buf: [26]u8 = undefined;
    protocol.buildSwitchResPacket(&buf, modeline);
    try self.sendRaw(&buf);
}

/// Send a frame to the FPGA. Compresses if a compressor is configured.
pub fn sendFrame(self: *Connection, frame: []const u8, opts: FrameOpts) Error!void {
    if (self.config.compressor) |comp| {
        const result = comp.compress(frame, opts.field) orelse return Error.CompressFailed;
        if (result.is_delta) {
            var header: [13]u8 = undefined;
            protocol.buildBlitHeaderLz4Delta(&header, opts.frame_num, opts.field,
                                             opts.vsync_line, @intCast(result.data.len));
            try self.sendRaw(&header);
        } else {
            var header: [12]u8 = undefined;
            protocol.buildBlitHeaderLz4(&header, opts.frame_num, opts.field,
                                        opts.vsync_line, @intCast(result.data.len));
            try self.sendRaw(&header);
        }
        var offset: usize = 0;
        while (offset < result.data.len) {
            const end = @min(offset + self.mtu, result.data.len);
            try self.sendRaw(result.data[offset..end]);
            offset = end;
        }
    } else {
        var header: [8]u8 = undefined;
        protocol.buildBlitHeader(&header, opts.frame_num, opts.field, opts.vsync_line);
        try self.sendRaw(&header);
        var offset: usize = 0;
        while (offset < frame.len) {
            const end = @min(offset + self.mtu, frame.len);
            try self.sendRaw(frame[offset..end]);
            offset = end;
        }
    }
}

/// Send CMD_AUDIO header + PCM data in MTU-sized chunks.
pub fn sendAudio(self: *Connection, pcm: []const u8) Error!void {
    if (pcm.len == 0) return;
    if (pcm.len > std.math.maxInt(u16)) return Error.AudioTooLarge;
    const sample_bytes: u16 = @intCast(pcm.len);
    var header: [3]u8 = undefined;
    protocol.buildAudioHeader(&header, sample_bytes);
    try self.sendRaw(&header);
    var offset: usize = 0;
    while (offset < pcm.len) {
        const end = @min(offset + self.mtu, pcm.len);
        try self.sendRaw(pcm[offset..end]);
        offset = end;
    }
}

pub fn fpgaStatus(self: *const Connection) protocol.FpgaStatus {
    return self.status;
}

pub fn pollStatus(self: *Connection) protocol.FpgaStatus {
    self.poll();
    return self.status;
}

pub fn getHealth(self: *const Connection) Health {
    return self.health;
}

/// Send CMD_GET_STATUS then block until the FPGA responds or timeout.
pub fn waitSync(self: *Connection, timeout_ms: i32) bool {
    self.sendGetStatus();
    var fds = [1]std.posix.pollfd{.{
        .fd = self.sock,
        .events = std.posix.POLL.IN,
        .revents = 0,
    }};
    const ready = std.posix.poll(&fds, timeout_ms) catch return false;
    if (ready > 0) {
        self.poll();
        return true;
    }
    return false;
}

fn sendGetStatus(self: *Connection) void {
    var buf: [1]u8 = .{@intFromEnum(protocol.Command.get_status)};
    const dest: *const c.sockaddr = @ptrCast(&self.dest_addr);
    _ = c.sendto(self.sock, &buf, 1, 0, dest, @sizeOf(std.posix.sockaddr.in));
}

fn sendRaw(self: *Connection, data: []const u8) Error!void {
    const dest: *const c.sockaddr = @ptrCast(&self.dest_addr);
    const n = c.sendto(self.sock, data.ptr, data.len, 0, dest, @sizeOf(std.posix.sockaddr.in));
    if (n < 0) return Error.SendFailed;
}

// --- Tests ---

test "Connection open and close without crash" {
    var conn = try Connection.open(.{ .host = "127.0.0.1", .port = 9999 });
    conn.close();
}

test "Connection poll returns immediately on empty socket" {
    var conn = try Connection.open(.{ .host = "127.0.0.1", .port = 9999 });
    defer conn.close();
    conn.poll();
    const s = conn.fpgaStatus();
    try std.testing.expectEqual(@as(u32, 0), s.frame);
}

test "Connection sendInit does not error on loopback" {
    var conn = try Connection.open(.{ .host = "127.0.0.1", .port = 9999 });
    defer conn.close();
    try conn.sendInit();
}

test "Connection switchRes does not error on loopback" {
    var conn = try Connection.open(.{ .host = "127.0.0.1", .port = 9999 });
    defer conn.close();
    try conn.switchRes(.{
        .pixel_clock = 6.7,
        .h_active = 320,
        .h_begin = 336,
        .h_end = 367,
        .h_total = 426,
        .v_active = 240,
        .v_begin = 244,
        .v_end = 247,
        .v_total = 262,
        .interlaced = false,
    });
}

test "parseIp4 loopback" {
    const addr = try parseIp4("127.0.0.1", 32100);
    try std.testing.expectEqual(@as(u16, std.posix.AF.INET), addr.family);
}
