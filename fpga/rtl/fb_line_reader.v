// SPDX-License-Identifier: GPL-2.0-or-later
// Ping-pong line reader for native 480i / 576i CRT.
//
// Legacy mode (req_yuv=0, default): 720x576 BGR0, stride 2880.
// YUV mode (req_yuv=1): planar YUV420P in the same A/B slots,
// FPGA BT.601 limited-range convert. Raster inputs are never stalled.
//
// NTSC: src_y = vc*2 + field for vc 0..239 → lines 0..479.
// PAL:  src_y = vc*2 + field for vc 0..287 → lines 0..575.
// display_buf / display_yuv / display_intl / display_tff snapshot together
// once per complete native frame (field 1 → 0). HDMI ASCAL captures the
// same VGA_* stream. yuv_plane_addr uses display_intl, never live req_intl.
//
// display_tff is latched for a future BFF temporal-order fix. This build
// does not change the proven field-1 wrap / A-B swap boundary: genuine BFF
// material is still shown top-field-first (temporally reversed).

module fb_line_reader
(
	input         clk,
	input         reset,

	input  [9:0]  hc,
	input  [9:0]  vc,
	input         field,
	input         ce_pix,
	input         req_buf,
	input         req_yuv,
	input         req_intl,
	input         req_tff,
	output reg    display_buf,
	output reg    display_yuv,
	output reg    display_intl,
	output reg    display_tff,
	input         pal,
	input         dup_even,

	input         mb_idle,      // mailbox allows a new video burst to start
	input         ddr_quiet,    // no accepted-but-undelivered DDR read beats
	output        vid_req,
	output        vid_active,
	output        ddr_rd,
	output [28:0] ddr_addr,
	output  [7:0] ddr_burstcnt,
	input         DDRAM_BUSY,
	input  [63:0] DDRAM_DOUT,
	input         DDRAM_DOUT_READY,

	output  [7:0] pix_r,
	output  [7:0] pix_g,
	output  [7:0] pix_b
);

localparam [28:0] FB_A_ADDR   = 29'h0600_0000; // 0x30000000 >> 3
localparam [28:0] FB_B_ADDR   = 29'h0604_0000; // 0x30200000 >> 3
localparam [8:0]  BGR_BEATS   = 9'd360;        // 720 pixels * 4 / 8
localparam [7:0]  BGR_BURST   = 8'd120;        // 3 bursts per line
localparam [8:0]  Y_BEATS     = 9'd90;         // 720 Y bytes / 8
localparam [8:0]  C_BEATS     = 9'd45;         // 360 U/V bytes / 8
localparam [9:0]  H_ACTIVE    = 10'd720;

localparam [1:0]  ST_IDLE  = 2'd0;
localparam [1:0]  ST_ISSUE = 2'd1;
localparam [1:0]  ST_WAIT  = 2'd2;

// Mode-dependent wrap / field height. NTSC values match proven Stage C.
wire [9:0] H_LAST   = pal ? 10'd863 : 10'd857;
wire [9:0] V_ACTIVE = pal ? 10'd288 : 10'd240;

reg  [63:0] line0 [0:359];
reg  [63:0] line1 [0:359];
reg  [63:0] y0    [0:89];
reg  [63:0] y1    [0:89];
reg  [63:0] u0    [0:44];
reg  [63:0] u1    [0:44];
reg  [63:0] v0    [0:44];
reg  [63:0] v1    [0:44];

reg         buf_ok [0:1];
reg   [9:0] buf_y  [0:1];

