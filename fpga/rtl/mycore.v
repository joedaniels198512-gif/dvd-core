
module mycore
(
	input         clk,          // 27 MHz CLK_VIDEO
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

	output  [7:0] video
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

reg   [9:0] hc = 0;
reg   [9:0] vc = 0;
reg         field_r = 0;
reg   [9:0] vvc = 0;
reg  [63:0] rnd_reg;

wire  [5:0] rnd_c = {rnd_reg[0],rnd_reg[1],rnd_reg[2],rnd_reg[2],rnd_reg[2],rnd_reg[2]};
wire [63:0] rnd;

lfsr random(rnd);

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
				vvc <= vvc + 9'd6;
			end else begin
				vc <= vc + 1'd1;
			end
		end else begin
			hc <= hc + 1'd1;
		end

		rnd_reg <= rnd;
	end

	if(reset) vvc <= 0;
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

reg  [7:0] cos_out;
wire [5:0] cos_g = cos_out[7:3]+6'd32;
cos cos(vvc + {vc[8:0], 2'b00}, cos_out);

// Logical 480-line Y so the two fields weave instead of overwriting.
wire [8:0] y = {vc[7:0], field_r};

wire border = (hc < 10'd8) || (hc > 10'd711) ||
              (y  <  9'd8) || (y  >  9'd471);

wire [9:0] mid_h = 10'd360;
wire [8:0] mid_v = 9'd240;
wire xhair = ((hc >= (mid_h - 10'd1)) && (hc <= (mid_h + 10'd1))) ||
             ((y  >= (mid_v -  9'd1)) && (y  <= (mid_v +  9'd1)));

wire [7:0] bars =
	(hc < 10'd90)  ? 8'h00 :
	(hc < 10'd180) ? 8'h24 :
	(hc < 10'd270) ? 8'h48 :
	(hc < 10'd360) ? 8'h6C :
	(hc < 10'd450) ? 8'h90 :
	(hc < 10'd540) ? 8'hB4 :
	(hc < 10'd630) ? 8'hD8 :
	                 8'hFC;

wire lower = (y >= 9'd360);
wire check = hc[4] ^ y[4];

// 1-line on / 1-line off grating on logical Y. True 480i weaves this into
// a fine horizontal texture; duplicated 240p becomes a solid 30 Hz flash.
wire stripes = (y >= 9'd64) && (y < 9'd128) && (hc > 10'd8) && (hc < 10'd711);

assign video = (border | xhair) ? 8'hFF :
               stripes          ? (y[0] ? 8'hFF : 8'h00) :
               lower            ? (check ? 8'hE0 : 8'h20) :
                                  bars;

endmodule
