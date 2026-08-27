// Drive fb_line_reader with a behavioural DDR model.
// Progressive PAL/NTSC, interlaced PAL/NTSC, atomic A/B+metadata switch,
// frame-to-frame prog/intl transitions, dup_even, reset/pal_chg, BGR0.
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
reg         req_intl;
reg         req_tff;
wire        display_buf;
wire        display_yuv;
wire        display_intl;
wire        display_tff;
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
	.req_intl(req_intl),
	.req_tff(req_tff),
	.display_buf(display_buf),
	.display_yuv(display_yuv),
	.display_intl(display_intl),
	.display_tff(display_tff),
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
integer chroma_row_fail;
integer mix_fail;
integer mid_frame_chg;
integer odd_cy_dup;
integer bgr_yuv_leak;
integer latch_atomic_fail;
integer just_latched;
integer pix_checked;
integer share_ok;
integer er, eg, eb;
integer yv, uv, vv;
integer src_y;
integer saw_y0, saw_y1, saw_ylast_top, saw_ylast_bot;
integer last_pix_ok;
integer sg_before, sg_after, sg_reqp, sg_was_latch;
integer samp_hc, samp_vc, samp_field, samp_ce, samp_yuv;

reg pal_d_tb;
wire pal_chg_tb = pal_d_tb !== pal;

function integer chroma_row;
	input integer y;
	input integer intl;
	begin
		if (intl)
			chroma_row = ((y >> 2) << 1) | (y & 1);
		else
			chroma_row = y >> 1;
	end
endfunction