// Normal: src_field follows the raster field (even/odd weave).
// Duplicate Even: both output fields read even source lines only.
// Raster timing / VGA_F1 / display_buf latch still use `field`.
wire        src_field      = dup_even ? 1'b0 : field;
wire        src_next_field = dup_even ? 1'b0 : ~field;
wire  [9:0] y_disp         = {vc[8:0], src_field};
wire  [9:0] y_next_active  = {vc[8:0] + 9'd1, src_field};
wire  [9:0] y_nf0          = {9'd0, src_next_field};
wire  [9:0] y_nf1          = {9'd1, src_next_field};

wire        have0_disp = buf_ok[0] && buf_y[0] == y_disp;
wire        have1_disp = buf_ok[1] && buf_y[1] == y_disp;
wire        disp_ok    = (vc < V_ACTIVE) && (have0_disp || have1_disp);
wire        disp_sel   = have1_disp && !have0_disp;

wire        have_nf0 =
	(buf_ok[0] && buf_y[0] == y_nf0) ||
	(buf_ok[1] && buf_y[1] == y_nf0);

// During the last active line only one RAM is free, so prefetch solely
// the first line of the next field. The second line waits for VBlank.
wire  [9:0] y_cand =
	(vc < (V_ACTIVE - 10'd1)) ? y_next_active :
	(vc < V_ACTIVE)           ? y_nf0 :
	have_nf0                  ? y_nf1 : y_nf0;

wire        have_cand =
	(buf_ok[0] && buf_y[0] == y_cand) ||
	(buf_ok[1] && buf_y[1] == y_cand);

wire        fill_need = !have_cand;

// Do not overwrite the line currently on screen. In VBlank, keep nf0.
wire        fill_sel =
	((vc < V_ACTIVE) && have0_disp) ||
	((vc >= V_ACTIVE) && buf_ok[0] && buf_y[0] == y_nf0);

reg   [1:0] st;
reg         fill_sel_r;
reg   [9:0] y_fill_r;
reg  [28:0] line_base;
reg   [8:0] beats_got;
reg   [7:0] burst_got;
reg   [7:0] burst_len_r;
reg   [1:0] plane_r;
reg         ddr_rd_r;
reg         pal_d = 0;
reg  [28:0] y_base_r;
reg  [28:0] u_base_r;
reg  [28:0] v_base_r;

wire [28:0] y_addr_w;
wire [28:0] u_addr_w;
wire [28:0] v_addr_w;

yuv_plane_addr u_yuv_addr
(
	.display_buf(display_buf),
	.interlaced(display_intl),
	.y(y_cand),
	.y_addr(y_addr_w),
	.u_addr(u_addr_w),
	.v_addr(v_addr_w)
);

assign vid_req      = (st != ST_IDLE) || fill_need;
assign vid_active   = (st != ST_IDLE);
assign ddr_rd       = ddr_rd_r;
assign ddr_addr     = line_base + {20'd0, beats_got};
assign ddr_burstcnt = burst_len_r;

// Snapshot req_buf before field-1 last active line, where y_cand becomes
// source line 0 of field 0. Display of the last odd line is already in the
// ping-pong RAM; only subsequent fills (field 0 line 0 onward) use the new
// base. PAL uses vc==286 / hc==863; NTSC stays vc==238 / hc==857.
wire [28:0] fb_base        = display_buf ? FB_B_ADDR : FB_A_ADDR;
wire [28:0] cand_base_bgr  = fb_base + y_cand * 29'd360;

wire pal_chg = pal_d != pal;

always @(posedge clk) begin
	pal_d <= pal;
	if (reset || pal_chg) begin
		display_buf  <= 1'b0;
		display_yuv  <= 1'b0;
		display_intl <= 1'b0;
		display_tff  <= 1'b0;
	end else if (ce_pix && field && (vc == (V_ACTIVE - 10'd2)) && (hc == H_LAST)) begin
		display_buf  <= req_buf;
		display_yuv  <= req_yuv;
		display_intl <= req_intl;
		display_tff  <= req_tff;
	end
end

wire [8:0] plane_beats = (plane_r == 2'd0) ? Y_BEATS : C_BEATS;

always @(posedge clk) begin
	ddr_rd_r <= 1'b0;

	if (reset || pal_chg) begin
		st          <= ST_IDLE;
		buf_ok[0]   <= 1'b0;
		buf_ok[1]   <= 1'b0;
		beats_got   <= 9'd0;
		burst_got   <= 8'd0;
		burst_len_r <= BGR_BURST;
		fill_sel_r  <= 1'b0;
		y_fill_r    <= 10'd0;
		line_base   <= 29'd0;
		plane_r     <= 2'd0;
		y_base_r    <= 29'd0;
		u_base_r    <= 29'd0;
		v_base_r    <= 29'd0;
	end else begin
		case (st)
			ST_IDLE: if (fill_need && mb_idle) begin
					// mb_idle is mb_allow_vid: mailbox idle and no poll/follow-up due.
					fill_sel_r     <= fill_sel;
					y_fill_r       <= y_cand;
					beats_got      <= 9'd0;
					burst_got      <= 8'd0;
					plane_r        <= 2'd0;
					buf_ok[fill_sel] <= 1'b0;
					if (display_yuv) begin
						y_base_r    <= y_addr_w;
						u_base_r    <= u_addr_w;
						v_base_r    <= v_addr_w;
						line_base   <= y_addr_w;
						burst_len_r <= Y_BEATS[7:0];
					end else begin
						line_base   <= cand_base_bgr;
						burst_len_r <= BGR_BURST;
					end
					st             <= ST_ISSUE;
				end

			ST_ISSUE: begin
				// Pre-reset YUV (commit 000ccd5) issued on !BUSY in the
				// same cycle as ST_WAIT. Reset-safety then required
				// ddr_quiet and delayed WAIT until ddr_rd_r && !BUSY.
				// That extra cycle plus quiet-gating *every* burst is a
				// YUV-only presentation bug: Y/U/V are three sequential
				// reads with beats_got cleared and a new line_base each
				// plane, so buf_ok is delayed past y_disp → pix_en=0
				// (black) while RGB's single-plane 120-beat chunks still
				// complete in time.
				//
				// Restore the proven idle-port issue (RD+WAIT same cycle
				// when !BUSY). Hold RD only while waitrequest is high.
				// ddr_quiet applies to a *new* fill from ST_IDLE
				// (plane_r=0, beats_got=0) so leftover pre-reset beats
				// cannot attach to the first command. Intra-line YUV
				// U/V and RGB bursts 2/3 already consumed every beat of
				// the previous burst in ST_WAIT.
				if (!DDRAM_BUSY &&
				    (ddr_rd_r || ddr_quiet ||
				     (plane_r != 2'd0) || (beats_got != 9'd0))) begin
					ddr_rd_r  <= 1'b1;
					burst_got <= 8'd0;
					st        <= ST_WAIT;
				end else if (ddr_rd_r || ddr_quiet ||
				             (plane_r != 2'd0) || (beats_got != 9'd0))
					ddr_rd_r <= 1'b1;
			end

			ST_WAIT: begin
				if (DDRAM_DOUT_READY) begin
					if (display_yuv) begin
						if (plane_r == 2'd0) begin
							if (fill_sel_r)
								y1[beats_got[6:0]] <= DDRAM_DOUT;
							else
								y0[beats_got[6:0]] <= DDRAM_DOUT;
						end else if (plane_r == 2'd1) begin
							if (fill_sel_r)
								u1[beats_got[5:0]] <= DDRAM_DOUT;
							else
								u0[beats_got[5:0]] <= DDRAM_DOUT;
						end else begin
							if (fill_sel_r)
								v1[beats_got[5:0]] <= DDRAM_DOUT;
							else
								v0[beats_got[5:0]] <= DDRAM_DOUT;
						end

						if (beats_got == (plane_beats - 9'd1)) begin
							if (plane_r == 2'd2) begin
								buf_ok[fill_sel_r] <= 1'b1;
								buf_y[fill_sel_r]  <= y_fill_r;
								beats_got          <= 9'd0;
								burst_got          <= 8'd0;
								plane_r            <= 2'd0;
								st                 <= ST_IDLE;
							end else begin
								beats_got   <= 9'd0;
								burst_got   <= 8'd0;
								plane_r     <= plane_r + 2'd1;
								line_base   <= (plane_r == 2'd0) ? u_base_r : v_base_r;
								burst_len_r <= C_BEATS[7:0];
								st          <= ST_ISSUE;
							end
						end else begin
							beats_got <= beats_got + 9'd1;
							burst_got <= burst_got + 8'd1;
						end
					end else begin
						if (fill_sel_r)
							line1[beats_got] <= DDRAM_DOUT;
						else
							line0[beats_got] <= DDRAM_DOUT;

						if (beats_got == (BGR_BEATS - 9'd1)) begin
							buf_ok[fill_sel_r] <= 1'b1;
							buf_y[fill_sel_r]  <= y_fill_r;
							beats_got          <= 9'd0;
							burst_got          <= 8'd0;
							st                 <= ST_IDLE;
						end else if (burst_got == (BGR_BURST - 8'd1)) begin
							beats_got <= beats_got + 9'd1;
							burst_got <= 8'd0;
							st        <= ST_ISSUE;
						end else begin
							beats_got <= beats_got + 9'd1;
							burst_got <= burst_got + 8'd1;
						end
					end
				end
			end

			default: st <= ST_IDLE;
		endcase
	end
end

reg  [63:0] pair_d;
reg         hc0_d;
reg         pix_ok_d;
reg         h_active_d;
reg  [63:0] y_word_d;
reg  [63:0] u_word_d;
reg  [63:0] v_word_d;
reg   [2:0] y_byte_d;
reg   [2:0] c_byte_d;
reg         yuv_d;

wire [8:0] rd_beat = (hc < H_ACTIVE) ? hc[9:1] : 9'd0;
wire [6:0] y_beat  = (hc < H_ACTIVE) ? hc[9:3] : 7'd0;
wire [5:0] c_beat  = (hc < H_ACTIVE) ? hc[9:4] : 6'd0;
wire [2:0] y_byte  = hc[2:0];
wire [2:0] c_byte  = hc[3:1];

always @(posedge clk) begin
	pair_d     <= disp_sel ? line1[rd_beat] : line0[rd_beat];
	hc0_d      <= hc[0];
	pix_ok_d   <= disp_ok;
	h_active_d <= (hc < H_ACTIVE);
	yuv_d      <= display_yuv;
	y_byte_d   <= y_byte;
	c_byte_d   <= c_byte;
	if (disp_sel) begin
		y_word_d <= y1[y_beat];
		u_word_d <= u1[c_beat];
		v_word_d <= v1[c_beat];
	end else begin
		y_word_d <= y0[y_beat];
		u_word_d <= u0[c_beat];
		v_word_d <= v0[c_beat];
	end
end

wire [31:0] pix32 = hc0_d ? pair_d[63:32] : pair_d[31:0];
wire        pix_en = pix_ok_d && h_active_d;

wire [7:0] y_s = y_word_d[y_byte_d*8 +: 8];
wire [7:0] u_s = u_word_d[c_byte_d*8 +: 8];
wire [7:0] v_s = v_word_d[c_byte_d*8 +: 8];

wire [7:0] yuv_r, yuv_g, yuv_b;

yuv601_rgb u_yuv601
(
	.y(y_s),
	.u(u_s),
	.v(v_s),
	.r(yuv_r),
	.g(yuv_g),
	.b(yuv_b)
);

assign pix_b = pix_en ? (yuv_d ? yuv_b : pix32[7:0])   : 8'd0;
assign pix_g = pix_en ? (yuv_d ? yuv_g : pix32[15:8])  : 8'd0;
assign pix_r = pix_en ? (yuv_d ? yuv_r : pix32[23:16]) : 8'd0;

endmodule
