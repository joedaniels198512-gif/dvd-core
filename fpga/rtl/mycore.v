
module mycore
(
	input         clk,
	input         reset,
	
	input         pal,
	input         scandouble,

	output reg    ce_pix,
	output        hvcnt_atzero,

	output reg    HBlank,
	output reg    HSync,
	output reg    VBlank,
	output reg    VSync,

	output  [7:0] video
);

reg   [9:0] hc = 0;
reg   [9:0] vc = 0;
reg   [9:0] vvc = 0;
reg  [63:0] rnd_reg;

wire  [5:0] rnd_c = {rnd_reg[0],rnd_reg[1],rnd_reg[2],rnd_reg[2],rnd_reg[2],rnd_reg[2]};
wire [63:0] rnd;

lfsr random(rnd);

// In case the H/V counters need to be cleared after a reset, feed a signal to release
// the reset at the right time.
assign hvcnt_atzero = ce_pix && !hc && !vc;

always @(posedge clk) begin
	if(scandouble) ce_pix <= 1;
		else ce_pix <= ~ce_pix;

	if(ce_pix) begin
		if(hc == 637) begin
			hc <= 0;
			if(vc >= (pal ? (scandouble ? 623 : 311) : (scandouble ? 523 : 261))) begin
				vc <= 0;
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
	if (hc == 529) HBlank <= 1;
		else if (hc == 0) HBlank <= 0;

	if (hc == 544) begin
		HSync <= 1;

		if(pal) begin
			if(vc == (scandouble ? 609 : 304)) VSync <= 1;
				else if (vc == (scandouble ? 617 : 308)) VSync <= 0;

			if(vc == (scandouble ? 601 : 300)) VBlank <= 1;
				else if (vc == 0) VBlank <= 0;
		end
		else begin
			if(vc == (scandouble ? 490 : 245)) VSync <= 1;
				else if (vc == (scandouble ? 496 : 248)) VSync <= 0;

			if(vc == (scandouble ? 480 : 240)) VBlank <= 1;
				else if (vc == 0) VBlank <= 0;
		end
	end
	
	if (hc == 590) HSync <= 0;
end

reg  [7:0] cos_out;
wire [5:0] cos_g = cos_out[7:3]+6'd32;
cos cos(vvc + {vc>>scandouble, 2'b00}, cos_out);

// Static monochrome test card for the analogue VGA_* path.
// LFSR/cos remain instantiated (unused) so timing counters stay untouched.
wire [9:0] vact = pal ? (scandouble ? 10'd601 : 10'd300)
                      : (scandouble ? 10'd480 : 10'd240);

wire border = (hc < 10'd8) || (hc > 10'd520) ||
              (vc < 10'd8) || (vc > (vact - 10'd9));

wire [9:0] mid_h = 10'd264;
wire [9:0] mid_v = vact >> 1;
wire cross = ((hc >= (mid_h - 10'd1)) && (hc <= (mid_h + 10'd1))) ||
             ((vc >= (mid_v - 10'd1)) && (vc <= (mid_v + 10'd1)));

wire [7:0] bars =
	(hc < 10'd66)  ? 8'h00 :
	(hc < 10'd132) ? 8'h24 :
	(hc < 10'd198) ? 8'h48 :
	(hc < 10'd264) ? 8'h6C :
	(hc < 10'd330) ? 8'h90 :
	(hc < 10'd396) ? 8'hB4 :
	(hc < 10'd462) ? 8'hD8 :
	                 8'hFC;

wire lower = (vc >= (vact - (vact >> 2)));
wire check = hc[4] ^ vc[4];

assign video = (border | cross) ? 8'hFF :
               lower            ? (check ? 8'hE0 : 8'h20) :
                                  bars;

endmodule
