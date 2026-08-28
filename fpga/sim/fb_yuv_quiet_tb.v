// Reproduce hold-until-accepted + ddr_quiet vs YUV 3-plane fills.
// Uses the same outstanding-beat tracker as DVD.sv.
`timescale 1ns/1ps

module fb_yuv_quiet_tb;

integer fails = 0;
integer n_y_cmd = 0, n_u_cmd = 0, n_v_cmd = 0, n_bgr_cmd = 0;
integer n_issue_wait = 0, n_issue_wait_max = 0;
integer n_line_ok = 0, n_disp_ok = 0, n_disp_black = 0;
integer n_stale_ready = 0;
integer n_pend_peak = 0;

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
wire        display_buf, display_yuv, display_intl, display_tff;
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

wire        ddr_rd_acc = ddr_rd & ~DDRAM_BUSY;
reg  [7:0]  ddr_pend = 8'd0;
wire        ddr_quiet = (ddr_pend == 8'd0);

always @(posedge clk) begin
	case ({ddr_rd_acc, DDRAM_DOUT_READY})
		2'b10:   ddr_pend <= ddr_pend + ddr_burstcnt;
		2'b01:   ddr_pend <= ddr_pend - {7'd0, |ddr_pend};
		2'b11:   ddr_pend <= ddr_pend + ddr_burstcnt - 8'd1;
		default: ;
	endcase
	if (ddr_pend > n_pend_peak)
		n_pend_peak = ddr_pend;
end

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
	.ddr_quiet(ddr_quiet),
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
localparam [28:0] U_OFF = 29'h0001_0000;
localparam [28:0] V_OFF = 29'h0001_4000;

// Avalon slave: waitrequest high for 1 cycle after accept; first beat
// 3 cycles later (registered READY, like f2sdram). Exactly burstcount beats.
integer burst_left;
reg [28:0] beat_addr;
integer busy_left;
integer lat_left;

initial clk = 0;
always #5 clk = ~clk;

always @(posedge clk) begin
	DDRAM_DOUT_READY <= 1'b0;
	if (reset) begin
		burst_left <= 0;
		busy_left <= 0;
		lat_left <= 0;
		DDRAM_BUSY <= 1'b0;
		beat_addr <= 0;
	end else begin
		if (busy_left > 0) begin
			busy_left <= busy_left - 1;
			DDRAM_BUSY <= (busy_left > 1);
		end else
			DDRAM_BUSY <= 1'b0;

		if (ddr_rd && !DDRAM_BUSY) begin
			if (burst_left != 0)
				n_stale_ready = n_stale_ready + 1;
			burst_left <= ddr_burstcnt;
			beat_addr <= ddr_addr;
			lat_left <= 3;
			busy_left <= 2;
			DDRAM_BUSY <= 1'b1;
			if (display_yuv) begin
				if (ddr_addr >= (FB_A + V_OFF))
					n_v_cmd = n_v_cmd + 1;
				else if (ddr_addr >= (FB_A + U_OFF))
					n_u_cmd = n_u_cmd + 1;
				else
					n_y_cmd = n_y_cmd + 1;
			end else
				n_bgr_cmd = n_bgr_cmd + 1;
		end else if (lat_left > 0) begin
			lat_left <= lat_left - 1;
		end else if (burst_left > 0) begin
			DDRAM_DOUT <= {32'hA5A5A5A5, beat_addr[31:0]};
			DDRAM_DOUT_READY <= 1'b1;
			beat_addr <= beat_addr + 1;
			burst_left <= burst_left - 1;
		end
	end
end

always @(posedge clk) begin
	if (!reset && dut.st == 2'd1 && !ddr_rd) begin
		n_issue_wait = n_issue_wait + 1;
		if (dut.st == 2'd1)
			n_issue_wait_max = n_issue_wait;
	end else if (dut.st != 2'd1)
		n_issue_wait = 0;
end

task step_raster;
	input integer hlast, vlast0, vlast1;
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
	input integer want_yuv;
	input integer clocks_max;
	integer clocks, hlast, vlast0, vlast1;
	begin
		hlast = 863; vlast0 = 311; vlast1 = 312;
		reset = 1;
		hc = 0; vc = 0; field = 0; ce_pix = 0;
		pal = 1; dup_even = 0; mb_idle = 1;
		req_buf = 0; req_yuv = want_yuv[0]; req_intl = want_yuv[0]; req_tff = 1;
		n_y_cmd = 0; n_u_cmd = 0; n_v_cmd = 0; n_bgr_cmd = 0;
		n_line_ok = 0; n_disp_ok = 0; n_disp_black = 0;
		n_pend_peak = 0; n_stale_ready = 0;
		repeat (8) @(posedge clk);
		reset = 0;
		clocks = 0;
		while (clocks < clocks_max) begin
			step_raster(hlast, vlast0, vlast1);
			clocks = clocks + 1;
			if (ce_pix == 1'b0 && vc < 10'd288 && hc < 10'd720) begin
				if (pix_r != 8'd0 || pix_g != 8'd0 || pix_b != 8'd0)
					n_disp_ok = n_disp_ok + 1;
				else
					n_disp_black = n_disp_black + 1;
			end
			if (dut.buf_ok[0] && dut.buf_ok[1])
				n_line_ok = n_line_ok + 1;
		end
		$display("mode yuv=%0d display_yuv=%0d y/u/v/bgr cmds=%0d/%0d/%0d/%0d",
		         want_yuv, display_yuv, n_y_cmd, n_u_cmd, n_v_cmd, n_bgr_cmd);
		$display("  pix_ok=%0d pix_black=%0d pend_peak=%0d stale_overlap=%0d buf_ok=%0d%0d st=%0d pend=%0d",
		         n_disp_ok, n_disp_black, n_pend_peak, n_stale_ready,
		         dut.buf_ok[0], dut.buf_ok[1], dut.st, ddr_pend);
		if (want_yuv) begin
			if (!display_yuv) begin
				$display("FAIL YUV never latched display_yuv");
				fails = fails + 1;
			end
			if (n_u_cmd < 8 || n_v_cmd < 8) begin
				$display("FAIL YUV U/V commands too few U=%0d V=%0d",
				         n_u_cmd, n_v_cmd);
				fails = fails + 1;
			end
			if (n_y_cmd < 8) begin
				$display("FAIL YUV Y commands too few %0d", n_y_cmd);
				fails = fails + 1;
			end
			if (n_disp_ok < 64) begin
				$display("FAIL YUV visible pixels %0d black=%0d",
				         n_disp_ok, n_disp_black);
				fails = fails + 1;
			end else
				$display("OK   YUV visible pixels %0d", n_disp_ok);
			if (n_y_cmd < 8 || n_u_cmd + 4 < n_y_cmd || n_v_cmd + 4 < n_y_cmd) begin
				$display("FAIL YUV plane imbalance Y=%0d U=%0d V=%0d",
				         n_y_cmd, n_u_cmd, n_v_cmd);
				fails = fails + 1;
			end else
				$display("OK   YUV 3-plane command balance Y=%0d U=%0d V=%0d",
				         n_y_cmd, n_u_cmd, n_v_cmd);
		end else begin
			if (n_bgr_cmd < 8) begin
				$display("FAIL BGR commands %0d", n_bgr_cmd);
				fails = fails + 1;
			end
			if (n_disp_ok < 64) begin
				$display("FAIL BGR visible pixels %0d black=%0d",
				         n_disp_ok, n_disp_black);
				fails = fails + 1;
			end else
				$display("OK   BGR visible pixels %0d cmds=%0d",
				         n_disp_ok, n_bgr_cmd);
		end
	end
endtask

initial begin
	DDRAM_BUSY = 0;
	DDRAM_DOUT = 0;
	DDRAM_DOUT_READY = 0;
	run_mode(0, 800000);
	run_mode(1, 1800000);
	if (fails) begin
		$display("fb_yuv_quiet_tb FAIL (%0d)", fails);
		$fatal(1);
	end
	$display("fb_yuv_quiet_tb PASS");
	$finish;
end

endmodule
