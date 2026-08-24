// Stage A: ping-pong line reader for native 480i CRT.
// Source is a static 720x576 BGR0 framebuffer at 0x30000000, stride 2880.
// CRT uses source lines 0..479 only (src_y = vc*2 + field).
// Raster counters are inputs; this module must never stall them.

module fb_line_reader
(
	input         clk,
	input         reset,

	input  [9:0]  hc,
	input  [9:0]  vc,
	input         field,

	input         mb_idle,
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
localparam [8:0]  BEATS_LINE  = 9'd360;        // 720 pixels * 4 / 8
localparam [7:0]  BURST_BEATS = 8'd120;        // 3 bursts per line
localparam [9:0]  H_ACTIVE    = 10'd720;
localparam [9:0]  V_ACTIVE    = 10'd240;

localparam [1:0]  ST_IDLE  = 2'd0;
localparam [1:0]  ST_ISSUE = 2'd1;
localparam [1:0]  ST_WAIT  = 2'd2;

reg  [63:0] line0 [0:359];
reg  [63:0] line1 [0:359];

reg         buf_ok [0:1];
reg   [8:0] buf_y  [0:1];

wire        next_field    = ~field;
wire  [8:0] y_disp        = {vc[7:0], field};
wire  [8:0] y_next_active = {vc[7:0] + 8'd1, field};
wire  [8:0] y_nf0         = {8'd0, next_field};
wire  [8:0] y_nf1         = {8'd1, next_field};

wire        have0_disp = buf_ok[0] && buf_y[0] == y_disp;
wire        have1_disp = buf_ok[1] && buf_y[1] == y_disp;
wire        disp_ok    = (vc < V_ACTIVE) && (have0_disp || have1_disp);
wire        disp_sel   = have1_disp && !have0_disp;

wire        have_nf0 =
	(buf_ok[0] && buf_y[0] == y_nf0) ||
	(buf_ok[1] && buf_y[1] == y_nf0);

// During the last active line only one RAM is free, so prefetch solely
// the first line of the next field. The second line waits for VBlank.
wire  [8:0] y_cand =
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
reg   [8:0] y_fill_r;
reg  [28:0] line_base;
reg   [8:0] beats_got;
reg   [7:0] burst_got;
reg         ddr_rd_r;

assign vid_req    = (st != ST_IDLE) || fill_need;
assign vid_active = (st != ST_IDLE);
assign ddr_rd     = ddr_rd_r;
assign ddr_addr   = line_base + {20'd0, beats_got};
assign ddr_burstcnt = BURST_BEATS;

wire [28:0] cand_base = FB_A_ADDR + y_cand * 29'd360;

always @(posedge clk) begin
	ddr_rd_r <= 1'b0;

	if (reset) begin
		st          <= ST_IDLE;
		buf_ok[0]   <= 1'b0;
		buf_ok[1]   <= 1'b0;
		beats_got   <= 9'd0;
		burst_got   <= 8'd0;
		fill_sel_r  <= 1'b0;
		y_fill_r    <= 9'd0;
		line_base   <= 29'd0;
	end else begin
		case (st)
			ST_IDLE: if (fill_need && mb_idle) begin
					fill_sel_r     <= fill_sel;
					y_fill_r       <= y_cand;
					line_base      <= cand_base;
					beats_got      <= 9'd0;
					burst_got      <= 8'd0;
					buf_ok[fill_sel] <= 1'b0;
					st             <= ST_ISSUE;
				end

			ST_ISSUE: begin
				if (!DDRAM_BUSY) begin
					ddr_rd_r  <= 1'b1;
					burst_got <= 8'd0;
					st        <= ST_WAIT;
				end
			end

			ST_WAIT: begin
				if (DDRAM_DOUT_READY) begin
					if (fill_sel_r)
						line1[beats_got] <= DDRAM_DOUT;
					else
						line0[beats_got] <= DDRAM_DOUT;

					if (beats_got == (BEATS_LINE - 9'd1)) begin
						buf_ok[fill_sel_r] <= 1'b1;
						buf_y[fill_sel_r]  <= y_fill_r;
						beats_got          <= 9'd0;
						burst_got          <= 8'd0;
						st                 <= ST_IDLE;
					end else if (burst_got == (BURST_BEATS - 8'd1)) begin
						beats_got <= beats_got + 9'd1;
						burst_got <= 8'd0;
						st        <= ST_ISSUE;
					end else begin
						beats_got <= beats_got + 9'd1;
						burst_got <= burst_got + 8'd1;
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

wire [8:0] rd_beat = (hc < H_ACTIVE) ? hc[9:1] : 9'd0;

always @(posedge clk) begin
	pair_d     <= disp_sel ? line1[rd_beat] : line0[rd_beat];
	hc0_d      <= hc[0];
	pix_ok_d   <= disp_ok;
	h_active_d <= (hc < H_ACTIVE);
end

wire [31:0] pix32 = hc0_d ? pair_d[63:32] : pair_d[31:0];
wire        pix_en = pix_ok_d && h_active_d;

assign pix_b = pix_en ? pix32[7:0]   : 8'd0;
assign pix_g = pix_en ? pix32[15:8]  : 8'd0;
assign pix_r = pix_en ? pix32[23:16] : 8'd0;

endmodule
