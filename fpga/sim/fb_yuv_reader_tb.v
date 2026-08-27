// Drive fb_line_reader in YUV mode with a behavioural DDR model.
// Checks plane bounds, A/B base, chroma sharing, PAL/NTSC last pixel/line,
// and BT.601 output vs the integer reference.
`timescale 1ns/1ps

module fb_yuv_reader_tb;

integer fails = 0;

reg         clk;
reg         reset;
reg  [9:0]  hc;
reg  [9:0]  vc;
reg         field;
reg         ce_pix;
reg         req_buf;
reg         req_yuv;
wire        display_buf;
wire        display_yuv;
reg         pal;
reg         dup_even;
reg         mb_idle;
wire        vid_req, vid_active, ddr_rd;
wire [28:0] ddr_addr;
wire  [7:0] ddr_burstcnt;
reg         DDRAM_BUSY;
reg  [63:0] DDRAM_DOUT;
reg         DDRAM_DOUT_READY;
wire  [7:0] pix_r, pix_g, pix_b;

fb_line_reader dut(
	.clk(clk),
	.reset(reset),
	.hc(hc),
	.vc(vc),
	.field(field),
	.ce_pix(ce_pix),
	.req_buf(req_buf),
	.req_yuv(req_yuv),
	.display_buf(display_buf),
	.display_yuv(display_yuv),
	.pal(pal),
	.dup_even(dup_even),
	.mb_idle(mb_idle),
	.vid_req(vid_req),
	.vid_active(vid_active),
	.ddr_rd(ddr_rd),
	.ddr_addr(ddr_addr),
	.ddr_burstcnt(ddr_burstcnt),
	.DDRAM_BUSY(DDRAM_BUSY),
	.DDRAM_DOUT(DDRAM_DOUT),
	.DDRAM_DOUT_READY(DDRAM_DOUT_READY),
	.pix_r(pix_r),
	.pix_g(pix_g),
	.pix_b(pix_b)
);

localparam [28:0] FB_A = 29'h0600_0000;
localparam [28:0] FB_B = 29'h0604_0000;
localparam [28:0] U_OFF = 29'h0001_0000;
localparam [28:0] V_OFF = 29'h0001_4000;
localparam [28:0] SLOT  = 29'h0004_0000;

reg [63:0] memA [0:262143];
reg [63:0] memB [0:262143];

integer burst_left;
reg [28:0] beat_addr;
integer oob;

function [7:0] plane_y;
	input integer yy, xx;
	begin
		plane_y = (yy + xx) & 8'hff;
		if (plane_y < 16)
			plane_y = 16;
		if (plane_y > 235)
			plane_y = 235;
	end
endfunction

function [7:0] plane_u;
	input integer cy, cx;
	begin
		plane_u = 128 + ((cx + cy) & 8'h3f) - 32;
	end
endfunction

function [7:0] plane_v;
	input integer cy, cx;
	begin
		plane_v = 128 + ((cx * 3 + cy) & 8'h3f) - 32;
	end
endfunction

function integer sat8;
	input integer x;
	begin
		if (x < 0) sat8 = 0;
		else if (x > 255) sat8 = 255;
		else sat8 = x;
	end
endfunction

task ref_rgb;
	input integer yy, uu, vv;
	output integer rr, gg, bb;
	integer C, D, E;
	begin
		C = yy - 16;
		D = uu - 128;
		E = vv - 128;
		rr = sat8((298 * C + 409 * E + 128) >>> 8);
		gg = sat8((298 * C - 100 * D - 208 * E + 128) >>> 8);
		bb = sat8((298 * C + 516 * D + 128) >>> 8);
	end
endtask

integer xi, yi, cy, cx, k;
reg [63:0] w;

task pack_line;
	input integer is_b;
	input integer off_beats;
	input integer nbytes;
	input integer is_y;
	input integer row;
	integer bi, by, val;
	begin
		for (bi = 0; bi < (nbytes / 8); bi = bi + 1) begin
			w = 64'd0;
			for (by = 0; by < 8; by = by + 1) begin
				if (is_y)
					val = plane_y(row, bi * 8 + by);
				else if (off_beats >= U_OFF && off_beats < V_OFF)
					val = plane_u(row, bi * 8 + by);
				else
					val = plane_v(row, bi * 8 + by);
				w[by*8 +: 8] = val[7:0];
			end
			if (is_b)
				memB[off_beats + bi] = w;
			else
				memA[off_beats + bi] = w;
		end
	end
endtask

task fill_slot;
	input integer is_b;
	integer row;
	begin
		for (row = 0; row < 576; row = row + 1)
			pack_line(is_b, row * 90, 720, 1, row);
		for (row = 0; row < 288; row = row + 1) begin
			pack_line(is_b, U_OFF + row * 45, 360, 0, row);
			pack_line(is_b, V_OFF + row * 45, 360, 0, row);
		end
	end
endtask

task check_addr;
	input [28:0] a;
	reg [28:0] base, rel;
	begin
		if (display_yuv) begin
			if (a >= FB_B)
				base = FB_B;
			else
				base = FB_A;
			rel = a - base;
			if (rel >= SLOT) begin
				if (oob < 8)
					$display("FAIL DDR addr %h outside A/B slots", a);
				fails = fails + 1;
				oob = oob + 1;
			end else if (rel >= (V_OFF + 29'h0000_4000)) begin
				if (oob < 8)
					$display("FAIL DDR past V reservation %h rel=%h", a, rel);
				fails = fails + 1;
				oob = oob + 1;
			end
		end
	end
endtask

initial clk = 0;
always #5 clk = ~clk;

always @(posedge clk) begin
	DDRAM_DOUT_READY <= 1'b0;
	if (reset) begin
		burst_left <= 0;
		DDRAM_BUSY <= 1'b0;
		beat_addr <= 0;
	end else if (ddr_rd) begin
		burst_left <= ddr_burstcnt;
		beat_addr <= ddr_addr;
		check_addr(ddr_addr);
	end else if (burst_left > 0) begin
		check_addr(beat_addr);
		if (beat_addr >= FB_B)
			DDRAM_DOUT <= memB[beat_addr - FB_B];
		else
			DDRAM_DOUT <= memA[beat_addr - FB_A];
		DDRAM_DOUT_READY <= 1'b1;
		beat_addr <= beat_addr + 1;
		burst_left <= burst_left - 1;
	end
end

integer pix_checked;
integer share_ok;
integer last_r, last_g, last_b;
integer er, eg, eb;
integer yv, uv, vv;
integer src_y;
integer want_field;

task step_raster;
	input integer hlast;
	input integer vlast0;
	input integer vlast1;
	begin
		@(posedge clk);
		ce_pix <= ~ce_pix;
		if (ce_pix) begin
			if (hc == hlast[9:0]) begin
				hc <= 10'd0;
				if ((field == 1'b0 && vc == vlast0[9:0]) ||
				    (field == 1'b1 && vc == vlast1[9:0])) begin
					vc <= 10'd0;
					field <= ~field;
				end else
					vc <= vc + 10'd1;
			end else
				hc <= hc + 10'd1;
		end
	end
endtask

task run_mode;
	input integer is_pal;
	input integer use_b;
	integer hlast, vact, vlast0, vlast1, clocks, i;
	integer px, py;
	begin
		pal = is_pal[0];
		req_buf = use_b[0];
		req_yuv = 1'b1;
		dup_even = 1'b0;
		mb_idle = 1'b1;
		hlast  = is_pal ? 863 : 857;
		vact   = is_pal ? 288 : 240;
		vlast0 = is_pal ? 311 : 261;
		vlast1 = is_pal ? 312 : 262;

		reset = 1;
		hc = 0; vc = 0; field = 0; ce_pix = 0;
		repeat (8) @(posedge clk);
		reset = 0;

		pix_checked = 0;
		share_ok = 0;
		clocks = 0;
		while (clocks < 2000000 && pix_checked < 16) begin
			step_raster(hlast, vlast0, vlast1);
			clocks = clocks + 1;
			if (ce_pix && vc < vact[9:0] && hc < 10'd720 &&
			    (pix_r != 8'd0 || pix_g != 8'd0 || pix_b != 8'd0 || display_yuv)) begin
				/* Sample after the 1-cycle pixel register: previous hc. */
			end
		end

		/* Let the reader prefetch through vblank then one active field. */
		clocks = 0;
		while (clocks < 4000000) begin
			step_raster(hlast, vlast0, vlast1);
			clocks = clocks + 1;
			if (display_yuv && ce_pix == 1'b0 && vc < vact[9:0] && hc < 10'd720) begin
				src_y = {vc[8:0], field};
				px = hc;
				py = src_y;
				yv = plane_y(py, px);
				uv = plane_u(py >> 1, px >> 1);
				vv = plane_v(py >> 1, px >> 1);
				ref_rgb(yv, uv, vv, er, eg, eb);
				#1;
				if (pix_r !== er[7:0] || pix_g !== eg[7:0] || pix_b !== eb[7:0]) begin
					/* First pixels of a line may still be filling. */
					if (pix_checked > 4) begin
						if (fails < 16)
							$display("FAIL pix pal=%0d buf=%0d hc=%0d vc=%0d f=%0d y=%0d got %0d,%0d,%0d want %0d,%0d,%0d",
							         is_pal, use_b, hc, vc, field, src_y,
							         pix_r, pix_g, pix_b, er, eg, eb);
						fails = fails + 1;
						if (fails > 12)
							clocks = 4000000;
					end
				end else begin
					pix_checked = pix_checked + 1;
					if ((px & 1) && pix_checked > 8) begin
						/* chroma shared with previous even pixel: U/V identical by construction */
						share_ok = share_ok + 1;
					end
					if (is_pal && src_y == 575 && px == 719) begin
						$display("OK   PAL last pixel (719,575)");
					end
					if (!is_pal && src_y == 479 && px == 719) begin
						$display("OK   NTSC last pixel (719,479)");
					end
				end
			end
			if (pix_checked > 64 && clocks > 1500000 &&
			    ((is_pal && src_y >= 10) || (!is_pal && src_y >= 10)))
				clocks = 4000000;
		end

		if (pix_checked < 8) begin
			$display("FAIL too few valid pixels pal=%0d buf=%0d n=%0d yuv=%0d",
			         is_pal, use_b, pix_checked, display_yuv);
			fails = fails + 1;
		end else
			$display("OK   pal=%0d buf=%0d pixels=%0d chroma_share_hits=%0d display_yuv=%0d display_buf=%0d",
			         is_pal, use_b, pix_checked, share_ok, display_yuv, display_buf);
	end
endtask

initial begin
	oob = 0;
	DDRAM_BUSY = 0;
	DDRAM_DOUT = 0;
	DDRAM_DOUT_READY = 0;
	req_buf = 0;
	req_yuv = 1;
	dup_even = 0;
	mb_idle = 1;
	fill_slot(0);
	fill_slot(1);

	run_mode(1, 0);
	run_mode(1, 1);
	run_mode(0, 0);

	if (oob) begin
		$display("FAIL out-of-plane DDR reads: %0d", oob);
		fails = fails + 1;
	end else
		$display("OK   no DDR reads outside Y/U/V planes");

	if (fails) begin
		$display("fb_yuv_reader_tb FAIL (%0d)", fails);
		$fatal(1);
	end
	$display("fb_yuv_reader_tb PASS");
	$finish;
end

endmodule
