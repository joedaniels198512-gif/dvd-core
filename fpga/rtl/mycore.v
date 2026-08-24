
module mycore
(
	input         clk,          // 27 MHz clk_sys / CLK_VIDEO
	input         reset,
	
	input         pal,          // 0 = proven NTSC 480i, 1 = PAL 576i
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

// Shared 13.5 MHz pixel rate (CE_PIXEL = clk/2). Active width is 720 in both
// modes. pal selects only the Rec.601 line/field totals and sync placement.
localparam [9:0] H_ACTIVE    = 10'd720;

// ---------------------------------------------------------------------------
// NTSC BT.601 / DVD 480i — values frozen from hardware-proven Stage C.
// Horizontal: 858 samples, 720 active.
// Vertical:   field 0 = 262 lines, field 1 = 263 lines (525 per frame).
// ---------------------------------------------------------------------------
localparam [9:0] NTSC_H_TOTAL    = 10'd858;
localparam [9:0] NTSC_H_LAST     = 10'd857;
localparam [9:0] NTSC_HS_START   = 10'd736;
localparam [9:0] NTSC_HS_END     = 10'd798;   // 736..797 inclusive
localparam [9:0] NTSC_H_HALF     = 10'd429;   // 858/2

localparam [9:0] NTSC_V_ACTIVE   = 10'd240;
localparam [9:0] NTSC_V_LAST_F0  = 10'd261;   // 262 lines
localparam [9:0] NTSC_V_LAST_F1  = 10'd262;   // 263 lines

// 3-line VSync during vertical blank.
// Field 0: edges coincident with HSync (hc == 736) on vc 244..246.
// Field 1: same pulse shifted +429 pixels (half a line):
//   736+429 = 1165, 1165-858 = 307 on the next line → vc 245..247, hc 307.
localparam [9:0] NTSC_VS_VC0     = 10'd244;
localparam [9:0] NTSC_VS_VC0_END = 10'd247;
localparam [9:0] NTSC_VS_HC0     = 10'd736;
localparam [9:0] NTSC_VS_VC1     = 10'd245;
localparam [9:0] NTSC_VS_VC1_END = 10'd248;
localparam [9:0] NTSC_VS_HC1     = 10'd307;

// ---------------------------------------------------------------------------
// PAL BT.601 / DVD 576i at 13.5 MHz.
// Horizontal: 864 samples, 720 active → 15.625 kHz.
// Vertical:   field 0 = 312 lines, field 1 = 313 lines (625 per frame).
//
// PAL H blanking is Rec.601's 144 samples, porches taken from ITU-R BT.1700
// analog 4.7 µs sync / 1.65 µs front porch, quantized to 13.5 MHz:
//   active      hc   0..719   720 clks   53.333 µs
//   front porch hc 720..741    22 clks    1.630 µs   (BT.1700 1.65 µs)
//   HSync       hc 742..805    64 clks    4.741 µs   (BT.1700 4.7 µs)
//   back porch  hc 806..863    58 clks    4.296 µs   (digital remainder)
// Analog PAL back porch is ~5.7 µs; the extra lives outside the 144-sample
// digital blanking window, same Rec.601 compromise as NTSC.
// ---------------------------------------------------------------------------
localparam [9:0] PAL_H_TOTAL     = 10'd864;
localparam [9:0] PAL_H_LAST      = 10'd863;
localparam [9:0] PAL_HS_START    = 10'd742;
localparam [9:0] PAL_HS_END      = 10'd806;   // 742..805 inclusive
localparam [9:0] PAL_H_HALF      = 10'd432;   // 864/2

localparam [9:0] PAL_V_ACTIVE    = 10'd288;
localparam [9:0] PAL_V_LAST_F0   = 10'd311;   // 312 lines
localparam [9:0] PAL_V_LAST_F1   = 10'd312;   // 313 lines

// Same 3-line VGA-style VSync FSM as NTSC (integer lines, same-hc edges).
// Field 0: coincident with HSync (hc == 742) on vc 292..294.
// Field 1: +432 pixels (half a line):
//   742+432 = 1174, 1174-864 = 310 on the next line → vc 293..295, hc 310.
localparam [9:0] PAL_VS_VC0      = 10'd292;
localparam [9:0] PAL_VS_VC0_END  = 10'd295;
localparam [9:0] PAL_VS_HC0      = 10'd742;
localparam [9:0] PAL_VS_VC1      = 10'd293;
localparam [9:0] PAL_VS_VC1_END  = 10'd296;
localparam [9:0] PAL_VS_HC1      = 10'd310;

wire [9:0] H_LAST     = pal ? PAL_H_LAST     : NTSC_H_LAST;
wire [9:0] HS_START   = pal ? PAL_HS_START   : NTSC_HS_START;
wire [9:0] HS_END     = pal ? PAL_HS_END     : NTSC_HS_END;
wire [9:0] V_ACTIVE   = pal ? PAL_V_ACTIVE   : NTSC_V_ACTIVE;
wire [9:0] V_LAST_F0  = pal ? PAL_V_LAST_F0  : NTSC_V_LAST_F0;
wire [9:0] V_LAST_F1  = pal ? PAL_V_LAST_F1  : NTSC_V_LAST_F1;
wire [9:0] VS_VC0     = pal ? PAL_VS_VC0     : NTSC_VS_VC0;
wire [9:0] VS_VC0_END = pal ? PAL_VS_VC0_END : NTSC_VS_VC0_END;
wire [9:0] VS_HC0     = pal ? PAL_VS_HC0     : NTSC_VS_HC0;
wire [9:0] VS_VC1     = pal ? PAL_VS_VC1     : NTSC_VS_VC1;
wire [9:0] VS_VC1_END = pal ? PAL_VS_VC1_END : NTSC_VS_VC1_END;
wire [9:0] VS_HC1     = pal ? PAL_VS_HC1     : NTSC_VS_HC1;

reg         field_r = 0;
reg         pal_d   = 0;

assign field = field_r;

// Release reset at the start of field 0 so the first picture is even-field.
assign hvcnt_atzero = ce_pix && !hc && !vc && !field_r;

always @(posedge clk) begin
	ce_pix <= ~ce_pix;
	pal_d  <= pal;

	// OSD PAL/NTSC switch: resync so hc/vc cannot run past the new wrap.
	if (pal_d != pal) begin
		hc      <= 10'd0;
		vc      <= 10'd0;
		field_r <= 1'b0;
	end else if(ce_pix) begin
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
	if (pal_d != pal) begin
		HBlank <= 0;
		HSync  <= 0;
		VBlank <= 0;
		VSync  <= 0;
	end else begin
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
end

endmodule