function integer at_latch;
	input integer vact, hlast;
	begin
		at_latch = (ce_pix && field &&
		            (vc == (vact[9:0] - 10'd2)) &&
		            (hc == hlast[9:0]));
	end
endfunction

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

function [7:0] bgr_r;
	input integer yy, xx;
	begin
		bgr_r = (yy + 3) & 8'hff;
	end
endfunction

function [7:0] bgr_g;
	input integer yy, xx;
	begin
		bgr_g = (xx + 5) & 8'hff;
	end
endfunction

function [7:0] bgr_b;
	input integer yy, xx;
	begin
		bgr_b = 8'h40 + (yy[0] << 4);
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

task fill_bgr_slot;
	input integer is_b;
	integer row, bi, xx;
	begin
		for (row = 0; row < 576; row = row + 1) begin
			for (bi = 0; bi < 360; bi = bi + 1) begin
				xx = bi * 2;
				w = 64'd0;
				w[7:0]    = bgr_b(row, xx);
				w[15:8]   = bgr_g(row, xx);
				w[23:16]  = bgr_r(row, xx);
				w[31:24]  = 8'd0;
				w[39:32]  = bgr_b(row, xx + 1);
				w[47:40]  = bgr_g(row, xx + 1);
				w[55:48]  = bgr_r(row, xx + 1);
				w[63:56]  = 8'd0;
				if (is_b)
					memB[row * 360 + bi] = w;
				else
					memA[row * 360 + bi] = w;
			end
		end
	end
endtask

task check_addr;
	input [28:0] a;
	reg [28:0] base, rel, u_rel;
	integer yfill, cy_exp, cy_got, is_u;
	begin
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
		end else if (display_yuv) begin
			if ((base == FB_B) !== display_buf) begin
				mix_fail = mix_fail + 1;
				if (mix_fail < 8)
					$display("FAIL mix: DDR base vs display_buf a=%h disp=%0d",
					         a, display_buf);
				fails = fails + 1;
			end
			if (rel >= (V_OFF + 29'h0000_4000)) begin
				if (oob < 8)
					$display("FAIL DDR past V reservation %h rel=%h", a, rel);
				fails = fails + 1;
				oob = oob + 1;
			end
			if (rel >= U_OFF && rel < (V_OFF + 29'h0000_4000)) begin
				is_u = (rel < V_OFF);
				u_rel = is_u ? (rel - U_OFF) : (rel - V_OFF);
				cy_got = u_rel / 45;
				yfill = dut.y_fill_r;
				cy_exp = chroma_row(yfill, display_intl);
				if (cy_got !== cy_exp) begin
					chroma_row_fail = chroma_row_fail + 1;
					if (chroma_row_fail < 8)
						$display("FAIL chroma row y=%0d intl=%0d got c%0d want c%0d",
						         yfill, display_intl, cy_got, cy_exp);
					fails = fails + 1;
				end
				if (display_intl && ((yfill & 1) !== (cy_got & 1))) begin
					$display("FAIL cross-field chroma y=%0d cy=%0d", yfill, cy_got);
					fails = fails + 1;
				end
				if (dup_even && display_intl && (cy_got & 1)) begin
					odd_cy_dup = odd_cy_dup + 1;
					if (odd_cy_dup < 8)
						$display("FAIL dup_even fetched odd chroma row %0d y=%0d",
						         cy_got, yfill);
					fails = fails + 1;
				end
			end
		end
	end
endtask

initial clk = 0;
always #5 clk = ~clk;

always @(posedge clk) begin
	pal_d_tb <= pal;
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

task raster_consts;
	input integer is_pal;
	output integer hlast;
	output integer vact;
	output integer vlast0;
	output integer vlast1;
	output integer ylast;
	begin
		hlast  = is_pal ? 863 : 857;
		vact   = is_pal ? 288 : 240;
		vlast0 = is_pal ? 311 : 261;
		vlast1 = is_pal ? 312 : 262;
		ylast  = is_pal ? 575 : 479;
	end
endtask

task rst_raster;
	begin
		reset = 1;
		hc = 0; vc = 0; field = 0; ce_pix = 0;
		repeat (8) @(posedge clk);
		reset = 0;
		repeat (2) @(posedge clk);
	end
endtask

task step_guard;
	input integer hlast;
	input integer vact;
	input integer vlast0;
	input integer vlast1;
	begin
		step_raster(hlast, vlast0, vlast1);
		samp_hc = hc;
		samp_vc = vc;
		samp_field = field;
		samp_ce = ce_pix;
		samp_yuv = display_yuv;
		sg_before = {display_tff, display_intl, display_yuv, display_buf};
		sg_was_latch = at_latch(vact, hlast);
		just_latched = sg_was_latch;
		#1;
		sg_after = {display_tff, display_intl, display_yuv, display_buf};
		sg_reqp = {req_tff, req_intl, req_yuv, req_buf};
		if (!reset && !pal_chg_tb && (sg_after !== sg_before)) begin
			if (!sg_was_latch) begin
				mid_frame_chg = mid_frame_chg + 1;
				if (mid_frame_chg < 8)
					$display("FAIL display_* changed mid-frame hc=%0d vc=%0d f=%0d",
					         samp_hc, samp_vc, samp_field);
				fails = fails + 1;
			end else begin
				if (sg_after !== sg_reqp) begin
					latch_atomic_fail = latch_atomic_fail + 1;
					if (latch_atomic_fail < 8)
						$display("FAIL latch not atomic got=%b req=%b", sg_after, sg_reqp);
					fails = fails + 1;
				end
			end
		end
	end
endtask

task check_yuv_pix;
	input integer vact, ylast, want_intl, use_dup;
	integer px, py, cy_e, last_top;
	begin
		if (samp_yuv && samp_ce == 1'b0 && samp_vc < vact[9:0] &&
		    samp_hc < 10'd720) begin
			py = use_dup ? {samp_vc[8:0], 1'b0} : {samp_vc[8:0], samp_field[0]};
			px = samp_hc;
			cy_e = chroma_row(py, want_intl);
			yv = plane_y(py, px);
			uv = plane_u(cy_e, px >> 1);
			vv = plane_v(cy_e, px >> 1);
			ref_rgb(yv, uv, vv, er, eg, eb);
			if (pix_r !== er[7:0] || pix_g !== eg[7:0] || pix_b !== eb[7:0]) begin
				if (pix_checked > 8) begin
					if (fails < 20)
						$display("FAIL pix y=%0d x=%0d intl=%0d got %0d,%0d,%0d want %0d,%0d,%0d",
						         py, px, want_intl, pix_r, pix_g, pix_b, er, eg, eb);
					fails = fails + 1;
				end
			end else begin
				pix_checked = pix_checked + 1;
				if (py == 0)
					saw_y0 = 1;
				if (py == 1)
					saw_y1 = 1;
				last_top = ylast - 1;
				if (py == last_top)
					saw_ylast_top = 1;
				if (py == ylast) begin
					saw_ylast_bot = 1;
					last_pix_ok = 1;
				end
			end
		end
	end
endtask

task run_prog_mode;
	input integer is_pal;
	input integer use_b;
	integer hlast, vact, vlast0, vlast1, ylast, clocks;
	begin
		raster_consts(is_pal, hlast, vact, vlast0, vlast1, ylast);
		pal = is_pal[0];
		req_buf = use_b[0];
		req_yuv = 1'b1;
		req_intl = 1'b0;
		req_tff = 1'b0;
		dup_even = 1'b0;
		mb_idle = 1'b1;
		rst_raster();

		pix_checked = 0;
		share_ok = 0;
		clocks = 0;
		while (clocks < 2500000) begin
			step_guard(hlast, vact, vlast0, vlast1);
			clocks = clocks + 1;
			check_yuv_pix(vact, ylast, 0, 0);
			if (pix_checked > 80 && clocks > 400000)
				clocks = 2500000;
		end
		if (pix_checked < 8) begin
			$display("FAIL progressive pal=%0d buf=%0d pixels=%0d",
			         is_pal, use_b, pix_checked);
			fails = fails + 1;
		end else
			$display("OK   progressive pal=%0d buf=%0d pixels=%0d display_yuv=%0d",
			         is_pal, use_b, pix_checked, display_yuv);
	end
endtask

task run_intl_full;
	input integer is_pal;
	input integer use_b;
	integer hlast, vact, vlast0, vlast1, ylast, clocks;
	integer want_c_top, want_c_bot;
	begin
		raster_consts(is_pal, hlast, vact, vlast0, vlast1, ylast);
		want_c_top = chroma_row(ylast - 1, 1);
		want_c_bot = chroma_row(ylast, 1);
		pal = is_pal[0];
		req_buf = use_b[0];
		req_yuv = 1'b1;
		req_intl = 1'b1;
		req_tff = 1'b1;
		dup_even = 1'b0;
		mb_idle = 1'b1;
		rst_raster();
		pix_checked = 0;
		saw_y0 = 0; saw_y1 = 0;
		saw_ylast_top = 0; saw_ylast_bot = 0;
		last_pix_ok = 0;
		clocks = 0;
		while (clocks < 3500000) begin
			step_guard(hlast, vact, vlast0, vlast1);
			clocks = clocks + 1;
			if (display_yuv && display_intl && display_buf == use_b[0])
				check_yuv_pix(vact, ylast, 1, 0);
			if (saw_ylast_bot && saw_y0 && saw_y1 && pix_checked > 200)
				clocks = 3500000;
		end
		if (want_c_top !== (is_pal ? 286 : 238) ||
		    want_c_bot !== (is_pal ? 287 : 239)) begin
			$display("FAIL last-line chroma map pal=%0d top=%0d bot=%0d",
			         is_pal, want_c_top, want_c_bot);
			fails = fails + 1;
		end else
			$display("OK   %s y%0d->c%0d y%0d->c%0d",
			         is_pal ? "PAL" : "NTSC",
			         ylast - 1, want_c_top, ylast, want_c_bot);
		if (!saw_y0 || !saw_y1) begin
			$display("FAIL %s missing first field lines y0=%0d y1=%0d",
			         is_pal ? "PAL" : "NTSC", saw_y0, saw_y1);
			fails = fails + 1;
		end else
			$display("OK   %s first top/bottom field lines",
			         is_pal ? "PAL" : "NTSC");
		if (!saw_ylast_top || !saw_ylast_bot) begin
			$display("FAIL %s missing last field lines top=%0d bot=%0d pixels=%0d",
			         is_pal ? "PAL" : "NTSC",
			         saw_ylast_top, saw_ylast_bot, pix_checked);
			fails = fails + 1;
		end else
			$display("OK   %s last top/bottom field pixels pal last=%0d",
			         is_pal ? "PAL" : "NTSC", last_pix_ok);
		if (pix_checked < 64) begin
			$display("FAIL interlaced pal=%0d too few pixels %0d",
			         is_pal, pix_checked);
			fails = fails + 1;
		end else
			$display("OK   interlaced pal=%0d buf=%0d pixels=%0d",
			         is_pal, use_b, pix_checked);
	end
endtask

task run_atomic_switch;
	integer hlast, vact, vlast0, vlast1, ylast, clocks;
	integer armed, saw_hold, saw_latch;
	reg [3:0] held;
	begin
		raster_consts(1, hlast, vact, vlast0, vlast1, ylast);
		pal = 1;
		req_buf = 0; req_yuv = 1; req_intl = 0; req_tff = 0;
		dup_even = 0;
		mb_idle = 1;
		rst_raster();
		armed = 0; saw_hold = 0; saw_latch = 0;
		clocks = 0;
		while (clocks < 3000000) begin
			step_guard(hlast, vact, vlast0, vlast1);
			clocks = clocks + 1;
			if (!armed && display_yuv && field == 1'b0 && vc == 10'd40 &&
			    hc == 10'd10) begin
				held = {display_tff, display_intl, display_yuv, display_buf};
				req_buf = 1; req_yuv = 1; req_intl = 1; req_tff = 1;
				armed = 1;
			end
			if (armed && !saw_latch) begin
				if ({display_tff, display_intl, display_yuv, display_buf} !== held)
					;
				else
					saw_hold = 1;
				if (display_buf && display_yuv && display_intl && display_tff) begin
					saw_latch = 1;
					clocks = 3000000;
				end
			end
		end
		if (!armed || !saw_hold) begin
			$display("FAIL atomic: did not hold old display_* mid-frame");
			fails = fails + 1;
		end else if (!saw_latch) begin
			$display("FAIL atomic: display_* never took new req together");
			fails = fails + 1;
		end else if (!(display_buf && display_yuv && display_intl && display_tff)) begin
			$display("FAIL atomic: torn latch buf=%0d yuv=%0d intl=%0d tff=%0d",
			         display_buf, display_yuv, display_intl, display_tff);
			fails = fails + 1;
		end else
			$display("OK   atomic A/B+metadata switch (hold then same-cycle latch)");
	end
endtask

task run_mode_transitions;
	integer hlast, vact, vlast0, vlast1, ylast, clocks, latches;
	integer n_intl_a, n_prog_b, n_intl_a2;
	begin
		raster_consts(1, hlast, vact, vlast0, vlast1, ylast);
		pal = 1;
		dup_even = 0;
		mb_idle = 1;
		req_buf = 0; req_yuv = 1; req_intl = 1; req_tff = 1;
		rst_raster();
		latches = 0;
		n_intl_a = 0; n_prog_b = 0; n_intl_a2 = 0;
		pix_checked = 0;
		clocks = 0;
		while (clocks < 5000000 && latches < 4) begin
			step_guard(hlast, vact, vlast0, vlast1);
			clocks = clocks + 1;
			if (samp_yuv && samp_ce == 1'b0 && samp_vc < vact[9:0] &&
			    samp_hc < 10'd720 &&
			    !(samp_field && samp_vc >= (vact[9:0] - 10'd2))) begin
				check_yuv_pix(vact, ylast, display_intl, 0);
				if (display_intl && display_buf == 1'b0 && latches == 1)
					n_intl_a = n_intl_a + 1;
				if (!display_intl && display_buf == 1'b1 && latches == 2)
					n_prog_b = n_prog_b + 1;
				if (display_intl && display_buf == 1'b0 && latches == 3)
					n_intl_a2 = n_intl_a2 + 1;
			end
			if (just_latched) begin
				latches = latches + 1;
				if (latches == 1) begin
					req_buf = 1; req_intl = 0; req_tff = 0;
				end else if (latches == 2) begin
					req_buf = 0; req_intl = 1; req_tff = 1;
				end
			end
		end
		if (n_intl_a < 8 || n_prog_b < 8 || n_intl_a2 < 8) begin
			$display("FAIL transitions intlA=%0d progB=%0d intlA2=%0d latches=%0d",
			         n_intl_a, n_prog_b, n_intl_a2, latches);
			fails = fails + 1;
		end else
			$display("OK   prog/intl transitions intlA=%0d progB=%0d intlA2=%0d",
			         n_intl_a, n_prog_b, n_intl_a2);
	end
endtask

task run_dup_even;
	integer hlast, vact, vlast0, vlast1, ylast, clocks;
	begin
		raster_consts(1, hlast, vact, vlast0, vlast1, ylast);
		pal = 1;
		req_buf = 0; req_yuv = 1; req_intl = 1; req_tff = 1;
		dup_even = 1;
		mb_idle = 1;
		rst_raster();
		pix_checked = 0;
		odd_cy_dup = 0;
		clocks = 0;
		while (clocks < 2500000) begin
			step_guard(hlast, vact, vlast0, vlast1);
			clocks = clocks + 1;
			if (display_yuv && display_intl)
				check_yuv_pix(vact, ylast, 1, 1);
			if (pix_checked > 80 && clocks > 600000)
				clocks = 2500000;
		end
		if (odd_cy_dup) begin
			$display("FAIL dup_even odd chroma fetches %0d", odd_cy_dup);
			fails = fails + 1;
		end else if (pix_checked < 8) begin
			$display("FAIL dup_even too few pixels %0d", pix_checked);
			fails = fails + 1;
		end else
			$display("OK   dup_even interlaced even-field chroma pixels=%0d",
			         pix_checked);
	end
endtask

task run_reset_palchg;
	integer hlast, vact, vlast0, vlast1, ylast, clocks;
	begin
		raster_consts(1, hlast, vact, vlast0, vlast1, ylast);
		pal = 1;
		req_buf = 1; req_yuv = 1; req_intl = 1; req_tff = 1;
		dup_even = 0;
		mb_idle = 1;
		rst_raster();
		clocks = 0;
		while (clocks < 2000000 &&
		       !(display_buf && display_yuv && display_intl && display_tff)) begin
			step_guard(hlast, vact, vlast0, vlast1);
			clocks = clocks + 1;
		end
		if (!(display_buf && display_yuv && display_intl && display_tff)) begin
			$display("FAIL reset/pal: never latched metadata on");
			fails = fails + 1;
		end else begin
			reset = 1;
			@(posedge clk); #1;
			if (display_buf || display_yuv || display_intl || display_tff) begin
				$display("FAIL reset did not clear display_*");
				fails = fails + 1;
			end else
				$display("OK   reset clears display_buf/yuv/intl/tff together");
			reset = 0;
			repeat (4) @(posedge clk);
			req_buf = 1; req_yuv = 1; req_intl = 1; req_tff = 1;
			clocks = 0;
			while (clocks < 2000000 &&
			       !(display_buf && display_yuv && display_intl)) begin
				step_guard(hlast, vact, vlast0, vlast1);
				clocks = clocks + 1;
			end
			pal = 0;
			@(posedge clk); #1;
			if (display_buf || display_yuv || display_intl || display_tff) begin
				$display("FAIL pal_chg did not clear display_* (buf=%0d yuv=%0d intl=%0d tff=%0d)",
				         display_buf, display_yuv, display_intl, display_tff);
				fails = fails + 1;
			end else
				$display("OK   pal_chg clears display_buf/yuv/intl/tff together");
		end
	end
endtask

task run_bgr_regression;
	integer hlast, vact, vlast0, vlast1, ylast, clocks, ok, py, px;
	begin
		fill_bgr_slot(0);
		raster_consts(1, hlast, vact, vlast0, vlast1, ylast);
		pal = 1;
		req_buf = 0; req_yuv = 0; req_intl = 1; req_tff = 1;
		dup_even = 0;
		mb_idle = 1;
		rst_raster();
		ok = 0;
		clocks = 0;
		while (clocks < 2000000) begin
			step_guard(hlast, vact, vlast0, vlast1);
			clocks = clocks + 1;
			if (!samp_yuv && samp_ce == 1'b0 && samp_vc < vact[9:0] &&
			    samp_hc < 10'd720) begin
				py = {samp_vc[8:0], samp_field[0]};
				px = samp_hc;
				if (pix_r === bgr_r(py, px) &&
				    pix_g === bgr_g(py, px) &&
				    pix_b === bgr_b(py, px))
					ok = ok + 1;
				else if (ok > 8) begin
					if (fails < 16)
						$display("FAIL BGR y=%0d x=%0d got %0d,%0d,%0d",
						         py, px, pix_r, pix_g, pix_b);
					fails = fails + 1;
				end
			end
			if (ok > 40 && clocks > 400000)
				clocks = 2000000;
		end
		if (display_yuv) begin
			$display("FAIL BGR: display_yuv set while req_yuv=0");
			fails = fails + 1;
		end
		if (ok < 8) begin
			$display("FAIL BGR too few matching pixels %0d", ok);
			fails = fails + 1;
		end else
			$display("OK   BGR0 regression req_intl=1 ignored pixels=%0d", ok);
		fill_slot(0);
		fill_slot(1);
	end
endtask

initial begin
	oob = 0;
	chroma_row_fail = 0;
	mix_fail = 0;
	mid_frame_chg = 0;
	odd_cy_dup = 0;
	bgr_yuv_leak = 0;
	latch_atomic_fail = 0;
	DDRAM_BUSY = 0;
	DDRAM_DOUT = 0;
	DDRAM_DOUT_READY = 0;
	req_buf = 0;
	req_yuv = 1;
	req_intl = 0;
	req_tff = 0;
	dup_even = 0;
	mb_idle = 1;
	pal = 1;
	pal_d_tb = 1;
	fill_slot(0);
	fill_slot(1);

	run_prog_mode(1, 0);
	run_prog_mode(1, 1);
	run_prog_mode(0, 0);

	run_intl_full(1, 0);
	run_intl_full(0, 0);

	run_atomic_switch();
	run_mode_transitions();
	run_dup_even();
	run_reset_palchg();
	run_bgr_regression();

	if (oob) begin
		$display("FAIL out-of-plane DDR reads: %0d", oob);
		fails = fails + 1;
	end else
		$display("OK   no DDR reads outside Y/U/V / slot bounds");
	if (chroma_row_fail)
		$display("FAIL chroma-row address mismatches: %0d", chroma_row_fail);
	else
		$display("OK   chroma addresses match latched display_intl");
	if (mix_fail)
		$display("FAIL buffer/metadata mix: %0d", mix_fail);
	else
		$display("OK   no old-buffer + new-metadata pairing");
	if (mid_frame_chg)
		$display("FAIL mid-frame display_* changes: %0d", mid_frame_chg);
	else
		$display("OK   display_* only change at wrap / reset / pal_chg");

	if (fails) begin
		$display("fb_yuv_reader_tb FAIL (%0d)", fails);
		$fatal(1);
	end
	$display("fb_yuv_reader_tb PASS");
	$finish;
end

endmodule
