// Ping-pong line reader for native 480i / 576i CRT.
// Source is 720x576 BGR0, stride 2880.
// NTSC: src_y = vc*2 + field for vc 0..239 → lines 0..479.
// PAL:  src_y = vc*2 + field for vc 0..287 → lines 0..575.
// Raster counters are inputs; never stall them.
// display_buf snapshots req_buf once per complete native frame (field 1 -> 0).
// HDMI ASCAL captures the same VGA_* stream; it does not read these DDR buffers.
//
// crt_stab: 0/3=Off (true even/odd weave), 1=Gentle (75/25 pair blend),
//           2=Strong (duplicate even). Off/Strong use the original two line
// RAMs and one DDR line per output line. Gentle uses two E/O pairs (four
// RAMs) and two DDR lines per output line.

module fb_line_reader
(
	input         clk,
	input         reset,

	input  [9:0]  hc,
	input  [9:0]  vc,
	input         field,
	input         ce_pix,
	input         req_buf,
	output reg    display_buf,
	input         pal,
	input   [1:0] crt_stab,

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
localparam [28:0] FB_B_ADDR   = 29'h0604_0000; // 0x30200000 >> 3
localparam [8:0]  BEATS_LINE  = 9'd360;        // 720 pixels * 4 / 8
localparam [7:0]  BURST_BEATS = 8'd120;        // 3 bursts per line
localparam [9:0]  H_ACTIVE    = 10'd720;

localparam [1:0]  ST_IDLE  = 2'd0;
localparam [1:0]  ST_ISSUE = 2'd1;
localparam [1:0]  ST_WAIT  = 2'd2;

// Mode-dependent wrap / field height. NTSC values match proven Stage C.
wire [9:0] H_LAST   = pal ? 10'd863 : 10'd857;
wire [9:0] V_ACTIVE = pal ? 10'd288 : 10'd240;

wire        gentle = (crt_stab == 2'd1);
wire        strong = (crt_stab == 2'd2);

reg  [63:0] line0 [0:359];
reg  [63:0] line1 [0:359];

reg         buf_ok [0:1];
reg   [9:0] buf_y  [0:1];

// Gentle: two E/O pairs. Pair A displayed, pair B prefetched, then swap.
reg  [63:0] pe0 [0:359];
reg  [63:0] po0 [0:359];
reg  [63:0] pe1 [0:359];
reg  [63:0] po1 [0:359];
reg         pe_ok [0:1];
reg         po_ok [0:1];
reg   [9:0] pe_y  [0:1];
reg   [9:0] po_y  [0:1];

// Off: src_field follows the raster field. Strong: both fields read even.
// Gentle does not use these wires for DDR/display.
wire        src_field      = strong ? 1'b0 : field;
wire        src_next_field = strong ? 1'b0 : ~field;
wire  [9:0] y_disp         = {vc[8:0], src_field};
wire  [9:0] y_next_active  = {vc[8:0] + 9'd1, src_field};
wire  [9:0] y_nf0          = {9'd0, src_next_field};
wire  [9:0] y_nf1          = {9'd1, src_next_field};

wire        have0_disp = buf_ok[0] && buf_y[0] == y_disp;
wire        have1_disp = buf_ok[1] && buf_y[1] == y_disp;
wire        off_disp_ok = (vc < V_ACTIVE) && (have0_disp || have1_disp);
wire        disp_sel    = have1_disp && !have0_disp;

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

wire        off_fill_need = !have_cand;

// Do not overwrite the line currently on screen. In VBlank, keep nf0.
wire        fill_sel =
	((vc < V_ACTIVE) && have0_disp) ||
	((vc >= V_ACTIVE) && buf_ok[0] && buf_y[0] == y_nf0);

// ---- Gentle pair fetch: E=2*vc, O=2*vc+1. Never 576/577 or 480/481. ----
wire        in_pic   = (vc < V_ACTIVE);
wire        last_pic = (vc == (V_ACTIVE - 10'd1));
wire  [9:0] y_e_disp = {vc[8:0], 1'b0};
wire  [9:0] y_o_disp = {vc[8:0], 1'b1};

wire g_have0_disp = in_pic && pe_ok[0] && po_ok[0] &&
	pe_y[0] == y_e_disp && po_y[0] == y_o_disp;
wire g_have1_disp = in_pic && pe_ok[1] && po_ok[1] &&
	pe_y[1] == y_e_disp && po_y[1] == y_o_disp;
wire g_disp_ok   = g_have0_disp || g_have1_disp;
wire g_disp_pair = g_have1_disp && !g_have0_disp;

// Current pair if display not ready; else next pair, or 0/1 after last line.
wire [9:0] y_e_need = !in_pic ? 10'd0 :
	(!g_disp_ok ? y_e_disp :
	 (last_pic  ? 10'd0 : {vc[8:0] + 9'd1, 1'b0}));
wire [9:0] y_o_need = !in_pic ? 10'd1 :
	(!g_disp_ok ? y_o_disp :
	 (last_pic  ? 10'd1 : {vc[8:0] + 9'd1, 1'b1}));

wire g_have_need =
	(pe_ok[0] && po_ok[0] && pe_y[0] == y_e_need && po_y[0] == y_o_need) ||
	(pe_ok[1] && po_ok[1] && pe_y[1] == y_e_need && po_y[1] == y_o_need);

wire g_fill_need = !g_have_need;

// Do not overwrite the pair currently on screen.
wire g_fill_pair = g_have0_disp ? 1'b1 :
                   g_have1_disp ? 1'b0 :
                   (pe_ok[0] && po_ok[0] &&
                    pe_y[0] == y_e_need && po_y[0] == y_o_need) ? 1'b1 : 1'b0;

wire g_have_fill_e = pe_ok[g_fill_pair] && pe_y[g_fill_pair] == y_e_need;
wire g_fill_odd    = g_have_fill_e;
wire [9:0] y_g_fill = g_fill_odd ? y_o_need : y_e_need;

wire        fill_need = gentle ? g_fill_need : off_fill_need;
wire [9:0]  y_issue   = gentle ? y_g_fill    : y_cand;

reg   [1:0] st;
reg         fill_sel_r;
reg         gentle_fill_r;
reg         fill_pair_r;
reg         fill_odd_r;
reg   [9:0] y_fill_r;
reg  [28:0] line_base;
reg   [8:0] beats_got;
reg   [7:0] burst_got;
reg         ddr_rd_r;
reg         pal_d = 0;
reg   [1:0] stab_d = 0;

assign vid_req    = (st != ST_IDLE) || fill_need;
assign vid_active = (st != ST_IDLE);
assign ddr_rd     = ddr_rd_r;
assign ddr_addr   = line_base + {20'd0, beats_got};
assign ddr_burstcnt = BURST_BEATS;

// Snapshot req_buf before field-1 last active line, where y_cand becomes
// source line 0 of field 0. Display of the last odd line is already in the
// ping-pong RAM; only subsequent fills (field 0 line 0 onward) use the new
// base. PAL uses vc==286 / hc==863; NTSC stays vc==238 / hc==857.
wire [28:0] fb_base    = display_buf ? FB_B_ADDR : FB_A_ADDR;
wire [28:0] issue_base = fb_base + y_issue * 29'd360;

wire pal_chg  = pal_d != pal;
wire stab_chg = stab_d != crt_stab;

always @(posedge clk) begin
	pal_d  <= pal;
	stab_d <= crt_stab;
	if (reset || pal_chg)
		display_buf <= 1'b0;
	else if (ce_pix && field && (vc == (V_ACTIVE - 10'd2)) && (hc == H_LAST))
		display_buf <= req_buf;
end

always @(posedge clk) begin
	ddr_rd_r <= 1'b0;

	if (reset || pal_chg || stab_chg) begin
		st            <= ST_IDLE;
		buf_ok[0]     <= 1'b0;
		buf_ok[1]     <= 1'b0;
		pe_ok[0]      <= 1'b0;
		pe_ok[1]      <= 1'b0;
		po_ok[0]      <= 1'b0;
		po_ok[1]      <= 1'b0;
		beats_got     <= 9'd0;
		burst_got     <= 8'd0;
		fill_sel_r    <= 1'b0;
		gentle_fill_r <= 1'b0;
		fill_pair_r   <= 1'b0;
		fill_odd_r    <= 1'b0;
		y_fill_r      <= 10'd0;
		line_base     <= 29'd0;
	end else begin
		case (st)
			ST_IDLE: if (fill_need && mb_idle) begin
					gentle_fill_r  <= gentle;
					fill_sel_r     <= fill_sel;
					fill_pair_r    <= g_fill_pair;
					fill_odd_r     <= g_fill_odd;
					y_fill_r       <= y_issue;
					line_base      <= issue_base;
					beats_got      <= 9'd0;
					burst_got      <= 8'd0;
					if (gentle) begin
						if (g_fill_odd)
							po_ok[g_fill_pair] <= 1'b0;
						else
							pe_ok[g_fill_pair] <= 1'b0;
					end else
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
					if (gentle_fill_r) begin
						if (fill_pair_r) begin
							if (fill_odd_r)
								po1[beats_got] <= DDRAM_DOUT;
							else
								pe1[beats_got] <= DDRAM_DOUT;
						end else begin
							if (fill_odd_r)
								po0[beats_got] <= DDRAM_DOUT;
							else
								pe0[beats_got] <= DDRAM_DOUT;
						end
					end else if (fill_sel_r)
						line1[beats_got] <= DDRAM_DOUT;
					else
						line0[beats_got] <= DDRAM_DOUT;

					if (beats_got == (BEATS_LINE - 9'd1)) begin
						if (gentle_fill_r) begin
							if (fill_odd_r) begin
								po_ok[fill_pair_r] <= 1'b1;
								po_y[fill_pair_r]  <= y_fill_r;
							end else begin
								pe_ok[fill_pair_r] <= 1'b1;
								pe_y[fill_pair_r]  <= y_fill_r;
							end
						end else begin
							buf_ok[fill_sel_r] <= 1'b1;
							buf_y[fill_sel_r]  <= y_fill_r;
						end
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
reg  [63:0] pair_e_d;
reg  [63:0] pair_o_d;
reg         hc0_d;
reg         pix_ok_d;
reg         h_active_d;
reg         gentle_d;
reg         field_d;

wire [8:0] rd_beat = (hc < H_ACTIVE) ? hc[9:1] : 9'd0;
wire       disp_ok = gentle ? g_disp_ok : off_disp_ok;

always @(posedge clk) begin
	pair_d     <= disp_sel ? line1[rd_beat] : line0[rd_beat];
	pair_e_d   <= g_disp_pair ? pe1[rd_beat] : pe0[rd_beat];
	pair_o_d   <= g_disp_pair ? po1[rd_beat] : po0[rd_beat];
	hc0_d      <= hc[0];
	pix_ok_d   <= disp_ok;
	h_active_d <= (hc < H_ACTIVE);
	gentle_d   <= gentle;
	field_d    <= field;
end

wire [31:0] pix32 = hc0_d ? pair_d[63:32] : pair_d[31:0];
wire [31:0] pix_e = hc0_d ? pair_e_d[63:32] : pair_e_d[31:0];
wire [31:0] pix_o = hc0_d ? pair_o_d[63:32] : pair_o_d[31:0];
wire        pix_en = pix_ok_d && h_active_d;

wire [7:0] e_b = pix_e[7:0];
wire [7:0] e_g = pix_e[15:8];
wire [7:0] e_r = pix_e[23:16];
wire [7:0] o_b = pix_o[7:0];
wire [7:0] o_g = pix_o[15:8];
wire [7:0] o_r = pix_o[23:16];

// Gentle field 0: (3*E + O + 2) >> 2; field 1: (E + 3*O + 2) >> 2.
wire [9:0] mix0_r = {1'b0, e_r, 1'b0} + {2'b0, e_r} + {2'b0, o_r} + 10'd2;
wire [9:0] mix0_g = {1'b0, e_g, 1'b0} + {2'b0, e_g} + {2'b0, o_g} + 10'd2;
wire [9:0] mix0_b = {1'b0, e_b, 1'b0} + {2'b0, e_b} + {2'b0, o_b} + 10'd2;
wire [9:0] mix1_r = {1'b0, o_r, 1'b0} + {2'b0, o_r} + {2'b0, e_r} + 10'd2;
wire [9:0] mix1_g = {1'b0, o_g, 1'b0} + {2'b0, o_g} + {2'b0, e_g} + 10'd2;
wire [9:0] mix1_b = {1'b0, o_b, 1'b0} + {2'b0, o_b} + {2'b0, e_b} + 10'd2;

wire [7:0] g_r = field_d ? mix1_r[9:2] : mix0_r[9:2];
wire [7:0] g_g = field_d ? mix1_g[9:2] : mix0_g[9:2];
wire [7:0] g_b = field_d ? mix1_b[9:2] : mix0_b[9:2];

assign pix_b = pix_en ? (gentle_d ? g_b : pix32[7:0])   : 8'd0;
assign pix_g = pix_en ? (gentle_d ? g_g : pix32[15:8])  : 8'd0;
assign pix_r = pix_en ? (gentle_d ? g_r : pix32[23:16]) : 8'd0;

endmodule
