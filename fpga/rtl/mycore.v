
module mycore
(
	input         clk,          // 27 MHz clk_sys / CLK_VIDEO
	input         reset,
	
	input         pal,          // unused: this milestone is NTSC 480i only
	input         scandouble,   // unused: native 15 kHz interlaced, not 480p

	output reg    ce_pix,
	output        hvcnt_atzero,

	output reg    HBlank,
	output reg    HSync,
	output reg    VBlank,
	output reg    VSync,
	output        field,
	output reg [9:0] hc = 0,
	output reg [9:0] vc = 0
);

// NTSC BT.601 / DVD 480i at 13.5 MHz (CE_PIXEL = clk/2).
// Horizontal: 858 samples, 720 active.
// Vertical:   field 0 = 262 lines, field 1 = 263 lines (525 per frame).
localparam [9:0] H_TOTAL     = 10'd858;
localparam [9:0] H_LAST      = 10'd857;
localparam [9:0] H_ACTIVE    = 10'd720;
localparam [9:0] HS_START    = 10'd736;
localparam [9:0] HS_END      = 10'd798;   // 736..797 inclusive
localparam [9:0] H_HALF      = 10'd429;   // 858/2

localparam [9:0] V_ACTIVE    = 10'd240;
localparam [9:0] V_LAST_F0   = 10'd261;   // 262 lines
localparam [9:0] V_LAST_F1   = 10'd262;   // 263 lines

// 3-line VSync during vertical blank.
// Field 0: edges coincident with HSync (hc == 736) on vc 244..246.
// Field 1: same pulse shifted +429 pixels (half a line):
//   736+429 = 1165, 1165-858 = 307 on the next line → vc 245..247, hc 307.
localparam [9:0] VS_VC0      = 10'd244;
localparam [9:0] VS_VC0_END  = 10'd247;
localparam [9:0] VS_HC0      = 10'd736;
localparam [9:0] VS_VC1      = 10'd245;
localparam [9:0] VS_VC1_END  = 10'd248;
localparam [9:0] VS_HC1      = 10'd307;

reg         field_r = 0;

assign field = field_r;

// Release reset at the start of field 0 so the first picture is even-field.
assign hvcnt_atzero = ce_pix && !hc && !vc && !field_r;

always @(posedge clk) begin
	ce_pix <= ~ce_pix;

	if(ce_pix) begin
		if(hc == H_LAST) begin
			hc <= 0;
			if(vc == (field_r ? V_LAST_F1 : V_LAST_F0)) begin
				vc <= 0;
				field_r <= ~field_r;
			end else begin
				vc <= vc + 1'd1;
			end
		end else begin
			hc <= hc + 1'd1;
		end
	end
end

always @(posedge clk) begin
	if (hc == H_ACTIVE) HBlank <= 1;
		else if (hc == 0) HBlank <= 0;

	if (hc == HS_START) HSync <= 1;
		else if (hc == HS_END) HSync <= 0;

	if (vc == V_ACTIVE) VBlank <= 1;
		else if (vc == 0) VBlank <= 0;

	if (!field_r) begin
		if (hc == VS_HC0) begin
			if (vc == VS_VC0)     VSync <= 1;
			else if (vc == VS_VC0_END) VSync <= 0;
		end
	end else begin
		if (hc == VS_HC1) begin
			if (vc == VS_VC1)     VSync <= 1;
			else if (vc == VS_VC1_END) VSync <= 0;
		end
	end
end

endmodule
